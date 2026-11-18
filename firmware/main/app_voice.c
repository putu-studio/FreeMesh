// Voice messages. See app_voice.h.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_random.h"
#include "esp_http_server.h"
#include "codec2.h"
#include "app_voice.h"
#include "app_chat.h"
#include "app_radio.h"        // LORA_MAX_PAYLOAD.
#include "app_crypto.h"       // crypto_locked, CRYPTO_OVERHEAD.
#include "app_friends.h"      // friends_seal, friends_open, SEAL_OVERHEAD.
#include "app_groups.h"
#include "app_settings.h"     // node_id_str, device_settings.

#define MODE        CODEC2_MODE_1200
#define FRAME_SAMP  320                    // Samples per Codec2 frame: 40 ms at 8000 Hz.
#define FRAME_BYTES 6                      // Bytes per Codec2 frame (48 bits).
#define MAX_FRAMES  (VOICE_MAX_S * 25)     // 25 frames per second.
#define CLIP_MAX    (MAX_FRAMES * FRAME_BYTES)   // 1500 bytes.
#define SEALED_MAX  (CLIP_MAX + 12 + 16)         // Clip locked on the phone: nonce + Codec2 bytes + tag.
#define HDR         13                     // Length of the packet header "V|*----|AAAA|".
#define NAMES_MAX   34                     // "10|" + device name 16 + "|" + name 12 + "|" + terminator.
#define MAX_CHUNKS  32                     // One bit per chunk in the 32-bit "missing" mask.
#define STORE       8                      // Newest clips kept for playing.
#define RX_SLOTS    3                      // Clips that can be received at the same time.
#define ROUNDS      3                      // Send everything once, then resend missing chunks up to twice.

static SemaphoreHandle_t s_mu;

// ---------- Clips we can play ----------
// sealed: the clip was locked on a phone, so the box cannot play it.
typedef struct { uint32_t msg_id; uint16_t len; uint8_t sealed; uint8_t data[SEALED_MAX]; } clip_t;
static clip_t s_store[STORE];
static int s_store_next;

static void store_clip(uint32_t msg_id, const uint8_t *data, int len, uint8_t sealed)   // Call with s_mu held.
{
    clip_t *c = &s_store[s_store_next++ % STORE];        // Overwrites the oldest clip.
    c->msg_id = msg_id; c->len = len; c->sealed = sealed; memcpy(c->data, data, len);
}

// Clip bytes per chunk. Every chunk has the same size and leaves room for the names that only chunk 0 carries.
static int chunk_bytes(void)
{
    int pkt = crypto_locked() ? LORA_MAX_PAYLOAD - CRYPTO_OVERHEAD : LORA_MAX_PAYLOAD;
    return pkt - HDR - SEAL_OVERHEAD - 5 - NAMES_MAX;    // 120 bytes of a 200-byte packet, or 90 with a passphrase set.
}

// ---------- Recording (web task) ----------
struct voice_rec {
    struct CODEC2 *c2;
    int16_t frame[FRAME_SAMP]; int fill;               // Samples waiting for a full frame.
    uint8_t odd; uint8_t odd_byte;                     // First byte of a sample split across two feed calls.
    uint8_t out[CLIP_MAX]; int frames;
};

voice_rec_t *voice_rec_start(void)
{
    voice_rec_t *r = calloc(1, sizeof(*r));
    if (!r) return NULL;
    r->c2 = codec2_create(MODE);
    if (!r->c2) { free(r); return NULL; }
    return r;
}

static void put_sample(voice_rec_t *r, int16_t s)
{
    if (r->frames >= MAX_FRAMES) return;               // Cut the clip at 10 s.
    r->frame[r->fill++] = s;
    if (r->fill == FRAME_SAMP) {
        codec2_encode(r->c2, r->out + r->frames * FRAME_BYTES, r->frame);
        r->frames++; r->fill = 0;
    }
}

void voice_rec_feed(voice_rec_t *r, const uint8_t *pcm, int len)
{
    int i = 0;
    if (r->odd && len > 0) { put_sample(r, (int16_t)(r->odd_byte | (pcm[0] << 8))); r->odd = 0; i = 1; }
    for (; i + 1 < len; i += 2) put_sample(r, (int16_t)(pcm[i] | (pcm[i + 1] << 8)));
    if (i < len) { r->odd = 1; r->odd_byte = pcm[i]; }
}

void voice_rec_drop(voice_rec_t *r)
{
    if (!r) return;
    codec2_destroy(r->c2);
    free(r);
}

// ---------- Sending (radio task) ----------
static struct {
    uint8_t  active, phase;              // phase: 0 = sending chunks, 1 = waiting for W answers.
    uint32_t msg_id;
    uint16_t clip;
    char     target[6];                  // "*----", "pAAAA", "g3fa9" or "uAAAA" (one person on box AAAA).
    char     names[NAMES_MAX + 1];       // Example: "4|Box B|Alex|".
    uint8_t  data[SEALED_MAX]; int len, n_chunks, per;
    uint32_t todo;                       // Chunks still to send in this round.
    uint32_t missing;                    // Chunks that a receiver reported missing.
    uint8_t  round, need, done;          // need and done: bit i = chat peer slot i.
    TickType_t next_at;
} s_tx;
static uint16_t   s_clip_ctr;
static TickType_t s_last_clip;           // When this box last started a clip.

int voice_wait_s(void)
{
    if (!s_last_clip) return 0;
    TickType_t gone = xTaskGetTickCount() - s_last_clip;
    TickType_t gap = pdMS_TO_TICKS(VOICE_GAP_S * 1000);
    return gone >= gap ? 0 : (int)((gap - gone) * portTICK_PERIOD_MS / 1000) + 1;
}

uint32_t voice_rec_finish(voice_rec_t *r, const char *from, const char *to, const char *group, const char **why)
{
    return voice_send_codec2(from, to, group, r->out, r->frames * FRAME_BYTES, why);
}

// Take a clip that is already Codec2 (compressed on the phone with codec2.wasm, or by voice_rec_finish above),
// keep it for playing and queue it for the radio.
uint32_t voice_send_codec2(const char *from, const char *to, const char *group, const uint8_t *c2, int len, const char **why)
{
    *why = "Box busy";
    if (len < 0) { *why = "Not sent"; return 0; }
    if (len > CLIP_MAX) len = CLIP_MAX;                  // Phone timing can run slightly over 10 s: keep the first 10 s.
    int frames = len / FRAME_BYTES;
    int secs = (frames + 24) / 25;
    if (frames < 13) { *why = "Too short. Hold the button while you talk"; return 0; }   // Under half a second.
    if (voice_wait_s()) { *why = "Wait a bit: one voice message every 30 s"; return 0; }
    xSemaphoreTake(s_mu, portMAX_DELAY);
    if (s_tx.active) { xSemaphoreGive(s_mu); *why = "Still sending the last voice message"; return 0; }
    xSemaphoreGive(s_mu);

    char text[40];
    snprintf(text, sizeof(text), "🎤 Voice message (%d s)", secs);
    uint32_t id = chat_add_voice(from, device_settings.devicename, text, secs, 1, 0, 0, to, group);
    if (!id) return 0;

    xSemaphoreTake(s_mu, portMAX_DELAY);
    store_clip(id, c2, frames * FRAME_BYTES, 0);
    memset(&s_tx, 0, sizeof(s_tx));
    s_tx.msg_id = id;
    s_tx.clip = ++s_clip_ctr;
    chat_set_origin(id, node_id_str(), s_tx.clip);       // Lets other boxes unsend the clip later.
    if (group[0])   snprintf(s_tx.target, sizeof(s_tx.target), "g%s", group);
    else if (to[0]) snprintf(s_tx.target, sizeof(s_tx.target), "p%s", to);
    else            strcpy(s_tx.target, "*----");
    char f[13]; strlcpy(f, from, sizeof(f));
    for (char *p = f; *p; p++) if (*p == '|') *p = ' ';
    snprintf(s_tx.names, sizeof(s_tx.names), "%u|%.16s|%.12s|", (unsigned)secs % 100u, device_settings.devicename, f);
    memcpy(s_tx.data, c2, frames * FRAME_BYTES);
    s_tx.len = frames * FRAME_BYTES;
    s_tx.per = chunk_bytes();
    s_tx.n_chunks = (s_tx.len + s_tx.per - 1) / s_tx.per;
    s_tx.todo = s_tx.n_chunks >= 32 ? 0xFFFFFFFFu : (1u << s_tx.n_chunks) - 1;
    s_tx.active = 1;
    s_last_clip = xTaskGetTickCount();
    xSemaphoreGive(s_mu);
    printf("Voice: %d s clip, %d bytes, %d chunks\n", secs, frames * FRAME_BYTES, s_tx.n_chunks);
    *why = NULL;
    return id;
}

// Voice to one person, end to end. The phone has already compressed the clip (Codec2 in WebAssembly) and locked
// it with the person key, so the box only carries sealed bytes that it cannot open.
// Same box: only store it. Friend box: send it like a private clip with target "uAAAA". The name slot of chunk 0
// carries "to_pid:from_pid" so the other box knows which phone the clip is for.
uint32_t voice_send_sealed(const char *to_box, const char *to_pid, const char *from_pid, int secs,
                           const uint8_t *sealed, int len, const char **why)
{
    *why = "Box busy";
    if (len < 12 + 16 + 13 * FRAME_BYTES || len > SEALED_MAX) { *why = "Voice message too short or too long"; return 0; }
    if (secs < 1) secs = 1;
    if (secs > VOICE_MAX_S) secs = VOICE_MAX_S;
    bool here = strcmp(to_box, node_id_str()) == 0;
    if (!here && voice_wait_s()) { *why = "Wait a bit: one voice message every 30 s"; return 0; }
    xSemaphoreTake(s_mu, portMAX_DELAY);
    bool busy = s_tx.active;
    xSemaphoreGive(s_mu);
    if (!here && busy) { *why = "Still sending the last voice message"; return 0; }

    uint32_t id = chat_add_e2e_voice(to_box, device_settings.devicename, to_pid, from_pid, secs, 1, 0, 0);
    if (!id) return 0;
    xSemaphoreTake(s_mu, portMAX_DELAY);
    store_clip(id, sealed, len, 1);
    if (here) {                                          // Both people use this box: no radio needed.
        xSemaphoreGive(s_mu);
        chat_set_status(id, 3, 0, 0);
        *why = NULL;
        return id;
    }
    memset(&s_tx, 0, sizeof(s_tx));
    s_tx.msg_id = id;
    s_tx.clip = ++s_clip_ctr;
    chat_set_origin(id, node_id_str(), s_tx.clip);
    snprintf(s_tx.target, sizeof(s_tx.target), "u%.4s", to_box);
    snprintf(s_tx.names, sizeof(s_tx.names), "%d|%.16s|%.4s:%.4s|", secs, device_settings.devicename, to_pid, from_pid);
    memcpy(s_tx.data, sealed, len);
    s_tx.len = len;
    s_tx.per = chunk_bytes();
    s_tx.n_chunks = (s_tx.len + s_tx.per - 1) / s_tx.per;
    s_tx.todo = s_tx.n_chunks >= 32 ? 0xFFFFFFFFu : (1u << s_tx.n_chunks) - 1;
    s_tx.active = 1;
    s_last_clip = xTaskGetTickCount();
    xSemaphoreGive(s_mu);
    printf("Voice: %d s locked clip for one person on %s, %d bytes, %d chunks\n", secs, to_box, len, s_tx.n_chunks);
    *why = NULL;
    return id;
}

// Peers that must answer: everyone in range, the one friend box, or every group member.
static uint8_t need_mask(void)
{
    if (s_tx.target[0] == 'p' || s_tx.target[0] == 'u') return 1u << chat_peer_slot(s_tx.target + 1);
    if (s_tx.target[0] == 'g') {
        char ids[GROUP_MAX_MEMBERS][5]; uint8_t m = 0;
        int n = groups_members_of(s_tx.target + 1, ids, GROUP_MAX_MEMBERS);
        for (int k = 0; k < n; k++) m |= 1u << chat_peer_slot(ids[k]);
        return m;
    }
    return chat_peers_in_range();
}

// Build chunk n into out. Returns its length, or -1 on failure.
static int build_chunk(int n, uint8_t *out, int max)
{
    uint8_t body[LORA_MAX_PAYLOAD]; int bl = 0;
    body[bl++] = s_tx.clip >> 8; body[bl++] = s_tx.clip & 0xFF; body[bl++] = n; body[bl++] = s_tx.n_chunks; body[bl++] = s_tx.per;
    if (n == 0) { int l = strlen(s_tx.names); memcpy(body + bl, s_tx.names, l); bl += l; }
    int off = n * s_tx.per, cl = s_tx.len - off < s_tx.per ? s_tx.len - off : s_tx.per;
    memcpy(body + bl, s_tx.data + off, cl); bl += cl;
    int hl = snprintf((char *)out, max, "V|%s|%s|", s_tx.target, node_id_str());
    if (s_tx.target[0] == '*') { if (hl + bl > max) return -1; memcpy(out + hl, body, bl); return hl + bl; }
    int sl = (s_tx.target[0] == 'p' || s_tx.target[0] == 'u')
        ? friends_seal(s_tx.target + 1, body, bl, out + hl, max - hl)
        : groups_seal(s_tx.target + 1, node_id_str(), body, bl, out + hl, max - hl);
    return sl < 0 ? -1 : hl + sl;
}

static void tx_finish(void)
{
    uint8_t acks = 0, need = 0;
    for (int i = 0; i < 8; i++) { acks += (s_tx.done >> i) & 1; need += (s_tx.need >> i) & 1; }
    chat_set_status(s_tx.msg_id, need && acks >= need ? 3 : acks ? 1 : (need ? 2 : 1), acks, need);
    s_tx.active = 0;
}

// ---------- Receiving (radio task) ----------
typedef struct {
    uint8_t  used, complete;
    char     from[5], target[6];
    uint16_t clip;
    uint8_t  n_chunks;
    uint32_t have;
    char     names[NAMES_MAX + 1];
    uint8_t  data[SEALED_MAX + 128]; int per, len;   // +128: the last chunk can end anywhere.
    int16_t  rssi; float snr;
    TickType_t last, w_at;               // When the last chunk was heard, and when to send the W answer.
    uint8_t  w_due;
} rx_t;
static rx_t s_rx[RX_SLOTS];
static TickType_t s_busy_until;

static int rx_find(const char *from, uint16_t clip, bool make)
{
    int old = 0;
    for (int i = 0; i < RX_SLOTS; i++) {
        if (s_rx[i].used && !strcmp(s_rx[i].from, from) && s_rx[i].clip == clip) return i;
        if (s_rx[i].last < s_rx[old].last) old = i;
    }
    if (!make) return -1;
    for (int i = 0; i < RX_SLOTS; i++) if (!s_rx[i].used) { old = i; break; }
    memset(&s_rx[old], 0, sizeof(rx_t));
    s_rx[old].used = 1;
    return old;
}

static uint32_t all_bits(int n) { return n >= 32 ? 0xFFFFFFFFu : (1u << n) - 1; }

static void rx_complete(rx_t *x)
{
    x->complete = 1;                                     // Set even when the names are bad, so the box stops asking.
    // names is "secs|devicename|name|".
    char *secs = x->names, *dn = strchr(secs, '|'); if (!dn) return; *dn++ = 0;
    char *nm = strchr(dn, '|'); if (!nm) return; *nm++ = 0;
    char *end = strchr(nm, '|'); if (end) *end = 0;
    int s = atoi(secs); if (s < 1 || s > VOICE_MAX_S) s = 1;
    char text[40]; snprintf(text, sizeof(text), "🎤 Voice message (%d s)", s);
    if (x->target[0] == 'u') {                           // For one person: the name slot holds "to_pid:from_pid".
        char *fp = strchr(nm, ':');
        if (!fp || fp - nm != 4 || strlen(fp + 1) != 4 || x->len < 12 + 16 || x->len > SEALED_MAX) return;
        *fp++ = 0;
        uint32_t id = chat_add_e2e_voice(x->from, dn, nm, fp, s, 0, x->rssi, x->snr);
        // Store the locked bytes exactly as they arrived.
        if (id) { store_clip(id, x->data, x->len, 1); chat_set_origin(id, x->from, x->clip); }
        printf("Voice: %d s locked clip from %s for one person\n", s, x->from);
        return;
    }
    const char *peer = x->target[0] == 'p' ? x->from : "";
    const char *group = x->target[0] == 'g' ? x->target + 1 : "";
    int len = x->len - (x->len % FRAME_BYTES);
    uint32_t id = chat_add_voice(nm, dn, text, s, 0, x->rssi, x->snr, peer, group);
    if (id) { store_clip(id, x->data, len, 0); chat_set_origin(id, x->from, x->clip); }
    x->complete = 1;
    printf("Voice: %d s clip from %s (%s)\n", s, x->from, nm);
}

void voice_on_packet(char type, uint8_t *buf, int len, int rssi, float snr)
{
    const char *me = node_id_str();
    if (type == 'W') {                                   // Packet layout: W|to|from|clip|missing.
        if (len != 25 || buf[6] != '|' || buf[11] != '|' || buf[16] != '|') return;
        char *p = (char *)buf + 2; p[4] = 0; p[9] = 0; p[14] = 0;
        if (!strcmp(p + 5, me)) return;
        int slot = chat_peer_heard(p + 5, rssi, snr);
        if (strcmp(p, me)) return;
        uint16_t clip = strtoul(p + 10, NULL, 16);
        uint32_t miss = strtoul(p + 15, NULL, 16);
        xSemaphoreTake(s_mu, portMAX_DELAY);
        if (s_tx.active && s_tx.clip == clip) {
            s_tx.need |= 1u << slot;
            if (!miss) s_tx.done |= 1u << slot;
            else s_tx.missing |= miss & all_bits(s_tx.n_chunks);
            uint8_t acks = 0, need = 0;
            for (int i = 0; i < 8; i++) { acks += (s_tx.done >> i) & 1; need += (s_tx.need >> i) & 1; }
            if ((s_tx.done & s_tx.need) == s_tx.need) tx_finish();
            else chat_set_status(s_tx.msg_id, 1, acks, need);
        }
        xSemaphoreGive(s_mu);
        return;
    }
    // Packet layout: V|target|from|body.
    if (len < HDR + 5 || buf[7] != '|' || buf[12] != '|') return;
    char target[6], from[5];
    memcpy(target, buf + 2, 5); target[5] = 0;
    memcpy(from, buf + 8, 4); from[4] = 0;
    if (!strcmp(from, me)) return;
    chat_peer_heard(from, rssi, snr);
    uint8_t body[LORA_MAX_PAYLOAD]; int bl;
    if (target[0] == '*') { bl = len - HDR; memcpy(body, buf + HDR, bl); }
    else if (target[0] == 'p' || target[0] == 'u') {    // u: a clip for one person on this box, locked on a phone.
        if (strcmp(target + 1, me)) {                    // A private clip for another box: only stay quiet for 1.5 s.
            s_busy_until = xTaskGetTickCount() + pdMS_TO_TICKS(1500);
            return;
        }
        bl = friends_open(from, buf + HDR, len - HDR, body, sizeof(body));
    }
    else if (target[0] == 'g') bl = groups_open(target + 1, from, buf + HDR, len - HDR, body, sizeof(body));
    else return;
    if (bl < 5) { s_busy_until = xTaskGetTickCount() + pdMS_TO_TICKS(1500); return; }   // Not ours, but the air is busy.

    uint16_t clip = body[0] << 8 | body[1];
    int n = body[2], N = body[3], per = body[4];
    if (N < 1 || N > MAX_CHUNKS || n >= N || per < 1) return;
    int off = 5;
    TickType_t now = xTaskGetTickCount();
    xSemaphoreTake(s_mu, portMAX_DELAY);
    int i = rx_find(from, clip, true);
    rx_t *x = &s_rx[i];
    if (!x->n_chunks) { strcpy(x->from, from); strcpy(x->target, target); x->clip = clip; x->n_chunks = N; x->per = per; }
    if (n == 0) {                                        // Chunk 0 starts with "secs|devicename|name|".
        int bars = 0, k = off;
        while (k < bl && bars < 3) { if (body[k] == '|') bars++; k++; }
        if (bars < 3 || k - off > NAMES_MAX) { xSemaphoreGive(s_mu); return; }
        memcpy(x->names, body + off, k - off); x->names[k - off] = 0;
        off = k;
    }
    int cl = bl - off;
    if (!x->complete && !(x->have & (1u << n)) && cl > 0 && cl <= x->per
        && n * x->per + cl <= (int)sizeof(x->data)) {
        memcpy(x->data + n * x->per, body + off, cl);   // Chunk n always starts at n * per.
        if (n == N - 1) x->len = n * x->per + cl;
        x->have |= 1u << n;
    }
    x->rssi = rssi; x->snr = snr; x->last = now;
    if (!x->complete && x->have == all_bits(N)) rx_complete(x);
    // Answer in a random slot after the last chunk, or 3 s after the latest chunk heard.
    x->w_due = 1;
    x->w_at = now + pdMS_TO_TICKS(n == N - 1 ? chat_slot_ms() * (1 + esp_random() % 8) : 3000);
    // The sender is still sending: hold this box's own packets back until its last chunk.
    TickType_t rest = pdMS_TO_TICKS((N - 1 - n) * (chat_toa_ms(len) + 3 * chat_slot_ms()) + 300);
    if ((int32_t)(now + rest - s_busy_until) > 0) s_busy_until = now + rest;
    xSemaphoreGive(s_mu);
}

void voice_forget(uint32_t msg_id)
{
    if (!msg_id) return;
    xSemaphoreTake(s_mu, portMAX_DELAY);
    for (int i = 0; i < STORE; i++) if (s_store[i].msg_id == msg_id) s_store[i].msg_id = 0;   // No longer playable.
    if (s_tx.active && s_tx.msg_id == msg_id) s_tx.active = 0;                              // Stop sending it.
    xSemaphoreGive(s_mu);
}

bool voice_air_busy(void)
{
    return (int32_t)(xTaskGetTickCount() - s_busy_until) < 0;
}

int voice_next_packet(uint8_t *out, int max)
{
    TickType_t now = xTaskGetTickCount();
    int r = 0;
    xSemaphoreTake(s_mu, portMAX_DELAY);
    // First choice: a W answer that is due.
    for (int i = 0; i < RX_SLOTS && !r; i++) {
        rx_t *x = &s_rx[i];
        if (!x->used || !x->w_due || (int32_t)(now - x->w_at) < 0) continue;
        uint32_t miss = x->complete ? 0 : all_bits(x->n_chunks) & ~x->have;
        r = snprintf((char *)out, max, "W|%s|%s|%04X|%08lX", x->from, node_id_str(), x->clip, (unsigned long)miss);
        x->w_due = 0;
    }
    // Second choice: the next chunk of this box's own clip.
    if (!r && s_tx.active && (int32_t)(now - s_tx.next_at) >= 0) {
        if (s_tx.phase == 0 && s_tx.todo) {
            int n = __builtin_ctz(s_tx.todo);
            s_tx.todo &= ~(1u << n);
            r = build_chunk(n, out, max);
            if (r < 0) { r = 0; chat_set_status(s_tx.msg_id, 2, 0, 0); s_tx.active = 0; }
            else s_tx.next_at = now + pdMS_TO_TICKS(chat_toa_ms(r) + chat_slot_ms() * (1 + esp_random() % 3));
            if (s_tx.active && !s_tx.todo) {             // All chunks sent: wait for the answers.
                if (s_tx.round == 0) { s_tx.need = need_mask(); chat_set_status(s_tx.msg_id, 1, 0, 0); }
                s_tx.phase = 1;
                s_tx.next_at = now + pdMS_TO_TICKS(3500 + 10 * chat_slot_ms());
            }
        } else if (s_tx.phase == 1) {                    // The wait for answers is over.
            bool silent = (s_tx.done & s_tx.need) != s_tx.need;      // A peer has not answered; its W may be lost.
            if ((s_tx.missing || silent) && ++s_tx.round < ROUNDS) {
                // Resend what is missing. If no chunk is reported missing but a peer stayed silent, its answer
                // was probably lost: resend only the last chunk, so that every box with the whole clip answers
                // again. Without this, one lost answer marks the clip as not delivered.
                s_tx.todo = s_tx.missing ? s_tx.missing : 1u << (s_tx.n_chunks - 1);
                s_tx.missing = 0; s_tx.phase = 0;
            } else tx_finish();
        }
    }
    xSemaphoreGive(s_mu);
    return r;
}

// ---------- Playing (web task) ----------
esp_err_t voice_play(void *req_, uint32_t msg_id, bool raw)
{
    httpd_req_t *req = req_;
    static uint8_t clip[SEALED_MAX]; int len = -1; uint8_t sealed = 0;   // static: safe, the web server is one task.
    xSemaphoreTake(s_mu, portMAX_DELAY);
    for (int i = 0; i < STORE; i++) if (s_store[i].msg_id == msg_id && s_store[i].len) { len = s_store[i].len; sealed = s_store[i].sealed; memcpy(clip, s_store[i].data, len); }
    xSemaphoreGive(s_mu);
    if (len < 0) return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "That voice message is gone (the box keeps the last 8)");
    if (sealed || raw) {                                 // Locked, or the phone decodes it: send the stored bytes.
        httpd_resp_set_type(req, "application/octet-stream");
        return httpd_resp_send(req, (const char *)clip, len);
    }
    struct CODEC2 *c2 = codec2_create(MODE);
    if (!c2) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No memory");
    int frames = len / FRAME_BYTES;
    uint32_t data_len = frames * FRAME_SAMP * 2;
    uint8_t h[45] = "RIFF____WAVEfmt \x10\0\0\0\x01\0\x01\0\x40\x1f\0\0\x80\x3e\0\0\x02\0\x10\0data____";
    uint32_t riff = 36 + data_len;
    memcpy(h + 4, &riff, 4); memcpy(h + 40, &data_len, 4);
    httpd_resp_set_type(req, "audio/wav");
    httpd_resp_send_chunk(req, (const char *)h, 44);
    static int16_t pcm[FRAME_SAMP * 4];
    for (int f = 0; f < frames; f += 4) {
        int k = 0;
        for (; k < 4 && f + k < frames; k++) codec2_decode(c2, pcm + k * FRAME_SAMP, clip + (f + k) * FRAME_BYTES);
        if (httpd_resp_send_chunk(req, (const char *)pcm, k * FRAME_SAMP * 2) != ESP_OK) break;
    }
    codec2_destroy(c2);
    return httpd_resp_send_chunk(req, NULL, 0);
}

void voice_init(void)
{
    s_mu = xSemaphoreCreateMutex();
    s_clip_ctr = (uint16_t)esp_random();
}

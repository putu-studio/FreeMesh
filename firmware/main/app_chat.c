#include <stdio.h>                  // printf, snprintf
#include <stdlib.h>                 // strtoul
#include <string.h>                 // memset, strchr, strlcpy, strcmp
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"          // xTaskCreate, vTaskDelay, xTaskGetTickCount
#include "freertos/queue.h"         // xQueueCreate, xQueueSend, xQueueReceive
#include "freertos/semphr.h"        // xSemaphoreCreateMutex (the lock)
#include "app_chat.h"
#include "app_radio.h"              // radio_send, radio_check_rx, radio_start_rx, radio_apply
#include "app_settings.h"           // device_settings, node_id_str()
#include "esp_random.h"             // esp_random(): hardware random 32-bit number.
#include "esp_http_server.h"        // httpd_req_t, httpd_resp_sendstr_chunk
#include "app_io.h"                  // io_notify: show an event on the LED.
#include "app_people.h"
#include "app_crypto.h"              // Network lock: encrypt before sending, verify and decrypt after receiving.
#include "app_friends.h"             // Private messages, sealed with the friend key.
#include "app_groups.h"              // Group messages, sealed with the group key.
#include "app_voice.h"               // Voice messages: V chunks and W answers.
#include "app_sos.h"                 // SOS that also reaches Meshtastic and MeshCore boxes.
#include "app_gw.h"                  // Passes Everyone messages to the optional gateway add-on when it is built and on.

static chat_msg_t s_log[CHAT_MAX];       // Ring buffer: message id is stored in slot id % CHAT_MAX.
static uint32_t s_next_id = 1;           // Next message id to hand out.
static volatile uint32_t s_version;      // Goes up on every new message or status change. The Bluetooth app watches it.
static SemaphoreHandle_t s_lock;         // Mutex that protects s_log and s_peers.
static QueueHandle_t s_txq;              // Web to radio task: ids of messages to send.
static QueueHandle_t s_cfgq;             // Web to radio task: the settings changed.
static uint32_t s_boot;                  // Random number, new at every start, so phones notice a restart.

// ---------- Other boxes ----------
#define MAX_PEERS 8                      // One bit per box in a uint8_t mask.
#define PEER_GONE_MS 90000               // A box not heard for 90 s counts as out of range.

typedef struct {
    uint8_t    used;                     // 0 = empty slot.
    char       id[5];                    // Box id, such as "A1B2".
    char       devicename[17];           // Box name, such as "LoRa A1B2" (empty until it is learned).
    uint16_t   last_seq;                 // Number of the last message shown from this box.
    uint8_t    seq_known;                // 0 = no message from this box yet.
    TickType_t heard;                    // Time of the last packet of any type from this box.
    int16_t    rssi;                     // Signal strength of that packet in dBm.
    float      snr;
} peer_t;
static peer_t s_peers[MAX_PEERS];        // Also read by the web task: access it only with s_lock held.

// ---------- The one message waiting for ACKs ----------
typedef struct {
    uint8_t    active;                   // 0 = nothing is waiting.
    uint32_t   log_id;                   // Log entry whose status to update.
    uint16_t   seq;                      // Sequence number expected in the ACKs.
    uint8_t    tries;                    // Number of times sent so far.
    TickType_t next_try;                 // Time to stop waiting and send again.
    uint8_t    need;                     // Bit i = s_peers[i] should answer.
    uint8_t    acked;                    // Bit i = s_peers[i] has answered.
    uint8_t    pkt[LORA_MAX_PAYLOAD];    // Exact packet bytes, so every retry is identical.
    uint8_t    len;
} pending_t;
static pending_t s_pend;
static uint16_t  s_seq = 0;              // This box's message counter (starts at a random value).

typedef struct {                         // One ACK waiting for its random time slot.
    uint8_t    active;
    char       pkt[24];
    TickType_t at;
} ack_out_t;
static ack_out_t s_ack;

// ---------- Unsend ----------
// X|from|items   items = "1A2F,v03C1,...": hex numbers. A leading "v" marks a voice clip number; the rest are
// text message numbers. No ACKs: the packet goes out twice, 3 s apart. A box out of range then keeps the message.
#define UNSEND_ITEMS 100                 // Room for 16 items per packet ("v" + 4 hex digits + comma).
typedef struct { char items[UNSEND_ITEMS]; } unsend_t;
static QueueHandle_t s_unq;              // Web to radio task: X packets to send (2 deep).
static struct { uint8_t left; TickType_t at; char pkt[16 + UNSEND_ITEMS]; uint8_t len; } s_unx;   // The second copy.
#define GONE_MAX 16
static uint32_t s_gone[GONE_MAX];        // Last 16 unsent log ids. Phones remove these messages (JSON "gone").
static int s_gone_n;

static uint16_t box_num(const char *id) { return (uint16_t)strtoul(id, NULL, 16); }   // "A1B2" -> 0xA1B2.

// Remove one message. Call with s_lock held. Phones learn about it from the "gone" list.
static void mark_gone(chat_msg_t *m)
{
    m->gone = 1;
    memset(m->text, 0, sizeof(m->text));
    s_gone[s_gone_n++ % GONE_MAX] = m->id;
    s_version++;
}

// ---------- Heartbeat ----------
static TickType_t s_last_name_tx;        // Time of the last packet this box sent (ACKs excluded). Delays the heartbeat.
static TickType_t s_next_hb;             // Time the next heartbeat is due.

// Copy src into dst, escaping " and \ and dropping control characters.
void json_escape(char *dst, size_t dst_len, const char *src)
{
    size_t o = 0;
    for (; *src && o + 2 < dst_len; src++) {
        if ((unsigned char)*src < 0x20) continue;
        if (*src == '"' || *src == '\\') dst[o++] = '\\';
        dst[o++] = *src;
    }
    dst[o] = '\0';
}

static uint32_t chat_add_peer(const char *from, const char *devicename, const char *text,
                              uint8_t local, int16_t rssi, float snr, const char *peer, const char *group)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    chat_msg_t *m = &s_log[s_next_id % CHAT_MAX];
    memset(m, 0, sizeof(*m));
    m->id = s_next_id++;
    s_version++;
    strlcpy(m->from, from, sizeof(m->from));
    strlcpy(m->devicename, devicename, sizeof(m->devicename));
    strlcpy(m->text, text, sizeof(m->text));
    m->local = local;
    m->rssi = rssi;
    m->snr = snr;
    strlcpy(m->peer, peer, sizeof(m->peer));
    strlcpy(m->group, group, sizeof(m->group));
    m->voice = 0;
    uint32_t id = m->id;
    xSemaphoreGive(s_lock);
    if (!peer[0] && !group[0]) gw_on_message(from, devicename, text, local);   // The optional gateway add-on gets Everyone messages only.
    return id;
}

static uint32_t chat_add(const char *from, const char *devicename, const char *text,
                         uint8_t local, int16_t rssi, float snr)
{
    return chat_add_peer(from, devicename, text, local, rssi, snr, "", "");
}

// Count the 1 bits in a mask: 0b00000101 -> 2.
static uint8_t count_bits(uint8_t mask)
{
    uint8_t n = 0;
    for (int i = 0; i < 8; i++) if (mask & (1u << i)) n++;
    return n;
}

// Find the box with this id in s_peers, or give it a slot. Call with s_lock held.
static int peer_slot(const char *id)
{
    int free_i = -1, oldest_i = 0;
    for (int i = 0; i < MAX_PEERS; i++) {
        if (s_peers[i].used && strcmp(s_peers[i].id, id) == 0) return i;    // Known box.
        if (!s_peers[i].used && free_i < 0) free_i = i;                     // First empty slot.
        if (s_peers[i].heard < s_peers[oldest_i].heard) oldest_i = i;       // Heard longest ago.
    }
    int i = (free_i >= 0) ? free_i : oldest_i;                              // List full: replace the oldest.
    memset(&s_peers[i], 0, sizeof(s_peers[i]));
    s_peers[i].used = 1;
    strlcpy(s_peers[i].id, id, sizeof(s_peers[i].id));
    return i;
}

// Record that box "id" was just heard. Returns its slot.
static int peer_heard(const char *id, const char *devicename, int rssi, float snr)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int i = peer_slot(id);
    s_peers[i].heard = xTaskGetTickCount();
    s_peers[i].rssi = rssi;
    s_peers[i].snr = snr;
    if (devicename) strlcpy(s_peers[i].devicename, devicename, sizeof(s_peers[i].devicename));
    xSemaphoreGive(s_lock);
    people_box_heard(id);                                // A friend box that is back in range is told who uses this box.
    return i;
}

// Bit mask of the boxes heard in the last 90 s.
static uint8_t peers_in_range(void)
{
    uint8_t mask = 0;
    TickType_t now = xTaskGetTickCount();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < MAX_PEERS; i++)
        if (s_peers[i].used && now - s_peers[i].heard < pdMS_TO_TICKS(PEER_GONE_MS)) mask |= 1u << i;
    xSemaphoreGive(s_lock);
    return mask;
}

// Update the delivery status shown for a message.
static void set_status(uint32_t log_id, uint8_t st, uint8_t acks, uint8_t need)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_version++;
    chat_msg_t *m = &s_log[log_id % CHAT_MAX];
    if (m->id == log_id) { m->st = st; m->acks = acks; m->need = need; }
    xSemaphoreGive(s_lock);
}

// One ACK slot; it doubles per SF step. A locked ACK is 30 bytes longer and needs a longer slot (SF7: 60 -> 110 ms).
static uint32_t slot_ms(void) { return (crypto_locked() ? 110u : 60u) << (device_settings.sf - 7); }

static esp_err_t send_now(const uint8_t *pkt, int len)
{
    uint8_t air[LORA_MAX_PAYLOAD];
    int n = crypto_wrap(pkt, len, air, sizeof(air));   // Locked: encrypt and sign. Open: same bytes.
    esp_err_t err = n > 0 ? radio_send(air, n) : ESP_FAIL;
    radio_start_rx();                                    // Sending ends receive mode, so start it again.
    return err;
}

// Schedule the ACK for message number s from box "id" in a random slot and record the box.
// Returns true when the message was shown before (the sender retried because the ACK was lost).
static bool ack_and_repeat(const char *id, uint16_t s, const char *dn, int rssi, float snr)
{
    const char *me = node_id_str();
    if (s_ack.active) { send_now((uint8_t *)s_ack.pkt, strlen(s_ack.pkt)); s_ack.active = 0; }
    snprintf(s_ack.pkt, sizeof(s_ack.pkt), "A|%.4s|%u|%.4s", id, s, me);   // Ids are 4 characters.
    s_ack.at = xTaskGetTickCount() + pdMS_TO_TICKS(slot_ms() * (1 + esp_random() % 8));
    s_ack.active = 1;
    int i = peer_heard(id, dn, rssi, snr);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int repeat = s_peers[i].seq_known && s_peers[i].last_seq == s;
    s_peers[i].last_seq = s; s_peers[i].seq_known = 1;
    xSemaphoreGive(s_lock);
    if (repeat) printf("repeat #%u from %s, ACKed again, not shown\n", s, id);
    return repeat;
}

// Log a person message that arrived or was written here. Only the two phones can read it; the box cannot.
static uint32_t add_e2e(const char *box, const char *dn, const char *to_pid, const char *from_pid, const char *blob,
                        uint8_t local, int16_t rssi, float snr)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    chat_msg_t *m = &s_log[s_next_id % CHAT_MAX];
    memset(m, 0, sizeof(*m));
    m->id = s_next_id++;
    s_version++;
    strlcpy(m->devicename, dn, sizeof(m->devicename));
    strlcpy(m->text, blob, sizeof(m->text));
    strlcpy(m->peer, box, sizeof(m->peer));
    strlcpy(m->to_pid, to_pid, sizeof(m->to_pid));
    strlcpy(m->from_pid, from_pid, sizeof(m->from_pid));
    m->e2e = 1; m->local = local; m->rssi = rssi; m->snr = snr;
    uint32_t id = m->id;
    xSemaphoreGive(s_lock);
    return id;
}

static void handle_packet(char *buf, int len, int rssi, float snr)
{
    const char *me = node_id_str();
    char type = buf[0];
    if (len < 2 || buf[1] != '|' || !strchr("MAHFGPKLQVWUEX", type)) return;   // Not a known packet type.
    char *p = buf + 2;
    char inner[LORA_MAX_PAYLOAD + 1];                    // Decrypted inner part of a sealed packet.
    const char *peer = "";                               // "" = message to everyone.
    const char *group = "";                              // "" = not a group message.

    if (type == 'F' || type == 'G') {                    // Friend request or accept: F|to|from|devicename|pubkey.
        char *f[4] = { p };
        for (int k = 1; k < 4; k++) { f[k] = strchr(f[k - 1], '|'); if (!f[k]) return; *f[k]++ = '\0'; }
        if (strcmp(f[1], me) == 0) return;
        peer_heard(f[1], f[2], rssi, snr);
        friends_on_packet(type, f[0], f[1], f[2], f[3]);
        return;
    }
    if (type == 'P') {                                   // Private message: P|to|from|<sealed: seq|devicename|from|text>.
        if (len < 12 + SEAL_OVERHEAD || p[4] != '|' || p[9] != '|') return;
        p[4] = '\0'; p[9] = '\0';
        if (strcmp(p, me) != 0) return;                  // Private message for another box.
        static char from_box[5];
        strlcpy(from_box, p + 5, sizeof(from_box));
        int n = friends_open(from_box, (uint8_t *)p + 10, len - 12, (uint8_t *)inner, sizeof(inner) - 1);
        if (n <= 0) { printf("private message from %s: not a friend or changed, dropped\n", from_box); return; }
        inner[n] = '\0';
        // Rebuild it as the body of an M packet (id|inner) and continue on the normal M path below.
        static char as_m[LORA_MAX_PAYLOAD + 16];
        snprintf(as_m, sizeof(as_m), "%s|%s", from_box, inner);
        p = as_m;
        peer = from_box;
        type = 'M';
    }

    if (type == 'U') {                                   // U|to|from|<sealed with friend key: pid|online|name|pub>.
        if (len < 12 + SEAL_OVERHEAD || p[4] != '|' || p[9] != '|') return;
        p[4] = '\0'; p[9] = '\0';
        char *from_box = p + 5;
        if (strcmp(from_box, me) == 0) return;
        peer_heard(from_box, NULL, rssi, snr);
        if (strcmp(p, me) != 0) return;
        int n = friends_open(from_box, (uint8_t *)p + 10, len - 12, (uint8_t *)inner, sizeof(inner) - 1);
        if (n <= 0) return;
        inner[n] = '\0';
        people_on_packet(from_box, inner);
        return;
    }
    if (type == 'E') {                                   // E|to box|from box|seq|to pid|from pid|<encrypted bytes>.
        if (len < 2 + 10 + 2 + 10 + 28 || p[4] != '|' || p[9] != '|') return;
        p[4] = '\0'; p[9] = '\0';
        char *from_box = p + 5, *seq = p + 10;
        if (strcmp(from_box, me) == 0) return;
        char *tp = strchr(seq, '|'); if (!tp || tp - seq > 5) return; *tp++ = '\0';
        if (tp + 10 > buf + len || tp[4] != '|' || tp[9] != '|') return;
        tp[4] = '\0'; tp[9] = '\0';
        char *fp = tp + 5;
        const uint8_t *blob = (const uint8_t *)tp + 10;
        int bl = len - (int)((const char *)blob - buf);
        if (strcmp(p, me) != 0 || bl < 28) { peer_heard(from_box, NULL, rssi, snr); return; }
        if (!friends_is_friend(from_box)) { printf("person message from %s: not a friend box, dropped\n", from_box); return; }
        if (ack_and_repeat(from_box, (uint16_t)strtoul(seq, NULL, 10), NULL, rssi, snr)) return;
        char b64[sizeof(((chat_msg_t *)0)->text)];
        if (b64_encode(blob, bl, b64, sizeof(b64)) < 0) return;
        char dn[17] = "";
        xSemaphoreTake(s_lock, portMAX_DELAY);
        for (int i = 0; i < MAX_PEERS; i++) if (s_peers[i].used && strcmp(s_peers[i].id, from_box) == 0) strlcpy(dn, s_peers[i].devicename, sizeof(dn));
        xSemaphoreGive(s_lock);
        chat_set_origin(add_e2e(from_box, dn, tp, fp, b64, 0, rssi, snr), from_box, (uint16_t)strtoul(seq, NULL, 10));
        io_notify(IO_EVT_MESSAGE);
        printf("RX person message %s -> %s from box %s (%d bytes, locked)\n", fp, tp, from_box, bl);
        return;
    }

    if (type == 'X') {                                   // X|from|items: the writer unsent these messages.
        if (len < 8 || p[4] != '|') return;
        p[4] = '\0';
        if (strcmp(p, me) == 0) return;
        peer_heard(p, NULL, rssi, snr);
        uint16_t src = box_num(p);
        uint32_t voices[GONE_MAX]; int nv = 0, n = 0;
        char *save = NULL;
        for (char *it = strtok_r(p + 5, ",", &save); it; it = strtok_r(NULL, ",", &save)) {
            bool v = it[0] == 'v';
            uint16_t num = (uint16_t)strtoul(it + v, NULL, 16);
            xSemaphoreTake(s_lock, portMAX_DELAY);
            for (int i = 0; i < CHAT_MAX; i++) {
                chat_msg_t *m = &s_log[i];
                if (!m->id || m->gone || m->local || m->src != src || m->seq != num || (m->voice != 0) != v) continue;
                if (m->voice && nv < GONE_MAX) voices[nv++] = m->id;
                mark_gone(m); n++;
            }
            xSemaphoreGive(s_lock);
        }
        for (int i = 0; i < nv; i++) voice_forget(voices[i]);   // Outside s_lock: voice has its own lock.
        if (n) printf("unsend: %s removed %d message(s)\n", p, n);
        return;
    }
    if (type == 'V' || type == 'W') { voice_on_packet(type, (uint8_t *)buf, len, rssi, snr); return; }
    if (type == 'K') {                                   // K|to|from|<sealed with friend key: gid|name|key|members>.
        if (len < 12 + SEAL_OVERHEAD || p[4] != '|' || p[9] != '|') return;
        p[4] = '\0'; p[9] = '\0';
        char *from_box = p + 5;
        if (strcmp(from_box, me) != 0) peer_heard(from_box, NULL, rssi, snr);
        if (strcmp(p, me) != 0) return;                  // Key for another box.
        int n = friends_open(from_box, (uint8_t *)p + 10, len - 12, (uint8_t *)inner, sizeof(inner) - 1);
        if (n <= 0) { printf("group key from %s: not a friend or changed, dropped\n", from_box); return; }
        inner[n] = '\0';
        char gid[5]; strlcpy(gid, inner, sizeof(gid));
        bool ok = groups_on_key(from_box, inner);
        memset(inner, 0, sizeof(inner));                 // Wipe the buffer: it held the group key.
        if (ok) {                                        // Confirm to the owner: L|owner|me|gid.
            char l[24]; int ll = snprintf(l, sizeof(l), "L|%s|%s|%s", from_box, me, gid);
            vTaskDelay(pdMS_TO_TICKS(slot_ms() * (1 + esp_random() % 4)));
            send_now((uint8_t *)l, ll);
        }
        return;
    }
    if (type == 'L') {                                   // L|to|from|gid: the group key arrived.
        if (len != 16 || p[4] != '|' || p[9] != '|') return;
        p[4] = '\0'; p[9] = '\0';
        if (strcmp(p + 5, me) == 0) return;
        peer_heard(p + 5, NULL, rssi, snr);
        if (strcmp(p, me) == 0) groups_key_got(p + 10, p + 5);
        return;
    }
    if (type == 'Q') {                                   // Q|gid|from|<sealed with group key: seq|devicename|from|text>.
        if (len < 12 + SEAL_OVERHEAD || p[4] != '|' || p[9] != '|') return;
        p[4] = '\0'; p[9] = '\0';
        static char gid[5], from_box[5];
        strlcpy(gid, p, sizeof(gid));
        strlcpy(from_box, p + 5, sizeof(from_box));
        if (strcmp(from_box, me) == 0) return;
        int n = groups_open(gid, from_box, (uint8_t *)p + 10, len - 12, (uint8_t *)inner, sizeof(inner) - 1);
        if (n <= 0) { peer_heard(from_box, NULL, rssi, snr); return; }   // Another group: just note the box as heard.
        inner[n] = '\0';
        static char as_m[LORA_MAX_PAYLOAD + 16];
        snprintf(as_m, sizeof(as_m), "%s|%s", from_box, inner);
        p = as_m;
        group = gid;
        type = 'M';
    }

    if (type == 'M') {                                   // M|id|seq|devicename|from|text.
        char *id = p;
        char *seq = strchr(id, '|');   if (!seq)  return; *seq++  = '\0';
        char *dn  = strchr(seq, '|');  if (!dn)   return; *dn++   = '\0';
        char *from = strchr(dn, '|');  if (!from) return; *from++ = '\0';
        char *text = strchr(from, '|'); if (!text) return; *text++ = '\0';
        if (strcmp(id, me) == 0) return;                 // Own packet: ignore.
        uint16_t s = (uint16_t)strtoul(seq, NULL, 10);

        if (ack_and_repeat(id, s, dn, rssi, snr)) return;

        chat_set_origin(chat_add_peer(from, dn, text, 0, rssi, snr, peer, group), id, s);
        io_notify((strstr(from, "SOS") || strncmp(text, "SOS", 3) == 0) ? IO_EVT_SOS : IO_EVT_MESSAGE);
        printf("RX #%u from %s (%s): %s  [%d dBm, %.1f dB]\n", s, from, dn, text, rssi, snr);
    }
    else if (type == 'A') {                              // A|to|seq|from.
        char *to = p;
        char *seq = strchr(to, '|');  if (!seq) return; *seq++ = '\0';
        char *by  = strchr(seq, '|'); if (!by)  return; *by++  = '\0';
        if (strcmp(by, me) == 0) return;
        int i = peer_heard(by, NULL, rssi, snr);         // An ACK also shows that the box is in range.
        if (strcmp(to, me) != 0) return;                 // ACK for another box's message.
        if (!s_pend.active || (uint16_t)strtoul(seq, NULL, 10) != s_pend.seq) return;   // Old or repeated ACK.
        s_pend.acked |= 1u << i;
        s_pend.need  |= 1u << i;                         // A box that was not expected counts too.
        uint8_t acks = count_bits(s_pend.acked), need = count_bits(s_pend.need);
        if ((s_pend.acked & s_pend.need) == s_pend.need) {
            set_status(s_pend.log_id, 3, acks, need);    // Everyone answered: delivered.
            s_pend.active = 0;
        } else {
            set_status(s_pend.log_id, 1, acks, need);    // Some answered: sent, for example "1 of 2".
        }
    }
    else {                                               // H|id|devicename.
        char *id = p;
        char *dn = strchr(id, '|'); if (!dn) return; *dn++ = '\0';
        if (strcmp(id, me) == 0) return;
        peer_heard(id, dn, rssi, snr);                   // A heartbeat gets no ACK and no log entry.
    }
}

static void radio_task(void *arg)
{
    radio_start_rx();
    s_next_hb = xTaskGetTickCount() + pdMS_TO_TICKS(5000 + esp_random() % 5000);
    while (1) {
        TickType_t now = xTaskGetTickCount();

        // 1. Apply changed settings.
        uint8_t poke;
        if (xQueueReceive(s_cfgq, &poke, 0) == pdTRUE) {
            radio_apply(&device_settings);
            radio_start_rx();
        }

        // 2. Handle a received packet.
        uint8_t buf[LORA_MAX_PAYLOAD + 1];
        int rssi; float snr;
        int n = radio_check_rx(buf, sizeof(buf) - 1, &rssi, &snr);
        if (n > 0) {
            uint8_t plain[LORA_MAX_PAYLOAD + 1];
            int pl = crypto_unwrap(buf, n, plain, sizeof(plain) - 1);   // -1 = not from this network: ignore.
            if (pl > 0) {
                plain[pl] = '\0';
                handle_packet((char *)plain, pl, rssi, snr);
            }
        }

        // 3. Send the waiting ACK when its slot arrives.
        if (s_ack.active && (int32_t)(now - s_ack.at) >= 0) {
            send_now((uint8_t *)s_ack.pkt, strlen(s_ack.pkt));
            s_ack.active = 0;
        }

        // 4. Send the pending message, or send it again when the wait for ACKs is over.
        if (s_pend.active && s_log[s_pend.log_id % CHAT_MAX].gone) s_pend.active = 0;   // Unsent: stop trying.
        if (s_pend.active && (int32_t)(now - s_pend.next_try) >= 0) {
            uint8_t acks = count_bits(s_pend.acked), need = count_bits(s_pend.need);
            if (s_pend.tries >= 3) {                     // Give up after 3 tries.
                set_status(s_pend.log_id, acks ? 1 : 2, acks, need);   // Some answered: stays "sent". None: failed.
                s_pend.active = 0;
            } else {
                esp_err_t err = send_now(s_pend.pkt, s_pend.len);
                s_last_name_tx = now;
                s_pend.tries++;
                if (s_pend.tries == 1) set_status(s_pend.log_id, err == ESP_OK ? 1 : 2, acks, need);
                uint32_t wait = 1500 + 10 * slot_ms() + esp_random() % 500;
                s_pend.next_try = xTaskGetTickCount() + pdMS_TO_TICKS(wait);
            }
        }

        // 5. Take the next message when none is waiting for ACKs.
        uint32_t mid;
        if (!s_pend.active && !voice_air_busy() && xQueueReceive(s_txq, &mid, 0) == pdTRUE) {
            xSemaphoreTake(s_lock, portMAX_DELAY);
            chat_msg_t copy = s_log[mid % CHAT_MAX];
            if (copy.id == mid && !copy.gone) {          // Store its number so it can be unsent later.
                s_seq++;
                s_log[mid % CHAT_MAX].src = box_num(node_id_str());
                s_log[mid % CHAT_MAX].seq = s_seq;
            }
            xSemaphoreGive(s_lock);
            if (copy.id != mid || copy.gone) continue;   // Unsent before it left: nothing to send.
            int max = crypto_locked() ? LORA_MAX_PAYLOAD - CRYPTO_OVERHEAD : (int)sizeof(s_pend.pkt) - 1;
            int len;
            uint8_t need;
            if (copy.e2e) {                              // Person: E|to box|me|seq|to pid|from pid|encrypted bytes.
                len = snprintf((char *)s_pend.pkt, sizeof(s_pend.pkt), "E|%.4s|%.4s|%u|%.4s|%.4s|",
                               copy.peer, node_id_str(), s_seq, copy.to_pid, copy.from_pid);
                int bl = b64_decode(copy.text, s_pend.pkt + len, max - len);
                if (bl < 28) { set_status(mid, 2, 0, 0); continue; }   // Too long or damaged: failed.
                len += bl;
                xSemaphoreTake(s_lock, portMAX_DELAY);
                need = 1u << peer_slot(copy.peer);
                xSemaphoreGive(s_lock);
            } else if (copy.group[0]) {                         // Group: Q|gid|me|sealed(seq|devicename|from|text).
                char in[LORA_MAX_PAYLOAD];
                int il = snprintf(in, sizeof(in), "%u|%s|%s|%s", s_seq, copy.devicename, copy.from, copy.text);
                int imax = max - 12 - SEAL_OVERHEAD;
                if (il > imax) il = imax;
                len = snprintf((char *)s_pend.pkt, sizeof(s_pend.pkt), "Q|%s|%s|", copy.group, node_id_str());
                int sl = groups_seal(copy.group, node_id_str(), (uint8_t *)in, il, s_pend.pkt + len, sizeof(s_pend.pkt) - len);
                char ids[GROUP_MAX_MEMBERS][5];
                int nm = groups_members_of(copy.group, ids, GROUP_MAX_MEMBERS);
                if (sl < 0 || nm < 0) { set_status(mid, 2, 0, 0); continue; }   // No longer in the group: failed.
                len += sl;
                need = 0;
                xSemaphoreTake(s_lock, portMAX_DELAY);
                for (int k = 0; k < nm; k++) need |= 1u << peer_slot(ids[k]);   // Every member must answer.
                xSemaphoreGive(s_lock);
            } else if (copy.peer[0]) {                   // Private: P|to|me|sealed(seq|devicename|from|text).
                char in[LORA_MAX_PAYLOAD];
                int il = snprintf(in, sizeof(in), "%u|%s|%s|%s", s_seq, copy.devicename, copy.from, copy.text);
                int imax = max - 12 - SEAL_OVERHEAD;
                if (il > imax) il = imax;                // Too long: send only what fits.
                len = snprintf((char *)s_pend.pkt, sizeof(s_pend.pkt), "P|%s|%s|", copy.peer, node_id_str());
                int sl = friends_seal(copy.peer, (uint8_t *)in, il, s_pend.pkt + len, sizeof(s_pend.pkt) - len);
                if (sl < 0) { set_status(mid, 2, 0, 0); continue; }   // Not a friend (any more): failed.
                len += sl;
                xSemaphoreTake(s_lock, portMAX_DELAY);
                need = 1u << peer_slot(copy.peer);       // Only that box must answer.
                xSemaphoreGive(s_lock);
            } else {
                len = snprintf((char *)s_pend.pkt, sizeof(s_pend.pkt), "M|%s|%u|%s|%s|%s",
                               node_id_str(), s_seq, copy.devicename, copy.from, copy.text);
                if (len > max) len = max;                // Too long: send only what fits in one packet.
                need = peers_in_range();                 // Every box heard in the last 90 s must answer.
            }
            s_pend.len = len;
            s_pend.log_id = mid;
            s_pend.seq = s_seq;
            s_pend.tries = 0;
            s_pend.acked = 0;
            s_pend.need = need;
            s_pend.next_try = now;                       // Step 4 sends it on the next loop pass.
            s_pend.active = 1;
        }

        // 6. Send a waiting friend request or accept, group key, or U packet.
        if (!s_pend.active && !s_ack.active) {
            char fp[LORA_MAX_PAYLOAD];
            int max = crypto_locked() ? LORA_MAX_PAYLOAD - CRYPTO_OVERHEAD : (int)sizeof(fp);
            int fl = friends_next_packet(fp, max);
            if (fl <= 0) fl = groups_next_packet(fp, max);
            if (fl <= 0) fl = people_next_packet(fp, max);      // Tells friend boxes who uses this box.
            if (fl > 0) { send_now((uint8_t *)fp, fl); s_last_name_tx = now; }
        }

        // 7. Send a waiting unsend packet, or its second copy.
        if (!s_pend.active && !s_ack.active && !voice_air_busy()) {
            unsend_t u;
            if (s_unx.left && (int32_t)(now - s_unx.at) >= 0) {
                send_now((uint8_t *)s_unx.pkt, s_unx.len); s_unx.left = 0;
            } else if (!s_unx.left && xQueueReceive(s_unq, &u, 0) == pdTRUE) {
                s_unx.len = snprintf(s_unx.pkt, sizeof(s_unx.pkt), "X|%s|%s", node_id_str(), u.items);
                send_now((uint8_t *)s_unx.pkt, s_unx.len);
                s_unx.left = 1; s_unx.at = now + pdMS_TO_TICKS(3000);
                s_last_name_tx = now;
            }
        }

        // 8. Send a waiting SOS to Meshtastic and MeshCore boxes. This retunes the radio for about 1.5 s.
        if (!s_pend.active && !s_ack.active) sos_radio_step();

        // 9. Send a voice chunk or a voice answer. Text has priority: voice goes only when nothing else waits.
        if (!s_pend.active && !s_ack.active) {
            uint8_t vp[LORA_MAX_PAYLOAD];
            int max = crypto_locked() ? LORA_MAX_PAYLOAD - CRYPTO_OVERHEAD : (int)sizeof(vp);
            int vl = voice_next_packet(vp, max);
            if (vl > 0) { send_now(vp, vl); s_last_name_tx = now; }
        }

        // 10. Send a heartbeat when one is due and nothing else was sent for 30 s.
        if (!s_pend.active && !s_ack.active && !voice_air_busy() && (int32_t)(now - s_next_hb) >= 0) {
            if (s_last_name_tx == 0 || now - s_last_name_tx >= pdMS_TO_TICKS(30000)) {   // 0 = nothing sent yet.
                char hb[32];
                int len = snprintf(hb, sizeof(hb), "H|%s|%s", node_id_str(), device_settings.devicename);
                send_now((uint8_t *)hb, len);
                s_last_name_tx = now;
            }
            s_next_hb = now + pdMS_TO_TICKS(30000 + esp_random() % 5000);
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void chat_start(void)
{
    s_boot = esp_random();
    s_seq  = (uint16_t)esp_random();     // Start the message counter at a random number.
    s_lock = xSemaphoreCreateMutex();
    s_txq  = xQueueCreate(20, sizeof(uint32_t));
    s_cfgq = xQueueCreate(1, sizeof(uint8_t));
    s_unq  = xQueueCreate(2, sizeof(unsend_t));
    xTaskCreate(radio_task, "radio", 6144, NULL, 5, NULL);
}

uint32_t chat_send(const char *from, const char *text)
{
    return chat_send_to(from, text, "");
}

static uint32_t queue_send(const char *from, const char *text, const char *peer, const char *group)
{
    if (!s_lock || !s_txq) return 0;                 // Chat is not started (the radio failed): do nothing.
    uint32_t id = chat_add_peer(from, device_settings.devicename, text, 1, 0, 0, peer, group);
    if (xQueueSend(s_txq, &id, 0) != pdTRUE) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_log[id % CHAT_MAX].st = 2;
        xSemaphoreGive(s_lock);
    }
    return id;
}

uint32_t chat_send_to(const char *from, const char *text, const char *peer)
{
    return queue_send(from, text, peer, "");
}

uint32_t chat_send_group(const char *from, const char *text, const char *gid)
{
    return queue_send(from, text, "", gid);
}

uint32_t chat_send_e2e(const char *to_box, const char *to_pid, const char *from_pid, const char *blob)
{
    if (!s_lock || !s_txq) return 0;
    bool here = strcmp(to_box, node_id_str()) == 0;      // Both people use this box: no radio needed.
    uint32_t id = add_e2e(to_box, device_settings.devicename, to_pid, from_pid, blob, 1, 0, 0);
    if (here) { set_status(id, 3, 0, 0); return id; }
    if (xQueueSend(s_txq, &id, 0) != pdTRUE) set_status(id, 2, 0, 0);
    return id;
}

// ---------- For app_voice.c ----------
uint32_t chat_add_e2e_voice(const char *box, const char *devicename, const char *to_pid, const char *from_pid,
                            uint8_t secs, uint8_t local, int16_t rssi, float snr)
{
    if (!s_lock) return 0;
    uint32_t id = add_e2e(box, devicename, to_pid, from_pid, "", local, rssi, snr);   // No text: the clip is the message.
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_log[id % CHAT_MAX].voice = secs;
    if (local) s_log[id % CHAT_MAX].st = 0;              // Status "sending".
    xSemaphoreGive(s_lock);
    if (!local) io_notify(IO_EVT_MESSAGE);
    return id;
}
uint32_t chat_add_voice(const char *from, const char *devicename, const char *text, uint8_t secs,
                        uint8_t local, int16_t rssi, float snr, const char *peer, const char *group)
{
    if (!s_lock) return 0;
    uint32_t id = chat_add_peer(from, devicename, text, local, rssi, snr, peer, group);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_log[id % CHAT_MAX].voice = secs;
    if (local) s_log[id % CHAT_MAX].st = 0;              // Status "sending".
    xSemaphoreGive(s_lock);
    if (!local) io_notify(IO_EVT_MESSAGE);
    return id;
}
void chat_set_status(uint32_t id, uint8_t st, uint8_t acks, uint8_t need) { set_status(id, st, acks, need); }

void chat_set_origin(uint32_t id, const char *box, uint16_t seq)
{
    if (!id || !s_lock) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    chat_msg_t *m = &s_log[id % CHAT_MAX];
    if (m->id == id) { m->src = box_num(box); m->seq = seq; }
    xSemaphoreGive(s_lock);
}

int chat_unsend(const uint32_t *ids, int n, const char *from, const char *pid)
{
    if (!s_lock || !s_unq) return 0;
    if (uxQueueSpacesAvailable(s_unq) == 0) return -1;   // The previous unsend is still going out.
    unsend_t u = { 0 };
    uint32_t voices[16]; int nv = 0, done = 0, il = 0;
    if (n > 16) n = 16;                                  // The web page sends at most 16 at a time.
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int k = 0; k < n; k++) {
        chat_msg_t *m = &s_log[ids[k] % CHAT_MAX];
        if (!ids[k] || m->id != ids[k] || m->gone || !m->local) continue;          // Gone, or not sent from this box.
        bool mine = m->e2e ? (pid && pid[0] && strcasecmp(m->from_pid, pid) == 0)
                           : (from && from[0] && strcmp(m->from, from) == 0);
        if (!mine) continue;                                                     // Another phone's message.
        if (m->seq && il < UNSEND_ITEMS - 7)                                      // Transmitted: tell the other boxes.
            il += snprintf(u.items + il, sizeof(u.items) - il, "%s%s%X", il ? "," : "", m->voice ? "v" : "", m->seq);
        if (m->voice) voices[nv++] = m->id;
        mark_gone(m); done++;
    }
    xSemaphoreGive(s_lock);
    for (int i = 0; i < nv; i++) voice_forget(voices[i]);
    if (il) xQueueSend(s_unq, &u, 0);
    if (done) printf("unsend: %d message(s), %s\n", done, il ? "told the other boxes" : "none had left yet");
    return done;
}
uint32_t chat_version(void) { return s_version; }
int chat_peer_slot(const char *id)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int i = peer_slot(id);
    xSemaphoreGive(s_lock);
    return i;
}
int chat_peer_heard(const char *id, int rssi, float snr) { return peer_heard(id, NULL, rssi, snr); }
uint8_t chat_peers_in_range(void) { return peers_in_range(); }
uint32_t chat_slot_ms(void) { return slot_ms(); }

// LoRa time on air in ms for one packet of len bytes (+30 when locked). Assumes CR 4/5, 8 preamble symbols, CRC on.
uint32_t chat_toa_ms(int len)
{
    if (crypto_locked()) len += CRYPTO_OVERHEAD;
    int sf = device_settings.sf, bw = device_settings.bw_khz;
    float ts = (float)(1 << sf) / bw;                      // Duration of one symbol in ms.
    int de = ts > 16.0f;                                   // Low data rate optimization (SF11 and SF12 at 125 kHz).
    int num = 8 * len - 4 * sf + 28 + 16;
    int den = 4 * (sf - 2 * de);
    int n = num > 0 ? (num + den - 1) / den * 5 : 0;
    return (uint32_t)((12.25f + 8 + n) * ts + 1);
}

void chat_apply_settings(void)
{
    uint8_t poke = 1;
    xQueueOverwrite(s_cfgq, &poke);
}

static const char *st_word(uint8_t st)
{
    if (st == 1) return "sent";
    if (st == 2) return "fail";
    if (st == 3) return "delivered";
    return "sending";
}

void chat_write_json(void *req, uint32_t since, const char *pid)
{
    const char *me = node_id_str();
    httpd_req_t *r = (httpd_req_t *)req;
    httpd_resp_set_type(r, "application/json");

    xSemaphoreTake(s_lock, portMAX_DELAY);
    uint32_t newest = s_next_id - 1;
    xSemaphoreGive(s_lock);

    char head[64];
    snprintf(head, sizeof(head), "{\"boot\":%lu,\"next\":%lu,\"msgs\":[",
             (unsigned long)s_boot, (unsigned long)newest);
    httpd_resp_sendstr_chunk(r, head);

    uint32_t oldest = (newest > CHAT_MAX) ? newest - CHAT_MAX + 1 : 1;
    int first = 1;
    for (uint32_t id = oldest; id <= newest; id++) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        chat_msg_t m = s_log[id % CHAT_MAX];
        xSemaphoreGive(s_lock);
        if (m.id != id || m.gone) continue;
        int rule_a = (m.id > since);
        int rule_b = (m.local == 1 && m.id + 5 > newest);
        if (!rule_a && !rule_b) continue;
        if (m.e2e) {                                     // Person message: only for the two phones involved.
            bool here = strcmp(m.peer, me) == 0;
            bool mine = pid[0] && (m.local ? strcasecmp(m.from_pid, pid) == 0 : strcasecmp(m.to_pid, pid) == 0);
            if (here && pid[0] && strcasecmp(m.to_pid, pid) == 0) mine = true;
            if (!mine) continue;
        }
        char f[26], d[34], t[302];
        json_escape(f, sizeof(f), m.from);
        json_escape(d, sizeof(d), m.devicename);
        json_escape(t, sizeof(t), m.text);
        char one[720];
        snprintf(one, sizeof(one),
            "%s{\"id\":%lu,\"from\":\"%s\",\"devicename\":\"%s\",\"local\":%u,"
            "\"text\":\"%s\",\"rssi\":%d,\"snr\":%.1f,\"st\":\"%s\",\"acks\":%u,\"need\":%u,\"peer\":\"%.4s\",\"group\":\"%.4s\",\"voice\":%u,"
            "\"e2e\":%u,\"to_pid\":\"%.4s\",\"from_pid\":\"%.4s\"}",
            first ? "" : ",",
            (unsigned long)m.id, f, d, m.local, t, m.rssi, m.snr, st_word(m.st), m.acks, m.need, m.peer, m.group, m.voice,
            m.e2e, m.to_pid, m.from_pid);
        httpd_resp_sendstr_chunk(r, one);
        first = 0;
    }

    // Boxes in range.
    peer_t copy[MAX_PEERS];
    xSemaphoreTake(s_lock, portMAX_DELAY);
    memcpy(copy, s_peers, sizeof(copy));
    xSemaphoreGive(s_lock);
    TickType_t now = xTaskGetTickCount();
    // Recently unsent ids: phones that still show these messages remove them.
    httpd_resp_sendstr_chunk(r, "],\"gone\":[");
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int gn = s_gone_n < GONE_MAX ? s_gone_n : GONE_MAX;
    uint32_t gone[GONE_MAX]; memcpy(gone, s_gone, sizeof(gone));
    xSemaphoreGive(s_lock);
    for (int i = 0; i < gn; i++) {
        snprintf(head, sizeof(head), "%s%lu", i ? "," : "", (unsigned long)gone[i]);
        httpd_resp_sendstr_chunk(r, head);
    }
    snprintf(head, sizeof(head), "],\"sf\":%u,\"peers\":[", device_settings.sf);
    httpd_resp_sendstr_chunk(r, head);               // The leading "]" closes the gone list.
    first = 1;
    for (int i = 0; i < MAX_PEERS; i++) {
        uint32_t age_ms = (now - copy[i].heard) * portTICK_PERIOD_MS;
        if (!copy[i].used || age_ms >= PEER_GONE_MS) continue;
        char d[34], one[128];
        json_escape(d, sizeof(d), copy[i].devicename[0] ? copy[i].devicename : copy[i].id);
        snprintf(one, sizeof(one), "%s{\"id\":\"%s\",\"devicename\":\"%s\",\"rssi\":%d,\"snr\":%.1f,\"age_s\":%lu}",
                 first ? "" : ",", copy[i].id, d, copy[i].rssi, copy[i].snr, (unsigned long)(age_ms / 1000));
        httpd_resp_sendstr_chunk(r, one);
        first = 0;
    }
    httpd_resp_sendstr_chunk(r, "]}");
    httpd_resp_sendstr_chunk(r, NULL);
}

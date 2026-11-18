// People: tracks which person (phone) uses which box, so that two people can chat privately.
//
// Example: Liam's phone is on the Wi-Fi of Sara's box, and Sara's box and Alex's box are friends.
//   1. Liam's phone makes the id "7c21" and a key pair, and sends POST /api/me.
//   2. Sara's box sends Alex's box  U|<alex>|<sara>|sealed("7c21|1|Liam|<pub>"), sealed with the friend key.
//   3. Alex's page lists Liam. The phones encrypt the message with a key the boxes do not have
//      (X25519 of their two keys).
// U packets go out when a phone joins or changes its name or key, when it goes online or offline, when a
// friend box is heard again after 5 minutes of silence, and every 15 minutes for everyone (U has no ACK).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "app_people.h"
#include "app_friends.h"
#include "app_settings.h"

#define ONLINE_MS     (3 * 60 * 1000)   // No poll for 3 min means offline (the page stops polling when hidden).
#define REMOTE_OLD_MS (20 * 60 * 1000)  // No U packet for 20 min: show the person as offline.
#define AGAIN_MS      (5 * 60 * 1000)   // Friend box heard again after 5 min: send it everything again.
#define REFRESH_MS    (15 * 60 * 1000)
#define GAP_MS        2000              // At most one U packet every 2 s, so chat goes first.

// forget: a "forget me" request that the friend boxes still have to be told about.
typedef struct { uint8_t used, online_sent, forget; char pid[5], name[13], pub[65]; TickType_t seen; } local_t;
typedef struct { uint8_t used, online; char box[5], pid[5], name[13], pub[65]; TickType_t heard; } remote_t;
typedef struct { char box[5]; uint8_t need; TickType_t heard; uint8_t ever; } fbox_t;   // need: bit i = send s_loc[i].

static local_t  s_loc[PEOPLE_LOCAL_MAX];
static remote_t s_rem[PEOPLE_REMOTE_MAX];
static fbox_t   s_fb[MAX_FRIENDS];
static SemaphoreHandle_t s_mu;
static TickType_t s_last_tx, s_last_full;

// ---------- Kept in NVS ----------
// The people on this box and on friend boxes (id, name, public key) survive a restart. Without this, private
// sends fail with "Don't know that person" until the phones and friend boxes announce themselves again,
// which can take 15 minutes. Online state and last-heard times are not kept: everyone starts as offline.
// The lists are saved at most every 30 s, and only after a change, to limit flash wear.
typedef struct { char pid[5], name[13], pub[65]; } saved_loc_t;
typedef struct { char box[5], pid[5], name[13], pub[65]; } saved_rem_t;
#define SAVE_GAP_MS 30000
static bool s_dirty;
static TickType_t s_last_save;

static void load(void)                   // Runs at startup, before any other task can touch the lists.
{
    nvs_handle_t h;
    if (nvs_open("lora", NVS_READONLY, &h) != ESP_OK) return;
    size_t len = 0; int nl = 0, nr = 0;
    if (nvs_get_blob(h, "ppl_loc", NULL, &len) == ESP_OK && len && len % sizeof(saved_loc_t) == 0
        && len <= sizeof(saved_loc_t) * PEOPLE_LOCAL_MAX) {
        saved_loc_t *b = malloc(len);
        if (b && nvs_get_blob(h, "ppl_loc", b, &len) == ESP_OK)
            for (int i = 0; i < (int)(len / sizeof(*b)); i++) {
                local_t *l = &s_loc[nl++];
                l->used = 1;
                strlcpy(l->pid, b[i].pid, sizeof(l->pid)); strlcpy(l->name, b[i].name, sizeof(l->name)); strlcpy(l->pub, b[i].pub, sizeof(l->pub));
            }
        free(b);
    }
    len = 0;
    if (nvs_get_blob(h, "ppl_rem", NULL, &len) == ESP_OK && len && len % sizeof(saved_rem_t) == 0
        && len <= sizeof(saved_rem_t) * PEOPLE_REMOTE_MAX) {
        saved_rem_t *b = malloc(len);
        if (b && nvs_get_blob(h, "ppl_rem", b, &len) == ESP_OK)
            for (int i = 0; i < (int)(len / sizeof(*b)); i++) {
                remote_t *r = &s_rem[nr++];
                r->used = 1;
                strlcpy(r->box, b[i].box, sizeof(r->box)); strlcpy(r->pid, b[i].pid, sizeof(r->pid));
                strlcpy(r->name, b[i].name, sizeof(r->name)); strlcpy(r->pub, b[i].pub, sizeof(r->pub));
            }
        free(b);
    }
    nvs_close(h);
    if (nl || nr) printf("people: %d on this box, %d on friend boxes (kept from before the restart)\n", nl, nr);
}

static void save_if_due(TickType_t now)  // Radio task. Call without s_mu held.
{
    if (!s_dirty || (s_last_save && now - s_last_save < pdMS_TO_TICKS(SAVE_GAP_MS))) return;
    saved_loc_t *bl = calloc(PEOPLE_LOCAL_MAX, sizeof(*bl));
    saved_rem_t *br = calloc(PEOPLE_REMOTE_MAX, sizeof(*br));    // About 2.8 KB for a moment, then freed.
    if (!bl || !br) { free(bl); free(br); return; }              // Low on RAM: try again later.
    int nl = 0, nr = 0;
    xSemaphoreTake(s_mu, portMAX_DELAY);
    for (int k = 0; k < PEOPLE_LOCAL_MAX; k++) if (s_loc[k].used && !s_loc[k].forget) {
        strlcpy(bl[nl].pid, s_loc[k].pid, sizeof(bl[nl].pid)); strlcpy(bl[nl].name, s_loc[k].name, sizeof(bl[nl].name));
        strlcpy(bl[nl].pub, s_loc[k].pub, sizeof(bl[nl].pub)); nl++;
    }
    for (int k = 0; k < PEOPLE_REMOTE_MAX; k++) if (s_rem[k].used) {
        strlcpy(br[nr].box, s_rem[k].box, sizeof(br[nr].box)); strlcpy(br[nr].pid, s_rem[k].pid, sizeof(br[nr].pid));
        strlcpy(br[nr].name, s_rem[k].name, sizeof(br[nr].name)); strlcpy(br[nr].pub, s_rem[k].pub, sizeof(br[nr].pub)); nr++;
    }
    s_dirty = false;
    xSemaphoreGive(s_mu);
    nvs_handle_t h;
    if (nvs_open("lora", NVS_READWRITE, &h) == ESP_OK) {
        if (nl) nvs_set_blob(h, "ppl_loc", bl, nl * sizeof(*bl)); else nvs_erase_key(h, "ppl_loc");
        if (nr) nvs_set_blob(h, "ppl_rem", br, nr * sizeof(*br)); else nvs_erase_key(h, "ppl_rem");
        nvs_commit(h);
        nvs_close(h);
    }
    free(bl); free(br);
    s_last_save = now;
}

void people_init(void) { s_mu = xSemaphoreCreateMutex(); load(); }

static bool is_hex(const char *s, int n)
{
    for (int i = 0; i < n; i++) {
        char c = s[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) return false;
    }
    return s[n] == '\0';
}

static uint8_t used_mask(void)
{
    uint8_t m = 0;
    for (int i = 0; i < PEOPLE_LOCAL_MAX; i++) if (s_loc[i].used) m |= 1u << i;
    return m;
}

static void tell_all_friends(uint8_t bits) { for (int f = 0; f < MAX_FRIENDS; f++) if (s_fb[f].box[0]) s_fb[f].need |= bits; }

// seen == 0: the entry was restored after a restart and has not polled since, so it counts as offline.
static bool online_now(const local_t *l, TickType_t now) { return l->seen && now - l->seen < pdMS_TO_TICKS(ONLINE_MS); }

// ---------- Web task ----------
esp_err_t people_register(const char *pid, const char *name, const char *pubhex)
{
    if (!s_mu || !is_hex(pid, 4) || !is_hex(pubhex, 64) || !name[0] || strlen(name) > 12 || strchr(name, '|')) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(s_mu, portMAX_DELAY);
    TickType_t now = xTaskGetTickCount();
    int i = -1, oldest = 0;
    for (int k = 0; k < PEOPLE_LOCAL_MAX; k++) {
        if (s_loc[k].used && strcasecmp(s_loc[k].pid, pid) == 0) { i = k; break; }
        if (!s_loc[k].used && i < 0) i = -2 - k;                                  // Remember the first free slot.
        if (s_loc[k].seen < s_loc[oldest].seen) oldest = k;
    }
    bool changed = true;
    if (i >= 0) changed = s_loc[i].forget || strcmp(s_loc[i].name, name) != 0 || strcasecmp(s_loc[i].pub, pubhex) != 0;
    else i = (i <= -2) ? -2 - i : oldest;                                          // Full: reuse the quietest slot.
    if (changed) {
        memset(&s_loc[i], 0, sizeof(s_loc[i]));
        s_loc[i].used = 1;
        strlcpy(s_loc[i].pid, pid, sizeof(s_loc[i].pid));
        strlcpy(s_loc[i].name, name, sizeof(s_loc[i].name));
        strlcpy(s_loc[i].pub, pubhex, sizeof(s_loc[i].pub));
        for (char *c = s_loc[i].pid; *c; c++) if (*c >= 'A' && *c <= 'F') *c += 32;   // Store hex in lower case.
        for (char *c = s_loc[i].pub; *c; c++) if (*c >= 'A' && *c <= 'F') *c += 32;
        s_loc[i].online_sent = 1;
        tell_all_friends(1u << i);
        s_dirty = true;
    }
    s_loc[i].seen = now;
    xSemaphoreGive(s_mu);
    if (changed) printf("people: %s (%s) uses this box\n", name, pid);
    return ESP_OK;
}

void people_seen(const char *pid)
{
    if (!s_mu || !pid[0]) return;
    xSemaphoreTake(s_mu, portMAX_DELAY);
    for (int k = 0; k < PEOPLE_LOCAL_MAX; k++)
        if (s_loc[k].used && strcasecmp(s_loc[k].pid, pid) == 0) s_loc[k].seen = xTaskGetTickCount();
    xSemaphoreGive(s_mu);
}

int people_list(person_info_t *out, int max)
{
    if (!s_mu) return 0;
    int n = 0;
    TickType_t now = xTaskGetTickCount();
    xSemaphoreTake(s_mu, portMAX_DELAY);
    for (int k = 0; k < PEOPLE_LOCAL_MAX && n < max; k++) {
        if (!s_loc[k].used || s_loc[k].forget) continue;
        person_info_t *p = &out[n++];
        strlcpy(p->box, node_id_str(), sizeof(p->box));
        strlcpy(p->pid, s_loc[k].pid, sizeof(p->pid));
        strlcpy(p->name, s_loc[k].name, sizeof(p->name));
        strlcpy(p->pub, s_loc[k].pub, sizeof(p->pub));
        p->online = online_now(&s_loc[k], now);
        p->local = 1;
        p->age_s = (now - s_loc[k].seen) * portTICK_PERIOD_MS / 1000;
    }
    for (int k = 0; k < PEOPLE_REMOTE_MAX && n < max; k++) {
        if (!s_rem[k].used || !friends_is_friend(s_rem[k].box)) continue;         // Former friend box: hide its people.
        person_info_t *p = &out[n++];
        strlcpy(p->box, s_rem[k].box, sizeof(p->box));
        strlcpy(p->pid, s_rem[k].pid, sizeof(p->pid));
        strlcpy(p->name, s_rem[k].name, sizeof(p->name));
        strlcpy(p->pub, s_rem[k].pub, sizeof(p->pub));
        p->age_s = (now - s_rem[k].heard) * portTICK_PERIOD_MS / 1000;
        p->online = s_rem[k].online && p->age_s * 1000 < REMOTE_OLD_MS;
        p->local = 0;
    }
    xSemaphoreGive(s_mu);
    return n;
}

bool people_known(const char *box, const char *pid)
{
    if (!s_mu) return false;
    bool mine = strcmp(box, node_id_str()) == 0, found = false;
    xSemaphoreTake(s_mu, portMAX_DELAY);
    if (mine) { for (int k = 0; k < PEOPLE_LOCAL_MAX; k++) if (s_loc[k].used && !s_loc[k].forget && strcasecmp(s_loc[k].pid, pid) == 0) found = true; }
    else      { for (int k = 0; k < PEOPLE_REMOTE_MAX; k++) if (s_rem[k].used && strcmp(s_rem[k].box, box) == 0 && strcasecmp(s_rem[k].pid, pid) == 0) found = true; }
    xSemaphoreGive(s_mu);
    return found;
}

bool people_forget(const char *pid)
{
    if (!s_mu || !pid[0]) return false;
    bool found = false;
    xSemaphoreTake(s_mu, portMAX_DELAY);
    for (int k = 0; k < PEOPLE_LOCAL_MAX; k++)
        if (s_loc[k].used && !s_loc[k].forget && strcasecmp(s_loc[k].pid, pid) == 0) {
            s_loc[k].forget = 1; s_loc[k].seen = 0;
            tell_all_friends(1u << k);                           // Friend boxes get "pid|2|...", which means forget.
            found = true;
        }
    if (found) { s_dirty = true; s_last_save = 0; }              // Save at once, so the person leaves flash.
    xSemaphoreGive(s_mu);
    if (found) printf("people: %s asked to be forgotten on this box\n", pid);
    return found;
}

void people_clear_all(void)
{
    if (!s_mu) return;
    xSemaphoreTake(s_mu, portMAX_DELAY);
    memset(s_loc, 0, sizeof(s_loc)); memset(s_rem, 0, sizeof(s_rem)); memset(s_fb, 0, sizeof(s_fb));
    s_dirty = false;
    xSemaphoreGive(s_mu);
    nvs_handle_t h;
    if (nvs_open("lora", NVS_READWRITE, &h) == ESP_OK) { nvs_erase_key(h, "ppl_loc"); nvs_erase_key(h, "ppl_rem"); nvs_commit(h); nvs_close(h); }
}

// ---------- Radio task ----------
void people_box_heard(const char *box)
{
    if (!s_mu || !friends_is_friend(box)) return;
    TickType_t now = xTaskGetTickCount();
    xSemaphoreTake(s_mu, portMAX_DELAY);
    int f = -1;
    for (int k = 0; k < MAX_FRIENDS; k++) if (s_fb[k].box[0] && strcmp(s_fb[k].box, box) == 0) { f = k; break; }
    if (f < 0) for (int k = 0; k < MAX_FRIENDS; k++) if (!s_fb[k].box[0] || !friends_is_friend(s_fb[k].box)) { f = k; break; }
    if (f >= 0) {
        if (strcmp(s_fb[f].box, box) != 0) { memset(&s_fb[f], 0, sizeof(s_fb[f])); strlcpy(s_fb[f].box, box, sizeof(s_fb[f].box)); }
        // A new friend box, or one heard again after a silence, gets everything.
        if (!s_fb[f].ever || now - s_fb[f].heard > pdMS_TO_TICKS(AGAIN_MS)) s_fb[f].need = used_mask();
        s_fb[f].heard = now; s_fb[f].ever = 1;
    }
    xSemaphoreGive(s_mu);
}

void people_on_packet(const char *from_box, char *inner)      // inner is "pid|online|name|pubhex".
{
    char *f[4] = { inner };
    for (int k = 1; k < 4; k++) { f[k] = strchr(f[k - 1], '|'); if (!f[k]) return; *f[k]++ = '\0'; }
    if (!is_hex(f[0], 4) || !is_hex(f[3], 64) || !f[2][0] || strlen(f[2]) > 12) return;
    if (f[1][0] == '2') {                                        // "Forget me": drop the person here too.
        xSemaphoreTake(s_mu, portMAX_DELAY);
        for (int k = 0; k < PEOPLE_REMOTE_MAX; k++)
            if (s_rem[k].used && strcmp(s_rem[k].box, from_box) == 0 && strcasecmp(s_rem[k].pid, f[0]) == 0) { memset(&s_rem[k], 0, sizeof(s_rem[k])); s_dirty = true; }
        xSemaphoreGive(s_mu);
        printf("people: %s on box %s asked to be forgotten\n", f[0], from_box);
        return;
    }
    xSemaphoreTake(s_mu, portMAX_DELAY);
    int i = -1, oldest = 0;
    for (int k = 0; k < PEOPLE_REMOTE_MAX; k++) {
        if (s_rem[k].used && strcmp(s_rem[k].box, from_box) == 0 && strcasecmp(s_rem[k].pid, f[0]) == 0) { i = k; break; }
        if (!s_rem[k].used && i < 0) i = -2 - k;
        if (s_rem[k].heard < s_rem[oldest].heard) oldest = k;
    }
    if (i < 0) i = (i <= -2) ? -2 - i : oldest;
    remote_t *r = &s_rem[i];
    if (!r->used || strcmp(r->box, from_box) || strcasecmp(r->pid, f[0]) || strcmp(r->name, f[2]) || strcasecmp(r->pub, f[3])) s_dirty = true;
    r->used = 1; r->online = f[1][0] == '1'; r->heard = xTaskGetTickCount();
    strlcpy(r->box, from_box, sizeof(r->box));
    strlcpy(r->pid, f[0], sizeof(r->pid));
    strlcpy(r->name, f[2], sizeof(r->name));
    strlcpy(r->pub, f[3], sizeof(r->pub));
    xSemaphoreGive(s_mu);
    printf("people: %s (%s) is %s on box %s\n", f[2], f[0], f[1][0] == '1' ? "online" : "offline", from_box);
}

int people_next_packet(char *out, int max)
{
    if (!s_mu) return 0;
    TickType_t now = xTaskGetTickCount();
    save_if_due(now);
    if (s_last_tx && now - s_last_tx < pdMS_TO_TICKS(GAP_MS)) return 0;
    xSemaphoreTake(s_mu, portMAX_DELAY);
    for (int k = 0; k < PEOPLE_LOCAL_MAX; k++) {                // Online state changed since it was last sent.
        if (!s_loc[k].used || s_loc[k].forget) continue;
        uint8_t on = online_now(&s_loc[k], now);
        if (on != s_loc[k].online_sent) { s_loc[k].online_sent = on; tell_all_friends(1u << k); }
    }
    if (!s_last_full || now - s_last_full > pdMS_TO_TICKS(REFRESH_MS)) { s_last_full = now; tell_all_friends(used_mask()); }
    char box[5] = "", inner[96]; int il = 0;
    for (int f = 0; f < MAX_FRIENDS && !box[0]; f++) {
        if (!s_fb[f].need) continue;
        if (!friends_is_friend(s_fb[f].box)) { s_fb[f].need = 0; continue; }
        for (int k = 0; k < PEOPLE_LOCAL_MAX; k++) {
            if (!(s_fb[f].need & (1u << k))) continue;
            s_fb[f].need &= ~(1u << k);
            if (!s_loc[k].used) continue;
            il = snprintf(inner, sizeof(inner), "%.4s|%u|%.12s|%.64s", s_loc[k].pid, s_loc[k].forget ? 2 : s_loc[k].online_sent, s_loc[k].name, s_loc[k].pub);
            strlcpy(box, s_fb[f].box, sizeof(box));
            break;
        }
    }
    for (int k = 0; k < PEOPLE_LOCAL_MAX; k++) {                // Forgotten and every friend box told: free the slot.
        if (!s_loc[k].used || !s_loc[k].forget) continue;
        bool waiting = false;
        for (int f = 0; f < MAX_FRIENDS; f++) if (s_fb[f].need & (1u << k)) waiting = true;
        if (!waiting) memset(&s_loc[k], 0, sizeof(s_loc[k]));
    }
    xSemaphoreGive(s_mu);
    if (!box[0]) return 0;
    int n = snprintf(out, max, "U|%s|%s|", box, node_id_str());
    if (n < 0 || n >= max) return 0;
    int sl = friends_seal(box, (const uint8_t *)inner, il, (uint8_t *)out + n, max - n);
    if (sl < 0) return 0;
    s_last_tx = now;
    return n + sl;
}

// ---------- Base64url ----------
static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

int b64_encode(const uint8_t *in, int n, char *out, int max)
{
    int o = 0;
    for (int i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16 | (i + 1 < n ? (uint32_t)in[i + 1] << 8 : 0) | (i + 2 < n ? in[i + 2] : 0);
        int letters = (n - i >= 3) ? 4 : (n - i) + 1;            // 1 byte gives 2 letters, 2 bytes give 3.
        if (o + letters >= max) return -1;
        for (int k = 0; k < letters; k++) out[o++] = B64[(v >> (18 - 6 * k)) & 63];
    }
    out[o] = '\0';
    return o;
}

int b64_decode(const char *in, uint8_t *out, int max)
{
    uint32_t v = 0; int bits = 0, o = 0;
    for (; *in; in++) {
        const char *c = strchr(B64, *in);
        if (!c || !*in) return -1;
        v = v << 6 | (uint32_t)(c - B64); bits += 6;
        if (bits >= 8) { bits -= 8; if (o >= max) return -1; out[o++] = (uint8_t)(v >> bits); }
    }
    return o;
}

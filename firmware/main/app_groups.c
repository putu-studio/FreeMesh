// Groups. See app_groups.h for the packets.
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"          // xTaskGetTickCount.
#include "psa/crypto.h"
#include "nvs.h"
#include "esp_random.h"
#include "app_groups.h"
#include "app_friends.h"
#include "app_settings.h"

typedef struct {                    // Flash record of one group.
    char    gid[5];
    char    name[17];
    char    members[GROUP_MAX_MEMBERS][5];
    uint8_t count;
    char    owner[5];
    uint8_t key[16];
    uint8_t used;
} grp_t;

static grp_t        s_g[MAX_GROUPS];
static psa_key_id_t s_kid[MAX_GROUPS];
static uint8_t      s_todo[MAX_GROUPS];   // Bit i = the key still has to go to members[i].
static uint8_t      s_tries[MAX_GROUPS][GROUP_MAX_MEMBERS];   // Times the key was sent to each member.
static TickType_t   s_last_k;             // Time of the last K packet.
#define KEY_TRIES   5
#define KEY_GAP_MS  8000                  // Pause between K packets.
static SemaphoreHandle_t s_mu;

static int find(const char *gid)
{
    for (int i = 0; i < MAX_GROUPS; i++) if (s_g[i].used && strcmp(s_g[i].gid, gid) == 0) return i;
    return -1;
}

static void load_key(int i)
{
    if (s_kid[i]) { psa_destroy_key(s_kid[i]); s_kid[i] = 0; }
    if (!s_g[i].used) return;
    psa_key_attributes_t a = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&a, PSA_KEY_TYPE_AES);
    psa_set_key_bits(&a, 128);
    psa_set_key_usage_flags(&a, PSA_KEY_USAGE_ENCRYPT | PSA_KEY_USAGE_DECRYPT);
    psa_set_key_algorithm(&a, PSA_ALG_GCM);
    psa_import_key(&a, s_g[i].key, 16, &s_kid[i]);
}

static esp_err_t save(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open("lora", NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_blob(h, "groups", s_g, sizeof(s_g));
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

static bool good_id(const char *s) { return strlen(s) == 4; }

esp_err_t groups_init(void)
{
    s_mu = xSemaphoreCreateMutex();
    memset(s_g, 0, sizeof(s_g));
    nvs_handle_t h;
    if (nvs_open("lora", NVS_READONLY, &h) == ESP_OK) {
        size_t l = sizeof(s_g);
        if (nvs_get_blob(h, "groups", s_g, &l) != ESP_OK || l != sizeof(s_g)) memset(s_g, 0, sizeof(s_g));
        nvs_close(h);
    }
    int n = 0;
    for (int i = 0; i < MAX_GROUPS; i++) if (s_g[i].used) { load_key(i); n++; }
    printf("Groups: %d saved\n", n);
    return ESP_OK;
}

static void copy_info(const grp_t *g, group_info_t *o)
{
    memcpy(o->gid, g->gid, 5); memcpy(o->name, g->name, 17);
    memcpy(o->members, g->members, sizeof(o->members));
    o->count = g->count; memcpy(o->owner, g->owner, 5);
}

int groups_list(group_info_t *out, int max)
{
    int n = 0;
    xSemaphoreTake(s_mu, portMAX_DELAY);
    for (int i = 0; i < MAX_GROUPS && n < max; i++) if (s_g[i].used) copy_info(&s_g[i], &out[n++]);
    xSemaphoreGive(s_mu);
    return n;
}

bool groups_get(const char *gid, group_info_t *out)
{
    xSemaphoreTake(s_mu, portMAX_DELAY);
    int i = find(gid);
    if (i >= 0 && out) copy_info(&s_g[i], out);
    xSemaphoreGive(s_mu);
    return i >= 0;
}

esp_err_t groups_create(const char *name, const char ids[][5], int n, char gid_out[5])
{
    if (!name[0] || n < 1 || n > GROUP_MAX_MEMBERS) return ESP_ERR_INVALID_ARG;
    for (int k = 0; k < n; k++) if (!friends_is_friend(ids[k])) return ESP_ERR_NOT_FOUND;   // Members must be friends.
    xSemaphoreTake(s_mu, portMAX_DELAY);
    int i = -1;
    for (int k = 0; k < MAX_GROUPS; k++) if (!s_g[k].used) { i = k; break; }
    esp_err_t r = ESP_ERR_NO_MEM;
    if (i >= 0) {
        grp_t *g = &s_g[i];
        memset(g, 0, sizeof(*g));
        do { snprintf(g->gid, sizeof(g->gid), "%04x", (unsigned)(esp_random() & 0xFFFF)); } while (find(g->gid) >= 0 && find(g->gid) != i);
        strlcpy(g->name, name, sizeof(g->name));
        for (int k = 0; k < n; k++) strlcpy(g->members[k], ids[k], 5);
        g->count = n;
        strlcpy(g->owner, node_id_str(), sizeof(g->owner));
        for (int k = 0; k < 16; k += 4) { uint32_t v = esp_random(); memcpy(g->key + k, &v, 4); }   // Random group key.
        g->used = 1;
        load_key(i);
        s_todo[i] = (uint8_t)((1u << n) - 1);         // Send the key to every member.
        memset(s_tries[i], 0, sizeof(s_tries[i]));
        r = save();
        if (gid_out) memcpy(gid_out, g->gid, 5);
        printf("Groups: made \"%s\" (%s) with %d boxes\n", g->name, g->gid, n);
    }
    xSemaphoreGive(s_mu);
    return r;
}

esp_err_t groups_delete(const char *gid)
{
    xSemaphoreTake(s_mu, portMAX_DELAY);
    int i = find(gid);
    if (i >= 0) { memset(&s_g[i], 0, sizeof(grp_t)); s_todo[i] = 0; load_key(i); save(); }
    xSemaphoreGive(s_mu);
    return i >= 0 ? ESP_OK : ESP_ERR_NOT_FOUND;
}

// inner = "gid|name|keyhex|AAAA,BBBB,..."  The member list holds everyone in the group, the owner included.
bool groups_on_key(const char *from_id, char *inner)
{
    char *f[4] = { inner };
    for (int k = 1; k < 4; k++) { f[k] = strchr(f[k - 1], '|'); if (!f[k]) return false; *f[k]++ = '\0'; }
    if (!good_id(f[0]) || strlen(f[2]) != 32) return false;
    uint8_t key[16];
    for (int k = 0; k < 16; k++) { unsigned v; if (sscanf(f[2] + 2 * k, "%2x", &v) != 1) return false; key[k] = v; }

    xSemaphoreTake(s_mu, portMAX_DELAY);
    int i = find(f[0]);
    if (i >= 0 && strcmp(s_g[i].owner, from_id) != 0) {   // Same gid from another box: only the owner may change a group.
        xSemaphoreGive(s_mu); return false;
    }
    if (i < 0) for (int k = 0; k < MAX_GROUPS; k++) if (!s_g[k].used) { i = k; break; }
    if (i >= 0) {
        grp_t *g = &s_g[i];
        memset(g, 0, sizeof(*g));
        strlcpy(g->gid, f[0], 5);
        strlcpy(g->name, f[1], sizeof(g->name));
        strlcpy(g->owner, from_id, 5);
        memcpy(g->key, key, 16);
        // Member list: everyone except this box. The owner is in it, so replies reach the owner too.
        char *m = f[3];
        while (m && *m && g->count < GROUP_MAX_MEMBERS) {
            char *next = strchr(m, ','); if (next) *next++ = '\0';
            if (good_id(m) && strcmp(m, node_id_str()) != 0) strlcpy(g->members[g->count++], m, 5);
            m = next;
        }
        g->used = 1;
        load_key(i);
        save();
        printf("Groups: %s added us to \"%s\" (%s)\n", from_id, g->name, g->gid);
    }
    memset(key, 0, sizeof(key));
    xSemaphoreGive(s_mu);
    return i >= 0;
}

void groups_key_got(const char *gid, const char *from_id)
{
    xSemaphoreTake(s_mu, portMAX_DELAY);
    int i = find(gid);
    if (i >= 0 && strcmp(s_g[i].owner, node_id_str()) == 0)
        for (int k = 0; k < s_g[i].count; k++)
            if (strcmp(s_g[i].members[k], from_id) == 0) s_todo[i] &= ~(1u << k);
    xSemaphoreGive(s_mu);
}

esp_err_t groups_resend(const char *gid)
{
    xSemaphoreTake(s_mu, portMAX_DELAY);
    int i = find(gid);
    bool mine = i >= 0 && strcmp(s_g[i].owner, node_id_str()) == 0;
    if (mine) { s_todo[i] = (uint8_t)((1u << s_g[i].count) - 1); memset(s_tries[i], 0, sizeof(s_tries[i])); }
    xSemaphoreGive(s_mu);
    return mine ? ESP_OK : ESP_ERR_NOT_FOUND;
}

int groups_next_packet(char *out, int max)
{
    TickType_t now = xTaskGetTickCount();
    if (s_last_k && now - s_last_k < pdMS_TO_TICKS(KEY_GAP_MS)) return 0;   // Limits the airtime used by key packets.
    int n = 0;
    xSemaphoreTake(s_mu, portMAX_DELAY);
    for (int i = 0; i < MAX_GROUPS && !n; i++) {
        int k = -1;                                   // Waiting member with the fewest tries, so members take turns.
        for (int j = 0; j < s_g[i].count; j++) {
            if (!(s_todo[i] & (1u << j))) continue;
            if (s_tries[i][j] >= KEY_TRIES || !friends_is_friend(s_g[i].members[j])) { s_todo[i] &= ~(1u << j); continue; }   // Give up until "Resend key" in the app.
            if (k < 0 || s_tries[i][j] < s_tries[i][k]) k = j;
        }
        if (k < 0) continue;
        grp_t *g = &s_g[i];
        char inner[160];
        int il = snprintf(inner, sizeof(inner), "%s|%s|", g->gid, g->name);
        for (int b = 0; b < 16; b++) il += snprintf(inner + il, sizeof(inner) - il, "%02x", g->key[b]);
        il += snprintf(inner + il, sizeof(inner) - il, "|%s", node_id_str());          // Owner first.
        for (int m = 0; m < g->count; m++) il += snprintf(inner + il, sizeof(inner) - il, ",%s", g->members[m]);
        int hl = snprintf(out, max, "K|%s|%s|", g->members[k], node_id_str());
        int sl = friends_seal(g->members[k], (uint8_t *)inner, il, (uint8_t *)out + hl, max - hl);
        memset(inner, 0, sizeof(inner));
        s_tries[i][k]++;
        if (sl > 0) n = hl + sl;
    }
    xSemaphoreGive(s_mu);
    if (n) s_last_k = now ? now : 1;
    return n;
}

int groups_members_of(const char *gid, char ids[][5], int max)
{
    xSemaphoreTake(s_mu, portMAX_DELAY);
    int i = find(gid), n = 0;
    if (i >= 0) for (; n < s_g[i].count && n < max; n++) memcpy(ids[n], s_g[i].members[n], 5);
    xSemaphoreGive(s_mu);
    return i >= 0 ? n : -1;
}

int groups_seal(const char *gid, const char *from_box, const uint8_t *plain, int len, uint8_t *out, int out_max)
{
    if (len + SEAL_OVERHEAD > out_max || !good_id(from_box)) return -1;
    xSemaphoreTake(s_mu, portMAX_DELAY);
    int i = find(gid), r = -1;
    uint8_t ad[8]; memcpy(ad, gid, 4); memcpy(ad + 4, from_box, 4);   // gid and sender ID are authenticated too.
    if (i >= 0 && s_kid[i]) {
        for (int k = 0; k < 12; k += 4) { uint32_t v = esp_random(); memcpy(out + k, &v, 4); }
        size_t cl;
        if (psa_aead_encrypt(s_kid[i], PSA_ALG_GCM, out, 12, ad, 8, plain, len, out + 12, out_max - 12, &cl) == PSA_SUCCESS)
            r = 12 + (int)cl;
    }
    xSemaphoreGive(s_mu);
    return r;
}

int groups_open(const char *gid, const char *from_box, const uint8_t *sealed, int len, uint8_t *out, int out_max)
{
    if (len <= SEAL_OVERHEAD || !good_id(from_box)) return -1;
    xSemaphoreTake(s_mu, portMAX_DELAY);
    int i = find(gid), r = -1;
    uint8_t ad[8]; memcpy(ad, gid, 4); memcpy(ad + 4, from_box, 4);
    if (i >= 0 && s_kid[i]) {
        size_t pl;
        if (psa_aead_decrypt(s_kid[i], PSA_ALG_GCM, sealed, 12, ad, 8, sealed + 12, len - 12, out, out_max, &pl) == PSA_SUCCESS)
            r = (int)pl;
    }
    xSemaphoreGive(s_mu);
    return r;
}

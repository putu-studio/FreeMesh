// Friends and their private keys. See app_friends.h for the handshake.
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "psa/crypto.h"
#include "nvs.h"
#include "esp_random.h"
#include "app_friends.h"
#include "app_settings.h"           // node_id_str(), device_settings.

#define SLOTS (MAX_FRIENDS + MAX_REQUESTS)
enum { EMPTY = 0, THEY_ASKED = 1, WE_ASKED = 2, FRIEND = 3 };

typedef struct {                    // Flash record of one friend.
    char    id[5];
    char    devicename[17];
    uint8_t pub[32];
    uint8_t key[16];
    uint8_t state;
} entry_t;

static entry_t      s_e[SLOTS];
static psa_key_id_t s_kid[SLOTS];   // AES key loaded for each friend (0 = none).
static uint8_t      s_send[SLOTS];  // 'F' or 'G' = packet still to send for this entry.
static SemaphoreHandle_t s_mu;
static psa_key_id_t s_priv;         // This box's X25519 secret key.
static uint8_t      s_pub[32];      // This box's X25519 public key.
static const char   SALT[] = "FreeMesh friend v1";

// ---------- Helpers ----------
static void to_hex(const uint8_t *b, int n, char *out) { for (int i = 0; i < n; i++) sprintf(out + 2 * i, "%02x", b[i]); }
static int from_hex(const char *s, uint8_t *out, int n)
{
    if ((int)strlen(s) != 2 * n) return -1;
    for (int i = 0; i < n; i++) {
        unsigned v;
        if (sscanf(s + 2 * i, "%2x", &v) != 1) return -1;
        out[i] = (uint8_t)v;
    }
    return 0;
}

static int find(const char *id)     // Caller holds s_mu.
{
    for (int i = 0; i < SLOTS; i++) if (s_e[i].state && strcmp(s_e[i].id, id) == 0) return i;
    return -1;
}
static int free_slot(bool for_request)
{
    int start = for_request ? 0 : 0;
    int requests = 0;
    for (int i = 0; i < SLOTS; i++) if (s_e[i].state == THEY_ASKED || s_e[i].state == WE_ASKED) requests++;
    if (for_request && requests >= MAX_REQUESTS) return -1;
    for (int i = start; i < SLOTS; i++) if (!s_e[i].state) return i;
    return -1;
}

static void safety_code(const uint8_t *other_pub, char out[5])
{
    uint8_t buf[64], h[32]; size_t hl;
    bool mine_first = memcmp(s_pub, other_pub, 32) < 0;          // Same order on both boxes, so both show the same code.
    memcpy(buf, mine_first ? s_pub : other_pub, 32);
    memcpy(buf + 32, mine_first ? other_pub : s_pub, 32);
    psa_hash_compute(PSA_ALG_SHA_256, buf, 64, h, sizeof(h), &hl);
    uint32_t v = ((uint32_t)h[0] << 24 | h[1] << 16 | h[2] << 8 | h[3]) % 10000;
    snprintf(out, 5, "%04u", (unsigned)v);
}

static psa_status_t hmac(const uint8_t *k, size_t kl, const uint8_t *m, size_t ml, uint8_t out[32])
{
    psa_key_attributes_t a = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&a, PSA_KEY_TYPE_HMAC);
    psa_set_key_usage_flags(&a, PSA_KEY_USAGE_SIGN_MESSAGE);
    psa_set_key_algorithm(&a, PSA_ALG_HMAC(PSA_ALG_SHA_256));
    psa_key_id_t id = 0; size_t ol;
    psa_status_t st = psa_import_key(&a, k, kl, &id);
    if (st == PSA_SUCCESS) st = psa_mac_compute(id, PSA_ALG_HMAC(PSA_ALG_SHA_256), m, ml, out, 32, &ol);
    if (id) psa_destroy_key(id);
    return st;
}

// X25519 key agreement, then HKDF-SHA256 (RFC 5869), gives a 16-byte AES key that is the same on both boxes.
static esp_err_t derive(const uint8_t *other_pub, const char *other_id, uint8_t key[16])
{
    uint8_t shared[32], prk[32], okm[32], info[16]; size_t sl;
    if (psa_raw_key_agreement(PSA_ALG_ECDH, s_priv, other_pub, 32, shared, sizeof(shared), &sl) != PSA_SUCCESS)
        return ESP_FAIL;
    const char *me = node_id_str();
    bool me_first = strcmp(me, other_id) < 0;
    int n = snprintf((char *)info, sizeof(info), "%s%s", me_first ? me : other_id, me_first ? other_id : me);
    info[n] = 0x01;
    esp_err_t r = ESP_FAIL;
    if (hmac((const uint8_t *)SALT, sizeof(SALT) - 1, shared, 32, prk) == PSA_SUCCESS &&
        hmac(prk, 32, info, n + 1, okm) == PSA_SUCCESS) {
        memcpy(key, okm, 16);
        r = ESP_OK;
    }
    memset(shared, 0, sizeof(shared)); memset(prk, 0, sizeof(prk)); memset(okm, 0, sizeof(okm));
    return r;
}

static void load_key(int i)         // Caller holds s_mu.
{
    if (s_kid[i]) { psa_destroy_key(s_kid[i]); s_kid[i] = 0; }
    if (s_e[i].state != FRIEND) return;
    psa_key_attributes_t a = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&a, PSA_KEY_TYPE_AES);
    psa_set_key_bits(&a, 128);
    psa_set_key_usage_flags(&a, PSA_KEY_USAGE_ENCRYPT | PSA_KEY_USAGE_DECRYPT);
    psa_set_key_algorithm(&a, PSA_ALG_GCM);
    psa_import_key(&a, s_e[i].key, 16, &s_kid[i]);
}

static esp_err_t save(void)         // Caller holds s_mu. Only approved friends go to flash.
{
    entry_t keep[MAX_FRIENDS]; int n = 0;
    for (int i = 0; i < SLOTS && n < MAX_FRIENDS; i++) if (s_e[i].state == FRIEND) keep[n++] = s_e[i];
    nvs_handle_t h;
    esp_err_t err = nvs_open("lora", NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = n ? nvs_set_blob(h, "friends", keep, n * sizeof(entry_t)) : nvs_erase_key(h, "friends");
    if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    memset(keep, 0, sizeof(keep));
    return err;
}

static void make_friend(int i, const uint8_t *pub)   // Caller holds s_mu.
{
    if (derive(pub, s_e[i].id, s_e[i].key) != ESP_OK) return;
    memcpy(s_e[i].pub, pub, 32);
    s_e[i].state = FRIEND;
    load_key(i);
    save();
    printf("Friends: now friends with %s (%s)\n", s_e[i].id, s_e[i].devicename);
}

// ---------- Public ----------
esp_err_t friends_init(void)
{
    s_mu = xSemaphoreCreateMutex();
    psa_key_attributes_t a = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&a, PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_MONTGOMERY));
    psa_set_key_bits(&a, 255);
    psa_set_key_usage_flags(&a, PSA_KEY_USAGE_DERIVE | PSA_KEY_USAGE_EXPORT);
    psa_set_key_algorithm(&a, PSA_ALG_ECDH);

    nvs_handle_t h;
    if (nvs_open("lora", NVS_READWRITE, &h) != ESP_OK) return ESP_FAIL;
    uint8_t priv[32]; size_t pl = sizeof(priv);
    if (nvs_get_blob(h, "idpriv", priv, &pl) == ESP_OK && pl == 32) {
        if (psa_import_key(&a, priv, 32, &s_priv) != PSA_SUCCESS) s_priv = 0;
    }
    if (!s_priv) {                                   // First start: create this box's key pair.
        if (psa_generate_key(&a, &s_priv) != PSA_SUCCESS) { nvs_close(h); return ESP_FAIL; }
        size_t el;
        psa_export_key(s_priv, priv, sizeof(priv), &el);
        nvs_set_blob(h, "idpriv", priv, 32);
        nvs_commit(h);
        printf("Friends: made a new box key\n");
    }
    memset(priv, 0, sizeof(priv));
    size_t ol;
    psa_export_public_key(s_priv, s_pub, sizeof(s_pub), &ol);

    size_t fl = sizeof(entry_t) * MAX_FRIENDS;
    memset(s_e, 0, sizeof(s_e));
    if (nvs_get_blob(h, "friends", s_e, &fl) != ESP_OK) memset(s_e, 0, sizeof(s_e));
    nvs_close(h);
    int n = 0;
    for (int i = 0; i < SLOTS; i++) { if (s_e[i].state != FRIEND) memset(&s_e[i], 0, sizeof(entry_t)); else { load_key(i); n++; } }
    printf("Friends: %d saved\n", n);
    return ESP_OK;
}

int friends_list(friend_info_t *out, int max)
{
    int n = 0;
    xSemaphoreTake(s_mu, portMAX_DELAY);
    for (int i = 0; i < SLOTS && n < max; i++) {
        if (!s_e[i].state) continue;
        strlcpy(out[n].id, s_e[i].id, sizeof(out[n].id));
        strlcpy(out[n].devicename, s_e[i].devicename, sizeof(out[n].devicename));
        if (s_e[i].state == WE_ASKED) strcpy(out[n].code, "----");   // Their key is not known yet.
        else safety_code(s_e[i].pub, out[n].code);
        out[n].state = s_e[i].state;
        n++;
    }
    xSemaphoreGive(s_mu);
    return n;
}

esp_err_t friends_ask(const char *id)
{
    if (strlen(id) != 4 || strcmp(id, node_id_str()) == 0) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(s_mu, portMAX_DELAY);
    esp_err_t r = ESP_OK;
    int i = find(id);
    if (i >= 0 && s_e[i].state == FRIEND) r = ESP_OK;                 // Already friends.
    else if (i >= 0 && s_e[i].state == THEY_ASKED) { s_send[i] = 'G'; make_friend(i, s_e[i].pub); }   // Both asked.
    else {
        if (i < 0) i = free_slot(true);
        if (i < 0) r = ESP_ERR_NO_MEM;
        else {
            memset(&s_e[i], 0, sizeof(entry_t));
            strlcpy(s_e[i].id, id, sizeof(s_e[i].id));
            s_e[i].state = WE_ASKED;
            s_send[i] = 'F';
        }
    }
    xSemaphoreGive(s_mu);
    return r;
}

esp_err_t friends_approve(const char *id)
{
    xSemaphoreTake(s_mu, portMAX_DELAY);
    int i = find(id);
    esp_err_t r = ESP_ERR_NOT_FOUND;
    if (i >= 0 && s_e[i].state == THEY_ASKED) {
        make_friend(i, s_e[i].pub);
        s_send[i] = 'G';
        r = s_e[i].state == FRIEND ? ESP_OK : ESP_FAIL;
    }
    xSemaphoreGive(s_mu);
    return r;
}

esp_err_t friends_remove(const char *id)
{
    xSemaphoreTake(s_mu, portMAX_DELAY);
    int i = find(id);
    if (i >= 0) {
        bool was_friend = s_e[i].state == FRIEND;
        memset(&s_e[i], 0, sizeof(entry_t));
        s_send[i] = 0;
        load_key(i);                                 // State is EMPTY now, so this drops the AES key.
        if (was_friend) save();
    }
    xSemaphoreGive(s_mu);
    return i >= 0 ? ESP_OK : ESP_ERR_NOT_FOUND;
}

bool friends_is_friend(const char *id)
{
    xSemaphoreTake(s_mu, portMAX_DELAY);
    int i = find(id);
    bool f = i >= 0 && s_e[i].state == FRIEND;
    xSemaphoreGive(s_mu);
    return f;
}

void friends_on_packet(char type, const char *to, const char *from_id, const char *devicename, const char *pub_hex)
{
    if (strcmp(to, node_id_str()) != 0 || strlen(from_id) != 4) return;   // Not for this box.
    uint8_t pub[32];
    if (from_hex(pub_hex, pub, 32) != 0) return;
    xSemaphoreTake(s_mu, portMAX_DELAY);
    int i = find(from_id);
    if (type == 'F') {                                       // Friend request.
        if (i >= 0 && s_e[i].state == FRIEND && memcmp(s_e[i].pub, pub, 32) == 0) {
            s_send[i] = 'G';                                 // The answer was lost: send it again.
        } else if (i >= 0 && s_e[i].state == WE_ASKED) {     // Both asked: become friends.
            strlcpy(s_e[i].devicename, devicename, sizeof(s_e[i].devicename));
            make_friend(i, pub);
            s_send[i] = 'G';
        } else if (i < 0 || s_e[i].state == THEY_ASKED) {
            if (i < 0) i = free_slot(true);
            if (i >= 0) {
                strlcpy(s_e[i].id, from_id, sizeof(s_e[i].id));
                strlcpy(s_e[i].devicename, devicename, sizeof(s_e[i].devicename));
                memcpy(s_e[i].pub, pub, 32);
                s_e[i].state = THEY_ASKED;
                printf("Friends: request from %s (%s)\n", from_id, devicename);
            }
        }
        // A friend that asks again with a new key is ignored until it is removed, so a fake box cannot replace it.
    } else if (type == 'G' && i >= 0 && s_e[i].state == WE_ASKED) {   // Accept: valid only if this box asked.
        strlcpy(s_e[i].devicename, devicename, sizeof(s_e[i].devicename));
        make_friend(i, pub);
    }
    xSemaphoreGive(s_mu);
}

int friends_next_packet(char *out, int max)
{
    int n = 0;
    xSemaphoreTake(s_mu, portMAX_DELAY);
    for (int i = 0; i < SLOTS; i++) {
        if (!s_send[i]) continue;
        char hex[65];
        to_hex(s_pub, 32, hex);
        n = snprintf(out, max, "%c|%s|%s|%s|%s", s_send[i], s_e[i].id, node_id_str(), device_settings.devicename, hex);
        if (n >= max) n = 0;
        s_send[i] = 0;
        break;
    }
    xSemaphoreGive(s_mu);
    return n;
}

int friends_seal(const char *friend_id, const uint8_t *plain, int len, uint8_t *out, int out_max)
{
    if (len + SEAL_OVERHEAD > out_max) return -1;
    xSemaphoreTake(s_mu, portMAX_DELAY);
    int i = find(friend_id), r = -1;
    if (i >= 0 && s_kid[i]) {
        for (int k = 0; k < 12; k += 4) { uint32_t v = esp_random(); memcpy(out + k, &v, 4); }   // Random nonce.
        size_t cl;
        if (psa_aead_encrypt(s_kid[i], PSA_ALG_GCM, out, 12, NULL, 0, plain, len, out + 12, out_max - 12, &cl) == PSA_SUCCESS)
            r = 12 + (int)cl;
    }
    xSemaphoreGive(s_mu);
    return r;
}

int friends_open(const char *friend_id, const uint8_t *sealed, int len, uint8_t *out, int out_max)
{
    if (len <= SEAL_OVERHEAD) return -1;
    xSemaphoreTake(s_mu, portMAX_DELAY);
    int i = find(friend_id), r = -1;
    if (i >= 0 && s_kid[i]) {
        size_t pl;
        if (psa_aead_decrypt(s_kid[i], PSA_ALG_GCM, sealed, 12, NULL, 0, sealed + 12, len - 12, out, out_max, &pl) == PSA_SUCCESS)
            r = (int)pl;
    }
    xSemaphoreGive(s_mu);
    return r;
}

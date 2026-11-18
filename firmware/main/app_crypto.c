// Network lock. See app_crypto.h for the packet layout.
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "psa/crypto.h"             // PSA Crypto API (mbedTLS): AES-GCM, HMAC, SHA-256.
#include "nvs.h"
#include "esp_mac.h"
#include "app_crypto.h"

#define MAGIC        0xE1           // First byte of a locked packet. Never a letter, so plain-text parsers ignore it.
#define NONCE_LEN    12
#define TAG_LEN      16
#define HDR_LEN      (2 + NONCE_LEN)
#define PBKDF2_ITER  10000          // Slows down passphrase guessing from recorded packets.
static const char SALT[] = "FreeMesh network v1";   // Same on every box, so the same passphrase gives the same key.

static SemaphoreHandle_t s_cmu;     // Guards the key: the web task sets it, the radio task wraps and unwraps with it.
static psa_key_id_t s_key;          // 0 = open (no passphrase).
static uint8_t  s_kid;              // 1-byte fingerprint of the key.
static char     s_code[3];          // Check code for the page: 2 hex characters.
static uint8_t  s_me[4];            // This box's sender ID inside the nonce.
static uint64_t s_ctr;              // Nonce counter: (boot number << 32) + packets sent.

// Replay guard: the highest counter seen from each sender.
#define MAX_SENDERS 16
static struct { uint8_t id[4]; uint64_t ctr; uint8_t used; } s_seen[MAX_SENDERS];

// ---------- Small helpers ----------
static void put_be64(uint8_t *p, uint64_t v) { for (int i = 7; i >= 0; i--) { p[i] = v & 0xFF; v >>= 8; } }
static uint64_t get_be64(const uint8_t *p) { uint64_t v = 0; for (int i = 0; i < 8; i++) v = (v << 8) | p[i]; return v; }

static psa_key_id_t import_aes(const uint8_t key[16])
{
    psa_key_attributes_t a = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&a, PSA_KEY_TYPE_AES);
    psa_set_key_bits(&a, 128);
    psa_set_key_usage_flags(&a, PSA_KEY_USAGE_ENCRYPT | PSA_KEY_USAGE_DECRYPT);
    psa_set_key_algorithm(&a, PSA_ALG_GCM);
    psa_key_id_t id = 0;
    if (psa_import_key(&a, key, 16, &id) != PSA_SUCCESS) return 0;
    return id;
}

static void use_key(const uint8_t key[16])          // Caller holds s_cmu.
{
    if (s_key) { psa_destroy_key(s_key); s_key = 0; }
    s_code[0] = '\0';
    if (!key) return;
    s_key = import_aes(key);
    uint8_t h[32]; size_t hl;
    psa_hash_compute(PSA_ALG_SHA_256, key, 16, h, sizeof(h), &hl);
    s_kid = h[0];
    snprintf(s_code, sizeof(s_code), "%02X", h[1]);  // Check code for users: a different hash byte than the kid.
    memset(s_seen, 0, sizeof(s_seen));               // New key: forget the old counters.
}

// PBKDF2-HMAC-SHA256 (RFC 8018). Computes one 32-byte block and keeps the first 16 bytes as the key.
static esp_err_t pbkdf2(const char *pass, uint8_t out[16])
{
    psa_key_attributes_t a = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&a, PSA_KEY_TYPE_HMAC);
    psa_set_key_usage_flags(&a, PSA_KEY_USAGE_SIGN_MESSAGE);
    psa_set_key_algorithm(&a, PSA_ALG_HMAC(PSA_ALG_SHA_256));
    psa_key_id_t k = 0;
    if (psa_import_key(&a, (const uint8_t *)pass, strlen(pass), &k) != PSA_SUCCESS) return ESP_FAIL;

    uint8_t msg[sizeof(SALT) - 1 + 4], u[32], u2[32], t[32]; size_t ul;
    memcpy(msg, SALT, sizeof(SALT) - 1);
    msg[sizeof(SALT) - 1] = 0; msg[sizeof(SALT)] = 0; msg[sizeof(SALT) + 1] = 0; msg[sizeof(SALT) + 2] = 1;   // Block number 1.
    psa_status_t st = psa_mac_compute(k, PSA_ALG_HMAC(PSA_ALG_SHA_256), msg, sizeof(msg), u, sizeof(u), &ul);
    memcpy(t, u, 32);
    for (int i = 1; i < PBKDF2_ITER && st == PSA_SUCCESS; i++) {
        st = psa_mac_compute(k, PSA_ALG_HMAC(PSA_ALG_SHA_256), u, 32, u2, sizeof(u2), &ul);
        memcpy(u, u2, 32);
        for (int j = 0; j < 32; j++) t[j] ^= u[j];
    }
    psa_destroy_key(k);
    memcpy(out, t, 16);
    memset(t, 0, sizeof(t)); memset(u, 0, sizeof(u)); memset(u2, 0, sizeof(u2));
    return st == PSA_SUCCESS ? ESP_OK : ESP_FAIL;
}

// ---------- Public ----------
esp_err_t crypto_init(void)
{
    s_cmu = xSemaphoreCreateMutex();
    if (psa_crypto_init() != PSA_SUCCESS) { printf("Crypto: init failed\n"); return ESP_FAIL; }

    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_BASE);
    memcpy(s_me, mac + 2, 4);

    nvs_handle_t h;
    uint32_t boots = 0;
    uint8_t key[16]; size_t kl = sizeof(key);
    bool have = false;
    if (nvs_open("lora", NVS_READWRITE, &h) == ESP_OK) {
        nvs_get_u32(h, "boots", &boots);
        boots++;                                     // New boot number: nonces stay unique across restarts.
        nvs_set_u32(h, "boots", boots);
        have = nvs_get_blob(h, "ckey", key, &kl) == ESP_OK && kl == 16;
        nvs_commit(h);
        nvs_close(h);
    }
    s_ctr = (uint64_t)boots << 32;

    xSemaphoreTake(s_cmu, portMAX_DELAY);
    use_key(have ? key : NULL);
    xSemaphoreGive(s_cmu);
    memset(key, 0, sizeof(key));
    printf("Network lock: %s%s\n", have ? "ON, code " : "OFF (open)", s_code);
    return ESP_OK;
}

bool crypto_locked(void) { return s_key != 0; }
const char *crypto_check_code(void) { return s_code; }

esp_err_t crypto_set_passphrase(const char *pass)
{
    size_t n = strlen(pass);
    if (n != 0 && (n < 8 || n > 64)) return ESP_ERR_INVALID_ARG;

    uint8_t key[16];
    if (n && pbkdf2(pass, key) != ESP_OK) return ESP_FAIL;

    nvs_handle_t h;
    esp_err_t err = nvs_open("lora", NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = n ? nvs_set_blob(h, "ckey", key, 16) : nvs_erase_key(h, "ckey");
    if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;  // Unlocking a box that was already open.
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) return err;

    xSemaphoreTake(s_cmu, portMAX_DELAY);
    use_key(n ? key : NULL);
    xSemaphoreGive(s_cmu);
    memset(key, 0, sizeof(key));
    printf("Network lock: %s%s\n", n ? "ON, code " : "OFF (open)", s_code);
    return ESP_OK;
}

int crypto_wrap(const uint8_t *plain, int len, uint8_t *out, int out_max)
{
    xSemaphoreTake(s_cmu, portMAX_DELAY);
    int r = -1;
    if (!s_key) {                                    // Open: send the packet as it is.
        if (len <= out_max) { memcpy(out, plain, len); r = len; }
    } else if (HDR_LEN + len + TAG_LEN <= out_max) {
        out[0] = MAGIC;
        out[1] = s_kid;
        memcpy(out + 2, s_me, 4);
        put_be64(out + 6, ++s_ctr);                  // The counter only goes up, so no nonce is used twice.
        size_t cl = 0;
        if (psa_aead_encrypt(s_key, PSA_ALG_GCM, out + 2, NONCE_LEN, out, HDR_LEN,   // Header: authenticated, not hidden.
                             plain, len, out + HDR_LEN, out_max - HDR_LEN, &cl) == PSA_SUCCESS)
            r = HDR_LEN + (int)cl;
    }
    xSemaphoreGive(s_cmu);
    return r;
}

int crypto_unwrap(const uint8_t *pkt, int len, uint8_t *out, int out_max)
{
    xSemaphoreTake(s_cmu, portMAX_DELAY);
    int r = -1;
    if (!s_key) {                                    // Open box: accept only plain packets.
        if (len > 0 && pkt[0] != MAGIC && len <= out_max) { memcpy(out, pkt, len); r = len; }
    } else if (len > HDR_LEN + TAG_LEN && pkt[0] == MAGIC && pkt[1] == s_kid) {
        size_t pl = 0;
        if (psa_aead_decrypt(s_key, PSA_ALG_GCM, pkt + 2, NONCE_LEN, pkt, HDR_LEN,
                             pkt + HDR_LEN, len - HDR_LEN, out, out_max, &pl) == PSA_SUCCESS) {
            // The packet is genuine. Replay check: its counter must be higher than the last one from this sender.
            uint64_t c = get_be64(pkt + 6);
            int slot = -1, free_slot = -1, oldest = 0;
            for (int i = 0; i < MAX_SENDERS; i++) {
                if (s_seen[i].used && memcmp(s_seen[i].id, pkt + 2, 4) == 0) { slot = i; break; }
                if (!s_seen[i].used && free_slot < 0) free_slot = i;
                if (s_seen[i].ctr < s_seen[oldest].ctr) oldest = i;
            }
            if (slot >= 0 && c <= s_seen[slot].ctr) {
                printf("Network lock: replayed packet dropped\n");
            } else {
                if (slot < 0) {
                    slot = free_slot >= 0 ? free_slot : oldest;
                    memcpy(s_seen[slot].id, pkt + 2, 4);
                    s_seen[slot].used = 1;
                }
                s_seen[slot].ctr = c;
                r = (int)pl;
            }
        }
    }
    xSemaphoreGive(s_cmu);
    return r;
}

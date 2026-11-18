// SOS alert. One alert is sent as a FreeMesh message and also as one packet each in the Meshtastic and MeshCore formats:
//   1. FreeMesh    the normal "M" message (app_io.c calls chat_send).
//   2. Meshtastic  a text on the default channel "LongFast" (US: 906.875 MHz, SF11, 250 kHz, sync 0x2B),
//                  encrypted with Meshtastic's public default key, as any Meshtastic node sends it.
//   3. MeshCore    a text on the "Public" channel (US/Canada preset: 910.525 MHz, SF7, 62.5 kHz).
// Both channels use publicly known keys, so the alert is not private: anyone in range can read it.
// The packet formats follow the Meshtastic 2.8.3 and MeshCore d2096bd sources (see tools/patches).
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "psa/crypto.h"
#include "esp_random.h"
#include "app_sos.h"
#include "app_radio.h"
#include "app_settings.h"

// Meshtastic's default channel key ("AQ==" = key 1).
static const uint8_t MT_KEY[16] = {0xd4, 0xf1, 0xbb, 0x3a, 0x20, 0x29, 0x07, 0x59,
                                   0xf0, 0xbc, 0xff, 0xab, 0xcf, 0x4e, 0x69, 0x01};
// MeshCore's Public channel key: base64 "izOH6cXN6mrJ5e26oRXNcg==".
static const uint8_t MC_KEY[16] = {0x8b, 0x33, 0x87, 0xe9, 0xc5, 0xcd, 0xea, 0x6a,
                                   0xc9, 0xe5, 0xed, 0xba, 0xa1, 0x15, 0xcd, 0x72};

static const radio_foreign_t MT_RADIO = { 906875000, 11, 0x05, 0x01, 0x00, 0x24B4, 16 };   // LongFast, US.
static const radio_foreign_t MC_RADIO = { 910525000,  7, 0x03, 0x01, 0x00, 0x1424, 32 };   // MeshCore US/Canada.

static volatile uint8_t s_pending;       // 1 = the radio task should send the two foreign SOS packets.
static char s_text[100];
static uint32_t s_unix_at_boot;          // Learned from the phone, because the box has no clock.

void sos_set_time(uint32_t unix_now)
{
    if (unix_now > 1700000000u) s_unix_at_boot = unix_now - xTaskGetTickCount() * portTICK_PERIOD_MS / 1000;
}

static uint32_t unix_now(void) { return s_unix_at_boot + xTaskGetTickCount() * portTICK_PERIOD_MS / 1000; }

static psa_key_id_t aes_key(const uint8_t k[16], psa_algorithm_t alg)
{
    psa_key_attributes_t a = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&a, PSA_KEY_TYPE_AES);
    psa_set_key_bits(&a, 128);
    psa_set_key_usage_flags(&a, PSA_KEY_USAGE_ENCRYPT);
    psa_set_key_algorithm(&a, alg);
    psa_key_id_t id = 0;
    return psa_import_key(&a, k, 16, &id) == PSA_SUCCESS ? id : 0;
}

// Encrypts data with AES-CTR (16-byte IV) or AES-ECB (iv = NULL). Returns the bytes written, or -1 on failure.
static int cipher(const uint8_t key[16], psa_algorithm_t alg, const uint8_t *iv, const uint8_t *in, int n, uint8_t *out, int max)
{
    psa_key_id_t k = aes_key(key, alg);
    if (!k) return -1;
    psa_cipher_operation_t op = PSA_CIPHER_OPERATION_INIT;
    size_t a = 0, b = 0;
    int ok = psa_cipher_encrypt_setup(&op, k, alg) == PSA_SUCCESS
          && (!iv || psa_cipher_set_iv(&op, iv, 16) == PSA_SUCCESS)
          && psa_cipher_update(&op, in, n, out, max, &a) == PSA_SUCCESS
          && psa_cipher_finish(&op, out + a, max - a, &b) == PSA_SUCCESS;
    psa_cipher_abort(&op);
    psa_destroy_key(k);
    return ok ? (int)(a + b) : -1;
}

static uint8_t xor_all(const uint8_t *p, int n) { uint8_t h = 0; while (n--) h ^= *p++; return h; }

// Meshtastic: 16-byte header + AES-CTR( Data{ portnum: 1 TEXT_MESSAGE_APP, payload: text } ).
int sos_build_meshtastic(const char *text, uint32_t from, uint32_t id, uint8_t *out, int max)
{
    int tl = strlen(text); if (tl > 200) tl = 200;
    uint8_t data[210]; int dl = 0;
    data[dl++] = 0x08; data[dl++] = 0x01;                // Field 1 (portnum) = 1.
    data[dl++] = 0x12; data[dl++] = tl;                  // Field 2 (payload), then its length.
    memcpy(data + dl, text, tl); dl += tl;
    if (16 + dl > max) return -1;
    uint32_t to = 0xFFFFFFFF;                            // Broadcast address: everyone.
    memcpy(out, &to, 4); memcpy(out + 4, &from, 4); memcpy(out + 8, &id, 4);   // Little endian, the ESP32's byte order.
    out[12] = 3 | (3 << 5);                              // Hop limit 3, hop start 3, no ACK wanted.
    out[13] = xor_all((const uint8_t *)"LongFast", 8) ^ xor_all(MT_KEY, 16);   // Channel hash.
    out[14] = 0;                                         // Next hop: none.
    out[15] = from & 0xFF;                               // Relay node: this box.
    uint8_t nonce[16] = {0};
    uint64_t pid = id;
    memcpy(nonce, &pid, 8); memcpy(nonce + 8, &from, 4); // Packet ID (8 bytes) + sender (4) + 0.
    int cl = cipher(MT_KEY, PSA_ALG_CTR, nonce, data, dl, out + 16, max - 16);
    return cl < 0 ? -1 : 16 + cl;
}

// MeshCore: header 0x15 (group text, flood) + path length 0 + channel hash + MAC(2) + AES-ECB(time|0|"name: text").
int sos_build_meshcore(const char *name, const char *text, uint32_t ts, uint8_t *out, int max)
{
    uint8_t plain[176] = {0}; int pl = 0;
    memcpy(plain, &ts, 4); pl = 4;
    plain[pl++] = 0;                                     // Type 0: plain text.
    pl += snprintf((char *)plain + pl, sizeof(plain) - pl - 1, "%s: %s", name, text);
    int padded = (pl + 15) / 16 * 16;                    // Zero-padded to whole 16-byte blocks.
    if (5 + padded > max) return -1;
    uint8_t secret[32] = {0}; memcpy(secret, MC_KEY, 16);
    size_t hl;
    uint8_t h[32];
    if (psa_hash_compute(PSA_ALG_SHA_256, MC_KEY, 16, h, sizeof(h), &hl) != PSA_SUCCESS) return -1;
    out[0] = (0x05 << 2) | 0x01;                         // PAYLOAD_TYPE_GRP_TXT, ROUTE_TYPE_FLOOD.
    out[1] = 0;                                          // No path yet.
    out[2] = h[0];                                       // Channel hash.
    if (cipher(MC_KEY, PSA_ALG_ECB_NO_PADDING, NULL, plain, padded, out + 5, max - 5) != padded) return -1;
    // MAC = first 2 bytes of HMAC-SHA256 over the ciphertext, keyed with the 32-byte secret.
    psa_key_attributes_t a = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&a, PSA_KEY_TYPE_HMAC);
    psa_set_key_bits(&a, 256);
    psa_set_key_usage_flags(&a, PSA_KEY_USAGE_SIGN_MESSAGE);
    psa_set_key_algorithm(&a, PSA_ALG_HMAC(PSA_ALG_SHA_256));
    psa_key_id_t k = 0;
    if (psa_import_key(&a, secret, 32, &k) != PSA_SUCCESS) return -1;
    uint8_t mac[32]; size_t ml;
    psa_status_t st = psa_mac_compute(k, PSA_ALG_HMAC(PSA_ALG_SHA_256), out + 5, padded, mac, sizeof(mac), &ml);
    psa_destroy_key(k);
    if (st != PSA_SUCCESS) return -1;
    out[3] = mac[0]; out[4] = mac[1];
    return 5 + padded;
}

void sos_everywhere(const char *text)
{
    strlcpy(s_text, text, sizeof(s_text));
    s_pending = 1;                                       // The radio task sends them when the channel is free.
}

bool sos_radio_step(void)
{
    if (!s_pending) return false;
    s_pending = 0;
    uint8_t pkt[220];
    // Meshtastic node number: "FM" + this box's ID, so users can see that it is a FreeMesh box.
    unsigned id16 = 0; sscanf(node_id_str(), "%4x", &id16);
    uint32_t from = 0x464D0000u | id16;
    int n = sos_build_meshtastic(s_text, from, esp_random() | 1, pkt, sizeof(pkt));
    if (n > 0 && radio_send_foreign(&MT_RADIO, pkt, n) == ESP_OK) printf("SOS: sent to Meshtastic (LongFast)\n");
    n = sos_build_meshcore(device_settings.devicename, s_text, unix_now(), pkt, sizeof(pkt));
    if (n > 0 && radio_send_foreign(&MC_RADIO, pkt, n) == ESP_OK) printf("SOS: sent to MeshCore (Public)\n");
    radio_apply(&device_settings);                       // Back to the FreeMesh radio settings.
    radio_start_rx();
    return true;
}

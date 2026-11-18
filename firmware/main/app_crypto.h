#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

// Network lock: every LoRa packet is encrypted and authenticated with one shared key (AES-128-GCM).
// The key is derived from a passphrase that is typed once into every box of the network.
// The key never leaves the box: the page sees only "locked" and a 2-character check code.
//
// On air:  0xE1 | kid | nonce (12) | encrypted packet | tag (16)
//   kid    = 1 byte of the key's fingerprint, so a box quickly skips packets of other networks.
//   nonce  = sender ID (4) + counter (8), never used twice: the counter goes up with every packet
//            and its top half is a boot number kept in flash.
//   tag    = shows that the packet comes from a holder of the key and was not changed.

#define CRYPTO_OVERHEAD 30           // 1 + 1 + 12 + 16 bytes added to every packet.

esp_err_t crypto_init(void);         // Call once at start, after nvs_flash_init.
bool      crypto_locked(void);       // True when a passphrase is set.
const char *crypto_check_code(void); // Fingerprint such as "A3" to compare between boxes. "" when open.

// Web task: set a new passphrase (8 to 64 characters), or "" to unlock. Takes about 1 s by design.
esp_err_t crypto_set_passphrase(const char *pass);

// Radio task only.
// Plain payload -> packet to send. Returns its length, or -1. Copies the payload unchanged when not locked.
int crypto_wrap(const uint8_t *plain, int len, uint8_t *out, int out_max);
// Received packet -> plain payload. Returns its length, or -1 to drop the packet (wrong network, changed,
// replayed, or a plain packet while this box is locked).
int crypto_unwrap(const uint8_t *pkt, int len, uint8_t *out, int out_max);

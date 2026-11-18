#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

// Friends: two boxes that approved each other share their own secret key.
// Private messages are encrypted with a key that only the two friend boxes derive.
//
// How two boxes become friends:
//   1. The user of box A taps "Add friend" on box B:   A sends  F|toB|A|nameA|pubA   (request)
//   2. Box B's page shows the request and a 4-digit safety code. The users compare the code
//      on both pages to detect a fake box in the middle.
//   3. The user of box B taps Approve:                 B sends  G|toA|B|nameB|pubB   (accept)
//   4. Both boxes compute the same key: X25519(own secret, other public key) -> HKDF -> 16 bytes.
//      The key itself is never sent.

#define MAX_FRIENDS   8
#define MAX_REQUESTS  4
#define SEAL_OVERHEAD 28            // Nonce 12 + tag 16, added by friends_seal().

typedef struct {
    char    id[5];                  // Example: "A1B2".
    char    devicename[17];
    char    code[5];                // Safety code, for example "4821".
    uint8_t state;                  // 0 empty, 1 they asked us, 2 we asked them, 3 friends.
} friend_info_t;

esp_err_t friends_init(void);       // Call after crypto_init(). Loads or creates this box's key pair, loads the friends.

// Web task.
int  friends_list(friend_info_t *out, int max);      // Friends and open requests, for the page.
esp_err_t friends_ask(const char *id);              // Send a request to a box in radio range.
esp_err_t friends_approve(const char *id);          // Accept a request: the two boxes become friends.
esp_err_t friends_remove(const char *id);           // Forget a friend or a request.
bool friends_is_friend(const char *id);             // True when the box is an approved friend.

// Radio task: handle a received F or G packet. The fields are already split into strings that end in '\0'.
void friends_on_packet(char type, const char *to, const char *from_id,
                       const char *devicename, const char *pub_b64);
// Radio task: write the packet to send for a pending ask or approve. Returns its length, or 0 if there is none.
int  friends_next_packet(char *out, int max);

// Seal or open a private message for one friend. Returns the length, or -1 (not a friend, message changed, ...).
int friends_seal(const char *friend_id, const uint8_t *plain, int len, uint8_t *out, int out_max);
int friends_open(const char *friend_id, const uint8_t *sealed, int len, uint8_t *out, int out_max);

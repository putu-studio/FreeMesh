#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

// People: private chats with a person, not with a whole box. See app_people.c.
//
// Every phone makes its own id and X25519 key pair in the page and keeps them on the phone.
// The box keeps a list of the phones that use it and tells its friend boxes about them:
//   U|to|from|<sealed with the friend key: pid|online|name|pubkey hex>
// Messages between people are locked on the sender's phone, and only the other phone can open them
// (end to end). The boxes only carry the locked bytes:
//   E|to box|from box|seq|to pid|from pid|<12-byte nonce + AES-GCM cipher text + 16-byte tag>

#define PEOPLE_LOCAL_MAX   8            // Phones that use this box.
#define PEOPLE_REMOTE_MAX  32           // People on friend boxes.
#define PERSON_TEXT_MAX    110          // Bytes the phone may lock in one message (fits in one packet).

typedef struct {
    char     box[5];                    // The box this person uses.
    char     pid[5];                    // Example: "a3f0". Made by the phone.
    char     name[13];
    char     pub[65];                   // The phone's public key, 64 hex letters.
    uint8_t  online;                    // Page open in the last 3 minutes, as far as this box knows.
    uint8_t  local;                     // 1 = on this box.
    uint32_t age_s;                     // Seconds since this box last heard of the person.
} person_info_t;

void people_init(void);                 // Call once at startup. Loads the saved lists from NVS.

// Web task.
esp_err_t people_register(const char *pid, const char *name, const char *pubhex);   // POST /api/me.
void      people_seen(const char *pid);                                             // Called on every poll.
int       people_list(person_info_t *out, int max);                                 // GET /api/people.
bool      people_known(const char *box, const char *pid);                           // True when the person is known.
bool      people_forget(const char *pid);        // "Forget me on this box": removed here, and friend boxes are told.
void      people_clear_all(void);                // "Hand over this box": removes everyone, in RAM and in NVS.

// Radio task.
void people_box_heard(const char *box);                     // Call for any packet heard from this box.
void people_on_packet(const char *from_box, char *inner);   // Handle a U packet that is already opened.
int  people_next_packet(char *out, int max);                // Build a U packet to send. Returns its length, or 0.

// base64url without padding (uses '-' and '_'), so it needs no escaping in a form or JSON.
int b64_encode(const uint8_t *in, int n, char *out, int max);   // Returns the letters written, or -1 when out of room.
int b64_decode(const char *in, uint8_t *out, int max);          // Returns the bytes written, or -1 on error.

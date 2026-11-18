#pragma once
#include <stdint.h>
#include <stddef.h> // size_t

#define CHAT_MAX 150                 // Number of messages the log keeps.

typedef struct {
    uint32_t id;                    // 1, 2, 3, ... (0 = empty slot).
    char from[13];                  // Name of the person who wrote it.
    char devicename[17];            // Name of the box it came from.
    char text[205];                 // Message text. For a person message: the encrypted bytes in base64url.
    int16_t rssi;                   // Signal strength in dBm (received messages only).
    float snr;
    uint8_t local;                  // 1 = sent from this box, 0 = received over the radio.
    uint8_t st;                     // 0 sending, 1 sent, 2 failed, 3 delivered (local only).
    uint8_t acks;                   // Number of boxes that confirmed it (local only).
    uint8_t need;                   // Number of boxes expected to confirm it (local only).
    char peer[5];                   // "" = to everyone. Otherwise the friend box this private message is to or from.
    char group[5];                  // "" = not a group message. Otherwise the group id.
    uint8_t voice;                  // 0 = text. Otherwise the length of the voice message in seconds.
    uint8_t e2e;                    // 1 = person to person, encrypted on the phone.
    char to_pid[5], from_pid[5];    // Phone ids of the receiver and the sender (e2e only).
    // Unsend: the box that wrote the message and that box's number for it, so an X packet can find it.
    uint16_t src;                   // Writer box's 4-character id as a number ("A1B2" = 0xA1B2). 0 = not sent yet.
    uint16_t seq;                   // Text: its M/P/Q/E sequence number. Voice: its clip number.
    uint8_t gone;                   // 1 = the writer unsent it; it is never shown again.
} chat_msg_t;   // 280 bytes each, so the 150-message log uses 42 KB of RAM.

void chat_start(void);                                       // Create the queues and start the radio task.
uint32_t chat_send(const char *from, const char *text);      // Log a message and queue it for sending to everyone.
uint32_t chat_send_to(const char *from, const char *text, const char *peer);   // To one friend box ("" = everyone).
uint32_t chat_send_group(const char *from, const char *text, const char *gid); // To every box in a group.
void chat_apply_settings(void);                              // Tell the radio task that the settings changed.
// Person to person: blob = base64url(nonce + ciphertext + tag), made by the sender's phone.
uint32_t chat_send_e2e(const char *to_box, const char *to_pid, const char *from_pid, const char *blob);
// Log a person-to-person voice message. The clip is encrypted on the phone and stored by app_voice.c.
uint32_t chat_add_e2e_voice(const char *box, const char *devicename, const char *to_pid, const char *from_pid,
                            uint8_t secs, uint8_t local, int16_t rssi, float snr);

// Used by app_voice.c: log a voice message that is secs seconds long.
uint32_t chat_add_voice(const char *from, const char *devicename, const char *text, uint8_t secs,
                        uint8_t local, int16_t rssi, float snr, const char *peer, const char *group);
void     chat_set_status(uint32_t id, uint8_t st, uint8_t acks, uint8_t need);   // Update a message's delivery status.
uint32_t chat_version(void);             // Changes whenever GET /api/messages would return something new.
int      chat_peer_slot(const char *id);                   // Bit number of box id in the need masks.
int      chat_peer_heard(const char *id, int rssi, float snr);   // Record that box id was just heard. Returns its slot.
uint8_t  chat_peers_in_range(void);                        // Bit mask of the boxes heard in the last 90 s.
uint32_t chat_slot_ms(void);                               // Length of one ACK slot in ms.
uint32_t chat_toa_ms(int len);                             // LoRa time on air of one packet in ms.
void chat_write_json(void *req, uint32_t since, const char *pid);   // Answers GET /api/messages (pid = the asking phone).
void json_escape(char *dst, size_t dst_len, const char *src);   // Make text safe to place inside a JSON string.

// Unsend. ids = log ids picked by a phone. Only messages written on this box by that phone (same name, or
// same phone id for person messages) are removed. Other boxes are told with one X packet, sent twice.
// Returns the number removed, or -1 when the previous unsend is still going out (try again).
int  chat_unsend(const uint32_t *ids, int n, const char *from, const char *pid);
void chat_set_origin(uint32_t id, const char *box, uint16_t seq);   // Record a message's writer box and its number there.
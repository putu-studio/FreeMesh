#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

// Voice messages: short clips recorded on the phone, compressed with Codec2 at 1200 bit/s and sent as numbered chunks.
//   The phone records 8000 Hz 16-bit mono sound. 10 s is 160 KB of sound, or 1500 bytes of Codec2.
//   Chunk:   V|<target>|from|<body>     target = "*----" everyone, "pAAAA" private, "g3fa9" group
//            body = [clip hi][clip lo][n][N][per] + (n == 0: "secs|devicename|name|") + Codec2 bytes
//            n = chunk number, N = chunk count, per = clip bytes per chunk.
//            private: body sealed with the friend key. group: body sealed with the group key.
//   Answer:  W|to|from|clip|missing       missing = 8 hex digits, bit n = chunk n still missing. 0 = all received.
//   The sender resends only the missing chunks, for at most 2 more rounds.

#define VOICE_MAX_S       10           // Longest clip, in seconds.
#define VOICE_GAP_S       30           // One clip per 30 s per box, to leave the air free for others.

void voice_init(void);                                               // Call once at startup.

// Web task: one recording.
typedef struct voice_rec voice_rec_t;
voice_rec_t *voice_rec_start(void);                                  // Returns NULL when out of memory.
void voice_rec_feed(voice_rec_t *r, const uint8_t *pcm, int len);    // 16-bit little-endian samples, any piece size.
// Finish: add the chat bubble and queue the clip for the radio. to and group: "" means everyone.
// Returns the message id, or 0 on failure (*why holds the reason).
uint32_t voice_rec_finish(voice_rec_t *r, const char *from, const char *to, const char *group, const char **why);
void voice_rec_drop(voice_rec_t *r);                                 // Free the recording (voice_rec_finish does not).
// Same, for a clip the phone already compressed (Codec2 1200, 6 bytes per 40 ms): no 160 KB upload, no box CPU time.
uint32_t voice_send_codec2(const char *from, const char *to, const char *group, const uint8_t *c2, int len, const char **why);
int  voice_wait_s(void);                                             // Seconds until this box may send the next clip.

// Web task: play a clip as a .wav file (8000 Hz, 16-bit, mono).
// raw: send the stored bytes unchanged (Codec2, or locked) and let the phone decode them with codec2.wasm.
// The box then decodes nothing: no 160 KB WAV, no seconds of CPU time, and the web server stays free.
esp_err_t voice_play(void *httpd_req, uint32_t msg_id, bool raw);
// Voice to one person, end to end: the phone compresses and locks the clip, and the box only carries it.
//   Chunk target "uAAAA" (box AAAA, sealed with the friend key). Chunk 0 names = "secs|devicename|to_pid:from_pid|".
uint32_t voice_send_sealed(const char *to_box, const char *to_pid, const char *from_pid, int secs,
                           const uint8_t *sealed, int len, const char **why);

// Radio task.
void voice_on_packet(char type, uint8_t *buf, int len, int rssi, float snr);   // Handle a received V or W packet.
int  voice_next_packet(uint8_t *out, int max);                       // Next V or W packet to send now, or 0 for none.
void voice_forget(uint32_t msg_id);   // Message unsent: drop its clip and stop sending it.
bool voice_air_busy(void);                                           // Another box's clip is still on the air: wait.

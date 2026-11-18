#pragma once
#include <stdint.h>
#include <stdbool.h>

// SOS everywhere: also send the SOS as a Meshtastic packet and a MeshCore packet.
void sos_everywhere(const char *text);          // Any task: queue the SOS. FreeMesh's own SOS goes through chat_send.
bool sos_radio_step(void);                      // Radio task: send the queued SOS packets. True = it used the radio.
void sos_set_time(uint32_t unix_now);           // Time from the phone: the box has no clock, MeshCore packets carry one.

// Packet builders, also used by the host test.
int sos_build_meshtastic(const char *text, uint32_t from, uint32_t id, uint8_t *out, int max);
int sos_build_meshcore(const char *name, const char *text, uint32_t ts, uint8_t *out, int max);

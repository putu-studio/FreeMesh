#pragma once
#include "esp_err.h" 
#include <stdint.h>
#include <stdbool.h> 
#include "app_settings.h"


#define LORA_MAX_PAYLOAD 200


esp_err_t radio_send(const uint8_t *data, uint8_t len);   // Send one packet and wait until it is on the air (up to 4 s).

void radio_self_test(void);                               // Read the sync word register (0x0740) and print it.
esp_err_t radio_init(void);                               // Reset the SX1262 and apply the saved settings.
void radio_health_printer(void);                          // Print the chip mode, command status and error flags.

esp_err_t radio_start_rx(void);                           // Start continuous receive.
// Check for a received packet. Returns its length, or -1 when there is none or it is corrupted.
int  radio_check_rx(uint8_t *buf, int max_len, int *rssi_dbm, float *snr_db);

esp_err_t radio_apply(const settings_t *s);               // Set frequency, TX power, modulation and sync word.

// Another network's radio settings, used for one SOS packet (see app_sos.c).
typedef struct {
    uint32_t freq_hz;
    uint8_t  sf, bw_code, cr, ldro;     // bw_code: 0x03 = 62.5 kHz, 0x04 = 125, 0x05 = 250. cr: 1 = 4/5.
    uint16_t sync_word;                 // Register value: 0x24B4 = Meshtastic (0x2B), 0x1424 = private (0x12).
    uint16_t preamble;                  // Length in symbols.
} radio_foreign_t;
// Send one packet with these settings. Call radio_apply and radio_start_rx afterwards to return to FreeMesh.
esp_err_t radio_send_foreign(const radio_foreign_t *f, const uint8_t *data, uint8_t len);
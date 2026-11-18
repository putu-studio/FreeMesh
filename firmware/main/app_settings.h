#pragma once
#include "esp_err.h" 
#include <stdint.h>


typedef struct {
    uint32_t    freq_hz;        // For example 915000000.
    uint8_t     sf;             // Spreading factor, 7 to 12.
    uint16_t    bw_khz;         // 125, 250 or 500.
    int8_t      tx_dbm;         // -9 to 22.
    char        devicename[17]; // Up to 16 characters.
} settings_t;

extern settings_t device_settings;  // Current settings, defined in app_settings.c.
void settings_load(void);           // Load from NVS. Defaults apply when nothing valid is saved.
esp_err_t settings_save(void);      // Check, then save to NVS.
const char *settings_check(const settings_t *s);    // NULL if valid, otherwise an error text for the user.
const char *node_id_str(void);      // This box's id: four hex digits from its MAC address, for example "A1B2".




// Factory reset (Profile > "Factory reset"): the box forgets everything and starts like new.
// The erase runs at the very start of the next boot, before anything else uses the flash.
esp_err_t settings_factory_request(void);   // Mark the box for erasing. The caller then restarts it.
void settings_factory_apply(void);          // Call first in app_main: erases if a reset was requested.

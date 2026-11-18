#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

// The box's internet Wi-Fi (app_gw.c): the box joins an internet Wi-Fi, such as a home router or a phone
// hotspot, while its own hotspot stays on. The updater (app_ota.c) uses it.
// The optional gateway add-on, not included in this repository, can use it too and receives Everyone
// messages. The add-on is built only when its source file is in this folder (see CMakeLists.txt).

typedef struct {                 // Saved in NVS "gw". Do not change this layout: boxes in use have it saved.
    uint8_t on;                  // The optional gateway add-on is switched on (never in a build without the add-on).
    char ssid[33];               // Name of the internet Wi-Fi, such as a home router or a phone hotspot.
    char pass[65];
    char token[64];              // Reserved for the optional gateway add-on. Never sent to the page.
} gw_cfg_t;

typedef struct {                 // Status for the pages.
    bool on, online, has_pass, has_token;
    char ssid[33];
} gw_status_t;

void gw_start(void);                                         // Call after wifi_start_ap().
void gw_on_message(const char *from, const char *devicename, const char *text, bool local);   // For each Everyone message.
void gw_get(gw_status_t *st);                                // Fills the status for the pages.

// The updater (app_ota.c) borrows the internet Wi-Fi, also when the optional gateway add-on is off or not built.
void gw_wifi_set(const char *ssid, const char *pass);        // Saves the Wi-Fi. "" pass = keep, or none for a new Wi-Fi.
void gw_wifi_need(bool need);                                // True = join it now, false = done with it.
void gw_wifi_retry(void);                                    // Retries after a drop; call about every 10 s while waiting.
bool gw_online(void);                                        // Joined and has an address.

// Only the optional gateway add-on calls these.
esp_err_t gw_set(bool on, const char *ssid, const char *pass, const char *token);   // "" keeps the old value.
bool gw_is_on(void);                                         // The optional gateway add-on is switched on.
const char *gw_token(void);                                  // "" means none saved.
bool gw_reconnect_due(void);                                 // True once after a Wi-Fi drop; the caller rejoins.

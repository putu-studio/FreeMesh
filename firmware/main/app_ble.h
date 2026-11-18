#pragma once
// app_ble: lets the FreeMesh phone app talk to the box over Bluetooth Low Energy (BLE).
//
// The app sends the same requests the web page sends ("GET /api/messages"), and the box replays them
// to its own web server on 127.0.0.1. Every /api handler therefore stays in one place.
//
// One GATT service with 3 characteristics:
//   REQ  (phone writes)  : request pieces.
//   RSP  (box notifies)  : answer pieces.
//   EVT  (box notifies)  : "something changed" ticks, so the phone does not have to keep asking.
//
// Security: the phone must pair with a 6-digit PIN before it can use REQ, RSP or EVT. The owner can change the PIN.

#include <stdint.h>
#include "esp_err.h"
#include "esp_http_server.h"

void ble_start(void);                     // Call once, after web_start().
uint32_t ble_pin(void);                   // Current 6-digit PIN.
esp_err_t ble_set_pin(uint32_t pin);      // Accepts 100000..999999 and saves it in flash.
void ble_forget_phones(void);             // Deletes every paired phone; each must enter the PIN again.
int ble_phones_connected(void);           // Number of phones connected now.

// For register_all() in app_web.c: GET /api/ble and POST /api/ble.
// POST body: "do=pin&pin=123456", "do=forget", "do=on" or "do=off".
esp_err_t ble_http_get(httpd_req_t *req);
esp_err_t ble_http_post(httpd_req_t *req);

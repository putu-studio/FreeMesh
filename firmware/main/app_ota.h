#pragma once
#include <stdbool.h>
#include "esp_err.h"
#include "esp_partition.h"

// Update over the internet ("OTA"). The box joins a Wi-Fi with internet (home Wi-Fi or a phone hotspot),
// downloads a signed list of the newest FreeMesh, Meshtastic and MeshCore, and installs what changed.
// The top of app_ota.c explains how the update stays safe.

typedef struct {                 // Status for the Update page.
    char state[12];              // One of: idle, joining, checking, downloading, restarting, finishing, done, failed.
    char what[12];               // Program being downloaded.
    int  percent;                // Progress of that download.
    char msg[112];               // Last result, in plain words.
    char version[24];            // Version of this FreeMesh.
    char url[160];               // Address of the update list.
    bool unfinished;             // A program still has to be put back, which needs the internet Wi-Fi again.
} ota_status_t;

void ota_boot_check(void);       // Call once NVS is ready. Counts a new version's starts and goes back after too many.
void ota_start(void);            // Call last at start. Confirms a new version or continues an unfinished update.
esp_err_t ota_begin(const char *ssid, const char *pass, const char *url);   // Starts an update. "" keeps the saved one.
void ota_get(ota_status_t *st);                         // Fills the status for the Update page.
bool ota_busy(void);                                    // An update is running: do not switch mode.
bool ota_is_freemesh_image(const esp_partition_t *p);    // True if the slot holds FreeMesh, not Meshtastic or MeshCore.

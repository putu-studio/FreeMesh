#pragma once
#include <stdbool.h>
#include "esp_err.h"

// The three programs the box can run. Each has its own flash slot (see partitions.csv).
typedef enum {
    MODE_FREEMESH = 0,       // This firmware.
    MODE_MESHTASTIC = 1,    // Meshtastic firmware.
    MODE_MESHCORE = 2,      // MeshCore firmware.
    MODE_COUNT = 3
} box_mode_t;

box_mode_t  mode_current(void);              // Program that is running now.
bool        mode_installed(box_mode_t m);    // True if the slot of that program holds a valid image.
const char *mode_id(box_mode_t m);           // "freemesh", "meshtastic" or "meshcore" (also the partition label).
const char *mode_name(box_mode_t m);         // "FreeMesh", "Meshtastic" or "MeshCore".
int         mode_from_id(const char *id);    // "meshtastic" -> 1. Returns -1 for an unknown id.
esp_err_t   mode_switch(box_mode_t m);       // Select a program for the next start, then restart. Returns only on error.

#pragma once
#include <stdbool.h>

// Battery level (see app_batt.c). On a board without the measuring resistors, present is false.
typedef struct {
    bool present;       // False when this board cannot measure its battery: show nothing.
    int  mv;            // Battery voltage in millivolts, for example 3950.
    int  percent;       // 0 to 100.
} batt_t;

void batt_start(void);          // Call once at start: sets up the pin and detects whether a battery is measured.
void batt_get(batt_t *b);       // Callable from any task. Measures again at most every 5 s.

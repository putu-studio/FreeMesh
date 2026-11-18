#pragma once
#include <stdint.h>

// Events the LED can show besides the mode blinks.
typedef enum {
    IO_EVT_MESSAGE = 1,     // A message arrived over LoRa: LED on for 1 s.
    IO_EVT_SOS     = 2,     // An SOS arrived or went out: fast flicker for 5 s.
    IO_EVT_SENT    = 3,     // A quick message went out: 2 short blinks.
    IO_EVT_ERROR   = 4,     // Something failed: 1 long and 1 short blink.
} io_event_t;

void io_start(void);                 // Set up the LED and buttons and start the io task.
void io_notify(io_event_t e);        // Show an event on the LED. Safe to call from any task.

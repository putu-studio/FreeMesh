// The LED and the buttons.
//
// LED (one green LED, GPIO48): normally off.
//   holding the mode button   fast pulse ("keep holding")
//   choosing (after 5 s)      1, 2 or 3 blinks = the mode on offer (1 FreeMesh, 2 Meshtastic, 3 MeshCore)
//   switching                 fast pulse, then on until the box restarts
//   (LED_SHOW_EVENTS 1 also shows messages, SOS and sent quick messages. A failed mode switch always shows.)
//
// Mode button (the Wio board's button on GPIO21, or an added button on D1):
//   hold 5 s       choosing starts: the LED blinks the next mode's count and moves on every 2.5 s.
//                  Release on the wanted mode: the box restarts in that mode.
//
// SOS button (D3), only when SOS_BUTTON_ON is 1 in board.h:
//   hold 2 s, release   quick message "Please stop at the next safe spot" (LED solid)
//   hold 5 s, release   SOS to every box in range (LED flickers)
//   shorter             nothing, so an accidental bump sends nothing
#include <stdio.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/gpio.h"
#include "board.h"
#include "app_io.h"
#include "app_mode.h"
#include "app_chat.h"
#include "app_settings.h"
#include "app_sos.h"

#define TICK_MS        20
#define HOLD_MODE_MS   5000      // Hold time before choosing starts.
#define STEP_MODE_MS   2500      // Time per mode while choosing.
#define HOLD_QUICK_MS  2000
#define HOLD_SOS_MS    5000
#define PULSE_AFTER_MS 500       // Delay before the "keep holding" pulse starts.
#ifndef LED_SHOW_EVENTS
#define LED_SHOW_EVENTS 0        // 1 = also flash for messages, SOS and sent quick messages.
#endif

static QueueHandle_t s_evq;      // LED events, sent from any task.

void io_notify(io_event_t e)
{
    if (s_evq) { uint8_t v = e; xQueueSend(s_evq, &v, 0); }   // A full queue drops the event.
}

static uint32_t now_ms(void) { return xTaskGetTickCount() * portTICK_PERIOD_MS; }

static void led(bool on) { gpio_set_level(PIN_LED, on ? LED_ON_LEVEL : !LED_ON_LEVEL); }

static bool pressed(gpio_num_t pin) { return gpio_get_level(pin) == 0; }   // A pressed button pulls the pin to GND.

// True while the LED is on in a pattern of n short blinks. phase = time in ms since the pattern started.
static bool blinks(uint32_t phase, int n, uint32_t on_ms, uint32_t period_ms)
{
    return phase < (uint32_t)n * period_ms && (phase % period_ms) < on_ms;
}

static box_mode_t next_installed(box_mode_t from)
{
    for (int i = 1; i <= MODE_COUNT; i++) {
        box_mode_t m = (from + i) % MODE_COUNT;
        if (mode_installed(m)) return m;
    }
    return from;
}

static void io_task(void *arg)
{
    box_mode_t current = mode_current();

    // Mode button state.
    enum { M_IDLE, M_HOLD, M_CHOOSE } mst = M_IDLE;
    uint32_t   m_t0 = 0, m_tc = 0;
    box_mode_t cand = current;

    // SOS button state.
    bool      s_down = false;
    uint32_t   s_t0 = 0;

    // LED event state.
    uint8_t   evt = 0;
    uint32_t   evt_t0 = 0;

    while (1) {
        uint32_t t = now_ms();

        // ---- Events from other tasks ----
        uint8_t e;
        if (xQueueReceive(s_evq, &e, 0) == pdTRUE) { evt = e; evt_t0 = t; }

        // ---- Mode button ----
        bool mdown = pressed(PIN_BTN_WIO) || pressed(PIN_BTN_MODE);
        if (mst == M_IDLE && mdown) { mst = M_HOLD; m_t0 = t; }
        else if (mst == M_HOLD && !mdown) {                  // Released before 5 s: nothing happens.
            mst = M_IDLE;
        }
        else if (mst == M_HOLD && t - m_t0 >= HOLD_MODE_MS) {
            mst = M_CHOOSE; cand = next_installed(current); m_tc = t;
            printf("Mode button: choosing. Let go on the one you want.\n");
        }
        else if (mst == M_CHOOSE && mdown && t - m_tc >= STEP_MODE_MS) {
            cand = next_installed(cand); m_tc = t;
        }
        else if (mst == M_CHOOSE && !mdown) {
            mst = M_IDLE;
            if (cand != current) {
                for (int i = 0; i < 8; i++) {                // Fast pulse: switching.
                    led(i % 2 == 0);
                    vTaskDelay(pdMS_TO_TICKS(100));
                }
                led(true);                                   // Stays on until the chip restarts.
                esp_err_t err = mode_switch(cand);           // Restarts the chip; returns only on error.
                printf("Mode switch failed: %s\n", esp_err_to_name(err));
                evt = IO_EVT_ERROR; evt_t0 = now_ms();
            }
        }

        // ---- SOS button ----
        bool sdown = SOS_BUTTON_ON && pressed(PIN_BTN_SOS);   // Switched off in board.h: never pressed.
        if (!s_down && sdown) { s_down = true; s_t0 = t; }
        else if (s_down && !sdown) {
            s_down = false;
            uint32_t held = t - s_t0;
            if (held >= HOLD_SOS_MS) {
                char text[80];
                snprintf(text, sizeof(text), "SOS from %s. We need help!", device_settings.devicename);
                chat_send("\xF0\x9F\x86\x98 SOS", text);    // Sender name: the SOS emoji (UTF-8 bytes) and "SOS".
                sos_everywhere(text);                        // Also to Meshtastic and MeshCore boxes.
                evt = IO_EVT_SOS; evt_t0 = t;
                printf("SOS sent\n");
            } else if (held >= HOLD_QUICK_MS) {
                chat_send(device_settings.devicename, "Please stop at the next safe spot");
                evt = IO_EVT_SENT; evt_t0 = t;
                printf("Quick message sent\n");
            }
        }

        // ---- What the LED shows, most important first ----
        bool on = false;                                     // Normally off.
        uint32_t held_sos = s_down ? t - s_t0 : 0;
        if (mst == M_CHOOSE) {
            on = blinks(t - m_tc, cand + 1, 200, 400);       // Blink count of the candidate mode.
        } else if (mst == M_HOLD && t - m_t0 >= PULSE_AFTER_MS) {
            on = (t / 100) % 2;                              // Holding: fast pulse.
        } else if (s_down && held_sos >= HOLD_SOS_MS) {
            on = (t / 60) % 2;                               // Armed for SOS: fast flicker.
        } else if (s_down && held_sos >= HOLD_QUICK_MS) {
            on = true;                                       // Armed for the quick message: solid.
        } else if (LED_SHOW_EVENTS && evt == IO_EVT_MESSAGE && t - evt_t0 < 1000) {
            on = true;
        } else if (LED_SHOW_EVENTS && evt == IO_EVT_SOS && t - evt_t0 < 5000) {
            on = (t / 60) % 2;
        } else if (LED_SHOW_EVENTS && evt == IO_EVT_SENT && t - evt_t0 < 700) {
            on = blinks(t - evt_t0, 2, 120, 300);
        } else if (evt == IO_EVT_ERROR && t - evt_t0 < 1500) {   // A failed mode switch is always shown.
            on = (t - evt_t0 < 800) || (t - evt_t0 >= 1100 && t - evt_t0 < 1300);
        }
        led(on);

        vTaskDelay(pdMS_TO_TICKS(TICK_MS));
    }
}

void io_start(void)
{
    gpio_config_t out = { .pin_bit_mask = 1ULL << PIN_LED, .mode = GPIO_MODE_OUTPUT };
    gpio_config(&out);
    led(false);

    gpio_config_t btn = {
        .pin_bit_mask = (1ULL << PIN_BTN_WIO) | (1ULL << PIN_BTN_MODE) | (SOS_BUTTON_ON ? (1ULL << PIN_BTN_SOS) : 0),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,            // Not pressed = HIGH, pressed = LOW (to GND).
    };
    gpio_config(&btn);

    s_evq = xQueueCreate(4, sizeof(uint8_t));
    xTaskCreate(io_task, "io", 4096, NULL, 3, NULL);
    printf("Mode: %s. Hold the mode button 5 s to switch.\n", mode_name(mode_current()));
}

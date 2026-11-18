// Battery level.
//
// The chip measures up to about 3 V, and a full battery is 4.2 V. The carrier board therefore feeds the
// battery through two equal resistors (100k + 100k, with 100 nF to steady it) to pin D0 / GPIO1.
// The pin sees half the battery voltage, and the code multiplies by 2 (BAT_DIVIDER in board.h).
//
// A lithium battery falls from 4.2 V (full) to about 3.3 V (empty), but not in a straight line, so a
// table converts volts to percent. The reading is higher while the charger is plugged in, and the board
// has no "USB plugged in" signal, so the percent can jump up while charging.
//
// On a board without the two resistors the pin floats and reads noise. batt_start() detects this with
// the chip's own pull-down resistor: an unconnected pin falls to 0 V, and the battery resistors keep
// it well above. A board that cannot measure its battery shows no level.
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_timer.h"
#include "board.h"
#include "app_batt.h"

#define BAT_MIN_MV  2800        // A reading outside 2.8 V to 4.5 V is not a lithium battery.
#define BAT_MAX_MV  4500
#define AGAIN_US    5000000     // Measure again at most every 5 s.

static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t s_cali;        // Factory calibration of the chip: raw number -> millivolts.
static bool    s_present;
static int     s_mv;                    // Smoothed battery millivolts.
static int64_t s_at;                    // Time of the last measurement.

// Volts to percent for a resting lithium (LiPo or Li-ion) cell, highest first.
static const struct { int mv, pct; } TABLE[] = {
    { 4200, 100 }, { 4150, 95 }, { 4110, 90 }, { 4080, 85 }, { 4020, 80 }, { 3980, 75 }, { 3950, 70 },
    { 3910, 65 }, { 3870, 60 }, { 3850, 55 }, { 3840, 50 }, { 3820, 45 }, { 3800, 40 }, { 3790, 35 },
    { 3770, 30 }, { 3750, 25 }, { 3730, 20 }, { 3710, 15 }, { 3690, 10 }, { 3610, 5 }, { 3270, 0 },
};
#define ROWS ((int)(sizeof(TABLE) / sizeof(TABLE[0])))

static int percent_of(int mv)
{
    if (mv >= TABLE[0].mv) return 100;
    for (int i = 1; i < ROWS; i++) {
        if (mv >= TABLE[i].mv) {                             // Straight line between row i (lower) and row i-1 (higher).
            int span = TABLE[i - 1].mv - TABLE[i].mv;
            return TABLE[i].pct + (mv - TABLE[i].mv) * (TABLE[i - 1].pct - TABLE[i].pct) / span;
        }
    }
    return 0;
}

// Millivolts at the pin now: the average of 16 readings, because one reading is noisy. -1 = cannot read.
static int pin_mv(void)
{
    int sum = 0, n = 0;
    for (int i = 0; i < 16; i++) {
        int raw, mv;
        if (adc_oneshot_read(s_adc, BAT_ADC_CHANNEL, &raw) != ESP_OK) continue;
        if (s_cali && adc_cali_raw_to_voltage(s_cali, raw, &mv) == ESP_OK) sum += mv;
        else sum += raw * 3100 / 4095;                       // No calibration: estimate (12 bits, 3.1 V full scale).
        n++;
    }
    return n ? sum / n : -1;
}

void batt_start(void)
{
    adc_oneshot_unit_init_cfg_t unit = { .unit_id = ADC_UNIT_1 };
    if (adc_oneshot_new_unit(&unit, &s_adc) != ESP_OK) { printf("Battery: can't start the measuring part\n"); return; }
    // 12 dB attenuation measures up to about 3.1 V.
    adc_oneshot_chan_cfg_t ch = { .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT };
    if (adc_oneshot_config_channel(s_adc, BAT_ADC_CHANNEL, &ch) != ESP_OK) { printf("Battery: can't set up pin D0\n"); return; }
    adc_cali_curve_fitting_config_t cal = { .unit_id = ADC_UNIT_1, .chan = BAT_ADC_CHANNEL, .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT };
    if (adc_cali_create_scheme_curve_fitting(&cal, &s_cali) != ESP_OK) s_cali = NULL;

#ifdef FREEMESH_TEST_BATT
    // Test build only (idf.py -DFREEMESH_TEST_BATT=1 build): a bench board has no resistors, so the chip's
    // pull-up and pull-down together fake a battery. The pin sits near half of 3.3 V, like a 3.3 V battery.
    gpio_set_pull_mode(PIN_BAT_SENSE, GPIO_PULLUP_PULLDOWN);
    vTaskDelay(pdMS_TO_TICKS(30));
    s_present = true;
    printf("Battery: TEST BUILD, faking a battery with the chip's own resistors\n");
#else
    gpio_set_pull_mode(PIN_BAT_SENSE, GPIO_PULLDOWN_ONLY);   // Detects an unconnected pin (see the top of this file).
    vTaskDelay(pdMS_TO_TICKS(30));
    int pulled = pin_mv();
    gpio_set_pull_mode(PIN_BAT_SENSE, GPIO_FLOATING);
    vTaskDelay(pdMS_TO_TICKS(50));                           // Lets the 100 nF capacitor settle again.
    s_present = pulled > 300;
    if (!s_present) { printf("Battery: this board does not measure its battery (pin D0 reads %d mV when pulled down)\n", pulled); return; }
#endif
    batt_t b; batt_get(&b);
    if (b.present) printf("Battery: %d.%02d V = %d%%\n", b.mv / 1000, (b.mv % 1000) / 10, b.percent);
    else printf("Battery: pin D0 reads %d.%02d V as a battery, that is not a lithium battery: hidden\n", b.mv / 1000, (b.mv % 1000) / 10);
}

void batt_get(batt_t *b)
{
    b->present = false; b->mv = 0; b->percent = 0;
    if (!s_adc || !s_present) return;
    int64_t now = esp_timer_get_time();
    if (!s_at || now - s_at > AGAIN_US) {
        int pin = pin_mv();
        if (pin >= 0) {
            int mv = pin * BAT_DIVIDER;
            s_mv = s_at ? (s_mv * 3 + mv) / 4 : mv;          // Smoothed: a radio burst pulls the voltage down briefly.
            s_at = now;
        }
    }
    b->mv = s_mv;
#ifndef FREEMESH_TEST_BATT
    if (s_mv < BAT_MIN_MV || s_mv > BAT_MAX_MV) return;      // Not a battery: stay hidden.
#endif
    b->present = true;
    b->percent = percent_of(s_mv);
}

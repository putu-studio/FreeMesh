#pragma once
#include "driver/gpio.h"            // Defines the GPIO_NUM_x names.

#define PIN_LORA_SCK    GPIO_NUM_7  // XIAO D8: SPI clock.
#define PIN_LORA_MISO   GPIO_NUM_8  // XIAO D9: data from the radio to the ESP32.
#define PIN_LORA_MOSI   GPIO_NUM_9  // XIAO D10: data from the ESP32 to the radio.
#define PIN_LORA_NSS    GPIO_NUM_41 // SPI chip select.
#define PIN_LORA_RST    GPIO_NUM_42 // Radio reset.
#define PIN_LORA_DIO1   GPIO_NUM_39 // Interrupt line from the radio.
#define PIN_LORA_BUSY   GPIO_NUM_40 // High = the radio is busy.
#define PIN_LORA_RXEN   GPIO_NUM_38 // High = antenna switched to receive.
#define LORA_TX_DBM     14          // Transmit power in dBm (14 = 25 mW, SX1262 maximum 22). Check the local limit.


#define LORA_FREQ_HZ 915000000

// ---------- Battery level (carrier board; app_batt.c) ----------
#define PIN_BAT_SENSE   GPIO_NUM_1      // XIAO D0: battery voltage through two equal resistors (100k + 100k).
#define BAT_ADC_CHANNEL ADC_CHANNEL_0   // GPIO1 is ADC1 channel 0 on the ESP32-S3.
#define BAT_DIVIDER     2               // The pin sees half the battery voltage, so pin volts x 2 = battery volts.

// ---------- Buttons and LED ----------
#define PIN_LED         GPIO_NUM_48 // Green LED on the Wio-SX1262 board.
#define LED_ON_LEVEL    1           // 1 = HIGH turns the LED on. Set it to 0 if the LED is on when it should be off.
#define PIN_BTN_WIO     GPIO_NUM_21 // Mode button: the Wio board's user button (this pin is also the XIAO's orange LED).
#define PIN_BTN_MODE    GPIO_NUM_2  // XIAO D1: optional added mode button, wired to GND.
#define PIN_BTN_SOS     GPIO_NUM_4  // XIAO D3: SOS and quick-message button, wired to GND.
#define SOS_BUTTON_ON   0           // 0 = SOS button off (nothing is sent). 1 = on.

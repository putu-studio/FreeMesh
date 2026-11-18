#include <stdio.h>
#include <stdlib.h>             // abort (test build only)
#include "board.h"
#include "app_radio.h"
#include "nvs_flash.h"
#include "app_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "app_web.h"
#include "app_chat.h"
#include "app_settings.h"
#include "app_io.h"
#include "app_crypto.h"
#include "app_friends.h"
#include "app_groups.h"
#include "app_voice.h"
#include "app_gw.h"
#include "app_people.h"
#include "app_ble.h"
#include "app_ota.h"
#include "app_batt.h"
#include "esp_heap_caps.h"
#include "esp_system.h"         // esp_reset_reason

void app_main(void)
{
    // Print why the box started: 1 = power on, 3 = software restart (settings or mode change), 4 = crash (panic),
    // 5/6/7 = watchdog (a task was stuck), 9 = brownout (supply voltage too low). See esp_reset_reason_t.
    static const char *why[] = { "unknown", "power on", "external pin", "software restart", "CRASH (panic)",
        "watchdog (interrupt)", "watchdog (task)", "watchdog (other)", "deep sleep", "BROWNOUT (power too weak)", "SDIO", "USB", "JTAG", "eFuse", "power glitch", "CPU lockup" };
    int rr = (int)esp_reset_reason();
    printf("Start reason: %d = %s\n", rr, rr < (int)(sizeof(why) / sizeof(why[0])) ? why[rr] : "?");
    printf("RAM at start: %u KB\n", (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)/1024));
    esp_err_t e = nvs_flash_init();

    if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND){
        ESP_ERROR_CHECK(nvs_flash_erase());
        e = nvs_flash_init();
    }
    ESP_ERROR_CHECK(e);

    settings_factory_apply();        // Carry out a requested factory reset before any module reads its data.
    ota_boot_check();               // Update: count the tries of a new version and roll back if it never works.
#ifdef FREEMESH_TEST_CRASH            // Test build only (idf.py -DFREEMESH_TEST_CRASH=1 build): it stops on purpose at start, to test the update fallback.
    printf("TEST BUILD: crashing on purpose\n");   // A version that never starts shows that the box returns to the old version.
    abort();
#endif
    settings_load();
    crypto_init();                   // Network lock: load the key (if any) before the radio starts.
    friends_init();                  // This box's key pair and saved friends.
    people_init();                   // People who use this box.
    groups_init();                   // Saved groups and their keys.
    voice_init();                    // Voice messages.
    io_start();                      // LED and buttons first, so the mode button works even if the radio fails.
    batt_start();                    // Battery level (only on boards with the two measuring resistors on D0).

    if (radio_init() != ESP_OK) { 
        printf("Radio init failed\n");
        return;
    }

    radio_self_test();
    radio_health_printer();

    chat_start();     

    wifi_start_ap();     
    gw_start();                      // Internet Wi-Fi for the updater and the optional gateway add-on.
    web_start();     
    ble_start();                     // Bluetooth for the phone app (PIN pairing).
    ota_start();                     // Everything started: confirm a new version and resume an unfinished update.

    for (int s = 0; ; s++) {        // RAM check: print free RAM now, then every 60 s.
        if (s % 60 == 0) {
            printf("RAM: %u KB free, lowest ever %u KB, biggest block %u KB\n",
                (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
                (unsigned)(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL) / 1024),
                (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024));
            // Stack check: report any task that has ever come within 1 KB of the end of its stack.
            static TaskStatus_t ts[24];
            int n = uxTaskGetSystemState(ts, 24, NULL);
            for (int i = 0; i < n; i++)
                if (ts[i].usStackHighWaterMark < 1024)
                    printf("STACK LOW: task \"%s\" had only %u bytes left\n", ts[i].pcTaskName, (unsigned)ts[i].usStackHighWaterMark);
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

}

#include <string.h>
#include <stdio.h>
#include "esp_ota_ops.h"            // Boot slot selection and program headers.
#include "esp_partition.h"          // Partition lookup by label.
#include "esp_system.h"             // esp_restart.
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "app_mode.h"
#include "app_ota.h"

static const char *IDS[MODE_COUNT]   = { "freemesh", "meshtastic", "meshcore" };   // Also the partition labels.
static const char *NAMES[MODE_COUNT] = { "FreeMesh", "Meshtastic", "MeshCore" };

static const esp_partition_t *slot(box_mode_t m)
{
    if (m < 0 || m >= MODE_COUNT) return NULL;
    return esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, IDS[m]);
}

const char *mode_id(box_mode_t m)   { return (m >= 0 && m < MODE_COUNT) ? IDS[m] : "?"; }
const char *mode_name(box_mode_t m) { return (m >= 0 && m < MODE_COUNT) ? NAMES[m] : "?"; }

int mode_from_id(const char *id)
{
    for (int i = 0; i < MODE_COUNT; i++) if (strcmp(id, IDS[i]) == 0) return i;
    return -1;
}

box_mode_t mode_current(void)
{
    // This firmware is FreeMesh whichever slot it runs from: during an update the new FreeMesh
    // runs briefly from the MeshCore slot (see app_ota.c).
    return MODE_FREEMESH;
}

bool mode_installed(box_mode_t m)
{
    const esp_partition_t *p = slot(m);
    if (!p) return false;                    // This flash layout has no such slot.
    if (m == MODE_FREEMESH) return true;      // The running program.
    esp_app_desc_t d;
    if (esp_ota_get_partition_description(p, &d) != ESP_OK) return false;   // No valid program header in the slot.
    return !ota_is_freemesh_image(p);         // During an update the MeshCore slot holds a FreeMesh copy, not MeshCore.
}

esp_err_t mode_switch(box_mode_t m)
{
    if (ota_busy()) return ESP_ERR_INVALID_STATE;               // An update is writing the slots.
    if (!mode_installed(m)) return ESP_ERR_NOT_FOUND;
    esp_err_t err = esp_ota_set_boot_partition(slot(m));        // Records the slot to start next in otadata.
    if (err != ESP_OK) return err;
    printf("Switching to %s, restarting...\n", NAMES[m]);
    vTaskDelay(pdMS_TO_TICKS(300));          // Lets the log and any web response go out first.
    esp_restart();
    return ESP_OK;                           // Not reached.
}

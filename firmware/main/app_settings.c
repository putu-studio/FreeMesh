#include "app_settings.h"
#include "esp_mac.h"    // Device MAC address.
#include <stdio.h>
#include <string.h>
#include "board.h"
#include "nvs.h"
#include <stdbool.h>
#include "nvs_flash.h"          // Factory reset: erases the whole settings area.
#include "esp_partition.h"      // Factory reset: erases the storage of Meshtastic and MeshCore.

settings_t device_settings;

static const settings_t DEFAULTS = {
        .freq_hz = LORA_FREQ_HZ,
        .sf = 9,
        .bw_khz = 125,
        .tx_dbm = LORA_TX_DBM,
};

const char *node_id_str(void){
    static char id[5];
    if (id[0] == '\0'){
        uint8_t mac[6];
        esp_read_mac(mac, ESP_MAC_BASE);
        snprintf(id, sizeof(id), "%02X%02X", mac[4], mac[5]);
        return id;
    }
    return id;
}

const char *settings_check(const settings_t *s)
{
    if (s->freq_hz < 902000000 || s->freq_hz > 928000000) return "Frequency must be 902-928 MHz";
    if (s->sf < 7 || s->sf > 12)                           return "Spreading factor must be between 7-12";
    if (s->bw_khz != 125 && s->bw_khz != 250 && s->bw_khz != 500)
                                                           return "Bandwidth must be 125, 250 or 500 kHz";
    if (s->tx_dbm < -9 || s->tx_dbm > 22)                  return "TX power must be -9 to 22 dBm";
    if (s->devicename[0] == '\0')                          return "Device name can't be empty";
    return NULL;                                           // All checks passed.
}


void settings_load(void){

    device_settings = DEFAULTS;
    snprintf(device_settings.devicename, sizeof(device_settings.devicename),
             "LoRa %s", node_id_str()); 
    
    settings_t temp_settings;

    size_t len = sizeof(temp_settings);
    nvs_handle_t h;  
    esp_err_t err = nvs_open("lora", NVS_READONLY, &h);                 
    if (err != ESP_OK){
        return;
    }   

    err = nvs_get_blob(h, "configs", &temp_settings, &len);   
    nvs_close(h);

    if (err == ESP_OK && len == sizeof(temp_settings) && settings_check(&temp_settings) == NULL) {
        device_settings = temp_settings;                    
    }
    return;
}


esp_err_t settings_save(void){
    
    if (settings_check(&device_settings) != NULL) return ESP_ERR_INVALID_STATE;

    nvs_handle_t h;  
    esp_err_t err = nvs_open("lora", NVS_READWRITE, &h); 
    if (err != ESP_OK){
        printf("Could not access storage\n");
        return ESP_FAIL;
    }

    err = nvs_set_blob(h, "configs", &device_settings, sizeof(device_settings)); 
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);

    return err;

}




// ---------- Factory reset ----------
// Everything the box remembers is in the NVS flash area: names, radio settings, network lock, people,
// friend boxes, groups, Bluetooth PIN and paired phones, the internet Wi-Fi and its password, the
// settings of the optional gateway add-on, this box's key pair and https certificate. Meshtastic and MeshCore keep their
// settings in two storage areas of their own. A reset erases all of it except the update counter
// (NVS "ota"/"st"), so a reset box still refuses an update older than the one it has.
#define FACTORY_FLAG "factory"

esp_err_t settings_factory_request(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open("lora", NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_u8(h, FACTORY_FLAG, 1);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

void settings_factory_apply(void)
{
    nvs_handle_t h;
    uint8_t asked = 0;
    if (nvs_open("lora", NVS_READONLY, &h) != ESP_OK) return;       // Nothing saved yet: the box is already like new.
    nvs_get_u8(h, FACTORY_FLAG, &asked);
    nvs_close(h);
    if (!asked) return;
    printf("FACTORY RESET: erasing everything this box remembers\n");
    uint8_t keep[32]; size_t keep_len = sizeof(keep);               // The update counter survives the erase.
    bool have = false;
    if (nvs_open("ota", NVS_READONLY, &h) == ESP_OK) {
        have = nvs_get_blob(h, "st", keep, &keep_len) == ESP_OK;
        nvs_close(h);
    }
    nvs_flash_deinit();
    ESP_ERROR_CHECK(nvs_flash_erase());
    ESP_ERROR_CHECK(nvs_flash_init());
    if (have && nvs_open("ota", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_blob(h, "st", keep, keep_len);
        nvs_commit(h);
        nvs_close(h);
    }
    static const char *areas[] = { "mtfs", "mcfs" };                // Storage areas of Meshtastic and MeshCore.
    for (int i = 0; i < 2; i++) {
        const esp_partition_t *p = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, areas[i]);
        if (p && esp_partition_erase_range(p, 0, p->size) == ESP_OK) printf("FACTORY RESET: %s storage erased\n", areas[i]);
    }
    printf("FACTORY RESET: done, starting like new\n");
}

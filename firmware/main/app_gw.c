// The box's internet Wi-Fi: it joins an internet Wi-Fi, such as a home router or a phone hotspot, while its own
// hotspot stays on (AP + station). Two users:
//   - The updater (app_ota.c) borrows it to download a new version.
//   - The optional gateway add-on (built only with FREEMESH_BRIDGE) keeps it joined and receives Everyone messages.
// The saved settings (NVS "gw") live here because both users need the Wi-Fi name and password.
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "nvs.h"
#include "app_gw.h"
#ifdef FREEMESH_BRIDGE
#include "app_bridge.h"
#endif

static gw_cfg_t s_cfg;                   // Saved settings. The password and the add-on's reserved field never go back to the page.
static volatile bool s_online;           // Wi-Fi with internet is up.
static esp_netif_t  *s_sta;
static bool s_started;
static volatile bool s_reconnect;    // The internet Wi-Fi dropped: the optional gateway add-on tries again every 5 s.
static volatile bool s_need;         // The updater wants the internet Wi-Fi, even when the optional gateway add-on is off.

// ---------- Settings in NVS ----------
static void load(void)
{
    memset(&s_cfg, 0, sizeof(s_cfg));
    nvs_handle_t h;
    if (nvs_open("lora", NVS_READONLY, &h) != ESP_OK) return;
    size_t l = sizeof(s_cfg);
    if (nvs_get_blob(h, "gw", &s_cfg, &l) != ESP_OK || l != sizeof(s_cfg)) memset(&s_cfg, 0, sizeof(s_cfg));
    nvs_close(h);
#ifndef FREEMESH_BRIDGE
    s_cfg.on = 0;                        // This build has no optional gateway add-on: nothing keeps the internet Wi-Fi joined.
#endif
}

static void save(void)
{
    nvs_handle_t h;
    if (nvs_open("lora", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, "gw", &s_cfg, sizeof(s_cfg));
    nvs_commit(h);
    nvs_close(h);
}

// ---------- Wi-Fi (the box's own hotspot stays on: AP + station) ----------
static void on_wifi(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) esp_wifi_connect();
    else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_online = false;
        s_reconnect = true;                                                 // Rejoin later: this handler must stay quick.
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        s_online = true;
        printf("Gateway: online via \"%s\"\n", s_cfg.ssid);
    }
}

static void wifi_join(void)
{
    if ((!s_cfg.on && !s_need) || !s_cfg.ssid[0]) { esp_wifi_disconnect(); s_online = false; return; }
    if (!s_sta) {
        s_sta = esp_netif_create_default_wifi_sta();
        esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi, NULL);
        esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_wifi, NULL);
    }
    wifi_config_t sta = { 0 };
    strlcpy((char *)sta.sta.ssid, s_cfg.ssid, sizeof(sta.sta.ssid));
    strlcpy((char *)sta.sta.password, s_cfg.pass, sizeof(sta.sta.password));
    esp_wifi_set_mode(WIFI_MODE_APSTA);       // The hotspot moves to the internet Wi-Fi's channel.
    esp_wifi_set_config(WIFI_IF_STA, &sta);
    esp_wifi_disconnect();
    esp_wifi_connect();
}

// ---------- For the rest of the box ----------
void gw_start(void)
{
    load();
#ifdef FREEMESH_BRIDGE
    bridge_start();
#endif
    s_started = true;
    wifi_join();
}

void gw_on_message(const char *from, const char *devicename, const char *text, bool local)
{
#ifdef FREEMESH_BRIDGE
    if (s_started && s_cfg.on) bridge_on_message(from, devicename, text, local);
#endif
}

void gw_get(gw_status_t *st)
{
    memset(st, 0, sizeof(*st));
    st->on = s_cfg.on; st->online = s_online;
    strlcpy(st->ssid, s_cfg.ssid, sizeof(st->ssid));
    st->has_pass = s_cfg.pass[0] != 0; st->has_token = s_cfg.token[0] != 0;
}

// ---------- The updater borrows the internet Wi-Fi ----------
bool gw_online(void) { return s_online; }

void gw_wifi_set(const char *ssid, const char *pass)
{
    bool other = ssid[0] && strcmp(ssid, s_cfg.ssid) != 0;
    if (other) { strlcpy(s_cfg.ssid, ssid, sizeof(s_cfg.ssid)); s_cfg.pass[0] = 0; }   // New Wi-Fi: drop the old password.
    if (pass[0]) strlcpy(s_cfg.pass, pass, sizeof(s_cfg.pass));
    save();
    if (other && s_started && s_cfg.on) wifi_join();         // The optional gateway add-on moves to the new Wi-Fi too.
}

void gw_wifi_need(bool need)
{
    if (s_need == need) return;
    s_need = need;
    if (!s_started) return;
    if (need && s_online) return;                            // Already joined (the optional gateway add-on uses it).
    if (!need && s_cfg.on) return;                           // The optional gateway add-on still wants it.
    wifi_join();
}

void gw_wifi_retry(void)
{
    if (s_reconnect && s_need && !s_cfg.on) { s_reconnect = false; esp_wifi_connect(); }   // When the optional gateway add-on is on, its task does this.
}

// ---------- Only the optional gateway add-on calls these ----------
bool gw_is_on(void) { return s_cfg.on; }
const char *gw_token(void) { return s_cfg.token; }

bool gw_reconnect_due(void)
{
    if (!s_reconnect) return false;
    s_reconnect = false;
    return true;
}

esp_err_t gw_set(bool on, const char *ssid, const char *pass, const char *token)
{
#ifndef FREEMESH_BRIDGE
    on = false;
#endif
    if (on && (!ssid[0] && !s_cfg.ssid[0])) return ESP_ERR_INVALID_ARG;
    if (strlen(token) >= sizeof(s_cfg.token)) return ESP_ERR_INVALID_SIZE;
    if (token[0]) strlcpy(s_cfg.token, token, sizeof(s_cfg.token));        // Empty keeps the old one.
    if (ssid[0]) strlcpy(s_cfg.ssid, ssid, sizeof(s_cfg.ssid));
    if (pass[0]) strlcpy(s_cfg.pass, pass, sizeof(s_cfg.pass));            // Empty keeps the old one.
    s_cfg.on = on;
    save();
#ifdef FREEMESH_BRIDGE
    bridge_wake();
#endif
    wifi_join();
    return ESP_OK;
}

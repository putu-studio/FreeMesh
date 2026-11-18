#include <string.h>
#include <stdio.h>
#include "esp_wifi.h" 
#include "esp_netif.h" // Network interface (IP address, DHCP).
#include "esp_event.h"
#include "esp_log.h"
#include "dhcpserver/dhcpserver.h"   // OFFER_DNS
#include "dns_server.h"              // components/dns_server, from the ESP-IDF captive portal example.
#include "app_settings.h"
#include "app_wifi.h"
#include "secrets.h"

// Keep phones on the box's Wi-Fi. A phone that decides a Wi-Fi network has no internet drops it (often
// when the screen sleeps) and returns to mobile data. Phones test a network by fetching one known address
// (Android /generate_204, Apple /hotspot-detect.html, Windows /connecttest.txt). So DHCP names the box as
// the DNS server, the box answers every name with its own address, and app_web.c answers the test pages
// the way each phone expects. Side effect: on the box's Wi-Fi a phone does not use mobile data for internet.
static void keep_phones_connected(esp_netif_t *netif)
{
    esp_netif_ip_info_t ip;
    esp_netif_get_ip_info(netif, &ip);
    esp_netif_dns_info_t dns = { 0 };
    dns.ip.type = ESP_IPADDR_TYPE_V4;
    dns.ip.u_addr.ip4.addr = ip.ip.addr;
    dhcps_offer_t offer = OFFER_DNS;
    esp_netif_dhcps_stop(netif);
    esp_netif_dhcps_option(netif, ESP_NETIF_OP_SET, ESP_NETIF_DOMAIN_NAME_SERVER, &offer, sizeof(offer));
    esp_netif_set_dns_info(netif, ESP_NETIF_DNS_MAIN, &dns);
    esp_netif_dhcps_start(netif);

    esp_log_level_set("example_dns_redirect_server", ESP_LOG_WARN);          // Logging every lookup slows the box.
    dns_server_config_t cfg = DNS_SERVER_CONFIG_SINGLE("*", "WIFI_AP_DEF");   // Every name resolves to 192.168.4.1.
    if (!start_dns_server(&cfg)) printf("DNS: could not start\n");
}

void wifi_start_ap(void){
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_t *ap_netif = esp_netif_create_default_wifi_ap();

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));


    wifi_config_t ap = {
        .ap.channel = 1, // Channel 6 or 11 works too.
        .ap.max_connection = 10, 
        .ap.authmode = WIFI_AUTH_WPA2_PSK,
        .ap.password = WIFI_PASSWORD,
    };

    snprintf((char *)ap.ap.ssid, sizeof(ap.ap.ssid),
             "LORA-%s",node_id_str());
    ap.ap.ssid_len = strlen((char *)ap.ap.ssid);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
    ESP_ERROR_CHECK(esp_wifi_start());
    printf("Wi-Fi hotspot %s started, open http://192.168.4.1\n", (char *)ap.ap.ssid);
    keep_phones_connected(ap_netif);
}
// Copy to firmware/main/secrets.h and change the value. secrets.h is gitignored. Never commit real values.
#pragma once
#define WIFI_PASSWORD "change-me-8chars"   // Password of the box's own Wi-Fi (WPA2), at least 8 characters.
// Not here on purpose: the network-lock passphrase, the internet Wi-Fi and the Bluetooth PIN.
// Those are typed on the box's page and saved in the box's NVS.

#pragma once

#define WIFI_SSID "your-wifi-ssid"
#define WIFI_PSK "your-wifi-password"
#define OTA_UPDATE_PASSWORD "replace-with-a-long-random-password"
#define WIFI_2_SSID "your-work-wifi-ssid"
#define WIFI_2_PSK "your-work-wifi-password"
#define WIFI_3_SSID "your-hotspot-ssid"
#define WIFI_3_PSK "your-hotspot-password"
#define CLOUD_WS_URL "wss://car.garyjs.com/ws/device"
#define CLOUD_DEVICE_ID "obd-bridge-01"
#define CLOUD_DEVICE_TOKEN "replace-with-a-long-random-device-token"
// Paste the PEM root CA that signs the HTTPS certificate for car.garyjs.com.
// Never disable certificate validation. Keep this value with the other local secrets.
#define CLOUD_CA_CERT ""

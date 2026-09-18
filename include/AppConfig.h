#pragma once

#include <Arduino.h>
#include <Preferences.h>
#include <vector>

#if __has_include("secrets.h")
#include "secrets.h"
#else
#define WIFI_SSID "your-wifi-ssid"
#define WIFI_PSK "your-wifi-password"
#define OTA_UPDATE_PASSWORD "replace-with-a-long-random-password"
#define CLOUD_WS_URL "wss://car.garyjs.com/ws/device"
#define CLOUD_DEVICE_ID "obd-bridge-01"
#define CLOUD_DEVICE_TOKEN "replace-with-a-long-random-device-token"
#define CLOUD_CA_CERT ""
#define WIFI_2_SSID ""
#define WIFI_2_PSK ""
#define WIFI_3_SSID ""
#define WIFI_3_PSK ""
#endif

#ifndef WIFI_2_SSID
#define WIFI_2_SSID ""
#endif
#ifndef WIFI_2_PSK
#define WIFI_2_PSK ""
#endif
#ifndef WIFI_3_SSID
#define WIFI_3_SSID ""
#endif
#ifndef WIFI_3_PSK
#define WIFI_3_PSK ""
#endif
#ifndef CLOUD_WS_URL
#define CLOUD_WS_URL "wss://car.garyjs.com/ws/device"
#endif
#ifndef CLOUD_DEVICE_ID
#define CLOUD_DEVICE_ID "obd-bridge-01"
#endif
#ifndef CLOUD_DEVICE_TOKEN
#define CLOUD_DEVICE_TOKEN "replace-with-a-long-random-device-token"
#endif
#ifndef CLOUD_CA_CERT
#define CLOUD_CA_CERT ""
#endif

struct WifiProfile
{
    String Ssid;
    String Password;
};

struct AppConfig
{
    std::vector<WifiProfile> WifiProfiles;
    String WifiSsid;
    String WifiPassword;
    uint8_t ObdMac[6];
    uint16_t TcpPort;
    String Hostname;
    String CloudUrl;
    String DeviceId;
    String DeviceToken;
    String CloudCaCert;

    static AppConfig Load();
};

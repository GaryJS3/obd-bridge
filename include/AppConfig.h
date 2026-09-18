#pragma once

#include <Arduino.h>
#include <Preferences.h>

#if __has_include("secrets.h")
#include "secrets.h"
#else
#define WIFI_SSID "your-wifi-ssid"
#define WIFI_PSK "your-wifi-password"
#define OTA_UPDATE_PASSWORD "replace-with-a-long-random-password"
#endif

struct AppConfig
{
    String WifiSsid;
    String WifiPassword;
    uint8_t ObdMac[6];
    uint16_t TcpPort;
    String Hostname;

    static AppConfig Load();
};

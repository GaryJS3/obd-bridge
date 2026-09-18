#include "AppConfig.h"

AppConfig AppConfig::Load()
{
    AppConfig config{};
    const uint8_t defaultMac[6] = {0x00, 0x04, 0x3E, 0x5F, 0x5E, 0x9C};
    memcpy(config.ObdMac, defaultMac, sizeof(config.ObdMac));
    config.WifiSsid = WIFI_SSID;
    config.WifiPassword = WIFI_PSK;
    config.TcpPort = 35000;
    config.Hostname = "obd-bridge";

    Preferences preferences;
    if (!preferences.begin("obd-bridge", false))
    {
        Serial.println("[CONFIG] NVS unavailable; using compiled defaults");
        return config;
    }

    if (preferences.isKey("wifi_ssid"))
    {
        config.WifiSsid = preferences.getString("wifi_ssid", config.WifiSsid);
    }
    if (preferences.isKey("wifi_psk"))
    {
        config.WifiPassword = preferences.getString("wifi_psk", config.WifiPassword);
    }
    if (preferences.isKey("tcp_port"))
    {
        config.TcpPort = preferences.getUShort("tcp_port", config.TcpPort);
    }
    if (preferences.isKey("hostname"))
    {
        config.Hostname = preferences.getString("hostname", config.Hostname);
    }
    if (preferences.isKey("obd_mac") && preferences.getBytesLength("obd_mac") == sizeof(config.ObdMac))
    {
        preferences.getBytes("obd_mac", config.ObdMac, sizeof(config.ObdMac));
    }
    preferences.end();
    return config;
}

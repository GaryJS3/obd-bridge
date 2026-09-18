#include "AppConfig.h"

AppConfig AppConfig::Load()
{
    AppConfig config{};
    const uint8_t defaultMac[6] = {0x00, 0x04, 0x3E, 0x5F, 0x5E, 0x9C};
    memcpy(config.ObdMac, defaultMac, sizeof(config.ObdMac));
    config.WifiSsid = WIFI_SSID;
    config.WifiPassword = WIFI_PSK;
    config.WifiProfiles.push_back({config.WifiSsid, config.WifiPassword});
#if defined(WIFI_2_SSID) && defined(WIFI_2_PSK)
    if (strlen(WIFI_2_SSID) > 0) config.WifiProfiles.push_back({WIFI_2_SSID, WIFI_2_PSK});
#endif
#if defined(WIFI_3_SSID) && defined(WIFI_3_PSK)
    if (strlen(WIFI_3_SSID) > 0) config.WifiProfiles.push_back({WIFI_3_SSID, WIFI_3_PSK});
#endif
    config.TcpPort = 35000;
    config.Hostname = "obd-bridge";
    config.CloudUrl = CLOUD_WS_URL;
    config.DeviceId = CLOUD_DEVICE_ID;
    config.DeviceToken = CLOUD_DEVICE_TOKEN;
    config.CloudCaCert = CLOUD_CA_CERT;

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
    if (config.WifiProfiles.size() > 0)
    {
        config.WifiProfiles[0] = {config.WifiSsid, config.WifiPassword};
    }
    for (size_t i = 1; i < 3; ++i)
    {
        const String ssidKey = "wifi" + String(i + 1) + "_ssid";
        const String pskKey = "wifi" + String(i + 1) + "_psk";
        const char *ssidDefault = i == 1 ? WIFI_2_SSID : WIFI_3_SSID;
        const char *pskDefault = i == 1 ? WIFI_2_PSK : WIFI_3_PSK;
        String ssid = preferences.getString(ssidKey.c_str(), ssidDefault);
        String psk = preferences.getString(pskKey.c_str(), pskDefault);
        if (!ssid.isEmpty())
        {
            if (config.WifiProfiles.size() <= i) config.WifiProfiles.push_back({ssid, psk});
            else config.WifiProfiles[i] = {ssid, psk};
        }
    }
    if (preferences.isKey("tcp_port"))
    {
        config.TcpPort = preferences.getUShort("tcp_port", config.TcpPort);
    }
    if (preferences.isKey("hostname"))
    {
        config.Hostname = preferences.getString("hostname", config.Hostname);
    }
    config.CloudUrl = preferences.getString("cloud_url", config.CloudUrl);
    config.DeviceId = preferences.getString("device_id", config.DeviceId);
    config.DeviceToken = preferences.getString("device_token", config.DeviceToken);
    if (preferences.isKey("obd_mac") && preferences.getBytesLength("obd_mac") == sizeof(config.ObdMac))
    {
        preferences.getBytes("obd_mac", config.ObdMac, sizeof(config.ObdMac));
    }
    preferences.end();
    return config;
}

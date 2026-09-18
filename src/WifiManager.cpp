#include "WifiManager.h"

namespace
{
constexpr uint32_t ReconnectIntervalMs = 10000;
}

WifiManager::WifiManager(const AppConfig &config, AppStats &stats)
    : Config(config), Stats(stats)
{
}

void WifiManager::Begin()
{
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    WiFi.setHostname(Config.Hostname.c_str());
    Connect();
}

void WifiManager::Connect()
{
    if (HasAttempted)
    {
        Stats.AddWifiReconnect();
        Serial.println("[WIFI] Reconnecting...");
    }
    else
    {
        Serial.printf("[WIFI] Connecting to %s\n", Config.WifiSsid.c_str());
    }
    HasAttempted = true;
    LastAttemptMs = millis();
    WiFi.begin(Config.WifiSsid.c_str(), Config.WifiPassword.c_str());
}

void WifiManager::Loop()
{
    const wl_status_t status = WiFi.status();
    if (status != LastStatus)
    {
        if (status == WL_CONNECTED)
        {
            Serial.println("[WIFI] Connected");
            Serial.printf("[WIFI] IP: %s\n", WiFi.localIP().toString().c_str());
        }
        else if (LastStatus == WL_CONNECTED)
        {
            Serial.println("[WIFI] Disconnected");
        }
        LastStatus = status;
    }

    if (status != WL_CONNECTED && static_cast<uint32_t>(millis() - LastAttemptMs) >= ReconnectIntervalMs)
    {
        WiFi.disconnect();
        Connect();
    }
}

bool WifiManager::IsConnected() const
{
    return WiFi.status() == WL_CONNECTED;
}

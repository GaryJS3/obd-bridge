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
    WiFi.setAutoReconnect(false);
    WiFi.setHostname(Config.Hostname.c_str());
    Connect();
}

void WifiManager::Connect()
{
    if (HasAttempted)
    {
        Stats.AddWifiReconnect();
        if (!Config.WifiProfiles.empty()) NextProfile = (NextProfile + 1) % Config.WifiProfiles.size();
        Serial.println("[WIFI] Trying known networks...");
    }
    else
    {
        Serial.printf("[WIFI] Connecting to %s\n", Config.WifiSsid.c_str());
    }
    HasAttempted = true;
    LastAttemptMs = millis();
    if (Config.WifiProfiles.empty())
    {
        WiFi.begin(Config.WifiSsid.c_str(), Config.WifiPassword.c_str());
        return;
    }
    const WifiProfile &profile = Config.WifiProfiles[NextProfile % Config.WifiProfiles.size()];
    Serial.printf("[WIFI] Connecting to profile %u of %u: %s\n",
        static_cast<unsigned>(NextProfile + 1), static_cast<unsigned>(Config.WifiProfiles.size()), profile.Ssid.c_str());
    WiFi.begin(profile.Ssid.c_str(), profile.Password.c_str());
}

void WifiManager::Loop()
{
    const wl_status_t status = WiFi.status();
    if (status != LastStatus)
    {
        if (status == WL_CONNECTED)
        {
            Serial.println("[WIFI] Connected");
            if (!Config.WifiProfiles.empty())
            {
                const String currentSsid = WiFi.SSID();
                for (size_t i = 0; i < Config.WifiProfiles.size(); ++i)
                {
                    if (Config.WifiProfiles[i].Ssid == currentSsid)
                    {
                        NextProfile = i;
                        break;
                    }
                }
            }
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
        WiFi.disconnect(false);
        Connect();
    }
}

bool WifiManager::IsConnected() const
{
    return WiFi.status() == WL_CONNECTED;
}

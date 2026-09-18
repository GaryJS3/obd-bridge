#pragma once

#include <Arduino.h>
#include <WiFi.h>
#include "AppConfig.h"
#include "AppStats.h"

class WifiManager
{
public:
    WifiManager(const AppConfig &config, AppStats &stats);
    void Begin();
    void Loop();
    bool IsConnected() const;

private:
    const AppConfig &Config;
    AppStats &Stats;
    wl_status_t LastStatus = WL_NO_SHIELD;
    uint32_t LastAttemptMs = 0;
    bool HasAttempted = false;
    void Connect();
};


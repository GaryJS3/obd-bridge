#include <Arduino.h>
#include "AppConfig.h"
#include "AppStats.h"
#include "BluetoothObd.h"
#include "CloudBridge.h"
#include "StatusServer.h"
#include "TcpBridge.h"
#include "WifiManager.h"

namespace
{
AppConfig Config;
AppStats Stats;
WifiManager *Wifi = nullptr;
BluetoothObd *Bluetooth = nullptr;
TcpBridge *Bridge = nullptr;
CloudBridge *Cloud = nullptr;
StatusServer *Status = nullptr;
}

void setup()
{
    Serial.begin(115200);
    delay(200);
    Serial.println();
    Serial.println("[BOOT] OBD WiFi Bridge starting");

    Config = AppConfig::Load();
    Wifi = new WifiManager(Config, Stats);
    Bluetooth = new BluetoothObd(Config, Stats);
    Bridge = new TcpBridge(Config.TcpPort, *Bluetooth, Stats);
    Cloud = new CloudBridge(Config, Stats, *Bluetooth, *Bridge);
    Bridge->SetRemoteSink(Cloud);
    Status = new StatusServer(Config, Stats, *Bluetooth, *Bridge);

    Wifi->Begin();
    Bridge->Begin();
    Status->Begin();
    if (!Bluetooth->Begin())
    {
        Serial.println("[BT] Bluetooth task failed to start");
    }
    Cloud->Begin();
}

void loop()
{
    Wifi->Loop();
    Cloud->Loop();
    Bridge->Loop();
    Status->Loop();
    delay(1);
}

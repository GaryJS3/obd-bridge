#pragma once

#include <Arduino.h>
#include <BluetoothSerial.h>
#include "AppConfig.h"
#include "AppStats.h"

class BluetoothObd
{
public:
    BluetoothObd(const AppConfig &config, AppStats &stats);
    bool Begin();
    bool IsConnected() const;
    int Available();
    size_t Read(uint8_t *buffer, size_t length);
    size_t Write(const uint8_t *buffer, size_t length);
    void RequestReconnect();

private:
    const AppConfig &Config;
    AppStats &Stats;
    BluetoothSerial SerialBt;
    TaskHandle_t TaskHandle = nullptr;
    volatile bool Connected = false;
    volatile bool ReconnectRequested = false;
    static void TaskEntry(void *argument);
    void TaskLoop();
    void SetConnected(bool connected);
};


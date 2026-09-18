#pragma once

#include <Arduino.h>
#include <WebServer.h>
#include "AppConfig.h"
#include "AppStats.h"
#include "BluetoothObd.h"
#include "TcpBridge.h"

class StatusServer
{
public:
    StatusServer(const AppConfig &config, AppStats &stats, BluetoothObd &bluetooth, TcpBridge &tcpBridge);
    void Begin();
    void Loop();

private:
    const AppConfig &Config;
    AppStats &Stats;
    BluetoothObd &Bluetooth;
    TcpBridge &Bridge;
    WebServer Server{80};
    bool OtaUploadAuthorized = false;
    bool OtaUploadSucceeded = false;
    String OtaError;
    void SendStatus();
    void FinishFirmwareUpload();
    void HandleFirmwareUpload();
    static String JsonEscape(const String &value);
};

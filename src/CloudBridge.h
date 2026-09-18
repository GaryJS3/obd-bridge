#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include <WebSocketsClient.h>
#include <mbedtls/sha256.h>
#include "AppConfig.h"
#include "AppStats.h"
#include "BluetoothObd.h"
#include "RemoteObdSink.h"
#include "TcpBridge.h"
#include "WifiManager.h"

class CloudBridge : public RemoteObdSink
{
public:
    CloudBridge(const AppConfig &config, AppStats &stats, BluetoothObd &bluetooth, TcpBridge &bridge);
    void Begin();
    void Loop();
    bool IsConnected() const { return CloudOnline; }
    bool QueueObdOutput(const uint8_t *data, size_t length) override;

private:
    const AppConfig &Config;
    AppStats &Stats;
    BluetoothObd &Bluetooth;
    TcpBridge &Bridge;
    WebSocketsClient Client;
    ByteRing<8192> BluetoothToCloud;
    String Host;
    String Path;
    uint16_t Port = 443;
    bool UrlValid = false;
    bool Configured = false;
    bool Started = false;
    bool TimeSyncRequested = false;
    bool CloudOnline = false;
    bool OtaInProgress = false;
    bool OtaHashStarted = false;
    uint32_t LastStatusMs = 0;
    uint32_t LastLogMs = 0;
    uint32_t RestartAtMs = 0;
    uint32_t StatusSequence = 0;
    uint64_t CloudTxBytes = 0;
    uint64_t CloudRxBytes = 0;
    String TransferId;
    uint32_t OtaExpectedBytes = 0;
    uint32_t OtaReceivedBytes = 0;
    uint8_t ExpectedSha256[32]{};
    mbedtls_sha256_context Sha256{};

    void OnEvent(WStype_t type, uint8_t *payload, size_t length);
    void ParseUrl();
    bool StartSocket();
    void SendHello();
    void SendStatus();
    void SendResult(const String &requestId, const String &command, bool success, const String &error = "");
    void SendJson(JsonDocument &document);
    void HandleText(uint8_t *payload, size_t length);
    void HandleBinary(uint8_t *payload, size_t length);
    void BeginOta(JsonDocument &document);
    void ReceiveOtaChunk(uint8_t *payload, size_t length);
    void CommitOta(JsonDocument &document);
    void AbortOta(const char *reason, bool notifyServer);
};

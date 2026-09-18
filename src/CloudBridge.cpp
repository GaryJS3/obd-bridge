#include "CloudBridge.h"

#include <WiFi.h>
#include <Update.h>
#include <time.h>
#include <esp_ota_ops.h>
#include <mbedtls/sha256.h>
#include "Diagnostics.h"

namespace
{
constexpr uint32_t StatusIntervalMs = 10000;
constexpr uint32_t OtaAckIntervalBytes = 65536;
constexpr uint32_t RebootDelayMs = 1200;
constexpr size_t ObdChunkSize = 512;

int HexDigit(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
}

CloudBridge::CloudBridge(const AppConfig &config, AppStats &stats, BluetoothObd &bluetooth, TcpBridge &bridge)
    : Config(config), Stats(stats), Bluetooth(bluetooth), Bridge(bridge)
{
}

void CloudBridge::Begin()
{
    ParseUrl();
    if (!UrlValid)
    {
        Serial.println("[CLOUD] Cloud URL must use wss:// and include a valid host");
        return;
    }
    if (!Config.CloudCaCert.startsWith("-----BEGIN CERTIFICATE-----"))
    {
        Serial.println("[CLOUD] TLS CA certificate is missing; cloud connection remains disabled");
        return;
    }
    if (Config.DeviceToken.length() < 32 || Config.DeviceToken.startsWith("replace-with"))
    {
        Serial.println("[CLOUD] Device token is not configured; cloud connection remains disabled");
        return;
    }
    Configured = true;

    Client.setReconnectInterval(5000);
    Client.enableHeartbeat(20000, 5000, 2);
    Client.onEvent([this](WStype_t type, uint8_t *payload, size_t length)
    {
        OnEvent(type, payload, length);
    });
    Serial.printf("[CLOUD] Ready for secure connection to %s:%u%s\n", Host.c_str(), Port, Path.c_str());
}

void CloudBridge::ParseUrl()
{
    String url = Config.CloudUrl;
    if (!url.startsWith("wss://")) return;
    url.remove(0, 6);
    const int pathStart = url.indexOf('/');
    String authority = pathStart < 0 ? url : url.substring(0, pathStart);
    Path = pathStart < 0 ? "/" : url.substring(pathStart);
    if (authority.isEmpty() || Path.indexOf('#') >= 0 || Path.indexOf(' ') >= 0) return;
    const int colon = authority.lastIndexOf(':');
    if (colon >= 0)
    {
        const long parsedPort = authority.substring(colon + 1).toInt();
        if (parsedPort < 1 || parsedPort > 65535) return;
        Port = static_cast<uint16_t>(parsedPort);
        authority.remove(colon);
    }
    if (authority.indexOf('@') >= 0 || authority.indexOf('?') >= 0 || authority.indexOf('#') >= 0) return;
    Host = authority;
    UrlValid = true;
}

void CloudBridge::Loop()
{
    const uint32_t now = millis();
    if (RestartAtMs != 0 && static_cast<int32_t>(now - RestartAtMs) >= 0)
    {
        Serial.println("[OTA] Restarting into the verified firmware slot");
        ESP.restart();
    }
    if (!Configured) return;
    if (WiFi.status() != WL_CONNECTED)
    {
        if (Started)
        {
            Client.disconnect();
            Started = false;
            CloudOnline = false;
            BluetoothToCloud.Clear();
            Bridge.SetRemoteControl(false);
            AbortOta("wifi_disconnected", false);
        }
        return;
    }

    if (!TimeSyncRequested)
    {
        configTime(0, 0, "pool.ntp.org", "time.google.com");
        TimeSyncRequested = true;
        Serial.println("[CLOUD] Waiting for clock synchronization before TLS validation");
    }
    if (!Started)
    {
        if (time(nullptr) < 1700000000) return;
        Started = StartSocket();
        if (!Started) return;
    }

    Client.loop();
    if (CloudOnline && static_cast<uint32_t>(now - LastStatusMs) >= StatusIntervalMs)
    {
        LastStatusMs = now;
        SendStatus();
    }
    if (CloudOnline && !OtaInProgress && BluetoothToCloud.Size() > 0)
    {
        size_t contiguous = 0;
        const uint8_t *data = BluetoothToCloud.Peek(contiguous);
        const size_t count = min(contiguous, ObdChunkSize);
        uint8_t frame[ObdChunkSize + 1];
        frame[0] = 2;
        memcpy(frame + 1, data, count);
        if (Client.sendBIN(frame, count + 1))
        {
            BluetoothToCloud.Consume(count);
            CloudTxBytes += count;
            Stats.AddBtToTcp(count);
        }
    }
}

bool CloudBridge::StartSocket()
{
    String headers;
    headers.reserve(Config.DeviceToken.length() + Config.DeviceId.length() + 96);
    headers = "Authorization: Bearer ";
    headers += Config.DeviceToken;
    headers += "\r\nX-Device-Id: ";
    headers += Config.DeviceId;
    headers += "\r\nX-Protocol-Version: 1\r\n";
    Client.setExtraHeaders(headers.c_str());
    Client.beginSslWithCA(Host.c_str(), Port, Path.c_str(), Config.CloudCaCert.c_str());
    Serial.printf("[CLOUD] Connecting to %s:%u%s\n", Host.c_str(), Port, Path.c_str());
    return true;
}

void CloudBridge::OnEvent(WStype_t type, uint8_t *payload, size_t length)
{
    switch (type)
    {
    case WStype_CONNECTED:
        CloudOnline = true;
        LastStatusMs = millis();
        Serial.println("[CLOUD] Authenticated WebSocket connected");
        SendHello();
        SendStatus();
        break;
    case WStype_DISCONNECTED:
        CloudOnline = false;
        BluetoothToCloud.Clear();
        Bridge.SetRemoteControl(false);
        AbortOta("cloud_disconnected", false);
        Serial.println("[CLOUD] WebSocket disconnected; reconnect is automatic");
        break;
    case WStype_TEXT:
        HandleText(payload, length);
        break;
    case WStype_BIN:
        HandleBinary(payload, length);
        break;
    case WStype_ERROR:
        if (static_cast<uint32_t>(millis() - LastLogMs) > 5000)
        {
            Serial.printf("[CLOUD] WebSocket transport error (%u bytes)\n", static_cast<unsigned>(length));
            LastLogMs = millis();
        }
        break;
    default:
        break;
    }
}

bool CloudBridge::QueueObdOutput(const uint8_t *data, size_t length)
{
    if (!CloudOnline || OtaInProgress || length == 0 || BluetoothToCloud.Free() < length)
        return false;
    return BluetoothToCloud.Push(data, length) == length;
}

void CloudBridge::SendHello()
{
    StaticJsonDocument<512> document;
    document["version"] = 1;
    document["type"] = "hello";
    document["deviceId"] = Config.DeviceId;
    document["firmwareVersion"] = "1.0.0";
    document["buildId"] = String(__DATE__) + " " + __TIME__;
    document["runningSlot"] = esp_ota_get_running_partition()->label;
    JsonArray capabilities = document.createNestedArray("capabilities");
    capabilities.add("obd");
    capabilities.add("status");
    capabilities.add("diagnostics");
    capabilities.add("ota");
    SendJson(document);
}

void CloudBridge::SendStatus()
{
    if (!CloudOnline) return;
    StaticJsonDocument<1536> document;
    document["version"] = 1;
    document["type"] = "status";
    document["sequence"] = ++StatusSequence;
    document["uptimeMs"] = millis();

    JsonObject wifi = document.createNestedObject("wifi");
    const bool wifiConnected = WiFi.status() == WL_CONNECTED;
    wifi["connected"] = wifiConnected;
    wifi["ssid"] = wifiConnected ? WiFi.SSID() : "";
    wifi["ip"] = wifiConnected ? WiFi.localIP().toString() : "";
    wifi["rssi"] = wifiConnected ? WiFi.RSSI() : 0;

    JsonObject bluetooth = document.createNestedObject("bluetooth");
    bluetooth["connected"] = Bluetooth.IsConnected();
    bluetooth["targetName"] = "OBDLink MX+ 80685";

    JsonObject bridge = document.createNestedObject("bridge");
    bridge["owner"] = OtaInProgress ? "ota" : Bridge.RemoteControlActive() ? "cloud" : Bridge.HasClient() ? "local" : "none";
    bridge["localTcpConnected"] = Bridge.HasClient();
    bridge["port"] = Bridge.PortNumber();
    bridge["pendingToObd"] = Bridge.PendingToBluetooth();
    bridge["pendingFromObd"] = BluetoothToCloud.Size();

    const StatsSnapshot stats = Stats.Snapshot();
    JsonObject counters = document.createNestedObject("stats");
    counters["cloudTxBytes"] = CloudTxBytes;
    counters["cloudRxBytes"] = CloudRxBytes;
    counters["tcpToBtDroppedBytes"] = stats.TcpToBtDroppedBytes;
    counters["btToTcpDroppedBytes"] = stats.BtToTcpDroppedBytes;
    counters["bluetoothReconnectAttempts"] = stats.BluetoothReconnectAttempts;
    counters["bluetoothDisconnects"] = stats.BluetoothDisconnects;
    counters["wifiReconnects"] = stats.WifiReconnects;

    JsonObject diagnostics = document.createNestedObject("diagnostics");
    diagnostics["build"] = String(__DATE__) + " " + __TIME__;
    diagnostics["running_slot"] = esp_ota_get_running_partition()->label;
    diagnostics["pending_to_bt"] = Bridge.PendingToBluetooth();
    diagnostics["pending_to_tcp"] = BluetoothToCloud.Size();
    SendJson(document);
}

void CloudBridge::SendResult(const String &requestId, const String &command, bool success, const String &error)
{
    StaticJsonDocument<384> document;
    document["version"] = 1;
    document["type"] = "result";
    document["requestId"] = requestId;
    document["command"] = command;
    document["success"] = success;
    if (!error.isEmpty()) document["error"] = error;
    SendJson(document);
}

void CloudBridge::SendJson(JsonDocument &document)
{
    if (!CloudOnline) return;
    String output;
    output.reserve(measureJson(document) + 1);
    serializeJson(document, output);
    Client.sendTXT(reinterpret_cast<const uint8_t *>(output.c_str()), output.length());
}

void CloudBridge::HandleText(uint8_t *payload, size_t length)
{
    StaticJsonDocument<1536> document;
    if (deserializeJson(document, payload, length))
    {
        Serial.println("[CLOUD] Ignored malformed protocol message");
        return;
    }
    const char *type = document["type"] | "";
    if (strcmp(type, "command") == 0)
    {
        const String requestId = document["requestId"] | "";
        const String command = document["command"] | "";
        JsonObjectConst parameters = document["parameters"].as<JsonObjectConst>();
        if (command == "control")
        {
            const bool enabled = parameters["enabled"] | false;
            const bool success = Bridge.SetRemoteControl(enabled);
            SendResult(requestId, command, success, success ? "" : "local_tcp_in_use_or_ota_active");
        }
        else if (command == "bluetooth.reconnect")
        {
            Bluetooth.RequestReconnect();
            SendResult(requestId, command, true);
        }
        else if (command == "status.get")
        {
            SendResult(requestId, command, true);
            SendStatus();
        }
        else if (command == "diagnostics.get")
        {
            SendResult(requestId, command, true);
            DynamicJsonDocument response(4096);
            response["version"] = 1;
            response["type"] = "diagnostics";
            const String logs = Diagnostics::Json();
            DynamicJsonDocument parsedLogs(3072);
            if (deserializeJson(parsedLogs, logs)) response["data"]["error"] = "diagnostics_unavailable";
            else response["data"] = parsedLogs.as<JsonVariantConst>();
            SendJson(response);
        }
        else if (command == "device.reboot")
        {
            SendResult(requestId, command, true);
            RestartAtMs = millis() + RebootDelayMs;
        }
        else
        {
            SendResult(requestId, command, false, "unknown_command");
        }
    }
    else if (strcmp(type, "ota.begin") == 0)
    {
        BeginOta(document);
    }
    else if (strcmp(type, "ota.commit") == 0)
    {
        CommitOta(document);
    }
    else if (strcmp(type, "ota.abort") == 0)
    {
        AbortOta("server_abort", true);
    }
}

void CloudBridge::HandleBinary(uint8_t *payload, size_t length)
{
    if (length < 2) return;
    if (payload[0] == 1)
    {
        if (!Bridge.EnqueueRemoteCommand(payload + 1, length - 1))
            Serial.println("[CLOUD] Remote OBD command rejected by owner or bounded queue");
        else
            CloudRxBytes += length - 1;
    }
    else if (payload[0] == 3)
    {
        ReceiveOtaChunk(payload, length);
    }
}

void CloudBridge::BeginOta(JsonDocument &document)
{
    const String transferId = document["transferId"] | "";
    const uint32_t totalBytes = document["totalBytes"] | 0U;
    const String digest = document["sha256"] | "";
    bool valid = CloudOnline && Bridge.RemoteControlActive() && !OtaInProgress && totalBytes > 0 &&
        totalBytes <= ESP.getFreeSketchSpace() && digest.length() == 64 && transferId.length() > 0;
    for (size_t i = 0; valid && i < sizeof(ExpectedSha256); ++i)
    {
        const int high = HexDigit(digest[i * 2]);
        const int low = HexDigit(digest[i * 2 + 1]);
        if (high < 0 || low < 0) valid = false;
        else ExpectedSha256[i] = static_cast<uint8_t>((high << 4) | low);
    }

    if (valid && Update.begin(totalBytes, U_FLASH))
    {
        OtaInProgress = true;
        OtaExpectedBytes = totalBytes;
        OtaReceivedBytes = 0;
        TransferId = transferId;
        Bridge.SetOtaActive(true);
        mbedtls_sha256_init(&Sha256);
        mbedtls_sha256_starts(&Sha256, 0);
        OtaHashStarted = true;
        Serial.printf("[OTA] Remote image accepted for inactive slot: %u bytes\n", static_cast<unsigned>(totalBytes));
    }
    else
    {
        valid = false;
    }

    StaticJsonDocument<384> response;
    response["version"] = 1;
    response["type"] = "otaReady";
    response["transferId"] = transferId;
    response["success"] = valid;
    if (!valid) response["error"] = Update.hasError() ? Update.errorString() : "ota_preconditions_failed";
    SendJson(response);
}

void CloudBridge::ReceiveOtaChunk(uint8_t *payload, size_t length)
{
    if (!OtaInProgress || length <= 5)
    {
        AbortOta("no_active_transfer", true);
        return;
    }
    const uint32_t offset = (static_cast<uint32_t>(payload[1]) << 24) |
        (static_cast<uint32_t>(payload[2]) << 16) |
        (static_cast<uint32_t>(payload[3]) << 8) | payload[4];
    const size_t chunkLength = length - 5;
    if (offset != OtaReceivedBytes || OtaReceivedBytes + chunkLength > OtaExpectedBytes)
    {
        AbortOta("invalid_offset_or_length", true);
        return;
    }
    if (Update.write(payload + 5, chunkLength) != chunkLength)
    {
        AbortOta("firmware_write_failed", true);
        return;
    }
    mbedtls_sha256_update(&Sha256, payload + 5, chunkLength);
    OtaReceivedBytes += chunkLength;
    if ((OtaReceivedBytes % OtaAckIntervalBytes) == 0 || OtaReceivedBytes == OtaExpectedBytes)
    {
        StaticJsonDocument<256> response;
        response["version"] = 1;
        response["type"] = "otaAck";
        response["transferId"] = TransferId;
        response["offset"] = OtaReceivedBytes;
        SendJson(response);
    }
}

void CloudBridge::CommitOta(JsonDocument &document)
{
    const String transferId = document["transferId"] | "";
    if (!OtaInProgress || transferId != TransferId || OtaReceivedBytes != OtaExpectedBytes)
    {
        AbortOta("transfer_incomplete", true);
        return;
    }
    uint8_t actualSha[32];
    mbedtls_sha256_finish(&Sha256, actualSha);
    uint8_t difference = 0;
    for (size_t i = 0; i < sizeof(actualSha); ++i) difference |= actualSha[i] ^ ExpectedSha256[i];
    mbedtls_sha256_free(&Sha256);
    OtaHashStarted = false;
    if (difference != 0)
    {
        AbortOta("sha256_mismatch", true);
        return;
    }
    if (!Update.end(true))
    {
        AbortOta(Update.errorString(), true);
        return;
    }

    Serial.printf("[OTA] Remote image verified: %u bytes; reboot scheduled\n", static_cast<unsigned>(OtaExpectedBytes));
    StaticJsonDocument<384> response;
    response["version"] = 1;
    response["type"] = "otaResult";
    response["transferId"] = TransferId;
    response["success"] = true;
    response["bytes"] = OtaReceivedBytes;
    SendJson(response);
    OtaInProgress = false;
    RestartAtMs = millis() + RebootDelayMs;
}

void CloudBridge::AbortOta(const char *reason, bool notifyServer)
{
    if (!OtaInProgress)
    {
        if (notifyServer && CloudOnline)
        {
            StaticJsonDocument<256> response;
            response["version"] = 1;
            response["type"] = "otaResult";
            response["transferId"] = TransferId;
            response["success"] = false;
            response["error"] = reason;
            SendJson(response);
        }
        return;
    }

    Update.abort();
    if (OtaHashStarted) mbedtls_sha256_free(&Sha256);
    OtaHashStarted = false;
    OtaInProgress = false;
    Bridge.SetOtaActive(false);
    Serial.printf("[OTA] Remote update aborted: %s\n", reason);
    if (notifyServer && CloudOnline)
    {
        StaticJsonDocument<256> response;
        response["version"] = 1;
        response["type"] = "otaResult";
        response["transferId"] = TransferId;
        response["success"] = false;
        response["error"] = reason;
        SendJson(response);
    }
    TransferId = "";
    OtaExpectedBytes = 0;
    OtaReceivedBytes = 0;
}

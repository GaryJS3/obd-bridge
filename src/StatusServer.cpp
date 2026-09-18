#include "StatusServer.h"
#include <WiFi.h>
#include <esp_system.h>
#include <Update.h>
#include "Diagnostics.h"
#include <esp_ota_ops.h>

StatusServer::StatusServer(const AppConfig &config, AppStats &stats, BluetoothObd &bluetooth, TcpBridge &tcpBridge)
    : Config(config), Stats(stats), Bluetooth(bluetooth), Bridge(tcpBridge)
{
}

void StatusServer::Begin()
{
    const char *headers[] = {"Content-Type"};
    Server.collectHeaders(headers, 1);
    Server.on("/api/status", HTTP_GET, [this]() { SendStatus(); });
    Server.on("/api/logs", HTTP_GET, [this]() { Server.send(200, "application/json", Diagnostics::Json()); });
    Server.on("/api/bluetooth/reconnect", HTTP_POST, [this]()
    {
        Bluetooth.RequestReconnect();
        Server.send(202, "application/json", "{\"accepted\":true}");
    });
    Server.on("/api/reboot", HTTP_POST, [this]()
    {
        Server.send(202, "application/json", "{\"accepted\":true}");
        delay(100);
        ESP.restart();
    });
    Server.on("/api/firmware", HTTP_POST,
        [this]() { FinishFirmwareUpload(); },
        [this]() { HandleFirmwareUpload(); });
    Server.onNotFound([this]() { Server.send(404, "application/json", "{\"error\":\"not_found\"}"); });
    Server.begin();
    Serial.println("[HTTP] Status API listening on port 80");
}

void StatusServer::HandleFirmwareUpload()
{
    if (!Server.authenticate("admin", OTA_UPDATE_PASSWORD))
    {
        OtaUploadAuthorized = false;
        return;
    }

    OtaUploadAuthorized = true;
    if (!Server.header("Content-Type").startsWith("multipart/form-data"))
    {
        OtaUploadSucceeded = false;
        OtaError = "multipart_form_required";
        return;
    }
    HTTPUpload &upload = Server.upload();
    if (upload.status == UPLOAD_FILE_START)
    {
        OtaUploadSucceeded = false;
        OtaError = "";
        Serial.printf("[OTA] Firmware upload started: %s\n", upload.filename.c_str());
        if (!Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH))
        {
            OtaError = Update.errorString();
        }
    }
    else if (upload.status == UPLOAD_FILE_WRITE && OtaUploadAuthorized && OtaError.isEmpty())
    {
        if (Update.write(upload.buf, upload.currentSize) != upload.currentSize)
        {
            OtaError = Update.errorString();
        }
    }
    else if (upload.status == UPLOAD_FILE_END && OtaUploadAuthorized && OtaError.isEmpty())
    {
        if (Update.end(true))
        {
            OtaUploadSucceeded = true;
            Serial.printf("[OTA] Firmware verified: %u bytes\n", static_cast<unsigned>(upload.totalSize));
        }
        else
        {
            OtaError = Update.errorString();
        }
    }
    else if (upload.status == UPLOAD_FILE_ABORTED && OtaUploadAuthorized)
    {
        Update.abort();
        OtaError = "upload_aborted";
    }
}

void StatusServer::FinishFirmwareUpload()
{
    if (!Server.authenticate("admin", OTA_UPDATE_PASSWORD))
    {
        Server.requestAuthentication(BASIC_AUTH, "OBD WiFi Bridge OTA");
        return;
    }
    if (!Server.header("Content-Type").startsWith("multipart/form-data"))
    {
        Server.send(400, "application/json", "{\"error\":\"multipart_form_required\"}");
        return;
    }
    if (!OtaUploadSucceeded)
    {
        const String error = OtaError.isEmpty() ? "update_failed" : JsonEscape(OtaError);
        Serial.printf("[OTA] Firmware update failed: %s\n", error.c_str());
        Server.send(400, "application/json", "{\"updated\":false,\"error\":\"" + error + "\"}");
        return;
    }

    Server.send(200, "application/json", "{\"updated\":true,\"rebooting\":true}");
    delay(250);
    ESP.restart();
}

void StatusServer::Loop()
{
    Server.handleClient();
}

String StatusServer::JsonEscape(const String &value)
{
    String output;
    output.reserve(value.length() + 8);
    for (size_t i = 0; i < value.length(); ++i)
    {
        const char c = value[i];
        if (c == '"' || c == '\\')
        {
            output += '\\';
        }
        if (static_cast<uint8_t>(c) >= 0x20)
        {
            output += c;
        }
    }
    return output;
}

void StatusServer::SendStatus()
{
    const StatsSnapshot stats = Stats.Snapshot();
    const bool wifiConnected = WiFi.status() == WL_CONNECTED;
    char mac[18];
    snprintf(mac, sizeof(mac), "%02X:%02X:%02X:%02X:%02X:%02X",
        Config.ObdMac[0], Config.ObdMac[1], Config.ObdMac[2],
        Config.ObdMac[3], Config.ObdMac[4], Config.ObdMac[5]);

    String json;
    json.reserve(768);
    json += "{\"device\":\"OBD-WiFi-Bridge\",\"uptime_ms\":" + String(millis());
    json += ",\"wifi\":{\"connected\":" + String(wifiConnected ? "true" : "false");
    json += ",\"ssid\":\"" + JsonEscape(Config.WifiSsid) + "\"";
    json += ",\"ip\":\"" + WiFi.localIP().toString() + "\",\"rssi\":" + String(wifiConnected ? WiFi.RSSI() : 0) + "}";
    json += ",\"bluetooth\":{\"connected\":" + String(Bluetooth.IsConnected() ? "true" : "false");
    json += ",\"target_name\":\"OBDLink MX+ 80685\",\"target_mac\":\"" + String(mac) + "\"}";
    json += ",\"tcp\":{\"port\":" + String(Config.TcpPort) + ",\"client_connected\":" + String(Bridge.HasClient() ? "true" : "false") + "}";
    const String buildId = String(__DATE__) + " " + __TIME__;
    json += ",\"diagnostics\":{\"build\":\"" + JsonEscape(buildId) + "\",\"running_slot\":\"" + String(esp_ota_get_running_partition()->label) + "\",\"pending_to_bt\":" + String(Bridge.PendingToBluetooth()) + ",\"pending_to_tcp\":" + String(Bridge.PendingToTcp()) + "}";
    json += ",\"stats\":{";
    json += "\"tcp_to_bt_bytes\":" + String(stats.TcpToBtBytes);
    json += ",\"bt_to_tcp_bytes\":" + String(stats.BtToTcpBytes);
    json += ",\"tcp_connections\":" + String(stats.TcpConnections);
    json += ",\"bluetooth_reconnect_attempts\":" + String(stats.BluetoothReconnectAttempts);
    json += ",\"bluetooth_disconnects\":" + String(stats.BluetoothDisconnects);
    json += ",\"wifi_reconnects\":" + String(stats.WifiReconnects);
    json += ",\"tcp_to_bt_dropped_bytes\":" + String(stats.TcpToBtDroppedBytes);
    json += ",\"bt_to_tcp_dropped_bytes\":" + String(stats.BtToTcpDroppedBytes) + "}}";
    Server.send(200, "application/json", json);
}

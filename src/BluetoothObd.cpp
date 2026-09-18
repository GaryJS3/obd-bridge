#include "BluetoothObd.h"
#include "Diagnostics.h"

namespace
{
constexpr uint32_t ReconnectDelayMs = 5000;
}

BluetoothObd::BluetoothObd(const AppConfig &config, AppStats &stats)
    : Config(config), Stats(stats)
{
}

bool BluetoothObd::Begin()
{
    Serial.println("[BT] Initializing Bluetooth Classic SPP master");
    Diagnostics::Event("bt_initializing");
    SerialBt.register_callback([](esp_spp_cb_event_t event, esp_spp_cb_param_t *p)
    {
        switch (event)
        {
        case ESP_SPP_DISCOVERY_COMP_EVT:
            Diagnostics::Event("spp_discovery", p->disc_comp.status, p->disc_comp.scn_num);
            break;
        case ESP_SPP_OPEN_EVT:
            Diagnostics::Event("spp_open", p->open.status, p->open.handle);
            break;
        case ESP_SPP_CLOSE_EVT:
            Diagnostics::Event("spp_close", p->close.status, p->close.port_status);
            break;
        case ESP_SPP_WRITE_EVT:
            if (p->write.status == ESP_SPP_SUCCESS) Diagnostics::Written(p->write.len);
            else Diagnostics::Event("spp_write_failed", p->write.status, p->write.len);
            break;
        case ESP_SPP_DATA_IND_EVT:
            Diagnostics::Receive(p->data_ind.len);
            break;
        case ESP_SPP_CONG_EVT:
            Diagnostics::Event("spp_congestion", 0, p->cong.cong);
            break;
        default: break;
        }
    });
    SerialBt.enableSSP();
    SerialBt.onConfirmRequest([this](uint32_t numericValue)
    {
        Serial.printf("[BT] Secure Simple Pairing confirmation: %06lu (accepting)\n",
            static_cast<unsigned long>(numericValue));
        SerialBt.confirmReply(true);
    });
    SerialBt.onAuthComplete([](bool success)
    {
        Diagnostics::Event("bt_auth", success ? 0 : -1);
        Serial.printf("[BT] Authentication %s\n", success ? "succeeded" : "failed");
    });
    if (!SerialBt.begin("OBD-WiFi-Bridge", true))
    {
        Serial.println("[BT] Initialization failed");
        return false;
    }

    return xTaskCreatePinnedToCore(TaskEntry, "bt-reconnect", 4096, this, 1, &TaskHandle, 0) == pdPASS;
}

void BluetoothObd::TaskEntry(void *argument)
{
    static_cast<BluetoothObd *>(argument)->TaskLoop();
}

void BluetoothObd::TaskLoop()
{
    for (;;)
    {
        if (ReconnectRequested)
        {
            ReconnectRequested = false;
            if (SerialBt.connected())
            {
                Serial.println("[BT] Reconnect requested; disconnecting current session");
                SerialBt.disconnect();
            }
            SetConnected(false);
        }

        const bool actualConnected = SerialBt.connected(0);
        if (actualConnected)
        {
            SetConnected(true);
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        SetConnected(false);
        Stats.AddBluetoothReconnectAttempt();
        Serial.printf("[BT] Connecting to %02X:%02X:%02X:%02X:%02X:%02X\n",
            Config.ObdMac[0], Config.ObdMac[1], Config.ObdMac[2],
            Config.ObdMac[3], Config.ObdMac[4], Config.ObdMac[5]);

        uint8_t address[6];
        memcpy(address, Config.ObdMac, sizeof(address));
        if (SerialBt.connect(address))
        {
            SetConnected(true);
        }
        else
        {
            Serial.println("[BT] Connection failed; will retry");
        }
        vTaskDelay(pdMS_TO_TICKS(ReconnectDelayMs));
    }
}

void BluetoothObd::SetConnected(bool connected)
{
    if (Connected == connected)
    {
        return;
    }
    Connected = connected;
    if (connected)
    {
        Serial.println("[BT] Connected: OBDLink MX+ 80685");
    }
    else
    {
        Stats.AddBluetoothDisconnect();
        Serial.println("[BT] Disconnected");
    }
}

bool BluetoothObd::IsConnected() const { return Connected; }
int BluetoothObd::Available() { return SerialBt.available(); }
size_t BluetoothObd::Read(uint8_t *buffer, size_t length) { return SerialBt.readBytes(buffer, length); }
size_t BluetoothObd::Write(const uint8_t *buffer, size_t length) { return SerialBt.write(buffer, length); }
void BluetoothObd::RequestReconnect() { ReconnectRequested = true; }

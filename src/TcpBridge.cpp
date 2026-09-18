#include "TcpBridge.h"
#include "Diagnostics.h"
#include <lwip/sockets.h>
#include <errno.h>

namespace
{
constexpr size_t ChunkSize = 512;
constexpr uint32_t DropLogIntervalMs = 1000;
}

TcpBridge::TcpBridge(uint16_t port, BluetoothObd &bluetooth, AppStats &stats)
    : Port(port), Bluetooth(bluetooth), Stats(stats), Server(port)
{
}

void TcpBridge::Begin()
{
    Server.begin();
    Server.setNoDelay(true);
    Serial.printf("[TCP] Listening on %u\n", Port);
}

void TcpBridge::Loop()
{
    AcceptClient();
    if (Client && !Client.connected())
    {
        Serial.println("[TCP] Client disconnected");
        Client.stop();
        TcpToBt.Clear();
        BtToTcp.Clear();
    }

    ReadTcp();
    ReadBluetooth();
    FlushBluetooth();
    FlushTcp();
    LogDrops();
}

void TcpBridge::AcceptClient()
{
    WiFiClient incoming = Server.available();
    if (!incoming)
    {
        return;
    }
    if ((Client && Client.connected()) || RemoteControl || OtaActive)
    {
        Serial.printf("[TCP] Rejected second client: %s\n", incoming.remoteIP().toString().c_str());
        incoming.stop();
        return;
    }
    Client = incoming;
    Client.setNoDelay(true);
    Stats.AddTcpConnection();
    Serial.printf("[TCP] Client connected: %s\n", Client.remoteIP().toString().c_str());
}

void TcpBridge::ReadTcp()
{
    if (RemoteControl || OtaActive || !Client || !Client.connected())
    {
        return;
    }
    uint8_t buffer[ChunkSize];
    const size_t wanted = min(static_cast<size_t>(Client.available()), sizeof(buffer));
    if (wanted == 0)
    {
        return;
    }
    const int read = Client.read(buffer, wanted);
    if (read <= 0)
    {
        return;
    }
#if TRACE_BRIDGE_DATA
    Serial.printf("[TRACE] TCP -> BT: %d bytes\n", read);
#endif
    const size_t accepted = TcpToBt.Push(buffer, static_cast<size_t>(read));
    if (accepted < static_cast<size_t>(read))
    {
        const size_t dropped = static_cast<size_t>(read) - accepted;
        Stats.AddTcpToBtDropped(dropped);
        PendingTcpDropLog += dropped;
    }
}

void TcpBridge::ReadBluetooth()
{
    const int available = Bluetooth.Available();
    if (available <= 0)
    {
        return;
    }
    uint8_t buffer[ChunkSize];
    const size_t wanted = min(static_cast<size_t>(available), sizeof(buffer));
    const size_t read = Bluetooth.Read(buffer, wanted);
    if (read == 0)
    {
        return;
    }
#if TRACE_BRIDGE_DATA
    Serial.printf("[TRACE] BT -> TCP: %u bytes\n", static_cast<unsigned>(read));
#endif
    if (OtaActive)
    {
        Stats.AddBtToTcpDropped(read);
        PendingNoClientDropLog += read;
        return;
    }
    if (RemoteControl)
    {
        if (!RemoteSink || !RemoteSink->QueueObdOutput(buffer, read))
        {
            Stats.AddBtToTcpDropped(read);
            PendingNoClientDropLog += read;
        }
        return;
    }
    if (!Client || !Client.connected())
    {
        Stats.AddBtToTcpDropped(read);
        PendingNoClientDropLog += read;
        return;
    }
    const size_t accepted = BtToTcp.Push(buffer, read);
    if (accepted < read)
    {
        const size_t dropped = read - accepted;
        Stats.AddBtToTcpDropped(dropped);
        PendingNoClientDropLog += dropped;
    }
}

void TcpBridge::FlushBluetooth()
{
    if (OtaActive || !Bluetooth.IsConnected() || (RemoteControl ? RemoteToBt.Size() == 0 : TcpToBt.Size() == 0))
    {
        return;
    }
    size_t contiguous = 0;
    const uint8_t *data = RemoteControl ? RemoteToBt.Peek(contiguous) : TcpToBt.Peek(contiguous);
    const size_t written = Bluetooth.Write(data, min(contiguous, ChunkSize));
    if (RemoteControl) RemoteToBt.Consume(written);
    else TcpToBt.Consume(written);
    Stats.AddTcpToBt(written);
}

void TcpBridge::FlushTcp()
{
    if (!Client || !Client.connected() || BtToTcp.Size() == 0)
    {
        return;
    }
    size_t contiguous = 0;
    const uint8_t *data = BtToTcp.Peek(contiguous);
    const size_t wanted = min(contiguous, ChunkSize);
    const int written = send(Client.fd(), data, wanted, MSG_DONTWAIT);
    if (written <= 0)
    {
        if (written < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
        {
            Diagnostics::Event("tcp_send_failed", errno);
            Client.stop();
        }
        return;
    }
    BtToTcp.Consume(written);
    Stats.AddBtToTcp(written);
}

void TcpBridge::LogDrops()
{
    if (static_cast<uint32_t>(millis() - LastDropLogMs) < DropLogIntervalMs)
    {
        return;
    }
    LastDropLogMs = millis();
    if (PendingTcpDropLog > 0)
    {
        Serial.printf("[BRIDGE] TCP -> Bluetooth buffer full; dropped %llu bytes\n", PendingTcpDropLog);
        PendingTcpDropLog = 0;
    }
    if (PendingNoClientDropLog > 0)
    {
        Serial.printf("[BRIDGE] Bluetooth output had no available TCP destination; dropped %llu bytes\n", PendingNoClientDropLog);
        PendingNoClientDropLog = 0;
    }
}

bool TcpBridge::HasClient()
{
    return Client && Client.connected();
}

bool TcpBridge::SetRemoteControl(bool enabled)
{
    if (enabled && (HasClient() || OtaActive))
    {
        return false;
    }
    if (RemoteControl == enabled)
    {
        return true;
    }
    RemoteControl = enabled;
    TcpToBt.Clear();
    BtToTcp.Clear();
    RemoteToBt.Clear();
    Serial.printf("[TCP] OBD command owner: %s\n", enabled ? "cloud" : "local TCP / none");
    return true;
}

bool TcpBridge::EnqueueRemoteCommand(const uint8_t *data, size_t length)
{
    if (!RemoteControl || OtaActive || length == 0)
    {
        return false;
    }
    const size_t accepted = RemoteToBt.Push(data, length);
    if (accepted < length)
    {
        const size_t dropped = length - accepted;
        Stats.AddTcpToBtDropped(dropped);
        PendingTcpDropLog += dropped;
        return false;
    }
    return true;
}

void TcpBridge::SetOtaActive(bool active)
{
    OtaActive = active;
    if (active)
    {
        TcpToBt.Clear();
        BtToTcp.Clear();
        RemoteToBt.Clear();
    }
}

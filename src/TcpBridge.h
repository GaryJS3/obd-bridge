#pragma once

#include <Arduino.h>
#include <WiFi.h>
#include "AppStats.h"
#include "BluetoothObd.h"

#ifndef TRACE_BRIDGE_DATA
#define TRACE_BRIDGE_DATA 0
#endif

template <size_t Capacity>
class ByteRing
{
public:
    size_t Size() const { return Count; }
    size_t Free() const { return Capacity - Count; }
    size_t Push(const uint8_t *data, size_t length)
    {
        const size_t accepted = min(length, Free());
        for (size_t i = 0; i < accepted; ++i)
        {
            Data[(Head + Count + i) % Capacity] = data[i];
        }
        Count += accepted;
        return accepted;
    }
    const uint8_t *Peek(size_t &length) const
    {
        length = min(Count, Capacity - Head);
        return &Data[Head];
    }
    void Consume(size_t length)
    {
        length = min(length, Count);
        Head = (Head + length) % Capacity;
        Count -= length;
    }
    void Clear() { Head = 0; Count = 0; }

private:
    uint8_t Data[Capacity]{};
    size_t Head = 0;
    size_t Count = 0;
};

class TcpBridge
{
public:
    TcpBridge(uint16_t port, BluetoothObd &bluetooth, AppStats &stats);
    void Begin();
    void Loop();
    bool HasClient();
    size_t PendingToBluetooth() const { return TcpToBt.Size(); }
    size_t PendingToTcp() const { return BtToTcp.Size(); }

private:
    uint16_t Port;
    BluetoothObd &Bluetooth;
    AppStats &Stats;
    WiFiServer Server;
    WiFiClient Client;
    ByteRing<4096> TcpToBt;
    ByteRing<8192> BtToTcp;
    uint64_t PendingTcpDropLog = 0;
    uint64_t PendingNoClientDropLog = 0;
    uint32_t LastDropLogMs = 0;

    void AcceptClient();
    void ReadTcp();
    void ReadBluetooth();
    void FlushBluetooth();
    void FlushTcp();
    void LogDrops();
};

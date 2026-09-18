#pragma once

#include <Arduino.h>

struct StatsSnapshot
{
    uint64_t TcpToBtBytes;
    uint64_t BtToTcpBytes;
    uint64_t TcpConnections;
    uint64_t BluetoothReconnectAttempts;
    uint64_t BluetoothDisconnects;
    uint64_t WifiReconnects;
    uint64_t TcpToBtDroppedBytes;
    uint64_t BtToTcpDroppedBytes;
};

class AppStats
{
public:
    void AddTcpToBt(uint64_t count);
    void AddBtToTcp(uint64_t count);
    void AddTcpConnection();
    void AddBluetoothReconnectAttempt();
    void AddBluetoothDisconnect();
    void AddWifiReconnect();
    void AddTcpToBtDropped(uint64_t count);
    void AddBtToTcpDropped(uint64_t count);
    StatsSnapshot Snapshot() const;

private:
    mutable portMUX_TYPE Mutex = portMUX_INITIALIZER_UNLOCKED;
    StatsSnapshot Values{};
};


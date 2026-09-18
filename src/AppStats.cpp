#include "AppStats.h"

#define STAT_ADD(field, count) do { portENTER_CRITICAL(&Mutex); Values.field += (count); portEXIT_CRITICAL(&Mutex); } while (0)

void AppStats::AddTcpToBt(uint64_t count) { STAT_ADD(TcpToBtBytes, count); }
void AppStats::AddBtToTcp(uint64_t count) { STAT_ADD(BtToTcpBytes, count); }
void AppStats::AddTcpConnection() { STAT_ADD(TcpConnections, 1); }
void AppStats::AddBluetoothReconnectAttempt() { STAT_ADD(BluetoothReconnectAttempts, 1); }
void AppStats::AddBluetoothDisconnect() { STAT_ADD(BluetoothDisconnects, 1); }
void AppStats::AddWifiReconnect() { STAT_ADD(WifiReconnects, 1); }
void AppStats::AddTcpToBtDropped(uint64_t count) { STAT_ADD(TcpToBtDroppedBytes, count); }
void AppStats::AddBtToTcpDropped(uint64_t count) { STAT_ADD(BtToTcpDroppedBytes, count); }

StatsSnapshot AppStats::Snapshot() const
{
    portENTER_CRITICAL(&Mutex);
    StatsSnapshot snapshot = Values;
    portEXIT_CRITICAL(&Mutex);
    return snapshot;
}


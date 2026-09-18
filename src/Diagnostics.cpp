#include "Diagnostics.h"

namespace
{
struct Entry { uint32_t Time; char Name[32]; int Status; int Value; };
Entry Entries[48]{};
size_t Next = 0, Count = 0;
uint64_t Received = 0, Completed = 0;
portMUX_TYPE Mutex = portMUX_INITIALIZER_UNLOCKED;
}
void Diagnostics::Event(const char *name, int status, int value)
{
    portENTER_CRITICAL(&Mutex);
    Entry &entry = Entries[Next];
    entry.Time = millis();
    snprintf(entry.Name, sizeof(entry.Name), "%s", name);
    entry.Status = status;
    entry.Value = value;
    Next = (Next + 1) % 48;
    if (Count < 48) ++Count;
    portEXIT_CRITICAL(&Mutex);
}
void Diagnostics::Receive(size_t count)
{
    portENTER_CRITICAL(&Mutex);
    Received += count;
    portEXIT_CRITICAL(&Mutex);
}
void Diagnostics::Written(size_t count)
{
    portENTER_CRITICAL(&Mutex);
    Completed += count;
    portEXIT_CRITICAL(&Mutex);
}
String Diagnostics::Json()
{
    Entry snapshot[48];
    portENTER_CRITICAL(&Mutex);
    const size_t count = Count;
    for (size_t i = 0; i < count; ++i) snapshot[i] = Entries[(Next + 48 - Count + i) % 48];
    const uint64_t received = Received, completed = Completed;
    portEXIT_CRITICAL(&Mutex);
    String json = "{\"spp_rx_bytes\":" + String(received) + ",\"spp_write_completed_bytes\":" + String(completed) + ",\"events\":[";
    for (size_t i = 0; i < count; ++i)
    {
        if (i) json += ',';
        json += "{\"ms\":" + String(snapshot[i].Time) + ",\"event\":\"" + snapshot[i].Name + "\",\"status\":" + String(snapshot[i].Status) + ",\"value\":" + String(snapshot[i].Value) + "}";
    }
    return json + "]}";
}

#pragma once

#include <Arduino.h>

class RemoteObdSink
{
public:
    virtual ~RemoteObdSink() = default;
    virtual bool QueueObdOutput(const uint8_t *data, size_t length) = 0;
};

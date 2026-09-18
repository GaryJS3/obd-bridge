#pragma once
#include <Arduino.h>

namespace Diagnostics
{
void Event(const char *name, int status = 0, int value = 0);
void Receive(size_t count);
void Written(size_t count);
String Json();
}

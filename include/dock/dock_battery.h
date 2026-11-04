#pragma once

#include "simple_dbus.h"

struct Dock;

void fill_battery_container(Dock *dock);
bool has_internal_battery();
void watch_battery_level();
BatteryStatus battery_status_snapshot();
void request_battery_refresh();
void battery_stop();

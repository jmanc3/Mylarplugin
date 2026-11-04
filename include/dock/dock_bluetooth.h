#pragma once

#include <atomic>

struct Dock;

void fill_bluetooth_container(Dock *dock);
void bluetooth_watch_dock(Dock *dock);
void bluetooth_start();
void bluetooth_stop();

extern std::atomic<bool> bluetooth_available;

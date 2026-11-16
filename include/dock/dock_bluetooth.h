#pragma once

#include "simple_dbus.h"

#include <atomic>
#include <map>
#include <memory>
#include <string>
#include <vector>

struct Dock;

void fill_bluetooth_container(Dock *dock);
void bluetooth_watch_dock(Dock *dock);
void bluetooth_start();
void bluetooth_stop();

extern std::atomic<bool> bluetooth_available;

// Shared Bluetooth state. D-Bus belongs to the compositor main thread, which
// publishes value snapshots; any Bluetooth UI (the dock menu, the settings
// page) calls bluetooth_menu_opened while visible and bluetooth_menu_closed
// when it goes away, polls bluetooth_latest, and posts actions with
// bluetooth_command / bluetooth_submit. All four are safe from any thread.
struct BluetoothMenuView {
    unsigned long revision = 0;
    std::vector<Adapter> adapters;
    std::vector<Device> devices;
    bool running = true;
    bool agent_ready = true;
    bool busy = false;
    bool scanning = false;
    bool scan_pending = false;
    bool scan_stopping = false;
    bool showing_paired = true;
    std::map<std::string, std::string> actions;
    bool error = false;
    std::string message;
    std::string pairing_path;
    std::string request_type;
    std::string request_device;
    std::string pin;
    unsigned long request_id = 0;
};

void bluetooth_menu_opened();
void bluetooth_menu_closed();
std::shared_ptr<const BluetoothMenuView> bluetooth_latest();
// path: device object path ("" for adapter wide commands).
// command: "Scan", "Power On", "Power Off", "Pair", "Cancel Pair", "Connect", "Disconnect", "Unpair".
void bluetooth_command(const std::string &path, const std::string &command);
// Answers the pairing agent request `id` (BluetoothMenuView::request_id).
void bluetooth_submit(unsigned long id, bool accept, std::string input);
std::string bluetooth_display_name(const BluetoothInterface &device);
// "Pairing…" style label for a pending action.
std::string bluetooth_action_label(const std::string &action);

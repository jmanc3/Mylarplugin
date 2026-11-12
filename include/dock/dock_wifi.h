#pragma once

#include "simple_dbus.h"

#include <memory>
#include <string>

struct Dock;

void fill_wifi_container(Dock *dock);
void wifi_stop();

// Network state for the dock button, mirroring winbar's tray icon (wifi_state()).
struct SNetworkIconState {
    bool up = false;
    bool wired = false;
};

SNetworkIconState network_icon_state();
// Polls the default route every few seconds and repaints the dock when it changes.
void wifi_watch_dock(Dock *dock);

// Shared Wi-Fi worker. Any Wi-Fi UI (the dock menu, the settings page) calls
// wifi_menu_opened while visible and wifi_menu_closed when it goes away, posts
// commands with wifi_command, and polls wifi_latest for value snapshots.
struct WifiMenuView {
    unsigned long revision = 0;
    WifiStatus status;
    bool scanning = false;
    bool busy = false;
    std::string connecting_ssid;
    std::string action_ssid;
    std::string action;
    bool error = false;
    std::string message;
};

// type: "scan", "toggle", "device", "notice", "sign-in", "connect", "disconnect", "forget".
struct WifiCommand {
    std::string type;
    std::string ssid;
    std::string text;
};

void wifi_menu_opened();
void wifi_menu_closed();
void wifi_command(WifiCommand command);
std::shared_ptr<const WifiMenuView> wifi_latest();

const WifiDevice *wifi_device(const WifiStatus &status);
const WifiAccessPoint *wifi_access_point(const WifiStatus &status, const std::string &ssid);
std::string wifi_band(unsigned frequency);
bool wifi_valid_password(const WifiAccessPoint &ap, const std::string &password, std::string &problem);
// Wi-Fi state only; used for the empty network list.
std::string wifi_state_line(const WifiMenuView &view);
// Header line: reports a wired default route first.
std::string wifi_status_line(const WifiMenuView &view);

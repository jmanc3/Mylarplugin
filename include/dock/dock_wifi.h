#pragma once

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

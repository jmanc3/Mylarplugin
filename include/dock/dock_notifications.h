#pragma once

#include "dock/dock_types.h"
#include "simple_dbus.h"

#include <cstdint>
#include <string>

struct DockNotification {
    Dock *dock = nullptr;
    MylarWindow *window = nullptr;
    NotificationInfo info;
    int logical_w = 0;
    int logical_h = 0;
    int margin_bottom = 0;
    int timer_fd = -1;
    bool closed = false;
    bool close_signal_sent = false;
    cairo_surface_t *icon_surface = nullptr;

    ~DockNotification() {
        if (icon_surface) {
            cairo_surface_destroy(icon_surface);
            icon_surface = nullptr;
        }
    }
};

std::string strip_html(const std::string &text);

// Thread-safe saved notification history shown in the menu above the date menu.
void save_notification(const NotificationInfo &info);
void clear_saved_notifications();
void remove_saved_notification(uint64_t uid);

// Populates or rebuilds the notifications panel above the date menu.
// Must be called on the dock's own Wayland app thread.
void fill_date_notifications_panel(Dock *dock, Container *panel);
void refresh_date_notifications_panel(Dock *dock);

// Must be called on the dock's own Wayland app thread.
void show_notification_on_dock(Dock *dock, const NotificationInfo &info);

// Must be called on the dock's own Wayland app thread.
void close_notification_on_dock(Dock *dock, uint32_t id, bool emit_signal = true);

// Called on dock teardown after main_loop exits.
void cleanup_dock_notifications(Dock *dock);

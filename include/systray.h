#pragma once

// System tray host for the dock's extra menu.
//
// Two protocols feed the same list of items:
//  - StatusNotifierItem (SNI) over the session D-Bus. Mylar becomes the
//    org.kde.StatusNotifierWatcher when nobody else is, otherwise it registers
//    as a host with the existing watcher. Menus come from com.canonical.dbusmenu.
//  - The legacy XEmbed system tray (_NET_SYSTEM_TRAY_S0) on Xwayland. Icon
//    windows are embedded into off-screen containers, their pixels are read
//    back and clicks are forwarded to them.
//
// All protocol work happens on the compositor's main thread. Docks read a
// snapshot of the items from their own threads.

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

struct TrayIconPixels {
    int w = 0;
    int h = 0;
    std::vector<uint32_t> argb; // premultiplied, native endian (cairo ARGB32 layout)
};

struct TrayMenuItem {
    int id = 0;
    std::string label;
    bool separator = false;
    bool enabled = true;
    bool visible = true;
    std::string toggle_type; // "", "checkmark" or "radio"
    int toggle_state = -1;   // 0 off, 1 on, -1 unknown
    std::vector<TrayMenuItem> children;
};

struct TrayItemView {
    std::string key;
    bool xembed = false;
    std::string title;
    std::string icon_name;       // name, or absolute path
    std::string icon_theme_path; // extra directory to look for icon_name in
    TrayIconPixels pixels;       // used when non-empty (SNI pixmap or XEmbed capture)
    bool item_is_menu = false;   // primary click should show the menu
    bool has_menu = false;
    bool needs_attention = false;
    unsigned long revision = 0;  // changes whenever the icon changes
};

namespace systray {
    // Main thread. start() after the session bus is up; stop() before it goes down.
    void start();
    void stop();

    // Any thread.
    std::vector<TrayItemView> snapshot(unsigned long *revision = nullptr);
    unsigned long revision();

    // Main thread. button: 1 primary, 2 middle, 3 secondary.
    void click(const std::string &key, int button);
    void scroll(const std::string &key, int delta, bool horizontal);
    // on_ready runs on the main thread (with an empty list if the item has no menu).
    void fetch_menu(const std::string &key, std::function<void(std::vector<TrayMenuItem>)> on_ready);
    void menu_clicked(const std::string &key, int id);
}

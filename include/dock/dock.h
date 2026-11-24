#pragma once

#include <cstdint>
#include <string>
#include <map>
#include <vector>

struct Bounds;
struct NotificationInfo;

namespace dock {
    inline constexpr double popup_corner_radius = 10.0;

    void start(std::string monitor_name = "");
    void stop(std::string monitor_name = "");

    void show_notification(const NotificationInfo &ni);
    void close_notification(uint32_t id);

    void redraw();
    void update_workspaces();

    void add_window(int cid);
    void remove_window(int cid);
    void title_change(int cid, std::string title);
    void on_activated(int cid);
    // A window was minimized (hidden) or restored. Main thread.
    void on_hidden(int cid, bool hidden);

    void create_slept_button();
    void throb_slept_button();
    void remove_slept_button();

    void change_in_audio();
    void change_in_battery();
    void change_in_bluetooth();
    // The system tray items changed (called on the main thread).
    void change_in_systray();

    void edit_pin(std::string original_stacking_rule, std::string new_stacking_rule, std::string new_icon, std::string new_command);

    void toggle_dock_merge();
    void set_dock_merge(bool enabled);
    // "Default", "Bubbly" or "Bubbly flat".
    void set_dock_style(const std::string &style);
    // Recompute icon widths and slide icons to their new places.
    void relayout();

    void open_applications();
    // The extra menu's Project page (mirror / extend / turn off other monitors)
    // on the dock under the cursor. Main thread.
    void open_projection();

    Bounds get_location(std::string name, int cid);
    std::map<int, Bounds> try_get_locations(const std::string &name);
    Bounds get_item_location(std::string monitor_name, std::string item_name);
    bool try_get_date_menu_cards(const std::string &monitor_name, std::vector<Bounds> &out);
};

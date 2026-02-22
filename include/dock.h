#pragma once

#include <string>

struct Bounds;

namespace dock {
    void start(std::string monitor_name = "");
    void stop(std::string monitor_name = "");

    void redraw();

    void add_window(int cid);
    void remove_window(int cid);
    void title_change(int cid, std::string title);
    void on_activated(int cid);

    void change_in_audio();
    void change_in_battery();

    void edit_pin(std::string original_stacking_rule, std::string new_stacking_rule, std::string new_icon, std::string new_command);

    void toggle_dock_merge();

    Bounds get_location(std::string name, int cid);
};

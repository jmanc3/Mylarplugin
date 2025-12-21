#pragma once

#include <string>

namespace dock {
    void start(std::string monitor_name = "");
    void stop(std::string monitor_name = "");

    void redraw();

    void add_window(int cid);
    void remove_window(int cid);
    void title_change(int cid, std::string title);
    void on_activated(int cid);

    void toggle_dock_merge();
};

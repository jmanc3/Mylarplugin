#pragma once

#include <string>

namespace dock {
    void start();
    void stop();

    void add_window(int cid);
    void remove_window(int cid);
    void title_change(int cid, std::string title);
    void on_activated(int cid);
};

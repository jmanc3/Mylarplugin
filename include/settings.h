#pragma once

#include <string>

struct ConfigSettings;

namespace settings {
    // focus_search: put the keyboard cursor in the search field, ready for typing.
    void start(std::string page = "", bool focus_search = false);
    void stop();
    void load_save_settings(bool save, ConfigSettings* settings);
    // Call on the main thread right before changing set->monitors. The change
    // is rolled back unless the user keeps it from the display settings page.
    void display_change_begin();
};

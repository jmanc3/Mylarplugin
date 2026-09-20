#pragma once

#include <string>

struct ConfigSettings;

namespace settings {
    void start(std::string page = "");
    void stop();
    void load_save_settings(bool save, ConfigSettings* settings);
};

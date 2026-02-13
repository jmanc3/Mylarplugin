#pragma once

struct ConfigSettings;

namespace settings {
    void start();
    void stop();
    void load_save_settings(bool save, ConfigSettings* settings);
};

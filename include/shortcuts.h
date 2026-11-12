#pragma once

#include <functional>
#include <string>
#include <vector>

struct ConfigSettings;

// One bindable action and the shortcut it ships with.
struct ShortcutDefault {
    std::string id;      // Stable key used in the settings file
    std::string name;    // Shown on the Shortcuts page
    std::string keys;    // Default shortcut, empty when the action ships unbound
    std::string action;  // Lua expression handed to hl.bind
    std::string options; // Lua table of bind options, may be empty
    bool plugin = false; // Action lives in hl.plugin.mylar
};

namespace shortcuts {
    // Every bindable action, ordered by how likely a user is to want it.
    const std::vector<ShortcutDefault> &defaults();

    // The shortcut currently in effect: the user's override, or else the default.
    std::string keys_for(const ConfigSettings *settings, const ShortcutDefault &shortcut);

    // Order and case independent form, for telling whether two shortcuts collide.
    std::string normalized(const std::string &keys);

    // The hl.bind lines for the generated Hyprland config.
    std::string lua(const ConfigSettings *settings);

    // Key capture for the Shortcuts page. Compositor main thread only.
    // While capturing, every key event is swallowed and each completed combo is
    // reported through on_keys. An empty string means the capture was cancelled
    // (Escape or inactivity) and has already stopped.
    void capture_start(std::function<void(std::string keys)> on_keys);
    void capture_stop();
    bool on_key_press(int key, int state);
}

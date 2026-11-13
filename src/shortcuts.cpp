#include "shortcuts.h"

#include "hypriso.h"

#include <algorithm>
#include <cctype>
#include <linux/input-event-codes.h>
#include <set>
#include <sstream>

static const char *exit_command =
    "hl.dsp.exec_cmd(\"command -v hyprshutdown >/dev/null 2>&1 && hyprshutdown || hyprctl dispatch 'hl.dsp.exit()'\")";
static const char *locked = "{ locked = true }";
static const char *locked_repeating = "{ locked = true, repeating = true }";

static std::vector<ShortcutDefault> build_defaults() {
    std::vector<ShortcutDefault> list = {
        // Desktop
        {"applications", "Open applications menu", "SUPER_L", "hl.plugin.mylar.applications", "{ release = true }", true},
        {"toggle_desktop_show", "Show desktop", "SUPER_L + D", "hl.plugin.mylar.toggle_desktop_show", "", true},
        {"terminal", "Open terminal", "ALT + Q", "hl.dsp.exec_cmd(terminal)", ""},
        {"close_window", "Close window", "ALT + C", "hl.dsp.window.close()", ""},
        {"screenshot_tool", "Take a screenshot", "Print", "hl.plugin.mylar.screenshot_tool", "", true},
        {"screenshot_tool_draw", "Take a screenshot and draw on it", "", "hl.plugin.mylar.screenshot_tool_draw", "", true},

        // Snapping
        {"snap_left", "Snap window left", "", "hl.plugin.mylar.snap_left", "", true},
        {"snap_right", "Snap window right", "", "hl.plugin.mylar.snap_right", "", true},
        {"snap_up", "Maximize window", "", "hl.plugin.mylar.snap_up", "", true},
        {"snap_down", "Restore window", "", "hl.plugin.mylar.snap_down", "", true},

        // Overview
        {"overview_open_or_show_desktop", "Open overview", "", "hl.plugin.mylar.overview_open_or_show_desktop", "", true},
        {"overview_close_or_hide_desktop", "Close overview, or hide windows", "", "hl.plugin.mylar.overview_close_or_hide_desktop", "", true},

        // Floating and focus
        {"toggle_floating", "Toggle floating", "ALT + V", "hl.dsp.window.float({ action = \"toggle\" })", ""},
        {"toggle_floating_alt", "Toggle floating (alternate)", "SUPER + G", "hl.dsp.window.float({ action = \"toggle\" })", ""},
        {"toggle_layout", "Toggle tiling on this workspace", "", "hl.plugin.mylar.toggle_layout", "", true},

        // Keyboard, also shown on the Keyboard page
        {"switch_layout", "Switch layout", "SUPER + SPACE", "hl.dsp.exec_cmd(\"hyprctl switchxkblayout all next\")", ""},
    };

    static const char *directions[] = {"left", "right", "up", "down"};
    for (auto direction : directions) {
        std::string d = direction;
        list.push_back({"focus_" + d, "Focus window " + d, "ALT + " + d, "hl.dsp.focus({ direction = \"" + d + "\" })", ""});
    }
    for (auto direction : directions) {
        std::string d = direction;
        list.push_back({"focus_" + d + "_alt", "Focus window " + d + " (alternate)", "SUPER + " + d, "hl.dsp.focus({ direction = \"" + d + "\" })", ""});
    }
    for (auto direction : directions) {
        std::string d = direction;
        list.push_back({"move_" + d, "Move window " + d, "SHIFT + SUPER + " + d, "hl.dsp.window.move({ direction = \"" + d + "\" })", ""});
    }

    // Workspaces, 10 lives on the 0 key.
    for (int i = 1; i <= 10; i++) {
        auto n = std::to_string(i);
        list.push_back({"workspace_" + n, "Switch to workspace " + n, "ALT + " + std::to_string(i % 10), "hl.dsp.focus({ workspace = " + n + " })", ""});
    }
    for (int i = 1; i <= 10; i++) {
        auto n = std::to_string(i);
        list.push_back({"move_to_workspace_" + n, "Move window to workspace " + n, "ALT + SHIFT + " + std::to_string(i % 10),
                        "hl.dsp.window.move({ workspace = " + n + " })", ""});
    }

    std::vector<ShortcutDefault> rest = {
        // Media keys
        {"volume_up", "Volume up", "XF86AudioRaiseVolume", "hl.dsp.exec_cmd(\"wpctl set-volume -l 1 @DEFAULT_AUDIO_SINK@ 5%+\")", locked_repeating},
        {"volume_down", "Volume down", "XF86AudioLowerVolume", "hl.dsp.exec_cmd(\"wpctl set-volume @DEFAULT_AUDIO_SINK@ 5%-\")", locked_repeating},
        {"volume_mute", "Mute", "XF86AudioMute", "hl.dsp.exec_cmd(\"wpctl set-mute @DEFAULT_AUDIO_SINK@ toggle\")", locked_repeating},
        {"mic_mute", "Mute microphone", "XF86AudioMicMute", "hl.dsp.exec_cmd(\"wpctl set-mute @DEFAULT_AUDIO_SOURCE@ toggle\")", locked_repeating},
        {"brightness_up", "Brightness up", "XF86MonBrightnessUp", "hl.dsp.exec_cmd(\"brightnessctl -e4 -n2 set 5%+\")", locked_repeating},
        {"brightness_down", "Brightness down", "XF86MonBrightnessDown", "hl.dsp.exec_cmd(\"brightnessctl -e4 -n2 set 5%-\")", locked_repeating},
        {"media_play", "Play / pause", "XF86AudioPlay", "hl.dsp.exec_cmd(\"playerctl play-pause\")", locked},
        {"media_pause", "Play / pause (pause key)", "XF86AudioPause", "hl.dsp.exec_cmd(\"playerctl play-pause\")", locked},
        {"media_next", "Next track", "XF86AudioNext", "hl.dsp.exec_cmd(\"playerctl next\")", locked},
        {"media_previous", "Previous track", "XF86AudioPrev", "hl.dsp.exec_cmd(\"playerctl previous\")", locked},

        // Tiling layout
        {"layout_next", "Tiling: focus next window", "SUPER + J", "hl.dsp.layout(\"cyclenext\")", ""},
        {"layout_previous", "Tiling: focus previous window", "SUPER + K", "hl.dsp.layout(\"cycleprev\")", ""},
        {"layout_swap_master", "Tiling: swap with main window", "SUPER + Return", "hl.dsp.layout(\"swapwithmaster auto\")", ""},
        {"layout_shrink_master", "Tiling: shrink main area", "SUPER + H", "hl.dsp.layout(\"mfact -0.05\")", ""},
        {"layout_grow_master", "Tiling: grow main area", "SUPER + L", "hl.dsp.layout(\"mfact +0.05\")", ""},

        // Scrolling layout
        {"scrolling_width_next", "Scrolling: next column width", "SUPER + R", "hl.dsp.layout(\"colresize +conf\")", ""},
        {"scrolling_width_previous", "Scrolling: previous column width", "SUPER + SHIFT + R", "hl.dsp.layout(\"colresize -conf\")", ""},
        {"scrolling_center", "Scrolling: center column", "SUPER + C", "hl.dsp.layout(\"center\")", ""},
        {"scrolling_consume_previous", "Scrolling: move window into column on the left", "SUPER + bracketleft", "hl.dsp.layout(\"consume_or_expel prev\")", ""},
        {"scrolling_consume_next", "Scrolling: move window into column on the right", "SUPER + bracketright", "hl.dsp.layout(\"consume_or_expel next\")", ""},
        {"scrolling_swap_column_left", "Scrolling: move column left", "SUPER + CTRL + left", "hl.dsp.layout(\"swapcol l\")", ""},
        {"scrolling_swap_column_right", "Scrolling: move column right", "SUPER + CTRL + right", "hl.dsp.layout(\"swapcol r\")", ""},
        {"scrolling_full_width", "Scrolling: make column full width", "SUPER + F", "hl.dsp.layout(\"fit active\")", ""},
        {"scrolling_fit_visible", "Scrolling: fit visible columns on screen", "SUPER + SHIFT + F", "hl.dsp.layout(\"fit visible\")", ""},
        {"scrolling_promote", "Scrolling: move window into its own column", "SUPER + P", "hl.dsp.layout(\"promote\")", ""},

        // Scratchpad
        {"scratchpad_toggle", "Toggle scratchpad", "ALT + S", "hl.dsp.workspace.toggle_special(\"magic\")", ""},
        {"scratchpad_move", "Move window to scratchpad", "ALT + SHIFT + S", "hl.dsp.window.move({ workspace = \"special:magic\" })", ""},

        {"exit", "Log out", "ALT + M", exit_command, ""},

        // Remaining plugin actions, unbound unless the user picks a shortcut.
        {"desktop_show", "Hide all windows", "", "hl.plugin.mylar.desktop_show", "", true},
        {"desktop_hide", "Bring back hidden windows", "", "hl.plugin.mylar.desktop_hide", "", true},
        {"right_click_activate", "Open window menu", "", "hl.plugin.mylar.right_click_activate", "", true},
        {"toggle_dock_merge", "Toggle merging of dock windows", "", "hl.plugin.mylar.toggle_dock_merge", "", true},
        {"dock_start", "Start the dock", "", "hl.plugin.mylar.dock_start", "", true},
        {"dock_stop", "Stop the dock", "", "hl.plugin.mylar.dock_stop", "", true},
        {"desktop_icons_start", "Show desktop icons", "", "hl.plugin.mylar.desktop_icons_start", "", true},
        {"desktop_icons_stop", "Hide desktop icons", "", "hl.plugin.mylar.desktop_icons_stop", "", true},
    };
    list.insert(list.end(), rest.begin(), rest.end());
    return list;
}

const std::vector<ShortcutDefault> &shortcuts::defaults() {
    static const std::vector<ShortcutDefault> list = build_defaults();
    return list;
}

std::string shortcuts::keys_for(const ConfigSettings *settings, const ShortcutDefault &shortcut) {
    for (const auto &override : settings->shortcuts)
        if (override.id == shortcut.id)
            return override.keys;
    return shortcut.keys;
}

std::string shortcuts::normalized(const std::string &keys) {
    std::vector<std::string> parts;
    std::string part;
    std::istringstream stream(keys);
    while (std::getline(stream, part, '+')) {
        std::string token;
        for (char ch : part)
            if (!std::isspace((unsigned char) ch))
                token += (char) std::toupper((unsigned char) ch);
        if (!token.empty())
            parts.push_back(token);
    }
    // A sided modifier in a combo acts like the plain modifier.
    if (parts.size() > 1)
        for (auto &token : parts)
            if (token.size() > 2 && (token.ends_with("_L") || token.ends_with("_R")))
                token.resize(token.size() - 2);
    std::sort(parts.begin(), parts.end());
    std::string out;
    for (const auto &token : parts)
        out += (out.empty() ? "" : "+") + token;
    return out;
}

static std::string lua_quote(const std::string &text) {
    std::string out = "\"";
    for (char ch : text) {
        if (ch == '"' || ch == '\\')
            out += '\\';
        if (ch == '\n')
            continue;
        out += ch;
    }
    return out + "\"";
}

std::string shortcuts::lua(const ConfigSettings *settings) {
    std::string general = "---- SHORTCUTS (edit them in Settings > Shortcuts) ----\n";
    std::string plugin;
    for (const auto &shortcut : defaults()) {
        auto keys = keys_for(settings, shortcut);
        if (keys.empty())
            continue;
        std::string line = "hl.bind(" + lua_quote(keys) + ", " + shortcut.action;
        if (!shortcut.options.empty())
            line += ", " + shortcut.options;
        line += ")\n";
        if (shortcut.plugin)
            plugin += "    " + line;
        else
            general += line;
    }
    for (const auto &custom : settings->custom_shortcuts) {
        if (custom.keys.empty() || custom.command.empty())
            continue;
        general += "hl.bind(" + lua_quote(custom.keys) + ", hl.dsp.exec_cmd(" + lua_quote(custom.command) + "))\n";
    }
    if (!plugin.empty())
        general += "\nif hl.plugin.mylar ~= nil then\n" + plugin + "end\n";
    return general + "\n";
}

/*************************************************
 *
 * Key capture, compositor main thread
 *
 *************************************************/

static const float capture_timeout_ms = 15000;

static bool capturing = false;
// After a capture ends, keys that are still down must not leak their release into Hyprland's binds.
static bool draining = false;
static std::set<int> held;
static std::function<void(std::string)> capture_on_keys;
static unsigned capture_activity = 0;

static bool is_modifier(int key) {
    switch (key) {
        case KEY_LEFTCTRL:
        case KEY_RIGHTCTRL:
        case KEY_LEFTALT:
        case KEY_RIGHTALT:
        case KEY_LEFTSHIFT:
        case KEY_RIGHTSHIFT:
        case KEY_LEFTMETA:
        case KEY_RIGHTMETA:
            return true;
    }
    return false;
}

static void capture_finish() {
    capturing = false;
    draining = !held.empty();
    capture_on_keys = nullptr;
    capture_activity++;
}

// Cancels a capture the user walked away from, so the keyboard is never left swallowed.
static void capture_arm_timeout() {
    const unsigned activity = ++capture_activity;
    later(capture_timeout_ms, [activity](Timer *) {
        if (!capturing || activity != capture_activity)
            return;
        auto on_keys = capture_on_keys;
        capture_finish();
        if (on_keys)
            on_keys("");
    });
}

void shortcuts::capture_start(std::function<void(std::string keys)> on_keys) {
    capturing = true;
    draining = false;
    held.clear();
    capture_on_keys = std::move(on_keys);
    capture_arm_timeout();
}

void shortcuts::capture_stop() {
    if (!capturing)
        return;
    capture_finish();
}

bool shortcuts::on_key_press(int key, int state) {
    if (!capturing && !draining)
        return false;

    if (state)
        held.insert(key);
    else
        held.erase(key);

    if (draining) {
        if (held.empty())
            draining = false;
        return true;
    }
    if (!state)
        return true;

    capture_arm_timeout();
    if (is_modifier(key))
        return true;

    std::string keys;
    if (held.contains(KEY_LEFTCTRL) || held.contains(KEY_RIGHTCTRL))
        keys += "CTRL + ";
    if (held.contains(KEY_LEFTALT) || held.contains(KEY_RIGHTALT))
        keys += "ALT + ";
    if (held.contains(KEY_LEFTSHIFT) || held.contains(KEY_RIGHTSHIFT))
        keys += "SHIFT + ";
    if (held.contains(KEY_LEFTMETA) || held.contains(KEY_RIGHTMETA))
        keys += "SUPER + ";

    auto on_keys = capture_on_keys;
    if (key == KEY_ESC && keys.empty()) {
        capture_finish();
        if (on_keys)
            on_keys("");
        return true;
    }

    keys += hypriso->keycode_to_key_name(key);
    if (on_keys)
        on_keys(keys);
    return true;
}

#include "cancellable_command.h"
#include "plugin_lifetime.h"

#include "settings.h"
#include "pango_font_cache.h"
#include "text_editor.h"

#include "container.h"
#include "desktop_icons.h"
#include "heart.h"
#include "icons.h"
#include "hypriso.h"
#include "shortcuts.h"
#include "client/raw_windowing.h"
#include "client/windowing.h"
#include "dock/dock.h"
#include "dock/dock_wifi.h"
#include "dock/dock_bluetooth.h"
#include "dock/dock_brightness.h"
#include "dock/dock_battery.h"
#include "dock/dock_extra.h"
#include "audio.h"
#include "events.h"
#include "datetime.h"
#include "smooth_scroll.h"
#include "wallpaper_layout.h"
#include <chrono>
#include <csetjmp>
#include <dlfcn.h>
#include <hyprland/src/helpers/MiscFunctions.hpp>

#include <gtk/gtk.h>
#include <thread>
#include <unistd.h>
#include <sys/wait.h>
#include <atomic>
#include <mutex>
#include <optional>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <filesystem>
#include <pango/pango-font.h>
#include <cairo.h>
#include <pango/pango-layout.h>
#include <pango/pango-types.h>
#include <pango/pangocairo.h>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <xkbcommon/xkbcommon-keysyms.h>

static RawApp *settings_app = nullptr;
static std::atomic<RawWindow *> settings_redraw_window = nullptr;
static MylarWindow *settings_mylar = nullptr;
static std::string requested_page;
static bool icon_cache_rescan_in_progress = false;

struct SettingsTheme {
    RGBA background;
    RGBA row_background;
    RGBA row_hover;
    RGBA row_border;
    RGBA text;
    RGBA text_secondary;
    RGBA accent;
    RGBA toggle_border;
    RGBA toggle_knob_on;
    RGBA toggle_knob_off;
    RGBA slider_track;
    RGBA slider_thumb;
    RGBA segment_background;
    RGBA segment_selected;
    RGBA dropdown_background;
    RGBA dropdown_hover;
    RGBA dropdown_pressed;
    RGBA dropdown_border;
    RGBA popup_background;
    RGBA popup_hover;
    RGBA popup_pressed;
    RGBA button_background;
    RGBA button_hover;
    RGBA button_pressed;
    RGBA field_background;
    RGBA field_border;
    RGBA monitor_background;
    RGBA monitor_surface;
    RGBA navigation_hover;
    RGBA navigation_pressed;
};

static const SettingsTheme dark_theme = {
    .background = RGBA(0x15 / 255.0f, 0x1A / 255.0f, 0x21 / 255.0f, 1),
    .row_background = RGBA(0x20 / 255.0f, 0x27 / 255.0f, 0x31 / 255.0f, 1),
    .row_hover = RGBA(0x27 / 255.0f, 0x2F / 255.0f, 0x3B / 255.0f, 1),
    .row_border = RGBA(0x44 / 255.0f, 0x52 / 255.0f, 0x67 / 255.0f, 1),
    .text = RGBA(0xE7 / 255.0f, 0xEC / 255.0f, 0xF3 / 255.0f, 1),
    .text_secondary = RGBA(0xA9 / 255.0f, 0xB4 / 255.0f, 0xC3 / 255.0f, 1),
    .accent = RGBA(0x4D / 255.0f, 0xA3 / 255.0f, 0xFF / 255.0f, 1),
    .toggle_border = RGBA(0x44 / 255.0f, 0x52 / 255.0f, 0x67 / 255.0f, 1),
    .toggle_knob_on = RGBA(0xF7 / 255.0f, 0xFB / 255.0f, 0xFF / 255.0f, 1),
    .toggle_knob_off = RGBA(0xAD / 255.0f, 0xB9 / 255.0f, 0xC9 / 255.0f, 1),
    .slider_track = RGBA(0x44 / 255.0f, 0x52 / 255.0f, 0x67 / 255.0f, 1),
    .slider_thumb = RGBA(0xF7 / 255.0f, 0xFB / 255.0f, 0xFF / 255.0f, 1),
    .segment_background = RGBA(0x1B / 255.0f, 0x22 / 255.0f, 0x2C / 255.0f, 1),
    .segment_selected = RGBA(0x34 / 255.0f, 0x46 / 255.0f, 0x5D / 255.0f, 1),
    .dropdown_background = RGBA(0x31 / 255.0f, 0x3A / 255.0f, 0x46 / 255.0f, 1),
    .dropdown_hover = RGBA(0x3B / 255.0f, 0x46 / 255.0f, 0x54 / 255.0f, 1),
    .dropdown_pressed = RGBA(0x46 / 255.0f, 0x54 / 255.0f, 0x66 / 255.0f, 1),
    .dropdown_border = RGBA(0x44 / 255.0f, 0x52 / 255.0f, 0x67 / 255.0f, 1),
    .popup_background = RGBA(0x1B / 255.0f, 0x22 / 255.0f, 0x2C / 255.0f, 1),
    .popup_hover = RGBA(0x3B / 255.0f, 0x46 / 255.0f, 0x54 / 255.0f, 1),
    .popup_pressed = RGBA(0x46 / 255.0f, 0x54 / 255.0f, 0x66 / 255.0f, 1),
    .button_background = RGBA(0x31 / 255.0f, 0x3A / 255.0f, 0x46 / 255.0f, 1),
    .button_hover = RGBA(0x3B / 255.0f, 0x46 / 255.0f, 0x54 / 255.0f, 1),
    .button_pressed = RGBA(0x46 / 255.0f, 0x54 / 255.0f, 0x66 / 255.0f, 1),
    .field_background = RGBA(0x1B / 255.0f, 0x22 / 255.0f, 0x2C / 255.0f, 1),
    .field_border = RGBA(0x44 / 255.0f, 0x52 / 255.0f, 0x67 / 255.0f, 1),
    .monitor_background = RGBA(0x1B / 255.0f, 0x22 / 255.0f, 0x2C / 255.0f, 1),
    .monitor_surface = RGBA(0x20 / 255.0f, 0x27 / 255.0f, 0x31 / 255.0f, 1),
    .navigation_hover = RGBA(0x3B / 255.0f, 0x46 / 255.0f, 0x54 / 255.0f, 1),
    .navigation_pressed = RGBA(0x46 / 255.0f, 0x54 / 255.0f, 0x66 / 255.0f, 1),
};

static const SettingsTheme light_theme = {
    .background = RGBA(.941, .957, .976, 1),
    .row_background = RGBA(.984, .988, .992, 1),
    .row_hover = RGBA(.965, .965, .965, 1),
    .row_border = RGBA(.818, .818, .818, 1),
    .text = RGBA(0, 0, 0, 1),
    .text_secondary = RGBA(0, 0, 0, .5),
    .accent = RGBA(0, .52, .9, 1),
    .toggle_border = RGBA(.537, .537, .537, 1),
    .toggle_knob_on = RGBA(1, 1, 1, 1),
    .toggle_knob_off = RGBA(.365, .365, .365, 1),
    .slider_track = RGBA(.84, .84, .84, 1),
    .slider_thumb = RGBA(1, 1, 1, 1),
    .segment_background = RGBA(.84, .84, .84, 1),
    .segment_selected = RGBA(1, 1, 1, 1),
    .dropdown_background = RGBA(1, 1, 1, 1),
    .dropdown_hover = RGBA(.9, .9, .9, 1),
    .dropdown_pressed = RGBA(.7, .7, .7, 1),
    .dropdown_border = RGBA(.7, .7, .7, 1),
    .popup_background = RGBA(1, 1, 1, 1),
    .popup_hover = RGBA(.9, .9, .9, 1),
    .popup_pressed = RGBA(.8, .8, .8, 1),
    .button_background = RGBA(.84, .84, .84, 1),
    .button_hover = RGBA(.65, .65, .65, 1),
    .button_pressed = RGBA(.5, .5, .5, 1),
    .field_background = RGBA(1, 1, 1, 1),
    .field_border = RGBA(.8, .8, .8, 1),
    .monitor_background = RGBA(0, 0, 0, .1),
    .monitor_surface = RGBA(1, 1, 1, 1),
    .navigation_hover = RGBA(0, 0, 0, .2),
    .navigation_pressed = RGBA(0, 0, 0, .1),
};

static SettingsTheme theme = dark_theme;

static float settings_control_rounding = 6;
static float optiontopbottompad = 15;
static float optionleftpad = 60;
static float optionrighttpad = 25;

struct LineParser {
    std::string_view input;
    size_t index = 0;

    explicit LineParser(std::string_view input)
        : input(input) {}

    bool has_another_line() const {
        return index < input.size();
    }

    void next_line() {
        while (index < input.size()) {
            char c = input[index++];

            if (c == '\r' || c == '\n') {
                // Treat \r\n as a single line ending.
                if (c == '\r' &&
                    index < input.size() &&
                    input[index] == '\n') {
                    ++index;
                }

                break;
            }
        }
    }

    bool not_end_of_line() const {
        return index < input.size() &&
               input[index] != '\n' &&
               input[index] != '\r';
    }

    void eat_blank() {
        while (not_end_of_line()) {
            char c = input[index];

            if (c != ' ' && c != '\t')
                break;

            ++index;
        }
    }

    std::optional<std::string_view> eat_until(std::string_view delimiter) {
        size_t start = index;

        while (not_end_of_line()) {
            if (input.substr(index, delimiter.size()) == delimiter) {
                auto result = input.substr(start, index - start);
                index += delimiter.size();
                return result;
            }

            ++index;
        }

        // Return whatever was consumed before reaching the end of the line.
        if (index > start) {
            return input.substr(start, index - start);
        }

        return std::nullopt;
    }

    std::string_view eat_until_line_end() {
        size_t start = index;

        while (not_end_of_line())
            ++index;

        return input.substr(start, index - start);
    }
};

struct RightData : UserData {
    float scroll         = 0.0;
    float applied_scroll = 0.0;
    float overflow       = 0.0;
    float content_h      = 0.0;

    // Scrollbar state
    bool bar_hovered      = false;
    bool bar_dragging     = false;
    double bar_grab       = 0;     // Mouse offset from the thumb top while dragging
    long last_interaction = -100000;
    bool fade_timer       = false;

    SmoothScroll smooth;
};

std::vector<MylarWindow *> popups;

#include <sstream>
#include <algorithm>

static std::string trim(std::string s) {
    s.erase(s.begin(), std::find_if(s.begin(), s.end(),
        [](unsigned char c){ return !std::isspace(c); }));
    s.erase(std::find_if(s.rbegin(), s.rend(),
        [](unsigned char c){ return !std::isspace(c); }).base(), s.end());
    return s;
}

/*************************************************
 *
 * Search registration
 *
 * Every row that has a title tags its container (Container::uuid) with a key
 * built from the section it's in and its title, so the search can find it
 * on the live page to scroll to and flash. Rows built while a page is filled
 * are also learned into the index, which keeps titles and descriptions of
 * visited pages exactly as shown. The static index near the end of the file
 * covers pages that weren't opened yet.
 *
 *************************************************/

struct SearchEntry {
    std::string page;        // Left tab label that opens the page
    std::string section;     // Section title the row sits under
    std::string title;
    std::string description;
    std::string keywords;    // Extra words people may search with, never shown
    // Runs after the page is filled, before looking for the row (e.g. picking a sub tab).
    std::function<void(Container *root)> after_open;
};

static std::string search_current_page;    // Page being filled, empty when none
static std::string search_current_section; // Last section title seen while filling
static std::vector<SearchEntry> search_learned;
// Guards search_learned and building an index, which the dock's applications
// menu also does from its own thread (settings::search).
static std::mutex search_build_mutex;

// Marks a container as an accordion whose hidden rows the search may unfold.
static constexpr int search_accordion_type = 0x5ea4c;

static std::string search_key(const std::string &section, const std::string &title) {
    return "search:" + section + "\x1f" + title;
}

static void search_tag(Container *row, const std::string &title, const std::string &description, bool learn = true) {
    if (!row || title.empty())
        return;
    row->uuid = search_key(search_current_section, title);
    if (!learn || search_current_page.empty())
        return;
    std::lock_guard<std::mutex> lock(search_build_mutex);
    for (auto &e : search_learned) {
        if (e.page == search_current_page && e.section == search_current_section && e.title == title) {
            e.description = description;
            return;
        }
    }
    search_learned.push_back({search_current_page, search_current_section, title, description, "", nullptr});
}

/*************************************************
 *
 * Categories
 *
 * The left column lists categories (like Windows 11). A category with
 * several pages opens an overview linking to them; one with a single page
 * opens that page directly. Page ids are what settings::start and search
 * entries use, so they stay stable even where the shown title differs.
 *
 *************************************************/

struct SettingsPageInfo {
    std::string id;
    std::string title;
    std::string description;
    std::string glyph; // Icon on the category overview
};

struct SettingsCategory {
    std::string name;
    std::string icon;     // res/icons/<icon>.svg, installed to /usr/share/mylardesktop/icons
    std::string fallback; // Icon theme name used when the bundled icon is missing
    std::vector<SettingsPageInfo> pages;
};

static bool has_sddm_service_enabled() {
    std::error_code ec;
    auto exists = [&](const std::filesystem::path &p) {
        return std::filesystem::exists(p, ec) || std::filesystem::is_symlink(p, ec);
    };

    // systemd: display-manager.service symlink pointing to sddm, or direct wants symlink.
    if (std::filesystem::is_symlink("/etc/systemd/system/display-manager.service", ec)) {
        auto target = std::filesystem::read_symlink("/etc/systemd/system/display-manager.service", ec).string();
        if (!ec && target.find("sddm") != std::string::npos)
            return true;
    }
    if (exists("/etc/systemd/system/multi-user.target.wants/sddm.service") ||
        exists("/etc/systemd/system/graphical.target.wants/sddm.service"))
        return true;

    // runit: enabled in active or default runsvdir.
    for (const char *p : {
             "/var/service/sddm",
             "/run/runit/service/sddm",
             "/run/runit/runsvdir/current/sddm",
             "/etc/runit/runsvdir/default/sddm",
         }) {
        if (exists(p))
            return true;
    }

    // s6 / s6-rc / 66 (r6): enabled in s6-rc bundle, live scandir, or 66 tree.
    for (const char *p : {
             "/etc/s6/adminsv/default/contents.d/sddm",
             "/etc/s6/adminsv/default/contents.d/sddm-srv",
             "/etc/s6/rc/compiled/servicedirs/sddm",
             "/etc/s6/rc/compiled/servicedirs/sddm-srv",
             "/etc/s6/current/sddm",
             "/etc/s6/current/sddm-srv",
             "/run/service/sddm",
             "/run/service/sddm-srv",
             "/run/s6/services/sddm",
             "/run/s6/services/sddm-srv",
         }) {
        if (exists(p))
            return true;
    }
    if (std::filesystem::is_directory("/var/lib/66/system", ec)) {
        for (const auto &tree : std::filesystem::directory_iterator("/var/lib/66/system", ec)) {
            if (!tree.is_directory(ec))
                continue;
            if (exists(tree.path() / "servicedirs/sddm") ||
                exists(tree.path() / "servicedirs/svc/sddm") ||
                exists(tree.path() / "servicedirs/sddm-srv"))
                return true;
        }
    }
    return false;
}

static const std::vector<SettingsCategory> &settings_categories() {
    static const std::vector<SettingsCategory> categories = []() {
        std::vector<SettingsCategory> list = {
            {"Display", "settings-display", "preferences-desktop-display", {
                {"Display", "Display", "Monitors, resolution, scale, brightness, wallpaper, desktop icons", "\uE7F4"},
            }},
            {"Network & internet", "settings-network", "network-wireless", {
                {"Wifi", "Wi-Fi", "Connect to wireless networks", "\uE701"},
            }},
            {"Sound", "settings-sound", "preferences-desktop-sound", {
                {"Audio", "Sound", "Volume, output and input devices, app volume", "\uE767"},
            }},
            {"Bluetooth", "settings-bluetooth", "preferences-system-bluetooth", {
                {"Bluetooth", "Bluetooth", "Pair, connect and forget devices", "\uE702"},
            }},
            {"Behaviour", "settings-behaviour", "preferences-desktop-wallpaper", {
                {"Desktop", "Desktop", "Theme, font, blur, overview, icon cache", "\uE771"},
                {"Dock", "Dock", "Dock style, labels, which windows show", "\uE75B"},
                {"Multitasking", "Multitasking", "App switching, workspaces, snap windows, hot corners", "\uE7C4"},
                {"Autostart", "Autostart", "Commands run when the desktop or session starts", "\uE7E8"},
            }},
            {"Devices", "settings-devices", "preferences-desktop-peripherals", {
                {"Mouse & Touchpad", "Mouse & touchpad", "Pointer speed, scrolling direction, buttons", "\uE962"},
                {"Keyboard", "Keyboard", "Layouts, repeat delay and rate", "\uE765"},
                {"Shortcuts", "Keyboard shortcuts", "System and custom key combinations", "\uEDA7"},
            }},
            {"Time & language", "settings-time", "preferences-system-time", {
                {"Time & Date", "Date & time", "Time zone, clock format, automatic time", "\uEC92"},
            }},
        };
        if (has_sddm_service_enabled()) {
            list[4].pages.push_back(
                {"SDDM", "Login screen (SDDM)", "Wallpaper and display scale for the SDDM login screen", "\uE77B"});
        }
        if (has_internal_battery()) {
            // After Bluetooth, before Behaviour.
            list.insert(list.begin() + 4, {"Battery", "settings-power", "battery", {
                {"Battery", "Battery", "Charge, battery usage history, time remaining", "\uEBAA"},
            }});
        }
        return list;
    }();
    return categories;
}

// The category holding `id`, which is either a page id or a category name.
static const SettingsCategory *settings_category_of(const std::string &id) {
    for (const auto &category : settings_categories()) {
        if (category.name == id)
            return &category;
        for (const auto &page : category.pages)
            if (page.id == id)
                return &category;
    }
    return nullptr;
}

static const SettingsPageInfo *settings_page_info(const std::string &id) {
    for (const auto &category : settings_categories())
        for (const auto &page : category.pages)
            if (page.id == id)
                return &page;
    return nullptr;
}

static std::string settings_page_title(const std::string &id) {
    auto page = settings_page_info(id);
    return page ? page->title : id;
}

// Opens a page or a category; false when `id` is neither.
static bool settings_show(Container *root, const std::string &id);

static std::string trim_newline(std::string s) {
    s.erase(s.begin(), std::find_if(s.begin(), s.end(),
        [](unsigned char c){ return !std::iscntrl(c); }));
    s.erase(std::find_if(s.rbegin(), s.rend(),
        [](unsigned char c){ return !std::iscntrl(c); }).base(), s.end());
    return s;
}

struct SettingsWorker {
    std::atomic<bool> done = false;
    std::thread thread;
};

static std::atomic<bool> settings_workers_stopping = false;
static std::mutex settings_workers_mutex;
static std::vector<std::shared_ptr<SettingsWorker>> settings_workers;


static void reap_settings_workers_locked() {
    for (int i = (int) settings_workers.size() - 1; i >= 0; i--) {
        if (settings_workers[i]->done.load()) {
            if (settings_workers[i]->thread.joinable())
                settings_workers[i]->thread.join();
            settings_workers.erase(settings_workers.begin() + i);
        }
    }
}

static void spawn_settings_worker(std::function<void()> fn) {
    std::lock_guard<std::mutex> lock(settings_workers_mutex);
    reap_settings_workers_locked();
    if (settings_workers_stopping.load() || plugin_lifetime.stop_requested())
        return;
    auto worker = std::make_shared<SettingsWorker>();
    worker->thread = std::thread([worker, fn = std::move(fn)]() {
        fn();
        worker->done = true;
    });
    settings_workers.push_back(std::move(worker));
}

#ifndef MYLAR_FILE_CHOOSER_PATH
#define MYLAR_FILE_CHOOSER_PATH "/usr/share/mylardesktop/mylar-file-chooser"
#endif

std::string choose_file(bool folder) {
    // A separate GTK process can be interrupted even during GTK's first
    // Wayland round trip. Local builds keep it beside the plugin.
    std::filesystem::path helper = MYLAR_FILE_CHOOSER_PATH;
    Dl_info info{};
    if (dladdr(reinterpret_cast<void *>(&choose_file), &info) && info.dli_fname) {
        auto local = std::filesystem::path(info.dli_fname).parent_path() / "mylar-file-chooser";
        if (access(local.c_str(), X_OK) == 0)
            helper = local;
    }
    std::vector<std::string> args{helper.string()};
    if (folder)
        args.emplace_back("--directory");
    std::string filename;
    if (run_cancellable_command(args, filename) != 0)
        return {};
    return filename;
}

// TODO: bad memory churn
template<typename T>
void parse(const std::vector<std::string>& lines,
           const std::string& name,
           T* ptr,
           ConfigSettings*)
{
    for (const auto& line_raw : lines) {
        std::string line = trim(line_raw);

        if (line.empty() || line[0] == '#')
            continue;

        auto pos = line.find('=');
        if (pos == std::string::npos)
            continue;

        std::string key = trim(line.substr(0, pos));
        if (key != name)
            continue;

        std::string value = trim(line.substr(pos + 1));

        try {
            if constexpr (std::is_same_v<T, float>) {
                *ptr = std::stof(value);
            }
            else if constexpr (std::is_same_v<T, std::string>) {
                *ptr = value;
            }
            else if constexpr (std::is_same_v<T, bool>) {
                std::string lower_value = value;
                std::transform(lower_value.begin(), lower_value.end(), lower_value.begin(), ::tolower);
                *ptr = (lower_value == "true" || lower_value == "1" || lower_value == "yes");
            }
            else if constexpr (std::is_integral_v<T>) {
                *ptr = static_cast<T>(std::stoll(value));
            }
            else {
                static_assert(sizeof(T) == 0, "Unsupported config type");
            }
        }
        catch (...) {
            // silently ignore any parsing errors, ptr remains unchanged
        }

        return; // stop after first matching key
    }
}

// Monitor changes take effect immediately but stay on trial: unless the user
// keeps them before the deadline they are rolled back. While a trial runs the
// settings file keeps holding the last confirmed monitors, so a change that
// crashes the compositor, or blanks every screen until the session is killed,
// is gone on the next start.
static const long display_trial_ms = 10000;

struct DisplayTrial {
    std::mutex                mutex; // `confirmed` is read by whichever thread saves settings
    std::vector<MylarMonitor> confirmed;
    std::atomic<bool>         active     = false;
    std::atomic<long>         deadline   = 0;
    std::atomic<bool>         page_stale = false; // the display page shows values that were rolled back
    unsigned                  generation = 0;     // main thread only, invalidates older timers
};
static DisplayTrial display_trial;
static void trial_dialog_sync_soon();

// Main thread only.
static void display_trial_finish(bool keep) {
    if (!display_trial.active)
        return;
    {
        std::lock_guard lock(display_trial.mutex);
        if (!keep)
            set->monitors = display_trial.confirmed;
        display_trial.confirmed.clear();
        display_trial.active = false;
    }
    display_trial.generation++;
    settings::load_save_settings(true, set);

    if (!keep) {
        heart::remember_window_positions();
        hypriso->reload();
        notify("Display settings reverted");
        // Outputs come back asynchronously, give them a moment before the page re-reads them.
        later(500, [](Timer*) {
            display_trial.page_stale = true;
            if (settings_mylar)
                windowing::redraw(settings_mylar->raw_window);
        });
    }
    if (settings_mylar)
        windowing::redraw(settings_mylar->raw_window);
    trial_dialog_sync_soon();
}

void settings::display_change_begin() {
    {
        std::lock_guard lock(display_trial.mutex);
        // Changes made during a trial extend it, the rollback target stays the last confirmed state.
        if (!display_trial.active) {
            display_trial.confirmed = set->monitors;
            display_trial.active    = true;
        }
    }
    display_trial.deadline  = get_current_time_in_ms() + display_trial_ms;
    const unsigned generation = ++display_trial.generation;
    later(display_trial_ms, [generation](Timer*) {
        if (display_trial.generation == generation)
            display_trial_finish(false);
    });
    trial_dialog_sync_soon();
}

void settings::load_save_settings(bool save, ConfigSettings* settings) {
    const char* home = std::getenv("HOME");
    if (!home) return;

#ifdef NDEBUG
    std::filesystem::path filepath =
        std::filesystem::path(home) / ".config/mylar/mylar_settings.txt";
#else
    std::filesystem::path filepath =
        std::filesystem::path(home) / ".config/mylar/debug_mylar_settings.txt";
#endif

    std::filesystem::create_directories(filepath.parent_path());

    std::ofstream out;
    std::vector<std::string> lines;

    int file_version = 1;

    if (save) {
        out.open(filepath, std::ios::trunc);
        if (!out) return;
        out << "#version " << settings->version << "\n\n";
    } else {
        std::ifstream in(filepath);
        if (!in) return;

        std::string line;
        while (std::getline(in, line)) {
            lines.push_back(line);
        }

        for (const std::string& line : lines) {
            size_t i = 0;
            while (i < line.size() && std::isspace((unsigned char)line[i]))
                ++i;
            const char* tag = "#version";
            size_t tag_len = 8;
            if (line.compare(i, tag_len, tag) != 0)
                continue;
            i += tag_len;
            while (i < line.size() &&
                   (std::isspace((unsigned char)line[i]) || line[i] == '='))
                ++i;
            if (i < line.size())
                file_version = std::strtol(line.c_str() + i, nullptr, 10);
            break;
        }
    }

    #define bind(type, name, ptr) \
    do { \
        if (save) { \
            out << name << " = " << *ptr << "\n"; \
        } else { \
            parse<type>(lines, name, ptr, settings); \
        } \
    } while(0)
        
    bind(std::string, "touchpad_acceleration_curve", &settings->touchpad_acceleration_curve);
    bind(std::string, "primary_mouse_button", &settings->primary_mouse_button);
    bind(float, "cursor_speed", &settings->touchpad_speed);
    bind(float, "mouse_speed", &settings->mouse_speed);
    bind(bool, "natural_scrolling_mouse", &settings->natural_scrolling_mouse);
    bind(bool, "natural_scrolling_touchpad", &settings->natural_scrolling_touchpad);
    bind(bool, "touchpad_disable_while_typing", &settings->touchpad_disable_while_typing);
    bind(int, "repeat_delay", &settings->repeat_delay);
    bind(int, "repeat_rate", &settings->repeat_rate);
    bind(bool, "show_docks", &settings->show_docks);
    bind(bool, "battery_history_enabled", &settings->battery_history_enabled);
    bind(bool, "dock_current_workspace_only", &settings->dock_current_workspace_only);
    bind(bool, "dock_merge_windows", &settings->dock_merge_windows);
    bind(std::string, "dock_style", &settings->dock_style);
    bind(bool, "dock_bounce_animation", &settings->dock_bounce_animation);
    bind(bool, "dock_label_same_width", &settings->dock_label_same_width);
    bind(bool, "draw_wallpaper", &settings->draw_wallpaper);
    bind(std::string, "wallpaper_scaling", &settings->wallpaper_scaling);
    bind(float, "wallpaper_zoom", &settings->wallpaper_zoom);
    bind(float, "wallpaper_center_x", &settings->wallpaper_center_x);
    bind(float, "wallpaper_center_y", &settings->wallpaper_center_y);
    bind(bool, "wallpaper_span", &settings->wallpaper_span);
    bind(std::string, "wallpaper_monitors", &settings->wallpaper_monitors);
    bind(std::string, "sddm_wallpaper_mode", &settings->sddm_wallpaper_mode);
    if (settings->sddm_wallpaper_mode != "Display" && settings->sddm_wallpaper_mode != "Custom" && settings->sddm_wallpaper_mode != "Theme default")
        settings->sddm_wallpaper_mode = "Display";
    bind(std::string, "sddm_wallpaper_monitor", &settings->sddm_wallpaper_monitor);
    bind(std::string, "sddm_scale", &settings->sddm_scale);
    if (settings->sddm_scale.empty())
        settings->sddm_scale = "auto";
    bind(bool, "dark_theme", &settings->dark_theme);
    bind(bool, "clock_24h", &settings->clock_24h);
    bind(bool, "hotcorners", &settings->hotcorners);
    bind(bool, "app_switcher_current_workspace_only", &settings->app_switcher_current_workspace_only);
    bind(bool, "dynamic_workspaces", &settings->dynamic_workspaces);
    bind(int, "fixed_workspace_count", &settings->fixed_workspace_count);
    settings->fixed_workspace_count = std::clamp(settings->fixed_workspace_count, 1, 20);
    bind(bool, "workspaces_span_monitors", &settings->workspaces_span_monitors);
    bind(bool, "resize_at_screen_edges", &settings->resize_at_screen_edges);
    bind(bool, "respect_csd_requests", &settings->respect_csd_requests);
    bind(bool, "desktop_icons", &settings->desktop_icons);
    bind(int, "desktop_vertical_override", &settings->desktop_vertical_override);
    bind(std::string, "desktop_sort_by", &settings->desktop_sort_by);
    bind(bool, "desktop_sort_ascending", &settings->desktop_sort_ascending);
    bind(std::string, "desktop_folder", &settings->desktop_folder);
    bind(std::string, "overview_layout_type", &settings->overview_layout_type);
    bind(std::string, "font", &settings->font);
    bind(bool, "font_suggestions_in_own_font", &settings->font_suggestions_in_own_font);
    bind(bool, "applications_show_other_desktops", &settings->applications_show_other_desktops);
    bind(bool, "is_tiling", &settings->is_tiling);
    bind(bool, "tile_all_workspaces", &settings->tile_all_workspaces);
    bind(bool, "new_workspace_is_tiling", &settings->new_workspace_is_tiling);
    bind(bool, "active_window_border_hint", &settings->active_window_border_hint);
    bind(int, "tiling_window_gap", &settings->tiling_window_gap);
    settings->tiling_window_gap = std::clamp(settings->tiling_window_gap, 0, 32);
    bind(std::string, "tiling_layout", &settings->tiling_layout);
    bind(int, "nightlight_temperature", &settings->nightlight_temperature);
    settings->nightlight_temperature = std::clamp(settings->nightlight_temperature, 2500, 6500);
    if (settings->tiling_layout != "Scrolling")
        settings->tiling_layout = "Master";

    bind(bool, "blur_enabled", &settings->blur_enabled);
    bind(std::string, "blur_variant", &settings->blur_variant);
    {
        static const std::array<const char *, 11> valid_variants = {
            "kawase", "frost", "ripple", "drops", "water",
            "fluid_jar", "prism", "heat_shimmer", "acrylic", "aurora", "haze"
        };
        if (std::ranges::find(valid_variants, settings->blur_variant) == valid_variants.end())
            settings->blur_variant = "kawase";
    }
    bind(int, "blur_size", &settings->blur_size);
    settings->blur_size = std::clamp(settings->blur_size, 0, 100);
    bind(int, "blur_passes", &settings->blur_passes);
    settings->blur_passes = std::clamp(settings->blur_passes, 0, 10);
    bind(float, "blur_noise", &settings->blur_noise);
    settings->blur_noise = std::clamp(settings->blur_noise, 0.0f, 1.0f);
    bind(float, "blur_contrast", &settings->blur_contrast);
    settings->blur_contrast = std::clamp(settings->blur_contrast, 0.0f, 2.0f);
    bind(float, "blur_brightness", &settings->blur_brightness);
    settings->blur_brightness = std::clamp(settings->blur_brightness, 0.0f, 2.0f);
    bind(float, "blur_vibrancy", &settings->blur_vibrancy);
    settings->blur_vibrancy = std::clamp(settings->blur_vibrancy, 0.0f, 1.0f);
    bind(float, "blur_vibrancy_darkness", &settings->blur_vibrancy_darkness);
    settings->blur_vibrancy_darkness = std::clamp(settings->blur_vibrancy_darkness, 0.0f, 1.0f);
    bind(bool, "blur_ignore_opacity", &settings->blur_ignore_opacity);
    bind(bool, "blur_xray", &settings->blur_xray);
    bind(bool, "blur_popups", &settings->blur_popups);
    bind(bool, "blur_special", &settings->blur_special);

    if (save) {
        for (const auto& workspace : settings->workspace_tiling)
            out << "workspace_tiling = " << std::quoted(workspace.workspace) << " " << workspace.is_tiling << "\n";
    } else {
        settings->workspace_tiling.clear();
        for (const auto& line : lines) {
            std::istringstream entry(line);
            std::string key;
            char separator = '\0';
            SWorkspaceTiling workspace;
            int state = 0;
            if (!(entry >> key >> separator) || key != "workspace_tiling" || separator != '=')
                continue;
            if (!(entry >> std::quoted(workspace.workspace) >> state) || workspace.workspace.empty() || (state != 0 && state != 1))
                continue;
            entry >> std::ws;
            if (!entry.eof())
                continue;
            workspace.is_tiling = state == 1;
            auto existing = std::ranges::find(settings->workspace_tiling, workspace.workspace, &SWorkspaceTiling::workspace);
            if (existing != settings->workspace_tiling.end())
                *existing = workspace;
            else
                settings->workspace_tiling.push_back(workspace);
        }
        // Version 1 used a global override instead of storing effective modes.
        // Preserve those modes while allowing new workspaces their own default.
        if (file_version < 2 && settings->tile_all_workspaces)
            for (auto& workspace : settings->workspace_tiling)
                workspace.is_tiling = settings->is_tiling;
    }

    if (save) {
        for (const auto& shortcut : settings->shortcuts)
            out << "shortcut = " << std::quoted(shortcut.id) << " " << std::quoted(shortcut.keys) << "\n";
    } else {
        settings->shortcuts.clear();
        for (const auto& line : lines) {
            std::istringstream entry(line);
            std::string key;
            char separator = '\0';
            SShortcutOverride shortcut;
            if (!(entry >> key >> separator) || key != "shortcut" || separator != '=')
                continue;
            if (!(entry >> std::quoted(shortcut.id) >> std::quoted(shortcut.keys)) || shortcut.id.empty())
                continue;
            auto existing = std::ranges::find(settings->shortcuts, shortcut.id, &SShortcutOverride::id);
            if (existing != settings->shortcuts.end())
                *existing = shortcut;
            else
                settings->shortcuts.push_back(shortcut);
        }
    }

    if (save) {
        for (const auto& shortcut : settings->custom_shortcuts)
            out << "custom_shortcut = " << std::quoted(shortcut.name) << " " << std::quoted(shortcut.command) << " "
                << std::quoted(shortcut.keys) << "\n";
    } else {
        settings->custom_shortcuts.clear();
        for (const auto& line : lines) {
            std::istringstream entry(line);
            std::string key;
            char separator = '\0';
            SCustomShortcut shortcut;
            if (!(entry >> key >> separator) || key != "custom_shortcut" || separator != '=')
                continue;
            if (!(entry >> std::quoted(shortcut.name) >> std::quoted(shortcut.command) >> std::quoted(shortcut.keys)))
                continue;
            if (shortcut.command.empty())
                continue;
            settings->custom_shortcuts.push_back(shortcut);
        }
    }

    if (save) {
        for (const auto& entry : settings->autostart)
            out << "autostart = " << std::quoted(entry.when) << " " << std::quoted(entry.command) << "\n";
    } else {
        settings->autostart.clear();
        for (const auto& line : lines) {
            std::istringstream entry(line);
            std::string key;
            char separator = '\0';
            SAutostart autostart;
            if (!(entry >> key >> separator) || key != "autostart" || separator != '=')
                continue;
            if (!(entry >> std::quoted(autostart.when) >> std::quoted(autostart.command)) || autostart.command.empty())
                continue;
            if (autostart.when != "Session start")
                autostart.when = "Plugin start";
            settings->autostart.push_back(autostart);
        }
    }

    if (save) {
        for (const auto& layout : settings->keyboard_layouts)
            out << "keyboard_layout = " << std::quoted(layout.layout) << " " << std::quoted(layout.variant) << "\n";
    } else {
        settings->keyboard_layouts.clear();
        for (const auto& line : lines) {
            std::istringstream entry(line);
            std::string key;
            char separator = '\0';
            SKeyboardLayout layout;
            if (!(entry >> key >> separator) || key != "keyboard_layout" || separator != '=')
                continue;
            if (!(entry >> std::quoted(layout.layout) >> std::quoted(layout.variant)) || layout.layout.empty())
                continue;
            settings->keyboard_layouts.push_back(layout);
        }
    }

    if (save) {
        // An unconfirmed display trial must never reach the disk.
        std::lock_guard lock(display_trial.mutex);
        const auto&     monitors = display_trial.active && settings == set ? display_trial.confirmed : settings->monitors;
        for (const auto& monitor : monitors) {
            if (monitor.output.empty())
                continue;
            out << "monitor = " << std::quoted(monitor.output) << " " << monitor.disabled << " " << std::quoted(monitor.mode) << " " << std::quoted(monitor.scale) << " "
                << monitor.transform << " " << std::quoted(monitor.position) << " " << std::quoted(monitor.mirror) << " " << monitor.bitdepth << " " << std::quoted(monitor.cm)
                << " " << std::quoted(monitor.sdr_eotf) << " " << monitor.vrr << " " << std::quoted(monitor.icc) << " " << monitor.reserved_area << " "
                << monitor.supports_wide_color << " " << monitor.supports_hdr << " " << monitor.sdrbrightness << " " << monitor.sdrsaturation << " " << monitor.sdr_min_luminance
                << " " << monitor.sdr_max_luminance << " " << monitor.min_luminance << " " << monitor.max_luminance << " " << monitor.max_avg_luminance << "\n";
        }
    } else {
        settings->monitors.clear();
        for (const auto& line : lines) {
            std::istringstream entry(line);
            std::string        key;
            char               separator = '\0';
            MylarMonitor       monitor;
            int                disabled = 0;
            if (!(entry >> key >> separator) || key != "monitor" || separator != '=')
                continue;
            if (!(entry >> std::quoted(monitor.output) >> disabled >> std::quoted(monitor.mode) >> std::quoted(monitor.scale) >> monitor.transform >>
                  std::quoted(monitor.position) >> std::quoted(monitor.mirror) >> monitor.bitdepth >> std::quoted(monitor.cm) >> std::quoted(monitor.sdr_eotf) >> monitor.vrr >>
                  std::quoted(monitor.icc) >> monitor.reserved_area >> monitor.supports_wide_color >> monitor.supports_hdr >> monitor.sdrbrightness >> monitor.sdrsaturation >>
                  monitor.sdr_min_luminance >> monitor.sdr_max_luminance >> monitor.min_luminance >> monitor.max_luminance >> monitor.max_avg_luminance))
                continue;
            entry >> std::ws;
            if (!entry.eof() || monitor.output.empty() || (disabled != 0 && disabled != 1))
                continue;
            monitor.disabled            = disabled == 1;
            monitor.transform           = std::clamp(monitor.transform, 0, 7);
            monitor.bitdepth            = monitor.bitdepth == 10 ? 10 : 8;
            monitor.vrr                 = std::clamp(monitor.vrr, -1, 3);
            monitor.reserved_area       = std::max(monitor.reserved_area, 0);
            monitor.supports_wide_color = std::clamp(monitor.supports_wide_color, -1, 1);
            monitor.supports_hdr        = std::clamp(monitor.supports_hdr, -1, 1);
            auto existing               = std::ranges::find(settings->monitors, monitor.output, &MylarMonitor::output);
            if (existing != settings->monitors.end())
                *existing = monitor;
            else
                settings->monitors.push_back(monitor);
        }
    }

#undef bind
}

// Pango re-lays out the shared layout on every extents query, which is slow when many
// rows measure their labels each frame. Extents only depend on the text and layout
// parameters, so remember them; the GL context already caches the rasterized text.
static std::unordered_map<std::string, Bounds> text_extents_cache;

static Bounds draw_text(drawing::Context *cr, int x, int y, std::string text, int size, bool draw, std::string font, int wrap, int h, RGBA color, bool bold, int align = 0) {
    auto layout = get_cached_pango_font(cr, font, size, bold ? PANGO_WEIGHT_BOLD : PANGO_WEIGHT_NORMAL, false);

    std::string key = fz("{}\x1f{}\x1f{}\x1f{}\x1f{}\x1f{}\x1f", font, size, (int) bold, wrap, h, align);
    key += text;
    auto cached = text_extents_cache.find(key);
    if (cached != text_extents_cache.end() && !draw)
        return cached->second;

    //pango_layout_set_text(layout, "\uE7E7", strlen("\uE83F"));
    pango_layout_set_text(layout, text.data(), text.size());
    pango_layout_set_alignment(layout, (PangoAlignment) align);
    if (wrap == -1) {
        pango_layout_set_wrap(layout, PangoWrapMode::PANGO_WRAP_NONE);
        pango_layout_set_width(layout, -1);
        pango_layout_set_height(layout, -1);
        pango_layout_set_ellipsize(layout, PangoEllipsizeMode::PANGO_ELLIPSIZE_NONE);
    } else {
        pango_layout_set_wrap(layout, PangoWrapMode::PANGO_WRAP_WORD_CHAR);
        pango_layout_set_width(layout, wrap * PANGO_SCALE);
        pango_layout_set_height(layout, h);
        if (h != -1)
            pango_layout_set_ellipsize(layout, PangoEllipsizeMode::PANGO_ELLIPSIZE_MIDDLE);
    }
    set_argb(cr, color);
    if (draw) {
        cr->move_to(std::round(x), std::round(y));
        cr->show_layout(layout);
    }
    if (cached != text_extents_cache.end())
        return cached->second;
    PangoRectangle ink;
    PangoRectangle logical;
    pango_layout_get_pixel_extents(layout, &ink, &logical);
    if (text_extents_cache.size() > 4096)
        text_extents_cache.clear();
    Bounds result(ink.width, ink.height, logical.width, logical.height);
    text_extents_cache.emplace(std::move(key), result);
    return result;
}

static void paint_label(Container *root, Container *c, std::string text) {
    auto mylar = (MylarWindow*)root->user_data;
    auto cr = mylar->raw_window->drawing_context;
    auto dpi = mylar->raw_window->dpi;
    auto size = 12 * dpi;

    auto b = draw_text(cr, 0, 0, text, size, false, set->font, -1, 0, theme.text, false);
    draw_text(cr, 
        c->real_bounds.x + 12 * dpi, 
        c->real_bounds.y + c->real_bounds.h * .5 - b.h * .5, text, size, true, set->font, -1, 0, theme.text,  false);
}

static void drawRoundedRect(drawing::Context *cr, double x, double y, double width, double height,
                     double radius, double stroke_width,
                     bool round_top_left = true, bool round_top_right = true,
                     bool round_bottom_right = true, bool round_bottom_left = true) {
    // Ensure the stroke width does not exceed the bounds
    double half_stroke = stroke_width / 2.0;
    double adjusted_radius = std::fmin(radius, std::fmin(width, height) / 2.0);
    double inner_width = width - stroke_width;
    double inner_height = height - stroke_width - 1;
    
    if (inner_width <= 0 || inner_height <= 0) {
        // Cannot draw if the stroke width exceeds or equals the bounds
        return;
    }
    
    // Adjusted bounds to ensure the stroke remains inside
    double adjusted_x = x + half_stroke;
    double adjusted_y = y + half_stroke;
    
    // Begin path for rounded rectangle
    cr->new_path();
    
    cr->move_to(adjusted_x + (round_top_left ? adjusted_radius : 0), adjusted_y);
    
    cr->line_to(adjusted_x + inner_width - (round_top_right ? adjusted_radius : 0), adjusted_y);
    
    if (round_top_right)
        cr->arc(adjusted_x + inner_width - adjusted_radius, adjusted_y + adjusted_radius,
                  adjusted_radius, -M_PI / 2, 0);
    else
        cr->line_to(adjusted_x + inner_width, adjusted_y);
    
    cr->line_to(adjusted_x + inner_width, adjusted_y + inner_height - (round_bottom_right ? adjusted_radius : 0));
    
    if (round_bottom_right)
        cr->arc(adjusted_x + inner_width - adjusted_radius, adjusted_y + inner_height - adjusted_radius,
                  adjusted_radius, 0, M_PI / 2);
    else
        cr->line_to(adjusted_x + inner_width, adjusted_y + inner_height);
    
    cr->line_to(adjusted_x + (round_bottom_left ? adjusted_radius : 0), adjusted_y + inner_height);
    
    if (round_bottom_left)
        cr->arc(adjusted_x + adjusted_radius, adjusted_y + inner_height - adjusted_radius,
                  adjusted_radius, M_PI / 2, M_PI);
    else
        cr->line_to(adjusted_x, adjusted_y + inner_height);
    
    cr->line_to(adjusted_x, adjusted_y + (round_top_left ? adjusted_radius : 0));
    
    if (round_top_left)
        cr->arc(adjusted_x + adjusted_radius, adjusted_y + adjusted_radius,
                  adjusted_radius, M_PI, 3 * M_PI / 2);
    else
        cr->line_to(adjusted_x, adjusted_y);
    
    // Close the path
    cr->close_path();
    
    // Set stroke width and stroke
    cr->set_line_width(stroke_width);
}

/*
 * Paints a filled, bordered rounded rectangle with Cairo's analytic antialiasing.
 * The GL backend only has 4x MSAA, which leaves visibly stepped corners on large
 * radii (popups, drop-down buttons). The frame is rasterized once into a small
 * image surface per (size, radius, colors) and blitted, so repaints stay cheap.
 */
struct SmoothFrameKey {
    int w, h;
    int fx, fy; // sub-pixel offset, 1/8th px
    int radius, border; // 1/8th px
    uint32_t fill, stroke;
    bool operator==(const SmoothFrameKey &) const = default;
};
struct SmoothFrameEntry {
    SmoothFrameKey key;
    cairo_surface_t *surface;
};
// A plain pointer: a thread_local with a destructor would keep the plugin from
// unloading while its thread is alive. Freed by cleanup_smooth_frames().
static thread_local std::vector<SmoothFrameEntry> *smooth_frames = nullptr;

static void cleanup_smooth_frames() {
    if (!smooth_frames)
        return;
    for (auto &entry : *smooth_frames)
        cairo_surface_destroy(entry.surface);
    delete smooth_frames;
    smooth_frames = nullptr;
}

static void paint_smooth_frame(drawing::Context *cr, Bounds b, double radius, RGBA fill, RGBA border, double border_width = 1.0) {
    using FrameKey = SmoothFrameKey;
    if (!smooth_frames)
        smooth_frames = new std::vector<SmoothFrameEntry>;
    auto &cache = *smooth_frames;

    auto pack = [](RGBA c) {
        auto ch = [](double v) { return (uint32_t) std::lround(std::clamp(v, 0.0, 1.0) * 255); };
        return ch(c.r) << 24 | ch(c.g) << 16 | ch(c.b) << 8 | ch(c.a);
    };
    double x0 = std::floor(b.x), y0 = std::floor(b.y);
    int w = (int) std::ceil(b.x + b.w) - (int) x0;
    int h = (int) std::ceil(b.y + b.h) - (int) y0;
    if (w <= 0 || h <= 0)
        return;
    FrameKey key{w, h, (int) std::lround((b.x - x0) * 8), (int) std::lround((b.y - y0) * 8),
                 (int) std::lround(radius * 8), (int) std::lround(border_width * 8), pack(fill), pack(border)};

    cairo_surface_t *surface = nullptr;
    for (auto &entry : cache) {
        if (entry.key == key) {
            surface = entry.surface;
            break;
        }
    }
    if (!surface) {
        if (cache.size() >= 48) {
            cairo_surface_destroy(cache.front().surface);
            cache.erase(cache.begin());
        }
        surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
        auto c = cairo_create(surface);
        cairo_set_antialias(c, CAIRO_ANTIALIAS_BEST);
        double half = border_width / 2.0;
        double x = key.fx / 8.0 + half, y = key.fy / 8.0 + half;
        double rw = b.w - border_width, rh = b.h - border_width;
        double r = std::max(0.0, std::min(radius, std::min(rw, rh) / 2.0));
        cairo_new_sub_path(c);
        cairo_arc(c, x + rw - r, y + r, r, -M_PI / 2, 0);
        cairo_arc(c, x + rw - r, y + rh - r, r, 0, M_PI / 2);
        cairo_arc(c, x + r, y + rh - r, r, M_PI / 2, M_PI);
        cairo_arc(c, x + r, y + r, r, M_PI, 3 * M_PI / 2);
        cairo_close_path(c);
        cairo_set_source_rgba(c, fill.r, fill.g, fill.b, fill.a);
        cairo_fill_preserve(c);
        if (border_width > 0) {
            cairo_set_line_width(c, border_width);
            cairo_set_source_rgba(c, border.r, border.g, border.b, border.a);
            cairo_stroke(c);
        }
        cairo_destroy(c);
        cairo_surface_flush(surface);
        cache.push_back({key, surface});
    }

    cr->save();
    cr->new_path();
    cr->set_source_surface(surface, x0, y0);
    cr->rectangle(x0, y0, w, h);
    cr->fill();
    cr->restore();
}

// Text and glyph helpers shared by the Wi-Fi, Audio pages and drop-down menus.
static RawWindow *swifi_window(Container *root) {
    return ((MylarWindow *) root->user_data)->raw_window;
}

static void swifi_redraw(Container *root) {
    windowing::redraw(swifi_window(root));
}

static RGBA swifi_mix(RGBA a, RGBA b, double t) {
    return RGBA(a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t);
}

static RGBA swifi_dim(RGBA color) {
    color.a *= .5;
    return color;
}

// Like swifi_text, but in the font `family` instead of the UI font.
static void swifi_text_in_family(Container *root, Bounds b, const std::string &text, const std::string &family, double size,
                                 RGBA color, bool centered = false, bool bold = false) {
    auto window = swifi_window(root);
    auto font = get_cached_pango_font(window->drawing_context, family, size * window->dpi,
        bold ? PANGO_WEIGHT_BOLD : PANGO_WEIGHT_NORMAL, false);
    pango_layout_set_text(font, text.c_str(), -1);
    pango_layout_set_width(font, std::max(1.0, b.w) * PANGO_SCALE);
    pango_layout_set_height(font, std::max(1.0, b.h) * PANGO_SCALE);
    pango_layout_set_wrap(font, PANGO_WRAP_WORD_CHAR);
    pango_layout_set_ellipsize(font, PANGO_ELLIPSIZE_END);
    pango_layout_set_alignment(font, centered ? PANGO_ALIGN_CENTER : PANGO_ALIGN_LEFT);
    int width, height;
    pango_layout_get_pixel_size(font, &width, &height);
    window->drawing_context->save();
    window->drawing_context->rectangle(b.x, b.y, b.w, b.h);
    window->drawing_context->clip_path();
    set_argb(window->drawing_context, color);
    window->drawing_context->move_to(b.x, b.y + std::max(0.0, (b.h - height) * .5));
    window->drawing_context->show_layout(font);
    window->drawing_context->restore();
    pango_layout_set_alignment(font, PANGO_ALIGN_LEFT);
}

static void swifi_text(Container *root, Bounds b, const std::string &text, double size, RGBA color,
                       bool centered = false, bool bold = false) {
    swifi_text_in_family(root, b, text, set->font, size, color, centered, bold);
}

// Pages measure their labels on every layout (the Shortcuts page alone has
// hundreds of buttons), so widths are remembered instead of reshaped each time.
static double swifi_text_width(Container *root, const std::string &text, double size) {
    static std::unordered_map<std::string, double> widths;
    auto window = swifi_window(root);
    char numbers[32];
    std::snprintf(numbers, sizeof numbers, "\x1f%g", size * window->dpi);
    std::string key = text + numbers + "\x1f" + set->font;
    if (auto it = widths.find(key); it != widths.end())
        return it->second;
    auto font = get_cached_pango_font(window->drawing_context, set->font, size * window->dpi, PANGO_WEIGHT_NORMAL, false);
    pango_layout_set_width(font, -1);
    pango_layout_set_height(font, -1);
    pango_layout_set_ellipsize(font, PANGO_ELLIPSIZE_NONE);
    pango_layout_set_text(font, text.c_str(), -1);
    int width = 0, height = 0;
    pango_layout_get_pixel_size(font, &width, &height);
    if (widths.size() > 4096)
        widths.clear();
    widths[key] = width;
    return width;
}

static double swifi_text_height(Container *root, const std::string &text, double size, bool bold = false) {
    auto window = swifi_window(root);
    auto font = get_cached_pango_font(window->drawing_context, set->font, size * window->dpi,
        bold ? PANGO_WEIGHT_BOLD : PANGO_WEIGHT_NORMAL, false);
    pango_layout_set_width(font, -1);
    pango_layout_set_height(font, -1);
    pango_layout_set_ellipsize(font, PANGO_ELLIPSIZE_NONE);
    pango_layout_set_text(font, text.c_str(), -1);
    int width = 0, height = 0;
    pango_layout_get_pixel_size(font, &width, &height);
    return height;
}

// A title with a smaller line under it, the pair centered vertically in `b`.
static void swifi_two_lines(Container *root, Bounds b, const std::string &title, const std::string &subtitle,
                            bool bold_title = false) {
    auto dpi = swifi_window(root)->dpi;
    const double title_size = 12, subtitle_size = 10;
    double th = swifi_text_height(root, title, title_size, bold_title);
    double sh = swifi_text_height(root, subtitle, subtitle_size);
    double gap = 3 * dpi;
    double y = b.y + std::max(0.0, (b.h - th - gap - sh) * .5);
    swifi_text(root, Bounds(b.x, y, b.w, th), title, title_size, theme.text, false, bold_title);
    swifi_text(root, Bounds(b.x, y + th + gap, b.w, sh), subtitle, subtitle_size, theme.text_secondary);
}

static void swifi_glyph(drawing::Context *cr, double x, double y, const char *glyph, double size, RGBA color, bool path = false) {
    auto layout = get_cached_pango_font(cr, icon_font, size, PANGO_WEIGHT_NORMAL, false);
    pango_layout_set_width(layout, -1);
    pango_layout_set_height(layout, -1);
    pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_NONE);
    pango_layout_set_alignment(layout, PANGO_ALIGN_LEFT);
    pango_layout_set_text(layout, glyph, -1);
    cr->move_to(x, y);
    if (path) {
        cr->layout_path(layout);
        return;
    }
    set_argb(cr, color);
    cr->show_layout(layout);
}

/*************************************************
 *
 * Keyboard focus
 *
 * Tab and Shift+Tab walk the window's tab stops in tree order, Enter and Space
 * activate the focused stop, arrow keys move inside a control (a slider, a
 * button group, an open dropdown menu) or else to the neighbouring stop, and
 * Escape hides the focus ring. Every container with when_clicked is a tab stop
 * unless focus_skip marks it; focus_register gives a stop its own keys and ring.
 * The ring only shows once the keyboard moved focus; a mouse press hides it.
 * The navigation itself lives with fill_root (focus_key).
 *
 *************************************************/

struct FocusSpec {
    // Enter and Space. Unset: when_clicked.
    std::function<void(Container *root, Container *c)> activate;
    // Arrow keys (dx, dy in -1..1). Returning false lets them move focus instead.
    std::function<bool(Container *root, Container *c, int dx, int dy)> arrow;
    // Where the ring goes. Unset: real_bounds.
    std::function<Bounds(Container *root, Container *c)> ring;
    float radius = -1;  // Logical px, -1 for settings_control_rounding
    bool text = false;  // Text field: Enter, Space and arrows are its own
    bool skip = false;  // Never a tab stop, even though it is clickable
};

// Lives as long as the container: the entry is erased from its on_closed.
static std::unordered_map<Container *, FocusSpec> focus_specs;

static FocusSpec &focus_spec_for(Container *c) {
    auto [it, inserted] = focus_specs.try_emplace(c);
    if (inserted) {
        auto previous = std::move(c->on_closed);
        c->on_closed = [previous](Container *c) {
            focus_specs.erase(c);
            if (previous)
                previous(c);
        };
    }
    return it->second;
}

static const FocusSpec *focus_spec(Container *c) {
    auto it = focus_specs.find(c);
    return it == focus_specs.end() ? nullptr : &it->second;
}

static void focus_register(Container *c, FocusSpec spec) {
    focus_spec_for(c) = std::move(spec);
}

static void focus_skip(Container *c) {
    focus_spec_for(c).skip = true;
}

static void focus_text(Container *c) {
    focus_spec_for(c).text = true;
}

// Each window keeps its own focus: the settings window, and the dialog popup
// opened from it (only one is open at a time).
struct FocusState {
    Container *current = nullptr;
    std::weak_ptr<bool> life;
    // The ring is drawn; only after the keyboard moved focus.
    bool visible = false;
    // Focus placed by the program (the row a search result opened, a dialog's
    // first field): the first Tab shows the ring there instead of moving on.
    bool reveal_first = false;
    // The focused stop went away with the page it replaced (a category link was
    // activated): the next Tab continues on the new page.
    bool into_page = false;
};
static FocusState focus_main;
static FocusState focus_dialog;

static FocusState &focus_of(Container *root) {
    return settings_mylar && root == settings_mylar->root ? focus_main : focus_dialog;
}

// The row a search result opened, focused once it has been laid out.
static Container *focus_pending_row = nullptr;
static std::weak_ptr<bool> focus_pending_row_life;

static void focus_set(Container *root, Container *c, bool show);
static void focus_dialog_start(Container *popup_root, Container *c);
static void focus_install_dialog(MylarWindow *popup);

// Creates a container that sizes itself based on children size
// It takes full width of parent
// It lays out right child first and then left with remainder of space
// Marks rows made by make_self_height_sized_parent, so neighbouring rows can find each other.
static const int settings_option_row_type = 0x5e770;

// Rows placed directly after one another (no make_vert_space between) join into one
// block: they share their border line and only the block's outer corners are rounded.
// Accordion rows are option rows too, they only carry a different type for search.
static bool is_option_row(Container *c) {
    return c->custom_type == settings_option_row_type || c->custom_type == search_accordion_type;
}

static void option_row_neighbours(Container *c, bool *is_first, bool *is_last) {
    *is_first = true;
    *is_last = true;
    auto parent = c->parent;
    if (!parent)
        return;
    auto &siblings = parent->children;
    auto it = std::find(siblings.begin(), siblings.end(), c);
    if (it == siblings.end())
        return;
    for (auto prev = it; prev != siblings.begin();) {
        --prev;
        if (!(*prev)->exists)
            continue;
        *is_first = !is_option_row(*prev);
        break;
    }
    for (auto next = it + 1; next != siblings.end(); ++next) {
        if (!(*next)->exists)
            continue;
        *is_last = !is_option_row(*next);
        break;
    }
}

static Container *make_self_height_sized_parent(Container *parent) {
    auto c = parent->child(::absolute, FILL_SPACE, FILL_SPACE);
    c->custom_type = settings_option_row_type;
    c->receive_events_even_if_obstructed = true;
    // The page container is only viewport tall and moves with the scroll, so rows
    // scrolled into view lie outside it and would otherwise never be hit (no hover,
    // no clicks on the row itself).
    c->parent_bounds_limit_input_bounds = false;
    c->pre_layout = [](Container *root, Container *c, const Bounds &b) {
        auto mylar = (MylarWindow*)root->user_data;
        auto dpi = mylar->raw_window->dpi;

        auto left = c->children[0];
        auto right = c->children[1];

        assert(left->pre_layout && right->pre_layout);

        right->real_bounds = b;
        right->pre_layout(root, right, b);
        auto space = std::max(b.w - right->real_bounds.w, 0.0);
        right->real_bounds.x += space;

        left->real_bounds = b;
        left->real_bounds.w = space - optionrighttpad * dpi;
        left->pre_layout(root, left, left->real_bounds);

        float tallest = right->real_bounds.h;
        if (tallest < left->real_bounds.h)
            tallest = left->real_bounds.h;
        if (tallest < 55 * dpi)
            tallest = 55 * dpi;
        
        auto bcopy = b;
        bcopy.h = tallest;

        layout(root, right, right->real_bounds);
        modify_all(right, -25 * dpi, 0);
        
        modify_all(right, 0, tallest * .5 - right->real_bounds.h * .5);
        modify_all(left, 0, tallest * .5 - left->real_bounds.h * .5);
        
        layout(root, left, left->real_bounds);

        c->real_bounds = bcopy;
    };
    static const float border_rounding = 5;
    c->when_paint = paint {
        auto mylar = (MylarWindow*)root->user_data;
        auto cr = mylar->raw_window->drawing_context;
        auto dpi = mylar->raw_window->dpi;
        auto b = c->real_bounds;

        bool is_first, is_last;
        option_row_neighbours(c, &is_first, &is_last);
        // Joined rows overlap by their border so the shared line is drawn once.
        if (!is_first || !is_last) {
            const auto overlap = (std::floor(1.0 * dpi) + 1.0) * 0.5;
            if (!is_first) {
                b.y -= overlap;
                b.h += overlap;
            }
            if (!is_last)
                b.h += overlap;
        }

        RGBA bg_color = (c->state.mouse_hovering || c->state.mouse_pressing) ? theme.row_hover : theme.row_background;
        drawRoundedRect(cr, b.x, b.y, b.w, b.h, border_rounding * dpi, 0, is_first, is_first, is_last, is_last);
        set_argb(cr, bg_color);
        cr->fill();

        drawRoundedRect(cr, b.x, b.y, b.w, b.h, border_rounding * dpi, 1.0, is_first, is_first, is_last, is_last);
        set_argb(cr, theme.row_border);
        cr->stroke();
    };

    return c;
}

static void make_label_like(Container *parent, std::string title, std::string description, std::string icon = "") {
    search_tag(parent, title, description);
    auto left = parent->child(FILL_SPACE, FILL_SPACE);
    //static float button_text_pad = 8; 
    left->when_paint = [title, description, icon](Container *root, Container *c) {
        auto mylar = (MylarWindow*)root->user_data;
        auto cr = mylar->raw_window->drawing_context;
        auto dpi = mylar->raw_window->dpi;
        auto size_ico = 16 * dpi;
        auto size_title = 12 * dpi;
        auto size_desc = 11 * dpi;

        float yoff = optiontopbottompad * dpi;
        if (!icon.empty()) {
            auto bo = draw_text(cr, 0, 0, icon, size_ico, false, icon_font, -1, -1, theme.text_secondary, false);
            draw_text(cr, c->real_bounds.x + 19 * dpi, center_y(c, bo.h), icon, size_ico, true, icon_font, -1, -1, theme.text, false);
        }
        {
            auto bo = draw_text(cr, 0, 0, title, size_title, false, set->font, c->real_bounds.w - ((optionleftpad + optionrighttpad) * dpi), -1, theme.text_secondary, false);
            auto regular = draw_text(cr, 0, 0, title, size_title, false, set->font, -1, -1, theme.text_secondary, false);
            float desc_yoff = c->real_bounds.h * .5 - bo.h * .5;
            bool not_wrapped = std::abs(regular.h - bo.h) < 4;
            if (!not_wrapped) {
                desc_yoff = yoff;
            }
            if (description.empty()) {
                draw_text(cr,
                    c->real_bounds.x + 60 * dpi, 
                    c->real_bounds.y + desc_yoff, title, size_title, true, set->font, c->real_bounds.w - ((optionleftpad + optionrighttpad) * dpi), -1, theme.text, false);
            } else {
                draw_text(cr,
                    c->real_bounds.x + 60 * dpi, 
                    c->real_bounds.y + yoff, title, size_title, true, set->font, c->real_bounds.w - ((optionleftpad + optionrighttpad) * dpi), -1, theme.text, false);
            }

            yoff += bo.h;
        }
        if (!description.empty()) {
            draw_text(cr,
                c->real_bounds.x + 60 * dpi, 
                c->real_bounds.y + yoff, description, size_desc, true, set->font, c->real_bounds.w - ((optionleftpad + optionrighttpad) * dpi), -1, theme.text_secondary, false);
        }
    };
    left->pre_layout = [title, description, icon](Container *root, Container *c, const Bounds &b) {
        auto mylar = (MylarWindow*)root->user_data;
        auto cr = mylar->raw_window->drawing_context;
        auto dpi = mylar->raw_window->dpi;
        auto size_title = 12 * dpi;
        auto size_desc = 11 * dpi;
 
        auto bo1 = draw_text(cr, 0, 0, title, size_title, false, set->font, b.w - ((optionleftpad + optionrighttpad) * dpi), -1, theme.text, false);
        if (description.empty()) {
            c->real_bounds.h = bo1.h + optiontopbottompad * dpi * 2;
        } else {
            auto bo2 = draw_text(cr, 0, 0, description, size_desc, false, set->font, b.w - ((optionleftpad + optionrighttpad) * dpi), -1, theme.text, false);
            c->real_bounds.h = bo1.h + bo2.h + optiontopbottompad * dpi * 2;
        }

        if (c->real_bounds.h < 70 * dpi)
            c->real_bounds.h = 70 * dpi;
    };
}

static Container *make_section_title(Container *parent, std::string title) {
    auto section_title = parent->child(FILL_SPACE, FILL_SPACE);
    search_current_section = trim(title);
    search_tag(section_title, search_current_section, "");
    static float size = 13;
    section_title->pre_layout = [title](Container *root, Container *c, const Bounds &b) {
        auto mylar = (MylarWindow*)root->user_data;
        auto cr = mylar->raw_window->drawing_context;
        auto dpi = mylar->raw_window->dpi;
        auto size_title = size * dpi;
        auto bo = draw_text(cr, 0, 0, title, size_title, false, set->font, b.w, -1, theme.text, false);
        c->wanted_bounds.h = bo.h;
        c->real_bounds.h = bo.h;
    };
    section_title->when_paint = [title](Container *root, Container *c) {
        auto mylar = (MylarWindow*)root->user_data;
        auto cr = mylar->raw_window->drawing_context;
        auto dpi = mylar->raw_window->dpi;
        auto size_title = size * dpi;
        draw_text(cr,
            c->real_bounds.x, 
            c->real_bounds.y, title, size_title, true, set->font, c->real_bounds.w, -1, theme.text, false);
    };
    return section_title;
}

// Animated on/off switch shared by every boolean in Settings.
// Mirrors winbar's setting_bool: the knob grows while its row is hovered, stretches while the
// switch itself is hovered or pressed, slides between the ends, and can be dragged across.
// Settings runs its own event loop, so the animation is a tween advanced while painting
// (redrawing until it settles) rather than the compositor-thread animate().
struct ToggleTween {
    float value = 0;
    float from = 0;
    float target = 0;
    long start = 0;
    float duration = 0;

    // Moves toward `new_target` (linearly over `duration_ms`); returns true while still moving.
    bool step(float new_target, float duration_ms, long now) {
        if (new_target != target) {
            from = value;
            target = new_target;
            start = now;
            duration = duration_ms;
        }
        if (value == target)
            return false;
        float t = duration <= 0 ? 1.0f : std::clamp((float) (now - start) / duration, 0.0f, 1.0f);
        value = t >= 1 ? target : from + (target - from) * t;
        return value != target;
    }

    // Continues from `v` (used when a dragged knob is let go).
    void jump(float v) {
        value = from = v;
        target = -1;
    }
};

struct ToggleAnim {
    ToggleTween slide;         // 0 off, 1 on
    ToggleTween stretch;       // knob stretch while the switch is hovered/pressed
    ToggleTween grow;          // knob growth while the row is hovered
    bool initialized = false;
    bool dragging = false;
    int drag_start_x = 0;
    float position_scalar = 0; // where the knob is while dragging (0..1)
};

static constexpr float toggle_w = 40;
static constexpr float toggle_h = 20;
static constexpr float toggle_anim_ms = 70;

// Sliding highlight for segmented controls (button groups, the Audio tabs).
// The choice takes effect immediately; only the raised pill glides from the
// old option to the new one, advanced while painting like ToggleAnim.
struct SegmentSlide {
    ToggleTween tween;
    Bounds from_rel;          // where the pill started, relative to the group
    Bounds last_rel;          // where it was last painted, relative to the group
    bool painted = false;
};

static constexpr float segment_slide_ms = 180;

// Call before changing the selection so the pill leaves from where it is now.
static void segment_slide_start(SegmentSlide *s) {
    if (!s->painted)
        return;
    s->from_rel = s->last_rel;
    s->tween.jump(0);
}

// Paints the pill for `selected` (a child of `group`) and keeps redrawing while it moves.
static void segment_slide_paint(Container *root, Container *group, SegmentSlide *s, Container *selected) {
    if (!selected)
        return;
    auto mylar = (MylarWindow *) root->user_data;
    auto cr = mylar->raw_window->drawing_context;
    auto dpi = mylar->raw_window->dpi;
    auto g = group->real_bounds;
    auto t_abs = selected->real_bounds;
    Bounds target(t_abs.x - g.x, t_abs.y - g.y, t_abs.w, t_abs.h);

    if (!s->painted) {
        s->tween.value = s->tween.from = s->tween.target = 1;
        s->from_rel = target;
    }
    if (s->tween.step(1, segment_slide_ms, get_current_time_in_ms()))
        windowing::redraw(mylar->raw_window);
    float t = s->tween.value;
    float e = 1 - (1 - t) * (1 - t) * (1 - t); // ease out
    Bounds r(s->from_rel.x + (target.x - s->from_rel.x) * e,
             s->from_rel.y + (target.y - s->from_rel.y) * e,
             s->from_rel.w + (target.w - s->from_rel.w) * e,
             s->from_rel.h + (target.h - s->from_rel.h) * e);
    s->last_rel = r;
    s->painted = true;

    set_argb(cr, theme.segment_selected);
    drawRoundedRect(cr, g.x + r.x, g.y + r.y, r.w, r.h, 8 * dpi, 1.0);
    cr->fill();
}


static void paint_toggle(Container *root, Container *c, ToggleAnim *t, Bounds check, bool on, bool row_hovered, bool pressing, bool enabled = true) {
    auto mylar = (MylarWindow *) root->user_data;
    auto cr = mylar->raw_window->drawing_context;
    auto dpi = mylar->raw_window->dpi;

    if (!t->initialized) {
        t->initialized = true;
        t->slide.value = t->slide.from = t->slide.target = on ? 1 : 0;
    }

    const float circ_size_small = 12;
    const float circ_size_big = 14;

    long now = get_current_time_in_ms();
    bool sliding = t->slide.value != (on ? 1.0f : 0.0f);
    bool over_switch = row_hovered && bounds_contains(check, root->mouse_current_x, root->mouse_current_y);
    bool animating = false;
    animating |= t->stretch.step(enabled && (over_switch || pressing || sliding || t->dragging) ? 1 : 0, toggle_anim_ms, now);
    animating |= t->grow.step(enabled && (row_hovered || t->dragging) ? 1 : 0, toggle_anim_ms, now);
    if (!t->dragging)
        animating |= t->slide.step(on ? 1 : 0, toggle_anim_ms * .5, now);
    if (animating)
        windowing::redraw(mylar->raw_window);
    float slide_open = t->stretch.value;
    float open_amount = t->grow.value;
    float slide_amount = t->slide.value;

    float circ_extra_width = 5.5 * .75 * dpi * slide_open; // 25% narrower stretch than winbar
    float circ_size = circ_size_small + (circ_size_big - circ_size_small) * open_amount;
    float left = check.x + 4 * dpi - ((circ_size - circ_size_small) * .5) * dpi;
    float right = check.x + check.w - circ_size * dpi + ((circ_size - circ_size_small) * .5) * dpi - 4 * dpi - circ_extra_width * open_amount;
    float position = (right - left) * slide_amount + left;
    if (t->dragging) {
        position += root->mouse_current_x - t->drag_start_x;
        position = std::clamp(position, left, right);
        t->position_scalar = right > left ? (position - left) / (right - left) : 0;
    }

    float alpha = enabled ? 1.0 : .45;
    if (on) {
        RGBA track = theme.accent;
        track.a *= alpha;
        drawRoundedRect(cr, check.x, check.y, check.w, check.h, 10 * dpi, 1.0);
        set_argb(cr, track);
        cr->fill();
    } else {
        RGBA border = theme.toggle_border;
        border.a *= alpha;
        drawRoundedRect(cr, check.x, check.y, check.w, check.h, 10 * dpi, std::floor(1.0 * dpi));
        set_argb(cr, border);
        cr->stroke();
    }

    Bounds knob;
    knob.x = position;
    knob.y = check.y + check.h / 2 - (circ_size * .5) * dpi;
    knob.w = circ_size * dpi + circ_extra_width * open_amount;
    knob.h = circ_size * dpi;
    RGBA knob_color = on ? theme.toggle_knob_on : theme.toggle_knob_off;
    knob_color.a *= enabled ? 1.0 : .6;
    // A true pill: drawRoundedRect trims a pixel off the height but keeps the
    // full radius, which made the knob look lopsided and pointy.
    double r = knob.h * .5;
    cr->new_path();
    cr->arc(knob.x + knob.w - r, knob.y + r, r, -M_PI / 2, M_PI / 2);
    cr->arc(knob.x + r, knob.y + r, r, M_PI / 2, 3 * M_PI / 2);
    cr->close_path();
    set_argb(cr, knob_color);
    cr->fill();
}

// With row_toggles, clicking anywhere on the row flips the switch (for rows that hold
// nothing but the bool), and pressing the row stretches the knob like pressing the switch.
static void make_bool_control(Container *parent, bool initial_value, std::function<void(bool)> on_change, Container *row = nullptr, bool row_toggles = false) {
    struct BoolInfo : UserData {
        bool on = false;
        Container *row = nullptr;
        bool row_toggles = false;
        ToggleAnim anim;
    };

    auto right = parent->child(::hbox, FILL_SPACE, FILL_SPACE);
    auto bool_info = new BoolInfo;
    bool_info->on = initial_value;
    bool_info->row = row ? row : parent;
    bool_info->row_toggles = row_toggles;
    right->user_data = bool_info;
    focus_register(right, {.radius = toggle_h * .5f});
    right->when_paint = paint {
        auto mylar = (MylarWindow*)root->user_data;
        auto cr = mylar->raw_window->drawing_context;
        auto dpi = mylar->raw_window->dpi;
        auto data = (BoolInfo *) c->user_data;

        bool row_hovered = c->state.mouse_hovering || data->row->state.mouse_hovering;
        bool pressing = c->state.mouse_pressing || (data->row_toggles && data->row->state.mouse_pressing);
        paint_toggle(root, c, &data->anim, c->real_bounds, data->on, row_hovered, pressing);

        // On/Off text
        std::string text = data->on ? "On" : "Off";
        auto tb = draw_text(cr, 0, 0, text, 11 * dpi, false, set->font, -1, -1, theme.text, false, 0);
        draw_text(cr, 
            c->real_bounds.x - tb.w - 12 * dpi, 
            c->real_bounds.y + c->real_bounds.h * .5 - tb.h * .5, text, 11 * dpi, 
            true, set->font, -1, -1, theme.text, false, 0);
    };
    right->pre_layout = [](Container *root, Container *c, const Bounds &b) {
        auto mylar = (MylarWindow*)root->user_data;
        auto dpi = mylar->raw_window->dpi;
        c->wanted_bounds.w = c->real_bounds.w = toggle_w * dpi;
        c->wanted_bounds.h = c->real_bounds.h = toggle_h * dpi;
        c->minimum_x_distance_to_move_before_drag_begins = 4 * dpi;
        c->minimum_y_distance_to_move_before_drag_begins = 4 * dpi;
    };
    right->when_clicked = [on_change](Container *root, Container *c) {
        auto data = (BoolInfo *) c->user_data;
        data->on = !data->on;
        if (on_change)
            on_change(data->on);
    };
    // The knob can be dragged from one side to the other, like winbar.
    right->when_drag_end_is_click = false;
    right->when_drag_start = [](Container *root, Container *c) {
        auto data = (BoolInfo *) c->user_data;
        data->anim.dragging = true;
        data->anim.drag_start_x = root->mouse_initial_x;
        data->anim.position_scalar = data->anim.slide.value;
    };
    right->when_drag = [](Container *root, Container *c) {
        windowing::redraw(((MylarWindow *) root->user_data)->raw_window);
    };
    right->when_drag_end = [on_change](Container *root, Container *c) {
        auto data = (BoolInfo *) c->user_data;
        if (!data->anim.dragging)
            return;
        data->anim.dragging = false;
        bool on = data->anim.position_scalar > .5;
        data->anim.slide.jump(data->anim.position_scalar);
        if (on != data->on) {
            data->on = on;
            if (on_change)
                on_change(on);
        }
        windowing::redraw(((MylarWindow *) root->user_data)->raw_window);
    };

    if (row_toggles) {
        auto row_c = bool_info->row;
        // The row gets presses even over the switch; those are the switch's own.
        row_c->when_clicked = [right, on_change](Container *root, Container *c) {
            if (bounds_contains(right->real_bounds, root->mouse_initial_x, root->mouse_initial_y))
                return;
            auto data = (BoolInfo *) right->user_data;
            data->on = !data->on;
            if (on_change)
                on_change(data->on);
            windowing::redraw(((MylarWindow *) root->user_data)->raw_window);
        };
    }
}

static void make_bool(Container *parent, std::string title, std::string description, bool initial_value, std::function<void(bool)> on_change, std::string icon = "") {
    auto p = make_self_height_sized_parent(parent);
    make_label_like(p, title, description, icon);
    make_bool_control(p, initial_value, on_change, nullptr, true);
}

static void make_button(Container *parent, std::string text, std::function<void()> func, std::function<bool()> is_enabled);

// A negative reset_value means no Reset button.
static void make_slider(Container *parent, std::string title, std::string description, float initial_value, std::function<void(float)> on_change, int notches = 0, std::string icon = "", float reset_value = -1) {
    auto p = make_self_height_sized_parent(parent);
    
    make_label_like(p, title, description, icon);

    struct SliderInfo : UserData {
        float value = .5;
    };

    Container *right = nullptr;
    Container *row = nullptr;
    // The slider's width comes from the whole option row. The hbox holding the
    // slider and its Reset button runs the slider's pre_layout again with the
    // (narrower) hbox bounds, so the width is decided once per row layout and
    // remembered; recomputing it there shrank the slider and left a gap.
    auto row_slider_w = std::make_shared<float>(0);
    if (reset_value >= 0) {
        // Slider and Reset button side by side.
        row = p->child(::hbox, FILL_SPACE, FILL_SPACE);
        row->alignment = container_alignment::ALIGN_CENTER;
        row->pre_layout = [row_slider_w](Container *root, Container *c, const Bounds &b) {
            auto mylar = (MylarWindow*)root->user_data;
            auto dpi = mylar->raw_window->dpi;
            c->spacing = 24 * dpi;
            *row_slider_w = 0;
            float w = 0;
            for (auto child : c->children) {
                if (child->pre_layout)
                    child->pre_layout(root, child, b);
                w += child->wanted_bounds.w;
            }
            c->real_bounds.w = w + c->spacing * (c->children.size() - 1);
            c->real_bounds.h = 40 * dpi;
        };
        right = row->child(FILL_SPACE, FILL_SPACE);
    } else {
        right = p->child(::hbox, FILL_SPACE, FILL_SPACE);
    }
    auto slider_info = new SliderInfo;
    if (notches != 0) {
        float snap_percentage = 1.0f / ((float) notches);
        initial_value = std::round(initial_value / snap_percentage) * snap_percentage;
        initial_value = std::clamp(initial_value, 0.0f, 1.0f);
    }
    slider_info->value = initial_value;
    right->user_data = slider_info;
    right->when_paint = [notches](Container *root, Container *c) {
        auto mylar = (MylarWindow*)root->user_data;
        auto cr = mylar->raw_window->drawing_context;
        auto dpi = mylar->raw_window->dpi;

        {
            auto b = c->real_bounds;
            float h = 8.5 * dpi;
            b.y += b.h * .5 - h * .5;
            b.h = h;

            drawRoundedRect(cr, b.x - std::round(5 * dpi), b.y, b.w + std::round(10 * dpi), b.h, h * .5, 1.0);
            set_argb(cr, theme.slider_track);
            cr->fill(); 

            if (notches != 0) {
                float snap_percentage = 1.0f / ((float) notches);
                int count = std::round(1.0f / snap_percentage);
                float spacing = b.w * snap_percentage;
                float x_off = 0.0;
                for (int i = 0; i < count + 1; i++) {
                    set_rect(cr, {b.x + x_off - std::round(1 * dpi), b.y + std::round(10 * dpi), std::round(2 * dpi), std::round(6 * dpi)});
                    set_argb(cr, theme.slider_track);
                    cr->fill(); 

                    x_off += spacing;
                }
            }
        }
        
        {
            auto data = (SliderInfo *) c->user_data;
            auto b = c->real_bounds;
            b.w = b.h;
            b.x += c->real_bounds.w * data->value - b.h * .5;
            drawRoundedRect(cr, b.x, b.y, b.w, b.h, b.h * .5, 1.0);
            set_argb(cr, theme.slider_thumb);
            cr->fill();

            //b.shrink(1.0);
            
            drawRoundedRect(cr, b.x, b.y, b.w, b.h, b.h * .5, 1.0);
            set_argb(cr, theme.slider_track);
            cr->stroke();

            b.shrink(5 * dpi);
            drawRoundedRect(cr, b.x, b.y, b.w, b.h, b.h * .5, 1.0);
            set_argb(cr, theme.accent);
            cr->fill();
        }

    };
    right->when_mouse_down = [on_change, notches](Container *root, Container *c) {
        auto b = c->real_bounds;
        float scalar = (root->mouse_current_x - b.x) / b.w;
        if (scalar < 0)
            scalar = 0;
        if (scalar > 1)
            scalar = 1;
        if (notches != 0) {
            float snap_percentage = 1.0f / ((float) notches);
            scalar = std::round(scalar / snap_percentage) * snap_percentage;
            scalar = std::clamp(scalar, 0.0f, 1.0f);
        }
 
        ((SliderInfo *) c->user_data)->value = scalar;
        if (on_change)
            on_change(scalar);
    };
    right->when_mouse_up = right->when_mouse_down;
    right->when_drag = right->when_mouse_down;
    right->when_drag_end = right->when_mouse_down;
    // Left and Right step the value: one notch, or 5% without notches.
    focus_register(right, {
        .activate = [](Container *, Container *) {},
        .arrow = [on_change, notches](Container *, Container *c, int dx, int) {
            if (dx == 0)
                return false;
            auto data = (SliderInfo *) c->user_data;
            float step = notches != 0 ? 1.0f / (float) notches : .05f;
            float value = std::clamp(std::round(data->value / step + dx) * step, 0.0f, 1.0f);
            if (value != data->value) {
                data->value = value;
                if (on_change)
                    on_change(value);
            }
            return true;
        },
        // The thumb overhangs the track by half its width on both ends.
        .ring = [](Container *, Container *c) {
            auto b = c->real_bounds;
            return Bounds(b.x - b.h * .5, b.y, b.w + b.h, b.h);
        },
        .radius = 10,
    });
    
    bool has_reset = row != nullptr;
    right->pre_layout = [has_reset, row_slider_w](Container *root, Container *c, const Bounds &b) {
        auto mylar = (MylarWindow*)root->user_data;
        auto dpi = mylar->raw_window->dpi;
        // `b` is the whole row. Narrow windows shrink the slider so the label
        // (and the Reset button) keep their room.
        double room = (has_reset ? 360 : 260) * dpi;
        if (!has_reset || *row_slider_w <= 0)
            *row_slider_w = std::clamp(b.w - room, 80.0 * dpi, 350.0 * dpi);
        c->real_bounds.w = *row_slider_w;
        c->real_bounds.h = 20 * dpi;
        c->wanted_bounds.w = c->real_bounds.w;
        c->wanted_bounds.h = c->real_bounds.h;
    };

    if (row) {
        make_button(row, "Reset", [slider_info, reset_value, on_change, notches]() {
            float value = reset_value;
            if (notches != 0) {
                float snap_percentage = 1.0f / ((float) notches);
                value = std::round(value / snap_percentage) * snap_percentage;
            }
            value = std::clamp(value, 0.0f, 1.0f);
            slider_info->value = value;
            if (on_change)
                on_change(value);
        }, {});
    }
}

static void make_button_group(Container *parent, std::string title, std::string description, std::vector<std::string> options, std::function<void(std::string)> on_selected, std::string default_value, std::string icon = "") {
    auto p = make_self_height_sized_parent(parent);
    
    make_label_like(p, title, description, icon);
    
    // Drawn like the Audio page's tabs: a tray with the selected option raised and the others dimmed.
    static const float option_text_size = 11;
    struct GroupData : UserData {
        SegmentSlide slide;
        int selected = -1;
    };
    auto right = p->child(::hbox, FILL_SPACE, FILL_SPACE);
    auto group_data = new GroupData;
    for (int i = 0; i < (int) options.size(); i++)
        if (options[i] == default_value)
            group_data->selected = i;
    right->user_data = group_data;
    // One tab stop for the whole group; Left and Right pick the neighbouring option.
    focus_register(right, {
        .activate = [](Container *, Container *) {},
        .arrow = [](Container *root, Container *c, int dx, int) {
            if (dx == 0)
                return false;
            auto data = (GroupData *) c->user_data;
            int count = (int) c->children.size();
            if (count == 0)
                return true;
            int next = data->selected < 0 ? (dx > 0 ? 0 : count - 1) : std::clamp(data->selected + dx, 0, count - 1);
            auto option = c->children[next];
            if (next != data->selected && option->when_clicked)
                option->when_clicked(root, option);
            return true;
        },
        .radius = 10,
    });
    right->when_paint = paint {
        auto mylar = (MylarWindow*)root->user_data;
        auto cr = mylar->raw_window->drawing_context;
        auto dpi = mylar->raw_window->dpi;
        auto b = c->real_bounds;
        set_argb(cr, theme.segment_background);
        drawRoundedRect(cr, b.x, b.y, b.w, b.h, 10 * dpi, 1.0);
        cr->fill();
        auto data = (GroupData *) c->user_data;
        if (data->selected >= 0 && data->selected < (int) c->children.size())
            segment_slide_paint(root, c, &data->slide, c->children[data->selected]);
    };
    right->pre_layout = [options](Container *root, Container *c, const Bounds &b) {
        auto mylar = (MylarWindow*)root->user_data;
        auto dpi = mylar->raw_window->dpi;

        c->wanted_pad = Bounds(4 * dpi, 4 * dpi, 4 * dpi, 4 * dpi);
        c->spacing = 4 * dpi;

        float w = c->wanted_pad.x + c->wanted_pad.w + c->spacing * (options.size() - 1);
        for (int i = 0; i < (int) options.size() && i < (int) c->children.size(); i++) {
            float ow = swifi_text_width(root, options[i], option_text_size) + 28 * dpi;
            c->children[i]->wanted_bounds.w = ow;
            w += ow;
        }

        c->real_bounds.w = w;
        c->real_bounds.h = 40 * dpi;
    };
    for (int i = 0; i < (int) options.size(); i++) {
        auto o = options[i];
        auto option = right->child(FILL_SPACE, FILL_SPACE);
        focus_skip(option);
        option->when_paint = [o, i](Container *root, Container *c) {
            auto group = (GroupData *) c->parent->user_data;
            auto mylar = (MylarWindow*)root->user_data;
            auto cr = mylar->raw_window->drawing_context;
            auto dpi = mylar->raw_window->dpi;
            auto b = c->real_bounds;
            bool selected = group->selected == i;
            // The selected pill is painted (and slid) by the group.
            if (!selected && c->state.mouse_hovering) {
                set_argb(cr, swifi_mix(theme.segment_background, theme.segment_selected, .5));
                drawRoundedRect(cr, b.x, b.y, b.w, b.h, 8 * dpi, 1.0);
                cr->fill();
            }
            swifi_text(root, b, o, option_text_size, selected ? theme.text : theme.text_secondary, true);
        };
        option->when_clicked = [o, i, on_selected](Container *root, Container *c) {
            auto group = (GroupData *) c->parent->user_data;
            if (group->selected == i)
                return;
            segment_slide_start(&group->slide);
            group->selected = i;
            if (on_selected)
                on_selected(o);
        };
    }

    // Clicking elsewhere on the row moves to the next option, wrapping around. The row
    // also gets presses on the group itself; those belong to the options.
    p->when_clicked = [right](Container *root, Container *c) {
        if (bounds_contains(right->real_bounds, root->mouse_initial_x, root->mouse_initial_y))
            return;
        auto data = (GroupData *) right->user_data;
        int count = (int) right->children.size();
        if (count == 0)
            return;
        int next = data->selected < 0 ? 0 : (data->selected + 1) % count;
        auto option = right->children[next];
        if (next != data->selected && option->when_clicked)
            option->when_clicked(root, option);
        windowing::redraw(((MylarWindow *) root->user_data)->raw_window);
    };
}

static Container *make_vert_space(Container *parent, float amount) {
    auto pad = parent->child(FILL_SPACE, 8);
    pad->pre_layout = [amount](Container *root, Container *c, const Bounds &b) {
        auto mylar = (MylarWindow*)root->user_data;
        auto dpi = mylar->raw_window->dpi;
        c->wanted_bounds.h = amount * dpi;
    };
    return pad;
}

static RawWindowSettings make_icon_anchored_popup_settings(Container *icon,
                                                           float dpi,
                                                           int popup_w,
                                                           int popup_h) {
    const int icon_x = (int) std::round(icon->real_bounds.x / dpi);
    const int icon_y = (int) std::round(icon->real_bounds.y / dpi);
    const int icon_w = std::max(1, (int) std::round(icon->real_bounds.w / dpi));
    const int icon_h = std::max(1, (int) std::round(icon->real_bounds.h / dpi));

    RawWindowSettings settings;
    settings.pos.w = popup_w;
    settings.pos.h = popup_h;
    settings.name = "Popup";
    settings.popup.use_explicit_anchor_rect = true;
    settings.popup.anchor_rect_x = icon_x;
    settings.popup.anchor_rect_y = icon_y;
    settings.popup.anchor_rect_w = icon_w;
    settings.popup.anchor_rect_h = icon_h;
    settings.popup.anchor = RawWindowSettings::PopupAnchor::BOTTOM;
    settings.popup.gravity = RawWindowSettings::PopupGravity::BOTTOM;
    settings.popup.use_offset = true;
    settings.popup.offset_y = -8;
    settings.popup.constraint_adjustment =
        RawWindowSettings::POPUP_CONSTRAINT_SLIDE_X |
        RawWindowSettings::POPUP_CONSTRAINT_SLIDE_Y |
        RawWindowSettings::POPUP_CONSTRAINT_FLIP_X |
        RawWindowSettings::POPUP_CONSTRAINT_FLIP_Y;
    return settings;
}

/*
 * Drop-down menus: a button showing the current choice and a popup listing the options.
 * Long lists scroll; the current choice is marked with an accent bar and scrolled into view.
 */

struct DropdownOption {
    std::string id;
    std::string label;
};

struct DropdownMenuData : UserData {
    float scroll = 0;
    float overflow = 0;
    int selected_index = -1;
    int key_index = -1; // Row highlighted with the arrow keys, -1 for none
    bool scrolled_to_selection = false;
    SmoothScroll smooth;
};

static MylarWindow *dropdown_popup = nullptr;
// The open menu's list, while dropdown_popup is a menu (it can also be a dialog).
static Container *dropdown_menu_list = nullptr;
static constexpr int dropdown_row_h = 32;
static constexpr int dropdown_max_visible_rows = 12;

static void close_dropdown_menu() {
    if (!dropdown_popup)
        return;
    auto popup = dropdown_popup;
    dropdown_popup = nullptr;
    dropdown_menu_list = nullptr;
    std::erase(popups, popup);
    windowing::timer(settings_app, 1, [](void *data) {
        windowing::close_window((RawWindow *) data);
    }, popup->raw_window);
}

static void open_dropdown_menu(Container *root, Container *anchor, std::vector<DropdownOption> options,
                               std::string selected, std::function<void(std::string)> on_pick) {
    auto mylar = (MylarWindow *) root->user_data;
    if (options.empty() || dropdown_popup || mylar->popup_window)
        return;
    auto dpi = mylar->raw_window->dpi;
    double widest = 0;
    for (auto &option : options)
        widest = std::max(widest, swifi_text_width(root, option.label, 11) / dpi);
    int w = (int) std::clamp(std::max(widest + 40, anchor->real_bounds.w / dpi), 200.0, 460.0);
    int visible = std::min<int>((int) options.size(), dropdown_max_visible_rows);
    int h = visible * dropdown_row_h + 8;
    auto popup = open_mylar_popup(mylar, make_icon_anchored_popup_settings(anchor, dpi, w, h));
    if (!popup)
        return;
    dropdown_popup = popup;
    popups.push_back(popup);

    // The root holds the MylarWindow; the scrolling state lives on the list inside it.
    popup->root->skip_delete = true;
    popup->root->user_data = popup;
    popup->root->wanted_bounds.w = FILL_SPACE;
    popup->root->wanted_bounds.h = FILL_SPACE;
    popup->root->on_closed = [popup](Container *) {
        if (dropdown_popup == popup) {
            dropdown_popup = nullptr;
            dropdown_menu_list = nullptr;
        }
        std::erase(popups, popup);
    };
    popup->root->when_paint = [](Container *root, Container *c) {
        auto window = ((MylarWindow *) root->user_data)->raw_window;
        auto b = c->real_bounds;
        paint_smooth_frame(window->drawing_context, b, 8 * window->dpi, theme.popup_background, theme.dropdown_border);
    };

    auto list = popup->root->child(::fullycustom, FILL_SPACE, FILL_SPACE);
    dropdown_menu_list = list;
    auto menu = new DropdownMenuData;
    for (size_t i = 0; i < options.size(); i++)
        if (options[i].id == selected)
            menu->selected_index = (int) i;
    list->user_data = menu;
    list->receive_events_even_if_obstructed = true;
    list->pre_layout = [menu](Container *root, Container *c, const Bounds &b) {
        // No wanted_pad: a fullycustom container gets its children_bounds passed back into
        // pre_layout, so a pad would shrink `b` and count twice, making every list overflow.
        // The 4px inset is applied by hand instead.
        auto dpi = ((MylarWindow *) root->user_data)->raw_window->dpi;
        const double inset = 4 * dpi;
        c->wanted_pad = Bounds(0, 0, 0, 0);
        for (auto ch : c->children)
            ch->wanted_bounds.h = dropdown_row_h * dpi;
        c->type = ::vbox;
        layout(root, c, b);
        c->type = ::fullycustom;
        double content = c->children.size() * dropdown_row_h * dpi + inset * 2;
        menu->overflow = (float) std::max(0.0, std::floor(content - b.h));
        if (!menu->scrolled_to_selection && b.h > 1) {
            menu->scrolled_to_selection = true;
            if (menu->selected_index >= 0)
                menu->scroll = -(float) ((menu->selected_index + .5) * dropdown_row_h * dpi + inset - b.h * .5);
        }
        if (menu->smooth.step(menu->scroll))
            windowing::redraw(((MylarWindow *) root->user_data)->raw_window);
        menu->scroll = std::clamp(menu->scroll, -menu->overflow, 0.0f);
        for (auto ch : c->children) {
            ch->real_bounds.x += inset;
            ch->real_bounds.w -= inset * 2;
            modify_all(ch, 0, inset + menu->scroll);
        }
    };
    list->when_fine_scrolled = [menu](Container *root, Container *, double, double scroll_y, bool touchpad) {
        auto window = ((MylarWindow *) root->user_data)->raw_window;
        float delta = (float) (scroll_y * 3 * window->dpi);
        if (touchpad) {
            menu->smooth.stop();
            menu->scroll = std::clamp(menu->scroll + delta, -menu->overflow, 0.0f);
        } else {
            menu->smooth.scroll_by(menu->scroll, delta, -menu->overflow, 0.0f);
        }
        windowing::redraw(window);
    };
    // The pointer takes over from the arrow keys.
    list->when_mouse_motion = [menu](Container *root, Container *) {
        if (menu->key_index < 0)
            return;
        menu->key_index = -1;
        windowing::redraw(((MylarWindow *) root->user_data)->raw_window);
    };
    list->when_paint = [](Container *root, Container *c) {
        auto window = ((MylarWindow *) root->user_data)->raw_window;
        window->drawing_context->save();
        auto b = c->real_bounds;
        window->drawing_context->rectangle(b.x, b.y + 2 * window->dpi, b.w, b.h - 4 * window->dpi);
        window->drawing_context->clip_path();
    };
    list->after_paint = [menu](Container *root, Container *c) {
        auto window = ((MylarWindow *) root->user_data)->raw_window;
        window->drawing_context->restore();
        if (menu->overflow <= 0)
            return;
        // Thin scroll indicator.
        auto dpi = window->dpi;
        auto b = c->real_bounds;
        double content = b.h + menu->overflow;
        double thumb_h = std::max(24.0 * dpi, b.h * b.h / content);
        double y = b.y + (-menu->scroll / menu->overflow) * (b.h - thumb_h);
        RGBA color = theme.text_secondary;
        color.a *= .4;
        set_argb(window->drawing_context, color);
        drawRoundedRect(window->drawing_context, b.x + b.w - 5 * dpi, y, 3 * dpi, thumb_h, 1.5 * dpi, 1.0);
        window->drawing_context->fill();
    };

    for (size_t i = 0; i < options.size(); i++) {
        auto ch = list->child(FILL_SPACE, dropdown_row_h);
        bool is_selected = (int) i == menu->selected_index;
        auto label = options[i].label;
        int index = (int) i;
        ch->when_paint = [label, is_selected, menu, index](Container *root, Container *c) {
            auto window = ((MylarWindow *) root->user_data)->raw_window;
            auto cr = window->drawing_context;
            auto dpi = window->dpi;
            auto b = c->real_bounds;
            if (c->state.mouse_pressing || c->state.mouse_hovering || menu->key_index == index) {
                set_argb(cr, c->state.mouse_pressing ? theme.popup_pressed : theme.popup_hover);
                drawRoundedRect(cr, b.x, b.y, b.w, b.h, 6 * dpi, 1.0);
                cr->fill();
            }
            if (is_selected) {
                set_argb(cr, theme.accent);
                drawRoundedRect(cr, b.x + 4 * dpi, b.y + b.h * .3, 3 * dpi, b.h * .4, 1.5 * dpi, 1.0);
                cr->fill();
            }
            swifi_text(root, Bounds(b.x + 16 * dpi, b.y, b.w - 24 * dpi, b.h), label, 11, theme.text, false, is_selected);
        };
        auto id = options[i].id;
        ch->when_clicked = [id, on_pick](Container *, Container *) {
            if (on_pick)
                on_pick(id);
            close_dropdown_menu();
        };
    }
    windowing::redraw(popup->raw_window);
}

// Keys for an open dropdown menu: Up, Down, Home and End move the highlight,
// Enter and Space pick it, Escape and Tab close the menu. True when used.
static bool dropdown_menu_key(xkb_keysym_t sym) {
    if (!dropdown_popup || !dropdown_menu_list)
        return false;
    auto list = dropdown_menu_list;
    auto menu = (DropdownMenuData *) list->user_data;
    int count = (int) list->children.size();
    if (sym == XKB_KEY_Escape || sym == XKB_KEY_Tab || sym == XKB_KEY_ISO_Left_Tab) {
        close_dropdown_menu();
        return true;
    }
    if (sym == XKB_KEY_Return || sym == XKB_KEY_KP_Enter || sym == XKB_KEY_space) {
        if (menu->key_index >= 0 && menu->key_index < count) {
            auto row = list->children[menu->key_index];
            if (row->when_clicked)
                row->when_clicked(dropdown_popup->root, row);
        } else {
            close_dropdown_menu();
        }
        return true;
    }
    int from = menu->key_index >= 0 ? menu->key_index : menu->selected_index;
    int index;
    if (sym == XKB_KEY_Down)
        index = from < 0 ? 0 : from + 1;
    else if (sym == XKB_KEY_Up)
        index = from < 0 ? count - 1 : from - 1;
    else if (sym == XKB_KEY_Home)
        index = 0;
    else if (sym == XKB_KEY_End)
        index = count - 1;
    else
        return false;
    if (count == 0)
        return true;
    menu->key_index = std::clamp(index, 0, count - 1);
    // Keep the row in view.
    auto dpi = dropdown_popup->raw_window->dpi;
    double inset = 4 * dpi;
    double top = menu->key_index * dropdown_row_h * dpi + inset;
    double bottom = top + dropdown_row_h * dpi;
    double view = list->real_bounds.h;
    float scroll = menu->scroll;
    if (top + scroll < 0)
        scroll = (float) -(top - inset);
    else if (bottom + scroll > view)
        scroll = (float) -(bottom + inset - view);
    menu->smooth.stop();
    menu->scroll = std::clamp(scroll, -menu->overflow, 0.0f);
    windowing::redraw(dropdown_popup->raw_window);
    return true;
}

static double dropdown_button_width(Container *root, const std::string &label) {
    auto dpi = swifi_window(root)->dpi;
    return std::clamp(swifi_text_width(root, label, 11) + 46 * dpi, 120.0 * dpi, 250.0 * dpi);
}

// The button's frame, vertically centered in its container.
static Bounds dropdown_button_frame(Container *root, Container *c) {
    double h = std::min<double>(30 * swifi_window(root)->dpi, c->real_bounds.h);
    return Bounds(c->real_bounds.x, c->real_bounds.y + (c->real_bounds.h - h) * .5, c->real_bounds.w, h);
}

static void paint_dropdown_button(Container *root, Container *c, const std::string &label) {
    auto window = swifi_window(root);
    auto cr = window->drawing_context;
    auto dpi = window->dpi;
    Bounds b = dropdown_button_frame(root, c);
    paint_smooth_frame(cr, b, settings_control_rounding * dpi,
        c->state.mouse_pressing ? theme.dropdown_pressed :
        c->state.mouse_hovering ? theme.dropdown_hover : theme.dropdown_background,
        theme.dropdown_border);
    swifi_text(root, Bounds(b.x + 10 * dpi, b.y, b.w - 38 * dpi, b.h), label, 11, theme.text);
    // ChevronDown (U+E70D) marks the button as a dropdown
    const std::string chevron = "\uE70D";
    auto cb = draw_text(cr, 0, 0, chevron, 10 * dpi, false, icon_font, -1, -1, theme.text_secondary, false);
    draw_text(cr, b.x + b.w - 14 * dpi - cb.w, b.y + b.h * .5 - cb.h * .5, chevron, 10 * dpi, true, icon_font, -1, -1, theme.text_secondary, false);
}

// live: when given, the shown value is read from it on every layout, for
// values that can also change from outside the dropdown.
static void make_dropdown(Container *parent, std::string text_, std::vector<std::string> options, std::function<void(std::string)> func,
                          std::function<std::string()> live = {}) {
    struct TextData : UserData {
        std::string text;
        std::function<std::string()> live;
    };
    auto pad = parent->child(FILL_SPACE, FILL_SPACE);
    auto text_data = new TextData;
    text_data->text = text_;
    text_data->live = live;
    pad->user_data = text_data;
    pad->pre_layout = [](Container *root, Container *c, const Bounds &) {
        auto td = (TextData *) c->user_data;
        if (td->live)
            td->text = td->live();
        c->wanted_bounds.w = dropdown_button_width(root, td->text);
        c->wanted_bounds.h = 34 * swifi_window(root)->dpi;
    };
    pad->when_paint = [](Container *root, Container *c) {
        paint_dropdown_button(root, c, ((TextData *) c->user_data)->text);
    };
    focus_register(pad, {.ring = dropdown_button_frame});
    pad->when_clicked = [func, options](Container *root, Container *c) {
        auto td = (TextData *) c->user_data;
        std::vector<DropdownOption> menu_options;
        for (auto &option : options)
            menu_options.push_back({option, option});
        open_dropdown_menu(root, c, menu_options, td->text, [td, func](std::string picked) {
            td->text = picked;
            if (func)
                func(picked);
        });
    };
    pad->when_key_event = [](Container *, Container *, int, bool pressed, xkb_keysym_t sym, int, bool, std::string) {
        if (pressed && sym == XKB_KEY_Escape)
            close_dropdown_menu();
    };
}

static Container *make_dropdown_option(Container *parent, std::string title, std::string description, std::string icon, std::string selected, std::vector<std::string> options, std::function<void(std::string)> on_change, std::function<std::string()> live = {}) {
    assert(on_change);
    auto p = make_self_height_sized_parent(parent);

    make_label_like(p, title, description, icon);

    auto right = p->child(::hbox, FILL_SPACE, FILL_SPACE);
    right->alignment = container_alignment::ALIGN_CENTER;
    
    right->pre_layout = [](Container *root, Container *c, const Bounds &b) {
        auto mylar = (MylarWindow*)root->user_data;
        auto dpi = mylar->raw_window->dpi;
        c->real_bounds.w = 250 * dpi;
        c->real_bounds.h = 70 * dpi;
        c->spacing = 5 * dpi;
    };
    right->child(FILL_SPACE, FILL_SPACE);
    right->parent_bounds_limit_input_bounds = false;
    
    make_dropdown(right, selected, options, [on_change](std::string selected) {
        on_change(selected);
    }, live);
    
    return p;
}

static void make_button(Container *parent, std::string text, std::function<void()> func, std::function<bool()> is_enabled = {}) {
    static float pad_amount = 11;
    static float text_height = 11;
    auto pad = parent->child(FILL_SPACE, FILL_SPACE);
    pad->pre_layout = [text](Container *root, Container *c, const Bounds &b) {
        auto mylar = (MylarWindow*)root->user_data;
        auto cr = mylar->raw_window->drawing_context;
        auto dpi = mylar->raw_window->dpi;
        text_height = 11 * dpi;
        pad_amount = 11 * dpi;
        Bounds bounds = draw_text(cr, 0, 0, text, text_height, false, set->font, -1, -1, theme.text, true);
        c->wanted_bounds.w = bounds.w + pad_amount * 2;
        c->wanted_bounds.h = bounds.h + (pad_amount * 2 * .8);
    };
    pad->when_paint = [text, is_enabled](Container *root, Container *c) {
        auto mylar = (MylarWindow*)root->user_data;
        auto cr = mylar->raw_window->drawing_context;
        auto dpi = mylar->raw_window->dpi;
        const bool enabled = !is_enabled || is_enabled();
        auto text_color = enabled ? theme.text : theme.text_secondary;
        if (!enabled) {
            text_color.a *= .5;
            set_argb(cr, theme.slider_track);
        } else if (c->state.mouse_pressing) {
            set_argb(cr, theme.button_pressed);
        } else if (c->state.mouse_hovering) {
            set_argb(cr, theme.button_hover);
        } else {
            set_argb(cr, theme.button_background);
        }
        drawRoundedRect(cr, c->real_bounds.x, c->real_bounds.y,
                        c->real_bounds.w, c->real_bounds.h,
                        settings_control_rounding * dpi, 1.0);
        cr->fill();
        Bounds bounds = draw_text(cr, 0, 0, text, text_height, false, set->font, -1, -1, theme.text, false);
        draw_text(cr, 
            c->real_bounds.x + c->real_bounds.w * .5 - bounds.w * .5, 
            c->real_bounds.y + c->real_bounds.h * .5 - bounds.h * .5, 
            text, text_height, true, set->font, -1, -1, text_color, false);
    };
   pad->when_clicked = [func, is_enabled](Container *root, Container *c) {
       if (func && (!is_enabled || is_enabled())) {
           func();
       }
   };
}

static void make_button_option(Container *parent, std::string title, std::string description, std::string button_text, std::function<void()> on_click, std::string icon = "", std::function<bool()> is_enabled = {}) {
    auto p = make_self_height_sized_parent(parent);
    make_label_like(p, title, description, icon);

    auto right = p->child(::hbox, FILL_SPACE, FILL_SPACE);
    right->alignment = container_alignment::ALIGN_CENTER | container_alignment::ALIGN_RIGHT;
    right->pre_layout = [](Container *root, Container *c, const Bounds &b) {
        auto mylar = (MylarWindow*)root->user_data;
        auto dpi = mylar->raw_window->dpi;
        c->real_bounds.w = 250 * dpi;
        c->real_bounds.h = 70 * dpi;
    };
    right->child(FILL_SPACE, FILL_SPACE);
    right->parent_bounds_limit_input_bounds = false;
    make_button(right, button_text, on_click, is_enabled);
}

static void make_bool_with_button(Container *parent, std::string title, std::string description, bool initial_value, std::function<void(bool)> on_change, std::string button_text, std::function<void()> on_click, std::string icon = "") {
    auto p = make_self_height_sized_parent(parent);
    make_label_like(p, title, description, icon);

    auto right = p->child(::hbox, FILL_SPACE, FILL_SPACE);
    right->alignment = container_alignment::ALIGN_CENTER | container_alignment::ALIGN_RIGHT;
    right->pre_layout = [](Container *root, Container *c, const Bounds &b) {
        auto mylar = (MylarWindow*)root->user_data;
        auto dpi = mylar->raw_window->dpi;
        c->real_bounds.w = 250 * dpi;
        c->real_bounds.h = 70 * dpi;
        c->spacing = 5 * dpi;
    };

    // The toggle paints its On/Off label to the left of its own bounds.
    auto label_space = right->child(FILL_SPACE, FILL_SPACE);
    label_space->pre_layout = [](Container *root, Container *c, const Bounds &b) {
        auto mylar = (MylarWindow*)root->user_data;
        auto cr = mylar->raw_window->drawing_context;
        auto dpi = mylar->raw_window->dpi;
        auto on = draw_text(cr, 0, 0, "On", 11 * dpi, false, set->font, -1, -1, theme.text, false, 0);
        auto off = draw_text(cr, 0, 0, "Off", 11 * dpi, false, set->font, -1, -1, theme.text, false, 0);
        c->wanted_bounds.w = std::max(on.w, off.w) + 12 * dpi;
    };

    make_bool_control(right, initial_value, on_change, p);
    make_button(right, button_text, on_click);
}

// What the focused field expects ("1 is 100%", "Whole number from 1 to 20"),
// one line per '\n'. Set when a field with a hint paints while focused and
// drawn in a box under it once the page is painted (paint_field_hint), so the
// box lies over the rows below.
static std::string field_hint_text;
static Bounds field_hint_anchor;

static void paint_field_hint(Container *root, Container *right) {
    if (field_hint_text.empty())
        return;
    std::vector<std::string> lines;
    std::stringstream ss(field_hint_text);
    for (std::string line; std::getline(ss, line);)
        lines.push_back(line);
    field_hint_text.clear();

    auto window = swifi_window(root);
    auto cr = window->drawing_context;
    auto dpi = window->dpi;
    static const std::string info_icon = "\uE946";
    const double text_size = 10;
    const double pad = 10 * dpi;
    const double line_h = 18 * dpi;
    const int icon_size = std::round(12 * dpi);
    auto icon = draw_text(cr, 0, 0, info_icon, icon_size, false, icon_font, -1, -1, theme.accent, false);
    const double text_x = pad + icon.w + 8 * dpi;
    double text_w = 0;
    for (const auto &line : lines)
        text_w = std::max(text_w, swifi_text_width(root, line, text_size));

    const auto page = right->real_bounds;
    const auto field = field_hint_anchor;
    const double gap = 6 * dpi;
    Bounds box(field.x, field.y + field.h + gap, text_x + text_w + pad + 2 * dpi, lines.size() * line_h + 12 * dpi);
    box.w = std::min(box.w, page.w - 2 * gap);
    // Stay inside the page: slide left, and go above a field at the bottom.
    box.x = std::max(page.x + gap, std::min(box.x, page.x + page.w - gap - box.w));
    if (box.y + box.h > page.y + page.h - gap)
        box.y = field.y - gap - box.h;

    paint_smooth_frame(cr, box, 8 * dpi, theme.popup_background, theme.dropdown_border);
    draw_text(cr, box.x + pad, box.y + 6 * dpi + line_h * .5 - icon.h * .5, info_icon, icon_size, true, icon_font, -1, -1,
              theme.accent, false);
    for (int i = 0; i < (int) lines.size(); i++) {
        Bounds line(box.x + text_x, box.y + 6 * dpi + i * line_h, box.w - text_x - pad, line_h);
        swifi_text(root, line, lines[i], text_size, i == 0 ? theme.text : theme.text_secondary);
    }
}

static Container *make_field(Container *parent, bool only_numbers, std::string initial_value, std::function<void (std::string)> on_change,
                             std::string hint = "") {
    auto pad = parent->child(FILL_SPACE, FILL_SPACE);
    STextEditorOptions options;
    options.multiline = false;
    options.only_numbers = only_numbers;
    options.font_size = 11;
    options.window = [](Container *root) { return static_cast<MylarWindow *>(root->user_data)->raw_window; };
    options.viewport = [](Container *root, Container *c) {
        auto dpi = static_cast<MylarWindow *>(root->user_data)->raw_window->dpi;
        auto bounds = c->real_bounds;
        bounds.x += 11 * dpi;
        bounds.w = std::max(0.0, bounds.w - 22 * dpi);
        return bounds;
    };
    options.on_change = std::move(on_change);
    setup_text_editor(pad, std::move(initial_value), std::move(options));
    focus_text(pad);
    pad->pre_layout = [](Container *root, Container *c, const Bounds &b) {
        auto dpi = static_cast<MylarWindow *>(root->user_data)->raw_window->dpi;
        auto bounds = measure_text_editor(root, c);
        c->wanted_bounds.w = FILL_SPACE;
        c->wanted_bounds.h = bounds.h + 11 * dpi * 2 * .8;
    };
    pad->when_paint = [hint](Container *root, Container *c) {
        auto mylar = static_cast<MylarWindow *>(root->user_data);
        auto cr = mylar->raw_window->drawing_context;
        auto dpi = mylar->raw_window->dpi;
        paint_smooth_frame(cr, c->real_bounds, settings_control_rounding * dpi, theme.field_background,
                           c->active ? theme.accent : theme.field_border, std::max(1.0, (double) std::round(1 * dpi)));
        paint_text_editor(root, c, theme.text, theme.accent);
        if (c->active && !hint.empty()) {
            field_hint_text = hint;
            field_hint_anchor = c->real_bounds;
        }
    };
    return pad;
}

struct MonitorOption {
    std::string name;
    std::vector<std::string> option;
    float w = 1920;
    float h = 1080;
    float fps = 60;
    int x = 0; 
    int y = 0; 
    float scale = 1.0;
    int transform = 0;

    MylarMonitor             config;
    bool                     custom_scale_mode = false;
};

static void commit_monitor_configs(std::vector<MylarMonitor> configs);

// Where a monitor sits in the compositor layout, in logical pixels.
static Bounds monitor_logical_bounds(const MonitorOption *o) {
    double w = std::round(o->w / std::max(o->scale, 0.1f));
    double h = std::round(o->h / std::max(o->scale, 0.1f));
    if (o->transform % 2 == 1)
        std::swap(w, h);
    return Bounds(o->x, o->y, w, h);
}

// Tries to line [pos, pos + size] up with the edges and the center of
// [other, other + other_size]. `best` is the closest distance found so far.
static void monitor_snap_axis(double pos, double size, double other, double other_size, double &best, double &snapped, double &guide) {
    const double far_edge = other + other_size;
    const double center   = other + other_size * .5;
    // {where pos would end up, where the guide line goes}
    const double candidates[5][2] = {
        {other, other},
        {far_edge, far_edge},
        {other - size, other},
        {far_edge - size, far_edge},
        {center - size * .5, center},
    };
    for (const auto &candidate : candidates) {
        const double distance = std::abs(candidate[0] - pos);
        if (distance >= best)
            continue;
        best    = distance;
        snapped = candidate[0];
        guide   = candidate[1];
    }
}

static bool monitor_rects_overlap(const Bounds &a, const Bounds &b) {
    const double slack = .5;
    return a.x < b.x + b.w - slack && b.x < a.x + a.w - slack && a.y < b.y + b.h - slack && b.y < a.y + a.h - slack;
}

// True when the rects share part of an edge, a shared corner does not count.
static bool monitor_rects_touch(const Bounds &a, const Bounds &b) {
    const double slack     = .5;
    const double overlap_x = std::min(a.x + a.w, b.x + b.w) - std::max(a.x, b.x);
    const double overlap_y = std::min(a.y + a.h, b.y + b.h) - std::max(a.y, b.y);
    return (overlap_x >= -slack && overlap_y > slack) || (overlap_y >= -slack && overlap_x > slack);
}

static void monitor_push_out(Bounds &me, const std::vector<Bounds> &others) {
    for (int pass = 0; pass < 8; pass++) {
        bool moved = false;
        for (const auto &other : others) {
            if (!monitor_rects_overlap(me, other))
                continue;
            // Leave through the side that needs the least movement.
            const double moves[4] = {other.x - (me.x + me.w), other.x + other.w - me.x, other.y - (me.y + me.h), other.y + other.h - me.y};
            int          least    = 0;
            for (int i = 1; i < 4; i++)
                if (std::abs(moves[i]) < std::abs(moves[least]))
                    least = i;
            if (least < 2)
                me.x += moves[least];
            else
                me.y += moves[least];
            moved = true;
        }
        if (!moved)
            return;
    }
}

// Turns a dropped position into a valid layout: no overlaps, and attached to
// another monitor so the cursor can always cross. False if no spot was found.
static bool monitor_settle(Bounds &me, const std::vector<Bounds> &others) {
    if (others.empty())
        return true;

    monitor_push_out(me, others);

    bool attached = false;
    for (const auto &other : others)
        attached |= monitor_rects_touch(me, other);

    if (!attached) {
        const Bounds *nearest      = nullptr;
        double        nearest_dist = 0;
        double        nearest_gx   = 0;
        double        nearest_gy   = 0;
        for (const auto &other : others) {
            const double gx   = std::max({other.x - (me.x + me.w), me.x - (other.x + other.w), 0.0});
            const double gy   = std::max({other.y - (me.y + me.h), me.y - (other.y + other.h), 0.0});
            const double dist = std::hypot(gx, gy);
            if (nearest && dist >= nearest_dist)
                continue;
            nearest      = &other;
            nearest_dist = dist;
            nearest_gx   = gx;
            nearest_gy   = gy;
        }
        const bool before = me.x < nearest->x;
        const bool above  = me.y < nearest->y;
        if (nearest_gx > 0 && nearest_gy > 0) {
            // Diagonal: close the horizontal gap and share the top or bottom edge.
            me.x += before ? nearest_gx : -nearest_gx;
            me.y = above ? nearest->y : nearest->y + nearest->h - me.h;
        } else if (nearest_gx > 0) {
            me.x += before ? nearest_gx : -nearest_gx;
        } else {
            me.y += above ? nearest_gy : -nearest_gy;
        }
        monitor_push_out(me, others);
    }

    for (const auto &other : others)
        if (monitor_rects_overlap(me, other))
            return false;
    return true;
}

// The monitor layout at the top of the display page. Monitors can be dragged
// around, they snap to the edges and centers of the others, and dropping one
// applies the new layout.
static void make_screen_positioner(Container *parent, std::vector<MonitorOption *> &options, std::function<void (std::string)> on_clicked) {
    static int height = 300;
    auto c = parent->child(::fullycustom, FILL_SPACE, height);
    struct MonitorCanvas : UserData {
        // Logical to canvas pixels. Frozen while dragging so the view holds still.
        double scale    = .1;
        double origin_x = 0; // logical point shown at the center of the canvas
        double origin_y = 0;
        bool   dragging = false;
        bool   guide_x_on = false;
        bool   guide_y_on = false;
        double guide_x    = 0; // logical
        double guide_y    = 0;
    };
    struct MonitorInfo : UserData {
        MonitorOption *o = nullptr;
        bool selected = false;
        bool dragging = false;
        double drag_x = 0; // logical position while dragging
        double drag_y = 0;
    };
    c->user_data = new MonitorCanvas;
    c->pre_layout = [](Container *root, Container *c, const Bounds &b) {
        auto mylar = (MylarWindow*)root->user_data;
        auto dpi = mylar->raw_window->dpi;
        c->wanted_bounds.h = height * dpi;

        if (c->children.empty())
            return;

        auto canvas = (MonitorCanvas *) c->user_data;
        if (!canvas->dragging) {
            double min_x = INT_MAX;
            double min_y = INT_MAX;
            double max_x = INT_MIN;
            double max_y = INT_MIN;
            for (auto ch : c->children) {
                auto r = monitor_logical_bounds(((MonitorInfo *) ch->user_data)->o);
                min_x  = std::min(min_x, r.x);
                min_y  = std::min(min_y, r.y);
                max_x  = std::max(max_x, r.x + r.w);
                max_y  = std::max(max_y, r.y + r.h);
            }
            const double total_w = std::max(max_x - min_x, 1.0);
            const double total_h = std::max(max_y - min_y, 1.0);
            canvas->origin_x     = min_x + total_w * .5;
            canvas->origin_y     = min_y + total_h * .5;
            // Leave room around the layout to drag monitors into.
            canvas->scale = std::min(c->real_bounds.w * .5 / total_w, c->real_bounds.h * .5 / total_h);
            if (canvas->scale <= 0)
                canvas->scale = .1;
        }

        const double center_x = c->real_bounds.x + c->real_bounds.w * .5;
        const double center_y = c->real_bounds.y + c->real_bounds.h * .5;
        for (auto ch : c->children) {
            auto data = (MonitorInfo *) ch->user_data;
            auto r    = monitor_logical_bounds(data->o);
            if (data->dragging) {
                r.x = data->drag_x;
                r.y = data->drag_y;
            }
            ch->real_bounds = Bounds(center_x + (r.x - canvas->origin_x) * canvas->scale, center_y + (r.y - canvas->origin_y) * canvas->scale, r.w * canvas->scale,
                                     r.h * canvas->scale);
            ch->wanted_bounds = ch->real_bounds;
            layout(root, ch, ch->real_bounds);
        }
    };
    c->when_paint = paint {
        auto mylar = (MylarWindow*)root->user_data;
        auto cr = mylar->raw_window->drawing_context;
        auto dpi = mylar->raw_window->dpi;
        set_argb(cr, theme.monitor_background);
        auto b = c->real_bounds;
        drawRoundedRect(cr, b.x, b.y, b.w, b.h, 10 * dpi, 1.0 * dpi);
        cr->fill();
    };
    // Alignment guides, drawn over the monitors while one is snapped.
    c->after_paint = paint {
        auto canvas = (MonitorCanvas *) c->user_data;
        if (!canvas->dragging || (!canvas->guide_x_on && !canvas->guide_y_on))
            return;
        auto mylar = (MylarWindow*)root->user_data;
        auto cr = mylar->raw_window->drawing_context;
        auto dpi = mylar->raw_window->dpi;
        auto b = c->real_bounds;

        cr->save();
        set_rect(cr, b);
        cr->clip_path();
        set_argb(cr, theme.accent);
        cr->set_line_width(std::max(1.0, std::floor(1.0 * dpi)));
        if (canvas->guide_x_on) {
            const double x = std::round(b.x + b.w * .5 + (canvas->guide_x - canvas->origin_x) * canvas->scale) + .5;
            cr->move_to(x, b.y);
            cr->line_to(x, b.y + b.h);
            cr->stroke();
        }
        if (canvas->guide_y_on) {
            const double y = std::round(b.y + b.h * .5 + (canvas->guide_y - canvas->origin_y) * canvas->scale) + .5;
            cr->move_to(b.x, y);
            cr->line_to(b.x + b.w, y);
            cr->stroke();
        }
        cr->restore();
    };

    bool first = true;
    for (auto option : options) {
        auto main = c->child(10, 10);
        main->when_paint = paint {
            auto mylar = (MylarWindow*)root->user_data;
            auto cr = mylar->raw_window->drawing_context;
            auto dpi = mylar->raw_window->dpi;
            auto data = (MonitorInfo *) c->user_data;
            auto b = c->real_bounds;
            
            cr->save();
            set_rect(cr, c->parent->real_bounds);
            cr->clip_path();
            
            auto surface = theme.monitor_surface;
            if (data->dragging)
                surface.a *= .85;
            set_argb(cr, surface);
            drawRoundedRect(cr, b.x, b.y, b.w, b.h, 10 * dpi, 1.0 * dpi);
            cr->fill();

            if (data->selected) {
                set_argb(cr, theme.accent);
                drawRoundedRect(cr, b.x, b.y, b.w, b.h, 10 * dpi, 2.0 * dpi);
                cr->stroke();
            }

            auto text = fz("{}\n({}x{})", data->o->name, data->o->w, data->o->h);
            if (data->dragging)
                text += fz("\n{}, {}", (int) std::round(data->drag_x), (int) std::round(data->drag_y));
            auto tb = draw_text(cr, 0, 0, text, 12 * dpi, false, set->font, c->real_bounds.w, -1, theme.text, false, 1);
            
            draw_text(cr, 
                c->real_bounds.x,
                c->real_bounds.y + c->real_bounds.h * .5 - tb.h * .5, 
                text, 12 * dpi, true, set->font, c->real_bounds.w, -1, theme.text, false, 1);

            cr->reset_clip();
            cr->restore();
        };
        auto select = [on_clicked](Container *root, Container *c) {
            auto data = (MonitorInfo *) c->user_data;
            if (data->selected)
                return;
            for (auto ch : c->parent->children)
                ((MonitorInfo *) ch->user_data)->selected = false;
            data->selected = true;
            if (on_clicked)
                on_clicked(data->o->name);
        };
        main->when_clicked = select;
        main->when_drag_end_is_click = false;
        main->when_drag_start = [select](Container *root, Container *c) {
            select(root, c);
            // A lone monitor has nothing to be positioned against: leave its position (usually "auto") alone.
            if (c->parent->children.size() <= 1)
                return;
            auto canvas = (MonitorCanvas *) c->parent->user_data;
            auto data   = (MonitorInfo *) c->user_data;
            c->z_index       = 100;
            canvas->dragging = true;
            data->dragging   = true;
            data->drag_x     = data->o->x;
            data->drag_y     = data->o->y;
        };
        main->when_drag = [](Container *root, Container *c) {
            auto mylar  = (MylarWindow*)root->user_data;
            auto dpi    = mylar->raw_window->dpi;
            auto canvas = (MonitorCanvas *) c->parent->user_data;
            auto data   = (MonitorInfo *) c->user_data;
            if (!data->dragging)
                return;
            auto me     = monitor_logical_bounds(data->o);

            const double x = me.x + (root->mouse_current_x - root->mouse_initial_x) / canvas->scale;
            const double y = me.y + (root->mouse_current_y - root->mouse_initial_y) / canvas->scale;

            // Snap distance is constant on screen, whatever the zoom of the layout.
            const double threshold = 10 * dpi / canvas->scale;
            double       best_x    = threshold;
            double       best_y    = threshold;
            data->drag_x           = x;
            data->drag_y           = y;
            for (auto ch : c->parent->children) {
                if (ch == c)
                    continue;
                auto other = monitor_logical_bounds(((MonitorInfo *) ch->user_data)->o);
                monitor_snap_axis(x, me.w, other.x, other.w, best_x, data->drag_x, canvas->guide_x);
                monitor_snap_axis(y, me.h, other.y, other.h, best_y, data->drag_y, canvas->guide_y);
            }
            canvas->guide_x_on = best_x < threshold;
            canvas->guide_y_on = best_y < threshold;
        };
        main->when_drag_end = [on_clicked](Container *root, Container *c) {
            auto canvas = (MonitorCanvas *) c->parent->user_data;
            auto data   = (MonitorInfo *) c->user_data;
            if (!data->dragging)
                return;
            c->z_index         = 0;
            canvas->dragging   = false;
            canvas->guide_x_on = false;
            canvas->guide_y_on = false;
            data->dragging     = false;

            auto me = monitor_logical_bounds(data->o);
            me.x    = data->drag_x;
            me.y    = data->drag_y;
            std::vector<Bounds> others;
            for (auto ch : c->parent->children)
                if (ch != c)
                    others.push_back(monitor_logical_bounds(((MonitorInfo *) ch->user_data)->o));
            if (!monitor_settle(me, others))
                return;

            const int x = (int) std::round(me.x);
            const int y = (int) std::round(me.y);
            if (x == data->o->x && y == data->o->y)
                return;
            data->o->x = x;
            data->o->y = y;

            // Pin every monitor, ones left on "auto" would shuffle around the moved one.
            std::vector<MylarMonitor> configs;
            for (auto ch : c->parent->children) {
                auto o             = ((MonitorInfo *) ch->user_data)->o;
                o->config.position = fz("{}x{}", o->x, o->y);
                configs.push_back(o->config);
            }
            commit_monitor_configs(configs);

            // Rebuild the option rows so the position field shows the new value.
            if (on_clicked)
                on_clicked(data->o->name);
        };
     
        auto monitor_info = new MonitorInfo;
        monitor_info->o = option;
        monitor_info->selected = first;
        first = false;
        
        main->user_data = monitor_info;        
    }
}

static Container *make_reset_textfield(Container* parent, std::string title, std::string description, std::string icon, bool only_numbers, std::string initial_value,
                                 std::string reset_value, std::function<void(std::string)> on_change, std::function<std::string()> left_text = nullptr,
                                 std::string hint = "");
static std::string resolved_font_family(const std::string &name);
static void attach_font_completions(Container *field, std::function<void(std::string)> on_pick);

static bool monitor_int_from_text(const std::string& text, int& value) {
    try {
        size_t    parsed = 0;
        const int result = std::stoi(text, &parsed);
        if (parsed != text.size())
            return false;
        value = result;
        return true;
    } catch (...) { return false; }
}

static bool monitor_float_from_text(const std::string& text, float& value) {
    try {
        size_t      parsed = 0;
        const float result = std::stof(text, &parsed);
        if (parsed != text.size())
            return false;
        value = result;
        return true;
    } catch (...) { return false; }
}

// Applies the configs as one change: a single trial and a single reload.
static void commit_monitor_configs(std::vector<MylarMonitor> configs) {
    main_thread([configs = std::move(configs)]() {
        auto find = [](const MylarMonitor& config) { return std::ranges::find(set->monitors, config.output, &MylarMonitor::output); };

        const bool changed = std::ranges::any_of(configs, [&find](const MylarMonitor& config) {
            auto existing = find(config);
            return existing == set->monitors.end() || *existing != config;
        });
        if (!changed)
            return;

        settings::display_change_begin();
        for (const auto& config : configs) {
            // Only one nested config is kept (its output name isn't stable),
            // so the latest one always wins.
            if (is_nested_output(config.output))
                std::erase_if(set->monitors, [&config](const MylarMonitor& m) { return is_nested_output(m.output) && m.output != config.output; });
            auto existing = find(config);
            if (existing != set->monitors.end())
                *existing = config;
            else
                set->monitors.push_back(config);
        }

        heart::remember_window_positions();
        settings::load_save_settings(true, set);
        hypriso->reload();
    });
}

static void commit_monitor_config(const MylarMonitor& config) {
    commit_monitor_configs({config});
}

struct AccordionData : UserData {
    std::vector<Container *> rows;
    bool *open = nullptr;
};

// A row that shows or hides the rows listed in its AccordionData when clicked.
static Container *make_accordion(Container *parent, std::string title, std::string description, std::string icon, bool *open) {
    auto p = make_self_height_sized_parent(parent);
    auto data = new AccordionData;
    data->open = open;
    p->user_data = data;
    p->custom_type = search_accordion_type;
    make_label_like(p, title, description, icon);

    auto right = p->child(FILL_SPACE, FILL_SPACE);
    right->pre_layout = [](Container *root, Container *c, const Bounds &b) {
        auto mylar = (MylarWindow*)root->user_data;
        auto dpi = mylar->raw_window->dpi;
        c->real_bounds.w = 30 * dpi;
        c->real_bounds.h = 70 * dpi;
    };
    right->when_paint = [open](Container *root, Container *c) {
        auto mylar = (MylarWindow*)root->user_data;
        auto cr = mylar->raw_window->drawing_context;
        auto dpi = mylar->raw_window->dpi;
        const std::string chevron = *open ? "\uE70E" : "\uE70D";
        auto b = draw_text(cr, 0, 0, chevron, 12 * dpi, false, icon_font, -1, -1, theme.text_secondary, false);
        draw_text(cr, c->real_bounds.x + c->real_bounds.w * .5 - b.w * .5, c->real_bounds.y + c->real_bounds.h * .5 - b.h * .5, chevron, 12 * dpi, true, icon_font, -1, -1,
                  theme.text_secondary, false);
    };
    // The row receives clicks even when a child is under the cursor.
    p->when_clicked = [data](Container *root, Container *c) {
        *data->open = !*data->open;
        for (auto row : data->rows)
            row->exists = *data->open;
    };
    return p;
}

// The kind of number a field takes and its range, then `note` on its own line.
template <typename T>
static std::string number_hint(const char *kind, T min_value, T max_value, const std::string &note) {
    std::string hint = kind;
    const bool has_min = min_value != std::numeric_limits<T>::lowest();
    const bool has_max = max_value != std::numeric_limits<T>::max();
    if (has_min && has_max)
        hint += fz(" from {} to {}", min_value, max_value);
    else if (has_min)
        hint += fz(", {} or more", min_value);
    else if (has_max)
        hint += fz(", {} or less", max_value);
    if (!note.empty())
        hint += "\n" + note;
    return hint;
}

static void add_monitor_int_field(Container* parent, std::string title, std::string description, std::string icon, int initial_value, int reset_value, int min_value, int max_value,
                                  std::function<void(int)> on_change, std::string note = "") {
    make_reset_textfield(parent, std::move(title), std::move(description), std::move(icon), false, std::to_string(initial_value), std::to_string(reset_value),
                         [min_value, max_value, on_change = std::move(on_change)](std::string value) {
                             int parsed = 0;
                             if (!monitor_int_from_text(trim(value), parsed)) {
                                 notify("Invalid value");
                                 return;
                             }
                             on_change(std::clamp(parsed, min_value, max_value));
                         }, nullptr, number_hint("Whole number", min_value, max_value, note));
}

static void add_monitor_float_field(Container* parent, std::string title, std::string description, std::string icon, float initial_value, float reset_value,
                                    float min_value, float max_value, std::function<void(float)> on_change, std::string note = "") {
    make_reset_textfield(parent, std::move(title), std::move(description), std::move(icon), false, fz("{}", initial_value), fz("{}", reset_value),
                         [min_value, max_value, on_change = std::move(on_change)](std::string value) {
                             float parsed = 0;
                             if (!monitor_float_from_text(trim(value), parsed)) {
                                 notify("Invalid value");
                                 return;
                             }
                             if (!std::isfinite(parsed)) {
                                 notify("Invalid value");
                                 return;
                             }
                             on_change(std::clamp(parsed, min_value, max_value));
                         }, nullptr, number_hint("Decimal number", min_value, max_value, note));
}

// Monitor whose option rows need rebuilding on the next layout, settings thread only.
static MonitorOption *display_options_stale = nullptr;

// The HDR row is a single switch, like Windows: it sets every related field to
// what HDR (or plain SDR) needs, so nobody has to understand the Advanced rows.
//
// How Hyprland uses them (see CMonitor::applyMonitorRuleSoft and getCMSettings):
// - cm "hdr" outputs PQ with BT.2020 primaries. It silently falls back to sRGB
//   when the EDID does not report HDR, and an ICC profile replaces cm entirely.
// - SDR content (almost every window) is mapped into HDR with SDR black at
//   sdr_min_luminance and SDR white at sdr_max_luminance nits. The defaults
//   (0.2 / 80) make the desktop look grey and dim next to SDR mode, which is the
//   usual "HDR looks washed out" complaint. 203 nits is the BT.2408 reference
//   white Hyprland itself uses for HDR, close to the 200 nits Windows picks by
//   default, and 0.005 is PQ's floor, so SDR black stays truly black.
// - sdrbrightness / sdrsaturation scale the already PQ-encoded signal. Guides
//   suggest sdrbrightness 1.2 to undo the 80 nit default; with white placed at
//   203 nits that hack would double up, so it goes back to 1.
// - The SDR-in-HDR values do nothing outside HDR, so turning HDR off keeps them
//   and a later turn on brings back whatever the user tuned, like Windows'
//   "SDR content brightness" slider.
// - Min/max/average luminance describe the panel (EDID overrides) and VRR is
//   unrelated, so they are left alone.
static void apply_hdr_defaults(MylarMonitor &config, bool on) {
    if (on) {
        if (config.cm != "hdredid")
            config.cm = "hdr";
        config.bitdepth = 10; // PQ in 8 bits bands badly
        if (!config.icc.empty()) {
            notify(fz("ICC profile \"{}\" removed, it overrides HDR", config.icc));
            config.icc.clear();
        }
        // Only replace values still at Hyprland's SDR defaults, keep anything tuned.
        if (config.sdr_max_luminance == 80) {
            config.sdr_max_luminance = 203;
            config.sdrbrightness     = 1.0f;
        }
        if (config.sdr_min_luminance == 0.2f)
            config.sdr_min_luminance = 0.005f;
        if (config.sdrsaturation <= 0)
            config.sdrsaturation = 1.0f;
    } else {
        config.cm       = "srgb";
        config.bitdepth = 8;
    }
}

static const std::vector<std::string> display_scale_presets = {"1.0", "1.25", "1.5", "1.75", "2.0"};

static std::string display_format_scale(float s) {
    if (!std::isfinite(s) || s <= 0)
        s = 1.0f;
    s = std::clamp(s, 0.3f, 6.0f);
    char buf[32];
    snprintf(buf, sizeof(buf), "%.2f", s);
    std::string out = buf;
    if (out.size() >= 4 && out.back() == '0' && out[out.size() - 3] == '.')
        out.pop_back();
    return out;
}

static std::string display_scale_matching_preset(const std::string &scale) {
    float parsed = 0;
    if (!monitor_float_from_text(trim(scale), parsed) || !std::isfinite(parsed))
        return "";
    for (const auto &p : display_scale_presets) {
        float preset_val = 0;
        if (monitor_float_from_text(p, preset_val) && std::abs(parsed - preset_val) < 1e-4f)
            return p;
    }
    return "";
}

static std::string display_scale_dropdown_label(const MonitorOption *m) {
    if (m->custom_scale_mode)
        return "Custom";
    const std::string s = trim(m->config.scale);
    if (s.empty() || s == "auto")
        return "Auto";
    if (auto preset = display_scale_matching_preset(s); !preset.empty())
        return preset;
    return "Custom";
}

static void change_display_options(MonitorOption *m) {
    auto right = container_by_name("settings_right", settings_mylar->root);
    if (!right)
        return;
    assert(!right->children.empty());
    auto padded_right = right->children[0];
    for (int i = padded_right->children.size() - 1; i >= 0; i--) {
        if (padded_right->children[i]->name == "removable") {
            delete padded_right->children[i];
            padded_right->children.erase(padded_right->children.begin() + i);
        }
    }

    const size_t first_new = padded_right->children.size();

    auto         commit = [m]() { commit_monitor_config(m->config); };

    make_vert_space(padded_right, 20);

    make_section_title(padded_right, fz(" {}", m->name));
    // Rows are the same for every monitor, so they're indexed under one name.
    search_current_section = "Monitor";

    make_vert_space(padded_right, 10);

    std::vector<std::string> modes = {"Preferred", "High refresh rate", "High resolution", "Maximum width"};
    modes.insert(modes.end(), m->option.begin(), m->option.end());

    std::string selected_mode = m->config.mode;
    if (selected_mode == "preferred")
        selected_mode = "Preferred";
    else if (selected_mode == "highrr")
        selected_mode = "High refresh rate";
    else if (selected_mode == "highres")
        selected_mode = "High resolution";
    else if (selected_mode == "maxwidth")
        selected_mode = "Maximum width";
    else if (selected_mode.contains('@') && !selected_mode.ends_with("Hz"))
        selected_mode += "Hz";

    make_dropdown_option(padded_right, "Mode", "Resolution and refresh rate", "\uE7F4", selected_mode, modes, [m, commit](std::string selected) {
        if (selected == "Preferred")
            m->config.mode = "preferred";
        else if (selected == "High refresh rate")
            m->config.mode = "highrr";
        else if (selected == "High resolution")
            m->config.mode = "highres";
        else if (selected == "Maximum width")
            m->config.mode = "maxwidth";
        else {
            if (selected.ends_with("Hz"))
                selected.resize(selected.size() - 2);
            m->config.mode = selected;
        }
        commit();
    });


    const std::vector<std::string> scale_options = {"Auto", "1.0", "1.25", "1.5", "1.75", "2.0", "Custom"};
    make_dropdown_option(padded_right, "Scale", "Scale factor, or \"auto\"", "\uE740", display_scale_dropdown_label(m), scale_options, [m, commit](std::string selected) {
        if (selected == "Auto") {
            m->custom_scale_mode = false;
            m->config.scale      = "auto";
            commit();
        } else if (selected == "Custom") {
            m->custom_scale_mode = true;
        } else {
            m->custom_scale_mode = false;
            m->config.scale      = selected;
            commit();
        }
        display_options_stale = m;
        if (settings_mylar)
            windowing::redraw(settings_mylar->raw_window);
    });

    const std::string trimmed_scale     = trim(m->config.scale);
    const bool        show_custom_scale = m->custom_scale_mode || (!trimmed_scale.empty() && trimmed_scale != "auto" && display_scale_matching_preset(trimmed_scale).empty());
    if (show_custom_scale) {
        const std::string initial_scale = (trimmed_scale.empty() || trimmed_scale == "auto") ? display_format_scale(m->scale) : trimmed_scale;
        make_reset_textfield(padded_right, "Custom scale", "Scale factor from 0.3 to 6 (e.g. 1.25)", "\uE740", false, initial_scale, "1.0", [m, commit](std::string value) {
            value = trim(value);
            if (value.empty() || value == "auto") {
                m->custom_scale_mode = false;
                m->config.scale      = "auto";
                commit();
                display_options_stale = m;
                if (settings_mylar)
                    windowing::redraw(settings_mylar->raw_window);
                return;
            }
            float scale = 0;
            if (!monitor_float_from_text(value, scale) || !std::isfinite(scale) || scale < 0.3f) {
                notify("Scale must be auto or a number from 0.3 to 6");
                return;
            }
            m->config.scale = fz("{}", std::min(scale, 6.0f));
            commit();
        }, nullptr, "Decimal number from 0.3 to 6, or \"auto\"\nA multiplier, not a percentage: 1 is 100%, 1.5 is 150%");
    }


    const std::vector<std::string> transforms = {"Normal (0°)", "Rotate 90°", "Rotate 180°", "Rotate 270°", "Flipped", "Flipped + 90°", "Flipped + 180°", "Flipped + 270°"};
    const int transform_index = std::clamp(m->config.transform, 0, 7);
    make_dropdown_option(padded_right, "Transform", "Rotation and reflection applied to the display", "\uE7AD", transforms[transform_index], transforms,
                         [m, commit, transforms](std::string selected) {
                             auto found = std::ranges::find(transforms, selected);
                             if (found == transforms.end())
                                 return;
                             m->config.transform = static_cast<int>(std::distance(transforms.begin(), found));
                             commit();
                         });


    const bool hdr_on = m->config.cm == "hdr" || m->config.cm == "hdredid";
    make_bool(padded_right, "HDR", "High dynamic range, for displays that support it", hdr_on, [m, commit](bool value) {
        apply_hdr_defaults(m->config, value);
        commit();
        // The Advanced rows show the values this just changed.
        display_options_stale = m;
        if (settings_mylar)
            windowing::redraw(settings_mylar->raw_window);
    }, "\uE793");


    // Everything below is rarely touched, keep it folded away until asked for.
    static bool advanced_open = false;
    auto        advanced      = make_accordion(padded_right, "Advanced", "Position, mirroring, bit depth, color management, VRR and more", "\uE9E9", &advanced_open);
    const size_t first_advanced = padded_right->children.size();


    make_reset_textfield(padded_right, "Position", "Position such as \"1920x0\" or \"auto\"", "\uE7C2", false, m->config.position, "auto", [m, commit](std::string value) {
        value              = trim(value);
        m->config.position = value.empty() ? "auto" : value;
        commit();
    }, nullptr, "Two whole numbers as XxY, or \"auto\"\nPixels from the left and from the top, such as 1920x0");


    make_reset_textfield(padded_right, "Mirror", "Monitor output to mirror, leave empty for none", "\uEBC6", false, m->config.mirror, "", [m, commit](std::string value) {
        m->config.mirror = trim(value);
        commit();
    });


    make_dropdown_option(padded_right, "Bit depth", "Color bit depth", "\uE81E", std::to_string(m->config.bitdepth), {"8", "10"}, [m, commit](std::string selected) {
        m->config.bitdepth = selected == "10" ? 10 : 8;
        commit();
    });


    const std::vector<std::string> cm_options = {"auto", "srgb", "wide", "edid", "hdr", "hdredid", "dcip3", "dp3", "adobe"};
    make_dropdown_option(padded_right, "Color management", "Color management preset", "\uE790", m->config.cm, cm_options, [m, commit](std::string selected) {
        m->config.cm = selected;
        commit();
    });


    const std::vector<std::string> eotf_options = {"default", "auto", "srgb", "gamma22", "gamma22force"};
    make_dropdown_option(padded_right, "SDR EOTF", "Transfer function used for SDR content", "\uE9D2", m->config.sdr_eotf, eotf_options, [m, commit](std::string selected) {
        m->config.sdr_eotf = selected;
        commit();
    });


    const std::vector<std::string> vrr_options = {"-1", "0", "1", "2", "3"};
    make_dropdown_option(padded_right, "VRR", "Variable refresh rate mode (-1 default, 0 off, 1 on, 2 fullscreen, 3 fullscreen only)", "\uEC4A", std::to_string(m->config.vrr),
                         vrr_options, [m, commit](std::string selected) {
                             int parsed = 0;
                             if (!monitor_int_from_text(selected, parsed))
                                 return;
                             m->config.vrr = std::clamp(parsed, -1, 3);
                             commit();
                         });


    make_reset_textfield(padded_right, "ICC profile", "Path to an ICC profile, leave empty for none", "\uE8A5", false, m->config.icc, "", [m, commit](std::string value) {
        m->config.icc = trim(value);
        commit();
    });


    add_monitor_int_field(padded_right, "Reserved area", "Reserved area in pixels", "\uE9A6", m->config.reserved_area, 0, 0, INT_MAX, [m, commit](int value) {
        m->config.reserved_area = value;
        commit();
    }, "In pixels");


    const std::vector<std::string> support_options = {"Auto", "Force enable", "Force disable"};
    make_dropdown_option(padded_right, "Supports wide color", "Override wide color gamut support detection", "\uE2B1",
                         m->config.supports_wide_color == 1 ? "Force enable" : (m->config.supports_wide_color == -1 ? "Force disable" : "Auto"), support_options,
                         [m, commit](std::string selected) {
                             m->config.supports_wide_color = selected == "Force enable" ? 1 : (selected == "Force disable" ? -1 : 0);
                             commit();
                         });


    make_dropdown_option(padded_right, "Supports HDR", "Override HDR support detection", "\uE793",
                         m->config.supports_hdr == 1 ? "Force enable" : (m->config.supports_hdr == -1 ? "Force disable" : "Auto"), support_options,
                         [m, commit](std::string selected) {
                             m->config.supports_hdr = selected == "Force enable" ? 1 : (selected == "Force disable" ? -1 : 0);
                             commit();
                         });


    add_monitor_float_field(padded_right, "SDR brightness", "SDR brightness multiplier (0–10)", "\uE706", m->config.sdrbrightness, 1.0f, 0.0f, 10.0f, [m, commit](float value) {
        m->config.sdrbrightness = value;
        commit();
    }, "A multiplier, not a percentage: 1 is 100%, 1.5 is 150%");


    add_monitor_float_field(padded_right, "SDR saturation", "SDR saturation multiplier (0–10)", "\uEB42", m->config.sdrsaturation, 1.0f, 0.0f, 10.0f, [m, commit](float value) {
        m->config.sdrsaturation = value;
        commit();
    }, "A multiplier, not a percentage: 1 is 100%, 1.5 is 150%");


    add_monitor_float_field(padded_right, "SDR min luminance", "Minimum SDR luminance", "\uF08C", m->config.sdr_min_luminance, 0.2f, 0.0f, 10000.0f, [m, commit](float value) {
        m->config.sdr_min_luminance = value;
        commit();
    }, "In nits");


    add_monitor_int_field(padded_right, "SDR max luminance", "Maximum SDR luminance", "\uE9A9", m->config.sdr_max_luminance, 80, INT_MIN, INT_MAX, [m, commit](int value) {
        m->config.sdr_max_luminance = value;
        commit();
    }, "In nits");


    add_monitor_float_field(padded_right, "Min luminance", "Minimum luminance override (-1 uses automatic)", "\uEA80", m->config.min_luminance, -1.0f, -1.0f, 10000.0f, [m, commit](float value) {
        m->config.min_luminance = value;
        commit();
    }, "In nits, -1 for automatic");


    add_monitor_int_field(padded_right, "Max luminance", "Maximum luminance override", "\uEA80", m->config.max_luminance, -1, INT_MIN, INT_MAX, [m, commit](int value) {
        m->config.max_luminance = value;
        commit();
    }, "In nits, -1 for automatic");


    add_monitor_int_field(padded_right, "Max average luminance", "Maximum average luminance override", "\uEA80", m->config.max_avg_luminance, -1, INT_MIN, INT_MAX,
                          [m, commit](int value) {
                              m->config.max_avg_luminance = value;
                              commit();
                          }, "In nits, -1 for automatic");


    make_bool(padded_right, "Disabled", "Turn this monitor off without disconnecting it", m->config.disabled, [m, commit](bool value) {
        m->config.disabled = value;
        commit();
    }, "\uE7E8");

    auto accordion = (AccordionData *) advanced->user_data;
    for (size_t i = first_advanced; i < padded_right->children.size(); i++) {
        accordion->rows.push_back(padded_right->children[i]);
        padded_right->children[i]->exists = advanced_open;
    }

    for (size_t i = first_new; i < padded_right->children.size(); i++)
        padded_right->children[i]->name = "removable";
}

// Height of the page content inside `right`. `right` holds a single FILL_SPACE
// child whose own bounds always match the viewport, so the real extent has to
// be measured from that child's children.
static float right_content_height(Container *right) {
    double content_h = 0;
    for (auto child : right->children) {
        if (!child->exists)
            continue;
        double h  = child->children.empty() ? child->real_bounds.h : actual_true_height(child) + 1;
        content_h = std::max(content_h, (child->real_bounds.y - right->real_bounds.y) + h);
    }
    return (float) (content_h + right->wanted_pad.h);
}

// Paints the children of a page that overlap the viewport (the page's parent).
// Pages can hold hundreds of rows and most of them are scrolled out of view.
static void paint_right_page_visible(Container *root, Container *c) {
    auto viewport = c->parent ? c->parent->real_bounds : c->real_bounds;
    // Leave room for anything drawn slightly outside its own bounds.
    double margin = 24 * ((MylarWindow *) root->user_data)->raw_window->dpi;
    viewport.x -= margin;
    viewport.y -= margin;
    viewport.w += margin * 2;
    viewport.h += margin * 2;
    std::vector<Container *> order;
    for (auto child : c->children)
        if (child->exists && overlaps(child->real_bounds, viewport))
            order.push_back(child);
    std::stable_sort(order.begin(), order.end(), [](Container *a, Container *b) { return a->z_index < b->z_index; });
    for (auto child : order)
        paint_outline(root, child);
}

// Lays out a settings page as a vbox and offsets it by the current scroll.
// layout_hbox calls pre_layout once with the parent's bounds before the real
// layout, so this must not clamp the stored scroll: doing so in that bogus
// pass permanently cut off the bottom of the page.
static void layout_right_page(Container *root, Container *c, const Bounds &b) {
    auto mylar    = (MylarWindow *) root->user_data;
    auto dpi      = mylar->raw_window->dpi;
    c->wanted_pad = Bounds(16 * dpi, 16 * dpi, 16 * dpi, 16 * dpi);
    c->type       = ::vbox;
    layout(root, c, b);
    c->type = ::fullycustom;

    auto d            = (RightData *) c->user_data;
    d->content_h      = right_content_height(c);
    d->overflow       = std::max(0.0f, d->content_h - (float) c->real_bounds.h);
    if (d->smooth.step(d->scroll))
        windowing::redraw(mylar->raw_window);
    d->applied_scroll = std::clamp(d->scroll, -d->overflow, 0.0f);

    for (auto child : c->children) {
        modify_all(child, 0, d->applied_scroll);
        // Only rows inside the viewport are painted.
        if (!child->when_paint && child->automatically_paint_children) {
            child->automatically_paint_children = false;
            child->when_paint = paint_right_page_visible;
        }
    }
}

// The page scrollbar: thin while scrolling, thicker while hovered or dragged,
// hidden until used and fading out after a moment without interaction.
static constexpr float rbar_thin       = 3;
static constexpr float rbar_thick      = 6;
static constexpr float rbar_hit_w      = 14; // Fits inside the page's right padding
static constexpr float rbar_margin     = 4;
static constexpr long  rbar_hold_ms    = 1000;
static constexpr long  rbar_fade_ms    = 350;

struct RightBarGeometry {
    double track_y = 0, track_h = 0, thumb_y = 0, thumb_h = 0;
};

static bool right_bar_geometry(Container *c, double dpi, RightBarGeometry &g) {
    auto d = (RightData *) c->user_data;
    if (!d || d->overflow <= 0 || d->content_h <= 0)
        return false;
    auto b    = c->real_bounds;
    g.track_y = b.y + rbar_margin * dpi;
    g.track_h = std::max(1.0, b.h - 2 * rbar_margin * dpi);
    g.thumb_h = std::min(g.track_h, std::max(30.0 * dpi, g.track_h * (b.h / d->content_h)));
    double t  = -d->applied_scroll / d->overflow;
    g.thumb_y = g.track_y + t * (g.track_h - g.thumb_h);
    return true;
}

static bool right_bar_hit(Container *root, Container *c) {
    auto dpi = swifi_window(root)->dpi;
    auto b   = c->real_bounds;
    return ((RightData *) c->user_data)->overflow > 0 && root->mouse_current_x >= b.x + b.w - rbar_hit_w * dpi &&
        root->mouse_current_x <= b.x + b.w && root->mouse_current_y >= b.y && root->mouse_current_y <= b.y + b.h;
}

static void right_bar_touch(Container *c) {
    ((RightData *) c->user_data)->last_interaction = get_current_time_in_ms();
}

// Moves the page so the thumb top lands at `thumb_y`.
static void right_bar_scroll_to(Container *root, Container *c, double thumb_y) {
    auto d = (RightData *) c->user_data;
    RightBarGeometry g;
    if (!right_bar_geometry(c, swifi_window(root)->dpi, g))
        return;
    double room = g.track_h - g.thumb_h;
    double t    = room > 0 ? std::clamp((thumb_y - g.track_y) / room, 0.0, 1.0) : 0.0;
    d->smooth.stop();
    d->scroll   = (float) (-t * d->overflow);
    swifi_redraw(root);
}

static void paint_right_scrollbar(Container *root, Container *c) {
    auto d = (RightData *) c->user_data;
    auto window = swifi_window(root);
    auto dpi    = window->dpi;
    RightBarGeometry g;
    if (!right_bar_geometry(c, dpi, g))
        return;

    const bool active = d->bar_hovered || d->bar_dragging;
    const long idle   = get_current_time_in_ms() - d->last_interaction;
    double alpha      = 1.0;
    if (!active) {
        if (idle >= rbar_hold_ms + rbar_fade_ms)
            alpha = 0;
        else if (idle > rbar_hold_ms)
            alpha = 1.0 - (double) (idle - rbar_hold_ms) / rbar_fade_ms;
    }

    // Keep repainting until the fade finishes.
    if (!active && alpha > 0 && !d->fade_timer && settings_app) {
        d->fade_timer = true;
        int delay     = idle < rbar_hold_ms ? (int) (rbar_hold_ms - idle) + 1 : 16;
        windowing::timer(settings_app, delay, [](void *data) {
            ((RightData *) data)->fade_timer = false;
            if (settings_mylar)
                windowing::redraw(settings_mylar->raw_window);
        }, d);
    }
    if (alpha <= 0)
        return;

    double w = (active ? rbar_thick : rbar_thin) * dpi;
    double x = c->real_bounds.x + c->real_bounds.w - w - rbar_margin * dpi;

    if (active) {
        RGBA track = theme.text_secondary;
        track.a *= .12 * alpha;
        drawRoundedRect(window->drawing_context, x, g.track_y, w, g.track_h, w * .5, 1.0);
        set_argb(window->drawing_context, track);
        window->drawing_context->fill();
    }

    RGBA color = theme.text_secondary;
    color.a *= (d->bar_dragging ? .8 : active ? .65 : .5) * alpha;
    drawRoundedRect(window->drawing_context, x, g.thumb_y, w, g.thumb_h, w * .5, 1.0);
    set_argb(window->drawing_context, color);
    window->drawing_context->fill();
}

// While a display change is on trial, a small dialog (a child window of
// Settings, centered over it) shows the countdown with Apply and Revert.
// Revert is the default: it has the keyboard focus when the dialog opens,
// Tab / Shift+Tab / Left / Right move the focus, Space and Return press the
// focused button, and Escape or closing the dialog reverts.
// Settings thread only.
static MylarWindow *trial_dialog = nullptr;
static int trial_dialog_focus = 1; // 0 Apply, 1 Revert

static void trial_dialog_choose(bool keep) {
    main_thread([keep]() { display_trial_finish(keep); });
}

static void trial_dialog_button(Container *parent, std::string text, int index) {
    auto b = parent->child(FILL_SPACE, FILL_SPACE);
    b->pre_layout = [text](Container *root, Container *c, const Bounds &) {
        auto mylar = (MylarWindow *) root->user_data;
        auto cr = mylar->raw_window->drawing_context;
        auto dpi = mylar->raw_window->dpi;
        Bounds bounds = draw_text(cr, 0, 0, text, 11 * dpi, false, set->font, -1, -1, theme.text, true);
        c->wanted_bounds.w = std::max<double>(bounds.w + 22 * dpi, 96 * dpi);
        c->wanted_bounds.h = bounds.h + 18 * dpi;
    };
    b->when_paint = [text, index](Container *root, Container *c) {
        auto mylar = (MylarWindow *) root->user_data;
        auto cr = mylar->raw_window->drawing_context;
        auto dpi = mylar->raw_window->dpi;
        const bool focused = trial_dialog_focus == index;
        RGBA fill = theme.button_background;
        RGBA text_color = theme.text;
        if (focused) {
            fill = theme.accent;
            if (c->state.mouse_pressing)
                fill.a *= .75;
            else if (c->state.mouse_hovering)
                fill.a *= .88;
            text_color = RGBA(1, 1, 1, 1);
        } else if (c->state.mouse_pressing) {
            fill = theme.button_pressed;
        } else if (c->state.mouse_hovering) {
            fill = theme.button_hover;
        }
        set_argb(cr, fill);
        drawRoundedRect(cr, c->real_bounds.x, c->real_bounds.y, c->real_bounds.w, c->real_bounds.h, settings_control_rounding * dpi, 1.0);
        cr->fill();
        Bounds bounds = draw_text(cr, 0, 0, text, 11 * dpi, false, set->font, -1, -1, theme.text, false);
        draw_text(cr, c->real_bounds.x + c->real_bounds.w * .5 - bounds.w * .5, c->real_bounds.y + c->real_bounds.h * .5 - bounds.h * .5, text,
                  11 * dpi, true, set->font, -1, -1, text_color, false);
    };
    b->when_clicked = [index](Container *root, Container *c) {
        trial_dialog_focus = index;
        trial_dialog_choose(index == 0);
    };
}

static void trial_dialog_fill(Container *root) {
    static const std::string title = "Keep these display settings?";
    root->type = ::vbox;
    root->when_paint = [](Container *root, Container *c) {
        auto mylar = (MylarWindow *) root->user_data;
        auto cr = mylar->raw_window->drawing_context;
        set_argb(cr, theme.background);
        cr->rectangle(c->real_bounds.x, c->real_bounds.y, c->real_bounds.w, c->real_bounds.h);
        cr->fill();
    };
    // pre_layout only runs for children, so the padding lives one level down.
    auto inner = root->child(::vbox, FILL_SPACE, FILL_SPACE);
    inner->pre_layout = [](Container *root, Container *c, const Bounds &) {
        auto dpi = ((MylarWindow *) root->user_data)->raw_window->dpi;
        c->wanted_pad = Bounds(20 * dpi, 20 * dpi, 20 * dpi, 20 * dpi);
    };

    auto message = inner->child(FILL_SPACE, FILL_SPACE);
    message->when_paint = [](Container *root, Container *c) {
        auto mylar = (MylarWindow *) root->user_data;
        auto cr = mylar->raw_window->drawing_context;
        auto dpi = mylar->raw_window->dpi;
        long seconds = (display_trial.deadline - get_current_time_in_ms() + 999) / 1000;
        seconds = std::max(seconds, 0l);
        auto text = fz("Reverting to the previous settings in {} second{}", seconds, seconds == 1 ? "" : "s");
        auto ico = draw_text(cr, 0, 0, "\uE7F4", 20 * dpi, false, icon_font, -1, -1, theme.text, false);
        draw_text(cr, c->real_bounds.x, c->real_bounds.y + 2 * dpi, "\uE7F4", 20 * dpi, true, icon_font, -1, -1, theme.accent, false);
        const float x = c->real_bounds.x + ico.w + 14 * dpi;
        auto t = draw_text(cr, x, c->real_bounds.y, title, 13 * dpi, true, set->font, -1, -1, theme.text, false);
        draw_text(cr, x, c->real_bounds.y + t.h + 6 * dpi, text, 11 * dpi, true, set->font, -1, -1, theme.text_secondary, false);
    };

    auto buttons = inner->child(::hbox, FILL_SPACE, FILL_SPACE);
    buttons->alignment = container_alignment::ALIGN_RIGHT;
    buttons->pre_layout = [](Container *root, Container *c, const Bounds &) {
        auto dpi = ((MylarWindow *) root->user_data)->raw_window->dpi;
        c->wanted_bounds.h = 34 * dpi;
        c->spacing = 8 * dpi;
    };
    trial_dialog_button(buttons, "Apply", 0);
    trial_dialog_button(buttons, "Revert", 1);
}

static void trial_dialog_close() {
    if (!trial_dialog)
        return;
    auto rw = trial_dialog->raw_window;
    trial_dialog = nullptr; // first, so its on_close knows it wasn't the user
    windowing::close_window(rw);
}

static void trial_dialog_open() {
    if (trial_dialog || !settings_app || !settings_mylar)
        return;
    trial_dialog_focus = 1;
    RawWindowSettings s;
    s.pos.w = s.pos.min_w = s.pos.max_w = 440;
    s.pos.h = s.pos.min_h = s.pos.max_h = 150;
    s.name = "Keep display settings?";
    s.app_id = "preferences-system"; // same class as Settings: restore rules leave dialogs alone
    s.parent_window = settings_mylar->raw_window;
    auto m = open_mylar_window(settings_app, WindowType::NORMAL, s);
    if (!m) return;
    m->root->user_data = m;
    trial_dialog = m;
    trial_dialog_fill(m->root);

    auto forward_key = m->raw_window->on_key_press;
    m->raw_window->on_key_press = [forward_key](RawWindow *rw, int key, bool pressed, xkb_keysym_t sym, int mods, bool is_text, std::string text) {
        switch (sym) {
            case XKB_KEY_Tab:
            case XKB_KEY_ISO_Left_Tab:
            case XKB_KEY_Left:
            case XKB_KEY_Right:
                if (pressed) {
                    trial_dialog_focus = 1 - trial_dialog_focus;
                    windowing::redraw(rw);
                }
                return true;
            case XKB_KEY_space:
            case XKB_KEY_Return:
            case XKB_KEY_KP_Enter:
                if (pressed)
                    trial_dialog_choose(trial_dialog_focus == 0);
                return true;
            case XKB_KEY_Escape:
                if (pressed)
                    trial_dialog_choose(false);
                return true;
        }
        return forward_key ? forward_key(rw, key, pressed, sym, mods, is_text, text) : false;
    };
    auto forward_close = m->raw_window->on_close;
    m->raw_window->on_close = [forward_close](RawWindow *rw) {
        // Closed by the user (titlebar, shortcut) rather than by the trial ending.
        if (trial_dialog && trial_dialog->raw_window == rw) {
            trial_dialog = nullptr;
            trial_dialog_choose(false);
        }
        if (forward_close)
            forward_close(rw);
    };
    windowing::request_keyboard_focus(m->raw_window);
}

// Opens or closes the dialog to match the trial. Settings thread only.
static void trial_dialog_sync() {
    if (display_trial.active)
        trial_dialog_open();
    else
        trial_dialog_close();
}

// Runs trial_dialog_sync on the Settings thread, from any thread.
static void trial_dialog_sync_soon() {
    if (auto app = settings_app)
        windowing::run_on_app(app, trial_dialog_sync);
}

// Redraws the countdown while a display change is on trial.
static void display_trial_watch() {
    windowing::timer(settings_app, 250, [](void *) {
        if (!settings_app || !settings_mylar)
            return;
        trial_dialog_sync();
        if (trial_dialog)
            windowing::redraw(trial_dialog->raw_window);
        display_trial_watch();
    }, nullptr);
}

// ---- Wallpaper ---------------------------------------------------------------
//
// The Display page edits either the shared wallpaper (wall_target empty) or
// one monitor's own. Editing a monitor that follows the shared wallpaper gives
// it its own (an entry in wallpaper_monitors). See wallpaper_layout.h.

static std::string wall_target; // "" = all monitors
// Held Ctrl turns scrolling over the preview into zooming (otherwise it
// scrolls the page). Tracked from the Settings window's key events.
static bool settings_ctrl_held = false;
// Scroll events reach every container under the pointer, the preview first;
// a zoom sets this so the page under it doesn't also scroll.
static bool wall_preview_ate_scroll = false;

// For Settings UI callbacks, which already hold the window's lock.
static void settings_redraw_from_ui() {
    if (settings_mylar)
        windowing::redraw(settings_mylar->raw_window);
}

// For worker threads.
static void settings_request_redraw() {
    windowing::redraw(settings_redraw_window.load());
}

// Applies wallpaper setting changes in the compositor, and saves them.
static void wall_commit(bool save = true) {
    main_thread([save]() {
        wallpaper_settings_changed();
        if (save)
            debounce_main("wallpaper_settings_save", 400, []() { settings::load_save_settings(true, set); });
    });
}

static bool wall_target_is_separate(const std::string &target) {
    return !target.empty() && wallpaper_find_override(wallpaper_parse_overrides(set->wallpaper_monitors), target);
}

// What the target shows now.
static WallParams wall_target_params(const std::string &target) {
    if (!target.empty())
        if (auto o = wallpaper_find_override(wallpaper_parse_overrides(set->wallpaper_monitors), target))
            return o->params;
    return wallpaper_shared_params(*set);
}

// Returns true when this gave the monitor its own wallpaper (the page then
// needs rebuilding, but not in the middle of a drag).
static bool wall_set_target_params(const std::string &target, const WallParams &p) {
    if (target.empty()) {
        wallpaper_set_shared_params(*set, p);
        return false;
    }
    auto overrides = wallpaper_parse_overrides(set->wallpaper_monitors);
    bool found = false;
    for (auto &o : overrides) {
        if (o.name == target) {
            o.params = p;
            found = true;
        }
    }
    if (!found)
        overrides.push_back({target, p});
    set->wallpaper_monitors = wallpaper_serialize_overrides(overrides);
    return !found;
}

// Makes the monitor follow the shared wallpaper again.
static void wall_use_shared(const std::string &target) {
    auto overrides = wallpaper_parse_overrides(set->wallpaper_monitors);
    std::erase_if(overrides, [&target](const WallMonitorOverride &o) { return o.name == target; });
    set->wallpaper_monitors = wallpaper_serialize_overrides(overrides);
    std::error_code ec;
    std::filesystem::remove(wallpaper_monitor_path(target), ec);
}

// Copies an image to the target's wallpaper file (~/.config/mylar/wall.png,
// or wall-<monitor>.png). Non-images are ignored. Blocking (runs `file`), so
// call it off the UI thread.
bool settings::set_wallpaper_from_file(const std::string &file, const std::string &target) {
    std::error_code ec;
    if (file.empty() || !std::filesystem::is_regular_file(file, ec) || ec)
        return false;
    std::string quoted = "'";
    for (char ch : file) {
        if (ch == '\'')
            quoted += "'\\''";
        else
            quoted += ch;
    }
    quoted += "'";
    auto mime = trim_newline(execAndGet(std::string("file --mime-type -b " + quoted).c_str()));
    if (!mime.starts_with("image/"))
        return false;
    main_thread([]() { wallpaper_working_indicator_start(); });
    std::filesystem::path filepath = target.empty() ? wallpaper_shared_path() : wallpaper_monitor_path(target);
    std::filesystem::create_directories(filepath.parent_path(), ec);
    ec.clear();
    if (!std::filesystem::equivalent(file, filepath, ec) || ec) {
        ec.clear();
        std::filesystem::copy_file(file, filepath, std::filesystem::copy_options::overwrite_existing, ec);
        if (ec)
            return false;
    }
    if (!target.empty() && !wall_target_is_separate(target))
        wall_set_target_params(target, wall_target_params(target));
    wall_commit();
    settings_request_redraw();
    return true;
}

// Downscaled wallpaper images for the preview, by path. Loaded on worker
// threads; the Settings thread takes finished ones in when it paints.
struct WallThumb {
    std::string stamp; // of the shown surface
    cairo_surface_t *surface = nullptr;
    int iw = 0, ih = 0; // size of the original image
    std::string loading_stamp;
    bool has_pending = false;
    cairo_surface_t *pending = nullptr;
    int pending_iw = 0, pending_ih = 0;
    std::string pending_stamp;
};

static std::mutex wall_thumbs_mutex;
static std::unordered_map<std::string, WallThumb> wall_thumbs;

static void wall_thumb_load(std::string path, std::string stamp) {
    spawn_settings_worker([path, stamp]() {
        cairo_surface_t *full = nullptr;
        load_icon_full_path(&full, path);
        cairo_surface_t *small = nullptr;
        int iw = 0, ih = 0;
        if (full) {
            iw = cairo_image_surface_get_width(full);
            ih = cairo_image_surface_get_height(full);
            const double s = std::min(1.0, 1400.0 / std::max(1, std::max(iw, ih)));
            const int tw = std::max(1, (int) std::round(iw * s));
            const int th = std::max(1, (int) std::round(ih * s));
            small = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, tw, th);
            auto cr = cairo_create(small);
            cairo_scale(cr, (double) tw / iw, (double) th / ih);
            cairo_set_source_surface(cr, full, 0, 0);
            cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
            cairo_paint(cr);
            cairo_destroy(cr);
            cairo_surface_flush(small);
            cairo_surface_destroy(full);
        }
        {
            std::lock_guard<std::mutex> lock(wall_thumbs_mutex);
            auto &t = wall_thumbs[path];
            if (t.loading_stamp != stamp) {
                if (small)
                    cairo_surface_destroy(small);
                return;
            }
            if (t.has_pending && t.pending)
                cairo_surface_destroy(t.pending);
            t.has_pending = true;
            t.pending = small;
            t.pending_iw = iw;
            t.pending_ih = ih;
            t.pending_stamp = stamp;
            t.loading_stamp.clear();
        }
        settings_request_redraw();
    });
}

// The thumbnail for path, or nullptr while it loads. Settings thread only.
static const WallThumb *wall_thumb(const std::string &path) {
    const auto stamp = wallpaper_file_stamp(path);
    std::lock_guard<std::mutex> lock(wall_thumbs_mutex);
    auto &t = wall_thumbs[path];
    if (t.has_pending) {
        if (t.surface) {
            drawing::invalidate_surface(t.surface);
            cairo_surface_destroy(t.surface);
        }
        t.surface = t.pending;
        t.iw = t.pending_iw;
        t.ih = t.pending_ih;
        t.stamp = t.pending_stamp;
        t.pending = nullptr;
        t.has_pending = false;
    }
    if (stamp != t.stamp && stamp != t.loading_stamp && !stamp.empty()) {
        t.loading_stamp = stamp;
        wall_thumb_load(path, stamp);
    }
    if (stamp.empty() || !t.surface)
        return nullptr;
    return &t;
}

// Starts decoding every wallpaper file in the background so the preview and
// crop tool have it in memory before the Display page is first opened.
// Safe to call from any thread.
void settings::preload_wallpapers() {
    std::vector<std::string> paths;
    std::error_code ec;
    for (const auto &entry : std::filesystem::directory_iterator(wallpaper_config_dir(), ec)) {
        auto name = entry.path().filename().string();
        if (name == "wall.png" || (name.rfind("wall-", 0) == 0 && entry.path().extension() == ".png"))
            paths.push_back(entry.path().string());
    }
    for (auto &path : paths) {
        const auto stamp = wallpaper_file_stamp(path);
        if (stamp.empty())
            continue;
        std::lock_guard<std::mutex> lock(wall_thumbs_mutex);
        auto &t = wall_thumbs[path];
        if (stamp == t.stamp || stamp == t.loading_stamp || (t.has_pending && stamp == t.pending_stamp))
            continue;
        t.loading_stamp = stamp;
        wall_thumb_load(path, stamp);
    }
}

// Maps the monitor layout (logical) into the preview canvas.
struct WallView {
    std::vector<WallMonitor> monitors;
    double k = 1, ox = 0, oy = 0;

    Bounds to_canvas(double x, double y, double w, double h) const {
        return Bounds(ox + x * k, oy + y * k, w * k, h * k);
    }
};

static WallView wall_view(Container *c, double dpi) {
    WallView v;
    v.monitors = wallpaper_monitors_copy();
    if (v.monitors.empty())
        return v;
    double x0 = v.monitors[0].x, y0 = v.monitors[0].y, x1 = x0 + v.monitors[0].w, y1 = y0 + v.monitors[0].h;
    for (auto &m : v.monitors) {
        x0 = std::min(x0, m.x), y0 = std::min(y0, m.y);
        x1 = std::max(x1, m.x + m.w), y1 = std::max(y1, m.y + m.h);
    }
    const double uw = std::max(1.0, x1 - x0), uh = std::max(1.0, y1 - y0);
    auto b = c->real_bounds;
    // Leave room around the monitors to show the cropped off parts of the image.
    v.k = std::min(b.w * .62 / uw, (b.h - 30 * dpi) * .72 / uh);
    v.ox = b.x + b.w * .5 - (x0 + uw * .5) * v.k;
    v.oy = b.y + 12 * dpi + (b.h - 12 * dpi) * .5 - (y0 + uh * .5) * v.k;
    return v;
}

static bool wall_monitor_targeted(const WallMonitor &m, const std::vector<WallMonitorOverride> &overrides) {
    if (wall_target.empty())
        return !wallpaper_find_override(overrides, m.name);
    return m.name == wall_target;
}

// The targeted monitor a pointer position edits: the one under it, else the
// first targeted one.
static const WallMonitor *wall_edit_monitor(const WallView &v, double x, double y) {
    auto overrides = wallpaper_parse_overrides(set->wallpaper_monitors);
    const WallMonitor *first = nullptr;
    for (auto &m : v.monitors) {
        if (!wall_monitor_targeted(m, overrides))
            continue;
        if (!first)
            first = &m;
        if (bounds_contains(v.to_canvas(m.x, m.y, m.w, m.h), x, y))
            return &m;
    }
    return first;
}

static void wall_draw_image(drawing::Context *cr, const WallThumb *thumb, Bounds r, double alpha) {
    const double tw = cairo_image_surface_get_width(thumb->surface);
    const double th = cairo_image_surface_get_height(thumb->surface);
    if (r.w <= 0 || r.h <= 0 || tw <= 0 || th <= 0)
        return;
    cr->save();
    cr->new_path();
    cr->rectangle(r.x, r.y, r.w, r.h);
    cr->clip_path();
    cr->translate(r.x, r.y);
    cr->scale(r.w / tw, r.h / th);
    cr->set_source_surface(thumb->surface, 0, 0);
    cr->paint_source(alpha);
    cr->restore();
}

struct WallPreviewData : UserData {
    bool dragging = false;
    bool became_separate = false;
    WallParams start;
    WallRect area;
    double image_w = 0, image_h = 0; // image rect size (logical) at drag start
    double iw = 0, ih = 0; // image size, for keeping the crop inside it
};

static void paint_wall_preview(Container *root, Container *c) {
    auto window = swifi_window(root);
    auto cr = window->drawing_context;
    auto dpi = window->dpi;
    auto b = c->real_bounds;
    paint_smooth_frame(cr, b, settings_control_rounding * dpi, theme.dropdown_background, theme.dropdown_border);

    auto v = wall_view(c, dpi);
    if (v.monitors.empty())
        return;
    auto overrides = wallpaper_parse_overrides(set->wallpaper_monitors);
    auto shared = wallpaper_shared_params(*set);

    cr->save();
    cr->new_path();
    cr->rectangle(b.x + 1, b.y + 1, b.w - 2, b.h - 2);
    cr->clip_path();

    // Ghost of the whole image behind the targeted monitors: the parts the crop cuts off.
    std::vector<std::string> ghosted;
    for (auto &m : v.monitors) {
        if (!wall_monitor_targeted(m, overrides))
            continue;
        auto res = wallpaper_resolve(shared, set->wallpaper_span, overrides, v.monitors, m);
        if (res.params.mode == "Tile" || res.params.mode == "Stretch")
            continue;
        char id[160];
        snprintf(id, sizeof(id), "%.1f %.1f %.1f %.1f", res.area.x, res.area.y, res.area.w, res.area.h);
        if (std::find(ghosted.begin(), ghosted.end(), id) != ghosted.end())
            continue;
        ghosted.push_back(id);
        auto thumb = wall_thumb(res.path);
        if (!thumb)
            continue;
        auto r = wallpaper_image_rect(res.params, thumb->iw, thumb->ih, res.area.w, res.area.h, res.pixel_scale);
        auto cb = v.to_canvas(res.area.x + r.x, res.area.y + r.y, r.w, r.h);
        wall_draw_image(cr, thumb, cb, .3);
        drawRoundedRect(cr, cb.x, cb.y, cb.w, cb.h, 0, std::max(1.0, (double) std::floor(dpi)));
        set_argb(cr, RGBA(1, 1, 1, .25));
        cr->stroke();
    }

    // Each monitor as it will look.
    for (auto &m : v.monitors) {
        auto res = wallpaper_resolve(shared, set->wallpaper_span, overrides, v.monitors, m);
        auto mb = v.to_canvas(m.x, m.y, m.w, m.h);
        cr->save();
        cr->new_path();
        cr->rectangle(mb.x, mb.y, mb.w, mb.h);
        cr->clip_path();
        cr->set_source_rgba(0, 0, 0, 1);
        cr->new_path();
        cr->rectangle(mb.x, mb.y, mb.w, mb.h);
        cr->fill();
        if (set->draw_wallpaper) {
            if (auto thumb = wall_thumb(res.path)) {
                for (auto &r : wallpaper_area_rects(res.params, thumb->iw, thumb->ih, res.area.w, res.area.h, res.pixel_scale)) {
                    auto rb = v.to_canvas(res.area.x + r.x, res.area.y + r.y, r.w, r.h);
                    if (rb.x > mb.x + mb.w || rb.y > mb.y + mb.h || rb.x + rb.w < mb.x || rb.y + rb.h < mb.y)
                        continue;
                    wall_draw_image(cr, thumb, rb, 1.0);
                }
            }
        }
        cr->restore();

        const bool targeted = wall_monitor_targeted(m, overrides);
        const double line = targeted ? std::max(2.0, (double) std::floor(2 * dpi)) : std::max(1.0, (double) std::floor(dpi));
        drawRoundedRect(cr, mb.x, mb.y, mb.w, mb.h, 0, line);
        set_argb(cr, targeted ? theme.accent : RGBA(1, 1, 1, .35));
        cr->stroke();

        if (v.monitors.size() > 1) {
            std::string label = m.name;
            if (wallpaper_find_override(overrides, m.name))
                label += " (own)";
            double tw = swifi_text_width(root, label, 9);
            Bounds pill(mb.x + 6 * dpi, mb.y + mb.h - 22 * dpi, tw + 12 * dpi, 16 * dpi);
            if (pill.w < mb.w - 8 * dpi && pill.h < mb.h - 8 * dpi) {
                drawRoundedRect(cr, pill.x, pill.y, pill.w, pill.h, 4 * dpi, 1.0);
                set_argb(cr, RGBA(0, 0, 0, .55));
                cr->fill();
                swifi_text(root, pill, label, 9, RGBA(1, 1, 1, .95), true);
            }
        }
    }
    cr->restore();

    std::string hint = v.monitors.size() > 1 ? "Drag to move, Ctrl+scroll to zoom, click a monitor to select it" : "Drag to move, Ctrl+scroll to zoom";
    swifi_text(root, Bounds(b.x + 10 * dpi, b.y + 6 * dpi, b.w - 20 * dpi, 18 * dpi), hint, 9, theme.text_secondary);
}

// The Custom parameters to start editing the target from, with the area and
// image size the pointer edits in. False when the image is not loaded yet.
static bool wall_edit_start(Container *root, Container *c, WallParams &p, WallRect &area, double &iw, double &ih, double &pixel_scale) {
    auto dpi = swifi_window(root)->dpi;
    auto v = wall_view(c, dpi);
    auto m = wall_edit_monitor(v, root->mouse_current_x, root->mouse_current_y);
    if (!m)
        return false;
    auto overrides = wallpaper_parse_overrides(set->wallpaper_monitors);
    auto res = wallpaper_resolve(wallpaper_shared_params(*set), set->wallpaper_span, overrides, v.monitors, *m);
    auto thumb = wall_thumb(res.path);
    if (!thumb)
        return false;
    iw = thumb->iw, ih = thumb->ih;
    area = res.area;
    pixel_scale = res.pixel_scale;
    p = wallpaper_custom_from(wall_target_params(wall_target), iw, ih, area.w, area.h, pixel_scale);
    return true;
}

static void make_wall_preview(Container *parent) {
    static const int height = 260;
    auto c = parent->child(FILL_SPACE, height);
    c->name = "wallpaper_preview";
    c->receive_events_even_if_obstructed = true;
    c->parent_bounds_limit_input_bounds = false;
    c->user_data = new WallPreviewData;
    c->pre_layout = [](Container *root, Container *c, const Bounds &) {
        c->wanted_bounds.h = height * swifi_window(root)->dpi;
    };
    c->when_paint = paint_wall_preview;
    focus_skip(c); // Picks the monitor under the pointer
    c->when_drag_end_is_click = false;
    c->when_clicked = [](Container *root, Container *c) {
        auto v = wall_view(c, swifi_window(root)->dpi);
        if (v.monitors.size() < 2)
            return;
        std::string picked;
        for (auto &m : v.monitors)
            if (bounds_contains(v.to_canvas(m.x, m.y, m.w, m.h), root->mouse_current_x, root->mouse_current_y))
                picked = m.name;
        if (picked == wall_target)
            return;
        wall_target = picked;
        display_trial.page_stale = true;
        swifi_redraw(root);
    };
    c->when_drag_start = [](Container *root, Container *c) {
        auto data = (WallPreviewData *) c->user_data;
        WallParams p;
        double iw, ih, pixel_scale;
        data->dragging = wall_edit_start(root, c, p, data->area, iw, ih, pixel_scale);
        if (!data->dragging)
            return;
        p = wallpaper_clamp_custom(p, iw, ih, data->area.w, data->area.h);
        data->start = p;
        data->iw = iw;
        data->ih = ih;
        auto r = wallpaper_image_rect(p, iw, ih, data->area.w, data->area.h, pixel_scale);
        data->image_w = r.w;
        data->image_h = r.h;
        data->became_separate = wall_set_target_params(wall_target, p);
        wall_commit(false);
    };
    c->when_drag = [](Container *root, Container *c) {
        auto data = (WallPreviewData *) c->user_data;
        if (!data->dragging || data->image_w <= 0 || data->image_h <= 0)
            return;
        auto v = wall_view(c, swifi_window(root)->dpi);
        auto p = data->start;
        p.center_x -= (root->mouse_current_x - root->mouse_initial_x) / v.k / data->image_w;
        p.center_y -= (root->mouse_current_y - root->mouse_initial_y) / v.k / data->image_h;
        p = wallpaper_clamp_custom(p, data->iw, data->ih, data->area.w, data->area.h);
        wall_set_target_params(wall_target, p);
        wall_commit(false);
        swifi_redraw(root);
    };
    c->when_drag_end = [](Container *root, Container *c) {
        auto data = (WallPreviewData *) c->user_data;
        if (!data->dragging)
            return;
        data->dragging = false;
        if (data->became_separate)
            display_trial.page_stale = true;
        data->became_separate = false;
        wall_commit();
        swifi_redraw(root);
    };
    c->when_fine_scrolled = [](Container *root, Container *c, double scroll_x, double scroll_y, bool touchpad) {
        if (!settings_ctrl_held || scroll_y == 0)
            return;
        wall_preview_ate_scroll = true;
        WallParams p;
        WallRect area;
        double iw, ih, pixel_scale;
        if (!wall_edit_start(root, c, p, area, iw, ih, pixel_scale))
            return;
        auto v = wall_view(c, swifi_window(root)->dpi);
        // Keep the image point under the pointer where it is.
        const double mx = (root->mouse_current_x - v.ox) / v.k - area.x;
        const double my = (root->mouse_current_y - v.oy) / v.k - area.y;
        auto before = wallpaper_image_rect(p, iw, ih, area.w, area.h, pixel_scale);
        if (before.w <= 0 || before.h <= 0)
            return;
        const double u = (mx - before.x) / before.w;
        const double w = (my - before.y) / before.h;
        p = wallpaper_clamp_custom(p, iw, ih, area.w, area.h);
        p.zoom = std::clamp(p.zoom * std::exp(scroll_y * .012), 1.0, 40.0);
        auto after = wallpaper_image_rect(p, iw, ih, area.w, area.h, pixel_scale);
        p.center_x = (area.w * .5 - mx) / after.w + u;
        p.center_y = (area.h * .5 - my) / after.h + w;
        p = wallpaper_clamp_custom(p, iw, ih, area.w, area.h);
        if (wall_set_target_params(wall_target, p))
            display_trial.page_stale = true;
        wall_commit();
        swifi_redraw(root);
    };
}

// Local file paths in a text/uri-list ("file:///a%20b" -> "/a b").
static std::vector<std::string> paths_from_uri_list(const std::string &uri_list) {
    std::vector<std::string> paths;
    std::istringstream in(uri_list);
    std::string line;
    while (std::getline(in, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
            line.pop_back();
        if (line.empty() || line[0] == '#')
            continue;
        std::string rest;
        const std::string scheme = "file://";
        if (line.compare(0, scheme.size(), scheme) == 0) {
            // Skip an optional host ("file://localhost/path").
            size_t path_start = line.find('/', scheme.size());
            if (path_start == std::string::npos)
                continue;
            rest = line.substr(path_start);
        } else if (line[0] == '/') {
            rest = line;
        } else {
            continue;
        }
        std::string path;
        for (size_t i = 0; i < rest.size(); i++) {
            if (rest[i] == '%' && i + 2 < rest.size() && std::isxdigit((unsigned char) rest[i + 1]) &&
                std::isxdigit((unsigned char) rest[i + 2])) {
                path += (char) std::stoi(rest.substr(i + 1, 2), nullptr, 16);
                i += 2;
            } else {
                path += rest[i];
            }
        }
        paths.push_back(path);
    }
    return paths;
}

// Files dropped anywhere on the Display page: the first image becomes the
// wallpaper, as if it had been picked with "Choose file".
static void wallpaper_file_dropped(std::string uri_list) {
    std::string target = wall_target;
    spawn_settings_worker([uri_list, target]() {
        for (const auto &path : paths_from_uri_list(uri_list))
            if (settings::set_wallpaper_from_file(path, target))
                break;
    });
}

// Shown over the Display or SDDM page while files are dragged over the window.
static void paint_wallpaper_drop_hint(Container *root, Container *right) {
    auto window = swifi_window(root);
    bool on_display = container_by_name("display_wallpaper_row", right) != nullptr;
    bool on_sddm = container_by_name("sddm_page", right) != nullptr;
    if (!window->dnd_hovering || (!on_display && !on_sddm))
        return;
    auto cr = window->drawing_context;
    auto dpi = window->dpi;
    auto b = right->real_bounds;
    double inset = 10 * dpi;
    Bounds r(b.x + inset, b.y + inset, b.w - inset * 2, b.h - inset * 2);
    if (r.w <= 0 || r.h <= 0)
        return;
    RGBA fill = theme.accent;
    fill.a = .12;
    drawRoundedRect(cr, r.x, r.y, r.w, r.h, 10 * dpi, 1.0);
    set_argb(cr, fill);
    cr->fill();
    drawRoundedRect(cr, r.x, r.y, r.w, r.h, 10 * dpi, std::max(1.0, (double) std::floor(2 * dpi)));
    set_argb(cr, theme.accent);
    cr->stroke();

    std::string text = "Drop image to set as wallpaper";
    double size = 14;
    double tw = swifi_text_width(root, text, size);
    double th = swifi_text_height(root, text, size, true);
    double pad = 14 * dpi;
    Bounds pill(r.x + r.w * .5 - tw * .5 - pad, r.y + r.h * .5 - th * .5 - pad * .7, tw + pad * 2, th + pad * 1.4);
    drawRoundedRect(cr, pill.x, pill.y, pill.w, pill.h, 8 * dpi, 1.0);
    set_argb(cr, theme.accent);
    cr->fill();
    swifi_text(root, pill, text, size, RGBA(1, 1, 1, 1), true);
}

static void make_wallpaper_row(Container *padded_right) {
    auto monitors = wallpaper_monitors_copy();
    if (!wall_target.empty()) {
        bool exists = false;
        for (auto &m : monitors)
            exists |= m.name == wall_target;
        if (!exists)
            wall_target.clear();
    }
    const bool many = monitors.size() > 1;

    make_bool_with_button(padded_right, "Draw wallpaper", "Can drag and drop image here", set->draw_wallpaper, [](bool c) {
        set->draw_wallpaper = c;
        main_thread([]() { damage_all(); });
    }, "Choose file", []() {
        static std::atomic<bool> chooser_open = false;
        if (chooser_open.exchange(true))
            return;

        std::string target = wall_target;
        spawn_settings_worker([target]() {
            settings::set_wallpaper_from_file(trim_newline(choose_file()), target);
            chooser_open = false;
        });
    }, "\uE8b9");
    padded_right->children.back()->name = "display_wallpaper_row";

    if (many) {
        std::vector<std::string> options = {"All monitors"};
        for (auto &m : monitors)
            options.push_back(m.name);
        make_dropdown_option(padded_right, "Wallpaper for", "Monitors can each have their own wallpaper", "\uE7F4",
                             wall_target.empty() ? "All monitors" : wall_target, options, [](std::string selected) {
            wall_target = selected == "All monitors" ? "" : selected;
            display_trial.page_stale = true;
            settings_redraw_from_ui();
        }, []() { return wall_target.empty() ? std::string("All monitors") : wall_target; });
    }

    make_dropdown_option(padded_right, "Scaling", "How the image fits the screen", "\uE740", wall_target_params(wall_target).mode,
                         wallpaper_scaling_modes, [](std::string selected) {
        auto p = wall_target_params(wall_target);
        if (selected == p.mode)
            return;
        if (selected == "Custom") {
            // Start from what the previous mode showed.
            auto v_monitors = wallpaper_monitors_copy();
            auto overrides = wallpaper_parse_overrides(set->wallpaper_monitors);
            for (auto &m : v_monitors) {
                if (!wall_monitor_targeted(m, overrides))
                    continue;
                auto res = wallpaper_resolve(wallpaper_shared_params(*set), set->wallpaper_span, overrides, v_monitors, m);
                if (auto thumb = wall_thumb(res.path))
                    p = wallpaper_custom_from(p, thumb->iw, thumb->ih, res.area.w, res.area.h, res.pixel_scale);
                break;
            }
        }
        p.mode = selected;
        if (wall_set_target_params(wall_target, p))
            display_trial.page_stale = true;
        wall_commit();
        settings_redraw_from_ui();
    }, []() { return wall_target_params(wall_target).mode; });

    if (many && wall_target.empty()) {
        make_bool(padded_right, "Span across monitors", "Stretch one image over every monitor using the shared wallpaper", set->wallpaper_span, [](bool on) {
            set->wallpaper_span = on;
            wall_commit();
            settings_redraw_from_ui();
        }, "\uEBC6");
    }

    if (wall_target_is_separate(wall_target)) {
        make_button_option(padded_right, "Own wallpaper", wall_target + " doesn't follow the shared wallpaper", "Use shared", []() {
            wall_use_shared(wall_target);
            wall_commit();
            display_trial.page_stale = true;
            settings_redraw_from_ui();
        }, "\uE8B9");
    }

    make_vert_space(padded_right, 8);
    make_wall_preview(padded_right);
    make_vert_space(padded_right, 8);

    make_button_option(padded_right, "Crop", "Drag the preview to move the image, Ctrl+scroll to zoom", "Reset", []() {
        if (wall_set_target_params(wall_target, WallParams()))
            display_trial.page_stale = true;
        wall_commit();
        settings_redraw_from_ui();
    }, "\uE7A8");


    // Same mechanism as the dock's brightness popup (brightnessctl). The dock
    // keeps brightness_level current, so reading it here doesn't block.
    make_slider(padded_right, "Brightness", "Screen backlight level", std::clamp(brightness_level / 100.0f, 0.0f, 1.0f), [](float value) {
        set_brightness(std::round(value * 100.0f));
    }, 0, "\uE706");

    make_bool(padded_right, "Night light", "Warmer colors that cut down on blue light", nightlight_on, [](bool value) {
        if (value != nightlight_on)
            toggle_nightlight();
    }, "\uE708");

    auto nightlight_slider_value = [](int kelvin) {
        return std::clamp((float) (nightlight_max_kelvin - kelvin) / (nightlight_max_kelvin - nightlight_min_kelvin), 0.0f, 1.0f);
    };
    make_slider(padded_right, "Night light temperature", "Higher values make the screen warmer",
        nightlight_slider_value(nightlight_temperature()),
        [](float value) {
            set_nightlight_temperature(nightlight_max_kelvin - std::clamp(value, 0.0f, 1.0f) * (nightlight_max_kelvin - nightlight_min_kelvin));
        }, 0, "\uE790", nightlight_slider_value(ConfigSettings().nightlight_temperature));
}

static void fill_display_settings(Container *root) {
    display_trial.page_stale = false;
    auto right = container_by_name("settings_right", root);
    if (!right)
        return;
    for (auto child: right->children)
        delete child;
    right->children.clear();

    right->pre_layout = layout_right_page;
    auto padded_right = right->child(FILL_SPACE, FILL_SPACE);

    make_section_title(padded_right, "Display Settings");
    
    make_vert_space(padded_right, 10);

    padded_right->name = "display_page";

    std::vector<MonitorOption *> options;
    auto data = execAndGet(std::string("hyprctl monitors").c_str());
    // no/checkin
    //auto data = execAndGet(std::string("cat /tmp/out").c_str());

    auto p = LineParser(data);
    MonitorOption *option = nullptr;
    while (p.has_another_line()) {
        defer(p.next_line());

        p.eat_blank();
        
        std::string_view s = p.eat_until_line_end();
        
        auto l = LineParser(s);

        try {
            if (s.starts_with("Monitor")) {
                option = new MonitorOption;
                options.push_back(option);
                
                l.eat_until(" ");
                option->name = trim(std::string(*l.eat_until(" ")));
            } else if (s.contains(" at ")) {
                option->w = std::stof(trim(std::string(*l.eat_until("x"))));
                option->h = std::stof(trim(std::string(*l.eat_until("@"))));
                option->fps = std::stof(trim(std::string(*l.eat_until(" "))));
                l.eat_until(" ");
                option->x = std::stoi(trim(std::string(*l.eat_until("x"))));
                option->y = std::stoi(trim(std::string(l.eat_until_line_end())));
            } else if (s.starts_with("scale")) {
                l.eat_until(": ");
                option->scale = std::stof(trim(std::string(l.eat_until_line_end())));
            } else if (s.starts_with("transform")) {
                l.eat_until(": ");
                option->transform = std::stoi(trim(std::string(l.eat_until_line_end())));
            } else if (s.starts_with("availableModes")) {
                l.eat_until(": ");
                while (l.not_end_of_line()) {
                    auto f = l.eat_until(" ");
                    option->option.push_back(std::string(*f));
                }
            }
        } catch (...) {
            main_thread([]() {
                notify("Failed to parse a monitor please report this issue on github!");
            });
            return;
        }
    }

    for (auto o : options) {
        auto existing = std::ranges::find(set->monitors, o->name, &MylarMonitor::output);
        if (existing != set->monitors.end()) {
            o->config = *existing;
            continue;
        }

        o->config.output    = o->name;
        o->config.mode      = fz("{}x{}@{:.2f}", o->w, o->h, o->fps);
        o->config.scale     = fz("{}", o->scale);
        o->config.transform = o->transform;
    }

    if (options.size() > 0) {
        make_screen_positioner(padded_right, options, [options](std::string monitor_name) {
            for (auto o : options) {
                if (o->name == monitor_name) {
                    change_display_options(o);
                    return;
                }
            }
        });
        
        make_vert_space(padded_right, 10);
    }

    make_wallpaper_row(padded_right);

    make_bool_with_button(padded_right, "Desktop Icons", "", set->desktop_icons, [](bool c) {
        set->desktop_icons = c;
        main_thread([]() {
            desktop_icons::stop();
            desktop_icons::start();
            damage_all();
        });
    }, "Change folder", []() {
        static std::atomic<bool> chooser_open = false;
        if (chooser_open.exchange(true))
            return;

        spawn_settings_worker([]() {
            const auto file = trim_newline(choose_file(true));
            main_thread([file]() {
                    notify(file);
            });
            
            if (!file.empty() && std::filesystem::exists(file) && std::filesystem::is_directory(file)) {
                main_thread([file]() {
                    set->desktop_folder = file;
                    desktop_icons::stop();
                    desktop_icons::start();
                    settings::load_save_settings(true, set); // save
                });
            }

            chooser_open = false;
        });
    }, "\uec6c");

    make_vert_space(padded_right, 10);
        
    for (auto m : options) {
        change_display_options(m);
        break;
    }

}

static void save_multitasking_settings() {
    main_thread([]() {
        settings::load_save_settings(true, set);
        hypriso->apply_workspace_settings();
        hypriso->generate_mylar_hyprland_config();
    });
}

// Empties settings_right and returns the container a page fills.
static Container *settings_begin_page(Container *root) {
    auto right = container_by_name("settings_right", root);
    if (!right)
        return nullptr;
    for (auto child: right->children)
        delete child;
    right->children.clear();

    right->pre_layout = layout_right_page;
    return right->child(FILL_SPACE, FILL_SPACE);
}

static void fill_multitasking_settings(Container *root) {
    auto padded_right = settings_begin_page(root);
    if (!padded_right)
        return;
    make_section_title(padded_right, "Multitasking Settings");
    make_vert_space(padded_right, 10);
    make_button_group(padded_right, "App switching",
        "Choose which windows appear in the app switcher",
        {"All workspaces", "Current workspace"}, [](std::string selected) {
            set->app_switcher_current_workspace_only = selected == "Current workspace";
            save_multitasking_settings();
        }, set->app_switcher_current_workspace_only ? "Current workspace" : "All workspaces", "\uE8AB");
    make_button_group(padded_right, "Workspace behavior",
        "Create workspaces as needed or keep a fixed number",
        {"Dynamic", "Fixed"}, [](std::string selected) {
            set->dynamic_workspaces = selected == "Dynamic";
            save_multitasking_settings();
        }, set->dynamic_workspaces ? "Dynamic" : "Fixed", "\uE8A9");
    make_reset_textfield(padded_right, "Number of workspaces",
        "Per monitor in Fixed mode (1–20); occupied extras are kept", "\uE80A", true,
        std::to_string(set->fixed_workspace_count), "4", [](std::string value) {
            try {
                size_t end = 0;
                const int count = std::stoi(value, &end);
                if (end != value.size() || count < 1 || count > 20)
                    throw std::out_of_range("workspace count");
                set->fixed_workspace_count = count;
                save_multitasking_settings();
            } catch (const std::exception&) {
                main_thread([]() {
                    notify("Enter a workspace count from 1 to 20");
                });
            }
        }, nullptr, "Whole number from 1 to 20");
    make_button_group(padded_right, "Multiple monitors",
        "Switch workspaces independently or on all monitors together",
        {"Independent", "Together"}, [](std::string selected) {
            set->workspaces_span_monitors = selected == "Together";
            save_multitasking_settings();
        }, set->workspaces_span_monitors ? "Together" : "Independent", "\uEBC6");
    make_bool(padded_right, "Hotcorners", "Activates shortcuts when an edge is hit", set->hotcorners, [](bool value) {
        set->hotcorners = value;
        save_multitasking_settings();
    }, "\ue8b0");
    make_bool(padded_right, "Snap windows",
        "Drag a window to a screen edge or corner to fill half or a quarter of the screen", set->resize_at_screen_edges, [](bool value) {
            set->resize_at_screen_edges = value;
            save_multitasking_settings();
        }, "\uF246");
    make_bool(padded_right, "Attempt to respect CSD requests",
        "Don't add a titlebar to windows that ask to draw their own decorations", set->respect_csd_requests, [](bool value) {
            set->respect_csd_requests = value;
            save_multitasking_settings();
        }, "\uE737");
}

static void fill_desktop_settings(Container *root) {
    auto padded_right = settings_begin_page(root);
    if (!padded_right)
        return;

    make_section_title(padded_right, "Desktop Settings");
    
    make_vert_space(padded_right, 10);

    make_button_option(padded_right, "Icon Cache", "Re-scan installed icons", "Re-scan Icons", []() {
        if (icon_cache_rescan_in_progress)
            return;

        icon_cache_rescan_in_progress = true;
        damage_all();

        spawn_settings_worker([]() {
            bool succeeded = true;
            try {
                icon_cache_generate();
                icon_cache_load();
            } catch (...) {
                succeeded = false;
            }

            main_thread([succeeded]() {
                icon_cache_rescan_in_progress = false;
                if (!succeeded)
                    notify("Failed to re-scan icons");
                damage_all();
            });
        });
    }, "\uE71D", []() { return !icon_cache_rescan_in_progress; });

    make_dropdown_option(padded_right, "Overview", "Change layout type", "\uE8A9", set->overview_layout_type, {"Grid", "Adaptive"}, [](std::string new_type) {
        set->overview_layout_type = new_type;
    });
    
    make_dropdown_option(padded_right, "Theme", "Choose the settings appearance", "\uE790", set->dark_theme ? "Dark" : "Light", {"Light", "Dark"}, [](std::string selected) {
        set->dark_theme = selected == "Dark";
        theme = set->dark_theme ? dark_theme : light_theme;
        settings::load_save_settings(true, set);
        if (settings_mylar)
            windowing::redraw(settings_mylar->raw_window);
    });
    
    auto font_field = make_reset_textfield(padded_right, "Font", "User interface font", "\uE8D2", false, set->font, ConfigSettings().font, [](std::string value) {
        set->font = value;
    }, []() { return resolved_font_family(set->font); });
    attach_font_completions(font_field, [](std::string value) {
        set->font = value;
    });

    make_vert_space(padded_right, 24);
    make_section_title(padded_right, "Blur");
    make_vert_space(padded_right, 10);

    auto apply_blur_now = []() {
        main_thread([]() {
            settings::load_save_settings(true, set);
            hypriso->generate_mylar_hyprland_config();
        });
    };
    auto apply_blur_debounced = []() {
        debounce_main("blur_settings", 200, []() {
            settings::load_save_settings(true, set);
            hypriso->generate_mylar_hyprland_config();
        });
    };

    const ConfigSettings defaults;

    make_bool(padded_right, "Window blur", "Blur backgrounds behind translucent windows and surfaces", set->blur_enabled,
        [apply_blur_now](bool value) {
            set->blur_enabled = value;
            apply_blur_now();
        }, "\uE771");

    struct BlurVariantEntry {
        const char *label;
        const char *id;
    };
    static const std::array<BlurVariantEntry, 11> blur_variants = {{
        {"Kawase", "kawase"},
        {"Frost", "frost"},
        {"Acrylic", "acrylic"},
        {"Prism", "prism"},
        {"Haze", "haze"},
        {"Aurora", "aurora"},
        {"Ripple", "ripple"},
        {"Drops", "drops"},
        {"Water", "water"},
        {"Heat shimmer", "heat_shimmer"},
        {"Fluid jar", "fluid_jar"},
    }};
    std::vector<std::string> variant_labels;
    variant_labels.reserve(blur_variants.size());
    std::string selected_variant = blur_variants[0].label;
    for (const auto &v : blur_variants) {
        variant_labels.push_back(v.label);
        if (set->blur_variant == v.id)
            selected_variant = v.label;
    }
    make_dropdown_option(padded_right, "Blur style", "Blur effect style; animated styles use more GPU", "\uE790",
        selected_variant, variant_labels, [apply_blur_now](std::string selected) {
            for (const auto &v : blur_variants) {
                if (selected == v.label) {
                    set->blur_variant = v.id;
                    apply_blur_now();
                    return;
                }
            }
        });

    auto norm = [](float v, float lo, float hi) {
        return std::clamp((v - lo) / (hi - lo), 0.0f, 1.0f);
    };

    make_slider(padded_right, "Blur size", "Blur radius distance (1–20)",
        norm((float) set->blur_size, 1.0f, 20.0f),
        [apply_blur_debounced](float value) {
            set->blur_size = std::clamp((int) std::round(1.0f + value * 19.0f), 1, 20);
            apply_blur_debounced();
        }, 0, "\uE740", norm((float) defaults.blur_size, 1.0f, 20.0f));

    make_slider(padded_right, "Blur passes", "Number of blur passes (1–6); more passes look smoother but use more GPU",
        norm((float) set->blur_passes, 1.0f, 6.0f),
        [apply_blur_debounced](float value) {
            set->blur_passes = std::clamp((int) std::round(1.0f + value * 5.0f), 1, 6);
            apply_blur_debounced();
        }, 5, "\uE81E", norm((float) defaults.blur_passes, 1.0f, 6.0f));

    make_slider(padded_right, "Vibrancy", "Increase saturation of blurred colors",
        norm(set->blur_vibrancy, 0.0f, 1.0f),
        [apply_blur_debounced](float value) {
            set->blur_vibrancy = std::clamp(value, 0.0f, 1.0f);
            apply_blur_debounced();
        }, 0, "\uEB42", norm(defaults.blur_vibrancy, 0.0f, 1.0f));

    static bool blur_advanced_open = false;
    auto blur_advanced = make_accordion(padded_right, "Advanced",
        "Contrast, brightness, noise, X-ray, popups and special workspace blur", "\uE9E9", &blur_advanced_open);
    const size_t first_blur_advanced = padded_right->children.size();

    make_slider(padded_right, "Contrast", "Contrast modulation applied to blurred backgrounds",
        norm(set->blur_contrast, 0.0f, 2.0f),
        [apply_blur_debounced](float value) {
            set->blur_contrast = std::clamp(value * 2.0f, 0.0f, 2.0f);
            apply_blur_debounced();
        }, 0, "\uE793", norm(defaults.blur_contrast, 0.0f, 2.0f));

    make_slider(padded_right, "Brightness", "Brightness modulation applied to blurred backgrounds",
        norm(set->blur_brightness, 0.0f, 2.0f),
        [apply_blur_debounced](float value) {
            set->blur_brightness = std::clamp(value * 2.0f, 0.0f, 2.0f);
            apply_blur_debounced();
        }, 0, "\uE706", norm(defaults.blur_brightness, 0.0f, 2.0f));

    make_slider(padded_right, "Noise", "Subtle grain added to blurred backgrounds",
        norm(set->blur_noise, 0.0f, 0.25f),
        [apply_blur_debounced](float value) {
            set->blur_noise = std::clamp(value * 0.25f, 0.0f, 0.25f);
            apply_blur_debounced();
        }, 0, "\uE9D2", norm(defaults.blur_noise, 0.0f, 0.25f));

    make_slider(padded_right, "Vibrancy darkness", "Strength of vibrancy on dark blurred areas",
        norm(set->blur_vibrancy_darkness, 0.0f, 1.0f),
        [apply_blur_debounced](float value) {
            set->blur_vibrancy_darkness = std::clamp(value, 0.0f, 1.0f);
            apply_blur_debounced();
        }, 0, "\uF08C", norm(defaults.blur_vibrancy_darkness, 0.0f, 1.0f));

    make_bool(padded_right, "Ignore window opacity", "Keep full blur strength even on semi-transparent windows", set->blur_ignore_opacity,
        [apply_blur_now](bool value) {
            set->blur_ignore_opacity = value;
            apply_blur_now();
        }, "\uE737");

    make_bool(padded_right, "X-ray blur", "Floating windows blur the wallpaper directly, ignoring tiled windows behind them", set->blur_xray,
        [apply_blur_now](bool value) {
            set->blur_xray = value;
            apply_blur_now();
        }, "\uE2B1");

    make_bool(padded_right, "Blur popups", "Apply background blur behind menus and popups", set->blur_popups,
        [apply_blur_now](bool value) {
            set->blur_popups = value;
            apply_blur_now();
        }, "\uE8A9");

    make_bool(padded_right, "Blur special workspace", "Apply background blur behind the special workspace", set->blur_special,
        [apply_blur_now](bool value) {
            set->blur_special = value;
            apply_blur_now();
        }, "\uE7C4");

    auto blur_accordion = (AccordionData *) blur_advanced->user_data;
    for (size_t i = first_blur_advanced; i < padded_right->children.size(); i++) {
        blur_accordion->rows.push_back(padded_right->children[i]);
        padded_right->children[i]->exists = blur_advanced_open;
    }
}

static void fill_dock_settings(Container *root) {
    auto padded_right = settings_begin_page(root);
    if (!padded_right)
        return;

    make_section_title(padded_right, "Dock Settings");

    make_vert_space(padded_right, 10);

    make_bool(padded_right, "Show docks", "", set->show_docks, [](bool c) {
        main_thread([c] {
            set->show_docks = c;
            if (c)
                dock::start();
            else
                dock::stop();
        });
    }, "\ue75b");

    make_dropdown_option(padded_right, "Dock style", "How dock icons are drawn", "\ue790", set->dock_style,
        {"Default", "Bubbly", "Bubbly flat"}, [](std::string style) {
            main_thread([style]() {
                set->dock_style = style;
                dock::set_dock_style(style);
                settings::load_save_settings(true, set);
            });
        });

    make_bool(padded_right, "Bounce icons on minimize and restore",
        "Dock icons dip when their window minimizes and hop when it comes back",
        set->dock_bounce_animation, [](bool value) {
            main_thread([value]() {
                set->dock_bounce_animation = value;
                settings::load_save_settings(true, set);
            });
        }, "\ue75b");

    make_bool(padded_right, "Only show windows on the current workspace",
        "Turn off to show windows from all workspaces and monitors",
        set->dock_current_workspace_only, [](bool value) {
            main_thread([value]() {
                set->dock_current_workspace_only = value;
                dock::update_workspaces();
                settings::load_save_settings(true, set);
            });
        }, "\ue75b");


    // Labels is the inverse of dock_merge_windows: off groups an app's
    // windows under one icon without titles.
    make_bool(padded_right,
        "Labels",
        "Give each window its own dock button with its title next to the icon",
        !set->dock_merge_windows,
        [](bool value) {
            main_thread([value]() {
                set->dock_merge_windows = !value;
                dock::set_dock_merge(!value);
                settings::load_save_settings(true, set);
            });
        },
        "\ue75b");


    make_bool(padded_right, "Same width",
        "When labels are showing, give every dock button the same width instead of fitting its title",
        set->dock_label_same_width, [](bool value) {
            main_thread([value]() {
                set->dock_label_same_width = value;
                dock::relayout();
                settings::load_save_settings(true, set);
            });
        }, "\ue75b");

    // Read by the applications menu each time it opens.
    make_bool(padded_right,
        "Show applications from other desktops",
        "List applications in the applications menu that are meant only for GNOME, KDE or another desktop",
        set->applications_show_other_desktops,
        [](bool value) {
            main_thread([value]() {
                set->applications_show_other_desktops = value;
                settings::load_save_settings(true, set);
            });
        },
        "\ue75b");
}



// Returns the family Pango actually uses for `name`, after fontconfig fallback.
static std::string resolved_font_family(const std::string &name) {
    static std::string cached_name;
    static std::string cached_family;
    if (!cached_family.empty() && cached_name == name)
        return cached_family;

    std::string family = name;
    PangoFontMap *map = pango_cairo_font_map_get_default();
    PangoContext *ctx = pango_font_map_create_context(map);
    PangoFontDescription *desc = pango_font_description_new();
    pango_font_description_set_family(desc, name.c_str());
    pango_font_description_set_size(desc, 11 * PANGO_SCALE);
    if (PangoFont *font = pango_font_map_load_font(map, ctx, desc)) {
        PangoFontDescription *actual = pango_font_describe(font);
        if (const char *f = pango_font_description_get_family(actual))
            family = f;
        pango_font_description_free(actual);
        g_object_unref(font);
    }
    pango_font_description_free(desc);
    g_object_unref(ctx);

    cached_name = name;
    cached_family = family;
    return family;
}

static Container *make_reset_textfield(Container *parent, std::string title, std::string description, std::string icon, bool only_numbers, std::string initial_value, std::string reset_value, std::function<void(std::string)> on_change, std::function<std::string()> left_text, std::string hint) {
    auto p = make_self_height_sized_parent(parent);
    
    make_label_like(p, title, description, icon);

    auto right = p->child(::hbox, FILL_SPACE, FILL_SPACE);
    right->alignment = container_alignment::ALIGN_CENTER;
    
    right->pre_layout = [left_text](Container *root, Container *c, const Bounds &b) {
        auto mylar = (MylarWindow*)root->user_data;
        auto cr = mylar->raw_window->drawing_context;
        auto dpi = mylar->raw_window->dpi;
        c->real_bounds.h = 70 * dpi;
        c->spacing = 5 * dpi;
        // `b` is the whole row; leave the title its room. In narrow windows the
        // hint before the field goes first, then the field shrinks.
        double max_w = std::max(260.0 * dpi, b.w - 260.0 * dpi);
        c->real_bounds.w = std::min(340.0 * dpi, max_w);
        if (left_text) {
            auto tb = draw_text(cr, 0, 0, left_text(), 11 * dpi, false, set->font, -1, -1, theme.text_secondary, false);
            bool fits = c->real_bounds.w + tb.w + 5 * dpi <= max_w;
            c->children[0]->exists = fits;
            if (fits)
                c->real_bounds.w += tb.w + 5 * dpi;
        }
    };

    if (left_text) {
        auto label = right->child(FILL_SPACE, FILL_SPACE);
        label->pre_layout = [left_text](Container *root, Container *c, const Bounds &b) {
            auto mylar = (MylarWindow*)root->user_data;
            auto dpi = mylar->raw_window->dpi;
            auto tb = draw_text(mylar->raw_window->drawing_context, 0, 0, left_text(), 11 * dpi, false, set->font, -1, -1, theme.text_secondary, false);
            c->wanted_bounds.w = tb.w;
            c->real_bounds.w = tb.w;
        };
        label->when_paint = [left_text](Container *root, Container *c) {
            auto mylar = (MylarWindow*)root->user_data;
            auto cr = mylar->raw_window->drawing_context;
            auto dpi = mylar->raw_window->dpi;
            auto text = left_text();
            auto tb = draw_text(cr, 0, 0, text, 11 * dpi, false, set->font, -1, -1, theme.text_secondary, false);
            draw_text(cr, c->real_bounds.x, c->real_bounds.y + c->real_bounds.h * .5 - tb.h * .5, text, 11 * dpi, true, set->font, -1, -1, theme.text_secondary, false);
        };
    }

    auto field = make_field(right, only_numbers, initial_value, [](std::string latest_text) {
    }, std::move(hint));
    
    make_button(right, "Apply", [field, on_change]() {
        auto value = static_cast<STextEditorData *>(field->user_data)->text;
        on_change(value);
    });
    make_button(right, "Reset", [field, reset_value, on_change]() {
        reset_text_editor(field, reset_value);
        auto value = static_cast<STextEditorData *>(field->user_data)->text;
        on_change(value);
    });
    return field;
}

static void reset_conf() {
    static long last_time = 0;
    static bool already_queued = false;
    long current = get_current_time_in_ms();
    long delta = current - last_time;
    if (delta > 200) {
        last_time = current;
        main_thread([]() {
            hypriso->generate_mylar_hyprland_config();
        });
    } else {
        if (!already_queued) {
            already_queued = true;
            main_thread([]() {
                later(100, [](Timer *) {
                    hypriso->generate_mylar_hyprland_config();
                    already_queued = false;
                });
            });
        }
    }
}

static void fill_keyboard_settings(Container *root, Container *c, bool reload = true);

static void fill_mouse_settings(Container *root, Container *c) {
    auto right = container_by_name("settings_right", root);
    if (!right)
        return;
    for (auto child: right->children)
        delete child;
    right->children.clear();

    right->pre_layout = layout_right_page;
    auto padded_right = right->child(FILL_SPACE, FILL_SPACE);
    
    make_section_title(padded_right, "Mouse");
    
    make_vert_space(padded_right, 10); 

    make_bool(padded_right, "Reverse scrolling direction", "Phone like scrolling behaviour", set->natural_scrolling_mouse, [](bool value) {
        set->natural_scrolling_mouse = value;
        main_thread([]() {
            hypriso->generate_mylar_hyprland_config();
        });
    }, "\uece7");
    

    make_button_group(padded_right, 
        "Primary mouse button", 
        "Which side performs the main function of the mouse",
        {"Left", "Right"}, 
        [](std::string selected) {
            set->primary_mouse_button = selected;
            main_thread([]() {
                hypriso->generate_mylar_hyprland_config();
            });
        }, set->primary_mouse_button, "\ue962");
    
    
    make_slider(padded_right, "Mouse speed", "How far the pointer moves when you move the mouse", set->mouse_speed, [](float value) {
        set->mouse_speed = value;
        // Writing the config makes Hyprland reload it, so wait until the slider settles.
        debounce_main("mouse_speed", 200, []() {
            hypriso->generate_mylar_hyprland_config();
        });
    }, 0, "\uec49", ConfigSettings().mouse_speed);

    make_vert_space(padded_right, 24);

    make_section_title(padded_right, "Touchpad");

    make_vert_space(padded_right, 10); 
    
    make_bool(padded_right, "Reverse scrolling direction", "Phone like scrolling behaviour", set->natural_scrolling_touchpad, [](bool value) {
        set->natural_scrolling_touchpad = value;
        main_thread([]() {
            hypriso->generate_mylar_hyprland_config();
        });
    }, "\uece7");
    

    make_bool(padded_right, "Disable while typing", "Prevents touchpad from receiving input while typing", set->touchpad_disable_while_typing, [](bool value) {
        set->touchpad_disable_while_typing = value;
        main_thread([]() {
            hypriso->generate_mylar_hyprland_config();
        });
    }, "\uefa5");


    make_button_group(padded_right, 
        "Touchpad acceleration", 
        "Cursor moves different distances depending on how fast you flick",
        {"Custom", "Adaptive", "Flat"}, 
        [](std::string selected) {
            set->touchpad_acceleration_curve = selected;
            main_thread([]() {
                hypriso->generate_mylar_hyprland_config();
            });
        }, set->touchpad_acceleration_curve, "\uec49");


    make_slider(padded_right, "Touchpad speed", "How far the pointer moves when you slide on the touchpad", set->touchpad_speed, [](float value) {
        set->touchpad_speed = value;
        debounce_main("touchpad_speed", 200, []() {
            hypriso->generate_mylar_hyprland_config();
        });
    }, 0, "\uec49", ConfigSettings().touchpad_speed);

}

/*************************************************
 *
 * Wi-Fi page (same behaviour as the dock's Wi-Fi menu, sharing its worker)
 *
 *************************************************/

struct SettingsWifiPage : UserData {
    WifiMenuView view;
    unsigned long revision = 0;
    unsigned generation = 0;
    bool animating = false;
    std::string selected;
    bool password_mode = false;
    bool show_password = false;
    bool confirming_forget = false;
    // Mirror of the selected row's text editor, kept current by its on_change.
    std::string password;
    Container *list = nullptr;
    ~SettingsWifiPage() override;
};

struct SettingsWifiRow : UserData {
    WifiAccessPoint ap;
    bool hovered = false;
    double expansion = 0;
    double expansion_from = 0;
    double expansion_target = 0;
    long animation_start = 0;
    Container *field = nullptr;
    Container *input = nullptr;
    Container *buttons = nullptr;
    Container *secondary = nullptr;
};

// The live page, if the Wi-Fi tab is showing. Only touched on the settings thread.
static SettingsWifiPage *settings_wifi = nullptr;
static unsigned settings_wifi_generation = 0;

static void settings_wifi_release() {
    if (!settings_wifi)
        return;
    settings_wifi = nullptr;
    wifi_menu_closed();
}

SettingsWifiPage::~SettingsWifiPage() {
    if (settings_wifi == this)
        settings_wifi_release();
}

// Signal strength like the dock menu: the full Wi-Fi glyph drawn faintly, the bars
// for the current level on top, and the lock badge outlined in the row colour.
static void swifi_paint_signal(drawing::Context *cr, double x, double y, double size, int strength, bool secured,
                               double dpi, RGBA background) {
    const char *level = strength >= 75 ? "" : strength >= 50 ? "" : strength >= 25 ? "" : "";
    RGBA faint = theme.text;
    faint.a *= .22;
    swifi_glyph(cr, x, y, "", size, faint);
    swifi_glyph(cr, x, y, level, size, theme.text);
    if (!secured)
        return;
    cr->save();
    cr->new_path();
    swifi_glyph(cr, x, y, "", size, theme.text, true);
    set_argb(cr, background);
    cr->set_line_width(3 * dpi);
    cr->set_line_join(CAIRO_LINE_JOIN_ROUND);
    cr->stroke_preserve();
    set_argb(cr, theme.text);
    cr->fill();
    cr->restore();
}

static void swifi_paint_button(Container *root, Container *c, const std::string &text, bool enabled) {
    auto window = swifi_window(root);
    auto cr = window->drawing_context;
    auto dpi = window->dpi;
    auto b = c->real_bounds;
    if (b.h <= 1 || b.w <= 1)
        return;
    // Like winbar's wifi menu: a translucent white wash over whatever is underneath, so
    // the button picks up the accent tint of an expanded row instead of fighting it.
    double alpha;
    if (set->dark_theme)
        alpha = !enabled ? .04 : c->state.mouse_pressing ? .22 : c->state.mouse_hovering ? .16 : .10;
    else
        alpha = !enabled ? .35 : c->state.mouse_pressing ? .55 : c->state.mouse_hovering ? .92 : .75;
    set_argb(cr, RGBA(1, 1, 1, alpha));
    drawRoundedRect(cr, b.x, b.y, b.w, b.h, settings_control_rounding * dpi, 1.0);
    cr->fill();
    b.x += 6 * dpi;
    b.w -= 12 * dpi;
    swifi_text(root, b, text, 11, enabled ? theme.text : swifi_dim(theme.text_secondary), true);
}

static RGBA swifi_row_color(SettingsWifiRow *data) {
    auto base = data->hovered ? theme.row_hover : theme.row_background;
    return swifi_mix(base, theme.accent, .14 * data->expansion);
}

static void swifi_reset_selection() {
    auto page = settings_wifi;
    if (!page)
        return;
    page->password_mode = false;
    page->show_password = false;
    page->confirming_forget = false;
    page->password.clear();
    // Don't leave typed passwords (or their undo history) sitting in hidden editors.
    for (auto row : page->list->children)
        if (auto input = ((SettingsWifiRow *) row->user_data)->input)
            reset_text_editor(input, "");
}

static void swifi_submit_password(Container *root, SettingsWifiRow *row) {
    auto page = settings_wifi;
    std::string problem;
    if (!wifi_valid_password(row->ap, page->password, problem)) {
        wifi_command({"notice", "", problem});
        return;
    }
    wifi_command({"connect", row->ap.ssid, page->password});
    swifi_reset_selection();
    swifi_redraw(root);
}

static std::string swifi_primary_label(SettingsWifiPage *page, SettingsWifiRow *row, bool *enabled) {
    const auto &view = page->view;
    *enabled = !view.busy && view.action_ssid.empty();
    if (view.connecting_ssid == row->ap.ssid) return "Connecting…";
    if (view.action_ssid == row->ap.ssid) return view.action;
    if (page->password_mode) {
        *enabled = *enabled && !page->password.empty();
        return "Connect";
    }
    return row->ap.active ? "Disconnect" : "Connect";
}

static void swifi_primary_clicked(Container *root, SettingsWifiRow *row) {
    auto page = settings_wifi;
    if (page->password_mode) {
        swifi_submit_password(root, row);
    } else if (row->ap.active) {
        wifi_command({"disconnect", row->ap.ssid, ""});
    } else if (!row->ap.saved_connections.empty() || !row->ap.secured) {
        wifi_command({"connect", row->ap.ssid, ""});
    } else if (row->ap.enterprise) {
        wifi_command({"notice", "", "Enterprise (802.1X) networks need to be set up in your network settings first."});
    } else {
        page->password_mode = true;
        page->password.clear();
        reset_text_editor(row->input, "");
        set_active(root, {row->input}, root, true, false);
    }
    page->confirming_forget = false;
    swifi_redraw(root);
}

// Advances the row's open/close animation and returns its height.
static double swifi_row_step(SettingsWifiRow *data, double dpi) {
    auto page = settings_wifi;
    const bool selected = page->selected == data->ap.ssid;
    const auto now = get_current_time_in_ms();
    double elapsed = std::clamp((now - data->animation_start) / 180.0, 0.0, 1.0);
    data->expansion = data->expansion_from + (data->expansion_target - data->expansion_from) *
        (1 - std::pow(1 - elapsed, 3));
    double target = selected ? 1 : 0;
    if (target != data->expansion_target) {
        data->expansion_from = data->expansion;
        data->expansion_target = target;
        data->animation_start = now;
    }
    if (std::abs(data->expansion - data->expansion_target) > .001)
        page->animating = true;
    const bool password = selected && page->password_mode;
    data->field->exists = password && data->expansion > .001;
    data->buttons->exists = data->expansion > .001;
    data->secondary->exists = password || !data->ap.saved_connections.empty();
    return (62 + ((password ? 44 : 0) + 42) * data->expansion) * dpi;
}

static Container *swifi_network_row(Container *parent) {
    auto row = parent->child(::vbox, FILL_SPACE, 62);
    auto data = new SettingsWifiRow;
    row->user_data = data;
    row->receive_events_even_if_obstructed = true;
    row->when_mouse_enters_container = [data](Container *root, Container *) {
        data->hovered = true;
        swifi_redraw(root);
    };
    row->when_mouse_leaves_container = [data](Container *root, Container *) {
        data->hovered = false;
        swifi_redraw(root);
    };
    row->when_paint = [data](Container *root, Container *c) {
        auto window = swifi_window(root);
        auto b = c->real_bounds;
        window->drawing_context->save();
        drawRoundedRect(window->drawing_context, b.x, b.y, b.w, b.h, 5 * window->dpi, 1.0);
        window->drawing_context->clip_path();
        set_argb(window->drawing_context, swifi_row_color(data));
        window->drawing_context->paint_source();
    };
    row->after_paint = [](Container *root, Container *c) {
        auto window = swifi_window(root);
        auto b = c->real_bounds;
        window->drawing_context->restore();
        drawRoundedRect(window->drawing_context, b.x, b.y, b.w, b.h, 5 * window->dpi, 1.0);
        set_argb(window->drawing_context, theme.row_border);
        window->drawing_context->stroke();
    };

    auto header = row->child(FILL_SPACE, 62);
    header->pre_layout = [](Container *root, Container *c, const Bounds &) {
        c->wanted_bounds.h = 62 * swifi_window(root)->dpi;
    };
    header->when_paint = [data](Container *root, Container *c) {
        auto window = swifi_window(root);
        auto dpi = window->dpi;
        auto page = settings_wifi;
        auto b = c->real_bounds;
        swifi_paint_signal(window->drawing_context, b.x + 19 * dpi, b.y + 19 * dpi, 20 * dpi, data->ap.strength, data->ap.secured,
            dpi, swifi_row_color(data));
        std::string security = data->ap.security == "OWE" ? "Open (encrypted)" :
            data->ap.security.empty() ? "Open" : data->ap.security == "WEP" ? "WEP (insecure)" : data->ap.security;
        std::string subtitle;
        if (data->ap.active) {
            const auto &status = page->view.status;
            auto device = wifi_device(status);
            subtitle = !(device && device->state == 100) ? "Connecting…" :
                status.connectivity == 2 ? "Connected, sign-in required" :
                status.connectivity == 1 || status.connectivity == 3 ? "Connected, no internet" : "Connected";
            subtitle += " · " + security;
        } else if (page->view.connecting_ssid == data->ap.ssid) {
            subtitle = "Connecting…";
        } else {
            subtitle = (data->ap.saved_connections.empty() ? "" : "Saved · ") + security;
        }
        if (auto band = wifi_band(data->ap.frequency); !band.empty()) subtitle += " · " + band;
        if ((data->hovered || page->selected == data->ap.ssid) && !data->ap.bssid.empty())
            subtitle += " · " + data->ap.bssid;
        swifi_text(root, Bounds(b.x + 60 * dpi, b.y + 9 * dpi, b.w - 76 * dpi, 22 * dpi), data->ap.ssid, 12, theme.text);
        swifi_text(root, Bounds(b.x + 60 * dpi, b.y + 31 * dpi, b.w - 76 * dpi, 22 * dpi), subtitle, 10,
            theme.text_secondary);
    };
    header->when_clicked = [data](Container *root, Container *) {
        auto page = settings_wifi;
        page->selected = page->selected == data->ap.ssid ? "" : data->ap.ssid;
        swifi_reset_selection();
        swifi_redraw(root);
    };

    data->field = row->child(::hbox, FILL_SPACE, 0);
    data->field->pre_layout = [data](Container *root, Container *c, const Bounds &) {
        double dpi = swifi_window(root)->dpi;
        c->wanted_bounds.h = 44 * data->expansion * dpi;
        c->wanted_pad = Bounds(60 * dpi, 2 * dpi, 16 * dpi, 8 * dpi);
        c->spacing = 8 * dpi;
    };
    auto input = data->field->child(FILL_SPACE, FILL_SPACE);
    data->input = input;
    STextEditorOptions options;
    options.multiline = false;
    options.password = true;
    options.max_length = 64; // WPA allows 63 characters or 64 hex digits.
    options.font_size = 11;
    options.window = [](Container *root) { return swifi_window(root); };
    options.viewport = [](Container *root, Container *c) {
        auto dpi = swifi_window(root)->dpi;
        auto bounds = c->real_bounds;
        bounds.x += 10 * dpi;
        bounds.w = std::max(0.0, bounds.w - 20 * dpi);
        return bounds;
    };
    // Fires for typing and for asynchronous clipboard pastes alike.
    options.on_change = [](std::string text) {
        if (settings_wifi)
            settings_wifi->password = std::move(text);
    };
    setup_text_editor(input, "", std::move(options));
    focus_text(input);
    input->pre_layout = [](Container *root, Container *c, const Bounds &) {
        // "Show" turns masking off on the shared editor.
        static_cast<STextEditorData *>(c->user_data)->options.password = !settings_wifi->show_password;
    };
    input->when_paint = [](Container *root, Container *c) {
        auto window = swifi_window(root);
        auto cr = window->drawing_context;
        auto dpi = window->dpi;
        auto b = c->real_bounds;
        if (b.h <= 2 * dpi)
            return;
        paint_smooth_frame(cr, b, settings_control_rounding * dpi, theme.field_background,
                           c->active ? theme.accent : theme.field_border, std::max(1.0, (double) std::round(1 * dpi)));
        if (static_cast<STextEditorData *>(c->user_data)->text.empty()) {
            auto hint = b;
            hint.x += 10 * dpi;
            hint.w -= 20 * dpi;
            swifi_text(root, hint, "Enter the network security key", 11, swifi_dim(theme.text_secondary));
        }
        paint_text_editor(root, c, theme.text, theme.accent);
    };
    auto edit_key = std::move(input->when_key_event);
    input->when_key_event = [edit_key, data](Container *root, Container *c, int key, bool pressed, xkb_keysym_t sym,
                                             int mods, bool is_text, std::string text) {
        auto page = settings_wifi;
        // Key events reach every container; only the open password row reacts.
        if (!page || !page->password_mode || page->selected != data->ap.ssid) return;
        if (pressed && (sym == XKB_KEY_Return || sym == XKB_KEY_KP_Enter)) {
            swifi_submit_password(root, data);
            return;
        }
        if (pressed && sym == XKB_KEY_Escape) {
            swifi_reset_selection();
            swifi_redraw(root);
            return;
        }
        edit_key(root, c, key, pressed, sym, mods, is_text, text);
        swifi_redraw(root);
    };
    auto reveal = data->field->child(70, FILL_SPACE);
    reveal->pre_layout = [](Container *root, Container *c, const Bounds &) {
        c->wanted_bounds.w = 70 * swifi_window(root)->dpi;
    };
    reveal->when_paint = [](Container *root, Container *c) {
        swifi_paint_button(root, c, settings_wifi->show_password ? "Hide" : "Show", true);
    };
    reveal->when_clicked = [data](Container *root, Container *) {
        settings_wifi->show_password = !settings_wifi->show_password;
        // Clicking the button took focus from the field; hand it back.
        set_active(root, {data->input}, root, true, false);
        swifi_redraw(root);
    };

    data->buttons = row->child(::hbox, FILL_SPACE, 0);
    data->buttons->pre_layout = [data](Container *root, Container *c, const Bounds &) {
        double dpi = swifi_window(root)->dpi;
        c->wanted_bounds.h = 42 * data->expansion * dpi;
        c->wanted_pad = Bounds(60 * dpi, 0, 16 * dpi, 8 * dpi);
        c->spacing = 8 * dpi;
    };
    auto primary = data->buttons->child(FILL_SPACE, FILL_SPACE);
    primary->when_paint = [data](Container *root, Container *c) {
        bool enabled = false;
        auto label = swifi_primary_label(settings_wifi, data, &enabled);
        swifi_paint_button(root, c, label, enabled);
    };
    primary->when_clicked = [data](Container *root, Container *) {
        bool enabled = false;
        swifi_primary_label(settings_wifi, data, &enabled);
        if (enabled) swifi_primary_clicked(root, data);
    };
    data->secondary = data->buttons->child(FILL_SPACE, FILL_SPACE);
    data->secondary->when_paint = [](Container *root, Container *c) {
        auto page = settings_wifi;
        if (page->password_mode) {
            swifi_paint_button(root, c, "Cancel", true);
            return;
        }
        swifi_paint_button(root, c, page->confirming_forget ? "Are you sure?" : "Forget",
            !page->view.busy && page->view.action_ssid.empty());
    };
    data->secondary->when_clicked = [data](Container *root, Container *) {
        auto page = settings_wifi;
        if (page->password_mode) {
            swifi_reset_selection();
        } else if (!page->view.busy && page->view.action_ssid.empty()) {
            if (!page->confirming_forget) {
                page->confirming_forget = true;
            } else {
                page->confirming_forget = false;
                wifi_command({"forget", data->ap.ssid, ""});
            }
        }
        swifi_redraw(root);
    };
    return row;
}

static void swifi_apply(SettingsWifiPage *page, const WifiMenuView &view) {
    page->view = view;
    if (!page->selected.empty() && !wifi_access_point(view.status, page->selected)) {
        page->selected.clear();
        swifi_reset_selection();
    }
    auto content = page->list;
    std::unordered_set<std::string> wanted;
    for (const auto &ap : view.status.access_points) wanted.insert(ap.ssid);
    std::unordered_map<std::string, Container *> rows;
    std::erase_if(content->children, [&wanted, &rows](Container *row) {
        if (wanted.contains(row->name)) {
            rows.emplace(row->name, row);
            return false;
        }
        delete row;
        return true;
    });
    // Rows survive refreshes (keyed by SSID) so selection, typing and animations persist.
    std::vector<Container *> ordered;
    for (const auto &ap : view.status.access_points) {
        auto found = rows.find(ap.ssid);
        Container *row = nullptr;
        if (found != rows.end()) {
            row = found->second;
        } else {
            row = swifi_network_row(content);
            content->children.pop_back(); // Re-inserted below in sorted order.
            row->name = ap.ssid;
        }
        ((SettingsWifiRow *) row->user_data)->ap = ap;
        ordered.push_back(row);
    }
    content->children = std::move(ordered);
}

static void swifi_watch(unsigned generation) {
    windowing::timer(settings_app, 16, [generation](void *) {
        if (!settings_app || !settings_mylar)
            return;
        bool redraw = false;
        {
            std::lock_guard<std::mutex> lock(settings_app->mutex);
            auto page = settings_wifi;
            if (!page || page->generation != generation)
                return; // Page left or rebuilt; stop polling.
            auto view = wifi_latest();
            if (view && view->revision != page->revision) {
                swifi_apply(page, *view);
                page->revision = view->revision;
                redraw = true;
            }
            redraw |= page->animating || page->view.scanning || page->view.busy;
            page->animating = false;
        }
        if (redraw)
            windowing::redraw(settings_mylar->raw_window);
        swifi_watch(generation);
    }, nullptr);
}

// The dock menu's scanning animation: five dots easing along the card's bottom
// edge, clipped to the inside of the card's rounded border so they never cover it.
static void paint_card_progress_dots(drawing::Context *cr, Bounds b, double dpi) {
    double phase = (get_current_time_in_ms() % 2000) / 2000.0;
    double inset = std::max(1.0, std::floor(1.0 * dpi));
    Bounds inner(b.x + inset, b.y + inset, b.w - inset * 2, b.h - inset * 2);
    double radius = 2 * dpi;
    cr->save();
    cr->new_path();
    drawRoundedRect(cr, inner.x, inner.y, inner.w, inner.h, std::max(0.0, 5 * dpi - inset), 0.0);
    cr->clip_path();
    set_argb(cr, theme.accent);
    for (int i = 0; i < 5; ++i) {
        double step = (phase - i * .08) / .6;
        if (step < 0 || step > 1) continue;
        double x = .5 - .5 * std::cos(step * M_PI);
        cr->new_path();
        cr->arc(inner.x + x * inner.w, inner.y + inner.h - 5 * dpi, radius, 0, 2 * M_PI);
        cr->fill();
    }
    cr->restore();
}

// Text-sized button for the status card.
static Container *swifi_card_button(Container *parent, std::function<std::string()> label,
                                    std::function<bool()> enabled, std::function<void()> on_click) {
    auto button = parent->child(FILL_SPACE, FILL_SPACE);
    button->pre_layout = [label](Container *root, Container *c, const Bounds &) {
        c->wanted_bounds.w = swifi_text_width(root, label(), 11) + 24 * swifi_window(root)->dpi;
    };
    button->when_paint = [label, enabled](Container *root, Container *c) {
        swifi_paint_button(root, c, label(), enabled());
    };
    button->when_clicked = [enabled, on_click](Container *, Container *) {
        if (enabled()) on_click();
    };
    return button;
}

/*************************************************
 *
 * Shortcuts page
 *
 *************************************************/

struct SettingsShortcutRow {
    ShortcutDefault shortcut; // For custom rows only name is used, keys are the empty default
    std::string keys;         // Shortcut in effect, empty when unbound
    bool custom = false;
    std::string command;      // Custom rows only
};

// Only touched on the settings thread.
static std::vector<SettingsShortcutRow> sshortcut_rows;
static int sshortcut_editing = -1;
// The combo typed since Edit was pressed, waiting for Set.
static std::string sshortcut_pending;
static unsigned sshortcut_generation = 0;
// The Shortcuts page must be rebuilt from sshortcut_rows (rows were added or removed).
static bool sshortcut_page_stale = false;
// Row to start recording for once the page is rebuilt, -1 for none.
static int sshortcut_edit_after_rebuild = -1;

static void sshortcut_load_rows() {
    sshortcut_rows.clear();
    for (const auto &custom : set->custom_shortcuts) {
        SettingsShortcutRow row;
        row.shortcut.id = "custom";
        row.shortcut.name = custom.name;
        row.keys = custom.keys;
        row.custom = true;
        row.command = custom.command;
        sshortcut_rows.push_back(row);
    }
    for (const auto &shortcut : shortcuts::defaults())
        sshortcut_rows.push_back({shortcut, shortcuts::keys_for(set, shortcut)});
}

static int sshortcut_index_of(const std::string &id) {
    for (int i = 0; i < (int) sshortcut_rows.size(); i++)
        if (!sshortcut_rows[i].custom && sshortcut_rows[i].shortcut.id == id)
            return i;
    return -1;
}

// Row whose shortcut is being typed into a text field, -1 for none.
static int sshortcut_typing = -1;

// Turns "super+shift + t" into "SUPER + SHIFT + t"; empty when nothing was typed.
static std::string sshortcut_clean_typed(const std::string &text) {
    std::string out;
    std::istringstream stream(text);
    std::string part;
    while (std::getline(stream, part, '+')) {
        part = trim(part);
        if (part.empty())
            continue;
        std::string upper;
        for (char ch : part)
            upper += (char) std::toupper((unsigned char) ch);
        if (upper == "CONTROL")
            upper = "CTRL";
        if (upper == "WIN" || upper == "META" || upper == "LOGO" || upper == "MOD4")
            upper = "SUPER";
        static const char *modifiers[] = {"CTRL", "ALT", "SHIFT", "SUPER", "SUPER_L", "SUPER_R"};
        for (auto modifier : modifiers)
            if (upper == modifier)
                part = upper;
        out += (out.empty() ? "" : " + ") + part;
    }
    return out;
}

static void sshortcut_stop_editing() {
    sshortcut_typing = -1;
    if (sshortcut_editing == -1)
        return;
    sshortcut_editing = -1;
    sshortcut_pending.clear();
    sshortcut_generation++;
    main_thread([]() {
        shortcuts::capture_stop();
    });
}

// The compositor records the combo, since bound keys never reach this window.
static void sshortcut_start_editing(int index) {
    sshortcut_stop_editing();
    sshortcut_editing = index;
    const unsigned generation = ++sshortcut_generation;
    main_thread([generation]() {
        shortcuts::capture_start([generation](std::string keys) {
            auto app = settings_app;
            if (!app)
                return;
            windowing::run_on_app(app, [generation, keys]() {
                if (generation != sshortcut_generation || sshortcut_editing == -1)
                    return;
                if (keys.empty()) {
                    // Escape or inactivity, the capture has already stopped.
                    sshortcut_editing = -1;
                    sshortcut_pending.clear();
                    sshortcut_generation++;
                } else {
                    sshortcut_pending = keys;
                }
                if (settings_mylar)
                    windowing::redraw(settings_mylar->raw_window);
            });
        });
    });
}

// Hands the rows to the compositor, which rewrites the Hyprland config and saves.
static void sshortcut_commit(std::string displaced = "", std::string keys = "") {
    std::vector<SShortcutOverride> overrides;
    std::vector<SCustomShortcut> customs;
    for (const auto &row : sshortcut_rows) {
        if (row.custom)
            customs.push_back({row.shortcut.name, row.command, row.keys});
        else if (row.keys != row.shortcut.keys)
            overrides.push_back({row.shortcut.id, row.keys});
    }
    main_thread([overrides, customs, displaced, keys]() {
        set->shortcuts = overrides;
        set->custom_shortcuts = customs;
        settings::load_save_settings(true, set);
        hypriso->generate_mylar_hyprland_config();
        if (!displaced.empty())
            notify(keys + " was taken from: " + displaced);
    });
}

static void sshortcut_apply(int index, const std::string &keys) {
    std::string displaced;
    if (!keys.empty()) {
        // A combo can only drive one action, the latest assignment wins.
        const auto wanted = shortcuts::normalized(keys);
        for (int i = 0; i < (int) sshortcut_rows.size(); i++) {
            auto &row = sshortcut_rows[i];
            if (i == index || row.keys.empty() || shortcuts::normalized(row.keys) != wanted)
                continue;
            row.keys.clear();
            displaced += (displaced.empty() ? "" : ", ") + row.shortcut.name;
        }
    }
    sshortcut_rows[index].keys = keys;
    sshortcut_stop_editing();
    sshortcut_commit(displaced, keys);
}

static void sshortcut_request_rebuild() {
    sshortcut_page_stale = true;
    if (settings_mylar)
        windowing::redraw(settings_mylar->raw_window);
}

static void sshortcut_remove(int index) {
    if (index < 0 || index >= (int) sshortcut_rows.size() || !sshortcut_rows[index].custom)
        return;
    sshortcut_stop_editing();
    sshortcut_rows.erase(sshortcut_rows.begin() + index);
    sshortcut_commit();
    sshortcut_request_rebuild();
}

static void sshortcut_add(const std::string &name, const std::string &command) {
    sshortcut_stop_editing();
    SettingsShortcutRow row;
    row.shortcut.id = "custom";
    row.shortcut.name = name.empty() ? command : name;
    row.custom = true;
    row.command = command;
    int index = 0;
    while (index < (int) sshortcut_rows.size() && sshortcut_rows[index].custom)
        index++;
    sshortcut_rows.insert(sshortcut_rows.begin() + index, row);
    sshortcut_commit();
    // Record the shortcut right away, the row is unbound until Set is pressed.
    sshortcut_edit_after_rebuild = index;
    sshortcut_request_rebuild();
}

static Container *make_placeholder_field(Container *parent, std::string placeholder,
                                        std::string initial_value = "", std::function<void(std::string)> on_change = nullptr);

// Filter typed above the system shortcuts; rows not matching it are hidden.
static std::string sshortcut_filter;

static std::string sshortcut_lower(std::string text) {
    for (auto &ch : text)
        ch = (char) std::tolower((unsigned char) ch);
    return text;
}

// Every space-separated word of the filter must appear in the row's name or keys.
static bool sshortcut_matches_filter(int index) {
    if (index < 0 || index >= (int) sshortcut_rows.size())
        return false;
    const auto &row = sshortcut_rows[index];
    auto haystack = sshortcut_lower(row.shortcut.name + " " + row.keys + " " + row.command);
    std::istringstream words(sshortcut_lower(sshortcut_filter));
    std::string word;
    while (words >> word)
        if (haystack.find(word) == std::string::npos)
            return false;
    return true;
}

// Shows only the system rows matching the filter, or the "no match" note when none do.
static void sshortcut_apply_filter(Container *page) {
    if (!page)
        return;
    int index = 0;
    while (index < (int) sshortcut_rows.size() && sshortcut_rows[index].custom)
        index++;
    bool any = false;
    Container *empty = nullptr;
    for (auto child : page->children) {
        if (child->name == "system_row") {
            child->exists = sshortcut_matches_filter(index++);
            any = any || child->exists;
        } else if (child->name == "system_empty") {
            empty = child;
        }
    }
    if (empty)
        empty->exists = !any;
}
static std::string sshortcut_field_text(Container *field);

static void make_shortcut_row(Container *parent, int index) {
    static const float height = 60;
    static const float round = 8;
    auto row = parent->child(::hbox, FILL_SPACE, height);
    // Rows of the same kind form one rounded group; the filter can hide system rows.
    row->name = index < (int) sshortcut_rows.size() && sshortcut_rows[index].custom ? "custom_row" : "system_row";
    if (index < (int) sshortcut_rows.size())
        search_tag(row, sshortcut_rows[index].shortcut.name, sshortcut_rows[index].keys);
    // The page container is only viewport tall and moves with the scroll.
    row->parent_bounds_limit_input_bounds = false;
    // Stay hovered over the children too, so custom rows can swap name for command.
    row->receive_events_even_if_obstructed = true;
    row->pre_layout = [index](Container *root, Container *c, const Bounds &) {
        double dpi = swifi_window(root)->dpi;
        c->wanted_bounds.h = height * dpi;
        c->wanted_pad = Bounds(20 * dpi, 14 * dpi, 14 * dpi, 14 * dpi);
        c->spacing = 8 * dpi;
        // Children named "idle" show normally, "editing" ones while recording,
        // "typing" ones while the shortcut is typed into the field.
        const bool editing = sshortcut_editing == index;
        const bool typing = sshortcut_typing == index;
        for (auto child : c->children) {
            if (child->name == "idle")
                child->exists = !editing && !typing;
            else if (child->name == "keys")
                child->exists = !typing;
            else if (child->name == "editing")
                child->exists = editing;
            else if (child->name == "typing")
                child->exists = typing;
        }
    };
    row->when_paint = [index](Container *root, Container *c) {
        auto window = swifi_window(root);
        auto cr = window->drawing_context;
        auto dpi = window->dpi;

        // First/last among the visible rows of the same group get the rounded corners.
        bool is_first = true, is_last = true;
        if (auto parent = c->parent) {
            auto &siblings = parent->children;
            auto it = std::find(siblings.begin(), siblings.end(), c);
            for (auto prev = it; prev != siblings.begin();) {
                --prev;
                if (!(*prev)->exists)
                    continue;
                is_first = (*prev)->name != c->name;
                break;
            }
            for (auto next = it == siblings.end() ? it : it + 1; next != siblings.end(); ++next) {
                if (!(*next)->exists)
                    continue;
                is_last = (*next)->name != c->name;
                break;
            }
        }

        // Neighbouring rows share their border line.
        auto b = c->real_bounds;
        const auto overlap = (std::floor(1.0 * dpi) + 1.0) * 0.5;
        b.y -= overlap;
        b.h += 2.0 * overlap;

        drawRoundedRect(cr, b.x, b.y, b.w, b.h, round * dpi, 0, is_first, is_first, is_last, is_last);
        const bool active = sshortcut_editing == index || sshortcut_typing == index;
        set_argb(cr, active ? swifi_mix(theme.row_background, theme.accent, .14) : theme.row_background);
        cr->fill();

        drawRoundedRect(cr, b.x, b.y, b.w, b.h, round * dpi, std::floor(1.0 * dpi), is_first, is_first, is_last, is_last);
        set_argb(cr, theme.row_border);
        cr->set_line_width(std::floor(1.0 * dpi));
        cr->stroke();
    };

    auto name = row->child(FILL_SPACE, FILL_SPACE);
    name->when_paint = [index, row](Container *root, Container *c) {
        if (index >= (int) sshortcut_rows.size())
            return;
        const auto &data = sshortcut_rows[index];
        // Custom rows show the command they run instead of the name while hovered.
        const bool show_command = data.custom && row->state.mouse_hovering && !data.command.empty();
        swifi_text(root, c->real_bounds, show_command ? data.command : data.shortcut.name, 12, theme.text);
    };

    auto keys_text = [index]() -> std::string {
        if (sshortcut_editing == index)
            return sshortcut_pending.empty() ? "Press a shortcut…" : sshortcut_pending;
        if (index >= (int) sshortcut_rows.size())
            return "";
        const auto &keys = sshortcut_rows[index].keys;
        return keys.empty() ? "Unassigned" : keys;
    };
    auto keys = row->child(FILL_SPACE, FILL_SPACE);
    keys->name = "keys";
    keys->pre_layout = [keys_text](Container *root, Container *c, const Bounds &) {
        c->wanted_bounds.w = swifi_text_width(root, keys_text(), 12) + 8 * swifi_window(root)->dpi;
    };
    keys->when_paint = [index, keys_text](Container *root, Container *c) {
        const bool typed = sshortcut_editing == index && !sshortcut_pending.empty();
        const bool unassigned = sshortcut_editing != index && sshortcut_rows[index].keys.empty();
        auto color = typed ? theme.text : unassigned ? swifi_dim(theme.text_secondary) : theme.text_secondary;
        swifi_text(root, c->real_bounds, keys_text(), 12, color);
    };

    // Typing the shortcut by hand, e.g. "SUPER + SHIFT + T".
    auto field = make_placeholder_field(row, "e.g. SUPER + SHIFT + T");
    field->name = "typing";
    field->exists = false;
    auto field_layout = field->pre_layout;
    field->pre_layout = [field_layout](Container *root, Container *c, const Bounds &b) {
        if (field_layout)
            field_layout(root, c, b);
        auto dpi = swifi_window(root)->dpi;
        c->wanted_bounds.w = 230 * dpi;
        c->wanted_bounds.h = 30 * dpi;
    };
    auto submit_typed = [index, field]() {
        if (sshortcut_typing != index)
            return;
        sshortcut_apply(index, sshortcut_clean_typed(sshortcut_field_text(field)));
    };
    auto edit_key = std::move(field->when_key_event);
    field->when_key_event = [edit_key, index, submit_typed](Container *root, Container *c, int key, bool pressed,
                                                           xkb_keysym_t sym, int mods, bool is_text, std::string text) {
        // The field stays focused after it hides, ignore keys meant for something else.
        if (sshortcut_typing != index)
            return;
        if (pressed && sym == XKB_KEY_Escape) {
            sshortcut_stop_editing();
            swifi_redraw(root);
            return;
        }
        if (pressed && (sym == XKB_KEY_Return || sym == XKB_KEY_KP_Enter)) {
            submit_typed();
            swifi_redraw(root);
            return;
        }
        if (edit_key)
            edit_key(root, c, key, pressed, sym, mods, is_text, text);
    };

    auto always = []() { return true; };
    const bool custom = sshortcut_rows[index].custom;
    swifi_card_button(row, []() { return std::string("Edit"); }, always, [index]() {
        sshortcut_start_editing(index);
    })->name = "idle";
    swifi_card_button(row, []() { return std::string("Type"); }, always, [index, field]() {
        sshortcut_stop_editing();
        sshortcut_typing = index;
        reset_text_editor(field, sshortcut_rows[index].keys);
        if (settings_mylar)
            set_active(settings_mylar->root, {field}, field, true, false);
    })->name = "idle";
    if (custom) {
        swifi_card_button(row, []() { return std::string("Remove"); }, always, [index]() {
            sshortcut_remove(index);
        })->name = "idle";
    }
    swifi_card_button(row, []() { return std::string("Set"); }, []() { return !sshortcut_pending.empty(); }, [index]() {
        sshortcut_apply(index, sshortcut_pending);
    })->name = "editing";
    swifi_card_button(row, []() { return std::string("Clear"); }, [index]() { return !sshortcut_rows[index].keys.empty(); }, [index]() {
        sshortcut_apply(index, "");
    })->name = "editing";
    if (!custom) {
        swifi_card_button(row, []() { return std::string("Reset"); },
            [index]() { return sshortcut_rows[index].keys != sshortcut_rows[index].shortcut.keys; }, [index]() {
                sshortcut_apply(index, sshortcut_rows[index].shortcut.keys);
            })->name = "editing";
    }
    swifi_card_button(row, []() { return std::string("Cancel"); }, always, []() {
        sshortcut_stop_editing();
    })->name = "editing";

    swifi_card_button(row, []() { return std::string("Set"); }, always, submit_typed)->name = "typing";
    swifi_card_button(row, []() { return std::string("Cancel"); }, always, []() {
        sshortcut_stop_editing();
    })->name = "typing";
}

// A make_field that shows `placeholder` until something is typed.
static Container *make_placeholder_field(Container *parent, std::string placeholder,
                                        std::string initial_value, std::function<void(std::string)> on_change) {
    if (!on_change)
        on_change = [](std::string) {};
    auto field = make_field(parent, false, std::move(initial_value), std::move(on_change));
    field->parent_bounds_limit_input_bounds = false;
    auto paint_field = std::move(field->when_paint);
    field->when_paint = [paint_field, placeholder](Container *root, Container *c) {
        if (paint_field)
            paint_field(root, c);
        auto data = static_cast<STextEditorData *>(c->user_data);
        if (!data || !data->text.empty())
            return;
        auto dpi = swifi_window(root)->dpi;
        auto b = c->real_bounds;
        b.x += 11 * dpi;
        b.w = std::max(0.0, b.w - 22 * dpi);
        swifi_text(root, b, placeholder, 11, swifi_dim(theme.text_secondary));
    };
    return field;
}

static std::string sshortcut_field_text(Container *field) {
    auto data = static_cast<STextEditorData *>(field->user_data);
    return data ? trim(data->text) : "";
}

// Name, command and an Add button; the shortcut itself is recorded on the new row.
static void make_custom_shortcut_form(Container *parent) {
    auto card = parent->child(::vbox, FILL_SPACE, FILL_SPACE);
    card->parent_bounds_limit_input_bounds = false;
    card->pre_layout = [](Container *root, Container *c, const Bounds &b) {
        double dpi = swifi_window(root)->dpi;
        c->wanted_pad = Bounds(16 * dpi, 12 * dpi, 12 * dpi, 12 * dpi);
        c->spacing = 8 * dpi;
        double h = c->wanted_pad.y + c->wanted_pad.h;
        int shown = 0;
        for (auto child : c->children) {
            if (!child->exists)
                continue;
            if (child->pre_layout)
                child->pre_layout(root, child, b);
            h += child->wanted_bounds.h;
            shown++;
        }
        h += std::max(0, shown - 1) * c->spacing;
        c->wanted_bounds.h = h;
    };
    card->when_paint = [](Container *root, Container *c) {
        auto window = swifi_window(root);
        auto dpi = window->dpi;
        auto b = c->real_bounds;
        drawRoundedRect(window->drawing_context, b.x, b.y, b.w, b.h, 8 * dpi, 1.0);
        set_argb(window->drawing_context, theme.row_background);
        window->drawing_context->fill();
        drawRoundedRect(window->drawing_context, b.x, b.y, b.w, b.h, 8 * dpi, std::floor(1.0 * dpi));
        set_argb(window->drawing_context, theme.row_border);
        window->drawing_context->set_line_width(std::floor(1.0 * dpi));
        window->drawing_context->stroke();
    };

    auto caption = card->child(FILL_SPACE, FILL_SPACE);
    caption->pre_layout = [](Container *root, Container *c, const Bounds &) {
        c->wanted_bounds.h = 20 * swifi_window(root)->dpi;
    };
    caption->when_paint = [](Container *root, Container *c) {
        swifi_text(root, c->real_bounds, "Add a custom shortcut", 12, theme.text);
    };

    auto line = card->child(::hbox, FILL_SPACE, FILL_SPACE);
    line->parent_bounds_limit_input_bounds = false;
    line->pre_layout = [](Container *root, Container *c, const Bounds &) {
        double dpi = swifi_window(root)->dpi;
        c->wanted_bounds.h = 32 * dpi;
        c->spacing = 8 * dpi;
    };
    auto name = make_placeholder_field(line, "Name");
    auto command = make_placeholder_field(line, "Command to run");
    // Tab / Shift+Tab move between the form's fields (traversal needs them named).
    name->name = "custom_shortcut_name";
    command->name = "custom_shortcut_command";
    for (auto field : {name, command}) {
        static_cast<STextEditorData *>(field->user_data)->options.on_tab = [card](Container *root, Container *c, bool previous) {
            std::weak_ptr<bool> lifetime = c->lifetime;
            std::weak_ptr<bool> card_lifetime = card->lifetime;
            // Next frame, or the newly focused field would also receive this Tab.
            windowing::timer(settings_app, 1, [root, c, card, previous, lifetime, card_lifetime](void *) {
                std::lock_guard<std::mutex> lock(settings_app->mutex);
                if (lifetime.expired() || card_lifetime.expired())
                    return;
                if (previous)
                    activate_previous_activatable(root, c, card);
                else
                    activate_next_activatable(root, c, card);
                swifi_redraw(root);
            }, nullptr);
        };
    }
    // The command usually needs more room than the name.
    auto name_layout = name->pre_layout;
    name->pre_layout = [name_layout](Container *root, Container *c, const Bounds &b) {
        if (name_layout)
            name_layout(root, c, b);
        c->wanted_bounds.w = 200 * swifi_window(root)->dpi;
        c->wanted_bounds.h = 32 * swifi_window(root)->dpi;
    };
    auto command_layout = command->pre_layout;
    command->pre_layout = [command_layout](Container *root, Container *c, const Bounds &b) {
        if (command_layout)
            command_layout(root, c, b);
        c->wanted_bounds.h = 32 * swifi_window(root)->dpi;
    };
    auto can_add = [command]() { return !sshortcut_field_text(command).empty(); };
    auto add = swifi_card_button(line, []() { return std::string("Add"); }, can_add, [name, command]() {
        sshortcut_add(sshortcut_field_text(name), sshortcut_field_text(command));
    });
    add->parent_bounds_limit_input_bounds = false;

    auto hint = card->child(FILL_SPACE, FILL_SPACE);
    hint->pre_layout = [](Container *root, Container *c, const Bounds &) {
        c->wanted_bounds.h = 18 * swifi_window(root)->dpi;
    };
    hint->when_paint = [](Container *root, Container *c) {
        swifi_text(root, c->real_bounds, "After adding, press the keys you want and then Set, or use Type to write them", 10, theme.text_secondary);
    };
}

static void fill_shortcuts_settings(Container *root, bool reload_rows = true) {
    auto right = container_by_name("settings_right", root);
    if (!right)
        return;
    for (auto child: right->children)
        delete child;
    right->children.clear();

    right->pre_layout = layout_right_page;
    auto padded_right = right->child(FILL_SPACE, FILL_SPACE);
    padded_right->name = "shortcuts_page";

    if (reload_rows)
        sshortcut_load_rows();
    sshortcut_page_stale = false;

    make_section_title(padded_right, "Custom shortcuts");
    make_vert_space(padded_right, 10);
    make_custom_shortcut_form(padded_right);

    int custom_count = 0;
    while (custom_count < (int) sshortcut_rows.size() && sshortcut_rows[custom_count].custom)
        custom_count++;
    if (custom_count > 0) {
        make_vert_space(padded_right, 8);
        for (int i = 0; i < custom_count; i++)
            make_shortcut_row(padded_right, i);
    }

    // The gap between the user's own shortcuts and the system ones.
    make_vert_space(padded_right, 28);
    make_section_title(padded_right, "System shortcuts");
    make_vert_space(padded_right, 10);

    auto filter = make_placeholder_field(padded_right, "Filter shortcuts by name or keys", sshortcut_filter,
                                         [padded_right](std::string text) {
        sshortcut_filter = text;
        sshortcut_apply_filter(padded_right);
    });
    auto filter_layout = filter->pre_layout;
    filter->pre_layout = [filter_layout](Container *root, Container *c, const Bounds &b) {
        if (filter_layout)
            filter_layout(root, c, b);
        c->wanted_bounds.h = 32 * swifi_window(root)->dpi;
    };
    make_vert_space(padded_right, 10);

    const int total = (int) sshortcut_rows.size();
    for (int i = custom_count; i < total; i++)
        make_shortcut_row(padded_right, i);

    auto empty = padded_right->child(FILL_SPACE, FILL_SPACE);
    empty->name = "system_empty";
    empty->pre_layout = [](Container *root, Container *c, const Bounds &) {
        c->wanted_bounds.h = 40 * swifi_window(root)->dpi;
    };
    empty->when_paint = [](Container *root, Container *c) {
        swifi_text(root, c->real_bounds, "No shortcuts match the filter", 11, theme.text_secondary);
    };
    sshortcut_apply_filter(padded_right);

    if (sshortcut_edit_after_rebuild >= 0 && sshortcut_edit_after_rebuild < total) {
        auto index = sshortcut_edit_after_rebuild;
        sshortcut_edit_after_rebuild = -1;
        sshortcut_start_editing(index);
    }
    sshortcut_edit_after_rebuild = -1;
}

/*************************************************
 *
 * Autostart page
 *
 *************************************************/

static const std::vector<std::string> sautostart_whens = {"Session start", "Plugin start"};

// Shown under a row's dropdown while it is hovered or focused with the keyboard.
static const char *sautostart_when_hint =
    "When this command runs\n"
    "Session start: once, when a new Hyprland session starts (not when Mylar is reloaded)\n"
    "Plugin start: every time Mylar loads, including reloads in the same session";

// Only touched on the settings thread; handed to the compositor on change.
static std::vector<SAutostart> sautostart_rows;
// The Autostart page must be rebuilt from sautostart_rows (rows were added or removed).
static bool sautostart_page_stale = false;
static void sautostart_commit() {
    auto rows = sautostart_rows;
    main_thread([rows]() {
        set->autostart = rows;
        settings::load_save_settings(true, set);
    });
}

static void sautostart_request_rebuild() {
    sautostart_page_stale = true;
    if (settings_mylar)
        windowing::redraw(settings_mylar->raw_window);
}

static void make_autostart_row(Container *parent, int index) {
    static const float height = 62;
    static const float round = 8;
    auto row = parent->child(::hbox, FILL_SPACE, height);
    row->name = "autostart_row";
    search_tag(row, sautostart_rows[index].command, sautostart_rows[index].when);
    row->parent_bounds_limit_input_bounds = false;
    row->pre_layout = [](Container *root, Container *c, const Bounds &) {
        double dpi = swifi_window(root)->dpi;
        c->wanted_bounds.h = height * dpi;
        c->wanted_pad = Bounds(20 * dpi, 14 * dpi, 14 * dpi, 14 * dpi);
        c->spacing = 8 * dpi;
    };
    row->when_paint = [](Container *root, Container *c) {
        auto window = swifi_window(root);
        auto cr = window->drawing_context;
        auto dpi = window->dpi;

        // Neighbouring rows form one rounded group and share their border line.
        bool is_first = true, is_last = true;
        if (auto parent = c->parent) {
            auto &siblings = parent->children;
            auto it = std::find(siblings.begin(), siblings.end(), c);
            if (it != siblings.begin())
                is_first = (*(it - 1))->name != c->name;
            if (it != siblings.end() && it + 1 != siblings.end())
                is_last = (*(it + 1))->name != c->name;
        }
        auto b = c->real_bounds;
        const auto overlap = (std::floor(1.0 * dpi) + 1.0) * 0.5;
        b.y -= overlap;
        b.h += 2.0 * overlap;

        drawRoundedRect(cr, b.x, b.y, b.w, b.h, round * dpi, 0, is_first, is_first, is_last, is_last);
        set_argb(cr, theme.row_background);
        cr->fill();
        drawRoundedRect(cr, b.x, b.y, b.w, b.h, round * dpi, std::floor(1.0 * dpi), is_first, is_first, is_last, is_last);
        set_argb(cr, theme.row_border);
        cr->set_line_width(std::floor(1.0 * dpi));
        cr->stroke();
    };

    auto command = row->child(FILL_SPACE, FILL_SPACE);
    command->when_paint = [index](Container *root, Container *c) {
        if (index >= (int) sautostart_rows.size())
            return;
        swifi_text(root, c->real_bounds, sautostart_rows[index].command, 12, theme.text);
    };

    make_dropdown(row, sautostart_rows[index].when, sautostart_whens, [index](std::string picked) {
        if (index >= (int) sautostart_rows.size())
            return;
        sautostart_rows[index].when = picked;
        sautostart_commit();
    });
    auto when = row->children.back();
    when->parent_bounds_limit_input_bounds = false;
    auto paint_when = std::move(when->when_paint);
    when->when_paint = [paint_when](Container *root, Container *c) {
        if (paint_when)
            paint_when(root, c);
        auto &focus = focus_of(root);
        const bool focused = focus.visible && focus.current == c;
        if (!dropdown_popup && (c->state.mouse_hovering || focused)) {
            field_hint_text = sautostart_when_hint;
            field_hint_anchor = c->real_bounds;
        }
    };
    when->when_mouse_enters_container = [](Container *root, Container *) { swifi_redraw(root); };
    when->when_mouse_leaves_container = [](Container *root, Container *) { swifi_redraw(root); };

    swifi_card_button(row, []() { return std::string("Remove"); }, []() { return true; }, [index]() {
        if (index >= (int) sautostart_rows.size())
            return;
        sautostart_rows.erase(sautostart_rows.begin() + index);
        sautostart_commit();
        sautostart_request_rebuild();
    });
}

// Command and an Add button; new commands run at session start until changed on their row.
static void make_autostart_form(Container *parent) {
    auto card = parent->child(::vbox, FILL_SPACE, FILL_SPACE);
    card->parent_bounds_limit_input_bounds = false;
    card->pre_layout = [](Container *root, Container *c, const Bounds &b) {
        double dpi = swifi_window(root)->dpi;
        c->wanted_pad = Bounds(16 * dpi, 12 * dpi, 12 * dpi, 12 * dpi);
        c->spacing = 8 * dpi;
        double h = c->wanted_pad.y + c->wanted_pad.h;
        int shown = 0;
        for (auto child : c->children) {
            if (!child->exists)
                continue;
            if (child->pre_layout)
                child->pre_layout(root, child, b);
            h += child->wanted_bounds.h;
            shown++;
        }
        h += std::max(0, shown - 1) * c->spacing;
        c->wanted_bounds.h = h;
    };
    card->when_paint = [](Container *root, Container *c) {
        auto window = swifi_window(root);
        auto dpi = window->dpi;
        auto b = c->real_bounds;
        drawRoundedRect(window->drawing_context, b.x, b.y, b.w, b.h, 8 * dpi, 1.0);
        set_argb(window->drawing_context, theme.row_background);
        window->drawing_context->fill();
        drawRoundedRect(window->drawing_context, b.x, b.y, b.w, b.h, 8 * dpi, std::floor(1.0 * dpi));
        set_argb(window->drawing_context, theme.row_border);
        window->drawing_context->set_line_width(std::floor(1.0 * dpi));
        window->drawing_context->stroke();
    };

    auto caption = card->child(FILL_SPACE, FILL_SPACE);
    caption->pre_layout = [](Container *root, Container *c, const Bounds &) {
        c->wanted_bounds.h = 20 * swifi_window(root)->dpi;
    };
    caption->when_paint = [](Container *root, Container *c) {
        swifi_text(root, c->real_bounds, "Add a command", 12, theme.text);
    };

    auto line = card->child(::hbox, FILL_SPACE, FILL_SPACE);
    line->parent_bounds_limit_input_bounds = false;
    line->pre_layout = [](Container *root, Container *c, const Bounds &) {
        double dpi = swifi_window(root)->dpi;
        c->wanted_bounds.h = 34 * dpi;
        c->spacing = 8 * dpi;
    };
    auto command = make_placeholder_field(line, "Command to run");
    auto command_layout = command->pre_layout;
    command->pre_layout = [command_layout](Container *root, Container *c, const Bounds &b) {
        if (command_layout)
            command_layout(root, c, b);
        c->wanted_bounds.h = 34 * swifi_window(root)->dpi;
    };
    auto add_command = [command]() {
        auto text = sshortcut_field_text(command);
        if (text.empty())
            return;
        sautostart_rows.push_back({"Session start", text});
        sautostart_commit();
        sautostart_request_rebuild();
    };
    auto edit_key = std::move(command->when_key_event);
    command->when_key_event = [edit_key, add_command](Container *root, Container *c, int key, bool pressed,
                                                      xkb_keysym_t sym, int mods, bool is_text, std::string text) {
        if (pressed && (sym == XKB_KEY_Return || sym == XKB_KEY_KP_Enter)) {
            add_command();
            return;
        }
        if (edit_key)
            edit_key(root, c, key, pressed, sym, mods, is_text, text);
    };
    auto can_add = [command]() { return !sshortcut_field_text(command).empty(); };
    auto add = swifi_card_button(line, []() { return std::string("Add"); }, can_add, add_command);
    add->parent_bounds_limit_input_bounds = false;
}

static void fill_autostart_settings(Container *root, bool reload_rows = true) {
    auto padded_right = settings_begin_page(root);
    if (!padded_right)
        return;
    padded_right->name = "autostart_page";

    if (reload_rows)
        sautostart_rows = set->autostart;
    sautostart_page_stale = false;

    make_section_title(padded_right, "Autostart");
    make_vert_space(padded_right, 10);
    make_autostart_form(padded_right);

    if (!sautostart_rows.empty()) {
        make_vert_space(padded_right, 8);
        for (int i = 0; i < (int) sautostart_rows.size(); i++)
            make_autostart_row(padded_right, i);
    }
}

/*************************************************
 *
 * Login screen (SDDM)
 *
 *************************************************/

struct SddmSystemState {
    std::string theme;
    std::string theme_dir;
    std::string applied_scale;
    std::string theme_default_background;
    std::string applied_background;
};

static std::mutex ssddm_mutex;
static bool ssddm_page_stale = false;
static std::atomic<bool> ssddm_applying = false;
static std::string ssddm_status;
static bool ssddm_custom_scale_mode = false;
static SddmSystemState ssddm_sys;

static const std::vector<std::string> sddm_wallpaper_modes = {"Display", "Custom", "Theme default"};
static const std::vector<std::string> sddm_scale_presets = {"1.0", "1.25", "1.5", "1.75", "2.0"};

static std::string sddm_custom_wallpaper_path() {
    return (wallpaper_config_dir() / "sddm-wall.png").string();
}

static std::string sddm_format_scale(double s) {
    if (!std::isfinite(s) || s <= 0)
        s = 1.0;
    s = std::clamp(s, 0.5, 4.0);
    char buf[32];
    snprintf(buf, sizeof(buf), "%.2f", s);
    std::string out = buf;
    // Trim trailing zero only when there are two decimal digits and the last is '0' (keep e.g. "1.0", "1.5").
    if (out.size() >= 4 && out.back() == '0' && out[out.size() - 3] == '.')
        out.pop_back();
    return out;
}

static bool sddm_parse_scale(const std::string &text, double &out) {
    std::string t = trim(text);
    if (t.empty())
        return false;
    char *end = nullptr;
    double v = std::strtod(t.c_str(), &end);
    if (!end || *end != '\0' || !std::isfinite(v) || v < 0.5 || v > 4.0)
        return false;
    out = v;
    return true;
}

static WallMonitor sddm_pick_monitor(bool *found_real = nullptr) {
    auto monitors = wallpaper_monitors_copy();
    if (!monitors.empty()) {
        if (found_real)
            *found_real = true;
        if (!set->sddm_wallpaper_monitor.empty()) {
            for (const auto &m : monitors) {
                if (m.name == set->sddm_wallpaper_monitor)
                    return m;
            }
        }
        return monitors.front();
    }
    if (found_real)
        *found_real = false;
    WallMonitor fallback;
    fallback.name = "Monitor";
    fallback.w = 1920;
    fallback.h = 1080;
    fallback.scale = 1.0;
    return fallback;
}

static double sddm_display_scale_value() {
    bool found_real = false;
    auto m = sddm_pick_monitor(&found_real);
    if (found_real && m.scale > 0)
        return m.scale;
    for (const auto &mon : set->monitors) {
        double v = 1.0;
        if (sddm_parse_scale(mon.scale, v))
            return v;
    }
    return 1.0;
}

static std::string sddm_effective_scale() {
    if (set->sddm_scale == "auto")
        return sddm_format_scale(sddm_display_scale_value());
    double v = 1.0;
    if (sddm_parse_scale(set->sddm_scale, v))
        return sddm_format_scale(v);
    return sddm_format_scale(sddm_display_scale_value());
}

static bool sddm_scale_is_preset(const std::string &scale) {
    double val = 0;
    if (!sddm_parse_scale(scale, val))
        return false;
    std::string fmt = sddm_format_scale(val);
    for (const auto &p : sddm_scale_presets) {
        if (p == fmt)
            return true;
    }
    return false;
}

static std::string sddm_scale_dropdown_label() {
    if (set->sddm_scale == "auto")
        return "Auto";
    if (!ssddm_custom_scale_mode && sddm_scale_is_preset(set->sddm_scale)) {
        double val = 1.0;
        sddm_parse_scale(set->sddm_scale, val);
        return sddm_format_scale(val);
    }
    return "Custom";
}

static void sddm_parse_ini_file(const std::filesystem::path &path,
                                std::unordered_map<std::string, std::unordered_map<std::string, std::string>> &sections) {
    std::ifstream in(path);
    if (!in.is_open())
        return;
    std::string section;
    std::string line;
    while (std::getline(in, line)) {
        std::string t = trim(line);
        if (t.empty() || t[0] == '#' || t[0] == ';')
            continue;
        if (t.front() == '[' && t.back() == ']') {
            section = trim(t.substr(1, t.size() - 2));
            continue;
        }
        auto eq = t.find('=');
        if (eq == std::string::npos)
            continue;
        std::string key = trim(t.substr(0, eq));
        std::string val = trim(t.substr(eq + 1));
        sections[section][key] = val;
    }
}

static std::string sddm_extract_qt_scale(const std::string &greeter_env) {
    std::stringstream ss(greeter_env);
    std::string item;
    while (std::getline(ss, item, ',')) {
        std::string t = trim(item);
        const std::string prefix = "QT_SCALE_FACTOR=";
        if (t.rfind(prefix, 0) == 0)
            return trim(t.substr(prefix.size()));
    }
    return "";
}

static std::string sddm_resolve_theme_bg(const std::string &theme_dir, const std::string &bg) {
    if (bg.empty())
        return "";
    std::filesystem::path p(bg);
    if (p.is_absolute())
        return p.string();
    return (std::filesystem::path(theme_dir) / p).string();
}

static SddmSystemState sddm_read_system_state() {
    SddmSystemState state;
    std::unordered_map<std::string, std::unordered_map<std::string, std::string>> cfg;

    auto read_dir = [&](const char *dir) {
        std::error_code ec;
        if (!std::filesystem::is_directory(dir, ec))
            return;
        std::vector<std::filesystem::path> files;
        for (const auto &entry : std::filesystem::directory_iterator(dir, ec)) {
            if (entry.is_regular_file(ec) && entry.path().extension() == ".conf")
                files.push_back(entry.path());
        }
        std::sort(files.begin(), files.end());
        for (const auto &f : files)
            sddm_parse_ini_file(f, cfg);
    };

    read_dir("/usr/lib/sddm/sddm.conf.d");
    read_dir("/etc/sddm.conf.d");
    sddm_parse_ini_file("/etc/sddm.conf", cfg);

    state.theme = cfg["Theme"]["Current"];
    state.applied_scale = sddm_extract_qt_scale(cfg["General"]["GreeterEnvironment"]);

    std::error_code ec;
    if (state.theme.empty()) {
        if (std::filesystem::is_directory("/usr/share/sddm/themes/breeze", ec))
            state.theme = "breeze";
        else if (std::filesystem::is_directory("/usr/share/sddm/themes", ec)) {
            for (const auto &entry : std::filesystem::directory_iterator("/usr/share/sddm/themes", ec)) {
                if (entry.is_directory(ec)) {
                    state.theme = entry.path().filename().string();
                    break;
                }
            }
        }
    }

    if (!state.theme.empty()) {
        state.theme_dir = "/usr/share/sddm/themes/" + state.theme;
        std::unordered_map<std::string, std::unordered_map<std::string, std::string>> base_cfg;
        sddm_parse_ini_file(std::filesystem::path(state.theme_dir) / "theme.conf", base_cfg);
        state.theme_default_background = sddm_resolve_theme_bg(state.theme_dir, base_cfg["General"]["background"]);

        auto user_cfg = base_cfg;
        sddm_parse_ini_file(std::filesystem::path(state.theme_dir) / "theme.conf.user", user_cfg);
        state.applied_background = sddm_resolve_theme_bg(state.theme_dir, user_cfg["General"]["background"]);
    }
    return state;
}

static bool sddm_set_custom_wallpaper_from_file(const std::string &file) {
    if (file.empty() || !std::filesystem::is_regular_file(file))
        return false;
    std::string quoted = "'";
    for (char ch : file) {
        if (ch == '\'')
            quoted += "'\\''";
        else
            quoted += ch;
    }
    quoted += "'";
    auto mime = trim_newline(execAndGet(std::string("file --mime-type -b " + quoted).c_str()));
    if (!mime.starts_with("image/"))
        return false;
    std::filesystem::path dest = sddm_custom_wallpaper_path();
    std::error_code ec;
    std::filesystem::create_directories(dest.parent_path(), ec);
    std::filesystem::copy_file(file, dest, std::filesystem::copy_options::overwrite_existing, ec);
    if (ec)
        return false;
    set->sddm_wallpaper_mode = "Custom";
    main_thread([]() { settings::load_save_settings(true, set); });
    ssddm_page_stale = true;
    settings_request_redraw();
    return true;
}

static void sddm_file_dropped(std::string uri_list) {
    spawn_settings_worker([uri_list]() {
        for (const auto &path : paths_from_uri_list(uri_list))
            if (sddm_set_custom_wallpaper_from_file(path))
                break;
    });
}

// Renders the Display wallpaper with the exact same cropping/layout as heart.cpp.
static bool sddm_render_display_wallpaper_png(const WallMonitor &m, const std::string &dest_path, std::string &err) {
    auto monitors = wallpaper_monitors_copy();
    if (monitors.empty())
        monitors.push_back(m);
    auto overrides = wallpaper_parse_overrides(set->wallpaper_monitors);
    auto res = wallpaper_resolve(wallpaper_shared_params(*set), set->wallpaper_span, overrides, monitors, m);

    std::error_code ec;
    if (!std::filesystem::is_regular_file(res.path, ec)) {
        err = "No Display wallpaper is set yet";
        return false;
    }

    cairo_surface_t *src = nullptr;
    load_icon_full_path(&src, res.path);
    if (!src) {
        err = "Could not load Display wallpaper image";
        return false;
    }

    const double iw = cairo_image_surface_get_width(src);
    const double ih = cairo_image_surface_get_height(src);
    const int ow = std::max(1, (int) std::round(m.w * (m.scale > 0 ? m.scale : 1.0)));
    const int oh = std::max(1, (int) std::round(m.h * (m.scale > 0 ? m.scale : 1.0)));

    cairo_surface_t *out = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, ow, oh);
    auto cr = cairo_create(out);
    cairo_set_source_rgb(cr, 0, 0, 0);
    cairo_paint(cr);
    for (auto &r : wallpaper_monitor_rects(res, iw, ih, m)) {
        cairo_save(cr);
        cairo_rectangle(cr, r.x, r.y, r.w, r.h);
        cairo_clip(cr);
        cairo_translate(cr, r.x, r.y);
        cairo_scale(cr, r.w / iw, r.h / ih);
        cairo_set_source_surface(cr, src, 0, 0);
        auto pattern = cairo_get_source(cr);
        cairo_pattern_set_filter(pattern, CAIRO_FILTER_GOOD);
        cairo_pattern_set_extend(pattern, CAIRO_EXTEND_PAD);
        cairo_paint(cr);
        cairo_restore(cr);
    }
    cairo_destroy(cr);
    cairo_surface_flush(out);
    cairo_surface_destroy(src);

    cairo_status_t st = cairo_surface_write_to_png(out, dest_path.c_str());
    cairo_surface_destroy(out);
    if (st != CAIRO_STATUS_SUCCESS) {
        err = "Failed to encode cropped wallpaper PNG";
        return false;
    }
    return true;
}

static std::string sddm_build_theme_conf_user(const std::filesystem::path &existing_path,
                                              bool set_background, const std::string &background_path) {
    std::vector<std::string> lines;
    {
        std::ifstream in(existing_path);
        std::string line;
        while (std::getline(in, line))
            lines.push_back(line);
    }

    std::vector<std::string> out;
    std::string section;
    bool saw_general = false;
    bool wrote_type = false;
    bool wrote_bg = false;

    auto flush_general = [&]() {
        if (!set_background)
            return;
        if (!wrote_type) {
            out.push_back("type=image");
            wrote_type = true;
        }
        if (!wrote_bg) {
            out.push_back("background=" + background_path);
            wrote_bg = true;
        }
    };

    for (const auto &raw : lines) {
        std::string t = trim(raw);
        if (!t.empty() && t.front() == '[' && t.back() == ']') {
            if (section == "General")
                flush_general();
            section = trim(t.substr(1, t.size() - 2));
            if (section == "General")
                saw_general = true;
            out.push_back(raw);
            continue;
        }
        if (section == "General") {
            auto eq = t.find('=');
            if (eq != std::string::npos) {
                std::string key = trim(t.substr(0, eq));
                if (key == "background") {
                    if (set_background && !wrote_bg) {
                        out.push_back("background=" + background_path);
                        wrote_bg = true;
                    }
                    continue;
                }
                if (key == "type" && set_background) {
                    if (!wrote_type) {
                        out.push_back("type=image");
                        wrote_type = true;
                    }
                    continue;
                }
            }
        }
        out.push_back(raw);
    }

    if (section == "General") {
        flush_general();
    } else if (!saw_general && set_background) {
        if (!out.empty() && !out.back().empty())
            out.push_back("");
        out.push_back("[General]");
        flush_general();
    }

    std::string result;
    for (const auto &l : out)
        result += l + "\n";
    return result;
}

static std::string sddm_shell_quote(const std::string &s) {
    std::string out = "'";
    for (char ch : s) {
        if (ch == '\'')
            out += "'\\''";
        else
            out += ch;
    }
    out += "'";
    return out;
}

static void sddm_save_settings() {
    main_thread([]() { settings::load_save_settings(true, set); });
}

static bool sddm_convert_to_png(const std::string &src_path, const std::string &dest_path, std::string &err) {
    cairo_surface_t *src = nullptr;
    load_icon_full_path(&src, src_path);
    if (!src) {
        err = "Could not load custom wallpaper image";
        return false;
    }
    cairo_status_t st = cairo_surface_write_to_png(src, dest_path.c_str());
    cairo_surface_destroy(src);
    if (st != CAIRO_STATUS_SUCCESS) {
        err = "Failed to encode custom wallpaper PNG";
        return false;
    }
    return true;
}

static void sddm_apply_async() {
    if (ssddm_applying.exchange(true))
        return;
    {
        std::lock_guard<std::mutex> lock(ssddm_mutex);
        ssddm_status = "Applying settings...";
    }
    settings_redraw_from_ui();

    const std::string mode = set->sddm_wallpaper_mode;
    const std::string scale = sddm_effective_scale();
    const WallMonitor monitor = sddm_pick_monitor();

    spawn_settings_worker([mode, scale, monitor]() {
        auto finish = [](const std::string &status, const std::string &toast = "", bool refresh = false) {
            {
                std::lock_guard<std::mutex> lock(ssddm_mutex);
                ssddm_status = status;
                if (refresh)
                    ssddm_sys = sddm_read_system_state();
            }
            ssddm_applying = false;
            ssddm_page_stale = true;
            if (!toast.empty())
                main_thread([toast]() { notify(toast); });
            settings_request_redraw();
        };

        auto sys = sddm_read_system_state();
        std::error_code ec;
        if (sys.theme.empty() || sys.theme_dir.empty() || !std::filesystem::is_directory(sys.theme_dir, ec)) {
            finish("No SDDM theme directory found in /usr/share/sddm/themes", "SDDM theme directory not found");
            return;
        }

        char tmpl[] = "/tmp/mylar-sddm-XXXXXX";
        char *dir_c = mkdtemp(tmpl);
        if (!dir_c) {
            finish("Failed to create temporary directory", "Failed to apply SDDM settings");
            return;
        }
        std::filesystem::path tmpdir(dir_c);
        defer(std::filesystem::remove_all(tmpdir, ec));

        const std::filesystem::path staged_wall = tmpdir / "mylar-wallpaper.png";
        const std::filesystem::path dest_wall = std::filesystem::path(sys.theme_dir) / "mylar-wallpaper.png";
        bool install_wall = false;

        if (mode == "Display") {
            std::string err;
            if (!sddm_render_display_wallpaper_png(monitor, staged_wall.string(), err)) {
                finish(err, err);
                return;
            }
            install_wall = true;
        } else if (mode == "Custom") {
            auto custom_path = sddm_custom_wallpaper_path();
            if (!std::filesystem::is_regular_file(custom_path, ec)) {
                finish("Choose a custom wallpaper image first", "Choose a custom SDDM wallpaper first");
                return;
            }
            std::string err;
            if (!sddm_convert_to_png(custom_path, staged_wall.string(), err)) {
                finish(err, err);
                return;
            }
            install_wall = true;
        }

        const std::filesystem::path staged_theme_user = tmpdir / "theme.conf.user";
        {
            std::ofstream out(staged_theme_user);
            out << sddm_build_theme_conf_user(std::filesystem::path(sys.theme_dir) / "theme.conf.user",
                                              install_wall, dest_wall.string());
        }

        const std::filesystem::path staged_greeter_conf = tmpdir / "99-mylar.conf";
        {
            std::ofstream out(staged_greeter_conf);
            out << "# Managed by Mylar Settings\n"
                << "[General]\n"
                << "GreeterEnvironment=QT_SCALE_FACTOR=" << scale << "\n";
        }

        std::string cmd =
            "install -Dm644 " + sddm_shell_quote(staged_greeter_conf.string()) + " /etc/sddm.conf.d/99-mylar.conf && " +
            (install_wall
                 ? ("install -Dm644 " + sddm_shell_quote(staged_wall.string()) + " " + sddm_shell_quote(dest_wall.string()))
                 : ("rm -f " + sddm_shell_quote(dest_wall.string()))) +
            " && install -Dm644 " + sddm_shell_quote(staged_theme_user.string()) + " " +
            sddm_shell_quote((std::filesystem::path(sys.theme_dir) / "theme.conf.user").string());

        std::string output;
        int status = run_cancellable_command({"pkexec", "sh", "-c", cmd}, output);
        if (status == 0) {
            finish("Applied to SDDM (" + sys.theme + ", scale " + scale + ")", "SDDM settings applied", true);
        } else if (status == 126 || status == 127) {
            finish("Authentication cancelled");
        } else {
            finish("Failed to apply SDDM settings", "Failed to apply SDDM settings");
        }
    });
}

static void paint_sddm_preview(Container *root, Container *c) {
    auto window = swifi_window(root);
    auto cr = window->drawing_context;
    auto dpi = window->dpi;
    auto b = c->real_bounds;
    paint_smooth_frame(cr, b, settings_control_rounding * dpi, theme.dropdown_background, theme.dropdown_border);

    auto m = sddm_pick_monitor();
    const double aspect = (m.w > 0 && m.h > 0) ? (m.w / m.h) : (16.0 / 9.0);
    const double pad = 16 * dpi;
    double avail_w = std::max(1.0, b.w - pad * 2);
    double avail_h = std::max(1.0, b.h - pad * 2);
    double pw = avail_w;
    double ph = pw / aspect;
    if (ph > avail_h) {
        ph = avail_h;
        pw = ph * aspect;
    }
    Bounds screen(b.x + (b.w - pw) * .5, b.y + (b.h - ph) * .5, pw, ph);

    cr->save();
    drawRoundedRect(cr, screen.x, screen.y, screen.w, screen.h, 6 * dpi, 1.0);
    cr->clip_path();

    cr->set_source_rgba(0.08, 0.09, 0.11, 1.0);
    cr->new_path();
    cr->rectangle(screen.x, screen.y, screen.w, screen.h);
    cr->fill();

    const std::string mode = set->sddm_wallpaper_mode;
    if (mode == "Display") {
        auto monitors = wallpaper_monitors_copy();
        if (monitors.empty())
            monitors.push_back(m);
        auto overrides = wallpaper_parse_overrides(set->wallpaper_monitors);
        auto res = wallpaper_resolve(wallpaper_shared_params(*set), set->wallpaper_span, overrides, monitors, m);
        if (auto thumb = wall_thumb(res.path)) {
            const double k = screen.w / std::max(1.0, m.w);
            for (auto &r : wallpaper_area_rects(res.params, thumb->iw, thumb->ih, res.area.w, res.area.h, res.pixel_scale)) {
                Bounds rb(screen.x + (res.area.x + r.x - m.x) * k,
                          screen.y + (res.area.y + r.y - m.y) * k,
                          r.w * k, r.h * k);
                if (rb.x > screen.x + screen.w || rb.y > screen.y + screen.h ||
                    rb.x + rb.w < screen.x || rb.y + rb.h < screen.y)
                    continue;
                wall_draw_image(cr, thumb, rb, 1.0);
            }
        }
    } else if (mode == "Custom") {
        if (auto thumb = wall_thumb(sddm_custom_wallpaper_path())) {
            auto r = wallpaper_image_rect(WallParams(), thumb->iw, thumb->ih, screen.w, screen.h, 1.0);
            wall_draw_image(cr, thumb, Bounds(screen.x + r.x, screen.y + r.y, r.w, r.h), 1.0);
        }
    } else {
        std::string default_bg;
        {
            std::lock_guard<std::mutex> lock(ssddm_mutex);
            default_bg = ssddm_sys.theme_default_background;
        }
        if (!default_bg.empty()) {
            if (auto thumb = wall_thumb(default_bg)) {
                auto r = wallpaper_image_rect(WallParams(), thumb->iw, thumb->ih, screen.w, screen.h, 1.0);
                wall_draw_image(cr, thumb, Bounds(screen.x + r.x, screen.y + r.y, r.w, r.h), 1.0);
            }
        }
    }

    // Subtle dark scrim + minimalist login UI mockup scaled with the chosen DPI factor.
    cr->set_source_rgba(0, 0, 0, 0.25);
    cr->new_path();
    cr->rectangle(screen.x, screen.y, screen.w, screen.h);
    cr->fill();

    double eff_scale = 1.0;
    sddm_parse_scale(sddm_effective_scale(), eff_scale);
    const double ui = std::clamp(eff_scale, 0.5, 2.5) * (screen.h / 220.0);

    // Mock clock at top-center
    swifi_text(root, Bounds(screen.x, screen.y + 18 * ui, screen.w, 26 * ui), "12:00",
               std::clamp(16.0 * ui / dpi, 7.0, 28.0), RGBA(1, 1, 1, 0.92), true);

    // Mock avatar circle + password field in center
    const double avatar_r = std::clamp(16.0 * ui, 8.0 * dpi, screen.h * 0.16);
    const double cx = screen.x + screen.w * 0.5;
    const double cy = screen.y + screen.h * 0.52;
    drawRoundedRect(cr, cx - avatar_r, cy - avatar_r, avatar_r * 2, avatar_r * 2, avatar_r, 1.0);
    set_argb(cr, RGBA(1, 1, 1, 0.28));
    cr->fill();
    drawRoundedRect(cr, cx - avatar_r, cy - avatar_r, avatar_r * 2, avatar_r * 2, avatar_r, std::max(1.0, 1.0 * dpi));
    set_argb(cr, RGBA(1, 1, 1, 0.6));
    cr->stroke();

    const double field_w = std::clamp(92.0 * ui, 48.0 * dpi, screen.w * 0.65);
    const double field_h = std::clamp(14.0 * ui, 8.0 * dpi, screen.h * 0.12);
    const double fy = cy + avatar_r + 8 * ui;
    drawRoundedRect(cr, cx - field_w * 0.5, fy, field_w, field_h, field_h * 0.5, 1.0);
    set_argb(cr, RGBA(0, 0, 0, 0.45));
    cr->fill();
    drawRoundedRect(cr, cx - field_w * 0.5, fy, field_w, field_h, field_h * 0.5, std::max(1.0, 1.0 * dpi));
    set_argb(cr, RGBA(1, 1, 1, 0.45));
    cr->stroke();

    // Corner badge: active theme + effective scale
    std::string theme_name;
    {
        std::lock_guard<std::mutex> lock(ssddm_mutex);
        theme_name = ssddm_sys.theme;
    }
    std::string badge = (theme_name.empty() ? "SDDM" : theme_name) + " · " + sddm_effective_scale() + "x";
    double tw = swifi_text_width(root, badge, 9);
    Bounds pill(screen.x + 8 * dpi, screen.y + screen.h - 24 * dpi, tw + 14 * dpi, 18 * dpi);
    drawRoundedRect(cr, pill.x, pill.y, pill.w, pill.h, 4 * dpi, 1.0);
    set_argb(cr, RGBA(0, 0, 0, 0.55));
    cr->fill();
    swifi_text(root, pill, badge, 9, RGBA(1, 1, 1, 0.95), true);

    cr->restore();

    drawRoundedRect(cr, screen.x, screen.y, screen.w, screen.h, 6 * dpi, std::max(1.0, (double) std::floor(dpi)));
    set_argb(cr, RGBA(1, 1, 1, 0.2));
    cr->stroke();
}

static void make_sddm_preview(Container *parent) {
    static const int height = 220;
    auto c = parent->child(FILL_SPACE, height);
    c->name = "sddm_preview";
    c->pre_layout = [](Container *root, Container *c, const Bounds &) {
        c->wanted_bounds.h = height * swifi_window(root)->dpi;
    };
    c->when_paint = paint_sddm_preview;
}

static void fill_sddm_settings(Container *root) {
    auto padded_right = settings_begin_page(root);
    if (!padded_right)
        return;
    padded_right->name = "sddm_page";
    ssddm_page_stale = false;

    SddmSystemState sys = sddm_read_system_state();
    std::string status_text;
    {
        std::lock_guard<std::mutex> lock(ssddm_mutex);
        ssddm_sys = sys;
        status_text = ssddm_status;
    }

    make_section_title(padded_right, "Login screen (SDDM)");
    make_vert_space(padded_right, 10);
    make_sddm_preview(padded_right);
    make_vert_space(padded_right, 14);

    // ---- Wallpaper ----
    make_section_title(padded_right, "Wallpaper");
    make_vert_space(padded_right, 8);

    std::string wp_desc = "Uses your Display wallpaper with the same cropping";
    if (set->sddm_wallpaper_mode == "Custom")
        wp_desc = "Uses a custom image chosen for SDDM";
    else if (set->sddm_wallpaper_mode == "Theme default")
        wp_desc = "Uses the active SDDM theme's default background";

    make_dropdown_option(padded_right, "Wallpaper source", wp_desc, "\uE8B9",
                         set->sddm_wallpaper_mode, sddm_wallpaper_modes, [](std::string selected) {
        if (selected == set->sddm_wallpaper_mode)
            return;
        set->sddm_wallpaper_mode = selected;
        sddm_save_settings();
        ssddm_page_stale = true;
        settings_redraw_from_ui();
    });

    auto monitors = wallpaper_monitors_copy();
    if (set->sddm_wallpaper_mode == "Display" && monitors.size() > 1) {
        std::vector<std::string> mon_options = {"Primary"};
        bool selected_exists = false;
        for (const auto &m : monitors) {
            mon_options.push_back(m.name);
            if (m.name == set->sddm_wallpaper_monitor)
                selected_exists = true;
        }
        if (!selected_exists)
            set->sddm_wallpaper_monitor.clear();
        std::string current_mon = set->sddm_wallpaper_monitor.empty() ? "Primary" : set->sddm_wallpaper_monitor;
        make_dropdown_option(padded_right, "Source monitor", "Which monitor's wallpaper and crop to use", "\uE7F4",
                             current_mon, mon_options, [](std::string selected) {
            set->sddm_wallpaper_monitor = (selected == "Primary") ? "" : selected;
            sddm_save_settings();
            settings_redraw_from_ui();
        });
    }

    if (set->sddm_wallpaper_mode == "Custom") {
        std::error_code ec;
        bool has_custom = std::filesystem::is_regular_file(sddm_custom_wallpaper_path(), ec);
        make_button_option(padded_right, "Custom wallpaper",
                           has_custom ? "Custom image selected (can also drag and drop here)" : "Choose an image or drag and drop one here",
                           "Choose file", []() {
            static std::atomic<bool> chooser_open = false;
            if (chooser_open.exchange(true))
                return;
            spawn_settings_worker([]() {
                sddm_set_custom_wallpaper_from_file(trim_newline(choose_file()));
                chooser_open = false;
            });
        }, "\uE8B9");
    }

    make_vert_space(padded_right, 14);

    // ---- Display scale ----
    make_section_title(padded_right, "Display scale");
    make_vert_space(padded_right, 8);

    std::vector<std::string> scale_options = {"Auto", "1.0", "1.25", "1.5", "1.75", "2.0", "Custom"};
    std::string scale_desc = set->sddm_scale == "auto"
        ? ("Pulled from Display scale (currently " + sddm_format_scale(sddm_display_scale_value()) + ")")
        : ("Sets QT_SCALE_FACTOR=" + sddm_effective_scale() + " for the SDDM greeter");

    make_dropdown_option(padded_right, "Scale", scale_desc, "\uE740",
                         sddm_scale_dropdown_label(), scale_options, [](std::string selected) {
        if (selected == "Auto") {
            ssddm_custom_scale_mode = false;
            set->sddm_scale = "auto";
        } else if (selected == "Custom") {
            ssddm_custom_scale_mode = true;
            if (set->sddm_scale == "auto")
                set->sddm_scale = sddm_format_scale(sddm_display_scale_value());
        } else {
            ssddm_custom_scale_mode = false;
            set->sddm_scale = selected;
        }
        sddm_save_settings();
        ssddm_page_stale = true;
        settings_redraw_from_ui();
    });

    if (ssddm_custom_scale_mode || (set->sddm_scale != "auto" && !sddm_scale_is_preset(set->sddm_scale))) {
        make_reset_textfield(padded_right, "Custom scale", "DPI scale factor from 0.5 to 4.0 (e.g. 1.25)", "\uE740",
                             false, sddm_effective_scale(), "1.0", [](std::string value) {
            double parsed = 1.0;
            if (!sddm_parse_scale(value, parsed)) {
                main_thread([]() { notify("Enter a scale factor between 0.5 and 4.0"); });
                return;
            }
            set->sddm_scale = sddm_format_scale(parsed);
            sddm_save_settings();
            settings_redraw_from_ui();
        }, nullptr, "e.g. 1.0, 1.25, 1.5, 2.0");
    }

    make_vert_space(padded_right, 14);

    // ---- Apply ----
    make_section_title(padded_right, "System configuration");
    make_vert_space(padded_right, 8);

    std::string apply_desc;
    if (!status_text.empty()) {
        apply_desc = status_text;
    } else {
        apply_desc = "Theme: " + (sys.theme.empty() ? "unknown" : sys.theme);
        if (!sys.applied_scale.empty())
            apply_desc += " · Current SDDM scale: " + sys.applied_scale;
        apply_desc += " · Requires administrator privileges";
    }

    make_button_option(padded_right, "Apply to SDDM", apply_desc,
                       ssddm_applying.load() ? "Applying..." : "Apply",
                       []() { sddm_apply_async(); }, "\uE77B",
                       []() { return !ssddm_applying.load(); });
}

static void fill_wifi_settings(Container *root) {
    auto right = container_by_name("settings_right", root);
    if (!right)
        return;
    for (auto child: right->children)
        delete child;
    right->children.clear();

    right->pre_layout = layout_right_page;
    auto padded_right = right->child(FILL_SPACE, FILL_SPACE);

    make_section_title(padded_right, "Wi-Fi");
    make_vert_space(padded_right, 10);

    // Status card: state, adapter, refresh and on/off.
    auto card = padded_right->child(::hbox, FILL_SPACE, 70);
    card->pre_layout = [](Container *root, Container *c, const Bounds &) {
        double dpi = swifi_window(root)->dpi;
        c->wanted_bounds.h = 70 * dpi;
        c->wanted_pad = Bounds(0, 17 * dpi, 16 * dpi, 17 * dpi);
        c->spacing = 8 * dpi;
    };
    card->when_paint = [](Container *root, Container *c) {
        auto window = swifi_window(root);
        auto cr = window->drawing_context;
        auto dpi = window->dpi;
        auto b = c->real_bounds;
        drawRoundedRect(cr, b.x, b.y, b.w, b.h, 5 * dpi, 1.0);
        set_argb(cr, theme.row_background);
        cr->fill();

        const auto &view = settings_wifi->view;
        if (view.scanning || view.busy)
            paint_card_progress_dots(cr, b, dpi);

        // Border last so nothing drawn inside the card can sit on top of it.
        drawRoundedRect(cr, b.x, b.y, b.w, b.h, 5 * dpi, 1.0);
        set_argb(cr, theme.row_border);
        cr->stroke();
    };
    auto heading = card->child(FILL_SPACE, FILL_SPACE);
    heading->when_paint = [](Container *root, Container *c) {
        auto window = swifi_window(root);
        auto dpi = window->dpi;
        const auto &view = settings_wifi->view;
        auto b = c->real_bounds;
        bool on = view.status.service_running && !view.status.devices.empty() && view.status.wireless_enabled;
        swifi_glyph(window->drawing_context, b.x + 19 * dpi, b.y + b.h * .5 - 10 * dpi, "", 16 * dpi,
            on ? theme.text : swifi_dim(theme.text_secondary));
        swifi_text(root, Bounds(b.x + 60 * dpi, b.y - 6 * dpi, b.w - 60 * dpi, 24 * dpi), "Wi-Fi", 12, theme.text);
        swifi_text(root, Bounds(b.x + 60 * dpi, b.y + 16 * dpi, b.w - 60 * dpi, 22 * dpi),
            wifi_status_line(view), 10, theme.text_secondary);
    };
    // Adapter pill; with several adapters, clicking it cycles through them.
    auto adapter_label = []() {
        const auto &view = settings_wifi->view;
        std::string label;
        if (auto device = wifi_device(view.status)) label = device->interface;
        if (view.status.devices.size() > 1) label += "  ▾";
        return label;
    };
    auto adapter = card->child(0, FILL_SPACE);
    adapter->pre_layout = [adapter_label](Container *root, Container *c, const Bounds &) {
        auto dpi = swifi_window(root)->dpi;
        auto label = adapter_label();
        c->exists = !label.empty();
        c->wanted_bounds.w = std::min<double>(swifi_text_width(root, label, 10) + 20 * dpi, 160 * dpi);
    };
    adapter->when_paint = [adapter_label](Container *root, Container *c) {
        auto window = swifi_window(root);
        auto dpi = window->dpi;
        const bool switchable = settings_wifi->view.status.devices.size() > 1;
        const double h = 24 * dpi;
        Bounds pill(c->real_bounds.x, c->real_bounds.y + (c->real_bounds.h - h) * .5, c->real_bounds.w, h);
        set_argb(window->drawing_context, switchable && c->state.mouse_pressing ? theme.button_pressed :
            switchable && c->state.mouse_hovering ? theme.button_hover : theme.segment_background);
        drawRoundedRect(window->drawing_context, pill.x, pill.y, pill.w, pill.h, h * .5, 1.0);
        window->drawing_context->fill();
        pill.x += 10 * dpi;
        pill.w -= 20 * dpi;
        swifi_text(root, pill, adapter_label(), 10, theme.text_secondary, true);
    };
    adapter->when_clicked = [](Container *, Container *) {
        if (settings_wifi->view.status.devices.size() > 1) wifi_command({"device", "", ""});
    };
    swifi_card_button(card, []() {
        return settings_wifi->view.scanning ? std::string("Scanning…") : std::string("Refresh");
    }, []() {
        const auto &view = settings_wifi->view;
        return view.status.service_running && !view.status.devices.empty() && view.status.wireless_enabled && !view.scanning;
    }, []() { wifi_command({"scan", "", ""}); });
    swifi_card_button(card, []() {
        return settings_wifi->view.status.wireless_enabled ? std::string("Turn off Wi-Fi") : std::string("Turn on Wi-Fi");
    }, []() {
        const auto &view = settings_wifi->view;
        return view.status.service_running && !view.status.devices.empty() && view.status.hardware_enabled;
    }, []() { wifi_command({"toggle", "", ""}); });

    // Captive portal banner, shown while NetworkManager reports Connectivity == PORTAL.
    auto portal = padded_right->child(FILL_SPACE, 0);
    portal->pre_layout = [](Container *root, Container *c, const Bounds &) {
        double dpi = swifi_window(root)->dpi;
        c->exists = settings_wifi->view.status.connectivity == 2;
        c->wanted_bounds.h = 54 * dpi;
        c->wanted_pad = Bounds(0, 8 * dpi, 0, 0);
    };
    auto sign_in = portal->child(FILL_SPACE, FILL_SPACE);
    sign_in->when_paint = [](Container *root, Container *c) {
        auto window = swifi_window(root);
        auto dpi = window->dpi;
        auto b = c->real_bounds;
        double tint = c->state.mouse_pressing ? .34 : c->state.mouse_hovering ? .26 : .18;
        set_argb(window->drawing_context, swifi_mix(theme.row_background, theme.accent, tint));
        drawRoundedRect(window->drawing_context, b.x, b.y, b.w, b.h, 5 * dpi, 1.0);
        window->drawing_context->fill();
        b.x += 16 * dpi;
        b.w -= 32 * dpi;
        swifi_text(root, b, "This network needs you to sign in · Open sign-in page", 11, theme.text);
    };
    sign_in->when_clicked = [](Container *, Container *) { wifi_command({"sign-in", "", ""}); };

    // Result of the last action (connected, wrong password, ...).
    auto feedback = padded_right->child(FILL_SPACE, 0);
    feedback->pre_layout = [](Container *root, Container *c, const Bounds &b) {
        auto window = swifi_window(root);
        auto dpi = window->dpi;
        const auto &view = settings_wifi->view;
        c->exists = !view.message.empty();
        auto font = get_cached_pango_font(window->drawing_context, set->font, 11 * dpi, PANGO_WEIGHT_NORMAL, false);
        pango_layout_set_text(font, view.message.c_str(), -1);
        pango_layout_set_width(font, std::max(1.0, b.w - 32 * dpi) * PANGO_SCALE);
        pango_layout_set_height(font, -1);
        pango_layout_set_wrap(font, PANGO_WRAP_WORD_CHAR);
        pango_layout_set_ellipsize(font, PANGO_ELLIPSIZE_NONE);
        int width, height;
        pango_layout_get_pixel_size(font, &width, &height);
        c->wanted_bounds.h = std::min(height + 24.0 * dpi, 100.0 * dpi) + 8 * dpi;
    };
    feedback->when_paint = [](Container *root, Container *c) {
        auto window = swifi_window(root);
        auto dpi = window->dpi;
        const auto &view = settings_wifi->view;
        RGBA hue = view.error ? RGBA(.86, .26, .22, 1) : RGBA(.20, .66, .36, 1);
        RGBA text = set->dark_theme ? swifi_mix(hue, RGBA(1, 1, 1, 1), .45) : swifi_mix(hue, RGBA(0, 0, 0, 1), .25);
        auto b = c->real_bounds;
        b.y += 8 * dpi;
        b.h -= 8 * dpi;
        set_argb(window->drawing_context, swifi_mix(theme.row_background, hue, .18));
        drawRoundedRect(window->drawing_context, b.x, b.y, b.w, b.h, 5 * dpi, 1.0);
        window->drawing_context->fill();
        b.x += 16 * dpi;
        b.w -= 32 * dpi;
        swifi_text(root, b, view.message, 11, text);
    };

    make_vert_space(padded_right, 20);
    make_section_title(padded_right, "Available networks");
    make_vert_space(padded_right, 10);

    auto list = padded_right->child(::vbox, FILL_SPACE, 0);
    auto page = new SettingsWifiPage;
    page->list = list;
    page->generation = ++settings_wifi_generation;
    list->user_data = page;
    list->pre_layout = [](Container *root, Container *c, const Bounds &) {
        double dpi = swifi_window(root)->dpi;
        c->spacing = 4 * dpi;
        double h = 0;
        for (auto row : c->children) {
            row->wanted_bounds.h = swifi_row_step((SettingsWifiRow *) row->user_data, dpi);
            h += row->wanted_bounds.h;
        }
        if (c->children.empty())
            h = 120 * dpi;
        else
            h += c->spacing * (c->children.size() - 1);
        c->wanted_bounds.h = h;
    };
    list->when_paint = [](Container *root, Container *c) {
        if (!c->children.empty())
            return;
        auto window = swifi_window(root);
        auto dpi = window->dpi;
        const auto &view = settings_wifi->view;
        std::string message = wifi_state_line(view);
        if (view.status.service_running && !view.status.devices.empty() && view.status.wireless_enabled)
            message = view.scanning || view.revision <= 1 ? "Searching for networks…" : "No networks found";
        auto b = c->real_bounds;
        drawRoundedRect(window->drawing_context, b.x, b.y, b.w, b.h, 5 * dpi, 1.0);
        set_argb(window->drawing_context, theme.row_background);
        window->drawing_context->fill();
        drawRoundedRect(window->drawing_context, b.x, b.y, b.w, b.h, 5 * dpi, 1.0);
        set_argb(window->drawing_context, theme.row_border);
        window->drawing_context->stroke();
        b.x += 30 * dpi;
        b.w -= 60 * dpi;
        swifi_text(root, b, message, 11, theme.text_secondary, true);
    };

    // The previous page (if any) was deleted above and released its hold on the worker.
    settings_wifi = page;
    // Show the last snapshot immediately; the worker refreshes it right away.
    if (auto previous = wifi_latest()) {
        swifi_apply(page, *previous);
        page->revision = previous->revision;
    }
    wifi_menu_opened();
    swifi_watch(page->generation);
}

/*************************************************
 *
 * Bluetooth page (same behaviour as the dock's Bluetooth menu, sharing its state)
 *
 *************************************************/

struct SettingsBluetoothPage : UserData {
    BluetoothMenuView view;
    unsigned long revision = 0;
    unsigned generation = 0;
    bool animating = false;
    std::string selected; // Object path of the open device row
    bool confirming_forget = false;
    std::string prompt_key; // Pairing request the prompt was built for
    std::string input;      // Mirror of the PIN field, kept current by its on_change
    Container *list = nullptr;
    Container *prompt = nullptr;
    ~SettingsBluetoothPage() override;
};

struct SettingsBluetoothRow : UserData {
    Device device{""};
    std::string action;
    bool hovered = false;
    double expansion = 0;
    double expansion_from = 0;
    double expansion_target = 0;
    long animation_start = 0;
    cairo_surface_t *icon = nullptr;
    std::string icon_name;
    double icon_dpi = 0;
    Container *buttons = nullptr;
    Container *forget = nullptr;
    ~SettingsBluetoothRow() override {
        if (icon)
            cairo_surface_destroy(icon);
    }
};

// The live page, if the Bluetooth page is showing. Only touched on the settings thread.
static SettingsBluetoothPage *settings_bluetooth = nullptr;
static unsigned settings_bluetooth_generation = 0;

static void settings_bluetooth_release() {
    if (!settings_bluetooth)
        return;
    settings_bluetooth = nullptr;
    bluetooth_menu_closed();
}

SettingsBluetoothPage::~SettingsBluetoothPage() {
    if (settings_bluetooth == this)
        settings_bluetooth_release();
}

static bool sbt_powered(const BluetoothMenuView &view) {
    return std::any_of(view.adapters.begin(), view.adapters.end(), [](const Adapter &a) { return a.powered; });
}

static RGBA sbt_row_color(SettingsBluetoothRow *data) {
    auto base = data->hovered ? theme.row_hover : theme.row_background;
    return swifi_mix(base, theme.accent, .14 * data->expansion);
}

static std::string sbt_primary_label(SettingsBluetoothRow *data) {
    if (data->action == "Pair")
        return "Cancel";
    if (!data->action.empty() && data->action != "Unpair")
        return bluetooth_action_label(data->action);
    if (!data->device.paired)
        return "Pair";
    return data->device.connected ? "Disconnect" : "Connect";
}

static bool sbt_primary_enabled(SettingsBluetoothRow *data) {
    const auto &view = settings_bluetooth->view;
    if (data->action == "Pair")
        return true;
    return !view.busy && (data->device.paired || view.agent_ready);
}

// Advances the row's open/close animation and returns its height.
static double sbt_row_step(SettingsBluetoothRow *data, double dpi) {
    auto page = settings_bluetooth;
    const auto now = get_current_time_in_ms();
    double elapsed = std::clamp((now - data->animation_start) / 180.0, 0.0, 1.0);
    data->expansion = data->expansion_from + (data->expansion_target - data->expansion_from) *
        (1 - std::pow(1 - elapsed, 3));
    // Rows with an operation in flight stay open so its progress shows.
    double target = page->selected == data->device.object_path || !data->action.empty() ? 1 : 0;
    if (target != data->expansion_target) {
        data->expansion_from = data->expansion;
        data->expansion_target = target;
        data->animation_start = now;
    }
    if (std::abs(data->expansion - data->expansion_target) > .001)
        page->animating = true;
    data->buttons->exists = data->expansion > .001;
    data->forget->exists = data->device.paired;
    return (62 + 42 * data->expansion) * dpi;
}

static Container *sbt_device_row(Container *parent) {
    auto row = parent->child(::vbox, FILL_SPACE, 62);
    auto data = new SettingsBluetoothRow;
    row->user_data = data;
    row->receive_events_even_if_obstructed = true;
    row->when_mouse_enters_container = [data](Container *root, Container *) {
        data->hovered = true;
        swifi_redraw(root);
    };
    row->when_mouse_leaves_container = [data](Container *root, Container *) {
        data->hovered = false;
        swifi_redraw(root);
    };
    row->when_paint = [data](Container *root, Container *c) {
        auto window = swifi_window(root);
        auto b = c->real_bounds;
        window->drawing_context->save();
        drawRoundedRect(window->drawing_context, b.x, b.y, b.w, b.h, 5 * window->dpi, 1.0);
        window->drawing_context->clip_path();
        set_argb(window->drawing_context, sbt_row_color(data));
        window->drawing_context->paint_source();
    };
    row->after_paint = [](Container *root, Container *c) {
        auto window = swifi_window(root);
        auto b = c->real_bounds;
        window->drawing_context->restore();
        drawRoundedRect(window->drawing_context, b.x, b.y, b.w, b.h, 5 * window->dpi, 1.0);
        set_argb(window->drawing_context, theme.row_border);
        window->drawing_context->stroke();
    };

    auto header = row->child(FILL_SPACE, 62);
    header->pre_layout = [](Container *root, Container *c, const Bounds &) {
        c->wanted_bounds.h = 62 * swifi_window(root)->dpi;
    };
    header->when_paint = [data](Container *root, Container *c) {
        auto window = swifi_window(root);
        auto cr = window->drawing_context;
        auto dpi = window->dpi;
        auto b = c->real_bounds;
        int size = (int) std::round(24 * dpi);
        if (icons_loaded && (data->icon_name != data->device.icon || data->icon_dpi != dpi)) {
            data->icon_name = data->device.icon;
            data->icon_dpi = dpi;
            if (data->icon) {
                cairo_surface_destroy(data->icon);
                data->icon = nullptr;
            }
            auto path = one_shot_icon(size, {data->device.icon, "bluetooth"});
            if (!path.empty())
                load_icon_full_path(&data->icon, path, size);
        }
        if (data->icon) {
            cr->set_source_surface(data->icon, std::round(b.x + 17 * dpi), std::round(b.y + (b.h - size) * .5));
            cr->paint_source();
        } else {
            swifi_glyph(cr, b.x + 19 * dpi, b.y + b.h * .5 - 10 * dpi, "", 16 * dpi, theme.text);
        }
        std::string subtitle;
        if (!data->device.paired) {
            subtitle = data->action == "Pair" ? "Pairing…" : "Not paired";
        } else {
            subtitle = data->device.connected ? "Connected" : "Paired";
            if (data->device.connected && !data->device.percentage.empty())
                subtitle += " · Battery " + data->device.percentage + "%";
        }
        if ((data->hovered || settings_bluetooth->selected == data->device.object_path) && !data->device.mac_address.empty())
            subtitle += " · " + data->device.mac_address;
        swifi_text(root, Bounds(b.x + 60 * dpi, b.y + 9 * dpi, b.w - 76 * dpi, 22 * dpi), bluetooth_display_name(data->device), 12,
                   theme.text);
        swifi_text(root, Bounds(b.x + 60 * dpi, b.y + 31 * dpi, b.w - 76 * dpi, 22 * dpi), subtitle, 10, theme.text_secondary);
    };
    header->when_clicked = [data](Container *root, Container *) {
        auto page = settings_bluetooth;
        page->selected = page->selected == data->device.object_path ? "" : data->device.object_path;
        page->confirming_forget = false;
        swifi_redraw(root);
    };

    data->buttons = row->child(::hbox, FILL_SPACE, 0);
    data->buttons->pre_layout = [data](Container *root, Container *c, const Bounds &) {
        double dpi = swifi_window(root)->dpi;
        c->wanted_bounds.h = 42 * data->expansion * dpi;
        c->wanted_pad = Bounds(60 * dpi, 0, 16 * dpi, 8 * dpi);
        c->spacing = 8 * dpi;
    };
    auto primary = data->buttons->child(FILL_SPACE, FILL_SPACE);
    primary->when_paint = [data](Container *root, Container *c) {
        swifi_paint_button(root, c, sbt_primary_label(data), sbt_primary_enabled(data));
    };
    primary->when_clicked = [data](Container *root, Container *) {
        if (!sbt_primary_enabled(data))
            return;
        std::string command = data->action == "Pair" ? "Cancel Pair" :
            !data->device.paired ? "Pair" : data->device.connected ? "Disconnect" : "Connect";
        settings_bluetooth->confirming_forget = false;
        bluetooth_command(data->device.object_path, command);
        swifi_redraw(root);
    };
    data->forget = data->buttons->child(FILL_SPACE, FILL_SPACE);
    data->forget->when_paint = [data](Container *root, Container *c) {
        auto page = settings_bluetooth;
        std::string label = data->action == "Unpair" ? "Forgetting…" : page->confirming_forget ? "Are you sure?" : "Forget";
        swifi_paint_button(root, c, label, data->device.paired && !page->view.busy);
    };
    data->forget->when_clicked = [data](Container *root, Container *) {
        auto page = settings_bluetooth;
        if (!data->device.paired || page->view.busy)
            return;
        if (!page->confirming_forget) {
            page->confirming_forget = true;
        } else {
            page->confirming_forget = false;
            bluetooth_command(data->device.object_path, "Unpair");
        }
        swifi_redraw(root);
    };
    return row;
}

// Line of text inside the pairing prompt card.
static void sbt_prompt_line(Container *parent, std::string text, double size, bool secondary, bool centered = false,
                            bool bold = false) {
    auto line = parent->child(FILL_SPACE, FILL_SPACE);
    line->pre_layout = [text, size, bold](Container *root, Container *c, const Bounds &) {
        c->wanted_bounds.h = swifi_text_height(root, text.empty() ? " " : text, size, bold) + 6 * swifi_window(root)->dpi;
    };
    line->when_paint = [text, size, secondary, centered, bold](Container *root, Container *c) {
        swifi_text(root, c->real_bounds, text, size, secondary ? theme.text_secondary : theme.text, centered, bold);
    };
}

// Rebuilds the pairing prompt when BlueZ's agent asks something new.
static void sbt_build_prompt(SettingsBluetoothPage *page) {
    const auto &view = page->view;
    auto prompt = page->prompt;
    auto key = std::to_string(view.request_id) + view.request_type;
    if (page->prompt_key == key)
        return;
    page->prompt_key = key;
    page->input.clear();
    for (auto child : prompt->children)
        delete child;
    prompt->children.clear();
    if (view.request_type.empty())
        return;
    const bool input = view.request_type == "RequestPinCode" || view.request_type == "RequestPasskey";
    const bool display = view.request_type == "DisplayPinCode" || view.request_type == "DisplayPasskey";
    std::string question = input ? (view.request_type == "RequestPinCode" ? "Enter the PIN for this device" : "Enter the passkey for this device") :
        display ? "Type this code on the device, then press Enter on it" : view.request_type == "RequestConfirmation" ?
        "Does this passkey match the one shown on the device?" : view.request_type == "AuthorizeService" ?
        "Allow this device to connect?" : "Allow this device to pair?";
    sbt_prompt_line(prompt, view.request_device.empty() ? "Bluetooth device" : view.request_device, 12, false, false, true);
    sbt_prompt_line(prompt, question, 11, true);
    if (!view.pin.empty())
        sbt_prompt_line(prompt, view.pin, 20, false, true, true);
    auto id = view.request_id;
    Container *field = nullptr;
    if (input) {
        auto holder = prompt->child(::hbox, FILL_SPACE, FILL_SPACE);
        holder->pre_layout = [](Container *root, Container *c, const Bounds &) {
            double dpi = swifi_window(root)->dpi;
            c->wanted_bounds.h = 44 * dpi;
            c->wanted_pad = Bounds(0, 6 * dpi, 0, 4 * dpi);
        };
        field = holder->child(FILL_SPACE, FILL_SPACE);
        STextEditorOptions options;
        options.multiline = false;
        options.only_numbers = view.request_type == "RequestPasskey";
        options.max_length = view.request_type == "RequestPasskey" ? 6 : 16;
        options.font_size = 11;
        options.window = [](Container *root) { return swifi_window(root); };
        options.viewport = [](Container *root, Container *c) {
            auto dpi = swifi_window(root)->dpi;
            auto bounds = c->real_bounds;
            bounds.x += 10 * dpi;
            bounds.w = std::max(0.0, bounds.w - 20 * dpi);
            return bounds;
        };
        options.on_change = [](std::string text) {
            if (settings_bluetooth)
                settings_bluetooth->input = std::move(text);
        };
        setup_text_editor(field, "", std::move(options));
        focus_text(field);
        field->when_paint = [](Container *root, Container *c) {
            auto window = swifi_window(root);
            auto dpi = window->dpi;
            auto b = c->real_bounds;
            paint_smooth_frame(window->drawing_context, b, settings_control_rounding * dpi, theme.field_background,
                               c->active ? theme.accent : theme.field_border, std::max(1.0, (double) std::round(1 * dpi)));
            if (static_cast<STextEditorData *>(c->user_data)->text.empty()) {
                auto hint = b;
                hint.x += 10 * dpi;
                hint.w -= 20 * dpi;
                swifi_text(root, hint, "Type here", 11, swifi_dim(theme.text_secondary));
            }
            paint_text_editor(root, c, theme.text, theme.accent);
        };
        auto edit_key = std::move(field->when_key_event);
        field->when_key_event = [edit_key, id](Container *root, Container *c, int key, bool pressed, xkb_keysym_t sym, int mods,
                                               bool is_text, std::string text) {
            if (!c->active)
                return;
            if (pressed && (sym == XKB_KEY_Return || sym == XKB_KEY_KP_Enter)) {
                if (settings_bluetooth)
                    bluetooth_submit(id, true, settings_bluetooth->input);
                return;
            }
            edit_key(root, c, key, pressed, sym, mods, is_text, text);
            swifi_redraw(root);
        };
    }
    if (!display) {
        auto buttons = prompt->child(::hbox, FILL_SPACE, FILL_SPACE);
        buttons->pre_layout = [](Container *root, Container *c, const Bounds &) {
            double dpi = swifi_window(root)->dpi;
            c->wanted_bounds.h = 44 * dpi;
            c->wanted_pad = Bounds(0, 8 * dpi, 0, 2 * dpi);
            c->spacing = 8 * dpi;
        };
        std::string accept_label = input ? "Submit" : view.request_type == "RequestConfirmation" ? "Matches" : "Allow";
        auto accept = buttons->child(FILL_SPACE, FILL_SPACE);
        accept->when_paint = [accept_label](Container *root, Container *c) { swifi_paint_button(root, c, accept_label, true); };
        accept->when_clicked = [id](Container *, Container *) {
            if (settings_bluetooth)
                bluetooth_submit(id, true, settings_bluetooth->input);
        };
        std::string reject_label = input ? "Cancel" : "Reject";
        auto reject = buttons->child(FILL_SPACE, FILL_SPACE);
        reject->when_paint = [reject_label](Container *root, Container *c) { swifi_paint_button(root, c, reject_label, true); };
        reject->when_clicked = [id](Container *, Container *) { bluetooth_submit(id, false, ""); };
    }
    if (field && settings_mylar)
        set_active(settings_mylar->root, {field}, settings_mylar->root, true, false);
}

static void sbt_apply(SettingsBluetoothPage *page, const BluetoothMenuView &view) {
    bool mode_changed = page->view.showing_paired != view.showing_paired;
    page->view = view;
    auto content = page->list;
    std::vector<const Device *> wanted;
    for (const auto &device : view.devices) {
        bool powered = std::any_of(view.adapters.begin(), view.adapters.end(), [&device](const Adapter &adapter) {
            return adapter.object_path == device.adapter && adapter.powered;
        });
        if (powered && (device.paired == view.showing_paired || view.pairing_path == device.object_path))
            wanted.push_back(&device);
    }
    std::unordered_set<std::string> paths;
    for (auto device : wanted)
        paths.insert(device->object_path);
    if (mode_changed || !paths.contains(page->selected)) {
        page->selected.clear();
        page->confirming_forget = false;
    }
    std::unordered_map<std::string, Container *> rows;
    std::erase_if(content->children, [&paths, &rows](Container *row) {
        if (paths.contains(row->name)) {
            rows.emplace(row->name, row);
            return false;
        }
        delete row;
        return true;
    });
    // Rows survive property updates (keyed by object path) so selection and animations persist.
    std::vector<Container *> ordered;
    for (auto device : wanted) {
        Container *row = nullptr;
        if (auto found = rows.find(device->object_path); found != rows.end()) {
            row = found->second;
        } else {
            row = sbt_device_row(content);
            content->children.pop_back(); // Re-inserted below in sorted order.
            row->name = device->object_path;
        }
        auto data = (SettingsBluetoothRow *) row->user_data;
        data->device = *device;
        auto action = view.actions.find(device->object_path);
        data->action = action == view.actions.end() ? "" : action->second;
        ordered.push_back(row);
    }
    content->children = std::move(ordered);
    sbt_build_prompt(page);
}

static void sbt_watch(unsigned generation) {
    windowing::timer(settings_app, 16, [generation](void *) {
        if (!settings_app || !settings_mylar)
            return;
        bool redraw = false;
        {
            std::lock_guard<std::mutex> lock(settings_app->mutex);
            auto page = settings_bluetooth;
            if (!page || page->generation != generation)
                return; // Page left or rebuilt; stop polling.
            auto view = bluetooth_latest();
            if (view && view->revision != page->revision) {
                sbt_apply(page, *view);
                page->revision = view->revision;
                redraw = true;
            }
            redraw |= page->animating || page->view.scanning || page->view.busy;
            page->animating = false;
        }
        if (redraw)
            windowing::redraw(settings_mylar->raw_window);
        sbt_watch(generation);
    }, nullptr);
}

static std::string sbt_status_line(const BluetoothMenuView &view) {
    if (!view.running)
        return "Bluetooth service is unavailable";
    if (view.adapters.empty())
        return "No Bluetooth adapter found";
    for (const auto &[path, action] : view.actions) {
        if (action == "Power On") return "Turning on…";
        if (action == "Power Off") return "Turning off…";
    }
    if (!sbt_powered(view))
        return "Off";
    if (view.scanning && !view.scan_stopping)
        return "On · Searching for devices…";
    int connected = 0;
    for (const auto &device : view.devices)
        connected += device.paired && device.connected;
    std::string line = "On";
    for (const auto &adapter : view.adapters) {
        if (!adapter.powered)
            continue;
        auto name = bluetooth_display_name(adapter);
        if (!name.empty())
            line += " · Visible as “" + name + "”";
        break;
    }
    if (connected)
        line += " · " + std::to_string(connected) + " connected";
    return line;
}

static void fill_bluetooth_settings(Container *root) {
    auto right = container_by_name("settings_right", root);
    if (!right)
        return;
    for (auto child: right->children)
        delete child;
    right->children.clear();

    right->pre_layout = layout_right_page;
    auto padded_right = right->child(FILL_SPACE, FILL_SPACE);

    make_section_title(padded_right, "Bluetooth");
    make_vert_space(padded_right, 10);

    // Status card: state, add device and on/off.
    auto card = padded_right->child(::hbox, FILL_SPACE, 70);
    search_tag(card, "Add device", "Search for nearby devices to pair");
    card->pre_layout = [](Container *root, Container *c, const Bounds &) {
        double dpi = swifi_window(root)->dpi;
        c->wanted_bounds.h = 70 * dpi;
        c->wanted_pad = Bounds(0, 17 * dpi, 16 * dpi, 17 * dpi);
        c->spacing = 8 * dpi;
    };
    card->when_paint = [](Container *root, Container *c) {
        auto window = swifi_window(root);
        auto cr = window->drawing_context;
        auto dpi = window->dpi;
        auto b = c->real_bounds;
        drawRoundedRect(cr, b.x, b.y, b.w, b.h, 5 * dpi, 1.0);
        set_argb(cr, theme.row_background);
        cr->fill();
        const auto &view = settings_bluetooth->view;
        if (view.scanning || view.busy)
            paint_card_progress_dots(cr, b, dpi);
        // Border last so nothing drawn inside the card can sit on top of it.
        drawRoundedRect(cr, b.x, b.y, b.w, b.h, 5 * dpi, 1.0);
        set_argb(cr, theme.row_border);
        cr->stroke();
    };
    auto heading = card->child(FILL_SPACE, FILL_SPACE);
    heading->when_paint = [](Container *root, Container *c) {
        auto window = swifi_window(root);
        auto dpi = window->dpi;
        const auto &view = settings_bluetooth->view;
        auto b = c->real_bounds;
        bool on = view.running && sbt_powered(view);
        swifi_glyph(window->drawing_context, b.x + 19 * dpi, b.y + b.h * .5 - 10 * dpi, "", 16 * dpi,
            on ? theme.text : swifi_dim(theme.text_secondary));
        swifi_text(root, Bounds(b.x + 60 * dpi, b.y - 6 * dpi, b.w - 60 * dpi, 24 * dpi), "Bluetooth", 12, theme.text);
        swifi_text(root, Bounds(b.x + 60 * dpi, b.y + 16 * dpi, b.w - 60 * dpi, 22 * dpi), sbt_status_line(view), 10,
            theme.text_secondary);
    };
    swifi_card_button(card, []() {
        const auto &view = settings_bluetooth->view;
        if (view.scan_pending)
            return std::string(view.scan_stopping ? "Cancelling…" : "Starting…");
        return std::string(view.showing_paired ? "Add device" : "Stop searching");
    }, []() {
        const auto &view = settings_bluetooth->view;
        return view.running && sbt_powered(view) && !view.busy && !view.scan_pending;
    }, []() { bluetooth_command("", "Scan"); });
    swifi_card_button(card, []() {
        const auto &view = settings_bluetooth->view;
        for (const auto &[path, action] : view.actions) {
            if (action == "Power On") return std::string("Turning on…");
            if (action == "Power Off") return std::string("Turning off…");
        }
        return std::string(sbt_powered(view) ? "Turn off Bluetooth" : "Turn on Bluetooth");
    }, []() {
        const auto &view = settings_bluetooth->view;
        return view.running && !view.adapters.empty() && !view.busy && !view.scanning;
    }, []() {
        bluetooth_command("", sbt_powered(settings_bluetooth->view) ? "Power Off" : "Power On");
    });

    // Questions from the pairing agent (PIN, passkey, confirmation).
    auto prompt_space = padded_right->child(::vbox, FILL_SPACE, 0);
    prompt_space->pre_layout = [](Container *root, Container *c, const Bounds &b) {
        double dpi = swifi_window(root)->dpi;
        auto prompt = c->children[0];
        c->exists = !prompt->children.empty();
        c->wanted_pad = Bounds(0, 8 * dpi, 0, 0);
        double h = 0;
        for (auto child : prompt->children) {
            if (child->pre_layout)
                child->pre_layout(root, child, b);
            h += child->wanted_bounds.h;
        }
        prompt->wanted_pad = Bounds(60 * dpi, 12 * dpi, 16 * dpi, 12 * dpi);
        prompt->wanted_bounds.h = h + 24 * dpi;
        c->wanted_bounds.h = prompt->wanted_bounds.h + 8 * dpi;
    };
    auto prompt = prompt_space->child(::vbox, FILL_SPACE, 0);
    prompt->when_paint = [](Container *root, Container *c) {
        auto window = swifi_window(root);
        auto cr = window->drawing_context;
        auto dpi = window->dpi;
        auto b = c->real_bounds;
        drawRoundedRect(cr, b.x, b.y, b.w, b.h, 5 * dpi, 1.0);
        set_argb(cr, swifi_mix(theme.row_background, theme.accent, .14));
        cr->fill();
        drawRoundedRect(cr, b.x, b.y, b.w, b.h, 5 * dpi, 1.0);
        set_argb(cr, theme.row_border);
        cr->stroke();
        swifi_glyph(cr, b.x + 19 * dpi, b.y + 14 * dpi, "", 16 * dpi, theme.accent);
    };

    // Result of the last action (paired, connect failed, ...).
    auto feedback = padded_right->child(FILL_SPACE, 0);
    feedback->pre_layout = [](Container *root, Container *c, const Bounds &b) {
        auto window = swifi_window(root);
        auto dpi = window->dpi;
        const auto &view = settings_bluetooth->view;
        c->exists = !view.message.empty();
        auto font = get_cached_pango_font(window->drawing_context, set->font, 11 * dpi, PANGO_WEIGHT_NORMAL, false);
        pango_layout_set_text(font, view.message.c_str(), -1);
        pango_layout_set_width(font, std::max(1.0, b.w - 32 * dpi) * PANGO_SCALE);
        pango_layout_set_height(font, -1);
        pango_layout_set_wrap(font, PANGO_WRAP_WORD_CHAR);
        pango_layout_set_ellipsize(font, PANGO_ELLIPSIZE_NONE);
        int width, height;
        pango_layout_get_pixel_size(font, &width, &height);
        c->wanted_bounds.h = std::min(height + 24.0 * dpi, 100.0 * dpi) + 8 * dpi;
    };
    feedback->when_paint = [](Container *root, Container *c) {
        auto window = swifi_window(root);
        auto dpi = window->dpi;
        const auto &view = settings_bluetooth->view;
        RGBA hue = view.error ? RGBA(.86, .26, .22, 1) : RGBA(.20, .66, .36, 1);
        RGBA text = set->dark_theme ? swifi_mix(hue, RGBA(1, 1, 1, 1), .45) : swifi_mix(hue, RGBA(0, 0, 0, 1), .25);
        auto b = c->real_bounds;
        b.y += 8 * dpi;
        b.h -= 8 * dpi;
        set_argb(window->drawing_context, swifi_mix(theme.row_background, hue, .18));
        drawRoundedRect(window->drawing_context, b.x, b.y, b.w, b.h, 5 * dpi, 1.0);
        window->drawing_context->fill();
        b.x += 16 * dpi;
        b.w -= 32 * dpi;
        swifi_text(root, b, view.message, 11, text);
    };

    make_vert_space(padded_right, 20);
    // The title follows the list: paired devices, or nearby ones while adding a device.
    search_current_section = "Devices";
    auto list_title = padded_right->child(FILL_SPACE, FILL_SPACE);
    search_tag(list_title, "Devices", "");
    list_title->pre_layout = [](Container *root, Container *c, const Bounds &b) {
        c->wanted_bounds.h = swifi_text_height(root, "Devices", 13);
    };
    list_title->when_paint = [](Container *root, Container *c) {
        auto title = settings_bluetooth->view.showing_paired ? "Paired devices" : "Nearby devices";
        swifi_text(root, c->real_bounds, title, 13, theme.text);
    };
    make_vert_space(padded_right, 10);

    auto list = padded_right->child(::vbox, FILL_SPACE, 0);
    auto page = new SettingsBluetoothPage;
    page->list = list;
    page->prompt = prompt;
    page->generation = ++settings_bluetooth_generation;
    list->user_data = page;
    list->pre_layout = [](Container *root, Container *c, const Bounds &) {
        double dpi = swifi_window(root)->dpi;
        c->spacing = 4 * dpi;
        double h = 0;
        for (auto row : c->children) {
            row->wanted_bounds.h = sbt_row_step((SettingsBluetoothRow *) row->user_data, dpi);
            h += row->wanted_bounds.h;
        }
        if (c->children.empty())
            h = 120 * dpi;
        else
            h += c->spacing * (c->children.size() - 1);
        c->wanted_bounds.h = h;
    };
    list->when_paint = [](Container *root, Container *c) {
        if (!c->children.empty())
            return;
        auto window = swifi_window(root);
        auto dpi = window->dpi;
        const auto &view = settings_bluetooth->view;
        std::string message = !view.running ? "Bluetooth service is unavailable" : view.adapters.empty() ?
            "No Bluetooth adapter found" : !sbt_powered(view) ? "Turn on Bluetooth to see your devices" :
            view.showing_paired ? "No paired devices · Use Add device to pair one" : "Searching for devices…";
        auto b = c->real_bounds;
        drawRoundedRect(window->drawing_context, b.x, b.y, b.w, b.h, 5 * dpi, 1.0);
        set_argb(window->drawing_context, theme.row_background);
        window->drawing_context->fill();
        drawRoundedRect(window->drawing_context, b.x, b.y, b.w, b.h, 5 * dpi, 1.0);
        set_argb(window->drawing_context, theme.row_border);
        window->drawing_context->stroke();
        b.x += 30 * dpi;
        b.w -= 60 * dpi;
        swifi_text(root, b, message, 11, theme.text_secondary, true);
    };
    make_vert_space(padded_right, 16);

    // The previous page (if any) was deleted above and released its hold on the shared state.
    settings_bluetooth = page;
    // Show the last snapshot immediately; opening asks the compositor for a fresh one.
    if (auto previous = bluetooth_latest()) {
        sbt_apply(page, *previous);
        page->revision = previous->revision;
    }
    bluetooth_menu_opened();
    sbt_watch(page->generation);
}

/*************************************************
 *
 * Audio page (pavucontrol-like: Playback, Recording, Output Devices, Input Devices, Configuration)
 *
 *************************************************/

static const char *saudio_tab_names[] = {"Playback", "Recording", "Output Devices", "Input Devices", "Configuration"};
static int settings_audio_tab = 0;

struct SettingsAudioRow : UserData {
    bool is_card = false;
    AudioNodeInfo node;
    AudioCardInfo card;
    float shown_peak = 0;
    MeterHighWater high_water;
    float high_water_shown = 0; // meter level scale, 0..1
    bool dragging = false;
    double drag_value = 0;
    double last_sent = -1;
    long last_local_change = 0;
    bool hovered = false;
    std::string loaded_icon;
    float loaded_dpi = 0;
    cairo_surface_t *icon = nullptr;

    ~SettingsAudioRow() override {
        if (icon)
            cairo_surface_destroy(icon);
    }
};

struct SettingsAudioPage : UserData {
    unsigned generation = 0;
    unsigned long revision = 0;
    std::string backend;
    std::vector<AudioNodeInfo> nodes;
    std::vector<AudioCardInfo> cards;
    std::string default_sink;
    std::string default_source;
    bool show_monitors = false;
    Container *list = nullptr;
    ~SettingsAudioPage() override;
};

// The live page, if the Audio tab is showing. Only touched on the settings thread.
static SettingsAudioPage *settings_audio = nullptr;
static unsigned settings_audio_generation = 0;

static void settings_audio_release() {
    if (!settings_audio)
        return;
    settings_audio = nullptr;
    audio_mixer_meters(false);
}

SettingsAudioPage::~SettingsAudioPage() {
    if (settings_audio == this)
        settings_audio_release();
}

static bool saudio_is_stream(const AudioNodeInfo &n) {
    return n.kind == AudioNodeKind::SinkInput || n.kind == AudioNodeKind::SourceOutput;
}

static const AudioNodeInfo *saudio_node(AudioNodeKind kind, uint32_t index) {
    if (!settings_audio || index == UINT32_MAX)
        return nullptr;
    for (auto &n : settings_audio->nodes)
        if (n.kind == kind && n.index == index)
            return &n;
    return nullptr;
}

static bool saudio_is_default(const AudioNodeInfo &n) {
    if (n.name.empty())
        return false;
    if (n.kind == AudioNodeKind::Sink)
        return n.name == settings_audio->default_sink;
    if (n.kind == AudioNodeKind::Source)
        return n.name == settings_audio->default_source;
    return false;
}

static std::string saudio_device_label(const AudioNodeInfo &n) {
    auto kind = n.kind == AudioNodeKind::SinkInput ? AudioNodeKind::Sink : AudioNodeKind::Source;
    if (auto device = saudio_node(kind, n.device))
        return device->description;
    if (n.kind == AudioNodeKind::SourceOutput)
        if (auto sink = saudio_node(AudioNodeKind::Sink, n.device))
            return "Monitor of " + sink->description;
    return n.device == UINT32_MAX ? "Not connected" : "Unknown device";
}

static std::string saudio_port_label(const AudioNodeInfo &n) {
    for (auto &port : n.ports)
        if (port.name == n.active_port)
            return port.description;
    return n.active_port.empty() ? "No port" : n.active_port;
}

static std::string saudio_profile_label(const AudioCardInfo &c) {
    for (auto &profile : c.profiles)
        if (profile.name == c.active_profile)
            return profile.description;
    return c.active_profile.empty() ? "Off" : c.active_profile;
}

static double saudio_shown_volume(SettingsAudioRow *data) {
    if (data->dragging || get_current_time_in_ms() - data->last_local_change < 600)
        return data->drag_value;
    return data->node.volume();
}

// Peaks are linear amplitude; show them on a 60 dB scale like a VU meter so speech is visible.
static float saudio_meter_level(float peak) {
    if (peak <= 0.0001f)
        return 0;
    return std::clamp(1.0f + 20.0f * std::log10(peak) / 60.0f, 0.0f, 1.0f);
}

static std::string saudio_row_key(const AudioNodeInfo &n) {
    return std::to_string((int) n.kind) + ":" + std::to_string(n.index);
}

// --- Drop-down menus (output device of a stream, port of a device, profile of a card) ---

enum class SAudioMenu { Port, Device, Profile };

static bool saudio_menu_visible(SettingsAudioRow *data, SAudioMenu menu) {
    switch (menu) {
        case SAudioMenu::Port: return !data->is_card && !saudio_is_stream(data->node) && !data->node.ports.empty();
        case SAudioMenu::Device: return !data->is_card && saudio_is_stream(data->node);
        case SAudioMenu::Profile: return data->is_card && !data->card.profiles.empty();
    }
    return false;
}

static std::string saudio_menu_label(SettingsAudioRow *data, SAudioMenu menu) {
    switch (menu) {
        case SAudioMenu::Port: return saudio_port_label(data->node);
        case SAudioMenu::Device: return saudio_device_label(data->node);
        case SAudioMenu::Profile: return saudio_profile_label(data->card);
    }
    return "";
}

static std::vector<DropdownOption> saudio_menu_options(SettingsAudioRow *data, SAudioMenu menu, std::string *selected) {
    std::vector<DropdownOption> options;
    if (menu == SAudioMenu::Port) {
        *selected = data->node.active_port;
        for (auto &port : data->node.ports)
            options.push_back({port.name, port.description + (port.available ? "" : " (unplugged)")});
    } else if (menu == SAudioMenu::Device) {
        *selected = std::to_string(data->node.device);
        auto kind = data->node.kind == AudioNodeKind::SinkInput ? AudioNodeKind::Sink : AudioNodeKind::Source;
        for (auto &n : settings_audio->nodes)
            if (n.kind == kind)
                options.push_back({std::to_string(n.index), n.description});
    } else {
        *selected = data->card.active_profile;
        for (auto &profile : data->card.profiles)
            options.push_back({profile.name, profile.description + (profile.available ? "" : " (unavailable)")});
    }
    return options;
}

static void saudio_menu_pick(SettingsAudioRow *data, SAudioMenu menu, const std::string &id) {
    if (menu == SAudioMenu::Port) {
        audio_mixer_set_port(data->node.kind, data->node.index, id);
    } else if (menu == SAudioMenu::Device) {
        audio_mixer_move_stream(data->node.kind, data->node.index, (uint32_t) std::stoul(id));
    } else {
        audio_mixer_set_card_profile(data->card.index, id);
    }
}

static Container *saudio_dropdown(Container *parent, SettingsAudioRow *data, SAudioMenu menu) {
    auto dd = parent->child(0, FILL_SPACE);
    dd->pre_layout = [data, menu](Container *root, Container *c, const Bounds &) {
        c->exists = saudio_menu_visible(data, menu);
        if (c->exists)
            c->wanted_bounds.w = dropdown_button_width(root, saudio_menu_label(data, menu));
    };
    dd->when_paint = [data, menu](Container *root, Container *c) {
        paint_dropdown_button(root, c, saudio_menu_label(data, menu));
    };
    dd->when_clicked = [data, menu](Container *root, Container *c) {
        std::string selected;
        auto options = saudio_menu_options(data, menu, &selected);
        open_dropdown_menu(root, c, options, selected, [data, menu](std::string id) {
            if (settings_audio)
                saudio_menu_pick(data, menu, id);
        });
    };
    return dd;
}

// --- Rows ---

static void saudio_set_volume_from_mouse(Container *root, Container *c, SettingsAudioRow *data, bool finished) {
    if (!data->node.has_volume)
        return;
    auto dpi = swifi_window(root)->dpi;
    double r = 8 * dpi;
    auto b = c->real_bounds;
    double scalar = std::clamp((root->mouse_current_x - (b.x + r)) / std::max(1.0, b.w - 2 * r), 0.0, 1.0);
    data->drag_value = scalar;
    data->dragging = !finished;
    data->last_local_change = get_current_time_in_ms();
    if (finished || std::abs(scalar - data->last_sent) > 0.004) {
        data->last_sent = scalar;
        audio_mixer_set_volume(data->node.kind, data->node.index, scalar);
    }
    swifi_redraw(root);
}

static Container *saudio_row(Container *parent) {
    auto row = parent->child(::vbox, FILL_SPACE, 92);
    auto data = new SettingsAudioRow;
    row->user_data = data;
    row->receive_events_even_if_obstructed = true;
    row->when_mouse_enters_container = [data](Container *root, Container *) {
        data->hovered = true;
        swifi_redraw(root);
    };
    row->when_mouse_leaves_container = [data](Container *root, Container *) {
        data->hovered = false;
        swifi_redraw(root);
    };
    row->pre_layout = [](Container *root, Container *c, const Bounds &) {
        auto dpi = swifi_window(root)->dpi;
        c->wanted_pad = Bounds(0, 8 * dpi, 16 * dpi, 8 * dpi);
    };
    row->when_paint = [data](Container *root, Container *c) {
        auto window = swifi_window(root);
        auto b = c->real_bounds;
        drawRoundedRect(window->drawing_context, b.x, b.y, b.w, b.h, 5 * window->dpi, 1.0);
        set_argb(window->drawing_context, data->hovered ? theme.row_hover : theme.row_background);
        window->drawing_context->fill();
        drawRoundedRect(window->drawing_context, b.x, b.y, b.w, b.h, 5 * window->dpi, 1.0);
        set_argb(window->drawing_context, theme.row_border);
        window->drawing_context->stroke();
    };

    auto header = row->child(::hbox, FILL_SPACE, 46);
    header->pre_layout = [data](Container *root, Container *c, const Bounds &) {
        auto dpi = swifi_window(root)->dpi;
        c->wanted_bounds.h = (data->is_card ? 54 : 46) * dpi;
        c->spacing = 8 * dpi;
    };

    auto icon = header->child(56, FILL_SPACE);
    icon->pre_layout = [](Container *root, Container *c, const Bounds &) {
        c->wanted_bounds.w = 56 * swifi_window(root)->dpi;
    };
    icon->when_paint = [data](Container *root, Container *c) {
        auto window = swifi_window(root);
        auto cr = window->drawing_context;
        auto dpi = window->dpi;
        std::string wanted = data->is_card ? data->card.icon_name : data->node.icon_name;
        if (data->loaded_icon != wanted || std::abs(data->loaded_dpi - dpi) > 0.001) {
            data->loaded_icon = wanted;
            data->loaded_dpi = dpi;
            if (data->icon) {
                cairo_surface_destroy(data->icon);
                data->icon = nullptr;
            }
            std::string fallback = data->is_card || data->node.kind == AudioNodeKind::Sink ? "audio-card" :
                data->node.kind == AudioNodeKind::Source ? "audio-input-microphone" :
                data->node.kind == AudioNodeKind::SinkInput ? "applications-multimedia" : "audio-input-microphone";
            auto full = one_shot_icon(32 * dpi, {wanted, to_lower(wanted), c3ic_fix_wm_class(wanted), fallback,
                                                 "audio-speakers", "multimedia-volume-control"});
            if (!full.empty())
                load_icon_full_path(&data->icon, full, 32 * dpi);
        }
        if (data->icon) {
            auto h = cairo_image_surface_get_height(data->icon);
            cr->set_source_surface(data->icon, std::round(c->real_bounds.x + 16 * dpi),
                                     std::round(c->real_bounds.y + c->real_bounds.h * .5 - h * .5));
            bool paused = !data->is_card && saudio_is_stream(data->node) && data->node.corked;
            cr->paint_source(paused ? .6 : 1.0);
        } else {
            swifi_glyph(cr, c->real_bounds.x + 19 * dpi, c->real_bounds.y + c->real_bounds.h * .5 - 10 * dpi,
                        "\uE995", 18 * dpi, theme.text);
        }
    };

    auto titles = header->child(FILL_SPACE, FILL_SPACE);
    titles->when_paint = [data](Container *root, Container *c) {
        auto dpi = swifi_window(root)->dpi;
        auto b = c->real_bounds;
        std::string title, subtitle;
        if (data->is_card) {
            title = data->card.description;
            subtitle = data->card.name;
        } else if (saudio_is_stream(data->node)) {
            title = data->node.app_name.empty() ? data->node.description : data->node.app_name;
            if (!data->node.app_name.empty() && data->node.description != data->node.app_name)
                subtitle = data->node.description;
            if (data->node.corked)
                subtitle += subtitle.empty() ? "Paused" : " · Paused";
        } else {
            title = data->node.description;
            if (data->node.monitor_of != UINT32_MAX) {
                if (auto sink = saudio_node(AudioNodeKind::Sink, data->node.monitor_of))
                    subtitle = "Monitor of " + sink->description;
                else
                    subtitle = "Monitor";
            } else {
                subtitle = data->node.name;
            }
        }
        if (subtitle.empty()) {
            swifi_text(root, Bounds(b.x, b.y, b.w, b.h), title, 12, theme.text);
        } else {
            swifi_text(root, Bounds(b.x, b.y + b.h * .5 - 21 * dpi, b.w, 22 * dpi), title, 12, theme.text);
            swifi_text(root, Bounds(b.x, b.y + b.h * .5 + 1 * dpi, b.w, 20 * dpi), subtitle, 10, theme.text_secondary);
        }
    };

    saudio_dropdown(header, data, SAudioMenu::Port);
    saudio_dropdown(header, data, SAudioMenu::Device);
    saudio_dropdown(header, data, SAudioMenu::Profile);

    // "Default" toggle for output/input devices (pavucontrol's "Set as fallback").
    auto default_button = header->child(0, FILL_SPACE);
    default_button->pre_layout = [data](Container *root, Container *c, const Bounds &) {
        auto dpi = swifi_window(root)->dpi;
        c->exists = !data->is_card && !saudio_is_stream(data->node);
        bool is_default = saudio_is_default(data->node);
        c->wanted_bounds.w = swifi_text_width(root, is_default ? "Default" : "Set as default", 11) +
                             (is_default ? 46 : 24) * dpi;
    };
    default_button->when_paint = [data](Container *root, Container *c) {
        auto window = swifi_window(root);
        auto cr = window->drawing_context;
        auto dpi = window->dpi;
        bool is_default = saudio_is_default(data->node);
        double h = 30 * dpi;
        Bounds b(c->real_bounds.x, c->real_bounds.y + (c->real_bounds.h - h) * .5, c->real_bounds.w, h);
        if (is_default) {
            set_argb(cr, swifi_mix(theme.row_background, theme.accent, .25));
        } else {
            set_argb(cr, c->state.mouse_pressing ? theme.button_pressed :
                c->state.mouse_hovering ? theme.button_hover : theme.button_background);
        }
        drawRoundedRect(cr, b.x, b.y, b.w, b.h, settings_control_rounding * dpi, 1.0);
        cr->fill();
        if (is_default) {
            // Check mark glyph followed by the label.
            swifi_glyph(cr, b.x + 12 * dpi, b.y + b.h * .5 - 6 * dpi, "\uE73E", 11 * dpi, theme.accent);
            swifi_text(root, Bounds(b.x + 30 * dpi, b.y, b.w - 38 * dpi, b.h), "Default", 11, theme.text, true);
        } else {
            swifi_text(root, b, "Set as default", 11, theme.text, true);
        }
    };
    default_button->when_clicked = [data](Container *root, Container *) {
        if (saudio_is_default(data->node))
            return;
        audio_mixer_set_default(data->node.kind, data->node.index);
        // Optimistic: show it right away, the server confirms shortly.
        if (data->node.kind == AudioNodeKind::Sink)
            settings_audio->default_sink = data->node.name;
        else
            settings_audio->default_source = data->node.name;
        swifi_redraw(root);
    };

    // Mute button, volume slider and percentage.
    auto line = row->child(::hbox, FILL_SPACE, 30);
    line->pre_layout = [data](Container *root, Container *c, const Bounds &) {
        c->exists = !data->is_card;
        c->wanted_bounds.h = 30 * swifi_window(root)->dpi;
    };
    auto mute = line->child(56, FILL_SPACE);
    mute->pre_layout = [](Container *root, Container *c, const Bounds &) {
        c->wanted_bounds.w = 56 * swifi_window(root)->dpi;
    };
    mute->when_paint = [data](Container *root, Container *c) {
        auto window = swifi_window(root);
        auto cr = window->drawing_context;
        auto dpi = window->dpi;
        auto b = c->real_bounds;
        double size = 30 * dpi;
        Bounds circle(b.x + 13 * dpi, b.y + (b.h - size) * .5, size, size);
        if (c->state.mouse_hovering || c->state.mouse_pressing) {
            set_argb(cr, c->state.mouse_pressing ? theme.button_pressed : theme.button_hover);
            drawRoundedRect(cr, circle.x, circle.y, circle.w, circle.h, size * .5, 1.0);
            cr->fill();
        }
        double gx = circle.x + 8 * dpi;
        double gy = circle.y + circle.h * .5 - 7.5 * dpi;
        auto color = data->node.mute ? theme.text_secondary : theme.text;
        bool input = data->node.kind == AudioNodeKind::Source || data->node.kind == AudioNodeKind::SourceOutput;
        if (input) {
            // Microphone / microphone off.
            swifi_glyph(cr, gx, gy, data->node.mute ? "\uEC54" : "\uE720", 14 * dpi, color);
            return;
        }
        int val = (int) std::round(saudio_shown_volume(data) * 100);
        const char *glyph = data->node.mute ? "\uE74F" : val == 0 ? "\uE992" : val < 33 ? "\uE993" : val < 66 ? "\uE994" : "\uE995";
        RGBA faint = theme.text;
        faint.a *= .3;
        if (!data->node.mute)
            swifi_glyph(cr, gx, gy, "\uE995", 14 * dpi, faint);
        swifi_glyph(cr, gx, gy, glyph, 14 * dpi, color);
    };
    mute->when_clicked = [data](Container *root, Container *) {
        data->node.mute = !data->node.mute;
        audio_mixer_set_mute(data->node.kind, data->node.index, data->node.mute);
        swifi_redraw(root);
    };

    auto slider = line->child(FILL_SPACE, FILL_SPACE);
    slider->when_paint = [data](Container *root, Container *c) {
        auto window = swifi_window(root);
        auto cr = window->drawing_context;
        auto dpi = window->dpi;
        auto b = c->real_bounds;
        double r = 8 * dpi;
        double value = std::clamp(saudio_shown_volume(data), 0.0, 1.0);
        double track_h = 6 * dpi;
        double x0 = b.x + r;
        double w = std::max(1.0, b.w - 2 * r);
        double y = b.y + b.h * .5 - track_h * .5;
        RGBA fill = data->node.mute || !data->node.has_volume ? theme.text_secondary : theme.accent;
        set_argb(cr, theme.slider_track);
        drawRoundedRect(cr, x0, y, w, track_h, track_h * .5, 1.0);
        cr->fill();
        double cx = x0 + w * value;
        // The level meter fills the track, scaled by the volume so it never runs past the knob.
        // Behind it, like winbar, a pale high-water mark that holds the recent peak and falls slowly.
        if (data->node.peak_supported && !data->node.mute) {
            double level_w = (cx - x0) * saudio_meter_level(data->shown_peak);
            double mark_w = std::max(level_w, (cx - x0) * data->high_water_shown);
            if (mark_w > 0.5) {
                cr->save();
                drawRoundedRect(cr, x0, y, w, track_h, track_h * .5, 1.0);
                cr->clip_path();
                // A pill whose right end sits at the level; when it is shorter than it is tall,
                // its left end slides out of the track and is clipped away.
                auto pill = [cr, x0, y, track_h](double fill_w) {
                    double pill_w = std::max(fill_w, track_h);
                    double pill_x = x0 + fill_w - pill_w;
                    double rad = track_h * .5;
                    cr->new_path();
                    cr->arc(pill_x + pill_w - rad, y + rad, rad, -M_PI / 2, M_PI / 2);
                    cr->arc(pill_x + rad, y + rad, rad, M_PI / 2, 3 * M_PI / 2);
                    cr->close_path();
                };
                RGBA color = theme.accent;
                color.a = .22;
                set_argb(cr, color);
                pill(mark_w);
                cr->fill();
                if (level_w > 0.5) {
                    color.a = .55;
                    set_argb(cr, color);
                    pill(level_w);
                    cr->fill();
                }
                cr->restore();
            }
        }
        Bounds thumb(cx - r, b.y + b.h * .5 - r, r * 2, r * 2);
        set_argb(cr, theme.slider_thumb);
        drawRoundedRect(cr, thumb.x, thumb.y, thumb.w, thumb.h, r, 1.0);
        cr->fill();
        set_argb(cr, theme.slider_track);
        drawRoundedRect(cr, thumb.x, thumb.y, thumb.w, thumb.h, r, 1.0);
        cr->stroke();
        thumb.shrink((c->state.mouse_hovering || data->dragging ? 4 : 5) * dpi);
        set_argb(cr, fill);
        drawRoundedRect(cr, thumb.x, thumb.y, thumb.w, thumb.h, thumb.w * .5, 1.0);
        cr->fill();
    };
    slider->when_mouse_down = [data](Container *root, Container *c) {
        saudio_set_volume_from_mouse(root, c, data, false);
    };
    slider->when_drag = slider->when_mouse_down;
    slider->when_drag_end = [data](Container *root, Container *c) {
        saudio_set_volume_from_mouse(root, c, data, true);
    };
    slider->when_mouse_up = slider->when_drag_end;

    auto percent = line->child(64, FILL_SPACE);
    percent->pre_layout = [](Container *root, Container *c, const Bounds &) {
        c->wanted_bounds.w = 64 * swifi_window(root)->dpi;
    };
    percent->when_paint = [data](Container *root, Container *c) {
        auto b = c->real_bounds;
        auto dpi = swifi_window(root)->dpi;
        auto text = std::to_string((int) std::round(saudio_shown_volume(data) * 100)) + "%";
        swifi_text(root, Bounds(b.x + 10 * dpi, b.y, b.w - 10 * dpi, b.h), text, 11,
                   data->node.mute ? theme.text_secondary : theme.text);
    };

    return row;
}

static bool saudio_row_wanted(SettingsAudioPage *page, const AudioNodeInfo &n) {
    switch (settings_audio_tab) {
        case 0: return n.kind == AudioNodeKind::SinkInput;
        case 1: return n.kind == AudioNodeKind::SourceOutput;
        case 2: return n.kind == AudioNodeKind::Sink;
        case 3: return n.kind == AudioNodeKind::Source && (page->show_monitors || n.monitor_of == UINT32_MAX);
    }
    return false;
}

// Rebuilds the rows of the current tab from the latest snapshot. Rows are keyed so that a slider
// being dragged keeps its state while updates stream in.
static void saudio_apply(SettingsAudioPage *page) {
    auto content = page->list;
    std::vector<std::string> keys;
    if (settings_audio_tab == 4) {
        for (auto &card : page->cards)
            keys.push_back("card:" + std::to_string(card.index));
    } else {
        for (auto &n : page->nodes)
            if (saudio_row_wanted(page, n))
                keys.push_back(saudio_row_key(n));
    }
    std::unordered_set<std::string> wanted(keys.begin(), keys.end());
    std::unordered_map<std::string, Container *> rows;
    std::erase_if(content->children, [&wanted, &rows](Container *row) {
        if (wanted.contains(row->name)) {
            rows.emplace(row->name, row);
            return false;
        }
        delete row;
        return true;
    });
    auto row_for = [&](const std::string &key) {
        auto found = rows.find(key);
        if (found != rows.end())
            return found->second;
        auto row = saudio_row(content);
        content->children.pop_back(); // Re-inserted below in order.
        row->name = key;
        return row;
    };
    std::vector<Container *> ordered;
    if (settings_audio_tab == 4) {
        for (auto &card : page->cards) {
            auto row = row_for("card:" + std::to_string(card.index));
            auto data = (SettingsAudioRow *) row->user_data;
            data->is_card = true;
            data->card = card;
            ordered.push_back(row);
        }
    } else {
        for (auto &n : page->nodes) {
            if (!saudio_row_wanted(page, n))
                continue;
            auto row = row_for(saudio_row_key(n));
            auto data = (SettingsAudioRow *) row->user_data;
            data->is_card = false;
            data->node = n;
            ordered.push_back(row);
        }
    }
    content->children = std::move(ordered);
}

// Pulls the latest mixer state; returns true when something visible changed.
static bool saudio_refresh(SettingsAudioPage *page) {
    bool redraw = false;
    auto revision = audio_mixer_revision();
    auto nodes = audio_mixer_nodes();
    auto backend = audio_backend_name();
    if (revision != page->revision || backend != page->backend) {
        page->revision = revision;
        page->backend = backend;
        page->nodes = nodes;
        page->cards = audio_mixer_cards();
        page->default_sink = audio_mixer_default_sink();
        page->default_source = audio_mixer_default_source();
        saudio_apply(page);
        redraw = true;
    }
    // Peaks change constantly and aren't part of the revision; update them every tick.
    std::unordered_map<std::string, float> peaks;
    for (auto &n : nodes)
        peaks[saudio_row_key(n)] = n.peak;
    for (auto row : page->list->children) {
        auto data = (SettingsAudioRow *) row->user_data;
        if (data->is_card)
            continue;
        float peak = 0;
        if (data->node.kind == AudioNodeKind::SourceOutput) {
            // Recording streams show the level of the source they record from.
            AudioNodeInfo source;
            source.kind = AudioNodeKind::Source;
            source.index = data->node.device;
            auto it = peaks.find(saudio_row_key(source));
            if (it == peaks.end()) {
                source.kind = AudioNodeKind::Sink; // PipeWire: recording a sink's output directly.
                it = peaks.find(saudio_row_key(source));
            }
            if (it != peaks.end())
                peak = it->second;
        } else {
            auto it = peaks.find(saudio_row_key(data->node));
            if (it != peaks.end())
                peak = it->second;
        }
        for (auto &n : nodes)
            if (n.kind == data->node.kind && n.index == data->node.index)
                data->node.peak_supported = n.peak_supported;
        float decayed = std::max(0.0f, data->shown_peak * .82f - .002f);
        float shown = std::max(peak, decayed);
        if (std::abs(saudio_meter_level(shown) - saudio_meter_level(data->shown_peak)) > 0.002)
            redraw = true;
        data->shown_peak = shown;
        float mark = data->high_water.feed(saudio_meter_level(shown), get_current_time_in_ms());
        if (std::abs(mark - data->high_water_shown) > 0.002)
            redraw = true;
        data->high_water_shown = mark;
        if (data->dragging || get_current_time_in_ms() - data->last_local_change < 700)
            redraw = true;
    }
    return redraw;
}

// If the startup connection attempt failed (e.g. the sound server wasn't up yet), nothing else
// retries it until the dock volume button is used; do the same here for the first 40 seconds
// after the page is opened.
static long saudio_connect_deadline_ms = 0;

static void saudio_ensure_connected() {
    static long last_attempt_ms = 0;
    auto now = get_current_time_in_ms();
    if (audio_running || now > saudio_connect_deadline_ms || now - last_attempt_ms < 1000)
        return;
    last_attempt_ms = now;
    main_thread([]() {
        if (!audio_running)
            audio_start();
    });
}

static void saudio_watch(unsigned generation) {
    windowing::timer(settings_app, 33, [generation](void *) {
        if (!settings_app || !settings_mylar)
            return;
        bool redraw = false;
        {
            std::lock_guard<std::mutex> lock(settings_app->mutex);
            auto page = settings_audio;
            if (!page || page->generation != generation)
                return; // Page left or rebuilt; stop polling.
            if (page->backend.empty())
                saudio_ensure_connected();
            redraw = saudio_refresh(page);
        }
        if (redraw)
            windowing::redraw(settings_mylar->raw_window);
        saudio_watch(generation);
    }, nullptr);
}

static void fill_audio_settings(Container *root) {
    auto right = container_by_name("settings_right", root);
    if (!right)
        return;
    for (auto child: right->children)
        delete child;
    right->children.clear();

    right->pre_layout = layout_right_page;
    auto padded_right = right->child(FILL_SPACE, FILL_SPACE);

    make_section_title(padded_right, "Audio");
    make_vert_space(padded_right, 4);

    auto status = padded_right->child(FILL_SPACE, 20);
    status->pre_layout = [](Container *root, Container *c, const Bounds &) {
        c->wanted_bounds.h = 20 * swifi_window(root)->dpi;
    };
    status->when_paint = [](Container *root, Container *c) {
        auto backend = settings_audio ? settings_audio->backend : "";
        std::string text = backend.empty() ? "Not connected to a sound server" : "Using " + backend;
        swifi_text(root, c->real_bounds, text, 10, theme.text_secondary);
    };
    make_vert_space(padded_right, 12);

    // The five pavucontrol tabs.
    auto tabs = padded_right->child(::hbox, FILL_SPACE, 40);
    tabs->pre_layout = [](Container *root, Container *c, const Bounds &) {
        auto dpi = swifi_window(root)->dpi;
        c->wanted_bounds.h = 40 * dpi;
        c->wanted_pad = Bounds(4 * dpi, 4 * dpi, 4 * dpi, 4 * dpi);
        c->spacing = 4 * dpi;
    };
    // Kept across page rebuilds so a tab change still slides.
    static SegmentSlide audio_tab_slide;
    audio_tab_slide.painted = false;
    tabs->when_paint = [](Container *root, Container *c) {
        auto window = swifi_window(root);
        auto b = c->real_bounds;
        set_argb(window->drawing_context, theme.segment_background);
        drawRoundedRect(window->drawing_context, b.x, b.y, b.w, b.h, 10 * window->dpi, 1.0);
        window->drawing_context->fill();
        if (settings_audio_tab >= 0 && settings_audio_tab < (int) c->children.size())
            segment_slide_paint(root, c, &audio_tab_slide, c->children[settings_audio_tab]);
    };
    for (int i = 0; i < 5; i++) {
        auto tab = tabs->child(FILL_SPACE, FILL_SPACE);
        search_tag(tab, saudio_tab_names[i], "", false);
        tab->when_paint = [i](Container *root, Container *c) {
            auto window = swifi_window(root);
            auto cr = window->drawing_context;
            auto dpi = window->dpi;
            auto b = c->real_bounds;
            bool selected = settings_audio_tab == i;
            if (!selected && c->state.mouse_hovering) {
                set_argb(cr, swifi_mix(theme.segment_background, theme.segment_selected, .5));
                drawRoundedRect(cr, b.x, b.y, b.w, b.h, 8 * dpi, 1.0);
                cr->fill();
            }
            swifi_text(root, b, saudio_tab_names[i], 11, selected ? theme.text : theme.text_secondary, true);
        };
        tab->when_clicked = [i](Container *root, Container *) {
            if (settings_audio_tab == i || !settings_audio)
                return;
            segment_slide_start(&audio_tab_slide);
            settings_audio_tab = i;
            if (auto right = container_by_name("settings_right", root))
                ((RightData *) right->user_data)->scroll = 0;
            saudio_apply(settings_audio);
            swifi_redraw(root);
        };
    }
    make_vert_space(padded_right, 12);

    auto list = padded_right->child(::vbox, FILL_SPACE, 0);
    auto page = new SettingsAudioPage;
    page->list = list;
    page->generation = ++settings_audio_generation;
    list->user_data = page;
    list->pre_layout = [](Container *root, Container *c, const Bounds &) {
        auto dpi = swifi_window(root)->dpi;
        c->spacing = 6 * dpi;
        double h = 0;
        for (auto row : c->children) {
            auto data = (SettingsAudioRow *) row->user_data;
            row->wanted_bounds.h = (data->is_card ? 70 : 92) * dpi;
            h += row->wanted_bounds.h;
        }
        if (c->children.empty())
            h = 120 * dpi;
        else
            h += c->spacing * (c->children.size() - 1);
        c->wanted_bounds.h = h;
    };
    list->when_paint = [](Container *root, Container *c) {
        if (!c->children.empty())
            return;
        auto window = swifi_window(root);
        auto dpi = window->dpi;
        static const char *empty_messages[] = {
            "No application is currently playing audio.",
            "No application is currently recording audio.",
            "No output devices available.",
            "No input devices available.",
            "No sound cards available.",
        };
        std::string message = settings_audio && settings_audio->backend.empty()
            ? "Connecting to the sound server…" : empty_messages[settings_audio_tab];
        auto b = c->real_bounds;
        drawRoundedRect(window->drawing_context, b.x, b.y, b.w, b.h, 5 * dpi, 1.0);
        set_argb(window->drawing_context, theme.row_background);
        window->drawing_context->fill();
        drawRoundedRect(window->drawing_context, b.x, b.y, b.w, b.h, 5 * dpi, 1.0);
        set_argb(window->drawing_context, theme.row_border);
        window->drawing_context->stroke();
        b.x += 30 * dpi;
        b.w -= 60 * dpi;
        swifi_text(root, b, message, 11, theme.text_secondary, true);
    };

    // Input Devices: monitors of output devices are hidden unless asked for.
    auto footer = padded_right->child(::hbox, FILL_SPACE, 0);
    footer->pre_layout = [](Container *root, Container *c, const Bounds &) {
        auto dpi = swifi_window(root)->dpi;
        bool any_monitor = false;
        if (settings_audio)
            for (auto &n : settings_audio->nodes)
                any_monitor |= n.kind == AudioNodeKind::Source && n.monitor_of != UINT32_MAX;
        c->exists = settings_audio_tab == 3 && any_monitor;
        c->wanted_bounds.h = 46 * dpi;
        c->wanted_pad = Bounds(0, 12 * dpi, 0, 0);
    };
    footer->child(FILL_SPACE, FILL_SPACE);
    swifi_card_button(footer, []() {
        return settings_audio && settings_audio->show_monitors ? std::string("Hide monitor sources")
                                                               : std::string("Show monitor sources");
    }, []() { return settings_audio != nullptr; }, []() {
        settings_audio->show_monitors = !settings_audio->show_monitors;
        saudio_apply(settings_audio);
    });

    // The previous page (if any) was deleted above and stopped its meters.
    settings_audio = page;
    saudio_connect_deadline_ms = get_current_time_in_ms() + 40000;
    saudio_ensure_connected();
    saudio_refresh(page);
    audio_mixer_meters(true);
    saudio_watch(page->generation);
}

/*************************************************
 *
 * Time & Date page (modelled on GNOME's Date & Time panel)
 *
 *************************************************/

struct SettingsDateTimeState {
    std::mutex mutex;
    datetime::Status status;
    bool loaded = false;
    bool busy = false;
    unsigned generation = 0;
};
static SettingsDateTimeState sdt;

static void sdt_invalidate() {
    std::lock_guard lock(sdt.mutex);
    sdt.generation++;
}

static datetime::Status sdt_snapshot(bool *loaded = nullptr, bool *busy = nullptr) {
    std::lock_guard lock(sdt.mutex);
    if (loaded)
        *loaded = sdt.loaded;
    if (busy)
        *busy = sdt.busy;
    return sdt.status;
}

static void sdt_refresh_async() {
    spawn_settings_worker([]() {
        datetime::timezones(); // Parse the zone tables off the UI thread.
        auto status = datetime::query_status();
        std::lock_guard lock(sdt.mutex);
        sdt.status = status;
        sdt.loaded = true;
    });
}

// Runs a privileged change on a worker thread (it may wait on a polkit prompt), then re-reads the state.
static void sdt_run_async(std::function<std::string()> job) {
    {
        std::lock_guard lock(sdt.mutex);
        if (sdt.busy)
            return;
        sdt.busy = true;
    }
    spawn_settings_worker([job]() {
        auto error = job();
        auto status = datetime::query_status();
        {
            std::lock_guard lock(sdt.mutex);
            sdt.status = status;
            sdt.loaded = true;
            sdt.busy = false;
        }
        main_thread([error]() {
            if (!error.empty())
                notify(error);
            dock::redraw();
        });
    });
}

// Keeps the clock text fresh and picks up worker results while the page is showing.
static void sdt_watch(unsigned generation) {
    windowing::timer(settings_app, 500, [generation](void *) {
        if (!settings_app || !settings_mylar)
            return;
        {
            std::lock_guard lock(sdt.mutex);
            if (sdt.generation != generation)
                return;
        }
        windowing::redraw(settings_mylar->raw_window);
        sdt_watch(generation);
    }, nullptr);
}

struct SdtRowSpec {
    float height = 52;
    std::function<std::string()> title;
    std::function<std::string()> subtitle;
    std::function<std::string()> value;      // Text on the right, if any
    std::function<bool()> enabled;
    std::function<int()> accessory;          // 0 none, 1 chevron, 2 dropdown arrow, 3 switch
    std::function<bool()> switch_on;
    std::function<void(Container *root, Container *anchor)> on_click;
};

// A row in a group of connected rounded rows, drawn like the Shortcuts list.
static Container *sdt_row(Container *parent, SdtRowSpec spec, bool is_first, bool is_last) {
    auto spec_ptr = std::make_shared<SdtRowSpec>(std::move(spec));
    auto toggle = std::make_shared<ToggleAnim>();
    auto row = parent->child(::hbox, FILL_SPACE, FILL_SPACE);
    if (spec_ptr->title)
        search_tag(row, spec_ptr->title(), spec_ptr->subtitle ? spec_ptr->subtitle() : "");
    row->receive_events_even_if_obstructed = true;
    row->child(FILL_SPACE, FILL_SPACE);
    auto anchor = row->child(FILL_SPACE, FILL_SPACE);

    row->pre_layout = [spec_ptr](Container *root, Container *c, const Bounds &) {
        auto dpi = swifi_window(root)->dpi;
        c->wanted_bounds.h = spec_ptr->height * dpi;
        double w = 0;
        if (spec_ptr->value)
            w += swifi_text_width(root, spec_ptr->value(), 12) + 12 * dpi;
        int accessory = spec_ptr->accessory ? spec_ptr->accessory() : 0;
        w += accessory == 3 ? toggle_w * dpi : accessory ? 14 * dpi : 0;
        if (accessory == 3) // On/Off label, like the other boolean rows
            w += std::max(swifi_text_width(root, "On", 11), swifi_text_width(root, "Off", 11)) + 12 * dpi;
        c->children[1]->wanted_bounds.w = w + 18 * dpi;
    };
    row->when_paint = [spec_ptr, toggle, is_first, is_last](Container *root, Container *c) {
        auto window = swifi_window(root);
        auto cr = window->drawing_context;
        auto dpi = window->dpi;
        bool enabled = !spec_ptr->enabled || spec_ptr->enabled();
        bool clickable = enabled && spec_ptr->on_click;

        auto b = c->real_bounds;
        const auto overlap = (std::floor(1.0 * dpi) + 1.0) * 0.5;
        b.y -= overlap;
        b.h += 2.0 * overlap;
        drawRoundedRect(cr, b.x, b.y, b.w, b.h, 8 * dpi, 0, is_first, is_first, is_last, is_last);
        RGBA bg = theme.row_background;
        if (clickable && (c->state.mouse_hovering || c->state.mouse_pressing))
            bg = theme.row_hover;
        set_argb(cr, bg);
        cr->fill();
        drawRoundedRect(cr, b.x, b.y, b.w, b.h, 8 * dpi, std::floor(1.0 * dpi), is_first, is_first, is_last, is_last);
        set_argb(cr, theme.row_border);
        cr->set_line_width(std::floor(1.0 * dpi));
        cr->stroke();

        RGBA primary = theme.text;
        RGBA secondary = theme.text_secondary;
        if (!enabled) {
            primary.a *= .5;
            secondary.a *= .5;
        }

        double pad = 16 * dpi;
        b = c->real_bounds;
        std::string title = spec_ptr->title();
        std::string subtitle = spec_ptr->subtitle ? spec_ptr->subtitle() : "";
        auto tb = draw_text(cr, 0, 0, title, 12 * dpi, false, set->font, -1, -1, primary, false);
        if (subtitle.empty()) {
            draw_text(cr, b.x + pad, b.y + b.h * .5 - tb.h * .5, title, 12 * dpi, true, set->font, -1, -1, primary, false);
        } else {
            auto sb = draw_text(cr, 0, 0, subtitle, 10 * dpi, false, set->font, -1, -1, secondary, false);
            double total = tb.h + 2 * dpi + sb.h;
            double y = b.y + b.h * .5 - total * .5;
            draw_text(cr, b.x + pad, y, title, 12 * dpi, true, set->font, -1, -1, primary, false);
            draw_text(cr, b.x + pad, y + tb.h + 2 * dpi, subtitle, 10 * dpi, true, set->font, -1, -1, secondary, false);
        }

        double right = b.x + b.w - pad;
        int accessory = spec_ptr->accessory ? spec_ptr->accessory() : 0;
        if (accessory == 3) {
            right -= toggle_w * dpi;
            Bounds check(right, b.y + b.h * .5 - toggle_h * .5 * dpi, toggle_w * dpi, toggle_h * dpi);
            bool hovered = c->state.mouse_hovering || c->state.mouse_pressing;
            bool on = spec_ptr->switch_on && spec_ptr->switch_on();
            paint_toggle(root, c, toggle.get(), check, on, hovered, c->state.mouse_pressing, enabled);
            right -= 12 * dpi;
            std::string text = on ? "On" : "Off";
            auto ob = draw_text(cr, 0, 0, text, 11 * dpi, false, set->font, -1, -1, primary, false);
            right -= ob.w;
            draw_text(cr, right, b.y + b.h * .5 - ob.h * .5, text, 11 * dpi, true, set->font, -1, -1, primary, false);
            right -= 12 * dpi;
        } else if (accessory == 1) {
            auto cb = draw_text(cr, 0, 0, "›", 16 * dpi, false, set->font, -1, -1, secondary, false);
            right -= cb.w;
            draw_text(cr, right, b.y + b.h * .5 - cb.h * .5 - 1 * dpi, "›", 16 * dpi, true, set->font, -1, -1, secondary, false);
            right -= 10 * dpi;
        } else if (accessory == 2) {
            right -= 10 * dpi;
            swifi_glyph(cr, right, b.y + b.h * .5 - 6 * dpi, "", 10 * dpi, primary);
            right -= 10 * dpi;
        }
        if (spec_ptr->value) {
            auto value = spec_ptr->value();
            auto vb = draw_text(cr, 0, 0, value, 12 * dpi, false, set->font, -1, -1, secondary, false);
            draw_text(cr, right - vb.w, b.y + b.h * .5 - vb.h * .5, value, 12 * dpi, true, set->font, -1, -1,
                accessory == 2 ? primary : secondary, false);
        }
    };
    row->when_clicked = [spec_ptr, anchor](Container *root, Container *c) {
        bool enabled = !spec_ptr->enabled || spec_ptr->enabled();
        if (enabled && spec_ptr->on_click)
            spec_ptr->on_click(root, anchor);
    };
    return row;
}

// Opens an empty popup below `anchor` that behaves like the drop-down menus (one at a time, closes on Esc).
static MylarWindow *sdt_open_popup(Container *root, Container *anchor, int w, int h) {
    auto mylar = (MylarWindow *) root->user_data;
    if (dropdown_popup || mylar->popup_window)
        return nullptr;
    auto settings = make_icon_anchored_popup_settings(anchor, mylar->raw_window->dpi, w, h);
    // Without this, key presses that land on the settings window aren't forwarded to the
    // popup, so the time zone search field never receives typing.
    settings.keyboard_focus_on_click = true;
    auto popup = open_mylar_popup(mylar, settings);
    if (!popup)
        return nullptr;
    dropdown_popup = popup;
    popups.push_back(popup);
    popup->root->skip_delete = true;
    popup->root->user_data = popup;
    popup->root->wanted_bounds.w = FILL_SPACE;
    popup->root->wanted_bounds.h = FILL_SPACE;
    popup->root->on_closed = [popup](Container *) {
        if (dropdown_popup == popup)
            dropdown_popup = nullptr;
        std::erase(popups, popup);
    };
    popup->root->when_paint = [](Container *root, Container *c) {
        auto window = ((MylarWindow *) root->user_data)->raw_window;
        auto b = c->real_bounds;
        paint_smooth_frame(window->drawing_context, b, 8 * window->dpi, theme.popup_background, theme.dropdown_border);
    };
    popup->root->when_key_event = [](Container *, Container *, int, bool pressed, xkb_keysym_t sym, int, bool, std::string) {
        if (pressed && sym == XKB_KEY_Escape)
            close_dropdown_menu();
    };
    focus_install_dialog(popup);
    return popup;
}

static Container *sdt_fixed(Container *parent, int type, float h) {
    auto c = parent->child(type, FILL_SPACE, FILL_SPACE);
    c->pre_layout = [h](Container *root, Container *c, const Bounds &) {
        auto dpi = swifi_window(root)->dpi;
        c->wanted_bounds.h = h * dpi;
        c->spacing = 8 * dpi;
    };
    return c;
}

static Container *sdt_popup_button(Container *parent, std::string label, bool primary, std::function<void()> on_click) {
    auto button = parent->child(FILL_SPACE, FILL_SPACE);
    button->pre_layout = [label](Container *root, Container *c, const Bounds &) {
        c->wanted_bounds.w = swifi_text_width(root, label, 11) + 32 * swifi_window(root)->dpi;
    };
    button->when_paint = [label, primary](Container *root, Container *c) {
        auto window = swifi_window(root);
        auto b = c->real_bounds;
        RGBA bg = primary ? theme.accent : theme.dropdown_background;
        if (c->state.mouse_pressing)
            bg = primary ? swifi_mix(theme.accent, RGBA(0, 0, 0, 1), .15) : theme.dropdown_pressed;
        else if (c->state.mouse_hovering)
            bg = primary ? swifi_mix(theme.accent, RGBA(1, 1, 1, 1), .12) : theme.dropdown_hover;
        set_argb(window->drawing_context, bg);
        drawRoundedRect(window->drawing_context, b.x, b.y, b.w, b.h, settings_control_rounding * window->dpi, 1.0);
        window->drawing_context->fill();
        if (!primary) {
            set_argb(window->drawing_context, theme.dropdown_border);
            drawRoundedRect(window->drawing_context, b.x, b.y, b.w, b.h, settings_control_rounding * window->dpi, 1.0);
            window->drawing_context->stroke();
        }
        swifi_text(root, b, label, 11, primary ? RGBA(1, 1, 1, 1) : theme.text, true, primary);
    };
    button->when_clicked = [on_click](Container *, Container *) {
        on_click();
    };
    return button;
}

struct SdtDraft {
    int year = 2000, month = 0, day = 1, hour = 0, minute = 0, second = 0;
};

static int sdt_days_in_month(int year, int month) {
    static const int days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
    return month == 1 && leap ? 29 : days[month];
}

// A value with − and + halves; the mouse wheel also steps it.
static void sdt_spin(Container *parent, std::function<std::string()> label, std::function<void(int)> step) {
    auto cell = parent->child(FILL_SPACE, FILL_SPACE);
    cell->when_paint = [label](Container *root, Container *c) {
        auto window = swifi_window(root);
        auto cr = window->drawing_context;
        auto dpi = window->dpi;
        auto b = c->real_bounds;
        paint_smooth_frame(cr, b, settings_control_rounding * dpi, theme.field_background, theme.field_border);
        double third = b.w / 4;
        if (c->state.mouse_hovering) {
            bool left = root->mouse_current_x < b.x + third;
            bool right = root->mouse_current_x > b.x + b.w - third;
            if (left || right) {
                set_argb(cr, c->state.mouse_pressing ? theme.dropdown_pressed : theme.dropdown_hover);
                drawRoundedRect(cr, left ? b.x + 2 * dpi : b.x + b.w - third + 1 * dpi, b.y + 2 * dpi, third - 3 * dpi, b.h - 4 * dpi,
                    (settings_control_rounding - 1) * dpi, 1.0);
                cr->fill();
            }
        }
        swifi_text(root, Bounds(b.x, b.y, third, b.h), "−", 12, theme.text_secondary, true);
        swifi_text(root, Bounds(b.x + b.w - third, b.y, third, b.h), "+", 12, theme.text_secondary, true);
        swifi_text(root, Bounds(b.x + third, b.y, b.w - third * 2, b.h), label(), 11, theme.text, true);
    };
    cell->when_clicked = [step](Container *root, Container *c) {
        auto b = c->real_bounds;
        double third = b.w / 4;
        if (root->mouse_current_x < b.x + third)
            step(-1);
        else if (root->mouse_current_x > b.x + b.w - third)
            step(1);
    };
    cell->when_fine_scrolled = [step](Container *root, Container *, double, double scroll_y, bool) {
        if (scroll_y > 0)
            step(1);
        else if (scroll_y < 0)
            step(-1);
        swifi_redraw(root);
    };
    // Up and Down step the value, like a spin box; Left and Right move on.
    focus_register(cell, {
        .activate = [](Container *, Container *) {},
        .arrow = [step](Container *, Container *, int, int dy) {
            if (dy == 0)
                return false;
            step(-dy);
            return true;
        },
    });
}

static void sdt_caption(Container *parent, std::string text) {
    auto c = parent->child(FILL_SPACE, FILL_SPACE);
    c->when_paint = [text](Container *root, Container *c) {
        swifi_text(root, c->real_bounds, text, 10, theme.text_secondary, true);
    };
}

// GNOME's date & time dialog: day, month, year, hour, minute (and AM/PM in 12-hour mode).
static void sdt_open_datetime_dialog(Container *root, Container *anchor) {
    auto popup = sdt_open_popup(root, anchor, 360, 240);
    if (!popup)
        return;
    auto draft = std::make_shared<SdtDraft>();
    auto now = datetime::now_tm();
    draft->year = now.tm_year + 1900;
    draft->month = now.tm_mon;
    draft->day = now.tm_mday;
    draft->hour = now.tm_hour;
    draft->minute = now.tm_min;
    draft->second = now.tm_sec;
    bool twenty_four = set->clock_24h;

    auto body = popup->root->child(::vbox, FILL_SPACE, FILL_SPACE);
    body->pre_layout = [](Container *root, Container *c, const Bounds &) {
        auto dpi = swifi_window(root)->dpi;
        c->wanted_pad = Bounds(16 * dpi, 14 * dpi, 16 * dpi, 14 * dpi);
        c->spacing = 4 * dpi;
    };
    auto title = sdt_fixed(body, ::hbox, 26);
    title->when_paint = [](Container *root, Container *c) {
        swifi_text(root, c->real_bounds, "Date & Time", 12, theme.text, false, true);
    };

    auto clamp_day = [draft]() {
        draft->day = std::clamp(draft->day, 1, sdt_days_in_month(draft->year, draft->month));
    };
    auto captions1 = sdt_fixed(body, ::hbox, 18);
    sdt_caption(captions1, "Day");
    sdt_caption(captions1, "Month");
    sdt_caption(captions1, "Year");
    auto row1 = sdt_fixed(body, ::hbox, 34);
    sdt_spin(row1, [draft]() { return std::to_string(draft->day); }, [draft](int d) {
        int n = sdt_days_in_month(draft->year, draft->month);
        draft->day = (draft->day - 1 + d + n) % n + 1;
    });
    focus_dialog_start(popup->root, row1->children.back());
    sdt_spin(row1, [draft]() {
        static const char *months[] = {"January", "February", "March", "April", "May", "June", "July",
                                       "August", "September", "October", "November", "December"};
        return std::string(months[draft->month]);
    }, [draft, clamp_day](int d) {
        draft->month = (draft->month + d + 12) % 12;
        clamp_day();
    });
    sdt_spin(row1, [draft]() { return std::to_string(draft->year); }, [draft, clamp_day](int d) {
        draft->year = std::clamp(draft->year + d, 1970, 9999);
        clamp_day();
    });

    auto captions2 = sdt_fixed(body, ::hbox, 18);
    sdt_caption(captions2, "Hour");
    sdt_caption(captions2, "Minute");
    if (!twenty_four)
        sdt_caption(captions2, "");
    auto row2 = sdt_fixed(body, ::hbox, 34);
    sdt_spin(row2, [draft, twenty_four]() {
        if (twenty_four) {
            char buf[8];
            std::snprintf(buf, sizeof(buf), "%02d", draft->hour);
            return std::string(buf);
        }
        int h = draft->hour % 12;
        return std::to_string(h == 0 ? 12 : h);
    }, [draft, twenty_four](int d) {
        if (twenty_four)
            draft->hour = (draft->hour + d + 24) % 24;
        else
            draft->hour = (draft->hour / 12) * 12 + (draft->hour % 12 + d + 12) % 12;
    });
    sdt_spin(row2, [draft]() {
        char buf[8];
        std::snprintf(buf, sizeof(buf), "%02d", draft->minute);
        return std::string(buf);
    }, [draft](int d) {
        draft->minute = (draft->minute + d + 60) % 60;
    });
    if (!twenty_four) {
        sdt_spin(row2, [draft]() { return std::string(draft->hour < 12 ? "AM" : "PM"); }, [draft](int) {
            draft->hour = (draft->hour + 12) % 24;
        });
    }

    body->child(FILL_SPACE, FILL_SPACE);
    auto buttons = sdt_fixed(body, ::hbox, 30);
    buttons->child(FILL_SPACE, FILL_SPACE);
    sdt_popup_button(buttons, "Cancel", false, []() { close_dropdown_menu(); });
    sdt_popup_button(buttons, "Set", true, [draft]() {
        auto when = datetime::local_to_unix(draft->year, draft->month, draft->day, draft->hour, draft->minute, draft->second);
        close_dropdown_menu();
        sdt_run_async([when]() { return datetime::set_time(when); });
    });
    windowing::redraw(popup->raw_window);
}

struct SdtZoneRow {
    const datetime::TimezoneEntry *entry;
    std::string abbreviation;
};

// Scrollbar for the popup lists (time zones, keyboard layouts): thin, wider while
// hovered or dragged, draggable, and clicking the track jumps there.
// T needs `float scroll`, `float overflow` and `ListScrollbar bar`.
struct ListScrollbar {
    bool hovered = false;
    bool dragging = false;
    bool pressed = false; // The press began on the bar, so it must not pick a row
    double grab = 0;
};

static constexpr float list_bar_hit_w = 12;

static bool list_bar_hit(Container *root, Container *c, float overflow) {
    auto dpi = swifi_window(root)->dpi;
    auto b = c->real_bounds;
    return overflow > 0 && root->mouse_current_x >= b.x + b.w - list_bar_hit_w * dpi &&
        root->mouse_current_x <= b.x + b.w && root->mouse_current_y >= b.y && root->mouse_current_y <= b.y + b.h;
}

static void list_bar_thumb(Container *c, double dpi, float scroll, float overflow, double &thumb_y, double &thumb_h) {
    auto b = c->real_bounds;
    double content = b.h + overflow;
    thumb_h = std::min(b.h, std::max(24.0 * dpi, b.h * b.h / content));
    thumb_y = b.y + (overflow > 0 ? (-scroll / overflow) * (b.h - thumb_h) : 0);
}

static void list_bar_paint(Container *root, Container *c, float scroll, float overflow, const ListScrollbar &bar) {
    if (overflow <= 0)
        return;
    auto window = swifi_window(root);
    auto cr = window->drawing_context;
    auto dpi = window->dpi;
    auto b = c->real_bounds;
    const bool active = bar.hovered || bar.dragging;
    double ty, thumb_h;
    list_bar_thumb(c, dpi, scroll, overflow, ty, thumb_h);
    double w = (active ? 6 : 3) * dpi;
    double x = b.x + b.w - w - 2 * dpi;
    if (active) {
        RGBA track = theme.text_secondary;
        track.a *= .12;
        set_argb(cr, track);
        drawRoundedRect(cr, x, b.y, w, b.h, w * .5, 1.0);
        cr->fill();
    }
    RGBA color = theme.text_secondary;
    color.a *= bar.dragging ? .8 : active ? .65 : .4;
    set_argb(cr, color);
    drawRoundedRect(cr, x, ty, w, thumb_h, w * .5, 1.0);
    cr->fill();
}

// Whether a row under the mouse should show as hovered.
static bool list_bar_blocks_hover(const ListScrollbar &bar) {
    return bar.hovered || bar.dragging;
}

template <typename T>
static void list_bar_scroll_to(Container *root, Container *c, double thumb_top) {
    auto data = (T *) c->user_data;
    double thumb_y, thumb_h;
    list_bar_thumb(c, swifi_window(root)->dpi, data->scroll, data->overflow, thumb_y, thumb_h);
    double room = c->real_bounds.h - thumb_h;
    double t = room > 0 ? std::clamp((thumb_top - c->real_bounds.y) / room, 0.0, 1.0) : 0.0;
    data->smooth.stop();
    data->scroll = (float) (-t * data->overflow);
    swifi_redraw(root);
}

// Call after setting the list's when_clicked, which is skipped for presses on the bar.
template <typename T>
static void list_bar_install(Container *list) {
    list->when_mouse_motion = [](Container *root, Container *c) {
        auto data = (T *) c->user_data;
        data->bar.hovered = list_bar_hit(root, c, data->overflow);
        swifi_redraw(root);
    };
    list->when_mouse_leaves_container = [](Container *root, Container *c) {
        auto data = (T *) c->user_data;
        if (!data->bar.dragging)
            data->bar.hovered = false;
        swifi_redraw(root);
    };
    list->when_mouse_down = [](Container *root, Container *c) {
        auto data = (T *) c->user_data;
        data->bar.pressed = list_bar_hit(root, c, data->overflow);
        if (!data->bar.pressed)
            return;
        data->bar.dragging = true;
        data->bar.hovered = true;
        double thumb_y, thumb_h;
        list_bar_thumb(c, swifi_window(root)->dpi, data->scroll, data->overflow, thumb_y, thumb_h);
        double y = root->mouse_current_y;
        if (y >= thumb_y && y <= thumb_y + thumb_h) {
            data->bar.grab = y - thumb_y;
        } else {
            // Clicking the track jumps there, centering the thumb under the mouse.
            data->bar.grab = thumb_h * .5;
            list_bar_scroll_to<T>(root, c, y - data->bar.grab);
        }
        swifi_redraw(root);
    };
    list->when_drag = [](Container *root, Container *c) {
        auto data = (T *) c->user_data;
        if (data->bar.dragging)
            list_bar_scroll_to<T>(root, c, root->mouse_current_y - data->bar.grab);
    };
    auto release = [](Container *root, Container *c) {
        auto data = (T *) c->user_data;
        if (!data->bar.dragging)
            return;
        data->bar.dragging = false;
        data->bar.hovered = list_bar_hit(root, c, data->overflow);
        swifi_redraw(root);
    };
    list->when_mouse_up = release;
    list->when_drag_end = release;
    auto clicked = std::move(list->when_clicked);
    list->when_clicked = [clicked](Container *root, Container *c) {
        auto data = (T *) c->user_data;
        if (data->bar.pressed) {
            data->bar.pressed = false;
            return;
        }
        if (clicked)
            clicked(root, c);
    };
}

// Keyboard rows for the popup lists: Up, Down, Page Up/Down via the arrows keep a
// highlighted row in view, Enter or Space picks it. Up from the first row moves
// focus back out (to the search field). The pointer clears the highlight.
// T needs `shown`, `scroll`, `overflow`, `key_index` and `smooth`.
// The first row fully in view, where keyboard selection starts.
template <typename T>
static int list_first_visible(Container *root, T *data, int row_h) {
    double h = row_h * swifi_window(root)->dpi;
    int first = (int) std::ceil(-data->scroll / h - .01);
    return std::clamp(first, 0, std::max(0, (int) data->shown.size() - 1));
}

template <typename T>
static void list_keys_install(Container *list, int row_h, std::function<void(Container *root, T *data, int index)> pick) {
    auto motion = std::move(list->when_mouse_motion);
    list->when_mouse_motion = [motion](Container *root, Container *c) {
        ((T *) c->user_data)->key_index = -1;
        if (motion)
            motion(root, c);
    };
    focus_register(list, {
        .activate = [pick](Container *root, Container *c) {
            auto data = (T *) c->user_data;
            if (data->key_index >= 0 && data->key_index < (int) data->shown.size())
                pick(root, data, data->key_index);
        },
        .arrow = [row_h](Container *root, Container *c, int, int dy) {
            auto data = (T *) c->user_data;
            int count = (int) data->shown.size();
            if (dy == 0 || count == 0)
                return false;
            if (dy < 0 && data->key_index <= 0) {
                data->key_index = -1;
                return false;
            }
            if (data->key_index < 0)
                data->key_index = list_first_visible(root, data, row_h);
            else
                data->key_index = std::clamp(data->key_index + dy, 0, count - 1);
            auto dpi = swifi_window(root)->dpi;
            double top = data->key_index * row_h * dpi;
            double bottom = top + row_h * dpi;
            double view = c->real_bounds.h;
            float scroll = data->scroll;
            if (top + scroll < 0)
                scroll = (float) -top;
            else if (bottom + scroll > view)
                scroll = (float) -(bottom - view);
            data->smooth.stop();
            data->scroll = std::clamp(scroll, -data->overflow, 0.0f);
            return true;
        },
        .radius = 6,
    });
}

/*
 * Font completions: while the Font field is typed in, a popup under it lists the installed
 * font families that continue the text (then the ones containing it elsewhere). Each row is
 * drawn in the font it names unless set->font_suggestions_in_own_font is off.
 * The popup's xdg grab includes the settings window, so typing keeps reaching the field.
 */

struct FontCompletionList : UserData {
    std::vector<std::string> shown;
    float scroll = 0;
    float overflow = 0;
    float view_h = 0;
    int highlighted = -1;
    ListScrollbar bar;
    SmoothScroll smooth;
};

static constexpr int font_completion_row_h = 32;
static constexpr int font_completion_max_rows = 8;

static MylarWindow *font_completion_popup = nullptr;
static FontCompletionList *font_completion_list = nullptr;
static RawWindowSettings font_completion_settings;

static std::string font_completion_lower(std::string s) {
    for (auto &ch : s)
        ch = (char) std::tolower((unsigned char) ch);
    return s;
}

struct InstalledFontFamilies {
    std::vector<std::string> names;
    std::vector<std::string> lowered;
};

// Loaded once: fontconfig's family list doesn't change while the settings are open.
static const InstalledFontFamilies &installed_font_families() {
    static InstalledFontFamilies families;
    static bool loaded = false;
    if (loaded)
        return families;
    loaded = true;
    PangoFontFamily **list = nullptr;
    int count = 0;
    pango_font_map_list_families(pango_cairo_font_map_get_default(), &list, &count);
    for (int i = 0; i < count; i++)
        if (const char *name = pango_font_family_get_name(list[i]))
            families.names.emplace_back(name);
    g_free(list);
    std::sort(families.names.begin(), families.names.end(), [](const std::string &a, const std::string &b) {
        return font_completion_lower(a) < font_completion_lower(b);
    });
    families.names.erase(std::unique(families.names.begin(), families.names.end()), families.names.end());
    for (auto &name : families.names)
        families.lowered.push_back(font_completion_lower(name));
    return families;
}

// Families starting with `query`, followed by families containing it elsewhere.
static std::vector<std::string> font_completions(const std::string &query) {
    std::vector<std::string> prefix;
    if (query.empty())
        return prefix;
    std::vector<std::string> inner;
    auto q = font_completion_lower(query);
    const auto &families = installed_font_families();
    for (size_t i = 0; i < families.names.size(); i++) {
        auto pos = families.lowered[i].find(q);
        if (pos == std::string::npos)
            continue;
        (pos == 0 ? prefix : inner).push_back(families.names[i]);
    }
    prefix.insert(prefix.end(), inner.begin(), inner.end());
    return prefix;
}

static int font_completion_popup_h(size_t rows) {
    return (int) std::min<size_t>(rows, font_completion_max_rows) * font_completion_row_h + 8;
}

static bool font_completions_open() {
    return font_completion_popup && font_completion_list && dropdown_popup == font_completion_popup;
}

static void close_font_completions() {
    if (font_completions_open())
        close_dropdown_menu();
    font_completion_popup = nullptr;
    font_completion_list = nullptr;
}

static void pick_font_completion(Container *field, const std::string &family, const std::function<void(std::string)> &on_pick) {
    close_font_completions();
    reset_text_editor(field, family);
    if (on_pick)
        on_pick(family);
    if (settings_mylar)
        windowing::redraw(settings_mylar->raw_window);
}

// Scrolls the list so the highlighted row is fully visible.
static void reveal_font_completion(FontCompletionList *data) {
    if (!font_completion_popup || data->highlighted < 0)
        return;
    double row_h = font_completion_row_h * font_completion_popup->raw_window->dpi;
    double top = data->highlighted * row_h;
    data->smooth.stop();
    if (top + data->scroll < 0)
        data->scroll = (float) -top;
    else if (top + row_h + data->scroll > data->view_h)
        data->scroll = (float) -(top + row_h - data->view_h);
    data->scroll = std::clamp(data->scroll, -data->overflow, 0.0f);
}

static void open_font_completions(Container *root, Container *field, std::vector<std::string> shown,
                                  std::function<void(std::string)> on_pick) {
    auto mylar = (MylarWindow *) root->user_data;
    if (dropdown_popup || mylar->popup_window)
        return;
    auto dpi = mylar->raw_window->dpi;
    int w = (int) std::max(240.0, field->real_bounds.w / dpi);
    font_completion_settings = make_icon_anchored_popup_settings(field, dpi, w, font_completion_popup_h(shown.size()));
    font_completion_settings.popup.offset_y = 4;
    auto popup = open_mylar_popup(mylar, font_completion_settings);
    if (!popup)
        return;
    dropdown_popup = popup;
    popups.push_back(popup);
    popup->root->skip_delete = true;
    popup->root->user_data = popup;
    popup->root->wanted_bounds.w = FILL_SPACE;
    popup->root->wanted_bounds.h = FILL_SPACE;
    popup->root->on_closed = [popup](Container *) {
        if (dropdown_popup == popup)
            dropdown_popup = nullptr;
        if (font_completion_popup == popup) {
            font_completion_popup = nullptr;
            font_completion_list = nullptr;
        }
        std::erase(popups, popup);
    };
    popup->root->when_paint = [](Container *root, Container *c) {
        auto window = ((MylarWindow *) root->user_data)->raw_window;
        paint_smooth_frame(window->drawing_context, c->real_bounds, 8 * window->dpi, theme.popup_background, theme.dropdown_border);
    };
    // Keys normally stay with the settings window; if the compositor hands them to the
    // popup instead, pass them on to the field so typing continues.
    popup->root->when_key_event = [field](Container *, Container *, int key, bool pressed, xkb_keysym_t sym, int mods, bool is_text,
                                          std::string text) {
        if (!settings_mylar || !field->when_key_event)
            return;
        field->when_key_event(settings_mylar->root, field, key, pressed, sym, mods, is_text, text);
        windowing::redraw(settings_mylar->raw_window);
    };

    auto data = new FontCompletionList;
    data->shown = std::move(shown);
    font_completion_popup = popup;
    font_completion_list = data;

    auto body = popup->root->child(::vbox, FILL_SPACE, FILL_SPACE);
    body->pre_layout = [](Container *root, Container *c, const Bounds &) {
        auto dpi = swifi_window(root)->dpi;
        c->wanted_pad = Bounds(4 * dpi, 4 * dpi, 4 * dpi, 4 * dpi);
    };

    auto list = body->child(::fullycustom, FILL_SPACE, FILL_SPACE);
    list->user_data = data;
    list->receive_events_even_if_obstructed = true;
    list->pre_layout = [](Container *root, Container *c, const Bounds &b) {
        auto data = (FontCompletionList *) c->user_data;
        auto dpi = swifi_window(root)->dpi;
        double content = data->shown.size() * font_completion_row_h * dpi;
        data->view_h = (float) b.h;
        data->overflow = (float) std::max(0.0, std::floor(content - b.h));
        if (data->smooth.step(data->scroll))
            swifi_redraw(root);
        data->scroll = std::clamp(data->scroll, -data->overflow, 0.0f);
    };
    list->when_fine_scrolled = [](Container *root, Container *c, double, double scroll_y, bool touchpad) {
        auto data = (FontCompletionList *) c->user_data;
        float delta = (float) (scroll_y * 3 * swifi_window(root)->dpi);
        if (touchpad) {
            data->smooth.stop();
            data->scroll = std::clamp(data->scroll + delta, -data->overflow, 0.0f);
        } else {
            data->smooth.scroll_by(data->scroll, delta, -data->overflow, 0.0f);
        }
        swifi_redraw(root);
    };
    // Rows are painted directly and hit-tested by position; only visible ones are drawn,
    // so at most font_completion_max_rows + 1 fonts are shaped per frame.
    list->when_paint = [](Container *root, Container *c) {
        auto data = (FontCompletionList *) c->user_data;
        auto window = swifi_window(root);
        auto cr = window->drawing_context;
        auto dpi = window->dpi;
        auto b = c->real_bounds;
        cr->save();
        cr->rectangle(b.x, b.y, b.w, b.h);
        cr->clip_path();
        double row_h = font_completion_row_h * dpi;
        int first = std::max(0, (int) std::floor(-data->scroll / row_h));
        for (int i = first; i < (int) data->shown.size(); i++) {
            double y = b.y + data->scroll + i * row_h;
            if (y > b.y + b.h)
                break;
            Bounds rb(b.x, y, b.w - list_bar_hit_w * dpi, row_h);
            bool hovered = c->state.mouse_hovering && !list_bar_blocks_hover(data->bar) && root->mouse_current_y >= y &&
                root->mouse_current_y < y + row_h;
            if (hovered || i == data->highlighted) {
                set_argb(cr, hovered && c->state.mouse_pressing ? theme.popup_pressed : theme.popup_hover);
                drawRoundedRect(cr, rb.x, rb.y, rb.w, rb.h, 6 * dpi, 1.0);
                cr->fill();
            }
            if (font_completion_lower(data->shown[i]) == font_completion_lower(set->font)) {
                set_argb(cr, theme.accent);
                drawRoundedRect(cr, rb.x + 4 * dpi, rb.y + rb.h * .3, 3 * dpi, rb.h * .4, 1.5 * dpi, 1.0);
                cr->fill();
            }
            Bounds tb(rb.x + 16 * dpi, rb.y, rb.w - 24 * dpi, rb.h);
            if (set->font_suggestions_in_own_font)
                swifi_text_in_family(root, tb, data->shown[i], data->shown[i], 11, theme.text);
            else
                swifi_text(root, tb, data->shown[i], 11, theme.text);
        }
        cr->restore();
        list_bar_paint(root, c, data->scroll, data->overflow, data->bar);
    };
    list->when_clicked = [field, on_pick](Container *root, Container *c) {
        auto data = (FontCompletionList *) c->user_data;
        auto dpi = swifi_window(root)->dpi;
        int i = (int) std::floor((root->mouse_current_y - c->real_bounds.y - data->scroll) / (font_completion_row_h * dpi));
        if (i >= 0 && i < (int) data->shown.size())
            pick_font_completion(field, data->shown[i], on_pick);
    };
    list_bar_install<FontCompletionList>(list);
    windowing::redraw(popup->raw_window);
}

// Shows, refreshes or hides the popup for the field's current text.
static void update_font_completions(Container *root, Container *field, const std::function<void(std::string)> &on_pick) {
    auto text = static_cast<STextEditorData *>(field->user_data)->text;
    auto shown = font_completions(text);
    if (shown.empty() || (shown.size() == 1 && font_completion_lower(shown[0]) == font_completion_lower(text))) {
        close_font_completions();
        return;
    }
    if (!font_completions_open()) {
        open_font_completions(root, field, std::move(shown), on_pick);
        return;
    }
    auto data = font_completion_list;
    int old_h = font_completion_popup_h(data->shown.size());
    data->shown = std::move(shown);
    data->scroll = 0;
    data->highlighted = -1;
    data->smooth.stop();
    int h = font_completion_popup_h(data->shown.size());
    if (h != old_h) {
        font_completion_settings.pos.h = h;
        windowing::set_popup_size(font_completion_popup->raw_window, font_completion_settings.pos.w, h, font_completion_settings);
    }
    windowing::redraw(font_completion_popup->raw_window);
}

static void attach_font_completions(Container *field, std::function<void(std::string)> on_pick) {
    auto edit_key = std::move(field->when_key_event);
    field->when_key_event = [edit_key, on_pick](Container *root, Container *c, int key, bool pressed, xkb_keysym_t sym, int mods,
                                                bool is_text, std::string text) {
        auto data = font_completion_list;
        if (pressed && c->active && font_completions_open()) {
            if (sym == XKB_KEY_Escape) {
                close_font_completions();
                return;
            }
            if (sym == XKB_KEY_Down || sym == XKB_KEY_Up) {
                int count = (int) data->shown.size();
                if (data->highlighted < 0)
                    data->highlighted = sym == XKB_KEY_Down ? 0 : count - 1;
                else
                    data->highlighted = (data->highlighted + (sym == XKB_KEY_Down ? 1 : -1) + count) % count;
                reveal_font_completion(data);
                windowing::redraw(font_completion_popup->raw_window);
                return;
            }
            if ((sym == XKB_KEY_Return || sym == XKB_KEY_KP_Enter) && data->highlighted >= 0 &&
                data->highlighted < (int) data->shown.size()) {
                pick_font_completion(c, data->shown[data->highlighted], on_pick);
                return;
            }
        }
        auto editor = static_cast<STextEditorData *>(c->user_data);
        auto before = editor->text;
        if (edit_key)
            edit_key(root, c, key, pressed, sym, mods, is_text, text);
        if (editor->text != before && c->active)
            update_font_completions(root, c, on_pick);
    };
    // A plain function pointer, so the editor's own handler (the same for every text field)
    // is kept in a static rather than captured.
    static void (*editor_active_changed)(Container *, Container *) = nullptr;
    editor_active_changed = field->when_active_status_changed;
    field->when_active_status_changed = [](Container *root, Container *c) {
        if (editor_active_changed)
            editor_active_changed(root, c);
        if (!c->active)
            close_font_completions();
    };
}

struct SdtZoneList : UserData {
    std::vector<SdtZoneRow> all;
    std::vector<const SdtZoneRow *> shown;
    std::string current;
    float scroll = 0;
    float overflow = 0;
    bool scrolled_to_current = false;
    int key_index = -1; // Row picked with the arrow keys, -1 for none
    ListScrollbar bar;
    SmoothScroll smooth;
};

static constexpr int sdt_zone_row_h = 40;

static std::string sdt_lower(std::string s) {
    for (auto &ch : s)
        ch = (char) std::tolower((unsigned char) ch);
    return s;
}

static void sdt_filter_zones(SdtZoneList *list, const std::string &query) {
    auto q = sdt_lower(query);
    list->shown.clear();
    for (auto &row : list->all) {
        if (q.empty() || sdt_lower(row.entry->city).find(q) != std::string::npos ||
            sdt_lower(row.entry->country).find(q) != std::string::npos ||
            sdt_lower(row.entry->id).find(q) != std::string::npos || sdt_lower(row.abbreviation) == q)
            list->shown.push_back(&row);
    }
    list->scroll = 0;
    list->key_index = -1;
}

static void sdt_pick_zone(const std::string &id) {
    close_dropdown_menu();
    sdt_run_async([id]() { return datetime::set_timezone(id); });
}

// GNOME's time zone dialog: a search field over the tz database's locations.
static void sdt_open_timezone_dialog(Container *root, Container *anchor) {
    auto popup = sdt_open_popup(root, anchor, 400, 440);
    if (!popup)
        return;
    auto data = new SdtZoneList;
    data->current = sdt_snapshot().timezone;
    for (auto &entry : datetime::timezones())
        data->all.push_back({&entry, datetime::timezone_abbreviation(entry.id)});
    sdt_filter_zones(data, "");

    auto body = popup->root->child(::vbox, FILL_SPACE, FILL_SPACE);
    body->pre_layout = [](Container *root, Container *c, const Bounds &) {
        auto dpi = swifi_window(root)->dpi;
        c->wanted_pad = Bounds(10 * dpi, 10 * dpi, 10 * dpi, 6 * dpi);
        c->spacing = 8 * dpi;
    };

    auto list = body->child(::fullycustom, FILL_SPACE, FILL_SPACE);
    list->user_data = data;
    list->receive_events_even_if_obstructed = true;
    list->pre_layout = [](Container *root, Container *c, const Bounds &b) {
        auto data = (SdtZoneList *) c->user_data;
        auto dpi = swifi_window(root)->dpi;
        double content = data->shown.size() * sdt_zone_row_h * dpi;
        data->overflow = (float) std::max(0.0, content - b.h);
        if (!data->scrolled_to_current && b.h > 1) {
            data->scrolled_to_current = true;
            for (size_t i = 0; i < data->shown.size(); i++)
                if (data->shown[i]->entry->id == data->current)
                    data->scroll = -(float) ((i + .5) * sdt_zone_row_h * dpi - b.h * .5);
        }
        if (data->smooth.step(data->scroll))
            swifi_redraw(root);
        data->scroll = std::clamp(data->scroll, -data->overflow, 0.0f);
    };
    list->when_fine_scrolled = [](Container *root, Container *c, double, double scroll_y, bool touchpad) {
        auto data = (SdtZoneList *) c->user_data;
        auto dpi = swifi_window(root)->dpi;
        float delta = (float) (scroll_y * 3 * dpi);
        if (touchpad) {
            data->smooth.stop();
            data->scroll = std::clamp(data->scroll + delta, -data->overflow, 0.0f);
        } else {
            data->smooth.scroll_by(data->scroll, delta, -data->overflow, 0.0f);
        }
        swifi_redraw(root);
    };
    // Rows are painted directly (hundreds of zones), hit-testing by position.
    list->when_paint = [](Container *root, Container *c) {
        auto data = (SdtZoneList *) c->user_data;
        auto window = swifi_window(root);
        auto cr = window->drawing_context;
        auto dpi = window->dpi;
        auto b = c->real_bounds;
        cr->save();
        cr->rectangle(b.x, b.y, b.w, b.h);
        cr->clip_path();
        double row_h = sdt_zone_row_h * dpi;
        int first = std::max(0, (int) std::floor(-data->scroll / row_h));
        for (int i = first; i < (int) data->shown.size(); i++) {
            double y = b.y + data->scroll + i * row_h;
            if (y > b.y + b.h)
                break;
            auto row = data->shown[i];
            Bounds rb(b.x, y, b.w - list_bar_hit_w * dpi, row_h);
            bool hovered = (c->state.mouse_hovering && !list_bar_blocks_hover(data->bar) &&
                root->mouse_current_y >= y && root->mouse_current_y < y + row_h) || data->key_index == i;
            if (hovered) {
                set_argb(cr, c->state.mouse_pressing ? theme.popup_pressed : theme.popup_hover);
                drawRoundedRect(cr, rb.x, rb.y, rb.w, rb.h, 6 * dpi, 1.0);
                cr->fill();
            }
            bool current = row->entry->id == data->current;
            if (current) {
                set_argb(cr, theme.accent);
                drawRoundedRect(cr, rb.x + 4 * dpi, rb.y + rb.h * .3, 3 * dpi, rb.h * .4, 1.5 * dpi, 1.0);
                cr->fill();
            }
            double abbrev_w = swifi_text_width(root, row->abbreviation, 10) + 8 * dpi;
            swifi_text(root, Bounds(rb.x + 16 * dpi, rb.y + 3 * dpi, rb.w - 32 * dpi - abbrev_w, rb.h * .5),
                row->entry->city, 11, theme.text, false, current);
            swifi_text(root, Bounds(rb.x + 16 * dpi, rb.y + rb.h * .5 - 2 * dpi, rb.w - 32 * dpi - abbrev_w, rb.h * .5),
                row->entry->country, 9, theme.text_secondary);
            swifi_text(root, Bounds(rb.x + rb.w - abbrev_w - 10 * dpi, rb.y, abbrev_w, rb.h),
                row->abbreviation, 10, theme.text_secondary);
        }
        if (data->shown.empty())
            swifi_text(root, b, "No matching time zones", 11, theme.text_secondary, true);
        cr->restore();
        list_bar_paint(root, c, data->scroll, data->overflow, data->bar);
    };
    list->when_clicked = [](Container *root, Container *c) {
        auto data = (SdtZoneList *) c->user_data;
        auto dpi = swifi_window(root)->dpi;
        int i = (int) std::floor((root->mouse_current_y - c->real_bounds.y - data->scroll) / (sdt_zone_row_h * dpi));
        if (i >= 0 && i < (int) data->shown.size())
            sdt_pick_zone(data->shown[i]->entry->id);
    };
    list_bar_install<SdtZoneList>(list);
    list_keys_install<SdtZoneList>(list, sdt_zone_row_h, [](Container *, SdtZoneList *data, int i) {
        sdt_pick_zone(data->shown[i]->entry->id);
    });

    // The search field goes above the list.
    auto field = make_field(body, false, "", [data](std::string text) {
        sdt_filter_zones(data, text);
    });
    std::rotate(body->children.begin(), body->children.end() - 1, body->children.end());
    auto edit_key = std::move(field->when_key_event);
    field->when_key_event = [edit_key, data, list](Container *root, Container *c, int key, bool pressed, xkb_keysym_t sym,
                                            int mods, bool is_text, std::string text) {
        if (pressed && sym == XKB_KEY_Escape) {
            close_dropdown_menu();
            return;
        }
        if (pressed && (sym == XKB_KEY_Return || sym == XKB_KEY_KP_Enter)) {
            if (!data->shown.empty())
                sdt_pick_zone(data->shown[0]->entry->id);
            return;
        }
        // Down moves into the list.
        if (pressed && c->active && sym == XKB_KEY_Down && !data->shown.empty()) {
            data->key_index = list_first_visible(root, data, sdt_zone_row_h);
            focus_set(root, list, true);
            return;
        }
        if (edit_key)
            edit_key(root, c, key, pressed, sym, mods, is_text, text);
    };
    focus_dialog_start(popup->root, field);
    windowing::redraw(popup->raw_window);
}

static void fill_datetime_settings(Container *root) {
    auto right = container_by_name("settings_right", root);
    if (!right)
        return;
    for (auto child: right->children)
        delete child;
    right->children.clear();

    right->pre_layout = layout_right_page;
    auto padded_right = right->child(FILL_SPACE, FILL_SPACE);

    make_section_title(padded_right, "Time & Date");
    make_vert_space(padded_right, 10);

    auto loaded = []() {
        bool l = false, busy = false;
        sdt_snapshot(&l, &busy);
        return l && !busy;
    };

    // Automatic Date & Time + manual Date & Time.
    sdt_row(padded_right, {
        .height = 60,
        .title = []() { return std::string("Automatic Date & Time"); },
        .subtitle = []() {
            bool l = false;
            auto s = sdt_snapshot(&l);
            if (l && !s.can_ntp)
                return std::string("Install chrony or ntpd to set the time from the internet");
            return std::string("Requires internet access");
        },
        .enabled = [loaded]() { return loaded() && sdt_snapshot().can_ntp; },
        .accessory = []() { return 3; },
        .switch_on = []() { return sdt_snapshot().ntp; },
        .on_click = [](Container *, Container *) {
            bool enable = !sdt_snapshot().ntp;
            sdt_run_async([enable]() { return datetime::set_ntp(enable); });
        },
    }, true, false);
    sdt_row(padded_right, {
        .title = []() { return std::string("Date & Time"); },
        .value = []() { return datetime::long_datetime_text(); },
        .enabled = [loaded]() { return loaded() && !sdt_snapshot().ntp; },
        .accessory = []() { return 1; },
        .on_click = sdt_open_datetime_dialog,
    }, false, true);

    make_vert_space(padded_right, 20);

    // Automatic Time Zone needs location services (geoclue), which Mylar doesn't provide; like
    // gnome-control-center built without location services, the switch stays off.
    sdt_row(padded_right, {
        .height = 60,
        .title = []() { return std::string("Automatic Time Zone"); },
        .subtitle = []() { return std::string("Requires location services enabled and internet access"); },
        .enabled = []() { return false; },
        .accessory = []() { return 3; },
        .switch_on = []() { return false; },
    }, true, false);
    sdt_row(padded_right, {
        .title = []() { return std::string("Time Zone"); },
        .value = []() {
            bool l = false;
            auto s = sdt_snapshot(&l);
            return l ? datetime::timezone_label(s.timezone) : std::string("…");
        },
        .enabled = loaded,
        .accessory = []() { return 1; },
        .on_click = sdt_open_timezone_dialog,
    }, false, true);

    make_vert_space(padded_right, 20);

    sdt_row(padded_right, {
        .title = []() { return std::string("Time Format"); },
        .value = []() { return std::string(set->clock_24h ? "24-hour" : "AM / PM"); },
        .accessory = []() { return 2; },
        .on_click = [](Container *root, Container *anchor) {
            open_dropdown_menu(root, anchor, {{"24h", "24-hour"}, {"12h", "AM / PM"}},
                set->clock_24h ? "24h" : "12h", [](std::string picked) {
                    bool value = picked == "24h";
                    set->clock_24h = value;
                    main_thread([value]() {
                        set->clock_24h = value;
                        settings::load_save_settings(true, set);
                        dock::redraw();
                    });
                });
        },
    }, true, true);

    unsigned generation;
    {
        std::lock_guard lock(sdt.mutex);
        generation = ++sdt.generation;
    }
    sdt_refresh_async();
    sdt_watch(generation);
}

/*************************************************
 *
 * Battery page (the charge history moved here from the dock's battery menu)
 *
 *************************************************/

static void fill_battery_settings(Container *root) {
    auto right = container_by_name("settings_right", root);
    if (!right)
        return;
    for (auto child: right->children)
        delete child;
    right->children.clear();

    right->pre_layout = layout_right_page;
    auto padded_right = right->child(FILL_SPACE, FILL_SPACE);
    padded_right->name = "battery_page";

    // Readings come from the dock's battery thread; start it in case no dock is running.
    main_thread([]() {
        watch_battery_level();
        request_battery_refresh();
    });

    make_section_title(padded_right, "Status");
    make_vert_space(padded_right, 10);

    auto summary = padded_right->child(FILL_SPACE, FILL_SPACE);
    search_tag(summary, "Battery level", "Charge, charging state and time remaining");
    summary->pre_layout = [](Container *root, Container *c, const Bounds &) {
        c->wanted_bounds.h = 104 * swifi_window(root)->dpi;
    };
    summary->when_paint = [](Container *root, Container *c) {
        auto window = swifi_window(root);
        auto cr = window->drawing_context;
        auto dpi = window->dpi;
        auto b = c->real_bounds;
        paint_smooth_frame(cr, b, 5 * dpi, theme.row_background, theme.row_border, std::max(1.0, (double) std::round(1 * dpi)));
        auto s = battery_summary();
        double percent_w = std::max(swifi_text_width(root, "100%", 26), swifi_text_width(root, s.percent, 26));
        swifi_text(root, Bounds(b.x + 20 * dpi, b.y, percent_w, b.h), s.percent, 26, theme.text, false, true);
        double x = b.x + 20 * dpi + percent_w + 24 * dpi;
        double w = std::max(1.0, b.right() - 20 * dpi - x);
        swifi_text(root, Bounds(x, b.y + 18 * dpi, w, 20 * dpi), s.state, 12, theme.text, false, true);
        swifi_text(root, Bounds(x, b.y + 42 * dpi, w, 20 * dpi), s.estimate, 11, theme.text);
        if (!s.detail.empty())
            swifi_text(root, Bounds(x, b.y + 64 * dpi, w, 20 * dpi), s.detail, 10, theme.text_secondary);
    };
    make_vert_space(padded_right, 28);

    make_section_title(padded_right, "Battery usage");
    make_vert_space(padded_right, 10);

    auto graph = padded_right->child(FILL_SPACE, FILL_SPACE);
    search_tag(graph, "Battery history", "Charge over the last 3 hours, with an estimate of what is left");
    // The page container is only viewport tall and moves with the scroll.
    graph->parent_bounds_limit_input_bounds = false;
    graph->pre_layout = [](Container *root, Container *c, const Bounds &) {
        c->wanted_bounds.h = 300 * swifi_window(root)->dpi;
    };
    graph->when_paint = [](Container *root, Container *c) {
        auto window = swifi_window(root);
        auto cr = window->drawing_context;
        auto dpi = window->dpi;
        auto b = c->real_bounds;
        paint_smooth_frame(cr, b, 5 * dpi, theme.row_background, theme.row_border, std::max(1.0, (double) std::round(1 * dpi)));
        auto s = battery_summary();
        Bounds line(b.x + 20 * dpi, b.y + 14 * dpi, b.w - 40 * dpi, 20 * dpi);
        swifi_text(root, line, "Last 3 hours · charge %", 11, theme.text_secondary);
        if (!s.rate.empty()) {
            double rate_w = swifi_text_width(root, s.rate, 10);
            swifi_text(root, Bounds(line.right() - rate_w, line.y, rate_w, line.h), s.rate, 10, theme.accent);
        }
        BatteryGraphPalette palette;
        palette.text = theme.text;
        palette.muted = theme.text_secondary;
        palette.grid = swifi_mix(theme.row_background, theme.text, .1);
        palette.now_line = swifi_mix(theme.row_background, theme.text, .3);
        paint_battery_history(cr, Bounds(b.x + 20 * dpi, b.y + 46 * dpi, b.w - 40 * dpi, 210 * dpi), dpi, root->mouse_current_x,
                              c->state.mouse_hovering, palette);
        swifi_text(root, Bounds(b.x + 20 * dpi, b.bottom() - 36 * dpi, b.w - 40 * dpi, 20 * dpi),
                   "Solid: recorded charge   ·   Dashed: estimate", 9, theme.text_secondary);
    };
    graph->when_mouse_motion = [](Container *root, Container *) { swifi_redraw(root); };
    graph->when_mouse_enters_container = [](Container *root, Container *) { swifi_redraw(root); };
    graph->when_mouse_leaves_container = [](Container *root, Container *) { swifi_redraw(root); };
    make_vert_space(padded_right, 4);

    make_bool(padded_right, "Record battery history", "Saves the charge every 5 minutes and keeps the last 24 hours",
              battery_history_enabled(), [](bool on) { set_battery_history_enabled(on); }, "\uE81C");

    auto message = padded_right->child(FILL_SPACE, FILL_SPACE);
    message->pre_layout = [](Container *root, Container *c, const Bounds &) {
        c->wanted_bounds.h = 30 * swifi_window(root)->dpi;
    };
    message->when_paint = [](Container *root, Container *c) {
        auto s = battery_summary();
        if (s.message.empty())
            return;
        RGBA color = s.message_is_error ? RGBA(.85, .30, .22, 1) : theme.text_secondary;
        swifi_text(root, c->real_bounds, s.message, 10, color);
    };

    // New readings arrive every 30 seconds; repaint while the page shows.
    unsigned generation;
    {
        std::lock_guard lock(sdt.mutex);
        generation = ++sdt.generation;
    }
    sdt_watch(generation);
}

/*************************************************
 *
 * Keyboard page
 *
 *************************************************/

struct SkbdLayoutEntry {
    std::string layout;
    std::string variant;
    std::string description;
    std::string search; // Lower case text the filter matches against
};

// Every layout and variant xkb knows about, from its rules listing.
static const std::vector<SkbdLayoutEntry> &skbd_all_layouts() {
    static std::vector<SkbdLayoutEntry> list;
    static bool loaded = false;
    if (loaded)
        return list;
    loaded = true;
    const char *paths[] = {"/usr/share/X11/xkb/rules/evdev.lst", "/usr/share/X11/xkb/rules/base.lst",
                           "/usr/local/share/X11/xkb/rules/evdev.lst"};
    for (auto path : paths) {
        std::ifstream in(path);
        if (!in)
            continue;
        std::string line, section;
        while (std::getline(in, line)) {
            if (line.starts_with("!")) {
                section = trim(line.substr(1));
                continue;
            }
            if (section != "layout" && section != "variant")
                continue;
            std::istringstream entry(line);
            std::string name;
            if (!(entry >> name))
                continue;
            std::string rest;
            std::getline(entry, rest);
            rest = trim(rest);
            SkbdLayoutEntry e;
            if (section == "layout") {
                e.layout = name;
                e.description = rest;
            } else {
                // "  variant   layout: Description"
                auto colon = rest.find(':');
                if (colon == std::string::npos)
                    continue;
                e.layout = trim(rest.substr(0, colon));
                e.variant = name;
                e.description = trim(rest.substr(colon + 1));
            }
            e.search = sdt_lower(e.description + " " + e.layout + " " + e.variant);
            list.push_back(e);
        }
        if (!list.empty())
            break;
    }
    // Layouts first, each followed by its variants, sorted by name.
    std::stable_sort(list.begin(), list.end(), [](const SkbdLayoutEntry &a, const SkbdLayoutEntry &b) {
        return sdt_lower(a.description) < sdt_lower(b.description);
    });
    return list;
}

static std::string skbd_id(const SKeyboardLayout &layout) {
    return layout.variant.empty() ? layout.layout : layout.layout + " (" + layout.variant + ")";
}

static std::string skbd_description(const SKeyboardLayout &layout) {
    for (const auto &entry : skbd_all_layouts())
        if (entry.layout == layout.layout && entry.variant == layout.variant)
            return entry.description;
    return skbd_id(layout);
}

// Only touched on the settings thread.
static std::vector<SKeyboardLayout> skbd_layouts;
static bool skbd_page_stale = false;

static void skbd_commit() {
    auto layouts = skbd_layouts;
    main_thread([layouts]() {
        set->keyboard_layouts = layouts;
        settings::load_save_settings(true, set);
        hypriso->generate_mylar_hyprland_config();
    });
    skbd_page_stale = true;
    if (settings_mylar)
        windowing::redraw(settings_mylar->raw_window);
}

static bool skbd_contains(const std::string &layout, const std::string &variant) {
    for (const auto &l : skbd_layouts)
        if (l.layout == layout && l.variant == variant)
            return true;
    return false;
}

static void skbd_add(const SkbdLayoutEntry &entry) {
    close_dropdown_menu();
    if (skbd_contains(entry.layout, entry.variant))
        return;
    // Without an explicit list Hyprland uses US English, keep it as the first layout.
    if (skbd_layouts.empty() && !(entry.layout == "us" && entry.variant.empty()))
        skbd_layouts.push_back({"us", ""});
    skbd_layouts.push_back({entry.layout, entry.variant});
    skbd_commit();
}

struct SkbdLayoutList : UserData {
    std::vector<const SkbdLayoutEntry *> shown;
    float scroll = 0;
    float overflow = 0;
    int key_index = -1; // Row picked with the arrow keys, -1 for none
    ListScrollbar bar;
    SmoothScroll smooth;
};

static constexpr int skbd_row_h = 40;

static void skbd_filter(SkbdLayoutList *list, const std::string &query) {
    // Every word has to match somewhere, so "german mac" finds "German (Macintosh)".
    std::vector<std::string> words;
    std::istringstream stream(sdt_lower(query));
    std::string word;
    while (stream >> word)
        words.push_back(word);
    list->shown.clear();
    for (const auto &entry : skbd_all_layouts()) {
        bool match = true;
        for (const auto &w : words)
            if (entry.search.find(w) == std::string::npos)
                match = false;
        if (match)
            list->shown.push_back(&entry);
    }
    list->scroll = 0;
    list->key_index = -1;
}

static void skbd_open_layout_dialog(Container *root, Container *anchor) {
    auto popup = sdt_open_popup(root, anchor, 440, 460);
    if (!popup)
        return;
    auto data = new SkbdLayoutList;
    skbd_filter(data, "");

    auto body = popup->root->child(::vbox, FILL_SPACE, FILL_SPACE);
    body->pre_layout = [](Container *root, Container *c, const Bounds &) {
        auto dpi = swifi_window(root)->dpi;
        c->wanted_pad = Bounds(10 * dpi, 10 * dpi, 10 * dpi, 6 * dpi);
        c->spacing = 8 * dpi;
    };

    auto list = body->child(::fullycustom, FILL_SPACE, FILL_SPACE);
    list->user_data = data;
    list->receive_events_even_if_obstructed = true;
    list->pre_layout = [](Container *root, Container *c, const Bounds &b) {
        auto data = (SkbdLayoutList *) c->user_data;
        auto dpi = swifi_window(root)->dpi;
        double content = data->shown.size() * skbd_row_h * dpi;
        data->overflow = (float) std::max(0.0, content - b.h);
        if (data->smooth.step(data->scroll))
            swifi_redraw(root);
        data->scroll = std::clamp(data->scroll, -data->overflow, 0.0f);
    };
    list->when_fine_scrolled = [](Container *root, Container *c, double, double scroll_y, bool touchpad) {
        auto data = (SkbdLayoutList *) c->user_data;
        auto dpi = swifi_window(root)->dpi;
        float delta = (float) (scroll_y * 3 * dpi);
        if (touchpad) {
            data->smooth.stop();
            data->scroll = std::clamp(data->scroll + delta, -data->overflow, 0.0f);
        } else {
            data->smooth.scroll_by(data->scroll, delta, -data->overflow, 0.0f);
        }
        swifi_redraw(root);
    };
    // Rows are painted directly (hundreds of layouts), hit-testing by position.
    list->when_paint = [](Container *root, Container *c) {
        auto data = (SkbdLayoutList *) c->user_data;
        auto window = swifi_window(root);
        auto cr = window->drawing_context;
        auto dpi = window->dpi;
        auto b = c->real_bounds;
        cr->save();
        cr->rectangle(b.x, b.y, b.w, b.h);
        cr->clip_path();
        double row_h = skbd_row_h * dpi;
        int first = std::max(0, (int) std::floor(-data->scroll / row_h));
        for (int i = first; i < (int) data->shown.size(); i++) {
            double y = b.y + data->scroll + i * row_h;
            if (y > b.y + b.h)
                break;
            auto entry = data->shown[i];
            Bounds rb(b.x, y, b.w - list_bar_hit_w * dpi, row_h);
            const bool added = skbd_contains(entry->layout, entry->variant);
            bool hovered = (c->state.mouse_hovering && !list_bar_blocks_hover(data->bar) &&
                root->mouse_current_y >= y && root->mouse_current_y < y + row_h) || data->key_index == i;
            if (hovered && !added) {
                set_argb(cr, c->state.mouse_pressing ? theme.popup_pressed : theme.popup_hover);
                drawRoundedRect(cr, rb.x, rb.y, rb.w, rb.h, 6 * dpi, 1.0);
                cr->fill();
            }
            if (added) {
                set_argb(cr, theme.accent);
                drawRoundedRect(cr, rb.x + 4 * dpi, rb.y + rb.h * .3, 3 * dpi, rb.h * .4, 1.5 * dpi, 1.0);
                cr->fill();
            }
            std::string id = entry->variant.empty() ? entry->layout : entry->layout + "(" + entry->variant + ")";
            std::string right = added ? "Added" : id;
            double right_w = swifi_text_width(root, right, 10) + 8 * dpi;
            swifi_text(root, Bounds(rb.x + 16 * dpi, rb.y, rb.w - 32 * dpi - right_w, rb.h),
                entry->description, 11, added ? theme.text_secondary : theme.text, false, entry->variant.empty());
            swifi_text(root, Bounds(rb.x + rb.w - right_w - 10 * dpi, rb.y, right_w, rb.h), right, 10, theme.text_secondary);
        }
        if (data->shown.empty())
            swifi_text(root, b, "No matching layouts", 11, theme.text_secondary, true);
        cr->restore();
        list_bar_paint(root, c, data->scroll, data->overflow, data->bar);
    };
    list->when_clicked = [](Container *root, Container *c) {
        auto data = (SkbdLayoutList *) c->user_data;
        auto dpi = swifi_window(root)->dpi;
        int i = (int) std::floor((root->mouse_current_y - c->real_bounds.y - data->scroll) / (skbd_row_h * dpi));
        if (i >= 0 && i < (int) data->shown.size())
            skbd_add(*data->shown[i]);
    };
    list_bar_install<SkbdLayoutList>(list);
    list_keys_install<SkbdLayoutList>(list, skbd_row_h, [](Container *, SkbdLayoutList *data, int i) {
        skbd_add(*data->shown[i]);
    });

    // The search field goes above the list.
    auto field = make_field(body, false, "", [data](std::string text) {
        skbd_filter(data, text);
    });
    std::rotate(body->children.begin(), body->children.end() - 1, body->children.end());
    auto paint_field = std::move(field->when_paint);
    field->when_paint = [paint_field](Container *root, Container *c) {
        if (paint_field)
            paint_field(root, c);
        auto text = static_cast<STextEditorData *>(c->user_data);
        if (text && !text->text.empty())
            return;
        auto dpi = swifi_window(root)->dpi;
        auto b = c->real_bounds;
        b.x += 11 * dpi;
        b.w = std::max(0.0, b.w - 22 * dpi);
        swifi_text(root, b, "Search layouts by language, country or name", 11, swifi_dim(theme.text_secondary));
    };
    auto edit_key = std::move(field->when_key_event);
    field->when_key_event = [edit_key, data, list](Container *root, Container *c, int key, bool pressed, xkb_keysym_t sym,
                                            int mods, bool is_text, std::string text) {
        if (pressed && sym == XKB_KEY_Escape) {
            close_dropdown_menu();
            return;
        }
        if (pressed && (sym == XKB_KEY_Return || sym == XKB_KEY_KP_Enter)) {
            for (auto entry : data->shown) {
                if (!skbd_contains(entry->layout, entry->variant)) {
                    skbd_add(*entry);
                    break;
                }
            }
            return;
        }
        // Down moves into the list.
        if (pressed && c->active && sym == XKB_KEY_Down && !data->shown.empty()) {
            data->key_index = list_first_visible(root, data, skbd_row_h);
            focus_set(root, list, true);
            return;
        }
        if (edit_key)
            edit_key(root, c, key, pressed, sym, mods, is_text, text);
    };
    focus_dialog_start(popup->root, field);
    windowing::redraw(popup->raw_window);
}

// Index of the layout Hyprland is using, polled while the Keyboard page shows.
static int skbd_active = -1;
static unsigned skbd_watch_generation = 0;

static void skbd_watch(unsigned generation) {
    if (!settings_app)
        return;
    windowing::timer(settings_app, 400, [generation](void *) {
        if (generation != skbd_watch_generation || !settings_mylar)
            return;
        if (!container_by_name("keyboard_page", settings_mylar->root))
            return;
        main_thread([generation]() {
            int index = hypriso->active_keyboard_layout_index();
            auto app = settings_app;
            if (!app)
                return;
            windowing::run_on_app(app, [generation, index]() {
                if (generation != skbd_watch_generation)
                    return;
                if (index != skbd_active) {
                    skbd_active = index;
                    if (settings_mylar)
                        windowing::redraw(settings_mylar->raw_window);
                }
                skbd_watch(generation);
            });
        });
    }, nullptr);
}

static void skbd_watch_start() {
    const unsigned generation = ++skbd_watch_generation;
    main_thread([generation]() {
        int index = hypriso->active_keyboard_layout_index();
        auto app = settings_app;
        if (!app)
            return;
        windowing::run_on_app(app, [generation, index]() {
            if (generation != skbd_watch_generation)
                return;
            skbd_active = index;
            if (settings_mylar)
                windowing::redraw(settings_mylar->raw_window);
            skbd_watch(generation);
        });
    });
}

// One layout: its name and id, then Make default and Remove. `implicit` is the
// US layout Hyprland falls back to when none were added, shown without buttons.
static void skbd_layout_row(Container *parent, int index, SKeyboardLayout layout, bool implicit, bool is_first, bool is_last) {
    static const float height = 64;
    static const float round = 8;
    auto row = parent->child(::hbox, FILL_SPACE, height);
    row->parent_bounds_limit_input_bounds = false;
    row->pre_layout = [](Container *root, Container *c, const Bounds &) {
        double dpi = swifi_window(root)->dpi;
        c->wanted_bounds.h = height * dpi;
        c->wanted_pad = Bounds(20 * dpi, 15 * dpi, 14 * dpi, 15 * dpi);
        c->spacing = 10 * dpi;
    };
    row->when_paint = [index, is_first, is_last](Container *root, Container *c) {
        auto window = swifi_window(root);
        auto cr = window->drawing_context;
        auto dpi = window->dpi;
        const bool active = index == skbd_active;
        auto b = c->real_bounds;
        const auto overlap = (std::floor(1.0 * dpi) + 1.0) * 0.5;
        b.y -= overlap;
        b.h += 2.0 * overlap;
        drawRoundedRect(cr, b.x, b.y, b.w, b.h, round * dpi, 0, is_first, is_first, is_last, is_last);
        set_argb(cr, active ? swifi_mix(theme.row_background, theme.accent, .16) : theme.row_background);
        cr->fill();
        drawRoundedRect(cr, b.x, b.y, b.w, b.h, round * dpi, std::floor(1.0 * dpi), is_first, is_first, is_last, is_last);
        set_argb(cr, theme.row_border);
        cr->set_line_width(std::floor(1.0 * dpi));
        cr->stroke();
        if (active) {
            // Accent bar on the left edge.
            set_argb(cr, theme.accent);
            drawRoundedRect(cr, c->real_bounds.x + 6 * dpi, c->real_bounds.y + c->real_bounds.h * .28,
                3 * dpi, c->real_bounds.h * .44, 1.5 * dpi, 1.0);
            cr->fill();
        }
    };

    const auto description = skbd_description(layout);
    const auto id = skbd_id(layout);
    auto name = row->child(FILL_SPACE, FILL_SPACE);
    name->when_paint = [description, id, index](Container *root, Container *c) {
        std::string subtitle = id;
        if (index == 0)
            subtitle += "  ·  Default";
        swifi_two_lines(root, c->real_bounds, description, subtitle, index == skbd_active);
    };

    // "Active" tag for the layout in use.
    auto tag = row->child(FILL_SPACE, FILL_SPACE);
    tag->pre_layout = [index](Container *root, Container *c, const Bounds &) {
        auto dpi = swifi_window(root)->dpi;
        c->wanted_bounds.w = index == skbd_active ? swifi_text_width(root, "Active", 10) + 20 * dpi : 0;
    };
    tag->when_paint = [index](Container *root, Container *c) {
        if (index != skbd_active)
            return;
        auto window = swifi_window(root);
        auto dpi = window->dpi;
        auto b = c->real_bounds;
        double h = 22 * dpi;
        Bounds pill(b.x, b.y + (b.h - h) * .5, b.w, h);
        RGBA bg = theme.accent;
        bg.a *= .22;
        set_argb(window->drawing_context, bg);
        drawRoundedRect(window->drawing_context, pill.x, pill.y, pill.w, pill.h, h * .5, 1.0);
        window->drawing_context->fill();
        swifi_text(root, pill, "Active", 10, theme.text, true, true);
    };

    if (implicit)
        return;
    auto always = []() { return true; };
    if (index > 0) {
        swifi_card_button(row, []() { return std::string("Make default"); }, always, [index]() {
            if (index <= 0 || index >= (int) skbd_layouts.size())
                return;
            auto moved = skbd_layouts[index];
            skbd_layouts.erase(skbd_layouts.begin() + index);
            skbd_layouts.insert(skbd_layouts.begin(), moved);
            skbd_commit();
        });
    }
    swifi_card_button(row, []() { return std::string("Remove"); }, always, [index]() {
        if (index < 0 || index >= (int) skbd_layouts.size())
            return;
        skbd_layouts.erase(skbd_layouts.begin() + index);
        skbd_commit();
    });
}

// A line of secondary text under a section title.
static void skbd_caption(Container *parent, std::string text) {
    auto caption = parent->child(FILL_SPACE, FILL_SPACE);
    caption->pre_layout = [text](Container *root, Container *c, const Bounds &b) {
        auto dpi = swifi_window(root)->dpi;
        c->wanted_bounds.h = swifi_text_height(root, text, 11) + 4 * dpi;
    };
    caption->when_paint = [text](Container *root, Container *c) {
        swifi_text(root, c->real_bounds, text, 11, theme.text_secondary);
    };
}

static void fill_keyboard_settings(Container *root, Container *c, bool reload) {
    auto right = container_by_name("settings_right", root);
    if (!right)
        return;
    for (auto child: right->children)
        delete child;
    right->children.clear();

    right->pre_layout = layout_right_page;
    auto padded_right = right->child(FILL_SPACE, FILL_SPACE);
    padded_right->name = "keyboard_page";

    if (reload) {
        skbd_layouts = set->keyboard_layouts;
        sshortcut_load_rows();
        skbd_watch_start();
    }
    skbd_page_stale = false;

    make_section_title(padded_right, "Keyboard Settings");

    make_vert_space(padded_right, 10);

    bool only_numbers = true;
    make_reset_textfield(padded_right, "Repeat delay", "Delay before a held key starts repeating, in milliseconds", "", only_numbers, std::to_string(set->repeat_delay), "600", [](std::string value) {
        set->repeat_delay = std::atoi(value.c_str());
        reset_conf();
    }, nullptr, "Whole number of milliseconds\n1000 is one second");

    make_reset_textfield(padded_right, "Repeat rate", "Number of key repeats per second", "", only_numbers, std::to_string(set->repeat_rate), "25", [](std::string value) {
        set->repeat_rate = std::atoi(value.c_str());
        reset_conf();
    }, nullptr, "Whole number of repeats per second");

    make_vert_space(padded_right, 28);

    make_section_title(padded_right, "Layouts");

    make_vert_space(padded_right, 6);

    skbd_caption(padded_right, "The first layout is used when you log in");

    make_vert_space(padded_right, 12);

    // Same row and state as on the Shortcuts page.
    int switch_index = sshortcut_index_of("switch_layout");
    if (switch_index >= 0) {
        make_shortcut_row(padded_right, switch_index);
        make_vert_space(padded_right, 16);
    }

    if (skbd_layouts.empty()) {
        skbd_layout_row(padded_right, 0, {"us", ""}, true, true, true);
    } else {
        for (int i = 0; i < (int) skbd_layouts.size(); i++)
            skbd_layout_row(padded_right, i, skbd_layouts[i], false, i == 0, i == (int) skbd_layouts.size() - 1);
    }

    make_vert_space(padded_right, 12);

    auto add_line = padded_right->child(::hbox, FILL_SPACE, FILL_SPACE);
    add_line->parent_bounds_limit_input_bounds = false;
    add_line->pre_layout = [](Container *root, Container *c, const Bounds &) {
        c->wanted_bounds.h = 34 * swifi_window(root)->dpi;
    };
    auto add = swifi_card_button(add_line, []() { return std::string("Add a layout…"); }, []() { return true; }, []() {});
    add->parent_bounds_limit_input_bounds = false;
    add->when_clicked = [](Container *root, Container *c) {
        skbd_open_layout_dialog(root, c);
    };

    make_vert_space(padded_right, 16);
}

/*************************************************
 *
 * Search
 *
 * The field at the top of the left column filters every setting. Results
 * replace the page on the right; picking one opens its page, scrolls the row
 * into view and flashes an outline around it.
 *
 *************************************************/

static std::vector<SearchEntry> search_index;
static std::string search_query;
static std::vector<int> search_results;     // Indices into search_index, best first
static int search_selected = 0;             // Result chosen by Return, moved with Up/Down
static bool search_reveal_selected = false; // Scroll the selected result into view on the next paint
static int search_pending_jump = -1;        // Result to open on the next layout, -1 for none
static std::string search_last_page;        // Page shown before searching, restored when the query is cleared
// Narrow windows fold the page list behind a hamburger button; settings_nav_open
// is whether it is currently expanded over the page.
// The page list is this wide when shown beside the page; pages need
// settings_page_min_w. Below their sum the list folds behind the hamburger.
static constexpr double settings_left_w     = 250;
static constexpr double settings_page_min_w = 600;
static bool settings_narrow   = false;
// Set by settings::start from the main thread; the settings thread focuses the search field.
static std::atomic<bool> search_focus_requested = false;
static bool settings_nav_open = false;
// The page list scrolls when the window is too short to show all of it.
struct NavScroll {
    float scroll   = 0;
    float overflow = 0;
    SmoothScroll smooth;
    bool bar_hovered  = false;
    bool bar_dragging = false;
    double bar_grab   = 0; // Mouse offset from the thumb top while dragging
};
static NavScroll nav_scroll;

static constexpr long search_flash_ms = 1600;

struct SearchFlash {
    std::string key;   // Tag of the row to find
    std::string title; // Fallback when the row sits under another section than indexed
    bool locating = false;
    int attempts = 0;
    long start = 0;    // 0 while not flashing
    bool timer = false;
};
static SearchFlash search_flash;

// Where search_add puts entries; only changed while search_build_mutex is held.
static std::vector<SearchEntry> *search_add_target = &search_index;

static void search_add(std::string page, std::string section, std::string title, std::string description,
                       std::string keywords = "", std::function<void(Container *)> after_open = nullptr) {
    search_add_target->push_back({std::move(page), std::move(section), std::move(title), std::move(description), std::move(keywords),
                            std::move(after_open)});
}

// Everything the search can find. Rows seen while their page was open
// (search_learned) are merged in on top, so visited pages are always exact.
static void search_build_into(std::vector<SearchEntry> &out) {
    std::lock_guard<std::mutex> lock(search_build_mutex);
    search_add_target = &out;
    out.clear();

    search_add("Display", "Display Settings", "Display Settings", "Arrange monitors and change how each one is driven",
               "monitors screens displays arrange layout position outputs");
    search_add("Display", "Display Settings", "Draw wallpaper", "Can drag and drop image here", "background image picture photo choose file wallpaper drop");
    search_add("Display", "Display Settings", "Scaling", "How the image fits the screen", "wallpaper background fill fit stretch center tile custom zoom crop scale");
    search_add("Display", "Display Settings", "Crop", "Drag the preview to move the image, Ctrl+scroll to zoom", "wallpaper background crop zoom move position reset");
    search_add("Display", "Display Settings", "Span across monitors", "Stretch one image over every monitor using the shared wallpaper", "wallpaper background span stretch multiple monitors displays");
    search_add("Display", "Display Settings", "Wallpaper for", "Monitors can each have their own wallpaper", "wallpaper background per monitor display separate own");
    search_add("Display", "Display Settings", "Brightness", "Screen backlight level", "brightness backlight dim bright screen");
    search_add("Display", "Display Settings", "Night light", "Warmer colors that cut down on blue light", "night light blue light warm yellow hyprsunset");
    search_add("Display", "Display Settings", "Night light temperature", "Higher values make the screen warmer", "night light temperature kelvin warm yellow color tone hyprsunset");
    search_add("Display", "Display Settings", "Desktop Icons", "", "show icons files folders on desktop");
    search_add("Display", "Monitor", "Mode", "Resolution and refresh rate", "resolution refresh rate hz fps size");
    search_add("Display", "Monitor", "Scale", "Scale factor, or \"auto\"", "zoom size dpi hidpi fractional scaling bigger smaller");
    search_add("Display", "Monitor", "Transform", "Rotation and reflection applied to the display", "rotate rotation flip portrait landscape orientation");
    search_add("Display", "Monitor", "HDR", "High dynamic range, for displays that support it", "high dynamic range color");
    search_add("Display", "Monitor", "Advanced", "Position, mirroring, bit depth, color management, VRR and more");
    search_add("Display", "Monitor", "Position", "Position such as \"1920x0\" or \"auto\"", "offset coordinates arrange placement");
    search_add("Display", "Monitor", "Mirror", "Monitor output to mirror, leave empty for none", "duplicate clone same image projector");
    search_add("Display", "Monitor", "Bit depth", "Color bit depth", "10 bit 8 bit colour");
    search_add("Display", "Monitor", "Color management", "Color management preset", "colour gamut srgb wide preset cm");
    search_add("Display", "Monitor", "SDR EOTF", "Transfer function used for SDR content", "gamma transfer function");
    search_add("Display", "Monitor", "VRR", "Variable refresh rate mode (-1 default, 0 off, 1 on, 2 fullscreen, 3 fullscreen only)",
               "variable refresh rate freesync gsync adaptive sync tearing");
    search_add("Display", "Monitor", "ICC profile", "Path to an ICC profile, leave empty for none", "color profile calibration colour");
    search_add("Display", "Monitor", "Reserved area", "Reserved area in pixels", "margin gap bar space");
    search_add("Display", "Monitor", "Supports wide color", "Override wide color gamut support detection", "wide gamut colour");
    search_add("Display", "Monitor", "Supports HDR", "Override HDR support detection", "high dynamic range");
    search_add("Display", "Monitor", "SDR brightness", "SDR brightness multiplier (0–10)", "brighter dimmer light");
    search_add("Display", "Monitor", "SDR saturation", "SDR saturation multiplier (0–10)", "vibrance colour color");
    search_add("Display", "Monitor", "SDR min luminance", "Minimum SDR luminance", "brightness black level");
    search_add("Display", "Monitor", "SDR max luminance", "Maximum SDR luminance", "brightness nits peak");
    search_add("Display", "Monitor", "Min luminance", "Minimum luminance override (-1 uses automatic)", "brightness black level");
    search_add("Display", "Monitor", "Max luminance", "Maximum luminance override", "brightness nits peak");
    search_add("Display", "Monitor", "Max average luminance", "Maximum average luminance override", "brightness nits");
    search_add("Display", "Monitor", "Disabled", "Turn this monitor off without disconnecting it", "disable turn off monitor screen enable");

    search_add("Desktop", "Desktop Settings", "Desktop Settings", "", "desktop");
    search_add("Desktop", "Desktop Settings", "Icon Cache", "Re-scan installed icons", "icons refresh rescan reload missing icon theme");
    search_add("Desktop", "Desktop Settings", "Overview", "Change layout type", "expose grid adaptive mission control windows overview");
    search_add("Desktop", "Desktop Settings", "Theme", "Choose the settings appearance", "dark light mode colors colours appearance night");
    search_add("Desktop", "Desktop Settings", "Font", "User interface font", "typeface text fonts ui");
    search_add("Desktop", "Blur", "Blur", "", "blur transparency glass acrylic frost kawase decoration");
    search_add("Desktop", "Blur", "Window blur", "Blur backgrounds behind translucent windows and surfaces",
               "blur enable disable background glass frosted transparency");
    search_add("Desktop", "Blur", "Blur style", "Blur effect style; animated styles use more GPU",
               "blur variant kawase frost acrylic prism haze aurora ripple drops water heat shimmer fluid jar");
    search_add("Desktop", "Blur", "Blur size", "Blur radius distance (1–20)", "blur radius distance strength amount");
    search_add("Desktop", "Blur", "Blur passes", "Number of blur passes (1–6); more passes look smoother but use more GPU",
               "blur passes quality smoothness iterations");
    search_add("Desktop", "Blur", "Vibrancy", "Increase saturation of blurred colors", "blur saturation color vibrancy");
    search_add("Desktop", "Blur", "Advanced", "Contrast, brightness, noise, X-ray, popups and special workspace blur",
               "blur advanced tuning");
    search_add("Desktop", "Blur", "Contrast", "Contrast modulation applied to blurred backgrounds", "blur contrast");
    search_add("Desktop", "Blur", "Brightness", "Brightness modulation applied to blurred backgrounds", "blur brightness");
    search_add("Desktop", "Blur", "Noise", "Subtle grain added to blurred backgrounds", "blur noise grain");
    search_add("Desktop", "Blur", "Vibrancy darkness", "Strength of vibrancy on dark blurred areas", "blur vibrancy darkness dark");
    search_add("Desktop", "Blur", "Ignore window opacity", "Keep full blur strength even on semi-transparent windows",
               "blur ignore opacity alpha transparency");
    search_add("Desktop", "Blur", "X-ray blur", "Floating windows blur the wallpaper directly, ignoring tiled windows behind them",
               "blur xray x-ray wallpaper floating");
    search_add("Desktop", "Blur", "Blur popups", "Apply background blur behind menus and popups",
               "blur popups menus context right click");
    search_add("Desktop", "Blur", "Blur special workspace", "Apply background blur behind the special workspace",
               "blur special workspace scratchpad");
    search_add("Dock", "Dock Settings", "Dock Settings", "", "dock taskbar panel bar");
    search_add("Dock", "Dock Settings", "Show docks", "", "taskbar panel bar dock hide");
    search_add("Dock", "Dock Settings", "Bounce icons on minimize and restore", "Dock icons dip when their window minimizes and hop when it comes back",
               "taskbar dock bounce animation minimize restore unminimize hide");
    search_add("Dock", "Dock Settings", "Same width", "When labels are showing, give every dock button the same width instead of fitting its title",
               "taskbar dock labels width uniform same size buttons title");
    search_add("Dock", "Dock Settings", "Dock style", "How dock icons are drawn", "taskbar dock style bubbly flat windows 7 glass look appearance");
    search_add("Dock", "Dock Settings", "Only show windows on the current workspace", "Turn off to show windows from all workspaces and monitors",
               "taskbar dock workspace windows monitors");
    search_add("Dock", "Dock Settings", "Show applications from other desktops",
               "List applications in the applications menu that are meant only for GNOME, KDE or another desktop",
               "applications menu launcher desktop files gnome kde xfce session onlyshowin apps all");
    search_add("Dock", "Dock Settings", "Labels", "Give each window its own dock button with its title next to the icon",
               "group merge stack combine labels titles dock taskbar buttons");

    search_add("Multitasking", "Multitasking Settings", "Multitasking Settings", "", "multitasking windows workspaces");
    search_add("Multitasking", "Multitasking Settings", "App switching", "Choose which windows appear in the app switcher",
               "alt tab switcher windows workspace");
    search_add("Multitasking", "Multitasking Settings", "Workspace behavior", "Create workspaces as needed or keep a fixed number",
               "dynamic fixed workspaces virtual desktops behaviour");
    search_add("Multitasking", "Multitasking Settings", "Number of workspaces", "Per monitor in Fixed mode (1–20); occupied extras are kept",
               "count workspaces virtual desktops amount");
    search_add("Multitasking", "Multitasking Settings", "Multiple monitors", "Switch workspaces independently or on all monitors together",
               "workspaces span displays screens independent together");
    search_add("Multitasking", "Multitasking Settings", "Hotcorners", "Activates shortcuts when an edge is hit", "hot corners edge corner");
    search_add("Multitasking", "Multitasking Settings", "Snap windows", "Drag a window to a screen edge or corner to fill half or a quarter of the screen",
               "window resizing screen edges corners quarter snap snapping tile tiling half maximize drag edges");
    search_add("Multitasking", "Multitasking Settings", "Attempt to respect CSD requests", "Don't add a titlebar to windows that ask to draw their own decorations",
               "csd ssd client side server side decorations titlebar title bar xdg kde");

    search_add("Autostart", "Autostart", "Autostart", "Commands run when the desktop or session starts",
               "autostart startup start up exec launch login boot programs apps run commands session");
    for (const auto &entry : set->autostart)
        search_add("Autostart", "Autostart", entry.command, entry.when, "autostart startup exec command");

    if (has_sddm_service_enabled()) {
        search_add("SDDM", "Login screen (SDDM)", "Login screen (SDDM)", "Wallpaper and display scale for the SDDM login screen",
                   "sddm login greeter lock screen display manager");
        search_add("SDDM", "Wallpaper", "Wallpaper source", "Uses your Display wallpaper with the same cropping",
                   "sddm login greeter wallpaper background image crop");
        search_add("SDDM", "Display scale", "Scale", "DPI scale factor for the SDDM greeter",
                   "sddm login greeter scale dpi hidpi qt_scale_factor zoom");
        search_add("SDDM", "System configuration", "Apply to SDDM", "Write wallpaper and scale configuration to SDDM",
                   "sddm apply save pkexec greeter");
    }

    search_add("Mouse & Touchpad", "Mouse", "Mouse", "", "mouse pointer");
    search_add("Mouse & Touchpad", "Mouse", "Reverse scrolling direction", "Phone like scrolling behaviour", "natural scrolling invert scroll wheel");
    search_add("Mouse & Touchpad", "Mouse", "Primary mouse button", "Which side performs the main function of the mouse",
               "left handed right handed swap buttons click");
    search_add("Mouse & Touchpad", "Mouse", "Mouse speed", "How far the pointer moves when you move the mouse", "cursor pointer sensitivity speed fast slow mouse");
    search_add("Mouse & Touchpad", "Touchpad", "Touchpad", "", "trackpad touchpad laptop");
    search_add("Mouse & Touchpad", "Touchpad", "Reverse scrolling direction", "Phone like scrolling behaviour", "natural scrolling invert trackpad");
    search_add("Mouse & Touchpad", "Touchpad", "Disable while typing", "Prevents touchpad from receiving input while typing", "palm rejection trackpad dwt");
    search_add("Mouse & Touchpad", "Touchpad", "Touchpad acceleration", "Cursor moves different distances depending on how fast you flick",
               "pointer curve flat adaptive custom trackpad speed");
    search_add("Mouse & Touchpad", "Touchpad", "Touchpad speed", "How far the pointer moves when you slide on the touchpad",
               "cursor pointer sensitivity speed fast slow trackpad");

    search_add("Keyboard", "Keyboard Settings", "Keyboard Settings", "", "keyboard keys typing");
    search_add("Keyboard", "Keyboard Settings", "Repeat delay", "Delay before a held key starts repeating, in milliseconds", "key repeat typing hold");
    search_add("Keyboard", "Keyboard Settings", "Repeat rate", "Number of key repeats per second", "key repeat typing speed");
    search_add("Keyboard", "Layouts", "Layouts", "The first layout is used when you log in",
               "keyboard layout language input source keymap xkb add layout dvorak colemak qwerty");

    search_add("Shortcuts", "Custom shortcuts", "Custom shortcuts", "Run your own commands with a key combination",
               "add custom command keybind keybinding hotkey shortcut");
    search_add("Shortcuts", "System shortcuts", "System shortcuts", "", "keybindings keybinds hotkeys keys shortcuts");
    for (const auto &custom : set->custom_shortcuts)
        search_add("Shortcuts", "Custom shortcuts", custom.name, custom.keys, "shortcut keybind hotkey " + custom.command);
    for (const auto &shortcut : shortcuts::defaults())
        search_add("Shortcuts", "System shortcuts", shortcut.name, shortcuts::keys_for(set, shortcut), "shortcut keybind hotkey");

    search_add("Time & Date", "Time & Date", "Time & Date", "", "clock date time");
    search_add("Time & Date", "Time & Date", "Automatic Date & Time", "Requires internet access", "ntp sync clock network time internet");
    search_add("Time & Date", "Time & Date", "Date & Time", "", "clock set time manually change date");
    search_add("Time & Date", "Time & Date", "Automatic Time Zone", "Requires location services enabled and internet access", "timezone location");
    search_add("Time & Date", "Time & Date", "Time Zone", "", "timezone region city location utc gmt");
    search_add("Time & Date", "Time & Date", "Time Format", "", "24-hour 12-hour am pm clock military");

    search_add("Audio", "Audio", "Audio", "", "sound volume speakers microphone pipewire pulseaudio");
    for (int i = 0; i < 5; i++) {
        static const char *descriptions[] = {"Applications playing sound", "Applications using a microphone", "Speakers and headphones",
                                             "Microphones", "Sound card profiles"};
        static const char *keywords[] = {"streams apps applications volume sound", "streams apps microphone mic record",
                                         "sink speakers headphones default output volume", "source mic microphone input default",
                                         "card profile hdmi analog configuration"};
        search_add("Audio", "Audio", saudio_tab_names[i], descriptions[i], keywords[i], [i](Container *root) {
            if (settings_audio_tab == i)
                return;
            settings_audio_tab = i;
            if (settings_audio)
                saudio_apply(settings_audio);
        });
    }

    if (settings_category_of("Battery")) {
        search_add("Battery", "Status", "Status", "", "battery power charge laptop");
        search_add("Battery", "Status", "Battery level", "Charge, charging state and time remaining",
                   "percent percentage charging remaining time left estimate empty");
        search_add("Battery", "Battery usage", "Battery history", "Charge over the last 3 hours, with an estimate of what is left",
                   "graph usage history drain discharge rate watts chart");
        search_add("Battery", "Battery usage", "Record battery history", "Saves the charge every 5 minutes and keeps the last 24 hours",
                   "history record log save graph");
    }
    search_add("Wifi", "Wi-Fi", "Wi-Fi", "", "wifi wireless network internet connect wlan");
    search_add("Wifi", "Available networks", "Available networks", "", "wifi ssid connect password network join");

    search_add("Bluetooth", "Bluetooth", "Bluetooth", "Turn Bluetooth on or off",
               "bluetooth wireless devices headphones headset earbuds speaker controller gamepad enable disable");
    search_add("Bluetooth", "Bluetooth", "Add device", "Search for nearby devices to pair",
               "bluetooth pair pairing discover scan new device connect");
    search_add("Bluetooth", "Devices", "Devices", "Connect, disconnect or forget paired devices",
               "bluetooth paired devices connect disconnect forget unpair remove");

    for (const auto &learned : search_learned) {
        bool found = false;
        for (auto &e : out) {
            if (e.page == learned.page && e.section == learned.section && e.title == learned.title) {
                e.description = learned.description;
                found = true;
                break;
            }
        }
        if (!found)
            out.push_back(learned);
    }
    search_add_target = &search_index;
}

static void search_build_index() {
    search_build_into(search_index);
}

// Lower case words; anything that isn't a letter or a digit separates them.
static std::vector<std::string> search_words(const std::string &text) {
    std::vector<std::string> words;
    std::string word;
    for (unsigned char ch : text) {
        if (std::isalnum(ch) || ch >= 0x80) {
            word += (char) std::tolower(ch);
        } else if (!word.empty()) {
            words.push_back(std::move(word));
            word.clear();
        }
    }
    if (!word.empty())
        words.push_back(std::move(word));
    return words;
}

// Optimal string alignment distance (Levenshtein plus adjacent swaps), the
// usual shape of a typo.
static int search_edit_distance(const std::string &a, const std::string &b) {
    const int n = (int) a.size(), m = (int) b.size();
    std::vector<std::vector<int>> d(n + 1, std::vector<int>(m + 1));
    for (int i = 0; i <= n; i++)
        d[i][0] = i;
    for (int j = 0; j <= m; j++)
        d[0][j] = j;
    for (int i = 1; i <= n; i++) {
        for (int j = 1; j <= m; j++) {
            int cost = a[i - 1] == b[j - 1] ? 0 : 1;
            d[i][j] = std::min({d[i - 1][j] + 1, d[i][j - 1] + 1, d[i - 1][j - 1] + cost});
            if (i > 1 && j > 1 && a[i - 1] == b[j - 2] && a[i - 2] == b[j - 1])
                d[i][j] = std::min(d[i][j], d[i - 2][j - 2] + 1);
        }
    }
    return d[n][m];
}

// How well one query word matches one word of the text, 0 to 1.
static double search_word_score(const std::string &q, const std::string &w) {
    if (q == w)
        return 1.0;
    if (w.size() > q.size() && w.compare(0, q.size(), q) == 0) // Still typing it
        return 0.8 + 0.15 * q.size() / w.size();
    if (q.size() >= 3 && w.find(q) != std::string::npos)
        return 0.55;
    double best = 0;
    // Typos: one mistake from 4 letters, two from 7, against the whole word or
    // against as much of it as has been typed so far.
    if (q.size() >= 4) {
        int allowed = q.size() >= 7 ? 2 : 1;
        int whole = std::abs((int) w.size() - (int) q.size()) <= allowed ? search_edit_distance(q, w) : 99;
        if (whole <= allowed)
            best = std::max(best, 0.7 - 0.12 * whole);
        if (w.size() > q.size()) {
            int prefix = search_edit_distance(q, w.substr(0, q.size()));
            if (prefix <= allowed)
                best = std::max(best, 0.6 - 0.12 * prefix);
        }
    }
    // Abbreviations: the letters in order, starting at the same letter ("brt" for brightness).
    if (best == 0 && q.size() >= 3 && q[0] == w[0]) {
        size_t at = 0, first = std::string::npos, last = 0;
        for (char ch : q) {
            at = w.find(ch, at);
            if (at == std::string::npos)
                return 0;
            if (first == std::string::npos)
                first = at;
            last = at++;
        }
        best = 0.45 * q.size() / (double) (last - first + 1);
    }
    return best;
}

static double search_field_score(const std::string &q, const std::vector<std::string> &words) {
    double best = 0;
    for (const auto &w : words)
        best = std::max(best, search_word_score(q, w));
    return best;
}

// Every query word must match something; the title counts most, then the
// keywords, section, page and description. -1 when the entry doesn't match.
static double search_score(const SearchEntry &e, const std::vector<std::string> &query, const std::string &phrase) {
    struct Field {
        std::vector<std::string> words;
        double weight;
    };
    Field fields[] = {
        {search_words(e.title), 1.0},
        {search_words(e.keywords), 0.75},
        {search_words(e.section), 0.6},
        {search_words(e.page), 0.55},
        {search_words(e.description), 0.5},
    };
    double total = 0;
    for (const auto &q : query) {
        double best = 0;
        for (const auto &f : fields)
            best = std::max(best, f.weight * search_field_score(q, f.words));
        if (best < 0.2)
            return -1;
        total += best;
    }
    total /= query.size();

    // The whole query reads like the title.
    auto title = sdt_lower(e.title);
    if (title == phrase)
        total += 1.0;
    else if (title.compare(0, phrase.size(), phrase) == 0)
        total += 0.6;
    else if (phrase.size() >= 2 && (" " + title).find(" " + phrase) != std::string::npos) // Starts a later word
        total += 0.45;
    else if (phrase.size() >= 3 && title.find(phrase) != std::string::npos)
        total += 0.3;
    // There are many shortcuts, keep them from crowding out settings with similar words.
    if (e.section == "System shortcuts" && e.title != e.section)
        total *= 0.8;
    return total;
}

// Indices into index matching text, best first.
static std::vector<int> search_rank(const std::vector<SearchEntry> &index, const std::string &text, int limit) {
    std::vector<int> results;
    auto query = search_words(text);
    if (query.empty())
        return results;
    auto phrase = sdt_lower(trim(text));
    std::vector<std::pair<double, int>> scored;
    for (int i = 0; i < (int) index.size(); i++) {
        double score = search_score(index[i], query, phrase);
        if (score >= 0)
            scored.push_back({score, i});
    }
    std::stable_sort(scored.begin(), scored.end(), [](const auto &a, const auto &b) { return a.first > b.first; });
    for (int i = 0; i < (int) scored.size() && i < limit; i++)
        results.push_back(scored[i].second);
    return results;
}

static void search_run() {
    search_results = search_rank(search_index, search_query, 60);
}

static std::string search_breadcrumb(const SearchEntry &e) {
    auto title = settings_page_title(e.page);
    if (e.section.empty() || e.section == e.title || e.section == e.page || e.section == title || e.section == e.page + " Settings")
        return title;
    return title + "  ›  " + e.section;
}

static void fill_search_results(Container *root) {
    auto right = container_by_name("settings_right", root);
    if (!right)
        return;
    for (auto child : right->children)
        delete child;
    right->children.clear();
    ((RightData *) right->user_data)->scroll = 0;
    ((RightData *) right->user_data)->smooth.stop();
    search_flash = {};

    right->pre_layout = layout_right_page;
    auto padded_right = right->child(FILL_SPACE, FILL_SPACE);
    padded_right->name = "search_page";

    search_current_page.clear();
    make_section_title(padded_right, "Search");
    make_vert_space(padded_right, 6);

    std::string summary;
    if (search_results.empty())
        summary = "No settings match “" + trim(search_query) + "”";
    else
        summary = std::to_string(search_results.size()) + (search_results.size() == 1 ? " result" : " results") + " for “" + trim(search_query) + "”";
    auto caption = padded_right->child(FILL_SPACE, FILL_SPACE);
    caption->pre_layout = [summary](Container *root, Container *c, const Bounds &) {
        c->wanted_bounds.h = swifi_text_height(root, summary, 11) + 4 * swifi_window(root)->dpi;
    };
    caption->when_paint = [summary](Container *root, Container *c) {
        swifi_text(root, c->real_bounds, summary, 11, theme.text_secondary);
    };
    make_vert_space(padded_right, 12);

    for (int i = 0; i < (int) search_results.size(); i++) {
        const int index = search_results[i];
        auto row = padded_right->child(::hbox, FILL_SPACE, FILL_SPACE);
        row->uuid = "search_result:" + std::to_string(i);
        row->parent_bounds_limit_input_bounds = false;
        row->receive_events_even_if_obstructed = true;
        row->pre_layout = [index](Container *root, Container *c, const Bounds &) {
            const auto &e = search_index[index];
            c->wanted_bounds.h = (e.description.empty() ? 52 : 68) * swifi_window(root)->dpi;
        };
        row->when_paint = [index, i](Container *root, Container *c) {
            if (index >= (int) search_index.size())
                return;
            const auto &e = search_index[index];
            auto window = swifi_window(root);
            auto cr = window->drawing_context;
            auto dpi = window->dpi;
            auto b = c->real_bounds;
            bool selected = i == search_selected;
            RGBA bg = c->state.mouse_pressing ? theme.navigation_pressed : (c->state.mouse_hovering || selected) ? theme.row_hover : theme.row_background;
            paint_smooth_frame(cr, b, 5 * dpi, bg, selected ? theme.accent : theme.row_border, std::max(1.0, (double) std::round(1 * dpi)));

            double x = b.x + 18 * dpi;
            double w = std::max(1.0, b.w - 36 * dpi);
            // Title with the page it lives on to the right, what it does underneath.
            auto crumb = search_breadcrumb(e);
            double th = swifi_text_height(root, e.title, 12, true);
            double dh = e.description.empty() ? 0 : swifi_text_height(root, e.description, 11);
            double gap = 4 * dpi;
            double y = b.y + std::max(0.0, (b.h - th - (dh > 0 ? gap + dh : 0)) * .5);
            double crumb_w = std::min(w * .45, swifi_text_width(root, crumb, 10) + 4 * dpi);
            swifi_text(root, Bounds(x, y, std::max(1.0, w - crumb_w - 12 * dpi), th), e.title, 12, theme.text, false, true);
            swifi_text(root, Bounds(x + w - crumb_w, y, crumb_w, th), crumb, 10, theme.accent);
            if (dh > 0)
                swifi_text(root, Bounds(x, y + th + gap, w, dh), e.description, 11, theme.text_secondary);
        };
        row->when_clicked = [i](Container *root, Container *) {
            // Opening the page deletes this row, so it happens on the next layout.
            search_pending_jump = i;
            swifi_redraw(root);
        };
        make_vert_space(padded_right, 3);
    }
    make_vert_space(padded_right, 16);
}

static Container *search_field(Container *root) {
    return container_by_name("settings_search", root);
}

// Empties the field without restoring the page (the caller shows one).
static void search_clear_field(Container *root) {
    search_query.clear();
    search_results.clear();
    search_pending_jump = -1;
    if (auto field = search_field(root)) {
        auto data = static_cast<STextEditorData *>(field->user_data);
        if (data && !data->text.empty())
            reset_text_editor(field, "");
    }
}

static void search_set_query(Container *root, const std::string &text) {
    bool was_searching = !trim(search_query).empty();
    search_query = text;
    if (trim(text).empty()) {
        search_results.clear();
        if (!was_searching)
            return;
        // Back to where the user was before searching.
        auto right = container_by_name("settings_right", root);
        if (!search_last_page.empty() && settings_category_of(search_last_page)) {
            if (right)
                ((RightData *) right->user_data)->scroll = 0;
            bool nav_open = settings_nav_open;
            settings_show(root, search_last_page);
            settings_nav_open = nav_open;
        } else if (right) {
            for (auto child : right->children)
                delete child;
            right->children.clear();
        }
        swifi_redraw(root);
        return;
    }
    if (!was_searching)
        search_build_index();
    search_run();
    search_selected = 0;
    fill_search_results(root);
    swifi_redraw(root);
}

static Container *search_find_tagged(Container *c, const std::string &key) {
    if (c->uuid == key)
        return c;
    for (auto child : c->children)
        if (auto found = search_find_tagged(child, key))
            return found;
    return nullptr;
}

// A row with the right title under any section (e.g. the monitor's name).
static Container *search_find_title(Container *c, const std::string &suffix) {
    if (c->uuid.size() > suffix.size() && c->uuid.starts_with("search:") && c->uuid.ends_with(suffix))
        return c;
    for (auto child : c->children)
        if (auto found = search_find_title(child, suffix))
            return found;
    return nullptr;
}

static void search_open_entry(Container *root, SearchEntry e) {
    if (!settings_category_of(e.page))
        return;
    // A filter on the Shortcuts page could hide the row.
    if (e.page == "Shortcuts") {
        sshortcut_filter.clear();
    }
    if (auto right = container_by_name("settings_right", root)) {
        auto d = (RightData *) right->user_data;
        d->scroll = 0;
        d->smooth.stop();
    }
    settings_show(root, e.page); // Also empties the search field
    if (e.after_open)
        e.after_open(root);
    search_flash = {};
    search_flash.key = search_key(e.section, e.title);
    search_flash.title = e.title;
    search_flash.locating = true;
    swifi_redraw(root);
}

static void search_jump(Container *root, int result) {
    if (result < 0 || result >= (int) search_results.size())
        return;
    search_open_entry(root, search_index[search_results[result]]);
}

// Set by settings::open_search_result on the main thread, opened on the settings thread.
static std::mutex search_external_mutex;
static std::optional<settings::SearchResult> search_external_jump;

static void search_open_external(Container *root) {
    std::optional<settings::SearchResult> wanted;
    {
        std::lock_guard<std::mutex> lock(search_external_mutex);
        wanted.swap(search_external_jump);
    }
    if (!wanted)
        return;
    // Rebuilt for the entry's after_open, which picks sub tabs.
    search_build_index();
    SearchEntry entry{wanted->page, wanted->section, wanted->title, wanted->description, "", nullptr};
    for (const auto &e : search_index) {
        if (e.page == entry.page && e.section == entry.section && e.title == entry.title) {
            entry = e;
            break;
        }
    }
    search_open_entry(root, entry);
    settings_nav_open = false;
}

std::vector<settings::SearchResult> settings::search(const std::string &query, int limit) {
    std::vector<SearchResult> results;
    if (trim(query).empty() || limit <= 0)
        return results;
    std::vector<SearchEntry> index;
    search_build_into(index);
    for (int i : search_rank(index, query, limit)) {
        const auto &e = index[i];
        results.push_back({e.page, e.section, e.title, e.description, search_breadcrumb(e)});
    }
    return results;
}

void settings::open_search_result(const SearchResult &result) {
    {
        std::lock_guard<std::mutex> lock(search_external_mutex);
        search_external_jump = result;
    }
    settings::start(result.page);
}

static void search_keep_animating(Container *root) {
    if (search_flash.timer || !settings_app)
        return;
    search_flash.timer = true;
    windowing::timer(settings_app, 16, [](void *) {
        search_flash.timer = false;
        if (settings_mylar)
            windowing::redraw(settings_mylar->raw_window);
    }, nullptr);
}

// Runs after the right side is painted: finds the row a result pointed at,
// scrolls it into view, then draws the fading outline around it.
static void paint_search_flash(Container *root, Container *right) {
    auto d = (RightData *) right->user_data;
    auto window = swifi_window(root);
    auto dpi = window->dpi;

    if (search_reveal_selected) {
        search_reveal_selected = false;
        if (auto row = search_find_tagged(right, "search_result:" + std::to_string(search_selected))) {
            double margin = 12 * dpi;
            double top = row->real_bounds.y - right->real_bounds.y;
            double bottom = top + row->real_bounds.h;
            float scroll = d->applied_scroll;
            if (top < margin)
                scroll += (float) (margin - top);
            else if (bottom > right->real_bounds.h - margin)
                scroll -= (float) (bottom - (right->real_bounds.h - margin));
            scroll = std::clamp(scroll, -d->overflow, 0.0f);
            if (scroll != d->applied_scroll) {
                d->smooth.stop();
                d->scroll = scroll;
                swifi_redraw(root);
            }
        }
    }

    if (search_flash.key.empty())
        return;

    Container *target = search_find_tagged(right, search_flash.key);
    if (!target)
        target = search_find_title(right, "\x1f" + search_flash.title);

    if (search_flash.locating) {
        if (!target) {
            // Pages filled in the background may need a few frames.
            if (++search_flash.attempts > 30)
                search_flash = {};
            else
                search_keep_animating(root);
            return;
        }
        // Rows folded inside an accordion: unfold it and look again next frame.
        bool hidden = false;
        Container *top = target;
        for (auto c = target; c && c != right; c = c->parent) {
            if (!c->exists)
                hidden = true;
            if (c->parent && c->parent->parent == right)
                top = c;
        }
        if (hidden) {
            if (++search_flash.attempts > 30) {
                search_flash = {};
                return;
            }
            if (auto page = top->parent) {
                auto it = std::find(page->children.begin(), page->children.end(), top);
                while (it != page->children.begin()) {
                    --it;
                    if ((*it)->custom_type == search_accordion_type && (*it)->exists) {
                        if ((*it)->when_clicked)
                            (*it)->when_clicked(root, *it);
                        break;
                    }
                }
            }
            swifi_redraw(root);
            return;
        }
        // Put the row a quarter of the way down the view.
        double content_y = target->real_bounds.y - right->real_bounds.y - d->applied_scroll;
        float scroll = (float) -(content_y - right->real_bounds.h * .25);
        d->smooth.stop();
        d->scroll = std::clamp(scroll, -d->overflow, 0.0f);
        search_flash.locating = false;
        search_flash.start = get_current_time_in_ms();
        // Its control takes keyboard focus once laid out at the new scroll.
        focus_pending_row = target;
        focus_pending_row_life = target->lifetime;
        right_bar_touch(right);
        swifi_redraw(root);
        return;
    }

    long elapsed = get_current_time_in_ms() - search_flash.start;
    if (!target || elapsed >= search_flash_ms) {
        search_flash = {};
        return;
    }

    // Two pulses that fade out.
    double t = (double) elapsed / search_flash_ms;
    double pulse = .65 + .35 * std::cos(t * 4 * M_PI);
    double fade = t < .75 ? 1.0 : 1.0 - (t - .75) / .25;
    double intensity = std::clamp(pulse * fade, 0.0, 1.0);

    auto cr = window->drawing_context;
    auto b = target->real_bounds;
    double grow = 3 * dpi;
    b.x -= grow;
    b.y -= grow;
    b.w += grow * 2;
    b.h += grow * 2;
    cr->save();
    cr->rectangle(right->real_bounds.x, right->real_bounds.y, right->real_bounds.w, right->real_bounds.h);
    cr->clip_path();
    RGBA fill = theme.accent;
    fill.a *= .10 * intensity;
    drawRoundedRect(cr, b.x, b.y, b.w, b.h, 8 * dpi, 1.0);
    set_argb(cr, fill);
    cr->fill();
    RGBA line = theme.accent;
    line.a *= intensity;
    cr->set_line_width(2 * dpi);
    drawRoundedRect(cr, b.x, b.y, b.w, b.h, 8 * dpi, 1.0);
    set_argb(cr, line);
    cr->stroke();
    cr->set_line_width(1);
    cr->restore();
    search_keep_animating(root);
}

// The field in the left column, in the slot a "Search" tab would have.
static void make_search_field(Container *left) {
    auto slot = left->child(::vbox, FILL_SPACE, FILL_SPACE);
    slot->pre_layout = [](Container *root, Container *c, const Bounds &) {
        auto dpi = swifi_window(root)->dpi;
        c->wanted_bounds.h = 40 * dpi;
        c->wanted_pad = Bounds(3 * dpi, 4 * dpi, 3 * dpi, 4 * dpi);
    };

    auto field = slot->child(FILL_SPACE, FILL_SPACE);
    field->name = "settings_search";
    STextEditorOptions options;
    options.multiline = false;
    options.font_size = 11;
    options.window = [](Container *root) { return static_cast<MylarWindow *>(root->user_data)->raw_window; };
    options.viewport = [](Container *root, Container *c) {
        auto dpi = static_cast<MylarWindow *>(root->user_data)->raw_window->dpi;
        auto bounds = c->real_bounds;
        bounds.x += 12 * dpi;
        bounds.w = std::max(0.0, bounds.w - 12 * dpi - 11 * dpi);
        return bounds;
    };
    options.on_change = [](std::string text) {
        if (settings_mylar)
            search_set_query(settings_mylar->root, text);
    };
    setup_text_editor(field, "", std::move(options));
    focus_text(field);
    field->when_paint = [](Container *root, Container *c) {
        auto window = swifi_window(root);
        auto cr = window->drawing_context;
        auto dpi = window->dpi;
        auto b = c->real_bounds;
        paint_smooth_frame(cr, b, settings_control_rounding * dpi, theme.field_background,
                           c->active ? theme.accent : theme.field_border, std::max(1.0, (double) std::round(1 * dpi)));
        auto data = static_cast<STextEditorData *>(c->user_data);
        if (data && data->text.empty()) {
            // The clear button takes this spot while there is text.
            const std::string glyph = "\uE721"; // Search
            auto gb = draw_text(cr, 0, 0, glyph, 11 * dpi, false, icon_font, -1, -1, theme.text_secondary, false);
            draw_text(cr, b.x + b.w - 14 * dpi - gb.w, b.y + (b.h - gb.h) * .5, glyph, 11 * dpi, true, icon_font, -1, -1,
                      c->active ? theme.text : theme.text_secondary, false);
        }
        paint_text_editor(root, c, theme.text, theme.accent);
        if (data && data->text.empty()) {
            Bounds text(b.x + 12 * dpi, b.y, std::max(0.0, b.w - 48 * dpi), b.h);
            swifi_text(root, text, "Find a setting (Ctrl + L)", 11, swifi_dim(theme.text_secondary));
        }
    };
    auto edit_key = std::move(field->when_key_event);
    field->when_key_event = [edit_key](Container *root, Container *c, int key, bool pressed, xkb_keysym_t sym, int mods, bool is_text,
                                       std::string text) {
        bool searching = !trim(search_query).empty();
        if (pressed && sym == XKB_KEY_Escape) {
            if (searching) {
                reset_text_editor(c, "");
                search_set_query(root, "");
            }
            return;
        }
        if (pressed && searching && (sym == XKB_KEY_Return || sym == XKB_KEY_KP_Enter)) {
            if (!search_results.empty()) {
                search_pending_jump = search_selected;
                swifi_redraw(root);
            }
            return;
        }
        if (pressed && searching && (sym == XKB_KEY_Down || sym == XKB_KEY_Up) && !search_results.empty()) {
            int count = (int) search_results.size();
            search_selected = (search_selected + (sym == XKB_KEY_Down ? 1 : count - 1)) % count;
            search_reveal_selected = true;
            swifi_redraw(root);
            return;
        }
        if (edit_key)
            edit_key(root, c, key, pressed, sym, mods, is_text, text);
    };
}

// Category whose entry is highlighted in the left column.
static std::string settings_current_category;

// Colourful category icons, keyed by icon name and pixel size. Only touched on the settings thread.
static std::unordered_map<std::string, cairo_surface_t *> settings_icon_cache;

static void settings_icon_cache_clear() {
    for (auto &[key, surface] : settings_icon_cache)
        if (surface)
            cairo_surface_destroy(surface);
    settings_icon_cache.clear();
}

static cairo_surface_t *settings_category_icon(const SettingsCategory &category, int size) {
    auto key = category.icon + "\x1f" + std::to_string(size);
    if (auto it = settings_icon_cache.find(key); it != settings_icon_cache.end())
        return it->second;
    std::string path = "/usr/share/mylardesktop/icons/" + category.icon + ".svg";
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        // Not installed yet: the icon theme's version of the same icon.
        if (!icons_loaded)
            return nullptr; // Try again on a later paint.
        path = one_shot_icon(size, {category.fallback, "preferences-system"});
    }
    cairo_surface_t *surface = nullptr;
    if (!path.empty())
        load_icon_full_path(&surface, path, size);
    settings_icon_cache[key] = surface;
    return surface;
}

// How much of the text colour tints a left column entry for hover and press. The
// selected entry's own tint is painted by settings_nav_indicator_paint underneath.
static double settings_nav_hover_tint(Container *c) {
    return c->state.mouse_pressing ? .035 : c->state.mouse_hovering ? .05 : 0;
}

// The selected entry's highlight and accent pill glide to a newly selected entry.
struct NavIndicator {
    ToggleTween tween;
    Bounds from_rel; // Where it started, relative to the list
    Bounds last_rel; // Where it was last painted, relative to the list
    std::string selected;
    bool painted = false;
};
static NavIndicator nav_indicator;
static constexpr float nav_indicator_ms = 220;

// Painted by the left column before its entries.
static void settings_nav_indicator_paint(Container *root, Container *list) {
    auto window = swifi_window(root);
    auto cr = window->drawing_context;
    auto dpi = window->dpi;
    Container *selected = nullptr;
    if (trim(search_query).empty())
        selected = container_by_name("category:" + settings_current_category, list);
    if (!selected || !selected->exists) {
        // Nothing to slide from once it reappears.
        nav_indicator.painted = false;
        return;
    }
    auto l = list->real_bounds;
    auto target_abs = selected->real_bounds;
    target_abs.shrink(3 * dpi);
    Bounds target(target_abs.x - l.x, target_abs.y - l.y, target_abs.w, target_abs.h);

    long now = get_current_time_in_ms();
    if (!nav_indicator.painted) {
        nav_indicator.tween.value = nav_indicator.tween.from = nav_indicator.tween.target = 1;
        nav_indicator.from_rel = target;
        nav_indicator.selected = selected->name;
    } else if (nav_indicator.selected != selected->name) {
        nav_indicator.selected = selected->name;
        nav_indicator.from_rel = nav_indicator.last_rel;
        nav_indicator.tween.jump(0);
    }
    if (nav_indicator.tween.step(1, nav_indicator_ms, now))
        windowing::redraw(window);
    float t = nav_indicator.tween.value;
    float e = 1 - (1 - t) * (1 - t) * (1 - t); // ease out
    auto &f = nav_indicator.from_rel;
    Bounds r(f.x + (target.x - f.x) * e, f.y + (target.y - f.y) * e, f.w + (target.w - f.w) * e, f.h + (target.h - f.h) * e);
    nav_indicator.last_rel = r;
    nav_indicator.painted = true;

    Bounds b(l.x + r.x, l.y + r.y, r.w, r.h);
    drawRoundedRect(cr, b.x, b.y, b.w, b.h, 5 * dpi, 1.0);
    set_argb(cr, swifi_mix(theme.background, theme.text, .075));
    cr->fill();
    // The pill stretches along the way, like Windows 11's.
    double stretch = std::sin(e * M_PI) * std::min(std::abs(target.y - f.y), 40.0 * dpi) * .5;
    double pill_h = 16 * dpi + stretch;
    double pill_w = 3 * dpi;
    cr->new_path();
    drawRoundedRect(cr, b.x, b.y + (b.h - pill_h) * .5, pill_w, pill_h, pill_w * .5, 0.0);
    set_argb(cr, theme.accent);
    cr->fill();
}

static void create_category_option(Container *parent, const SettingsCategory &category) {
    auto c = parent->child(::hbox, FILL_SPACE, FILL_SPACE);
    c->name = "category:" + category.name;
    c->pre_layout = [](Container *root, Container *c, const Bounds &b) {
        c->wanted_bounds.h = 40 * swifi_window(root)->dpi;
    };
    c->when_paint = [&category](Container *root, Container *c) {
        auto window = swifi_window(root);
        auto cr = window->drawing_context;
        auto dpi = window->dpi;
        auto b = c->real_bounds;
        b.shrink(3 * dpi);
        // Over the selection indicator, so it is translucent.
        if (double tint = settings_nav_hover_tint(c); tint > 0) {
            drawRoundedRect(cr, b.x, b.y, b.w, b.h, 5 * dpi, 1.0);
            RGBA overlay = theme.text;
            overlay.a *= tint;
            set_argb(cr, overlay);
            cr->fill();
        }
        int size = (int) std::round(20 * dpi);
        if (auto icon = settings_category_icon(category, size)) {
            cr->set_source_surface(icon, std::round(b.x + 13 * dpi), std::round(b.y + (b.h - size) * .5));
            cr->paint_source();
        }
        Bounds text(b.x + 46 * dpi, b.y, std::max(1.0, b.w - 50 * dpi), b.h);
        swifi_text(root, text, category.name, 11, theme.text);
    };
    c->when_clicked = [&category](Container *root, Container *) {
        // A category with several pages always opens its overview.
        settings_show(root, category.name);
    };
    focus_register(c, {
        .ring = [](Container *root, Container *c) {
            auto b = c->real_bounds;
            b.shrink(3 * swifi_window(root)->dpi);
            return b;
        },
        .radius = 5,
    });
}

void fill_left(Container *left) {
    make_search_field(left);
    auto gap = left->child(FILL_SPACE, FILL_SPACE);
    gap->pre_layout = [](Container *root, Container *c, const Bounds &) {
        c->wanted_bounds.h = 6 * swifi_window(root)->dpi;
    };
    for (const auto &category : settings_categories())
        create_category_option(left, category);
}

// One link on a category overview: icon, title, description and a chevron.
static void make_category_link(Container *parent, const SettingsPageInfo &page) {
    auto row = parent->child(FILL_SPACE, FILL_SPACE);
    row->parent_bounds_limit_input_bounds = false;
    row->receive_events_even_if_obstructed = true;
    row->pre_layout = [](Container *root, Container *c, const Bounds &) {
        c->wanted_bounds.h = 68 * swifi_window(root)->dpi;
    };
    row->when_paint = [&page](Container *root, Container *c) {
        auto window = swifi_window(root);
        auto cr = window->drawing_context;
        auto dpi = window->dpi;
        auto b = c->real_bounds;
        RGBA bg = c->state.mouse_pressing ? swifi_mix(theme.row_background, theme.background, .5) :
            c->state.mouse_hovering ? theme.row_hover : theme.row_background;
        paint_smooth_frame(cr, b, 5 * dpi, bg, theme.row_border, std::max(1.0, (double) std::round(1 * dpi)));
        auto gb = draw_text(cr, 0, 0, page.glyph, 16 * dpi, false, icon_font, -1, -1, theme.text, false);
        draw_text(cr, b.x + 20 * dpi, b.y + (b.h - gb.h) * .5, page.glyph, 16 * dpi, true, icon_font, -1, -1, theme.text, false);
        swifi_two_lines(root, Bounds(b.x + 60 * dpi, b.y, std::max(1.0, b.w - 110 * dpi), b.h), page.title, page.description);
        const std::string chevron = ""; // ChevronRight
        auto cb = draw_text(cr, 0, 0, chevron, 10 * dpi, false, icon_font, -1, -1, theme.text, false);
        draw_text(cr, b.x + b.w - 24 * dpi - cb.w, b.y + (b.h - cb.h) * .5, chevron, 10 * dpi, true, icon_font, -1, -1,
                  theme.text_secondary, false);
    };
    row->when_clicked = [&page](Container *root, Container *) {
        settings_show(root, page.id);
    };
    focus_register(row, {.radius = 5});
}

static void fill_category_page(Container *root, const SettingsCategory &category) {
    auto right = container_by_name("settings_right", root);
    if (!right)
        return;
    for (auto child : right->children)
        delete child;
    right->children.clear();

    right->pre_layout = layout_right_page;
    auto padded_right = right->child(FILL_SPACE, FILL_SPACE);
    padded_right->name = "category_page";
    for (const auto &page : category.pages) {
        make_category_link(padded_right, page);
        make_vert_space(padded_right, 4);
    }
    make_vert_space(padded_right, 16);
}

/*
 * Page slide: moving between a category overview and one of its pages slides the
 * area below the title. The page being left is kept (in `outgoing`, already laid out)
 * and painted into a layer that slides and fades away, while the new page is painted
 * into a layer that slides and fades in from the other side.
 */
struct PageSlide {
    Container *outgoing = nullptr; // Holder of the old page's containers, or null
    int dir = 0;                   // 1 into a page, -1 back to the overview, 0 idle
    long start = 0;                // Set on the first frame, so slow page builds don't eat the animation
    float eased = 1;               // This frame's progress
    bool layer_open = false;       // The incoming page's layer was pushed this frame
};
static PageSlide page_slide;
static constexpr float page_slide_ms = 260;
static constexpr float page_slide_distance = 56;

static void page_slide_finish() {
    delete page_slide.outgoing;
    page_slide.outgoing = nullptr;
    page_slide.dir = 0;
    page_slide.start = 0;
}

// 0 for a category overview, 1 for a page (single-page categories open their page).
static int settings_view_depth(const std::string &view) {
    auto category = settings_category_of(view);
    if (!category)
        return -1;
    return view == category->name && category->pages.size() > 1 ? 0 : 1;
}

// Called before the new page is built: keeps the current one around to slide away.
static void page_slide_begin(Container *root, const std::string &from, const std::string &to) {
    auto from_category = settings_category_of(from);
    int from_depth = settings_view_depth(from);
    int to_depth = settings_view_depth(to);
    int dir = 0;
    if (from_category && from_category == settings_category_of(to) && from_depth >= 0 && to_depth >= 0)
        dir = to_depth - from_depth;
    auto right = container_by_name("settings_right", root);
    if (dir == 0 || !right || !trim(search_query).empty()) {
        page_slide_finish();
        return;
    }
    page_slide_finish();
    auto holder = new Container;
    holder->name = "settings_outgoing";
    holder->real_bounds = right->real_bounds;
    holder->children = std::move(right->children);
    right->children.clear();
    for (auto child : holder->children)
        child->parent = holder;
    page_slide.outgoing = holder;
    page_slide.dir = dir;
}

// settings_right's when_paint: paints the old page sliding out, then opens the layer
// the new page (its children) paints into.
static void page_slide_paint_before(Container *root, Container *c) {
    if (page_slide.dir == 0)
        return;
    auto window = swifi_window(root);
    auto cr = window->drawing_context;
    long now = get_current_time_in_ms();
    if (page_slide.start == 0)
        page_slide.start = now;
    float t = std::clamp((float) (now - page_slide.start) / page_slide_ms, 0.0f, 1.0f);
    if (t >= 1) {
        page_slide_finish();
        return;
    }
    float e = 1 - (1 - t) * (1 - t) * (1 - t); // ease out
    page_slide.eased = e;
    double distance = page_slide_distance * window->dpi * page_slide.dir;

    cr->save();
    cr->new_path();
    cr->rectangle(c->real_bounds.x, c->real_bounds.y, c->real_bounds.w, c->real_bounds.h);
    cr->clip_path();
    if (page_slide.outgoing) {
        cr->push_group();
        cr->save();
        cr->translate(-distance * e, 0);
        paint_outline(root, page_slide.outgoing);
        cr->restore();
        cr->pop_group_to_source();
        // Gone a bit before the new page arrives, so the two don't muddle.
        cr->paint_source(std::max(0.0f, 1 - e * 1.4f));
    }
    cr->push_group();
    cr->save();
    cr->translate(distance * (1 - e), 0);
    page_slide.layer_open = true;
    windowing::redraw(window);
}

// settings_right's after_paint, before the scrollbar: composites the new page's layer.
static void page_slide_paint_after(Container *root, Container *) {
    if (!page_slide.layer_open)
        return;
    page_slide.layer_open = false;
    auto cr = swifi_window(root)->drawing_context;
    cr->restore();
    cr->pop_group_to_source();
    cr->paint_source(page_slide.eased);
    cr->restore();
}

static bool settings_show(Container *root, const std::string &id) {
    auto category = settings_category_of(id);
    if (!category)
        return false;
    std::string page = id;
    if (id == category->name)
        page = category->pages.size() == 1 ? category->pages[0].id : "";
    std::string view = page.empty() ? category->name : page;

    settings_nav_open = false;
    // A different page starts at its top.
    if (view != search_last_page) {
        if (auto right = container_by_name("settings_right", root)) {
            auto d = (RightData *) right->user_data;
            d->scroll = 0;
            d->smooth.stop();
        }
    }
    sdt_invalidate();
    sshortcut_stop_editing();
    page_slide_begin(root, search_last_page, view);
    search_clear_field(root);
    search_last_page = view;
    settings_current_category = category->name;
    if (page.empty()) {
        fill_category_page(root, *category);
        swifi_redraw(root);
        return true;
    }
    search_current_page = page;
    search_current_section.clear();
    if (page == "Mouse & Touchpad") {
        fill_mouse_settings(root, nullptr);
    } else if (page == "Keyboard") {
        fill_keyboard_settings(root, nullptr);
    } else if (page == "Desktop") {
        fill_desktop_settings(root);
    } else if (page == "Dock") {
        fill_dock_settings(root);
    } else if (page == "Multitasking") {
        fill_multitasking_settings(root);
    } else if (page == "Autostart") {
        fill_autostart_settings(root);
    } else if (page == "SDDM") {
        fill_sddm_settings(root);
    } else if (page == "Display") {
        fill_display_settings(root);
    } else if (page == "Shortcuts") {
        fill_shortcuts_settings(root);
    } else if (page == "Wifi") {
        fill_wifi_settings(root);
    } else if (page == "Bluetooth") {
        fill_bluetooth_settings(root);
    } else if (page == "Audio") {
        fill_audio_settings(root);
    } else if (page == "Time & Date") {
        fill_datetime_settings(root);
    } else if (page == "Battery") {
        fill_battery_settings(root);
    }
    search_current_page.clear();
    swifi_redraw(root);
    return true;
}

// Title above the page: the category, then the page when the category has several.
struct SettingsCrumbs {
    std::string parent; // Clickable, empty when there is none
    std::string title;
};

static SettingsCrumbs settings_crumbs() {
    SettingsCrumbs crumbs;
    auto category = settings_category_of(search_last_page);
    if (!category)
        return crumbs;
    if (search_last_page == category->name || category->pages.size() == 1) {
        crumbs.title = category->name;
    } else {
        crumbs.parent = category->name;
        crumbs.title = settings_page_title(search_last_page);
    }
    return crumbs;
}

static double settings_bold_text_width(Container *root, const std::string &text, double size) {
    auto window = swifi_window(root);
    auto font = get_cached_pango_font(window->drawing_context, set->font, size * window->dpi, PANGO_WEIGHT_BOLD, false);
    pango_layout_set_width(font, -1);
    pango_layout_set_height(font, -1);
    pango_layout_set_ellipsize(font, PANGO_ELLIPSIZE_NONE);
    pango_layout_set_text(font, text.c_str(), -1);
    int width = 0, height = 0;
    pango_layout_get_pixel_size(font, &width, &height);
    return width;
}

static const std::string settings_crumb_separator = "  ›  ";

// Width of the clickable parent crumb, 0 when there is none.
static double settings_crumb_parent_w(Container *root, const SettingsCrumbs &crumbs, double size) {
    return crumbs.parent.empty() ? 0 : settings_bold_text_width(root, crumbs.parent, size);
}

static void paint_settings_crumbs(Container *root, Bounds b, double size, bool parent_hovered) {
    auto crumbs = settings_crumbs();
    if (crumbs.title.empty())
        return;
    double x = b.x;
    if (!crumbs.parent.empty()) {
        double w = settings_crumb_parent_w(root, crumbs, size);
        swifi_text(root, Bounds(x, b.y, w + 2, b.h), crumbs.parent, size,
                   parent_hovered ? theme.text : theme.text_secondary, false, true);
        x += w;
        double sw = settings_bold_text_width(root, settings_crumb_separator, size);
        swifi_text(root, Bounds(x, b.y, sw + 2, b.h), settings_crumb_separator, size, theme.text_secondary, false, true);
        x += sw;
    }
    swifi_text(root, Bounds(x, b.y, std::max(1.0, b.x + b.w - x), b.h), crumbs.title, size, theme.text, false, true);
}

static bool settings_crumb_parent_hit(Container *root, Container *c, double x_offset, double size) {
    auto crumbs = settings_crumbs();
    if (crumbs.parent.empty())
        return false;
    double x = c->real_bounds.x + x_offset;
    return root->mouse_current_x >= x && root->mouse_current_x <= x + settings_crumb_parent_w(root, crumbs, size);
}

static constexpr double settings_header_size = 18;

static void make_settings_header(Container *parent) {
    auto header = parent->child(FILL_SPACE, FILL_SPACE);
    header->name = "settings_header";
    header->z_index = 1; // Covers page rows scrolled up past the top
    header->when_paint = [](Container *root, Container *c) {
        auto window = swifi_window(root);
        auto dpi = window->dpi;
        auto b = c->real_bounds;
        set_argb(window->drawing_context, theme.background);
        set_rect(window->drawing_context, b);
        window->drawing_context->fill();
        // Lined up with the page's rows.
        bool hovered = c->state.mouse_hovering && settings_crumb_parent_hit(root, c, 32 * dpi, settings_header_size);
        paint_settings_crumbs(root, Bounds(b.x + 32 * dpi, b.y + 8 * dpi, std::max(1.0, b.w - 48 * dpi), b.h - 8 * dpi),
                              settings_header_size, hovered);
    };
    header->when_mouse_motion = [](Container *root, Container *) { swifi_redraw(root); };
    header->when_mouse_leaves_container = [](Container *root, Container *) { swifi_redraw(root); };
    header->when_clicked = [](Container *root, Container *c) {
        auto dpi = swifi_window(root)->dpi;
        if (settings_crumb_parent_hit(root, c, 32 * dpi, settings_header_size))
            settings_show(root, settings_crumbs().parent);
    };
    focus_skip(header); // Only the parent crumb under the pointer is a link
}

static drawing::Context *cr_of(Container *root) {
    return swifi_window(root)->drawing_context;
}

// The page list's scrollbar: a thin thumb in a strip along the list's right edge,
// wider while hovered or dragged. Clicking the track jumps there.
static constexpr double nav_bar_strip = 12;

struct NavBarGeometry {
    double x = 0, track_y = 0, track_h = 0, thumb_y = 0, thumb_h = 0;
};

static bool nav_bar_geometry(Container *left, double dpi, NavBarGeometry &g) {
    if (nav_scroll.overflow <= 0)
        return false;
    auto b    = left->real_bounds;
    g.x       = b.x + b.w - nav_bar_strip * dpi;
    g.track_y = b.y + 4 * dpi;
    g.track_h = std::max(1.0, b.h - 8 * dpi);
    double content = b.h + nav_scroll.overflow;
    g.thumb_h = std::min(g.track_h, std::max(30.0 * dpi, g.track_h * b.h / content));
    g.thumb_y = g.track_y + (-nav_scroll.scroll / nav_scroll.overflow) * (g.track_h - g.thumb_h);
    return true;
}

static bool nav_bar_hit(Container *root, Container *left) {
    NavBarGeometry g;
    if (!nav_bar_geometry(left, swifi_window(root)->dpi, g))
        return false;
    auto b = left->real_bounds;
    return root->mouse_current_x >= g.x && root->mouse_current_x <= b.x + b.w &&
        root->mouse_current_y >= b.y && root->mouse_current_y <= b.y + b.h;
}

// Moves the list so the thumb top lands at `thumb_y`.
static void nav_bar_scroll_to(Container *root, Container *left, double thumb_y) {
    NavBarGeometry g;
    if (!nav_bar_geometry(left, swifi_window(root)->dpi, g))
        return;
    double room = g.track_h - g.thumb_h;
    double t    = room > 0 ? std::clamp((thumb_y - g.track_y) / room, 0.0, 1.0) : 0.0;
    nav_scroll.smooth.stop();
    nav_scroll.scroll = (float) (-t * nav_scroll.overflow);
    swifi_redraw(root);
}

// Lays out the page list in `b` and offsets its entries by nav_scroll.
static void layout_settings_left(Container *root, Container *left, const Bounds &b) {
    auto measure = [root, left, &b]() {
        layout(root, left, b);
        double bottom = left->real_bounds.y;
        for (auto ch : left->children)
            if (ch->exists)
                bottom = std::max(bottom, ch->real_bounds.y + ch->real_bounds.h);
        double content = bottom + left->wanted_pad.h - left->real_bounds.y;
        nav_scroll.overflow = (float) std::max(0.0, std::floor(content - left->real_bounds.h));
    };
    measure();
    if (nav_scroll.overflow > 0) {
        // Leave the scrollbar its own strip so grabbing it doesn't click an entry.
        left->wanted_pad.w = nav_bar_strip * swifi_window(root)->dpi;
        measure();
    }
    if (nav_scroll.smooth.step(nav_scroll.scroll))
        swifi_redraw(root);
    nav_scroll.scroll = std::clamp(nav_scroll.scroll, -nav_scroll.overflow, 0.0f);
    if (nav_scroll.scroll != 0)
        for (auto ch : left->children)
            modify_all(ch, 0, nav_scroll.scroll);
}

// Lays out left_right's children (it is fullycustom).
// Wide windows show the page list beside the page. Narrow ones replace it with
// a hamburger bar above the page; the list then opens over the page from there.
static void place_settings_columns(Container *root, Container *c, const Bounds &b) {
    auto left  = container_by_name("settings_left", c);
    auto scrim = container_by_name("settings_nav_scrim", c);
    auto bar   = container_by_name("settings_nav_bar", c);
    auto right = container_by_name("settings_right", c);
    auto header = container_by_name("settings_header", c);
    if (!left || !scrim || !bar || !right || !header)
        return;
    auto dpi        = swifi_window(root)->dpi;
    bool was_narrow = settings_narrow;
    settings_narrow = b.w < (settings_left_w + settings_page_min_w) * dpi;
    auto field      = search_field(root);
    if (!settings_narrow) {
        settings_nav_open = false;
    } else if (!was_narrow) {
        // Shrunk while typing in the search field: keep the field on screen.
        settings_nav_open = field && field->active;
    }
    bool searching = settings_narrow && settings_nav_open && !trim(search_query).empty();
    // While searching, only the search field stays open, so the results show.
    for (int i = 1; i < (int) left->children.size(); i++)
        left->children[i]->exists = !searching;

    if (!settings_narrow) {
        double left_w = settings_left_w * dpi;
        left->exists  = true;
        scrim->exists = false;
        bar->exists   = false;
        left->wanted_pad = Bounds(4 * dpi, 4 * dpi, 4 * dpi, 4 * dpi);
        layout_settings_left(root, left, Bounds(b.x, b.y, left_w, b.h));
        Bounds page(b.x + left_w, b.y, std::max(0.0, b.w - left_w), b.h);
        // The search results carry their own title.
        header->exists = trim(search_query).empty() && !settings_crumbs().title.empty();
        if (header->exists) {
            double header_h = 56 * dpi;
            layout(root, header, Bounds(page.x, page.y, page.w, header_h));
            page.y += header_h;
            page.h = std::max(0.0, page.h - header_h);
        }
        layout(root, right, page);
        return;
    }
    header->exists = false;

    double bar_h = 48 * dpi;
    bar->exists      = true;
    bar->wanted_pad  = Bounds(6 * dpi, 4 * dpi, 6 * dpi, 4 * dpi);
    bar->alignment   = container_alignment::ALIGN_CENTER;
    layout(root, bar, Bounds(b.x, b.y, b.w, bar_h));
    Bounds below(b.x, b.y + bar_h, b.w, std::max(0.0, b.h - bar_h));
    left->exists     = settings_nav_open;
    left->wanted_pad = Bounds(4 * dpi, 4 * dpi, 4 * dpi, 4 * dpi);
    if (searching) {
        double field_h = 48 * dpi;
        scrim->exists  = false;
        layout_settings_left(root, left, Bounds(below.x, below.y, below.w, field_h));
        layout(root, right, Bounds(below.x, below.y + field_h, below.w, std::max(0.0, below.h - field_h)));
        return;
    }
    layout(root, right, below);
    scrim->exists = settings_nav_open;
    if (settings_nav_open) {
        layout(root, scrim, below);
        layout_settings_left(root, left, Bounds(below.x, below.y, std::min(below.w, 260.0 * dpi), below.h));
    }
}

/*
 * Keyboard focus, continued (see FocusSpec): moving between stops, the keys,
 * and the ring.
 */

static bool focus_is_stop(Container *c) {
    auto spec = focus_spec(c);
    if (spec && spec->skip)
        return false;
    return (spec || c->when_clicked) && c->interactable;
}

// Every stop under `c` in tree order, shown or not.
static void focus_collect(Container *c, std::vector<Container *> &out) {
    if (focus_is_stop(c))
        out.push_back(c);
    auto &children = c->type == ::newscroll ? ((ScrollContainer *) c)->content->children : c->children;
    for (auto child : children)
        focus_collect(child, out);
}

static bool focus_inside(Container *c, Container *ancestor) {
    for (auto p = c; p; p = p->parent)
        if (p == ancestor)
            return true;
    return false;
}

// Laid out and not hidden by itself or a parent up to `root`.
static bool focus_shown(Container *root, Container *c) {
    if (c->real_bounds.w <= 0 || c->real_bounds.h <= 0)
        return false;
    for (auto p = c; p; p = p->parent) {
        if (!p->exists)
            return false;
        if (p == root)
            return true;
    }
    return false;
}

// The focused container while it is alive and part of this window.
static Container *focus_get(Container *root) {
    auto &f = focus_of(root);
    if (f.life.expired() || !focus_inside(f.current, root))
        return nullptr;
    return f.current;
}

static Bounds focus_ring_bounds(Container *root, Container *c) {
    auto spec = focus_spec(c);
    return spec && spec->ring ? spec->ring(root, c) : c->real_bounds;
}

// Scrolls the page so a focused row is fully visible.
static void focus_scroll_into_view(Container *root, Container *c) {
    auto left = container_by_name("settings_left", root);
    if (left && c != left && focus_inside(c, left)) {
        auto lb = left->real_bounds;
        auto b = focus_ring_bounds(root, c);
        double margin = 4 * swifi_window(root)->dpi;
        float scroll = nav_scroll.scroll;
        if (b.y - margin < lb.y)
            scroll += (float) (lb.y - (b.y - margin));
        else if (b.y + b.h + margin > lb.y + lb.h)
            scroll -= (float) (b.y + b.h + margin - (lb.y + lb.h));
        scroll = std::clamp(scroll, -nav_scroll.overflow, 0.0f);
        if (scroll != nav_scroll.scroll)
            nav_scroll.smooth.start(nav_scroll.scroll, scroll);
        return;
    }
    auto right = container_by_name("settings_right", root);
    if (!right || !focus_inside(c, right) || c == right)
        return;
    auto d = (RightData *) right->user_data;
    auto dpi = swifi_window(root)->dpi;
    auto b = focus_ring_bounds(root, c);
    double margin = 16 * dpi;
    double top = b.y - right->real_bounds.y;
    double bottom = top + b.h;
    float scroll = d->applied_scroll;
    if (top < margin)
        scroll += (float) (margin - top);
    else if (bottom > right->real_bounds.h - margin)
        scroll -= (float) std::min(bottom - (right->real_bounds.h - margin), top - margin);
    scroll = std::clamp(scroll, -d->overflow, 0.0f);
    if (scroll == d->applied_scroll)
        return;
    d->scroll = d->applied_scroll;
    d->smooth.start(d->applied_scroll, scroll);
    right_bar_touch(right);
}

static void focus_set(Container *root, Container *c, bool show) {
    auto &f = focus_of(root);
    f.current = c;
    f.life = c->lifetime;
    f.reveal_first = false;
    f.into_page = false;
    if (show)
        f.visible = true;
    // Text fields take typing while active; focusing anything else leaves them.
    set_active(root, {c}, root, true, true);
    focus_scroll_into_view(root, c);
    swifi_redraw(root);
}

// Moves to the next (step 1) or previous (step -1) shown stop after `from`, wrapping.
static void focus_move(Container *root, Container *from, int step) {
    auto &f = focus_of(root);
    bool main_window = &f == &focus_main;
    // The page list opened over a narrow window keeps focus to itself.
    Container *scope = root;
    if (main_window && settings_narrow && settings_nav_open)
        if (auto left = container_by_name("settings_left", root))
            scope = left;
    std::vector<Container *> stops;
    focus_collect(scope, stops);
    int count = (int) stops.size();
    if (count == 0)
        return;
    int start = -1;
    if (from) {
        auto it = std::find(stops.begin(), stops.end(), from);
        if (it != stops.end())
            start = (int) (it - stops.begin());
    }
    if (start < 0 && f.into_page && step > 0) {
        // Continue at the top of the page that replaced the focused stop.
        if (auto right = container_by_name("settings_right", root)) {
            for (auto stop : stops) {
                if (focus_inside(stop, right) && focus_shown(root, stop)) {
                    focus_set(root, stop, true);
                    return;
                }
            }
        }
    }
    if (start < 0)
        start = step > 0 ? -1 : count;
    for (int i = 1; i <= count; i++) {
        int index = ((start + step * i) % count + count) % count;
        if (focus_shown(root, stops[index])) {
            focus_set(root, stops[index], true);
            return;
        }
    }
}

// A press focuses the stop under the pointer (so Tab continues from there) and hides the ring.
static void focus_mouse_pressed(Container *root, int x, int y) {
    auto &f = focus_of(root);
    bool redraw = f.visible;
    f.visible = false;
    f.reveal_first = false;
    f.into_page = false;
    auto pierced = pierced_containers(root, x, y);
    if (!pierced.empty()) {
        for (auto p = pierced[0]; p; p = p->parent) {
            if (focus_is_stop(p)) {
                f.current = p;
                f.life = p->lifetime;
                break;
            }
        }
    }
    if (redraw)
        swifi_redraw(root);
}

// Keys for one window (the settings window or a dialog), before its containers
// see them. True when used up.
static bool focus_key(Container *root, xkb_keysym_t sym, int mods, bool pressed) {
    if (!pressed)
        return false;
    auto &f = focus_of(root);
    bool main_window = &f == &focus_main;
    bool tab = sym == XKB_KEY_Tab || sym == XKB_KEY_ISO_Left_Tab;
    bool shift = (mods & MOD_SHIFT) || sym == XKB_KEY_ISO_Left_Tab;
    bool others = mods & (MOD_CTRL | MOD_ALT | MOD_SUPER);
    auto c = focus_get(root);

    if (sym == XKB_KEY_Escape) {
        // Also reaches the containers, where it cancels what is going on (or closes the dialog).
        bool changed = f.visible;
        f.visible = false;
        f.reveal_first = false;
        if (main_window && settings_narrow && settings_nav_open) {
            settings_nav_open = false;
            changed = true;
        }
        if (changed)
            swifi_redraw(root);
        return false;
    }
    if (tab && !others) {
        if (c && !f.visible && f.reveal_first && focus_shown(root, c)) {
            f.visible = true;
            f.reveal_first = false;
            focus_scroll_into_view(root, c);
            swifi_redraw(root);
            return true;
        }
        focus_move(root, c, shift ? -1 : 1);
        return true;
    }
    // A hidden stop (a row's buttons swapped out) still orders Tab, but takes no keys.
    if (!c || !focus_shown(root, c) || others || (mods & MOD_SHIFT))
        return false;
    auto spec = focus_spec(c);
    if (spec && spec->text)
        return false; // Fields keep Enter, Space and the arrows
    int dx = sym == XKB_KEY_Left ? -1 : sym == XKB_KEY_Right ? 1 : 0;
    int dy = sym == XKB_KEY_Up ? -1 : sym == XKB_KEY_Down ? 1 : 0;
    if (dx || dy) {
        f.reveal_first = false;
        auto arrow = spec ? spec->arrow : nullptr;
        if (arrow && arrow(root, c, dx, dy)) {
            f.visible = true;
            focus_scroll_into_view(root, c);
            swifi_redraw(root);
            return true;
        }
        focus_move(root, c, dx + dy > 0 ? 1 : -1);
        return true;
    }
    if (sym == XKB_KEY_Return || sym == XKB_KEY_KP_Enter || sym == XKB_KEY_space) {
        // Copied: activating can delete the container and its spec.
        auto activate = spec && spec->activate ? spec->activate : c->when_clicked;
        if (activate)
            activate(root, c);
        // The dialog may have closed (its state is reset when the next one opens).
        if (main_window && !focus_get(root))
            focus_main.into_page = true;
        swifi_redraw(root);
        return true;
    }
    return false;
}

// Keys that reached the settings window: an open dropdown menu takes the
// navigation keys, an open dialog gets its own focus handling.
static bool focus_settings_key(Container *root, xkb_keysym_t sym, int mods, bool pressed) {
    if (dropdown_popup && dropdown_menu_list)
        return pressed && dropdown_menu_key(sym);
    if (dropdown_popup)
        return focus_key(dropdown_popup->root, sym, mods, pressed);
    return focus_key(root, sym, mods, pressed);
}

// Runs after the columns are laid out.
static void focus_after_layout(Container *root) {
    if (!focus_pending_row)
        return;
    auto row = focus_pending_row;
    focus_pending_row = nullptr;
    if (focus_pending_row_life.expired())
        return;
    std::vector<Container *> stops;
    focus_collect(row, stops);
    for (auto stop : stops) {
        if (focus_shown(root, stop)) {
            focus_set(root, stop, false);
            focus_main.reveal_first = !focus_main.visible;
            return;
        }
    }
}

static void paint_focus_ring(Container *root, Container *) {
    auto &f = focus_of(root);
    if (!f.visible || (&f == &focus_main && page_slide.outgoing))
        return;
    auto c = focus_get(root);
    if (!c || !focus_shown(root, c))
        return;
    auto window = swifi_window(root);
    auto cr = window->drawing_context;
    auto dpi = window->dpi;
    // Rows scrolled under the page title stay hidden.
    Bounds clip = root->real_bounds;
    for (auto p = c->parent; p; p = p->parent) {
        if (p->name == "settings_right" || p->name == "settings_left") {
            clip = p->real_bounds;
            break;
        }
    }
    auto spec = focus_spec(c);
    double line = std::max(1.0, (double) std::round(2 * dpi));
    double gap = std::round(2 * dpi);
    double radius = (spec && spec->radius >= 0 ? spec->radius : settings_control_rounding) * dpi + gap + line * .5;
    auto b = focus_ring_bounds(root, c);
    b.grow(gap + line);
    cr->save();
    cr->rectangle(clip.x, clip.y, clip.w, clip.h);
    cr->clip_path();
    drawRoundedRect(cr, b.x, b.y, b.w, b.h + 1, radius, line);
    set_argb(cr, theme.text);
    cr->stroke();
    cr->set_line_width(1);
    cr->restore();
}

// A dialog's starting focus. The ring shows right away when the keyboard opened it.
static void focus_dialog_start(Container *popup_root, Container *c) {
    focus_dialog = FocusState();
    focus_set(popup_root, c, focus_main.visible);
    focus_dialog.reveal_first = !focus_dialog.visible;
}

// Gives a dialog popup the same keys, ring and pointer handling as the settings window.
static void focus_install_dialog(MylarWindow *popup) {
    focus_dialog = FocusState();
    popup->root->after_paint = paint_focus_ring;
    auto forward_key = popup->raw_window->on_key_press;
    popup->raw_window->on_key_press = [popup, forward_key](RawWindow *rw, int key, bool pressed, xkb_keysym_t sym, int mods,
                                                          bool is_text, std::string text) {
        {
            std::lock_guard<std::mutex> lock(rw->creator->mutex);
            if (dropdown_popup == popup) {
                ::layout(popup->root, popup->root, popup->root->real_bounds);
                if (focus_key(popup->root, sym, mods, pressed)) {
                    windowing::redraw(rw);
                    return true;
                }
            }
        }
        return forward_key ? forward_key(rw, key, pressed, sym, mods, is_text, text) : false;
    };
    auto forward_press = popup->raw_window->on_mouse_press;
    popup->raw_window->on_mouse_press = [popup, forward_press](RawWindow *rw, int button, int state, float x, float y) {
        if (state == 1) { // WL_POINTER_BUTTON_STATE_PRESSED
            std::lock_guard<std::mutex> lock(rw->creator->mutex);
            if (dropdown_popup == popup)
                focus_mouse_pressed(popup->root, (int) (x * rw->dpi), (int) (y * rw->dpi));
        }
        return forward_press ? forward_press(rw, button, state, x, y) : false;
    };
}

void fill_root(Container *root) {
    root->when_paint = paint {
        auto mylar = (MylarWindow*)root->user_data;
        auto cr = mylar->raw_window->drawing_context;
        set_argb(cr, theme.background);
        set_rect(cr, c->real_bounds);
        cr->fill();
    };
    root->after_paint = paint_focus_ring;
    auto left_right = root->child(::fullycustom, FILL_SPACE, FILL_SPACE);
    // Handle external navigation on the settings thread, then lay out the page.
    left_right->pre_layout = [](Container *root, Container *c, const Bounds &b) {

        // A search result was picked; open its page.
        if (search_pending_jump >= 0) {
            int result = search_pending_jump;
            search_pending_jump = -1;
            search_jump(root, result);
            settings_nav_open = false;
        }

        std::string page;
        page.swap(requested_page);
        if (!page.empty() && settings_category_of(page)) {
            if (auto right = container_by_name("settings_right", root))
                ((RightData *) right->user_data)->scroll = 0;
            settings_show(root, page);
        }
        // A setting picked from the dock's applications menu.
        search_open_external(root);

        // Rows were added or removed, rebuild from the in-memory state.
        if (sshortcut_page_stale && container_by_name("shortcuts_page", root))
            fill_shortcuts_settings(root, false);
        sshortcut_page_stale = false;
        if (sautostart_page_stale && container_by_name("autostart_page", root))
            fill_autostart_settings(root, false);
        sautostart_page_stale = false;
        if (ssddm_page_stale && container_by_name("sddm_page", root))
            fill_sddm_settings(root);
        ssddm_page_stale = false;
        if (skbd_page_stale && container_by_name("keyboard_page", root))
            fill_keyboard_settings(root, nullptr, false);
        skbd_page_stale = false;

        // Only present while the display page is showing.
        if (display_trial.page_stale && container_by_name("display_page", root))
            fill_display_settings(root);
        else if (display_options_stale && container_by_name("display_page", root))
            change_display_options(display_options_stale);
        display_options_stale = nullptr;

        // Last, so pages rebuilt above are laid out in this pass.
        place_settings_columns(root, c, b);
        if (search_focus_requested.exchange(false)) {
            if (auto field = search_field(root)) {
                if (settings_narrow) {
                    // The field lives in the folded page list.
                    settings_nav_open = true;
                    place_settings_columns(root, c, b);
                }
                focus_set(root, field, false);
            }
        }
        focus_after_layout(root);
    };
    
    // Children come first in hit testing in this order, so the expanded page
    // list must be ahead of the scrim, which must be ahead of the page.
    // z_index: page 0, bar 1, scrim 2, list 3.
    auto left = left_right->child(::vbox, FILL_SPACE, FILL_SPACE);
    left->name = "settings_left";
    left->z_index = 3;
    left->when_paint = paint {
        if (settings_narrow) {
            auto mylar = (MylarWindow*)root->user_data;
            auto cr = mylar->raw_window->drawing_context;
            auto dpi = mylar->raw_window->dpi;
            auto b = c->real_bounds;
            set_argb(cr, theme.background);
            set_rect(cr, b);
            cr->fill();
            double line = std::max(1.0, (double) std::round(1 * dpi));
            set_argb(cr, theme.field_border);
            cr->rectangle(b.x + b.w - line, b.y, line, b.h);
            cr->fill();
        }
        // Entries scrolled out of the list stay hidden; restored in after_paint.
        auto window = swifi_window(root);
        auto b = c->real_bounds;
        window->drawing_context->save();
        window->drawing_context->rectangle(b.x, b.y, b.w, b.h);
        window->drawing_context->clip_path();
        settings_nav_indicator_paint(root, c);
    };
    left->after_paint = [](Container *root, Container *c) {
        auto window = swifi_window(root);
        auto cr = window->drawing_context;
        cr->restore();
        auto dpi = window->dpi;
        NavBarGeometry g;
        if (!nav_bar_geometry(c, dpi, g))
            return;
        bool active = nav_scroll.bar_hovered || nav_scroll.bar_dragging;
        double w = (active ? 6 : 3) * dpi;
        double x = g.x + (nav_bar_strip * dpi - w) * .5;
        RGBA color = theme.text_secondary;
        color.a *= active ? .7 : .4;
        set_argb(cr, color);
        drawRoundedRect(cr, x, g.thumb_y, w, g.thumb_h, w * .5, 1.0);
        cr->fill();
    };
    left->when_mouse_motion = [](Container *root, Container *c) {
        bool hovered = nav_bar_hit(root, c);
        if (hovered != nav_scroll.bar_hovered) {
            nav_scroll.bar_hovered = hovered;
            swifi_redraw(root);
        }
    };
    left->when_mouse_leaves_container = [](Container *root, Container *) {
        if (nav_scroll.bar_hovered && !nav_scroll.bar_dragging) {
            nav_scroll.bar_hovered = false;
            swifi_redraw(root);
        }
    };
    left->when_mouse_down = [](Container *root, Container *c) {
        if (!nav_bar_hit(root, c))
            return;
        NavBarGeometry g;
        if (!nav_bar_geometry(c, swifi_window(root)->dpi, g))
            return;
        nav_scroll.bar_dragging = true;
        nav_scroll.bar_hovered  = true;
        double y = root->mouse_current_y;
        if (y >= g.thumb_y && y <= g.thumb_y + g.thumb_h) {
            nav_scroll.bar_grab = y - g.thumb_y;
        } else {
            nav_scroll.bar_grab = g.thumb_h * .5;
            nav_bar_scroll_to(root, c, y - nav_scroll.bar_grab);
        }
        swifi_redraw(root);
    };
    left->when_drag = [](Container *root, Container *c) {
        if (nav_scroll.bar_dragging)
            nav_bar_scroll_to(root, c, root->mouse_current_y - nav_scroll.bar_grab);
    };
    auto nav_bar_release = [](Container *root, Container *c) {
        if (!nav_scroll.bar_dragging)
            return;
        nav_scroll.bar_dragging = false;
        nav_scroll.bar_hovered  = nav_bar_hit(root, c);
        swifi_redraw(root);
    };
    left->when_mouse_up = nav_bar_release;
    left->when_drag_end = nav_bar_release;
    // Scrolls come to the entry under the pointer first; the list must still get them.
    left->receive_events_even_if_obstructed = true;
    left->when_fine_scrolled = [](Container *root, Container *, double, double scroll_y, bool touchpad) {
        if (scroll_y == 0 || nav_scroll.overflow <= 0)
            return;
        float delta = (float) (scroll_y * 3 * swifi_window(root)->dpi);
        if (touchpad) {
            nav_scroll.smooth.stop();
            nav_scroll.scroll = std::clamp(nav_scroll.scroll + delta, -nav_scroll.overflow, 0.0f);
        } else {
            nav_scroll.smooth.scroll_by(nav_scroll.scroll, delta, -nav_scroll.overflow, 0.0f);
        }
        swifi_redraw(root);
    };
    fill_left(left);

    auto scrim = left_right->child(FILL_SPACE, FILL_SPACE);
    scrim->name = "settings_nav_scrim";
    scrim->z_index = 2;
    scrim->when_paint = paint {
        RGBA dim(0, 0, 0, set->dark_theme ? .35f : .2f);
        set_argb(cr_of(root), dim);
        set_rect(cr_of(root), c->real_bounds);
        cr_of(root)->fill();
    };
    scrim->when_clicked = [](Container *root, Container *) {
        settings_nav_open = false;
        swifi_redraw(root);
    };
    scrim->when_fine_scrolled = [](Container *, Container *, double, double, bool) {};
    focus_skip(scrim);

    auto bar = left_right->child(::hbox, FILL_SPACE, FILL_SPACE);
    bar->name = "settings_nav_bar";
    bar->z_index = 1; // Covers page rows scrolled up past the top
    bar->when_paint = paint {
        auto window = swifi_window(root);
        auto dpi = window->dpi;
        auto b = c->real_bounds;
        set_argb(window->drawing_context, theme.background);
        set_rect(window->drawing_context, b);
        window->drawing_context->fill();
        Bounds title(b.x + 52 * dpi, b.y, std::max(0.0, b.w - 60 * dpi), b.h);
        bool hovered = c->state.mouse_hovering && settings_crumb_parent_hit(root, c, 52 * dpi, 12);
        paint_settings_crumbs(root, title, 12, hovered);
    };
    bar->when_mouse_motion = [](Container *root, Container *) { swifi_redraw(root); };
    bar->when_clicked = [](Container *root, Container *c) {
        if (settings_crumb_parent_hit(root, c, 52 * swifi_window(root)->dpi, 12))
            settings_show(root, settings_crumbs().parent);
    };
    focus_skip(bar);
    auto hamburger = bar->child(FILL_SPACE, FILL_SPACE);
    hamburger->pre_layout = [](Container *root, Container *c, const Bounds &) {
        auto dpi = swifi_window(root)->dpi;
        c->wanted_bounds.w = 40 * dpi;
        c->wanted_bounds.h = 40 * dpi;
    };
    hamburger->when_paint = paint {
        auto window = swifi_window(root);
        auto cr = window->drawing_context;
        auto dpi = window->dpi;
        auto b = c->real_bounds;
        if (c->state.mouse_pressing || c->state.mouse_hovering || settings_nav_open) {
            auto hb = b;
            hb.shrink(2 * dpi);
            drawRoundedRect(cr, hb.x, hb.y, hb.w, hb.h, 5 * dpi, 1.0);
            set_argb(cr, c->state.mouse_pressing ? theme.navigation_pressed : theme.navigation_hover);
            cr->fill();
        }
        const std::string glyph = "\uE700"; // GlobalNavigationButton
        auto gb = draw_text(cr, 0, 0, glyph, 12 * dpi, false, icon_font, -1, -1, theme.text, false);
        draw_text(cr, b.x + (b.w - gb.w) * .5, b.y + (b.h - gb.h) * .5, glyph, 12 * dpi, true, icon_font, -1, -1, theme.text, false);
    };
    hamburger->when_clicked = [](Container *root, Container *) {
        settings_nav_open = !settings_nav_open;
        swifi_redraw(root);
    };

    make_settings_header(left_right);

    auto right = left_right->child(::fullycustom, FILL_SPACE, FILL_SPACE);
    right->name = "settings_right";
    auto d = new RightData;
    right->user_data = d;
    right->when_paint = page_slide_paint_before;
    right->receive_events_even_if_obstructed = true;
    right->when_fine_scrolled = [](Container* root, Container* c, double scroll_x, double scroll_y, bool came_from_touchpad) {
        if (wall_preview_ate_scroll) {
            wall_preview_ate_scroll = false;
            return;
        }
        // The page list opened over the page takes the scrolling.
        if (settings_narrow && settings_nav_open && trim(search_query).empty())
            return;
        // Horizontal-only events (touchpad drift, wheel tilt) and axis_stop
        // frames carry no vertical motion; don't restart the wheel easing.
        if (scroll_y == 0)
            return;
        auto d = (RightData *) c->user_data;
        auto mylar = (MylarWindow*)root->user_data;
        // Clamp against the limits from the last real layout pass.
        float delta = (float) (scroll_y * 3 * mylar->raw_window->dpi);
        if (came_from_touchpad) {
            d->smooth.stop();
            d->scroll = std::clamp(d->applied_scroll + delta, -d->overflow, 0.0f);
        } else {
            d->scroll = d->applied_scroll;
            d->smooth.scroll_by(d->applied_scroll, delta, -d->overflow, 0.0f);
        }
        right_bar_touch(c);
        swifi_redraw(root);
    };
    right->when_mouse_motion = [](Container *root, Container *c) {
        auto d = (RightData *) c->user_data;
        bool hovered = right_bar_hit(root, c);
        if (hovered != d->bar_hovered) {
            d->bar_hovered = hovered;
            right_bar_touch(c);
            swifi_redraw(root);
        }
    };
    right->when_mouse_leaves_container = [](Container *root, Container *c) {
        auto d = (RightData *) c->user_data;
        if (d->bar_hovered && !d->bar_dragging) {
            d->bar_hovered = false;
            right_bar_touch(c);
            swifi_redraw(root);
        }
    };
    right->when_mouse_down = [](Container *root, Container *c) {
        auto d = (RightData *) c->user_data;
        if (!right_bar_hit(root, c))
            return;
        RightBarGeometry g;
        if (!right_bar_geometry(c, swifi_window(root)->dpi, g))
            return;
        d->bar_dragging = true;
        d->bar_hovered  = true;
        double y        = root->mouse_current_y;
        if (y >= g.thumb_y && y <= g.thumb_y + g.thumb_h) {
            d->bar_grab = y - g.thumb_y;
        } else {
            // Clicking the track jumps there, centering the thumb under the mouse.
            d->bar_grab = g.thumb_h * .5;
            right_bar_scroll_to(root, c, y - d->bar_grab);
        }
        right_bar_touch(c);
        swifi_redraw(root);
    };
    right->when_drag = [](Container *root, Container *c) {
        auto d = (RightData *) c->user_data;
        if (!d->bar_dragging)
            return;
        right_bar_scroll_to(root, c, root->mouse_current_y - d->bar_grab);
        right_bar_touch(c);
    };
    auto bar_release = [](Container *root, Container *c) {
        auto d = (RightData *) c->user_data;
        if (!d->bar_dragging)
            return;
        d->bar_dragging = false;
        d->bar_hovered  = right_bar_hit(root, c);
        right_bar_touch(c);
        swifi_redraw(root);
    };
    right->when_mouse_up = bar_release;
    right->when_drag_end = bar_release;
    right->after_paint = [](Container *root, Container *c) {
        page_slide_paint_after(root, c);
        paint_search_flash(root, c);
        paint_right_scrollbar(root, c);
        paint_wallpaper_drop_hint(root, c);
        paint_field_hint(root, c);
    };
}

// Usable space (logical px) on the monitor the settings window opens on, minus
// the titlebar and a small gap. Written on the main thread in settings::start,
// read by the window thread. 0 means unknown.
static std::atomic<int> settings_fit_w = 0;
static std::atomic<int> settings_fit_h = 0;
// Starting size (logical px) matching what heart's restore rules will resize
// the window to once it maps, so the first frame is laid out at its final
// size. 0 means use the defaults.
static std::atomic<int> settings_open_w = 0;
static std::atomic<int> settings_open_h = 0;
// Scale of the monitor the window opens on, so its first frame is painted at
// that scale. 0 means unknown.
static std::atomic<float> settings_open_scale = 0;

static std::mutex settings_lifecycle_mutex;
static std::thread settings_thread;
static bool settings_close_requested = false;
static std::atomic<bool> settings_thread_exited = true;

void actual_start() {
    theme = set->dark_theme ? dark_theme : light_theme;
    auto app = windowing::open_app();
    if (!app) {
        settings_thread_exited = true;
        return;
    }
    {
        std::lock_guard<std::mutex> lock(settings_lifecycle_mutex);
        if (settings_close_requested) {
            windowing::close_app(app);
            windowing::main_loop(app);
            settings_thread_exited = true;
            return;
        }
        settings_app = app;
    }
    RawWindowSettings settings;
    settings.pos.w = 1000;
    settings.pos.h = 760;
    // Narrow windows fold the page list away, so the window can get much
    // smaller than the page's comfortable width (80% under the old minimum).
    settings.pos.min_w = (int) (settings_page_min_w * .8);
    settings.pos.min_h = (int) (600 * .8);
    // On displays too small for the defaults, shrink the minimum and the
    // starting size so the window fits in the usable area.
    int fit_w = settings_fit_w.load();
    int fit_h = settings_fit_h.load();
    if (fit_w > 0) {
        settings.pos.min_w = std::min(settings.pos.min_w, fit_w);
        settings.pos.w = std::min(settings.pos.w, fit_w);
    }
    if (fit_h > 0) {
        settings.pos.min_h = std::min(settings.pos.min_h, fit_h);
        settings.pos.h = std::min(settings.pos.h, fit_h);
    }
    if (settings_open_w.load() > 0 && settings_open_h.load() > 0) {
        settings.pos.w = std::max(settings.pos.min_w, settings_open_w.load());
        settings.pos.h = std::max(settings.pos.min_h, settings_open_h.load());
    }
    settings.name = "Settings";
    settings.app_id = "preferences-system";
    settings.scale_hint = settings_open_scale.load();
    auto mylar = open_mylar_window(settings_app, WindowType::NORMAL, settings);
    if (!mylar) {
        windowing::close_app(app);
        windowing::main_loop(app);
        std::lock_guard lock(settings_lifecycle_mutex);
        settings_app = nullptr;
        delete app;
        settings_thread_exited = true;
        return;
    }
    mylar->root->user_data = mylar;
    settings_mylar = mylar;
    settings_redraw_window = mylar->raw_window;
    fill_root(mylar->root);
    // Ctrl+L clears the search query and puts the cursor in the search field.
    auto forward_key = mylar->raw_window->on_key_press;
    mylar->raw_window->on_key_press = [forward_key](RawWindow *rw, int key, bool pressed, xkb_keysym_t sym, int mods, bool is_text, std::string text) {
        if (sym == XKB_KEY_Control_L || sym == XKB_KEY_Control_R)
            settings_ctrl_held = pressed;
        if ((sym == XKB_KEY_l || sym == XKB_KEY_L) && (mods & MOD_CTRL) && !(mods & ~(MOD_CTRL | MOD_SHIFT))) {
            if (pressed) {
                std::lock_guard<std::mutex> lock(rw->creator->mutex);
                if (settings_mylar) {
                    auto root = settings_mylar->root;
                    search_set_query(root, "");
                    search_clear_field(root);
                    search_focus_requested = true;
                    windowing::redraw(rw);
                }
            }
            return true;
        }
        {
            std::lock_guard<std::mutex> lock(rw->creator->mutex);
            if (settings_mylar) {
                auto root = settings_mylar->root;
                ::layout(root, root, root->real_bounds);
                if (focus_settings_key(root, sym, mods, pressed)) {
                    windowing::redraw(rw);
                    if (dropdown_popup)
                        windowing::redraw(dropdown_popup->raw_window);
                    return true;
                }
            }
        }
        return forward_key ? forward_key(rw, key, pressed, sym, mods, is_text, text) : false;
    };
    auto forward_press = mylar->raw_window->on_mouse_press;
    mylar->raw_window->on_mouse_press = [forward_press](RawWindow *rw, int button, int state, float x, float y) {
        if (state == 1) { // WL_POINTER_BUTTON_STATE_PRESSED
            std::lock_guard<std::mutex> lock(rw->creator->mutex);
            if (settings_mylar)
                focus_mouse_pressed(settings_mylar->root, (int) (x * rw->dpi), (int) (y * rw->dpi));
        }
        return forward_press ? forward_press(rw, button, state, x, y) : false;
    };
    // Files can be dropped anywhere on the Display or SDDM page to set the wallpaper.
    mylar->raw_window->accepts_file_drop = [](RawWindow *rw, float, float) {
        std::lock_guard<std::mutex> lock(rw->creator->mutex);
        return settings_mylar && (container_by_name("display_wallpaper_row", settings_mylar->root) != nullptr ||
                                  container_by_name("sddm_page", settings_mylar->root) != nullptr);
    };
    mylar->raw_window->on_file_drop = [](RawWindow *rw, float, float, std::string uri_list) {
        bool on_sddm = false;
        {
            std::lock_guard<std::mutex> lock(rw->creator->mutex);
            on_sddm = settings_mylar && container_by_name("sddm_page", settings_mylar->root) != nullptr;
        }
        if (on_sddm)
            sddm_file_dropped(uri_list);
        else
            wallpaper_file_dropped(uri_list);
    };
    mylar->raw_window->on_close = [](RawWindow *w) {
        settings_wifi_release();
        settings_bluetooth_release();
        settings_audio_release();
        sshortcut_stop_editing();
        trial_dialog_close();
        dropdown_popup = nullptr;
        for (auto p : popups) {
            windowing::close_window(p->raw_window);
        }
        popups.clear();
        std::this_thread::sleep_for(std::chrono::milliseconds(40));
    };

    display_trial_watch();

    windowing::main_loop(settings_app);
    settings_redraw_window = nullptr;

    settings_wifi_release();
    settings_bluetooth_release();
    settings_audio_release();
    sshortcut_stop_editing();
    trial_dialog_close();
    dropdown_popup = nullptr;
    popups.clear();
    page_slide_finish();
    nav_indicator = NavIndicator();
    focus_main = FocusState();
    focus_dialog = FocusState();
    focus_pending_row = nullptr;
    cleanup_smooth_frames();
    settings_icon_cache_clear();
    cleanup_cached_pango_fonts();
    drawing::cleanup_thread_resources();

    {
        std::lock_guard<std::mutex> lock(settings_lifecycle_mutex);
        settings_app = nullptr;
        settings_mylar = nullptr;
    }
    settings_thread_exited = true;
};

void settings::start(std::string page, bool focus_search) {
    if (plugin_lifetime.stop_requested())
        return;
    if (!page.empty()) {
        requested_page = page;
    } else if (!settings_app) {
        // Opening fresh without a page: show Display rather than a blank page.
        requested_page = "Display";
    }
    if (focus_search)
        search_focus_requested = true;
    if (settings_app) {
        if (settings_mylar)
            windowing::redraw(settings_mylar->raw_window);
        // Already open: pull it to the current workspace, un-minimize, and raise it.
        main_thread([]() {
            auto pid = getpid();
            for (auto cid : get_window_stacking_order()) {
                if (hypriso->get_pid(cid) != pid || hypriso->class_name(cid) != "preferences-system")
                    continue;
                if (!hypriso->is_window_workspace_visible(cid)) {
                    int workspace = hypriso->get_active_workspace(hypriso->monitor_from_cursor());
                    if (workspace != -1)
                        hypriso->move_to_workspace(cid, workspace, false);
                }
                if (hypriso->is_hidden(cid))
                    hypriso->set_hidden(cid, false);
                hypriso->bring_to_front(cid);
            }
        });
        return;
    }
    {
        Bounds area = bounds_reserved_monitor(hypriso->monitor_from_cursor());
        int gap = 20;
        settings_fit_w = area.w > 0 ? std::max(1, (int) area.w - gap * 2) : 0;
        settings_fit_h = area.h > 0 ? std::max(1, (int) area.h - titlebar_h - gap * 2) : 0;

        // heart's apply_restore_info and fit_on_screen resize the window right
        // after it maps. Open at that size instead, otherwise the first frame
        // is painted at the default size and visibly jumps on the next one.
        double w = 0, h = 0;
        auto saved = restore_infos.find("preferences-system");
        if (area.w > 0 && area.h > 0 && saved != restore_infos.end() && saved->second.remember_size &&
            saved->second.box.w > 0 && saved->second.box.h > 0) {
            auto s = scale(hypriso->monitor_from_cursor());
            w = std::min(area.w * saved->second.box.w, std::max(1.0, area.w - 60 * s));
            h = std::min(area.h * saved->second.box.h, std::max(1.0, area.h - 60 * s));
            w = std::min(w, area.w * .85);
            h = std::min(h, area.h * .85);
        }
        settings_open_w = (int) w;
        settings_open_h = (int) h;
        settings_open_scale = scale(hypriso->monitor_from_cursor());
    }
    {
        std::lock_guard<std::mutex> lock(settings_lifecycle_mutex);
        if (settings_thread.joinable()) {
            if (!settings_thread_exited.load())
                return;
            settings_thread.join();
        }
        settings_close_requested = false;
        settings_thread_exited = false;
        settings_thread = std::thread(actual_start);
    }
}

void settings::stop() {
    std::thread to_join;
    {
        std::lock_guard<std::mutex> lock(settings_lifecycle_mutex);
        settings_close_requested = true;
        if (settings_app)
            windowing::close_app(settings_app);
        if (settings_thread.joinable())
            to_join = std::move(settings_thread);
    }
    if (to_join.joinable())
        to_join.join();
    std::vector<std::shared_ptr<SettingsWorker>> workers;
    {
        std::lock_guard<std::mutex> lock(settings_workers_mutex);
        settings_workers_stopping = true;
        workers.swap(settings_workers);
    }
    for (auto &worker : workers) {
        if (worker->thread.joinable())
            worker->thread.join();
    }
    {
        std::lock_guard<std::mutex> lock(settings_lifecycle_mutex);
        settings_close_requested = false;
    }
    settings_workers_stopping = false;
}

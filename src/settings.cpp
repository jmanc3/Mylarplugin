
#include "settings.h"
#include "pango_font_cache.h"
#include "text_editor.h"

#include "container.h"
#include "desktop_icons.h"
#include "heart.h"
#include "icons.h"
#include "hypriso.h"
#include "client/raw_windowing.h"
#include "client/windowing.h"
#include "dock/dock.h"
#include "dock/dock_wifi.h"
#include "audio.h"
#include "events.h"
#include "datetime.h"
#include <chrono>
#include <csetjmp>
#include <hyprland/src/helpers/MiscFunctions.hpp>

#include <gtk/gtk.h>
#include <thread>
#include <atomic>
#include <mutex>
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

static std::string trim_newline(std::string s) {
    s.erase(s.begin(), std::find_if(s.begin(), s.end(),
        [](unsigned char c){ return !std::iscntrl(c); }));
    s.erase(std::find_if(s.rbegin(), s.rend(),
        [](unsigned char c){ return !std::iscntrl(c); }).base(), s.end());
    return s;
}

std::string choose_file(bool folder = false, GtkWindow* parent = nullptr) {
    std::string filename;

    if (!gtk_init_check(nullptr, nullptr))
        return filename;

    GtkFileChooserAction action = folder
        ? GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER
        : GTK_FILE_CHOOSER_ACTION_OPEN;

    const char* accept_label = folder ? "_Select" : "_Open";

    GtkWidget* dialog = gtk_file_chooser_dialog_new(
        folder ? "Select Folder" : "Select File",
        parent,
        action,
        "_Cancel", GTK_RESPONSE_CANCEL,
        accept_label, GTK_RESPONSE_ACCEPT,
        nullptr);

    if (!dialog)
        return filename;

    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
        char* path = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
        if (path) {
            filename = path;
            g_free(path);
        }
    }

    gtk_widget_destroy(dialog);

    // Needed if you're not already running gtk_main()
    while (gtk_events_pending())
        gtk_main_iteration();

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
static const long display_trial_ms = 15000;

struct DisplayTrial {
    std::mutex                mutex; // `confirmed` is read by whichever thread saves settings
    std::vector<MylarMonitor> confirmed;
    std::atomic<bool>         active     = false;
    std::atomic<long>         deadline   = 0;
    std::atomic<bool>         page_stale = false; // the display page shows values that were rolled back
    unsigned                  generation = 0;     // main thread only, invalidates older timers
};
static DisplayTrial display_trial;

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
    bind(float, "cursor_speed", &settings->cursor_speed);
    bind(bool, "natural_scrolling_mouse", &settings->natural_scrolling_mouse);
    bind(bool, "natural_scrolling_touchpad", &settings->natural_scrolling_touchpad);
    bind(bool, "touchpad_disable_while_typing", &settings->touchpad_disable_while_typing);
    bind(int, "repeat_delay", &settings->repeat_delay);
    bind(int, "repeat_rate", &settings->repeat_rate);
    bind(bool, "show_docks", &settings->show_docks);
    bind(bool, "battery_history_enabled", &settings->battery_history_enabled);
    bind(bool, "dock_current_workspace_only", &settings->dock_current_workspace_only);
    bind(bool, "dock_merge_windows", &settings->dock_merge_windows);
    bind(bool, "draw_wallpaper", &settings->draw_wallpaper);
    bind(bool, "dark_theme", &settings->dark_theme);
    bind(bool, "clock_24h", &settings->clock_24h);
    bind(bool, "hotcorners", &settings->hotcorners);
    bind(bool, "app_switcher_current_workspace_only", &settings->app_switcher_current_workspace_only);
    bind(bool, "dynamic_workspaces", &settings->dynamic_workspaces);
    bind(int, "fixed_workspace_count", &settings->fixed_workspace_count);
    settings->fixed_workspace_count = std::clamp(settings->fixed_workspace_count, 1, 20);
    bind(bool, "workspaces_span_monitors", &settings->workspaces_span_monitors);
    bind(bool, "resize_at_screen_edges", &settings->resize_at_screen_edges);
    bind(bool, "desktop_icons", &settings->desktop_icons);
    bind(int, "desktop_vertical_override", &settings->desktop_vertical_override);
    bind(std::string, "desktop_sort_by", &settings->desktop_sort_by);
    bind(bool, "desktop_sort_ascending", &settings->desktop_sort_ascending);
    bind(std::string, "desktop_folder", &settings->desktop_folder);
    bind(std::string, "overview_layout_type", &settings->overview_layout_type);
    bind(std::string, "font", &settings->font);
    bind(bool, "is_tiling", &settings->is_tiling);
    bind(bool, "tile_all_workspaces", &settings->tile_all_workspaces);
    bind(bool, "new_workspace_is_tiling", &settings->new_workspace_is_tiling);
    bind(bool, "active_window_border_hint", &settings->active_window_border_hint);
    bind(int, "tiling_window_gap", &settings->tiling_window_gap);
    settings->tiling_window_gap = std::clamp(settings->tiling_window_gap, 0, 32);

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

static Bounds draw_text(cairo_t *cr, int x, int y, std::string text, int size, bool draw, std::string font, int wrap, int h, RGBA color, bool bold, int align = 0) {
    auto layout = get_cached_pango_font(cr, set->font, size, bold ? PANGO_WEIGHT_BOLD : PANGO_WEIGHT_NORMAL, false);
    
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
    PangoRectangle ink;
    PangoRectangle logical;
    pango_layout_get_pixel_extents(layout, &ink, &logical);
    if (draw) {
        cairo_move_to(cr, std::round(x), std::round(y));
        pango_cairo_show_layout(cr, layout);
    }
    return Bounds(ink.width, ink.height, logical.width, logical.height);
}

static void paint_label(Container *root, Container *c, std::string text) {
    auto mylar = (MylarWindow*)root->user_data;
    auto cr = mylar->raw_window->cr;
    auto dpi = mylar->raw_window->dpi;
    auto size = 12 * dpi;

    auto b = draw_text(cr, 0, 0, text, size, false, set->font, -1, 0, theme.text, false);
    draw_text(cr, 
        c->real_bounds.x + 12 * dpi, 
        c->real_bounds.y + c->real_bounds.h * .5 - b.h * .5, text, size, true, set->font, -1, 0, theme.text,  false);
}

static void drawRoundedRect(cairo_t *cr, double x, double y, double width, double height,
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
    cairo_new_path(cr);
    
    cairo_move_to(cr, adjusted_x + (round_top_left ? adjusted_radius : 0), adjusted_y);
    
    cairo_line_to(cr, adjusted_x + inner_width - (round_top_right ? adjusted_radius : 0), adjusted_y);
    
    if (round_top_right)
        cairo_arc(cr, adjusted_x + inner_width - adjusted_radius, adjusted_y + adjusted_radius,
                  adjusted_radius, -M_PI / 2, 0);
    else
        cairo_line_to(cr, adjusted_x + inner_width, adjusted_y);
    
    cairo_line_to(cr, adjusted_x + inner_width, adjusted_y + inner_height - (round_bottom_right ? adjusted_radius : 0));
    
    if (round_bottom_right)
        cairo_arc(cr, adjusted_x + inner_width - adjusted_radius, adjusted_y + inner_height - adjusted_radius,
                  adjusted_radius, 0, M_PI / 2);
    else
        cairo_line_to(cr, adjusted_x + inner_width, adjusted_y + inner_height);
    
    cairo_line_to(cr, adjusted_x + (round_bottom_left ? adjusted_radius : 0), adjusted_y + inner_height);
    
    if (round_bottom_left)
        cairo_arc(cr, adjusted_x + adjusted_radius, adjusted_y + inner_height - adjusted_radius,
                  adjusted_radius, M_PI / 2, M_PI);
    else
        cairo_line_to(cr, adjusted_x, adjusted_y + inner_height);
    
    cairo_line_to(cr, adjusted_x, adjusted_y + (round_top_left ? adjusted_radius : 0));
    
    if (round_top_left)
        cairo_arc(cr, adjusted_x + adjusted_radius, adjusted_y + adjusted_radius,
                  adjusted_radius, M_PI, 3 * M_PI / 2);
    else
        cairo_line_to(cr, adjusted_x, adjusted_y);
    
    // Close the path
    cairo_close_path(cr);
    
    // Set stroke width and stroke
    cairo_set_line_width(cr, stroke_width);
}

// Creates a container that sizes itself based on children size
// It takes full width of parent
// It lays out right child first and then left with remainder of space
static Container *make_self_height_sized_parent(Container *parent) {
    auto c = parent->child(::absolute, FILL_SPACE, FILL_SPACE);
    c->receive_events_even_if_obstructed = true;
    c->pre_layout = [](Container *root, Container *c, const Bounds &b) {
        auto mylar = (MylarWindow*)root->user_data;
        auto cr = mylar->raw_window->cr;
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
        auto cr = mylar->raw_window->cr;
        auto dpi = mylar->raw_window->dpi;
        auto b = c->real_bounds;

        RGBA bg_color;
        if (c->state.mouse_hovering || c->state.mouse_pressing) {
            bg_color = theme.row_hover;
            drawRoundedRect(cr, b.x, b.y, b.w, b.h, border_rounding * dpi, 1.0);
            set_argb(cr, bg_color);
            cairo_fill(cr);
        } else {
            bg_color = theme.row_background;
            drawRoundedRect(cr, b.x, b.y, b.w, b.h, border_rounding * dpi, 1.0);
            set_argb(cr, bg_color);
            cairo_fill(cr);
        }

        drawRoundedRect(cr, b.x, b.y, b.w, b.h, border_rounding * dpi, 1.0);
        set_argb(cr, theme.row_border);
        cairo_stroke(cr);
    };

    return c;
}

static void make_label_like(Container *parent, std::string title, std::string description, std::string icon = "") {
    auto left = parent->child(FILL_SPACE, FILL_SPACE);
    //static float button_text_pad = 8; 
    left->when_paint = [title, description, icon](Container *root, Container *c) {
        auto mylar = (MylarWindow*)root->user_data;
        auto cr = mylar->raw_window->cr;
        auto dpi = mylar->raw_window->dpi;
        auto size_ico = 16 * dpi;
        auto size_title = 12 * dpi;
        auto size_desc = 11 * dpi;

        auto b = c->real_bounds;
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
            auto bo = draw_text(cr, 0, 0, description, size_desc, false, set->font, c->real_bounds.w - ((optionleftpad + optionrighttpad) * dpi), -1, theme.text, false);
            draw_text(cr,
                c->real_bounds.x + 60 * dpi, 
                c->real_bounds.y + yoff, description, size_desc, true, set->font, c->real_bounds.w - ((optionleftpad + optionrighttpad) * dpi), -1, theme.text_secondary, false);
        }
    };
    left->pre_layout = [title, description, icon](Container *root, Container *c, const Bounds &b) {
        auto mylar = (MylarWindow*)root->user_data;
        auto cr = mylar->raw_window->cr;
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
    static float size = 13;
    section_title->pre_layout = [title](Container *root, Container *c, const Bounds &b) {
        auto mylar = (MylarWindow*)root->user_data;
        auto cr = mylar->raw_window->cr;
        auto dpi = mylar->raw_window->dpi;
        auto size_title = size * dpi;
        auto bo = draw_text(cr, 0, 0, title, size_title, false, set->font, b.w, -1, theme.text, false);
        c->wanted_bounds.h = bo.h;
        c->real_bounds.h = bo.h;
    };
    section_title->when_paint = [title](Container *root, Container *c) {
        auto mylar = (MylarWindow*)root->user_data;
        auto cr = mylar->raw_window->cr;
        auto dpi = mylar->raw_window->dpi;
        auto size_title = size * dpi;
        draw_text(cr,
            c->real_bounds.x, 
            c->real_bounds.y, title, size_title, true, set->font, c->real_bounds.w, -1, theme.text, false);
    };
    return section_title;
}

static void make_bool_control(Container *parent, bool initial_value, std::function<void(bool)> on_change) {
    struct BoolInfo : UserData {
        bool on = false;
    };

    auto right = parent->child(::hbox, FILL_SPACE, FILL_SPACE);
    auto bool_info = new BoolInfo;
    bool_info->on = initial_value;
    right->user_data = bool_info;
    right->when_paint = paint {
        auto mylar = (MylarWindow*)root->user_data;
        auto cr = mylar->raw_window->cr;
        auto dpi = mylar->raw_window->dpi;
        auto b = c->real_bounds;
        auto data = (BoolInfo *) c->user_data;
        
        float data_openess = 0.0; // hovering
        float data_slide_amount = 0.0; // on off left or right toggle side
        float data_slide_open = 0.0;
        float data_open_amount = 0.0;
        if (c->state.mouse_hovering)
            data_openess = 1.0;
        if (data->on) {
            data_slide_amount = 1.0;
            //data_slide_open = 1.0;
            //data_open_amount = 1.0;
        }
        
        float circ_size_small = 12;
        float circ_size_big = 14;
        float circ_anim_time = 70;
        float circ_size = circ_size_small + (circ_size_big - circ_size_small) * data_openess;
        
        float circ_extra_width = 5.5 * data_slide_open;

        auto border = b;

        // Border
        if (data->on) {
            set_rect(cr, border);
            drawRoundedRect(cr, border.x, border.y, border.w, border.h, 10 * dpi, std::floor(1.0 * dpi));
            set_argb(cr, theme.accent);
            cairo_fill(cr);
        } else {
            set_rect(cr, border);
            drawRoundedRect(cr, border.x, border.y, border.w, border.h, 10 * dpi, std::floor(1.0 * dpi));
            set_argb(cr, theme.toggle_border);
            cairo_stroke(cr);
        }

        float left = b.x + 4 * dpi - ((circ_size - circ_size_small) * .5) * dpi;
        float right = b.x + b.w - circ_size * dpi + ((circ_size - circ_size_small) * .5) * dpi - 4 * dpi - circ_extra_width * data_openess;
        float position = (right - left) * data_slide_amount + left;
      
        b.x = position;
        b.y = b.y + b.h / 2 - (circ_size * .5) * dpi;
        b.w = circ_size * dpi + circ_extra_width * data_open_amount;
        b.h = circ_size * dpi;
        // Dot
        if (data->on) {
            set_rect(cr, b);
            drawRoundedRect(cr, b.x, b.y, b.w, b.h, circ_size * .5 * dpi, 1.0);
            set_argb(cr, theme.toggle_knob_on);
            cairo_fill(cr);
        } else {
            set_rect(cr, b);
            drawRoundedRect(cr, b.x, b.y, b.w, b.h, circ_size * .5 * dpi, 1.0);
            set_argb(cr, theme.toggle_knob_off);
            cairo_fill(cr);
        }

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
        auto cr = mylar->raw_window->cr;
        auto dpi = mylar->raw_window->dpi;
        c->wanted_bounds.w = c->real_bounds.w = 40 * dpi;
        c->wanted_bounds.h = c->real_bounds.h = 20 * dpi;
    };
    right->when_clicked = [on_change](Container *root, Container *c) {
        auto data = (BoolInfo *) c->user_data;
        data->on = !data->on;
        if (on_change)
            on_change(data->on);
    };
}

static void make_bool(Container *parent, std::string title, std::string description, bool initial_value, std::function<void(bool)> on_change, std::string icon = "") {
    auto p = make_self_height_sized_parent(parent);
    make_label_like(p, title, description, icon);
    make_bool_control(p, initial_value, on_change);
}

static void make_slider(Container *parent, std::string title, std::string description, float initial_value, std::function<void(float)> on_change, int notches = 0, std::string icon = "") {
    auto p = make_self_height_sized_parent(parent);
    
    make_label_like(p, title, description, icon);

    struct SliderInfo : UserData {
        float value = .5;
    };

    auto right = p->child(::hbox, FILL_SPACE, FILL_SPACE);
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
        auto cr = mylar->raw_window->cr;
        auto dpi = mylar->raw_window->dpi;

        {
            auto b = c->real_bounds;
            float h = 8.5 * dpi;
            b.y += b.h * .5 - h * .5;
            b.h = h;

            drawRoundedRect(cr, b.x - std::round(5 * dpi), b.y, b.w + std::round(10 * dpi), b.h, h * .5, 1.0);
            set_argb(cr, theme.slider_track);
            cairo_fill(cr); 

            if (notches != 0) {
                float snap_percentage = 1.0f / ((float) notches);
                int count = std::round(1.0f / snap_percentage);
                float spacing = b.w * snap_percentage;
                float x_off = 0.0;
                for (int i = 0; i < count + 1; i++) {
                    set_rect(cr, {b.x + x_off - std::round(1 * dpi), b.y + std::round(10 * dpi), std::round(2 * dpi), std::round(6 * dpi)});
                    set_argb(cr, theme.slider_track);
                    cairo_fill(cr); 

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
            cairo_fill(cr);

            //b.shrink(1.0);
            
            drawRoundedRect(cr, b.x, b.y, b.w, b.h, b.h * .5, 1.0);
            set_argb(cr, theme.slider_track);
            cairo_stroke(cr);

            b.shrink(5 * dpi);
            drawRoundedRect(cr, b.x, b.y, b.w, b.h, b.h * .5, 1.0);
            set_argb(cr, theme.accent);
            cairo_fill(cr);
        }

    };
    right->when_mouse_down = [on_change, notches](Container *root, Container *c) {
        auto mylar = (MylarWindow*)root->user_data;
        auto cr = mylar->raw_window->cr;
        auto dpi = mylar->raw_window->dpi;
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
    
    right->pre_layout = [](Container *root, Container *c, const Bounds &b) {
        auto mylar = (MylarWindow*)root->user_data;
        auto cr = mylar->raw_window->cr;
        auto dpi = mylar->raw_window->dpi;
        c->real_bounds.w = 350 * dpi;
        c->real_bounds.h = 20 * dpi;
    };
}

static void make_button_group(Container *parent, std::string title, std::string description, std::vector<std::string> options, std::function<void(std::string)> on_selected, std::string default_value, std::string icon = "") {
    auto p = make_self_height_sized_parent(parent);
    
    make_label_like(p, title, description, icon);
    
    auto right = p->child(::hbox, FILL_SPACE, FILL_SPACE);
    right->when_paint = paint {
        auto mylar = (MylarWindow*)root->user_data;
        auto cr = mylar->raw_window->cr;
        auto dpi = mylar->raw_window->dpi;
        auto b = c->real_bounds;
        drawRoundedRect(cr, b.x, b.y, b.w, b.h, 10 * dpi, 1.0);
        set_argb(cr, theme.segment_background);
        cairo_fill(cr);
    };
    right->pre_layout = [options](Container *root, Container *c, const Bounds &b) {
        auto mylar = (MylarWindow*)root->user_data;
        auto cr = mylar->raw_window->cr;
        auto dpi = mylar->raw_window->dpi;
        auto size = 12 * dpi;

        float w = 0;
        float h = 10;
        std::vector<float> ow;
        for (auto o : options) {
            auto bo1 = draw_text(cr, 0, 0, o, size, false, set->font, -1, -1, theme.text, false);
            ow.push_back(bo1.w);
            w += bo1.w;
            h = bo1.h;
        }
        for (int i = 0; i < ow.size(); i++)
            ow[i] = ow[i] / w;

        h += ((6 + 4) * 2) * dpi;
        // out pad, spacing, per button text pad
        w += (4 * 2 + 4 * (options.size() - 1) + (6 * (options.size() + 2) * 2))  * dpi;

        for (int i = 0; i < ow.size(); i++)
            c->children[i]->wanted_bounds.w = ow[i] * w;

        c->real_bounds.w = w;
        c->real_bounds.h = h;
    };
    for (int i = 0; i < options.size(); i++) {
        auto o = options[i];
        auto option = right->child(FILL_SPACE, FILL_SPACE);
        struct OptionData : UserData {
            bool selected = false;
        };
        auto option_data = new OptionData;
        if (o == default_value)
            option_data->selected = true;
        option->user_data = option_data;
        option->when_paint = [i, o, options](Container *root, Container *c) {
            auto data = (OptionData *) c->user_data;
            auto mylar = (MylarWindow*)root->user_data;
            auto cr = mylar->raw_window->cr;
            auto dpi = mylar->raw_window->dpi;
            auto size = 12 * dpi;
            auto backup = c->real_bounds;
            defer(c->real_bounds = backup);
            if (i == 0) {
                c->real_bounds.shrink(4 * dpi);
                c->real_bounds.w += 4 * dpi;
            } else if (i == options.size() - 1) {
                c->real_bounds.shrink(4 * dpi);
                c->real_bounds.x -= 4 * dpi;
                c->real_bounds.w += 4 * dpi;
            } else {
                c->real_bounds.shrink(4 * dpi);
            }

            if (data->selected) {
                set_argb(cr, theme.segment_selected);
                drawRoundedRect(cr, c->real_bounds.x, c->real_bounds.y, c->real_bounds.w, c->real_bounds.h, 8 * dpi, 1.0);
                cairo_fill(cr);
            }

            auto bo = draw_text(cr, 0, 0, o, size, false, set->font, -1, -1, theme.text_secondary, false);
            draw_text(cr,
                c->real_bounds.x + c->real_bounds.w * .5 - bo.w * .5, 
                c->real_bounds.y + c->real_bounds.h * .5 - bo.h * .5, o, size, true, set->font, -1, -1, theme.text, false);
        };
        option->when_clicked = [o, on_selected](Container *root, Container *c) {
            for (auto ch : c->parent->children) {
                auto data = (OptionData *) ch->user_data;
                data->selected = false;
            }
            auto data = (OptionData *) c->user_data;
            data->selected = true;
            if (on_selected)
                on_selected(o);
        };
    }
}

static Container *make_vert_space(Container *parent, float amount) {
    auto pad = parent->child(FILL_SPACE, 8);
    pad->pre_layout = [amount](Container *root, Container *c, const Bounds &b) {
        auto mylar = (MylarWindow*)root->user_data;
        auto cr = mylar->raw_window->cr;
        auto dpi = mylar->raw_window->dpi;
        c->wanted_bounds.h = amount * dpi;
    };
    return pad;
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

static void swifi_text(Container *root, Bounds b, const std::string &text, double size, RGBA color,
                       bool centered = false, bool bold = false) {
    auto window = swifi_window(root);
    auto font = get_cached_pango_font(window->cr, set->font, size * window->dpi,
        bold ? PANGO_WEIGHT_BOLD : PANGO_WEIGHT_NORMAL, false);
    pango_layout_set_text(font, text.c_str(), -1);
    pango_layout_set_width(font, std::max(1.0, b.w) * PANGO_SCALE);
    pango_layout_set_height(font, std::max(1.0, b.h) * PANGO_SCALE);
    pango_layout_set_wrap(font, PANGO_WRAP_WORD_CHAR);
    pango_layout_set_ellipsize(font, PANGO_ELLIPSIZE_END);
    pango_layout_set_alignment(font, centered ? PANGO_ALIGN_CENTER : PANGO_ALIGN_LEFT);
    int width, height;
    pango_layout_get_pixel_size(font, &width, &height);
    cairo_save(window->cr);
    cairo_rectangle(window->cr, b.x, b.y, b.w, b.h);
    cairo_clip(window->cr);
    set_argb(window->cr, color);
    cairo_move_to(window->cr, b.x, b.y + std::max(0.0, (b.h - height) * .5));
    pango_cairo_show_layout(window->cr, font);
    cairo_restore(window->cr);
    pango_layout_set_alignment(font, PANGO_ALIGN_LEFT);
}

static double swifi_text_width(Container *root, const std::string &text, double size) {
    auto window = swifi_window(root);
    auto font = get_cached_pango_font(window->cr, set->font, size * window->dpi, PANGO_WEIGHT_NORMAL, false);
    pango_layout_set_width(font, -1);
    pango_layout_set_height(font, -1);
    pango_layout_set_ellipsize(font, PANGO_ELLIPSIZE_NONE);
    pango_layout_set_text(font, text.c_str(), -1);
    int width = 0, height = 0;
    pango_layout_get_pixel_size(font, &width, &height);
    return width;
}

static void swifi_glyph(cairo_t *cr, double x, double y, const char *glyph, double size, RGBA color, bool path = false) {
    auto layout = get_cached_pango_font(cr, icon_font, size, PANGO_WEIGHT_NORMAL, false);
    pango_layout_set_width(layout, -1);
    pango_layout_set_height(layout, -1);
    pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_NONE);
    pango_layout_set_alignment(layout, PANGO_ALIGN_LEFT);
    pango_layout_set_text(layout, glyph, -1);
    cairo_move_to(cr, x, y);
    if (path) {
        pango_cairo_layout_path(cr, layout);
        return;
    }
    set_argb(cr, color);
    pango_cairo_show_layout(cr, layout);
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
    bool scrolled_to_selection = false;
};

static MylarWindow *dropdown_popup = nullptr;
static constexpr int dropdown_row_h = 32;
static constexpr int dropdown_max_visible_rows = 12;

static void close_dropdown_menu() {
    if (!dropdown_popup)
        return;
    auto popup = dropdown_popup;
    dropdown_popup = nullptr;
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
        if (dropdown_popup == popup)
            dropdown_popup = nullptr;
        std::erase(popups, popup);
    };
    popup->root->when_paint = [](Container *root, Container *c) {
        auto window = ((MylarWindow *) root->user_data)->raw_window;
        auto b = c->real_bounds;
        set_argb(window->cr, theme.popup_background);
        drawRoundedRect(window->cr, b.x, b.y, b.w, b.h, 8 * window->dpi, 1.0);
        cairo_fill(window->cr);
        set_argb(window->cr, theme.dropdown_border);
        drawRoundedRect(window->cr, b.x, b.y, b.w, b.h, 8 * window->dpi, 1.0);
        cairo_stroke(window->cr);
    };

    auto list = popup->root->child(::fullycustom, FILL_SPACE, FILL_SPACE);
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
        menu->scroll = std::clamp(menu->scroll, -menu->overflow, 0.0f);
        for (auto ch : c->children) {
            ch->real_bounds.x += inset;
            ch->real_bounds.w -= inset * 2;
            modify_all(ch, 0, inset + menu->scroll);
        }
    };
    list->when_fine_scrolled = [menu](Container *root, Container *, double, double scroll_y, bool) {
        auto window = ((MylarWindow *) root->user_data)->raw_window;
        menu->scroll = std::clamp(menu->scroll + (float) (scroll_y * 3 * window->dpi), -menu->overflow, 0.0f);
        windowing::redraw(window);
    };
    list->when_paint = [](Container *root, Container *c) {
        auto window = ((MylarWindow *) root->user_data)->raw_window;
        cairo_save(window->cr);
        auto b = c->real_bounds;
        cairo_rectangle(window->cr, b.x, b.y + 2 * window->dpi, b.w, b.h - 4 * window->dpi);
        cairo_clip(window->cr);
    };
    list->after_paint = [menu](Container *root, Container *c) {
        auto window = ((MylarWindow *) root->user_data)->raw_window;
        cairo_restore(window->cr);
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
        set_argb(window->cr, color);
        drawRoundedRect(window->cr, b.x + b.w - 5 * dpi, y, 3 * dpi, thumb_h, 1.5 * dpi, 1.0);
        cairo_fill(window->cr);
    };

    for (size_t i = 0; i < options.size(); i++) {
        auto ch = list->child(FILL_SPACE, dropdown_row_h);
        bool is_selected = (int) i == menu->selected_index;
        auto label = options[i].label;
        ch->when_paint = [label, is_selected](Container *root, Container *c) {
            auto window = ((MylarWindow *) root->user_data)->raw_window;
            auto cr = window->cr;
            auto dpi = window->dpi;
            auto b = c->real_bounds;
            if (c->state.mouse_pressing || c->state.mouse_hovering) {
                set_argb(cr, c->state.mouse_pressing ? theme.popup_pressed : theme.popup_hover);
                drawRoundedRect(cr, b.x, b.y, b.w, b.h, 6 * dpi, 1.0);
                cairo_fill(cr);
            }
            if (is_selected) {
                set_argb(cr, theme.accent);
                drawRoundedRect(cr, b.x + 4 * dpi, b.y + b.h * .3, 3 * dpi, b.h * .4, 1.5 * dpi, 1.0);
                cairo_fill(cr);
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

static double dropdown_button_width(Container *root, const std::string &label) {
    auto dpi = swifi_window(root)->dpi;
    return std::clamp(swifi_text_width(root, label, 11) + 46 * dpi, 120.0 * dpi, 250.0 * dpi);
}

static void paint_dropdown_button(Container *root, Container *c, const std::string &label) {
    auto window = swifi_window(root);
    auto cr = window->cr;
    auto dpi = window->dpi;
    double h = std::min<double>(30 * dpi, c->real_bounds.h);
    Bounds b(c->real_bounds.x, c->real_bounds.y + (c->real_bounds.h - h) * .5, c->real_bounds.w, h);
    set_argb(cr, c->state.mouse_pressing ? theme.dropdown_pressed :
        c->state.mouse_hovering ? theme.dropdown_hover : theme.dropdown_background);
    drawRoundedRect(cr, b.x, b.y, b.w, b.h, settings_control_rounding * dpi, 1.0);
    cairo_fill(cr);
    set_argb(cr, theme.dropdown_border);
    drawRoundedRect(cr, b.x, b.y, b.w, b.h, settings_control_rounding * dpi, 1.0);
    cairo_stroke(cr);
    swifi_text(root, Bounds(b.x + 10 * dpi, b.y, b.w - 38 * dpi, b.h), label, 11, theme.text);
    // ChevronDown (U+E70D) marks the button as a dropdown
    const std::string chevron = "\uE70D";
    auto cb = draw_text(cr, 0, 0, chevron, 10 * dpi, false, icon_font, -1, -1, theme.text_secondary, false);
    draw_text(cr, b.x + b.w - 14 * dpi - cb.w, b.y + b.h * .5 - cb.h * .5, chevron, 10 * dpi, true, icon_font, -1, -1, theme.text_secondary, false);
}

static void make_dropdown(Container *parent, std::string text_, std::vector<std::string> options, std::function<void(std::string)> func) {
    struct TextData : UserData {
        std::string text;
    };
    auto pad = parent->child(FILL_SPACE, FILL_SPACE);
    auto text_data = new TextData;
    text_data->text = text_;
    pad->user_data = text_data;
    pad->pre_layout = [](Container *root, Container *c, const Bounds &) {
        auto td = (TextData *) c->user_data;
        c->wanted_bounds.w = dropdown_button_width(root, td->text);
        c->wanted_bounds.h = 34 * swifi_window(root)->dpi;
    };
    pad->when_paint = [](Container *root, Container *c) {
        paint_dropdown_button(root, c, ((TextData *) c->user_data)->text);
    };
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

static Container *make_dropdown_option(Container *parent, std::string title, std::string description, std::string icon, std::string selected, std::vector<std::string> options, std::function<void(std::string)> on_change) {
    assert(on_change);
    auto p = make_self_height_sized_parent(parent);

    make_label_like(p, title, description, icon);

    auto right = p->child(::hbox, FILL_SPACE, FILL_SPACE);
    right->alignment = container_alignment::ALIGN_CENTER;
    
    right->pre_layout = [](Container *root, Container *c, const Bounds &b) {
        auto mylar = (MylarWindow*)root->user_data;
        auto cr = mylar->raw_window->cr;
        auto dpi = mylar->raw_window->dpi;
        c->real_bounds.w = 250 * dpi;
        c->real_bounds.h = 70 * dpi;
        c->spacing = 5 * dpi;
    };
    right->child(FILL_SPACE, FILL_SPACE);
    right->parent_bounds_limit_input_bounds = false;
    
    make_dropdown(right, selected, options, [on_change](std::string selected) {
        on_change(selected);
    });
    
    return p;
}

static void make_button(Container *parent, std::string text, std::function<void()> func, std::function<bool()> is_enabled = {}) {
    static float pad_amount = 11;
    static float text_height = 11;
    auto pad = parent->child(FILL_SPACE, FILL_SPACE);
    pad->pre_layout = [text](Container *root, Container *c, const Bounds &b) {
        auto mylar = (MylarWindow*)root->user_data;
        auto cr = mylar->raw_window->cr;
        auto dpi = mylar->raw_window->dpi;
        text_height = 11 * dpi;
        pad_amount = 11 * dpi;
        Bounds bounds = draw_text(cr, 0, 0, text, text_height, false, set->font, -1, -1, theme.text, true);
        c->wanted_bounds.w = bounds.w + pad_amount * 2;
        c->wanted_bounds.h = bounds.h + (pad_amount * 2 * .8);
    };
    pad->when_paint = [text, is_enabled](Container *root, Container *c) {
        auto mylar = (MylarWindow*)root->user_data;
        auto cr = mylar->raw_window->cr;
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
        cairo_fill(cr);
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
        auto cr = mylar->raw_window->cr;
        auto dpi = mylar->raw_window->dpi;
        auto on = draw_text(cr, 0, 0, "On", 11 * dpi, false, set->font, -1, -1, theme.text, false, 0);
        auto off = draw_text(cr, 0, 0, "Off", 11 * dpi, false, set->font, -1, -1, theme.text, false, 0);
        c->wanted_bounds.w = std::max(on.w, off.w) + 12 * dpi;
    };

    make_bool_control(right, initial_value, on_change);
    make_button(right, button_text, on_click);
}

static Container *make_field(Container *parent, bool only_numbers, std::string initial_value, std::function<void (std::string)> on_change) {
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
    pad->pre_layout = [](Container *root, Container *c, const Bounds &b) {
        auto dpi = static_cast<MylarWindow *>(root->user_data)->raw_window->dpi;
        auto bounds = measure_text_editor(root, c);
        c->wanted_bounds.w = FILL_SPACE;
        c->wanted_bounds.h = bounds.h + 11 * dpi * 2 * .8;
    };
    pad->when_paint = [](Container *root, Container *c) {
        auto mylar = static_cast<MylarWindow *>(root->user_data);
        auto cr = mylar->raw_window->cr;
        auto dpi = mylar->raw_window->dpi;
        set_argb(cr, c->active ? theme.accent : theme.field_border);
        drawRoundedRect(cr, c->real_bounds.x, c->real_bounds.y,
                        c->real_bounds.w, c->real_bounds.h,
                        settings_control_rounding * dpi, 1.0);
        cairo_fill(cr);
        set_argb(cr, theme.field_background);
        auto minus_border = c->real_bounds;
        minus_border.shrink(std::round(1 * dpi));
        drawRoundedRect(cr, minus_border.x, minus_border.y,
                        minus_border.w, minus_border.h,
                        settings_control_rounding * dpi, 1.0);
        cairo_fill(cr);
        paint_text_editor(root, c, theme.text, theme.accent);
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
        auto cr = mylar->raw_window->cr;
        auto dpi = mylar->raw_window->dpi;
        set_argb(cr, theme.monitor_background);
        auto b = c->real_bounds;
        drawRoundedRect(cr, b.x, b.y, b.w, b.h, 10 * dpi, 1.0 * dpi);
        cairo_fill(cr);
    };
    // Alignment guides, drawn over the monitors while one is snapped.
    c->after_paint = paint {
        auto canvas = (MonitorCanvas *) c->user_data;
        if (!canvas->dragging || (!canvas->guide_x_on && !canvas->guide_y_on))
            return;
        auto mylar = (MylarWindow*)root->user_data;
        auto cr = mylar->raw_window->cr;
        auto dpi = mylar->raw_window->dpi;
        auto b = c->real_bounds;

        cairo_save(cr);
        set_rect(cr, b);
        cairo_clip(cr);
        set_argb(cr, theme.accent);
        cairo_set_line_width(cr, std::max(1.0, std::floor(1.0 * dpi)));
        if (canvas->guide_x_on) {
            const double x = std::round(b.x + b.w * .5 + (canvas->guide_x - canvas->origin_x) * canvas->scale) + .5;
            cairo_move_to(cr, x, b.y);
            cairo_line_to(cr, x, b.y + b.h);
            cairo_stroke(cr);
        }
        if (canvas->guide_y_on) {
            const double y = std::round(b.y + b.h * .5 + (canvas->guide_y - canvas->origin_y) * canvas->scale) + .5;
            cairo_move_to(cr, b.x, y);
            cairo_line_to(cr, b.x + b.w, y);
            cairo_stroke(cr);
        }
        cairo_restore(cr);
    };

    bool first = true;
    for (auto option : options) {
        auto main = c->child(10, 10);
        main->when_paint = paint {
            auto mylar = (MylarWindow*)root->user_data;
            auto cr = mylar->raw_window->cr;
            auto dpi = mylar->raw_window->dpi;
            auto data = (MonitorInfo *) c->user_data;
            auto b = c->real_bounds;
            
            cairo_save(cr);
            set_rect(cr, c->parent->real_bounds);
            cairo_clip(cr);
            
            auto surface = theme.monitor_surface;
            if (data->dragging)
                surface.a *= .85;
            set_argb(cr, surface);
            drawRoundedRect(cr, b.x, b.y, b.w, b.h, 10 * dpi, 1.0 * dpi);
            cairo_fill(cr);

            if (data->selected) {
                set_argb(cr, theme.accent);
                drawRoundedRect(cr, b.x, b.y, b.w, b.h, 10 * dpi, 2.0 * dpi);
                cairo_stroke(cr);
            }

            auto text = fz("{}\n({}x{})", data->o->name, data->o->w, data->o->h);
            if (data->dragging)
                text += fz("\n{}, {}", (int) std::round(data->drag_x), (int) std::round(data->drag_y));
            auto tb = draw_text(cr, 0, 0, text, 12 * dpi, false, set->font, c->real_bounds.w, -1, theme.text, false, 1);
            
            draw_text(cr, 
                c->real_bounds.x,
                c->real_bounds.y + c->real_bounds.h * .5 - tb.h * .5, 
                text, 12 * dpi, true, set->font, c->real_bounds.w, -1, theme.text, false, 1);

            cairo_reset_clip(cr);
            cairo_restore(cr);
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

static void make_reset_textfield(Container* parent, std::string title, std::string description, std::string icon, bool only_numbers, std::string initial_value,
                                 std::string reset_value, std::function<void(std::string)> on_change);

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
            auto existing = find(config);
            if (existing != set->monitors.end())
                *existing = config;
            else
                set->monitors.push_back(config);
        }

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
        auto cr = mylar->raw_window->cr;
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

static void add_monitor_int_field(Container* parent, std::string title, std::string description, std::string icon, int initial_value, int reset_value, int min_value, int max_value,
                                  std::function<void(int)> on_change) {
    make_reset_textfield(parent, std::move(title), std::move(description), std::move(icon), false, std::to_string(initial_value), std::to_string(reset_value),
                         [min_value, max_value, on_change = std::move(on_change)](std::string value) {
                             int parsed = 0;
                             if (!monitor_int_from_text(trim(value), parsed)) {
                                 notify("Invalid value");
                                 return;
                             }
                             on_change(std::clamp(parsed, min_value, max_value));
                         });
}

static void add_monitor_float_field(Container* parent, std::string title, std::string description, std::string icon, float initial_value, float reset_value,
                                    float min_value, float max_value, std::function<void(float)> on_change) {
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
                         });
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

    make_vert_space(padded_right, 10);

    make_bool(padded_right, "Disabled", "Turn this monitor off without disconnecting it", m->config.disabled, [m, commit](bool value) {
        m->config.disabled = value;
        commit();
    });

    make_vert_space(padded_right, 4);

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

    make_vert_space(padded_right, 4);

    make_reset_textfield(padded_right, "Scale", "Scale factor, or \"auto\"", "\ue93a", false, m->config.scale, "auto", [m, commit](std::string value) {
        value           = trim(value);
        if (value.empty() || value == "auto") {
            m->config.scale = "auto";
        } else {
            float scale = 0;
            if (!monitor_float_from_text(value, scale) || !std::isfinite(scale) || scale < 0.3f) {
                notify("Scale must be auto or a number from 0.3 to 6");
                return;
            }
            m->config.scale = fz("{}", std::min(scale, 6.0f));
        }
        commit();
    });

    make_vert_space(padded_right, 4);

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

    make_vert_space(padded_right, 4);

    make_reset_textfield(padded_right, "Position", "Position such as \"1920x0\" or \"auto\"", "\uE7F4", false, m->config.position, "auto", [m, commit](std::string value) {
        value              = trim(value);
        m->config.position = value.empty() ? "auto" : value;
        commit();
    });

    make_vert_space(padded_right, 4);

    make_reset_textfield(padded_right, "Mirror", "Monitor output to mirror, leave empty for none", "\uE7F4", false, m->config.mirror, "", [m, commit](std::string value) {
        m->config.mirror = trim(value);
        commit();
    });

    make_vert_space(padded_right, 4);

    const bool hdr_on = m->config.cm == "hdr" || m->config.cm == "hdredid";
    make_bool(padded_right, "HDR", "High dynamic range, for displays that support it", hdr_on, [m, commit](bool value) {
        m->config.cm = value ? "hdr" : "srgb";
        // HDR needs 10 bit output.
        if (value)
            m->config.bitdepth = 10;
        commit();
    });

    make_vert_space(padded_right, 4);

    // Everything below is rarely touched, keep it folded away until asked for.
    static bool advanced_open = false;
    auto        advanced      = make_accordion(padded_right, "Advanced", "Bit depth, color management, VRR and luminance overrides", "\ue93a", &advanced_open);
    const size_t first_advanced = padded_right->children.size();

    make_vert_space(padded_right, 4);

    make_dropdown_option(padded_right, "Bit depth", "Color bit depth", "\ue93a", std::to_string(m->config.bitdepth), {"8", "10"}, [m, commit](std::string selected) {
        m->config.bitdepth = selected == "10" ? 10 : 8;
        commit();
    });

    make_vert_space(padded_right, 4);

    const std::vector<std::string> cm_options = {"auto", "srgb", "wide", "edid", "hdr", "hdredid", "dcip3", "dp3", "adobe"};
    make_dropdown_option(padded_right, "Color management", "Color management preset", "\ue93a", m->config.cm, cm_options, [m, commit](std::string selected) {
        m->config.cm = selected;
        commit();
    });

    make_vert_space(padded_right, 4);

    const std::vector<std::string> eotf_options = {"default", "auto", "srgb", "gamma22", "gamma22force"};
    make_dropdown_option(padded_right, "SDR EOTF", "Transfer function used for SDR content", "\ue93a", m->config.sdr_eotf, eotf_options, [m, commit](std::string selected) {
        m->config.sdr_eotf = selected;
        commit();
    });

    make_vert_space(padded_right, 4);

    const std::vector<std::string> vrr_options = {"-1", "0", "1", "2", "3"};
    make_dropdown_option(padded_right, "VRR", "Variable refresh rate mode (-1 default, 0 off, 1 on, 2 fullscreen, 3 fullscreen only)", "\ue93a", std::to_string(m->config.vrr),
                         vrr_options, [m, commit](std::string selected) {
                             int parsed = 0;
                             if (!monitor_int_from_text(selected, parsed))
                                 return;
                             m->config.vrr = std::clamp(parsed, -1, 3);
                             commit();
                         });

    make_vert_space(padded_right, 4);

    make_reset_textfield(padded_right, "ICC profile", "Path to an ICC profile, leave empty for none", "\ue93a", false, m->config.icc, "", [m, commit](std::string value) {
        m->config.icc = trim(value);
        commit();
    });

    make_vert_space(padded_right, 4);

    add_monitor_int_field(padded_right, "Reserved area", "Reserved area in pixels", "\ue93a", m->config.reserved_area, 0, 0, INT_MAX, [m, commit](int value) {
        m->config.reserved_area = value;
        commit();
    });

    make_vert_space(padded_right, 4);

    const std::vector<std::string> support_options = {"Auto", "Force enable", "Force disable"};
    make_dropdown_option(padded_right, "Supports wide color", "Override wide color gamut support detection", "\ue93a",
                         m->config.supports_wide_color == 1 ? "Force enable" : (m->config.supports_wide_color == -1 ? "Force disable" : "Auto"), support_options,
                         [m, commit](std::string selected) {
                             m->config.supports_wide_color = selected == "Force enable" ? 1 : (selected == "Force disable" ? -1 : 0);
                             commit();
                         });

    make_vert_space(padded_right, 4);

    make_dropdown_option(padded_right, "Supports HDR", "Override HDR support detection", "\ue93a",
                         m->config.supports_hdr == 1 ? "Force enable" : (m->config.supports_hdr == -1 ? "Force disable" : "Auto"), support_options,
                         [m, commit](std::string selected) {
                             m->config.supports_hdr = selected == "Force enable" ? 1 : (selected == "Force disable" ? -1 : 0);
                             commit();
                         });

    make_vert_space(padded_right, 4);

    add_monitor_float_field(padded_right, "SDR brightness", "SDR brightness multiplier (0–10)", "\ue93a", m->config.sdrbrightness, 1.0f, 0.0f, 10.0f, [m, commit](float value) {
        m->config.sdrbrightness = value;
        commit();
    });

    make_vert_space(padded_right, 4);

    add_monitor_float_field(padded_right, "SDR saturation", "SDR saturation multiplier (0–10)", "\ue93a", m->config.sdrsaturation, 1.0f, 0.0f, 10.0f, [m, commit](float value) {
        m->config.sdrsaturation = value;
        commit();
    });

    make_vert_space(padded_right, 4);

    add_monitor_float_field(padded_right, "SDR min luminance", "Minimum SDR luminance", "\ue93a", m->config.sdr_min_luminance, 0.2f, 0.0f, 10000.0f, [m, commit](float value) {
        m->config.sdr_min_luminance = value;
        commit();
    });

    make_vert_space(padded_right, 4);

    add_monitor_int_field(padded_right, "SDR max luminance", "Maximum SDR luminance", "\ue93a", m->config.sdr_max_luminance, 80, INT_MIN, INT_MAX, [m, commit](int value) {
        m->config.sdr_max_luminance = value;
        commit();
    });

    make_vert_space(padded_right, 4);

    add_monitor_float_field(padded_right, "Min luminance", "Minimum luminance override (-1 uses automatic)", "\ue93a", m->config.min_luminance, -1.0f, -1.0f, 10000.0f, [m, commit](float value) {
        m->config.min_luminance = value;
        commit();
    });

    make_vert_space(padded_right, 4);

    add_monitor_int_field(padded_right, "Max luminance", "Maximum luminance override", "\ue93a", m->config.max_luminance, -1, INT_MIN, INT_MAX, [m, commit](int value) {
        m->config.max_luminance = value;
        commit();
    });

    make_vert_space(padded_right, 4);

    add_monitor_int_field(padded_right, "Max average luminance", "Maximum average luminance override", "\ue93a", m->config.max_avg_luminance, -1, INT_MIN, INT_MAX,
                          [m, commit](int value) {
                              m->config.max_avg_luminance = value;
                              commit();
                          });

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
    d->applied_scroll = std::clamp(d->scroll, -d->overflow, 0.0f);

    for (auto child : c->children)
        modify_all(child, 0, d->applied_scroll);
}

static void paint_right_scrollbar(Container *root, Container *c) {
    return;
    auto d = (RightData *) c->user_data;
    if (d->overflow <= 0 || d->content_h <= 0)
        return;
    auto   mylar   = (MylarWindow *) root->user_data;
    auto   cr      = mylar->raw_window->cr;
    auto   dpi     = mylar->raw_window->dpi;
    auto   track   = c->real_bounds;
    double w       = 6 * dpi;
    double x       = track.x + track.w - w - 4 * dpi;
    double thumb_h = std::min(track.h, std::max(30.0 * dpi, track.h * (track.h / d->content_h)));
    double t       = -d->applied_scroll / d->overflow;
    double y       = track.y + t * (track.h - thumb_h);

    RGBA color = theme.text_secondary;
    color.a *= .5;
    drawRoundedRect(cr, x, y, w, thumb_h, w * .5, 1.0);
    set_argb(cr, color);
    cairo_fill(cr);
}

// Shown while a display change is on trial: a countdown plus keep / revert.
static void make_display_trial_banner(Container *parent) {
    static const std::string title = "Keep these display settings?";
    auto description = []() {
        long seconds = (display_trial.deadline - get_current_time_in_ms() + 999) / 1000;
        seconds      = std::max(seconds, 0l);
        return fz("Reverting to the previous settings in {} second{}", seconds, seconds == 1 ? "" : "s");
    };

    auto p    = make_self_height_sized_parent(parent);
    p->name   = "display_trial_banner";
    p->exists = display_trial.active;

    auto left = p->child(FILL_SPACE, FILL_SPACE);
    left->pre_layout = [](Container *root, Container *c, const Bounds &b) {
        auto mylar = (MylarWindow*)root->user_data;
        c->real_bounds.h = 70 * mylar->raw_window->dpi;
    };
    left->when_paint = [description](Container *root, Container *c) {
        auto mylar = (MylarWindow*)root->user_data;
        auto cr = mylar->raw_window->cr;
        auto dpi = mylar->raw_window->dpi;
        auto text = description();
        auto t = draw_text(cr, 0, 0, title, 12 * dpi, false, set->font, -1, -1, theme.text, false);
        auto d = draw_text(cr, 0, 0, text, 11 * dpi, false, set->font, -1, -1, theme.text, false);
        float y = c->real_bounds.y + c->real_bounds.h * .5 - (t.h + d.h) * .5;
        auto ico = draw_text(cr, 0, 0, "", 16 * dpi, false, icon_font, -1, -1, theme.text, false);
        draw_text(cr, c->real_bounds.x + 19 * dpi, c->real_bounds.y + c->real_bounds.h * .5 - ico.h * .5, "", 16 * dpi, true, icon_font, -1, -1, theme.accent, false);
        draw_text(cr, c->real_bounds.x + 60 * dpi, y, title, 12 * dpi, true, set->font, -1, -1, theme.text, false);
        draw_text(cr, c->real_bounds.x + 60 * dpi, y + t.h, text, 11 * dpi, true, set->font, -1, -1, theme.text_secondary, false);
    };

    auto right = p->child(::hbox, FILL_SPACE, FILL_SPACE);
    right->alignment = container_alignment::ALIGN_CENTER | container_alignment::ALIGN_RIGHT;
    right->pre_layout = [](Container *root, Container *c, const Bounds &b) {
        auto mylar = (MylarWindow*)root->user_data;
        auto dpi = mylar->raw_window->dpi;
        c->real_bounds.w = 250 * dpi;
        c->real_bounds.h = 70 * dpi;
        c->spacing = 5 * dpi;
    };
    right->child(FILL_SPACE, FILL_SPACE);
    right->parent_bounds_limit_input_bounds = false;
    make_button(right, "Keep changes", []() { main_thread([]() { display_trial_finish(true); }); });
    make_button(right, "Revert", []() { main_thread([]() { display_trial_finish(false); }); });

    auto space  = make_vert_space(parent, 10);
    space->name = "display_trial_space";
    space->exists = display_trial.active;
}

// Redraws the countdown while a display change is on trial.
static void display_trial_watch() {
    windowing::timer(settings_app, 250, [](void *) {
        if (!settings_app || !settings_mylar)
            return;
        if (display_trial.active)
            windowing::redraw(settings_mylar->raw_window);
        display_trial_watch();
    }, nullptr);
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

    make_display_trial_banner(padded_right);

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
        
    for (auto m : options) {
        change_display_options(m);
        break;
    }

}

static void fill_desktop_settings(Container *root, Container *c) {
    auto right = container_by_name("settings_right", root);
    if (!right)
        return;
    for (auto child: right->children)
        delete child;
    right->children.clear();

    right->pre_layout = layout_right_page;
    auto padded_right = right->child(FILL_SPACE, FILL_SPACE);

    make_section_title(padded_right, "Desktop Settings");
    
    make_vert_space(padded_right, 10);

    make_button_option(padded_right, "Icon Cache", "Re-scan installed icons", "Re-scan Icons", []() {
        if (icon_cache_rescan_in_progress)
            return;

        icon_cache_rescan_in_progress = true;
        damage_all();

        std::thread([]() {
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
        }).detach();
    }, "\uE71D", []() { return !icon_cache_rescan_in_progress; });

    make_vert_space(padded_right, 4);
    make_bool_with_button(padded_right, "Desktop Icons", "", set->desktop_icons, [](bool c) {
        set->desktop_icons = c;
        main_thread([]() {
            desktop_icons::stop();
            desktop_icons::start();
            damage_all();
        });
    }, "Change folder", []() {
        static bool chooser_open = false;
        if (chooser_open)
            return;

        chooser_open = true;

        std::thread t([]() {
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

        t.detach();
    }, "\uec6c");
    
    make_vert_space(padded_right, 4);
    make_dropdown_option(padded_right, "Overview", "Change layout type", "\uE8A9", set->overview_layout_type, {"Grid", "Adaptive"}, [](std::string new_type) {
        set->overview_layout_type = new_type;
    });
    
    make_vert_space(padded_right, 4);
    make_dropdown_option(padded_right, "Theme", "Choose the settings appearance", "\uE790", set->dark_theme ? "Dark" : "Light", {"Light", "Dark"}, [](std::string selected) {
        set->dark_theme = selected == "Dark";
        theme = set->dark_theme ? dark_theme : light_theme;
        settings::load_save_settings(true, set);
        if (settings_mylar)
            windowing::redraw(settings_mylar->raw_window);
    });
    
    make_vert_space(padded_right, 4); 
    make_reset_textfield(padded_right, "Font", "User interface font", "\uE8D2", false, set->font, "Segoe UI Variable", [](std::string value) {
        set->font = value;
    });
    
    make_vert_space(padded_right, 24);
    make_section_title(padded_right, "Wallpaper Settings");

    make_vert_space(padded_right, 10);
    make_bool_with_button(padded_right, "Draw wallpaper", "", set->draw_wallpaper, [](bool c) {
        set->draw_wallpaper = c;
        damage_all();
    }, "Choose file", []() {
        static bool chooser_open = false;
        if (chooser_open)
            return;

        chooser_open = true;

        std::thread t([]() {
            const auto file = trim_newline(choose_file());
            if (!file.empty() && std::filesystem::exists(file)) {
                auto mime = trim_newline(execAndGet(std::string("file --mime-type -b \"" + file + "\"").c_str()));

                if (mime.starts_with("image/")) {
                    const char* home = std::getenv("HOME");
                    std::filesystem::path filepath =
                        std::filesystem::path(home) / ".config/mylar/wall.png";
 
                    std::error_code ec;
                    std::filesystem::create_directories(filepath.parent_path(), ec);
                    std::filesystem::copy_file(file, filepath,
                                               std::filesystem::copy_options::overwrite_existing, ec);
                }
            }

            chooser_open = false;
        });

        t.detach();
    }, "\uE8b9");

    make_vert_space(padded_right, 24);
    make_section_title(padded_right, "Dock Settings");

    make_vert_space(padded_right, 10);

    make_bool(padded_right, "Show docks", "", set->show_docks, [](bool c) {
        set->show_docks = c;

        if (set->show_docks) {
            dock::start();
        } else {
            dock::stop();
        }
    }, "\ue75b");

    make_vert_space(padded_right, 4);

    make_bool(padded_right, "Only show windows on the current workspace",
        "Turn off to show windows from all workspaces and monitors",
        set->dock_current_workspace_only, [](bool value) {
            main_thread([value]() {
                set->dock_current_workspace_only = value;
                dock::update_workspaces();
                settings::load_save_settings(true, set);
            });
        }, "\ue75b");

    make_vert_space(padded_right, 4);

    make_bool(padded_right,
        "Combine taskbar buttons and hide labels",
        "Group all windows that belong to one dock icon under a single icon",
        set->dock_merge_windows,
        [](bool value) {
            main_thread([value]() {
                set->dock_merge_windows = value;
                dock::set_dock_merge(value);
                settings::load_save_settings(true, set);
            });
        },
        "\ue75b");
}



static void make_reset_textfield(Container *parent, std::string title, std::string description, std::string icon, bool only_numbers, std::string initial_value, std::string reset_value, std::function<void(std::string)> on_change) {
    auto p = make_self_height_sized_parent(parent);
    
    make_label_like(p, title, description, icon);

    auto right = p->child(::hbox, FILL_SPACE, FILL_SPACE);
    right->alignment = container_alignment::ALIGN_CENTER;
    
    right->pre_layout = [](Container *root, Container *c, const Bounds &b) {
        auto mylar = (MylarWindow*)root->user_data;
        auto cr = mylar->raw_window->cr;
        auto dpi = mylar->raw_window->dpi;
        c->real_bounds.w = 340 * dpi;
        c->real_bounds.h = 70 * dpi;
        c->spacing = 5 * dpi;
    };

    auto field = make_field(right, only_numbers, initial_value, [](std::string latest_text) {
    });
    
    make_button(right, "Apply", [field, on_change]() {
        auto value = static_cast<STextEditorData *>(field->user_data)->text;
        on_change(value);
    });
    make_button(right, "Reset", [field, reset_value, on_change]() {
        reset_text_editor(field, reset_value);
        auto value = static_cast<STextEditorData *>(field->user_data)->text;
        on_change(value);
    });
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

static void fill_keyboard_settings(Container *root, Container *c) {
    auto right = container_by_name("settings_right", root);
    if (!right)
        return;
    for (auto child: right->children)
        delete child;
    right->children.clear();

    right->pre_layout = layout_right_page;
    auto padded_right = right->child(FILL_SPACE, FILL_SPACE);

    make_section_title(padded_right, "Keyboard Settings");
    
    make_vert_space(padded_right, 10);

    bool only_numbers = true;
    make_reset_textfield(padded_right, "Repeat delay", "Delay before a held key starts repeating, in milliseconds", "\ue916", only_numbers, std::to_string(set->repeat_delay), "600", [](std::string value) {
        set->repeat_delay = std::atoi(value.c_str());
        reset_conf();
    });

    make_vert_space(padded_right, 4); 
    
    make_reset_textfield(padded_right, "Repeat rate", "Number of key repeats per second", "\ue8ee", only_numbers, std::to_string(set->repeat_rate), "25", [](std::string value) {
        set->repeat_rate = std::atoi(value.c_str());
        reset_conf();
    });
   
    make_vert_space(padded_right, 10); 
    
    make_section_title(padded_right, "Layouts");
}

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
    
    make_vert_space(padded_right, 4); 

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
    
    make_vert_space(padded_right, 4); 
    
    make_slider(padded_right, "Cursor speed", "Sets the speed factor of the cursor", set->cursor_speed, [](float value) {
        set->cursor_speed = value;
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
    }, 0, "\uec49");

    make_vert_space(padded_right, 10); 
    
    make_section_title(padded_right, "Touchpad");

    make_vert_space(padded_right, 10); 
    
    make_bool(padded_right, "Reverse scrolling direction", "Phone like scrolling behaviour", set->natural_scrolling_touchpad, [](bool value) {
        set->natural_scrolling_touchpad = value;
        main_thread([]() {
            hypriso->generate_mylar_hyprland_config();
        });
    }, "\uece7");
    
    make_vert_space(padded_right, 4); 

    make_bool(padded_right, "Disable while typing", "Prevents touchpad from receiving input while typing", set->touchpad_disable_while_typing, [](bool value) {
        set->touchpad_disable_while_typing = value;
        main_thread([]() {
            hypriso->generate_mylar_hyprland_config();
        });
    }, "\uefa5");

    make_vert_space(padded_right, 4); 

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

}

struct Shortcut {
    std::string name;
    std::string shortcut;
    std::string command;
};

void make_shortcut_option(Container *root, Shortcut &s, bool is_first = false, bool is_last = false) {
    static const float height = 56;
    static const float text_h = 12;
    static const float round = 8;
    auto line = root->child(FILL_SPACE, FILL_SPACE);
    line->pre_layout = [](Container *root, Container *c, const Bounds &b) {
        auto mylar = (MylarWindow*)root->user_data;
        auto cr = mylar->raw_window->cr;
        auto dpi = mylar->raw_window->dpi;
        c->wanted_bounds.h = height * dpi;
    };
    line->when_paint = [s, is_first, is_last](Container *root, Container *c) {
        auto mylar = (MylarWindow*)root->user_data;
        auto cr = mylar->raw_window->cr;
        auto dpi = mylar->raw_window->dpi;

        auto before = c->real_bounds;
        defer(c->real_bounds = before);
        const auto overlap = (std::floor(1.0 * dpi) + 1.0) * 0.5;
        c->real_bounds.y -= overlap;
        c->real_bounds.h += 2.0 * overlap;

        drawRoundedRect(cr, c->real_bounds.x, c->real_bounds.y,
                c->real_bounds.w, c->real_bounds.h, round * dpi, 0,
                is_first, is_first, is_last, is_last);
        set_argb(cr, theme.row_background); 
        cairo_fill(cr);
        
        drawRoundedRect(cr, c->real_bounds.x, c->real_bounds.y,
                        c->real_bounds.w, c->real_bounds.h, round * dpi,
                        std::floor(1.0 * dpi),
                        is_first, is_first, is_last, is_last);
        set_argb(cr, theme.row_border); 
        cairo_set_line_width(cr, std::floor(1.0 * dpi)); 
        cairo_stroke(cr);

        auto tb = draw_text(cr, 0, 0, s.name, text_h * dpi, false, set->font, -1, -1, theme.text, false, 0);
        auto overflow = ((c->real_bounds.h - tb.h) * .5) * 1.5;
        draw_text(cr, 
            c->real_bounds.x + overflow,
            c->real_bounds.y + c->real_bounds.h * .5 - tb.h * .5, 
            s.name, text_h * dpi, true, set->font, -1, -1, theme.text, false, 0);
        
        tb = draw_text(cr, 0, 0, s.shortcut, text_h * dpi, false, set->font, -1, -1, theme.text_secondary, false, 0);
        draw_text(cr, 
            c->real_bounds.x + c->real_bounds.w - tb.w - overflow,
            c->real_bounds.y + c->real_bounds.h * .5 - tb.h * .5, 
            s.shortcut, text_h * dpi, true, set->font, -1, -1, theme.text_secondary, false, 0);
    };
    line->when_clicked = [s](Container *root, Container *c) {
        if (!s.command.empty())
            launch_command(s.command);
    };
}

static void fill_shortcuts_settings(Container *root) {
    auto right = container_by_name("settings_right", root);
    if (!right)
        return;
    for (auto child: right->children)
        delete child;
    right->children.clear();

    right->pre_layout = layout_right_page;
    auto padded_right = right->child(FILL_SPACE, FILL_SPACE);
    
    make_section_title(padded_right, "Shortcuts");
    
    make_vert_space(padded_right, 10);

    std::vector<Shortcut> shortcuts = {
        {"Show desktop", "Super + D", "hyprctl dispatch hl.plugin.mylar.toggle_desktop_show"},
        {"Snap Window Left", "CTRL + ALT + Left", "hyprctl dispatch hl.plugin.mylar.snap_left"}
    };
    for (int i = 0; i < shortcuts.size(); i++) {
        make_shortcut_option(padded_right, shortcuts[i], i == 0, i == (shortcuts.size() - 1));
    }
}

static void save_multitasking_settings() {
    main_thread([]() {
        settings::load_save_settings(true, set);
        hypriso->apply_workspace_settings();
        hypriso->generate_mylar_hyprland_config();
    });
}

static void fill_multitasking_settings(Container *root) {
    auto right = container_by_name("settings_right", root);
    if (!right)
        return;
    for (auto child: right->children)
        delete child;
    right->children.clear();

    right->pre_layout = layout_right_page;
    auto padded_right = right->child(FILL_SPACE, FILL_SPACE);

    make_section_title(padded_right, "Multitasking Settings");
    make_vert_space(padded_right, 12);
    make_button_group(padded_right, "App switching",
        "Choose which windows appear in the app switcher",
        {"All workspaces", "Current workspace"}, [](std::string selected) {
            set->app_switcher_current_workspace_only = selected == "Current workspace";
            save_multitasking_settings();
        }, set->app_switcher_current_workspace_only ? "Current workspace" : "All workspaces");
    make_vert_space(padded_right, 4);
    make_button_group(padded_right, "Workspace behavior",
        "Create workspaces as needed or keep a fixed number",
        {"Dynamic", "Fixed"}, [](std::string selected) {
            set->dynamic_workspaces = selected == "Dynamic";
            save_multitasking_settings();
        }, set->dynamic_workspaces ? "Dynamic" : "Fixed");
    make_vert_space(padded_right, 4);
    make_reset_textfield(padded_right, "Number of workspaces",
        "Per monitor in Fixed mode (1–20); occupied extras are kept", "", true,
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
        });
    make_vert_space(padded_right, 4);
    make_button_group(padded_right, "Multiple monitors",
        "Switch workspaces independently or on all monitors together",
        {"Independent", "Together"}, [](std::string selected) {
            set->workspaces_span_monitors = selected == "Together";
            save_multitasking_settings();
        }, set->workspaces_span_monitors ? "Together" : "Independent");
    make_vert_space(padded_right, 4);
    make_bool(padded_right, "Hotcorners", "Activates shortcuts when an edge is hit", set->hotcorners, [](bool value) {
        set->hotcorners = value;
        save_multitasking_settings();
    }, "\ue8b0");
    make_vert_space(padded_right, 4);
    make_bool(padded_right, "Window resizing at screen edges",
        "Automatically resize windows dragged to screen edges", set->resize_at_screen_edges, [](bool value) {
            set->resize_at_screen_edges = value;
            save_multitasking_settings();
        });
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
static void swifi_paint_signal(cairo_t *cr, double x, double y, double size, int strength, bool secured,
                               double dpi, RGBA background) {
    const char *level = strength >= 75 ? "" : strength >= 50 ? "" : strength >= 25 ? "" : "";
    RGBA faint = theme.text;
    faint.a *= .22;
    swifi_glyph(cr, x, y, "", size, faint);
    swifi_glyph(cr, x, y, level, size, theme.text);
    if (!secured)
        return;
    cairo_save(cr);
    cairo_new_path(cr);
    swifi_glyph(cr, x, y, "", size, theme.text, true);
    set_argb(cr, background);
    cairo_set_line_width(cr, 3 * dpi);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
    cairo_stroke_preserve(cr);
    set_argb(cr, theme.text);
    cairo_fill(cr);
    cairo_restore(cr);
}

static void swifi_paint_button(Container *root, Container *c, const std::string &text, bool enabled) {
    auto window = swifi_window(root);
    auto cr = window->cr;
    auto dpi = window->dpi;
    auto b = c->real_bounds;
    if (b.h <= 1 || b.w <= 1)
        return;
    set_argb(cr, !enabled ? theme.slider_track : c->state.mouse_pressing ? theme.button_pressed :
        c->state.mouse_hovering ? theme.button_hover : theme.button_background);
    drawRoundedRect(cr, b.x, b.y, b.w, b.h, settings_control_rounding * dpi, 1.0);
    cairo_fill(cr);
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
        cairo_save(window->cr);
        drawRoundedRect(window->cr, b.x, b.y, b.w, b.h, 5 * window->dpi, 1.0);
        cairo_clip(window->cr);
        set_argb(window->cr, swifi_row_color(data));
        cairo_paint(window->cr);
    };
    row->after_paint = [](Container *root, Container *c) {
        auto window = swifi_window(root);
        auto b = c->real_bounds;
        cairo_restore(window->cr);
        drawRoundedRect(window->cr, b.x, b.y, b.w, b.h, 5 * window->dpi, 1.0);
        set_argb(window->cr, theme.row_border);
        cairo_stroke(window->cr);
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
        swifi_paint_signal(window->cr, b.x + 19 * dpi, b.y + 19 * dpi, 20 * dpi, data->ap.strength, data->ap.secured,
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
    input->pre_layout = [](Container *root, Container *c, const Bounds &) {
        // "Show" turns masking off on the shared editor.
        static_cast<STextEditorData *>(c->user_data)->options.password = !settings_wifi->show_password;
    };
    input->when_paint = [](Container *root, Container *c) {
        auto window = swifi_window(root);
        auto cr = window->cr;
        auto dpi = window->dpi;
        auto b = c->real_bounds;
        if (b.h <= 2 * dpi)
            return;
        set_argb(cr, c->active ? theme.accent : theme.field_border);
        drawRoundedRect(cr, b.x, b.y, b.w, b.h, settings_control_rounding * dpi, 1.0);
        cairo_fill(cr);
        auto inner = b;
        inner.shrink(std::round(1 * dpi));
        set_argb(cr, theme.field_background);
        drawRoundedRect(cr, inner.x, inner.y, inner.w, inner.h, settings_control_rounding * dpi, 1.0);
        cairo_fill(cr);
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
        auto cr = window->cr;
        auto dpi = window->dpi;
        auto b = c->real_bounds;
        drawRoundedRect(cr, b.x, b.y, b.w, b.h, 5 * dpi, 1.0);
        set_argb(cr, theme.row_background);
        cairo_fill(cr);
        drawRoundedRect(cr, b.x, b.y, b.w, b.h, 5 * dpi, 1.0);
        set_argb(cr, theme.row_border);
        cairo_stroke(cr);

        const auto &view = settings_wifi->view;
        if (!view.scanning && !view.busy)
            return;
        // The dock menu's scanning animation: five dots easing along the card's bottom edge.
        double phase = (get_current_time_in_ms() % 2000) / 2000.0;
        cairo_save(cr);
        drawRoundedRect(cr, b.x, b.y, b.w, b.h, 5 * dpi, 1.0);
        cairo_clip(cr);
        for (int i = 0; i < 5; ++i) {
            double step = (phase - i * .08) / .6;
            if (step < 0 || step > 1) continue;
            double x = .5 - .5 * std::cos(step * M_PI);
            set_argb(cr, theme.accent);
            cairo_arc(cr, b.x + x * b.w, b.y + b.h - 5 * dpi, 2 * dpi, 0, 2 * M_PI);
            cairo_fill(cr);
        }
        cairo_restore(cr);
    };
    auto heading = card->child(FILL_SPACE, FILL_SPACE);
    heading->when_paint = [](Container *root, Container *c) {
        auto window = swifi_window(root);
        auto dpi = window->dpi;
        const auto &view = settings_wifi->view;
        auto b = c->real_bounds;
        bool on = view.status.service_running && !view.status.devices.empty() && view.status.wireless_enabled;
        swifi_glyph(window->cr, b.x + 19 * dpi, b.y + b.h * .5 - 10 * dpi, "", 16 * dpi,
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
        set_argb(window->cr, switchable && c->state.mouse_pressing ? theme.button_pressed :
            switchable && c->state.mouse_hovering ? theme.button_hover : theme.segment_background);
        drawRoundedRect(window->cr, pill.x, pill.y, pill.w, pill.h, h * .5, 1.0);
        cairo_fill(window->cr);
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
        set_argb(window->cr, swifi_mix(theme.row_background, theme.accent, tint));
        drawRoundedRect(window->cr, b.x, b.y, b.w, b.h, 5 * dpi, 1.0);
        cairo_fill(window->cr);
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
        auto font = get_cached_pango_font(window->cr, set->font, 11 * dpi, PANGO_WEIGHT_NORMAL, false);
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
        set_argb(window->cr, swifi_mix(theme.row_background, hue, .18));
        drawRoundedRect(window->cr, b.x, b.y, b.w, b.h, 5 * dpi, 1.0);
        cairo_fill(window->cr);
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
        drawRoundedRect(window->cr, b.x, b.y, b.w, b.h, 5 * dpi, 1.0);
        set_argb(window->cr, theme.row_background);
        cairo_fill(window->cr);
        drawRoundedRect(window->cr, b.x, b.y, b.w, b.h, 5 * dpi, 1.0);
        set_argb(window->cr, theme.row_border);
        cairo_stroke(window->cr);
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
    auto row = parent->child(::vbox, FILL_SPACE, 106);
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
        drawRoundedRect(window->cr, b.x, b.y, b.w, b.h, 5 * window->dpi, 1.0);
        set_argb(window->cr, data->hovered ? theme.row_hover : theme.row_background);
        cairo_fill(window->cr);
        drawRoundedRect(window->cr, b.x, b.y, b.w, b.h, 5 * window->dpi, 1.0);
        set_argb(window->cr, theme.row_border);
        cairo_stroke(window->cr);
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
        auto cr = window->cr;
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
            cairo_set_source_surface(cr, data->icon, std::round(c->real_bounds.x + 16 * dpi),
                                     std::round(c->real_bounds.y + c->real_bounds.h * .5 - h * .5));
            bool paused = !data->is_card && saudio_is_stream(data->node) && data->node.corked;
            cairo_paint_with_alpha(cr, paused ? .6 : 1.0);
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
        auto cr = window->cr;
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
        cairo_fill(cr);
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
        auto cr = window->cr;
        auto dpi = window->dpi;
        auto b = c->real_bounds;
        double size = 30 * dpi;
        Bounds circle(b.x + 13 * dpi, b.y + (b.h - size) * .5, size, size);
        if (c->state.mouse_hovering || c->state.mouse_pressing) {
            set_argb(cr, c->state.mouse_pressing ? theme.button_pressed : theme.button_hover);
            drawRoundedRect(cr, circle.x, circle.y, circle.w, circle.h, size * .5, 1.0);
            cairo_fill(cr);
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
        auto cr = window->cr;
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
        cairo_fill(cr);
        if (value > 0) {
            set_argb(cr, fill);
            drawRoundedRect(cr, x0, y, std::max(track_h, w * value), track_h, track_h * .5, 1.0);
            cairo_fill(cr);
        }
        double cx = x0 + w * value;
        Bounds thumb(cx - r, b.y + b.h * .5 - r, r * 2, r * 2);
        set_argb(cr, theme.slider_thumb);
        drawRoundedRect(cr, thumb.x, thumb.y, thumb.w, thumb.h, r, 1.0);
        cairo_fill(cr);
        set_argb(cr, theme.slider_track);
        drawRoundedRect(cr, thumb.x, thumb.y, thumb.w, thumb.h, r, 1.0);
        cairo_stroke(cr);
        thumb.shrink((c->state.mouse_hovering || data->dragging ? 4 : 5) * dpi);
        set_argb(cr, fill);
        drawRoundedRect(cr, thumb.x, thumb.y, thumb.w, thumb.h, thumb.w * .5, 1.0);
        cairo_fill(cr);
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

    // Level meter under the slider: shows whether the device/stream is actually producing sound.
    auto meter_line = row->child(::hbox, FILL_SPACE, 14);
    meter_line->pre_layout = [data](Container *root, Container *c, const Bounds &) {
        c->exists = !data->is_card;
        c->wanted_bounds.h = 14 * swifi_window(root)->dpi;
    };
    auto meter_left = meter_line->child(56, FILL_SPACE);
    meter_left->pre_layout = [](Container *root, Container *c, const Bounds &) {
        c->wanted_bounds.w = 56 * swifi_window(root)->dpi;
    };
    auto meter = meter_line->child(FILL_SPACE, FILL_SPACE);
    meter->when_paint = [data](Container *root, Container *c) {
        if (!data->node.peak_supported)
            return;
        auto window = swifi_window(root);
        auto cr = window->cr;
        auto dpi = window->dpi;
        auto b = c->real_bounds;
        double r = 8 * dpi;
        double h = 4 * dpi;
        double x0 = b.x + r;
        double w = std::max(1.0, b.w - 2 * r);
        double y = b.y + 2 * dpi;
        RGBA track = theme.slider_track;
        track.a *= .5;
        auto pill = [cr](double px, double py, double pw, double ph) {
            double rad = ph * .5;
            cairo_new_path(cr);
            cairo_arc(cr, px + pw - rad, py + rad, rad, -M_PI / 2, M_PI / 2);
            cairo_arc(cr, px + rad, py + rad, rad, M_PI / 2, 3 * M_PI / 2);
            cairo_close_path(cr);
        };
        set_argb(cr, track);
        pill(x0, y, w, h);
        cairo_fill(cr);
        double level = saudio_meter_level(data->shown_peak);
        if (level <= 0.001)
            return;
        RGBA green = set->dark_theme ? RGBA(.35, .80, .50, 1) : RGBA(.15, .62, .32, 1);
        RGBA fill = data->node.mute ? theme.text_secondary : green;
        if (level > .92 && !data->node.mute)
            fill = RGBA(.90, .55, .20, 1);
        set_argb(cr, fill);
        // Keep the round end at the peak; at low levels the pill's left end is clipped by the track.
        double fill_w = w * level;
        double pill_w = std::max(h, fill_w);
        cairo_save(cr);
        pill(x0, y, w, h);
        cairo_clip(cr);
        pill(x0 + fill_w - pill_w, y, pill_w, h);
        cairo_fill(cr);
        cairo_restore(cr);
    };
    auto meter_right = meter_line->child(64, FILL_SPACE);
    meter_right->pre_layout = [](Container *root, Container *c, const Bounds &) {
        c->wanted_bounds.w = 64 * swifi_window(root)->dpi;
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
        if (data->dragging || get_current_time_in_ms() - data->last_local_change < 700)
            redraw = true;
    }
    return redraw;
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
    tabs->when_paint = [](Container *root, Container *c) {
        auto window = swifi_window(root);
        auto b = c->real_bounds;
        set_argb(window->cr, theme.segment_background);
        drawRoundedRect(window->cr, b.x, b.y, b.w, b.h, 10 * window->dpi, 1.0);
        cairo_fill(window->cr);
    };
    for (int i = 0; i < 5; i++) {
        auto tab = tabs->child(FILL_SPACE, FILL_SPACE);
        tab->when_paint = [i](Container *root, Container *c) {
            auto window = swifi_window(root);
            auto cr = window->cr;
            auto dpi = window->dpi;
            auto b = c->real_bounds;
            bool selected = settings_audio_tab == i;
            if (selected || c->state.mouse_hovering) {
                set_argb(cr, selected ? theme.segment_selected : swifi_mix(theme.segment_background, theme.segment_selected, .5));
                drawRoundedRect(cr, b.x, b.y, b.w, b.h, 8 * dpi, 1.0);
                cairo_fill(cr);
            }
            swifi_text(root, b, saudio_tab_names[i], 11, selected ? theme.text : theme.text_secondary, true);
        };
        tab->when_clicked = [i](Container *root, Container *) {
            if (settings_audio_tab == i || !settings_audio)
                return;
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
            row->wanted_bounds.h = (data->is_card ? 70 : 106) * dpi;
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
        drawRoundedRect(window->cr, b.x, b.y, b.w, b.h, 5 * dpi, 1.0);
        set_argb(window->cr, theme.row_background);
        cairo_fill(window->cr);
        drawRoundedRect(window->cr, b.x, b.y, b.w, b.h, 5 * dpi, 1.0);
        set_argb(window->cr, theme.row_border);
        cairo_stroke(window->cr);
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
    std::thread([]() {
        datetime::timezones(); // Parse the zone tables off the UI thread.
        auto status = datetime::query_status();
        std::lock_guard lock(sdt.mutex);
        sdt.status = status;
        sdt.loaded = true;
    }).detach();
}

// Runs a privileged change on a worker thread (it may wait on a polkit prompt), then re-reads the state.
static void sdt_run_async(std::function<std::string()> job) {
    {
        std::lock_guard lock(sdt.mutex);
        if (sdt.busy)
            return;
        sdt.busy = true;
    }
    std::thread([job]() {
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
    }).detach();
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

static void sdt_paint_switch(cairo_t *cr, double x, double y, double dpi, bool on, bool enabled) {
    double w = 42 * dpi, h = 24 * dpi;
    RGBA track = on ? theme.accent : theme.toggle_border;
    if (!enabled)
        track.a *= .45;
    drawRoundedRect(cr, x, y, w, h, h * .5, 1.0);
    set_argb(cr, track);
    cairo_fill(cr);
    double knob = h - 6 * dpi;
    double kx = on ? x + w - knob - 3 * dpi : x + 3 * dpi;
    RGBA knob_color = on ? theme.toggle_knob_on : theme.toggle_knob_off;
    if (!on)
        knob_color = RGBA(1, 1, 1, 1);
    if (!enabled)
        knob_color.a *= .6;
    drawRoundedRect(cr, kx, y + 3 * dpi, knob, knob, knob * .5, 1.0);
    set_argb(cr, knob_color);
    cairo_fill(cr);
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
    auto row = parent->child(::hbox, FILL_SPACE, FILL_SPACE);
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
        w += accessory == 3 ? 42 * dpi : accessory ? 14 * dpi : 0;
        c->children[1]->wanted_bounds.w = w + 18 * dpi;
    };
    row->when_paint = [spec_ptr, is_first, is_last](Container *root, Container *c) {
        auto window = swifi_window(root);
        auto cr = window->cr;
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
        cairo_fill(cr);
        drawRoundedRect(cr, b.x, b.y, b.w, b.h, 8 * dpi, std::floor(1.0 * dpi), is_first, is_first, is_last, is_last);
        set_argb(cr, theme.row_border);
        cairo_set_line_width(cr, std::floor(1.0 * dpi));
        cairo_stroke(cr);

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
            right -= 42 * dpi;
            sdt_paint_switch(cr, right, b.y + b.h * .5 - 12 * dpi, dpi, spec_ptr->switch_on && spec_ptr->switch_on(), enabled);
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
        set_argb(window->cr, theme.popup_background);
        drawRoundedRect(window->cr, b.x, b.y, b.w, b.h, 8 * window->dpi, 1.0);
        cairo_fill(window->cr);
        set_argb(window->cr, theme.dropdown_border);
        drawRoundedRect(window->cr, b.x, b.y, b.w, b.h, 8 * window->dpi, 1.0);
        cairo_stroke(window->cr);
    };
    popup->root->when_key_event = [](Container *, Container *, int, bool pressed, xkb_keysym_t sym, int, bool, std::string) {
        if (pressed && sym == XKB_KEY_Escape)
            close_dropdown_menu();
    };
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
        set_argb(window->cr, bg);
        drawRoundedRect(window->cr, b.x, b.y, b.w, b.h, settings_control_rounding * window->dpi, 1.0);
        cairo_fill(window->cr);
        if (!primary) {
            set_argb(window->cr, theme.dropdown_border);
            drawRoundedRect(window->cr, b.x, b.y, b.w, b.h, settings_control_rounding * window->dpi, 1.0);
            cairo_stroke(window->cr);
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
        auto cr = window->cr;
        auto dpi = window->dpi;
        auto b = c->real_bounds;
        set_argb(cr, theme.field_background);
        drawRoundedRect(cr, b.x, b.y, b.w, b.h, settings_control_rounding * dpi, 1.0);
        cairo_fill(cr);
        set_argb(cr, theme.field_border);
        drawRoundedRect(cr, b.x, b.y, b.w, b.h, settings_control_rounding * dpi, 1.0);
        cairo_stroke(cr);
        double third = b.w / 4;
        if (c->state.mouse_hovering) {
            bool left = root->mouse_current_x < b.x + third;
            bool right = root->mouse_current_x > b.x + b.w - third;
            if (left || right) {
                set_argb(cr, c->state.mouse_pressing ? theme.dropdown_pressed : theme.dropdown_hover);
                drawRoundedRect(cr, left ? b.x + 2 * dpi : b.x + b.w - third + 1 * dpi, b.y + 2 * dpi, third - 3 * dpi, b.h - 4 * dpi,
                    (settings_control_rounding - 1) * dpi, 1.0);
                cairo_fill(cr);
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

struct SdtZoneList : UserData {
    std::vector<SdtZoneRow> all;
    std::vector<const SdtZoneRow *> shown;
    std::string current;
    float scroll = 0;
    float overflow = 0;
    bool scrolled_to_current = false;
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
        data->scroll = std::clamp(data->scroll, -data->overflow, 0.0f);
    };
    list->when_fine_scrolled = [](Container *root, Container *c, double, double scroll_y, bool) {
        auto data = (SdtZoneList *) c->user_data;
        auto dpi = swifi_window(root)->dpi;
        data->scroll = std::clamp(data->scroll + (float) (scroll_y * 3 * dpi), -data->overflow, 0.0f);
        swifi_redraw(root);
    };
    // Rows are painted directly (hundreds of zones), hit-testing by position.
    list->when_paint = [](Container *root, Container *c) {
        auto data = (SdtZoneList *) c->user_data;
        auto window = swifi_window(root);
        auto cr = window->cr;
        auto dpi = window->dpi;
        auto b = c->real_bounds;
        cairo_save(cr);
        cairo_rectangle(cr, b.x, b.y, b.w, b.h);
        cairo_clip(cr);
        double row_h = sdt_zone_row_h * dpi;
        int first = std::max(0, (int) std::floor(-data->scroll / row_h));
        for (int i = first; i < (int) data->shown.size(); i++) {
            double y = b.y + data->scroll + i * row_h;
            if (y > b.y + b.h)
                break;
            auto row = data->shown[i];
            Bounds rb(b.x, y, b.w - 8 * dpi, row_h);
            bool hovered = c->state.mouse_hovering && root->mouse_current_y >= y && root->mouse_current_y < y + row_h;
            if (hovered) {
                set_argb(cr, c->state.mouse_pressing ? theme.popup_pressed : theme.popup_hover);
                drawRoundedRect(cr, rb.x, rb.y, rb.w, rb.h, 6 * dpi, 1.0);
                cairo_fill(cr);
            }
            bool current = row->entry->id == data->current;
            if (current) {
                set_argb(cr, theme.accent);
                drawRoundedRect(cr, rb.x + 4 * dpi, rb.y + rb.h * .3, 3 * dpi, rb.h * .4, 1.5 * dpi, 1.0);
                cairo_fill(cr);
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
        cairo_restore(cr);
        if (data->overflow > 0) {
            double content = b.h + data->overflow;
            double thumb_h = std::max(24.0 * dpi, b.h * b.h / content);
            double ty = b.y + (-data->scroll / data->overflow) * (b.h - thumb_h);
            RGBA color = theme.text_secondary;
            color.a *= .4;
            set_argb(cr, color);
            drawRoundedRect(cr, b.x + b.w - 4 * dpi, ty, 3 * dpi, thumb_h, 1.5 * dpi, 1.0);
            cairo_fill(cr);
        }
    };
    list->when_clicked = [](Container *root, Container *c) {
        auto data = (SdtZoneList *) c->user_data;
        auto dpi = swifi_window(root)->dpi;
        int i = (int) std::floor((root->mouse_current_y - c->real_bounds.y - data->scroll) / (sdt_zone_row_h * dpi));
        if (i >= 0 && i < (int) data->shown.size())
            sdt_pick_zone(data->shown[i]->entry->id);
    };

    // The search field goes above the list.
    auto field = make_field(body, false, "", [data](std::string text) {
        sdt_filter_zones(data, text);
    });
    std::rotate(body->children.begin(), body->children.end() - 1, body->children.end());
    auto edit_key = std::move(field->when_key_event);
    field->when_key_event = [edit_key, data](Container *root, Container *c, int key, bool pressed, xkb_keysym_t sym,
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
        if (edit_key)
            edit_key(root, c, key, pressed, sym, mods, is_text, text);
    };
    set_active(popup->root, {field}, popup->root, true, false);
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

void create_tab_option(Container *parent, std::string label) {
    auto c = parent->child(::hbox, FILL_SPACE, FILL_SPACE);
    c->name = label;
    c->pre_layout = [](Container *root, Container *c, const Bounds &b) {
        auto mylar = (MylarWindow*)root->user_data;
        c->wanted_bounds.h = 40 * mylar->raw_window->dpi;
    };
    c->when_paint = [label](Container *root, Container *c) {
        auto mylar = (MylarWindow*)root->user_data;
        auto cr = mylar->raw_window->cr;
        auto dpi = mylar->raw_window->dpi;

        if (c->state.mouse_pressing) {
            auto b = c->real_bounds;
            b.shrink(3 * dpi);
            drawRoundedRect(cr, b.x, b.y, b.w, b.h, 5 * dpi, 1.0);
            set_argb(cr, theme.navigation_pressed);
            cairo_fill(cr);
        } else if (c->state.mouse_hovering) {
            auto b = c->real_bounds;
            b.shrink(3 * dpi);
            drawRoundedRect(cr, b.x, b.y, b.w, b.h, 5 * dpi, 1.0);
            set_argb(cr, theme.navigation_hover);
            cairo_fill(cr);
        }
        paint_label(root, c, label);
    };
    c->when_clicked = [label](Container *root, Container *c) {
        sdt_invalidate();
        if (label == "Mouse & Touchpad") {
            fill_mouse_settings(root, c);
        } else if (label == "Keyboard") {
            fill_keyboard_settings(root, c);
        } else if (label == "Desktop") {
            fill_desktop_settings(root, c);
        } else if (label == "Display") {
            fill_display_settings(root);
        } else if (label == "Shortcuts") {
            fill_shortcuts_settings(root);
        } else if (label == "Multitasking") {
            fill_multitasking_settings(root);
        } else if (label == "Wifi") {
            fill_wifi_settings(root);
        } else if (label == "Audio") {
            fill_audio_settings(root);
        } else if (label == "Time & Date") {
            fill_datetime_settings(root);
        }
    };
}

void fill_left(Container *left) {
    create_tab_option(left, "Search");
    create_tab_option(left, "Display");
    create_tab_option(left, "Desktop");
    create_tab_option(left, "Multitasking");
    create_tab_option(left, "Mouse & Touchpad");
    create_tab_option(left, "Keyboard");
    create_tab_option(left, "Shortcuts");
    create_tab_option(left, "Time & Date");
    create_tab_option(left, "Audio");
    create_tab_option(left, "Wifi");
}

void fill_root(Container *root) {
    root->when_paint = paint {
        auto mylar = (MylarWindow*)root->user_data;
        auto cr = mylar->raw_window->cr;
        set_argb(cr, theme.background);
        set_rect(cr, c->real_bounds);
        cairo_fill(cr);
    };
    auto left_right = root->child(::hbox, FILL_SPACE, FILL_SPACE);
    // Handle external navigation on the settings thread before laying out the page.
    left_right->pre_layout = [](Container *root, Container *, const Bounds &) {
        std::string page;
        page.swap(requested_page);
        if (!page.empty()) {
            if (auto tab = container_by_name(page, root)) {
                if (tab->when_clicked) {
                    if (auto right = container_by_name("settings_right", root))
                        ((RightData *) right->user_data)->scroll = 0;
                    tab->when_clicked(root, tab);
                }
            }
        }

        // Only present while the display page is showing.
        if (container_by_name("display_trial_banner", root)) {
            if (display_trial.page_stale)
                fill_display_settings(root);
            const bool active = display_trial.active;
            container_by_name("display_trial_banner", root)->exists = active;
            container_by_name("display_trial_space", root)->exists  = active;
        }
    };
    
    auto left = left_right->child(::vbox, 300, FILL_SPACE);
    left->pre_layout = [](Container *root, Container *c, const Bounds &b) {
        auto mylar = (MylarWindow*)root->user_data;
        c->wanted_bounds.w = 200 * mylar->raw_window->dpi;
    };
    left->when_paint = paint {
        return;
        auto mylar = (MylarWindow*)root->user_data;
        auto cr = mylar->raw_window->cr;
        set_argb(cr, theme.background);
        set_rect(cr, c->real_bounds);
        cairo_fill(cr);
    };
    fill_left(left);

    auto right = left_right->child(::fullycustom, FILL_SPACE, FILL_SPACE);
    right->name = "settings_right";
    auto d = new RightData;
    right->user_data = d;
    right->when_paint = paint {
        return;
        auto mylar = (MylarWindow*)root->user_data;
        auto cr = mylar->raw_window->cr;
        set_argb(cr, theme.background);
        set_rect(cr, c->real_bounds);
        cairo_fill(cr);
    };
    right->receive_events_even_if_obstructed = true;
    right->when_fine_scrolled = [](Container* root, Container* c, double scroll_x, double scroll_y, bool came_from_touchpad) {
        auto d = (RightData *) c->user_data;
        auto mylar = (MylarWindow*)root->user_data;
        // Clamp against the limits from the last real layout pass.
        d->scroll = std::clamp(d->applied_scroll + (float) (scroll_y * 3 * mylar->raw_window->dpi), -d->overflow, 0.0f);
    };
    right->after_paint = paint_right_scrollbar;
}

void actual_start() {
    theme = set->dark_theme ? dark_theme : light_theme;
    settings_app = windowing::open_app();
    RawWindowSettings settings;
    settings.pos.w = 1000;
    settings.pos.h = 760;
    settings.pos.min_w = 800;
    settings.pos.min_h = 600;
    settings.name = "Settings";
    settings.app_id = "preferences-system";
    auto mylar = open_mylar_window(settings_app, WindowType::NORMAL, settings);
    mylar->root->user_data = mylar;
    settings_mylar = mylar;
    fill_root(mylar->root);
    mylar->raw_window->on_close = [](RawWindow *w) {
        settings_wifi_release();
        settings_audio_release();
        dropdown_popup = nullptr;
        for (auto p : popups) {
            windowing::close_window(p->raw_window);
        }
        popups.clear();
        std::this_thread::sleep_for(std::chrono::milliseconds(40));
    };

    display_trial_watch();

    windowing::main_loop(settings_app);

    cleanup_cached_pango_fonts();

    settings_app = nullptr;
    settings_mylar = nullptr;
};

void settings::start(std::string page) {
    if (!page.empty()) {
        requested_page = page;
    }
    if (settings_app) {
        if (settings_mylar)
            windowing::redraw(settings_mylar->raw_window);
        return;
    }
    std::thread t(actual_start);
    t.detach();
}

void settings::stop() {
    if (settings_app) {
        windowing::close_app(settings_app);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

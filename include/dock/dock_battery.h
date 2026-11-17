#pragma once

#include "simple_dbus.h"
#include "container.h"
#include "hypriso.h"

#include <string>

struct Dock;
namespace drawing { class Context; }

void fill_battery_container(Dock *dock);
bool has_internal_battery();
void watch_battery_level();
BatteryStatus battery_status_snapshot();
void request_battery_refresh();
void battery_stop();

// For the Battery page in Settings; safe to call from any thread.
struct BatterySummary {
    bool valid = false;
    std::string percent;  // "84%", or "—" without a reading
    std::string state;    // "On battery", "Charging", ...
    std::string estimate; // Remaining time, or why there is none
    std::string detail;   // When it runs out; empty without a reading
    std::string rate;     // Discharge rate; empty unless on battery
    std::string message;  // Pending action or the last error
    bool message_is_error = false;
};
BatterySummary battery_summary();

bool battery_history_enabled();
void set_battery_history_enabled(bool enabled);

struct BatteryGraphPalette {
    RGBA text = {.12, .17, .24, 1};
    RGBA muted = {.40, .46, .54, 1};
    RGBA grid = {.89, .92, .95, 1};
    RGBA now_line = {.68, .75, .83, 1};
};
// The last 3 hours of charge with a discharge projection; mouse_x is shown when hovered.
void paint_battery_history(drawing::Context *cr, const Bounds &b, double dpi, double mouse_x, bool hovered,
                           const BatteryGraphPalette &palette = {});

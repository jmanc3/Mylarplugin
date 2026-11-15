#pragma once

// Where the wallpaper goes on each monitor, shared by the compositor (which
// draws it) and the Settings preview (which shows and edits it).
//
// Monitors follow the shared wallpaper (~/.config/mylar/wall.png, with the
// wallpaper_* settings) unless they have an entry in wallpaper_monitors, which
// gives them their own scaling and, when ~/.config/mylar/wall-<name>.png
// exists, their own image. With wallpaper_span the shared wallpaper is laid
// out once over the bounding box of every monitor that follows it.
//
// Layout math happens in logical (layout) coordinates; the results are
// converted to each monitor's own pixels at the end.

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

#include "hypriso.h"

inline const std::vector<std::string> wallpaper_scaling_modes = {"Fill", "Fit", "Stretch", "Center", "Tile", "Custom"};

struct WallRect {
    double x = 0, y = 0, w = 0, h = 0;
};

struct WallParams {
    std::string mode = "Fill";
    // Custom only: zoom relative to Fill, and the image point (0..1) shown at
    // the center of the area.
    double zoom = 1.0;
    double center_x = .5;
    double center_y = .5;
};

struct WallMonitorOverride {
    std::string name;
    WallParams params;
};

// A monitor as the wallpaper sees it: logical bounds and scale.
struct WallMonitor {
    std::string name;
    double x = 0, y = 0, w = 0, h = 0;
    double scale = 1;
};

// Monitors, published by the compositor for the Settings thread.
inline std::mutex wallpaper_monitors_mutex;
inline std::vector<WallMonitor> wallpaper_monitors_snapshot;

inline std::vector<WallMonitor> wallpaper_monitors_copy() {
    std::lock_guard<std::mutex> lock(wallpaper_monitors_mutex);
    return wallpaper_monitors_snapshot;
}

// Implemented by the compositor (heart.cpp): reloads wallpapers whose file or
// settings changed. Call on the compositor's main thread.
void wallpaper_settings_changed();

// ---- Settings <-> overrides --------------------------------------------------

inline std::string wallpaper_mode_or_fill(const std::string &mode) {
    for (auto &m : wallpaper_scaling_modes)
        if (m == mode)
            return mode;
    return "Fill";
}

// "DP-1,Custom,1.2,0.5,0.5;HDMI-A-1,Fit,1,0.5,0.5"
inline std::vector<WallMonitorOverride> wallpaper_parse_overrides(const std::string &text) {
    std::vector<WallMonitorOverride> result;
    std::stringstream entries(text);
    std::string entry;
    while (std::getline(entries, entry, ';')) {
        std::vector<std::string> parts;
        std::stringstream fields(entry);
        std::string field;
        while (std::getline(fields, field, ','))
            parts.push_back(field);
        if (parts.size() < 2 || parts[0].empty())
            continue;
        WallMonitorOverride o;
        o.name = parts[0];
        o.params.mode = wallpaper_mode_or_fill(parts[1]);
        if (parts.size() >= 5) {
            o.params.zoom = std::strtod(parts[2].c_str(), nullptr);
            o.params.center_x = std::strtod(parts[3].c_str(), nullptr);
            o.params.center_y = std::strtod(parts[4].c_str(), nullptr);
            if (!(o.params.zoom > 0))
                o.params.zoom = 1;
        }
        result.push_back(o);
    }
    return result;
}

inline std::string wallpaper_serialize_overrides(const std::vector<WallMonitorOverride> &overrides) {
    std::string out;
    for (auto &o : overrides) {
        char buf[128];
        snprintf(buf, sizeof(buf), ",%s,%.5f,%.5f,%.5f", o.params.mode.c_str(), o.params.zoom, o.params.center_x, o.params.center_y);
        if (!out.empty())
            out += ";";
        out += o.name + buf;
    }
    return out;
}

inline WallParams wallpaper_shared_params(const ConfigSettings &s) {
    WallParams p;
    p.mode = wallpaper_mode_or_fill(s.wallpaper_scaling);
    p.zoom = s.wallpaper_zoom > 0 ? s.wallpaper_zoom : 1;
    p.center_x = s.wallpaper_center_x;
    p.center_y = s.wallpaper_center_y;
    return p;
}

inline void wallpaper_set_shared_params(ConfigSettings &s, const WallParams &p) {
    s.wallpaper_scaling = p.mode;
    s.wallpaper_zoom = p.zoom;
    s.wallpaper_center_x = p.center_x;
    s.wallpaper_center_y = p.center_y;
}

// ---- Files -------------------------------------------------------------------

inline std::string wallpaper_safe_name(const std::string &name) {
    std::string out;
    for (char ch : name)
        out += (std::isalnum((unsigned char) ch) || ch == '-' || ch == '_') ? ch : '_';
    return out;
}

inline std::filesystem::path wallpaper_config_dir() {
    const char *home = std::getenv("HOME");
    return std::filesystem::path(home ? home : "/tmp") / ".config/mylar";
}

inline std::string wallpaper_shared_path() {
    return (wallpaper_config_dir() / "wall.png").string();
}

inline std::string wallpaper_monitor_path(const std::string &monitor) {
    return (wallpaper_config_dir() / ("wall-" + wallpaper_safe_name(monitor) + ".png")).string();
}

// Changes when the file does; empty when it is missing.
inline std::string wallpaper_file_stamp(const std::string &path) {
    std::error_code ec;
    auto size = std::filesystem::file_size(path, ec);
    if (ec)
        return "";
    auto time = std::filesystem::last_write_time(path, ec);
    if (ec)
        return "";
    return std::to_string(size) + ":" + std::to_string(time.time_since_epoch().count());
}

// ---- Layout ------------------------------------------------------------------

// What one monitor shows.
struct WallResolved {
    WallParams params;
    bool separate = false;  // has its own entry in wallpaper_monitors
    bool spanning = false;  // part of a spanned shared wallpaper
    std::string path;       // image file
    WallRect area;          // logical area the image is laid out on
    double pixel_scale = 1; // image pixels to logical units (Center, Tile)
};

inline const WallMonitorOverride *wallpaper_find_override(const std::vector<WallMonitorOverride> &overrides, const std::string &name) {
    for (auto &o : overrides)
        if (o.name == name)
            return &o;
    return nullptr;
}

// Bounding box of every monitor following the shared wallpaper.
inline WallRect wallpaper_shared_union(const std::vector<WallMonitor> &monitors, const std::vector<WallMonitorOverride> &overrides, int *count = nullptr) {
    double x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    int n = 0;
    for (auto &m : monitors) {
        if (wallpaper_find_override(overrides, m.name))
            continue;
        if (n == 0) {
            x0 = m.x, y0 = m.y, x1 = m.x + m.w, y1 = m.y + m.h;
        } else {
            x0 = std::min(x0, m.x), y0 = std::min(y0, m.y);
            x1 = std::max(x1, m.x + m.w), y1 = std::max(y1, m.y + m.h);
        }
        n++;
    }
    if (count)
        *count = n;
    return {x0, y0, x1 - x0, y1 - y0};
}

inline WallResolved wallpaper_resolve(const WallParams &shared, bool span, const std::vector<WallMonitorOverride> &overrides,
                                      const std::vector<WallMonitor> &monitors, const WallMonitor &m) {
    WallResolved r;
    r.area = {m.x, m.y, m.w, m.h};
    r.pixel_scale = m.scale > 0 ? 1.0 / m.scale : 1.0;
    if (auto o = wallpaper_find_override(overrides, m.name)) {
        r.separate = true;
        r.params = o->params;
        auto own = wallpaper_monitor_path(m.name);
        std::error_code ec;
        r.path = std::filesystem::exists(own, ec) ? own : wallpaper_shared_path();
        return r;
    }
    r.params = shared;
    r.path = wallpaper_shared_path();
    int count = 0;
    auto u = wallpaper_shared_union(monitors, overrides, &count);
    if (span && count > 1) {
        r.spanning = true;
        r.area = u;
        r.pixel_scale = 1;
    }
    return r;
}

// Keeps a Custom crop a subregion of the image: never zoomed out past Fill and
// never moved so far that the area shows past the image's edges (black).
inline WallParams wallpaper_clamp_custom(WallParams p, double iw, double ih, double aw, double ah) {
    if (p.mode != "Custom" || iw <= 0 || ih <= 0 || aw <= 0 || ah <= 0)
        return p;
    if (!(p.zoom >= 1))
        p.zoom = 1;
    const double fill = std::max(aw / iw, ah / ih);
    const double w = iw * fill * p.zoom, h = ih * fill * p.zoom;
    // Half the area as a fraction of the image.
    const double hx = std::min(.5, aw * .5 / w), hy = std::min(.5, ah * .5 / h);
    p.center_x = std::clamp(std::isfinite(p.center_x) ? p.center_x : .5, hx, 1 - hx);
    p.center_y = std::clamp(std::isfinite(p.center_y) ? p.center_y : .5, hy, 1 - hy);
    return p;
}

// Rect the whole image covers within an area of aw x ah.
inline WallRect wallpaper_image_rect(const WallParams &p, double iw, double ih, double aw, double ah, double pixel_scale = 1.0) {
    if (iw <= 0 || ih <= 0)
        return {0, 0, aw, ah};
    const double fill = std::max(aw / iw, ah / ih);
    if (p.mode == "Stretch")
        return {0, 0, aw, ah};
    if (p.mode == "Fit") {
        const double s = std::min(aw / iw, ah / ih);
        return {(aw - iw * s) * .5, (ah - ih * s) * .5, iw * s, ih * s};
    }
    if (p.mode == "Center")
        return {(aw - iw * pixel_scale) * .5, (ah - ih * pixel_scale) * .5, iw * pixel_scale, ih * pixel_scale};
    if (p.mode == "Tile")
        return {0, 0, iw * pixel_scale, ih * pixel_scale};
    if (p.mode == "Custom") {
        const auto c = wallpaper_clamp_custom(p, iw, ih, aw, ah);
        const double s = fill * c.zoom;
        const double w = iw * s, h = ih * s;
        return {aw * .5 - c.center_x * w, ah * .5 - c.center_y * h, w, h};
    }
    // Fill
    return {(aw - iw * fill) * .5, (ah - ih * fill) * .5, iw * fill, ih * fill};
}

// Every rect the image is drawn into, relative to the area: one, or a grid
// covering the area (plus margin, for the zoomed initial fade in) when tiling.
inline std::vector<WallRect> wallpaper_area_rects(const WallParams &p, double iw, double ih, double aw, double ah, double pixel_scale = 1.0) {
    auto r = wallpaper_image_rect(p, iw, ih, aw, ah, pixel_scale);
    if (p.mode != "Tile" || r.w < 1 || r.h < 1)
        return {r};
    std::vector<WallRect> rects;
    const double margin_x = aw * .25, margin_y = ah * .25;
    const double start_x = -std::ceil(margin_x / r.w) * r.w;
    const double start_y = -std::ceil(margin_y / r.h) * r.h;
    for (double y = start_y; y < ah + margin_y; y += r.h)
        for (double x = start_x; x < aw + margin_x; x += r.w)
            rects.push_back({x, y, r.w, r.h});
    return rects;
}

// The rects to draw on one monitor, in that monitor's own pixels.
inline std::vector<WallRect> wallpaper_monitor_rects(const WallResolved &res, double iw, double ih, const WallMonitor &m) {
    std::vector<WallRect> out;
    const double s = m.scale > 0 ? m.scale : 1;
    const double mw = m.w * s, mh = m.h * s;
    for (auto &r : wallpaper_area_rects(res.params, iw, ih, res.area.w, res.area.h, res.pixel_scale)) {
        WallRect p{(res.area.x + r.x - m.x) * s, (res.area.y + r.y - m.y) * s, r.w * s, r.h * s};
        if (p.x > mw * 1.25 || p.y > mh * 1.25 || p.x + p.w < -mw * .25 || p.y + p.h < -mh * .25)
            continue;
        out.push_back(p);
    }
    return out;
}

// True when the image can leave parts of the monitor uncovered (shown black).
inline bool wallpaper_needs_backdrop(const WallParams &p) {
    return p.mode == "Fit" || p.mode == "Center" || p.mode == "Custom";
}

// The Custom parameters that reproduce what a mode shows (for switching to
// Custom by dragging the preview).
inline WallParams wallpaper_custom_from(const WallParams &p, double iw, double ih, double aw, double ah, double pixel_scale = 1.0) {
    WallParams c;
    c.mode = "Custom";
    if (p.mode == "Custom")
        return p;
    if (iw <= 0 || ih <= 0 || aw <= 0 || ah <= 0 || p.mode == "Stretch" || p.mode == "Tile")
        return c; // no exact equivalent: start from Fill
    auto r = wallpaper_image_rect(p, iw, ih, aw, ah, pixel_scale);
    const double fill = std::max(aw / iw, ah / ih);
    c.zoom = r.w / (iw * fill);
    c.center_x = (aw * .5 - r.x) / r.w;
    c.center_y = (ah * .5 - r.y) / r.h;
    return wallpaper_clamp_custom(c, iw, ih, aw, ah);
}

// Identifies one rendering of a monitor's wallpaper; a cached image is valid
// only for the key it was made with.
inline std::string wallpaper_cache_key(const WallResolved &res, const WallMonitor &m, const std::string &file_stamp) {
    char buf[320];
    snprintf(buf, sizeof(buf), "%s %.5f %.5f %.5f | %.2f %.2f %.2f %.2f %.4f | %.2f %.2f %.2f %.2f %.5f | ", res.params.mode.c_str(),
             res.params.mode == "Custom" ? res.params.zoom : 1.0, res.params.mode == "Custom" ? res.params.center_x : .5,
             res.params.mode == "Custom" ? res.params.center_y : .5, m.x, m.y, m.w, m.h, m.scale, res.area.x, res.area.y, res.area.w,
             res.area.h, res.pixel_scale);
    return std::string(buf) + res.path + " " + file_stamp;
}

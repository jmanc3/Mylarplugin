#include "dock_thumbnails.h"

#include "audio.h"
#include "heart.h"
#include "mpris.h"
#include "popup.h"
#include "titlebar.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <linux/input-event-codes.h>
#include <map>
#include <unordered_map>
#include <vector>

// The dock lives in its own Wayland client, so it cannot draw over its own
// windows. Instead the preview is a normal compositor-side overlay container
// painted in RENDER_PRE_CURSOR (above the dock layer) using the captured
// window thumbnails, exactly like alt_tab/coverflow do.

namespace {

constexpr float kPad = 10;
constexpr float kGap = 8;
constexpr float kThumbSize = 150;      // logical px, along the short side
// Widest a thumbnail is allowed to get. The short side stays at kThumbSize and
// wider windows have their screenshot letterboxed instead of stretching the
// preview past the width a 16:9 thumbnail used to occupy.
constexpr float kThumbMaxWidth = kThumbSize * 16.f / 9.f;
constexpr float kMaxPageFraction = .8; // of the monitor, before shrinking
constexpr float kDockMargin = 8;
constexpr int kCloseGraceMs = 220;
constexpr int kDndRaiseMs = 450;  // drag-and-drop hover time before a thumbnail raises its window
constexpr int kDndPollMs = 30;
constexpr float kCloseInset = 6;
constexpr float kCloseMinSize = 12;
constexpr float kCloseMaxSize = 22;
constexpr int kStripMax = 6;    // up to this many windows use a single strip
constexpr int kGridLineMax = 6; // grid rows/columns along the dock edge
constexpr float kMediaMinSize = 18;
constexpr float kMediaMaxSize = 28;
constexpr float kMediaGap = 4;
constexpr int kMediaPollMs = 1000; // re-link audio streams/players while the preview is open
// Timers use whole milliseconds: 42ms ~= 24 FPS for focused/below/hovered windows.
constexpr long kFastCaptureMs = 42;
constexpr long kMaxFastCaptureMs = 250;
constexpr long kSlowCaptureMs = 400;
constexpr long kMaxSlowCaptureMs = 2000;

// Roles of the buttons inside a thumbnail (stored in datum "role").
enum ButtonRole {
    kRoleClose = 1,
    kRolePrevious,
    kRolePlayPause,
    kRoleNext,
    kRoleMute,
};

// Media controls that apply to one window, found by linking MPRIS players and
// audio streams to the window's process (see compute_media).
struct Media {
    std::string player; // MPRIS bus name; empty when no player belongs to the window
    bool playing = false;
    bool can_play_pause = false;
    bool can_next = false;
    bool can_previous = false;

    // Audio backend fallback for windows without an MPRIS player: their
    // playback streams can at least be muted.
    std::vector<uint32_t> streams;
    bool muted = false;

    bool any() const { return !player.empty() || !streams.empty(); }
    bool operator==(const Media &) const = default;
};

// dock alignment: 1 top, 2 right, 3 bottom, 4 left
constexpr int kEdgeTop = 1;
constexpr int kEdgeRight = 2;
constexpr int kEdgeBottom = 3;
constexpr int kEdgeLeft = 4;

struct State {
    Container *container = nullptr;
    std::vector<int> ids;
    std::string source; // uuid of the dock icon this preview belongs to
    int generation = 0;
    bool hovered = false;
    bool pending_close = false;

    // Placement of the preview that is currently open, so a rebuild (after
    // closing one window) can stay anchored to the same dock edge.
    Bounds anchor_dock;
    int anchor_edge = kEdgeBottom;
    bool has_anchor = false;

    // Bounds of the owning icon (dock-local logical pixels), so the preview can
    // stay centered on it while the dock reflows.
    Bounds anchor_icon;
    bool has_icon = false;

    // Bumped every time a preview is (re)built so delayed screenshot refreshes
    // from an older preview become no-ops.
    int refresh_token = 0;

    // Drag-and-drop hover tracking (see watch_dnd_hover).
    bool dnd_watching = false;
    bool dnd_over_preview = false;
    int dnd_hover_cid = -1;
    long dnd_hover_start = 0;

    // Media controls per window id of the open preview.
    std::map<int, Media> media;

    // Adaptive thumbnail capture state (matches overview/alt_tab/snap_assist).
    std::map<int, long> window_captures;
    double smoothed_capture_ms = 0.0;
    size_t capture_cursor = 0;
};

State &state() {
    static State s;
    return s;
}

struct Placement {
    Bounds dock;
    int edge = kEdgeBottom;
    bool has_center = false;
    double center_x = 0;
    double center_y = 0;
    bool has_icon = false;
    Bounds icon; // dock-local logical pixels
};

bool same_ids(const std::vector<int> &a, const std::vector<int> &b) {
    if (a.size() != b.size())
        return false;
    for (size_t i = 0; i < a.size(); i++)
        if (a[i] != b[i])
            return false;
    return true;
}

bool same_bounds(const Bounds &a, const Bounds &b) {
    constexpr double epsilon = .5;
    return std::abs(a.x - b.x) < epsilon && std::abs(a.y - b.y) < epsilon &&
           std::abs(a.w - b.w) < epsilon && std::abs(a.h - b.h) < epsilon;
}

Bounds union_bounds(const Bounds &a, const Bounds &b) {
    if (a.w <= 0 || a.h <= 0)
        return b;
    if (b.w <= 0 || b.h <= 0)
        return a;
    const double x = std::min(a.x, b.x);
    const double y = std::min(a.y, b.y);
    const double right = std::max(a.right(), b.right());
    const double bottom = std::max(a.bottom(), b.bottom());
    return Bounds(x, y, right - x, bottom - y);
}

// Keeps a rebuilt preview open while the pointer is still within the area the
// old preview covered. Closing a thumbnail can shrink the panel away from the
// cursor; the user should still be able to reach the next close button.
void watch_rebuilt_preview(const std::string &source, int generation, const Bounds &safe_bounds, int attempts = 10) {
    later(120, [source, generation, safe_bounds, attempts](Timer *) {
        auto &s = state();
        if (!s.container || s.source != source || s.generation != generation)
            return;
        const auto m = mouse();
        if (bounds_contains(s.container->real_bounds, (int) m.x, (int) m.y)) {
            s.hovered = true;
            s.pending_close = false;
            s.generation++;
            return;
        }
        if (attempts <= 0 || !bounds_contains(safe_bounds, (int) m.x, (int) m.y)) {
            dock_thumbnails::close();
            return;
        }
        watch_rebuilt_preview(source, generation, safe_bounds, attempts - 1);
    });
}

// Captures live window thumbnails while the preview is open, using the same
// adaptive timing as overview: the top two focused windows on each monitor's
// active workspace and the hovered thumbnail refresh at ~24 FPS (42ms), while
// other windows refresh at a slower rate when dirty or due for fallback.
long capture_thumbnails(Container *preview) {
    if (!preview || preview->children.empty())
        return kFastCaptureMs;

    auto &s = state();
    const auto tick_start = std::chrono::steady_clock::now();
    const auto now = get_current_time_in_ms();
    const int monitor = *datum<int>(preview, "mid");

    const double render_ms = hypriso->last_render_ms(monitor);
    const double fps = std::max(1.0, (double) hypriso->fps(monitor));
    const double frame_budget_ms = 1000.0 / fps;
    const double load_ratio = (render_ms + s.smoothed_capture_ms) / std::max(1.0, frame_budget_ms);
    const double load_scale = load_ratio <= 0.35 ? 1.0 : 1.0 + (load_ratio - 0.35) * 4.0;

    const long effective_fast_ms = std::clamp<long>(std::lround(kFastCaptureMs * load_scale), kFastCaptureMs, kMaxFastCaptureMs);
    const long effective_slow_ms = std::clamp<long>(std::lround(kSlowCaptureMs * load_scale), kSlowCaptureMs, kMaxSlowCaptureMs);
    const double tick_budget_ms = std::clamp(frame_budget_ms * 0.35 - render_ms * 0.5, 1.5, 6.0);

    // The compositor stack runs from bottom to top. Capture the focused window
    // and the one directly below it on each monitor's active workspace at 24 FPS.
    std::map<int, int> active_workspaces;
    std::map<int, int> priority_counts;
    std::vector<int> priority_windows;
    for (auto m : actual_monitors) {
        const int mid = *datum<int>(m, "cid");
        active_workspaces[mid] = hypriso->get_active_workspace_id(mid);
    }
    const auto stacking = get_window_stacking_order();
    for (auto it = stacking.rbegin(); it != stacking.rend(); ++it) {
        const int mid = get_monitor(*it);
        if (!active_workspaces.contains(mid) || priority_counts[mid] >= 2 || is_slept(*it) || !get_cid_container(*it))
            continue;
        if (hypriso->get_active_workspace_id_client(*it) != active_workspaces.at(mid))
            continue;
        priority_windows.push_back(*it);
        priority_counts[mid]++;
    }

    struct CaptureCandidate {
        int cid;
        int tier; // 0 = missing thumbnail, 1 = hovered, 2 = priority, 3 = dirty/fallback
        Bounds bounds;
    };
    std::vector<CaptureCandidate> candidates;
    for (auto *thumb : preview->children) {
        const int cid = *datum<int>(thumb, "cid");
        if (!get_cid_container(cid))
            continue;
        const bool hovered = thumb->state.mouse_hovering ||
            (s.dnd_hover_cid != -1 && s.dnd_hover_cid == cid) ||
            std::any_of(thumb->children.begin(), thumb->children.end(), [](Container *sub) { return sub->state.mouse_hovering; });
        const bool missing = !hypriso->has_thumbnail(cid);
        const bool fast = hovered || std::find(priority_windows.begin(), priority_windows.end(), cid) != priority_windows.end();
        const bool dirty = hypriso->is_thumb_dirty(cid);
        auto prev = s.window_captures.find(cid);
        const long last_cap = prev != s.window_captures.end() ? prev->second : 0;
        const long elapsed = last_cap == 0 ? effective_slow_ms * 4 : now - last_cap;
        if (!missing) {
            const long min_interval = fast ? effective_fast_ms : effective_slow_ms;
            if (elapsed < min_interval)
                continue;
            // Focused, one-below, and hovered windows always refresh at the fast
            // 24 FPS rate so they appear real-time (including video subsurfaces).
            // Background windows skip re-capture unless dirty or due for fallback.
            if (!fast && !dirty && elapsed < effective_slow_ms * 3)
                continue;
        }
        const int tier = missing ? 0 : (hovered ? 1 : (fast ? 2 : 3));
        candidates.push_back({cid, tier, thumb->real_bounds});
    }

    if (!candidates.empty()) {
        std::stable_sort(candidates.begin(), candidates.end(), [](const CaptureCandidate &a, const CaptureCandidate &b) {
            return a.tier < b.tier;
        });
        // Rotate equal-priority non-missing candidates so budgeted ticks never starve a window.
        size_t rot_start = 0;
        while (rot_start < candidates.size() && candidates[rot_start].tier <= 1)
            rot_start++;
        if (candidates.size() - rot_start > 1) {
            const size_t span = candidates.size() - rot_start;
            const size_t shift = s.capture_cursor % span;
            std::rotate(candidates.begin() + rot_start, candidates.begin() + rot_start + shift, candidates.end());
        }
    }

    bool budget_exceeded = false;
    size_t captured_count = 0;
    for (const auto &cand : candidates) {
        const double elapsed_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - tick_start).count();
        if (cand.tier > 0 && captured_count > 0 && elapsed_ms >= tick_budget_ms) {
            budget_exceeded = true;
            break;
        }
        hypriso->screenshot(cand.cid);
        s.window_captures[cand.cid] = get_current_time_in_ms();
        captured_count++;
        if (cand.bounds.w > 0 && cand.bounds.h > 0) {
            Bounds dmg = cand.bounds;
            dmg.grow(4.0);
            hypriso->damage_box(dmg);
        }
    }
    if (captured_count > 0)
        s.capture_cursor += captured_count;
    std::erase_if(s.window_captures, [](const auto &entry) { return !get_cid_container(entry.first); });

    const double total_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - tick_start).count();
    s.smoothed_capture_ms = s.smoothed_capture_ms * 0.75 + total_ms * 0.25;
    if (budget_exceeded)
        return std::min(effective_fast_ms, 50L);
    return effective_fast_ms;
}

void destroy_now() {
    auto &s = state();
    s.dnd_hover_cid = -1;
    s.dnd_over_preview = false;
    s.generation++;
    s.refresh_token++;
    s.pending_close = false;
    s.hovered = false;
    s.has_anchor = false;
    s.has_icon = false;
    s.source.clear();
    s.ids.clear();
    s.media.clear();
    s.window_captures.clear();
    s.smoothed_capture_ms = 0.0;
    s.capture_cursor = 0;
    if (!s.container)
        return;
    auto *c = s.container;
    s.container = nullptr;
    if (auto *parent = c->parent) {
        parent->children.erase(std::remove(parent->children.begin(), parent->children.end(), c), parent->children.end());
    }
    delete c;
    damage_all();
}

bool cursor_over_dock(double x, double y, Bounds *out = nullptr) {
    for (auto *c : actual_root->children) {
        if (c->custom_type != (int) TYPE::LAYER)
            continue;
        const int cid = *datum<int>(c, "cid");
        if (!is_dock_layer(cid))
            continue;
        Bounds b = bounds_layer(cid);
        if (b.w <= 0 || b.h <= 0)
            b = c->real_bounds;
        if (b.w <= 0 || b.h <= 0)
            continue;
        if (bounds_contains(b, (int) x, (int) y)) {
            if (out)
                *out = b;
            return true;
        }
    }
    return false;
}

int dock_edge_for(const Bounds &dock, const Bounds &monitor) {
    // Docks are strips anchored to one monitor edge; compare against the
    // reserved monitor box so a full-width dock still resolves correctly.
    if (dock.w >= dock.h) {
        const double to_top = std::abs(dock.y - monitor.y);
        const double to_bottom = std::abs(monitor.bottom() - dock.bottom());
        return to_top < to_bottom ? kEdgeTop : kEdgeBottom;
    }
    const double to_left = std::abs(dock.x - monitor.x);
    const double to_right = std::abs(monitor.right() - dock.right());
    return to_left < to_right ? kEdgeLeft : kEdgeRight;
}

// While a drag-and-drop is in progress the normal hover events can be held back
// (the button that started the drag is still down), so poll the cursor instead.
// Keeps the preview open while the drag is over it, and raises the window of a
// thumbnail the drag has rested on for kDndRaiseMs so the drop can land there.
void watch_dnd_hover() {
    auto &s = state();
    if (s.dnd_watching)
        return;
    s.dnd_watching = true;
    later(kDndPollMs, [](Timer *) {
        auto &s = state();
        s.dnd_watching = false;
        if (!s.container)
            return;
        if (!hypriso->dnd_active()) {
            if (s.dnd_over_preview) {
                s.dnd_over_preview = false;
                s.hovered = false;
                dock_thumbnails::close();
            }
            if (s.dnd_hover_cid != -1) {
                s.dnd_hover_cid = -1;
                damage_all();
            }
            return;
        }

        const auto m = mouse();
        const bool over = bounds_contains(s.container->real_bounds, (int) m.x, (int) m.y);
        if (over && !s.dnd_over_preview) {
            s.dnd_over_preview = true;
            s.hovered = true;
            s.pending_close = false;
            s.generation++;
        } else if (!over && s.dnd_over_preview) {
            s.dnd_over_preview = false;
            s.hovered = false;
            dock_thumbnails::close();
        }

        int cid = -1;
        if (over) {
            for (auto *thumb : s.container->children) {
                if (bounds_contains(thumb->real_bounds, (int) m.x, (int) m.y)) {
                    cid = *datum<int>(thumb, "cid");
                    break;
                }
            }
        }
        const long now = get_current_time_in_ms();
        if (cid != s.dnd_hover_cid) {
            s.dnd_hover_cid = cid;
            s.dnd_hover_start = now;
            damage_all();
        } else if (cid != -1 && now - s.dnd_hover_start >= kDndRaiseMs) {
            hypriso->set_hidden(cid, false);
            hypriso->bring_to_front(cid);
            hypriso->hold_focus_during_dnd(cid);
            destroy_now();
            return;
        }
        watch_dnd_hover();
    });
}

void paint_preview(Container *actual_root, Container *c) {
    auto *root = get_rendering_root();
    if (!root)
        return;
    auto [rid, s, stage, active_id] = roots_info(actual_root, root);
    if (stage != (int) STAGE::RENDER_PRE_CURSOR)
        return;
    if (rid != *datum<int>(c, "mid"))
        return;

    renderfix
    render_drop_shadow(rid, 1.0, {0, 0, 0, .40f}, std::round(12 * s), 2.0f, c->real_bounds, std::round(20 * s));
    rect(c->real_bounds, {.11, .11, .12, .92}, 0, std::round(12 * s), 2.0f, true);
    border(c->real_bounds, {1, 1, 1, .1}, std::max(1.0f, std::round(s)), 0, std::round(12 * s));
}

void paint_media_backdrop(Container *thumb, float s);

void paint_thumbnail(Container *actual_root, Container *c) {
    auto *root = get_rendering_root();
    if (!root)
        return;
    auto [rid, s, stage, active_id] = roots_info(actual_root, root);
    if (stage != (int) STAGE::RENDER_PRE_CURSOR)
        return;
    if (rid != *datum<int>(c, "mid"))
        return;

    renderfix
    const int cid = *datum<int>(c, "cid");
    // Keep the screenshot's aspect ratio: a window wider than the preview gets
    // letterboxed and vertically centered instead of being stretched wider.
    Bounds thumb = c->real_bounds;
    const auto size = hypriso->thumbnail_size(cid);
    if (size.w > 1 && size.h > 1) {
        const double aspect = size.w / size.h;
        const double w = std::min(thumb.w, thumb.h * aspect);
        const double h = w / aspect;
        thumb = Bounds(thumb.x + (thumb.w - w) * .5, thumb.y + (thumb.h - h) * .5, w, h);
    }
    hypriso->draw_thumbnail(cid, thumb, std::round(9 * s), 2.0f, 0);

    // Window title along the bottom, styled like the drag workspace switcher's
    // "Desktop #" label: a translucent black bar with white text on it.
    const std::string title = hypriso->title_name(cid);
    if (!title.empty()) {
        auto *text = get_cached_texture(root, c, "dock_thumb_title", set->font, title, RGBA(1, 1, 1, 1), 12);
        if (text && text->id != -1) {
            const double padding = std::round(6 * s);
            auto b = c->real_bounds;
            const double bar_h = std::min<double>(b.h, std::max<double>(text->h + padding, std::round(b.h * .2)));
            b.y += b.h - bar_h;
            b.h = bar_h;
            b.round();
            rect(b, {0, 0, 0, .5f}, 3, std::round(9 * s));

            clip(c->real_bounds, 1.0);
            draw_texture(*text, {c->real_bounds.x + padding, b.y + (bar_h - text->h) * .5, (double) text->w, (double) text->h},
                         1.0, c->real_bounds.w - padding * 2);
        }
    }

    paint_media_backdrop(c, s);

    if (c->state.mouse_hovering || (state().dnd_hover_cid != -1 && state().dnd_hover_cid == cid)) {
        auto b = c->real_bounds;
        b.grow(std::round(2 * s));
        b.round();
        border(b, {1, 1, 1, .65}, std::max(1.0f, std::round(2 * s)), 0, std::round(9 * s));
    }
}

void paint_close_button(Container *actual_root, Container *c) {
    auto *root = get_rendering_root();
    if (!root)
        return;
    auto [rid, s, stage, active_id] = roots_info(actual_root, root);
    if (stage != (int) STAGE::RENDER_PRE_CURSOR)
        return;
    if (rid != *datum<int>(c, "mid"))
        return;

    renderfix
    auto b = c->real_bounds;
    b.round();
    const double rounding = std::round(b.w * .5);
    const bool hot = c->state.mouse_hovering || c->state.mouse_pressing;
    const RGBA bg = hot ? RGBA(.78f, .22f, .22f, .95f) : RGBA(0, 0, 0, .55f);
    rect(b, bg, 0, rounding, 2.0f);
    border(b, {1, 1, 1, .22}, std::max(1.0f, std::round(s)), 0, rounding, 2.0f);

    // Cache on the monitor root so rebuilding the preview does not leak a
    // texture per close button (and each monitor keeps its own scale).
    auto *texture = get_cached_texture(root, root, "dock_thumb_close", icon_font,
        "\uE8BB", RGBA(1, 1, 1, 1), 10);
    if (texture && texture->id != -1)
        draw_texture(*texture, center_x(c, texture->w), center_y(c, texture->h), .95f);
}


// ---------------------------------------------------------------------------
// Linking media players and audio streams to windows
// ---------------------------------------------------------------------------

int parent_pid(int pid) {
    char path[64];
    std::snprintf(path, sizeof(path), "/proc/%d/stat", pid);
    FILE *f = std::fopen(path, "r");
    if (!f)
        return -1;
    char buf[512];
    const size_t n = std::fread(buf, 1, sizeof(buf) - 1, f);
    std::fclose(f);
    buf[n] = '\0';
    // The command name is in parentheses and may contain spaces or ')'.
    const char *close = std::strrchr(buf, ')');
    if (!close)
        return -1;
    char state = 0;
    int ppid = -1;
    if (std::sscanf(close + 1, " %c %d", &state, &ppid) != 2)
        return -1;
    return ppid;
}

// Process ancestry, memoized for one compute_media pass.
struct Ancestry {
    std::unordered_map<int, std::vector<int>> chains;

    const std::vector<int> &of(int pid) {
        auto it = chains.find(pid);
        if (it != chains.end())
            return it->second;
        std::vector<int> chain;
        int p = pid;
        for (int depth = 0; depth < 64; depth++) {
            p = parent_pid(p);
            if (p <= 1)
                break;
            chain.push_back(p);
        }
        return chains[pid] = std::move(chain);
    }

    // 3: same process, 2: one process started the other, 0: unrelated.
    int relation(int a, int b) {
        if (a <= 0 || b <= 0)
            return 0;
        if (a == b)
            return 3;
        const auto &up_a = of(a);
        if (std::find(up_a.begin(), up_a.end(), b) != up_a.end())
            return 2;
        const auto &up_b = of(b);
        if (std::find(up_b.begin(), up_b.end(), a) != up_b.end())
            return 2;
        return 0;
    }
};

std::string lowercase(std::string text) {
    for (auto &ch : text)
        ch = (char) std::tolower((unsigned char) ch);
    return text;
}

bool desktop_entry_matches(const std::string &entry, const std::string &window_class) {
    if (entry.empty() || window_class.empty())
        return false;
    const std::string e = lowercase(entry);
    const std::string c = lowercase(window_class);
    return e == c || c.ends_with("." + e) || e.ends_with("." + c);
}

struct WindowInfo {
    int cid;
    int pid;
    std::string klass;
    std::string title; // lowercase
};

// Keeps the windows that relate most closely to `pid`. A window owned by the
// exact process wins over one whose process merely started (or was started by)
// it, so e.g. a terminal does not take over an mpv instance with its own window.
std::vector<const WindowInfo *> closest_windows(const std::vector<WindowInfo> &windows, int pid, Ancestry &ancestry) {
    int best = 0;
    std::vector<const WindowInfo *> result;
    for (const auto &w : windows) {
        const int r = ancestry.relation(pid, w.pid);
        if (r == 0 || r < best)
            continue;
        if (r > best) {
            best = r;
            result.clear();
        }
        result.push_back(&w);
    }
    return result;
}

std::map<int, Media> compute_media(const std::vector<int> &preview_ids) {
    std::map<int, Media> result;

    std::vector<int> all = get_window_stacking_order();
    for (int cid : preview_ids)
        if (std::find(all.begin(), all.end(), cid) == all.end())
            all.push_back(cid);

    std::vector<WindowInfo> windows;
    windows.reserve(all.size());
    for (int cid : all)
        windows.push_back({cid, hypriso->get_pid(cid), hypriso->class_name(cid), lowercase(hypriso->title_name(cid))});

    Ancestry ancestry;

    // MPRIS: the bus name's owner process, then the player's desktop entry.
    for (const auto &p : mpris::players()) {
        if (!p.active())
            continue;
        auto matched = closest_windows(windows, p.pid, ancestry);
        if (matched.empty()) {
            for (const auto &w : windows)
                if (desktop_entry_matches(p.desktop_entry, w.klass))
                    matched.push_back(&w);
        }
        if (matched.empty())
            continue;

        // Several windows of one process (browser windows): prefer the ones
        // whose title shows what is playing (browsers title windows by tab).
        if (matched.size() > 1 && !p.title.empty()) {
            const std::string title = lowercase(p.title);
            std::vector<const WindowInfo *> narrowed;
            for (auto *w : matched)
                if (w->title.find(title) != std::string::npos)
                    narrowed.push_back(w);
            if (!narrowed.empty())
                matched = std::move(narrowed);
        }

        for (auto *w : matched) {
            if (std::find(preview_ids.begin(), preview_ids.end(), w->cid) == preview_ids.end())
                continue;
            auto &m = result[w->cid];
            // One window, several players: keep the one that is playing.
            if (!m.player.empty() && (m.playing || !p.playing()))
                continue;
            m.player = p.name;
            m.playing = p.playing();
            m.can_play_pause = p.playing() ? p.can_pause : p.can_play;
            m.can_next = p.can_go_next;
            m.can_previous = p.can_go_previous;
        }
    }

    // Audio backend: playback streams carry the pid of the process that opened
    // them (for browsers often a helper process the window's process started).
    for (const auto &node : audio_mixer_nodes()) {
        if (node.kind != AudioNodeKind::SinkInput || node.pid <= 0)
            continue;
        if (node.corked && !node.mute)
            continue;
        for (auto *w : closest_windows(windows, node.pid, ancestry)) {
            if (std::find(preview_ids.begin(), preview_ids.end(), w->cid) == preview_ids.end())
                continue;
            auto &m = result[w->cid];
            m.streams.push_back(node.index);
            m.muted = m.streams.size() == 1 ? node.mute : (m.muted && node.mute);
        }
    }

    // The mute button is only a fallback for windows without a player.
    for (auto &[cid, m] : result)
        if (!m.player.empty())
            m.streams.clear();
    std::erase_if(result, [](const auto &entry) { return !entry.second.any(); });
    return result;
}

// Recomputes which thumbnails get media controls and relayouts when that changed.
void update_media() {
    auto &s = state();
    if (!s.container)
        return;
    auto media = compute_media(s.ids);
    if (media == s.media)
        return;
    s.media = std::move(media);
    s.container->pre_layout(actual_root, s.container, actual_root->real_bounds);
    damage_all();
}

void watch_media(Container *preview, int token) {
    later(kMediaPollMs, [preview, token](Timer *) {
        auto &s = state();
        if (!s.container || s.container != preview || s.refresh_token != token)
            return;
        update_media();
        watch_media(preview, token);
    });
}

void start_media_tracking() {
    static bool hooked = false;
    if (!hooked) {
        hooked = true;
        mpris::set_on_change([] { update_media(); });
    }
    mpris::refresh();
}

const Media *media_for(int cid) {
    auto &media = state().media;
    auto it = media.find(cid);
    return it == media.end() ? nullptr : &it->second;
}

bool media_button_enabled(const Media &m, int role) {
    switch (role) {
        case kRolePrevious: return m.can_previous;
        case kRoleNext: return m.can_next;
        case kRolePlayPause: return m.can_play_pause;
        default: return true;
    }
}

void paint_media_button(Container *actual_root, Container *c) {
    auto *root = get_rendering_root();
    if (!root)
        return;
    auto [rid, s, stage, active_id] = roots_info(actual_root, root);
    if (stage != (int) STAGE::RENDER_PRE_CURSOR)
        return;
    if (rid != *datum<int>(c, "mid"))
        return;
    if (c->real_bounds.w <= 0 || c->real_bounds.h <= 0)
        return;
    const Media *m = media_for(*datum<int>(c, "cid"));
    if (!m)
        return;

    renderfix
    const int role = *datum<int>(c, "role");
    const bool enabled = media_button_enabled(*m, role);
    auto b = c->real_bounds;
    b.round();
    const double rounding = std::round(b.w * .5);
    if (enabled && (c->state.mouse_hovering || c->state.mouse_pressing)) {
        const float a = c->state.mouse_pressing ? .32f : .2f;
        rect(b, {1, 1, 1, a}, 0, rounding, 2.0f);
    }

    const float alpha = enabled ? .95f : .35f;
    const char *key = "dock_thumb_media_play";
    const char *glyph = "\uF5B0";
    // One fixed size: the textures are shared, and differently sized buttons
    // would otherwise keep regenerating them.
    const int glyph_h = 11;
    switch (role) {
        case kRolePrevious: key = "dock_thumb_media_prev"; glyph = "\uF8AC"; break;
        case kRoleNext: key = "dock_thumb_media_next"; glyph = "\uF8AD"; break;
        case kRolePlayPause:
            if (m->playing) {
                key = "dock_thumb_media_pause";
                glyph = "\uF8AE";
            }
            break;
        case kRoleMute:
            key = m->muted ? "dock_thumb_muted" : "dock_thumb_unmuted";
            glyph = m->muted ? "\uE74F" : "\uE995";
            break;
        default: break;
    }
    // Cached on the monitor root (like the close button) so every thumbnail
    // shares one texture per glyph and each monitor keeps its own scale.
    auto *texture = get_cached_texture(root, root, key, icon_font, glyph, RGBA(1, 1, 1, 1), glyph_h);
    if (texture && texture->id != -1)
        draw_texture(*texture, center_x(c, texture->w), center_y(c, texture->h), alpha);
}

void media_button_clicked(int cid, int role) {
    auto &s = state();
    auto it = s.media.find(cid);
    if (it == s.media.end())
        return;
    auto &m = it->second;
    if (!media_button_enabled(m, role))
        return;
    switch (role) {
        case kRolePrevious: mpris::previous(m.player); break;
        case kRoleNext: mpris::next(m.player); break;
        case kRolePlayPause:
            mpris::play_pause(m.player);
            // Flip right away; the player's reply corrects it if it disagrees.
            m.playing = !m.playing;
            break;
        case kRoleMute:
            m.muted = !m.muted;
            for (uint32_t stream : m.streams)
                audio_mixer_set_mute(AudioNodeKind::SinkInput, stream, m.muted);
            break;
        default: break;
    }
    damage_all();
}

// Lays out the media buttons of `thumb` as a centered row just above the title
// bar. Buttons without a function for this window collapse to nothing so they
// cannot be hovered or clicked.
void layout_media_buttons(Container *thumb) {
    const int cid = *datum<int>(thumb, "cid");
    const Media *m = media_for(cid);
    std::vector<Container *> visible;
    for (auto *child : thumb->children) {
        const int role = *datum<int>(child, "role");
        if (role == kRoleClose)
            continue;
        bool show = false;
        if (m) {
            if (!m->player.empty())
                show = role == kRolePlayPause || (role == kRolePrevious && m->can_previous) ||
                       (role == kRoleNext && m->can_next);
            else
                show = role == kRoleMute;
        }
        if (show)
            visible.push_back(child);
        else
            child->real_bounds = Bounds(0, 0, 0, 0);
    }
    if (visible.empty())
        return;
    std::sort(visible.begin(), visible.end(), [](Container *a, Container *b) {
        return *datum<int>(a, "role") < *datum<int>(b, "role");
    });

    const auto tb = thumb->real_bounds;
    const double size = std::round(std::clamp(.2 * std::min(tb.w, tb.h), (double) kMediaMinSize, (double) kMediaMaxSize));
    auto row_width = [&] { return visible.size() * size + (visible.size() - 1) * kMediaGap; };
    // Tiny thumbnails (crowded grids) keep only the main button.
    if (visible.size() > 1 && row_width() > tb.w - 8) {
        for (auto *button : visible)
            if (*datum<int>(button, "role") != kRolePlayPause)
                button->real_bounds = Bounds(0, 0, 0, 0);
        std::erase_if(visible, [](Container *b) { return *datum<int>(b, "role") != kRolePlayPause; });
    }
    const double row_w = row_width();
    // Matches the title bar painted in paint_thumbnail (about a fifth of the height).
    const double title_h = std::max(20.0, std::round(tb.h * .2));
    double x = std::round(tb.x + (tb.w - row_w) * .5);
    const double y = std::round(std::max(tb.y + 4, tb.bottom() - title_h - 6 - size));
    for (auto *button : visible) {
        button->real_bounds = Bounds(x, y, size, size);
        x += size + kMediaGap;
    }
}

// Translucent pill behind the visible media buttons so they read on any screenshot.
void paint_media_backdrop(Container *thumb, float s) {
    Bounds row;
    for (auto *child : thumb->children) {
        if (*datum<int>(child, "role") == kRoleClose || child->real_bounds.w <= 0)
            continue;
        row = union_bounds(row, child->real_bounds);
    }
    if (row.w <= 0 || row.h <= 0)
        return;
    row.grow(std::round(3 * s));
    row.round();
    rect(row, {0, 0, 0, .6f}, 0, std::round(row.h * .5), 2.0f);
    border(row, {1, 1, 1, .14f}, std::max(1.0f, std::round(s)), 0, std::round(row.h * .5), 2.0f);
}

// Creates the overlay container and its thumbnails. Does not check whether the
// cursor is over the dock, so it can rebuild the preview after closing a window.
void build_preview(const std::vector<int> &ids, const Placement *placement = nullptr,
                   const std::string &source = "") {
    if (ids.empty()) {
        destroy_now();
        return;
    }

    const auto m = mouse();
    const int mid = hypriso->monitor_from_cursor();
    if (mid < 0)
        return;

    const Bounds mb = bounds_reserved_monitor(mid);
    if (mb.w <= 0 || mb.h <= 0)
        return;

    Bounds dock_b;
    int edge = kEdgeBottom;
    if (placement && placement->dock.w > 0 && placement->dock.h > 0) {
        dock_b = placement->dock;
        edge = placement->edge;
    } else if (cursor_over_dock(m.x, m.y, &dock_b)) {
        edge = dock_edge_for(dock_b, mb);
    } else {
        edge = m.y < mb.y + mb.h * .5 ? kEdgeTop : kEdgeBottom;
        const float thickness = 40;
        switch (edge) {
            case kEdgeTop: dock_b = Bounds(mb.x, mb.y, mb.w, thickness); break;
            case kEdgeRight: dock_b = Bounds(mb.right() - thickness, mb.y, thickness, mb.h); break;
            case kEdgeLeft: dock_b = Bounds(mb.x, mb.y, thickness, mb.h); break;
            default: dock_b = Bounds(mb.x, mb.bottom() - thickness, mb.w, thickness); break;
        }
    }

    const bool vertical = edge == kEdgeLeft || edge == kEdgeRight;

    // Capture before measuring the aspect ratio: a window that was never
    // captured has no thumbnail geometry yet, and falling back to 16:9 would
    // stretch the resulting screenshot. The fresh capture also makes sure the
    // width can be squeezed to the screenshot's real aspect ratio.
    const long initial_capture_time = get_current_time_in_ms();
    for (int cid : ids)
        hypriso->screenshot(cid);

    struct Item {
        int cid;
        double aspect;
    };
    std::vector<Item> items;
    items.reserve(ids.size());
    for (int cid : ids) {
        auto t = hypriso->thumbnail_size(cid);
        double aspect = (t.w > 1 && t.h > 1) ? t.w / t.h : 16.0 / 9.0;
        aspect = std::clamp(aspect, .4, 4.0);
        items.push_back({cid, aspect});
    }

    const int n = (int) items.size();
    const bool grid = n > kStripMax;

    int cols = 1;
    int rows = n;
    if (grid) {
        if (vertical) {
            rows = kGridLineMax;
            cols = (n + rows - 1) / rows;
        } else {
            cols = kGridLineMax;
            rows = (n + cols - 1) / cols;
        }
    } else if (!vertical) {
        cols = n;
        rows = 1;
    }

    const double max_panel_w = mb.w * kMaxPageFraction;
    const double max_panel_h = mb.h * kMaxPageFraction;

    double sum_along = 0;
    double max_w_unit = vertical ? 1.0 : 0.0;
    double max_h_unit = vertical ? 0.0 : 1.0;
    for (const auto &item : items) {
        if (vertical) {
            sum_along += 1.0 / item.aspect;
            max_h_unit = std::max(max_h_unit, 1.0 / item.aspect);
        } else {
            sum_along += item.aspect;
            max_w_unit = std::max(max_w_unit, item.aspect);
        }
    }

    std::vector<double> widths(n), heights(n);
    double cw = 0;
    double ch = 0;
    double cell_w = 0;
    double cell_h = 0;

    if (!grid) {
        // A single strip: every thumbnail shares the same short side.
        double s = kThumbSize;
        if (vertical)
            s = std::min<double>(kThumbSize * 1.5, mb.w * .35);

        const double gaps = kGap * std::max(0, n - 1);
        if (vertical) {
            const double avail_h = std::max(1.0, max_panel_h - 2 * kPad - gaps);
            const double avail_w = std::max(1.0, max_panel_w - 2 * kPad);
            if (sum_along > 0)
                s = std::min(s, avail_h / sum_along);
            s = std::min(s, avail_w);
        } else {
            const double avail_w = std::max(1.0, max_panel_w - 2 * kPad - gaps);
            const double avail_h = std::max(1.0, max_panel_h - 2 * kPad);
            if (sum_along > 0)
                s = std::min(s, avail_w / sum_along);
            s = std::min(s, avail_h);
        }
        s = std::max(1.0, s);

        double sum_w = 0;
        double sum_h = 0;
        for (int i = 0; i < n; i++) {
            if (vertical) {
                widths[i] = std::round(s);
                heights[i] = std::max(1.0, std::round(s / items[i].aspect));
            } else {
                widths[i] = std::min<double>(kThumbMaxWidth, std::max(1.0, std::round(s * items[i].aspect)));
                heights[i] = std::round(s);
            }
            sum_w += widths[i];
            sum_h += heights[i];
        }

        if (vertical) {
            cw = std::round(s) + 2 * kPad;
            ch = sum_h + gaps + 2 * kPad;
        } else {
            cw = sum_w + gaps + 2 * kPad;
            ch = std::round(s) + 2 * kPad;
        }
    } else {
        // A grid with uniform cells; each thumbnail keeps its own aspect ratio.
        double s = kThumbSize;
        const double avail_w = std::max(1.0, max_panel_w - 2 * kPad - (cols - 1) * kGap);
        const double avail_h = std::max(1.0, max_panel_h - 2 * kPad - (rows - 1) * kGap);
        if (max_w_unit > 0)
            s = std::min(s, (avail_w / cols) / max_w_unit);
        if (max_h_unit > 0)
            s = std::min(s, (avail_h / rows) / max_h_unit);
        s = std::max(1.0, s);

        cell_w = std::max(1.0, std::round(max_w_unit * s));
        if (!vertical)
            cell_w = std::min<double>(kThumbMaxWidth, cell_w);
        cell_h = std::max(1.0, std::round(max_h_unit * s));
        cw = cols * cell_w + (cols - 1) * kGap + 2 * kPad;
        ch = rows * cell_h + (rows - 1) * kGap + 2 * kPad;

        for (int i = 0; i < n; i++) {
            const double aspect = items[i].aspect;
            double w;
            double h;
            if (cell_w / cell_h > aspect) {
                h = cell_h;
                w = h * aspect;
            } else {
                w = cell_w;
                h = w / aspect;
            }
            widths[i] = std::max(1.0, std::round(w));
            heights[i] = std::max(1.0, std::round(h));
        }
    }

    double anchor_x = m.x;
    double anchor_y = m.y;
    if (placement && placement->has_icon) {
        // Center the preview on the dock icon rather than the cursor.
        anchor_x = dock_b.x + placement->icon.x + placement->icon.w * .5;
        anchor_y = dock_b.y + placement->icon.y + placement->icon.h * .5;
    } else if (placement && placement->has_center) {
        anchor_x = placement->center_x;
        anchor_y = placement->center_y;
    }

    double x = 0;
    double y = 0;
    if (vertical) {
        y = std::clamp<double>(anchor_y - ch * .5, mb.y + kDockMargin, std::max<double>(mb.y + kDockMargin, mb.bottom() - ch - kDockMargin));
        if (edge == kEdgeRight)
            x = dock_b.x - kDockMargin - cw;
        else
            x = dock_b.right() + kDockMargin;
    } else {
        x = std::clamp<double>(anchor_x - cw * .5, mb.x + kDockMargin, std::max<double>(mb.x + kDockMargin, mb.right() - cw - kDockMargin));
        if (edge == kEdgeBottom)
            y = dock_b.y - kDockMargin - ch;
        else
            y = dock_b.bottom() + kDockMargin;
    }
    x = std::clamp<double>(x, mb.x + kDockMargin, std::max<double>(mb.x + kDockMargin, mb.right() - cw - kDockMargin));
    y = std::clamp<double>(y, mb.y + kDockMargin, std::max<double>(mb.y + kDockMargin, mb.bottom() - ch - kDockMargin));

    auto *preview = actual_root->child((int) std::round(cw), (int) std::round(ch));
    preview->custom_type = (int) TYPE::DOCK_THUMBNAILS;
    preview->z_index = 2000;
    preview->wanted_bounds = Bounds(x, y, cw, ch);
    preview->real_bounds = Bounds(x, y, cw, ch);
    preview->automatically_paint_children = true;
    preview->receive_events_even_if_obstructed = true;
    *datum<int>(preview, "mid") = mid;
    *datum<bool>(preview, "vertical") = vertical;
    *datum<bool>(preview, "grid") = grid;
    *datum<int>(preview, "cols") = cols;
    *datum<int>(preview, "rows") = rows;
    *datum<double>(preview, "cell_w") = cell_w;
    *datum<double>(preview, "cell_h") = cell_h;

    consume_everything(preview);
    preview->when_mouse_enters_container = paint {
        auto &s = state();
        s.hovered = true;
        s.pending_close = false;
        s.generation++;
        consume_event(root, c);
    };
    preview->when_mouse_leaves_container = paint {
        state().hovered = false;
        consume_event(root, c);
        dock_thumbnails::close();
    };
    preview->when_paint = paint_preview;
    preview->pre_layout = [](Container *root, Container *c, const Bounds &) {
        const bool vertical = *datum<bool>(c, "vertical");
        const bool grid = *datum<bool>(c, "grid");
        const int cols = std::max(1, *datum<int>(c, "cols"));
        const double cell_w = *datum<double>(c, "cell_w");
        const double cell_h = *datum<double>(c, "cell_h");

        double x = c->real_bounds.x + kPad;
        double y = c->real_bounds.y + kPad;
        int index = 0;
        for (auto *thumb : c->children) {
            if (grid) {
                const int col = index % cols;
                const int row = index / cols;
                const double cx = x + col * (cell_w + kGap);
                const double cy = y + row * (cell_h + kGap);
                const double tw = thumb->wanted_bounds.w;
                const double th = thumb->wanted_bounds.h;
                thumb->real_bounds = Bounds(
                    std::round(cx + (cell_w - tw) * .5),
                    std::round(cy + (cell_h - th) * .5),
                    tw, th);
            } else {
                thumb->real_bounds = Bounds(x, y, thumb->wanted_bounds.w, thumb->wanted_bounds.h);
                if (vertical)
                    y += thumb->wanted_bounds.h + kGap;
                else
                    x += thumb->wanted_bounds.w + kGap;
            }

            // Position the close button in this thumbnail's top-right corner.
            for (auto *close : thumb->children) {
                if (*datum<int>(close, "role") != kRoleClose)
                    continue;
                const double size = close->wanted_bounds.w;
                const double room = std::min(thumb->real_bounds.w, thumb->real_bounds.h) - size;
                const double inset = std::clamp<double>(room * .5, 0.0, kCloseInset);
                close->real_bounds = Bounds(
                    thumb->real_bounds.x + thumb->real_bounds.w - inset - size,
                    thumb->real_bounds.y + inset,
                    size, size);
            }
            layout_media_buttons(thumb);
            index++;
        }
    };
    preview->on_closed = [](Container *c) {
        auto &s = state();
        if (s.container == c) {
            s.container = nullptr;
            s.ids.clear();
            s.source.clear();
            s.hovered = false;
            s.pending_close = false;
            s.has_anchor = false;
            s.has_icon = false;
            s.refresh_token++;
            s.window_captures.clear();
            s.smoothed_capture_ms = 0.0;
            s.capture_cursor = 0;
        }
    };

    for (int i = 0; i < n; i++) {
        auto *thumb = preview->child((int) widths[i], (int) heights[i]);
        thumb->z_index = i;
        thumb->wanted_bounds = Bounds(0, 0, widths[i], heights[i]);
        thumb->real_bounds = Bounds(0, 0, widths[i], heights[i]);
        // Keep the thumbnail hovered while the pointer is over its close
        // button, which sits above it in the pierced list.
        thumb->receive_events_even_if_obstructed_by_one = true;
        *datum<int>(thumb, "cid") = items[i].cid;
        *datum<int>(thumb, "mid") = mid;
        thumb->when_paint = paint_thumbnail;
        thumb->when_mouse_enters_container = paint {
            consume_event(root, c);
            damage_all();
        };
        thumb->when_mouse_leaves_container = paint {
            consume_event(root, c);
            damage_all();
        };
        thumb->when_clicked = paint {
            if (c->state.mouse_button_pressed == BTN_RIGHT) {
                consume_event(root, c);
                const int cid = *datum<int>(c, "cid");
                later_immediate([cid](Timer *) {
                    auto &s = state();
                    if (!s.container)
                        return;
                    titlebar::titlebar_right_click(cid);
                    // The preview paints in RENDER_PRE_CURSOR; lift the menu
                    // above it so it is not hidden behind the thumbnails.
                    popup::set_above_overlays();
                    s.pending_close = false;
                    s.generation++;
                });
                return;
            }
            if (c->state.mouse_button_pressed != BTN_LEFT)
                return;

            // The close button handles its own click; do not restore/raise the
            // window when the click landed on it.
            for (auto *child : c->children) {
                if (bounds_contains(child->real_bounds, root->mouse_current_x, root->mouse_current_y))
                    return;
            }

            consume_event(root, c);
            const int cid = *datum<int>(c, "cid");
            later_immediate([cid](Timer *) {
                hypriso->set_hidden(cid, false);
                hypriso->bring_to_front(cid);
                dock_thumbnails::open({});
            });
        };

        thumb->on_closed = [](Container *c) {
            // The cached title texture belongs to this thumbnail; release it
            // when the preview is rebuilt so it does not leak.
            auto *title = datum<TextureInfo>(c, "dock_thumb_title");
            if (title->id != -1) {
                free_text_texture(title->id);
                title->id = -1;
            }
        };

        const double close_size = std::round(std::clamp(0.35 * std::min(widths[i], heights[i]), (double) kCloseMinSize, (double) kCloseMaxSize));
        auto *close = thumb->child((int) close_size, (int) close_size);
        close->z_index = 100;
        close->wanted_bounds = Bounds(0, 0, close_size, close_size);
        close->real_bounds = Bounds(0, 0, close_size, close_size);
        *datum<int>(close, "cid") = items[i].cid;
        *datum<int>(close, "mid") = mid;
        *datum<int>(close, "role") = kRoleClose;
        close->when_paint = paint_close_button;
        close->when_mouse_down = consume_event;
        close->when_mouse_up = consume_event;
        close->when_mouse_enters_container = paint {
            consume_event(root, c);
            damage_all();
        };
        close->when_mouse_leaves_container = paint {
            consume_event(root, c);
            damage_all();
        };
        close->when_clicked = paint {
            if (c->state.mouse_button_pressed != BTN_LEFT)
                return;
            if (!bounds_contains(c->real_bounds, root->mouse_current_x, root->mouse_current_y))
                return;

            consume_event(root, c);
            const int cid = *datum<int>(c, "cid");
            later_immediate([cid](Timer *) {
                close_window(cid);
                dock_thumbnails::remove_window(cid);
            });
        };

        // Media controls; laid out (or collapsed) by layout_media_buttons.
        for (int role : {kRolePrevious, kRolePlayPause, kRoleNext, kRoleMute}) {
            auto *button = thumb->child(0, 0);
            button->z_index = 90;
            button->wanted_bounds = Bounds(0, 0, 0, 0);
            button->real_bounds = Bounds(0, 0, 0, 0);
            *datum<int>(button, "cid") = items[i].cid;
            *datum<int>(button, "mid") = mid;
            *datum<int>(button, "role") = role;
            button->when_paint = paint_media_button;
            button->when_mouse_down = consume_event;
            button->when_mouse_up = consume_event;
            button->when_mouse_enters_container = paint {
                consume_event(root, c);
                damage_all();
            };
            button->when_mouse_leaves_container = paint {
                consume_event(root, c);
                damage_all();
            };
            button->when_clicked = paint {
                if (c->state.mouse_button_pressed != BTN_LEFT)
                    return;
                if (!bounds_contains(c->real_bounds, root->mouse_current_x, root->mouse_current_y))
                    return;
                consume_event(root, c);
                const int cid = *datum<int>(c, "cid");
                const int role = *datum<int>(c, "role");
                later_immediate([cid, role](Timer *) { media_button_clicked(cid, role); });
            };
        }
    }

    auto &s = state();
    s.container = preview;
    s.ids = ids;
    s.source = source;
    s.hovered = false;
    s.pending_close = false;
    s.generation++;
    s.anchor_dock = dock_b;
    s.anchor_edge = edge;
    s.has_anchor = true;
    s.anchor_icon = placement && placement->has_icon ? placement->icon : Bounds{};
    s.has_icon = placement && placement->has_icon;
    const int token = ++s.refresh_token;

    s.window_captures.clear();
    for (int cid : ids)
        s.window_captures[cid] = initial_capture_time;
    s.smoothed_capture_ms = 0.0;
    s.capture_cursor = 0;

    start_media_tracking();
    s.media = compute_media(ids);
    watch_media(preview, token);

    preview->pre_layout(actual_root, preview, actual_root->real_bounds);
    damage_all();

    later(kFastCaptureMs, [preview, token](Timer *t) {
        auto &s = state();
        t->keep_running = s.container && s.container == preview && s.refresh_token == token;
        if (!t->keep_running)
            return;
        t->delay = static_cast<float>(capture_thumbnails(preview));
    });

    if (hypriso->dnd_active())
        watch_dnd_hover();
}

} // namespace

void dock_thumbnails::open(const std::vector<int> &ids, const Bounds &icon, const std::string &source) {
    auto &s = state();

    if (ids.empty()) {
        destroy_now();
        return;
    }

    // A right-click menu is showing; do not cover it with a hover preview.
    if (popup::is_open())
        return;

    // The hover timer fires on the dock thread; by the time it reaches us the
    // cursor may have left the dock already. Use the stored dock placement when
    // refreshing an already visible preview from a non-dock position.
    const auto m = mouse();
    Bounds dock_b;
    int edge = kEdgeBottom;
    bool have_dock = false;
    if (cursor_over_dock(m.x, m.y, &dock_b)) {
        const int mid = hypriso->monitor_from_cursor();
        if (mid < 0)
            return;
        const Bounds mb = bounds_reserved_monitor(mid);
        if (mb.w <= 0 || mb.h <= 0)
            return;
        edge = dock_edge_for(dock_b, mb);
        have_dock = true;
    } else if (s.container && s.has_anchor) {
        dock_b = s.anchor_dock;
        edge = s.anchor_edge;
        have_dock = true;
    }
    if (!have_dock)
        return;

    const bool has_icon = icon.w > 0 && icon.h > 0;
    if (s.container && same_ids(s.ids, ids) && s.source == source &&
        (!has_icon || (s.has_icon && same_bounds(s.anchor_icon, icon)))) {
        s.pending_close = false;
        s.generation++;
        damage_all();
        return;
    }

    const bool cursor_was_over_preview = s.container && bounds_contains(s.container->real_bounds, (int) m.x, (int) m.y);
    const Bounds old_preview_bounds = s.container ? s.container->real_bounds : Bounds{};
    destroy_now();

    Placement placement;
    placement.dock = dock_b;
    placement.edge = edge;
    placement.has_icon = has_icon;
    placement.icon = icon;
    build_preview(ids, &placement, source);

    // When a visible preview was rebuilt underneath the pointer, keep it
    // hovered. If it moved away from the pointer but the cursor is still in the
    // old panel area, keep it open for a moment so the user can keep closing
    // thumbnails with the X buttons.
    auto &next = state();
    if (!next.container)
        return;
    if (bounds_contains(next.container->real_bounds, (int) m.x, (int) m.y)) {
        next.hovered = true;
        next.pending_close = false;
        next.generation++;
    } else if (cursor_was_over_preview) {
        next.hovered = false;
        next.pending_close = false;
        watch_rebuilt_preview(next.source, next.generation, union_bounds(old_preview_bounds, next.container->real_bounds));
    }
}

void dock_thumbnails::refresh_if_open(const std::string &source, const std::vector<int> &ids, const Bounds &icon,
                                      bool allow_switch) {
    auto &s = state();
    if (!s.container)
        return;
    // Do not replace the preview while a context menu is open.
    if (popup::is_open())
        return;
    // Background reflows (new windows, dock relayout) may only update the
    // preview owned by the icon they came from. Switching to another icon is
    // reserved for actual hover, otherwise the preview jumps around whenever a
    // new window appears while it is visible.
    if (s.source != source && !allow_switch)
        return;

    if (ids.empty()) {
        destroy_now();
        return;
    }

    const bool has_icon = icon.w > 0 && icon.h > 0;
    if (s.source == source && same_ids(s.ids, ids) &&
        (!has_icon || (s.has_icon && same_bounds(s.anchor_icon, icon))))
        return;

    open(ids, icon, source);
}

void dock_thumbnails::remove_window(int cid) {
    auto &s = state();
    if (!s.container)
        return;

    std::vector<int> ids;
    ids.reserve(s.ids.size());
    for (int id : s.ids)
        if (id != cid)
            ids.push_back(id);

    if (ids.empty()) {
        destroy_now();
        return;
    }

    const bool had_anchor = s.has_anchor;
    const bool had_icon = s.has_icon;
    const Bounds old_icon = s.anchor_icon;
    const Bounds old_preview_bounds = s.container->real_bounds;
    const std::string source = s.source;
    Placement placement;
    if (had_anchor) {
        placement.dock = s.anchor_dock;
        placement.edge = s.anchor_edge;
        placement.has_center = true;
        placement.center_x = s.container->real_bounds.x + s.container->real_bounds.w * .5;
        placement.center_y = s.container->real_bounds.y + s.container->real_bounds.h * .5;
    }

    destroy_now();
    build_preview(ids, had_anchor ? &placement : nullptr, source);

    // Keep the icon association so later reflows still update this preview,
    // even though the rebuild was positioned around the old center.
    auto &next = state();
    if (!next.container)
        return;
    if (had_icon) {
        next.anchor_icon = old_icon;
        next.has_icon = true;
    }

    // Avoid a stale close timer destroying the rebuilt preview before the next
    // mouse-move enter/leave event. If the panel shrank away from the pointer,
    // keep it open while the cursor is still inside the old panel area.
    const auto m = mouse();
    if (bounds_contains(next.container->real_bounds, (int) m.x, (int) m.y)) {
        next.hovered = true;
        next.pending_close = false;
        next.generation++;
        return;
    }
    next.hovered = false;
    next.pending_close = false;
    watch_rebuilt_preview(next.source, next.generation, union_bounds(old_preview_bounds, next.container->real_bounds));
}

void dock_thumbnails::close() {
    auto &s = state();
    if (!s.container || s.pending_close)
        return;
    s.pending_close = true;
    const int generation = ++s.generation;
    later(kCloseGraceMs, [generation](Timer *) {
        auto &s = state();
        // Re-entering the preview or the dock icon bumps the generation
        // (keep_open/open), so a stale timer becomes a no-op.
        if (generation != s.generation || s.hovered || !s.container)
            return;
        // Keep the preview while the pointer is inside a context menu opened
        // from it (or back over the preview once that menu closes).
        const auto m = mouse();
        if (popup::contains((int) m.x, (int) m.y) ||
            (popup::is_open() && bounds_contains(s.container->real_bounds, (int) m.x, (int) m.y))) {
            s.pending_close = false;
            dock_thumbnails::close();
            return;
        }
        if (bounds_contains(s.container->real_bounds, (int) m.x, (int) m.y)) {
            s.hovered = true;
            s.pending_close = false;
            return;
        }
        destroy_now();
    });
}

void dock_thumbnails::keep_open() {
    auto &s = state();
    s.pending_close = false;
    s.generation++;
}

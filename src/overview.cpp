#include "overview.h"
#include "container.h"
#include "dock.h"
#include "drag_workspace_switcher.h"
#include "first.h"
#include "heart.h"
#include "hypriso.h"
#include "layout_thumbnails.h"
#include "titlebar.h"
#include "desktop_gesture.h"
#include "desktop_icons.h"
#include "show_desktop.h"
#include "spring.h"

#include <linux/input-event-codes.h>
#include <map>
#include <memory>

bool running = false;
float openess = 0.0f;
float overview_open_time_ms = 700;
static bool initialized = false;
static unsigned int lifecycle = 0;
static unsigned int animation_generation = 0;
static bool animating = false;
static float animation_target = 0.0f;
static int overview_monitor = -1;
static bool painting_workspace_snapshot = false;
static int snapshot_workspace = -1;
static constexpr int teaser_workspace = -1;
static constexpr double thumbnail_fade_ms = 200.0;
// Timers use whole milliseconds: rounding up keeps captures below 60 FPS.
static constexpr long fast_capture_ms = 17;
static constexpr long slow_capture_ms = 400;

// All geometry below is in global logical coordinates. Only painting converts
// to monitor-local pixels; hit testing and dragging use the same logical bounds.
struct OverviewSpring {
    double value = 0;
    double target = 0;
    double velocity = 0;

    void reset(double v) {
        value = target = v;
        velocity = 0;
    }

    bool advance(double dt) {
        if (value == target && velocity == 0)
            return false;
        auto next = springEvaluate(dt, value, target, velocity, {.38, .82});
        value = next.value;
        velocity = next.velocity;
        if (std::abs(value - target) < .0001 && std::abs(velocity) < .001) {
            reset(target);
            return false;
        }
        return true;
    }
};

struct OverviewBoundsSpring {
    OverviewSpring x, y, w, h;

    Bounds bounds() const { return {x.value, y.value, w.value, h.value}; }
    void reset(const Bounds &b) {
        x.reset(b.x); y.reset(b.y); w.reset(b.w); h.reset(b.h);
    }
    void retarget(const Bounds &b) {
        x.target = b.x; y.target = b.y; w.target = b.w; h.target = b.h;
    }
    bool advance(double dt) {
        bool moving = x.advance(dt);
        moving = y.advance(dt) || moving;
        moving = w.advance(dt) || moving;
        return h.advance(dt) || moving;
    }
};

struct OverviewThumbnail {
    Bounds natural;
    OverviewBoundsSpring slot;
    OverviewSpring drag_x, drag_y;
    bool placed = false;
    long fade_start = 0;
    float opacity = 1.0f;
    unsigned long long elevated_order = 0;
};

struct OverviewWorkspace {
    std::map<int, OverviewThumbnail> thumbnails;
    std::vector<int> stacking;
    OverviewSpring position;
    Bounds area;
    std::string layout_type;
    bool initialized = false;
    bool retiring = false;
};

struct WindowOption {
    int cid;
    int workspace;
    Bounds b;
};

struct WorkspaceOption {
    int workspace;
    Bounds b;
};

struct OverviewMonitor {
    std::map<int, OverviewWorkspace> workspaces;
    std::vector<int> order;
    std::vector<WindowOption> window_options;
    std::vector<WorkspaceOption> workspace_options;
    int active = -1;
    int pending_workspace = 0;
    int dragged = -1;
    int dragged_workspace = -1;
    double drag_start_x = 0;
    double drag_start_y = 0;
};

struct OverviewScene {
    std::map<int, OverviewMonitor> monitors;
    std::map<int, Bounds> desktop_origins;
    float desktop_origin_weight = 1.0f;
    bool taking_desktop = false;
    std::map<int, long> window_captures;
    std::map<int, long> wallpaper_captures;
    long last_update = 0;
    unsigned long long drag_order = 0;
};

static std::unique_ptr<OverviewScene> scene;

static bool same_bounds(const Bounds &a, const Bounds &b) {
    return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h;
}

static Bounds render_bounds(Bounds b, int monitor) {
    const auto mb = bounds_monitor(monitor);
    b.x -= mb.x;
    b.y -= mb.y;
    return b.scale(scale(monitor));
}

static void layout_workspace(OverviewWorkspace &workspace, const Bounds &area) {
    // The layout engine borrows cells only for this calculation. The scene owns
    // the resulting slots and their motion for the entire overview session.
    std::vector<DemoCell> storage;
    storage.reserve(workspace.thumbnails.size());
    for (auto &[cid, thumbnail] : workspace.thumbnails) {
        const auto &b = thumbnail.natural;
        storage.emplace_back(cid, b.x - area.x, b.y - area.y, b.w, b.h);
    }
    std::vector<ExpoCell *> cells;
    for (auto &cell : storage)
        cells.push_back(&cell);
    ExpoLayout layout;
    layout.setCells(cells);
    layout.setAreaSize(std::max(1, (int) area.w), std::max(1, (int) area.h));
    if (set->overview_layout_type == "Grid")
        layout.calculateGrid();
    else
        layout.calculate();

    Bounds content;
    bool first = true;
    for (auto &cell : storage) {
        const auto r = cell.result();
        if (first) {
            content = {double(r.x), double(r.y), double(r.w), double(r.h)};
            first = false;
        } else {
            const auto right = std::max(content.right(), double(r.x + r.w));
            const auto bottom = std::max(content.bottom(), double(r.y + r.h));
            content.x = std::min(content.x, double(r.x));
            content.y = std::min(content.y, double(r.y));
            content.w = right - content.x;
            content.h = bottom - content.y;
        }
    }
    for (auto &cell : storage) {
        const auto r = cell.result();
        Bounds target(area.x + (area.w - content.w) * .5 + r.x - content.x,
                      area.y + (area.h - content.h) * .5 + r.y - content.y, r.w, r.h);
        auto &thumbnail = workspace.thumbnails.at(cell.persistentKey());
        if (!thumbnail.placed) {
            thumbnail.slot.reset(target);
            thumbnail.placed = true;
        }
        thumbnail.slot.retarget(target);
    }
    workspace.area = area;
    workspace.layout_type = set->overview_layout_type;
    workspace.initialized = true;
}

void create_overview_for_monitor(int monitor);

static void update_scene() {
    if (!scene)
        return;
    const auto now = get_current_time_in_ms();
    const double dt = scene->last_update == 0 ? 0 : std::max(0.0, (now - scene->last_update) / 1000.0);
    scene->last_update = now;
    // Keep a copy before reconciling membership so a moved window carries its
    // springs to its new workspace, regardless of which workspace updates first.
    struct PreviousThumbnail {
        OverviewThumbnail thumbnail;
        int monitor;
        int workspace;
        double translation;
    };
    std::map<int, PreviousThumbnail> previous_thumbnails;
    for (const auto &[mid, monitor] : scene->monitors) {
        const double pitch = bounds_monitor(mid).w * (1.0 - .16 * std::clamp(openess, 0.0f, 1.0f));
        for (const auto &[wid, workspace] : monitor.workspaces) {
            for (const auto &[cid, thumbnail] : workspace.thumbnails)
                previous_thumbnails.emplace(cid, PreviousThumbnail{thumbnail, mid, wid, workspace.position.value * pitch});
        }
    }
    std::vector<int> monitor_ids;
    bool moving = false;
    for (auto m : actual_monitors) {
        const int mid = *datum<int>(m, "cid");
        monitor_ids.push_back(mid);
        auto [entry, added] = scene->monitors.try_emplace(mid);
        auto &monitor = entry->second;
        if (added)
            create_overview_for_monitor(mid);
        const int previous_active = monitor.active;
        auto previous_order = monitor.order;
        monitor.active = hypriso->get_active_workspace_id(mid);
        monitor.order = hypriso->get_workspace_ids(mid);
        if (std::find(monitor.order.begin(), monitor.order.end(), monitor.active) == monitor.order.end())
            monitor.order.push_back(monitor.active);
        // Materializing the teaser keeps the same card and spring, so clicking
        // or dropping into it does not replace the visible peek mid-animation.
        if (monitor.pending_workspace != 0) {
            for (auto wid : monitor.order) {
                if (hypriso->space_id_to_raw(wid) != monitor.pending_workspace)
                    continue;
                auto teaser = monitor.workspaces.extract(teaser_workspace);
                if (!teaser.empty()) {
                    teaser.key() = wid;
                    monitor.workspaces.insert(std::move(teaser));
                    std::replace(previous_order.begin(), previous_order.end(), teaser_workspace, wid);
                }
                monitor.pending_workspace = 0;
                break;
            }
        }
        // This virtual desktop always follows the real desktops. It only
        // becomes a compositor workspace when clicked or used as a drop target.
        monitor.order.push_back(teaser_workspace);
        const auto active_index = std::find(monitor.order.begin(), monitor.order.end(), monitor.active) - monitor.order.begin();
        double workspace_shift = 0;
        if (previous_active != -1 && previous_active != monitor.active) {
            const auto previous = std::find(monitor.order.begin(), monitor.order.end(), previous_active);
            if (previous != monitor.order.end()) {
                workspace_shift = active_index - (previous - monitor.order.begin());
            } else {
                const auto old_index = std::find(previous_order.begin(), previous_order.end(), previous_active);
                const auto new_index = std::find(previous_order.begin(), previous_order.end(), monitor.active);
                workspace_shift = new_index - old_index;
            }
        }
        auto area = bounds_monitor(mid);
        area.scale_from_center(.8);
        area = area.intersection(bounds_reserved_monitor(mid));
        area.shrink(std::min(20.0, std::min(area.w, area.h) * .05));

        for (size_t index = 0; index < monitor.order.size(); index++) {
            const int wid = monitor.order[index];
            auto [space_entry, new_space] = monitor.workspaces.try_emplace(wid);
            auto &workspace = space_entry->second;
            const double position = double(index) - active_index;
            if (new_space)
                workspace.position.reset(position + workspace_shift);
            workspace.retiring = false;
            workspace.position.target = position;
            moving = workspace.position.advance(dt) || moving;
            bool dirty = !workspace.initialized || !same_bounds(workspace.area, area) || workspace.layout_type != set->overview_layout_type;
            workspace.stacking.clear();
            for (int i = (int) actual_root->children.size() - 1; i >= 0; i--) {
                auto c = actual_root->children[i];
                if (c->custom_type != (int) TYPE::CLIENT)
                    continue;
                const int cid = *datum<int>(c, "cid");
                if (wid == teaser_workspace || get_monitor(cid) != mid || hypriso->get_active_workspace_id_client(cid) != wid)
                    continue;
                workspace.stacking.push_back(cid);
                auto [thumb_entry, new_thumbnail] = workspace.thumbnails.try_emplace(cid);
                auto &thumbnail = thumb_entry->second;
                if (new_thumbnail) {
                    auto previous = previous_thumbnails.find(cid);
                    if (previous != previous_thumbnails.end() && (previous->second.monitor != mid || previous->second.workspace != wid)) {
                        thumbnail = previous->second.thumbnail;
                        const double pitch = bounds_monitor(mid).w * (1.0 - .16 * std::clamp(openess, 0.0f, 1.0f));
                        thumbnail.slot.x.value += previous->second.translation - workspace.position.value * pitch;
                    } else if (initialized) {
                        thumbnail.fade_start = now;
                        thumbnail.opacity = 0.0f;
                    }
                }
                auto natural = bounds_client_final(cid);
                const auto extents = extents_client(cid);
                natural.x -= extents.left;
                natural.y -= extents.top;
                natural.w += extents.left + extents.right;
                natural.h += extents.top + extents.bottom;
                dirty = dirty || new_thumbnail || !same_bounds(thumbnail.natural, natural);
                thumbnail.natural = natural;
            }
            for (auto it = workspace.thumbnails.begin(); it != workspace.thumbnails.end();) {
                if (std::find(workspace.stacking.begin(), workspace.stacking.end(), it->first) == workspace.stacking.end()) {
                    it = workspace.thumbnails.erase(it);
                    dirty = true;
                } else {
                    ++it;
                }
            }
            if (dirty)
                layout_workspace(workspace, area);
            for (auto &[cid, thumbnail] : workspace.thumbnails) {
                if (thumbnail.fade_start != 0) {
                    const double progress = std::clamp((now - thumbnail.fade_start) / thumbnail_fade_ms, 0.0, 1.0);
                    thumbnail.opacity = progress * progress * (3.0 - 2.0 * progress);
                    if (progress == 1.0)
                        thumbnail.fade_start = 0;
                    else
                        moving = true;
                }
                if (monitor.dragged != cid) {
                    bool returning = thumbnail.slot.advance(dt);
                    returning = thumbnail.drag_x.advance(dt) || returning;
                    returning = thumbnail.drag_y.advance(dt) || returning;
                    moving = returning || moving;
                    if (!returning)
                        thumbnail.elevated_order = 0;
                }
            }
        }
        // Hyprland can remove an empty desktop as soon as it is left. Keep its
        // wallpaper until it has slid offscreen, but stop offering it as a peek.
        for (auto it = monitor.workspaces.begin(); it != monitor.workspaces.end();) {
            auto &workspace = it->second;
            if (std::find(monitor.order.begin(), monitor.order.end(), it->first) != monitor.order.end()) {
                ++it;
                continue;
            }
            if (!workspace.retiring) {
                workspace.retiring = true;
                const double direction = workspace.position.value - workspace_shift;
                workspace.position.target = direction < 0 ? -1.25 : 1.25;
                workspace.thumbnails.clear();
                workspace.stacking.clear();
            }
            moving = workspace.position.advance(dt) || moving;
            if (std::abs(workspace.position.value) >= 1.2) {
                it = monitor.workspaces.erase(it);
            } else {
                ++it;
            }
        }
        if (monitor.dragged != -1) {
            auto space = monitor.workspaces.find(monitor.dragged_workspace);
            if (space == monitor.workspaces.end() || !space->second.thumbnails.contains(monitor.dragged)) {
                monitor.dragged = -1;
                monitor.dragged_workspace = -1;
            }
        }
    }
    std::erase_if(scene->monitors, [&monitor_ids](const auto &entry) {
        return std::find(monitor_ids.begin(), monitor_ids.end(), entry.first) == monitor_ids.end();
    });
    for (int i = (int) actual_root->children.size() - 1; i >= 0; i--) {
        auto c = actual_root->children[i];
        if (c->custom_type == (int) TYPE::OVERVIEW && !scene->monitors.contains(*datum<int>(c, "overview_monitor"))) {
            delete c;
            actual_root->children.erase(actual_root->children.begin() + i);
        }
    }
    if (moving)
        request_refresh();
}

static void paint_monitor(int monitor_id) {
    if (!scene || !scene->monitors.contains(monitor_id))
        return;
    auto &monitor = scene->monitors.at(monitor_id);
    // screenshot_workspace temporarily replaces the compositor's active
    // workspace. Its preview must render that workspace, not our live selection.
    const bool snapshot = painting_workspace_snapshot || snapshot_workspace != -1;
    const int rendering_workspace = snapshot_workspace != -1 ? snapshot_workspace : hypriso->get_active_workspace_id(monitor_id);
    if (!snapshot) {
        monitor.window_options.clear();
        monitor.workspace_options.clear();
    }
    const auto mb = bounds_monitor(monitor_id);
    const auto s = scale(monitor_id);
    const auto progress = snapshot ? 1.0f : std::clamp(openess, 0.0f, 1.0f);
    const auto old_clip = hypriso->clip;
    const auto old_clipbox = hypriso->clipbox;
    defer(hypriso->clip = old_clip; hypriso->clipbox = old_clipbox);
    hypriso->clip = true;
    hypriso->clipbox = render_bounds(mb, monitor_id);
    rect(hypriso->clipbox, {0, 0, 0, .7f}, 0, 0, 2.0, true);

    // At full openness, a card occupies 80% of the monitor and the pitch is
    // 84%, leaving a gap and a visible slice of each neighboring workspace.
    const double pitch = mb.w * (1.0 - .16 * progress);
    auto paint_thumbnail = [&](int wid, int cid, double translation, const Bounds &clip) {
        auto &thumbnail = monitor.workspaces.at(wid).thumbnails.at(cid);
        auto natural = thumbnail.natural;
        const auto origin = scene->desktop_origins.find(cid);
        if (origin != scene->desktop_origins.end()) {
            natural = lerp(natural, origin->second, scene->desktop_origin_weight);
        } else if (hypriso->is_hidden(cid)) {
            auto dock_bounds = dock::get_location(hypriso->monitor_name(monitor_id), cid);
            dock_bounds.y += mb.h;
            natural = lerp(dock_bounds, natural, progress);
        }
        auto b = snapshot
            ? Bounds(thumbnail.slot.x.target, thumbnail.slot.y.target, thumbnail.slot.w.target, thumbnail.slot.h.target)
            : lerp(natural, thumbnail.slot.bounds(), progress);
        b.x += translation + (snapshot ? 0 : thumbnail.drag_x.value);
        b.y += snapshot ? 0 : thumbnail.drag_y.value;
        hypriso->clipbox = render_bounds(clip, monitor_id);
        hypriso->draw_deco_thumbnail(cid, render_bounds(b, monitor_id), 0, 2.0f, 0, snapshot ? 1.0f : thumbnail.opacity);
        const auto hit = b.intersection(clip);
        if (!snapshot && !hit.empty())
            monitor.window_options.push_back({cid, wid, hit});
    };
    auto paint_order = snapshot ? std::vector<int>{rendering_workspace} : monitor.order;
    if (!snapshot) {
        const auto active = std::find(monitor.order.begin(), monitor.order.end(), monitor.active);
        const auto workspace = monitor.workspaces.find(monitor.active);
        if (active != monitor.order.end() && workspace != monitor.workspaces.end() && workspace->second.thumbnails.empty()) {
            const bool has_workspace_to_right = std::any_of(active + 1, monitor.order.end(), [](int wid) {
                return wid != teaser_workspace;
            });
            if (!has_workspace_to_right)
                std::erase(paint_order, teaser_workspace);
        }
        for (const auto &[wid, workspace] : monitor.workspaces) {
            if (workspace.retiring)
                paint_order.insert(paint_order.begin(), wid);
        }
    }
    for (auto wid : paint_order) {
        if (!monitor.workspaces.contains(wid))
            continue;
        auto &workspace = monitor.workspaces.at(wid);
        const double translation = snapshot ? 0 : workspace.position.value * pitch;
        auto wallpaper = mb;
        wallpaper.scale_from_center(1.0 - .2 * progress);
        wallpaper.x += translation;
        const auto visible = wallpaper.intersection(mb);
        if (visible.empty())
            continue;
        hypriso->clipbox = render_bounds(mb, monitor_id);
        auto b = render_bounds(wallpaper, monitor_id);
        render_drop_shadow(monitor_id, 1, {.1, .1, .1, progress}, 14 * s * progress, 2.0, b, 50 * s);
        hypriso->draw_wallpaper(monitor_id, b, 14 * s * progress);
        // Fade the live icon layer every frame instead of baking its opacity
        // into the wallpaper capture, which refreshes much less frequently.
        if (!snapshot) {
            hypriso->clipbox = render_bounds(visible, monitor_id);
            desktop_icons::paint_overview(monitor_id, b, 1.0f - progress);
            hypriso->clipbox = render_bounds(mb, monitor_id);
        }
        b.shrink(2);
        border(b, {1, 1, 1, .05}, 1, 0, 14 * s * progress);
        if (!snapshot && !workspace.retiring)
            monitor.workspace_options.push_back({wid, visible});
        for (auto cid : workspace.stacking) {
            if (!snapshot && (cid == monitor.dragged || workspace.thumbnails.at(cid).elevated_order != 0))
                continue;
            // Allow the active desktop's original windows to reach the screen
            // edges during opening/closing and allow released drags to return.
            const auto clip = wid == monitor.active ? mb : visible;
            paint_thumbnail(wid, cid, translation, clip);
        }
    }
    if (snapshot)
        return;
    // Returning thumbnails stay above every card and ordinary thumbnail. Sort
    // by drag order so a newer release sits above older returning thumbnails.
    struct ElevatedThumbnail {
        unsigned long long order;
        int workspace;
        int cid;
    };
    std::vector<ElevatedThumbnail> elevated;
    for (const auto &[wid, workspace] : monitor.workspaces) {
        for (const auto &[cid, thumbnail] : workspace.thumbnails) {
            if (thumbnail.elevated_order != 0 && cid != monitor.dragged)
                elevated.push_back({thumbnail.elevated_order, wid, cid});
        }
    }
    std::sort(elevated.begin(), elevated.end(), [](const auto &a, const auto &b) { return a.order < b.order; });
    for (const auto &item : elevated)
        paint_thumbnail(item.workspace, item.cid, monitor.workspaces.at(item.workspace).position.value * pitch, mb);
    if (monitor.dragged != -1 && monitor.workspaces.contains(monitor.dragged_workspace)) {
        auto &workspace = monitor.workspaces.at(monitor.dragged_workspace);
        if (workspace.thumbnails.contains(monitor.dragged))
            paint_thumbnail(monitor.dragged_workspace, monitor.dragged, workspace.position.value * pitch, mb);
    }
}

static int reserve_teaser_workspace(int monitor_id) {
    auto &monitor = scene->monitors.at(monitor_id);
    if (monitor.pending_workspace != 0)
        return monitor.pending_workspace;
    // Workspace numbers are global. Avoid reusing a desktop on another monitor.
    int next = 1;
    for (auto m : actual_monitors) {
        for (auto raw : hypriso->get_workspaces(*datum<int>(m, "cid")))
            next = std::max(next, raw + 1);
    }
    for (const auto &[mid, state] : scene->monitors)
        next = std::max(next, state.pending_workspace + 1);
    monitor.pending_workspace = next;
    return next;
}

static void queue_workspace_drop(int cid, int raw_workspace) {
    const auto generation = lifecycle;
    later_immediate([cid, raw_workspace, generation](Timer *) {
        if (!running || generation != lifecycle || !get_cid_container(cid))
            return;
        hypriso->move_to_workspace(cid, raw_workspace, false);
        update_scene();
        request_refresh();
    });
}

static void drop_on_workspace(int cid, double x, double y) {
    const int mid = hypriso->monitor_from_cursor();
    // The switcher is above the overview, so it takes precedence over cards.
    for (auto c : actual_root->children) {
        if (c->custom_type != (int) TYPE::WORKSPACE_SWITCHER)
            continue;
        for (auto ch : c->children) {
            if (!(ch->handles_pierced ? ch->handles_pierced(ch, x, y) : bounds_contains(ch->real_bounds, x, y)))
                continue;
            const int workspace = *datum<int>(ch, "workspace");
            if (workspace == -1) {
                const auto spaces = hypriso->get_workspaces(mid);
                queue_workspace_drop(cid, spaces.empty() ? 1 : spaces.back() + 1);
            } else {
                queue_workspace_drop(cid, hypriso->space_id_to_raw(workspace));
            }
            return;
        }
    }
    if (!scene || !scene->monitors.contains(mid))
        return;
    const auto &monitor = scene->monitors.at(mid);
    for (auto it = monitor.workspace_options.rbegin(); it != monitor.workspace_options.rend(); ++it) {
        if (bounds_contains(it->b, x, y)) {
            const int raw = it->workspace == teaser_workspace
                ? reserve_teaser_workspace(mid) : hypriso->space_id_to_raw(it->workspace);
            queue_workspace_drop(cid, raw);
            return;
        }
    }
    // The space around the central card also belongs to the current desktop.
    if (bounds_contains(bounds_monitor(mid), x, y))
        queue_workspace_drop(cid, hypriso->space_id_to_raw(monitor.active));
}

static OverviewMonitor *monitor_state(int monitor) {
    if (!scene)
        return nullptr;
    auto it = scene->monitors.find(monitor);
    return it == scene->monitors.end() ? nullptr : &it->second;
}

static OverviewThumbnail *dragged_thumbnail(OverviewMonitor &monitor) {
    auto workspace = monitor.workspaces.find(monitor.dragged_workspace);
    if (workspace == monitor.workspaces.end())
        return nullptr;
    auto thumbnail = workspace->second.thumbnails.find(monitor.dragged);
    return thumbnail == workspace->second.thumbnails.end() ? nullptr : &thumbnail->second;
}

void create_overview_for_monitor(int monitor) {
    auto over = actual_root->child(FILL_SPACE, FILL_SPACE);
    over->custom_type = (int) TYPE::OVERVIEW;
    *datum<int>(over, "overview_monitor") = monitor;
    over->when_drag_end_is_click = false;
    over->pre_layout = [monitor](Container *root, Container *c, const Bounds &b) {
        c->wanted_bounds = bounds_monitor(monitor);
        c->real_bounds = c->wanted_bounds;
    };
    over->when_paint = [monitor](Container *root, Container *c) {
        auto [rid, s, stage, active_id] = roots_info(actual_root, root);
        if (rid == monitor && stage == (int) STAGE::RENDER_POST_WINDOWS)
            paint_monitor(monitor);
    };
    over->when_drag_start = [monitor](Container *root, Container *c) {
        auto state = monitor_state(monitor);
        if (!state || c->state.mouse_button_pressed != BTN_LEFT)
            return;
        state->dragged = -1;
        for (auto it = state->window_options.rbegin(); it != state->window_options.rend(); ++it) {
            if (!bounds_contains(it->b, root->mouse_initial_x, root->mouse_initial_y))
                continue;
            state->dragged = it->cid;
            state->dragged_workspace = it->workspace;
            if (auto thumbnail = dragged_thumbnail(*state)) {
                thumbnail->elevated_order = ++scene->drag_order;
                state->drag_start_x = root->mouse_initial_x - thumbnail->drag_x.value;
                state->drag_start_y = root->mouse_initial_y - thumbnail->drag_y.value;
                thumbnail->drag_x.value = root->mouse_current_x - state->drag_start_x;
                thumbnail->drag_y.value = root->mouse_current_y - state->drag_start_y;
                thumbnail->drag_x.velocity = 0;
                thumbnail->drag_y.velocity = 0;
            }
            break;
        }
    };
    over->when_drag = [monitor](Container *root, Container *c) {
        auto state = monitor_state(monitor);
        if (!state)
            return;
        if (auto thumbnail = dragged_thumbnail(*state)) {
            thumbnail->drag_x.value = root->mouse_current_x - state->drag_start_x;
            thumbnail->drag_y.value = root->mouse_current_y - state->drag_start_y;
            request_refresh();
        }
    };
    over->when_drag_end = [monitor](Container *root, Container *c) {
        auto state = monitor_state(monitor);
        if (!state)
            return;
        if (auto thumbnail = dragged_thumbnail(*state)) {
            thumbnail->drag_x.target = 0;
            thumbnail->drag_y.target = 0;
            drop_on_workspace(state->dragged, root->mouse_current_x, root->mouse_current_y);
        }
        state->dragged = -1;
        state->dragged_workspace = -1;
        request_refresh();
    };
    over->when_clicked = [monitor](Container *root, Container *c) {
        auto state = monitor_state(monitor);
        if (!state)
            return;
        const auto x = root->mouse_current_x;
        const auto y = root->mouse_current_y;
        // A thumbnail in a peek selects that desktop, just like its wallpaper.
        for (auto it = state->workspace_options.rbegin(); it != state->workspace_options.rend(); ++it) {
            if (it->workspace == state->active || !bounds_contains(it->b, x, y))
                continue;
            if (c->state.mouse_button_pressed == BTN_LEFT) {
                const int workspace = it->workspace;
                const auto generation = lifecycle;
                later_immediate([monitor, workspace, generation](Timer *) {
                    auto state = monitor_state(monitor);
                    if (!running || generation != lifecycle || !state || !state->workspaces.contains(workspace))
                        return;
                    if (workspace == teaser_workspace)
                        hypriso->move_to_workspace(reserve_teaser_workspace(monitor), false);
                    else
                        hypriso->move_to_workspace_id(workspace);
                    update_scene();
                    request_refresh();
                });
            }
            return;
        }
        for (auto it = state->window_options.rbegin(); it != state->window_options.rend(); ++it) {
            if (it->workspace != state->active || !bounds_contains(it->b, x, y) || !get_cid_container(it->cid))
                continue;
            if (c->state.mouse_button_pressed == BTN_LEFT) {
                hypriso->bring_to_front(it->cid, true);
                hypriso->set_hidden(it->cid, false, false);
            } else if (c->state.mouse_button_pressed == BTN_RIGHT) {
                titlebar::titlebar_right_click(it->cid);
            } else if (c->state.mouse_button_pressed == BTN_MIDDLE) {
                close_window(it->cid);
            }
            break;
        }
        if (c->state.mouse_button_pressed == BTN_LEFT) {
            const auto generation = lifecycle;
            later_immediate([generation](Timer *) {
                if (running && generation == lifecycle)
                    overview::close();
            });
        }
    };
}

static void screenshots() {
    if (!scene)
        return;
    const auto now = get_current_time_in_ms();
    std::map<int, int> active_workspaces;
    std::map<int, int> priority_counts;
    std::vector<int> priority_windows;
    for (auto m : actual_monitors) {
        const int mid = *datum<int>(m, "cid");
        active_workspaces[mid] = hypriso->get_active_workspace_id(mid);
    }
    // The compositor stack runs from bottom to top. Limit the fast group to
    // the top four clients on each monitor's active workspace.
    const auto stacking = get_window_stacking_order();
    for (auto it = stacking.rbegin(); it != stacking.rend(); ++it) {
        const int mid = get_monitor(*it);
        if (!active_workspaces.contains(mid) || priority_counts[mid] >= 4 || !get_cid_container(*it))
            continue;
        if (hypriso->get_active_workspace_id_client(*it) != active_workspaces.at(mid))
            continue;
        priority_windows.push_back(*it);
        priority_counts[mid]++;
    }
    // Use the rendered hit boxes, including peek and returning thumbnails,
    // rather than the original desktop window bounds.
    int hovered = -1;
    const auto monitor = scene->monitors.find(hypriso->monitor_from_cursor());
    if (monitor != scene->monitors.end()) {
        const auto &options = monitor->second.window_options;
        for (auto it = options.rbegin(); it != options.rend(); ++it) {
            if (bounds_contains(it->b, actual_root->mouse_current_x, actual_root->mouse_current_y)) {
                hovered = it->cid;
                break;
            }
        }
    }
    bool captured = false;
    for (int i = (int) actual_root->children.size() - 1; i >= 0; i--) {
        auto c = actual_root->children[i];
        if (c->custom_type != (int) TYPE::CLIENT)
            continue;
        const int cid = *datum<int>(c, "cid");
        const bool fast = cid == hovered || std::find(priority_windows.begin(), priority_windows.end(), cid) != priority_windows.end();
        const auto previous = scene->window_captures.find(cid);
        if (previous != scene->window_captures.end() && now - previous->second < (fast ? fast_capture_ms : slow_capture_ms))
            continue;
        // New windows are captured immediately, regardless of their tier.
        hypriso->screenshot_deco(cid);
        scene->window_captures[cid] = get_current_time_in_ms();
        captured = true;
    }
    std::erase_if(scene->window_captures, [](const auto &entry) { return !get_cid_container(entry.first); });
    for (auto m : actual_monitors) {
        const int mid = *datum<int>(m, "cid");
        const auto previous = scene->wallpaper_captures.find(mid);
        if (previous != scene->wallpaper_captures.end() && now - previous->second < slow_capture_ms)
            continue;
        hypriso->screenshot_wallpaper(mid);
        scene->wallpaper_captures[mid] = get_current_time_in_ms();
        captured = true;
    }
    std::erase_if(scene->wallpaper_captures, [&active_workspaces](const auto &entry) { return !active_workspaces.contains(entry.first); });
    if (captured)
        damage_all();
}

static void set_input_bypass(bool bypass) {
    if (hypriso->input_bypass_whitelist == bypass)
        return;
    hypriso->input_bypass_whitelist = bypass;
    hypriso->simulateMouseMovement();
}

static void hold_overview_open() {
    drag_workspace_switcher::open();
    // drag_workspace_switcher::force_hold_open(true);
    set_input_bypass(false);
}

static bool initialize_overview(int monitor) {
    if (running) {
        if (initialized)
            hold_overview_open();
        return true;
    }
    // Reserve ownership before deferred rendering setup can run.
    running = true;
    overview_monitor = monitor;
    scene = std::make_unique<OverviewScene>();
    scene->taking_desktop = show_desktop::is_opened();
    if (scene->taking_desktop)
        minimize_gesture_count++;
    const auto generation = ++lifecycle;
    later_immediate([monitor, generation](Timer *) {
        if (!running || generation != lifecycle)
            return;
        const bool taking_desktop = scene->taking_desktop;
        const auto scalar = std::clamp(show_desktop::get_scalar(), 0.0f, 1.0f);
        if (taking_desktop) {
            // Cancel desktop rendering and capture timers before taking any
            // overview screenshots, retaining the compositor's render filter.
            show_desktop::stop();
            hypriso->whitelist_on = true;
            scene->taking_desktop = false;
        }
        screenshots();
        update_scene();
        if (taking_desktop) {
            for (const auto &[mid, state] : scene->monitors) {
                const auto mb = bounds_monitor(mid);
                const auto locations = dock::try_get_locations(hypriso->monitor_name(mid));
                for (const auto &[wid, workspace] : state.workspaces) {
                    if (wid != hypriso->get_active_workspace_id(mid))
                        continue;
                    for (const auto &[cid, thumbnail] : workspace.thumbnails) {
                        if (hypriso->is_hidden(cid) || is_slept(cid))
                            continue;
                        const auto location = locations.find(cid);
                        // A busy dock falls back to its bottom-center area.
                        auto origin = location != locations.end() ? location->second : Bounds((mb.w - 100) * .5, 0, 100, 100);
                        origin.x += mb.x;
                        origin.y = mb.bottom();
                        const auto &natural = thumbnail.natural;
                        const auto aspect = natural.w / std::max(1.0, natural.h);
                        if (aspect > origin.w / std::max(1.0, origin.h)) {
                            const auto height = origin.w / aspect;
                            origin.y += (origin.h - height) * .5;
                            origin.h = height;
                        } else {
                            const auto width = origin.h * aspect;
                            origin.x += (origin.w - width) * .5;
                            origin.w = width;
                        }
                        scene->desktop_origins[cid] = lerp(natural, origin, scalar);
                    }
                }
            }
        }
        hypriso->whitelist_on = true;
        initialized = true;
        hold_overview_open();
        request_refresh();
    });

    later(1000.0f / hypriso->fps(monitor), [monitor, generation](Timer *t) {
        t->keep_running = running && generation == lifecycle;
        if (t->keep_running && initialized)
            update_scene();
    });
    later(fast_capture_ms, [generation](Timer *t) {
        t->keep_running = running && generation == lifecycle;
        if (t->keep_running && initialized)
            screenshots();
    });
    return true;
}

void overview_actual_close() {
    const bool was_initialized = initialized;
    lifecycle++;
    animation_generation++;
    animating = false;
    initialized = false;
    openess = 0.0;
    running = false;
    drag_workspace_switcher::close();
    if (scene && scene->taking_desktop)
        show_desktop::stop();
    scene.reset();

    auto m = actual_root;
    for (int i = m->children.size() - 1; i >= 0; i--) {
        auto c = m->children[i];
        if (c->custom_type == (int) TYPE::OVERVIEW) {
            delete c;
            m->children.erase(m->children.begin() + i);
        }
    }
    if (was_initialized) {
        hypriso->whitelist_on = false;
        hypriso->input_bypass_whitelist = false;
        hypriso->simulateMouseMovement();
    }
    damage_all();
    request_refresh();
}

static void animate_overview(float target, float velocity, SpringParams params, bool gesture_release = false) {
    const auto generation = ++animation_generation;
    animating = false;
    if (openess == target) {
        if (target == 0.0f)
            overview_actual_close();
        return;
    }

    animating = true;
    animation_target = target;
    const auto initial_progress = openess;
    const auto initial_origin_weight = scene->desktop_origin_weight;
    later((1000.0f / hypriso->fps(overview_monitor)) * .8,
          [generation, initial_progress, initial_origin_weight, target, velocity, params, gesture_release, start = 0L](Timer *t) mutable {
        t->keep_running = running && generation == animation_generation;
        if (!t->keep_running || !initialized)
            return;
        const auto now = get_current_time_in_ms();
        if (start == 0)
            start = now;
        const auto state = springEvaluate((now - start) / 1000.0, initial_progress, target, velocity, params);
        const bool finished = gesture_release
            ? (target == 0.0f ? state.value <= .001 : state.value >= .999)
            : (std::abs(state.value - target) <= .001 && std::abs(state.velocity) <= .001);
        openess = finished ? target : static_cast<float>(state.value);
        // An interrupted entrance must still close onto the desktop.
        if (target == 0.0f && initial_progress > 0.0f)
            scene->desktop_origin_weight = initial_origin_weight * std::clamp(openess / initial_progress, 0.0f, 1.0f);
        if (openess >= 1.0f)
            scene->desktop_origins.clear();
        if (target == 0.0f && openess < .3f)
            set_input_bypass(true);
        if (finished) {
            animating = false;
            t->keep_running = false;
            if (target == 0.0f)
                overview_actual_close();
        }
        request_refresh();
    });
}

void overview::open(int monitor) {
    if (!initialize_overview(monitor))
        return;
    if (!animating || animation_target != 1.0f)
        animate_overview(1.0f, 0.0f, {overview_open_time_ms / 2000.0, .97});
}

void overview::close(bool focus) {
    if (!running || (animating && animation_target == 0.0f))
        return;
    if (initialized)
        set_input_bypass(true);
    animate_overview(0.0f, 0.0f, {overview_open_time_ms / 2000.0, 1.0});
}

void overview::begin_gesture(int monitor) {
    if (!initialize_overview(monitor))
        return;
    animation_generation++;
    animating = false;
    openess = std::clamp(openess, 0.0f, 1.0f);
    request_refresh();
}

void overview::end_gesture(long start, long end, float y_offset) {
    if (!running)
        return;
    if (openess < .01f) {
        overview_actual_close();
    } else if (openess > .99f) {
        overwrite_openess(1.0f);
    } else {
        const auto release = desktop_gesture::release(start, end, -y_offset, openess);
        animate_overview(release.target, release.velocity, {.3, 1.0}, true);
    }
}

void overview::instant_close() {
    if (running)
        overview_actual_close();
}

void overview::click(int id, int button, int state, float x, float y) {

}

void overview::overwrite_openess(float a) {
    if (!running)
        return;
    animation_generation++;
    animating = false;
    openess = std::clamp(a, 0.0f, 1.0f);
    if (openess == 1.0f)
        scene->desktop_origins.clear();
    if (initialized)
        set_input_bypass(false);
    request_refresh();
}

void overview::fake_paint(int id) {
    snapshot_workspace = id;
}

void overview::should_draw(bool state) {

}

void overview::should_force_paint(bool state) {
    painting_workspace_snapshot = state;
}

bool overview::is_showing() {
    return running;
}

float overview::get_openess() {
    return openess;
}

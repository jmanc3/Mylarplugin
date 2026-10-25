#include "dock_thumbnails.h"

#include "heart.h"
#include "popup.h"
#include "titlebar.h"

#include <algorithm>
#include <cmath>
#include <linux/input-event-codes.h>
#include <vector>

// The dock lives in its own Wayland client, so it cannot draw over its own
// windows. Instead the preview is a normal compositor-side overlay container
// painted in RENDER_PRE_CURSOR (above the dock layer) using the captured
// window thumbnails, exactly like alt_tab/coverflow do.

namespace {

constexpr float kPad = 10;
constexpr float kGap = 8;
constexpr float kThumbSize = 150;      // logical px, along the short side
constexpr float kMaxPageFraction = .8; // of the monitor, before shrinking
constexpr float kDockMargin = 8;
constexpr int kCloseGraceMs = 220;
constexpr float kCloseInset = 6;
constexpr float kCloseMinSize = 12;
constexpr float kCloseMaxSize = 22;
constexpr int kStripMax = 6;    // up to this many windows use a single strip
constexpr int kGridLineMax = 6; // grid rows/columns along the dock edge

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

// Newly opened windows can be captured before they have drawn their first
// frame (the opening animation may still have size zero), which leaves the
// thumbnail black. Re-capture shortly after building the preview so the new
// window gets a real frame without waiting for the user to hover again.
void refresh_preview_screenshots(Container *preview, int token, std::vector<int> ids, int attempt) {
    static constexpr int delays[] = {120, 250, 500, 900, 1500};
    constexpr int delay_count = sizeof(delays) / sizeof(delays[0]);
    if (attempt >= delay_count)
        return;

    later(delays[attempt], [preview, token, ids = std::move(ids), attempt](Timer *) {
        auto &s = state();
        if (!s.container || s.container != preview || s.refresh_token != token || !same_ids(s.ids, ids))
            return;
        for (int cid : ids)
            hypriso->screenshot(cid);
        damage_all();
        refresh_preview_screenshots(preview, token, std::move(ids), attempt + 1);
    });
}

void destroy_now() {
    auto &s = state();
    s.generation++;
    s.refresh_token++;
    s.pending_close = false;
    s.hovered = false;
    s.has_anchor = false;
    s.has_icon = false;
    s.source.clear();
    s.ids.clear();
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
    render_drop_shadow(rid, 1.0, {0, 0, 0, .45}, std::round(12 * s), 2.0f, c->real_bounds, std::round(18 * s));
    rect(c->real_bounds, {.11, .11, .12, .92}, 0, std::round(12 * s), 2.0f, true);
    border(c->real_bounds, {1, 1, 1, .1}, std::max(1.0f, std::round(s)), 0, std::round(12 * s));
}

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
    hypriso->draw_thumbnail(cid, c->real_bounds, std::round(9 * s), 2.0f, 0);
    if (c->state.mouse_hovering) {
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
                widths[i] = std::max(1.0, std::round(s * items[i].aspect));
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

    // Capture before creating the overlay so the first frame already has pixels.
    for (const auto &item : items)
        hypriso->screenshot(item.cid);

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
                const double size = close->wanted_bounds.w;
                const double room = std::min(thumb->real_bounds.w, thumb->real_bounds.h) - size;
                const double inset = std::clamp<double>(room * .5, 0.0, kCloseInset);
                close->real_bounds = Bounds(
                    thumb->real_bounds.x + thumb->real_bounds.w - inset - size,
                    thumb->real_bounds.y + inset,
                    size, size);
            }
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

        const double close_size = std::round(std::clamp(0.35 * std::min(widths[i], heights[i]), (double) kCloseMinSize, (double) kCloseMaxSize));
        auto *close = thumb->child((int) close_size, (int) close_size);
        close->z_index = 100;
        close->wanted_bounds = Bounds(0, 0, close_size, close_size);
        close->real_bounds = Bounds(0, 0, close_size, close_size);
        *datum<int>(close, "cid") = items[i].cid;
        *datum<int>(close, "mid") = mid;
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

    preview->pre_layout(actual_root, preview, actual_root->real_bounds);
    damage_all();

    // Capture again shortly after so windows that are still opening produce a
    // real frame instead of a black thumbnail.
    refresh_preview_screenshots(preview, token, ids, 0);
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
        destroy_now();
    });
}

void dock_thumbnails::keep_open() {
    auto &s = state();
    s.pending_close = false;
    s.generation++;
}

#include "dock/dock_extra.h"

#include "dock/dock_internal.h"

#include "dock/dock_bluetooth.h"
#include "dock/dock_projection.h"
#include "dock/dock_volume.h"
#include "systray.h"

#include <cstdlib>
#include <linux/input-event-codes.h>

bool nightlight_on = false;

// State handed to the short-lived timer that reopens a popup after the extra
// menu window has closed.
struct Reformed {
    MylarWindow *window = nullptr;
    Dock *dock = nullptr;
    float dpi = 1.0;
};

static MylarWindow *extra_window(Dock *dock) {
    return dock->extra;
}

static Container *add_extra_button(Container *parent, std::string icon,
                                   std::function<void(Container *root, Container *c)> on_clicked) {
    static constexpr float button_pad = 10;
    auto button = parent->child(FILL_SPACE, FILL_SPACE);
    button->pre_layout = [](Container *root, Container *c, const Bounds &b) {
        auto dock = (Dock *) root->user_data;
        auto window = extra_window(dock);
        auto s = window->raw_window->dpi;
        c->wanted_pad = Bounds(button_pad * s, button_pad * s, button_pad * s, button_pad * s);
    };
    auto icon_button = button->child(FILL_SPACE, FILL_SPACE);
    icon_button->when_clicked = std::move(on_clicked);
    icon_button->when_paint = [icon = std::move(icon)](Container *root, Container *c) {
        auto dock = (Dock *) root->user_data;
        auto window = extra_window(dock);
        auto cr = window->raw_window->drawing_context;
        auto dpi = window->raw_window->dpi;
        if (c->state.mouse_pressing || c->state.mouse_hovering) {
            if (c->state.mouse_pressing) {
                set_argb(cr, {0, 0, 0, .2});
            } else {
                set_argb(cr, {0, 0, 0, .1});
            }
            drawRoundedRect(cr, c->real_bounds.x, c->real_bounds.y, c->real_bounds.w, c->real_bounds.h, 10 * dpi, 1.0);
            cr->fill();
        }
        set_argb(cr, {0, 0, 0, .2});
        drawRoundedRect(cr, c->real_bounds.x, c->real_bounds.y, c->real_bounds.w, c->real_bounds.h, 10 * dpi, 1.0);
        cr->stroke();

        auto b = draw_text(cr, 0, 0, icon, 36, false, icon_font, -1, -1, {0, 0, 0, 1});
        draw_text(cr, center_x(c, b.w), center_y(c, b.h), icon, 36, true, icon_font, -1, -1, {0, 0, 0, 1});
    };
    return button;
}

static void open_bluetooth_popup(Dock *dock) {
    auto window = extra_window(dock);
    windowing::close_window(window->raw_window);
    if (dock->window->popup_window == window)
        dock->window->popup_window = nullptr;

    auto mylar = dock->window;
    auto dpi = mylar->raw_window->dpi;

    auto reformed = new Reformed;
    reformed->dpi = dpi;
    reformed->window = mylar;
    reformed->dock = dock;

    windowing::timer(dock->app, 40, [](void *data) {
        auto r = std::unique_ptr<Reformed>(static_cast<Reformed *>(data));
        auto mylar = r->window;
        auto dock = r->dock;
        std::lock_guard<std::mutex> lock(dock->app->mutex);
        if (finished || !bluetooth_available.load() || mylar->popup_window || !windowing::has_window(mylar->raw_window))
            return;
        auto dpi = r->dpi;
        auto c = container_by_name("extra", dock->window->root);

        RawWindowSettings settings = make_icon_anchored_popup_settings(c, dpi, 376, 448);

        dock->bluetooth = open_mylar_popup(mylar, settings);
        if (!dock->bluetooth)
            return;
        dock->bluetooth->root->user_data = dock;
        dock->bluetooth->root->wanted_bounds.w = FILL_SPACE;
        dock->bluetooth->root->wanted_bounds.h = FILL_SPACE;
        fill_bluetooth_container(dock);
        windowing::redraw(dock->bluetooth->raw_window);
    }, reformed);
}

static void open_projection_popup(Dock *dock) {
    auto window = extra_window(dock);
    windowing::close_window(window->raw_window);

    auto mylar = dock->window;
    auto dpi = mylar->raw_window->dpi;

    auto reformed = new Reformed;
    reformed->dpi = dpi;
    reformed->window = mylar;
    reformed->dock = dock;

    windowing::timer(dock->app, 40, [](void *data) {
        main_thread([data]() {
            auto r = std::unique_ptr<Reformed>(static_cast<Reformed *>(data));
            auto mylar = r->window;
            auto dock = r->dock;
            auto dpi = r->dpi;
            auto c = container_by_name("extra", dock->window->root);

            RawWindowSettings settings = make_icon_anchored_popup_settings(c, dpi, volume_popup_w, volume_popup_w * .5);

            dock->projection = open_mylar_popup(mylar, settings);
            if (!dock->projection)
                return;
            dock->projection->root->on_closed = [](Container *root) {
                auto dock = (Dock *) root->user_data;
                dock->projection = nullptr;
            };
            dock->projection->root->user_data = dock;
            dock->projection->root->wanted_bounds.w = FILL_SPACE;
            dock->projection->root->wanted_bounds.h = FILL_SPACE;
            fill_projection_container(dock);
            windowing::redraw(dock->projection->raw_window);
        });
    }, reformed);
}

/*************************************************
 *
 * System tray (grey area under the big buttons)
 *
 *************************************************/

struct TrayIconCache {
    unsigned long revision = 0;
    int size = 0;
    cairo_surface_t *surface = nullptr;
    bool attempted = false;
};

struct TrayAreaData : UserData {
    unsigned long revision = (unsigned long) -1;
    std::vector<TrayItemView> items;
    std::map<std::string, TrayIconCache> icons;

    ~TrayAreaData() override {
        for (auto &[key, cache] : icons)
            if (cache.surface)
                cairo_surface_destroy(cache.surface);
    }
};

struct TrayLayout {
    double cell = 0;
    double icon = 0;
    double x = 0;
    double y = 0;
    int per_row = 1;
};

static TrayLayout tray_layout(const Bounds &b, size_t count, double dpi) {
    TrayLayout l;
    double pad = 8 * dpi;
    l.cell = 34 * dpi;
    for (int attempt = 0; attempt < 4; attempt++) {
        l.per_row = std::max(1, (int) std::floor((b.w - pad * 2) / l.cell));
        int rows = std::max(1, (int) std::ceil((double) count / l.per_row));
        if (rows * l.cell <= b.h - pad * 2 + .5 || l.cell <= 20 * dpi)
            break;
        // Shrink the cells until every icon fits.
        l.cell = std::max(20 * dpi, (b.h - pad * 2) / rows);
    }
    l.icon = std::round(l.cell * .65);
    int rows = std::max(1, (int) std::ceil((double) count / l.per_row));
    l.x = b.x + pad;
    l.y = b.y + std::max(pad, (b.h - rows * l.cell) * .5);
    return l;
}

static int tray_item_at(Container *root, Container *c, TrayAreaData *data, double dpi) {
    auto l = tray_layout(c->real_bounds, data->items.size(), dpi);
    int col = (int) std::floor((root->mouse_current_x - l.x) / l.cell);
    int row = (int) std::floor((root->mouse_current_y - l.y) / l.cell);
    if (col < 0 || col >= l.per_row || row < 0)
        return -1;
    int index = row * l.per_row + col;
    return index < (int) data->items.size() ? index : -1;
}

static std::string tray_icon_file(const TrayItemView &item, int size) {
    namespace fs = std::filesystem;
    const auto &name = item.icon_name;
    if (name.empty())
        return "";
    std::error_code ec;
    if (name[0] == '/')
        return fs::exists(name, ec) ? name : "";
    if (!item.icon_theme_path.empty()) {
        for (auto ext : {".png", ".svg", ".xpm"}) {
            auto path = item.icon_theme_path + "/" + name + ext;
            if (fs::exists(path, ec))
                return path;
        }
        // Apps often ship a small hicolor-style tree in IconThemePath.
        std::string best;
        fs::recursive_directory_iterator it(item.icon_theme_path, fs::directory_options::skip_permission_denied, ec), end;
        for (int visited = 0; !ec && it != end && visited < 2000; it.increment(ec), visited++) {
            if (!it->is_regular_file(ec))
                continue;
            auto path = it->path();
            if (path.stem() != name)
                continue;
            auto ext = path.extension().string();
            if (ext != ".png" && ext != ".svg" && ext != ".xpm")
                continue;
            best = path.string();
            if (ext == ".svg")
                break;
        }
        if (!best.empty())
            return best;
    }
    if (!icons_loaded)
        return "";
    return one_shot_icon(size, {name, to_lower(name)});
}

static cairo_surface_t *tray_icon_surface(TrayAreaData *data, const TrayItemView &item, int size) {
    auto &cache = data->icons[item.key];
    if (cache.attempted && cache.revision == item.revision && cache.size == size)
        return cache.surface;
    if (cache.surface)
        cairo_surface_destroy(cache.surface);
    cache = {};
    cache.revision = item.revision;
    cache.size = size;
    cache.attempted = true;
    auto path = tray_icon_file(item, size);
    if (!path.empty())
        load_icon_full_path(&cache.surface, path, size);
    if (!cache.surface && item.pixels.w > 0 && item.pixels.h > 0) {
        auto pixels = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, item.pixels.w, item.pixels.h);
        cairo_surface_flush(pixels);
        auto stride = cairo_image_surface_get_stride(pixels);
        auto dst = cairo_image_surface_get_data(pixels);
        for (int y = 0; y < item.pixels.h; y++)
            memcpy(dst + (size_t) y * stride, item.pixels.argb.data() + (size_t) y * item.pixels.w, (size_t) item.pixels.w * 4);
        cairo_surface_mark_dirty(pixels);
        // Scale to the cell size once instead of on every paint.
        cache.surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, size, size);
        // Offscreen icon preparation (once per icon), not window painting.
        auto scale_cr = cairo_create(cache.surface);
        double scale = (double) size / std::max(item.pixels.w, item.pixels.h);
        cairo_translate(scale_cr, (size - item.pixels.w * scale) * .5, (size - item.pixels.h * scale) * .5);
        cairo_scale(scale_cr, scale, scale);
        cairo_set_source_surface(scale_cr, pixels, 0, 0);
        cairo_pattern_set_filter(cairo_get_source(scale_cr), CAIRO_FILTER_GOOD);
        cairo_paint(scale_cr);
        cairo_destroy(scale_cr);
        cairo_surface_destroy(pixels);
    }
    return cache.surface;
}

static void open_tray_menu(Dock *dock, std::string key, std::vector<TrayMenuItem> items);

static void tray_clicked(Container *root, Container *c, int button) {
    auto dock = (Dock *) root->user_data;
    auto data = (TrayAreaData *) c->user_data;
    auto dpi = extra_window(dock)->raw_window->dpi;
    int index = tray_item_at(root, c, data, dpi);
    if (index == -1)
        return;
    auto item = data->items[index];
    bool show_menu = !item.xembed && item.has_menu && (button == 3 || (button == 1 && item.item_is_menu));

    windowing::close_window(extra_window(dock)->raw_window);
    if (dock->window->popup_window == extra_window(dock))
        dock->window->popup_window = nullptr;

    if (show_menu) {
        main_thread([dock, key = item.key]() {
            systray::fetch_menu(key, [dock, key](std::vector<TrayMenuItem> menu) {
                if (menu.empty()) {
                    // No usable dbusmenu: let the app show its own.
                    systray::click(key, 3);
                    return;
                }
                open_tray_menu(dock, key, std::move(menu));
            });
        });
    } else {
        main_thread([key = item.key, button]() {
            systray::click(key, button);
        });
    }
}

static void make_tray_area(Container *parent) {
    auto tray = parent->child(FILL_SPACE, FILL_SPACE);
    tray->name = "systray";
    tray->user_data = new TrayAreaData;
    tray->pre_layout = [](Container *root, Container *c, const Bounds &b) {
        auto dock = (Dock *) root->user_data;
        auto s = extra_window(dock)->raw_window->dpi;
        c->wanted_bounds = Bounds(0, 0, FILL_SPACE, 60 * s);
    };
    tray->when_paint = [](Container *root, Container *c) {
        auto dock = (Dock *) root->user_data;
        auto window = extra_window(dock);
        auto cr = window->raw_window->drawing_context;
        auto dpi = window->raw_window->dpi;
        auto data = (TrayAreaData *) c->user_data;
        auto b = c->real_bounds;

        set_argb(cr, {0, 0, 0, .08});
        drawRoundedRect(cr, b.x, b.y, b.w, b.h, 10 * dpi, 1.0);
        cr->fill();

        if (systray::revision() != data->revision) {
            data->items = systray::snapshot(&data->revision);
            // Forget icons of items that are gone.
            for (auto it = data->icons.begin(); it != data->icons.end();) {
                bool alive = std::any_of(data->items.begin(), data->items.end(), [&](const TrayItemView &item) { return item.key == it->first; });
                if (!alive) {
                    if (it->second.surface)
                        cairo_surface_destroy(it->second.surface);
                    it = data->icons.erase(it);
                } else {
                    ++it;
                }
            }
        }

        if (data->items.empty()) {
            auto tb = draw_text(cr, 0, 0, "No tray icons", 10 * dpi, false, set->font, -1, -1, {0, 0, 0, .35});
            draw_text(cr, center_x(c, tb.w), center_y(c, tb.h), "No tray icons", 10 * dpi, true, set->font, -1, -1, {0, 0, 0, .35});
            return;
        }

        auto l = tray_layout(b, data->items.size(), dpi);
        int hovered = c->state.mouse_hovering ? tray_item_at(root, c, data, dpi) : -1;
        cr->save();
        drawRoundedRect(cr, b.x, b.y, b.w, b.h, 10 * dpi, 1.0);
        cr->clip_path();
        for (int i = 0; i < (int) data->items.size(); i++) {
            auto &item = data->items[i];
            double x = l.x + (i % l.per_row) * l.cell;
            double y = l.y + (i / l.per_row) * l.cell;
            if (i == hovered) {
                set_argb(cr, c->state.mouse_pressing ? RGBA(0, 0, 0, .14) : RGBA(0, 0, 0, .08));
                drawRoundedRect(cr, x + 2 * dpi, y + 2 * dpi, l.cell - 4 * dpi, l.cell - 4 * dpi, 6 * dpi, 1.0);
                cr->fill();
            }
            if (item.needs_attention) {
                set_argb(cr, {.95, .45, .1, .25});
                drawRoundedRect(cr, x + 2 * dpi, y + 2 * dpi, l.cell - 4 * dpi, l.cell - 4 * dpi, 6 * dpi, 1.0);
                cr->fill();
            }
            int size = (int) l.icon;
            double ix = std::round(x + (l.cell - size) * .5);
            double iy = std::round(y + (l.cell - size) * .5);
            if (auto surface = tray_icon_surface(data, item, size)) {
                double sw = cairo_image_surface_get_width(surface);
                double sh = cairo_image_surface_get_height(surface);
                cr->save();
                cr->translate(ix, iy);
                if (sw > 0 && sh > 0 && (sw != size || sh != size))
                    cr->scale(size / sw, size / sh);
                cr->set_source_surface(surface, 0, 0);
                cr->paint_source();
                cr->restore();
            } else {
                // No icon: a circle with the first letter of the title.
                set_argb(cr, {0, 0, 0, .2});
                cr->arc(ix + size * .5, iy + size * .5, size * .5, 0, 2 * M_PI);
                cr->fill();
                std::string letter = item.title.empty() ? "?" : item.title.substr(0, 1);
                auto tb = draw_text(cr, 0, 0, letter, size * .5, false, set->font, -1, -1, {0, 0, 0, .8});
                draw_text(cr, ix + size * .5 - tb.w * .5, iy + size * .5 - tb.h * .5, letter, size * .5, true, set->font, -1, -1, {0, 0, 0, .8});
            }
        }
        cr->restore();
    };
    tray->when_mouse_motion = [](Container *root, Container *c) {
        auto dock = (Dock *) root->user_data;
        windowing::redraw(extra_window(dock)->raw_window);
    };
    tray->when_mouse_leaves_container = tray->when_mouse_motion;
    tray->when_clicked = [](Container *root, Container *c) {
        int button = c->state.mouse_button_pressed == BTN_RIGHT ? 3 : c->state.mouse_button_pressed == BTN_MIDDLE ? 2 : 1;
        tray_clicked(root, c, button);
    };
    tray->when_fine_scrolled = [](Container *root, Container *c, double scroll_x, double scroll_y, bool) {
        auto dock = (Dock *) root->user_data;
        auto data = (TrayAreaData *) c->user_data;
        int index = tray_item_at(root, c, data, extra_window(dock)->raw_window->dpi);
        if (index == -1)
            return;
        bool horizontal = std::abs(scroll_x) > std::abs(scroll_y);
        double amount = horizontal ? scroll_x : scroll_y;
        if (amount == 0)
            return;
        int delta = amount > 0 ? 1 : -1;
        main_thread([key = data->items[index].key, delta, horizontal]() {
            systray::scroll(key, delta, horizontal);
        });
    };
}

/*************************************************
 *
 * Tray item menu (com.canonical.dbusmenu)
 *
 *************************************************/

struct TrayMenuData : UserData {
    std::string key;
    std::vector<TrayMenuItem> items;
    std::vector<int> path; // indices of the open submenus
};

static constexpr int tray_menu_row_h = 30;
static constexpr int tray_menu_separator_h = 9;
static constexpr int tray_menu_w = 280;

static const std::vector<TrayMenuItem> &tray_menu_level(TrayMenuData *data) {
    const std::vector<TrayMenuItem> *level = &data->items;
    for (int index : data->path) {
        if (index < 0 || index >= (int) level->size())
            break;
        level = &(*level)[index].children;
    }
    return *level;
}

static int tray_menu_height(const std::vector<TrayMenuItem> &items, bool back_row) {
    int h = back_row ? tray_menu_row_h + tray_menu_separator_h : 0;
    int deepest = 0;
    for (auto &item : items) {
        if (!item.visible)
            continue;
        h += item.separator ? tray_menu_separator_h : tray_menu_row_h;
        if (!item.children.empty())
            deepest = std::max(deepest, tray_menu_height(item.children, true));
    }
    return std::max(h, deepest);
}

// Rows of the current level: -1 is the "Back" row, -2 a separator, otherwise an index into the level.
static std::vector<std::pair<int, Bounds>> tray_menu_rows(TrayMenuData *data, Bounds area, double dpi) {
    std::vector<std::pair<int, Bounds>> rows;
    double y = area.y;
    if (!data->path.empty()) {
        rows.push_back({-1, Bounds(area.x, y, area.w, tray_menu_row_h * dpi)});
        y += tray_menu_row_h * dpi;
        rows.push_back({-2, Bounds(area.x, y, area.w, tray_menu_separator_h * dpi)});
        y += tray_menu_separator_h * dpi;
    }
    auto &level = tray_menu_level(data);
    for (int i = 0; i < (int) level.size(); i++) {
        if (!level[i].visible)
            continue;
        double h = (level[i].separator ? tray_menu_separator_h : tray_menu_row_h) * dpi;
        rows.push_back({level[i].separator ? -2 : i, Bounds(area.x, y, area.w, h)});
        y += h;
    }
    return rows;
}

static void open_tray_menu(Dock *dock, std::string key, std::vector<TrayMenuItem> items) {
    struct Request {
        Dock *dock;
        std::string key;
        std::vector<TrayMenuItem> items;
    };
    auto request = new Request{dock, std::move(key), std::move(items)};
    // Called on the main thread: hop to the dock's thread, where its timers live.
    windowing::run_on_app(dock->app, [request]() {
    windowing::timer(request->dock->app, 40, [](void *raw) {
        auto request = std::unique_ptr<Request>(static_cast<Request *>(raw));
        auto dock = request->dock;
        std::lock_guard<std::mutex> lock(dock->app->mutex);
        auto mylar = dock->window;
        if (finished || dock->closing || mylar->popup_window || !windowing::has_window(mylar->raw_window))
            return;
        auto dpi = mylar->raw_window->dpi;
        auto anchor = container_by_name("extra", mylar->root);
        if (!anchor)
            return;
        int h = std::min(640, tray_menu_height(request->items, false) + 12);
        RawWindowSettings settings = make_icon_anchored_popup_settings(anchor, dpi, tray_menu_w, h);
        dock->tray_menu = open_mylar_popup(mylar, settings);
        if (!dock->tray_menu)
            return;
        auto root = dock->tray_menu->root;
        root->user_data = dock;
        root->wanted_bounds.w = FILL_SPACE;
        root->wanted_bounds.h = FILL_SPACE;
        root->on_closed = [](Container *root) {
            auto dock = (Dock *) root->user_data;
            dock->tray_menu = nullptr;
        };
        root->when_paint = [](Container *root, Container *c) {
            auto dock = (Dock *) root->user_data;
            auto window = dock->tray_menu->raw_window;
            paint_popup_background(window->drawing_context, c->real_bounds, window->dpi);
        };

        auto list = root->child(FILL_SPACE, FILL_SPACE);
        auto data = new TrayMenuData;
        data->key = request->key;
        data->items = std::move(request->items);
        list->user_data = data;
        auto area = [](Container *root, Container *c) {
            auto dpi = ((Dock *) root->user_data)->tray_menu->raw_window->dpi;
            auto b = c->real_bounds;
            return Bounds(b.x + 6 * dpi, b.y + 6 * dpi, b.w - 12 * dpi, b.h - 12 * dpi);
        };
        list->when_paint = [area](Container *root, Container *c) {
            auto dock = (Dock *) root->user_data;
            auto window = dock->tray_menu->raw_window;
            auto cr = window->drawing_context;
            auto dpi = window->dpi;
            auto data = (TrayMenuData *) c->user_data;
            auto &level = tray_menu_level(data);
            cr->save();
            clip_to_popup_background(cr, c->real_bounds, dpi);
            for (auto &[index, rb] : tray_menu_rows(data, area(root, c), dpi)) {
                if (index == -2) {
                    set_argb(cr, {0, 0, 0, .1});
                    cr->rectangle(rb.x + 8 * dpi, std::round(rb.y + rb.h * .5), rb.w - 16 * dpi, std::max(1.0, (double) std::floor(dpi)));
                    cr->fill();
                    continue;
                }
                bool enabled = index == -1 || level[index].enabled;
                bool hovered = c->state.mouse_hovering && bounds_contains(rb, root->mouse_current_x, root->mouse_current_y);
                if (hovered && enabled) {
                    set_argb(cr, c->state.mouse_pressing ? RGBA(0, 0, 0, .14) : RGBA(0, 0, 0, .07));
                    drawRoundedRect(cr, rb.x, rb.y, rb.w, rb.h, 6 * dpi, 1.0);
                    cr->fill();
                }
                RGBA ink = enabled ? RGBA(0, 0, 0, .9) : RGBA(0, 0, 0, .35);
                std::string label = index == -1 ? "‹  Back" : level[index].label;
                auto tb = draw_text(cr, 0, 0, label, 11 * dpi, false, set->font, -1, -1, ink);
                cr->save();
                cr->rectangle(rb.x, rb.y, rb.w - 26 * dpi, rb.h);
                cr->clip_path();
                draw_text(cr, rb.x + 30 * dpi, rb.y + rb.h * .5 - tb.h * .5, label, 11 * dpi, true, set->font, -1, -1, ink);
                cr->restore();
                if (index >= 0) {
                    auto &item = level[index];
                    if (item.toggle_state == 1 && !item.toggle_type.empty()) {
                        std::string mark = item.toggle_type == "radio" ? "●" : "✓";
                        auto mb = draw_text(cr, 0, 0, mark, 11 * dpi, false, set->font, -1, -1, ink);
                        draw_text(cr, rb.x + 15 * dpi - mb.w * .5, rb.y + rb.h * .5 - mb.h * .5, mark, 11 * dpi, true, set->font, -1, -1, ink);
                    }
                    if (!item.children.empty()) {
                        auto ab = draw_text(cr, 0, 0, "›", 14 * dpi, false, set->font, -1, -1, ink);
                        draw_text(cr, rb.x + rb.w - 12 * dpi - ab.w, rb.y + rb.h * .5 - ab.h * .5, "›", 14 * dpi, true, set->font, -1, -1, ink);
                    }
                }
            }
            cr->restore();
        };
        list->when_mouse_motion = [](Container *root, Container *c) {
            windowing::redraw(((Dock *) root->user_data)->tray_menu->raw_window);
        };
        list->when_mouse_leaves_container = list->when_mouse_motion;
        list->when_clicked = [area](Container *root, Container *c) {
            auto dock = (Dock *) root->user_data;
            auto window = dock->tray_menu->raw_window;
            auto data = (TrayMenuData *) c->user_data;
            auto &level = tray_menu_level(data);
            for (auto &[index, rb] : tray_menu_rows(data, area(root, c), window->dpi)) {
                if (!bounds_contains(rb, root->mouse_current_x, root->mouse_current_y))
                    continue;
                if (index == -1) {
                    data->path.pop_back();
                } else if (index >= 0 && level[index].enabled) {
                    if (!level[index].children.empty()) {
                        data->path.push_back(index);
                    } else {
                        int id = level[index].id;
                        main_thread([key = data->key, id]() {
                            systray::menu_clicked(key, id);
                        });
                        windowing::close_window(window);
                        if (dock->window->popup_window == dock->tray_menu)
                            dock->window->popup_window = nullptr;
                        return;
                    }
                }
                windowing::redraw(window);
                return;
            }
        };
        windowing::redraw(dock->tray_menu->raw_window);
    }, request);
    });
}

void fill_extra_container(Container *root) {
    root->type = ::vbox;
    root->when_paint = [](Container *root, Container *c) {
        auto dock = (Dock *) root->user_data;
        auto window = extra_window(dock);
        auto cr = window->raw_window->drawing_context;
        paint_popup_background(cr, c->real_bounds, dock->extra->raw_window->dpi);
    };

    auto top = root->child(::hbox, FILL_SPACE, FILL_SPACE);
    auto bluetooth = add_extra_button(top, "\uE702", [](Container *root, Container *c) {
        auto dock = (Dock *) root->user_data;
        open_bluetooth_popup(dock);
    });
    bluetooth->name = "bluetooth";
    bluetooth->exists = bluetooth_available.load();
    add_extra_button(top, "\uE708", [](Container *, Container *) {
        if (nightlight_on) {
            system("killall hyprsunset");
        } else {
            std::thread t([]() {
                system("hyprsunset -t 5000");
            });
            t.detach();
        }
        nightlight_on = !nightlight_on;
    });
    add_extra_button(top, "\uE087", nullptr);

    auto middle = root->child(::hbox, FILL_SPACE, FILL_SPACE);
    add_extra_button(middle, "\uEBC6", [](Container *root, Container *) {
        auto dock = (Dock *) root->user_data;
        open_projection_popup(dock);
    });
    add_extra_button(middle, "\uE722", [](Container *root, Container *) {
        auto dock = (Dock *) root->user_data;
        auto window = extra_window(dock);
        windowing::close_window(window->raw_window);

        windowing::timer(dock->app, 100, [](void *) {
            system("hyprctl dispatch \"hl.plugin.mylar.screenshot_tool()\"");
        }, nullptr);
    });
    add_extra_button(middle, "\uF140", [](Container *root, Container *) {
        auto dock = (Dock *) root->user_data;
        auto window = extra_window(dock);
        windowing::close_window(window->raw_window);

        system("hyprctl kill");
    });

    make_tray_area(root);
}

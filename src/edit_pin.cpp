#include "edit_pin.h"
#include "pango_font_cache.h"

#include "container.h"
#include "heart.h"

#include "client/raw_windowing.h"
#include "client/windowing.h"
#include "dock/dock.h"
#include "events.h"
#include "hypriso.h"
#include "icons.h"
#include "smooth_scroll.h"

#include <cairo.h>
#include <climits>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <cstdio>
#include <pango/pango-font.h>
#include <thread>
#include <utility>
#include <pango/pango-layout.h>
#include <pango/pango-types.h>
#include <pango/pangocairo.h>
#include <xkbcommon/xkbcommon-keysyms.h>

// One suggestion in the icon name drop-down.
struct IconOption {
    std::string value; // What goes in the field: "name", or ":theme:name" to pin a theme
    std::string name;
    std::string theme;
    std::string path;
    cairo_surface_t *surface = nullptr;
    bool attempted = false;
};

struct PinData : UserData {
    RawApp *app = nullptr;
    MylarWindow *window = nullptr;
    std::string original_stacking_rule;
    
    std::string stacking_rule;
    std::string icon;
    std::string command;

    // Icon name suggestions (like winbar's pinned icon editor)
    std::vector<IconOption> options;
    int active_option = 0;
    bool active_option_preferred = false;
    bool dropdown_open = false;
    float dropdown_scroll = 0;
    float dropdown_overflow = 0;
    float dropdown_view_h = 0;
    SmoothScroll dropdown_smooth;
    bool swallow_escape = false;
    bool dragging_scrollbar = false;
    float scrollbar_grab_offset = 0; // mouse y minus thumb top when the drag began

    // Preview of the icon at the top of the window
    cairo_surface_t *preview = nullptr;
    std::string preview_key;
    double preview_dpi = 0;
    bool preview_loaded = false;

    ~PinData() override {
        for (auto &o : options)
            if (o.surface)
                cairo_surface_destroy(o.surface);
        if (preview)
            cairo_surface_destroy(preview);
    }
};

static constexpr int icon_option_row_h = 34;
static constexpr int icon_option_icon_size = 22;
static constexpr int icon_preview_size = 64;

static void paint_bg(Container *root, Container *c) {
    auto mylar = (PinData*)root->user_data;
    auto cr = mylar->window->raw_window->drawing_context;
    cr->save();
    cr->set_operator(CAIRO_OPERATOR_CLEAR);
    cr->paint_source();
    cr->restore();

    set_rect(cr, c->real_bounds);
    set_argb(cr, RGBA(.941, .957, .976, .9));
    cr->fill();
}

static void rounded_rect_new(drawing::Context *cr, double corner_radius, double x, double y, double width, double height) {
    double radius = corner_radius;
    double degrees = M_PI / 180.0;
    
    cr->new_sub_path();
    
    cr->arc(x + width - radius, y + radius, radius, -90 * degrees, 0 * degrees);
    cr->arc(x + width - radius, y + height - radius, radius, 0 * degrees, 90 * degrees);
    cr->arc(x + radius, y + height - radius, radius, 90 * degrees, 180 * degrees);
    cr->arc(x + radius, y + radius, radius, 180 * degrees, 270 * degrees);
    cr->close_path();
}

static void collect_preorder(Container *node, std::vector<Container*> &out) {
    if (!node) return;
    for (auto *child : node->children)
        collect_preorder(child, out);
    if (node->when_key_event && !node->name.empty())
        out.push_back(node);
}

Container *get_root(Container *c) {
    Container *temp = c->parent;
    int max = 100;
    while (temp->parent != nullptr) {
        if (max-- < 0)
            return temp;
        temp = temp->parent;
    }
    return temp;
}

static Bounds draw_text(drawing::Context *cr, int x, int y, std::string text, int size = 10, bool draw = true, std::string font = set->font, int wrap = -1, int h = -1, RGBA color = {1, 1, 1, 1});

static Container *setup_label(Container *root, Container *label_parent, bool bold, bool editable, std::function<std::string (Container *root, Container *c)> func,
                              std::function<void(std::string)> on_change = nullptr) {
    label_parent->type = ::absolute;
    auto label = label_parent->child(FILL_SPACE, FILL_SPACE);
    STextEditorOptions options;
    options.bold = bold;
    options.editable = editable;
    options.multiline = false;
    options.parent_activates = true;
    options.on_change = std::move(on_change);
    options.window = [](Container *root) { return static_cast<PinData *>(root->user_data)->window->raw_window; };
    options.viewport = [](Container *root, Container *c) { return c->real_bounds; };
    options.on_tab = [](Container *root, Container *c, bool previous) {
        auto data = static_cast<PinData *>(root->user_data);
        std::weak_ptr<bool> lifetime = c->lifetime;
        std::weak_ptr<bool> root_lifetime = root->lifetime;
        windowing::timer(data->app, 1, [root, c, previous, lifetime, root_lifetime](void *) {
            if (lifetime.expired() || root_lifetime.expired())
                return;
            if (previous)
                activate_previous_activatable(root, c);
            else
                activate_next_activatable(root, c);
        }, nullptr);
    };
    setup_text_editor(label, func(root, label_parent), std::move(options));
    label->pre_layout = [](Container *root, Container *c, const Bounds &b) {
        auto bounds = measure_text_editor(root, c);
        c->wanted_bounds.w = FILL_SPACE;
        c->wanted_bounds.h = bounds.h;
    };
    label_parent->pre_layout = [](Container *root, Container *c, const Bounds &b) {
        auto child = c->children[0];
        auto dpi = static_cast<PinData *>(root->user_data)->window->raw_window->dpi;
        child->pre_layout(root, child, b);
        c->wanted_bounds.w = FILL_SPACE;
        c->wanted_bounds.h = child->wanted_bounds.h + 10 * dpi;
        ::layout(root, child, Bounds(b.x + 10 * dpi, b.y + 5 * dpi,
            std::max(0.0, b.w - 20 * dpi), child->wanted_bounds.h));
    };
    label_parent->when_paint = [editable](Container *root, Container *c) {
        if (!editable)
            return;
        auto window = static_cast<PinData *>(root->user_data)->window->raw_window;
        auto cr = window->drawing_context;
        set_argb(cr, {1, 1, 1, 1});
        rounded_rect_new(cr, 6 * window->dpi, c->real_bounds.x, c->real_bounds.y, c->real_bounds.w, c->real_bounds.h);
        cr->fill();
        set_argb(cr, c->active || c->children[0]->active ? RGBA(.23, .6, 1, 1) : RGBA(0, 0, 0, .2));
        rounded_rect_new(cr, 6 * window->dpi, c->real_bounds.x, c->real_bounds.y, c->real_bounds.w, c->real_bounds.h);
        cr->stroke();
    };
    label->when_paint = [](Container *root, Container *c) {
        paint_text_editor(root, c, RGBA(0, 0, 0, 1), RGBA(.2, .5, .8, 1));
    };
    if (editable) {
        label_parent->when_drag_end_is_click = false;
        // Padding around the text participates in the same selection gesture.
        label_parent->when_mouse_down = [label](Container *root, Container *c) {
            label->active = true;
            label->viakey = true;
            label->when_mouse_down(root, label);
        };
        label_parent->when_clicked = [label](Container *root, Container *c) { label->when_clicked(root, label); };
        label_parent->when_drag_start = [label](Container *root, Container *c) { label->when_drag_start(root, label); };
        label_parent->when_drag = [label](Container *root, Container *c) { label->when_drag(root, label); };
        label_parent->when_drag_end = [label](Container *root, Container *c) { label->when_drag_end(root, label); };
        label_parent->when_fine_scrolled = [label](Container *root, Container *c, double x, double y, bool touchpad) {
            label->when_fine_scrolled(root, label, x, y, touchpad);
        };
    }
    return label;
}

static Bounds draw_text(drawing::Context *cr, int x, int y, std::string text, int size, bool draw, std::string font, int wrap, int h, RGBA color) {
    auto layout = get_cached_pango_font(cr, set->font, size, PANGO_WEIGHT_NORMAL, false);
    //pango_layout_set_text(layout, "\uE7E7", strlen("\uE83F"));
    pango_layout_set_text(layout, text.data(), text.size());
    if (wrap == -1) {
        pango_layout_set_wrap(layout, PangoWrapMode::PANGO_WRAP_NONE);
        pango_layout_set_width(layout, -1);
        pango_layout_set_height(layout, -1);
        pango_layout_set_ellipsize(layout, PangoEllipsizeMode::PANGO_ELLIPSIZE_NONE);
    } else {
        pango_layout_set_wrap(layout, PangoWrapMode::PANGO_WRAP_WORD_CHAR);
        pango_layout_set_width(layout, wrap);
        pango_layout_set_height(layout, h);
        pango_layout_set_ellipsize(layout, PangoEllipsizeMode::PANGO_ELLIPSIZE_MIDDLE);
    }
    set_argb(cr, color);
    PangoRectangle ink;
    PangoRectangle logical;
    pango_layout_get_pixel_extents(layout, &ink, &logical);
    if (draw) {
        cr->move_to(std::round(x), std::round(y));
        cr->show_layout(layout);
    }
    return Bounds(ink.width, ink.height, logical.width, logical.height);
}

static void button(Container *root, std::function<std::string()> get_text, std::function<void(Container *, Container *)> on_click) {
    int size = 12;
    
    auto child = root->child(FILL_SPACE, FILL_SPACE);
    child->when_clicked = on_click;
    child->when_key_event = [](Container *root, Container* c, int key, bool pressed, xkb_keysym_t sym, int mods, bool is_text, std::string text) {
        if ((is_text && text == " ") || sym == XKB_KEY_Return) {
            if (c->active) {
                if (pressed) {
                    c->state.mouse_pressing = true;
                } else {
                    c->state.mouse_pressing = false;
                    if (c->when_clicked) {
                        c->when_clicked(root, c);
                    }                    
                }
            }
        }
        if (!c->active)
            return;
        if (!pressed)
            return;
        if (sym == XKB_KEY_Tab) {
            auto root_data = (PinData *) root->user_data;
            windowing::timer(root_data->app, 1, [root, c](void *data) {
                auto actual_root = get_root(c);
                activate_next_activatable(actual_root, c);
            }, nullptr);
        }
        if (sym == XKB_KEY_ISO_Left_Tab) {
            auto root_data = (PinData *) root->user_data;
            windowing::timer(root_data->app, 1, [root, c](void *data) {
                auto actual_root = get_root(c);
                activate_previous_activatable(actual_root, c);
            }, nullptr);
        }
    };
    child->when_active_status_changed = [](Container* root, Container* self) {
        if (!self->active)
            self->state.mouse_pressing = false;
    };
    child->name = child->uuid;
    child->when_paint = [size, get_text](Container *root, Container *c) {
        auto mylar = (PinData*)root->user_data;
        auto cr = mylar->window->raw_window->drawing_context;
        auto dpi = mylar->window->raw_window->dpi;
 
        set_rect(cr, c->real_bounds);
        if (c->state.mouse_pressing) {
            set_argb(cr, {.4, .4, .4, 1});
        } else if (c->state.mouse_hovering) {
            set_argb(cr, {.55, .55, .55, 1});
        } else {
            set_argb(cr, {.7, .7, .7, 1});
        }
        cr->fill();

        if (c->active && c->viakey) {
            auto b = c->real_bounds;
            b.shrink(std::round(1 * dpi));
            set_rect(cr, b);
            set_argb(cr, {.4, .4, .4, 1});
            auto hehe = cr->get_line_width();
            cr->set_line_width(std::round(2 * dpi));
            cr->stroke();
            cr->set_line_width(hehe);
        }
        auto text = get_text();

        auto bounds = draw_text(cr, 0, 0, text, size * dpi, false);
        draw_text(cr, 
            c->real_bounds.x + c->real_bounds.w * .5 - bounds.w * .5,
            c->real_bounds.y + c->real_bounds.h * .5 - bounds.h * .5, text, size * dpi, true, set->font, -1, -1, {0, 0, 0, 1});
    };
    child->pre_layout = [size, get_text](Container* root, Container* c, const Bounds& b) {
        auto mylar = (PinData*)root->user_data;
        auto cr = mylar->window->raw_window->drawing_context;
        auto dpi = mylar->window->raw_window->dpi;
        auto text = get_text();

        auto bounds = draw_text(cr, 0, 0, text, size * dpi, false);
        c->wanted_bounds.w = bounds.w + 50 * dpi;
    };
};

bool desktop_file_exists(std::string wm_class) {
    const char *home = getenv("HOME");
    std::string desktop_path(home);
    desktop_path += "/Desktop/" + wm_class + ".desktop";
    return std::filesystem::exists(desktop_path);
}

static void clicked_create_destroy_desktop_file(std::string wm_class, std::string icon, std::string command) {
    const char *home = getenv("HOME");
    std::string desktop_path(home);
    desktop_path += "/Desktop/" + wm_class + ".desktop";

    try {
        if (std::filesystem::exists(desktop_path)) {
            if (std::filesystem::exists(desktop_path)) {
                std::filesystem::remove(desktop_path);
            }
        } else {
            std::filesystem::path path(desktop_path);
            std::filesystem::create_directories(path.parent_path());
            
            std::ofstream outFile(desktop_path);
            if (!outFile) {
                throw std::ios_base::failure("Failed to open file for writing.");
            }
            
            outFile << "[Desktop Entry]\n";
            outFile << "Icon=" << icon << "\n";
            outFile << "Exec=" << command << "\n";
            outFile << "StartupWMClass=" << wm_class << "\n";
            outFile << "Name=" << wm_class << "\n";
            outFile.close();
        }
    } catch (const std::exception &e) {
    }
}

static RawWindow *pin_window(Container *root) {
    return ((PinData *) root->user_data)->window->raw_window;
}

static std::string strip_nulls(std::string s) {
    s.erase(std::remove(s.begin(), s.end(), '\0'), s.end());
    return s;
}

static void free_icon_options(PinData *data) {
    for (auto &o : data->options)
        if (o.surface)
            cairo_surface_destroy(o.surface);
    data->options.clear();
}

static void close_icon_dropdown(PinData *data) {
    data->dropdown_open = false;
    data->active_option_preferred = false;
}

// Fills the drop-down with icons whose names contain `text`: first the plain names (which follow
// the current theme), then the same names pinned to each other theme that provides them.
static void rebuild_icon_options(Container *root, const std::string &text) {
    auto data = (PinData *) root->user_data;
    auto dpi = pin_window(root)->dpi;
    free_icon_options(data);
    data->active_option = 0;
    data->active_option_preferred = false;
    data->dropdown_scroll = 0;
    data->dropdown_open = false;
    if (text.empty() || !icons_loaded)
        return;

    std::vector<std::string> names;
    get_options(names, text, 200);
    if (names.empty())
        return;

    std::string query = text;
    if (query.size() > 2 && query[0] == ':' && query.find(':', 1) != std::string::npos)
        query = query.substr(query.find(':', 1) + 1);
    query = to_lower(query);
    // Exact matches, then names starting with what was typed, then shortest.
    std::stable_sort(names.begin(), names.end(), [&query](const std::string &a, const std::string &b) {
        auto la = to_lower(a);
        auto lb = to_lower(b);
        bool ea = la == query, eb = lb == query;
        if (ea != eb)
            return ea;
        bool pa = la.starts_with(query), pb = lb.starts_with(query);
        if (pa != pb)
            return pa;
        if (a.size() != b.size())
            return a.size() < b.size();
        return a < b;
    });

    std::vector<IconTarget> targets;
    for (auto &n : names)
        targets.emplace_back(n);
    search_icons(targets);
    pick_best(targets, icon_option_icon_size * dpi);

    std::vector<IconOption> plain;
    std::vector<IconOption> themed;
    std::vector<std::string> seen;
    auto unseen = [&seen](const std::string &key) {
        if (std::find(seen.begin(), seen.end(), key) != seen.end())
            return false;
        seen.push_back(key);
        return true;
    };
    for (auto &t : targets) {
        if (t.candidates.empty())
            continue;
        auto &best = t.candidates[0];
        IconOption o;
        o.name = strip_nulls(best.filename);
        o.theme = strip_nulls(best.theme);
        o.value = o.name;
        o.path = best.full_path();
        // The plain entry already shows this name in its best theme.
        if (unseen(o.value)) {
            unseen(":" + o.theme + ":" + o.name);
            plain.push_back(o);
        }
    }
    for (auto &t : targets) {
        // Candidates are sorted best first, so the first one of each theme is the one to show.
        for (auto &candidate : t.candidates) {
            IconOption o;
            o.name = strip_nulls(candidate.filename);
            o.theme = strip_nulls(candidate.theme);
            if (o.theme.empty())
                continue;
            o.value = ":" + o.theme + ":" + o.name;
            o.path = candidate.full_path();
            if (unseen(o.value))
                themed.push_back(o);
        }
    }
    data->options = std::move(plain);
    data->options.insert(data->options.end(), themed.begin(), themed.end());
    data->dropdown_open = !data->options.empty();
}

static void scroll_to_active_option(PinData *data, double dpi) {
    double row_h = icon_option_row_h * dpi;
    double top = data->active_option * row_h;
    if (top + data->dropdown_scroll < 0)
        data->dropdown_scroll = -top;
    if (top + row_h + data->dropdown_scroll > data->dropdown_view_h)
        data->dropdown_scroll = -(top + row_h - data->dropdown_view_h);
    data->dropdown_scroll = std::clamp(data->dropdown_scroll, -data->dropdown_overflow, 0.0f);
}

// Puts the highlighted suggestion into the icon field. Returns whether one was applied.
static bool apply_active_option(Container *root) {
    auto data = (PinData *) root->user_data;
    bool applied = false;
    if (data->active_option_preferred && data->active_option >= 0 && data->active_option < (int) data->options.size()) {
        if (auto field = container_by_name("icon_container", root)) {
            reset_text_editor(field, data->options[data->active_option].value);
            applied = true;
        }
    }
    close_icon_dropdown(data);
    return applied;
}

// The name the preview shows: the highlighted suggestion while browsing, else the field's text.
static std::string preview_icon_name(Container *root) {
    auto data = (PinData *) root->user_data;
    if (data->dropdown_open && data->active_option_preferred && data->active_option >= 0 && data->active_option < (int) data->options.size())
        return data->options[data->active_option].value;
    if (auto field = container_by_name("icon_container", root))
        return ((LabelData *) field->user_data)->text;
    return data->icon;
}

static void make_icon_preview(Container *content) {
    auto preview = content->child(FILL_SPACE, icon_preview_size);
    preview->pre_layout = [](Container *root, Container *c, const Bounds &b) {
        c->wanted_bounds.h = (icon_preview_size + 12) * pin_window(root)->dpi;
    };
    preview->when_paint = [](Container *root, Container *c) {
        auto data = (PinData *) root->user_data;
        auto window = pin_window(root);
        auto cr = window->drawing_context;
        auto dpi = window->dpi;
        int size = icon_preview_size * dpi;
        auto name = preview_icon_name(root);
        if (!data->preview_loaded || name != data->preview_key || std::abs(data->preview_dpi - dpi) > .001) {
            if (data->preview) {
                cairo_surface_destroy(data->preview);
                data->preview = nullptr;
            }
            data->preview_key = name;
            data->preview_dpi = dpi;
            // Until the icon cache is loaded, keep retrying on later paints.
            data->preview_loaded = icons_loaded;
            if (icons_loaded && !name.empty()) {
                auto path = one_shot_icon(size, {name, to_lower(name), c3ic_fix_wm_class(name)});
                if (!path.empty())
                    load_icon_full_path(&data->preview, path, size);
            }
        }
        double x = std::round(c->real_bounds.x + c->real_bounds.w * .5 - size * .5);
        double y = std::round(c->real_bounds.y);
        if (data->preview) {
            cr->set_source_surface(data->preview, x, y);
            cr->paint_source();
        } else {
            set_argb(cr, {0, 0, 0, .15});
            cr->set_line_width(std::max(1.0, (double) std::floor(dpi)));
            rounded_rect_new(cr, 10 * dpi, x + .5, y + .5, size - 1, size - 1);
            cr->stroke();
        }
    };
}

// Scrollbar geometry of the icon drop-down (track on the right edge of the list).
struct DropdownScrollbar {
    Bounds track;
    Bounds thumb;
};

static DropdownScrollbar dropdown_scrollbar(PinData *data, Bounds dropdown, double dpi) {
    double pad = 4 * dpi;
    Bounds list(dropdown.x + pad, dropdown.y + pad, dropdown.w - pad * 2, dropdown.h - pad * 2);
    DropdownScrollbar bar;
    bar.track = Bounds(list.x + list.w - 10 * dpi, list.y, 10 * dpi, list.h);
    double content = list.h + data->dropdown_overflow;
    double thumb_h = std::max(24.0 * dpi, list.h * list.h / content);
    double ty = list.y + (data->dropdown_overflow > 0 ? (-data->dropdown_scroll / data->dropdown_overflow) * (list.h - thumb_h) : 0);
    bar.thumb = Bounds(bar.track.x, ty, bar.track.w, thumb_h);
    return bar;
}

static void scroll_dropdown_to_thumb_top(PinData *data, Bounds dropdown, double dpi, double thumb_top) {
    auto bar = dropdown_scrollbar(data, dropdown, dpi);
    double range = bar.track.h - bar.thumb.h;
    double t = range > 0 ? std::clamp((thumb_top - bar.track.y) / range, 0.0, 1.0) : 0;
    data->dropdown_smooth.stop();
    data->dropdown_scroll = (float) (-t * data->dropdown_overflow);
}

static void make_icon_dropdown(Container *root) {
    auto dropdown = root->child(FILL_SPACE, FILL_SPACE);
    dropdown->name = "icon_dropdown";
    dropdown->z_index = 10;
    dropdown->exists = false;
    dropdown->when_paint = [](Container *root, Container *c) {
        auto data = (PinData *) root->user_data;
        auto window = pin_window(root);
        auto cr = window->drawing_context;
        auto dpi = window->dpi;
        auto b = c->real_bounds;
        if (data->dropdown_smooth.step(data->dropdown_scroll))
            windowing::redraw(window);
        data->dropdown_scroll = std::clamp(data->dropdown_scroll, -data->dropdown_overflow, 0.0f);

        // Shadow, background and border
        set_argb(cr, {0, 0, 0, .08});
        rounded_rect_new(cr, 7 * dpi, b.x - 1 * dpi, b.y, b.w + 2 * dpi, b.h + 3 * dpi);
        cr->fill();
        set_argb(cr, {1, 1, 1, 1});
        rounded_rect_new(cr, 6 * dpi, b.x, b.y, b.w, b.h);
        cr->fill();
        set_argb(cr, {0, 0, 0, .2});
        cr->set_line_width(std::max(1.0, (double) std::floor(dpi)));
        rounded_rect_new(cr, 6 * dpi, b.x + .5, b.y + .5, b.w - 1, b.h - 1);
        cr->stroke();

        double pad = 4 * dpi;
        Bounds list(b.x + pad, b.y + pad, b.w - pad * 2, b.h - pad * 2);
        cr->save();
        cr->rectangle(list.x, list.y, list.w, list.h);
        cr->clip_path();
        double row_h = icon_option_row_h * dpi;
        int icon_size = icon_option_icon_size * dpi;
        int first = std::max(0, (int) std::floor(-data->dropdown_scroll / row_h));
        for (int i = first; i < (int) data->options.size(); i++) {
            double y = list.y + data->dropdown_scroll + i * row_h;
            if (y > list.y + list.h)
                break;
            auto &o = data->options[i];
            Bounds rb(list.x, y, list.w - (data->dropdown_overflow > 0 ? 10 * dpi : 0), row_h);
            if (i == data->active_option && data->active_option_preferred) {
                set_argb(cr, c->state.mouse_pressing ? RGBA(.23, .6, 1, .3) : RGBA(.23, .6, 1, .16));
                rounded_rect_new(cr, 5 * dpi, rb.x, rb.y, rb.w, rb.h);
                cr->fill();
            }

            // Icons are only loaded once their row is first shown.
            if (!o.attempted) {
                o.attempted = true;
                if (!o.path.empty())
                    load_icon_full_path(&o.surface, o.path, icon_size);
            }
            double icon_x = std::round(rb.x + 8 * dpi);
            double icon_y = std::round(rb.y + rb.h * .5 - icon_size * .5);
            if (o.surface) {
                cr->set_source_surface(o.surface, icon_x, icon_y);
                cr->paint_source();
            }

            std::string theme = o.theme.empty() ? "unthemed" : o.theme;
            auto theme_bounds = draw_text(cr, 0, 0, theme, 10 * dpi, false);
            double theme_x = rb.x + rb.w - 10 * dpi - theme_bounds.w;
            draw_text(cr, theme_x, rb.y + rb.h * .5 - theme_bounds.h * .5, theme, 10 * dpi, true, set->font, -1, -1, {0, 0, 0, .5});

            double name_x = icon_x + icon_size + 10 * dpi;
            auto name_bounds = draw_text(cr, 0, 0, o.name, 12 * dpi, false);
            cr->save();
            cr->rectangle(name_x, rb.y, std::max(0.0, theme_x - 12 * dpi - name_x), rb.h);
            cr->clip_path();
            draw_text(cr, name_x, rb.y + rb.h * .5 - name_bounds.h * .5, o.name, 12 * dpi, true, set->font, -1, -1, {0, 0, 0, .9});
            cr->restore();
        }
        cr->restore();

        if (data->dropdown_overflow > 0) {
            auto bar = dropdown_scrollbar(data, b, dpi);
            bool hot = data->dragging_scrollbar ||
                (c->state.mouse_hovering && bounds_contains(bar.track, root->mouse_current_x, root->mouse_current_y));
            double w = (hot ? 6 : 3) * dpi;
            set_argb(cr, {0, 0, 0, data->dragging_scrollbar ? .45 : hot ? .35 : .25});
            rounded_rect_new(cr, w * .5, bar.thumb.x + bar.thumb.w - w, bar.thumb.y, w, bar.thumb.h);
            cr->fill();
        }
    };
    auto option_under_mouse = [](Container *root, Container *c) {
        auto data = (PinData *) root->user_data;
        auto dpi = pin_window(root)->dpi;
        double list_y = c->real_bounds.y + 4 * dpi;
        int i = (int) std::floor((root->mouse_current_y - list_y - data->dropdown_scroll) / (icon_option_row_h * dpi));
        return i >= 0 && i < (int) data->options.size() ? i : -1;
    };
    // Hovering highlights a suggestion and previews it, like moving through them with the arrow keys.
    dropdown->when_mouse_motion = [option_under_mouse](Container *root, Container *c) {
        auto data = (PinData *) root->user_data;
        if (data->dropdown_overflow > 0 &&
            bounds_contains(dropdown_scrollbar(data, c->real_bounds, pin_window(root)->dpi).track, root->mouse_current_x, root->mouse_current_y)) {
            windowing::redraw(pin_window(root)); // scrollbar hover
            return;
        }
        int i = option_under_mouse(root, c);
        if (i == -1)
            return;
        if (i != data->active_option || !data->active_option_preferred) {
            data->active_option = i;
            data->active_option_preferred = true;
            windowing::redraw(pin_window(root));
        }
    };
    dropdown->when_mouse_enters_container = dropdown->when_mouse_motion;
    dropdown->when_mouse_leaves_container = [](Container *root, Container *c) {
        ((PinData *) root->user_data)->active_option_preferred = false;
        windowing::redraw(pin_window(root));
    };
    dropdown->when_fine_scrolled = [](Container *root, Container *c, double, double scroll_y, bool touchpad) {
        auto data = (PinData *) root->user_data;
        auto dpi = pin_window(root)->dpi;
        float delta = (float) (scroll_y * 3 * dpi);
        if (touchpad) {
            data->dropdown_smooth.stop();
            data->dropdown_scroll = std::clamp(data->dropdown_scroll + delta, -data->dropdown_overflow, 0.0f);
        } else {
            data->dropdown_smooth.scroll_by(data->dropdown_scroll, delta, -data->dropdown_overflow, 0.0f);
        }
        windowing::redraw(pin_window(root));
    };
    // The scrollbar can be grabbed and dragged; pressing the track elsewhere jumps the thumb there.
    auto give_keyboard_back = [](Container *root) {
        if (auto field = container_by_name("icon_container", root)) {
            std::vector<Container *> coll = {field, field->parent};
            set_active(root, coll, root, true, false);
        }
    };
    dropdown->when_mouse_down = [](Container *root, Container *c) {
        auto data = (PinData *) root->user_data;
        auto dpi = pin_window(root)->dpi;
        data->dragging_scrollbar = false;
        if (data->dropdown_overflow <= 0)
            return;
        auto bar = dropdown_scrollbar(data, c->real_bounds, dpi);
        if (!bounds_contains(bar.track, root->mouse_current_x, root->mouse_current_y))
            return;
        if (!bounds_contains(bar.thumb, root->mouse_current_x, root->mouse_current_y)) {
            scroll_dropdown_to_thumb_top(data, c->real_bounds, dpi, root->mouse_current_y - bar.thumb.h * .5);
            bar = dropdown_scrollbar(data, c->real_bounds, dpi);
        }
        data->dragging_scrollbar = true;
        data->scrollbar_grab_offset = root->mouse_current_y - bar.thumb.y;
        windowing::redraw(pin_window(root));
    };
    auto drag_scrollbar = [](Container *root, Container *c) {
        auto data = (PinData *) root->user_data;
        if (!data->dragging_scrollbar)
            return;
        scroll_dropdown_to_thumb_top(data, c->real_bounds, pin_window(root)->dpi, root->mouse_current_y - data->scrollbar_grab_offset);
        windowing::redraw(pin_window(root));
    };
    dropdown->when_drag_end_is_click = false;
    dropdown->when_drag_start = drag_scrollbar;
    dropdown->when_drag = drag_scrollbar;
    auto end_scrollbar_drag = [give_keyboard_back](Container *root, Container *c) {
        auto data = (PinData *) root->user_data;
        if (!data->dragging_scrollbar)
            return;
        data->dragging_scrollbar = false;
        give_keyboard_back(root);
        windowing::redraw(pin_window(root));
    };
    dropdown->when_drag_end = end_scrollbar_drag;
    dropdown->when_mouse_up = end_scrollbar_drag;
    dropdown->when_clicked = [option_under_mouse, give_keyboard_back](Container *root, Container *c) {
        auto data = (PinData *) root->user_data;
        auto dpi = pin_window(root)->dpi;
        if (data->dragging_scrollbar || (data->dropdown_overflow > 0 &&
            bounds_contains(dropdown_scrollbar(data, c->real_bounds, dpi).track, root->mouse_initial_x, root->mouse_initial_y))) {
            data->dragging_scrollbar = false;
            give_keyboard_back(root);
            return;
        }
        int i = option_under_mouse(root, c);
        if (i == -1)
            return;
        data->active_option = i;
        data->active_option_preferred = true;
        apply_active_option(root);
        give_keyboard_back(root);
        windowing::redraw(pin_window(root));
    };
}

// Closes the drop-down once the icon field loses focus, unless focus went to the drop-down itself.
static void icon_field_focus_changed(Container *root, Container *self) {
    auto data = (PinData *) root->user_data;
    auto field = container_by_name("icon_container", root);
    if (!field || field->active || (field->parent && field->parent->active))
        return;
    auto dropdown = container_by_name("icon_dropdown", root);
    if (dropdown && dropdown->exists && bounds_contains(dropdown->real_bounds, root->mouse_current_x, root->mouse_current_y))
        return;
    close_icon_dropdown(data);
}

// Up/Down move through the suggestions, Return/Tab take the highlighted one, Escape closes the list.
static void setup_icon_field_keys(Container *label) {
    auto edit_key = std::move(label->when_key_event);
    label->when_key_event = [edit_key](Container *root, Container *c, int key, bool pressed, xkb_keysym_t sym, int mods, bool is_text, std::string text) {
        auto data = (PinData *) root->user_data;
        bool focused = c->active || (c->parent && c->parent->active);
        if (pressed && focused && data->dropdown_open) {
            auto dpi = pin_window(root)->dpi;
            int max_option = (int) data->options.size() - 1;
            if (sym == XKB_KEY_Escape) {
                close_icon_dropdown(data);
                data->swallow_escape = true;
                return;
            } else if (sym == XKB_KEY_Up) {
                data->active_option = std::clamp(data->active_option - 1, 0, max_option);
                data->active_option_preferred = true;
                scroll_to_active_option(data, dpi);
                return;
            } else if (sym == XKB_KEY_Down) {
                // The first press highlights the first suggestion rather than skipping it.
                if (data->active_option == 0 && !data->active_option_preferred)
                    data->active_option--;
                data->active_option = std::clamp(data->active_option + 1, 0, max_option);
                data->active_option_preferred = true;
                scroll_to_active_option(data, dpi);
                return;
            } else if (sym == XKB_KEY_Return || sym == XKB_KEY_KP_Enter) {
                apply_active_option(root);
                return;
            } else if (sym == XKB_KEY_Tab || sym == XKB_KEY_ISO_Left_Tab) {
                apply_active_option(root);
                // Falls through so Tab still moves to the next field.
            }
        }
        if (edit_key)
            edit_key(root, c, key, pressed, sym, mods, is_text, text);
    };
    label->when_active_status_changed = icon_field_focus_changed;
    label->parent->when_active_status_changed = icon_field_focus_changed;
}

static void fill_root(Container *window_root) {
    window_root->when_paint = paint_bg;
    window_root->when_key_event = [](Container *root, Container* c, int key, bool pressed, xkb_keysym_t sym, int mods, bool is_text, std::string text) {
        auto pin_data = (PinData *) root->user_data;
        if (sym == XKB_KEY_Escape && pin_data->swallow_escape) {
            // Escape only closed the icon suggestions.
            if (pressed)
                pin_data->swallow_escape = false;
            return;
        }
        if (sym == XKB_KEY_Tab) {
            std::vector<Container*> order;
            collect_preorder(root, order);
            for (auto o : order)
                if (o->active)
                    return;
            activate_next_activatable(root, root);
        } else if (sym == XKB_KEY_Escape && pressed) {
            std::vector<Container*> order;
            collect_preorder(root, order);
            for (auto o : order)
                if (o->active) {
                    static std::vector<Container*> coll;
                    coll.push_back(c);
                    if (c->parent)
                        coll.push_back(c->parent);
                    set_active(root, coll, root, false, true);
                    return;
                }
            
            auto mylar = (PinData*)root->user_data;
            windowing::close_window(mylar->window->raw_window);
        }
    };
    // The window holds the form and, above it, the icon suggestions drop-down. The drop-down comes
    // first among the children so it is hit first, and its z_index paints it over the form.
    window_root->type = ::absolute;
    make_icon_dropdown(window_root);
    auto root = window_root->child(::vbox, FILL_SPACE, FILL_SPACE);
    root->name = "edit_pin_form";
    window_root->pre_layout = [](Container *window_root, Container *c, const Bounds &b) {
        auto data = (PinData *) window_root->user_data;
        auto dpi = pin_window(window_root)->dpi;
        auto form = container_by_name("edit_pin_form", window_root);
        auto dropdown = container_by_name("icon_dropdown", window_root);
        ::layout(window_root, form, b);

        auto field = container_by_name("icon_container", window_root);
        dropdown->exists = data->dropdown_open && field;
        if (!dropdown->exists)
            return;
        auto anchor = field->parent->real_bounds;
        double pad = 4 * dpi;
        double y = anchor.y + anchor.h + 2 * dpi;
        double content = data->options.size() * icon_option_row_h * dpi;
        double max_h = std::min(300.0 * dpi, b.y + b.h - y - 10 * dpi);
        double h = std::max(icon_option_row_h * dpi + pad * 2, std::min(content + pad * 2, max_h));
        data->dropdown_view_h = h - pad * 2;
        data->dropdown_overflow = (float) std::max(0.0, content - data->dropdown_view_h);
        data->dropdown_scroll = std::clamp(data->dropdown_scroll, -data->dropdown_overflow, 0.0f);
        dropdown->real_bounds = Bounds(anchor.x, y, anchor.w, h);
        dropdown->wanted_bounds.w = anchor.w;
        dropdown->wanted_bounds.h = h;
    };
    root->wanted_pad = Bounds(30, 30, 30, 30);
    make_icon_preview(root);
    {
        auto label = root->child(FILL_SPACE, FILL_SPACE);
        setup_label(window_root, label, true, false, [](Container* root, Container* c) { return "Icon"; });
    }
    {
        auto label_parent = root->child(FILL_SPACE, FILL_SPACE);
        auto label = setup_label(window_root, label_parent, false, true, [](Container* root, Container* c) { return ((PinData*)root->user_data)->icon; },
            [window_root](std::string text) {
                rebuild_icon_options(window_root, text);
            });
        label->name = "icon_container";
        setup_icon_field_keys(label);
        activate_next_activatable(window_root, window_root);
    }
    {        
        auto label = root->child(FILL_SPACE, FILL_SPACE);
        setup_label(window_root, label, true, false, [](Container* root, Container* c) { return "Terminal Command"; });
    } 
    {
        auto label_parent = root->child(FILL_SPACE, FILL_SPACE);
        auto label = setup_label(window_root, label_parent, false, true, [](Container* root, Container* c) { return ((PinData*)root->user_data)->command; });
        label->name = "command_container";
    }
    {
        auto label = root->child(FILL_SPACE, FILL_SPACE);
        setup_label(window_root, label, true, false, [](Container* root, Container* c) { return "Stacking rule"; });
    }
    {
        auto label_parent = root->child(FILL_SPACE, FILL_SPACE);
        auto label = setup_label(window_root, label_parent, false, true, [](Container* root, Container* c) { return ((PinData*)root->user_data)->stacking_rule; });
        label->name = "stacking_rule_container";
    }

    root->child(FILL_SPACE, FILL_SPACE);
    
    {
        auto parent = root->child(::hbox, FILL_SPACE, 32);
        parent->pre_layout = [](Container* root, Container* c, const Bounds& b) {
            auto mylar = (PinData*)root->user_data;
            auto dpi = mylar->window->raw_window->dpi;
            c->wanted_bounds.h = 32 * dpi;
            c->spacing = 10 * dpi;
        };
        //parent->alignment = ALIGN_RIGHT;
        static bool has_desktop_file = false;
        has_desktop_file = desktop_file_exists(((PinData *) window_root->user_data)->stacking_rule);
        button(parent, []() { return has_desktop_file ? "Remove desktop file" : "Create desktop file"; }, [](Container *root, Container *c) {
            auto stacking_rule_container = container_by_name("stacking_rule_container", root);
            auto stacking_rule_data = (LabelData*)stacking_rule_container->user_data;
            auto command_container = container_by_name("command_container", root);
            auto command_data = (LabelData*)command_container->user_data;
            auto icon_container = container_by_name("icon_container", root);
            auto icon_data = (LabelData*)icon_container->user_data;
 
            clicked_create_destroy_desktop_file(stacking_rule_data->text, icon_data->text, command_data->text);
            has_desktop_file = desktop_file_exists(((PinData *) root->user_data)->stacking_rule);
        });
        parent->child(FILL_SPACE, FILL_SPACE);
        
        button(parent, []() { return "Save & Quit"; }, [](Container *root, Container *c) {
            auto mylar = (PinData*)root->user_data;
            auto stacking_rule_container = container_by_name("stacking_rule_container", root);
            auto stacking_rule_data = (LabelData*)stacking_rule_container->user_data;
            auto command_container = container_by_name("command_container", root);
            auto command_data = (LabelData*)command_container->user_data;
            auto icon_container = container_by_name("icon_container", root);
            auto icon_data = (LabelData*)icon_container->user_data;
            auto pin_data = (PinData*)root->user_data;
            dock::edit_pin(pin_data->original_stacking_rule, stacking_rule_data->text, icon_data->text, command_data->text);
            windowing::close_window(mylar->window->raw_window); 
        });
        button(parent, []() { return "Close"; }, [](Container *root, Container *c) {
            auto mylar = (PinData*)root->user_data;
            windowing::close_window(mylar->window->raw_window);
        });
    }
}

static void start_edit_pin(std::string stacking_rule, std::string icon, std::string command) {
    auto app = windowing::open_app();
    RawWindowSettings settings;
    settings.pos.w = 800;
    settings.pos.h = 600;
    settings.name = "Edit pin";
    auto mylar = open_mylar_window(app, WindowType::NORMAL, settings);
    auto pin_data = new PinData;
    pin_data->original_stacking_rule = stacking_rule;
    pin_data->stacking_rule = stacking_rule;
    pin_data->icon = icon;
    pin_data->command = command;
    pin_data->window = mylar;
    pin_data->app = app;
    mylar->root->user_data = pin_data;
    
    fill_root(mylar->root);
    
    windowing::main_loop(app);
    
    cleanup_cached_pango_fonts();
    drawing::cleanup_thread_resources();
}

void edit_pin::open(std::string stacking_rule, std::string icon, std::string command) {
    std::thread t(start_edit_pin, stacking_rule, icon, command);
    t.detach();
}

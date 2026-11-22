#include "plugin_lifetime.h"
#include "edit_pin.h"
#include "pango_font_cache.h"

#include "container.h"
#include "heart.h"

#include "client/raw_windowing.h"
#include "client/windowing.h"
#include "dock/dock.h"
#include "dock/dock_applications.h"
#include "events.h"
#include "hypriso.h"
#include "icons.h"
#include "screenshot.h"
#include "smooth_scroll.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <cairo.h>
#include <climits>
#include <cmath>
#include <cstring>
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

enum class DropdownKind {
    None,
    Icon,
    Command,
};

// One suggestion in the icon name drop-down.
struct IconOption {
    std::string value; // What goes in the field: "name", or ":theme:name" to pin a theme
    std::string name;
    std::string theme;
    std::string path;
    cairo_surface_t *surface = nullptr;
    bool attempted = false;
};

// One suggestion in the command drop-down ($PATH executable or .desktop file).
struct CommandOption {
    std::string value; // What goes in the field (full path, or "gio launch <path>" for .desktop)
    std::string name;
    std::string full_path;
    ApplicationEntry entry;
    cairo_surface_t *surface = nullptr;
    bool attempted = false;
};

struct PinData : UserData {
    RawApp *app = nullptr;
    MylarWindow *window = nullptr;
    std::shared_ptr<std::atomic<bool>> alive = std::make_shared<std::atomic<bool>>(true);
    std::string original_stacking_rule;
    bool has_desktop_file = false;
    
    std::string stacking_rule;
    std::string icon;
    std::string command;

    // Icon and command suggestions drop-down (only one field's list is open at a time)
    DropdownKind dropdown_kind = DropdownKind::None;
    std::vector<IconOption> options;
    std::vector<CommandOption> command_options;
    std::vector<ApplicationEntry> cached_apps;
    int active_option = 0;
    bool active_option_preferred = false;
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
        for (auto &o : command_options)
            if (o.surface)
                cairo_surface_destroy(o.surface);
        if (preview)
            cairo_surface_destroy(preview);
    }
};

static constexpr int icon_option_row_h = 34;
static constexpr int icon_option_icon_size = 22;
static constexpr int command_option_row_h = 46;
static constexpr int command_option_icon_size = 26;
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
    auto layout = get_cached_pango_font(cr, font, size, PANGO_WEIGHT_NORMAL, false);
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

static void button(Container *root, std::function<std::string()> get_text, std::function<void(Container *, Container *)> on_click,
                   double corner_radius = 0, double horizontal_pad = 50) {
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
    child->when_paint = [size, get_text, corner_radius](Container *root, Container *c) {
        auto mylar = (PinData*)root->user_data;
        auto cr = mylar->window->raw_window->drawing_context;
        auto dpi = mylar->window->raw_window->dpi;
 
        if (corner_radius > 0) {
            rounded_rect_new(cr, corner_radius * dpi, c->real_bounds.x, c->real_bounds.y, c->real_bounds.w, c->real_bounds.h);
        } else {
            set_rect(cr, c->real_bounds);
        }
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
            if (corner_radius > 0) {
                rounded_rect_new(cr, std::max(0.0, corner_radius * dpi - std::round(1 * dpi)), b.x, b.y, b.w, b.h);
            } else {
                set_rect(cr, b);
            }
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
    child->pre_layout = [size, get_text, horizontal_pad](Container* root, Container* c, const Bounds& b) {
        auto mylar = (PinData*)root->user_data;
        auto cr = mylar->window->raw_window->drawing_context;
        auto dpi = mylar->window->raw_window->dpi;
        auto text = get_text();

        auto bounds = draw_text(cr, 0, 0, text, size * dpi, false);
        c->wanted_bounds.w = bounds.w + horizontal_pad * dpi;
    };
};

static std::filesystem::path desktop_file_path(const std::string &rule) {
    const char *home = getenv("HOME");
    if (!home || rule.empty() || rule.find('/') != std::string::npos)
        return {};
    return std::filesystem::path(home) / "Desktop" / (rule + ".desktop");
}

static void update_desktop_file_target(PinData *data, const std::string &rule) {
    data->stacking_rule = rule;
    std::error_code error;
    data->has_desktop_file = std::filesystem::is_regular_file(desktop_file_path(rule), error);
}

static std::string desktop_entry_name(const std::filesystem::path &path) {
    std::ifstream in(path);
    bool desktop_entry = false;
    for (std::string line; std::getline(in, line);) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.starts_with("["))
            desktop_entry = line == "[Desktop Entry]";
        else if (desktop_entry && line.starts_with("Name="))
            return line.substr(5);
    }
    return {};
}

static void clicked_create_destroy_desktop_file(const std::string &wm_class, const std::string &icon,
                                                const std::string &command, const std::string &name) {
    const auto desktop_path = desktop_file_path(wm_class);
    if (desktop_path.empty())
        return;
    try {
        if (std::filesystem::exists(desktop_path)) {
            if (std::filesystem::is_regular_file(desktop_path)) {
                std::filesystem::remove(desktop_path);
            }
        } else {
            std::filesystem::create_directories(desktop_path.parent_path());
            
            std::ofstream outFile(desktop_path);
            if (!outFile) {
                throw std::ios_base::failure("Failed to open file for writing.");
            }
            
            outFile << "[Desktop Entry]\n";
            outFile << "Icon=" << icon << "\n";
            outFile << "Exec=" << command << "\n";
            outFile << "StartupWMClass=" << wm_class << "\n";
            outFile << "Name=" << (name.find_first_not_of(" \t\r\n") == std::string::npos ? wm_class : name) << "\n";
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

static void free_command_options(PinData *data) {
    for (auto &o : data->command_options)
        if (o.surface)
            cairo_surface_destroy(o.surface);
    data->command_options.clear();
}

static void close_dropdown(PinData *data) {
    data->dropdown_kind = DropdownKind::None;
    data->active_option_preferred = false;
}

static bool dropdown_is_open(const PinData *data) {
    return data->dropdown_kind != DropdownKind::None;
}

static int dropdown_option_count(const PinData *data) {
    if (data->dropdown_kind == DropdownKind::Command)
        return (int) data->command_options.size();
    if (data->dropdown_kind == DropdownKind::Icon)
        return (int) data->options.size();
    return 0;
}

static double dropdown_row_height(const PinData *data, double dpi) {
    return (data->dropdown_kind == DropdownKind::Command ? command_option_row_h : icon_option_row_h) * dpi;
}

static const char *dropdown_field_name(DropdownKind kind) {
    return kind == DropdownKind::Command ? "command_container" : "icon_container";
}

static std::string shell_quote_if_needed(const std::string &path) {
    if (path.find_first_of(" \t'\"$`\\&;|<>()*?[]{}!#~") == std::string::npos)
        return path;
    std::string quoted = "'";
    for (char ch : path) {
        if (ch == '\'')
            quoted += "'\\''";
        else
            quoted += ch;
    }
    return quoted + "'";
}

// Fills the drop-down with icons whose names contain `text`: first the plain names (which follow
// the current theme), then the same names pinned to each other theme that provides them.
static void rebuild_icon_options(Container *root, const std::string &text) {
    auto data = (PinData *) root->user_data;
    auto dpi = pin_window(root)->dpi;
    free_icon_options(data);
    data->active_option = 0;
    data->active_option_preferred = false;
    data->dropdown_smooth.stop();
    data->dropdown_scroll = 0;
    data->dropdown_kind = DropdownKind::None;
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
    if (!data->options.empty())
        data->dropdown_kind = DropdownKind::Icon;
}

// Fills the drop-down with matching $PATH scripts and .desktop files (excluding Mylar's settings/builtins).
static void rebuild_command_options(Container *root, const std::string &text) {
    auto data = (PinData *) root->user_data;
    free_command_options(data);
    data->active_option = 0;
    data->active_option_preferred = false;
    data->dropdown_smooth.stop();
    data->dropdown_scroll = 0;
    data->dropdown_kind = DropdownKind::None;

    std::string query = text;
    auto ltrim = [](std::string &s) {
        size_t pos = s.find_first_not_of(" \t");
        if (pos == std::string::npos)
            s.clear();
        else if (pos > 0)
            s.erase(0, pos);
    };
    ltrim(query);
    if (query.starts_with("gio launch ")) {
        query.erase(0, strlen("gio launch "));
        ltrim(query);
    } else if (query.starts_with("xdg-open ")) {
        query.erase(0, strlen("xdg-open "));
        ltrim(query);
    }
    if (!query.empty() && (query.front() == '\'' || query.front() == '"')) {
        char q = query.front();
        query.erase(0, 1);
        if (!query.empty() && query.back() == q)
            query.pop_back();
    }
    if (query.empty() || data->cached_apps.empty())
        return;

    const std::string lowercase_query = to_lower(query);
    const bool has_slash = query.find('/') != std::string::npos;

    struct RankedEntry {
        int level;
        const ApplicationEntry *entry;
    };
    std::vector<RankedEntry> matches;
    matches.reserve(64);
    for (const auto &entry : data->cached_apps) {
        int level = match_application(entry, query, lowercase_query);
        if (level < 0 && has_slash) {
            if (entry.full_path == query) {
                level = 0;
            } else {
                auto lower_path = to_lower(entry.full_path);
                if (lower_path == lowercase_query)
                    level = 1;
                else if (entry.full_path.starts_with(query))
                    level = 2;
                else if (lower_path.starts_with(lowercase_query))
                    level = 3;
                else if (entry.full_path.find(query) != std::string::npos)
                    level = 6;
                else if (lower_path.find(lowercase_query) != std::string::npos)
                    level = 7;
            }
        }
        if (level >= 0)
            matches.push_back({level, &entry});
    }
    if (matches.empty())
        return;

    std::stable_sort(matches.begin(), matches.end(), [](const RankedEntry &a, const RankedEntry &b) {
        return a.level < b.level;
    });

    static constexpr size_t max_command_options = 200;
    if (matches.size() > max_command_options)
        matches.resize(max_command_options);

    data->command_options.reserve(matches.size());
    for (const auto &m : matches) {
        CommandOption o;
        o.entry = *m.entry;
        o.name = m.entry->name;
        o.full_path = m.entry->full_path;
        auto quoted_path = shell_quote_if_needed(m.entry->full_path);
        o.value = m.entry->is_desktop ? ("gio launch " + quoted_path) : quoted_path;
        data->command_options.push_back(std::move(o));
    }
    if (!data->command_options.empty())
        data->dropdown_kind = DropdownKind::Command;
}

static void watch_cached_apps(Container *window_root) {
    auto data = (PinData *) window_root->user_data;
    std::weak_ptr<bool> root_lifetime = window_root->lifetime;
    windowing::timer(data->app, 50, [window_root, root_lifetime](void *) {
        if (root_lifetime.expired())
            return;
        auto data = (PinData *) window_root->user_data;
        bool scan_in_progress = false;
        const bool was_empty = data->cached_apps.empty();
        auto entries = get_application_entries(false, &scan_in_progress);
        if (!entries.empty())
            data->cached_apps = std::move(entries);
        if (scan_in_progress) {
            watch_cached_apps(window_root);
            return;
        }
        if (auto field = container_by_name("command_container", window_root)) {
            bool focused = field->active || (field->parent && field->parent->active);
            const auto &text = ((LabelData *) field->user_data)->text;
            if (focused && !text.empty() && (data->dropdown_kind == DropdownKind::Command || was_empty)) {
                rebuild_command_options(window_root, text);
                windowing::redraw(pin_window(window_root));
            }
        }
    }, nullptr);
}

static void scroll_to_active_option(PinData *data, double dpi) {
    double row_h = dropdown_row_height(data, dpi);
    double top = data->active_option * row_h;
    if (top + data->dropdown_scroll < 0)
        data->dropdown_scroll = -top;
    if (top + row_h + data->dropdown_scroll > data->dropdown_view_h)
        data->dropdown_scroll = -(top + row_h - data->dropdown_view_h);
    data->dropdown_scroll = std::clamp(data->dropdown_scroll, -data->dropdown_overflow, 0.0f);
}

// Puts the highlighted suggestion into the active field. Returns whether one was applied.
static bool apply_active_option(Container *root) {
    auto data = (PinData *) root->user_data;
    bool applied = false;
    int count = dropdown_option_count(data);
    if (data->active_option_preferred && data->active_option >= 0 && data->active_option < count) {
        auto field = container_by_name(dropdown_field_name(data->dropdown_kind), root);
        if (field) {
            const std::string &val = data->dropdown_kind == DropdownKind::Command
                ? data->command_options[data->active_option].value
                : data->options[data->active_option].value;
            reset_text_editor(field, val);
            applied = true;
        }
    }
    close_dropdown(data);
    return applied;
}

// The name the preview shows: the highlighted suggestion while browsing icons, else the field's text.
static std::string preview_icon_name(Container *root) {
    auto data = (PinData *) root->user_data;
    if (data->dropdown_kind == DropdownKind::Icon && data->active_option_preferred && data->active_option >= 0 && data->active_option < (int) data->options.size())
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

static void make_suggestions_dropdown(Container *root) {
    auto dropdown = root->child(FILL_SPACE, FILL_SPACE);
    dropdown->name = "suggestions_dropdown";
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
        double row_h = dropdown_row_height(data, dpi);
        int count = dropdown_option_count(data);
        int first = std::max(0, (int) std::floor(-data->dropdown_scroll / row_h));
        if (data->dropdown_kind == DropdownKind::Icon) {
            int icon_size = icon_option_icon_size * dpi;
            for (int i = first; i < count; i++) {
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
        } else if (data->dropdown_kind == DropdownKind::Command) {
            int icon_size = command_option_icon_size * dpi;
            for (int i = first; i < count; i++) {
                double y = list.y + data->dropdown_scroll + i * row_h;
                if (y > list.y + list.h)
                    break;
                auto &o = data->command_options[i];
                Bounds rb(list.x, y, list.w - (data->dropdown_overflow > 0 ? 10 * dpi : 0), row_h);
                if (i == data->active_option && data->active_option_preferred) {
                    set_argb(cr, c->state.mouse_pressing ? RGBA(.23, .6, 1, .3) : RGBA(.23, .6, 1, .16));
                    rounded_rect_new(cr, 5 * dpi, rb.x, rb.y, rb.w, rb.h);
                    cr->fill();
                }

                if (!o.attempted && icons_loaded) {
                    o.attempted = true;
                    auto path = resolve_application_icon(o.entry, icon_size);
                    if (!path.empty())
                        load_icon_full_path(&o.surface, path, icon_size);
                }
                double icon_x = std::round(rb.x + 8 * dpi);
                double icon_y = std::round(rb.y + rb.h * .5 - icon_size * .5);
                if (o.surface) {
                    cr->set_source_surface(o.surface, icon_x, icon_y);
                    cr->paint_source();
                }

                double text_x = icon_x + icon_size + 10 * dpi;
                double text_w = std::max(0.0, rb.x + rb.w - 10 * dpi - text_x);
                auto name_bounds = draw_text(cr, 0, 0, o.name, 12 * dpi, false);
                auto path_bounds = draw_text(cr, 0, 0, o.full_path, 10 * dpi, false);
                double total_h = name_bounds.h + path_bounds.h;
                double text_y = rb.y + (rb.h - total_h) * .5;
                cr->save();
                cr->rectangle(text_x, rb.y, text_w, rb.h);
                cr->clip_path();
                draw_text(cr, text_x, text_y, o.name, 12 * dpi, true, set->font, -1, -1, {0, 0, 0, .9});
                draw_text(cr, text_x, text_y + name_bounds.h, o.full_path, 10 * dpi, true, set->font, -1, -1, {0, 0, 0, .5});
                cr->restore();
            }
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
        double row_h = dropdown_row_height(data, dpi);
        int i = (int) std::floor((root->mouse_current_y - list_y - data->dropdown_scroll) / row_h);
        int count = dropdown_option_count(data);
        return i >= 0 && i < count ? i : -1;
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
    auto give_keyboard_back = [](Container *root, DropdownKind kind) {
        if (kind == DropdownKind::None)
            return;
        if (auto field = container_by_name(dropdown_field_name(kind), root)) {
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
        give_keyboard_back(root, data->dropdown_kind);
        windowing::redraw(pin_window(root));
    };
    dropdown->when_drag_end = end_scrollbar_drag;
    dropdown->when_mouse_up = end_scrollbar_drag;
    dropdown->when_clicked = [option_under_mouse, give_keyboard_back](Container *root, Container *c) {
        auto data = (PinData *) root->user_data;
        auto dpi = pin_window(root)->dpi;
        DropdownKind kind = data->dropdown_kind;
        if (data->dragging_scrollbar || (data->dropdown_overflow > 0 &&
            bounds_contains(dropdown_scrollbar(data, c->real_bounds, dpi).track, root->mouse_initial_x, root->mouse_initial_y))) {
            data->dragging_scrollbar = false;
            give_keyboard_back(root, kind);
            return;
        }
        int i = option_under_mouse(root, c);
        if (i == -1)
            return;
        data->active_option = i;
        data->active_option_preferred = true;
        apply_active_option(root);
        give_keyboard_back(root, kind);
        windowing::redraw(pin_window(root));
    };
}

// Closes the drop-down once its owning field loses focus, unless focus went to the drop-down itself.
static void dropdown_field_focus_changed(Container *root, Container *) {
    auto data = (PinData *) root->user_data;
    if (!dropdown_is_open(data))
        return;
    auto field = container_by_name(dropdown_field_name(data->dropdown_kind), root);
    if (!field || field->active || (field->parent && field->parent->active))
        return;
    auto dropdown = container_by_name("suggestions_dropdown", root);
    if (dropdown && dropdown->exists && bounds_contains(dropdown->real_bounds, root->mouse_current_x, root->mouse_current_y))
        return;
    close_dropdown(data);
}

// Up/Down move through the suggestions, Return/Tab take the highlighted one, Escape closes the list.
static void setup_dropdown_field_keys(Container *label, DropdownKind kind) {
    auto edit_key = std::move(label->when_key_event);
    label->when_key_event = [edit_key, kind](Container *root, Container *c, int key, bool pressed, xkb_keysym_t sym, int mods, bool is_text, std::string text) {
        auto data = (PinData *) root->user_data;
        bool focused = c->active || (c->parent && c->parent->active);
        if (pressed && focused && data->dropdown_kind == kind) {
            auto dpi = pin_window(root)->dpi;
            int max_option = dropdown_option_count(data) - 1;
            if (sym == XKB_KEY_Escape) {
                close_dropdown(data);
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
    label->when_active_status_changed = dropdown_field_focus_changed;
    label->parent->when_active_status_changed = dropdown_field_focus_changed;
}

static void fill_root(Container *window_root) {
    {
        auto data = (PinData *) window_root->user_data;
        bool scan_in_progress = false;
        data->cached_apps = get_application_entries(true, &scan_in_progress);
        if (scan_in_progress)
            watch_cached_apps(window_root);
    }

    window_root->when_paint = paint_bg;
    window_root->when_key_event = [](Container *root, Container* c, int key, bool pressed, xkb_keysym_t sym, int mods, bool is_text, std::string text) {
        auto pin_data = (PinData *) root->user_data;
        if (sym == XKB_KEY_Escape && pin_data->swallow_escape) {
            // Escape only closed the suggestions drop-down.
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
    // The window holds the form and, above it, the suggestions drop-down. The drop-down comes
    // first among the children so it is hit first, and its z_index paints it over the form.
    window_root->type = ::absolute;
    make_suggestions_dropdown(window_root);
    auto root = window_root->child(::vbox, FILL_SPACE, FILL_SPACE);
    root->name = "edit_pin_form";
    window_root->pre_layout = [](Container *window_root, Container *c, const Bounds &b) {
        auto data = (PinData *) window_root->user_data;
        auto dpi = pin_window(window_root)->dpi;
        auto form = container_by_name("edit_pin_form", window_root);
        auto dropdown = container_by_name("suggestions_dropdown", window_root);
        ::layout(window_root, form, b);

        auto field = dropdown_is_open(data) ? container_by_name(dropdown_field_name(data->dropdown_kind), window_root) : nullptr;
        dropdown->exists = dropdown_is_open(data) && field;
        if (!dropdown->exists)
            return;
        auto anchor = field->parent->real_bounds;
        double pad = 4 * dpi;
        double y = anchor.y + anchor.h + 2 * dpi;
        double row_h = dropdown_row_height(data, dpi);
        double content = dropdown_option_count(data) * row_h;
        double max_h = std::min(300.0 * dpi, b.y + b.h - y - 10 * dpi);
        double h = std::max(row_h + pad * 2, std::min(content + pad * 2, max_h));
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
        setup_dropdown_field_keys(label, DropdownKind::Icon);
        activate_next_activatable(window_root, window_root);
    }
    {        
        auto label = root->child(FILL_SPACE, FILL_SPACE);
        setup_label(window_root, label, true, false, [](Container* root, Container* c) { return "Terminal Command"; });
    } 
    {
        auto label_parent = root->child(FILL_SPACE, FILL_SPACE);
        auto label = setup_label(window_root, label_parent, false, true, [](Container* root, Container* c) { return ((PinData*)root->user_data)->command; },
            [window_root](std::string text) {
                rebuild_command_options(window_root, text);
            });
        label->name = "command_container";
        setup_dropdown_field_keys(label, DropdownKind::Command);
    }
    {
        auto label = root->child(FILL_SPACE, FILL_SPACE);
        setup_label(window_root, label, true, false, [](Container* root, Container* c) { return "Stacking rule"; });
    }
    {
        auto row = root->child(::hbox, FILL_SPACE, FILL_SPACE);
        row->pre_layout = [](Container *root, Container *c, const Bounds &b) {
            auto dpi = static_cast<PinData *>(root->user_data)->window->raw_window->dpi;
            auto label_parent = c->children[0];
            label_parent->pre_layout(root, label_parent, b);
            c->wanted_bounds.w = FILL_SPACE;
            c->wanted_bounds.h = label_parent->wanted_bounds.h;
            c->spacing = 8 * dpi;
        };
        auto label_parent = row->child(FILL_SPACE, FILL_SPACE);
        auto label = setup_label(window_root, label_parent, false, true, [](Container* root, Container* c) { return ((PinData*)root->user_data)->stacking_rule; },
            [window_root](std::string rule) { update_desktop_file_target((PinData *) window_root->user_data, rule); });
        label->name = "stacking_rule_container";
        button(row, []() { return "Click window"; }, [](Container *root, Container *c) {
            auto data = static_cast<PinData *>(root->user_data);
            auto app = data->app;
            auto alive = data->alive;
            main_thread([app, root, alive]() {
                if (!*alive)
                    return;
                screenshot_tool::pick_window([app, root, alive](int cid) {
                    if (!*alive)
                        return;
                    std::string cls = hypriso->class_name(cid);
                    std::string rule = c3ic_fix_wm_class(cls);
                    if (rule.empty())
                        rule = cls;
                    if (rule.empty())
                        return;
                    windowing::run_on_app(app, [root, alive, rule = std::move(rule)]() {
                        if (!*alive)
                            return;
                        auto data = static_cast<PinData *>(root->user_data);
                        {
                            std::lock_guard<std::mutex> lock(data->app->mutex);
                            if (auto field = container_by_name("stacking_rule_container", root))
                                reset_text_editor(field, rule);
                            update_desktop_file_target(data, rule);
                        }
                        windowing::redraw(data->window->raw_window);
                    });
                });
            });
        }, 6, 28);
    }

    root->child(FILL_SPACE, FILL_SPACE);
    {
        auto caption = root->child(FILL_SPACE, FILL_SPACE);
        setup_label(window_root, caption, true, false, [](Container *, Container *) { return "Desktop file name"; });
        auto row = root->child(FILL_SPACE, FILL_SPACE);
        auto field = setup_label(window_root, row, false, true, [](Container *root, Container *) {
            auto *data = static_cast<PinData *>(root->user_data);
            return desktop_entry_name(desktop_file_path(data->stacking_rule));
        });
        field->name = "desktop_name_container";
        field->when_paint = [](Container *root, Container *c) {
            auto *editor = static_cast<STextEditorData *>(c->user_data);
            if (editor->text.empty()) {
                auto *data = static_cast<PinData *>(root->user_data);
                auto window = data->window->raw_window;
                auto cr = window->drawing_context;
                cr->save();
                set_rect(cr, c->real_bounds);
                cr->clip_path();
                draw_text(cr, c->real_bounds.x, c->real_bounds.y, data->stacking_rule,
                          editor->options.font_size * window->dpi, true, set->font, -1, -1, {0, 0, 0, .3});
                cr->restore();
            }
            paint_text_editor(root, c, RGBA(0, 0, 0, 1), RGBA(.2, .5, .8, 1));
        };
    }

    auto button_gap = root->child(FILL_SPACE, 10);
    button_gap->pre_layout = [](Container *root, Container *c, const Bounds &) {
        c->wanted_bounds.h = 10 * pin_window(root)->dpi;
    };
    {
        auto parent = root->child(::hbox, FILL_SPACE, 32);
        parent->pre_layout = [](Container* root, Container* c, const Bounds& b) {
            auto mylar = (PinData*)root->user_data;
            auto dpi = mylar->window->raw_window->dpi;
            c->wanted_bounds.h = 32 * dpi;
            c->spacing = 10 * dpi;
        };
        auto data = static_cast<PinData *>(window_root->user_data);
        update_desktop_file_target(data, data->stacking_rule);
        button(parent, [data]() { return data->has_desktop_file ? "Remove desktop file" : "Create desktop file"; }, [](Container *root, Container *c) {
            auto stacking_rule_container = container_by_name("stacking_rule_container", root);
            auto stacking_rule_data = (LabelData*)stacking_rule_container->user_data;
            auto command_container = container_by_name("command_container", root);
            auto command_data = (LabelData*)command_container->user_data;
            auto icon_container = container_by_name("icon_container", root);
            auto icon_data = (LabelData*)icon_container->user_data;
 
            auto name_field = container_by_name("desktop_name_container", root);
            auto name_data = static_cast<STextEditorData *>(name_field->user_data);
            clicked_create_destroy_desktop_file(stacking_rule_data->text, icon_data->text, command_data->text, name_data->text);
            update_desktop_file_target(static_cast<PinData *>(root->user_data), stacking_rule_data->text);
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

struct EditPinSession {
    std::atomic<bool> exited = false;
    std::thread thread;
};

static std::atomic<bool> edit_pin_stopping = false;
static std::mutex edit_pin_sessions_mutex;
static std::vector<std::shared_ptr<EditPinSession>> edit_pin_sessions;

static void reap_edit_pin_sessions_locked() {
    for (int i = (int) edit_pin_sessions.size() - 1; i >= 0; i--) {
        if (edit_pin_sessions[i]->exited.load()) {
            if (edit_pin_sessions[i]->thread.joinable())
                edit_pin_sessions[i]->thread.join();
            edit_pin_sessions.erase(edit_pin_sessions.begin() + i);
        }
    }
}

static void start_edit_pin(std::shared_ptr<EditPinSession> session, std::string stacking_rule, std::string icon, std::string command) {
    auto app = windowing::open_app();
    if (!app) {
        session->exited = true;
        return;
    }
    RawWindowSettings settings;
    settings.pos.w = 800;
    settings.pos.h = 600;
    settings.name = "Edit pin";
    settings.app_id = edit_pin::kAppId;
    auto mylar = open_mylar_window(app, WindowType::NORMAL, settings);
    if (!mylar) {
        windowing::close_app(app);
        windowing::main_loop(app);
        delete app;
        session->exited = true;
        return;
    }
    auto pin_data = new PinData;
    pin_data->original_stacking_rule = stacking_rule;
    pin_data->stacking_rule = stacking_rule;
    pin_data->icon = icon;
    pin_data->command = command;
    pin_data->window = mylar;
    pin_data->app = app;
    mylar->root->user_data = pin_data;
    auto alive = pin_data->alive;
    mylar->root->on_closed = [alive](Container *) {
        *alive = false;
    };
    
    fill_root(mylar->root);
    
    windowing::main_loop(app);
    *alive = false;
    
    cleanup_cached_pango_fonts();
    drawing::cleanup_thread_resources();
    session->exited = true;
}

void edit_pin::open(std::string stacking_rule, std::string icon, std::string command) {
    std::lock_guard<std::mutex> lock(edit_pin_sessions_mutex);
    reap_edit_pin_sessions_locked();
    if (edit_pin_stopping.load() || plugin_lifetime.stop_requested())
        return;
    auto session = std::make_shared<EditPinSession>();
    session->thread = std::thread(start_edit_pin, session, std::move(stacking_rule), std::move(icon), std::move(command));
    edit_pin_sessions.push_back(std::move(session));
}

void edit_pin::stop() {
    std::vector<std::shared_ptr<EditPinSession>> sessions;
    {
        std::lock_guard<std::mutex> lock(edit_pin_sessions_mutex);
        edit_pin_stopping = true;
        sessions.swap(edit_pin_sessions);
    }
    for (auto &session : sessions) {
        if (session->thread.joinable())
            session->thread.join();
    }
    edit_pin_stopping = false;
}

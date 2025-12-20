#include "dock.h"

#include "second.h"

#include "client/raw_windowing.h"
#include "client/windowing.h"
#include "process.hpp"
#include "hypriso.h"
#include "icons.h"
#include "popup.h"

#include <cairo.h>
#include "process.hpp"
#include <chrono>
#include <cmath>
#include <functional>
#include <pango/pango-layout.h>
#include <pango/pango-types.h>
#include <sys/wait.h>
#include <thread>
#include <memory>
#include <pango/pangocairo.h>

#define BTN_LEFT		0x110
#define BTN_RIGHT		0x111
#define BTN_MIDDLE		0x112
#define ICON(str) [](){ return str; }

static bool merge_windows = false;
static int pixel_spacing = 1;
static float max_width = 230;

class Window {
public:
    int cid; // unique id
    bool attempted_load = false;
    bool scale_change = false;
    
    std::string icon;
    std::string command;
    std::string title;
    std::string stack_rule; // Should be regex later, or multiple 

    cairo_surface_t* icon_surf = nullptr; 
    
    ~Window() {
        if (icon_surf)
            cairo_surface_destroy(icon_surf);
    }
    
    Window() {}

    Window(const Window& w) {
        cid = w.cid;
        attempted_load = w.attempted_load;
        scale_change = w.scale_change;
        icon = w.icon;
        command = w.command;
        title = w.title;
        stack_rule = w.stack_rule;
        icon_surf = nullptr;
    };
};

struct Pin : UserData {
    std::vector<Window> windows;

    std::string icon;
    std::string command;
    std::string stacking_rule;

    bool pinned = false;
    int natural_position_x = INT_MAX;
    int old_natural_position_x = INT_MAX;
    int initial_mouse_click_before_drag_offset_x = 0;
};

struct Windows {
    std::mutex mut;
    
    //std::vector<Pin *> pins;
    std::vector<Window *> list;

    std::vector<Window *> to_be_added;
    std::vector<int> to_be_removed;
};

struct Dock : UserData {
    RawApp *app = nullptr;
    MylarWindow *window = nullptr;
    Windows *collection = nullptr;
    bool first_fill = true;
    RawWindowSettings creation_settings;
    //bool vertical = false;
};

static std::vector<Dock *> docks;

struct CachedFont {
    std::string name;
    int size;
    int used_count;
    bool italic = false;
    PangoWeight weight;
    PangoLayout *layout;
    cairo_t *cr; // Creator
    
    ~CachedFont() { g_object_unref(layout); }
};

static std::vector<CachedFont *> cached_fonts;
static int active_cid = -1;

float get_icon_size(float dpi) {
    return 28 * dpi;
}

static float battery_level = 100;
static bool charging = true;
static float volume_level = 100;
static float brightness_level = 100;
static bool finished = false;
static bool nightlight_on = false;

struct BatteryData : UserData {
    float brightness_level = 100;
};

struct VolumeData : UserData {

};

struct BrightnessData : UserData {
    float value = 50;
};

PangoLayout *
get_cached_pango_font(cairo_t *cr, std::string name, int pixel_height, PangoWeight weight, bool italic) {
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif
    // Look for a matching font in the cache (including italic style)
    for (int i = cached_fonts.size() - 1; i >= 0; i--) {
        auto font = cached_fonts[i];
        if (font->name == name &&
            font->size == pixel_height &&
            font->weight == weight &&
            font->cr == cr &&
            font->italic == italic) { // New italic check
            pango_layout_set_attributes(font->layout, nullptr);
            font->used_count++;
            if (font->used_count < 512) {
//            printf("returned: %p\n", font->layout);
            	return font->layout;
            } else {
				delete font;
				cached_fonts.erase(cached_fonts.begin() + i);
            }
        }
    }

    // Create a new CachedFont entry
    auto *font = new CachedFont;
    assert(font);
    font->name = name;
    font->size = pixel_height;
    font->weight = weight;
    font->cr = cr;
    font->italic = italic; // Save the italic setting
    font->used_count = 0;

    PangoLayout *layout = pango_cairo_create_layout(cr);
    PangoFontDescription *desc = pango_font_description_new();

    pango_font_description_set_size(desc, pixel_height * PANGO_SCALE);
    pango_font_description_set_family_static(desc, name.c_str());
    pango_font_description_set_weight(desc, weight);
    // Set the style to italic or normal based on the parameter
    pango_font_description_set_style(desc, italic ? PANGO_STYLE_ITALIC : PANGO_STYLE_NORMAL);

    pango_layout_set_font_description(layout, desc);
    pango_font_description_free(desc);
    pango_layout_set_attributes(layout, nullptr);

    assert(layout);

    font->layout = layout;
    //printf("new: %p\n", font->layout);

    cached_fonts.push_back(font);

    assert(font->layout);

    return font->layout;
}

void cleanup_cached_fonts() {
    for (auto font: cached_fonts) {
        delete font;
    }
    cached_fonts.clear();
    cached_fonts.shrink_to_fit();
}

void remove_cached_fonts(cairo_t *cr) {
    for (int i = cached_fonts.size() - 1; i >= 0; --i) {
        if (cached_fonts[i]->cr == cr) {
            delete cached_fonts[i];
            cached_fonts.erase(cached_fonts.begin() + i);
        }
    }
}

static RGBA color_dock_color() {
    static RGBA default_color("00000088");
    return hypriso->get_varcolor("plugin:mylardesktop:dock_color", default_color);
}

static RGBA color_dock_sel_active_color() {
    static RGBA default_color("ffffff44");
    return hypriso->get_varcolor("plugin:mylardesktop:dock_sel_active_color", default_color);
}

static RGBA color_dock_sel_hover_color() {
    static RGBA default_color("ffffff44");
    return hypriso->get_varcolor("plugin:mylardesktop:dock_sel_hover_color", default_color);
}

static RGBA color_dock_sel_press_color() {
    static RGBA default_color("ffffff44");
    return hypriso->get_varcolor("plugin:mylardesktop:dock_sel_press_color", default_color);
}

static RGBA color_dock_sel_accent_color() {
    static RGBA default_color("ffffff44");
    return hypriso->get_varcolor("plugin:mylardesktop:dock_sel_accent_color", default_color);
}

static void paint_root(Container *root, Container *c) {
    auto dock = (Dock *) root->user_data;
    auto mylar = dock->window;
    auto cr = mylar->raw_window->cr;
    cairo_save(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
    cairo_paint(cr);
    cairo_restore(cr);

    //cairo_rectangle(cr, root->real_bounds.x, root->real_bounds.y, root->real_bounds.w, std::round(1 * mylar->raw_window->dpi));
    //cairo_set_source_rgba(cr, 1, 1, 1, .1);
    //cairo_fill(cr);

    cairo_rectangle(cr, root->real_bounds.x, root->real_bounds.y, root->real_bounds.w, root->real_bounds.h);
    auto color = color_dock_color();
    cairo_set_source_rgba(cr, color.r, color.g, color.b, color.a);
    cairo_fill(cr);
}

Bounds draw_text(cairo_t *cr, int x, int y, std::string text, int size = 10, bool draw = true, std::string font = mylar_font, int wrap = -1, int h = -1) {
    auto layout = get_cached_pango_font(cr, mylar_font, size, PANGO_WEIGHT_NORMAL, false);
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
    cairo_set_source_rgba(cr, 1, 1, 1, 1);
    PangoRectangle ink;
    PangoRectangle logical;
    pango_layout_get_pixel_extents(layout, &ink, &logical);
    if (draw) {
        cairo_move_to(cr, x, y);
        pango_cairo_show_layout(cr, layout);
    }
    return Bounds(ink.width, ink.height, logical.width, logical.height);
}

Bounds draw_text(cairo_t *cr, Container *c, std::string text, int size = 10, bool draw = true, std::string font = mylar_font, int wrap = -1, int h = -1) {
    auto layout = get_cached_pango_font(cr, mylar_font, size, PANGO_WEIGHT_NORMAL, false);
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
    cairo_set_source_rgba(cr, 1, 1, 1, 1);
    PangoRectangle ink;
    PangoRectangle logical;
    pango_layout_get_pixel_extents(layout, &ink, &logical);
    if (draw) {
        cairo_move_to(cr, center_x(c, logical.width), center_y(c, logical.height));
        pango_cairo_show_layout(cr, layout);
    }
    return Bounds(ink.width, ink.height, logical.width, logical.height);
}

static void paint_button_bg(Container *root, Container *c) {
    auto dock = (Dock *) root->user_data;
    auto mylar = dock->window;
    auto cr = mylar->raw_window->cr;
    if (c->state.mouse_pressing) {
        cairo_rectangle(cr, c->real_bounds.x, c->real_bounds.y, c->real_bounds.w, c->real_bounds.h);
        cairo_set_source_rgba(cr, 1, 1, 1, .2);
        cairo_fill(cr);
    } else if (c->state.mouse_hovering) {
        cairo_rectangle(cr, c->real_bounds.x, c->real_bounds.y, c->real_bounds.w, c->real_bounds.h);
        cairo_set_source_rgba(cr, 1, 1, 1, .3);
        cairo_fill(cr);
    }
}

static std::string get_date() {
    auto now = std::chrono::system_clock::now();
    auto local = std::chrono::current_zone()->to_local(now);

    std::string s = std::format("{:%I:%M %p — %A}\n{:%B %m/%d/%Y}", local, local);

    return s;
}

static float get_brightness() {
    float current = 50;
    float max = 100;
    {
        auto process = std::make_shared<TinyProcessLib::Process>("brightnessctl max", "", [&max](const char *bytes, size_t n) {
            std::string text(bytes, n);
            try {
                max = std::atoi(text.c_str());
            } catch (...) {

            }
        });
    }
    {
        auto process = std::make_shared<TinyProcessLib::Process>("brightnessctl get", "", [&current](const char *bytes, size_t n) {
            std::string text(bytes, n);
            try {
                current = std::atoi(text.c_str());
            } catch (...) {

            }
        });
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    return (current / max) * 100;
}

static bool battery_charging() {
    bool value = false;
    auto process = std::make_shared<TinyProcessLib::Process>("upower -i /org/freedesktop/UPower/devices/DisplayDevice | grep state", "", [&value](const char *bytes, size_t n) {
        std::string text(bytes, n);
        if (text.find("discharging") == std::string::npos) {
            value = true;
        } else {
            value = false;
        }
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    return value;
}

static float get_battery_level() {
    int value = 50;
    auto process = std::make_shared<TinyProcessLib::Process>("upower -i /org/freedesktop/UPower/devices/DisplayDevice | grep percentage | rg --only-matching '[0-9]*' | xargs", "", [&value](const char *bytes, size_t n) {
        std::string text(bytes, n);
        try {
            value = std::atoi(text.c_str());
        } catch (...) {

        }
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    return value;
}

static bool watching_battery = false;

static void watch_battery_level() {
    if (watching_battery)
        return;
    watching_battery = true;
    auto process = std::make_shared<TinyProcessLib::Process>("upower --monitor", "", [](const char *bytes, size_t n) {
        battery_level = get_battery_level();
        charging = battery_charging();
        for (auto d : docks)
            windowing::redraw(d->window->raw_window);
    });
    std::thread t([process]() {
        while (!finished) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        watching_battery = false;
        process->kill();
    });
    t.detach();
}

static long last_time_volume_adjusted = 0;

static float get_volume_level() {
    int value = 50;
    auto process = std::make_shared<TinyProcessLib::Process>("pactl list sinks | grep '^[[:space:]]Volume:' | head -n $(( $SINK + 1 )) | tail -n 1 | sed -e 's,.* \\([0-9][0-9]*\\)%.*,\\1,'", "", [&value](const char *bytes, size_t n) {
        std::string text(bytes, n);
        try {
            value = std::atoi(text.c_str());
        } catch (...) {

        }
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    return value;
}

static bool watching_volume = false;

static void watch_volume_level() {
    if (watching_volume)
        return;
    watching_volume = true;
    auto process = std::make_shared<TinyProcessLib::Process>("pactl subscribe", "", [](const char *bytes, size_t n) {
        long current = get_current_time_in_ms();
        std::string text(bytes, n);
        bool contains = text.find("change") != std::string::npos;
        if (current - last_time_volume_adjusted > 400 && contains) {
            volume_level = get_volume_level();
            for (auto d : docks)
                windowing::redraw(d->window->raw_window);
        }
    });
    std::thread t([process]() {
        while (!finished) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        watching_volume = false;
        process->kill();
    });
    t.detach();
}

static void set_brightness(float amount) {
    brightness_level = amount;
    static bool queued = false;
    static bool latest = amount;
    latest = amount;
    if (queued)
        return;
    queued = true;
    std::thread t([amount]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        auto process = std::make_shared<TinyProcessLib::Process>(fz("brightnessctl set {}", (int) std::round(amount)));

        for (auto d : docks)
            windowing::redraw(d->window->raw_window);

        queued = false;
    });
    t.detach();
}

static void set_volume(float amount) {
    static bool queued = false;
    static float latest = amount;
    latest = amount;
    if (queued)
        return;
    queued = true;
    std::thread t([]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        auto process = std::make_shared<TinyProcessLib::Process>(fz("pactl set-sink-volume @DEFAULT_SINK@ {}%", (int) std::round(latest)));

        for (auto d : docks)
            windowing::redraw(d->window->raw_window);

        queued = false;
    });
    t.detach();
}

Container *simple_dock_item(Container *root, std::function<std::string()> ico, std::function<std::string()> text = nullptr) {
    auto dock = (Dock *) root->user_data;
    auto c = root->child(40, FILL_SPACE);
    /*if (dock->vertical) {
        c->type = ::vbox;
        c->wanted_bounds = Bounds(0, 0, FILL_SPACE, 40);
    } else {
        c->type = ::hbox;
        c->wanted_bounds = Bounds(0, 0, 40, FILL_SPACE);
    }*/
    static int tex_size = 8;
    c->when_paint = [ico, text](Container *root, Container *c) {
        auto dock = (Dock *) root->user_data;
        auto mylar = dock->window;
        auto cr = mylar->raw_window->cr;
        paint_button_bg(root, c);

        auto ico_bounds = draw_text(cr, c, ico(), 12 * mylar->raw_window->dpi, false, "Segoe Fluent Icons");
        auto b = draw_text(cr,
            c->real_bounds.x + 10, c->real_bounds.y + c->real_bounds.h * .5 - ico_bounds.h * .5,
            ico(), 12 * mylar->raw_window->dpi, true, "Segoe Fluent Icons");
        if (text) {
            auto tb = draw_text(cr, c, text(), tex_size * mylar->raw_window->dpi, false);
            draw_text(cr,
                c->real_bounds.x + 20 + b.w, c->real_bounds.y + c->real_bounds.h * .5 - tb.h * .5,
                text(), tex_size * mylar->raw_window->dpi, true);
        }
    };
    c->pre_layout = [ico, text](Container *root, Container *c, const Bounds &b) {
        auto dock = (Dock *) root->user_data;
        auto mylar = dock->window;
        auto cr = mylar->raw_window->cr;
        auto bounds = draw_text(cr, c, ico(), 12 * mylar->raw_window->dpi, false, "Segoe Fluent Icons");
        if (text)
            bounds.w += draw_text(cr, c, text(), tex_size * mylar->raw_window->dpi, false).w + 10;
        
        c->wanted_bounds.w = bounds.w + 20;
    };

    return c;
}

static void create_pinned_icon(Container *icons, Window *window) {
    auto ch = icons->child(::absolute, FILL_SPACE, FILL_SPACE);
    auto pin = new Pin;
    pin->stacking_rule = window->stack_rule;
    pin->command = window->command;
    pin->icon = window->icon;
    pin->windows.push_back(*window);
    ch->user_data = pin;
    ch->when_drag_end_is_click = false;
    ch->when_paint = paint {        
        auto dock = (Dock*)root->user_data;
        if (!dock || !dock->window || !dock->window->raw_window || !dock->window->raw_window->cr)
            return;

        Pin* pin = (Pin*)c->user_data;
        auto mylar = dock->window;
        auto cr = mylar->raw_window->cr;
        
        bool is_active = false;
        for (auto window : pin->windows)
            if (window.cid == active_cid)
                is_active = true;

        if (c->state.mouse_pressing) {
            cairo_rectangle(cr, c->real_bounds.x, c->real_bounds.y, c->real_bounds.w, c->real_bounds.h);
            auto col = color_dock_sel_press_color();
            cairo_set_source_rgba(cr, col.r, col.g, col.b, col.a);
            cairo_fill(cr);
        } else if (c->state.mouse_hovering) {
            cairo_rectangle(cr, c->real_bounds.x, c->real_bounds.y, c->real_bounds.w, c->real_bounds.h);
            auto col = color_dock_sel_hover_color();
            cairo_set_source_rgba(cr, col.r, col.g, col.b, col.a);
            cairo_fill(cr);
        } else if (is_active) {
            cairo_rectangle(cr, c->real_bounds.x, c->real_bounds.y, c->real_bounds.w, c->real_bounds.h);
            auto col = color_dock_sel_active_color();
            cairo_set_source_rgba(cr, col.r, col.g, col.b, col.a);
            cairo_fill(cr);
        }

        int offx = 0;
        if (icons_loaded) {
            for (auto &w : pin->windows) {
                if (w.icon_surf) {
                    auto h = cairo_image_surface_get_height(w.icon_surf);
                    cairo_set_source_surface(cr, w.icon_surf, c->real_bounds.x + 10, c->real_bounds.y + c->real_bounds.h * .5 - h * .5);
                    cairo_paint(cr);
                    auto wi = cairo_image_surface_get_height(w.icon_surf);
                    offx = wi + 10;
                    break;
                }
            }
        }

        std::string title = pin->stacking_rule;
        if (!pin->windows.empty())
            title = pin->windows[0].title;
        auto text_w = (c->real_bounds.w - offx - 20) * PANGO_SCALE;
        auto b = draw_text(cr, c, title, 9 * mylar->raw_window->dpi, false, mylar_font, text_w, c->real_bounds.h * PANGO_SCALE);
        draw_text(cr, c->real_bounds.x + 10 + offx, c->real_bounds.y + c->real_bounds.h * .5 - b.h * .5, title, 9 * mylar->raw_window->dpi, true, mylar_font, text_w, c->real_bounds.h * PANGO_SCALE);

        if (is_active) {
            auto bar_h = std::round(2 * mylar->raw_window->dpi);
            cairo_rectangle(cr, c->real_bounds.x, c->real_bounds.y + c->real_bounds.h - bar_h, c->real_bounds.w, bar_h);
            auto col = color_dock_sel_accent_color();
            cairo_set_source_rgba(cr, col.r, col.g, col.b, col.a);
            cairo_fill(cr);
        }
    };
    ch->when_clicked = paint {
        Pin* pin = (Pin*)c->user_data;
        auto dock = (Dock*)root->user_data;
        auto mylar = dock->window;
        auto cr = mylar->raw_window->cr;

        if (pin->windows.empty()) {
            notify("launch command");
            return;
        }

        int cid = pin->windows[0].cid;
        
        if (c->state.mouse_button_pressed == BTN_LEFT) {
            main_thread([cid] {
                // todo we need to focus next (already wrote this combine code)
                bool is_hidden = hypriso->is_hidden(cid);
                if (is_hidden) {
                    hypriso->set_hidden(cid, false);
                    hypriso->bring_to_front(cid);
                } else {
                    if (active_cid == cid) {
                        hypriso->set_hidden(cid, true);
                    } else {
                        hypriso->bring_to_front(cid);
                    }
                }
            });
        } else if (c->state.mouse_button_pressed == BTN_RIGHT) {
            int startoff = (root->mouse_current_x - c->real_bounds.x) / mylar->raw_window->dpi;
            int cw = c->real_bounds.w / mylar->raw_window->dpi;
            main_thread([cid, startoff, cw] {
                auto m = mouse();
                std::vector<PopOption> root;
                {
                    PopOption pop;
                    pop.text = "Launch task";
                    pop.on_clicked = []() {};
                    root.push_back(pop);
                }
                {
                    PopOption pop;
                    pop.text = "Pin/unpin";
                    pop.on_clicked = []() {};
                    root.push_back(pop);
                }
                {
                    PopOption pop;
                    pop.text = "Edit pin";
                    pop.on_clicked = []() {};
                    root.push_back(pop);
                }
                {
                    PopOption pop;
                    pop.text = "End task";
                    pop.on_clicked = []() {};
                    root.push_back(pop);
                }
                {
                    PopOption pop;
                    pop.text = "Close window";
                    pop.on_clicked = [cid]() { close_window(cid); };
                    root.push_back(pop);
                }

                popup::open(root, m.x - startoff + cw * .5 - (277 * .5) + 1.4, m.y);
                //popup::open(root, m.x - (277 * .5), m.y);
            });
        }
    };
    ch->pre_layout = [](Container* root, Container* c, const Bounds& b) {
        Pin* pin = (Pin*)c->user_data;
        auto dock = (Dock*)root->user_data;
        auto mylar = dock->window;
        auto cr = mylar->raw_window->cr;

        std::string title = pin->stacking_rule;
        if (!pin->windows.empty())
            title = pin->windows[0].title;
        if (icons_loaded) {
            for (auto &w : pin->windows) {
                if (!w.attempted_load || w.scale_change) {
                    w.scale_change = false;
                    w.attempted_load = true;
                    auto size = get_icon_size(mylar->raw_window->dpi);
                    auto full = one_shot_icon(size, {w.icon, to_lower(w.icon), c3ic_fix_wm_class(w.icon), to_lower(w.icon)});
                    if (!full.empty()) {
                        load_icon_full_path(&w.icon_surf, full, size);
                    }
                }
            }
        }
    };
    ch->when_drag_start = paint {
        auto data = (Pin *) c->user_data;
        data->initial_mouse_click_before_drag_offset_x = c->real_bounds.x - root->mouse_initial_x;
    };
}

static void merge_list_into_icons(Dock *dock, Container *icons) {
    // merging: dock->collection->list -> icons->children

    // also title needs to be updated or scale change
    for (auto pin_container : icons->children) {
        auto pin = (Pin *) pin_container->user_data;
        for (auto &win : pin->windows) {
            for (auto l : dock->collection->list) {
                if (l->cid == win.cid) {
                    win.title = l->title;
                    win.scale_change = l->scale_change;
                    l->scale_change = false;
                }
            }
        }
    }
 
    // remove windows that no longer exist, and remove pin itself if it would be left empty (unless it's pinned, and last one if !merge_windows)
    for (int pin_index = icons->children.size() - 1; pin_index >= 0; pin_index--) {
        auto pin_container = icons->children[pin_index];
        auto pin = (Pin *) pin_container->user_data;
        
        // remove those windows in pin that no longer exist
        for (int window_index = pin->windows.size() -1; window_index >= 0; window_index--) {
            auto window = pin->windows[window_index];
            bool window_still_exists = false;
            for (auto existing_window : dock->collection->list) {
                if (existing_window->cid == window.cid) {
                    window_still_exists = true;
                }
            }

            if (!window_still_exists) {
                pin->windows.erase(pin->windows.begin() + window_index);
            }
        }

        // If pin is empty remove it if not pinned
        if (pin->windows.empty()) {

            if (!pin->pinned) {
                delete pin_container;
                icons->children.erase(icons->children.begin() + pin_index);
            } else if (!merge_windows) {
                // in the case where we are not merging windows, and the stacking rule is pinned, we need to remove it, unless it's the last
                int count = 0;
                for (auto pc : icons->children) {
                    auto pcdata = (Pin *) pc->user_data;
                    if (pcdata->stacking_rule == pin->stacking_rule) {
                        count++;
                    }
                }
                if (count > 1) {
                    delete pin_container;
                    icons->children.erase(icons->children.begin() + pin_index);
                }
            }
        }
    }

    // merging: dock->collection->list -> icons->children
    //

    // based on list, add to groups already existing (and merge true), or create the pin container
    for (auto window : dock->collection->list) {
        bool window_needs_to_be_added = true;
        for (auto pin_container : icons->children) {
            auto pin = (Pin *) pin_container->user_data;
            for (auto pin_window : pin->windows) {
                if (pin_window.cid == window->cid) {
                    window_needs_to_be_added = false; // already added
                }
            }
        }

        if (window_needs_to_be_added) {
            if (merge_windows) {
                // finds it's group and add it
                bool was_able_to_group = false;
                for (auto pin_container : icons->children) {
                    auto pin = (Pin *) pin_container->user_data;
                    if (pin->stacking_rule == window->stack_rule) {
                        was_able_to_group = true;
                        pin->windows.push_back(*window);
                        break;
                    }
                }
                if (was_able_to_group)
                    return;
            }

            create_pinned_icon(icons, window);
        }
    }
}

static void merge_to_be_into_list(Dock *dock, Container *c) {
    std::lock_guard<std::mutex> guard(dock->collection->mut);
    if (!dock->collection->to_be_removed.empty()) {
        for (auto t : dock->collection->to_be_removed) {
            for (int i = dock->collection->list.size() - 1; i >= 0; i--) {
                auto win = dock->collection->list[i];
                if (win->cid == t) {
                    dock->collection->list.erase(dock->collection->list.begin() + i);
                }
            }
        }

        dock->collection->to_be_removed.clear();
    }

    if (!dock->collection->to_be_added.empty()) {
        for (auto w : dock->collection->to_be_added) {
            bool added = false;
            for (auto window : dock->collection->list) {
                if (window->cid == w->cid) {
                    added = true;
                }
            }
            if (!added) {
                dock->collection->list.push_back(w);
            }
        }
        dock->collection->to_be_added.clear();
    }
}

/*

static int
calc_largest(Container *icons) {
    int largest = 0;
    for (auto c: icons->children)
        if (c->real_bounds.w > largest)
            largest = c->real_bounds.w;
    return largest;
}

static int
size_icons(AppClient *client, cairo_t *cr, Container *icons) {
    int total_width = 0;
    for (auto c: icons->children) {
        auto w = get_label_width(client, c);
        c->real_bounds.w = w;
        c->real_bounds.h = client->bounds->h;
        c->real_bounds.y = 0;
    }
    
    for (auto c: icons->children) {
        total_width += c->real_bounds.w;
    }
    
    // For pixel spacing between pinned icons
    int count = icons->children.size();
    if (count != 0)
        count--;
    total_width += count * pixel_spacing;
    
    if (total_width > icons->real_bounds.w) {
        auto overflow = total_width - icons->real_bounds.w;
        
        for (int i = 0; i < overflow; i++) {
            int largest = calc_largest(icons);
            
            for (auto c: icons->children) {
                if ((int) c->real_bounds.w == largest) {
                    c->real_bounds.w -= 1;
                    break;
                }
            }
        }
        total_width = icons->real_bounds.w;
    }
    
    return total_width;
}

static void
swap_icon(Container *icons, Container *dragging, Container *other, bool before) {
    // TODO: it's not a swap it's an insert after, or before
    for (int i = 0; i < icons->children.size(); i++) {
        if (icons->children[i] == dragging) {
            icons->children.erase(icons->children.begin() + i);
            break;
        }
    }
    for (int i = 0; i < icons->children.size(); i++) {
        if (icons->children[i] == other) {
            if (before) {
                icons->children.insert(icons->children.begin() + i, dragging);
            } else {
                icons->children.insert(icons->children.begin() + i + 1, dragging);
            }
            break;
        }
    }
}

static void
position_icons(AppClient *client, cairo_t *cr, Container *icons) {
    auto total_width = size_icons(client, cr, icons);
}
*/

int
would_be_x(Container *icons, Container *target, int pos_x) {
    for (int i = 0; i < icons->children.size(); i++) {
        if (icons->children[i] == target) {
            return pos_x;
        }
        pos_x += icons->children[i]->real_bounds.w;
    }
    return pos_x;
}



static int
calc_largest(Container *icons) {
    int largest = 0;
    for (auto c: icons->children)
        if (c->real_bounds.w > largest)
            largest = c->real_bounds.w;
    return largest;
}

static int
size_icons(Dock *dock, Container *icons) {
    int total_width = 0;
    for (auto c: icons->children) {
        //auto w = get_label_width(client, c);
        c->real_bounds.w = max_width * dock->window->raw_window->dpi;
        c->real_bounds.h = icons->real_bounds.h;
        c->real_bounds.y = 0;
    }
    
    for (auto c: icons->children) {
        total_width += c->real_bounds.w;
    }
    
    // For pixel spacing between pinned icons
    int count = icons->children.size();
    if (count != 0)
        count--;
    total_width += count * pixel_spacing;
    
    if (total_width > icons->real_bounds.w) {
        auto overflow = total_width - icons->real_bounds.w;
        
        for (int i = 0; i < overflow; i++) {
            int largest = calc_largest(icons);
            
            for (auto c: icons->children) {
                if ((int) c->real_bounds.w == largest) {
                    c->real_bounds.w -= 1;
                    break;
                }
            }
        }
        total_width = icons->real_bounds.w;
    }
    
    return total_width;
}


static void layout_icons(Container *root, Container *icons, Dock *dock) {
    float total_width = size_icons(dock, icons);
    
    auto align = container_alignment::ALIGN_LEFT; // todo: pull from setting
    
    int off = icons->real_bounds.x;
    if (align == container_alignment::ALIGN_RIGHT) {
        off += icons->real_bounds.w - total_width;
    } else if (align == container_alignment::ALIGN_GLOBAL_CENTER_HORIZONTALLY) {
        auto mid_point = root->real_bounds.w / 2;
        auto left_x = mid_point - (total_width / 2);
        auto right_x = left_x + total_width;
        auto min = icons->real_bounds.x;
        auto max = icons->real_bounds.x + icons->real_bounds.w;
        if (right_x > max) {
            left_x -= right_x - max;
            right_x = left_x + total_width;
        }
        if (left_x < min) {
            left_x += min - left_x;
            right_x = left_x + total_width;
        }
        off = left_x;
    } else if (align == container_alignment::ALIGN_CENTER_HORIZONTALLY) {
        off += (icons->real_bounds.w - total_width) / 2;
    }

    for (auto c : icons->children) {
        auto data = (Pin *) c->user_data;
        if (data->natural_position_x == INT_MAX) {
            data->old_natural_position_x = off;
        } else {
            data->old_natural_position_x = data->natural_position_x;
        }
        data->natural_position_x = off;
        off += c->real_bounds.w + pixel_spacing;
    }
    Container *dragging = nullptr;
    int drag_index = 0;
    
    // Position dragged icon based on current mouse position, and prevent it from leaving icons container
    for (auto c: icons->children) {
        auto data = (Pin *) c->user_data;
        if (c->state.mouse_dragging) {
            dragging = c;
            //auto diff = root->mouse_initial_x - root->mouse_current_x;
            //c->real_bounds.x -= diff;
            auto x = root->mouse_current_x + data->initial_mouse_click_before_drag_offset_x;
            c->real_bounds.x = x;
            if (c->real_bounds.x < icons->real_bounds.x) {
                c->real_bounds.x = icons->real_bounds.x;
            }
            if (c->real_bounds.x + c->real_bounds.w > icons->real_bounds.x + icons->real_bounds.w) {
                c->real_bounds.x = icons->real_bounds.x + icons->real_bounds.w - c->real_bounds.w;
            }
            break;
        }
        drag_index++;
    }

    if (dragging) {
        int distance = 100000;
        int index = 0;
        int w_b = 0;
        auto natural_x = ((Pin *) icons->children[0]->user_data)->natural_position_x;
        icons->children.erase(icons->children.begin() + drag_index);
        for (int i = 0; i < icons->children.size() + 1; i++) {
            icons->children.insert(icons->children.begin() + i, dragging);
            auto would_be = would_be_x(icons, dragging, natural_x);
            auto dist = std::abs(dragging->real_bounds.x - would_be);
            if (dist < distance) {
                distance = dist;
                index = i;
                w_b = would_be;
            }
            icons->children.erase(icons->children.begin() + i);
        }
        icons->children.insert(icons->children.begin() + index, dragging);
        auto *data = (Pin *) dragging->user_data;
        data->old_natural_position_x = dragging->real_bounds.x;
        data->natural_position_x = dragging->real_bounds.x;
    }
    
    for (auto c: icons->children) {
        if (c->state.mouse_dragging) continue;
        auto *data = (Pin *) c->user_data;
        c->real_bounds.x = data->natural_position_x;
        continue;
    }
    
    for (auto c: icons->children) {
        if (c->pre_layout) {
            c->pre_layout(root, c, c->real_bounds);
        }
    }
    
    
    // Start animating, if not already
    /*
    bool running = ((TaskbarData *) client->user_data)->spring_animating;
    for (auto c: icons->children) {
        auto *data = (LaunchableButton *) c->user_data;
        if (data->animating && !running) {
            running = true;
            client_register_animation(app, client);
            ((TaskbarData *) client->user_data)->spring_animating = true;
            return;
        }
    }
    if (running) {
        ((TaskbarData *) client->user_data)->spring_animating = false;
        client_unregister_animation(app, client);
        running = false;
    }    
    */
    
    /*
    // Calculate 'slot' the icon is closest to and swap into it

    
    // Queue spring animations

    */
    
    /*
    icons->should_layout_children = true;
    defer(icons->should_layout_children = false);
    ::layout(root, icons, icons->real_bounds);
    
    for (auto c : icons->children) {
       if (c->state.mouse_dragging) {
           auto diff = root->mouse_initial_x - root->mouse_current_x;
           c->real_bounds.x -= diff;
       }
    }
    */
    
}

static void fill_root(Container *root) {
    root->when_paint = paint_root;
    auto dock = (Dock *) root->user_data;
    {
        auto super = simple_dock_item(root, ICON("\uF4A5"), ICON("Applications"));
        super->when_clicked = paint {
            system("wofi --show run &");
        };
    }

    {
        auto icons = root->child(FILL_SPACE, FILL_SPACE);
        icons->type = ::hbox;
        icons->should_layout_children = false;

        //icons->distribute_overflow_to_children = true;
        icons->name = "icons";
        // todo it's not prelayout because that gets called by parent, we just staright up want to layout
        icons->pre_layout = [](Container *root, Container *c, const Bounds &b) {
            auto dock = (Dock *) root->user_data;
            if (dock->first_fill) {
                dock->first_fill = false;
                main_thread([dock]() {
                    for (auto w : get_window_stacking_order())
                        dock::add_window(w);
                });
            }

            merge_to_be_into_list(dock, c);
            
            merge_list_into_icons(dock, c);
        };

        icons->when_paint = paint {
            auto dock = (Dock *) root->user_data;
            layout_icons(root, c, dock);
        };
    }

    {
        auto active_settings = simple_dock_item(root, ICON("\uE9E9"));
        active_settings->when_clicked = paint {
            system("hyprctl dispatch plugin:mylar:right_click_active");
        };
    }

    {
        auto toggle = simple_dock_item(root, ICON("\uF0E2"));
        toggle->when_clicked = paint {
            system("hyprctl dispatch plugin:mylar:toggle_layout");
        };
    }

    {
        auto night = simple_dock_item(root, ICON("\uE708"));
        night->when_clicked = paint {
           if (nightlight_on)  {
               system("killall hyprsunset");
           } else {
               std::thread t([]() {
                   system("hyprsunset -t 5000");
               });
               t.detach();
           }
           nightlight_on = !nightlight_on;
        };
    }

    {
        auto bluetooth = simple_dock_item(root, ICON("\uE702"));
    }

    {
        auto wifi = simple_dock_item(root, ICON("\uE701"));
    }

    {
        auto brightness = simple_dock_item(root, ICON("\uE706"), []() {
           return fz("{}%", (int) std::round(brightness_level));
        });
        auto brightness_data = new BrightnessData;
        std::thread t([&brightness_data]() {
            brightness_level = get_brightness();
        });
        t.detach();

        brightness->when_fine_scrolled = [](Container* root, Container* c, int scroll_x, int scroll_y, bool came_from_touchpad) {
            auto dock = (Dock *) root->user_data;
            auto mylar = dock->window;
            auto brightness_data = (BrightnessData *) c->user_data;
            brightness_data->value += ((double) scroll_y) * .001;
            if (brightness_data->value > 100) {
               brightness_data->value = 100;
            }
            if (brightness_data->value < 0) {
               brightness_data->value = 0;
            }
            set_brightness(brightness_data->value);
        };

        brightness->user_data = brightness_data;
    }

    {
        auto volume = simple_dock_item(root, []() {
            std::string text = "";
            auto val = volume_level;
            bool mute_state = volume_level < 1;
            if (mute_state) {
                text = "\uE74F";
            } else if (val == 0) {
                text = "\uE992";
            } else if (val < 33) {
                text = "\uE993";
            } else if (val < 66) {
                text = "\uE994";
            } else {
                text = "\uE995";
            }
           return text;
        }, []() {
           return std::format("{}%", (int) volume_level);
        }) ;
        auto volume_data = new VolumeData;
        std::thread t([]() {
            volume_level = get_volume_level();
        });
        t.detach();
        watch_volume_level();
        volume->when_fine_scrolled = [](Container* root, Container* c, int scroll_x, int scroll_y, bool came_from_touchpad) {
            auto dock = (Dock *) root->user_data;
            auto mylar = dock->window;
            auto volume_data = (VolumeData *) c->user_data;
            volume_level += ((double) scroll_y) * .001;
            if (volume_level > 100) {
               volume_level = 100;
            }
            if (volume_level < 0) {
               volume_level = 0;
            }
            last_time_volume_adjusted = get_current_time_in_ms();
            set_volume(volume_level);
        };
        volume->user_data = volume_data;
    }

    {
        auto battery = simple_dock_item(root, []() {
            static std::string regular[] = {"\uEBA0", "\uEBA1", "\uEBA2", "\uEBA3", "\uEBA4", "\uEBA5", "\uEBA6", "\uEBA7", "\uEBA8", "\uEBA9", "\uEBAA" };
            static std::string charging[] = { "\uEBAB", "\uEBAC", "\uEBAD", "\uEBAE", "\uEBAF", "\uEBB0", "\uEBB1", "\uEBB2", "\uEBB3", "\uEBB4", "\uEBB5" };
            int capacity_index = std::floor(((double) (battery_level)) / 10.0);
            return regular[capacity_index];
        }, []() {
            std::string charging_text = charging ? "+" : "-";
            return std::format("{}{}%", charging_text, (int) std::round(battery_level));
        }) ;
        auto battery_data = new BatteryData;
        std::thread t([]() {
            battery_level = get_battery_level();
        });
        t.detach();
        charging = battery_charging();
        watch_battery_level();
        battery->user_data = battery_data;
    }

    {
        auto date = root->child(40, FILL_SPACE);
        date->when_paint = paint {
            auto dock = (Dock *) root->user_data;
            auto mylar = dock->window;
            auto cr = mylar->raw_window->cr;
            paint_button_bg(root, c);
            draw_text(cr, c, get_date(), 9 * mylar->raw_window->dpi);
        };
        date->pre_layout = [](Container *root, Container *c, const Bounds &b) {
            auto dock = (Dock *) root->user_data;
            auto mylar = dock->window;
            auto cr = mylar->raw_window->cr;
            auto bounds = draw_text(cr, c, get_date(), 9 * mylar->raw_window->dpi, false);
            c->wanted_bounds.w = bounds.w + 20;
        };
    }
};

static int current_alignment = 3;

void dock_start(std::string monitor_name) {
    if (!monitor_name.empty()) {
        for (auto d : docks) {
            if (d->creation_settings.monitor_name == monitor_name) {
                return; // already created that dock
            }
        }
    }
    auto dock = new Dock;
    dock->collection = new Windows;
    dock->app = windowing::open_app();
    dock->app->print_monitors();
    RawWindowSettings settings;
    settings.pos.w = 0;
    settings.pos.h = 40;
    settings.name = "Dock";
    settings.alignment = current_alignment;
    settings.monitor_name = monitor_name;
    dock->creation_settings = settings;
    dock->window = open_mylar_window(dock->app, WindowType::DOCK, settings);
    dock->window->raw_window->on_scale_change = [dock](RawWindow *rw, float dpi) {
        for (auto window : dock->collection->list) {
            window->scale_change = true;
        }
        //notify("scale change");
    };
    dock->window->root->skip_delete = true;
    dock->window->root->user_data = dock;
    fill_root(dock->window->root);
    dock->window->root->alignment = ALIGN_RIGHT;
    docks.push_back(dock);
    windowing::main_loop(dock->app);
    if (docks.size() == 1)
        finished = true; 
    for (int i = docks.size() - 1; i >= 0; i--) {
        if (docks[i] == dock) {
            docks.erase(docks.begin() + i);
        }
    }

    // Cleanup dock
    delete dock->app;
    delete dock->window;
    delete dock;
}

static int get_dock_alignment() {
    return hypriso->get_varint("plugin:mylardesktop:dock", 3);
}

void dock::start(std::string monitor_name) {
    if (monitor_name == "FALLBACK")
        return;
    current_alignment = get_dock_alignment();

    finished = false;
    //return;
    std::thread t(dock_start, monitor_name);
    t.detach();
}

void dock::stop(std::string monitor_name) {
    if (monitor_name.empty()) {
        finished = true;
        for (auto d : docks) {
            windowing::close_app(d->app);
        }
        docks.clear();
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        cleanup_cached_fonts();   
    } else {
        for (auto d : docks) {
            if (d->creation_settings.monitor_name == monitor_name) {
                windowing::close_app(d->app);
            }
        }
    } 
}

// This happens on the main thread, not the dock thread
void dock::add_window(int cid) {
    for (auto d : docks) {
        // Check if cid should even be displayed in dock
        if (!hypriso->alt_tabbable(cid))
            return;

        Window *window = new Window;
        window->cid = cid;
        window->title = hypriso->title_name(cid);
        window->stack_rule = hypriso->class_name(cid);
        window->icon = hypriso->class_name(cid);
        //auto size = get_icon_size();
        //auto fullpath = one_shot_icon(size, {window->icon});
        //notify(window->icon);
        //if (!fullpath.empty()) {
            //load_icon_full_path(&window->icon_surf, fullpath, size);
        //}
        window->command = "vlc";
        if (hypriso->has_focus(cid))
            active_cid = cid;

        // Synchronize
        std::lock_guard<std::mutex> guard(d->collection->mut);
        d->collection->to_be_added.push_back(window);
        windowing::redraw(d->window->raw_window);
    }
}

// This happens on the main thread, not the dock thread
void dock::remove_window(int cid) {
    for (auto d : docks) {
        std::lock_guard<std::mutex> guard(d->collection->mut);
        d->collection->to_be_removed.push_back(cid);
        windowing::redraw(d->window->raw_window);
    }
}

void dock::title_change(int cid, std::string title) {
    for (auto d : docks) {
        std::lock_guard<std::mutex> guard(d->collection->mut);
        for (auto window : d->collection->list) {
            if (window->cid == cid) {
                window->title = title;
            }
        }
        windowing::redraw(d->window->raw_window);
    }
}

void dock::on_activated(int cid) {
    active_cid = cid;
    for (auto d : docks)
        windowing::redraw(d->window->raw_window);
}

void dock::redraw() {
    for (auto d : docks)
        windowing::redraw(d->window->raw_window);
}

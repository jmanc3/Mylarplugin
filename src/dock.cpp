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

struct Dock : UserData {
    RawApp *app = nullptr;
    MylarWindow *window = nullptr;
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

class Window {
public:
    int cid; // unique id
    std::string icon;
    std::string command;
    std::string title;
    std::string stack_rule; // Should be regex later, or multiple 
    
    cairo_surface_t* icon_surf = nullptr; 
    bool attempted_load = false;

    bool scale_change = false;
};

struct Windows {
    std::mutex mut;

    std::vector<Window *> list;

    std::vector<Window *> to_be_added;
    std::vector<int> to_be_removed;
};

//static Windows *windows = new Windows;

static float battery_level = 100;
static bool charging = true;
static float volume_level = 100;
static float brightness_level = 100;
static bool finished = false;
static bool nightlight_on = false;

//static RawApp *dock_app = nullptr;
//static MylarWindow *mylar_window = nullptr;

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

static void paint_root(Container *root, Container *c) {
    auto dock = (Dock *) root->user_data;
    auto mylar = dock->window;
    auto cr = mylar->raw_window->cr;
    cairo_save(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
    cairo_paint(cr);
    cairo_restore(cr);

    cairo_rectangle(cr, root->real_bounds.x, root->real_bounds.y, root->real_bounds.w, std::round(1 * mylar->raw_window->dpi));
    cairo_set_source_rgba(cr, 1, 1, 1, .1);
    cairo_fill(cr);

    cairo_rectangle(cr, root->real_bounds.x, root->real_bounds.y, root->real_bounds.w, root->real_bounds.h);
    cairo_set_source_rgba(cr, 0, 0, 0, .5);
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
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
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
    auto c = root->child(40, FILL_SPACE);
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

static void fill_root(Container *root) {
    root->when_paint = paint_root;

    /*{
        auto icons = root->child(::hbox, FILL_SPACE, FILL_SPACE);
        icons->distribute_overflow_to_children = true;
        icons->name = "icons";
        icons->pre_layout = [](Container *root, Container *c, const Bounds &b) {
            {
                std::lock_guard<std::mutex> guard(windows->mut);
                if (!windows->to_be_removed.empty()) {
                    for (auto t : windows->to_be_removed) {
                        for (int i = windows->list.size() - 1; i >= 0; i--) {
                            if (windows->list[i]->cid == t) {
                                windows->list.erase(windows->list.begin() + i);
                            }
                        }
                    }
                    windows->to_be_removed.clear();
                }

                if (!windows->to_be_added.empty()) {
                    for (auto w : windows->to_be_added)
                        windows->list.push_back(w);
                    windows->to_be_added.clear();
                }
            }

            auto mylar = (MylarWindow*)root->user_data;

            // merge list with containers
            for (auto w : windows->list)  {
                bool found = false;
                for (auto ch : c->children)
                    if (ch->custom_type == w->cid)
                        found = true;
                if (!found) {
                    auto ch = c->child(::absolute, b.h * mylar->raw_window->dpi, FILL_SPACE);
                    ch->skip_delete = true;
                    ch->user_data = w;
                    ch->when_paint = paint {
                        auto w = (Window*)c->user_data;
                        auto mylar = (MylarWindow*)root->user_data;
                        auto cr = mylar->raw_window->cr;

                        if (active_cid == w->cid) {
                            cairo_rectangle(cr, c->real_bounds.x, c->real_bounds.y, c->real_bounds.w, c->real_bounds.h);
                            cairo_set_source_rgba(cr, 1, 1, 1, .15);
                            cairo_fill(cr);
                        }

                        paint_button_bg(root, c);

                        int offx = 0;
                        if (icons_loaded) {
                            if (w->icon_surf) {
                                auto h = cairo_image_surface_get_height(w->icon_surf);
                                cairo_set_source_surface(cr, w->icon_surf,
                                    c->real_bounds.x + 10, c->real_bounds.y + c->real_bounds.h * .5 - h * .5);
                                cairo_paint(cr);
                                auto wi = cairo_image_surface_get_height(w->icon_surf);
                                offx = wi + 10;
                            }
                        }
                        auto b = draw_text(cr, c, w->title, 9 * mylar->raw_window->dpi, false, mylar_font, 300 * PANGO_SCALE, c->real_bounds.h * PANGO_SCALE);
                        draw_text(cr,
                            c->real_bounds.x + 10 + offx, c->real_bounds.y + c->real_bounds.h * .5 - b.h * .5,
                            w->title, 9 * mylar->raw_window->dpi, true, mylar_font, 300 * PANGO_SCALE, c->real_bounds.h * PANGO_SCALE);

                        if (w->cid == active_cid) {
                            auto bar_h = std::round(2 * mylar->raw_window->dpi);
                            cairo_rectangle(cr, c->real_bounds.x, c->real_bounds.y + c->real_bounds.h - bar_h, c->real_bounds.w, bar_h);
                            cairo_set_source_rgba(cr, 1, 1, 1, 1);
                            cairo_fill(cr);
                        }
                   };
                   ch->when_clicked = paint {
                       auto mylar = (MylarWindow*)root->user_data;
                       auto cid = c->custom_type;
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
                                   pop.on_clicked = []() {  };
                                   root.push_back(pop);
                               }
                               {
                                   PopOption pop;
                                   pop.text = "Pin/unpin";
                                   pop.on_clicked = []() {  };
                                   root.push_back(pop);
                               }
                               {
                                   PopOption pop;
                                   pop.text = "Edit pin";
                                   pop.on_clicked = []() {  };
                                   root.push_back(pop);
                               }
                               {
                                   PopOption pop;
                                   pop.text = "End task";
                                   pop.on_clicked = []() { };
                                   root.push_back(pop);
                               }
                               {
                                   PopOption pop;
                                   pop.text = "Close window";
                                   pop.on_clicked = [cid]() {
                                       close_window(cid);
                                   };
                                   root.push_back(pop);
                               }

                               popup::open(root, m.x - startoff + cw * .5 - (277 * .5) + 1.4, m.y);
                               //popup::open(root, m.x - (277 * .5), m.y);
                           });
                       }
                   };
                   ch->pre_layout = [](Container *root, Container *c, const Bounds &b) {
                       auto w = (Window *) c->user_data;
                       auto mylar = (MylarWindow*)root->user_data;
                       auto cr = mylar->raw_window->cr;

                       auto bounds = draw_text(cr, c, w->title, 9 * mylar->raw_window->dpi, false, mylar_font, 300 * PANGO_SCALE, c->real_bounds.h * PANGO_SCALE);
                       if (icons_loaded) {
                           if (!w->attempted_load || w->scale_change) {
                               w->scale_change = false;
                               w->attempted_load = true;
                               auto size = get_icon_size(mylar->raw_window->dpi);
                               auto full = one_shot_icon(size, {w->icon, to_lower(w->icon), c3ic_fix_wm_class(w->icon), to_lower(w->icon)});
                               if (!full.empty()) {
                                   load_icon_full_path(&w->icon_surf, full, size);
                               }
                           }
                       }

                       if (w->icon_surf) {
                           bounds.w += cairo_image_surface_get_width(w->icon_surf) + 10;
                       }

                       c->wanted_bounds.w = bounds.w + 20;
                   };
                   ch->custom_type = w->cid;
                }
            }

            for (int i = c->children.size() - 1; i >= 0; i--) {
                auto ch = c->children[i];
                bool found = false;
                for (auto w : windows->list)
                    if (w->cid == ch->custom_type)
                        found = true;
                if (!found) {
                    delete c->children[i];
                    c->children.erase(c->children.begin() + i);
                }
            }
        };
    }*/

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
        brightness_data->value = get_brightness();

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
        volume_level = get_volume_level();
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
        battery_level = get_battery_level();
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

void dock_start() {
    auto dock = new Dock;
    docks.push_back(dock);
    dock->app = windowing::open_app();
    RawWindowSettings settings;
    settings.pos.w = 0;
    settings.pos.h = 40;
    settings.name = "Dock";
    dock->window = open_mylar_window(dock->app, WindowType::DOCK, settings);
    dock->window->raw_window->on_scale_change = [](RawWindow *rw, float dpi) {
        notify("scale change");
    };
    dock->window->root->skip_delete = true;
    dock->window->root->user_data = dock;
    fill_root(dock->window->root);
    dock->window->root->alignment = ALIGN_RIGHT;
    windowing::main_loop(dock->app);

    // Cleanup dock
    delete dock->app;
    delete dock->window;
    delete dock;
}


void dock::start() {
    //return;
    std::thread t(dock_start);
    t.detach();
}

void dock::stop() {
    //return;
    finished = true;
    for (auto d : docks) {
        windowing::close_app(d->app);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    cleanup_cached_fonts();
}

// This happens on the main thread, not the dock thread
void dock::add_window(int cid) {
    /*
    if (!windows)
        return;

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
    std::lock_guard<std::mutex> guard(windows->mut);
    windows->to_be_added.push_back(window);
    if (mylar_window)
        windowing::redraw(mylar_window->raw_window);
    */
}

// This happens on the main thread, not the dock thread
void dock::remove_window(int cid) {
    /*
    if (!windows)
        return;
    std::lock_guard<std::mutex> guard(windows->mut);
    windows->to_be_removed.push_back(cid);
    if (mylar_window)
        windowing::redraw(mylar_window->raw_window);
    */
}

void dock::title_change(int cid, std::string title) {
    /*
    std::lock_guard<std::mutex> guard(windows->mut);
    for (auto &w : windows->list) {
        if (w->cid == cid) {
            w->title = title;
        }
    }
    if (mylar_window)
        windowing::redraw(mylar_window->raw_window);
    */
}

void dock::on_activated(int cid) {
    active_cid = cid;
    for (auto d : docks)
        windowing::redraw(d->window->raw_window);
}

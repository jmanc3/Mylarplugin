#include "dock.h"

#include "second.h"

#include "client/raw_windowing.h"
#include "client/windowing.h"
#include "process.hpp"
#include "hypriso.h"

#include <cairo.h>
#include "process.hpp"
#include <chrono>
#include <thread>
#include <memory>
#include <pango/pangocairo.h>

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

static float battery_level = 100;
static bool charging = true;
static float volume_level = 100;
static bool finished = false;
static bool nightlight_on = false;
static RawApp *dock_app = nullptr;
static MylarWindow *mylar_window = nullptr;

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
    auto mylar = (MylarWindow *) root->user_data;
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

Bounds draw_text(cairo_t *cr, int x, int y, std::string text, int size = 10, bool draw = true, std::string font = mylar_font) {
    auto layout = get_cached_pango_font(cr, mylar_font, size, PANGO_WEIGHT_NORMAL, false);
    //pango_layout_set_text(layout, "\uE7E7", strlen("\uE83F"));
    pango_layout_set_text(layout, text.data(), text.size());
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

Bounds draw_text(cairo_t *cr, Container *c, std::string text, int size = 10, bool draw = true, std::string font = mylar_font) {
    auto layout = get_cached_pango_font(cr, mylar_font, size, PANGO_WEIGHT_NORMAL, false);
    //pango_layout_set_text(layout, "\uE7E7", strlen("\uE83F"));
    pango_layout_set_text(layout, text.data(), text.size());
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
    auto mylar = (MylarWindow*)root->user_data;
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

static void paint_battery(Container *root, Container *c) {
    auto mylar = (MylarWindow*)root->user_data;
    auto battery_data = (BatteryData *) c->user_data;
    
    auto cr = mylar->raw_window->cr;
    paint_button_bg(root, c);
    std::string charging_text = charging ? "+" : "-";

    auto ico = draw_text(cr, c, fz("\uEBA7"), 12 * mylar->raw_window->dpi, false, "Segoe MDL2 Assets");
    draw_text(cr, c->real_bounds.x + 10, center_y(c, ico.h), fz("\uEBA7"), 12 * mylar->raw_window->dpi, true, "Segoe MDL2 Assets");
    
    auto tex = draw_text(cr, c, fz("{}{}%", charging_text, (int) std::round(battery_level)), 9 * mylar->raw_window->dpi, false);
    draw_text(cr, c->real_bounds.x + 10 + ico.w + 10, center_y(c, tex.h), fz("{}{}%", charging_text, (int) std::round(battery_level)), 9 * mylar->raw_window->dpi, true); 
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

static void watch_battery_level() {
    auto process = std::make_shared<TinyProcessLib::Process>("upower --monitor", "", [](const char *bytes, size_t n) {
        battery_level = get_battery_level();
        charging = battery_charging();
        windowing::wake_up(mylar_window->raw_window);
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

static void watch_volume_level() {
    auto process = std::make_shared<TinyProcessLib::Process>("pactl subscribe", "", [](const char *bytes, size_t n) {
        long current = get_current_time_in_ms();
        std::string text(bytes, n);
        bool contains = text.find("change") != std::string::npos;
        if (current - last_time_volume_adjusted > 400 && contains) {
            volume_level = get_volume_level();
            windowing::wake_up(mylar_window->raw_window);            
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
    static bool queued = false;
    static bool latest = amount;
    latest = amount;
    if (queued)
        return;
    queued = true;
    std::thread t([amount]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        auto process = std::make_shared<TinyProcessLib::Process>(fz("brightnessctl set {}", (int) std::round(amount)));
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

        queued = false;
    });
    t.detach();
}

static void fill_root(Container *root) {
    root->when_paint = paint_root;

    { 
        auto active_settings = root->child(40, FILL_SPACE);
        active_settings->when_clicked = paint {
            system("hyprctl dispatch plugin:mylar:right_click_active");
        };
        active_settings->when_paint = paint {
            auto mylar = (MylarWindow*)root->user_data;
            auto cr = mylar->raw_window->cr;
            paint_button_bg(root, c);
//Bounds draw_text(cairo_t *cr, Container *c, std::string text, int size = 10, bool draw = true, std::string font = mylar_font) {
            
            draw_text(cr, c, fz("\uEB3C"), 12 * mylar->raw_window->dpi, true, "Segoe Fluent Icons");
        };
        active_settings->pre_layout = [](Container *root, Container *c, const Bounds &b) {
            auto mylar = (MylarWindow*)root->user_data;
            auto cr = mylar->raw_window->cr;
            auto bounds = draw_text(cr, c, fz("\uEB3C"), 12 * mylar->raw_window->dpi, false, "Segoe Fluent Icons");
            c->wanted_bounds.w = bounds.w + 20;
        }; 
    }

    { 
        auto toggle = root->child(40, FILL_SPACE);
        toggle->when_clicked = paint {
            system("hyprctl dispatch plugin:mylar:toggle_layout");
        };
        toggle->when_paint = paint {
            auto mylar = (MylarWindow*)root->user_data;
            auto cr = mylar->raw_window->cr;
            paint_button_bg(root, c);
            draw_text(cr, c, fz("\uF0E2"), 12 * mylar->raw_window->dpi, true, "Segoe Fluent Icons");
        };
        toggle->pre_layout = [](Container *root, Container *c, const Bounds &b) {
            auto mylar = (MylarWindow*)root->user_data;
            auto cr = mylar->raw_window->cr;
            auto bounds = draw_text(cr, c, fz("\uF0E2"), 12 * mylar->raw_window->dpi, false, "Segoe Fluent Icons");
            c->wanted_bounds.w = bounds.w + 20;
        }; 
    }

    { 
        auto night = root->child(40, FILL_SPACE);
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
        night->when_paint = paint {
            auto mylar = (MylarWindow*)root->user_data;
            auto cr = mylar->raw_window->cr;
            paint_button_bg(root, c);
            draw_text(cr, c, fz("\uF126"), 12 * mylar->raw_window->dpi, true, "Segoe Fluent Icons");
        };
        night->pre_layout = [](Container *root, Container *c, const Bounds &b) {
            auto mylar = (MylarWindow*)root->user_data;
            auto cr = mylar->raw_window->cr;
            auto bounds = draw_text(cr, c, fz("\uF126"), 12 * mylar->raw_window->dpi, false, "Segoe Fluent Icons");
            c->wanted_bounds.w = bounds.w + 20;
        }; 
    }
    
    {
        auto volume = root->child(40, FILL_SPACE);
        auto volume_data = new VolumeData;
        volume_level = get_volume_level();
        watch_volume_level();
        volume->when_fine_scrolled = [](Container* root, Container* c, int scroll_x, int scroll_y, bool came_from_touchpad) {
            //notify(fz("fine scrolled {} {}", ((double) scroll_y) * .001, came_from_touchpad));
            auto mylar = (MylarWindow*)root->user_data;
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
        volume->when_paint = paint {
            auto mylar = (MylarWindow*)root->user_data;
            auto volume_data = (VolumeData *) c->user_data;
            auto cr = mylar->raw_window->cr;
            paint_button_bg(root, c);
            
            auto ico = draw_text(cr, c, fz("\uE995"), 12 * mylar->raw_window->dpi, false, "Segoe MDL2 Assets");
            draw_text(cr, c->real_bounds.x + 10, center_y(c, ico.h), fz("\uE995"), 12 * mylar->raw_window->dpi, true, "Segoe MDL2 Assets");
            
            auto tex = draw_text(cr, c, fz("{}%", (int) std::round(volume_level)), 9 * mylar->raw_window->dpi, false);
            draw_text(cr, c->real_bounds.x + 10 + ico.w + 10, center_y(c, tex.h), fz("{}%", (int) std::round(volume_level)), 9 * mylar->raw_window->dpi, true);
        };
        volume->pre_layout = [](Container *root, Container *c, const Bounds &b) {
            auto mylar = (MylarWindow*)root->user_data;
            auto cr = mylar->raw_window->cr;
            auto ico = draw_text(cr, c, fz("\uE995"), 12 * mylar->raw_window->dpi, false, "Segoe MDL2 Assets");
            auto tex = draw_text(cr, c, fz("{}%", (int) std::round(volume_level)), 9 * mylar->raw_window->dpi, false);
            c->wanted_bounds.w = ico.w + tex.w + 30;
        };
        volume->when_clicked = paint {
            auto mylar = (MylarWindow*)root->user_data;
            windowing::close_window(mylar->raw_window);
        };
    }

    {
        auto battery = root->child(40, FILL_SPACE);
        auto battery_data = new BatteryData;
        battery_level = get_battery_level();
        charging = battery_charging();
        watch_battery_level();
        battery->user_data = battery_data;
        battery->when_paint = paint_battery;
        battery->pre_layout = [](Container *root, Container *c, const Bounds &b) {
            auto mylar = (MylarWindow*)root->user_data;
            auto cr = mylar->raw_window->cr;
            std::string charging_text = charging ? "+" : "-";

            auto ico = draw_text(cr, c, fz("\uEBA7"), 12 * mylar->raw_window->dpi, false, "Segoe MDL2 Assets");
            auto tex = draw_text(cr, c, fz("{}{}%", charging_text, (int) std::round(battery_level)), 9 * mylar->raw_window->dpi, false);
            c->wanted_bounds.w = ico.w + tex.w + 30;
 
            //auto bounds = draw_text(cr, c, fz("\uEBA7"), 12 * mylar->raw_window->dpi, false, "Segoe Fluent Icons");
            //c->wanted_bounds.w = bounds.w + 20;
        };
        battery->when_clicked = paint {
            auto mylar = (MylarWindow*)root->user_data;
            windowing::close_window(mylar->raw_window);
        };
    }
    
    {
        auto brightness = root->child(40, FILL_SPACE);
        auto brightness_data = new BrightnessData;
        brightness_data->value = get_brightness();

        brightness->when_fine_scrolled = [](Container* root, Container* c, int scroll_x, int scroll_y, bool came_from_touchpad) {
            //notify(fz("fine scrolled {} {}", ((double) scroll_y) * .001, came_from_touchpad));
            auto mylar = (MylarWindow*)root->user_data;
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
        brightness->when_paint = paint {
            auto mylar = (MylarWindow*)root->user_data;
            auto brightness_data = (BrightnessData *) c->user_data;
            auto cr = mylar->raw_window->cr;
            paint_button_bg(root, c);

            auto ico = draw_text(cr, c, fz("\uE793"), 12 * mylar->raw_window->dpi, false, "Segoe MDL2 Assets");
            draw_text(cr, c->real_bounds.x + 10, center_y(c, ico.h), fz("\uE793"), 12 * mylar->raw_window->dpi, true, "Segoe MDL2 Assets");
        
            auto tex = draw_text(cr, c, fz("{}%", (int) std::round(brightness_data->value)), 9 * mylar->raw_window->dpi, false);
            draw_text(cr, c->real_bounds.x + 10 + ico.w + 10, center_y(c, tex.h), fz("{}%", (int) std::round(brightness_data->value)), 9 * mylar->raw_window->dpi, true); 
        };
        brightness->pre_layout = [](Container *root, Container *c, const Bounds &b) {
            auto mylar = (MylarWindow*)root->user_data;
            auto cr = mylar->raw_window->cr;
            auto brightness_data = (BrightnessData *) c->user_data;
             
            auto ico = draw_text(cr, c, fz("\uE793"), 12 * mylar->raw_window->dpi, false, "Segoe MDL2 Assets");
            auto tex = draw_text(cr, c, fz("{}%", (int) std::round(brightness_data->value)), 9 * mylar->raw_window->dpi, false);
            c->wanted_bounds.w = ico.w + tex.w + 30;
        };
        brightness->when_clicked = paint {
            auto mylar = (MylarWindow*)root->user_data;
            windowing::close_window(mylar->raw_window);
        };
    }

    {
        auto date = root->child(40, FILL_SPACE);
        date->when_paint = paint {
            auto mylar = (MylarWindow*)root->user_data;
            auto cr = mylar->raw_window->cr;
            paint_button_bg(root, c);
            draw_text(cr, c, get_date(), 9 * mylar->raw_window->dpi);
        };
        date->pre_layout = [](Container *root, Container *c, const Bounds &b) {
            auto mylar = (MylarWindow*)root->user_data;
            auto cr = mylar->raw_window->cr;
            auto bounds = draw_text(cr, c, get_date(), 9 * mylar->raw_window->dpi, false);
            c->wanted_bounds.w = bounds.w + 20;
        };
        date->when_clicked = paint {
            auto mylar = (MylarWindow*)root->user_data;
            windowing::close_window(mylar->raw_window);
        };
    }
};

void dock_start() {
    dock_app = windowing::open_app();
    RawWindowSettings settings;
    settings.pos.w = 0;
    settings.pos.h = 40;
    settings.name = "Dock";
    auto mylar = open_mylar_window(dock_app, WindowType::DOCK, settings);
    mylar_window = mylar;
    mylar->root->user_data = mylar;
    mylar->root->alignment = ALIGN_RIGHT;
    fill_root(mylar->root);

    //notify("be");
    windowing::main_loop(dock_app);
    //notify("asdf");
}

void dock::start() {
    //return;
    std::thread t(dock_start);
    t.detach();
}

void dock::stop() {
    //return;
    finished = true;
    windowing::close_app(dock_app);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
}


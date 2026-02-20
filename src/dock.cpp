#include "dock.h"

#include "client/wlr-foreign-toplevel-management-unstable-v1-client-protocol.h"
#include "heart.h"
#include "dock_thumbnails.h"

#include "client/raw_windowing.h"
#include "client/windowing.h"
#include "process.hpp"
#include "hypriso.h"
#include "icons.h"
#include "popup.h"
#include "edit_pin.h"
#include "audio.h"

#include <algorithm>
#include <cairo.h>
#include "process.hpp"
#include <chrono>
#include <mutex>
#include <cmath>
#include <functional>
#include <pango/pango-layout.h>
#include <pango/pango-types.h>
#include <random>
#include <sys/wait.h>
#include <thread>
#include <memory>
#include <fstream>
#include <pango/pangocairo.h>
#include <sys/stat.h>
#include <filesystem>
#include <unordered_map>

#define BTN_LEFT		0x110
#define BTN_RIGHT		0x111
#define BTN_MIDDLE		0x112
#define ICON(str) [](){ return str; }

static bool merge_windows = false;
static bool labels = true;
static int pixel_spacing = 1;
static float max_width = 200;
static container_alignment icon_alignment = container_alignment::ALIGN_LEFT;

static void write_saved_pins_to_file(Container *icons);

class Window {
public:
    int cid; // unique id
   
    std::string window_icon;
    std::string command;
    std::string title;
    std::string stack_rule; // Should be regex later, or multiple 

    //cairo_surface_t* icon_surf = nullptr; 
    
    ~Window() {
        //if (icon_surf)
            //cairo_surface_destroy(icon_surf);
    }
    
    Window() {}

    Window(const Window& w) {
        cid = w.cid;
        window_icon = w.window_icon;
        command = w.command;
        title = w.title;
        stack_rule = w.stack_rule;
        //icon_surf = nullptr;
    };
};

struct SpringAnimation {
    // Parameters for the spring motion
    float position;
    float velocity = 0.0;
    float target;
    float damping;
    float stiffness;
    float mass;
    
    // Create a spring animation with initial position 0, target position 100
    // SpringAnimation spring(0.0f, 100.0f, 0.1f, 10.0f, 1.0f); // Adjusted for bounce
    
    // Simulate the spring animation
    float dt = 0.016f; // Assuming 60 updates per second
    
    // Constructor to initialize the parameters
    SpringAnimation(float pos = 0.0f, float tar = 0.0f, float damp = 29.5f, float stiff = 350.0f, float m = 1.0f)
            : position(pos), velocity(0.0f), target(tar), damping(damp), stiffness(stiff), mass(m) {}
    
    // Method to update the animation state
    void update(float deltaTime);
    
    // Method to set a new target position
    void setTarget(float newTarget);
};

void SpringAnimation::update(float deltaTime) {
    // Calculate the force based on Hooke's Law: F = -kx
    float force = -stiffness * (position - target);
    // Calculate the damping force: Fd = -bv
    float dampingForce = -damping * velocity;
    // Sum the forces
    float acceleration = (force + dampingForce) / mass;
    // Integrate to get the velocity and position
    velocity += acceleration * deltaTime;
    position += velocity * deltaTime;
}

void SpringAnimation::setTarget(float newTarget) {
    target = newTarget;
}

struct Pin : UserData {
    std::vector<Window> windows;

    std::string icon;
    std::string command;
    std::string stacking_rule;

    std::string full_icon;
    
    cairo_surface_t* icon_surf = nullptr; 
    bool attempted_load = false;
    bool scale_change = false;    

    long creation_time = get_current_time_in_ms();
    
    bool animating = false;
    SpringAnimation spring;
    double actual_w = 0;
    long animation_start_time = 0;

    int hover_timer_fd = -1;

    ~Pin() {
        if (icon_surf)
            cairo_surface_destroy(icon_surf);
    }
 

    bool pinned = false;
    int natural_position_x = INT_MAX;
    bool wants_reposition_animation = true;
    float init_repo_vel = 0;
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
    
    MylarWindow *volume = nullptr;
    
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

static RawWindowSettings make_icon_anchored_popup_settings(Container *icon,
                                                           float dpi,
                                                           int popup_w,
                                                           int popup_h) {
    const int icon_x = (int) std::round(icon->real_bounds.x / dpi);
    const int icon_y = (int) std::round(icon->real_bounds.y / dpi);
    const int icon_w = std::max(1, (int) std::round(icon->real_bounds.w / dpi));
    const int icon_h = std::max(1, (int) std::round(icon->real_bounds.h / dpi));

    RawWindowSettings settings;
    settings.pos.w = popup_w;
    settings.pos.h = popup_h;
    settings.name = "Popup";
    settings.popup.use_explicit_anchor_rect = true;
    settings.popup.anchor_rect_x = icon_x;
    settings.popup.anchor_rect_y = icon_y;
    settings.popup.anchor_rect_w = icon_w;
    settings.popup.anchor_rect_h = icon_h;
    settings.popup.anchor = RawWindowSettings::PopupAnchor::BOTTOM;
    settings.popup.gravity = RawWindowSettings::PopupGravity::BOTTOM;
    settings.popup.use_offset = true;
    settings.popup.offset_y = -8;
    settings.popup.constraint_adjustment =
        RawWindowSettings::POPUP_CONSTRAINT_SLIDE_X |
        RawWindowSettings::POPUP_CONSTRAINT_SLIDE_Y |
        RawWindowSettings::POPUP_CONSTRAINT_FLIP_X |
        RawWindowSettings::POPUP_CONSTRAINT_FLIP_Y;
    return settings;
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

struct BrightnessData : UserData {
    float value = 50;
};

static PangoLayout *
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
            return font->layout;
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

static void cleanup_cached_fonts() {
    for (auto font: cached_fonts) {
        delete font;
    }
    cached_fonts.clear();
    cached_fonts.shrink_to_fit();
}

static void remove_cached_fonts(cairo_t *cr) {
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
    static RGBA default_color("ffffff88");
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

static Bounds draw_text(cairo_t *cr, int x, int y, std::string text, int size = 10, bool draw = true, std::string font = mylar_font, int wrap = -1, int h = -1, RGBA color = {1, 1, 1, 1}) {
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
    cairo_set_source_rgba(cr, color.r, color.g, color.b, color.a);
    PangoRectangle ink;
    PangoRectangle logical;
    pango_layout_get_pixel_extents(layout, &ink, &logical);
    if (draw) {
        cairo_move_to(cr, std::round(x), std::round(y));
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
    return;
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

static void set_master_volume(float amount) {
    //notify("set master");
    audio([amount]() {
        for (auto client : audio_clients) {
            if (client->is_master_volume()) {
            //notify(fz("{}", amount / 100.0f));
                client->set_volume(amount / 100.0f);
            }
        }
    });
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

static void pinned_right_click(int cid, int startoff, int cw, std::string uuid, Pin *pin, float dpi, float yoff, std::string full_icon_path) {
    main_thread([cid, startoff, cw, uuid, pin, dpi, yoff, full_icon_path] {
        auto m = mouse();
        std::vector<PopOption> root;
        auto stacking_rule = pin->stacking_rule;
        {
            PopOption pop;
            pop.text = stacking_rule;
            pop.icon_left = full_icon_path;
            pop.on_clicked = [uuid]() {
                for (auto d : docks) {
                    if (auto icons = container_by_name("icons", d->window->root)) {
                        for (auto p : icons->children) {
                            auto pin = (Pin*)p->user_data;
                            if (p->uuid == uuid) {
                                launch_command(pin->command);
                            }
                        }
                    }
                }
            };
            root.push_back(pop);
        }
        {
            PopOption pop;
            pop.text = "Edit pin";
            pop.icon_left = "\uE713";
            pop.is_text_icon = true;
            pop.on_clicked = [stacking_rule]() {
                // go through the dock and find the first pin to match the stacking rule and copy over the data then, when edit done
                // call docK::edit_pin(original_stacking_rule, new_stacking_rule, new_command, new_icon);
                for (auto d : docks) {
                    if (auto icons = container_by_name("icons", d->window->root)) {
                        for (auto p : icons->children) {
                            auto pin = (Pin*)p->user_data;
                            if (pin->stacking_rule == stacking_rule) {
                                edit_pin::open(pin->stacking_rule, pin->icon, pin->command);
                                break;
                            }
                        }
                    }
                }
            };
            root.push_back(pop);
        }
        {
            PopOption pop;
#ifdef PROBLEMS
            assert(false && "Pin could've been destroyed");
#endif
            std::string pin_text;
            if (pin->pinned) {
                pop.text = "Unpin";
                pop.icon_left = "\uE77A";
            } else {
                pop.text = "Pin";
                pop.icon_left = "\uE718";
                pin_text = pop.text;
            }
            pop.is_text_icon = true;
            auto text = pop.text;
            pop.on_clicked = [text, stacking_rule, pin_text]() {
                for (auto d : docks) {
                    if (auto icons = container_by_name("icons", d->window->root)) {
                        for (auto p : icons->children) {
                            auto pin = (Pin*)p->user_data;
                            if (pin->stacking_rule == stacking_rule) {
                                pin->pinned = text == pin_text;
                            }
                        }
                    }
                }
                for (auto d : docks) {
                    if (auto icons = container_by_name("icons", d->window->root)) {
                        write_saved_pins_to_file(icons);
                        break;
                    }
                }
            };
            root.push_back(pop);
        }
        if (!pin->windows.empty()) {
            {
                PopOption pop;
                if (pin->windows.size() == 1) {
                    pop.text = "End task";
                } else {
                    pop.text = "End tasks";
                }
                pop.icon_left = "\uF140";
                pop.is_text_icon = true;
                auto text = pop.text;
                pop.on_clicked = [text, stacking_rule, uuid]() {
                    for (auto d : docks) {
                        if (auto icons = container_by_name("icons", d->window->root)) {
                            for (auto p : icons->children) {
                                auto pin = (Pin*)p->user_data;
                                if (text == "End task") {
                                    if (p->uuid == uuid) {
                                        for (auto client : pin->windows) {
                                            auto pid = hypriso->get_pid(client.cid);
                                            if (pid != -1) {
                                                kill(pid, SIGKILL);
                                            }
                                        }
                                    }
                                } else {
                                    if (pin->stacking_rule == stacking_rule) {
                                        for (auto client : pin->windows) {
                                            auto pid = hypriso->get_pid(client.cid);
                                            if (pid != -1) {
                                                kill(pid, SIGKILL);
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                };
                root.push_back(pop);
            }
            {
                PopOption pop;
                if (pin->windows.size() == 1) {
                    pop.text = "Close window";
                } else {
                    pop.text = "Close windows";
                }
                pop.is_text_icon = true;
                pop.icon_left = "\uE10A";
                auto text = pop.text;
                pop.on_clicked = [text, stacking_rule, uuid]() {
                    for (auto d : docks) {
                        if (auto icons = container_by_name("icons", d->window->root)) {
                            for (auto p : icons->children) {
                                auto pin = (Pin*)p->user_data;
                                if (text == "Close window") {
                                    if (p->uuid == uuid) {
                                        for (auto client : pin->windows) {
                                            close_window(client.cid);
                                        }
                                    }
                                } else {
                                    if (pin->stacking_rule == stacking_rule) {
                                        for (auto client : pin->windows) {
                                            close_window(client.cid);
                                        }
                                    }
                                }
                            }
                        }
                    }
                };
                root.push_back(pop);
            }
        }

        popup::open(root, m.x - startoff + cw * .5 - (277 * .5) + 1.4, m.y - (yoff / dpi) - 5 - (24 * root.size() * dpi));
    });
}

static void draw_clip_begin(cairo_t *cr, const Bounds &b) {
    cairo_save(cr);
    set_rect(cr, b);
    cairo_clip(cr);
}

void draw_clip_end(cairo_t *cr) {
    cairo_reset_clip(cr);
    cairo_restore(cr);
}

static void create_pinned_icon(Container *icons, std::string stack_rule, std::string command, std::string icon, Window *window = nullptr) {
    auto ch = icons->child(::absolute, FILL_SPACE, FILL_SPACE);
    auto pin = new Pin;
    for (auto icon : icons->children) {
        auto icon_data = (Pin *) icon->user_data;
        if (icon != ch && icon_data->pinned && stack_rule == icon_data->stacking_rule) {
            pin->pinned = true;
        }
    }
    pin->stacking_rule = stack_rule;
    pin->command = command;
    pin->icon = icon;
    if (window) {
        pin->windows.push_back(*window);
        // ch should be moved to right behind the last matching stacking rule
        for (int i = 0; i < icons->children.size(); i++) {
            if (ch == icons->children[i]) {
                icons->children.erase(icons->children.begin() + i);
            }
        }
        bool readded = false;
        for (int i = icons->children.size() - 1; i >= 0; i--) {
            auto ch_pin = (Pin*)icons->children[i]->user_data;
            if (ch_pin->stacking_rule == stack_rule) {
                readded = true;
                icons->children.insert(icons->children.begin() + i + 1, ch);
                break;
            }
        }
        if (!readded) {
            icons->children.push_back(ch);
        }
    } else
        pin->pinned = true;
    ch->user_data = pin;
    ch->when_drag_end_is_click = false;
    ch->minimum_x_distance_to_move_before_drag_begins = 3;
    ch->minimum_y_distance_to_move_before_drag_begins = 10;
    ch->when_paint = paint {        
        auto dock = (Dock*)root->user_data;
        if (!dock || !dock->window || !dock->window->raw_window || !dock->window->raw_window->cr)
            return;

        Pin* pin = (Pin*)c->user_data;
        auto mylar = dock->window;
        auto cr = mylar->raw_window->cr;

        auto backup = c->real_bounds;
        {
            auto b = backup;
            c->real_bounds = b.intersection(c->parent->real_bounds);
        }
        defer(c->real_bounds = backup);

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
        } else if (!pin->windows.empty()) {
            cairo_rectangle(cr, c->real_bounds.x, c->real_bounds.y, c->real_bounds.w, c->real_bounds.h);
            auto col = color_dock_sel_active_color();
            col.a *= .7;
            cairo_set_source_rgba(cr, col.r, col.g, col.b, col.a);
            cairo_fill(cr);
        }
        auto none = pin->windows.empty();
        if (!labels)
            none = true;

        int offx = 0;
        if (icons_loaded) {
            auto ico_surf = pin->icon_surf;
            if (ico_surf) {
                auto h = cairo_image_surface_get_height(ico_surf);
                auto w = cairo_image_surface_get_width(ico_surf);
                if (none) {
                    cairo_set_source_surface(cr, ico_surf, std::round(c->real_bounds.x + c->real_bounds.w * .5 - w * .5), std::round(c->real_bounds.y + c->real_bounds.h * .5 - h * .5));
                } else {
                    cairo_set_source_surface(cr, ico_surf, std::round(c->real_bounds.x + 10), std::round(c->real_bounds.y + c->real_bounds.h * .5 - h * .5));
                }
                cairo_paint(cr);
                auto wi = cairo_image_surface_get_height(ico_surf);
                offx = wi;
            }
        }

        if (!none) {
            std::string title = pin->stacking_rule;
            if (!pin->windows.empty())
                title = pin->windows[0].title;
            auto bc = c->real_bounds;
            if (offx == 0) { // No Icon
                auto text_w = (pin->actual_w - 20) * PANGO_SCALE;
                auto b = draw_text(cr, c, title, 9 * mylar->raw_window->dpi, false, mylar_font, text_w, c->real_bounds.h * PANGO_SCALE);
                draw_clip_begin(cr, bc);
                draw_text(cr, c->real_bounds.x + 10, c->real_bounds.y + c->real_bounds.h * .5 - b.h * .5, title, 9 * mylar->raw_window->dpi, true, mylar_font, text_w,
                          c->real_bounds.h * PANGO_SCALE);
                draw_clip_end(cr);
            } else {
                auto text_w = (pin->actual_w - offx - 30) * PANGO_SCALE;
                auto b = draw_text(cr, c, title, 9 * mylar->raw_window->dpi, false, mylar_font, text_w, c->real_bounds.h * PANGO_SCALE);
                draw_clip_begin(cr, bc);
                draw_text(cr, c->real_bounds.x + offx + 20, c->real_bounds.y + c->real_bounds.h * .5 - b.h * .5, title, 9 * mylar->raw_window->dpi, true, mylar_font, text_w, c->real_bounds.h * PANGO_SCALE);
                draw_clip_end(cr);
            }
        }

        if (is_active || !pin->windows.empty()) {
            auto bar_h = std::round(2 * mylar->raw_window->dpi);
            cairo_rectangle(cr, c->real_bounds.x, c->real_bounds.y + c->real_bounds.h - bar_h, c->real_bounds.w, bar_h);
            auto col = color_dock_sel_accent_color();
            if (!pin->windows.empty() && !is_active)
                col.a *= .4;
            cairo_set_source_rgba(cr, col.r, col.g, col.b, col.a);
            cairo_fill(cr);
        }
    };
    struct PopData {
        std::string uuid;
    };
    ch->when_mouse_enters_container = paint {
        return;
        Pin* pin = (Pin*)c->user_data;
        auto dock = (Dock*)root->user_data;
        auto uuid = c->uuid;
        auto popdata = new PopData{uuid};
        pin->hover_timer_fd = windowing::timer(dock->app, 500, [](void *data) {
            auto pop_data = ((PopData *) data);
            for (auto d : docks) {                
                if (auto icons = container_by_name("icons", d->window->root)) {
                    for (auto p : icons->children) {
                        if (p->uuid == pop_data->uuid) {
                            auto pin = (Pin *) p->user_data;
                            std::vector<int> ids;
                            for (auto w : pin->windows)
                                ids.push_back(w.cid);
                            delete pop_data;

                            float posx = (p->real_bounds.x + p->real_bounds.w * .5) / d->window->raw_window->dpi;
                            float posy = p->real_bounds.y / d->window->raw_window->dpi - 10;

                            main_thread([posx, posy, ids]() { dock_thumbnails::open(posx, posy, 1, ids); });
                            goto out;
                        }
                    }
                }
            }
            out:
        }, popdata); 
    };
    ch->when_mouse_leaves_container = paint {
        return;
        Pin* pin = (Pin*)c->user_data;
        auto dock = (Dock*)root->user_data;
        if (pin->hover_timer_fd != -1) {
            windowing::timer_stop(dock->app, pin->hover_timer_fd); 
            pin->hover_timer_fd = -1;
        }
    };
    ch->when_clicked = paint {
        Pin* pin = (Pin*)c->user_data;
        auto dock = (Dock*)root->user_data;
        auto mylar = dock->window;
        auto cr = mylar->raw_window->cr;

        if (pin->windows.empty() && c->state.mouse_button_pressed == BTN_LEFT) {
            launch_command(pin->command);
            return;
        }

        int cid = -1;
        if (!pin->windows.empty())
            cid = pin->windows[0].cid;
        
        if (c->state.mouse_button_pressed == BTN_LEFT) {
            main_thread([cid] {
                // todo we need to focus next (already wrote this combine code)
                bool is_hidden = hypriso->is_hidden(cid);
                if (is_hidden) {
                    hypriso->set_hidden(cid, false, true);
                    hypriso->bring_to_front(cid);
                } else {
                    if (active_cid == cid) {
                        hypriso->set_hidden(cid, true, true);
                    } else {
                        hypriso->bring_to_front(cid);
                    }
                }
            });
        } else if (c->state.mouse_button_pressed == BTN_MIDDLE) {
            launch_command(pin->command);
        } else if (c->state.mouse_button_pressed == BTN_RIGHT) {
            int startoff = (root->mouse_current_x - c->real_bounds.x) / mylar->raw_window->dpi;
            int cw = c->real_bounds.w / mylar->raw_window->dpi;
            auto uuid = c->uuid;

            pinned_right_click(cid, startoff, cw, uuid, pin, dock->window->raw_window->dpi, dock->window->root->mouse_current_y, pin->full_icon);
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
            if (!pin->attempted_load || pin->scale_change) {
                pin->scale_change = false;
                pin->attempted_load = true;
                auto size = get_icon_size(mylar->raw_window->dpi);
                auto icon = pin->icon;
                auto full = one_shot_icon(size, {pin->icon, to_lower(pin->icon), c3ic_fix_wm_class(pin->icon), to_lower(pin->icon)});
                if (!full.empty()) {                        
                    pin->full_icon = full;
                    load_icon_full_path(&pin->icon_surf, full, size);
                }
            }
        }
    };
    ch->when_drag_start = paint {
        auto data = (Pin *) c->user_data;
        data->initial_mouse_click_before_drag_offset_x = c->real_bounds.x - root->mouse_initial_x;
        c->z_index = 1;
    };
    ch->when_drag_end = paint {
        auto data = (Pin *) c->user_data;
        data->initial_mouse_click_before_drag_offset_x = c->real_bounds.x - root->mouse_initial_x;
        c->z_index = 0;
        data->wants_reposition_animation = true;
        data->init_repo_vel = 4500;
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
    }
    for (int pin_index = icons->children.size() - 1; pin_index >= 0; pin_index--) {
        auto pin_container = icons->children[pin_index];
        auto pin = (Pin *) pin_container->user_data;

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
            } else {
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
            bool needs_to_create_its_own_pin = true;
            for (auto pin_container : icons->children) {
                auto pin = (Pin *) pin_container->user_data;
                if (pin->stacking_rule == window->stack_rule) {
                    if (merge_windows) {
                        needs_to_create_its_own_pin = false;
                        pin->windows.push_back(*window);
                        break;
                    } else if (pin->windows.empty() && pin->pinned) {
                        // If we are not merging windows, we still need to group up with pinned no windows item
                        needs_to_create_its_own_pin = false;
                        pin->windows.push_back(*window);
                        break;
                    }
                }
            }
            
            if (needs_to_create_its_own_pin) {
                create_pinned_icon(icons, window->stack_rule, window->command, window->window_icon, window);
            }
        }
    }
}

static void merge_to_be_into_list(Dock *dock, Container *c) {
    std::lock_guard<std::mutex> guard(dock->collection->mut);
    bool no_modification = dock->collection->to_be_removed.empty() && dock->collection->to_be_added.empty();
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
    if (!no_modification) {
        if (auto icons = container_by_name("icons", dock->window->root)) {
            for (auto p : icons->children) {
                auto pin = (Pin *) p->user_data;
                pin->wants_reposition_animation = true;
            }
        }
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
        auto pin = (Pin *) c->user_data;
        if (pin->windows.empty() || !labels) {
            c->real_bounds.w = 50 * dock->window->raw_window->dpi;
        } else {
            c->real_bounds.w = max_width * dock->window->raw_window->dpi;
        }
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

void calc_natural_positions(Container *icons, float total_width, Container *root) {
    auto align = icon_alignment; // todo: pull from setting
    
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
        if (data->natural_position_x != off) 
            data->wants_reposition_animation = true;
        data->natural_position_x = off;
        off += c->real_bounds.w + pixel_spacing;
    }
}

static void debounce(std::string id, long time_ms, std::function<void()> func) {
    struct DebounceData {
        bool started = false;
        long start_time = get_current_time_in_ms();
        std::function<void()> func = nullptr; 
    };
    static std::unordered_map<std::string, DebounceData> datas;
    if (datas.find(id) == datas.end())
        datas[id] = DebounceData();
    auto data = &datas[id];
    
    if (!data->started) {
        data->started = true;
        data->start_time = get_current_time_in_ms();
        data->func = func;
    } else {
        if ((get_current_time_in_ms() - data->start_time) > time_ms) {
            data->started = false;
            data->func();
            data->func = nullptr;
        }
    }
}

static void layout_icons(Container *root, Container *icons, Dock *dock) {
    float total_width = size_icons(dock, icons);

    calc_natural_positions(icons, total_width, root);
    
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
        // swap icons on taskbar
        int distance = 100000;
        int index = 0;
        int w_b = 0;
        auto natural_x = ((Pin *) icons->children[0]->user_data)->natural_position_x;
        static std::vector<Container *> before;
        before.clear();
        for (auto ch : icons->children)
            before.push_back(ch);

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

        bool any_change = false;
        for (int i = 0; i < before.size(); i++) {
            if (before[i] != icons->children[i]) {
                auto ch_pin_data = (Pin*)before[i]->user_data;
                ch_pin_data->wants_reposition_animation = true;
                any_change = true;
            }
        }

        if (any_change) {
            calc_natural_positions(icons, total_width, root);
            debounce("icon reorder", 400, []() {
                for (auto d : docks) {
                    if (auto icons = container_by_name("icons", d->window->root)) {
                        write_saved_pins_to_file(icons);
                        break;
                    }
                }
            });
        }
    }

    auto current = get_current_time_in_ms();
    static long previous = current;
    auto delta = current - previous;
    if (delta > 16)
        delta = 16;
    previous = current;

    // Queue spring animations
    for (auto c: icons->children) {
        if (c->state.mouse_dragging) continue;
        auto *data = (Pin *) c->user_data;
        if (current - data->creation_time < 1000) {
            c->real_bounds.x = data->natural_position_x;
            continue;
        }

        if (data->animating && !data->wants_reposition_animation) {
            data->spring.update((((float) delta) / 1000.0f) * 1.5);
            c->real_bounds.x = data->spring.position;
            float abs_vel = std::abs(data->spring.velocity);
            if ((current - data->animation_start_time) > 1500.0f) {
                data->animating = false;
                c->real_bounds.x = data->natural_position_x;           
            }
        } else if (data->wants_reposition_animation) {
            data->wants_reposition_animation = false;
            auto dist = std::abs(c->real_bounds.x - data->natural_position_x);
            data->spring = SpringAnimation(c->real_bounds.x, data->natural_position_x);
            if (data->init_repo_vel != 0.0) {
                auto scalar = data->init_repo_vel / 10000.0f;
                data->spring.stiffness *= .3 + 0.7 * (1.0 - scalar);
                data->spring.damping *= 0.7 + 0.3 * (1.0 - scalar);
            }
            //data->spring.velocity = data->init_repo_vel;
            data->animating = true;
            data->animation_start_time = current;
            data->init_repo_vel = 0;
        }
    }

    for (auto c: icons->children) {
        if (c->pre_layout) {
            c->pre_layout(root, c, c->real_bounds);
        }
    }

    // Start animating, if not already
    bool needs_refresh = false;
    for (auto c: icons->children) {
        auto *data = (Pin *) c->user_data;
        if (data->animating) {
            needs_refresh = true;
            break;
        }
    }

    if (needs_refresh) {
        windowing::redraw(dock->window->raw_window);
    }
}

static void drawRoundedRect(cairo_t *cr, double x, double y, double width, double height,
                     double radius, double stroke_width) {
    // Ensure the stroke width does not exceed the bounds
    double half_stroke = stroke_width / 2.0;
    double adjusted_radius = std::fmin(radius, std::fmin(width, height) / 2.0);
    double inner_width = width - stroke_width;
    double inner_height = height - stroke_width - 1;
    
    if (inner_width <= 0 || inner_height <= 0) {
        // Cannot draw if the stroke width exceeds or equals the bounds
        return;
    }
    
    // Adjusted bounds to ensure the stroke remains inside
    double adjusted_x = x + half_stroke;
    double adjusted_y = y + half_stroke;
    
    // Begin path for rounded rectangle
    cairo_new_path(cr);
    
    // Move to the start of the top-right corner
    cairo_move_to(cr, adjusted_x + adjusted_radius, adjusted_y);
    
    // Top side
    cairo_line_to(cr, adjusted_x + inner_width - adjusted_radius, adjusted_y);
    
    // Top-right corner
    cairo_arc(cr, adjusted_x + inner_width - adjusted_radius, adjusted_y + adjusted_radius,
              adjusted_radius, -M_PI / 2, 0);
    
    // Right side
    cairo_line_to(cr, adjusted_x + inner_width, adjusted_y + inner_height - adjusted_radius);
    
    // Bottom-right corner
    cairo_arc(cr, adjusted_x + inner_width - adjusted_radius, adjusted_y + inner_height - adjusted_radius,
              adjusted_radius, 0, M_PI / 2);
    
    // Bottom side
    cairo_line_to(cr, adjusted_x + adjusted_radius, adjusted_y + inner_height);
    
    // Bottom-left corner
    cairo_arc(cr, adjusted_x + adjusted_radius, adjusted_y + inner_height - adjusted_radius,
              adjusted_radius, M_PI / 2, M_PI);
    
    // Left side
    cairo_line_to(cr, adjusted_x, adjusted_y + adjusted_radius);
    
    // Top-left corner
    cairo_arc(cr, adjusted_x + adjusted_radius, adjusted_y + adjusted_radius,
              adjusted_radius, M_PI, 3 * M_PI / 2);
    
    // Close the path
    cairo_close_path(cr);
    
    // Set stroke width and stroke
    cairo_set_line_width(cr, stroke_width);
}


static void fill_root(Container *root) {
    root->when_paint = paint_root;
    root->type = ::hbox;
    root->receive_events_even_if_obstructed = true;
    root->when_mouse_enters_container = paint {
        auto dock = (Dock *) root->user_data;
        //windowing::set_size(dock->window->raw_window, 0, 100);
    };
    root->when_mouse_leaves_container = paint {
        auto dock = (Dock *) root->user_data;
        //windowing::set_size(dock->window->raw_window, 0, 40);
    };

    {
        auto super = simple_dock_item(root, ICON("\uF4A5"), ICON("Applications"));
        super->when_clicked = paint {
            system("wofi --show run &");
        };
        super->after_paint = paint {
            return;
            auto dock = (Dock *) root->user_data;
            auto cr = dock->window->raw_window->cr;
            set_rect(cr, c->real_bounds);
            static int off = 0;
            off++;
            if (off % 2 == 0) {
                set_argb(cr, {1, 1, 0, 1});
            } else {
                set_argb(cr, {1, 0, 0, 1});
            }
            cairo_fill(cr);
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

            for (int i = 0; i < c->children.size() - 1; i++) {
                if (c->children.empty())
                    break;
                auto *active = c->children[i];
                auto *active_data = (Pin *) active->user_data;
                active_data->actual_w = active->real_bounds.w;
                auto *next = c->children[i + 1];
                auto *next_data = (Pin *) next->user_data;
                next_data->actual_w = next->real_bounds.w;
                if ((active->real_bounds.w + active->real_bounds.x) > next->real_bounds.x) {
                    if (!active->state.mouse_dragging && !next->state.mouse_dragging) {
                        if (!active_data->windows.empty()) {
                            active->real_bounds.w = (next->real_bounds.x - active->real_bounds.x);
                        }
                    }
                }
            }
        };
        icons->after_paint = paint {
            for (int i = 0; i < c->children.size(); i++) {
                if (c->children.empty())
                    break;
                auto *active = c->children[i];
                auto *active_data = (Pin *) active->user_data;
                active->real_bounds.w = active_data->actual_w;
            }
        };
    }
    if (false) {
        auto align = simple_dock_item(root, []() {
            if (icon_alignment == container_alignment::ALIGN_RIGHT) {
                return "\uE8E2";
            } else if (icon_alignment == container_alignment::ALIGN_CENTER || icon_alignment == container_alignment::ALIGN_GLOBAL_CENTER_HORIZONTALLY) {
                return "\uE8E3";
            }
            return "\uE8E4";
        });
        align->when_clicked = paint {
            if (icon_alignment == ALIGN_LEFT) {
                icon_alignment = ALIGN_GLOBAL_CENTER_HORIZONTALLY;
            } else if (icon_alignment == ALIGN_GLOBAL_CENTER_HORIZONTALLY) {
                icon_alignment = ALIGN_RIGHT;
            } else {
                icon_alignment = ALIGN_LEFT;
            }
            for (auto d : docks) {
                if (auto icons = container_by_name("icons", d->window->root)) {
                    for (auto p : icons->children) {
                        auto data = (Pin *) p->user_data;
                        data->wants_reposition_animation = true;
                        data->init_repo_vel = 10000;
                    }
                }
            }
        };
    }

    if (true) {
        auto active_settings = simple_dock_item(root, ICON("\uE9E9"));
        active_settings->when_clicked = paint {
            system("hyprctl dispatch plugin:mylar:right_click_active");
        };
    }

    if (false) {
        auto toggle = simple_dock_item(root, ICON("\uF0E2"));
        toggle->when_clicked = paint {
            system("hyprctl dispatch plugin:mylar:toggle_layout");
        };
    }

    {
        auto extra = simple_dock_item(root, ICON("\ue70e"));
        extra->when_clicked = paint {
            auto dock = (Dock *) root->user_data;
            auto mylar = dock->window;
            auto dpi = mylar->raw_window->dpi;

            RawWindowSettings settings = make_icon_anchored_popup_settings(c, dpi, 220, 140);

            auto out = open_mylar_popup(mylar, settings);
            if (!out)
                return;
            out->root->when_paint = [out](Container *root, Container *c) {
                auto cr = out->raw_window->cr;
                set_argb(cr, {1, 1, 1, .8});
                drawRoundedRect(cr, c->real_bounds.x, c->real_bounds.y, c->real_bounds.w, c->real_bounds.h, 10 * out->raw_window->dpi, 1.0);
                cairo_fill(cr);
            };
            out->root->when_clicked = paint {
                main_thread([] {
                    notify("clicked inside");
                });
            };
            windowing::redraw(out->raw_window);
        };
    }

    if (false) {
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
    
    if (false) {
        auto change = simple_dock_item(root, ICON("\uE705"));
         
        change->when_clicked = paint {
            auto dock = (Dock *) root->user_data;
            static bool first = true;
            if (first) {
                first = false;
                windowing::set_size(dock->window->raw_window, 0, 100);
            } else {
                first = true;
                windowing::set_size(dock->window->raw_window, 0, 40);
            }
        };
    }

    if (false) {
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
        volume->name = "volume";
        volume_level = 100;
        volume->when_clicked = paint {
            auto dock = (Dock *) root->user_data;
            auto mylar = dock->window;
            auto dpi = mylar->raw_window->dpi;

            RawWindowSettings settings = make_icon_anchored_popup_settings(c, dpi, 330, 400);

            dock->volume = open_mylar_popup(mylar, settings);
            if (!dock->volume)
                return;
            dock->volume->root->on_closed = [](Container *root) {
                auto dock = (Dock *) root->user_data;
                dock->volume = nullptr;
            };
            dock->volume->root->user_data = dock;
            dock->volume->root->when_paint = [](Container *root, Container *c) {
                auto dock = (Dock *) root->user_data;
                auto cr = dock->volume->raw_window->cr;
                set_argb(cr, {1, 1, 1, .8});
                drawRoundedRect(cr, c->real_bounds.x, c->real_bounds.y, c->real_bounds.w, c->real_bounds.h, 10 * dock->volume->raw_window->dpi, 1.0);
                cairo_fill(cr);
            };
            dock->volume->root->when_clicked = paint {
                main_thread([] {
                    notify("clicked volume");
                });
            };
            audio_read([]() {
                dock::change_in_audio();
            }); 
            windowing::redraw(dock->volume->raw_window);
        };
        volume->when_fine_scrolled = [](Container* root, Container* c, int scroll_x, int scroll_y, bool came_from_touchpad) {
            auto dock = (Dock *) root->user_data;
            auto mylar = dock->window;
            volume_level += ((double) scroll_y) * .001;
            if (volume_level > 100) {
               volume_level = 100;
            }
            if (volume_level < 0) {
               volume_level = 0;
            }
            last_time_volume_adjusted = get_current_time_in_ms();
            set_master_volume(volume_level);
        };
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

    {
        auto show_desktop = root->child(6, FILL_SPACE);
        show_desktop->when_paint = paint {
            auto dock = (Dock *) root->user_data;
            auto mylar = dock->window;
            auto cr = mylar->raw_window->cr;
            paint_button_bg(root, c);
            set_argb(cr, {1, 1, 1, .3});
            set_rect(cr, {c->real_bounds.x, c->real_bounds.y, 1, c->real_bounds.h});
            cairo_fill(cr);
        };
        show_desktop->when_clicked = [](Container *root, Container *c) {
            main_thread([]() {
                hypriso->whitelist_on = !hypriso->whitelist_on;
                damage_all();
            });
        };
        show_desktop->pre_layout = [](Container *root, Container *c, const Bounds &b) {
            auto dock = (Dock *) root->user_data;
            auto mylar = dock->window;
            auto cr = mylar->raw_window->cr;
            c->wanted_bounds.w = 4 * mylar->raw_window->dpi;
        };
    }
    
};

static int current_alignment = 3;

static inline std::string trim(const std::string& s) {
    const char* ws = " \t\r\n";
    const auto b = s.find_first_not_of(ws);
    if (b == std::string::npos) return {};
    const auto e = s.find_last_not_of(ws);
    return s.substr(b, e - b + 1);
}

static void load_saved_pins_from_file(Container *icons) {
    const char *home = getenv("HOME");
    std::string itemsPath(home);
    itemsPath += "/.config/";
    
    if (mkdir(itemsPath.c_str(), S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH) == -1) {
        if (errno != EEXIST) {
            printf("Couldn't mkdir %s\n", itemsPath.c_str());
            return;
        }
    }
    
    itemsPath += "/mylar/";
    
    if (mkdir(itemsPath.c_str(), S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH) == -1) {
        if (errno != EEXIST) {
            printf("Couldn't mkdir %s\n", itemsPath.c_str());
            return;
        }
    }
    
    itemsPath += "pinned_items.ini";
    
    if (!std::filesystem::exists(itemsPath)) {
        //write_default_pinned_icons_file_if_none_exists(itemsPath);
    }

    std::ifstream in(itemsPath);
    std::string line;
    std::string class_name;
    std::string icon_name;
    std::string command;
    while (std::getline(in, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#')
            continue;

        if (line.front() == '[') {
            if (!class_name.empty())
                create_pinned_icon(icons, class_name, command, icon_name);
            class_name = "";
            icon_name = "";
            command = "";
            continue;
        }

        // key=value
        const auto eq = line.find('=');
        if (eq == std::string::npos)
            continue;

        const std::string key = trim(line.substr(0, eq));
        const std::string value = trim(line.substr(eq + 1));

        if (key == "class_name")
            class_name = value;
        else if (key == "icon_name")
            icon_name = value;
        else if (key == "command")
            command = value;
    }
    if (!class_name.empty())
        create_pinned_icon(icons, class_name, command, icon_name);
}

static void write_saved_pins_to_file(Container *icons) {
    const char* home = std::getenv("HOME");
    if (!home)
        return;
    std::filesystem::path filepath = std::filesystem::path(home) / ".config/mylar/pinned_items.ini";
    std::filesystem::create_directories(filepath.parent_path());
    std::ofstream out(filepath, std::ios::trunc);
    if (!out)
        return;
    
    std::map<std::string, bool> seen_before;
    int i = 0;
    for (auto icon: icons->children) {
        auto *data = static_cast<Pin *>(icon->user_data);
        
        if (!data)
            continue;
        if (!data->pinned)
            continue;
        if (seen_before.find(data->stacking_rule) != seen_before.end())
            continue;
        seen_before[data->stacking_rule] = true;
        
        out << "[PinnedIcon" << i++ << "]" << std::endl;
        
        out << "#The class_name is a property that windows set on themselves so that they "
                     "can be stacked with windows of the same kind as them. If when you click this "
                     "pinned icon button, it launches a window that creates an icon button that "
                     "doesn't stack with this one then the this wm_class is wrong and you're going "
                     "to have to fix it by running xprop in your console and clicking the window "
                     "that opened to find the real WM_CLASS that should be set."
                  << std::endl;
        out << "class_name=" << data->stacking_rule << std::endl;
        out << "icon_name=" << data->icon << std::endl;
        out << "command=" << data->command << std::endl << std::endl;
        out << std::endl;
    }
    
    out.close();
}

void dock_start(std::string monitor_name) {
    if (!monitor_name.empty()) {
        for (auto d : docks) {
            std::lock_guard<std::mutex> lock(d->app->mutex);

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
        if (auto icons = container_by_name("icons", dock->window->root)) {
            for (auto p : icons->children) {
                auto pin = (Pin *) p->user_data;
                pin->scale_change = true;
            }
        }
        //notify("scale change");
    };
    dock->window->root->skip_delete = true;
    dock->window->root->user_data = dock;
    fill_root(dock->window->root);
    if (auto icons = container_by_name("icons", dock->window->root))
        load_saved_pins_from_file(icons); 
    dock->window->root->alignment = ALIGN_RIGHT;
    docks.push_back(dock);
    windowing::main_loop(dock->app);
    if (docks.size() == 1)
        finished = true;
    if (finished) {
        if (auto icons = container_by_name("icons", dock->window->root))
            write_saved_pins_to_file(icons);
    }
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
            std::lock_guard<std::mutex> lock(d->app->mutex);

            windowing::close_app(d->app);
        }
        docks.clear();
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        cleanup_cached_fonts();   
    } else {
        for (auto d : docks) {
            std::lock_guard<std::mutex> lock(d->app->mutex);

            if (d->creation_settings.monitor_name == monitor_name) {
                windowing::close_app(d->app);
            }
        }
    } 
}

// This happens on the main thread, not the dock thread
std::string get_launch_command(int cid) {
    std::string command_launched_by_line;

    int pid = hypriso->get_pid(cid);
    if (pid != -1) {
        std::ifstream cmdline("/proc/" + std::to_string(pid) + "/cmdline");
        std::getline((cmdline), command_launched_by_line);
        
        size_t index = 0;
        while (true) {
            /* Locate the substring to replace. */
            index = command_launched_by_line.find('\000', index);
            if (index == std::string::npos)
                break;
            
            /* Make the replacement. */
            command_launched_by_line.replace(index, 1, " ");
            
            /* Advance index forward so the next iteration doesn't pick it up as well. */
            index += 1;
        }
    }
    
    return command_launched_by_line;
}

// This happens on the main thread, not the dock thread
void dock::add_window(int cid) {
    std::string command = get_launch_command(cid);
    std::string stack_rule = hypriso->class_name(cid);
    
    for (auto d : docks) {
        std::lock_guard<std::mutex> lock(d->app->mutex);
        if (auto c = container_by_name("icons", d->window->root)) {
            for (auto ch : c->children) {
                auto pin = (Pin *) ch->user_data;
                if (pin->stacking_rule == stack_rule) {
                    command = pin->command;
                    break;
                }
            }
        }
    }

    
    for (auto d : docks) {
        std::lock_guard<std::mutex> lock(d->app->mutex);

        // Check if cid should even be displayed in dock
        if (!hypriso->alt_tabbable(cid))
            return;

        Window *window = new Window;
        window->cid = cid;
        window->stack_rule = stack_rule;
        window->title = hypriso->title_name(cid);
        window->window_icon = hypriso->class_name(cid);
        window->command = command;
        
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
        std::lock_guard<std::mutex> lock(d->app->mutex);
        d->collection->to_be_removed.push_back(cid);
        windowing::redraw(d->window->raw_window);
    }
}

void dock::title_change(int cid, std::string title) {
    for (auto d : docks) {
        std::lock_guard<std::mutex> lock(d->app->mutex);
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
    for (auto d : docks) {
        std::lock_guard<std::mutex> lock(d->app->mutex);
        if (auto icons = container_by_name("icons", d->window->root)) {
            for (auto p : icons->children) {
                auto pin = (Pin *) p->user_data;
                for (int i = 0; i < pin->windows.size(); i++) {
                    if (pin->windows[i].cid == cid) {
                        auto copy = pin->windows[i];
                        pin->windows.erase(pin->windows.begin() + i);
                        pin->windows.insert(pin->windows.begin(), copy);
                        break;
                    }
                }
            }
        }
    }
    for (auto d : docks) {
        std::lock_guard<std::mutex> lock(d->app->mutex);
        windowing::redraw(d->window->raw_window);
    }
}

void dock::redraw() {
    for (auto d : docks) {
        std::lock_guard<std::mutex> lock(d->app->mutex);
        windowing::redraw(d->window->raw_window);
    }
}

void dock::toggle_dock_merge() {
    merge_windows = !merge_windows;
    auto order = get_window_stacking_order();
    for (auto cid : order) {
        remove_window(cid);
    }
    for (auto cid : order) {
        add_window(cid);
    }
    dock::redraw();
}

void dock::edit_pin(std::string original_stacking_rule, std::string new_stacking_rule, std::string new_icon, std::string new_command) {
    for (auto d : docks) {
        std::lock_guard<std::mutex> lock(d->app->mutex);
        if (auto icons = container_by_name("icons", d->window->root)) {
            for (auto p : icons->children) {
                auto pin = (Pin*)p->user_data;
                if (pin->stacking_rule == original_stacking_rule) {
                    pin->stacking_rule = new_stacking_rule;
                    pin->command = new_command;
                    pin->icon = new_icon;
                    pin->scale_change = true;
                }
            }
        }
    }
    //notify(fz("{} {} {} {}", original_stacking_rule, new_stacking_rule, new_command, new_icon));
}

Bounds dock::get_location(std::string name, int cid) {
    for (auto d : docks) {
        std::lock_guard<std::mutex> lock(d->app->mutex);
        if (d->creation_settings.monitor_name == name) {
            if (auto icons = container_by_name("icons", d->window->root)) {
                for (auto p : icons->children) {
                    auto pin = (Pin*)p->user_data;
                    for (auto w : pin->windows) {
                        if (w.cid == cid) {
                            Bounds b = p->real_bounds;
                            b.scale(1.0f / d->window->raw_window->dpi);
                            return b;
                        }
                    }
                }
            }
        }
    }
     
    return {0, 0, 100, 100};
}

static void set_volume(std::string uuid, float scalar) {
    if (scalar < 0)
        scalar = 0;
    if (scalar > 1)
        scalar = 1;

    audio([uuid, scalar]() {
        for (auto c : audio_clients) {
            if (c->uuid == uuid)
               c->set_volume(scalar);
        }
    });
}

static void add_title(Container *parent, std::string title, std::string uuid_target) {
    auto line = parent->child(FILL_SPACE, 40);
    line->pre_layout = [](Container *root, Container *c, const Bounds &b) {
        c->wanted_bounds.h = 40 * ((Dock *) root->user_data)->volume->raw_window->dpi;
    };
    line->when_paint = [title](Container *root, Container *c) {
        auto dock = (Dock *) root->user_data;
        auto cr = dock->volume->raw_window->cr;
        paint_button_bg(root, c);

        auto bounds = draw_text(cr, c, title, 12 * dock->volume->raw_window->dpi, false, "Segoe Fluent Icons");
        auto b = draw_text(cr,
            c->real_bounds.x + 10, c->real_bounds.y + c->real_bounds.h * .5 - bounds.h * .5,
            title, 12 * dock->volume->raw_window->dpi, true, "Segoe Fluent Icons", -1, -1, {0, 0, 0, 1});
    };
    line->when_clicked = [uuid_target](Container *root, Container *c) {
        float scalar = (root->mouse_current_x - c->real_bounds.x) / c->real_bounds.w;
        set_volume(uuid_target, scalar);
    };
    
    line->when_drag_start = [uuid_target](Container *root, Container *c) {
        float scalar = (root->mouse_current_x - c->real_bounds.x) / c->real_bounds.w;
        set_volume(uuid_target, scalar);
    };
    line->when_drag = [uuid_target](Container *root, Container *c) {
        float scalar = (root->mouse_current_x - c->real_bounds.x) / c->real_bounds.w;
        set_volume(uuid_target, scalar);
    };
    line->when_drag_end = [uuid_target](Container *root, Container *c) {
        float scalar = (root->mouse_current_x - c->real_bounds.x) / c->real_bounds.w;
        set_volume(uuid_target, scalar);
    };
}

static void fill_volume_root(const std::vector<AudioClient> clients, Container *root) {
    for (auto ch : root->children)
        delete ch;
    root->children.clear();

    root->type = ::vbox;
    auto current = get_current_time_in_ms();
    for (auto c : clients) {
        if (c.is_master && (current - last_time_volume_adjusted) > 100) {
            volume_level = std::round(c.get_volume() * 100);
        }
        add_title(root, fz("({}) {}", c.get_volume(), c.title), c.uuid);
    }
}

void dock::change_in_audio() {
    std::vector<AudioClient> clients;
    clients.reserve(audio_clients.size());
    for (auto client : audio_clients) {
        client->is_master = client->is_master_volume();
        clients.push_back(*client);
    }
    
    main_thread([clients = std::move(clients)]() {
        for (auto d : docks) {
            std::lock_guard<std::mutex> lock(d->app->mutex);

            if (d->volume)
                fill_volume_root(clients, d->volume->root);
            
            windowing::redraw(d->window->raw_window);
        }
    });
}


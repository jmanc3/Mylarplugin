#include "dock/dock.h"

#include "dock/dock_applications.h"
#include "dock/dock_battery.h"
#include "dock/dock_bluetooth.h"
#include "dock/dock_brightness.h"
#include "dock/dock_extra.h"
#include "dock/dock_projection.h"
#include "dock/dock_tiling.h"
#include "dock/dock_ui.h"
#include "dock/dock_volume.h"
#include "dock/dock_wifi.h"

#include "pango_font_cache.h"

#include "container.h"
#include "events.h"
#include "heart.h"
#include "dock_thumbnails.h"

#include "client/raw_windowing.h"
#include "client/windowing.h"
#include "overview.h"
#include "process.hpp"
#include "hypriso.h"
#include "icons.h"
#include "popup.h"
#include "titlebar.h"
#include "edit_pin.h"
#include "audio.h"
#include "components.h"
#include "hsluv.h"
#include "settings.h"
#include "text_editor.h"
#include "simple_dbus.h"

#include "process.hpp"
#include "show_desktop.h"
#include "datetime.h"
#include <algorithm>
#include <cairo.h>

#include <atomic>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <cmath>
#include <condition_variable>
#include <dirent.h>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <pango/pango-layout.h>
#include <pango/pango-types.h>
#include <pango/pangocairo.h>
#include <random>
#include <sstream>
#include <sys/stat.h>
#include <sys/wait.h>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <xkbcommon/xkbcommon-keysyms.h>

#define BTN_LEFT		0x110
#define BTN_RIGHT		0x111
#define BTN_MIDDLE		0x112
#define ICON(str) [](){ return str; }

// Persisted in ConfigSettings::dock_merge_windows. The dock thread reads this
// while the compositor/settings threads write it.
static std::atomic<bool> merge_windows{false};
static bool labels = true;
static int pixel_spacing = 1;
static float max_width = 200;
static container_alignment icon_alignment = container_alignment::ALIGN_LEFT;

static std::vector<std::thread> dock_threads;

static void write_saved_pins_to_file(Container *icons);

class Window {
public:
    int cid; // unique id
    bool on_workspace = false;

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
        on_workspace = w.on_workspace;
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

// How long a drag-and-drop must rest on a pinned icon before it raises the
// window (or opens the preview when the app has several).
static constexpr int kDndHoverMs = 450;

struct HoverTimerData {
    RawApp *app = nullptr;
    std::string uuid;
};

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
    double hover_amount = 0;
    double active_amount = 0;
    long background_animation_time = 0;
    double actual_w = 0;
    long animation_start_time = 0;

    int hover_timer_fd = -1;
    void *hover_timer_data = nullptr;
    bool suppress_hover_preview = false;

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

static std::vector<Dock *> docks;

// Read compositor state on the main thread, before the dock thread groups windows.
static bool window_on_dock_workspace(Dock *dock, int cid) {
    if (!set->dock_current_workspace_only)
        return true;
    const int monitor = get_monitor(cid);
    if (monitor == -1)
        return false;
    if (hypriso->monitor_name(monitor) != dock->creation_settings.monitor_name)
        return false;
    const int workspace = hypriso->get_active_workspace_id(monitor);
    return workspace != -1 && hypriso->get_active_workspace_id_client(cid) == workspace;
}

static int active_cid = -1;

// {"anchors":[{"x":0,"y":1},{"x":2,"y":1}],"controls":[{"x":0.9860918958926828,"y":-0.9725501058797109}]}
std::vector<float> throb = { 0.000, 0.066, 0.129, 0.190, 0.248, 0.305, 0.359, 0.411, 0.461, 0.508, 0.553, 0.596, 0.637, 0.675, 0.711, 0.745, 0.776, 0.806, 0.833, 0.858, 0.881, 0.901, 0.920, 0.936, 0.949, 0.961, 0.970, 0.978, 0.983, 0.986, 0.986, 0.985, 0.981, 0.975, 0.967, 0.957, 0.944, 0.930, 0.913, 0.894, 0.873, 0.849, 0.824, 0.796, 0.766, 0.735, 0.701, 0.664, 0.626, 0.585, 0.543, 0.498, 0.451, 0.402, 0.351, 0.298, 0.243, 0.185, 0.125, 0.064, 0.000 };

float brightness_level = 100;
std::atomic<bool> finished = false;

struct BrightnessData : UserData {
    float value = 50;
};

static void make_vert_space(Container *parent, int wanted_h, std::function<MylarWindow * (Dock *)> get_window) {
    auto pad = parent->child(8, 8);
    pad->pre_layout = [wanted_h, get_window](Container *root, Container *c, const Bounds &b) {
        auto dock = (Dock *) root->user_data;
        auto window = get_window(dock);
        auto dpi = window->raw_window->dpi;
        auto cr = window->raw_window->drawing_context;
        c->wanted_bounds.h = wanted_h * dpi;
    };
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

// Brightness shift used to build the subtle left-to-right shading of the
// pinned icon background and highlight bar (ported from winbar's darken()).
static RGBA darken_dock_color(RGBA color, double amount) {
    double h; // hue
    double s; // saturation
    double p; // perceived brightness
    rgb2hsluv(color.r, color.g, color.b, &h, &s, &p);
    p = std::clamp(p - amount, 0.0, 100.0);
    hsluv2rgb(h, s, p, &color.r, &color.g, &color.b);
    return color;
}

// Paints a rectangle as three horizontal bands, leaving a slightly darker
// strip on the right side. This mirrors winbar's paint_double_bg /
// paint_double_bar helpers. The bands are composited in a group so their
// semi-transparent colors replace each other instead of double-blending.
static void paint_dock_horizontal_bands(drawing::Context *cr, const Bounds &bounds, double dpi,
                                        const RGBA &left, const RGBA &middle, const RGBA &right) {
    if (bounds.w <= 0 || bounds.h <= 0)
        return;

    auto fill = [&](const Bounds &b, const RGBA &color) {
        if (b.w <= 0 || b.h <= 0)
            return;
        cr->set_source_rgba(color.r, color.g, color.b, color.a);
        cr->rectangle(b.x, b.y, b.w, b.h);
        cr->fill();
    };

    cr->save();
    // Keep the temporary group from being the size of the entire dock.
    cr->rectangle(bounds.x, bounds.y, bounds.w, bounds.h);
    cr->clip_path();
    cr->push_group();
    cr->set_operator(CAIRO_OPERATOR_SOURCE);

    Bounds b = bounds;
    fill(b, right);

    b.w -= std::round(3 * dpi);
    if (b.w > 0) {
        fill(b, middle);

        b.w -= std::round(1 * dpi);
        if (b.w > 0)
            fill(b, left);
    }

    cr->pop_group_to_source();
    cr->set_operator(CAIRO_OPERATOR_OVER);
    cr->paint_source();
    cr->restore();
}

static void paint_dock_icon_pane(drawing::Context *cr, const Bounds &bounds, double dpi,
                                 const RGBA &left, const RGBA &middle, const RGBA &right) {
    paint_dock_horizontal_bands(cr, bounds, dpi, left, middle, right);
}

static void paint_dock_highlight_bar(drawing::Context *cr, const Bounds &bounds, double dpi, double bar_amount,
                                     const RGBA &left, const RGBA &middle, const RGBA &right) {
    bar_amount = std::clamp(bar_amount, 0.0, 1.0);

    Bounds bar = bounds;
    bar.h = std::round(2 * dpi);
    bar.y = bounds.y + bounds.h - bar.h;

    const double bar_inset = std::round(4 * dpi) * (1 - bar_amount);
    bar.x += bar_inset;
    bar.w -= bar_inset * 2;

    paint_dock_horizontal_bands(cr, bar, dpi, left, middle, right);
}

static void paint_root_func(Container *root, Container *c) {
    auto dock = (Dock *) root->user_data;
    auto mylar = dock->window;
    auto cr = mylar->raw_window->drawing_context;
    cr->save();
    cr->set_operator(CAIRO_OPERATOR_CLEAR);
    cr->paint_source();
    cr->restore();

    //cr->rectangle(root->real_bounds.x, root->real_bounds.y, root->real_bounds.w, std::round(1 * mylar->raw_window->dpi));
    //cr->set_source_rgba(1, 1, 1, .1);
    //cr->fill();

    cr->rectangle(root->real_bounds.x, root->real_bounds.y, root->real_bounds.w, root->real_bounds.h);
    auto color = color_dock_color();
    cr->set_source_rgba(color.r, color.g, color.b, color.a);
    cr->fill();
}

static void paint_label(Container *root, Container *c, std::string text) {
    auto mylar = (MylarWindow*)root->user_data;
    auto cr = mylar->raw_window->drawing_context;
    auto dpi = mylar->raw_window->dpi;
    auto size = 12 * dpi;

    auto b = draw_text(cr, 0, 0, text, size, false, set->font, -1, 0, {0, 0, 0, 1}, false);
    draw_text(cr,
        c->real_bounds.x + 12 * dpi,
        c->real_bounds.y + c->real_bounds.h * .5 - b.h * .5, text, size, true, set->font, -1, 0, {0, 0, 0, 1},  false);
}

static void paint_button_bg(Container *root, Container *c) {
    auto dock = (Dock *) root->user_data;
    auto mylar = dock->window;
    auto cr = mylar->raw_window->drawing_context;
    if (c->state.mouse_pressing) {
        cr->rectangle(c->real_bounds.x, c->real_bounds.y, c->real_bounds.w, c->real_bounds.h);
        cr->set_source_rgba(1, 1, 1, .2);
        cr->fill();
    } else if (c->state.mouse_hovering) {
        cr->rectangle(c->real_bounds.x, c->real_bounds.y, c->real_bounds.w, c->real_bounds.h);
        cr->set_source_rgba(1, 1, 1, .3);
        cr->fill();
    }
}

static std::string get_date() {
    auto local = datetime::now_tm();
    char weekday[32], date[64];
    std::strftime(weekday, sizeof(weekday), "%A", &local);
    std::strftime(date, sizeof(date), "%B %m/%d/%Y", &local);
    return datetime::clock_text(local) + " — " + weekday + "\n" + date;
}

float get_brightness() {
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

void set_brightness(float amount) {
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

Container *simple_dock_item(Container *root, std::function<std::string()> ico, std::function<std::string()> text = nullptr,
                            std::function<std::string()> layout_text = nullptr) {
    if (!layout_text)
        layout_text = text;
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
    c->when_paint = [ico, text, layout_text](Container *root, Container *c) {
        auto dock = (Dock *) root->user_data;
        auto mylar = dock->window;
        auto cr = mylar->raw_window->drawing_context;
        paint_button_bg(root, c);

        auto ico_bounds = draw_text(cr, c, ico(), 12 * mylar->raw_window->dpi, false, icon_font);
        auto b = draw_text(cr,
            c->real_bounds.x + 10, c->real_bounds.y + c->real_bounds.h * .5 - ico_bounds.h * .5,
            ico(), 12 * mylar->raw_window->dpi, true, icon_font);
        if (text) {
            auto visual_text = text();
            auto tb = draw_text(cr, c, visual_text, tex_size * mylar->raw_window->dpi, false);
            auto layout_bounds = draw_text(cr, c, layout_text(), tex_size * mylar->raw_window->dpi, false);
            draw_text(cr,
                c->real_bounds.x + 20 + b.w + (layout_bounds.w - tb.w) * .5,
                c->real_bounds.y + c->real_bounds.h * .5 - tb.h * .5,
                visual_text, tex_size * mylar->raw_window->dpi, true);
        }
    };
    c->pre_layout = [ico, layout_text](Container *root, Container *c, const Bounds &b) {
        auto dock = (Dock *) root->user_data;
        auto mylar = dock->window;
        auto cr = mylar->raw_window->drawing_context;
        auto bounds = draw_text(cr, c, ico(), 12 * mylar->raw_window->dpi, false, icon_font);
        if (layout_text)
            bounds.w += draw_text(cr, c, layout_text(), tex_size * mylar->raw_window->dpi, false).w + 10;

        c->wanted_bounds.w = bounds.w + 20;
    };

    return c;
}

static void icons_right_click(int startoff, int cw, std::string uuid, float dpi, float yoff) {
    main_thread([startoff, cw, uuid, dpi, yoff] {
        auto m = mouse();
        std::vector<PopOption> root;
        {
            PopOption pop;
            pop.text = "Add a pin";
            pop.icon_left = "\ue70f";
            pop.is_text_icon = true;
            pop.on_clicked = []() {
                edit_pin::open("", "", "");
            };
            root.push_back(pop);
        }
        {
            PopOption pop;
            pop.text = "Settings";
            pop.icon_left = "\uE713";
            pop.is_text_icon = true;
            pop.on_clicked = []() {
                settings::start();
            };
            root.push_back(pop);
        }
        popup::open(root,
            m.x - (277 * .5) + 1.4,
            m.y - (yoff / dpi) - 5 - (24 * root.size() * dpi));
    });
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
        if (pin->pinned) {
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
                dock::redraw();
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

static void draw_clip_begin(drawing::Context *cr, const Bounds &b) {
    cr->save();
    set_rect(cr, b);
    cr->clip_path();
}

void draw_clip_end(drawing::Context *cr) {
    cr->reset_clip();
    cr->restore();
}

static Bounds pin_thumbnail_bounds(Container *pin_container, Dock *dock, bool use_layout_target = false) {
    Bounds b = pin_container->real_bounds;
    auto *pin = (Pin *) pin_container->user_data;
    // While the dock animates a reflow, anchor to the position the icon is
    // moving to so the preview ends up on the icon instead of its old spot.
    if (use_layout_target && pin && pin->natural_position_x != INT_MAX)
        b.x = pin->natural_position_x;
    if (dock && dock->window && dock->window->raw_window && dock->window->raw_window->dpi > 0)
        b.scale(1.0f / dock->window->raw_window->dpi);
    return b;
}

static std::vector<int> pin_window_ids(Pin *pin) {
    std::vector<int> ids;
    ids.reserve(pin->windows.size());
    for (const auto &window : pin->windows)
        ids.push_back(window.cid);
    // Previews keep a stable creation-time order. pin->windows is reordered by
    // focus (see dock::on_activated) for title/activation purposes, but the
    // thumbnail strip should not jump around when the active window changes.
    std::sort(ids.begin(), ids.end());
    return ids;
}

static void cancel_hover_timer(Dock *dock, Pin *pin) {
    if (pin->hover_timer_fd != -1 && dock) {
        windowing::timer_stop(dock->app, pin->hover_timer_fd);
        pin->hover_timer_fd = -1;
    }
    delete (HoverTimerData *) pin->hover_timer_data;
    pin->hover_timer_data = nullptr;
}

static void sync_thumbnails(Dock *dock, Container *icons) {
    for (auto pin_container : icons->children) {
        auto *pin = (Pin *) pin_container->user_data;
        if (!pin)
            continue;
        auto ids = pin_window_ids(pin);
        auto icon = pin_thumbnail_bounds(pin_container, dock, true);
        const auto source = pin_container->uuid;
        main_thread([source, ids = std::move(ids), icon]() {
            dock_thumbnails::refresh_if_open(source, ids, icon);
        });
    }
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
        if (!dock || !dock->window || !dock->window->raw_window || !dock->window->raw_window->drawing_context)
            return;

        Pin* pin = (Pin*)c->user_data;
        auto mylar = dock->window;
        auto cr = mylar->raw_window->drawing_context;

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

        const int windows_count = (int) pin->windows.size();
        const bool pressed = c->state.mouse_pressing;
        const bool hovered = c->state.mouse_hovering;
        const double animation_seconds = std::clamp((get_current_time_in_ms() - pin->background_animation_time) / 1000.0, 0.0, 0.05);
        pin->background_animation_time = get_current_time_in_ms();
        const double animation_step = animation_seconds * 12.0;
        pin->hover_amount += std::clamp((hovered ? 1.0 : 0.0) - pin->hover_amount, -animation_step, animation_step);
        pin->active_amount += std::clamp((is_active ? 1.0 : 0.0) - pin->active_amount, -animation_step, animation_step);
        const double eased_active = pin->active_amount * pin->active_amount * (3.0 - 2.0 * pin->active_amount);

        // Ported from winbar's paint_icon_background: the background pane is
        // only shown while pressed/hovered, or while one of the pinned windows
        // is active. It gets a subtle left-to-right shading compared to the
        // flat rectangle the dock used to draw.
        if (pressed || pin->hover_amount > 0.001 || pin->active_amount > 0.001) {
            RGBA pane_left, pane_middle, pane_right;
            if (pressed) {
                auto base = color_dock_sel_press_color();
                pane_left = darken_dock_color(base, 20);
                pane_middle = windows_count > 1 ? darken_dock_color(base, 36) : pane_left;
                pane_right = windows_count > 1 ? darken_dock_color(base, 31) : pane_left;
            } else if (hovered) {
                auto base = color_dock_sel_hover_color();
                pane_left = darken_dock_color(base, 15);
                pane_middle = windows_count > 1 ? darken_dock_color(base, 34) : pane_left;
                pane_right = windows_count > 1 ? darken_dock_color(base, 28) : pane_left;
            } else if (pin->hover_amount > pin->active_amount) {
                auto base = color_dock_sel_active_color();
                auto target_left = darken_dock_color(color_dock_sel_hover_color(), 15);
                auto target_middle = windows_count > 1 ? darken_dock_color(color_dock_sel_hover_color(), 34) : target_left;
                auto target_right = windows_count > 1 ? darken_dock_color(color_dock_sel_hover_color(), 28) : target_left;
                pane_left = target_left; pane_middle = target_middle; pane_right = target_right;
                (void)base;
            } else {
                auto base = color_dock_sel_active_color();
                pane_left = darken_dock_color(base, 10);
                pane_middle = windows_count > 1 ? darken_dock_color(pane_left, 20) : pane_left;
                pane_right = windows_count > 1 ? darken_dock_color(pane_left, 15) : pane_left;
            }
            paint_dock_icon_pane(cr, c->real_bounds, mylar->raw_window->dpi,
                                 pane_left, pane_middle, pane_right);
        }
        auto none = pin->windows.empty();
        if (!labels || merge_windows.load())
            none = true;

        int offx = 0;
        if (icons_loaded) {
            auto ico_surf = pin->icon_surf;
            if (ico_surf) {
                auto h = cairo_image_surface_get_height(ico_surf);
                auto w = cairo_image_surface_get_width(ico_surf);
                if (none) {
                    cr->set_source_surface(ico_surf, std::round(c->real_bounds.x + c->real_bounds.w * .5 - w * .5), std::round(c->real_bounds.y + c->real_bounds.h * .5 - h * .5));
                } else {
                    cr->set_source_surface(ico_surf, std::round(c->real_bounds.x + 10), std::round(c->real_bounds.y + c->real_bounds.h * .5 - h * .5));
                }
                cr->paint_source();
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
                auto b = draw_text(cr, c, title, 9 * mylar->raw_window->dpi, false, set->font, text_w, c->real_bounds.h * PANGO_SCALE);
                draw_clip_begin(cr, bc);
                draw_text(cr, c->real_bounds.x + 10, c->real_bounds.y + c->real_bounds.h * .5 - b.h * .5, title, 9 * mylar->raw_window->dpi, true, set->font, text_w,
                          c->real_bounds.h * PANGO_SCALE);
                draw_clip_end(cr);
            } else {
                auto text_w = (pin->actual_w - offx - 30) * PANGO_SCALE;
                auto b = draw_text(cr, c, title, 9 * mylar->raw_window->dpi, false, set->font, text_w, c->real_bounds.h * PANGO_SCALE);
                draw_clip_begin(cr, bc);
                draw_text(cr, c->real_bounds.x + offx + 20, c->real_bounds.y + c->real_bounds.h * .5 - b.h * .5, title, 9 * mylar->raw_window->dpi, true, set->font, text_w, c->real_bounds.h * PANGO_SCALE);
                draw_clip_end(cr);
            }
        }

        if (windows_count > 0) {
            auto accent_color = color_dock_sel_accent_color();
            auto bar_left = accent_color;
            auto bar_middle = windows_count > 1 ? darken_dock_color(accent_color, 20) : accent_color;
            auto bar_right = windows_count > 1 ? darken_dock_color(accent_color, 15) : accent_color;
            paint_dock_highlight_bar(cr, c->real_bounds, mylar->raw_window->dpi, std::max(pin->hover_amount, eased_active),
                                     bar_left, bar_middle, bar_right);
        }
    };
    ch->when_mouse_enters_container = paint {
        Pin *pin = (Pin *) c->user_data;
        auto dock = (Dock *) root->user_data;
        if (!dock)
            return;

        // A drag-and-drop is passing over the icon. After a short hover,
        // raise its window so the drop can land there, or show the preview
        // so the user can pick which of its windows to raise.
        if (dock->window && dock->window->raw_window && dock->window->raw_window->dnd_hovering) {
            if (pin->windows.empty() || pin->hover_timer_fd != -1)
                return;
            auto *hover = new HoverTimerData{dock->app, c->uuid};
            pin->hover_timer_data = hover;
            pin->hover_timer_fd = windowing::timer(dock->app, kDndHoverMs, [](void *data) {
                auto *hover = (HoverTimerData *) data;
                std::vector<int> ids;
                Bounds icon;
                std::string source;
                bool still_dragging = false;
                for (auto d : docks) {
                    if (d->app != hover->app)
                        continue;
                    if (auto icons = container_by_name("icons", d->window->root)) {
                        for (auto p : icons->children) {
                            if (p->uuid != hover->uuid)
                                continue;
                            auto *pin = (Pin *) p->user_data;
                            ids = pin_window_ids(pin);
                            icon = pin_thumbnail_bounds(p, d);
                            source = p->uuid;
                            still_dragging = p->state.mouse_hovering && d->window->raw_window->dnd_hovering;
                            pin->hover_timer_fd = -1;
                            pin->hover_timer_data = nullptr;
                            break;
                        }
                    }
                    break;
                }
                delete hover;
                if (ids.empty() || !still_dragging)
                    return;
                if (ids.size() == 1) {
                    const int cid = ids[0];
                    main_thread([cid]() {
                        if (overview::is_showing())
                            return;
                        if (show_desktop::is_opened())
                            show_desktop::stop_animation();
                        if (hypriso->is_hidden(cid))
                            hypriso->set_hidden(cid, false, true);
                        hypriso->bring_to_front(cid);
                        hypriso->hold_focus_during_dnd(cid);
                        dock_thumbnails::open({});
                    });
                } else {
                    main_thread([ids = std::move(ids), icon, source = std::move(source)]() {
                        dock_thumbnails::open(ids, icon, source);
                    });
                }
            }, hover);
            return;
        }

        if (pin->suppress_hover_preview)
            return;

        // The cursor returned to the icon, so keep any visible preview alive.
        // If a preview is already showing, switch it to this icon immediately
        // instead of waiting for the hover timer.
        auto ids = pin_window_ids(pin);
        auto icon = pin_thumbnail_bounds(c, dock);
        const auto source = c->uuid;
        main_thread([source, ids = std::move(ids), icon]() {
            dock_thumbnails::keep_open();
            dock_thumbnails::refresh_if_open(source, ids, icon, true);
        });

        if (pin->windows.empty()) {
            // Nothing to preview; drop a preview from a previous icon.
            main_thread([]() { dock_thumbnails::open({}); });
            return;
        }

        if (pin->hover_timer_fd != -1)
            return;

        auto *hover = new HoverTimerData{dock->app, c->uuid};
        pin->hover_timer_data = hover;
        pin->hover_timer_fd = windowing::timer(dock->app, 450, [](void *data) {
            auto *hover = (HoverTimerData *) data;
            std::vector<int> ids;
            Bounds icon;
            std::string source;
            for (auto d : docks) {
                if (d->app != hover->app)
                    continue;
                if (auto icons = container_by_name("icons", d->window->root)) {
                    for (auto p : icons->children) {
                        if (p->uuid != hover->uuid)
                            continue;
                        auto *pin = (Pin *) p->user_data;
                        ids = pin_window_ids(pin);
                        icon = pin_thumbnail_bounds(p, d);
                        source = p->uuid;
                        pin->hover_timer_fd = -1;
                        pin->hover_timer_data = nullptr;
                        break;
                    }
                }
                break;
            }
            delete hover;
            if (ids.empty())
                return;
            main_thread([ids = std::move(ids), icon, source = std::move(source)]() {
                dock_thumbnails::open(ids, icon, source);
            });
        }, hover);
    };
    ch->when_mouse_leaves_container = paint {
        Pin *pin = (Pin *) c->user_data;
        auto dock = (Dock *) root->user_data;
        cancel_hover_timer(dock, pin);
        pin->suppress_hover_preview = false;
        main_thread([]() { dock_thumbnails::close(); });
    };
    ch->when_clicked = paint {
        Pin* pin = (Pin*)c->user_data;
        auto dock = (Dock*)root->user_data;
        auto mylar = dock->window;
        auto cr = mylar->raw_window->drawing_context;

        const auto windows_count = pin->windows.size();
        if (c->state.mouse_button_pressed == BTN_LEFT) {
            cancel_hover_timer(dock, pin);
            if (windows_count > 1) {
                auto ids = pin_window_ids(pin);
                auto icon = pin_thumbnail_bounds(c, dock);
                auto source = c->uuid;
                main_thread([ids = std::move(ids), icon, source = std::move(source)]() {
                    dock_thumbnails::open(ids, icon, source);
                });
            } else {
                // Suppress hover scheduling until the pointer leaves and
                // re-enters, including when this click dismisses an open preview.
                pin->suppress_hover_preview = true;
                main_thread([]() { dock_thumbnails::open({}); });
            }
        } else {
            main_thread([]() { dock_thumbnails::open({}); });
        }

        if (pin->windows.empty() && c->state.mouse_button_pressed == BTN_LEFT) {
            launch_command(pin->command);
            return;
        }

        int cid = -1;
        if (!pin->windows.empty())
            cid = pin->windows[0].cid;

        if (c->state.mouse_button_pressed == BTN_LEFT) {
            main_thread([cid] {
                if (overview::is_showing())
                    return;
                if (show_desktop::is_opened()) {
                    show_desktop::stop_animation();
                    hypriso->set_hidden(cid, false, false);
                    hypriso->bring_to_front(cid);
                    return;
                }

                // todo we need to focus next (already wrote this combine code)
                bool is_hidden = hypriso->is_hidden(cid);
                if (is_hidden) {
                    hypriso->set_hidden(cid, false, true);
                    hypriso->bring_to_front(cid);
                } else {
                    if (active_cid == cid) {
                        hypriso->set_hidden(cid, true, true);

                        auto order = get_window_stacking_order();
                        std::reverse(order.begin(), order.end());
                        bool focused_next = false;
                        for (auto candidate : order) {
                            if (candidate == cid || hypriso->is_hidden(candidate) || !hypriso->alt_tabbable(candidate))
                                continue;

                            hypriso->bring_to_front(candidate);
                            focused_next = true;
                            break;
                        }

                        if (!focused_next)
                            hypriso->all_lose_focus();
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
        auto cr = mylar->raw_window->drawing_context;

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
                //"application-x-executable"
                if (!full.empty()) {
                    pin->full_icon = full;
                    load_icon_full_path(&pin->icon_surf, full, size);
                } else {
                    for (auto w : pin->windows) {
                        auto cid = w.cid;
                        // Take the icon from the window
                        cairo_surface_t *surface = hypriso->get_icon_on_window(cid, size);
                        if (!surface)
                            continue;
                        if (pin->icon_surf)
                            cairo_surface_destroy(pin->icon_surf);
                        pin->icon_surf = surface;
                        break;
                    }
                }
            }
        }
    };
    ch->when_drag_start = paint {
        auto data = (Pin *) c->user_data;
        data->initial_mouse_click_before_drag_offset_x = c->real_bounds.x - root->mouse_initial_x;
        c->z_index = 1;

        // Dragging replaces hovering: cancel a preview that is still queued to
        // open, and dismiss one that is already visible so it does not stay
        // anchored to the icon as it moves across the dock.
        auto dock = (Dock *) root->user_data;
        cancel_hover_timer(dock, data);
        main_thread([]() { dock_thumbnails::open({}); });
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
    const bool merging = merge_windows.load();

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

    // Remove closed windows and windows outside this dock's active workspace.
    for (int pin_index = icons->children.size() - 1; pin_index >= 0; pin_index--) {
        auto pin_container = icons->children[pin_index];
        auto pin = (Pin *) pin_container->user_data;

        // Keep other workspaces in the collection so they return when switching back.
        for (int window_index = pin->windows.size() -1; window_index >= 0; window_index--) {
            auto window = pin->windows[window_index];
            bool window_still_exists = false;
            for (auto existing_window : dock->collection->list) {
                if (existing_window->cid == window.cid && existing_window->on_workspace) {
                    window_still_exists = true;
                }
            }

            if (!window_still_exists) {
                pin->windows.erase(pin->windows.begin() + window_index);
                dock->thumbnails_dirty = true;
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
            } else if (!merging) {
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
        if (!window->on_workspace)
            continue;
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
                    if (merging) {
                        needs_to_create_its_own_pin = false;
                        pin->windows.push_back(*window);
                        dock->thumbnails_dirty = true;
                        break;
                    } else if (pin->windows.empty() && pin->pinned) {
                        // If we are not merging windows, we still need to group up with pinned no windows item
                        needs_to_create_its_own_pin = false;
                        pin->windows.push_back(*window);
                        dock->thumbnails_dirty = true;
                        break;
                    }
                }
            }

            if (needs_to_create_its_own_pin) {
                create_pinned_icon(icons, window->stack_rule, window->command, window->window_icon, window);
                dock->thumbnails_dirty = true;
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
        dock->thumbnails_dirty = true;
        if (auto icons = container_by_name("icons", dock->window->root)) {
            for (auto p : icons->children) {
                auto pin = (Pin *) p->user_data;
                pin->wants_reposition_animation = true;
            }
        }
    }
}

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
        if (pin->windows.empty() || !labels || merge_windows.load()) {
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
        if (data->animating || std::abs(data->hover_amount - (c->state.mouse_hovering ? 1.0 : 0.0)) > 0.001 ||
            std::abs(data->active_amount - ([&] {
                for (const auto &window : data->windows)
                    if (window.cid == active_cid)
                        return 1.0;
                return 0.0;
            })()) > 0.001) {
            needs_refresh = true;
            break;
        }
    }

    if (needs_refresh) {
        windowing::redraw(dock->window->raw_window);
    }
}

struct DockDateCell : UserData {
    int day = 1;
    int month = 0;
    int year = 0;
};

static void update_dock_calendar(Dock *dock) {
    if (!dock || !dock->date_menu)
        return;
    auto *calendar = container_by_name("date_calendar", dock->date_menu->root);
    if (!calendar)
        return;

    std::tm first{};
    first.tm_year = dock->date_view_year - 1900;
    first.tm_mon = dock->date_view_month;
    first.tm_mday = 1;
    std::mktime(&first);
    const int leading_days = first.tm_wday;

    for (int i = 0; i < 42; ++i) {
        auto *cell = calendar->children[i / 7]->children[i % 7];
        auto *data = static_cast<DockDateCell *>(cell->user_data);
        const int date_number = i - leading_days + 1;
        std::tm date{};
        date.tm_year = dock->date_view_year - 1900;
        date.tm_mon = dock->date_view_month;
        date.tm_mday = date_number;
        std::mktime(&date);
        data->day = date.tm_mday;
        data->month = date.tm_mon;
        data->year = date.tm_year + 1900;
    }
    windowing::redraw(dock->date_menu->raw_window);
}

static void fill_date_menu(Dock *dock) {
    auto root = dock->date_menu->root;
    const auto dpi = dock->date_menu->raw_window->dpi;
    root->type = ::vbox;
    root->when_paint = [](Container *root, Container *c) {
        auto dock = static_cast<Dock *>(root->user_data);
        paint_popup_background(dock->date_menu->raw_window->drawing_context, c->real_bounds, dock->date_menu->raw_window->dpi);
    };
    auto content = root->child(::vbox, FILL_SPACE, FILL_SPACE);
    content->pre_layout = [](Container *root, Container *c, const Bounds &) {
        auto dock = static_cast<Dock *>(root->user_data);
        const auto dpi = dock->date_menu->raw_window->dpi;
        c->wanted_pad = Bounds(20 * dpi, 16 * dpi, 20 * dpi, 16 * dpi);
    };
    auto row = [content, dpi](double height) {
        auto c = content->child(FILL_SPACE, height * dpi);
        c->pre_layout = [height](Container *root, Container *c, const Bounds &) {
            auto dock = static_cast<Dock *>(root->user_data);
            c->wanted_bounds.h = height * dock->date_menu->raw_window->dpi;
        };
        return c;
    };

    auto title = row(88);
    title->when_paint = [](Container *root, Container *c) {
        auto dock = static_cast<Dock *>(root->user_data);
        auto cr = dock->date_menu->raw_window->drawing_context;
        const auto dpi = dock->date_menu->raw_window->dpi;
        const auto local = datetime::now_tm();
        char date_text[96];
        std::string clock_text = datetime::clock_text(local);
        std::strftime(date_text, sizeof(date_text), "%A, %B %d, %Y", &local);
        draw_text(cr, c->real_bounds.x, c->real_bounds.y + 2 * dpi, clock_text, 27 * dpi, true,
            set->font, -1, -1, {.14, .18, .24, 1});
        draw_text(cr, c->real_bounds.x, c->real_bounds.y + 47 * dpi, date_text, 11 * dpi, true,
            set->font, -1, -1, {.43, .48, .55, 1});
        auto border_c = border_color;
        border_c.a *= .2;
        set_argb(cr, border_c);
        cr->rectangle(c->real_bounds.x, c->real_bounds.bottom() - dpi, c->real_bounds.w, dpi);
        cr->fill();
    };

    auto navigation = row(38);
    navigation->type = ::hbox;
    auto month = navigation->child(FILL_SPACE, FILL_SPACE);
    month->when_paint = [](Container *root, Container *c) {
        auto dock = static_cast<Dock *>(root->user_data);
        static const char *months[] = {"January", "February", "March", "April", "May", "June",
            "July", "August", "September", "October", "November", "December"};
        const auto label = std::format("{} {}", months[dock->date_view_month], dock->date_view_year);
        auto dpi = dock->date_menu->raw_window->dpi;
        auto bounds = draw_text(dock->date_menu->raw_window->drawing_context, 0, 0, label, 13 * dpi, false);
        draw_text(dock->date_menu->raw_window->drawing_context, c->real_bounds.x,
            c->real_bounds.y + (c->real_bounds.h - bounds.h) / 2, label, 13 * dpi, true,
            set->font, -1, -1, {.14, .18, .24, 1});
    };
    auto arrow = [navigation, dock, dpi](const char *glyph, int direction) {
        auto button = navigation->child(48 * dpi, FILL_SPACE);
        button->when_paint = [glyph](Container *root, Container *c) {
            auto dock = static_cast<Dock *>(root->user_data);
            auto cr = dock->date_menu->raw_window->drawing_context;
            const auto dpi = dock->date_menu->raw_window->dpi;
            if (c->state.mouse_hovering || c->state.mouse_pressing) {
                set_argb(cr, c->state.mouse_pressing ? RGBA(.72, .83, .95, 1) : RGBA(.83, .90, .98, 1));
                cr->rectangle(c->real_bounds.x, c->real_bounds.y, c->real_bounds.w, c->real_bounds.h);
                cr->fill();
            }
            auto b = draw_text(cr, 0, 0, glyph, 12 * dpi, false, icon_font);
            draw_text(cr, c->real_bounds.x + (c->real_bounds.w - b.w) / 2,
                c->real_bounds.y + (c->real_bounds.h - b.h) / 2, glyph, 12 * dpi, true,
                icon_font, -1, -1, {.14, .18, .24, 1});
        };
        button->when_clicked = [direction](Container *root, Container *) {
            auto dock = static_cast<Dock *>(root->user_data);
            dock->date_view_month += direction;
            if (dock->date_view_month < 0) {
                dock->date_view_month = 11;
                --dock->date_view_year;
            } else if (dock->date_view_month > 11) {
                dock->date_view_month = 0;
                ++dock->date_view_year;
            }
            update_dock_calendar(dock);
        };
    };
    arrow("\uE971", -1);
    arrow("\uE972", 1);

    auto weekdays = row(28);
    weekdays->type = ::hbox;
    static const char *weekday_names[] = {"Su", "Mo", "Tu", "We", "Th", "Fr", "Sa"};
    for (const auto *name : weekday_names) {
        auto day = weekdays->child(FILL_SPACE, FILL_SPACE);
        day->when_paint = [name](Container *root, Container *c) {
            auto dock = static_cast<Dock *>(root->user_data);
            auto cr = dock->date_menu->raw_window->drawing_context;
            auto dpi = dock->date_menu->raw_window->dpi;
            auto b = draw_text(cr, 0, 0, name, 10 * dpi, false);
            draw_text(cr, c->real_bounds.x + (c->real_bounds.w - b.w) / 2,
                c->real_bounds.y + (c->real_bounds.h - b.h) / 2, name, 10 * dpi, true,
                set->font, -1, -1, {.43, .48, .55, 1});
        };
    }

    auto calendar = content->child(::vbox, FILL_SPACE, FILL_SPACE);
    calendar->name = "date_calendar";
    calendar->spacing = 2 * dpi;
    for (int week = 0; week < 6; ++week) {
        auto week_row = calendar->child(::hbox, FILL_SPACE, FILL_SPACE);
        week_row->spacing = 2 * dpi;
        for (int weekday = 0; weekday < 7; ++weekday) {
            auto cell = week_row->child(FILL_SPACE, FILL_SPACE);
            cell->when_paint = [](Container *root, Container *c) {
                auto dock = static_cast<Dock *>(root->user_data);
                auto data = static_cast<DockDateCell *>(c->user_data);
                auto cr = dock->date_menu->raw_window->drawing_context;
                const auto dpi = dock->date_menu->raw_window->dpi;
                const auto now = std::time(nullptr);
                std::tm today{};
                localtime_r(&now, &today);
                const bool is_today = data->year == today.tm_year + 1900 && data->month == today.tm_mon && data->day == today.tm_mday;
                const bool in_month = data->year == dock->date_view_year && data->month == dock->date_view_month;
                const bool is_selected = data->year == dock->selected_date_year && data->month == dock->selected_date_month && data->day == dock->selected_date_day;
                if (is_today) {
                    set_argb(cr, accent);
                    cr->rectangle(c->real_bounds.x + dpi, c->real_bounds.y + dpi,
                        c->real_bounds.w - 2 * dpi, c->real_bounds.h - 2 * dpi);
                    cr->fill();
                } else if (c->state.mouse_pressing || c->state.mouse_hovering) {
                    set_argb(cr, c->state.mouse_pressing ? RGBA(.72, .83, .95, 1) : RGBA(.83, .90, .98, 1));
                    cr->rectangle(c->real_bounds.x + dpi, c->real_bounds.y + dpi,
                        c->real_bounds.w - 2 * dpi, c->real_bounds.h - 2 * dpi);
                    cr->fill();
                }
                if (is_selected && !is_today) {
                    set_argb(cr, accent);
                    cr->set_line_width(dpi);
                    cr->rectangle(c->real_bounds.x + dpi, c->real_bounds.y + dpi,
                        c->real_bounds.w - 2 * dpi, c->real_bounds.h - 2 * dpi);
                    cr->stroke();
                }
                const auto text = std::to_string(data->day);
                auto b = draw_text(cr, 0, 0, text, 11 * dpi, false);
                RGBA color = in_month ? RGBA(.14, .18, .24, 1) : RGBA(.53, .57, .62, 1);
                if (is_today)
                    color = {1, 1, 1, 1};
                draw_text(cr, c->real_bounds.x + (c->real_bounds.w - b.w) / 2,
                    c->real_bounds.y + (c->real_bounds.h - b.h) / 2, text, 11 * dpi, true,
                    set->font, -1, -1, color);
            };
            cell->when_clicked = [](Container *root, Container *c) {
                auto dock = static_cast<Dock *>(root->user_data);
                auto data = static_cast<DockDateCell *>(c->user_data);
                dock->date_view_month = data->month;
                dock->date_view_year = data->year;
                dock->selected_date_day = data->day;
                dock->selected_date_month = data->month;
                dock->selected_date_year = data->year;
                update_dock_calendar(dock);
            };
            cell->user_data = new DockDateCell;
        }
    }

    const auto now = std::time(nullptr);
    std::tm today{};
    localtime_r(&now, &today);
    dock->date_view_month = today.tm_mon;
    dock->date_view_year = today.tm_year + 1900;
    dock->selected_date_day = today.tm_mday;
    dock->selected_date_month = today.tm_mon;
    dock->selected_date_year = today.tm_year + 1900;
    update_dock_calendar(dock);
}


static void fill_root(Container *root) {
    root->when_paint = paint_root_func;
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
        super->name = "super";
        super->when_clicked = paint {
            auto dock = (Dock *) root->user_data;
            auto mylar = dock->window;
            auto dpi = mylar->raw_window->dpi;

            RawWindowSettings settings = make_icon_anchored_popup_settings(
                c, dpi, volume_popup_w * 2.4, volume_popup_w * 1.9);
            settings.keyboard_focus_on_click = true;

            dock->applications = open_mylar_popup(mylar, settings);
            if (!dock->applications)
                return;
            auto applications = dock->applications;
            applications->root->on_closed = [applications](Container *root) {
                auto dock = (Dock *) root->user_data;
                if (dock->applications == applications)
                    dock->applications = nullptr;
            };
            dock->applications->root->user_data = dock;
            dock->applications->root->wanted_bounds.w = FILL_SPACE;
            dock->applications->root->wanted_bounds.h = FILL_SPACE;
            fill_applications_container(dock->applications->root);

            later(30, [dock, applications](Timer *) {
                if (dock->applications == applications)
                    windowing::request_keyboard_focus(applications->raw_window);
            });
            windowing::redraw(dock->applications->raw_window);
        };
        super->after_paint = paint {
            return;
            auto dock = (Dock *) root->user_data;
            auto cr = dock->window->raw_window->drawing_context;
            set_rect(cr, c->real_bounds);
            static int off = 0;
            off++;
            if (off % 2 == 0) {
                set_argb(cr, {1, 1, 0, 1});
            } else {
                set_argb(cr, {1, 0, 0, 1});
            }
            cr->fill();
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
        icons->when_clicked = [](Container *root, Container *c) {
            auto dock = (Dock *) root->user_data;
            auto mylar = dock->window;
            if (c->state.mouse_button_pressed == BTN_RIGHT) {
                int startoff = (root->mouse_current_x - c->real_bounds.x) / mylar->raw_window->dpi;
                int cw = c->real_bounds.w / mylar->raw_window->dpi;
                auto uuid = c->uuid;

                icons_right_click(startoff, cw, uuid, dock->window->raw_window->dpi, dock->window->root->mouse_current_y);
            }
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

            // Keep an open hover preview in sync when windows are added,
            // removed, or the icon moves because the dock reflowed.
            auto dock = (Dock *) root->user_data;
            if (dock && dock->thumbnails_dirty) {
                dock->thumbnails_dirty = false;
                sync_thumbnails(dock, c);
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
            auto dock = (Dock *) root->user_data;
            float dpi = dock->window->raw_window->dpi;
            float yoff = root->mouse_current_y;
            main_thread([dpi, yoff] {
                for (auto ch : actual_root->children) {
                    if (ch->custom_type != (int) TYPE::CLIENT)
                        continue;
                    auto cid = *datum<int>(ch, "cid");
                    if (!hypriso->has_focus(cid))
                        continue;
                    // Same gap above the dock as pinned_right_click
                    auto m = mouse();
                    titlebar::titlebar_right_click(cid, true, m.y - (yoff / dpi) - 5);
                    return;
                }
            });
        };
    }

    if (false) {
        auto toggle = simple_dock_item(root, ICON("\uF0E2"));
        toggle->when_clicked = paint {
            system("hyprctl dispatch \"hl.plugin.mylar.toggle_layout()\"");
        };
    }

    {
        auto extra = simple_dock_item(root, ICON("\ue70e"));
        extra->name = "extra";
        extra->when_clicked = [](Container *root, Container *c) {
            auto dock = (Dock *) root->user_data;
            auto mylar = dock->window;
            auto dpi = mylar->raw_window->dpi;

            RawWindowSettings settings = make_icon_anchored_popup_settings(c, dpi, 370, 260);

            dock->extra = open_mylar_popup(mylar, settings);
            if (!dock->extra)
                return;
            dock->extra->root->user_data = dock;
            auto extra_root = dock->extra->root;
            fill_extra_container(extra_root);

            windowing::redraw(dock->extra->raw_window);
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

    if (true) {
        auto tiling = simple_dock_item(root, ICON("\uECA5"));
        tiling->when_clicked = [](Container *root, Container *c) {
            auto dock = (Dock *) root->user_data;
            auto mylar = dock->window;
            auto dpi = mylar->raw_window->dpi;

            RawWindowSettings settings = make_icon_anchored_popup_settings(
                c, dpi, 420, 538);

            dock->tiling = open_mylar_popup(mylar, settings);
            if (!dock->tiling)
                return;
            dock->tiling->root->on_closed = [](Container *root) {
                auto dock = (Dock *) root->user_data;
                dock->tiling = nullptr;
            };
            dock->tiling->root->user_data = dock;
            dock->tiling->root->wanted_bounds.w = FILL_SPACE;
            dock->tiling->root->wanted_bounds.h = FILL_SPACE;
            fill_tiling_container(dock);
            main_thread(dock::redraw);
            windowing::redraw(dock->tiling->raw_window);
        };
    }

    if (true) {
        auto bluetooth = simple_dock_item(root, ICON("\uE702"));
        bluetooth->name = "bluetooth";
        bluetooth->exists = bluetooth_available.load();
        bluetooth->when_clicked = [](Container *root, Container *c) {
            auto dock = (Dock *) root->user_data;
            auto settings = make_icon_anchored_popup_settings(c, dock->window->raw_window->dpi, 376, 448);
            dock->bluetooth = open_mylar_popup(dock->window, settings);
            if (!dock->bluetooth) return;
            dock->bluetooth->root->user_data = dock;
            fill_bluetooth_container(dock);
            windowing::redraw(dock->bluetooth->raw_window);
        };
    }

    {
        // winbar's tray icon: Wi-Fi or Ethernet depending on the default route.
        auto wifi = simple_dock_item(root, []() {
            auto state = network_icon_state();
            if (state.wired) return std::string(state.up ? "\uE839" : "\uEB55");
            return std::string(state.up ? "\uE701" : "\uEB5E");
        });
        wifi->name = "wifi";
        wifi->when_clicked = [](Container *root, Container *c) {
            auto dock = (Dock *) root->user_data;
            auto settings = make_icon_anchored_popup_settings(c, dock->window->raw_window->dpi, 376, 520);
            // The password field needs keyboard input.
            settings.keyboard_focus_on_click = true;
            dock->wifi = open_mylar_popup(dock->window, settings);
            if (!dock->wifi) return;
            dock->wifi->root->user_data = dock;
            fill_wifi_container(dock);
            windowing::redraw(dock->wifi->raw_window);
        };
    }

    {
        auto brightness = simple_dock_item(root, ICON("\uE706"), []() {
           return fz("{}%", (int) std::round(brightness_level));
        }, []() {
           return std::string("100%");
        });
        auto brightness_data = new BrightnessData;
        std::thread t([&brightness_data]() {
            brightness_level = get_brightness();
        });
        t.detach();

        brightness->when_fine_scrolled = [](Container* root, Container* c, double scroll_x, double scroll_y, bool came_from_touchpad) {
            auto dock = (Dock *) root->user_data;
            auto mylar = dock->window;
            auto brightness_data = (BrightnessData *) c->user_data;
            brightness_data->value += scroll_y * .4;
            if (brightness_data->value > 100) {
               brightness_data->value = 100;
            }
            if (brightness_data->value < 0) {
               brightness_data->value = 0;
            }
            set_brightness(brightness_data->value);
        };
        brightness->when_clicked = [](Container *root, Container *c) {
            auto dock = (Dock *) root->user_data;
            auto mylar = dock->window;
            auto dpi = mylar->raw_window->dpi;

            RawWindowSettings settings = make_icon_anchored_popup_settings(c, dpi, volume_popup_w, 32 * dpi);

            dock->brightness = open_mylar_popup(mylar, settings);
            if (!dock->brightness)
                return;
            dock->brightness->root->on_closed = [](Container *root) {
                auto dock = (Dock *) root->user_data;
                dock->brightness = nullptr;
            };
            dock->brightness->root->user_data = dock;
            dock->brightness->root->wanted_bounds.w = FILL_SPACE;
            dock->brightness->root->wanted_bounds.h = FILL_SPACE;
            fill_brightness_container(dock);
            windowing::redraw(dock->brightness->raw_window);
        };
        brightness->user_data = brightness_data;
    }

    {
        auto volume = simple_dock_item(root, []() {
            std::string text = "";
            auto val = volume_level;
            bool mute_state = is_master_muted;
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
        }, []() {
            return "100%";
        });
        volume->name = "volume";
        volume_level = 100;
        volume->when_clicked = paint {
            if (!audio_running)
                audio_start();
            auto dock = (Dock *) root->user_data;
            auto mylar = dock->window;
            auto dpi = mylar->raw_window->dpi;

            auto clients = snapshot_audio_clients();
            RawWindowSettings settings = make_icon_anchored_popup_settings(
                c, dpi, volume_popup_w, volume_popup_height_for_clients(clients));

            dock->volume = open_mylar_popup(mylar, settings);
            if (!dock->volume)
                return;
            dock->volume->root->on_closed = [](Container *root) {
                auto dock = (Dock *) root->user_data;
                dock->volume = nullptr;
            };
            dock->volume->root->user_data = dock;
            dock->volume->root->wanted_bounds.w = FILL_SPACE;
            dock->volume->root->wanted_bounds.h = FILL_SPACE;
            build_volume_popup(dock);
            dock->volume->root->when_paint = [](Container *root, Container *c) {
                auto dock = (Dock *) root->user_data;
                auto cr = dock->volume->raw_window->drawing_context;
                paint_popup_background(cr, c->real_bounds, dock->volume->raw_window->dpi);
            };
            // Because we need to fill items on first frame
            audio_read([]() {
                dock::change_in_audio();
            });
            windowing::redraw(dock->volume->raw_window);
        };
        volume->when_fine_scrolled = [](Container* root, Container* c, double scroll_x, double scroll_y, bool came_from_touchpad) {
            static long last_time_checked_ms = 0;
            auto current = get_current_time_in_ms();
            if (!audio_running && (current - last_time_checked_ms) > 300) {
                audio_start();
                last_time_checked_ms = current;
            }
            auto dock = (Dock *) root->user_data;
            auto mylar = dock->window;
            if (came_from_touchpad) {
                volume_level += scroll_y * .25;
            } else {
                // When changing volume with mouse, round to next, 0, 5, 3, or 7
                int full = std::round(volume_level);
                int last_digit;
                do {
                    if (scroll_y > 0) {
                        full++;
                    } else if (scroll_y < 0) {
                        full--;
                    }
                    last_digit = full % 10;
                } while (!(last_digit == 0 || last_digit == 3 || last_digit == 5 || last_digit == 7));

                volume_level = ((double) full);
            }
            if (volume_level > 100) {
               volume_level = 100;
            }
            if (volume_level < 0) {
               volume_level = 0;
            }
            last_time_volume_adjusted = get_current_time_in_ms();
            set_master_volume(volume_level);
        };
        volume->when_mouse_enters_container = [](Container *root, Container *c) {
            dock::change_in_audio();
        };
    }

    if (has_internal_battery()) {
        watch_battery_level();
        auto battery = simple_dock_item(root, []() {
            static std::string regular[] = {"\uEBA0", "\uEBA1", "\uEBA2", "\uEBA3", "\uEBA4", "\uEBA5", "\uEBA6", "\uEBA7", "\uEBA8", "\uEBA9", "\uEBAA"};
            static std::string plugged[] = {"\uEBAB", "\uEBAC", "\uEBAD", "\uEBAE", "\uEBAF", "\uEBB0", "\uEBB1", "\uEBB2", "\uEBB3", "\uEBB4", "\uEBB5"};
            auto status = battery_status_snapshot();
            int index = status.valid ? std::clamp(int(status.percentage / 10), 0, 10) : 0;
            return status.valid && (status.state == 1 || status.state == 4 || status.state == 5) ? plugged[index] : regular[index];
        }, []() {
            auto status = battery_status_snapshot();
            if (!status.valid) return std::string("—");
            return std::format("{}{}%", status.state == 1 ? "+" : "", (int) std::round(status.percentage));
        }, []() {
            return std::string("+100%");
        });
        battery->when_clicked = [](Container *root, Container *c) {
            auto dock = (Dock *) root->user_data;
            auto mylar = dock->window;
            auto dpi = mylar->raw_window->dpi;
            RawWindowSettings settings = make_icon_anchored_popup_settings(c, dpi, 420, 550);
            dock->battery = open_mylar_popup(mylar, settings);
            if (!dock->battery)
                return;
            dock->battery->root->on_closed = [](Container *root) {
                auto dock = (Dock *) root->user_data;
                dock->battery = nullptr;
            };
            dock->battery->root->user_data = dock;
            dock->battery->root->wanted_bounds.w = FILL_SPACE;
            dock->battery->root->wanted_bounds.h = FILL_SPACE;
            fill_battery_container(dock);
            request_battery_refresh();
            windowing::redraw(dock->battery->raw_window);
        };
    }

    {
        auto date = root->child(40, FILL_SPACE);
        date->when_paint = paint {
            auto dock = (Dock *) root->user_data;
            auto mylar = dock->window;
            auto cr = mylar->raw_window->drawing_context;
            paint_button_bg(root, c);
            draw_text(cr, c, get_date(), 9 * mylar->raw_window->dpi);
        };
        date->pre_layout = [](Container *root, Container *c, const Bounds &b) {
            auto dock = (Dock *) root->user_data;
            auto mylar = dock->window;
            auto cr = mylar->raw_window->drawing_context;
            auto bounds = draw_text(cr, c, get_date(), 9 * mylar->raw_window->dpi, false);
            c->wanted_bounds.w = bounds.w + 20;
        };
        date->when_clicked = [](Container *root, Container *c) {
            auto dock = static_cast<Dock *>(root->user_data);
            if (dock->date_menu)
                return;
            const auto dpi = dock->window->raw_window->dpi;
            auto settings = make_icon_anchored_popup_settings(c, dpi, 360, 470);
            dock->date_menu = open_mylar_popup(dock->window, settings);
            if (!dock->date_menu)
                return;
            dock->date_menu->root->user_data = dock;
            dock->date_menu->root->skip_delete = true;
            dock->date_menu->root->wanted_bounds.w = FILL_SPACE;
            dock->date_menu->root->wanted_bounds.h = FILL_SPACE;
            dock->date_menu->root->on_closed = [](Container *root) {
                auto dock = static_cast<Dock *>(root->user_data);
                dock->date_menu = nullptr;
            };
            auto window = dock->date_menu;
            auto on_close = window->raw_window->on_close;
            window->raw_window->on_close = [dock, window, on_close](RawWindow *raw) {
                if (on_close)
                    on_close(raw);
                if (dock->window->popup_window == window)
                    dock->window->popup_window = nullptr;
                delete window->root;
                delete window;
            };
            fill_date_menu(dock);
            windowing::redraw(dock->date_menu->raw_window);
        };
    }

    {
        auto show_desktop = root->child(6, FILL_SPACE);
        show_desktop->when_paint = paint {
            auto dock = (Dock *) root->user_data;
            auto mylar = dock->window;
            auto cr = mylar->raw_window->drawing_context;
            paint_button_bg(root, c);
            set_argb(cr, {1, 1, 1, .3});
            set_rect(cr, {c->real_bounds.x, c->real_bounds.y, 1, c->real_bounds.h});
            cr->fill();
        };
        show_desktop->when_clicked = [](Container *root, Container *c) {
            main_thread([]() {
                if (show_desktop::is_opened()) {
                    show_desktop::stop_animation();
                } else {
                    show_desktop::start_animation();
                }
                damage_all();
            });
        };
        show_desktop->pre_layout = [](Container *root, Container *c, const Bounds &b) {
            auto dock = (Dock *) root->user_data;
            auto mylar = dock->window;
            auto cr = mylar->raw_window->drawing_context;
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

void total_update();

// First local file path in a text/uri-list ("file:///a%20b" -> "/a b").
static std::string first_path_from_uri_list(const std::string &uri_list) {
    size_t pos = 0;
    while (pos < uri_list.size()) {
        size_t end = uri_list.find('\n', pos);
        if (end == std::string::npos)
            end = uri_list.size();
        std::string line = uri_list.substr(pos, end - pos);
        pos = end + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
            line.pop_back();
        if (line.empty() || line[0] == '#')
            continue;

        const std::string scheme = "file://";
        if (line.compare(0, scheme.size(), scheme) != 0)
            continue;
        // Skip an optional host ("file://localhost/path").
        size_t path_start = line.find('/', scheme.size());
        if (path_start == std::string::npos)
            continue;

        std::string path;
        for (size_t i = path_start; i < line.size(); i++) {
            if (line[i] == '%' && i + 2 < line.size() && std::isxdigit((unsigned char) line[i + 1]) &&
                std::isxdigit((unsigned char) line[i + 2])) {
                path += (char) std::stoi(line.substr(i + 1, 2), nullptr, 16);
                i += 2;
            } else {
                path += line[i];
            }
        }
        return path;
    }
    return {};
}

// Files dropped onto the icons area of the dock open the pin editor with the
// file as the command, so it can be pinned.
static void setup_dock_file_drop(Dock *dock) {
    auto rw = dock->window->raw_window;
    rw->accepts_file_drop = [dock](RawWindow *rw, float x, float y) {
        std::lock_guard<std::mutex> lock(rw->creator->mutex);
        auto icons = container_by_name("icons", dock->window->root);
        if (!icons)
            return false;
        return bounds_contains(icons->real_bounds, (int) (x * rw->dpi), (int) (y * rw->dpi));
    };
    rw->on_file_drop = [](RawWindow *, float, float, std::string uri_list) {
        auto path = first_path_from_uri_list(uri_list);
        if (path.empty())
            return;
        // Commands run through "sh -c": quote paths the shell would split or expand.
        if (path.find_first_of(" \t'\"$`\\&;|<>()*?[]{}!#~") != std::string::npos) {
            std::string quoted = "'";
            for (char ch : path) {
                if (ch == '\'')
                    quoted += "'\\''";
                else
                    quoted += ch;
            }
            path = quoted + "'";
        }
        edit_pin::open("", "", path);
    };
}

static void schedule_startup_audio_update(Dock *dock) {
    windowing::timer(dock->app, 1000, [](void *data) {
        auto dock = static_cast<Dock *>(data);
        total_update();
        if (++dock->startup_audio_update_count < 30)
            schedule_startup_audio_update(dock);
    }, dock);
}

void dock_start(std::string monitor_name) {
    // Docks can be recreated, so always pick up the persisted setting before
    // the first grouping pass.
    if (set)
        merge_windows = set->dock_merge_windows;

    if (!monitor_name.empty()) {
        for (auto d : docks) {
            std::lock_guard<std::mutex> lock(d->app->mutex);

            // A dock that is still shutting down (monitor disabled then re-enabled) doesn't count.
            if (d->creation_settings.monitor_name == monitor_name && !d->closing) {
                return; // already created that dock
            }
        }
    }
    auto dock = new Dock;
    dock->collection = new Windows;
    dock->app = windowing::open_app();
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
    setup_dock_file_drop(dock);
    if (auto icons = container_by_name("icons", dock->window->root))
        load_saved_pins_from_file(icons);
    dock->window->root->alignment = ALIGN_RIGHT;
    docks.push_back(dock);
    bluetooth_watch_dock(dock);
    wifi_watch_dock(dock);
    total_update();
    schedule_startup_audio_update(dock);
    windowing::main_loop(dock->app);
    if (docks.size() == 1) {
        finished = true;
        battery_stop();
        wifi_stop();
    }
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
    cleanup_cached_pango_fonts();
    drawing::cleanup_thread_resources();
}

static int get_dock_alignment() {
    return hypriso->get_varint("plugin:mylardesktop:dock", 3);
}

void dock::start(std::string monitor_name) {
    if (set)
        merge_windows = set->dock_merge_windows;

    if (monitor_name.empty())
        monitor_name = hypriso->monitor_name(hypriso->monitor_from_cursor());
    if (monitor_name == "FALLBACK")
        return;
    current_alignment = get_dock_alignment();

    finished = false;
    bluetooth_start();

    std::thread t(dock_start, monitor_name);
    dock_threads.push_back(std::move(t));

    preload_scripts();
}

void dock::stop(std::string monitor_name) {
    if (monitor_name.empty()) {
        finished = true;
        bluetooth_stop();
        battery_stop();
        wifi_stop();

        for (auto d : docks) {
            std::lock_guard<std::mutex> lock(d->app->mutex);
            //windowing::close_window(d->window->raw_window);
            windowing::close_app(d->app);
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        for (int i = 0; i < dock_threads.size(); i++) {
            if (dock_threads[i].joinable()) {
                dock_threads[i].join();
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        dock_threads.clear();
        docks.clear();
    } else {
        for (auto d : docks) {
            std::lock_guard<std::mutex> lock(d->app->mutex);

            if (d->creation_settings.monitor_name == monitor_name) {
                d->closing = true;
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

std::string get_steam_launch_command(const std::string& app_name) {
    const std::string prefix = "steam_icon_";
    if (app_name.rfind(prefix, 0) == 0 && app_name.size() > prefix.size())
        return "steam steam://rungameid/" + app_name.substr(prefix.size());
    return "";
}

// This happens on the main thread, not the dock thread
void dock::add_window(int cid) {
    std::string command = get_launch_command(cid);
    std::string icon = c3ic_fix_wm_class(hypriso->class_name(cid));
    std::string stack_rule = icon;

    // Steam x11 games
    if (hypriso->is_x11(cid)) {
        int id = hypriso->steam_id(cid);
        if (id != -1 && stack_rule != "steam") {
            icon = fz("steam_icon_{}", id);
            command = get_steam_launch_command(icon);
        }
    }

    for (auto d : docks) {
        std::lock_guard<std::mutex> lock(d->app->mutex);
        if (auto c = container_by_name("icons", d->window->root)) {
            for (auto ch : c->children) {
                auto pin = (Pin *) ch->user_data;
                if (pin->stacking_rule == stack_rule) {
                    command = pin->command;
                    icon = pin->icon;
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
        window->on_workspace = window_on_dock_workspace(d, cid);
        window->stack_rule = stack_rule;
        window->title = hypriso->title_name(cid);
        window->window_icon = icon;
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

void dock::change_in_systray() {
    for (auto d : docks) {
        std::lock_guard<std::mutex> lock(d->app->mutex);
        if (d->extra && windowing::has_window(d->extra->raw_window))
            windowing::redraw(d->extra->raw_window);
    }
}

void dock::redraw() {
    for (auto d : docks) {
        std::lock_guard<std::mutex> lock(d->app->mutex);
        update_tiling_menu(d);
        windowing::redraw(d->window->raw_window);
    }
}

// Called on the main thread after workspace and monitor assignments settle.
void dock::update_workspaces() {
    for (auto d : docks) {
        std::lock_guard<std::mutex> lock(d->app->mutex);
        update_tiling_menu(d);
        for (auto window : d->collection->list)
            window->on_workspace = window_on_dock_workspace(d, window->cid);
        for (auto window : d->collection->to_be_added)
            window->on_workspace = window_on_dock_workspace(d, window->cid);
        windowing::redraw(d->window->raw_window);
    }
}

void dock::set_dock_merge(bool enabled) {
    merge_windows = enabled;
    // Rebuild every pin's window list from the collection instead of removing
    // and re-adding windows. Removing and re-adding created separate pins for
    // windows that should have been grouped under an existing pinned icon.
    for (auto d : docks) {
        std::lock_guard<std::mutex> lock(d->app->mutex);
        if (auto icons = container_by_name("icons", d->window->root)) {
            for (auto pin_container : icons->children) {
                auto *pin = (Pin *) pin_container->user_data;
                if (!pin)
                    continue;
                pin->windows.clear();
                pin->wants_reposition_animation = true;
            }
            d->thumbnails_dirty = true;
        }
    }
    dock::redraw();
}

void dock::toggle_dock_merge() {
    const bool next = set ? !set->dock_merge_windows : !merge_windows.load();
    if (set)
        set->dock_merge_windows = next;
    set_dock_merge(next);
    if (set)
        settings::load_save_settings(true, set);
}

void dock::edit_pin(std::string original_stacking_rule, std::string new_stacking_rule, std::string new_icon, std::string new_command) {
    if (!original_stacking_rule.empty()) {
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
    } else {
        for (auto d : docks) {
            std::lock_guard<std::mutex> lock(d->app->mutex);
            if (auto icons = container_by_name("icons", d->window->root)) {
                create_pinned_icon(icons, new_stacking_rule, new_command, new_icon);
            }
        }
    }
}

Bounds dock::get_item_location(std::string monitor_name, std::string item_name) {
    for (auto d : docks) {
        std::lock_guard<std::mutex> lock(d->app->mutex);
        if (d->creation_settings.monitor_name == monitor_name) {
            for (auto c : d->window->root->children) {
                if (c->name == item_name) {
                    Bounds b = c->real_bounds;
                    b.scale(1.0f / d->window->raw_window->dpi);
                    return b;
                }
            }
        }
    }

    return {0, 0, 100, 100};
}

std::map<int, Bounds> dock::try_get_locations(const std::string &name) {
    std::map<int, Bounds> locations;
    for (auto d : docks) {
        if (d->creation_settings.monitor_name != name)
            continue;
        // The compositor must not wait for the dock's rendering thread.
        std::unique_lock<std::mutex> lock(d->app->mutex, std::try_to_lock);
        if (!lock.owns_lock())
            continue;
        if (auto icons = container_by_name("icons", d->window->root)) {
            for (auto p : icons->children) {
                const auto pin = (Pin *) p->user_data;
                auto bounds = p->real_bounds;
                bounds.scale(1.0f / d->window->raw_window->dpi);
                for (const auto &window : pin->windows)
                    locations.emplace(window.cid, bounds);
            }
        }
    }
    return locations;
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

void total_update() {
    main_thread([]() {
        auto clients = snapshot_audio_clients();
        for (auto client : clients) {
            if (client.is_master_volume()) {
                volume_level = std::round(client.get_volume() * 100);
            }
        }
        for (auto d : docks) {
            std::lock_guard<std::mutex> lock(d->app->mutex);

            if (d->volume) {
                if (auto volume_root = container_by_name("volume_root", d->volume->root)) {
                    auto scroll = (ScrollContainer *) volume_root;
                    fill_volume_root(clients, scroll->content);
                    windowing::redraw(d->volume->raw_window);
                }
            }

            windowing::redraw(d->window->raw_window);
        }
    });
}

void dock::change_in_audio() {
    total_update();
}

void dock::change_in_battery() {
    main_thread([]() {
        if (finished) return;
        for (auto d : docks) {
            std::lock_guard<std::mutex> lock(d->app->mutex);
            windowing::redraw(d->window->raw_window);
            if (d->battery)
                windowing::redraw(d->battery->raw_window);
        }
    });
}

static long slept_creation_time;

void dock::throb_slept_button() {
    slept_creation_time = get_current_time_in_ms();
    dock::redraw();
}

void dock::create_slept_button() {
    if (slept_windows.empty())
        return;
    // look for c 'icons' and insert after
    for (auto d : docks) {
        std::lock_guard<std::mutex> lock(d->app->mutex);
        auto root = d->window->root;
        auto sleep_button = simple_dock_item(root, []() {
            return "\uF738";
        });
        sleep_button->when_paint = [](Container *root, Container *c) {
            auto dock = (Dock *) root->user_data;
            auto mylar = dock->window;
            auto cr = mylar->raw_window->drawing_context;
            paint_button_bg(root, c);

            auto current = get_current_time_in_ms();
            float delta = ((float)(current - slept_creation_time));
            static float throb_time = 700.0f;
            if (delta < throb_time) {
                windowing::redraw(mylar->raw_window);

                set_rect(cr, c->real_bounds);
                set_argb(cr, {.15, .52, .9, 0.7f * pull(throb, delta / throb_time)});
                cr->fill();
            }

            auto ico_bounds = draw_text(cr, c, "\uF738", 12 * mylar->raw_window->dpi, false, icon_font);
            auto b = draw_text(cr,
                c->real_bounds.x + 10, c->real_bounds.y + c->real_bounds.h * .5 - ico_bounds.h * .5,
                "\uF738", 12 * mylar->raw_window->dpi, true, icon_font);
        };
        sleep_button->name = "sleep_button";
        sleep_button->when_clicked = [d](Container *root, Container *c) {
            std::thread t([]() {
                dock::remove_slept_button();
            });
            t.detach();
        };
        for (int i = root->children.size() - 1; i >= 0; i--)
            if (root->children[i]->name == "sleep_button") {
                root->children.erase(root->children.begin() + i);
                break;
            }
        for (int i = 0; i < root->children.size(); i++)
            if (root->children[i]->name == "icons") {
                root->children.insert(root->children.begin() + i + 1, sleep_button);
                break;
            }
    }
}

void actual_remove_slept_button(Dock *d) {
   windowing::timer(d->app, 10, [](void *data) {
        auto d = (Dock *) data;
        std::lock_guard<std::mutex> lock(d->app->mutex);
        auto root = d->window->root;
        for (int i = root->children.size() - 1; i >= 0; i--) {
            auto c = root->children[i];
            if (c->name == "sleep_button") {
                delete c;
                root->children.erase(root->children.begin() + i);
                break;
            }
        }
    }, d);
}

static void send_signal(pid_t pid, int signal) {
    if (kill(pid, signal) == 0) {
        //std::cout << "Signal " << signal << " sent to process " << pid << std::endl;
    }
}

void dock::remove_slept_button() {
    for (auto zed : slept_windows) {
        send_signal(zed.pid, SIGCONT);
        dock::add_window(zed.cid);
        hypriso->set_hidden(zed.cid, false, false);
        if (zed.is_fullscreen) {
            hypriso->set_fullscreen(zed.cid, zed.is_fullscreen);
        } else if (zed.is_fake_fullscreen) {
            hypriso->fake_fullscreen(zed.cid, zed.is_fake_fullscreen);
        }
    }
    slept_windows.clear();

    for (auto d : docks) {
        std::lock_guard<std::mutex> lock(d->app->mutex);
        actual_remove_slept_button(d);
    }
}

void dock::open_applications() {
    auto monitor_id = hypriso->monitor_from_cursor();
    auto monitor_name = hypriso->monitor_name(monitor_id);
    for (auto d: docks) {
        std::lock_guard<std::mutex> lock(d->app->mutex);
        if (d->creation_settings.monitor_name == monitor_name) {
            bool closed_window = false;
            if (d->applications)
                closed_window = windowing::close_window(d->applications->raw_window);
            if (closed_window)
                return;

            if (auto s = container_by_name("super", d->window->root)) {
                if (s->when_clicked) {
                    s->when_clicked(d->window->root, s);
                }
            }
        }
    }
}

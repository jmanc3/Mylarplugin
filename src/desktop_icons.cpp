#include "desktop_icons.h"
#include "container.h"
#include "heart.h"

#include <linux/input-event-codes.h>
#include <filesystem>

static RGBA color_sel_color() {
    static RGBA default_color("99eeff25");
    return hypriso->get_varcolor("plugin:mylardesktop:sel_color", default_color);
}

static RGBA color_sel_border_color() {
    static RGBA default_color("99eeffff");
    return hypriso->get_varcolor("plugin:mylardesktop:sel_border_color", default_color);
}

struct IconButton {
    Bounds b;
    bool togged = false;
};


static int latest_desktop_watch = -1;

void watch_desktop_folder() {
    const char* home = std::getenv("HOME");
    std::filesystem::path filepath = std::filesystem::path(home) / "Desktop";
    if (!std::filesystem::exists(filepath))
        return;
    static Timer *t = nullptr;
    static long time_made = 0;
    
    latest_desktop_watch = watch_file(filepath.string(), [](FileWatchUpdate update, int fd) {
        if (t) {
            auto current = get_current_time_in_ms();
            if ((current - time_made) > 1200) { // allow a reset if timer still exists after a second
                t = nullptr;
            } else {
                return;
            }
        }
            
        t = later(1000, [](Timer *) {
            notify("change in desktop foldler");
            
            damage_all();
            t = nullptr;
        });
        time_made = get_current_time_in_ms();
    });
}

void desktop_icons::start() {
    watch_desktop_folder();
    
    // each monitor needs its own desktop pane possibly every workspace
    // assign each desktop pane a monitor id
    auto c = actual_root->child(FILL_SPACE, FILL_SPACE);
    c->custom_type = (int) TYPE::DESKTOP_ICONS;
    int monitor = hypriso->monitor_from_cursor();
    auto monitor_scale = scale(monitor);
    *datum<int>(c, "monitor") = monitor;

    static std::vector<Bounds> icons;
    icons.clear();
    int pad = 20 * monitor_scale;
    int start_x = pad;
    int start_y = pad;
    for (int i = 0; i < 3; i++) {
        icons.push_back(Bounds(start_x, start_y, 100, 100));
        start_x += 100 + pad; 
    }

    for (auto b : icons) {
        auto child = c->child(FILL_SPACE, FILL_SPACE);
        child->when_paint = [](Container* actual_root, Container* c) {
            return;
            auto root = get_rendering_root();
            auto [rid, s, stage, active_id] = roots_info(actual_root, root);
            if (stage == (int)STAGE::RENDER_POST_WALLPAPER) {
                renderfix
                rect(c->real_bounds, RGBA(1, 0, 0, 1));
            }
        };
    }

    c->pre_layout = [](Container *root, Container *c, const Bounds &b) {
        auto monitor = *datum<int>(c, "monitor");
        c->wanted_bounds = bounds_reserved_monitor(monitor);
        c->real_bounds = c->wanted_bounds;
        auto s = scale(monitor);
        for (int i = 0; i < c->children.size(); i++) {
            auto child = c->children[i];
            child->real_bounds = icons[i];
        }
    };
    static bool dragging = false;
    dragging = false;
    c->when_drag_start = [](Container* actual_root, Container* c) {
        dragging = true;
    };
    c->when_drag = [](Container *actual_root, Container *c) {
        actual_root->consumed_event = true;
        auto b = fixed_box(actual_root->mouse_initial_x, actual_root->mouse_initial_y, actual_root->mouse_current_x, actual_root->mouse_current_y);
        static Bounds previousB = b;
        b.grow(20);
        hypriso->damage_box(b);
        hypriso->damage_box(previousB);
        previousB = b;
    };
    c->when_paint = [](Container* actual_root, Container* c) {
        auto root = get_rendering_root();
        auto [rid, s, stage, active_id] = roots_info(actual_root, root);
        if (stage == (int)STAGE::RENDER_POST_WALLPAPER && dragging && c->state.mouse_button_pressed == BTN_LEFT) {
            auto b = fixed_box(actual_root->mouse_initial_x, actual_root->mouse_initial_y, actual_root->mouse_current_x, actual_root->mouse_current_y);

            // renderfix euivalent
            auto mb = bounds_monitor(rid);
            b.x -= mb.x;
            b.y -= mb.y;
            b.scale(s);
            b.round();

            auto col = color_sel_color();
            float rounding = 9.0f;
            auto shadow = b;
            shadow.grow(std::round(1.0f * s));
            render_drop_shadow(rid, 1.0, {0, 0, 0, .04f}, std::round(rounding * s), 2.0, shadow);
            rect(b, RGBA(col.r, col.g, col.b, col.a), 0, std::round(rounding * s), 2.0f, true, 0.1);
            col = color_sel_border_color();
            border(b, RGBA(col.r, col.g, col.b, col.a), std::round(1.0f * s), 0, std::round(rounding * s), 2.0f, false, 1.0);


            // TEXT is center aligned label with trailing ... and max height of 2 ilnes
        }
    };
}

void desktop_icons::stop() {
    for (int i = actual_root->children.size() - 1; i >= 0; i--) {
        auto c = actual_root->children[i];
        if (c->custom_type == (int) TYPE::DESKTOP_ICONS) {
            delete c;
            actual_root->children.erase(actual_root->children.begin() + i);
        }
    }
    damage_all();
    if (latest_desktop_watch != -1)
        remove_watch(latest_desktop_watch);
}

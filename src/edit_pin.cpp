#include "edit_pin.h"
#include "pango_font_cache.h"

#include "container.h"
#include "heart.h"

#include "client/raw_windowing.h"
#include "client/windowing.h"
#include "dock/dock.h"
#include "events.h"

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

struct PinData : UserData {
    RawApp *app = nullptr;
    MylarWindow *window = nullptr;
    std::string original_stacking_rule;
    
    std::string stacking_rule;
    std::string icon;
    std::string command;
};

static void paint_bg(Container *root, Container *c) {
    auto mylar = (PinData*)root->user_data;
    auto cr = mylar->window->raw_window->cr;
    cairo_save(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
    cairo_paint(cr);
    cairo_restore(cr);

    set_rect(cr, c->real_bounds);
    set_argb(cr, RGBA(.941, .957, .976, .9));
    cairo_fill(cr);
}

static void rounded_rect_new(cairo_t *cr, double corner_radius, double x, double y, double width, double height) {
    double radius = corner_radius;
    double degrees = M_PI / 180.0;
    
    cairo_new_sub_path(cr);
    
    cairo_arc(cr, x + width - radius, y + radius, radius, -90 * degrees, 0 * degrees);
    cairo_arc(cr, x + width - radius, y + height - radius, radius, 0 * degrees, 90 * degrees);
    cairo_arc(cr, x + radius, y + height - radius, radius, 90 * degrees, 180 * degrees);
    cairo_arc(cr, x + radius, y + radius, radius, 180 * degrees, 270 * degrees);
    cairo_close_path(cr);
}

static void collect_preorder(Container *node, std::vector<Container*> &out) {
    if (!node) return;
    for (auto *child : node->children)
        collect_preorder(child, out);
    if (node->when_key_event && !node->name.empty())
        out.push_back(node);
}

void activate_previous_activatable(Container *root, Container *c) {
    if (!root || !c) return;
    static std::vector<Container*> coll;
    coll.push_back(c);
    if (c->parent)
        coll.push_back(c->parent);
    set_active(root, coll, root, false, true);
    coll.clear();

    // 1. Collect a flat traversal of all containers.
    std::vector<Container*> order;
    collect_preorder(root, order);

    // 2. Find the index of `c`.
    int index_of_c = -1;
    for (int i = 0; i < (int)order.size(); i++) {
        if (order[i] == c) {
            index_of_c = i;
            break;
        }
    }
    if (index_of_c < 0)
        index_of_c = 0;

    if (index_of_c == 0) {
        coll.push_back(order[order.size() - 1]);
        coll.push_back(order[order.size() - 1]->parent);
        set_active(root, coll, root, true, true);
        coll.clear();
    } else {
        coll.push_back(order[index_of_c - 1]);
        coll.push_back(order[index_of_c - 1]->parent);
        set_active(root, coll, root, true, true);
        coll.clear();
    }
}

// needs to occur next frame because otherwise the tab key will effect the next container as well because it'll gain focus and then receive the tab event
void activate_next_activatable(Container *root, Container *c) {
    if (!root || !c) return;
    static std::vector<Container*> coll;
    coll.push_back(c);
    if (c->parent)
        coll.push_back(c->parent);
    set_active(root, coll, root, false, true);
    coll.clear();

    // 1. Collect a flat traversal of all containers.
    std::vector<Container*> order;
    collect_preorder(root, order);

    // 2. Find the index of `c`.
    int index_of_c = -1;
    for (int i = 0; i < (int)order.size(); i++) {
        if (order[i] == c) {
            index_of_c = i;
            break;
        }
    }
    if (index_of_c < 0)
        index_of_c = order.size() - 1;

    if (index_of_c == order.size() - 1) {
        coll.push_back(order[0]);
        coll.push_back(order[0]->parent);
        set_active(root, coll, root, true, true);
        coll.clear();
    } else {
        coll.push_back(order[index_of_c + 1]);
        coll.push_back(order[index_of_c + 1]->parent);
        set_active(root, coll, root, true, true);
        coll.clear();
    }
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

static Bounds draw_text(cairo_t *cr, int x, int y, std::string text, int size = 10, bool draw = true, std::string font = set->font, int wrap = -1, int h = -1, RGBA color = {1, 1, 1, 1});

static Container *setup_label(Container *root, Container *label_parent, bool bold, bool editable, std::function<std::string (Container *root, Container *c)> func) {
    label_parent->type = ::absolute;
    auto label = label_parent->child(FILL_SPACE, FILL_SPACE);
    STextEditorOptions options;
    options.bold = bold;
    options.editable = editable;
    options.parent_activates = true;
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
        auto cr = window->cr;
        set_argb(cr, {1, 1, 1, 1});
        rounded_rect_new(cr, 6 * window->dpi, c->real_bounds.x, c->real_bounds.y, c->real_bounds.w, c->real_bounds.h);
        cairo_fill(cr);
        set_argb(cr, c->active || c->children[0]->active ? RGBA(.23, .6, 1, 1) : RGBA(0, 0, 0, .2));
        rounded_rect_new(cr, 6 * window->dpi, c->real_bounds.x, c->real_bounds.y, c->real_bounds.w, c->real_bounds.h);
        cairo_stroke(cr);
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

static Bounds draw_text(cairo_t *cr, int x, int y, std::string text, int size, bool draw, std::string font, int wrap, int h, RGBA color) {
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
        cairo_move_to(cr, std::round(x), std::round(y));
        pango_cairo_show_layout(cr, layout);
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
        auto cr = mylar->window->raw_window->cr;
        auto dpi = mylar->window->raw_window->dpi;
 
        set_rect(cr, c->real_bounds);
        if (c->state.mouse_pressing) {
            set_argb(cr, {.4, .4, .4, 1});
        } else if (c->state.mouse_hovering) {
            set_argb(cr, {.55, .55, .55, 1});
        } else {
            set_argb(cr, {.7, .7, .7, 1});
        }
        cairo_fill(cr);

        if (c->active && c->viakey) {
            auto b = c->real_bounds;
            b.shrink(std::round(1 * dpi));
            set_rect(cr, b);
            set_argb(cr, {.4, .4, .4, 1});
            auto hehe = cairo_get_line_width(cr);
            cairo_set_line_width(cr, std::round(2 * dpi));
            cairo_stroke(cr);
            cairo_set_line_width(cr, hehe);
        }
        auto text = get_text();

        auto bounds = draw_text(cr, 0, 0, text, size * dpi, false);
        draw_text(cr, 
            c->real_bounds.x + c->real_bounds.w * .5 - bounds.w * .5,
            c->real_bounds.y + c->real_bounds.h * .5 - bounds.h * .5, text, size * dpi, true, set->font, -1, -1, {0, 0, 0, 1});
    };
    child->pre_layout = [size, get_text](Container* root, Container* c, const Bounds& b) {
        auto mylar = (PinData*)root->user_data;
        auto cr = mylar->window->raw_window->cr;
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

static void fill_root(Container *root) {
    root->when_paint = paint_bg;
    root->when_key_event = [](Container *root, Container* c, int key, bool pressed, xkb_keysym_t sym, int mods, bool is_text, std::string text) {
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
    root->type = ::vbox;
    root->wanted_pad = Bounds(30, 30, 30, 30);
    {
        auto label = root->child(FILL_SPACE, FILL_SPACE);
        setup_label(root, label, true, false, [](Container* root, Container* c) { return "Icon"; });
    }
    {
        auto label_parent = root->child(FILL_SPACE, FILL_SPACE);
        auto label = setup_label(root, label_parent, false, true, [](Container* root, Container* c) { return ((PinData*)root->user_data)->icon; });
        label->name = "icon_container";
        activate_next_activatable(root, root);
    }
    {        
        auto label = root->child(FILL_SPACE, FILL_SPACE);
        setup_label(root, label, true, false, [](Container* root, Container* c) { return "Terminal Command"; });
    } 
    {
        auto label_parent = root->child(FILL_SPACE, FILL_SPACE);
        auto label = setup_label(root, label_parent, false, true, [](Container* root, Container* c) { return ((PinData*)root->user_data)->command; });
        label->name = "command_container";
    }
    {
        auto label = root->child(FILL_SPACE, FILL_SPACE);
        setup_label(root, label, true, false, [](Container* root, Container* c) { return "Stacking rule"; });
    }
    {
        auto label_parent = root->child(FILL_SPACE, FILL_SPACE);
        auto label = setup_label(root, label_parent, false, true, [](Container* root, Container* c) { return ((PinData*)root->user_data)->stacking_rule; });
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
        has_desktop_file = desktop_file_exists(((PinData *) root->user_data)->stacking_rule);
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
}

void edit_pin::open(std::string stacking_rule, std::string icon, std::string command) {
    std::thread t(start_edit_pin, stacking_rule, icon, command);
    t.detach();
}

#include "edit_pin.h"

#include "second.h"

#include "client/raw_windowing.h"
#include "client/windowing.h"

#include <cairo.h>
#include <cmath>
#include <pango/pango-font.h>
#include <thread>
#include <pango/pango-layout.h>
#include <pango/pango-types.h>
#include <pango/pangocairo.h>
#include <xkbcommon/xkbcommon-keysyms.h>

struct PinData : UserData {
    RawApp *app = nullptr;
    MylarWindow *window = nullptr;
    std::string stacking_rule;
    std::string icon;
    std::string command;
};

static void paint_root(Container *root, Container *c) {
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

static void setup_label(Container *root, Container *label, bool bold, std::function<std::string (Container *root, Container *c)> func) {
    struct LabelData : UserData {
        int index = 0;
    };
    auto label_data = new LabelData;
    auto text = func(root, label);
    label_data->index = text.size();
    label->user_data = label_data;

    label->pre_layout = [bold, func](Container* root, Container* c, const Bounds &b) {
        auto data = (PinData *) root->user_data;
        auto text = func(root, c);
        auto cr = data->window->raw_window->cr;

        int size = 13 * data->window->raw_window->dpi;

        auto layout = get_cached_pango_font(cr, mylar_font, size, PANGO_WEIGHT_NORMAL, false);
        if (bold)
            layout = get_cached_pango_font(cr, mylar_font, size, PANGO_WEIGHT_BOLD, false);
        pango_layout_set_text(layout, text.data(), text.size());
        cairo_set_source_rgba(cr, 0, 0, 0, 1);
        PangoRectangle ink;
        PangoRectangle logical;
        pango_layout_get_pixel_extents(layout, &ink, &logical);
        
        c->wanted_bounds.w = logical.width;
        c->wanted_bounds.h = logical.height;
    };
    label->when_key_event = [func](Container *root, Container* c, int key, bool pressed, xkb_keysym_t sym, int mods, bool is_text, std::string text) {
        if (!c->active)
            return;
        auto label_text = func(root, c);
        if (!pressed)
            return;
        if (is_text) {
            label_text.append(text);
            return;
        }
        if (sym == XKB_KEY_Return) {
            label_text.append("\n");
        } else if (sym == XKB_KEY_Tab) {
            label_text.append("\t");
        } else if (sym == XKB_KEY_BackSpace) {
            if (!label_text.empty())
                label_text.pop_back();
        }
    };
    label->when_paint = [bold, func](Container* root, Container* c) {
        auto data = (PinData *) root->user_data;
        auto text = func(root, c);
        auto cr = data->window->raw_window->cr;
        auto label_data = (LabelData *) c->user_data;

        int size = 13 * data->window->raw_window->dpi;
        
        auto layout = get_cached_pango_font(cr, mylar_font, size, PANGO_WEIGHT_NORMAL, false);
        if (bold)
            layout = get_cached_pango_font(cr, mylar_font, size, PANGO_WEIGHT_BOLD, false);
        pango_layout_set_text(layout, text.data(), text.size());
        cairo_set_source_rgba(cr, 0, 0, 0, 1);
        PangoRectangle ink;
        PangoRectangle logical;
        pango_layout_get_pixel_extents(layout, &ink, &logical);
        
        cairo_move_to(cr, 
            c->real_bounds.x + c->real_bounds.w * .5 - logical.width * .5, 
            c->real_bounds.y + c->real_bounds.h * .5 - logical.height * .5);
        pango_cairo_show_layout(cr, layout);

        if (c->active && !bold) {
            PangoRectangle cursor_strong_pos;
            PangoRectangle cursor_weak_pos;
            pango_layout_get_cursor_pos(layout, label_data->index, &cursor_strong_pos, &cursor_weak_pos);
            int kern_offset = cursor_strong_pos.x != 0 ? -1 : 0;
            set_rect(cr, Bounds(cursor_strong_pos.x / PANGO_SCALE + c->real_bounds.x + kern_offset,
                            cursor_strong_pos.y / PANGO_SCALE + c->real_bounds.y,
                            1.0f,
                            cursor_strong_pos.height / PANGO_SCALE));
            set_argb(cr, {0, 0, 0, 1});
            cairo_fill(cr);
        }
    };
    static auto update_index = [](Container* root, Container* c, bool bold, std::function<std::string(Container *, Container*)> func) {
        auto data = (PinData *) root->user_data;
        auto text = func(root, c);
        auto cr = data->window->raw_window->cr;
        auto label_data = (LabelData *) c->user_data;
        
        int size = 13 * data->window->raw_window->dpi;

        auto layout = get_cached_pango_font(cr, mylar_font, size, PANGO_WEIGHT_NORMAL, false);
        if (bold)
            layout = get_cached_pango_font(cr, mylar_font, size, PANGO_WEIGHT_BOLD, false);
        pango_layout_set_text(layout, text.data(), text.size());
 
        int index;
        int trailing;
        int x = root->mouse_current_x - c->real_bounds.x;
        int y = root->mouse_current_y - c->real_bounds.y;
        bool inside = pango_layout_xy_to_index(layout, x * PANGO_SCALE, y * PANGO_SCALE, &index, &trailing);
        label_data->index = index;        
    };
    label->when_mouse_down = [bold, func](Container *root, Container *c) {
        update_index(root, c, bold, func);
    };
    label->when_mouse_up = [bold, func](Container *root, Container *c) {
        update_index(root, c, bold, func);
    };
    label->when_drag = [bold, func](Container *root, Container *c) {
        update_index(root, c, bold, func);
    };
}

static void fill_root(Container *root) {
    root->when_paint = paint_root;
    root->type = ::vbox;
    {
        auto label = root->child(FILL_SPACE, FILL_SPACE);
        setup_label(root, label, true, [](Container* root, Container* c) { return "Icon"; });
    }
    {
        auto label = root->child(FILL_SPACE, FILL_SPACE);
        setup_label(root, label, false, [](Container* root, Container* c) { return ((PinData*)root->user_data)->icon; });
    }
    {
        auto label = root->child(FILL_SPACE, FILL_SPACE);
        setup_label(root, label, true, [](Container* root, Container* c) { return "Terminal Command"; });
    } 
    {
        auto label = root->child(FILL_SPACE, FILL_SPACE);
        setup_label(root, label, false, [](Container* root, Container* c) { return ((PinData*)root->user_data)->command; });
    }
    {
        auto label = root->child(FILL_SPACE, FILL_SPACE);
        setup_label(root, label, true, [](Container* root, Container* c) { return "Stacking rule"; });
    }
    {
        auto label = root->child(FILL_SPACE, FILL_SPACE);
        setup_label(root, label, false, [](Container* root, Container* c) { return ((PinData*)root->user_data)->stacking_rule; });
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
    pin_data->stacking_rule = stacking_rule;
    pin_data->icon = icon;
    pin_data->command = command;
    pin_data->window = mylar;
    pin_data->app = app;
    mylar->root->user_data = pin_data;
    
    fill_root(mylar->root);
    
    windowing::main_loop(app);
    
    //cleanup_cached_fonts();
}

void edit_pin::open(std::string stacking_rule, std::string icon, std::string command) {
    std::thread t(start_edit_pin, stacking_rule, icon, command);
    t.detach();
}


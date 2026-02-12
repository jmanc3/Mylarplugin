
#include "settings.h"

#include "heart.h"
#include "hypriso.h"
#include "client/raw_windowing.h"
#include "client/windowing.h"

#include <thread>
#include <fstream>
#include <filesystem>
#include <pango/pango-font.h>
#include <cairo.h>
#include <pango/pango-layout.h>
#include <pango/pango-types.h>
#include <pango/pangocairo.h>

static RawApp *settings_app = nullptr;

static RGBA left_color = RGBA(.93, .93, .93, 1);
static RGBA right_color = RGBA(.89, .89, .89, 1);
static RGBA option_color = RGBA(.87, .87, .87, 1);
static RGBA option_widget_bg_color = RGBA(.84, .84, .84, 1);
static RGBA slider_bg = RGBA(.77, .77, .77, 1);
static RGBA bool_border = RGBA(.47, .47, .47, 1);
static RGBA accent = RGBA(.0, .52, .9, 1);

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

static void load_save_settings(bool saving, ConfigSettings *settings) {
    const char* home = std::getenv("HOME");
    if (!home) return;

    std::filesystem::path filepath = std::filesystem::path(home) / ".config/mylar/mylar_settings.txt";
    std::filesystem::create_directories(filepath.parent_path());
    if (saving) {
        std::ofstream out(filepath, std::ios::trunc);
        if (!out) return;
        out << "#version 1" << "\n\n";
    } else {

    }
}

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

static Bounds draw_text(cairo_t *cr, int x, int y, std::string text, int size, bool draw, std::string font, int wrap, int h, RGBA color) {
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
        pango_layout_set_width(layout, wrap * PANGO_SCALE);
        pango_layout_set_height(layout, h);
        if (h != -1)
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

static void paint_label(Container *root, Container *c, std::string text) {
    auto mylar = (MylarWindow*)root->user_data;
    auto cr = mylar->raw_window->cr;
    auto dpi = mylar->raw_window->dpi;
    auto size = 12 * dpi;

    auto b = draw_text(cr, 0, 0, text, size, false, mylar_font, -1, 0, {0, 0, 0, 1});
    draw_text(cr, 
        c->real_bounds.x + 12 * dpi, 
        c->real_bounds.y + c->real_bounds.h * .5 - b.h * .5, text, size, true, mylar_font, -1, 0, {0, 0, 0, 1});
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

// Creates a container that sizes itself based on children size
// It takes full width of parent
// It lays out right child first and then left with remainder of space
static Container *make_self_height_sized_parent(Container *parent) {
    auto c = parent->child(::absolute, FILL_SPACE, FILL_SPACE);
    static float button_text_pad = 8; 
    c->pre_layout = [](Container *root, Container *c, const Bounds &b) {
        auto mylar = (MylarWindow*)root->user_data;
        auto cr = mylar->raw_window->cr;
        auto dpi = mylar->raw_window->dpi;

        auto left = c->children[0];
        auto right = c->children[1];

        assert(left->pre_layout && right->pre_layout);

        right->real_bounds = b;
        right->pre_layout(root, right, b);
        auto space = std::max(b.w - right->real_bounds.w, 0.0);
        right->real_bounds.x += space;

        left->real_bounds = b;
        left->real_bounds.w = space - button_text_pad * dpi;
        left->pre_layout(root, left, left->real_bounds);

        float tallest = right->real_bounds.h;
        if (tallest < left->real_bounds.h)
            tallest = left->real_bounds.h;
        if (tallest < 55 * dpi)
            tallest = 55 * dpi;
        
        auto bcopy = b;
        bcopy.h = tallest;

        layout(root, right, right->real_bounds);
        modify_all(right, -button_text_pad * dpi, 0);
        
        modify_all(right, 0, tallest * .5 - right->real_bounds.h * .5);
        modify_all(left, 0, tallest * .5 - left->real_bounds.h * .5);
        
        layout(root, left, left->real_bounds);

        c->real_bounds = bcopy;
    };
    c->when_paint = paint {
        auto mylar = (MylarWindow*)root->user_data;
        auto cr = mylar->raw_window->cr;
        auto dpi = mylar->raw_window->dpi;
        auto b = c->real_bounds;
        drawRoundedRect(cr, b.x, b.y, b.w, b.h, 10 * dpi, 1.0);
        set_argb(cr, option_color);
        cairo_fill(cr);
    };

    return c;
}

static void make_label_like(Container *parent, std::string title, std::string description) {
    auto left = parent->child(FILL_SPACE, FILL_SPACE);
    static float button_text_pad = 8; 
    left->when_paint = [title, description](Container *root, Container *c) {
        auto mylar = (MylarWindow*)root->user_data;
        auto cr = mylar->raw_window->cr;
        auto dpi = mylar->raw_window->dpi;
        auto size_title = 12 * dpi;
        auto size_desc = 11 * dpi;

        auto b = c->real_bounds;
        float yoff = button_text_pad * dpi;
        {
            auto bo = draw_text(cr, 0, 0, title, size_title, false, mylar_font, c->real_bounds.w - button_text_pad * dpi * 2, -1, {0, 0, 0, .5});
            if (description.empty()) {
                draw_text(cr,
                    c->real_bounds.x + button_text_pad * dpi, 
                    c->real_bounds.y + (c->real_bounds.h - bo.h) * .5, title, size_title, true, mylar_font, c->real_bounds.w - button_text_pad * dpi * 2, -1, {0, 0, 0, 1});
            } else {
                draw_text(cr,
                    c->real_bounds.x + button_text_pad * dpi, 
                    c->real_bounds.y + yoff, title, size_title, true, mylar_font, c->real_bounds.w - button_text_pad * dpi * 2, -1, {0, 0, 0, 1});
            }

            yoff += bo.h;
        }
        if (!description.empty()) {
            auto bo = draw_text(cr, 0, 0, description, size_desc, false, mylar_font, c->real_bounds.w - button_text_pad * dpi * 2, -1, {0, 0, 0, 1});
            draw_text(cr,
                c->real_bounds.x + button_text_pad * dpi, 
                c->real_bounds.y + yoff, description, size_desc, true, mylar_font, c->real_bounds.w - button_text_pad * dpi * 2, -1, {0, 0, 0, .5});
        }
    };
    left->pre_layout = [title, description](Container *root, Container *c, const Bounds &b) {
        auto mylar = (MylarWindow*)root->user_data;
        auto cr = mylar->raw_window->cr;
        auto dpi = mylar->raw_window->dpi;
        auto size_title = 12 * dpi;
        auto size_desc = 11 * dpi;
 
        auto bo1 = draw_text(cr, 0, 0, title, size_title, false, mylar_font, b.w - button_text_pad * dpi * 2, -1, {0, 0, 0, 1});
        if (description.empty()) {
            c->real_bounds.h = bo1.h + button_text_pad * dpi * 2;
        } else {
            auto bo2 = draw_text(cr, 0, 0, description, size_desc, false, mylar_font, b.w - button_text_pad * dpi * 2, -1, {0, 0, 0, 1});
            c->real_bounds.h = bo1.h + bo2.h + button_text_pad * dpi * 2;
        }
    };
}

static void make_section_title(Container *parent, std::string title) {
    auto section_title = parent->child(FILL_SPACE, FILL_SPACE);
    section_title->pre_layout = [title](Container *root, Container *c, const Bounds &b) {
        auto mylar = (MylarWindow*)root->user_data;
        auto cr = mylar->raw_window->cr;
        auto dpi = mylar->raw_window->dpi;
        auto size_title = 12 * dpi;
        auto bo = draw_text(cr, 0, 0, title, size_title, false, mylar_font, b.w, -1, {0, 0, 0, 1});
        c->wanted_bounds.h = bo.h;
        c->real_bounds.h = bo.h;
    };
    section_title->when_paint = [title](Container *root, Container *c) {
        auto mylar = (MylarWindow*)root->user_data;
        auto cr = mylar->raw_window->cr;
        auto dpi = mylar->raw_window->dpi;
        auto size_title = 12 * dpi;
        draw_text(cr,
            c->real_bounds.x, 
            c->real_bounds.y, title, size_title, true, mylar_font, c->real_bounds.w, -1, {0, 0, 0, 1});
    };
}

static void make_bool(Container *parent, std::string title, std::string description, bool initial_value, std::function<void(bool)> on_change) {
    auto p = make_self_height_sized_parent(parent);

    make_label_like(p, title, description);

    struct BoolInfo : UserData {
        bool on = false;
    };

    auto right = p->child(::hbox, FILL_SPACE, FILL_SPACE);
    auto bool_info = new BoolInfo;
    bool_info->on = initial_value;
    right->user_data = bool_info;
    right->when_paint = paint {
        auto mylar = (MylarWindow*)root->user_data;
        auto cr = mylar->raw_window->cr;
        auto dpi = mylar->raw_window->dpi;
        auto b = c->real_bounds;
        auto data = (BoolInfo *) c->user_data;
        auto half_amount = .46f;
        auto half = std::round(b.h * half_amount);
        drawRoundedRect(cr, b.x, b.y, b.w, b.h, half, std::floor(1.0 * dpi));
        if (data->on) {
            set_argb(cr, accent);
            cairo_fill(cr);
        } else {
            set_argb(cr, bool_border);
            cairo_stroke(cr);
        }

        b.shrink(4.5 * dpi);
        half = std::round(b.h * half_amount);
        if (data->on) {
            drawRoundedRect(cr, b.x + b.w - b.h, b.y, b.h, b.h, half, std::floor(1.0 * dpi));
            set_argb(cr, {1, 1, 1, 1});
            cairo_fill(cr);
        } else {
            drawRoundedRect(cr, b.x, b.y, b.h, b.h, half, std::floor(1.0 * dpi));
            set_argb(cr, bool_border);
            cairo_fill(cr);
        }
    };
    right->pre_layout = [](Container *root, Container *c, const Bounds &b) {
        auto mylar = (MylarWindow*)root->user_data;
        auto cr = mylar->raw_window->cr;
        auto dpi = mylar->raw_window->dpi;
        c->real_bounds.w = 45 * dpi;
        c->real_bounds.h = 26 * dpi;
    };
    right->when_clicked = [on_change](Container *root, Container *c) {
        auto data = (BoolInfo *) c->user_data;
        data->on = !data->on;
        if (on_change)
            on_change(data->on);
    };
}

static void make_slider(Container *parent, std::string title, std::string description, float initial_value, std::function<void(float)> on_change) {
    auto p = make_self_height_sized_parent(parent);
    
    make_label_like(p, title, description);

    struct SliderInfo : UserData {
        float value = .5;
    };

    auto right = p->child(::hbox, FILL_SPACE, FILL_SPACE);
    auto slider_info = new SliderInfo;
    slider_info->value = initial_value;
    right->user_data = slider_info;
    right->when_paint = paint {
        auto mylar = (MylarWindow*)root->user_data;
        auto cr = mylar->raw_window->cr;
        auto dpi = mylar->raw_window->dpi;

        {
            auto b = c->real_bounds;
            float h = 8.5 * dpi;
            b.y += b.h * .5 - h * .5;
            b.h = h;

            drawRoundedRect(cr, b.x, b.y, b.w, b.h, h * .5, 1.0);
            set_argb(cr, slider_bg);
            cairo_fill(cr);
        }
        
        {
            auto data = (SliderInfo *) c->user_data;
            auto b = c->real_bounds;
            b.w = b.h;
            b.x += c->real_bounds.w * data->value - b.h * .5;
            drawRoundedRect(cr, b.x, b.y, b.w, b.h, b.h * .5, 1.0);
            set_argb(cr, {1, 1, 1, 1});
            cairo_fill(cr);

            b.shrink(5 * dpi);
            drawRoundedRect(cr, b.x, b.y, b.w, b.h, b.h * .5, 1.0);
            set_argb(cr, accent);
            cairo_fill(cr);
        }

    };
    right->when_mouse_down = [on_change](Container *root, Container *c) {
        auto mylar = (MylarWindow*)root->user_data;
        auto cr = mylar->raw_window->cr;
        auto dpi = mylar->raw_window->dpi;
        auto b = c->real_bounds;
        auto scalar = (root->mouse_current_x - b.x) / b.w;
        if (scalar < 0)
            scalar = 0;
        if (scalar > 1)
            scalar = 1;
        ((SliderInfo *) c->user_data)->value = scalar;
        if (on_change)
            on_change(scalar);
    };
    right->when_mouse_up = right->when_mouse_down;
    right->when_drag = right->when_mouse_down;
    right->when_drag_end = right->when_mouse_down;
    
    right->pre_layout = [](Container *root, Container *c, const Bounds &b) {
        auto mylar = (MylarWindow*)root->user_data;
        auto cr = mylar->raw_window->cr;
        auto dpi = mylar->raw_window->dpi;
        c->real_bounds.w = 350 * dpi;
        c->real_bounds.h = 20 * dpi;
    };
    right->when_fine_scrolled = [](Container* root, Container* c, int scroll_x, int scroll_y, bool came_from_touchpad) {
        auto data = ((SliderInfo *) c->user_data);
        data->value += (((float) scroll_y) * .00001);
        data->value = std::max(0.0f, std::min(1.0f, data->value));
    };
 
}

static void make_button_group(Container *parent, std::string title, std::string description, std::vector<std::string> options, std::function<void(std::string)> on_selected, std::string default_value) {
    auto p = make_self_height_sized_parent(parent);
    
    make_label_like(p, title, description);
    
    auto right = p->child(::hbox, FILL_SPACE, FILL_SPACE);
    right->when_paint = paint {
        auto mylar = (MylarWindow*)root->user_data;
        auto cr = mylar->raw_window->cr;
        auto dpi = mylar->raw_window->dpi;
        auto b = c->real_bounds;
        drawRoundedRect(cr, b.x, b.y, b.w, b.h, 10 * dpi, 1.0);
        set_argb(cr, option_widget_bg_color);
        cairo_fill(cr);
    };
    right->pre_layout = [options](Container *root, Container *c, const Bounds &b) {
        auto mylar = (MylarWindow*)root->user_data;
        auto cr = mylar->raw_window->cr;
        auto dpi = mylar->raw_window->dpi;
        auto size = 12 * dpi;

        float w = 0;
        float h = 10;
        std::vector<float> ow;
        for (auto o : options) {
            auto bo1 = draw_text(cr, 0, 0, o, size, false, mylar_font, -1, -1, {0, 0, 0, 1});
            ow.push_back(bo1.w);
            w += bo1.w;
            h = bo1.h;
        }
        for (int i = 0; i < ow.size(); i++)
            ow[i] = ow[i] / w;

        h += ((6 + 4) * 2) * dpi;
        // out pad, spacing, per button text pad
        w += (4 * 2 + 4 * (options.size() - 1) + (6 * (options.size() + 2) * 2))  * dpi;

        for (int i = 0; i < ow.size(); i++)
            c->children[i]->wanted_bounds.w = ow[i] * w;

        c->real_bounds.w = w;
        c->real_bounds.h = h;
    };
    for (int i = 0; i < options.size(); i++) {
        auto o = options[i];
        auto option = right->child(FILL_SPACE, FILL_SPACE);
        struct OptionData : UserData {
            bool selected = false;
        };
        auto option_data = new OptionData;
        if (o == default_value)
            option_data->selected = true;
        option->user_data = option_data;
        option->when_paint = [i, o, options](Container *root, Container *c) {
            auto data = (OptionData *) c->user_data;
            auto mylar = (MylarWindow*)root->user_data;
            auto cr = mylar->raw_window->cr;
            auto dpi = mylar->raw_window->dpi;
            auto size = 12 * dpi;
            auto backup = c->real_bounds;
            defer(c->real_bounds = backup);
            if (i == 0) {
                c->real_bounds.shrink(4 * dpi);
                c->real_bounds.w += 4 * dpi;
            } else if (i == options.size() - 1) {
                c->real_bounds.shrink(4 * dpi);
                c->real_bounds.x -= 4 * dpi;
                c->real_bounds.w += 4 * dpi;
            } else {
                c->real_bounds.shrink(4 * dpi);
            }

            if (data->selected) {
                set_argb(cr, {1, 1, 1, 1});
                drawRoundedRect(cr, c->real_bounds.x, c->real_bounds.y, c->real_bounds.w, c->real_bounds.h, 8 * dpi, 1.0);
                cairo_fill(cr);
            }

            auto bo = draw_text(cr, 0, 0, o, size, false, mylar_font, -1, -1, {0, 0, 0, .5});
            draw_text(cr,
                c->real_bounds.x + c->real_bounds.w * .5 - bo.w * .5, 
                c->real_bounds.y + c->real_bounds.h * .5 - bo.h * .5, o, size, true, mylar_font, -1, -1, {0, 0, 0, 1});
        };
        option->when_clicked = [o, on_selected](Container *root, Container *c) {
            for (auto ch : c->parent->children) {
                auto data = (OptionData *) ch->user_data;
                data->selected = false;
            }
            auto data = (OptionData *) c->user_data;
            data->selected = true;
            if (on_selected)
                on_selected(o);
        };
    }
}

static void make_vert_space(Container *parent, float amount) {
    auto pad = parent->child(FILL_SPACE, 8);
    pad->pre_layout = [amount](Container *root, Container *c, const Bounds &b) {
        auto mylar = (MylarWindow*)root->user_data;
        auto cr = mylar->raw_window->cr;
        auto dpi = mylar->raw_window->dpi;
        c->wanted_bounds.h = amount * dpi;
    };
}

static void fill_mouse_settings(Container *root, Container *c) {
    auto right = container_by_name("settings_right", root);
    if (!right)
        return;
    for (auto child: right->children)
        delete child;
    right->children.clear();

    right->pre_layout = [](Container *root, Container *c, const Bounds &b) {
        auto mylar = (MylarWindow*)root->user_data;
        auto cr = mylar->raw_window->cr;
        auto dpi = mylar->raw_window->dpi;
        c->wanted_pad = Bounds(16 * dpi, 16 * dpi, 16 * dpi, 16 * dpi);
    };
    auto padded_right = right->child(FILL_SPACE, FILL_SPACE);
    
    make_section_title(padded_right, "Mouse");
    
    make_vert_space(padded_right, 8); 

    make_button_group(padded_right, 
        "Primary mouse button", 
        "",
        {"Left", "Right"}, 
        [](std::string selected) {
            set->primary_mouse_button = selected;
            main_thread([]() {
                hypriso->generate_mylar_hyprland_config();
            });
        }, set->primary_mouse_button);
    
    make_vert_space(padded_right, 8); 
    
    make_slider(padded_right, "Cursor speed", "", set->cursor_speed, [](float value) {
        set->cursor_speed = value;
        //main_thread([]() {
            //hypriso->generate_mylar_hyprland_config();
        //});
    });

    make_vert_space(padded_right, 8); 
    
    make_section_title(padded_right, "Touchpad");

    make_vert_space(padded_right, 8); 
    
    make_bool(padded_right, "Natural scrolling", "", false, [](bool value) {
        set->natural_scrolling = value;
    });

    make_vert_space(padded_right, 8); 

    make_button_group(padded_right, 
        "Touchpad acceleration", 
        "Acceleration makes precision clicks easier by understanding that if the movement is slower, the mouse should travel less distance, and if it’s faster, it should travel a further distance.",
        {"Custom", "Adaptive", "Flat"}, 
        [](std::string selected) {
            set->touchpad_acceleration_curve = selected;
            main_thread([]() {
                hypriso->generate_mylar_hyprland_config();
            });
        }, set->touchpad_acceleration_curve);
}

void create_tab_option(Container *parent, std::string label) {
    auto c = parent->child(::hbox, FILL_SPACE, FILL_SPACE);
    c->pre_layout = [](Container *root, Container *c, const Bounds &b) {
        auto mylar = (MylarWindow*)root->user_data;
        c->wanted_bounds.h = 40 * mylar->raw_window->dpi;
    };
    c->when_paint = [label](Container *root, Container *c) {
        auto mylar = (MylarWindow*)root->user_data;
        auto cr = mylar->raw_window->cr;
        auto dpi = mylar->raw_window->dpi;

        if (c->state.mouse_pressing) {
            auto b = c->real_bounds;
            b.shrink(3 * dpi);
            drawRoundedRect(cr, b.x, b.y, b.w, b.h, 5 * dpi, 1.0);
            set_argb(cr, {0, 0, 0, .1});
            cairo_fill(cr);
        } else if (c->state.mouse_hovering) {
            auto b = c->real_bounds;
            b.shrink(3 * dpi);
            drawRoundedRect(cr, b.x, b.y, b.w, b.h, 5 * dpi, 1.0);
            set_argb(cr, {0, 0, 0, .2});
            cairo_fill(cr);
        }
        paint_label(root, c, label);
    };
    c->when_clicked = [label](Container *root, Container *c) {
        if (label == "Mouse & Touchpad") {
            fill_mouse_settings(root, c);
        }
    };
}

void fill_left(Container *left) {
    create_tab_option(left, "Search");
    create_tab_option(left, "Display");
    create_tab_option(left, "Mouse & Touchpad");
    create_tab_option(left, "Keyboard");
    create_tab_option(left, "Time & Date");
    create_tab_option(left, "Audio");
    create_tab_option(left, "Wifi");
}

void fill_root(Container *root) {
    auto left_right = root->child(::hbox, FILL_SPACE, FILL_SPACE);
    
    auto left = left_right->child(::vbox, 300, FILL_SPACE);
    left->pre_layout = [](Container *root, Container *c, const Bounds &b) {
        auto mylar = (MylarWindow*)root->user_data;
        c->wanted_bounds.w = 300 * mylar->raw_window->dpi;
    };
    left->when_paint = paint {
        auto mylar = (MylarWindow*)root->user_data;
        auto cr = mylar->raw_window->cr;
        set_argb(cr, left_color);
        set_rect(cr, c->real_bounds);
        cairo_fill(cr);
    };
    fill_left(left);
    
    auto right = left_right->child(::vbox, FILL_SPACE, FILL_SPACE);
    right->name = "settings_right";
    right->when_paint = paint {
        auto mylar = (MylarWindow*)root->user_data;
        auto cr = mylar->raw_window->cr;
        set_argb(cr, right_color);
        set_rect(cr, c->real_bounds);
        cairo_fill(cr);
    };
}

void actual_start() {
    settings_app = windowing::open_app();
    RawWindowSettings settings;
    settings.pos.w = 1200;
    settings.pos.h = 800;
    settings.name = "Settings";
    auto mylar = open_mylar_window(settings_app, WindowType::NORMAL, settings);
    mylar->root->user_data = mylar;
    fill_root(mylar->root);

    windowing::main_loop(settings_app);

    settings_app = nullptr;
};

void settings::start() {
    if (settings_app)
        return;
    std::thread t(actual_start);
    t.detach();
}

void settings::stop() {
    if (settings_app) {
        windowing::close_app(settings_app);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

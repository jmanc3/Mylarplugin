
#include "settings.h"

#include "heart.h"
#include "client/raw_windowing.h"
#include "client/windowing.h"

#include <thread>
#include <pango/pango-font.h>
#include <cairo.h>
#include <pango/pango-layout.h>
#include <pango/pango-types.h>
#include <pango/pangocairo.h>

static RawApp *settings_app = nullptr;

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
}

void fill_left(Container *left) {
    create_tab_option(left, "Search");
    create_tab_option(left, "Display");
    create_tab_option(left, "Mouse");
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
        set_argb(cr, {0.92, 0.92, 0.92, 1});
        set_rect(cr, c->real_bounds);
        cairo_fill(cr);
    };
    fill_left(left);
    
    auto right = left_right->child(::vbox, FILL_SPACE, FILL_SPACE);
    right->when_paint = paint {
        auto mylar = (MylarWindow*)root->user_data;
        auto cr = mylar->raw_window->cr;
        set_argb(cr, {0.98, 0.98, 0.98, 1});
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

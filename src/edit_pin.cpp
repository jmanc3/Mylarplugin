#include "edit_pin.h"

#include "second.h"

#include "client/raw_windowing.h"
#include "client/windowing.h"

#include <cairo.h>
#include <cmath>
#include <cstdio>
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

static void setup_label(Container *root, Container *label_parent, bool bold, std::function<std::string (Container *root, Container *c)> func) {
    struct LabelData : UserData {
        int cursor = 0;
        
        int selection = 0;
        bool selecting = false;
        
        std::string text;
    };
    auto label_data = new LabelData;
    
    auto text = func(root, label_parent);
    label_data->cursor = text.size();
    label_data->text = text;
    label_parent->type = ::absolute;

    static int padding = 10.0f;
    static float rounding = 6.0f;
    
    label_parent->pre_layout = [](Container *root, Container *c, const Bounds &b) {
        auto child = c->children[0];
        auto data = (PinData *) root->user_data;
        auto dpi = data->window->raw_window->dpi;
        
        if (child->pre_layout) {
            child->pre_layout(root, child, b);
            auto hh = child->wanted_bounds.w + padding * 2 * dpi;
            auto hhh = child->wanted_bounds.h + padding * dpi;
            //if (child->wanted_bounds.w > b.w)
                //child->wanted_bounds.w = b.w - padding;
            auto bounds = Bounds(b.x + hh * .5 - child->wanted_bounds.w * .5, b.y + hhh * .5 - child->wanted_bounds.h * .5, 
            child->wanted_bounds.w, child->wanted_bounds.h);
            ::layout(root, child, bounds);
        }
        
        c->wanted_bounds.w = FILL_SPACE;
        c->wanted_bounds.h = child->wanted_bounds.h + padding * dpi;
    };
    label_parent->when_paint = [bold](Container *root, Container *c) {
        if (bold)
            return;
        auto data = (PinData *) root->user_data;
        auto cr = data->window->raw_window->cr;
        auto dpi = data->window->raw_window->dpi;
        
        set_argb(cr, {1, 1, 1, 1});
        rounded_rect_new(cr, rounding * dpi, c->real_bounds.x, c->real_bounds.y, c->real_bounds.w, c->real_bounds.h);
        cairo_fill(cr);

        if (c->children[0]->active || c->active) {
            set_argb(cr, {.23, .6, 1, 1});
        } else {
            set_argb(cr, {0, 0, 0, .2});
        }
        rounded_rect_new(cr, rounding * dpi, c->real_bounds.x, c->real_bounds.y, c->real_bounds.w, c->real_bounds.h);
        cairo_stroke(cr);
    };

    auto label = label_parent->child(FILL_SPACE, FILL_SPACE);
    label->user_data = label_data;

    label->pre_layout = [bold](Container* root, Container* c, const Bounds &b) {
        auto data = (PinData *) root->user_data;
        auto label_data = (LabelData *) c->user_data;
        auto text = label_data->text;
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
    label->when_key_event = [](Container *root, Container* c, int key, bool pressed, xkb_keysym_t sym, int mods, bool is_text, std::string text) {
        if (!c->active && !c->parent->active)
            return;
        auto label_data = (LabelData *) c->user_data;
        auto label_text = &label_data->text;
        
        if (!pressed)
            return;
        if (is_text) {
            if (label_data->selecting) {
                int min = std::min(label_data->cursor, label_data->selection);
                int max = std::max(label_data->cursor, label_data->selection);
                int len = max - min;
                label_text->erase(min, len);
                label_data->selecting = false;
                label_data->cursor = min;
            } 
            label_text->insert(label_data->cursor, text);
            label_data->cursor++;
            return;
        }
        if (sym == XKB_KEY_Return) {
            if (label_data->selecting) {
                int min = std::min(label_data->cursor, label_data->selection);
                int max = std::max(label_data->cursor, label_data->selection);
                int len = max - min;
                label_text->erase(min, len);
                label_data->selecting = false;
                label_data->cursor = min;
            } 
            label_text->insert(label_data->cursor, "\n");
            label_data->cursor++;
        } else if (sym == XKB_KEY_Tab) {
            if (label_data->selecting) {
                int min = std::min(label_data->cursor, label_data->selection);
                int max = std::max(label_data->cursor, label_data->selection);
                int len = max - min;
                label_text->erase(min, len);
                label_data->selecting = false;
                label_data->cursor = min;
            } 
            label_text->insert(label_data->cursor, "\t");
            label_data->cursor++;
        } else if (sym == XKB_KEY_BackSpace) {
            if (label_data->selecting) {
                int min = std::min(label_data->cursor, label_data->selection);
                int max = std::max(label_data->cursor, label_data->selection);
                int len = max - min;
                label_text->erase(min, len);
                label_data->selecting = false;
                label_data->cursor = min;
            } else {
               if (!label_text->empty() && label_data->cursor > 0) {
                    label_text->erase(label_data->cursor - 1, 1);
                    label_data->cursor--;
               }
            }
        } else if (sym == XKB_KEY_Left) {
            label_data->selecting = false;
            label_data->cursor--;
            if (label_data->cursor < 0)
               label_data->cursor = 0;
        } else if (sym == XKB_KEY_Right) {
            label_data->selecting = false;
            label_data->cursor++;
            if (label_data->cursor > label_text->size())
               label_data->cursor = label_text->size();
        } else if (sym == XKB_KEY_Delete) {
            if (label_data->selecting) {
                int min = std::min(label_data->cursor, label_data->selection);
                int max = std::max(label_data->cursor, label_data->selection);
                int len = max - min;
                label_text->erase(min, len);
                label_data->selecting = false;
                label_data->cursor = min;
            } else {
                if (!label_text->empty() && label_data->cursor != label_text->size()) {
                    label_text->erase(label_data->cursor, 1);
                }
            }
        } else if (sym == XKB_KEY_a) {
            if (mods & Modifier::MOD_CTRL) {
                label_data->selecting = true;
                label_data->cursor = label_data->text.size();
                label_data->selection = 0;
            }
        }
    };
    label->when_paint = [bold](Container* root, Container* c) {
        auto data = (PinData *) root->user_data;
        auto label_data = (LabelData *) c->user_data;
        if (!c->active && !c->parent->active)
            label_data->selecting = false;
        auto text = label_data->text;
        auto cr = data->window->raw_window->cr;

        int size = 13 * data->window->raw_window->dpi;
        
        auto layout = get_cached_pango_font(cr, mylar_font, size, PANGO_WEIGHT_NORMAL, false);
        if (bold)
            layout = get_cached_pango_font(cr, mylar_font, size, PANGO_WEIGHT_BOLD, false);
        pango_layout_set_text(layout, text.data(), text.size());
        PangoRectangle ink;
        PangoRectangle logical;
        pango_layout_get_pixel_extents(layout, &ink, &logical);
        
        PangoRectangle cursor_strong_pos;
        PangoRectangle cursor_weak_pos;
        pango_layout_get_cursor_pos(layout, label_data->cursor, &cursor_strong_pos, &cursor_weak_pos);

        if (label_data->selecting) { // selection painting
            set_argb(cr, RGBA(.2, .5, .8, 1));
            PangoRectangle selection_strong_pos;
            PangoRectangle selection_weak_pos;
            pango_layout_get_cursor_pos(layout, label_data->selection, &selection_strong_pos, &selection_weak_pos);
            
            bool cursor_first = false;
            if (cursor_strong_pos.y == selection_strong_pos.y) {
                if (cursor_strong_pos.x < selection_strong_pos.x) {
                    cursor_first = true;
                }
            } else if (cursor_strong_pos.y < selection_strong_pos.y) {
                cursor_first = true;
            }
            
            double w = std::max(c->real_bounds.w, c->parent->real_bounds.w);
            
            double minx = std::min(selection_strong_pos.x, cursor_strong_pos.x) / PANGO_SCALE;
            double miny = std::min(selection_strong_pos.y, cursor_strong_pos.y) / PANGO_SCALE;
            double maxx = std::max(selection_strong_pos.x, cursor_strong_pos.x) / PANGO_SCALE;
            double maxy = std::max(selection_strong_pos.y, cursor_strong_pos.y) / PANGO_SCALE;
            double h = selection_strong_pos.height / PANGO_SCALE;
            
            if (maxy == miny) {// Same line
                set_argb(cr, RGBA(.2, .5, .8, 1));
                set_rect(cr, Bounds(c->real_bounds.x + minx, c->real_bounds.y + miny, maxx - minx, h));
                cairo_fill(cr);
            } else {
                Bounds b;
                if ((maxy - miny) > h) {// More than one line off difference
                    b = Bounds(c->real_bounds.x, c->real_bounds.y + miny + h, w, maxy - miny - h);
                    set_rect(cr, b);
                    set_argb(cr, RGBA(.2, .5, .8, 1));
                    cairo_fill(cr);
                }
                // If the y's aren't on the same line then we always draw the two rects
                // for when there's a one line diff
                
                if (cursor_first) {
                    // Top line
                    b = Bounds(c->real_bounds.x + cursor_strong_pos.x / PANGO_SCALE,
                               c->real_bounds.y + cursor_strong_pos.y / PANGO_SCALE,
                               w,
                               h);
                    set_rect(cr, b);
                    set_argb(cr, RGBA(.2, .5, .8, 1));
                    cairo_fill(cr);
                    
                    // Bottom line
                    int bottom_width = selection_strong_pos.x / PANGO_SCALE;
                    b = Bounds(c->real_bounds.x,
                               c->real_bounds.y + selection_strong_pos.y / PANGO_SCALE,
                               bottom_width,
                               h);
                    set_rect(cr, b);
                    set_argb(cr, RGBA(.2, .5, .8, 1));
                    cairo_fill(cr);
                } else {
                    // Top line
                    b = Bounds(c->real_bounds.x + selection_strong_pos.x / PANGO_SCALE,
                               c->real_bounds.y + selection_strong_pos.y / PANGO_SCALE,
                               w,
                               h);
                    set_rect(cr, b);
                    set_argb(cr, RGBA(.2, .5, .8, 1));
                    cairo_fill(cr);
                    
                    // Bottom line
                    int bottom_width = cursor_strong_pos.x / PANGO_SCALE;
                    b = Bounds(c->real_bounds.x,
                               c->real_bounds.y + cursor_strong_pos.y / PANGO_SCALE,
                               bottom_width,
                               h);
                    set_rect(cr, b);
                    set_argb(cr, RGBA(.2, .5, .8, 1));
                    cairo_fill(cr);
                 }
            }
        }
        
        cairo_set_source_rgba(cr, 0, 0, 0, 1);
        cairo_move_to(cr, 
            c->real_bounds.x + c->real_bounds.w * .5 - logical.width * .5, 
            c->real_bounds.y + c->real_bounds.h * .5 - logical.height * .5);
        pango_cairo_show_layout(cr, layout);

        if ((c->active || c->parent->active) && !bold) {
            int kern_offset = cursor_strong_pos.x != 0 ? -1 : 0;
            set_rect(cr, Bounds(cursor_strong_pos.x / PANGO_SCALE + c->real_bounds.x + kern_offset,
                            cursor_strong_pos.y / PANGO_SCALE + c->real_bounds.y,
                            1.0f,
                            cursor_strong_pos.height / PANGO_SCALE));
            set_argb(cr, {0, 0, 0, 1});
            cairo_fill(cr);
        }
    };
    static auto update_index = [](Container* root, Container* c, bool bold, std::string text, int *target = nullptr) {
        auto data = (PinData *) root->user_data;
        auto cr = data->window->raw_window->cr;
        auto label_data = (LabelData *) c->user_data;
        
        int size = 13 * data->window->raw_window->dpi;

        auto layout = get_cached_pango_font(cr, mylar_font, size, PANGO_WEIGHT_NORMAL, false);
        if (bold)
            layout = get_cached_pango_font(cr, mylar_font, size, PANGO_WEIGHT_BOLD, false);
        pango_layout_set_text(layout, text.data(), text.size());
 
        int index;
        int trailing;
        float x = root->mouse_current_x - c->real_bounds.x;
        float y = root->mouse_current_y - c->real_bounds.y;
        bool inside = pango_layout_xy_to_index(layout, x * PANGO_SCALE, y * PANGO_SCALE, &index, &trailing);
        if (target) {
            *target = index + trailing;        
        } else {
            label_data->cursor = index + trailing;        
        }
    };
    label->when_mouse_down = [bold](Container *root, Container *c) {
        auto label_data = (LabelData *) c->user_data;
        label_data->selecting = false;
        auto text = label_data->text;
        update_index(root, c, bold, text);
        label_data->selection = label_data->cursor;
    };
    label->when_drag_start = [bold](Container* root, Container* c) {
        auto label_data = (LabelData*)c->user_data;
        auto text = label_data->text;
        label_data->selecting = true;
        update_index(root, c, bold, text);
        label_data->selecting = label_data->cursor;
    };
    label->when_drag = [bold](Container *root, Container *c) {
        auto label_data = (LabelData *) c->user_data;
        label_data->selecting = true;
        auto text = label_data->text;
        update_index(root, c, bold, text);
    };
    label->when_drag_end = [bold](Container* root, Container* c) {
        auto label_data = (LabelData*)c->user_data;
        label_data->selecting = true;
        auto text = label_data->text;
        update_index(root, c, bold, text);
    };
    label_parent->when_mouse_down = [bold](Container *root, Container *c) {
        auto label_data = (LabelData *) c->children[0]->user_data;
        label_data->selecting = false;
        auto text = label_data->text;
        update_index(root, c->children[0], bold, text);
        c->children[0]->active = true;
        label_data->selection = label_data->cursor;
    };
    label_parent->when_drag = [bold](Container *root, Container *c) {
        auto label_data = (LabelData *) c->children[0]->user_data;
        label_data->selecting = true;
        auto text = label_data->text;
        update_index(root, c->children[0], bold, text);
        c->children[0]->active = true;
    };
    label_parent->when_drag_start = [bold](Container *root, Container *c) {
        auto label_data = (LabelData *) c->children[0]->user_data;
        label_data->selecting = true;
        auto text = label_data->text;
        update_index(root, c->children[0], bold, text);
        c->children[0]->active = true;
        label_data->selection = label_data->cursor;
    };
    label_parent->when_drag_end = [bold](Container *root, Container *c) {
        auto label_data = (LabelData *) c->children[0]->user_data;
        label_data->selecting = true;
        auto text = label_data->text;
        update_index(root, c->children[0], bold, text);
        c->children[0]->active = true;
    };
}

static void fill_root(Container *root) {
    root->when_paint = paint_root;
    root->type = ::vbox;
    root->wanted_pad = Bounds(30, 30, 30, 30);
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


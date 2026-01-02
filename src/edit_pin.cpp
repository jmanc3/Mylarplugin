#include "edit_pin.h"

#include "second.h"

#include "client/raw_windowing.h"
#include "client/windowing.h"
#include "dock.h"

#include <cairo.h>
#include <climits>
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
    std::string original_stacking_rule;
    
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

// Returns the starting index of the line containing `pos`.
static int line_start(const std::string &text, int pos) {
    if (pos <= 0) return 0;
    int nl = text.rfind('\n', pos - 1);
    return (nl == std::string::npos) ? 0 : nl + 1;
}

// Returns the index *after* the end of the line containing `pos`.
static int line_end(const std::string &text, int pos) {
    int nl = text.find('\n', pos);
    return (nl == std::string::npos) ? text.size() : nl;
}

struct LabelData : UserData {
    int cursor = 0;
    
    int selection = 0;
    bool selecting = false;

    std::string text;

    long last_time = 0;
    long last_activation = 0;
};

static bool did_double_click(long *last_time, long *last_activation, long timeout) {
    long current = get_current_time_in_ms();
    if (current - *last_time < timeout && current - *last_activation > timeout * 2) {
        *last_activation = current;
        return true; 
    }
    *last_time = current;
    return false;
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
    c->active = false;
    c->parent->active = false;

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
    if (index_of_c < 0) return; // c not found in the tree
    
    if (index_of_c == 0) {
        order[order.size() - 1]->active = true;
        order[order.size() - 1]->parent->active = true;
    } else {
        order[index_of_c - 1]->active = true;
        order[index_of_c - 1]->parent->active = true;
    }
}

// needs to occur next frame because otherwise the tab key will effect the next container as well because it'll gain focus and then receive the tab event
void activate_next_activatable(Container *root, Container *c) {
    if (!root || !c) return;
    c->active = false;
    c->parent->active = false;

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
    if (index_of_c < 0) return; // c not found in the tree
    
    if (index_of_c == order.size() - 1) {
        order[0]->active = true;
        order[0]->parent->active = true;
    } else {
        order[index_of_c + 1]->active = true;
        order[index_of_c + 1]->parent->active = true;
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

static Container *setup_label(Container *root, Container *label_parent, bool bold, bool editable, std::function<std::string (Container *root, Container *c)> func) {
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
    label_parent->when_paint = [bold, editable](Container *root, Container *c) {
        if (!editable)
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
    label_parent->when_drag_end_is_click = false;
    label->when_drag_end_is_click = false;
    
    label->user_data = label_data;

    label->pre_layout = [bold, editable](Container* root, Container* c, const Bounds &b) {
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
    label->when_key_event = [editable](Container *root, Container* c, int key, bool pressed, xkb_keysym_t sym, int mods, bool is_text, std::string text) {
        if (!c->active && !c->parent->active)
            return;
        auto label_data = (LabelData *) c->user_data;
        auto label_text = &label_data->text;
        
        if (!pressed)
            return;
        if (!editable)
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
            auto root_data = (PinData *) root->user_data;
            // we lose the first captured param for some reason, and crash if we attempt to use it?????
            windowing::timer(root_data->app, 1, [root, c](void *data) {
                auto actual_root = get_root(c);
                activate_next_activatable(actual_root, c);
            }, nullptr);
        } else if (sym == XKB_KEY_ISO_Left_Tab) {
            auto root_data = (PinData *) root->user_data;
            windowing::timer(root_data->app, 1, [root, c](void *data) {
                auto actual_root = get_root(c);
                activate_previous_activatable(actual_root, c);
            }, nullptr);
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
            if (mods & Modifier::MOD_SHIFT) {
                if (!label_data->selecting) {
                    label_data->selecting = true;
                    label_data->selection = label_data->cursor;
                    label_data->cursor = label_data->cursor;
                }
                label_data->cursor--;
                if (label_data->cursor < 0)
                   label_data->cursor = 0;                    
            } else {
                label_data->selecting = false;
                label_data->cursor--;
                if (label_data->cursor < 0)
                   label_data->cursor = 0;
            }
        } else if (sym == XKB_KEY_Right) {
            if (mods & Modifier::MOD_SHIFT) {
                if (!label_data->selecting) {
                    label_data->selecting = true;
                    label_data->selection = label_data->cursor;
                    label_data->cursor = label_data->cursor;
                }
                label_data->cursor++;
                if (label_data->cursor > label_text->size())
                    label_data->cursor = label_text->size();
            } else {
                label_data->selecting = false;
                label_data->cursor++;
                if (label_data->cursor > label_text->size())
                    label_data->cursor = label_text->size();
            }
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
        } else if (sym == XKB_KEY_Up) {
            int cur = label_data->cursor;
            int start = line_start(label_data->text, cur);
            if (start == 0)
                return; // already on first line

            // Current column
            int col = cur - start;

            // Previous line boundaries
            int prev_end = start - 1; // the '\n' before this line
            int prev_start = line_start(label_data->text, prev_end);

            int prev_len = prev_end - prev_start;
            int new_cursor = prev_start + std::min(col, prev_len);

            // Selection logic (matches your Left-arrow example)
            if (mods & Modifier::MOD_SHIFT) {
                if (!label_data->selecting) {
                    label_data->selecting = true;
                    label_data->selection = label_data->cursor;
                }
            } else {
                label_data->selecting = false;
            }

            label_data->cursor = new_cursor;
        } else if (sym == XKB_KEY_Down) {
            int cur = label_data->cursor;
            int start = line_start(label_data->text, cur);
            int end = line_end(label_data->text, cur);
            if (end >= (int)label_data->text.size())
                return; // last line; nowhere to go

            // Current column
            int col = cur - start;

            // Next line starts right after '\n'
            int next_start = end + 1;
            int next_end = line_end(label_data->text, next_start);
            int next_len = next_end - next_start;

            int new_cursor = next_start + std::min(col, next_len);

            // Selection logic
            if (mods & Modifier::MOD_SHIFT) {
                if (!label_data->selecting) {
                    label_data->selecting = true;
                    label_data->selection = label_data->cursor;
                }
            } else {
                label_data->selecting = false;
            }

            label_data->cursor = new_cursor;
        }
    };
    label->when_paint = [bold, editable](Container* root, Container* c) {
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
    label->when_clicked = paint {
        auto label_data = (LabelData *) c->user_data;
        if (did_double_click(&label_data->last_time, &label_data->last_activation, 400))  {
            if (label_data->selecting) {
                label_data->selecting = false;
            } else {
                label_data->selecting = true;
                label_data->selection = 0;
                label_data->cursor = label_data->text.size();
            }
        }
    };
    label->when_mouse_down = [bold](Container *root, Container *c) {
        auto label_data = (LabelData *) c->user_data;
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
    label_parent->when_clicked = paint {
        auto label_data = (LabelData *) c->children[0]->user_data;
        if (did_double_click(&label_data->last_time, &label_data->last_activation, 400))  {
            if (label_data->selecting) {
                label_data->selecting = false;
            } else {
                label_data->selecting = true;
                label_data->selection = 0;
                label_data->cursor = label_data->text.size();
            }
        }
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
    return label;
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

static void button(Container *root, std::string text, std::function<void(Container *, Container *)> on_click) {
    int size = 12;
    
    auto child = root->child(FILL_SPACE, FILL_SPACE);
    child->when_clicked = on_click;
    child->when_paint = [size, text](Container *root, Container *c) {
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
        
        auto bounds = draw_text(cr, 0, 0, text, size * dpi, false);
        draw_text(cr, 
            c->real_bounds.x + c->real_bounds.w * .5 - bounds.w * .5,
            c->real_bounds.y + c->real_bounds.h * .5 - bounds.h * .5, text, size * dpi, true, mylar_font, -1, -1, {0, 0, 0, 1});
    };
    child->pre_layout = [size, text](Container* root, Container* c, const Bounds& b) {
        auto mylar = (PinData*)root->user_data;
        auto cr = mylar->window->raw_window->cr;
        auto dpi = mylar->window->raw_window->dpi;

        auto bounds = draw_text(cr, 0, 0, text, size * dpi, false);
        c->wanted_bounds.w = bounds.w + 50 * dpi;
    };
};

static void fill_root(Container *root) {
    root->when_paint = paint_root;
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
        parent->alignment = ALIGN_RIGHT;
        
        button(parent, "Save & Quit", [](Container *root, Container *c) {
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
        button(parent, "Close", [](Container *root, Container *c) {
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
    
    //cleanup_cached_fonts();
}

void edit_pin::open(std::string stacking_rule, std::string icon, std::string command) {
    std::thread t(start_edit_pin, stacking_rule, icon, command);
    t.detach();
}


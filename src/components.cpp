//
// Created by jmanc3 on 6/14/20.
//

#include "components.h"
#include "hypriso.h"
#include "hsluv.h"
#include "heart.h"
#include <pango/pango-font.h>

#ifdef TRACY_ENABLE
#include "tracy/Tracy.hpp"
#endif

#include <atomic>
#include <utility>
#include <pango/pangocairo.h>
#include <cmath>

Bounds right_thumb_bounds(Container *scrollpane, Bounds thumb_area);
static void paint_right_thumb(Container *root, Container *container);
#define schedule_redraw(ctx) [ctx](float a) { ctx.on_needs_frame(); return a; }

static int scroll_amount = 120;

static void
mod_color(RGBA *color, double amount) {
    double h; // hue
    double s; // saturation
    double p; // perceived brightness
    rgb2hsluv(color->r, color->g, color->b, &h, &s, &p);
    
    p = p + amount;
    
    if (p < 0)
        p = 0;
    else if (p > 100)
        p = 100;
    hsluv2rgb(h, s, p, &color->r, &color->g, &color->b);
}

void
darken(RGBA *color, double amount) {
    mod_color(color, -amount);
}

void
lighten(RGBA *color, double amount) {
    mod_color(color, amount);
}

RGBA
darken(RGBA color, double amount) {
    RGBA result = color;
    mod_color(&result, -amount);
    return result;
}

RGBA
lighten(RGBA color, double amount) {
    RGBA result = color;
    mod_color(&result, amount);
    return result;
}

struct Config {    
    float dpi = 1.0;
    std::string font = "";
    std::string icons = "Segoe Icons";
    
    RGBA color_taskbar_background = RGBA("#101010dd");
    RGBA color_taskbar_button_icons = RGBA("#ffffffff");
    RGBA color_taskbar_button_default = RGBA("#ffffff00");
    RGBA color_taskbar_button_hovered = RGBA("#ffffff23");
    RGBA color_taskbar_button_pressed = RGBA("#ffffff35");
    RGBA color_taskbar_windows_button_default_icon = RGBA("#ffffffff");
    RGBA color_taskbar_windows_button_hovered_icon = RGBA("#429ce3ff");
    RGBA color_taskbar_windows_button_pressed_icon = RGBA("#0078d7ff");
    RGBA color_taskbar_search_bar_default_background = RGBA("#f3f3f3ff");
    RGBA color_taskbar_search_bar_hovered_background = RGBA("#ffffffff");
    RGBA color_taskbar_search_bar_pressed_background = RGBA("#ffffffff");
    RGBA color_taskbar_search_bar_default_text = RGBA("#2b2b2bff");
    RGBA color_taskbar_search_bar_hovered_text = RGBA("#2d2d2dff");
    RGBA color_taskbar_search_bar_pressed_text = RGBA("#020202ff");
    RGBA color_taskbar_search_bar_default_icon = RGBA("#020202ff");
    RGBA color_taskbar_search_bar_default_icon_short = RGBA("#ffffffff");
    RGBA color_taskbar_search_bar_hovered_icon = RGBA("#020202ff");
    RGBA color_taskbar_search_bar_pressed_icon = RGBA("#020202ff");
    RGBA color_taskbar_search_bar_default_border = RGBA("#b4b4b4ff");
    RGBA color_taskbar_search_bar_hovered_border = RGBA("#b4b4b4ff");
    RGBA color_taskbar_search_bar_pressed_border = RGBA("#0078d7ff");
    RGBA color_taskbar_date_time_text = RGBA("#ffffffff");
    RGBA color_taskbar_application_icons_background = RGBA("#ffffffff");
    RGBA color_taskbar_application_icons_accent = RGBA("#76b9edff");
    RGBA color_taskbar_minimize_line = RGBA("#222222ff");
    RGBA color_taskbar_attention_accent = RGBA("#fc8803ff");
    RGBA color_taskbar_attention_background = RGBA("#fc8803ff");
    
    RGBA color_systray_background = RGBA("#282828f3");
    
    RGBA color_battery_background = RGBA("#1f1f1ff3");
    RGBA color_battery_text = RGBA("#ffffffff");
    RGBA color_battery_icons = RGBA("#ffffffff");
    RGBA color_battery_slider_background = RGBA("#797979ff");
    RGBA color_battery_slider_foreground = RGBA("#0178d6ff");
    RGBA color_battery_slider_active = RGBA("#ffffffff");
    
    RGBA color_wifi_background = RGBA("#1f1f1ff3");
    RGBA color_wifi_icons = RGBA("#ffffffff");
    RGBA color_wifi_default_button = RGBA("#ffffff00");
    RGBA color_wifi_hovered_button = RGBA("#ffffff22");
    RGBA color_wifi_pressed_button = RGBA("#ffffff44");
    RGBA color_wifi_text_title = RGBA("#ffffffff");
    RGBA color_wifi_text_title_info = RGBA("#adadadff");
    RGBA color_wifi_text_settings_default_title = RGBA("#a5d6fdff");
    RGBA color_wifi_text_settings_hovered_title = RGBA("#a4a4a4ff");
    RGBA color_wifi_text_settings_pressed_title = RGBA("#787878ff");
    RGBA color_wifi_text_settings_title_info = RGBA("#a4a4a4ff");
    
    RGBA color_date_background = RGBA("#1f1f1ff3");
    RGBA color_date_seperator = RGBA("#4b4b4bff");
    RGBA color_date_text = RGBA("#ffffffff");
    RGBA color_date_text_title = RGBA("#ffffffff");
    RGBA color_date_text_title_period = RGBA("#a5a5a5ff");
    RGBA color_date_text_title_info = RGBA("#a5dafdff");
    RGBA color_date_text_month_year = RGBA("#dededeff");
    RGBA color_date_text_week_day = RGBA("#ffffffff");
    RGBA color_date_text_current_month = RGBA("#ffffffff");
    RGBA color_date_text_not_current_month = RGBA("#808080ff");
    RGBA color_date_cal_background = RGBA("#006fd8ff");
    RGBA color_date_cal_foreground = RGBA("#000000ff");
    RGBA color_date_cal_border = RGBA("#797979ff");
    RGBA color_date_weekday_monthday = RGBA("#ffffffff");
    RGBA color_date_default_arrow = RGBA("#dfdfdfff");
    RGBA color_date_hovered_arrow = RGBA("#efefefff");
    RGBA color_date_pressed_arrow = RGBA("#ffffffff");
    RGBA color_date_text_default_button = RGBA("#a5d6fdff");
    RGBA color_date_text_hovered_button = RGBA("#a4a4a4ff");
    RGBA color_date_text_pressed_button = RGBA("#787878ff");
    RGBA color_date_cursor = RGBA("#ffffffff");
    RGBA color_date_text_prompt = RGBA("#ccccccff");
    
    RGBA color_volume_background = RGBA("#1f1f1ff3");
    RGBA color_volume_text = RGBA("#ffffffff");
    RGBA color_volume_default_icon = RGBA("#d2d2d2ff");
    RGBA color_volume_hovered_icon = RGBA("#e8e8e8ff");
    RGBA color_volume_pressed_icon = RGBA("#ffffffff");
    RGBA color_volume_slider_background = RGBA("#797979ff");
    RGBA color_volume_slider_foreground = RGBA("#0178d6ff");
    RGBA color_volume_slider_active = RGBA("#ffffffff");
    
    RGBA color_apps_background = RGBA("#1f1f1ff3");
    RGBA color_apps_text = RGBA("#ffffffff");
    RGBA color_apps_text_inactive = RGBA("#505050ff");
    RGBA color_apps_icons = RGBA("#ffffffff");
    RGBA color_apps_default_item = RGBA("#ffffff00");
    RGBA color_apps_hovered_item = RGBA("#ffffff22");
    RGBA color_apps_pressed_item = RGBA("#ffffff44");
    RGBA color_apps_item_icon_background = RGBA("#3380ccff");
    RGBA color_apps_scrollbar_gutter = RGBA("#353535ff");
    RGBA color_apps_scrollbar_default_thumb = RGBA("#5d5d5dff");
    RGBA color_apps_scrollbar_hovered_thumb = RGBA("#868686ff");
    RGBA color_apps_scrollbar_pressed_thumb = RGBA("#aeaeaeff");
    RGBA color_apps_scrollbar_default_button = RGBA("#353535ff");
    RGBA color_apps_scrollbar_hovered_button = RGBA("#494949ff");
    RGBA color_apps_scrollbar_pressed_button = RGBA("#aeaeaeff");
    RGBA color_apps_scrollbar_default_button_icon = RGBA("#ffffffff");
    RGBA color_apps_scrollbar_hovered_button_icon = RGBA("#ffffffff");
    RGBA color_apps_scrollbar_pressed_button_icon = RGBA("#545454ff");
    
    RGBA color_pin_menu_background = RGBA("#1f1f1ff3");
    RGBA color_pin_menu_hovered_item = RGBA("#ffffff22");
    RGBA color_pin_menu_pressed_item = RGBA("#ffffff44");
    RGBA color_pin_menu_text = RGBA("#ffffffff");
    RGBA color_pin_menu_icons = RGBA("#ffffffff");
    
    RGBA color_windows_selector_default_background = RGBA("#282828f3");
    RGBA color_windows_selector_hovered_background = RGBA("#3d3d3df3");
    RGBA color_windows_selector_pressed_background = RGBA("#535353f3");
    RGBA color_windows_selector_close_icon = RGBA("#ffffffff");
    RGBA color_windows_selector_close_icon_hovered = RGBA("#ffffffff");
    RGBA color_windows_selector_close_icon_pressed = RGBA("#ffffffff");
    RGBA color_windows_selector_text = RGBA("#ffffffff");
    RGBA color_windows_selector_close_icon_hovered_background = RGBA("#c61a28ff");
    RGBA color_windows_selector_close_icon_pressed_background = RGBA("#e81123ff");
    RGBA color_windows_selector_attention_background = RGBA("#fc8803ff");
    
    RGBA color_search_tab_bar_background = RGBA("#1f1f1ff3");
    RGBA color_search_accent = RGBA("#0078d7ff");
    RGBA color_search_tab_bar_default_text = RGBA("#bfbfbfff");
    RGBA color_search_tab_bar_hovered_text = RGBA("#d9d9d9ff");
    RGBA color_search_tab_bar_pressed_text = RGBA("#a6a6a6ff");
    RGBA color_search_tab_bar_active_text = RGBA("#ffffffff");
    RGBA color_search_empty_tab_content_background = RGBA("#2a2a2af3");
    RGBA color_search_empty_tab_content_icon = RGBA("#6b6b6bff");
    RGBA color_search_empty_tab_content_text = RGBA("#aaaaaaff");
    RGBA color_search_content_left_background = RGBA("#f0f0f0ff");
    RGBA color_search_content_right_background = RGBA("#f5f5f5ff");
    RGBA color_search_content_right_foreground = RGBA("#ffffffff");
    RGBA color_search_content_right_splitter = RGBA("#f2f2f2ff");
    RGBA color_search_content_text_primary = RGBA("#010101ff");
    RGBA color_search_content_text_secondary = RGBA("#606060ff");
    RGBA color_search_content_right_button_default = RGBA("#00000000");
    RGBA color_search_content_right_button_hovered = RGBA("#00000026");
    RGBA color_search_content_right_button_pressed = RGBA("#00000051");
    RGBA color_search_content_left_button_splitter = RGBA("#ffffffff");
    RGBA color_search_content_left_button_default = RGBA("#00000000");
    RGBA color_search_content_left_button_hovered = RGBA("#00000024");
    RGBA color_search_content_left_button_pressed = RGBA("#00000048");
    RGBA color_search_content_left_button_active = RGBA("#a8cce9ff");
    RGBA color_search_content_left_set_active_button_default = RGBA("#00000000");
    RGBA color_search_content_left_set_active_button_hovered = RGBA("#00000022");
    RGBA color_search_content_left_set_active_button_pressed = RGBA("#00000019");
    RGBA color_search_content_left_set_active_button_active = RGBA("#97b8d2ff");
    RGBA color_search_content_left_set_active_button_icon_default = RGBA("#606060ff");
    RGBA color_search_content_left_set_active_button_icon_pressed = RGBA("#ffffffff");
    
    RGBA color_pinned_icon_editor_background = RGBA("#ffffffff");
    RGBA color_pinned_icon_editor_field_default_text = RGBA("#000000ff");
    RGBA color_pinned_icon_editor_field_hovered_text = RGBA("#2d2d2dff");
    RGBA color_pinned_icon_editor_field_pressed_text = RGBA("#020202ff");
    RGBA color_pinned_icon_editor_field_default_border = RGBA("#b4b4b4ff");
    RGBA color_pinned_icon_editor_field_hovered_border = RGBA("#646464ff");
    RGBA color_pinned_icon_editor_field_pressed_border = RGBA("#0078d7ff");
    RGBA color_pinned_icon_editor_cursor = RGBA("#000000ff");
    RGBA color_pinned_icon_editor_button_default = RGBA("#ccccccff");
    RGBA color_pinned_icon_editor_button_text_default = RGBA("#000000ff");
    
    RGBA color_notification_content_background = RGBA("#1f1f1fff");
    RGBA color_notification_title_background = RGBA("#191919ff");
    RGBA color_notification_content_text = RGBA("#ffffffff");
    RGBA color_notification_title_text = RGBA("#ffffffff");
    RGBA color_notification_button_default = RGBA("#545454ff");
    RGBA color_notification_button_hovered = RGBA("#616161ff");
    RGBA color_notification_button_pressed = RGBA("#474747ff");
    RGBA color_notification_button_text_default = RGBA("#ffffffff");
    RGBA color_notification_button_text_hovered = RGBA("#ffffffff");
    RGBA color_notification_button_text_pressed = RGBA("#ffffffff");
    RGBA color_notification_button_send_to_action_center_default = RGBA("#9c9c9cff");
    RGBA color_notification_button_send_to_action_center_hovered = RGBA("#ccccccff");
    RGBA color_notification_button_send_to_action_center_pressed = RGBA("#888888ff");
    
    RGBA color_action_center_background = RGBA("#1f1f1fff");
    RGBA color_action_center_history_text = RGBA("#a5d6fdff");
    RGBA color_action_center_no_new_text = RGBA("#ffffffff");
    RGBA color_action_center_notification_content_background = RGBA("#282828ff");
    RGBA color_action_center_notification_title_background = RGBA("#1f1f1fff");
    RGBA color_action_center_notification_content_text = RGBA("#ffffffff");
    RGBA color_action_center_notification_title_text = RGBA("#ffffffff");
    RGBA color_action_center_notification_button_default = RGBA("#545454ff");
    RGBA color_action_center_notification_button_hovered = RGBA("#616161ff");
    RGBA color_action_center_notification_button_pressed = RGBA("#474747ff");
    RGBA color_action_center_notification_button_text_default = RGBA("#ffffffff");
    RGBA color_action_center_notification_button_text_hovered = RGBA("#ffffffff");
    RGBA color_action_center_notification_button_text_pressed = RGBA("#ffffffff");
};

Config *config = new Config;

void draw_colored_rect(Container *root, Container *scroll_container, const RGBA &color, const Bounds &bounds) {
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif
    auto data = (ScrollData *) scroll_container->user_data;
    auto ctx = data->func(root);
    set_rect(ctx.cr, bounds);
    set_argb(ctx.cr, color);
    cairo_fill(ctx.cr);
    //rect(bounds, color);
}

void draw_margins_rect(Container *root, Container *scroll_container, const RGBA &color, const Bounds &bounds, float amount, float space) {
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif
    //border(bounds, color, amount);
    auto data = (ScrollData *) scroll_container->user_data;
    auto ctx = data->func(root);
    set_rect(ctx.cr, bounds);
    set_argb(ctx.cr, color);
    cairo_set_line_width(ctx.cr, amount);
    cairo_stroke(ctx.cr);
}

class HoverableButton : public UserData {
public:
    bool hovered = false;
    double hover_amount = 0;
    RGBA color = RGBA(0, 0, 0, 0);
    int previous_state = -1;
    
    std::string text;
    std::string icon;
    
    int color_option = 0;
    RGBA actual_border_color = RGBA(0, 0, 0, 0);
    RGBA actual_gradient_color = RGBA(0, 0, 0, 0);
    RGBA actual_pane_color = RGBA(0, 0, 0, 0);
};

class IconButton : public HoverableButton {
public:
    cairo_surface_t *surface__ = nullptr;
    cairo_surface_t *clicked_surface__ = nullptr;

    // These three things are so that opening menus with buttons toggles between opening and closing
    std::string invalidate_button_press_if_client_with_this_name_is_open;
    bool invalid_button_down = false;
    long timestamp = 0;
    
    IconButton() {
   }
    
    ~IconButton() {
        if (surface__)
            cairo_surface_destroy(surface__);
        if (clicked_surface__)
            cairo_surface_destroy(clicked_surface__);
    }
};


class ButtonData : public IconButton {
public:
    std::string text;
    std::string full_path;
    bool valid_button = true;
};

void fine_scrollpane_scrolled(Container *root,
                              Container *container,
                              int scroll_x,
                              int scroll_y,
                              bool came_from_touchpad) {
    root->consumed_event = true;
    container->scroll_h_real += scroll_x;
    container->scroll_v_real += scroll_y;
    container->scroll_h_visual = container->scroll_h_real;
    container->scroll_v_visual = container->scroll_v_real;
    ::layout(root, container, container->real_bounds);
}

static void
fine_right_thumb_scrolled(Container *root, Container *container, int scroll_x, int scroll_y,
                          bool came_from_touchpad) {
}

static void
fine_bottom_thumb_scrolled(Container *root, Container *container, int scroll_x, int scroll_y,
                           bool came_from_touchpad) {
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

static void
paint_arrow(Container *root, Container *container) {
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif
    bool minimal = ((ScrollContainer *) container->parent->parent)->settings.paint_minimal;
    if (minimal)
        return;
    auto scroll_container = ((ScrollContainer *) container->parent->parent);
    
    auto *data = (ButtonData *) container->user_data;
    
    if (container->state.mouse_pressing || container->state.mouse_hovering) {
        if (container->state.mouse_pressing) {
            RGBA color = config->color_apps_scrollbar_pressed_button;
            color.a = scroll_container->scrollbar_openess;
            draw_colored_rect(root, scroll_container, color, container->real_bounds);
        } else {
            RGBA color = config->color_apps_scrollbar_hovered_button;
            color.a = scroll_container->scrollbar_openess;
            draw_colored_rect(root, scroll_container, color, container->real_bounds);
        }
    } else {
        RGBA color = config->color_apps_scrollbar_default_button;
        color.a = scroll_container->scrollbar_openess;
        draw_colored_rect(root, scroll_container, color, container->real_bounds);
    }
    
    RGBA color;
    if (container->state.mouse_pressing || container->state.mouse_hovering) {
        if (container->state.mouse_pressing) {
            color = RGBA(config->color_apps_scrollbar_pressed_button_icon);
            color.a = scroll_container->scrollbar_openess;
        } else {
            color = RGBA(config->color_apps_scrollbar_hovered_button_icon);
            color.a = scroll_container->scrollbar_openess;
        }
    } else {
        color = RGBA(config->color_apps_scrollbar_default_button_icon);
        color.a = scroll_container->scrollbar_openess;
    }

    auto scroll_data = (ScrollData *) scroll_container->user_data;
    auto ctx = scroll_data->func(root);
    auto layout = get_cached_pango_font(ctx.cr, "Segoe Fluent Icons", 8 * ctx.dpi, PANGO_WEIGHT_NORMAL, false);
    std::string text = data->text;
    pango_layout_set_text(layout, text.data(), text.size());
    pango_layout_set_wrap(layout, PangoWrapMode::PANGO_WRAP_NONE);
    pango_layout_set_width(layout, -1);
    pango_layout_set_height(layout, -1);
    pango_layout_set_ellipsize(layout, PangoEllipsizeMode::PANGO_ELLIPSIZE_NONE);
    set_argb(ctx.cr, color);
    PangoRectangle ink;
    PangoRectangle logical;
    pango_layout_get_pixel_extents(layout, &ink, &logical);
    cairo_move_to(ctx.cr, center_x(container, logical.width), center_y(container, logical.height));
    pango_cairo_show_layout(ctx.cr, layout);
}


//static int scroll_amount = 120;
//static double scroll_anim_time = 120;
//static double repeat_time = scroll_anim_time * .9;
////static easingFunction easing_function = 0;

//
//#define EXPAND(c) c.r, c.g, c.b, c.a
//
//void draw_text(Container *root, int size, std::string font, float r, float g, float b, float a, std::string text, Bounds bounds, int alignment = 5, int x_off = -1, int y_off = -1) {
//}
//
//
//void scrollpane_scrolled(Container *root,
//                         Container *container,
//                         int scroll_x,
//                         int scroll_y) {
//#ifdef TRACY_ENABLE
//    ZoneScoped;
//#endif
///*
//    auto cookie = xcb_xkb_get_state(client->app->connection, client->keyboard->device_id);
//    auto reply = xcb_xkb_get_state_reply(client->app->connection, cookie, nullptr);
//    
//    if (reply->mods & XKB_KEY_Shift_L || reply->mods & XKB_KEY_Control_L) {
//        container->scroll_h_real += scroll_x * scroll_amount + scroll_y * scroll_amount;
//    } else {
//        container->scroll_h_real += scroll_x * scroll_amount;
//        container->scroll_v_real += scroll_y * scroll_amount;
//    }
//    */
//    
//    container->scroll_h_visual = container->scroll_h_real;
//    container->scroll_v_visual = container->scroll_v_real;
//    ::layout(root, container, container->real_bounds);
//}
//
//
//// amount: 0 to 100
//static void
//mod_color(RGBA *color, double amount) {
//    double h; // hue
//    double s; // saturation
//    double p; // perceived brightness
//    rgb2hsluv(color->r, color->g, color->b, &h, &s, &p);
//    
//    p = p + amount;
//    
//    if (p < 0)
//        p = 0;
//    else if (p > 100)
//        p = 100;
//    hsluv2rgb(h, s, p, &color->r, &color->g, &color->b);
//}
//
//void
//darken(RGBA *color, double amount) {
//    mod_color(color, -amount);
//}
//
//void
//lighten(RGBA *color, double amount) {
//    mod_color(color, amount);
//}
//
//RGBA
//darken(RGBA color, double amount) {
//    RGBA result = color;
//    mod_color(&result, -amount);
//    return result;
//}
//
//RGBA
//lighten(RGBA color, double amount) {
//    RGBA result = color;
//    mod_color(&result, amount);
//    return result;
//}
//
//struct Config {    
//    float dpi = 1.0;
//    std::string font = "";
//    std::string icons = "Segoe Icons";
//    
//    RGBA color_taskbar_background = RGBA("#dd101010");
//    RGBA color_taskbar_button_icons = RGBA("#ffffffff");
//    RGBA color_taskbar_button_default = RGBA("#00ffffff");
//    RGBA color_taskbar_button_hovered = RGBA("#23ffffff");
//    RGBA color_taskbar_button_pressed = RGBA("#35ffffff");
//    RGBA color_taskbar_windows_button_default_icon = RGBA("#ffffffff");
//    RGBA color_taskbar_windows_button_hovered_icon = RGBA("#ff429ce3");
//    RGBA color_taskbar_windows_button_pressed_icon = RGBA("#ff0078d7");
//    RGBA color_taskbar_search_bar_default_background = RGBA("#fff3f3f3");
//    RGBA color_taskbar_search_bar_hovered_background = RGBA("#ffffffff");
//    RGBA color_taskbar_search_bar_pressed_background = RGBA("#ffffffff");
//    RGBA color_taskbar_search_bar_default_text = RGBA("#ff2b2b2b");
//    RGBA color_taskbar_search_bar_hovered_text = RGBA("#ff2d2d2d");
//    RGBA color_taskbar_search_bar_pressed_text = RGBA("#ff020202");
//    RGBA color_taskbar_search_bar_default_icon = RGBA("#ff020202");
//    RGBA color_taskbar_search_bar_default_icon_short = RGBA("#ffffffff");
//    RGBA color_taskbar_search_bar_hovered_icon = RGBA("#ff020202");
//    RGBA color_taskbar_search_bar_pressed_icon = RGBA("#ff020202");
//    RGBA color_taskbar_search_bar_default_border = RGBA("#ffb4b4b4");
//    RGBA color_taskbar_search_bar_hovered_border = RGBA("#ffb4b4b4");
//    RGBA color_taskbar_search_bar_pressed_border = RGBA("#ff0078d7");
//    RGBA color_taskbar_date_time_text = RGBA("#ffffffff");
//    RGBA color_taskbar_application_icons_background = RGBA("#ffffffff");
//    RGBA color_taskbar_application_icons_accent = RGBA("#ff76b9ed");
//    RGBA color_taskbar_minimize_line = RGBA("#ff222222");
//    RGBA color_taskbar_attention_accent = RGBA("#fffc8803");
//    RGBA color_taskbar_attention_background = RGBA("#fffc8803");
//    
//    RGBA color_systray_background = RGBA("#f3282828");
//    
//    RGBA color_battery_background = RGBA("#f31f1f1f");
//    RGBA color_battery_text = RGBA("#ffffffff");
//    RGBA color_battery_icons = RGBA("#ffffffff");
//    RGBA color_battery_slider_background = RGBA("#ff797979");
//    RGBA color_battery_slider_foreground = RGBA("#ff0178d6");
//    RGBA color_battery_slider_active = RGBA("#ffffffff");
//    
//    RGBA color_wifi_background = RGBA("#f31f1f1f");
//    RGBA color_wifi_icons = RGBA("#ffffffff");
//    RGBA color_wifi_default_button = RGBA("#00ffffff");
//    RGBA color_wifi_hovered_button = RGBA("#22ffffff");
//    RGBA color_wifi_pressed_button = RGBA("#44ffffff");
//    RGBA color_wifi_text_title = RGBA("#ffffffff");
//    RGBA color_wifi_text_title_info = RGBA("#ffadadad");
//    RGBA color_wifi_text_settings_default_title = RGBA("#ffa5d6fd");
//    RGBA color_wifi_text_settings_hovered_title = RGBA("#ffa4a4a4");
//    RGBA color_wifi_text_settings_pressed_title = RGBA("#ff787878");
//    RGBA color_wifi_text_settings_title_info = RGBA("#ffa4a4a4");
//    
//    RGBA color_date_background = RGBA("#f31f1f1f");
//    RGBA color_date_seperator = RGBA("#ff4b4b4b");
//    RGBA color_date_text = RGBA("#ffffffff");
//    RGBA color_date_text_title = RGBA("#ffffffff");
//    RGBA color_date_text_title_period = RGBA("#ffa5a5a5");
//    RGBA color_date_text_title_info = RGBA("#ffa5dafd");
//    RGBA color_date_text_month_year = RGBA("#ffdedede");
//    RGBA color_date_text_week_day = RGBA("#ffffffff");
//    RGBA color_date_text_current_month = RGBA("#ffffffff");
//    RGBA color_date_text_not_current_month = RGBA("#ff808080");
//    RGBA color_date_cal_background = RGBA("#ff006fd8");
//    RGBA color_date_cal_foreground = RGBA("#ff000000");
//    RGBA color_date_cal_border = RGBA("#ff797979");
//    RGBA color_date_weekday_monthday = RGBA("#ffffffff");
//    RGBA color_date_default_arrow = RGBA("#ffdfdfdf");
//    RGBA color_date_hovered_arrow = RGBA("#ffefefef");
//    RGBA color_date_pressed_arrow = RGBA("#ffffffff");
//    RGBA color_date_text_default_button = RGBA("#ffa5d6fd");
//    RGBA color_date_text_hovered_button = RGBA("#ffa4a4a4");
//    RGBA color_date_text_pressed_button = RGBA("#ff787878");
//    RGBA color_date_cursor = RGBA("#ffffffff");
//    RGBA color_date_text_prompt = RGBA("#ffcccccc");
//    
//    RGBA color_volume_background = RGBA("#f31f1f1f");
//    RGBA color_volume_text = RGBA("#ffffffff");
//    RGBA color_volume_default_icon = RGBA("#ffd2d2d2");
//    RGBA color_volume_hovered_icon = RGBA("#ffe8e8e8");
//    RGBA color_volume_pressed_icon = RGBA("#ffffffff");
//    RGBA color_volume_slider_background = RGBA("#ff797979");
//    RGBA color_volume_slider_foreground = RGBA("#ff0178d6");
//    RGBA color_volume_slider_active = RGBA("#ffffffff");
//    
//    RGBA color_apps_background = RGBA("#f31f1f1f");
//    RGBA color_apps_text = RGBA("#ffffffff");
//    RGBA color_apps_text_inactive = RGBA("#ff505050");
//    RGBA color_apps_icons = RGBA("#ffffffff");
//    RGBA color_apps_default_item = RGBA("#00ffffff");
//    RGBA color_apps_hovered_item = RGBA("#22ffffff");
//    RGBA color_apps_pressed_item = RGBA("#44ffffff");
//    RGBA color_apps_item_icon_background = RGBA("#ff3380cc");
//    RGBA color_apps_scrollbar_gutter = RGBA("#ff353535");
//    RGBA color_apps_scrollbar_default_thumb = RGBA("#ff5d5d5d");
//    RGBA color_apps_scrollbar_hovered_thumb = RGBA("#ff868686");
//    RGBA color_apps_scrollbar_pressed_thumb = RGBA("#ffaeaeae");
//    RGBA color_apps_scrollbar_default_button = RGBA("#ff353535");
//    RGBA color_apps_scrollbar_hovered_button = RGBA("#ff494949");
//    RGBA color_apps_scrollbar_pressed_button = RGBA("#ffaeaeae");
//    RGBA color_apps_scrollbar_default_button_icon = RGBA("#ffffffff");
//    RGBA color_apps_scrollbar_hovered_button_icon = RGBA("#ffffffff");
//    RGBA color_apps_scrollbar_pressed_button_icon = RGBA("#ff545454");
//    
//    RGBA color_pin_menu_background = RGBA("#f31f1f1f");
//    RGBA color_pin_menu_hovered_item = RGBA("#22ffffff");
//    RGBA color_pin_menu_pressed_item = RGBA("#44ffffff");
//    RGBA color_pin_menu_text = RGBA("#ffffffff");
//    RGBA color_pin_menu_icons = RGBA("#ffffffff");
//    
//    RGBA color_windows_selector_default_background = RGBA("#f3282828");
//    RGBA color_windows_selector_hovered_background = RGBA("#f33d3d3d");
//    RGBA color_windows_selector_pressed_background = RGBA("#f3535353");
//    RGBA color_windows_selector_close_icon = RGBA("#ffffffff");
//    RGBA color_windows_selector_close_icon_hovered = RGBA("#ffffffff");
//    RGBA color_windows_selector_close_icon_pressed = RGBA("#ffffffff");
//    RGBA color_windows_selector_text = RGBA("#ffffffff");
//    RGBA color_windows_selector_close_icon_hovered_background = RGBA("#ffc61a28");
//    RGBA color_windows_selector_close_icon_pressed_background = RGBA("#ffe81123");
//    RGBA color_windows_selector_attention_background = RGBA("#fffc8803");
//    
//    RGBA color_search_tab_bar_background = RGBA("#f31f1f1f");
//    RGBA color_search_accent = RGBA("#ff0078d7");
//    RGBA color_search_tab_bar_default_text = RGBA("#ffbfbfbf");
//    RGBA color_search_tab_bar_hovered_text = RGBA("#ffd9d9d9");
//    RGBA color_search_tab_bar_pressed_text = RGBA("#ffa6a6a6");
//    RGBA color_search_tab_bar_active_text = RGBA("#ffffffff");
//    RGBA color_search_empty_tab_content_background = RGBA("#f32a2a2a");
//    RGBA color_search_empty_tab_content_icon = RGBA("#ff6b6b6b");
//    RGBA color_search_empty_tab_content_text = RGBA("#ffaaaaaa");
//    RGBA color_search_content_left_background = RGBA("#fff0f0f0");
//    RGBA color_search_content_right_background = RGBA("#fff5f5f5");
//    RGBA color_search_content_right_foreground = RGBA("#ffffffff");
//    RGBA color_search_content_right_splitter = RGBA("#fff2f2f2");
//    RGBA color_search_content_text_primary = RGBA("#ff010101");
//    RGBA color_search_content_text_secondary = RGBA("#ff606060");
//    RGBA color_search_content_right_button_default = RGBA("#00000000");
//    RGBA color_search_content_right_button_hovered = RGBA("#26000000");
//    RGBA color_search_content_right_button_pressed = RGBA("#51000000");
//    RGBA color_search_content_left_button_splitter = RGBA("#ffffffff");
//    RGBA color_search_content_left_button_default = RGBA("#00000000");
//    RGBA color_search_content_left_button_hovered = RGBA("#24000000");
//    RGBA color_search_content_left_button_pressed = RGBA("#48000000");
//    RGBA color_search_content_left_button_active = RGBA("#ffa8cce9");
//    RGBA color_search_content_left_set_active_button_default = RGBA("#00000000");
//    RGBA color_search_content_left_set_active_button_hovered = RGBA("#22000000");
//    RGBA color_search_content_left_set_active_button_pressed = RGBA("#19000000");
//    RGBA color_search_content_left_set_active_button_active = RGBA("#ff97b8d2");
//    RGBA color_search_content_left_set_active_button_icon_default = RGBA("#ff606060");
//    RGBA color_search_content_left_set_active_button_icon_pressed = RGBA("#ffffffff");
//    
//    RGBA color_pinned_icon_editor_background = RGBA("#ffffffff");
//    RGBA color_pinned_icon_editor_field_default_text = RGBA("#ff000000");
//    RGBA color_pinned_icon_editor_field_hovered_text = RGBA("#ff2d2d2d");
//    RGBA color_pinned_icon_editor_field_pressed_text = RGBA("#ff020202");
//    RGBA color_pinned_icon_editor_field_default_border = RGBA("#ffb4b4b4");
//    RGBA color_pinned_icon_editor_field_hovered_border = RGBA("#ff646464");
//    RGBA color_pinned_icon_editor_field_pressed_border = RGBA("#ff0078d7");
//    RGBA color_pinned_icon_editor_cursor = RGBA("#ff000000");
//    RGBA color_pinned_icon_editor_button_default = RGBA("#ffcccccc");
//    RGBA color_pinned_icon_editor_button_text_default = RGBA("#ff000000");
//    
//    RGBA color_notification_content_background = RGBA("#ff1f1f1f");
//    RGBA color_notification_title_background = RGBA("#ff191919");
//    RGBA color_notification_content_text = RGBA("#ffffffff");
//    RGBA color_notification_title_text = RGBA("#ffffffff");
//    RGBA color_notification_button_default = RGBA("#ff545454");
//    RGBA color_notification_button_hovered = RGBA("#ff616161");
//    RGBA color_notification_button_pressed = RGBA("#ff474747");
//    RGBA color_notification_button_text_default = RGBA("#ffffffff");
//    RGBA color_notification_button_text_hovered = RGBA("#ffffffff");
//    RGBA color_notification_button_text_pressed = RGBA("#ffffffff");
//    RGBA color_notification_button_send_to_action_center_default = RGBA("#ff9c9c9c");
//    RGBA color_notification_button_send_to_action_center_hovered = RGBA("#ffcccccc");
//    RGBA color_notification_button_send_to_action_center_pressed = RGBA("#ff888888");
//    
//    RGBA color_action_center_background = RGBA("#ff1f1f1f");
//    RGBA color_action_center_history_text = RGBA("#ffa5d6fd");
//    RGBA color_action_center_no_new_text = RGBA("#ffffffff");
//    RGBA color_action_center_notification_content_background = RGBA("#ff282828");
//    RGBA color_action_center_notification_title_background = RGBA("#ff1f1f1f");
//    RGBA color_action_center_notification_content_text = RGBA("#ffffffff");
//    RGBA color_action_center_notification_title_text = RGBA("#ffffffff");
//    RGBA color_action_center_notification_button_default = RGBA("#ff545454");
//    RGBA color_action_center_notification_button_hovered = RGBA("#ff616161");
//    RGBA color_action_center_notification_button_pressed = RGBA("#ff474747");
//    RGBA color_action_center_notification_button_text_default = RGBA("#ffffffff");
//    RGBA color_action_center_notification_button_text_hovered = RGBA("#ffffffff");
//    RGBA color_action_center_notification_button_text_pressed = RGBA("#ffffffff");
//};
//
//Config *config = new Config;
//
//void draw_colored_rect(const RGBA &color, const Bounds &bounds) {
//#ifdef TRACY_ENABLE
//    ZoneScoped;
//#endif
//    rect(bounds, color);
//}
//
//void draw_margins_rect(const RGBA &color, const Bounds &bounds, float amount, float space) {
//#ifdef TRACY_ENABLE
//    ZoneScoped;
//#endif
//    border(bounds, color, amount);
//}
//
//static long ms_between = 0;
//
//void fine_scrollpane_scrolled(Container *root,
//                              Container *container,
//                              int scroll_x,
//                              int scroll_y,
//                              bool came_from_touchpad) {
//#ifdef TRACY_ENABLE
//    ZoneScoped;
//#endif
//
//    container->scroll_h_visual = container->scroll_h_real;
//    container->scroll_v_visual = container->scroll_v_real;
//    ::layout(root, container, container->real_bounds);
//}

static void
paint_scroll_bg(Container *root, Container *container) {
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif
    auto scroll_container = ((ScrollContainer *) container->parent->parent);
    RGBA color = config->color_apps_scrollbar_gutter;
    color.a = scroll_container->scrollbar_openess;
    draw_colored_rect(root, scroll_container, color, container->real_bounds);
}

static void
paint_right_thumb(Container *root, Container *container) {
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif
    auto scroll_container = ((ScrollContainer *) container->parent->parent);
    bool minimal = scroll_container->settings.paint_minimal;
    
    if (!minimal)
        paint_scroll_bg(root, container);
    
    Container *scrollpane = container->parent->parent;
    
    auto right_bounds = right_thumb_bounds(scrollpane, container->real_bounds);
    auto data = (ScrollData *) scroll_container->user_data;
    float dpi = 1.0;
    if (data->func) {
        auto ctx = data->func(root);
        dpi = ctx.dpi;
    }
        
    int small_width = std::round(4.0 * dpi);
    
    right_bounds.x += right_bounds.w;
    right_bounds.w = std::max(right_bounds.w * scroll_container->scrollbar_openess, (double) small_width);
    if (minimal)
        right_bounds.w = small_width;
    right_bounds.x -= right_bounds.w;
    right_bounds.x -= ((float) small_width) * (1 - scroll_container->scrollbar_openess);
    
    RGBA color = config->color_apps_scrollbar_default_thumb;
    if (container->state.mouse_pressing) {
        color = config->color_apps_scrollbar_pressed_thumb;
    } else if (bounds_contains(right_bounds, root->mouse_current_x, root->mouse_current_y)) {
        color = config->color_apps_scrollbar_hovered_thumb;
    } else if (right_bounds.w == small_width) {
        color = config->color_apps_scrollbar_default_thumb;
        lighten(&color, 10);
    }
    color.a = scroll_container->scrollbar_visible;
    
    draw_colored_rect(root, scroll_container, color, right_bounds);
}

//
//static void
//paint_bottom_thumb(Container *root, Container *container) {
//#ifdef TRACY_ENABLE
//    ZoneScoped;
//#endif
//    bool minimal = ((ScrollContainer *) container->parent->parent)->settings.paint_minimal;
//    auto scroll_container = ((ScrollContainer *) container->parent->parent);
//    
//    if (!minimal)
//        paint_scroll_bg(root, container);
//    
//    Container *scrollpane = container->parent->parent;
//    
//    auto bottom_bounds = bottom_thumb_bounds(scrollpane, container->real_bounds);
//    
//    bottom_bounds.y += bottom_bounds.h;
//    bottom_bounds.h = std::max(bottom_bounds.h * scroll_container->scrollbar_openess, 2.0);
//    bottom_bounds.y -= bottom_bounds.h;
//    bottom_bounds.y -= 2 * (1 - scroll_container->scrollbar_openess);
//    
//    RGBA color = config->color_apps_scrollbar_default_thumb;
//    
//    if (container->state.mouse_pressing) {
//        color = config->color_apps_scrollbar_pressed_thumb;
//    } else if (bounds_contains(bottom_bounds, root->mouse_current_x, root->mouse_current_y)) {
//        color = config->color_apps_scrollbar_hovered_thumb;
//    } else if (bottom_bounds.w == 2.0) {
//        color = config->color_apps_scrollbar_default_thumb;
//        lighten(&color, 10);
//    }
//    color.a = scroll_container->scrollbar_visible;
//    
//    draw_colored_rect(color, bottom_bounds);
//}
//
//static void
//paint_arrow(Container *root, Container *container) {
//#ifdef TRACY_ENABLE
//    ZoneScoped;
//#endif
//    bool minimal = ((ScrollContainer *) container->parent->parent)->settings.paint_minimal;
//    if (minimal)
//        return;
//    auto scroll_container = ((ScrollContainer *) container->parent->parent);
//    
//    auto *data = (ButtonData *) container->user_data;
//    
//    if (container->state.mouse_pressing || container->state.mouse_hovering) {
//        if (container->state.mouse_pressing) {
//            RGBA color = config->color_apps_scrollbar_pressed_button;
//            color.a = scroll_container->scrollbar_openess;
//            draw_colored_rect(color, container->real_bounds);
//        } else {
//            RGBA color = config->color_apps_scrollbar_hovered_button;
//            color.a = scroll_container->scrollbar_openess;
//            draw_colored_rect(color, container->real_bounds);
//        }
//    } else {
//        RGBA color = config->color_apps_scrollbar_default_button;
//        color.a = scroll_container->scrollbar_openess;
//        draw_colored_rect(color, container->real_bounds);
//    }
//    
//    RGBA color;
//    if (container->state.mouse_pressing || container->state.mouse_hovering) {
//        if (container->state.mouse_pressing) {
//            color = RGBA(config->color_apps_scrollbar_pressed_button_icon);
//            color.a = scroll_container->scrollbar_openess;
//        } else {
//            color = RGBA(config->color_apps_scrollbar_hovered_button_icon);
//            color.a = scroll_container->scrollbar_openess;
//        }
//    } else {
//        color = RGBA(config->color_apps_scrollbar_default_button_icon);
//        color.a = scroll_container->scrollbar_openess;
//    }
//    
//    draw_text(root, 6 * config->dpi, config->icons, EXPAND(color), data->text, container->real_bounds);
//}
//
//static void
//paint_show(Container *root, Container *container) {
//#ifdef TRACY_ENABLE
//    ZoneScoped;
//#endif
//    if (container->state.mouse_hovering || container->state.mouse_pressing) {
//        if (container->state.mouse_pressing) {
//            draw_colored_rect(RGBA(0, 0, 0, .3), container->real_bounds);
//        } else {
//            draw_colored_rect(RGBA(0, 0, 0, .1), container->real_bounds);
//        }
//    }
//
//    auto color = RGBA(0, 1, 1, 1);
//    if (container->parent->active)
//        color = RGBA(1, 0, 1, 1);
//    
//    draw_margins_rect(color, container->real_bounds, 1, 0);
//}
//
//struct MouseDownInfo {
//    Container *container = nullptr;
//    int horizontal_change = 0;
//    int vertical_change = 0;
//};
//
//static void
//mouse_down_thread(App *app, Container *root, Timeout *, void *data) {
//    auto *mouse_info = (MouseDownInfo *) data;
//    
//    if (mouse_down_arrow_held.load()) {
//        if (bounds_contains(mouse_info->container->real_bounds, root->mouse_current_x, root->mouse_current_y)) {
//            Container *target = nullptr;
//            bool clamp = false;
//            if (auto *s = dynamic_cast<ScrollContainer *>(mouse_info->container->parent->parent)) {
//                target = s;
//                clamp = true;
//            } else {
//                target = mouse_info->container->parent->parent->children[2];
//            }
//        
//            target->scroll_h_real += mouse_info->horizontal_change * 1.5;
//            target->scroll_v_real += mouse_info->vertical_change * 1.5;
//            if (clamp) clamp_scroll((ScrollContainer *) target);
//        
//            // ::layout(target, target->real_bounds, false);
////            client_layout(client->app, client);
//
//        /*
//            if (mouse_info->horizontal_change != 0)
//                client_create_animation(client->app,
//                                        client,
//                                        &target->scroll_h_visual, target->lifetime, 0,
//                                        scroll_anim_time,
//                                        easing_function,
//                                        target->scroll_h_real,
//                                        true);
//            if (mouse_info->vertical_change != 0)
//                client_create_animation(client->app,
//                                        client,
//                                        &target->scroll_v_visual, target->lifetime, 0,
//                                        scroll_anim_time,
//                                        easing_function,
//                                        target->scroll_v_real,
//                                        true);
//            
//            app_timeout_create(app, client, repeat_time, mouse_down_thread, mouse_info, const_cast<char *>(__PRETTY_FUNCTION__));
//            */
//        } else {
//            //app_timeout_create(app, client, repeat_time, mouse_down_thread, mouse_info, const_cast<char *>(__PRETTY_FUNCTION__));
//        }
//    } else {
//        delete mouse_info;
//    }
//}
//
//static void
//mouse_down_arrow_up(Container *root, Container *container) {
//#ifdef TRACY_ENABLE
//    ZoneScoped;
//#endif
//    Container *target = nullptr;
//    bool clamp = false;
//    if (auto *s = dynamic_cast<ScrollContainer *>(container->parent->parent)) {
//        target = s;
//        clamp = true;
//    } else {
//        target = container->parent->parent->children[2];
//    }
//    target->scroll_v_real += scroll_amount;
//    if (clamp) clamp_scroll((ScrollContainer *) target);
//    
//    // ::layout(target, target->real_bounds, false);
//    //client_layout(client->app, client);
//    //::layout();
//
//    /*
//    client_create_animation(client->app,
//                            client,
//                            &target->scroll_h_visual, target->lifetime, 0,
//                            scroll_anim_time,
//                            easing_function,
//                            target->scroll_h_real,
//                            true);
//    client_create_animation(client->app,
//                            client,
//                            &target->scroll_v_visual, target->lifetime, 0,
//                            scroll_anim_time,
//                            easing_function,
//                            target->scroll_v_real,
//                            true);
//    */
//    
//    if (mouse_down_arrow_held.load())
//        return;
//    mouse_down_arrow_held.store(true);
//    auto *data = new MouseDownInfo;
//    data->container = container;
//    data->vertical_change = scroll_amount;
//    //app_timeout_create(client->app, client, scroll_anim_time * 1.56, mouse_down_thread, data, const_cast<char *>(__PRETTY_FUNCTION__));
//}

static void
mouse_down_arrow_bottom(Container *root, Container *container, bool first_time = true) {
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif
    auto target = (ScrollContainer *) container->parent->parent;
    bool clamp = true;
    target->scroll_v_real -= scroll_amount;
    target->scroll_v_visual = target->scroll_v_real;
    if (clamp) clamp_scroll((ScrollContainer *) target);
    ::layout(root, container, container->real_bounds);
    std::weak_ptr<bool> lifetime = container->lifetime;
    later(first_time ? 600 : 150, [lifetime, container, root, target](Timer *) {
        if (lifetime.lock()) {
            if (container->state.mouse_pressing) {
                mouse_down_arrow_bottom(root, container, false);
                auto scroll_data = (ScrollData *) target->user_data;
                auto ctx = scroll_data->func(root);
                ctx.on_needs_frame();
            }
        }
    });
    
}

static void
mouse_down_arrow_up(Container *root, Container *container, bool first_time = true) {
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif
    auto target = (ScrollContainer *) container->parent->parent;
    bool clamp = true;
    target->scroll_v_real += scroll_amount;
    target->scroll_v_visual = target->scroll_v_real;
    if (clamp) clamp_scroll((ScrollContainer *) target);
    ::layout(root, container, container->real_bounds);
    std::weak_ptr<bool> lifetime = container->lifetime;
    later(first_time ? 600 : 150, [lifetime, container, root, target](Timer *) {
        if (lifetime.lock()) {
            if (container->state.mouse_pressing) {
                mouse_down_arrow_up(root, container, false);
                auto scroll_data = (ScrollData *) target->user_data;
                auto ctx = scroll_data->func(root);
                ctx.on_needs_frame();
            }
        }
    });
}

//static void
//mouse_down_arrow_bottom(Container *root, Container *container) {
//#ifdef TRACY_ENABLE
//    ZoneScoped;
//#endif
//    Container *target = nullptr;
//    bool clamp = false;
//    if (auto *s = dynamic_cast<ScrollContainer *>(container->parent->parent)) {
//        target = s;
//        clamp = true;
//    } else {
//        target = container->parent->parent->children[2];
//    }
//    target->scroll_v_real -= scroll_amount;
//    if (clamp) clamp_scroll((ScrollContainer *) target);
//    // ::layout(target, target->real_bounds, false);
//    //client_layout(client->app, client);
//
//    /*
//    client_create_animation(client->app,
//                            client,
//                            &target->scroll_h_visual, target->lifetime, 0,
//                            scroll_anim_time,
//                            easing_function,
//                            target->scroll_h_real,
//                            true);
//    client_create_animation(client->app,
//                            client,
//                            &target->scroll_v_visual, target->lifetime, 0,
//                            scroll_anim_time,
//                            easing_function,
//                            target->scroll_v_real,
//                            true);
//    
//    if (mouse_down_arrow_held.load())
//        return;
//    mouse_down_arrow_held.store(true);
//    auto *data = new MouseDownInfo;
//    data->container = container;
//    data->vertical_change = -scroll_amount;
//    app_timeout_create(client->app, client, scroll_anim_time * 1.56, mouse_down_thread, data, const_cast<char *>(__PRETTY_FUNCTION__));
//    */
//}
//
//static void
//mouse_down_arrow_left(Container *root, Container *container) {
//#ifdef TRACY_ENABLE
//    ZoneScoped;
//#endif
//    Container *target = nullptr;
//    bool clamp = false;
//    if (auto *s = dynamic_cast<ScrollContainer *>(container->parent->parent)) {
//        target = s;
//        clamp = true;
//    } else {
//        target = container->parent->parent->children[2];
//    }
//    target->scroll_h_real += scroll_amount;
//    if (clamp) clamp_scroll((ScrollContainer *) target);
//    // ::layout(target, target->real_bounds, false);
//    /*
//    client_layout(client->app, client);
//    
//    client_create_animation(client->app,
//                            client,
//                            &target->scroll_h_visual, target->lifetime, 0,
//                            scroll_anim_time,
//                            easing_function,
//                            target->scroll_h_real,
//                            true);
//    client_create_animation(client->app,
//                            client,
//                            &target->scroll_v_visual, target->lifetime, 0,
//                            scroll_anim_time,
//                            easing_function,
//                            target->scroll_v_real,
//                            true);
//    
//    if (mouse_down_arrow_held.load())
//        return;
//    mouse_down_arrow_held.store(true);
//    auto *data = new MouseDownInfo;
//    data->container = container;
//    data->horizontal_change = scroll_amount;
//    app_timeout_create(client->app, client, scroll_anim_time * 1.56, mouse_down_thread, data, const_cast<char *>(__PRETTY_FUNCTION__));
//    */
//}
//
//static void
//mouse_down_arrow_right(Container *root, Container *container) {
//#ifdef TRACY_ENABLE
//    ZoneScoped;
//#endif
//    Container *target = nullptr;
//    bool clamp = false;
//    if (auto *s = dynamic_cast<ScrollContainer *>(container->parent->parent)) {
//        target = s;
//        clamp = true;
//    } else {
//        target = container->parent->parent->children[2];
//    }
//    target->scroll_h_real -= scroll_amount;
//    if (clamp) clamp_scroll((ScrollContainer *) target);
//    // ::layout(target, target->real_bounds, false);
//    /*client_layout(client->app, client);
//    
//    client_create_animation(client->app,
//                            client,
//                            &target->scroll_h_visual, target->lifetime, 0,
//                            scroll_anim_time,
//                            easing_function,
//                            target->scroll_h_real,
//                            true);
//    client_create_animation(client->app,
//                            client,
//                            &target->scroll_v_visual, target->lifetime, 0,
//                            scroll_anim_time,
//                            easing_function,
//                            target->scroll_v_real,
//                            true);
//    
//    if (mouse_down_arrow_held.load())
//        return;
//    mouse_down_arrow_held.store(true);
//    auto *data = new MouseDownInfo;
//    data->container = container;
//    data->horizontal_change = -scroll_amount;
//    app_timeout_create(client->app, client, scroll_anim_time * 1.56, mouse_down_thread, data, const_cast<char *>(__PRETTY_FUNCTION__));
//    */
//}
//
//static void
//right_thumb_scrolled(Container *root, Container *container, int scroll_x, int scroll_y) {
//#ifdef TRACY_ENABLE
//    ZoneScoped;
//#endif
//    if (auto *s = dynamic_cast<ScrollContainer *>(container)) {
//        fine_scrollpane_scrolled(root, container, 0, scroll_y * 120, false);
//    } else {
//        Container *target = container->parent->children[2];
//        scrollpane_scrolled(root, target, 0, scroll_y);
//    }
//}
//
//static void
//bottom_thumb_scrolled(AppClient *client, cairo_t *cr, Container *container, int scroll_x, int scroll_y) {
//#ifdef TRACY_ENABLE
//    ZoneScoped;
//#endif
//    if (auto *s = dynamic_cast<ScrollContainer *>(container)) {
//        fine_scrollpane_scrolled(client, cr, container, scroll_x * 120, 0, false);
//    } else {
//        Container *target = container->parent->children[2];
//        scrollpane_scrolled(client, cr, target, scroll_x, 0);
//    }
//}
//
//static void
//fine_right_thumb_scrolled(Container *root, Container *container, int scroll_x, int scroll_y,
//                          bool came_from_touchpad) {
//#ifdef TRACY_ENABLE
//    ZoneScoped;
//#endif
//    if (auto *s = dynamic_cast<ScrollContainer *>(container->parent)) {
//        fine_scrollpane_scrolled(root, container->parent, scroll_x, scroll_y, came_from_touchpad);
//    }
//}
//
//static void
//fine_bottom_thumb_scrolled(Container *root, Container *container, int scroll_x, int scroll_y,
//                           bool came_from_touchpad) {
//#ifdef TRACY_ENABLE
//    ZoneScoped;
//#endif
//    if (auto *s = dynamic_cast<ScrollContainer *>(container->parent)) {
//        fine_scrollpane_scrolled(root, container->parent, scroll_x, scroll_y, came_from_touchpad);
//    }
//}
//
static void
mouse_arrow_up(Container *root, Container *container) {
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif
    //mouse_down_arrow_held = false;
}
//
Bounds
right_thumb_bounds(Container *scrollpane, Bounds thumb_area) {
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif
    if (auto *s = dynamic_cast<ScrollContainer *>(scrollpane)) {
        double true_height = actual_true_height(s->content);
        if (s->bottom && s->bottom->exists && !s->settings.bottom_inline_track)
            true_height += s->bottom->real_bounds.h;
        
        double start_scalar = -s->scroll_v_visual / true_height;
        double thumb_height = thumb_area.h * (s->real_bounds.h / true_height);
        double start_off = thumb_area.y + thumb_area.h * start_scalar;
        double avail = thumb_area.h - (start_off - thumb_area.y);
        
        return {thumb_area.x, start_off, thumb_area.w, std::min(thumb_height, avail)};
    }
    
    auto content_area = scrollpane->children[2];
    double total_height = content_area->children[0]->real_bounds.h;
    
    double view_height = content_area->real_bounds.h;
    
    double view_scalar = view_height / total_height;
    double thumb_height = view_scalar * thumb_area.h;
    
    // 0 as min pos and 1 as max position
    double max_scroll = total_height - view_height;
    if (max_scroll < 0)
        max_scroll = 0;
    
    double scroll_scalar = (-content_area->scroll_v_visual) / max_scroll;
    double scroll_offset = (thumb_area.h - thumb_height) * scroll_scalar;
    if (max_scroll == 0) {
        scroll_offset = 0;
    }
    if (thumb_height > thumb_area.h) {
        thumb_height = thumb_area.h;
    }
    return Bounds(thumb_area.x, thumb_area.y + scroll_offset, thumb_area.w, thumb_height);
}
//
//Bounds
//bottom_thumb_bounds(Container *scrollpane, Bounds thumb_area) {
//#ifdef TRACY_ENABLE
//    ZoneScoped;
//#endif
//    if (auto *s = dynamic_cast<ScrollContainer *>(scrollpane)) {
//        double true_width = actual_true_width(s->content);
//        if (s->right && s->right->exists && !s->settings.right_inline_track)
//            true_width += s->right->real_bounds.w;
//        
//        double start_scalar = -s->scroll_h_visual / true_width;
//        double thumb_width = thumb_area.w * (s->real_bounds.w / true_width);
//        double start_off = thumb_area.x + thumb_area.w * start_scalar;
//        double avail = thumb_area.w - (start_off - thumb_area.x);
//        
//        return {thumb_area.x + thumb_area.w * start_scalar, thumb_area.y, std::min(thumb_width, avail), thumb_area.h};
//    }
//    
//    auto content_area = scrollpane->children[2];
//    double total_width = content_area->children[0]->real_bounds.w;
//    
//    double view_width = content_area->real_bounds.w;
//    
//    if (total_width == 0) {
//        total_width = view_width;
//    }
//    double view_scalar = view_width / total_width;
//    double thumb_width = view_scalar * thumb_area.w;
//    
//    // 0 as min pos and 1 as max position
//    double max_scroll = total_width - view_width;
//    if (max_scroll < 0)
//        max_scroll = 0;
//    
//    double scroll_scalar = (-content_area->scroll_h_visual) / max_scroll;
//    double scroll_offset = (thumb_area.w - thumb_width) * scroll_scalar;
//    if (max_scroll == 0) {
//        scroll_offset = 0;
//    }
//    
//    if (thumb_width > thumb_area.w) {
//        thumb_width = thumb_area.w;
//    }
//    return Bounds(thumb_area.x + scroll_offset, thumb_area.y, thumb_width, thumb_area.h);
//}
//
static void
clicked_right_thumb(Container *root, Container *thumb_container, bool animate) {
    if (auto *s = dynamic_cast<ScrollContainer *>(thumb_container->parent->parent)) {
        auto *content = s->content;
        
        double thumb_height = right_thumb_bounds(s, thumb_container->real_bounds).h;
        double mouse_y = root->mouse_current_y;
        if (mouse_y < thumb_container->real_bounds.y) {
            mouse_y = thumb_container->real_bounds.y;
        } else if (mouse_y > thumb_container->real_bounds.y + thumb_container->real_bounds.h) {
            mouse_y = thumb_container->real_bounds.y + thumb_container->real_bounds.h;
        }
        
        mouse_y -= thumb_container->real_bounds.y;
        mouse_y -= thumb_height / 2;
        double scalar = mouse_y / thumb_container->real_bounds.h;
        
         //why the fuck do I have to add the real...h to true height to actually get
         //the true height
        double true_height = actual_true_height(s->content);
        if (s->bottom && s->bottom->exists && !s->settings.bottom_inline_track)
            true_height += s->bottom->real_bounds.h;
        
        double y = true_height * scalar;
        
        s->scroll_v_real = -y;
        s->scroll_v_visual = -y;
        ::layout(root, s, s->real_bounds);
        
        return;
    }
}
//
//static void
//clicked_bottom_thumb(Container *root, Container *thumb_container, bool animate) {
//    if (auto *s = dynamic_cast<ScrollContainer *>(thumb_container->parent->parent)) {
//        auto *content = s->content;
//        
//        double thumb_width =
//                bottom_thumb_bounds(thumb_container->parent->parent, thumb_container->real_bounds).w;
//        double mouse_x = root->mouse_current_x;
//        if (mouse_x < thumb_container->real_bounds.x) {
//            mouse_x = thumb_container->real_bounds.x;
//        } else if (mouse_x > thumb_container->real_bounds.x + thumb_container->real_bounds.w) {
//            mouse_x = thumb_container->real_bounds.x + thumb_container->real_bounds.w;
//        }
//        
//        mouse_x -= thumb_container->real_bounds.x;
//        mouse_x -= thumb_width / 2;
//        double scalar = mouse_x / thumb_container->real_bounds.w;
//        
//        double true_width = actual_true_width(s->content);
//        if (s->right && s->right->exists && !s->settings.right_inline_track)
//            true_width += s->right->real_bounds.w;
//        
//        double x = true_width * scalar;
//        
//        s->scroll_h_real = -x;
//        s->scroll_h_visual = -x;
//        ::layout(root, s, s->real_bounds);
//        
//        return;
//    }
//    
//    auto *scrollpane = thumb_container->parent->parent;
//    auto *content = scrollpane->children[2];
//    
//    double thumb_width =
//            bottom_thumb_bounds(thumb_container->parent->parent, thumb_container->real_bounds).w;
//    double mouse_x = root->mouse_current_x;
//    if (mouse_x < thumb_container->real_bounds.x) {
//        mouse_x = thumb_container->real_bounds.x;
//    } else if (mouse_x > thumb_container->real_bounds.x + thumb_container->real_bounds.w) {
//        mouse_x = thumb_container->real_bounds.x + thumb_container->real_bounds.w;
//    }
//    
//    mouse_x -= thumb_container->real_bounds.x;
//    mouse_x -= thumb_width / 2;
//    double scalar = mouse_x / thumb_container->real_bounds.w;
//    
//    // why the fuck do I have to add the real...w to true width to actually get
//    // the true width
//    double content_width = true_width(content) + content->real_bounds.w;
//    double x = content_width * scalar;
//    
//    Container *content_area = thumb_container->parent->parent->children[2];
//    content_area->scroll_h_real = -x;
//    if (!animate)
//        content_area->scroll_h_visual = -x;
//    // ::layout(content_area, content_area->real_bounds, false);
//    /*client_layout(client->app, client);
//    
//    if (animate) {
//        client_create_animation(client->app,
//                                client,
//                                &content_area->scroll_h_visual, content_area->lifetime, 0,
//                                scroll_anim_time * 2,
//                                easing_function,
//                                content_area->scroll_h_real,
//                                true);
//        client_create_animation(client->app,
//                                client,
//                                &content_area->scroll_v_visual, content_area->lifetime, 0,
//                                scroll_anim_time * 2,
//                                easing_function,
//                                content_area->scroll_v_real,
//                                true);
//    } else {
//        client_create_animation(client->app,
//                                client,
//                                &content_area->scroll_h_visual, content_area->lifetime, 0,
//                                0,
//                                easing_function,
//                                content_area->scroll_h_real,
//                                true);
//        client_create_animation(client->app,
//                                client,
//                                &content_area->scroll_v_visual, content_area->lifetime, 0,
//                                0,
//                                easing_function,
//                                content_area->scroll_v_real,
//                                true);
//    }
//    */
//}
//
static void
right_scrollbar_mouse_down(Container *root, Container *container) {
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif
    clicked_right_thumb(root, container, true);
}

static void
right_scrollbar_drag_start(Container *root, Container *container) {
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif
    clicked_right_thumb(root, container, false);
}
//
static void
right_scrollbar_drag(Container *root, Container *container) {
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif
    clicked_right_thumb(root, container, false);
}

static void
right_scrollbar_drag_end(Container *root, Container *container) {
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif
    clicked_right_thumb(root, container, false);
}
//
//static void
//bottom_scrollbar_mouse_down(Container *root, Container *container) {
//#ifdef TRACY_ENABLE
//    ZoneScoped;
//#endif
//    clicked_bottom_thumb(root, container, true);
//}
//
//static void
//bottom_scrollbar_drag_start(Container *root, Container *container) {
//#ifdef TRACY_ENABLE
//    ZoneScoped;
//#endif
//    clicked_bottom_thumb(root, container, false);
//}
//
//static void
//bottom_scrollbar_drag(Container *root, Container *container) {
//#ifdef TRACY_ENABLE
//    ZoneScoped;
//#endif
//    clicked_bottom_thumb(root, container, false);
//}
//
//static void
//bottom_scrollbar_drag_end(Container *root, Container *container) {
//#ifdef TRACY_ENABLE
//    ZoneScoped;
//#endif
//    clicked_bottom_thumb(root, container, false);
//}
//
//static void
//paint_content(Container *root, Container *container) {
//#ifdef TRACY_ENABLE
//    ZoneScoped;
//#endif
//    //cairo_save(cr);
//    //cairo_push_group(cr);
//    
//    for (auto *c: container->children) {
//        if (overlaps(c->real_bounds, c->parent->parent->real_bounds)) {
//            if (c->when_paint) {
//                c->when_paint(root, c);
//            }
//        }
//    }
//    
//    /*cairo_pop_group_to_source(cr);
//    
//    cairo_rectangle(cr,
//                    container->parent->real_bounds.x,
//                    container->parent->real_bounds.y,
//                    container->parent->real_bounds.w,
//                    container->parent->real_bounds.h);
//    cairo_clip(cr);
//    cairo_paint(cr);
//    cairo_restore(cr);*/
//}
//
//Container *
//make_scrollpane(Container *parent, ScrollPaneSettings settings) {
//#ifdef TRACY_ENABLE
//    ZoneScoped;
//#endif
//    auto scrollable_pane = parent->child(FILL_SPACE, FILL_SPACE);
//    scrollable_pane->type = ::scrollpane;
//    if (settings.bottom_inline_track)
//        scrollable_pane->type |= ::scrollpane_inline_b;
//    if (settings.right_inline_track)
//        scrollable_pane->type |= ::scrollpane_inline_r;
//    
//    scrollable_pane->type |= (1 << (settings.bottom_show_amount + 8));
//    scrollable_pane->type |= (1 << (settings.right_show_amount + 5));
//    
//    auto right_vbox = scrollable_pane->child(settings.right_width, FILL_SPACE);
//    right_vbox->type = ::vbox;
//    right_vbox->when_scrolled = right_thumb_scrolled;
//    
//    auto right_top_arrow = right_vbox->child(FILL_SPACE, settings.right_arrow_height);
//    right_top_arrow->when_paint = paint_show;
//    right_top_arrow->when_mouse_down = mouse_down_arrow_up;
//    right_top_arrow->when_mouse_up = mouse_arrow_up;
//    right_top_arrow->when_clicked = mouse_arrow_up;
//    right_top_arrow->when_drag_end = mouse_arrow_up;
//    auto right_thumb = right_vbox->child(FILL_SPACE, FILL_SPACE);
//    right_thumb->when_paint = paint_show;
//    right_thumb->when_drag_start = right_scrollbar_drag_start;
//    right_thumb->when_drag = right_scrollbar_drag;
//    right_thumb->when_drag_end = right_scrollbar_drag_end;
//    right_thumb->when_mouse_down = right_scrollbar_mouse_down;
//    
//    auto right_bottom_arrow = right_vbox->child(FILL_SPACE, settings.right_arrow_height);
//    right_bottom_arrow->when_paint = paint_show;
//    right_bottom_arrow->when_mouse_down = mouse_down_arrow_bottom;
//    right_bottom_arrow->when_mouse_up = mouse_arrow_up;
//    right_bottom_arrow->when_clicked = mouse_arrow_up;
//    right_bottom_arrow->when_drag_end = mouse_arrow_up;
//    right_vbox->z_index += 1;
//    
//    auto bottom_hbox = scrollable_pane->child(FILL_SPACE, settings.bottom_height);
//    bottom_hbox->type = ::hbox;
//    auto bottom_left_arrow = bottom_hbox->child(settings.bottom_arrow_width, FILL_SPACE);
//    bottom_left_arrow->when_paint = paint_show;
//    bottom_left_arrow->when_mouse_down = mouse_down_arrow_left;
//    bottom_left_arrow->when_mouse_up = mouse_arrow_up;
//    bottom_left_arrow->when_clicked = mouse_arrow_up;
//    bottom_left_arrow->when_drag_end = mouse_arrow_up;
//    auto bottom_thumb = bottom_hbox->child(FILL_SPACE, FILL_SPACE);
//    bottom_thumb->when_paint = paint_show;
//    bottom_thumb->when_drag_start = bottom_scrollbar_drag_start;
//    bottom_thumb->when_drag = bottom_scrollbar_drag;
//    bottom_thumb->when_drag_end = bottom_scrollbar_drag_end;
//    bottom_thumb->when_mouse_down = bottom_scrollbar_mouse_down;
//    
//    auto bottom_right_arrow = bottom_hbox->child(settings.bottom_arrow_width, FILL_SPACE);
//    bottom_right_arrow->when_paint = paint_show;
//    bottom_right_arrow->when_mouse_down = mouse_down_arrow_right;
//    bottom_right_arrow->when_mouse_up = mouse_arrow_up;
//    bottom_right_arrow->when_clicked = mouse_arrow_up;
//    bottom_right_arrow->when_drag_end = mouse_arrow_up;
//    bottom_hbox->z_index += 1;
//    
//    auto content_container = scrollable_pane->child(FILL_SPACE, FILL_SPACE);
//    //    content_container->when_paint = paint_show;
//    content_container->when_scrolled = scrollpane_scrolled;
//    content_container->receive_events_even_if_obstructed = true;
//    
//    if (settings.make_content) {
//        // scroll_pane->children[2]->chilren[0]
//        auto content = content_container->child(FILL_SPACE, FILL_SPACE);
//        content->spacing = 0;
//        content->when_paint = paint_content;
//        content->clip_children = false; // We have to do custom clipping so don't waste calls on this
//        content->automatically_paint_children = false;
//        content->name = "content";
//    }
//    
//    return content_container;
//}
//
///*


/*
void scroll_hover_timeout(App *, AppClient *client, Timeout *,
                          void *userdata) {
    auto *container = (Container *) userdata;
    if (container_by_container(container, client->root)) {
        auto *scroll = (ScrollContainer *) container;
        scroll->openess_delay_timeout = nullptr;
        if (!bounds_contains(scroll->right->real_bounds, client->mouse_current_x, client->mouse_current_y)) {
            client_create_animation(client->app, client, &scroll->scrollbar_openess, scroll->lifetime,
                                    0, 100, nullptr, 0);
        }
    }
}
*/

void ignore_first_hover(Container *root, ScrollContainer *scroll) {
    if (bounds_contains(scroll->right->real_bounds, root->mouse_current_x, root->mouse_current_y)) {
        auto data = (ScrollData *) scroll->user_data;
        auto ctx = data->func(root);
        animate(&scroll->scrollbar_openess, 1.0, 100, scroll->lifetime, nullptr, schedule_redraw(ctx));
    }
}

//*/
//
//ScrollContainer *make_newscrollpane_as_child(Container *parent, const ScrollPaneSettings &settings) {
//    auto scrollpane = new ScrollContainer(settings);
//    // setup as child of root
//    if (parent)
//        scrollpane->parent = parent;
////    scrollpane->when_scrolled = scrollpane_scrolled;
//    scrollpane->when_fine_scrolled = fine_scrollpane_scrolled;
//    scrollpane->receive_events_even_if_obstructed = true;
//    if (settings.start_at_end) {
//        scrollpane->scroll_v_real = -1000000;
//        scrollpane->scroll_v_visual = -1000000;
//    }
//    scrollpane->scrollbar_openess = 0;
//    scrollpane->scrollbar_visible = 0;
//    if (parent)
//        parent->children.push_back(scrollpane);
//    scrollpane->when_mouse_leaves_container = [](Container *root, Container *container) {
//        auto scroll = (ScrollContainer *) container;
//        //client_create_animation(client->app, client, &scroll->scrollbar_visible, scroll->lifetime, 0, 100, nullptr, 0);
//        //client_create_animation(client->app, client, &scroll->scrollbar_openess, scroll->lifetime, 0, 100, nullptr, 0);
//    };
//    scrollpane->when_mouse_enters_container = [](Container *root, Container *container) {
//        auto scroll = (ScrollContainer *) container;
//        //client_create_animation(client->app, client, &scroll->scrollbar_visible, scroll->lifetime, 0, 100, nullptr, 1);
//    };
//    
//    auto content = new Container(::vbox, FILL_SPACE, FILL_SPACE);
//    // setup as content of scrollpane
//    content->parent = scrollpane;
//    scrollpane->content = content;
//    
//    auto right_vbox = new Container(settings.right_width, FILL_SPACE);
//    scrollpane->right = right_vbox;
//    right_vbox->parent = scrollpane;
//    right_vbox->type = ::vbox;
//    right_vbox->when_fine_scrolled = fine_right_thumb_scrolled;
//    /*
//    right_vbox->when_mouse_leaves_container = [](AppClient *client, cairo_t *, Container *container) {
//        auto scroll = (ScrollContainer *) container->parent;
//        int delay = 0;
//        if (bounds_contains(scroll->real_bounds, client->mouse_current_x, client->mouse_current_y)) {
//            delay = 3000;
//        }
//        if (delay == 0) {
//            client_create_animation(client->app, client, &scroll->scrollbar_openess, scroll->lifetime,
//                                    delay, 100, nullptr, 0);
//        } else {
//            if (!scroll->openess_delay_timeout) {
//                scroll->openess_delay_timeout =
//                        app_timeout_create(client->app, client, 3000, scroll_hover_timeout,
//                                           container->parent, "scrollpane_3000ms_timeout");
//            } else {
//                scroll->openess_delay_timeout = app_timeout_replace(client->app, client, scroll->openess_delay_timeout,
//                                                                    3000, scroll_hover_timeout, container->parent);
//            }
//        }
//    };
//    */
//    right_vbox->receive_events_even_if_obstructed = true;
//    /*
//    right_vbox->when_mouse_enters_container = [](AppClient *client, cairo_t *, Container *container) {
//        auto scroll = (ScrollContainer *) container->parent;
//        app_timeout_create(client->app, client, 200, ignore_first_hover, scroll,
//                           "ignore when mouse slides over");
//    };
//    */
//    
//    auto right_top_arrow = right_vbox->child(FILL_SPACE, settings.right_arrow_height);
//    right_top_arrow->user_data = new ButtonData;
//    ((ButtonData *) right_top_arrow->user_data)->text = "\uE971";
//    right_top_arrow->when_paint = paint_arrow;
//    right_top_arrow->when_mouse_down = mouse_down_arrow_up;
//    right_top_arrow->when_mouse_up = mouse_arrow_up;
//    right_top_arrow->when_clicked = mouse_arrow_up;
//    right_top_arrow->when_drag_end = mouse_arrow_up;
//    auto right_thumb = right_vbox->child(FILL_SPACE, FILL_SPACE);
//    right_thumb->when_paint = paint_right_thumb;
//    right_thumb->when_drag_start = right_scrollbar_drag_start;
//    right_thumb->when_drag = right_scrollbar_drag;
//    right_thumb->when_drag_end = right_scrollbar_drag_end;
//    right_thumb->when_mouse_down = right_scrollbar_mouse_down;
//    
//    auto right_bottom_arrow = right_vbox->child(FILL_SPACE, settings.right_arrow_height);
//    right_bottom_arrow->user_data = new ButtonData;
//    ((ButtonData *) right_bottom_arrow->user_data)->text = "\uE972";
//    right_bottom_arrow->when_paint = paint_arrow;
//    right_bottom_arrow->when_mouse_down = mouse_down_arrow_bottom;
//    right_bottom_arrow->when_mouse_up = mouse_arrow_up;
//    right_bottom_arrow->when_clicked = mouse_arrow_up;
//    right_bottom_arrow->when_drag_end = mouse_arrow_up;
//    right_vbox->z_index += 1;
//    
//    auto bottom_hbox = new Container(FILL_SPACE, settings.bottom_height);
//    scrollpane->bottom = bottom_hbox;
//    bottom_hbox->parent = scrollpane;
//    bottom_hbox->type = ::hbox;
//    bottom_hbox->when_fine_scrolled = fine_bottom_thumb_scrolled;
//    auto bottom_left_arrow = bottom_hbox->child(settings.bottom_arrow_width, FILL_SPACE);
//    bottom_left_arrow->user_data = new ButtonData;
//    ((ButtonData *) bottom_left_arrow->user_data)->text = "\uE973";
//    bottom_left_arrow->when_paint = paint_arrow;
//    bottom_left_arrow->when_mouse_down = mouse_down_arrow_left;
//    bottom_left_arrow->when_mouse_up = mouse_arrow_up;
//    bottom_left_arrow->when_clicked = mouse_arrow_up;
//    bottom_left_arrow->when_drag_end = mouse_arrow_up;
//    auto bottom_thumb = bottom_hbox->child(FILL_SPACE, FILL_SPACE);
//    bottom_thumb->when_paint = paint_bottom_thumb;
//    bottom_thumb->when_drag_start = bottom_scrollbar_drag_start;
//    bottom_thumb->when_drag = bottom_scrollbar_drag;
//    bottom_thumb->when_drag_end = bottom_scrollbar_drag_end;
//    bottom_thumb->when_mouse_down = bottom_scrollbar_mouse_down;
//    
//    auto bottom_right_arrow = bottom_hbox->child(settings.bottom_arrow_width, FILL_SPACE);
//    bottom_right_arrow->user_data = new ButtonData;
//    ((ButtonData *) bottom_right_arrow->user_data)->text = "\uE974";
//    bottom_right_arrow->when_paint = paint_arrow;
//    bottom_right_arrow->when_mouse_down = mouse_down_arrow_right;
//    bottom_right_arrow->when_mouse_up = mouse_arrow_up;
//    bottom_right_arrow->when_clicked = mouse_arrow_up;
//    bottom_right_arrow->when_drag_end = mouse_arrow_up;
//    bottom_hbox->z_index += 1;
//    
//    return scrollpane;
//}
//
//void combobox_key_event(AppClient *client, cairo_t *cr, Container *self, bool is_string, xkb_keysym_t keysym,
//                        char string[64],
//                        uint16_t mods, xkb_key_direction direction) {
//    if (!self->active)
//        return;
//    if (is_string) {
//        // append to text
//    } else {
//        // handle backspace
//        
//    }
//    printf("is_string: %b\n", is_string);
//    printf("keysym: %d\n", keysym);
//    printf("string: %s\n", string);
//    printf("mods: %d\n", mods);
//    printf("direction: %d\n", direction);
//}
//
//void combobox_popup_pressed(AppClient *client, cairo_t *cr, Container *self) {
//    //client_close_threaded(client->app, client);
//}
//
//struct ComboboxPopupData : UserData {
//    Container *parent;
//};
//
//void combobox_popup_key_event(AppClient *client, cairo_t *cr, Container *self, bool is_string, xkb_keysym_t keysym,
//                              char string[64],
//                              uint16_t mods, xkb_key_direction direction) {
//    auto c = (ComboboxPopupData *) self->user_data;
//    if (c->parent->when_key_event)
//        c->parent->when_key_event(client, cr, c->parent, is_string, keysym, string, mods, direction);
//}
//
//void combobox_pressed(AppClient *client, cairo_t *cr, Container *self) {
//    Settings settings;
//    PopupSettings popup_settings;
//    popup_settings.name = client->name + "_combobox_popup";
//    popup_settings.ignore_scroll = true;
//    popup_settings.takes_input_focus = true;
//    popup_settings.transparent_mouse_grab = false;
//    auto menu = client->create_popup(popup_settings, settings);
//    menu->root->when_mouse_down = combobox_popup_pressed;
//    menu->root->when_key_event = combobox_popup_key_event;
//    menu->root->user_data = new ComboboxPopupData;
//    ((ComboboxPopupData *) menu->root->user_data)->parent = self;
////    menu->root->user_data = self;
//    client_show(client->app, menu);
//}
//
//Container *make_combobox(Container *parent, const std::vector<std::string> &items) {
//    auto *combobox = parent->child(::vbox, FILL_SPACE, FILL_SPACE);
//    
//    // instead of re-using textarea, just make a new simple area meant just for combobox
//    combobox->when_key_event = combobox_key_event;
//    combobox->when_paint = paint_show;
//    combobox->when_mouse_down = combobox_pressed;
//    
//    return combobox;
//}
//
//static void
//update_preffered_x(AppClient *client, Container *textarea) {
//#ifdef TRACY_ENABLE
//    ZoneScoped;
//#endif
//    auto *data = (TextAreaData *) textarea->user_data;
//    
//    PangoLayout *layout = get_cached_pango_font(
//            client->cr, data->font, data->font_size, PangoWeight::PANGO_WEIGHT_NORMAL);
//    
//    pango_layout_set_text(layout, data->state->text.c_str(), data->state->text.length());
//    if (data->wrap) {
//        pango_layout_set_wrap(layout, PANGO_WRAP_WORD_CHAR);
//        pango_layout_set_width(layout, textarea->real_bounds.w * PANGO_SCALE);
//    }
//    
//    PangoRectangle strong_pos;
//    PangoRectangle weak_pos;
//    pango_layout_get_cursor_pos(layout, data->state->cursor, &strong_pos, &weak_pos);
//    
//    data->state->preferred_x = strong_pos.x;
//}
//
//static void
//put_cursor_on_screen(AppClient *client, Container *textarea) {
//#ifdef TRACY_ENABLE
//    ZoneScoped;
//#endif
//    auto *data = (TextAreaData *) textarea->user_data;
//    
//    PangoLayout *layout = get_cached_pango_font(
//            client->cr, data->font, data->font_size, PangoWeight::PANGO_WEIGHT_NORMAL);
//    
//    pango_layout_set_text(layout, data->state->text.c_str(), data->state->text.length());
//    if (data->wrap) {
//        pango_layout_set_wrap(layout, PANGO_WRAP_WORD_CHAR);
//        pango_layout_set_width(layout, textarea->real_bounds.w * PANGO_SCALE);
//    }
//    
//    PangoRectangle strong_pos;
//    PangoRectangle weak_pos;
//    pango_layout_get_cursor_pos(layout, data->state->cursor, &strong_pos, &weak_pos);
//    
//    Container *content_area = textarea->parent;
//    
//    Bounds view_bounds = Bounds(-content_area->scroll_h_real,
//                                -content_area->scroll_v_real,
//                                content_area->real_bounds.w,
//                                content_area->real_bounds.h);
//    
//    int x_pos = strong_pos.x / PANGO_SCALE;
//    if (x_pos < view_bounds.x) {
//        content_area->scroll_h_real = -(x_pos - scroll_amount);
//    } else if (x_pos > view_bounds.x + view_bounds.w) {
//        content_area->scroll_h_real = -(x_pos - content_area->real_bounds.w + scroll_amount);
//    }
//    int y_pos = strong_pos.y / PANGO_SCALE;
//    if (y_pos < view_bounds.y) {
//        content_area->scroll_v_real = -(y_pos - scroll_amount);
//    } else if (y_pos + strong_pos.height / PANGO_SCALE > (view_bounds.y + view_bounds.h)) {
//        content_area->scroll_v_real =
//                -(y_pos + strong_pos.height / PANGO_SCALE - content_area->real_bounds.h);
//    }
//    
//    // ::layout(content_area, content_area->real_bounds, false);
//    client_layout(client->app, client);
//    
//    client_create_animation(client->app,
//                            client,
//                            &content_area->scroll_h_visual, content_area->lifetime, 0,
//                            scroll_anim_time,
//                            easing_function,
//                            content_area->scroll_h_real,
//                            true);
//    client_create_animation(client->app,
//                            client,
//                            &content_area->scroll_v_visual, content_area->lifetime, 0,
//                            scroll_anim_time,
//                            easing_function,
//                            content_area->scroll_v_real,
//                            true);
//}
//
//static void
//update_bounds(AppClient *client, Container *container) {
//#ifdef TRACY_ENABLE
//    ZoneScoped;
//#endif
//    auto *data = (TextAreaData *) container->user_data;
//    
//    PangoLayout *layout = get_cached_pango_font(
//            client->cr, data->font, data->font_size, PangoWeight::PANGO_WEIGHT_NORMAL);
//    
//    pango_layout_set_text(layout, data->state->text.c_str(), data->state->text.length());
//    
//    PangoRectangle text_ink;
//    PangoRectangle text_logical;
//    pango_layout_get_extents(layout, &text_ink, &text_logical);
//    
//    int width = text_logical.width / PANGO_SCALE;
//    int height = text_logical.height / PANGO_SCALE;
//    
//    if (data->wrap) {
//        if (container->real_bounds.h != height) {
//            container->wanted_bounds.h = height;
//            // ::layout(container->parent->parent, container->parent->parent->real_bounds, false);
//            client_layout(client->app, client);
//    
//            client_create_animation(client->app,
//                                    client,
//                                    &container->parent->parent->scroll_h_visual, container->parent->parent->lifetime, 0,
//                                    scroll_anim_time,
//                                    easing_function,
//                                    container->parent->parent->scroll_h_real,
//                                    true);
//            client_create_animation(client->app,
//                                    client,
//                                    &container->parent->parent->scroll_v_visual, container->parent->parent->lifetime, 0,
//                                    scroll_anim_time,
//                                    easing_function,
//                                    container->parent->parent->scroll_v_real,
//                                    true);
//            
//            request_refresh(client->app, client);
//        }
//    } else {
//        if (container->real_bounds.h != height || container->wanted_bounds.w != width) {
//            container->wanted_bounds.w = width;
//            container->wanted_bounds.h = height;
//            client_layout(client->app, client);
//            // ::layout(container->parent->parent, container->parent->parent->real_bounds, false);
//    
//            client_create_animation(client->app,
//                                    client,
//                                    &container->parent->parent->scroll_h_visual, container->parent->parent->lifetime, 0,
//                                    scroll_anim_time,
//                                    easing_function,
//                                    container->parent->parent->scroll_h_real,
//                                    true);
//            client_create_animation(client->app,
//                                    client,
//                                    &container->parent->parent->scroll_v_visual, container->parent->parent->lifetime, 0,
//                                    scroll_anim_time,
//                                    easing_function,
//                                    container->parent->parent->scroll_v_real,
//                                    true);
//            
//            request_refresh(client->app, client);
//        }
//    }
//}
//
//static void
//paint_textarea(AppClient *client, cairo_t *cr, Container *container) {
//#ifdef TRACY_ENABLE
//    ZoneScoped;
//#endif
//    auto *data = (TextAreaData *) container->user_data;
//    if (data->state->first_bounds_update) {
//        update_bounds(client, container);
//        data->state->first_bounds_update = false;
//    }
//    
//    cairo_save(cr);
//    
//    // DEBUG
//    // paint_show(client, cr, container);
//    
//    // TEXT
//    PangoLayout *layout = get_cached_pango_font(
//            client->cr, data->font, data->font_size, PangoWeight::PANGO_WEIGHT_NORMAL);
//    
//    pango_layout_set_width(layout, -1);
//    pango_layout_set_text(layout, data->state->text.c_str(), data->state->text.length());
//    pango_layout_set_alignment(layout, PangoAlignment::PANGO_ALIGN_LEFT);
//    if (data->wrap) {
//        pango_layout_set_wrap(layout, PANGO_WRAP_WORD_CHAR);
//        pango_layout_set_width(layout, container->real_bounds.w * PANGO_SCALE);
//    }
//    
//    set_rect(cr, container->parent->real_bounds);
//    cairo_clip(cr);
//    
//    PangoRectangle cursor_strong_pos;
//    PangoRectangle cursor_weak_pos;
//    pango_layout_get_cursor_pos(layout, data->state->cursor, &cursor_strong_pos, &cursor_weak_pos);
//    
//    // SELECTION BACKGROUND
//    if (data->state->selection_x != -1) {
//        set_argb(cr, RGBA(.2, .5, .8, 1));
//        PangoRectangle selection_strong_pos;
//        PangoRectangle selection_weak_pos;
//        pango_layout_get_cursor_pos(
//                layout, data->state->selection_x, &selection_strong_pos, &selection_weak_pos);
//        
//        bool cursor_first = false;
//        if (cursor_strong_pos.y == selection_strong_pos.y) {
//            if (cursor_strong_pos.x < selection_strong_pos.x) {
//                cursor_first = true;
//            }
//        } else if (cursor_strong_pos.y < selection_strong_pos.y) {
//            cursor_first = true;
//        }
//        
//        double w = std::max(container->real_bounds.w, container->parent->real_bounds.w);
//        
//        double minx = std::min(selection_strong_pos.x, cursor_strong_pos.x) / PANGO_SCALE;
//        double miny = std::min(selection_strong_pos.y, cursor_strong_pos.y) / PANGO_SCALE;
//        double maxx = std::max(selection_strong_pos.x, cursor_strong_pos.x) / PANGO_SCALE;
//        double maxy = std::max(selection_strong_pos.y, cursor_strong_pos.y) / PANGO_SCALE;
//        double h = selection_strong_pos.height / PANGO_SCALE;
//        
//        if (maxy == miny) {// Same line
//            draw_colored_rect(RGBA(.2, .5, .8, 1), Bounds(container->real_bounds.x + minx, container->real_bounds.y + miny, maxx - minx, h));
//        } else {
//            Bounds b;
//            if ((maxy - miny) > h) {// More than one line off difference
//                b = Bounds(container->real_bounds.x,
//                                container->real_bounds.y + miny + h,
//                                w,
//                                maxy - miny - h);
//            }
//            // If the y's aren't on the same line then we always draw the two rects
//            // for when there's a one line diff
//            
//            if (cursor_first) {
//                // Top line
//                b = Bounds(container->real_bounds.x + cursor_strong_pos.x / PANGO_SCALE,
//                                container->real_bounds.y + cursor_strong_pos.y / PANGO_SCALE,
//                                w,
//                                h);
//                
//                // Bottom line
//                int bottom_width = selection_strong_pos.x / PANGO_SCALE;
//                b = Bounds(container->real_bounds.x,
//                                container->real_bounds.y + selection_strong_pos.y / PANGO_SCALE,
//                                bottom_width,
//                                h);
//            } else {
//                // Top line
//                b = Bounds(container->real_bounds.x + selection_strong_pos.x / PANGO_SCALE,
//                                container->real_bounds.y + selection_strong_pos.y / PANGO_SCALE,
//                                w,
//                                h);
//                
//                // Bottom line
//                int bottom_width = cursor_strong_pos.x / PANGO_SCALE;
//                b = Bounds(container->real_bounds.x,
//                                container->real_bounds.y + cursor_strong_pos.y / PANGO_SCALE,
//                                bottom_width,
//                                h);
//            }
//            draw_colored_rect(RGBA(.2, .5, .8, 1), b);
//        }
//    }// END Selection background
//    
//    // SHOW TEXT LAYOUT
//    set_argb(cr, data->color);
//    
//    //cairo_move_to(cr, container->real_bounds.x, container->real_bounds.y);
//    
//    // TODO: this 0, 0 position is wrong for cairo and makes is draw the cursor in the wrong spot
//    // The problem is that the texts are different widths
//    draw_text(client, data->font_size, config->font, EXPAND(data->color), data->state->text, container->real_bounds, 5, 0, 0);
//    
//    //pango_cairo_show_layout(cr, layout);
//    //pango_layout_set_alignment(layout, PangoAlignment::PANGO_ALIGN_LEFT);
//    
//    if (container->parent->active == false && data->state->text.empty()) {
//         draw_text(client, data->font_size, config->font, EXPAND(data->color_prompt), data->state->prompt, container->real_bounds, 5, 0, 0);
//   }
//    
//    // PAINT CURSOR
//    if (container->parent->active && data->state->cursor_on) {
//        int offset = cursor_strong_pos.x != 0 ? -1 : 0;
//        draw_colored_rect(data->color_cursor, Bounds(cursor_strong_pos.x / PANGO_SCALE + container->real_bounds.x + offset,
//                        cursor_strong_pos.y / PANGO_SCALE + container->real_bounds.y,
//                        data->cursor_width,
//                        cursor_strong_pos.height / PANGO_SCALE));
//    }
//    cairo_restore(cr);
//}
//
//static void
//move_cursor(TextAreaData *data, int byte_index, bool increase_selection) {
//#ifdef TRACY_ENABLE
//    ZoneScoped;
//#endif
//    if (increase_selection) {
//        if (data->state->selection_x == -1) {
//            data->state->selection_x = data->state->cursor;
//        }
//    } else {
//        data->state->selection_x = -1;
//    }
//    data->state->cursor = byte_index;
//}
//
//static void
//clicked_textarea(AppClient *client, cairo_t *cr, Container *container) {
//#ifdef TRACY_ENABLE
//    ZoneScoped;
//#endif
//    container = container->children[0];
//    auto *data = (TextAreaData *) container->user_data;
//    
//    if ((client->app->current - data->state->last_time_mouse_press) < 220) {
//        data->state->cursor = data->state->text.size();
//        data->state->selection_x = 0;
//        put_cursor_on_screen(client, container);
//        return;
//    }
//    data->state->last_time_mouse_press = client->app->current;
//    
//    PangoLayout *layout = get_cached_pango_font(
//            client->cr, data->font, data->font_size, PangoWeight::PANGO_WEIGHT_NORMAL);
//    
//    set_argb(cr, data->color);
//    pango_layout_set_text(layout, data->state->text.c_str(), data->state->text.length());
//    if (data->wrap) {
//        pango_layout_set_wrap(layout, PANGO_WRAP_WORD_CHAR);
//        pango_layout_set_width(layout, container->real_bounds.w * PANGO_SCALE);
//    }
//    
//    int index;
//    int trailing;
//    int x = client->mouse_current_x - container->real_bounds.x;
//    int y = client->mouse_current_y - container->real_bounds.y;
//    bool inside =
//            pango_layout_xy_to_index(layout, x * PANGO_SCALE, y * PANGO_SCALE, &index, &trailing);
//    
//    auto cookie = xcb_xkb_get_state(client->app->connection, client->keyboard->device_id);
//    auto reply = xcb_xkb_get_state_reply(client->app->connection, cookie, nullptr);
//    
//    bool shift = reply->mods & XKB_KEY_Shift_L;
//    
//    move_cursor(data, index + trailing, shift);
//    update_preffered_x(client, container);
//}
//
//static void
//drag_timeout(App *app, AppClient *client, Timeout *, void *data) {
//    auto *container = (Container *) data;
//    
//    Container *content_area = container->parent;
//    
//    blink_on(client->app, client, container);
//    
//    if (dragging.load()) {
//        auto *data = (TextAreaData *) container->user_data;
//        
//        PangoLayout *layout = get_cached_pango_font(
//                client->cr, data->font, data->font_size, PangoWeight::PANGO_WEIGHT_NORMAL);
//        
//        pango_layout_set_text(layout, data->state->text.c_str(), data->state->text.length());
//        if (data->wrap) {
//            pango_layout_set_wrap(layout, PANGO_WRAP_WORD_CHAR);
//            pango_layout_set_width(layout, container->real_bounds.w * PANGO_SCALE);
//        }
//        
//        int index;
//        int trailing;
//        int lx = client->mouse_current_x - container->real_bounds.x;
//        int ly = client->mouse_current_y - container->real_bounds.y;
//        bool inside = pango_layout_xy_to_index(
//                layout, lx * PANGO_SCALE, ly * PANGO_SCALE, &index, &trailing);
//        
//        Bounds bounds = container->parent->real_bounds;
//        int x = client->mouse_current_x;
//        int y = client->mouse_current_y;
//        
//        bool modified_x = false;
//        bool modified_y = false;
//        
//        if (x < bounds.x) {// off the left side
//            modified_x = true;
//            double multiplier =
//                    std::min((double) scroll_amount * 3, bounds.x - x) / scroll_amount;
//            content_area->scroll_h_real += scroll_amount * multiplier;
//        }
//        if (x > bounds.x + bounds.w) {// off the right side
//            modified_x = true;
//            double multiplier =
//                    std::min((double) scroll_amount * 3, x - (bounds.x + bounds.w)) / scroll_amount;
//            content_area->scroll_h_real -= scroll_amount * multiplier;
//        }
//        if (y < bounds.y) {// off the top
//            modified_y = true;
//            double multiplier =
//                    std::min((double) scroll_amount * 3, bounds.y - y) / scroll_amount;
//            content_area->scroll_v_real += scroll_amount * multiplier;
//        }
//        if (y > bounds.y + bounds.h) {// off the bottom
//            modified_y = true;
//            double multiplier =
//                    std::min((double) scroll_amount * 3, y - (bounds.y + bounds.h)) / scroll_amount;
//            content_area->scroll_v_real -= scroll_amount * multiplier;
//        }
//        
//        // ::layout(content_area, content_area->real_bounds, false);
//        client_layout(client->app, client);
//        
//        if (modified_x)
//            client_create_animation(client->app,
//                                    client,
//                                    &content_area->scroll_h_visual, content_area->lifetime, 0,
//                                    scroll_anim_time,
//                                    easing_function,
//                                    content_area->scroll_h_real,
//                                    true);
//        if (modified_y)
//            client_create_animation(client->app,
//                                    client,
//                                    &content_area->scroll_v_visual, content_area->lifetime, 0,
//                                    scroll_anim_time,
//                                    easing_function,
//                                    content_area->scroll_v_real,
//                                    true);
//        
//        app_timeout_create(client->app, client, scroll_anim_time, drag_timeout, container, const_cast<char *>(__PRETTY_FUNCTION__));
//    } else {
//        request_refresh(app, client);
//    }
//}
//
//static void
//drag_start_textarea(AppClient *client, cairo_t *cr, Container *container) {
//#ifdef TRACY_ENABLE
//    ZoneScoped;
//#endif
//    container = container->children[0];
//    auto *data = (TextAreaData *) container->user_data;
//    
//    PangoLayout *layout = get_cached_pango_font(
//            client->cr, data->font, data->font_size, PangoWeight::PANGO_WEIGHT_NORMAL);
//    
//    set_argb(cr, data->color);
//    pango_layout_set_text(layout, data->state->text.c_str(), data->state->text.length());
//    if (data->wrap) {
//        pango_layout_set_wrap(layout, PANGO_WRAP_WORD_CHAR);
//        pango_layout_set_width(layout, container->real_bounds.w * PANGO_SCALE);
//    }
//    
//    int index;
//    int trailing;
//    int x = client->mouse_current_x - container->real_bounds.x;
//    int y = client->mouse_current_y - container->real_bounds.y;
//    bool inside =
//            pango_layout_xy_to_index(layout, x * PANGO_SCALE, y * PANGO_SCALE, &index, &trailing);
//    
//    dragging = true;
//    app_timeout_create(client->app, client, 0, drag_timeout, container, const_cast<char *>(__PRETTY_FUNCTION__));
//    blink_on(client->app, client, container);
//    
//    auto cookie = xcb_xkb_get_state(client->app->connection, client->keyboard->device_id);
//    auto reply = xcb_xkb_get_state_reply(client->app->connection, cookie, nullptr);
//    
//    bool shift = reply->mods & XKB_KEY_Shift_L;
//    
//    move_cursor(data, index + trailing, shift);
//}
//
//static void
//mouse_down_textarea(AppClient *client, cairo_t *cr, Container *container) {
//#ifdef TRACY_ENABLE
//    ZoneScoped;
//#endif
//    container = container->children[0];
//    auto *data = (TextAreaData *) container->user_data;
//    
//    PangoLayout *layout = get_cached_pango_font(
//            client->cr, data->font, data->font_size, PangoWeight::PANGO_WEIGHT_NORMAL);
//    
//    set_argb(cr, data->color);
//    pango_layout_set_text(layout, data->state->text.c_str(), data->state->text.length());
//    if (data->wrap) {
//        pango_layout_set_wrap(layout, PANGO_WRAP_WORD_CHAR);
//        pango_layout_set_width(layout, container->real_bounds.w * PANGO_SCALE);
//    }
//    
//    int index;
//    int trailing;
//    int x = client->mouse_initial_x - container->real_bounds.x;
//    int y = client->mouse_initial_y - container->real_bounds.y;
//    bool inside =
//            pango_layout_xy_to_index(layout, x * PANGO_SCALE, y * PANGO_SCALE, &index, &trailing);
//    
//    auto cookie = xcb_xkb_get_state(client->app->connection, client->keyboard->device_id);
//    auto reply = xcb_xkb_get_state_reply(client->app->connection, cookie, nullptr);
//    
//    bool shift = reply->mods & XKB_KEY_Shift_L;
//    
//    move_cursor(data, index + trailing, shift);
//}
//
//static void
//drag_textarea(AppClient *client, cairo_t *cr, Container *container) {
//#ifdef TRACY_ENABLE
//    ZoneScoped;
//#endif
//    container = container->children[0];
//    auto *data = (TextAreaData *) container->user_data;
//    
//    PangoLayout *layout = get_cached_pango_font(
//            client->cr, data->font, data->font_size, PangoWeight::PANGO_WEIGHT_NORMAL);
//    
//    set_argb(cr, data->color);
//    pango_layout_set_text(layout, data->state->text.c_str(), data->state->text.length());
//    if (data->wrap) {
//        pango_layout_set_wrap(layout, PANGO_WRAP_WORD_CHAR);
//        pango_layout_set_width(layout, container->real_bounds.w * PANGO_SCALE);
//    }
//    
//    int index;
//    int trailing;
//    int x = client->mouse_current_x - container->real_bounds.x;
//    int y = client->mouse_current_y - container->real_bounds.y;
//    bool inside =
//            pango_layout_xy_to_index(layout, x * PANGO_SCALE, y * PANGO_SCALE, &index, &trailing);
//    
//    move_cursor(data, index + trailing, true);
//}
//
//static void
//drag_end_textarea(AppClient *client, cairo_t *cr, Container *container) {
//#ifdef TRACY_ENABLE
//    ZoneScoped;
//#endif
//    container = container->children[0];
//    auto *data = (TextAreaData *) container->user_data;
//    
//    PangoLayout *layout = get_cached_pango_font(
//            client->cr, data->font, data->font_size, PangoWeight::PANGO_WEIGHT_NORMAL);
//    
//    set_argb(cr, data->color);
//    pango_layout_set_text(layout, data->state->text.c_str(), data->state->text.length());
//    if (data->wrap) {
//        pango_layout_set_wrap(layout, PANGO_WRAP_WORD_CHAR);
//        pango_layout_set_width(layout, container->real_bounds.w * PANGO_SCALE);
//    }
//    
//    int index;
//    int trailing;
//    int x = client->mouse_current_x - container->real_bounds.x;
//    int y = client->mouse_current_y - container->real_bounds.y;
//    bool inside =
//            pango_layout_xy_to_index(layout, x * PANGO_SCALE, y * PANGO_SCALE, &index, &trailing);
//    
//    move_cursor(data, index + trailing, true);
//    
//    dragging = false;
//}
//
//static void
//textarea_key_release(AppClient *client,
//                     cairo_t *cr,
//                     Container *container,
//                     bool is_string, xkb_keysym_t keysym, char string[64],
//                     uint16_t mods,
//                     xkb_key_direction direction);
//
//static void
//textarea_active_status_changed(AppClient *client, cairo_t *cr, Container *container) {
//    container = container->children[0];
//    auto *data = (TextAreaData *) container->user_data;
//    if (!container->active) {
//        data->state->selection_x = data->state->cursor;
//    }
//}
//
//Container *
//make_textarea(App *app, AppClient *client, Container *parent, TextAreaSettings settings) {
//#ifdef TRACY_ENABLE
//    ZoneScoped;
//#endif
//    
//    Container *content_area = make_scrollpane(parent, settings);
//    content_area->wanted_pad = settings.pad;
//    
//    int width = 0;
//    if (settings.wrap)
//        width = FILL_SPACE;
//    Container *textarea = content_area->child(::vbox, width, settings.font_size__);
//    textarea->wanted_pad = settings.pad;
//    
//    textarea->when_paint = paint_textarea;
//    textarea->when_key_event = textarea_key_release;
//    content_area->when_drag_end_is_click = false;
//    content_area->when_drag_start = drag_start_textarea;
//    content_area->when_drag = drag_textarea;
//    content_area->when_drag_end = drag_end_textarea;
//    content_area->when_clicked = clicked_textarea;
//    content_area->when_mouse_down = mouse_down_textarea;
//    content_area->when_active_status_changed = textarea_active_status_changed;
//    
//    auto data = new TextAreaData();
//    data->cursor_width = settings.cursor_width;
//    data->color_cursor = settings.color_cursor;
//    data->font_size = settings.font_size__;
//    data->font = settings.font;
//    data->color = settings.color;
//    data->single_line = settings.single_line;
//    data->wrap = settings.wrap;
//    data->state->prompt = settings.prompt;
//    data->color_prompt = settings.color_prompt;
//    data->text_alignment = settings.text_alignment;
//    data->prompt_alignment = settings.prompt_alignment;
//    textarea->user_data = data;
//    
//    scroll_amount = data->font_size * 3;
//    
//    blink_on(app, client, textarea);
//    
//    return textarea;
//}
//
//void
//insert_action(AppClient *client, Container *textarea, TextAreaData *data, std::string text) {
//    // Try to merge with the previous
//    bool merged = false;
//    
//    if (!data->state->undo_stack.empty()) {
//        UndoAction *previous_action = data->state->undo_stack.back();
//        
//        if (previous_action->type == UndoType::INSERT) {
//            if (previous_action->cursor_end == data->state->cursor) {
//                char last_char = previous_action->inserted_text.back();
//                
//                bool text_is_split_token = text == " " || text == "\n" || text == "\r";
//                
//                if (text_is_split_token && last_char == text.back() && text.size() == 1) {
//                    previous_action->inserted_text.append(text);
//                    previous_action->cursor_end += text.size();
//                    merged = true;
//                } else {
//                    if (!text_is_split_token) {
//                        previous_action->inserted_text.append(text);
//                        previous_action->cursor_end += text.size();
//                        merged = true;
//                    }
//                }
//            } else {
//                auto undo_action = new UndoAction;
//                undo_action->type = UndoType::CURSOR;
//                undo_action->cursor_start = previous_action->cursor_end;
//                undo_action->cursor_end = data->state->cursor;
//                data->state->undo_stack.push_back(undo_action);
//            }
//        }
//    }
//    
//    if (!merged) {
//        auto undo_action = new UndoAction;
//        undo_action->type = UndoType::INSERT;
//        
//        undo_action->inserted_text = text;
//        undo_action->cursor_start = data->state->cursor;
//        undo_action->cursor_end = data->state->cursor + text.size();
//        data->state->undo_stack.push_back(undo_action);
//    }
//    
//    data->state->text.insert(data->state->cursor, text);
//    move_cursor(data, data->state->cursor + text.size(), false);
//    update_preffered_x(client, textarea);
//    update_bounds(client, textarea);
//    put_cursor_on_screen(client, textarea);
//    
//    data->state->redo_stack.clear();
//    data->state->redo_stack.shrink_to_fit();
//}
//
//static void
//delete_action(AppClient *client, Container *textarea, TextAreaData *data, int amount) {
//    auto undo_action = new UndoAction;
//    undo_action->type = UndoType::DELETE;
//    
//    if (amount > 0) {
//        undo_action->replaced_text = data->state->text.substr(data->state->cursor, amount);
//        undo_action->cursor_start = data->state->cursor;
//        undo_action->cursor_end = data->state->cursor;
//        
//        data->state->text.erase(data->state->cursor, amount);
//    } else {
//        undo_action->replaced_text =
//                data->state->text.substr(data->state->cursor + amount, -amount);
//        undo_action->cursor_start = data->state->cursor;
//        undo_action->cursor_end = data->state->cursor + amount;
//        
//        data->state->text.erase(data->state->cursor + amount, -amount);
//    }
//    data->state->undo_stack.push_back(undo_action);
//    
//    move_cursor(data, undo_action->cursor_end, false);
//    update_preffered_x(client, textarea);
//    update_bounds(client, textarea);
//    put_cursor_on_screen(client, textarea);
//}
//
//void
//replace_action(AppClient *client, Container *textarea, TextAreaData *data, std::string text) {
//    auto undo_action = new UndoAction;
//    undo_action->type = UndoType::REPLACE;
//    
//    int min_pos = std::min(data->state->cursor, data->state->selection_x);
//    int max_pos = std::max(data->state->cursor, data->state->selection_x);
//    
//    undo_action->inserted_text = text;
//    undo_action->replaced_text = data->state->text.substr(min_pos, max_pos - min_pos);
//    
//    undo_action->cursor_start = data->state->cursor;
//    undo_action->cursor_end = min_pos + text.size();
//    undo_action->selection_start = data->state->selection_x;
//    undo_action->selection_end = -1;
//    data->state->undo_stack.push_back(undo_action);
//    
//    data->state->text.erase(min_pos, max_pos - min_pos);
//    data->state->text.insert(min_pos, text);
//    
//    move_cursor(data, undo_action->cursor_end, false);
//    update_preffered_x(client, textarea);
//    update_bounds(client, textarea);
//    put_cursor_on_screen(client, textarea);
//}
//
//char tokens_list[] = {'[', ']', '|', '(', ')', '{', '}', ';', '.', '!', '@',
//                      '#', '$', '%', '^', '&', '*', '-', '=', '+', ':', '\'',
//                      '\'', '<', '>', '?', '|', '\\', '/', ',', '`', '~', '\t'};
//
//enum motion {
//    left = 0,
//    right = 1,
//};
//
//enum group {
//    none = 0,
//    space = 1,
//    newline = 2,
//    token = 3,
//    normal = 4,
//};
//
//class Seeker {
//public:
//    TextState *state;
//    
//    int start_pos;
//    int current_pos;
//    
//    group starting_group_to_left;
//    group starting_group_to_right;
//    
//    bool same_token_type_to_left_and_right;// we are always inside a group but this means that to
//    // either side is the same group type
//    bool different_token_type_to_atleast_one_side;// at the end of a group
//    
//    group group_at(int pos) {
//        if (pos < 0 || pos >= state->text.size()) {
//            return group::none;
//        }
//        char c = state->text.at(pos);
//        if (c == ' ')
//            return group::space;
//        if (c == '\n')
//            return group::newline;
//        for (auto token: tokens_list)
//            if (token == c)
//                return group::token;
//        return group::normal;
//    }
//    
//    group group_to(motion direction) {
//        int off = 0;
//        if (direction == motion::right)
//            off = 1;
//        else if (direction == motion::left)
//            off = -1;
//        return group_at(current_pos + off);
//    }
//    
//    group seek_until_different_token(motion direction, int off) {
//        // right now we are going to ignore the first character completely
//        if (direction == motion::left) {
//            while ((current_pos - off) >= 0) {
//                group new_group = group_at(current_pos - off);
//                if (new_group != starting_group_to_right) {
//                    return new_group;
//                }
//                current_pos -= 1;
//            }
//        } else if (direction == motion::right) {
//            while ((current_pos + off) < state->text.size()) {
//                group new_group = group_at(current_pos + off);
//                if (new_group != starting_group_to_right) {
//                    return new_group;
//                }
//                current_pos += 1;
//            }
//        }
//        return group::none;
//    }
//    
//    group seek_until_right_before_different_token(motion direction) {
//        return seek_until_different_token(direction, 1);
//    }
//    
//    group seek_and_cover_different_token(motion direction) {
//        return seek_until_different_token(direction, 0);
//    }
//    
//    bool seek_until_specific_token(motion direction, group specific_token) {
//        group active_group;
//        while ((active_group = group_to(direction)) != group::none) {
//            if (active_group == specific_token) {
//                return true;
//            }
//            current_pos += direction == motion::left ? -1 : 1;
//        }
//        return false;
//    }
//    
//    Seeker(TextState *state) {
//        this->state = state;
//        
//        start_pos = state->cursor;
//        current_pos = start_pos;
//        
//        starting_group_to_left = group_at(start_pos - 1);
//        starting_group_to_right = group_at(start_pos);
//        
//        same_token_type_to_left_and_right = starting_group_to_right == starting_group_to_left;
//        different_token_type_to_atleast_one_side = !same_token_type_to_left_and_right;
//    }
//};
//
//static void
//go_to_edge(Seeker &seeker, motion motion_direction) {
//    if (motion_direction == motion::left) {
//        seeker.seek_until_right_before_different_token(motion_direction);
//    } else if (motion_direction == motion::right) {
//        seeker.seek_and_cover_different_token(motion_direction);
//    }
//}
//
//static int
//seek_token(TextState *state, motion motion_direction) {
//    Seeker seeker(state);
//    
//    if (seeker.same_token_type_to_left_and_right) {
//        go_to_edge(seeker, motion_direction);
//        if (seeker.starting_group_to_right == group::space) {
//            seeker.starting_group_to_right = seeker.starting_group_to_left =
//                    seeker.group_to(motion_direction);
//            go_to_edge(seeker, motion_direction);
//        }
//    } else if (seeker.different_token_type_to_atleast_one_side) {
//        if (motion_direction == motion::left) {
//            seeker.starting_group_to_right = seeker.starting_group_to_left =
//                    seeker.group_to(motion_direction);
//            go_to_edge(seeker, motion_direction);
//            if (seeker.starting_group_to_right == group::space) {
//                seeker.starting_group_to_right = seeker.starting_group_to_left =
//                        seeker.group_to(motion_direction);
//                go_to_edge(seeker, motion_direction);
//            }
//        } else if (motion_direction == motion::right) {
//            go_to_edge(seeker, motion_direction);
//            if (seeker.starting_group_to_right == group::space) {
//                seeker.starting_group_to_right = seeker.starting_group_to_left =
//                        seeker.group_to(motion_direction);
//                go_to_edge(seeker, motion_direction);
//            }
//        }
//    }
//    
//    return seeker.current_pos;
//}
//
//static void
//move_vertically_lines(AppClient *client,
//                      TextAreaData *data,
//                      Container *textarea,
//                      bool shift,
//                      int multiplier) {
//    PangoLayout *layout = get_cached_pango_font(
//            client->cr, data->font, data->font_size, PangoWeight::PANGO_WEIGHT_NORMAL);
//    
//    pango_layout_set_text(layout, data->state->text.c_str(), data->state->text.length());
//    if (data->wrap) {
//        pango_layout_set_wrap(layout, PANGO_WRAP_WORD_CHAR);
//        pango_layout_set_width(layout, textarea->real_bounds.w * PANGO_SCALE);
//    }
//    
//    PangoRectangle strong_pos;
//    PangoRectangle weak_pos;
//    pango_layout_get_cursor_pos(layout, data->state->cursor, &strong_pos, &weak_pos);
//    
//    PangoLayoutLine *line = pango_layout_get_line(layout, 0);
//    PangoRectangle ink_rect;
//    PangoRectangle logical_rect;
//    pango_layout_line_get_extents(line, &ink_rect, &logical_rect);
//    
//    int index;
//    int trailing;
//    int x = data->state->preferred_x;
//    int y = strong_pos.y + (logical_rect.height * multiplier);
//    bool inside = pango_layout_xy_to_index(layout, x, y, &index, &trailing);
//    move_cursor(data, index + trailing, shift);
//    put_cursor_on_screen(client, textarea);
//}
//
//void
//textarea_handle_keypress(AppClient *client, Container *textarea, bool is_string, xkb_keysym_t keysym, char string[64],
//                         uint16_t mods, xkb_key_direction direction) {
//#ifdef TRACY_ENABLE
//    ZoneScoped;
//#endif
//    if (direction == XKB_KEY_UP) {
//        return;
//    }
//    auto *data = (TextAreaData *) textarea->user_data;
//    data->state->last_time_key_press = client->app->current;
//    data->state->cursor_on = true;
//    
//    blink_on(client->app, client, textarea);
//    
//    bool shift = false;
//    bool control = false;
//    if (mods & XCB_MOD_MASK_SHIFT) {
//        shift = true;
//    }
//    if (mods & XCB_MOD_MASK_CONTROL) {
//        control = true;
//    }
//    
//    if (is_string) {
//        if (data->state->selection_x != -1) {
//            replace_action(client, textarea, data, string);
//        } else {
//            insert_action(client, textarea, data, string);
//        }
//    } else {
//        if (keysym == XKB_KEY_BackSpace) {
//            if (data->state->selection_x != -1) {
//                replace_action(client, textarea, data, "");
//            } else {
//                if (control) {
//                    int jump_target = seek_token(data->state, motion::left);
//                    long absolute_distance = std::abs(data->state->cursor - jump_target);
//                    if (absolute_distance != 0) {
//                        delete_action(client, textarea, data, -absolute_distance);
//                    }
//                } else {
//                    if (!data->state->text.empty() && data->state->cursor > 0) {
//                        delete_action(client, textarea, data, -1);
//                    }
//                }
//            }
//        } else if (keysym == XKB_KEY_Delete) {
//            if (data->state->selection_x != -1) {
//                replace_action(client, textarea, data, "");
//            } else {
//                if (control) {
//                    int jump_target = seek_token(data->state, motion::right);
//                    long absolute_distance = std::abs(data->state->cursor - jump_target);
//                    if (absolute_distance != 0) {
//                        delete_action(client, textarea, data, absolute_distance);
//                    }
//                } else {
//                    if (data->state->cursor < data->state->text.size()) {
//                        delete_action(client, textarea, data, 1);
//                    }
//                }
//            }
//        } else if (keysym == XKB_KEY_Escape) {
//            move_cursor(data, data->state->cursor, false);
//        } else if (keysym == XKB_KEY_Return) {
//            if (!data->single_line) {
//                if (data->state->selection_x != -1) {
//                    replace_action(client, textarea, data, "\n");
//                } else {
//                    insert_action(client, textarea, data, "\n");
//                }
//            }
//        } else if (keysym == XKB_KEY_Tab) {
//            if (data->state->selection_x != -1) {
//                replace_action(client, textarea, data, "\t");
//            } else {
//                insert_action(client, textarea, data, "\t");
//            }
//        } else if (keysym == XKB_KEY_a) {
//            if (control) {
//                data->state->cursor = data->state->text.size();
//                data->state->selection_x = 0;
//                put_cursor_on_screen(client, textarea);
//                
//            }
//        } else if (keysym == XKB_KEY_z) {
//            if (control) {
//                // undo
//                if (!data->state->undo_stack.empty()) {
//                    UndoAction *action = data->state->undo_stack.back();
//                    data->state->undo_stack.pop_back();
//                    data->state->redo_stack.push_back(action);
//                    
//                    if (action->type == UndoType::INSERT) {
//                        int cursor_start = action->cursor_start;
//                        int cursor_end = action->cursor_end;
//                        std::string text = action->inserted_text;
//                        
//                        data->state->text.erase(cursor_start, text.size());
//                        
//                        data->state->cursor = cursor_start;
//                        data->state->selection_x = -1;
//                    } else if (action->type == UndoType::DELETE) {
//                        int cursor_start = action->cursor_start;
//                        int cursor_end = action->cursor_end;
//                        
//                        int min = std::min(cursor_start, cursor_end);
//                        int max = std::max(cursor_start, cursor_end);
//                        
//                        std::string text = action->replaced_text;
//                        
//                        data->state->text.insert(min, text);
//                        
//                        data->state->cursor = cursor_start;
//                        data->state->selection_x = -1;
//                    } else if (action->type == UndoType::REPLACE) {
//                        // undo a replace
//                        int cursor_start = action->cursor_start;
//                        int cursor_end = action->cursor_end;
//                        int selection_start = action->selection_start;
//                        int selection_end = action->selection_end;
//                        std::string replaced = action->replaced_text;
//                        std::string inserted = action->inserted_text;
//                        
//                        data->state->text.erase(cursor_end - inserted.size(),
//                                                inserted.size());
//                        data->state->text.insert(cursor_end - inserted.size(), replaced);
//                        
//                        data->state->cursor = cursor_start;
//                        data->state->selection_x = selection_start;
//                    } else if (action->type == UndoType::CURSOR) {
//                        data->state->cursor = action->cursor_start;
//                    }
//                }
//                update_preffered_x(client, textarea);
//                update_bounds(client, textarea);
//                put_cursor_on_screen(client, textarea);
//            }
//        } else if (keysym == XKB_KEY_Z) {
//            if (control) {
//                // redo
//                if (!data->state->redo_stack.empty()) {
//                    UndoAction *action = data->state->redo_stack.back();
//                    data->state->redo_stack.pop_back();
//                    data->state->undo_stack.push_back(action);
//                    
//                    if (action->type == UndoType::INSERT) {
//                        // do an insert
//                        int cursor_start = action->cursor_start;
//                        int cursor_end = action->cursor_end;
//                        std::string text = action->inserted_text;
//                        
//                        data->state->text.insert(cursor_start, text);
//                        
//                        data->state->cursor = cursor_end;
//                        data->state->selection_x = -1;
//                    } else if (action->type == UndoType::DELETE) {
//                        // do a delete
//                        int cursor_start = action->cursor_start;
//                        int cursor_end = action->cursor_end;
//                        
//                        int min = std::min(cursor_start, cursor_end);
//                        int max = std::max(cursor_start, cursor_end);
//                        
//                        std::string text = action->replaced_text;
//                        
//                        data->state->text.erase(min, text.size());
//                        
//                        data->state->cursor = cursor_end;
//                        data->state->selection_x = -1;
//                    } else if (action->type == UndoType::REPLACE) {
//                        // do a replace
//                        int cursor_start = action->cursor_start;
//                        int cursor_end = action->cursor_end;
//                        int selection_start = action->selection_start;
//                        int selection_end = action->selection_end;
//                        std::string replaced = action->replaced_text;
//                        std::string inserted = action->inserted_text;
//                        
//                        int min = std::min(cursor_start, selection_start);
//                        int max = std::max(cursor_start, selection_start);
//                        data->state->text.erase(min, max - min);
//                        data->state->text.insert(min, inserted);
//                        
//                        data->state->cursor = cursor_end;
//                        data->state->selection_x = -1;
//                    } else if (action->type == UndoType::CURSOR) {
//                        data->state->cursor = action->cursor_end;
//                    }
//                }
//                update_preffered_x(client, textarea);
//                update_bounds(client, textarea);
//                put_cursor_on_screen(client, textarea);
//            }
//        } else if (keysym == XKB_KEY_c) {
//            if (control) {
//                // copy current selection or whole text if no selection
//                if (data->state->selection_x == -1) {
//                    // copy whole text
//                    clipboard_set(client->app, data->state->text);
//                } else {
//                    int min_pos = std::min(data->state->cursor, data->state->selection_x);
//                    int max_pos = std::max(data->state->cursor, data->state->selection_x);
//                    std::string selected_text = data->state->text.substr(min_pos, max_pos - min_pos);
//                    clipboard_set(client->app, selected_text);
//                }
//            }
//        } else if (keysym == XKB_KEY_x) {
//            if (control) {
//                if (data->state->selection_x != -1) {
//                    replace_action(client, textarea, data, "");
//                }
//            }
//        } else if (keysym == XKB_KEY_v) {
//            std::string text = clipboard(client->app);
//            if (data->single_line) {
//                text.erase(std::remove_if(text.begin(), text.end(), [](char c) {
//                    return c == '\n' || c == '\r';
//                }), text.end());
//            }
//            if (data->state->selection_x != -1) {
//                replace_action(client, textarea, data, text);
//            } else {
//                insert_action(client, textarea, data, text);
//            }
//        } else if (keysym == XKB_KEY_Home) {
//            Seeker seeker(data->state);
//            
//            seeker.seek_until_specific_token(motion::left, group::newline);
//            
//            move_cursor(data, seeker.current_pos, shift);
//            update_preffered_x(client, textarea);
//            put_cursor_on_screen(client, textarea);
//        } else if (keysym == XKB_KEY_End) {
//            Seeker seeker(data->state);
//            
//            seeker.current_pos -= 1;
//            seeker.seek_until_specific_token(motion::right, group::newline);
//            
//            move_cursor(data, seeker.current_pos + 1, shift);
//            update_preffered_x(client, textarea);
//            put_cursor_on_screen(client, textarea);
//        } else if (keysym == XKB_KEY_Page_Up) {
//            move_vertically_lines(client, data, textarea, shift, -10);
//        } else if (keysym == XKB_KEY_Page_Down) {
//            move_vertically_lines(client, data, textarea, shift, 10);
//        } else if (keysym == XKB_KEY_Left) {
//            if (control) {
//                int jump_target = seek_token(data->state, motion::left);
//                move_cursor(data, jump_target, shift);
//                update_preffered_x(client, textarea);
//                put_cursor_on_screen(client, textarea);
//            } else {
//                int cursor_target = data->state->cursor;
//                cursor_target -= 1;
//                if (cursor_target < 0) {
//                    cursor_target = 0;
//                }
//                move_cursor(data, cursor_target, shift);
//                update_preffered_x(client, textarea);
//                put_cursor_on_screen(client, textarea);
//            }
//        } else if (keysym == XKB_KEY_Right) {
//            if (control) {
//                int jump_target = seek_token(data->state, motion::right);
//                move_cursor(data, jump_target, shift);
//                update_preffered_x(client, textarea);
//                put_cursor_on_screen(client, textarea);
//            } else {
//                int cursor_target = data->state->cursor + 1;
//                if (cursor_target > data->state->text.size()) {
//                    cursor_target = data->state->text.size();
//                }
//                move_cursor(data, cursor_target, shift);
//                update_preffered_x(client, textarea);
//                put_cursor_on_screen(client, textarea);
//            }
//        } else if (keysym == XKB_KEY_Up) {
//            move_vertically_lines(client, data, textarea, shift, -1);
//        } else if (keysym == XKB_KEY_Down) {
//            move_vertically_lines(client, data, textarea, shift, 1);
//        }
//    }
//}
//static void
//textarea_key_release(AppClient *client,
//                     cairo_t *cr,
//                     Container *container,
//                     bool is_string, xkb_keysym_t keysym, char string[64],
//                     uint16_t mods,
//                     xkb_key_direction direction) {
//    if (direction == XKB_KEY_UP) {
//        return;
//    }
//    if (container->parent->active || container->active) {
//        textarea_handle_keypress(client, container, is_string, keysym, string, mods, XKB_KEY_DOWN);
//    }
//}
//
//void key_event_textfield(AppClient *client, cairo_t *cr, Container *container, bool is_string, xkb_keysym_t keysym,
//                         char *string, uint16_t mods, xkb_key_direction direction) {
//    if (!container->active)
//        return;
//    auto *data = (FieldData *) container->user_data;
//    if (direction == XKB_KEY_UP)
//        return;
//    if (is_string) {
//        if (data->settings.only_numbers) {
//            if (string[0] >= '0' && string[0] <= '9') {
//                data->text += string[0];
//            }
//        } else {
//            data->text += string;
//        }
//        if (data->settings.max_size != -1) {
//            data->text = (data->text.length() > data->settings.max_size) ? data->text.substr(0, data->settings.max_size)
//                                                                         : data->text;
//        }
//    } else {
//        if (keysym == XKB_KEY_BackSpace) {
//            if (mods & XCB_MOD_MASK_CONTROL) {
//                data->text = "";
//            } else {
//                if (!data->text.empty()) {
//                    data->text.pop_back();
//                }
//            }
//        }
//    }
//}
//
//#define CURSOR_BLINK_ON_TIME 530
//#define CURSOR_BLINK_OFF_TIME 430
//
//void
//blink_on(App *app, AppClient *client, void *textarea) {
//    auto *container = (Container *) textarea;
//    auto *data = (TextAreaData *) container->user_data;
//    if (data->state->timeout_alive.lock()) {
//        data->state->cursor_blink = app_timeout_replace(app, client, data->state->cursor_blink,
//                            CURSOR_BLINK_ON_TIME, blink_loop, textarea);
//        data->state->timeout_alive = data->state->cursor_blink->lifetime;
//    } else {
//        if (!data->state->cursor_blink) {
//            data->state->cursor_blink = app_timeout_create(app, client, CURSOR_BLINK_ON_TIME, blink_loop, textarea, const_cast<char *>(__PRETTY_FUNCTION__));
//            data->state->timeout_alive = data->state->cursor_blink->lifetime;
//        }
//    }
//    data->state->cursor_on = true;
//    request_refresh(app, client);
//}
//
//void
//blink_loop(App *app, AppClient *client, Timeout *timeout, void *textarea) {
//    if (timeout)
//        timeout->keep_running = true;
//    auto *container = (Container *) textarea;
//    if (!container->parent->active) return;
//    auto *data = (TextAreaData *) container->user_data;
//    
//    float cursor_blink_time = data->state->cursor_on ? CURSOR_BLINK_OFF_TIME : CURSOR_BLINK_ON_TIME;
//    
//    if (!data->state->cursor_blink) {
//        data->state->cursor_blink = app_timeout_create(app, client, cursor_blink_time, blink_loop, textarea, const_cast<char *>(__PRETTY_FUNCTION__));
//    }
//    
//    data->state->cursor_on = !data->state->cursor_on;
//    request_refresh(app, client);
//}
//
//class TransitionData : public UserData {
//public:
//    Container *first = nullptr;
//    cairo_surface_t *original_surface = nullptr;
//    cairo_t *original_cr = nullptr;
//    
//    Container *second = nullptr;
//    cairo_surface_t *replacement_surface = nullptr;
//    cairo_t *replacement_cr = nullptr;
//    
//    double transition_scalar = 0;
//    std::shared_ptr<bool> lifetime = std::make_shared<bool>();
//    
//    int original_anim = 0;
//    int replacement_anim = 0;
//};
//
//void paint_default(AppClient *client, cairo_t *cr, Container *container,
//                   TransitionData *data, cairo_surface_t *surface) {
//#ifdef TRACY_ENABLE
//    ZoneScoped;
//#endif
//    cairo_set_source_surface(cr, surface, container->real_bounds.x, container->real_bounds.y);
//    cairo_paint(cr);
//}
//
//
//void paint_transition_scaled(AppClient *client, cairo_t *cr, Container *container,
//                             TransitionData *data, cairo_surface_t *surface, double scale_amount) {
//#ifdef TRACY_ENABLE
//    ZoneScoped;
//#endif
//    double translate_x = container->real_bounds.x;
//    double translate_y = container->real_bounds.y;
//    if (scale_amount < 1) {
//        // to x and y we will add
//        translate_x += (container->real_bounds.w / 2) * (1 - scale_amount);
//        translate_y += (container->real_bounds.h / 2) * (1 - scale_amount);
//    } else {
//        // to x and y we will subtract
//        translate_x -= (container->real_bounds.w / 2) * (scale_amount - 1);
//        translate_y -= (container->real_bounds.h / 2) * (scale_amount - 1);
//    }
//    
//    if (scale_amount == 0) {
//        // If you try passing cairo_scale "0" it breaks everything and it was VERY hard to debug that
//        return;
//    }
//    
//    cairo_save(cr);
//    cairo_translate(cr, translate_x, translate_y);
//    cairo_scale(cr, scale_amount, scale_amount);
//    cairo_set_source_surface(cr, surface, 0, 0);
//    cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_FAST);
//    cairo_paint(cr);
//    cairo_restore(cr);
//}
//
//void paint_default_to_squashed(AppClient *client, cairo_t *cr, Container *container,
//                               TransitionData *data, cairo_surface_t *surface) {
//#ifdef TRACY_ENABLE
//    ZoneScoped;
//#endif
//    double start = 1;
//    double target = .5;
//    double total_diff = target - start;
//    double scale_amount = start + (total_diff * data->transition_scalar);
//    
//    paint_transition_scaled(client, cr, container, data, surface, scale_amount);
//}
//
//void paint_squashed_to_default(AppClient *client, cairo_t *cr, Container *container,
//                               TransitionData *data, cairo_surface_t *surface) {
//#ifdef TRACY_ENABLE
//    ZoneScoped;
//#endif
//    double start = .5;
//    double target = 1;
//    double total_diff = target - start;
//    double scale_amount = start + (total_diff * data->transition_scalar);
//    
//    paint_transition_scaled(client, cr, container, data, surface, scale_amount);
//}
//
//void paint_default_to_expanded(AppClient *client, cairo_t *cr, Container *container,
//                               TransitionData *data, cairo_surface_t *surface) {
//#ifdef TRACY_ENABLE
//    ZoneScoped;
//#endif
//    double start = 1;
//    double target = 1.75;
//    double total_diff = target - start;
//    double scale_amount = start + (total_diff * data->transition_scalar);
//    
//    paint_transition_scaled(client, cr, container, data, surface, scale_amount);
//}
//
//void paint_expanded_to_default(AppClient *client, cairo_t *cr, Container *container,
//                               TransitionData *data, cairo_surface_t *surface) {
//#ifdef TRACY_ENABLE
//    ZoneScoped;
//#endif
//    double start = 1.75;
//    double target = 1;
//    double total_diff = target - start;
//    double scale_amount = start + (total_diff * data->transition_scalar);
//    
//    paint_transition_scaled(client, cr, container, data, surface, scale_amount);
//}
//
//void paint_transition_surface(AppClient *client, cairo_t *cr, Container *container,
//                              TransitionData *data, cairo_surface_t *surface, int anim) {
//#ifdef TRACY_ENABLE
//    ZoneScoped;
//#endif
//    if (anim & Transition::ANIM_FADE_IN || anim & Transition::ANIM_FADE_OUT) {
//        cairo_push_group(cr);
//    }
//    if (anim & Transition::ANIM_NONE) {
//        paint_default(client, cr, container, data, surface);
//    } else if (anim & Transition::ANIM_DEFAULT_TO_SQUASHED) {
//        paint_default_to_squashed(client, cr, container, data, surface);
//    } else if (anim & Transition::ANIM_SQUASHED_TO_DEFAULT) {
//        paint_squashed_to_default(client, cr, container, data, surface);
//    } else if (anim & Transition::ANIM_DEFAULT_TO_EXPANDED) {
//        paint_default_to_expanded(client, cr, container, data, surface);
//    } else if (anim & Transition::ANIM_EXPANDED_TO_DEFAULT) {
//        paint_expanded_to_default(client, cr, container, data, surface);
//    }
//    
//    if (anim & Transition::ANIM_FADE_IN) {
//        auto p = cairo_pop_group(cr);
//        
//        cairo_set_source(cr, p);
//        cairo_paint_with_alpha(cr, data->transition_scalar);
//    } else if (anim & Transition::ANIM_FADE_OUT) {
//        auto p = cairo_pop_group(cr);
//        
//        cairo_set_source(cr, p);
//        cairo_paint_with_alpha(cr, 1 - data->transition_scalar);
//    }
//}
//
//static void layout_and_repaint(App *app, AppClient *client, Timeout *, void *user_data) {
//#ifdef TRACY_ENABLE
//    ZoneScoped;
//#endif
//    auto *container = (Container *) user_data;
//    if (!container)
//        return;
//    auto data = (TransitionData *) container->user_data;
//    if (!data)
//        return;
//    container->children.clear();
//    container->children.shrink_to_fit();
//    container->children.push_back(data->second);
//    container->children.push_back(data->first);
//    data->first->interactable = true;
//    data->second->interactable = true;
//    delete data;
//    container->automatically_paint_children = true;
//    container->user_data = nullptr;
//    container->when_paint = nullptr;
//    
//    client_layout(client->app, client);
//    request_refresh(client->app, client);
//}
//
//void paint_transition(AppClient *client, cairo_t *cr, Container *container) {
//#ifdef TRACY_ENABLE
//    ZoneScoped;
//#endif
//    auto data = (TransitionData *) container->user_data;
//    if (!data->original_surface || !data->replacement_surface) {
//        return;
//    }
//    
//    paint_transition_surface(client, cr, container, data, data->original_surface,
//                             data->original_anim);
//    
//    paint_transition_surface(client, cr, container, data, data->replacement_surface,
//                             data->replacement_anim);
//    
//    if (data->transition_scalar >= 1) {
//        app_timeout_create(client->app, client, 0, layout_and_repaint, container, const_cast<char *>(__PRETTY_FUNCTION__));
//    }
//}
//
//void transition_same_container(AppClient *client, cairo_t *cr, Container *parent, int original_anim,
//                               int replacement_anim) {
//#ifdef TRACY_ENABLE
//    ZoneScoped;
//#endif
//    if (parent->user_data != nullptr) {
//        return;
//    }
//    auto data = new TransitionData;
//    parent->user_data = data;
//    parent->when_paint = paint_transition;
//    parent->automatically_paint_children = false;
//    
//    data->first = parent->children[0];
//    data->second = parent->children[1];
//    data->first->interactable = false;
//    data->second->interactable = false;
//    data->original_anim = original_anim;
//    data->replacement_anim = replacement_anim;
//    
//    // layout and paint both first and second containers into their own temp surfaces
//    {
//        data->original_surface = accelerated_surface(client->app, client, parent->real_bounds.w,
//                                                     parent->real_bounds.h);
//        data->original_cr = cairo_create(data->original_surface);
//        layout(client, cr, data->first,
//               Bounds(0,
//                      0,
//                      parent->real_bounds.w,
//                      parent->real_bounds.h));
//        auto main_cr = client->cr;
//        client->cr = data->original_cr;
//        paint_container(client->app, client, data->first);
//        client->cr = main_cr;
//    }
//    {
//        data->replacement_surface = accelerated_surface(client->app, client, parent->real_bounds.w,
//                                                        parent->real_bounds.h);
//        data->replacement_cr = cairo_create(data->replacement_surface);
//        data->second->exists = true;
//        layout(client, cr, data->second,
//               Bounds(0,
//                      0,
//                      parent->real_bounds.w,
//                      parent->real_bounds.h));
//        auto main_cr = client->cr;
//        client->cr = data->replacement_cr;
//        paint_container(client->app, client, data->second);
//        data->second->exists = false;
//        client->cr = main_cr;
//    }
//    
//    client_create_animation(client->app, client, &data->transition_scalar, data->lifetime, 0, 350,
//                            getEasingFunction(::EaseOutQuint), 1, false);
//}
//
//int get_offset(Container *target, ScrollContainer *scroll_pane) {
//    int offset = scroll_pane->content->wanted_pad.h;
//    
//    for (int i = 0; i < scroll_pane->content->children.size(); i++) {
//        auto possible = scroll_pane->content->children[i];
//        
//        if (possible == target) {
//            return offset;
//        }
//        
//        offset += possible->real_bounds.h;
//        offset += scroll_pane->content->spacing;
//    }
//    
//    return offset;
//}
//
//static void
//paint_textfield(AppClient *client, cairo_t *cr, Container *container) {
//    // paint blue border
//    auto *data = (FieldData *) container->user_data;
//
//    // clip text
//    PangoLayout *layout = get_cached_pango_font(cr, config->font, data->settings.font_size,
//                                                PangoWeight::PANGO_WEIGHT_NORMAL);
//    pango_layout_set_width(layout, -1); // disable wrapping
//    
//    set_argb(cr, config->color_pinned_icon_editor_field_default_text);
//    std::string text_set;
//    RGBA color = config->color_pinned_icon_editor_field_default_text;
//    if (!data->text.empty() || container->active) {
//        pango_layout_set_text(layout, data->text.c_str(), data->text.size());
//        text_set = data->text;
//    } else {
//        auto watered_down = RGBA(config->color_pinned_icon_editor_field_default_text);
//        watered_down.a = 0.6;
//        color = watered_down;
//        set_argb(cr, watered_down);
//        pango_layout_set_text(layout, data->settings.when_empty_text.c_str(), data->settings.when_empty_text.size());
//        text_set = data->settings.when_empty_text;
//    }
//    PangoRectangle ink;
//    PangoRectangle logical;
//    pango_layout_get_extents(layout, &ink, &logical);
//    
//    auto text_off_x = 10 * config->dpi;
//    auto text_off_y = container->real_bounds.h / 2 - ((logical.height / PANGO_SCALE) / 2);
//    
//    /*
//    cairo_move_to(cr,
//                  container->real_bounds.x + text_off_x,
//                  container->real_bounds.y + text_off_y);
//    pango_cairo_show_layout(cr, layout);
//    */
//    draw_text(client, data->settings.font_size, config->font, EXPAND(color), text_set, container->real_bounds, 5, text_off_x, text_off_y);
//    
//    if (container->active) {
//        PangoRectangle cursor_strong_pos;
//        PangoRectangle cursor_weak_pos;
//        pango_layout_get_cursor_pos(layout, data->text.size(), &cursor_strong_pos, &cursor_weak_pos);
//        
//        int offset = cursor_strong_pos.x != 0 ? -1 : 0;
//        draw_colored_rect(RGBA(0, 0, 0, 1), Bounds(cursor_strong_pos.x / PANGO_SCALE + container->real_bounds.x + offset + text_off_x,
//                        cursor_strong_pos.y / PANGO_SCALE + container->real_bounds.y + text_off_y,
//                        1 * config->dpi,
//                        cursor_strong_pos.height / PANGO_SCALE)); 
//    }
//    
//    color = config->color_pinned_icon_editor_field_default_border;
//    if (container->active) {
//        color = config->color_pinned_icon_editor_field_pressed_border;
//    } else if (container->state.mouse_hovering) {
//        color = config->color_pinned_icon_editor_field_hovered_border;
//    }
//    draw_margins_rect(color, container->real_bounds, 2, 0);
//}
//
//Container *make_textfield(Container *parent, FieldSettings settings, int w, int h) {
//    auto *field = parent->child(w, h);
//    auto data = new FieldData;
//    data->settings = std::move(settings);
//    field->user_data = data;
//    field->when_paint = paint_textfield;
//    field->when_key_event = key_event_textfield;
//    
//    return field;
//}
//
//PopupItemDraw::PopupItemDraw(const std::string &icon0, const std::string &icon1, const std::string &text,
//                             const std::string &iconend) : icon0(icon0), icon1(icon1), text(text), iconend(iconend) {}
//
//static void
//paint_combobox_root(AppClient *client, cairo_t *cr, Container *container) {
//    draw_colored_rect(correct_opaqueness(client, config->color_pinned_icon_editor_background),
//                      container->real_bounds);
//}
//
//static void paint_combox_item(AppClient *client, cairo_t *cr, Container *container) {
//    auto *label = (Label *) container->user_data;
//    if (container->state.mouse_pressing || container->state.mouse_hovering) {
//        if (container->state.mouse_pressing) {
//            draw_colored_rect(darken(config->color_pinned_icon_editor_background, 15), container->real_bounds);
//        } else if (container->state.mouse_hovering) {
//            draw_colored_rect(darken(config->color_pinned_icon_editor_background, 7), container->real_bounds);
//        }
//    }
//    draw_text(client, 9 * config->dpi, config->font, EXPAND(config->color_pinned_icon_editor_field_default_text), label->text, container->real_bounds, 5, container->wanted_pad.x + 12 * config->dpi);
//}
//
//static void paint_combox_item_dark(AppClient *client, cairo_t *cr, Container *container) {
//    auto *label = (Label *) container->user_data;
//    if (container->state.mouse_pressing || container->state.mouse_hovering) {
//        if (container->state.mouse_pressing) {
//            draw_colored_rect(darken(config->color_search_accent, 15), container->real_bounds);
//        } else if (container->state.mouse_hovering) {
//            draw_colored_rect(darken(config->color_search_accent, 7), container->real_bounds);
//        }
//    } else {
//        draw_colored_rect(config->color_search_accent, container->real_bounds);
//    }
//    
//    draw_text(client, 9 * config->dpi, config->font, EXPAND(config->color_wifi_icons), label->text, container->real_bounds, 5, container->wanted_pad.x + 12 * config->dpi);
//}
//
//void clicked_expand_generic_combobox_base(AppClient *client, cairo_t *cr, Container *container, double option_h, bool down, bool dark) {
//    auto *data = (GenericComboBox *) container->user_data;
//    data->creator_container = container;
//    data->lifetime = container->lifetime;
//    data->creator_client = client;
//    if (data->options.empty())
//        return;
//    
//    float total_text_height = 0;
//    
//    auto taskbar = client_by_name(client->app, "taskbar");
//    PangoLayout *layout = get_cached_pango_font(taskbar->cr, config->font, 9 * config->dpi, PANGO_WEIGHT_NORMAL);
//    int width;
//    int height;
//    for (const auto &m: data->options) {
//        pango_layout_set_text(layout, m.c_str(), -1);
//        pango_layout_get_pixel_size_safe(layout, &width, &height);
//        total_text_height += height;
//    }
//    total_text_height += data->options.size() * (option_h); // pad
//    
//    Settings settings;
//    settings.force_position = true;
//    settings.w = container->real_bounds.w;
//    settings.h = total_text_height;
//    settings.x = client->bounds->x + container->real_bounds.x;
//    settings.y = client->bounds->y + container->real_bounds.y + container->real_bounds.h - 2 * config->dpi;
//    settings.skip_taskbar = true;
//    settings.decorations = false;
//    settings.override_redirect = true;
//    settings.no_input_focus = true;
//    settings.slide = true;
//    settings.slide_data[0] = -1;
//    settings.slide_data[1] = 1;
//    settings.slide_data[2] = 160;
//    settings.slide_data[3] = 100;
//    settings.slide_data[4] = 80;
//    if (!down) {
//        settings.y = client->bounds->y + container->real_bounds.y + 2 * config->dpi - settings.h;
//        settings.slide_data[1] = 3;
//        int w = 1;
//        settings.x += w * config->dpi;
//        settings.w -= w * 2 * config->dpi;
//    }
//    
//    PopupSettings popup_settings;
//    popup_settings.takes_input_focus = false;
//    popup_settings.close_on_focus_out = true;
//    popup_settings.wants_grab = true;
//    
//    auto popup = client->create_popup(popup_settings, settings);
//    popup->root->when_paint = paint_combobox_root;
//    popup->root->type = ::vbox;
//    popup->root->skip_delete = true;
//    popup->root->user_data = data;
//    for (const auto &m: data->options) {
//        auto c = popup->root->child(FILL_SPACE, FILL_SPACE);
//        c->when_paint = paint_combox_item;
//        if (dark) {
//            c->when_paint = paint_combox_item_dark;
//        }
//        c->when_clicked = data->when_clicked;
//        auto label = new Label(m);
//        c->user_data = label;
//    }
//    client_show(client->app, popup);
//}
//
//void clicked_expand_generic_combobox(AppClient *client, cairo_t *cr, Container *container) {
//    clicked_expand_generic_combobox_base(client, cr, container, 12 * config->dpi, true);
//}
//
//void clicked_expand_generic_combobox_dark(AppClient *client, cairo_t *cr, Container *container) {
//    clicked_expand_generic_combobox_base(client, cr, container, 22 * config->dpi, false, true);
//}
//
//void paint_generic_combobox_dark(AppClient *client, cairo_t *cr, Container *container) {
//    auto color = config->color_wifi_default_button;
//    if (container->state.mouse_pressing || container->state.mouse_hovering) {
//        if (container->state.mouse_pressing) {
//            color = config->color_wifi_pressed_button;
//        } else {
//            color = config->color_wifi_hovered_button;
//        }
//    }
//    draw_colored_rect(color, container->real_bounds);
//
//    draw_margins_rect(config->color_wifi_hovered_button, container->real_bounds, 2 * config->dpi, 0);
//
//
//    auto *data = (GenericComboBox *) container->user_data;
//    std::string selected = data->prompt;
//    if (data->determine_selected)
//        selected += data->determine_selected(client, cr, container);
//    
//    draw_text(client, 9 * config->dpi, config->font, EXPAND(config->color_wifi_icons), selected, container->real_bounds, 5, container->wanted_pad.x + 8 * config->dpi);
//    
//    auto [f, w, h] = draw_text_begin(client, 9 * config->dpi, config->icons, EXPAND(config->color_wifi_icons),
//                                     "\uE70D", false);
//    f->draw_text((int) (container->real_bounds.x + container->real_bounds.w - w - 8 * config->dpi),
//                     (int) (container->real_bounds.y + container->real_bounds.h / 2 - h / 2));
//    f->end();
//}
//
//void paint_generic_combobox(AppClient *client, cairo_t *cr, Container *container) {
//    auto old = container->real_bounds;
//    int pad = 3;
//    container->real_bounds.y += pad * config->dpi;
//    container->real_bounds.h -= (pad * 2) * config->dpi;
//    paint_reordable_item(client, cr, container);
//    container->real_bounds = old;
//    
//    auto *data = (GenericComboBox *) container->user_data;
//    std::string selected = data->prompt;
//    if (data->determine_selected)
//        selected += data->determine_selected(client, cr, container);
//       
//    draw_text(client, 9 * config->dpi, config->font, EXPAND(config->color_pinned_icon_editor_field_default_text), selected, container->real_bounds, 5, container->wanted_pad.x + 8 * config->dpi);
//    
//    auto [f, w, h] = draw_text_begin(client, 9 * config->dpi, config->icons,
//                                     EXPAND(config->color_pinned_icon_editor_field_default_text), "\uE70D",
//                                     false);
//    f->draw_text_end((int) (container->real_bounds.x + container->real_bounds.w - w - 8 * config->dpi),
//                     (int) (container->real_bounds.y + container->real_bounds.h / 2 - h / 2));
//}
//
//void paint_reordable_item(AppClient *client, cairo_t *cr, Container *container) {
//    rounded_rect(client, 4 * config->dpi, container->real_bounds.x, container->real_bounds.y, container->real_bounds.w,
//                 container->real_bounds.h, RGBA(.984, .988, .992, 1));
//    
//    auto color = config->color_pinned_icon_editor_field_default_border;
//    if (container->state.mouse_hovering)
//        color = config->color_pinned_icon_editor_field_hovered_border;
//    if (container->state.mouse_pressing)
//        color = config->color_pinned_icon_editor_field_pressed_border;
//    rounded_rect(client, 4 * config->dpi, container->real_bounds.x, container->real_bounds.y, container->real_bounds.w,
//                 container->real_bounds.h, color, std::floor(1 * config->dpi));
//}


ScrollContainer *
make_newscrollpane_as_child(Container *parent, const ScrollPaneSettings &settings, std::function<DrawContext (Container *root)> func) {
    auto scrollpane = new ScrollContainer(settings);
    auto data = new ScrollData;
    data->func = func;
    scrollpane->user_data = data;
    scrollpane->pre_layout = [](Container *root, Container *c, const Bounds &b) {
        auto scroll = (ScrollContainer *) c;
        auto data = (ScrollData *) c->user_data;
        if (data->func) {
            DrawContext ctx = data->func(root);
            int amount = std::round(12.0f * (ctx.dpi));
            scroll->settings.right_width = amount; 
            scroll->settings.right_arrow_height = amount; 
            scroll->settings.bottom_height = amount; 
            scroll->settings.bottom_arrow_width = amount; 
        }
    };
    // setup as child of root
    if (parent)
        scrollpane->parent = parent;
//    scrollpane->when_scrolled = scrollpane_scrolled;
    scrollpane->when_fine_scrolled = fine_scrollpane_scrolled;
    scrollpane->receive_events_even_if_obstructed = true;
    if (settings.start_at_end) {
        scrollpane->scroll_v_real = -1000000;
        scrollpane->scroll_v_visual = -1000000;
    }
    scrollpane->scrollbar_openess = 0;
    scrollpane->scrollbar_visible = 0;
    if (parent)
        parent->children.push_back(scrollpane);
    scrollpane->when_mouse_leaves_container = [](Container *root, Container *container) {
        auto scroll = (ScrollContainer *) container;
        auto data = (ScrollData *) scroll->user_data;
        auto ctx = data->func(root);
        animate(&scroll->scrollbar_visible, 0.0, 100, scroll->lifetime, nullptr, schedule_redraw(ctx)); 
        animate(&scroll->scrollbar_openess, 0.0, 100, scroll->lifetime, nullptr, schedule_redraw(ctx)); 
    };
    scrollpane->when_mouse_enters_container = [](Container *root, Container *container) {
        auto scroll = (ScrollContainer *) container;
        auto data = (ScrollData *) scroll->user_data;
        auto ctx = data->func(root);
        animate(&scroll->scrollbar_visible, 1.0, 100, scroll->lifetime, nullptr, schedule_redraw(ctx)); 
    };
    
    auto content = new Container(::vbox, FILL_SPACE, FILL_SPACE);
    // setup as content of scrollpane
    content->parent = scrollpane;
    scrollpane->content = content;
    
    auto right_vbox = new Container(FILL_SPACE, FILL_SPACE);
    scrollpane->right = right_vbox;
    right_vbox->parent = scrollpane;
    right_vbox->type = ::vbox;
    right_vbox->when_fine_scrolled = fine_right_thumb_scrolled;
    right_vbox->when_mouse_leaves_container = [](Container *root, Container *container) {
        auto scroll = (ScrollContainer *) container->parent;
        auto data = (ScrollData *) scroll->user_data;
        auto ctx = data->func(root);
        animate(&scroll->scrollbar_openess, 0.0, 100, scroll->lifetime, nullptr, schedule_redraw(ctx));
        
        /*
            int delay = 0;
            if (bounds_contains(scroll->real_bounds, root->mouse_current_x, root->mouse_current_y)) {
                delay = 3000;
            }
            if (delay == 0) {
                animate(&scroll->scrollbar_openess, 0.0, 100, scroll->lifetime);
                //client_create_animation(client->app, client, &scroll->scrollbar_openess, scroll->lifetime,
                                        //delay, 100, nullptr, 0);
            } else {
                if (!scroll->openess_delay_timeout) {
                    scroll->openess_delay_timeout =
                            app_timeout_create(client->app, client, 3000, scroll_hover_timeout,
                                               container->parent, "scrollpane_3000ms_timeout");
                } else {
                    scroll->openess_delay_timeout = app_timeout_replace(client->app, client, scroll->openess_delay_timeout,
                                                                        3000, scroll_hover_timeout, container->parent);
                }
            }
        */
    };
    right_vbox->when_mouse_enters_container = [](Container *root, Container *container) {
        auto scroll = (ScrollContainer *) container->parent;
        later(200, [root, scroll](Timer *) {
            ignore_first_hover(root, scroll);
        });
    };
    right_vbox->receive_events_even_if_obstructed = true;

    auto right_top_arrow = right_vbox->child(FILL_SPACE, settings.right_arrow_height);
    right_top_arrow->user_data = new ButtonData;
    ((ButtonData *) right_top_arrow->user_data)->text = "\uE971";
    right_top_arrow->when_paint = paint_arrow;
    right_top_arrow->when_mouse_down = paint {
        mouse_down_arrow_up(root, c);
    };
    right_top_arrow->when_mouse_up = mouse_arrow_up;
    right_top_arrow->when_clicked = mouse_arrow_up;
    right_top_arrow->when_drag_end = mouse_arrow_up;
    auto right_thumb = right_vbox->child(FILL_SPACE, FILL_SPACE);
    right_thumb->when_paint = paint_right_thumb;
    right_thumb->when_drag_start = right_scrollbar_drag_start;
    right_thumb->when_drag = right_scrollbar_drag;
    right_thumb->when_drag_end = right_scrollbar_drag_end;
    right_thumb->when_mouse_down = right_scrollbar_mouse_down;
    
    auto right_bottom_arrow = right_vbox->child(FILL_SPACE, settings.right_arrow_height);
    right_bottom_arrow->user_data = new ButtonData;
    ((ButtonData *) right_bottom_arrow->user_data)->text = "\uE972";
    right_bottom_arrow->when_paint = paint_arrow;
    right_bottom_arrow->when_mouse_down = paint {
        mouse_down_arrow_bottom(root, c);
    };
    right_bottom_arrow->when_mouse_up = mouse_arrow_up;
    right_bottom_arrow->when_clicked = mouse_arrow_up;
    right_bottom_arrow->when_drag_end = mouse_arrow_up;
    right_vbox->z_index += 1;
    
    auto bottom_hbox = new Container(FILL_SPACE, FILL_SPACE);
    scrollpane->bottom = bottom_hbox;
    bottom_hbox->parent = scrollpane;
    bottom_hbox->type = ::hbox;
    bottom_hbox->when_fine_scrolled = fine_bottom_thumb_scrolled;
    auto bottom_left_arrow = bottom_hbox->child(settings.bottom_arrow_width, FILL_SPACE);
    bottom_left_arrow->user_data = new ButtonData;
    ((ButtonData *) bottom_left_arrow->user_data)->text = "\uE973";
    //bottom_left_arrow->when_paint = paint_arrow;
    //bottom_left_arrow->when_mouse_down = mouse_down_arrow_left;
    //bottom_left_arrow->when_mouse_up = mouse_arrow_up;
    //bottom_left_arrow->when_clicked = mouse_arrow_up;
    //bottom_left_arrow->when_drag_end = mouse_arrow_up;
    auto bottom_thumb = bottom_hbox->child(FILL_SPACE, FILL_SPACE);
    //bottom_thumb->when_paint = paint_bottom_thumb;
    //bottom_thumb->when_drag_start = bottom_scrollbar_drag_start;
    //bottom_thumb->when_drag = bottom_scrollbar_drag;
    //bottom_thumb->when_drag_end = bottom_scrollbar_drag_end;
    //bottom_thumb->when_mouse_down = bottom_scrollbar_mouse_down;
    
    auto bottom_right_arrow = bottom_hbox->child(settings.bottom_arrow_width, FILL_SPACE);
    bottom_right_arrow->user_data = new ButtonData;
    ((ButtonData *) bottom_right_arrow->user_data)->text = "\uE974";
    bottom_right_arrow->when_paint = paint_arrow;
    //bottom_right_arrow->when_mouse_down = mouse_down_arrow_right;
    //bottom_right_arrow->when_mouse_up = mouse_arrow_up;
    //bottom_right_arrow->when_clicked = mouse_arrow_up;
    //bottom_right_arrow->when_drag_end = mouse_arrow_up;
    bottom_hbox->z_index += 1;
    
    return scrollpane;    
}




//
// Created by jmanc3 on 6/14/20.
//

#include "components.h"
#include "hypriso.h"
#include "hsluv.h"
#include "heart.h"
#include "events.h"
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
Bounds bottom_thumb_bounds(Container *scrollpane, Bounds thumb_area);
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
    auto scroll = (ScrollContainer *) container;
    if (bounds_contains(scroll->bottom->real_bounds, root->mouse_current_x, root->mouse_current_y)) {
        float temp = scroll_y;
        scroll_y = scroll_x;
        scroll_x = temp;
    }

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
        
    int small_width = std::round(3.0 * dpi);
    
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


static void
paint_bottom_thumb(Container *root, Container *container) {
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif
    bool minimal = ((ScrollContainer *) container->parent->parent)->settings.paint_minimal;
    auto scroll_container = ((ScrollContainer *) container->parent->parent);
    
    if (!minimal)
        paint_scroll_bg(root, container);
    
    Container *scrollpane = container->parent->parent;
    
    auto bottom_bounds = bottom_thumb_bounds(scrollpane, container->real_bounds);

    auto data = (ScrollData *) scroll_container->user_data;
    float dpi = 1.0;
    if (data->func) {
        auto ctx = data->func(root);
        dpi = ctx.dpi;
    }
    int small_width = std::round(3.0 * dpi);

    bottom_bounds.y += bottom_bounds.h;
    bottom_bounds.h = std::max(bottom_bounds.h * scroll_container->scrollbar_openess, (double) small_width);
    bottom_bounds.y -= bottom_bounds.h;
    bottom_bounds.y -= small_width * (1 - scroll_container->scrollbar_openess);
    
    RGBA color = config->color_apps_scrollbar_default_thumb;
    
    if (container->state.mouse_pressing) {
        color = config->color_apps_scrollbar_pressed_thumb;
    } else if (bounds_contains(bottom_bounds, root->mouse_current_x, root->mouse_current_y)) {
        color = config->color_apps_scrollbar_hovered_thumb;
    } else if (bottom_bounds.w == small_width) {
        color = config->color_apps_scrollbar_default_thumb;
        lighten(&color, 10);
    }
    color.a = scroll_container->scrollbar_visible;

    draw_colored_rect(root, scroll_container, color, bottom_bounds);
}

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
    later(first_time ? 450 : 150, [lifetime, container, root, target](Timer *) {
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
    later(first_time ? 450 : 150, [lifetime, container, root, target](Timer *) {
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

static void
mouse_down_arrow_left(Container *root, Container *container, bool first_time = true) {
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif
    auto target = (ScrollContainer *) container->parent->parent;
    bool clamp = true;
    target->scroll_h_real += scroll_amount;
    target->scroll_h_visual = target->scroll_v_real;
    if (clamp) clamp_scroll((ScrollContainer *) target);
    ::layout(root, container, container->real_bounds);
    std::weak_ptr<bool> lifetime = container->lifetime;
    later(first_time ? 450 : 150, [lifetime, container, root, target](Timer *) {
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


static void
mouse_down_arrow_right(Container *root, Container *container, bool first_time = true) {
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif
    auto target = (ScrollContainer *) container->parent->parent;
    bool clamp = true;
    target->scroll_h_real -= scroll_amount;
    target->scroll_h_visual = target->scroll_v_real;
    if (clamp) clamp_scroll((ScrollContainer *) target);
    ::layout(root, container, container->real_bounds);
    std::weak_ptr<bool> lifetime = container->lifetime;
    later(first_time ? 450 : 150, [lifetime, container, root, target](Timer *) {
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

Bounds
bottom_thumb_bounds(Container *scrollpane, Bounds thumb_area) {
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif
    if (auto *s = dynamic_cast<ScrollContainer *>(scrollpane)) {
        double true_width = actual_true_width(s->content);
        if (s->right && s->right->exists && !s->settings.right_inline_track)
            true_width += s->right->real_bounds.w;
        
        double start_scalar = -s->scroll_h_visual / true_width;
        double thumb_width = thumb_area.w * (s->real_bounds.w / true_width);
        double start_off = thumb_area.x + thumb_area.w * start_scalar;
        double avail = thumb_area.w - (start_off - thumb_area.x);
        
        return {thumb_area.x + thumb_area.w * start_scalar, thumb_area.y, std::min(thumb_width, avail), thumb_area.h};
    }
    
    auto content_area = scrollpane->children[2];
    double total_width = content_area->children[0]->real_bounds.w;
    
    double view_width = content_area->real_bounds.w;
    
    if (total_width == 0) {
        total_width = view_width;
    }
    double view_scalar = view_width / total_width;
    double thumb_width = view_scalar * thumb_area.w;
    
     //0 as min pos and 1 as max position
    double max_scroll = total_width - view_width;
    if (max_scroll < 0)
        max_scroll = 0;
    
    double scroll_scalar = (-content_area->scroll_h_visual) / max_scroll;
    double scroll_offset = (thumb_area.w - thumb_width) * scroll_scalar;
    if (max_scroll == 0) {
        scroll_offset = 0;
    }
    
    if (thumb_width > thumb_area.w) {
        thumb_width = thumb_area.w;
    }
    return Bounds(thumb_area.x + scroll_offset, thumb_area.y, thumb_width, thumb_area.h);
}

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

static void
clicked_bottom_thumb(Container *root, Container *thumb_container, bool animate) {
    if (auto *s = dynamic_cast<ScrollContainer *>(thumb_container->parent->parent)) {
        auto *content = s->content;
        
        double thumb_width =
                bottom_thumb_bounds(thumb_container->parent->parent, thumb_container->real_bounds).w;
        double mouse_x = root->mouse_current_x;
        if (mouse_x < thumb_container->real_bounds.x) {
            mouse_x = thumb_container->real_bounds.x;
        } else if (mouse_x > thumb_container->real_bounds.x + thumb_container->real_bounds.w) {
            mouse_x = thumb_container->real_bounds.x + thumb_container->real_bounds.w;
        }
        
        mouse_x -= thumb_container->real_bounds.x;
        mouse_x -= thumb_width / 2;
        double scalar = mouse_x / thumb_container->real_bounds.w;
        
        double true_width = actual_true_width(s->content);
        if (s->right && s->right->exists && !s->settings.right_inline_track)
            true_width += s->right->real_bounds.w;
        
        double x = true_width * scalar;
        
        s->scroll_h_real = -x;
        s->scroll_h_visual = -x;
        ::layout(root, s, s->real_bounds);
        
        return;
    }
}

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

static void
bottom_scrollbar_mouse_down(Container *root, Container *container) {
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif
    clicked_bottom_thumb(root, container, true);
}

static void
bottom_scrollbar_drag_start(Container *root, Container *container) {
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif
    clicked_bottom_thumb(root, container, false);
}

static void
bottom_scrollbar_drag(Container *root, Container *container) {
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif
    clicked_bottom_thumb(root, container, false);
}

static void
bottom_scrollbar_drag_end(Container *root, Container *container) {
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif
    clicked_bottom_thumb(root, container, false);
}

void ignore_first_hover(Container *root, ScrollContainer *scroll) {
    if (bounds_contains(scroll->right->real_bounds, root->mouse_current_x, root->mouse_current_y)) {
        auto data = (ScrollData *) scroll->user_data;
        auto ctx = data->func(root);
        animate(&scroll->scrollbar_openess, 1.0, 100, scroll->lifetime, nullptr, schedule_redraw(ctx));
    }
}

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
            scroll->right->children[0]->wanted_bounds.h = amount;
            scroll->right->children[2]->wanted_bounds.h = amount;
            scroll->bottom->children[0]->wanted_bounds.w = amount;
            scroll->bottom->children[2]->wanted_bounds.w = amount;
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
    right_vbox->z_index += 1;
    
    auto bottom_hbox = new Container(FILL_SPACE, FILL_SPACE);
    bottom_hbox->receive_events_even_if_obstructed = true;
    scrollpane->bottom = bottom_hbox;
    bottom_hbox->parent = scrollpane;
    bottom_hbox->type = ::hbox;
    bottom_hbox->when_fine_scrolled = fine_bottom_thumb_scrolled;
    bottom_hbox->when_mouse_leaves_container = [](Container *root, Container *container) {
        auto scroll = (ScrollContainer *) container->parent;
        auto data = (ScrollData *) scroll->user_data;
        auto ctx = data->func(root);
        animate(&scroll->scrollbar_openess, 0.0, 100, scroll->lifetime, nullptr, schedule_redraw(ctx));
    };
    bottom_hbox->when_mouse_enters_container = [](Container *root, Container *container) {
        auto scroll = (ScrollContainer *) container->parent;
        later(200, [root, scroll](Timer *) {
            ignore_first_hover(root, scroll);
        });
    };

    auto bottom_left_arrow = bottom_hbox->child(settings.bottom_arrow_width, FILL_SPACE);
    bottom_left_arrow->user_data = new ButtonData;
    ((ButtonData *) bottom_left_arrow->user_data)->text = "\uE973";
    bottom_left_arrow->when_paint = paint_arrow;
    bottom_left_arrow->when_mouse_down = [](Container *root, Container *c) {
        mouse_down_arrow_right(root, c);
    };
    
    auto bottom_thumb = bottom_hbox->child(FILL_SPACE, FILL_SPACE);
    bottom_thumb->when_paint = paint_bottom_thumb;
    bottom_thumb->when_drag_start = bottom_scrollbar_drag_start;
    bottom_thumb->when_drag = bottom_scrollbar_drag;
    bottom_thumb->when_drag_end = bottom_scrollbar_drag_end;
    bottom_thumb->when_mouse_down = bottom_scrollbar_mouse_down;
    
    auto bottom_right_arrow = bottom_hbox->child(settings.bottom_arrow_width, FILL_SPACE);
    bottom_right_arrow->user_data = new ButtonData;
    ((ButtonData *) bottom_right_arrow->user_data)->text = "\uE974";
    bottom_right_arrow->when_paint = paint_arrow;
    bottom_right_arrow->when_mouse_down = [](Container *root, Container *c) {
        mouse_down_arrow_right(root, c);
    };
    bottom_hbox->z_index += 1;
    
    return scrollpane;    
}




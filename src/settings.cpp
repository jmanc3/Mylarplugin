
#include "settings.h"

#include "container.h"
#include "desktop_icons.h"
#include "heart.h"
#include "hypriso.h"
#include "client/raw_windowing.h"
#include "client/windowing.h"
#include "dock.h"
#include <chrono>
#include <csetjmp>
#include <hyprland/src/helpers/MiscFunctions.hpp>

#include <gtk/gtk.h>
#include <thread>
#include <fstream>
#include <filesystem>
#include <pango/pango-font.h>
#include <cairo.h>
#include <pango/pango-layout.h>
#include <pango/pango-types.h>
#include <pango/pangocairo.h>
#include <vector>
#include <xkbcommon/xkbcommon-keysyms.h>

static RawApp *settings_app = nullptr;
static MylarWindow *settings_mylar = nullptr;

static RGBA left_color = RGBA(.941, .957, .976, 1);
static RGBA right_color = RGBA(.941, .957, .976, 1);
static RGBA option_color = RGBA(.87, .87, .87, 1);
static RGBA option_widget_bg_color = RGBA(.84, .84, .84, 1);
static RGBA slider_bg = option_widget_bg_color;
static RGBA bool_border = option_widget_bg_color;
static RGBA accent = RGBA(.0, .52, .9, 1);

static float optiontopbottompad = 15;
static float optionleftpad = 14;
static float optionrighttpad = 14;

struct LineParser {
    std::string_view input;
    size_t index = 0;

    explicit LineParser(std::string_view input)
        : input(input) {}

    bool has_another_line() const {
        return index < input.size();
    }

    void next_line() {
        while (index < input.size()) {
            char c = input[index++];

            if (c == '\r' || c == '\n') {
                // Treat \r\n as a single line ending.
                if (c == '\r' &&
                    index < input.size() &&
                    input[index] == '\n') {
                    ++index;
                }

                break;
            }
        }
    }

    bool not_end_of_line() const {
        return index < input.size() &&
               input[index] != '\n' &&
               input[index] != '\r';
    }

    void eat_blank() {
        while (not_end_of_line()) {
            char c = input[index];

            if (c != ' ' && c != '\t')
                break;

            ++index;
        }
    }

    std::optional<std::string_view> eat_until(std::string_view delimiter) {
        size_t start = index;

        while (not_end_of_line()) {
            if (input.substr(index, delimiter.size()) == delimiter) {
                auto result = input.substr(start, index - start);
                index += delimiter.size();
                return result;
            }

            ++index;
        }

        // Return whatever was consumed before reaching the end of the line.
        if (index > start) {
            return input.substr(start, index - start);
        }

        return std::nullopt;
    }

    std::string_view eat_until_line_end() {
        size_t start = index;

        while (not_end_of_line())
            ++index;

        return input.substr(start, index - start);
    }
};

struct RightData : UserData {
    float scroll = 0.0;
};

std::vector<MylarWindow *> popups;

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

#include <sstream>
#include <algorithm>

static std::string trim(std::string s) {
    s.erase(s.begin(), std::find_if(s.begin(), s.end(),
        [](unsigned char c){ return !std::isspace(c); }));
    s.erase(std::find_if(s.rbegin(), s.rend(),
        [](unsigned char c){ return !std::isspace(c); }).base(), s.end());
    return s;
}

static std::string trim_newline(std::string s) {
    s.erase(s.begin(), std::find_if(s.begin(), s.end(),
        [](unsigned char c){ return !std::iscntrl(c); }));
    s.erase(std::find_if(s.rbegin(), s.rend(),
        [](unsigned char c){ return !std::iscntrl(c); }).base(), s.end());
    return s;
}

std::string choose_file(bool folder = false, GtkWindow* parent = nullptr) {
    std::string filename;

    if (!gtk_init_check(nullptr, nullptr))
        return filename;

    GtkFileChooserAction action = folder
        ? GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER
        : GTK_FILE_CHOOSER_ACTION_OPEN;

    const char* accept_label = folder ? "_Select" : "_Open";

    GtkWidget* dialog = gtk_file_chooser_dialog_new(
        folder ? "Select Folder" : "Select File",
        parent,
        action,
        "_Cancel", GTK_RESPONSE_CANCEL,
        accept_label, GTK_RESPONSE_ACCEPT,
        nullptr);

    if (!dialog)
        return filename;

    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
        char* path = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
        if (path) {
            filename = path;
            g_free(path);
        }
    }

    gtk_widget_destroy(dialog);

    // Needed if you're not already running gtk_main()
    while (gtk_events_pending())
        gtk_main_iteration();

    return filename;
}

// TODO: bad memory churn
template<typename T>
void parse(const std::vector<std::string>& lines,
           const std::string& name,
           T* ptr,
           ConfigSettings*)
{
    for (const auto& line_raw : lines) {
        std::string line = trim(line_raw);

        if (line.empty() || line[0] == '#')
            continue;

        auto pos = line.find('=');
        if (pos == std::string::npos)
            continue;

        std::string key = trim(line.substr(0, pos));
        if (key != name)
            continue;

        std::string value = trim(line.substr(pos + 1));

        try {
            if constexpr (std::is_same_v<T, float>) {
                *ptr = std::stof(value);
            }
            else if constexpr (std::is_same_v<T, std::string>) {
                *ptr = value;
            }
            else if constexpr (std::is_same_v<T, bool>) {
                std::string lower_value = value;
                std::transform(lower_value.begin(), lower_value.end(), lower_value.begin(), ::tolower);
                *ptr = (lower_value == "true" || lower_value == "1" || lower_value == "yes");
            }
            else if constexpr (std::is_integral_v<T>) {
                *ptr = static_cast<T>(std::stoll(value));
            }
            else {
                static_assert(sizeof(T) == 0, "Unsupported config type");
            }
        }
        catch (...) {
            // silently ignore any parsing errors, ptr remains unchanged
        }

        return; // stop after first matching key
    }
}

void settings::load_save_settings(bool save, ConfigSettings* settings) {
    const char* home = std::getenv("HOME");
    if (!home) return;

#ifdef NDEBUG
    std::filesystem::path filepath =
        std::filesystem::path(home) / ".config/mylar/mylar_settings.txt";
#else
    std::filesystem::path filepath =
        std::filesystem::path(home) / ".config/mylar/debug_mylar_settings.txt";
#endif

    std::filesystem::create_directories(filepath.parent_path());

    std::ofstream out;
    std::vector<std::string> lines;

    int file_version = 1;

    if (save) {
        out.open(filepath, std::ios::trunc);
        if (!out) return;
        out << "#version 1\n\n";
    } else {
        std::ifstream in(filepath);
        if (!in) return;

        std::string line;
        while (std::getline(in, line)) {
            lines.push_back(line);
        }

        for (const std::string& line : lines) {
            size_t i = 0;
            while (i < line.size() && std::isspace((unsigned char)line[i]))
                ++i;
            const char* tag = "#version";
            size_t tag_len = 8;
            if (line.compare(i, tag_len, tag) != 0)
                continue;
            i += tag_len;
            while (i < line.size() &&
                   (std::isspace((unsigned char)line[i]) || line[i] == '='))
                ++i;
            if (i < line.size())
                file_version = std::strtol(line.c_str() + i, nullptr, 10);
            break;
        }
    }

    if (!save && settings->version != file_version) {
        // ...
    }

    #define bind(type, name, ptr) \
    do { \
        if (save) { \
            out << name << " = " << *ptr << "\n"; \
        } else { \
            parse<type>(lines, name, ptr, settings); \
        } \
    } while(0)
        
    bind(std::string, "touchpad_acceleration_curve", &settings->touchpad_acceleration_curve);
    bind(std::string, "primary_mouse_button", &settings->primary_mouse_button);
    bind(float, "cursor_speed", &settings->cursor_speed);
    bind(bool, "natural_scrolling_mouse", &settings->natural_scrolling_mouse);
    bind(bool, "natural_scrolling_touchpad", &settings->natural_scrolling_touchpad);
    bind(bool, "touchpad_disable_while_typing", &settings->touchpad_disable_while_typing);
    bind(int, "repeat_delay", &settings->repeat_delay);
    bind(int, "repeat_rate", &settings->repeat_rate);
    bind(bool, "show_docks", &settings->show_docks);
    bind(bool, "draw_wallpaper", &settings->draw_wallpaper);
    bind(bool, "hotcorners", &settings->hotcorners);
    bind(bool, "desktop_icons", &settings->desktop_icons);
    bind(std::string, "desktop_folder", &settings->desktop_folder);
    bind(std::string, "overview_layout_type", &settings->overview_layout_type);
    
    #undef bind
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

static Bounds draw_text(cairo_t *cr, int x, int y, std::string text, int size, bool draw, std::string font, int wrap, int h, RGBA color, bool bold, int align = 0) {
    auto layout = get_cached_pango_font(cr, mylar_font, size, bold ? PANGO_WEIGHT_BOLD : PANGO_WEIGHT_NORMAL, false);
    
    //pango_layout_set_text(layout, "\uE7E7", strlen("\uE83F"));
    pango_layout_set_text(layout, text.data(), text.size());
    pango_layout_set_alignment(layout, (PangoAlignment) align);
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

    auto b = draw_text(cr, 0, 0, text, size, false, mylar_font, -1, 0, {0, 0, 0, 1}, false);
    draw_text(cr, 
        c->real_bounds.x + 12 * dpi, 
        c->real_bounds.y + c->real_bounds.h * .5 - b.h * .5, text, size, true, mylar_font, -1, 0, {0, 0, 0, 1},  false);
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
    c->receive_events_even_if_obstructed = true;
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
        left->real_bounds.w = space - optionrighttpad * dpi;
        left->pre_layout(root, left, left->real_bounds);

        float tallest = right->real_bounds.h;
        if (tallest < left->real_bounds.h)
            tallest = left->real_bounds.h;
        if (tallest < 55 * dpi)
            tallest = 55 * dpi;
        
        auto bcopy = b;
        bcopy.h = tallest;

        layout(root, right, right->real_bounds);
        modify_all(right, -optionrighttpad * dpi, 0);
        
        modify_all(right, 0, tallest * .5 - right->real_bounds.h * .5);
        modify_all(left, 0, tallest * .5 - left->real_bounds.h * .5);
        
        layout(root, left, left->real_bounds);

        c->real_bounds = bcopy;
    };
    static const float border_rounding = 5;
    c->when_paint = paint {
        auto mylar = (MylarWindow*)root->user_data;
        auto cr = mylar->raw_window->cr;
        auto dpi = mylar->raw_window->dpi;
        auto b = c->real_bounds;

        RGBA bg_color;
        if (c->state.mouse_hovering || c->state.mouse_pressing) {
            bg_color = RGBA(.965, .965, .965, 1);
            drawRoundedRect(cr, b.x, b.y, b.w, b.h, border_rounding * dpi, 1.0);
            set_argb(cr, bg_color);
            cairo_fill(cr);
        } else {
            //draw_round_rect(client, ArgbColor(.984, .984, .984, 1), c->real_bounds, 5 * config->dpi, 0);
            bg_color = RGBA(.984, .988, .992, 1);
            drawRoundedRect(cr, b.x, b.y, b.w, b.h, border_rounding * dpi, 1.0);
            set_argb(cr, bg_color);
            cairo_fill(cr);
        }

        drawRoundedRect(cr, b.x, b.y, b.w, b.h, border_rounding * dpi, 1.0);
        set_argb(cr, RGBA(.818, .818, .818, 1));
        cairo_stroke(cr);
    };

    return c;
}

static void make_label_like(Container *parent, std::string title, std::string description, std::string icon = "") {
    auto left = parent->child(FILL_SPACE, FILL_SPACE);
    //static float button_text_pad = 8; 
    left->when_paint = [title, description, icon](Container *root, Container *c) {
        auto mylar = (MylarWindow*)root->user_data;
        auto cr = mylar->raw_window->cr;
        auto dpi = mylar->raw_window->dpi;
        auto size_ico = 16 * dpi;
        auto size_title = 12 * dpi;
        auto size_desc = 11 * dpi;

        auto b = c->real_bounds;
        float yoff = optiontopbottompad * dpi;
        float xoff = 0;
        if (!icon.empty()) {
            auto bo = draw_text(cr, 0, 0, icon, size_ico, false, icon_font, -1, -1, {0, 0, 0, .5}, false);
            draw_text(cr, c->real_bounds.x + optionleftpad * dpi, center_y(c, bo.h), icon, size_ico, true, icon_font, -1, -1, {0, 0, 0, 1}, false);
            xoff += bo.w + optionleftpad * dpi;
        }
        {
            auto bo = draw_text(cr, 0, 0, title, size_title, false, mylar_font, c->real_bounds.w - ((optionleftpad + optionrighttpad) * dpi), -1, {0, 0, 0, .5}, false);
            if (description.empty()) {
                draw_text(cr,
                    c->real_bounds.x + optionleftpad * dpi + xoff, 
                    c->real_bounds.y + c->real_bounds.h * .5 - bo.h * .5, title, size_title, true, mylar_font, c->real_bounds.w - ((optionleftpad + optionrighttpad) * dpi), -1, {0, 0, 0, 1}, false);
            } else {
                draw_text(cr,
                    c->real_bounds.x + optionleftpad * dpi + xoff, 
                    c->real_bounds.y + yoff, title, size_title, true, mylar_font, c->real_bounds.w - ((optionleftpad + optionrighttpad) * dpi), -1, {0, 0, 0, 1}, false);
            }

            yoff += bo.h;
        }
        if (!description.empty()) {
            auto bo = draw_text(cr, 0, 0, description, size_desc, false, mylar_font, c->real_bounds.w - ((optionleftpad + optionrighttpad) * dpi), -1, {0, 0, 0, 1}, false);
            draw_text(cr,
                c->real_bounds.x + optionleftpad * dpi + xoff, 
                c->real_bounds.y + yoff, description, size_desc, true, mylar_font, c->real_bounds.w - ((optionleftpad + optionrighttpad) * dpi), -1, {0, 0, 0, .5}, false);
        }
    };
    left->pre_layout = [title, description, icon](Container *root, Container *c, const Bounds &b) {
        auto mylar = (MylarWindow*)root->user_data;
        auto cr = mylar->raw_window->cr;
        auto dpi = mylar->raw_window->dpi;
        auto size_title = 12 * dpi;
        auto size_desc = 11 * dpi;
 
        auto bo1 = draw_text(cr, 0, 0, title, size_title, false, mylar_font, b.w - ((optionleftpad + optionrighttpad) * dpi), -1, {0, 0, 0, 1}, false);
        if (description.empty()) {
            c->real_bounds.h = bo1.h + optiontopbottompad * dpi * 2;
        } else {
            auto bo2 = draw_text(cr, 0, 0, description, size_desc, false, mylar_font, b.w - ((optionleftpad + optionrighttpad) * dpi), -1, {0, 0, 0, 1}, false);
            c->real_bounds.h = bo1.h + bo2.h + optiontopbottompad * dpi * 2;
        }

        if (c->real_bounds.h < 70 * dpi)
            c->real_bounds.h = 70 * dpi;
    };
}

static Container *make_section_title(Container *parent, std::string title) {
    auto section_title = parent->child(FILL_SPACE, FILL_SPACE);
    section_title->pre_layout = [title](Container *root, Container *c, const Bounds &b) {
        auto mylar = (MylarWindow*)root->user_data;
        auto cr = mylar->raw_window->cr;
        auto dpi = mylar->raw_window->dpi;
        auto size_title = 12 * dpi;
        auto bo = draw_text(cr, 0, 0, title, size_title, false, mylar_font, b.w, -1, {0, 0, 0, 1}, true);
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
            c->real_bounds.y, title, size_title, true, mylar_font, c->real_bounds.w, -1, {0, 0, 0, 1}, true);
    };
    return section_title;
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
        
        float data_openess = 0.0; // hovering
        float data_slide_amount = 0.0; // on off left or right toggle side
        float data_slide_open = 0.0;
        float data_open_amount = 0.0;
        if (c->state.mouse_hovering)
            data_openess = 1.0;
        if (data->on) {
            data_slide_amount = 1.0;
            //data_slide_open = 1.0;
            //data_open_amount = 1.0;
        }
        
        float circ_size_small = 12;
        float circ_size_big = 14;
        float circ_anim_time = 70;
        float circ_size = circ_size_small + (circ_size_big - circ_size_small) * data_openess;
        
        float circ_extra_width = 5.5 * data_slide_open;

        auto border = b;

        // Border
        if (data->on) {
            set_rect(cr, border);
            drawRoundedRect(cr, border.x, border.y, border.w, border.h, 10 * dpi, std::floor(1.0 * dpi));
            set_argb(cr, accent);
            cairo_fill(cr);
        } else {
            set_rect(cr, border);
            drawRoundedRect(cr, border.x, border.y, border.w, border.h, 10 * dpi, std::floor(1.0 * dpi));
            set_argb(cr, RGBA(.537, .537, .537, 1));
            cairo_stroke(cr);
        }

        float left = b.x + 4 * dpi - ((circ_size - circ_size_small) * .5) * dpi;
        float right = b.x + b.w - circ_size * dpi + ((circ_size - circ_size_small) * .5) * dpi - 4 * dpi - circ_extra_width * data_openess;
        float position = (right - left) * data_slide_amount + left;
      
        b.x = position;
        b.y = b.y + b.h / 2 - (circ_size * .5) * dpi;
        b.w = circ_size * dpi + circ_extra_width * data_open_amount;
        b.h = circ_size * dpi;
        // Dot
        if (data->on) {
            set_rect(cr, b);
            drawRoundedRect(cr, b.x, b.y, b.w, b.h, circ_size * .5 * dpi, 1.0);
            set_argb(cr, RGBA(1, 1, 1, 1));
            cairo_fill(cr);
        } else {
            set_rect(cr, b);
            drawRoundedRect(cr, b.x, b.y, b.w, b.h, circ_size * .5 * dpi, 1.0);
            set_argb(cr, RGBA(.365, .365, .365, 1));
            cairo_fill(cr);
        }

        // On/Off text
        std::string text = data->on ? "On" : "Off";
        auto tb = draw_text(cr, 0, 0, text, 11 * dpi, false, mylar_font, -1, -1, RGBA(0, 0, 0, 1), false, 0);
        draw_text(cr, 
            c->real_bounds.x - tb.w - 12 * dpi, 
            c->real_bounds.y + c->real_bounds.h * .5 - tb.h * .5, text, 11 * dpi, 
            true, mylar_font, -1, -1, RGBA(0, 0, 0, 1), false, 0);
    };
    right->pre_layout = [](Container *root, Container *c, const Bounds &b) {
        auto mylar = (MylarWindow*)root->user_data;
        auto cr = mylar->raw_window->cr;
        auto dpi = mylar->raw_window->dpi;
        c->real_bounds.w = 40 * dpi;
        c->real_bounds.h = 20 * dpi;
    };
    right->when_clicked = [on_change](Container *root, Container *c) {
        auto data = (BoolInfo *) c->user_data;
        data->on = !data->on;
        if (on_change)
            on_change(data->on);
    };
}

static void make_slider(Container *parent, std::string title, std::string description, float initial_value, std::function<void(float)> on_change, int notches = 0) {
    auto p = make_self_height_sized_parent(parent);
    
    make_label_like(p, title, description);

    struct SliderInfo : UserData {
        float value = .5;
    };

    auto right = p->child(::hbox, FILL_SPACE, FILL_SPACE);
    auto slider_info = new SliderInfo;
    if (notches != 0) {
        float snap_percentage = 1.0f / ((float) notches);
        initial_value = std::round(initial_value / snap_percentage) * snap_percentage;
        initial_value = std::clamp(initial_value, 0.0f, 1.0f);
    }
    slider_info->value = initial_value;
    right->user_data = slider_info;
    right->when_paint = [notches](Container *root, Container *c) {
        auto mylar = (MylarWindow*)root->user_data;
        auto cr = mylar->raw_window->cr;
        auto dpi = mylar->raw_window->dpi;

        {
            auto b = c->real_bounds;
            float h = 8.5 * dpi;
            b.y += b.h * .5 - h * .5;
            b.h = h;

            drawRoundedRect(cr, b.x - std::round(5 * dpi), b.y, b.w + std::round(10 * dpi), b.h, h * .5, 1.0);
            set_argb(cr, slider_bg);
            cairo_fill(cr); 

            if (notches != 0) {
                float snap_percentage = 1.0f / ((float) notches);
                int count = std::round(1.0f / snap_percentage);
                float spacing = b.w * snap_percentage;
                float x_off = 0.0;
                for (int i = 0; i < count + 1; i++) {
                    set_rect(cr, {b.x + x_off - std::round(1 * dpi), b.y + std::round(10 * dpi), std::round(2 * dpi), std::round(6 * dpi)});
                    set_argb(cr, slider_bg);
                    cairo_fill(cr); 

                    x_off += spacing;
                }
            }
        }
        
        {
            auto data = (SliderInfo *) c->user_data;
            auto b = c->real_bounds;
            b.w = b.h;
            b.x += c->real_bounds.w * data->value - b.h * .5;
            drawRoundedRect(cr, b.x, b.y, b.w, b.h, b.h * .5, 1.0);
            set_argb(cr, {1, 1, 1, 1});
            cairo_fill(cr);

            //b.shrink(1.0);
            
            drawRoundedRect(cr, b.x, b.y, b.w, b.h, b.h * .5, 1.0);
            set_argb(cr, slider_bg);
            cairo_stroke(cr);

            b.shrink(5 * dpi);
            drawRoundedRect(cr, b.x, b.y, b.w, b.h, b.h * .5, 1.0);
            set_argb(cr, accent);
            cairo_fill(cr);
        }

    };
    right->when_mouse_down = [on_change, notches](Container *root, Container *c) {
        auto mylar = (MylarWindow*)root->user_data;
        auto cr = mylar->raw_window->cr;
        auto dpi = mylar->raw_window->dpi;
        auto b = c->real_bounds;
        float scalar = (root->mouse_current_x - b.x) / b.w;
        if (scalar < 0)
            scalar = 0;
        if (scalar > 1)
            scalar = 1;
        if (notches != 0) {
            float snap_percentage = 1.0f / ((float) notches);
            scalar = std::round(scalar / snap_percentage) * snap_percentage;
            scalar = std::clamp(scalar, 0.0f, 1.0f);
        }
 
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
            auto bo1 = draw_text(cr, 0, 0, o, size, false, mylar_font, -1, -1, {0, 0, 0, 1}, false);
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

            auto bo = draw_text(cr, 0, 0, o, size, false, mylar_font, -1, -1, {0, 0, 0, .5}, false);
            draw_text(cr,
                c->real_bounds.x + c->real_bounds.w * .5 - bo.w * .5, 
                c->real_bounds.y + c->real_bounds.h * .5 - bo.h * .5, o, size, true, mylar_font, -1, -1, {0, 0, 0, 1}, false);
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

static Container *make_vert_space(Container *parent, float amount) {
    auto pad = parent->child(FILL_SPACE, 8);
    pad->pre_layout = [amount](Container *root, Container *c, const Bounds &b) {
        auto mylar = (MylarWindow*)root->user_data;
        auto cr = mylar->raw_window->cr;
        auto dpi = mylar->raw_window->dpi;
        c->wanted_bounds.h = amount * dpi;
    };
    return pad;
}

static void fill_dock_settings(Container *root, Container *c) {
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
        c->type = ::vbox;
        layout(root, c, b);
        c->type = ::fullycustom;
        auto d = (RightData *) c->user_data;
        float overflow = -actual_true_height(c);
        d->scroll = std::min(std::max(overflow, d->scroll), 0.0f);

        for (auto child : c->children) {
            modify_all(child, 0, d->scroll);
        }
    };
    auto padded_right = right->child(FILL_SPACE, FILL_SPACE);

    make_section_title(padded_right, "Dock Settings");
    
    make_vert_space(padded_right, 10);
    
    make_bool(padded_right, "Show docks", "", set->show_docks, [](bool c) {
        set->show_docks = c;

        if (set->show_docks) {
            dock::start();
        } else {
            dock::stop();
        }
    });
}

static RawWindowSettings make_icon_anchored_popup_settings(Container *icon,
                                                           float dpi,
                                                           int popup_w,
                                                           int popup_h) {
    const int icon_x = (int) std::round(icon->real_bounds.x / dpi);
    const int icon_y = (int) std::round(icon->real_bounds.y / dpi);
    const int icon_w = std::max(1, (int) std::round(icon->real_bounds.w / dpi));
    const int icon_h = std::max(1, (int) std::round(icon->real_bounds.h / dpi));

    RawWindowSettings settings;
    settings.pos.w = popup_w;
    settings.pos.h = popup_h;
    settings.name = "Popup";
    settings.popup.use_explicit_anchor_rect = true;
    settings.popup.anchor_rect_x = icon_x;
    settings.popup.anchor_rect_y = icon_y;
    settings.popup.anchor_rect_w = icon_w;
    settings.popup.anchor_rect_h = icon_h;
    settings.popup.anchor = RawWindowSettings::PopupAnchor::BOTTOM;
    settings.popup.gravity = RawWindowSettings::PopupGravity::BOTTOM;
    settings.popup.use_offset = true;
    settings.popup.offset_y = -8;
    settings.popup.constraint_adjustment =
        RawWindowSettings::POPUP_CONSTRAINT_SLIDE_X |
        RawWindowSettings::POPUP_CONSTRAINT_SLIDE_Y |
        RawWindowSettings::POPUP_CONSTRAINT_FLIP_X |
        RawWindowSettings::POPUP_CONSTRAINT_FLIP_Y;
    return settings;
}

static void make_dropdown(Container *parent, std::string text_, std::vector<std::string> options, std::function<void(std::string)> func) {
    static float pad_amount = 11;
    static float vron_pad_amount = pad_amount * 1.7;
    static float text_height = 9;
    static float option_height = 11;
    static float chevron_height = 10;
    static std::string chevron = "\uE70D";
    auto pad = parent->child(FILL_SPACE, FILL_SPACE);
    struct TextData : UserData {
        std::string text;
    };
    auto text_data = new TextData;
    text_data->text = text_;
    pad->user_data = text_data;
    pad->pre_layout = [](Container *root, Container *c, const Bounds &b) {
        auto mylar = (MylarWindow*)root->user_data;
        auto cr = mylar->raw_window->cr;
        auto dpi = mylar->raw_window->dpi;
        auto td = (TextData *) c->user_data;
        text_height = 11 * dpi;
        pad_amount = 11 * dpi;
        Bounds bounds = draw_text(cr, chevron_height, 0, td->text, text_height, false, mylar_font, -1, -1, {1, 1, 1, 1}, true);
        Bounds vron = draw_text(cr, 0, 0, chevron, chevron_height, false, icon_font, -1, -1, {1, 1, 1, 1}, true);
        
        c->wanted_bounds.w = bounds.w + pad_amount * 2 + vron_pad_amount + vron.w;
        c->wanted_bounds.h = bounds.h + (pad_amount * 2 * .8);
    };
    pad->when_paint = [](Container *root, Container *c) {
        auto mylar = (MylarWindow*)root->user_data;
        auto cr = mylar->raw_window->cr;
        auto dpi = mylar->raw_window->dpi;
        auto td = (TextData *) c->user_data;
        if (c->state.mouse_pressing) {
            set_argb(cr, {.7, .7, .7, 1});
        } else if (c->state.mouse_hovering) {
            set_argb(cr, {.9, .9, .9, 1});
        } else {
            set_argb(cr, {1, 1, 1, 1});
        }
        auto b = c->real_bounds;
        drawRoundedRect(cr, b.x, b.y, b.w, b.h, dpi * 6, 1.0); 
        cairo_fill(cr);
        set_argb(cr, {.7, .7, .7, 1});
        drawRoundedRect(cr, b.x, b.y, b.w, b.h, dpi * 6, 1.0); 
        cairo_stroke(cr);
        
        Bounds bounds = draw_text(cr, 0, 0, td->text, text_height, false, mylar_font, -1, -1, {1, 1, 1, 1}, false);
        draw_text(cr, 
            c->real_bounds.x + pad_amount, 
            c->real_bounds.y + c->real_bounds.h * .5 - bounds.h * .5, 
            td->text, text_height, true, mylar_font, -1, -1, {0, 0, 0, 1}, false);

        auto vron_bounds = draw_text(cr, 0, 0, chevron, chevron_height, false, icon_font, -1, -1, {1, 1, 1, 1}, false);
        draw_text(cr, 
            c->real_bounds.x - pad_amount - vron_bounds.w + c->real_bounds.w, 
            c->real_bounds.y + c->real_bounds.h * .5 - vron_bounds.h * .5, 
             chevron, chevron_height, true, icon_font, -1, -1, {0, 0, 0, 1}, false);
    };
    static MylarWindow *popup = nullptr; 
    popup = nullptr;
    pad->when_clicked = [func, options](Container *root, Container *c) {
        auto dock = (MylarWindow*)root->user_data;
        auto cr = dock->raw_window->cr;
        auto dpi = dock->raw_window->dpi;
        auto td = (TextData *) c->user_data;
        
        RawWindowSettings settings = make_icon_anchored_popup_settings(
            c, dpi, 300, (std::max(1, (int) options.size()) * 25) * dpi);

        popup = open_mylar_popup(dock, settings);
        if (!popup)
            return;
        popups.push_back(popup);
        popup->root->when_paint = [](Container *root, Container *c) {
            auto popup = (MylarWindow *) root->user_data;
            auto cr = popup->raw_window->cr;
            set_argb(cr, {1, 1, 1, 1});
            drawRoundedRect(cr, c->real_bounds.x, c->real_bounds.y, c->real_bounds.w, c->real_bounds.h, 10 * popup->raw_window->dpi, 1.0);
            cairo_fill(cr);
        };
        for (auto m : options) {
            auto ch = popup->root->child(FILL_SPACE, FILL_SPACE);
            ch->when_paint = [m](Container *root, Container *c) {
                auto popup = (MylarWindow *) root->user_data;
                auto cr = popup->raw_window->cr;
                auto dpi = popup->raw_window->dpi;
                
                if (c->state.mouse_pressing) {
                    set_argb(cr, {.8, .8, .8, 1});
                    drawRoundedRect(cr, c->real_bounds.x, c->real_bounds.y, c->real_bounds.w, c->real_bounds.h, 10 * popup->raw_window->dpi, 1.0);
                } else if (c->state.mouse_hovering) {
                    set_argb(cr, {.9, .9, .9, 1});
                    drawRoundedRect(cr, c->real_bounds.x, c->real_bounds.y, c->real_bounds.w, c->real_bounds.h, 10 * popup->raw_window->dpi, 1.0);
                } else {
                    set_argb(cr, {1, 1, 1, 1});
                    drawRoundedRect(cr, c->real_bounds.x, c->real_bounds.y, c->real_bounds.w, c->real_bounds.h, 10 * popup->raw_window->dpi, 1.0);
                }
                cairo_fill(cr);
                
                Bounds b = draw_text(cr, 0, 0, m, option_height * dpi, false, mylar_font, -1, -1, RGBA(0, 0, 0, 1), false);
                draw_text(cr, 5 * dpi, center_y(c, b.h), m, option_height * dpi, true, mylar_font, -1, -1, RGBA(0, 0, 0, 1), false);
            };
            ch->when_clicked = [td, m, func](Container *root, Container *c) {
                if (func)
                    func(m);
                if (!popup)
                    return;
                td->text = m;
                windowing::timer(settings_app, 1, [](void *data) {
                    auto rw = (RawWindow *) data;
                    windowing::close_window(rw);
                    popup = nullptr;
                }, popup->raw_window);
            };
        }    
        popup->root->skip_delete = true;
        popup->root->user_data = popup;
        popup->root->type = ::vbox;
        popup->root->wanted_bounds.w = FILL_SPACE;
        popup->root->wanted_bounds.h = FILL_SPACE;
        windowing::redraw(popup->raw_window);
    };
    pad->when_key_event = [](Container *root, Container* c, int key, bool pressed, xkb_keysym_t sym, int mods, bool is_text, std::string text) {
        if (!pressed || !popup)
            return;
        if (sym == XKB_KEY_Escape) {
            windowing::timer(settings_app, 1, [](void *data) {
                auto rw = (RawWindow *) data;
                windowing::close_window(rw);
                popup = nullptr;
            }, popup->raw_window); 
        }
    };
   
}

static Container *make_dropdown_option(Container *parent, std::string title, std::string description, std::string icon, std::string selected, std::vector<std::string> options, std::function<void(std::string)> on_change) {
    assert(on_change);
    auto p = make_self_height_sized_parent(parent);

    make_label_like(p, title, description, icon);

    auto right = p->child(::hbox, FILL_SPACE, FILL_SPACE);
    right->alignment = container_alignment::ALIGN_CENTER;
    
    right->pre_layout = [](Container *root, Container *c, const Bounds &b) {
        auto mylar = (MylarWindow*)root->user_data;
        auto cr = mylar->raw_window->cr;
        auto dpi = mylar->raw_window->dpi;
        c->real_bounds.w = 250 * dpi;
        c->real_bounds.h = 70 * dpi;
        c->spacing = 5 * dpi;
    };
    right->child(FILL_SPACE, FILL_SPACE);
    right->parent_bounds_limit_input_bounds = false;
    
    make_dropdown(right, selected, options, [on_change](std::string selected) {
        on_change(selected);
    });
    
    return p;
}

static void make_button(Container *parent, std::string text, std::function<void()> func) {
    static float pad_amount = 11;
    static float text_height = 11;
    auto pad = parent->child(FILL_SPACE, FILL_SPACE);
    pad->pre_layout = [text](Container *root, Container *c, const Bounds &b) {
        auto mylar = (MylarWindow*)root->user_data;
        auto cr = mylar->raw_window->cr;
        auto dpi = mylar->raw_window->dpi;
        text_height = 11 * dpi;
        pad_amount = 11 * dpi;
        Bounds bounds = draw_text(cr, 0, 0, text, text_height, false, mylar_font, -1, -1, {1, 1, 1, 1}, true);
        c->wanted_bounds.w = bounds.w + pad_amount * 2;
        c->wanted_bounds.h = bounds.h + (pad_amount * 2 * .8);
    };
    pad->when_paint = [text](Container *root, Container *c) {
        auto mylar = (MylarWindow*)root->user_data;
        auto cr = mylar->raw_window->cr;
        auto dpi = mylar->raw_window->dpi;
        if (c->state.mouse_pressing) {
            set_argb(cr, {.5, .5, .5, 1});
        } else if (c->state.mouse_hovering) {
            set_argb(cr, {.65, .65, .65, 1});
        } else {
            set_argb(cr, {.8, .8, .8, 1});
        }
        set_rect(cr, c->real_bounds);
        cairo_fill(cr);
        Bounds bounds = draw_text(cr, 0, 0, text, text_height, false, mylar_font, -1, -1, {1, 1, 1, 1}, false);
        draw_text(cr, 
            c->real_bounds.x + c->real_bounds.w * .5 - bounds.w * .5, 
            c->real_bounds.y + c->real_bounds.h * .5 - bounds.h * .5, 
            text, text_height, true, mylar_font, -1, -1, {0, 0, 0, 1}, false);
    };
   pad->when_clicked = [func](Container *root, Container *c) {
       if (func) {
           func();
       }
   };
}

struct Field : UserData {
    std::string text;
};

static bool is_digits(const std::string& s) {
    return !s.empty() && std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isdigit(c); });
}

static Container *make_field(Container *parent, bool only_numbers, std::string initial_value, std::function<void (std::string)> on_change) {
    static float pad_amount = 11;
    static float text_height = 11;
    auto pad = parent->child(FILL_SPACE, FILL_SPACE);
    auto field = new Field;
    field->text = initial_value;
    pad->user_data = field;
    pad->pre_layout = [](Container *root, Container *c, const Bounds &b) {
        auto mylar = (MylarWindow*)root->user_data;
        auto cr = mylar->raw_window->cr;
        auto dpi = mylar->raw_window->dpi;
        text_height = 11 * dpi;
        pad_amount = 11 * dpi;
        Bounds bounds = draw_text(cr, 0, 0, "W", text_height, false, mylar_font, -1, -1, {1, 1, 1, 1}, true);
        c->wanted_bounds.w = FILL_SPACE;
        c->wanted_bounds.h = bounds.h + (pad_amount * 2 * .8);
    };
    pad->when_key_event = [only_numbers, on_change](Container *root, Container* c, int key, bool pressed, xkb_keysym_t sym, int mods, bool is_text, std::string text) {
        if (!c->active && !c->parent->active)
            return;
        if (!pressed)
            return;
        auto field = (Field *) c->user_data;
        auto start = field->text;
        defer(if (start != field->text) { on_change(field->text); });
        
        if (sym == XKB_KEY_BackSpace && !field->text.empty()) {
            field->text.pop_back();
        }
        
        if (is_text) {
            if (only_numbers && !is_digits(text))
                return;
            field->text += text;
        }
    };
  
    pad->when_paint = [](Container *root, Container *c) {
        auto mylar = (MylarWindow*)root->user_data;
        auto cr = mylar->raw_window->cr;
        auto dpi = mylar->raw_window->dpi;
        auto field = (Field *) c->user_data;

        set_argb(cr, c->active ? accent : RGBA(.8, .8, .8, 1));
        set_rect(cr, c->real_bounds);
        cairo_fill(cr);

        set_argb(cr, {1, 1, 1, 1});
        auto minus_border = c->real_bounds;
        minus_border.shrink(std::round(1 * dpi));
        set_rect(cr, minus_border);
        cairo_fill(cr);
        
        Bounds bounds = draw_text(cr, 0, 0, field->text, text_height, false, mylar_font, -1, -1, {1, 1, 1, 1}, false);

        float over = ((c->real_bounds.h - bounds.h) * .5);
        
        if (c->active) {
            set_argb(cr, {0, 0, 0, 1});
            auto cursor_width = std::round(1.0 * dpi);
            auto cursor_bounds = Bounds(c->real_bounds.x + c->real_bounds.w - cursor_width - over, 
                c->real_bounds.y + c->real_bounds.h * .5 - bounds.h * .5, 
                cursor_width, bounds.h);
            set_rect(cr, cursor_bounds);
            cairo_fill(cr);
        }
        
        draw_text(cr, 
            c->real_bounds.x + c->real_bounds.w - bounds.w - over, 
            c->real_bounds.y + c->real_bounds.h * .5 - bounds.h * .5, 
            field->text, text_height, true, mylar_font, -1, -1, {0, 0, 0, 1}, false);
    };
    return pad;
}

struct MonitorOption {
    std::string name;
    std::vector<std::string> option;
    float w = 1920;
    float h = 1080;
    float fps = 60;
    int x = 0; 
    int y = 0; 
    float scale = 1.0;
    int transform = 0;

    std::string wanted_mode_line;
    float wanted_scale;
    int wanted_transform = 0;
};

static void make_screen_positioner(Container *parent, std::vector<MonitorOption *> &options, std::function<void (std::string)> on_clicked) {
    static int height = 300;
    auto c = parent->child(::fullycustom, FILL_SPACE, height);
    struct MonitorInfo : UserData {
        int base_offset_x = 0;
        int base_offset_y = 0;
        int mouse_offset_x = 0;
        int mouse_offset_y = 0;
        MonitorOption *o = nullptr;
        bool selected = false;
    };
    static float scale_down = .1;
    c->pre_layout = [](Container *root, Container *c, const Bounds &b) {
        auto mylar = (MylarWindow*)root->user_data;
        auto dpi = mylar->raw_window->dpi;
        c->wanted_bounds.h = height * dpi;

        if (c->children.empty())
            return;

        auto main = c->children[0];

        int min_x = INT_MAX;
        int min_y = INT_MAX;
        int max_x = 0;
        int max_y = 0;
        for (auto ch : c->children) {
            auto data = (MonitorInfo *) ch->user_data;
            if (data->o->x < min_x)
                min_x = data->o->x;
             if (data->o->y < min_y)
                min_y = data->o->y;
             if (data->o->x + data->o->w > max_x)
                max_x = data->o->x + data->o->w;
             if (data->o->y + data->o->h > max_y)
                max_y = data->o->y + data->o->h;
        }
        float sizew = (float) (max_x - min_x) * scale_down;
        float sizeh = (float) (max_y - min_y) * scale_down;
        
        for (auto ch : c->children) {
            auto data = (MonitorInfo *) ch->user_data;
            float offsetx = data->base_offset_x + data->mouse_offset_x;
            float offsety = data->base_offset_y + data->mouse_offset_y;
            ch->real_bounds = Bounds(c->real_bounds.x + c->real_bounds.w * .5 + offsetx - sizew * .5,
                                       c->real_bounds.y + c->real_bounds.h * .5 + offsety - sizeh * .5,
                                       ch->wanted_bounds.w, ch->wanted_bounds.h);
            ch->wanted_bounds = ch->real_bounds;
            layout(root, ch, ch->real_bounds);
        }
    };
    c->when_paint = paint {
        auto mylar = (MylarWindow*)root->user_data;
        auto cr = mylar->raw_window->cr;
        auto dpi = mylar->raw_window->dpi;
        set_argb(cr, RGBA(0, 0, 0, .1));
        auto b = c->real_bounds;
        drawRoundedRect(cr, b.x, b.y, b.w, b.h, 10 * dpi, 1.0 * dpi);
        cairo_fill(cr);
    };

    bool first = true;
    for (auto option : options) {
        auto main = c->child(option->w * scale_down, option->h * scale_down);
        main->when_paint = paint {
            auto mylar = (MylarWindow*)root->user_data;
            auto cr = mylar->raw_window->cr;
            auto dpi = mylar->raw_window->dpi;
            auto data = (MonitorInfo *) c->user_data;
            set_argb(cr, RGBA(1, 1, 1, 1));
            auto b = c->real_bounds;
            
            cairo_save(cr);
            set_rect(cr, c->parent->real_bounds);
            cairo_clip(cr);
            
            drawRoundedRect(cr, b.x, b.y, b.w, b.h, 10 * dpi, 1.0 * dpi);
            cairo_fill(cr);

            if (data->selected) {
                set_argb(cr, accent);
                drawRoundedRect(cr, b.x, b.y, b.w, b.h, 10 * dpi, 2.0 * dpi);
                cairo_stroke(cr);
            }

            auto text = fz("{}\n({}x{})", data->o->name, data->o->w, data->o->h);
            auto tb = draw_text(cr, 0, 0, text, 12 * dpi, false, mylar_font, c->real_bounds.w, -1, RGBA(0, 0, 0, 1), false, 1);
            
            draw_text(cr, 
                c->real_bounds.x,
                c->real_bounds.y + c->real_bounds.h * .5 - tb.h * .5, 
                text, 12 * dpi, true, mylar_font, c->real_bounds.w, -1, RGBA(0, 0, 0, 1), false, 1);

            cairo_reset_clip(cr);
            cairo_restore(cr);
        };
        main->when_drag_start = [on_clicked](Container *root, Container *c) {
            c->z_index = 100;
            auto data = (MonitorInfo *) c->user_data;
            for (auto ch : c->parent->children) {
                auto data = (MonitorInfo *) ch->user_data;
                data->selected = false;
            }
            data->selected = true;
            if (on_clicked)
                on_clicked(data->o->name);
        };
        main->when_drag = [](Container *root, Container *c) {
            auto data = (MonitorInfo *) c->user_data;
            data->mouse_offset_x = root->mouse_current_x - root->mouse_initial_x;
            data->mouse_offset_y = root->mouse_current_y - root->mouse_initial_y;
        };
        main->when_drag_end = [](Container *root, Container *c) {
            c->z_index = 0;
            auto data = (MonitorInfo *) c->user_data;
            data->mouse_offset_x = 0;
            data->mouse_offset_y = 0;
        };
        main->when_clicked = main->when_drag_start;
     
        auto monitor_info = new MonitorInfo;
        monitor_info->o = option;
        monitor_info->base_offset_x = monitor_info->o->x * scale_down;
        monitor_info->base_offset_y = monitor_info->o->y * scale_down;
        monitor_info->selected = first;
        first = false;
        
        main->user_data = monitor_info;        
    }
}

static void current_to_wanted(MonitorOption *m) {
    m->wanted_mode_line = fz("{}x{}@{:.2f}", m->w, m->h, m->fps);
    m->wanted_scale = m->scale;
    m->wanted_transform = m->transform;
}

static void fill_display_settings(Container *root);

static void apply_wanted(MonitorOption *m) {
    std::string wanted_scale = std::to_string(m->wanted_scale); 
    if (m->wanted_scale == -1) {
        wanted_scale = "auto";
    }
    std::ostringstream ss;
        ss << "hl.monitor({ output = \"" << m->name << "\", mode = \"" << m->wanted_mode_line << "\", position = \"auto\", "
        << "scale = " << wanted_scale
        << ", transform = " << m->wanted_transform
        << " })";

    auto str = ss.str();
    main_thread([str, m]() {
        MylarMonitor mon;
        mon.name = m->name;
        mon.lua_config = str;

        bool found = false;
        for (auto &hm : set->monitors) {
            if (hm.name == m->name) {
                found = true;
                hm.lua_config = str;
            }
        }
            
        if (!found)
            set->monitors.push_back(mon);
        
        hypriso->reload();
    });
}

static void change_display_options(MonitorOption *m) {
    auto right = container_by_name("settings_right", settings_mylar->root);
    if (!right)
        return;
    assert(!right->children.empty());
    auto padded_right = right->children[0];
    for (int i = padded_right->children.size() - 1; i >= 0; i--) {
        if (padded_right->children[i]->name == "removable") {
            delete padded_right->children[i];
            padded_right->children.erase(padded_right->children.begin() + i);
        }
    }
    
    auto p = make_vert_space(padded_right, 20);
    p->name = "removable";
    
    p = make_section_title(padded_right, fz(" {}", m->name));
    p->name = "removable";
    
    p = make_vert_space(padded_right, 10);
    p->name = "removable";
    
    auto res = fz("{}x{}@{:.2f}Hz", m->w, m->h, m->fps);
    p = make_dropdown_option(padded_right, "Resolution", "Adjust the resolution of your display", "", res, m->option, [m](std::string selected) {
        current_to_wanted(m); 
        
        m->wanted_mode_line = selected;
        m->wanted_mode_line.pop_back();
        m->wanted_mode_line.pop_back();
       
        apply_wanted(m);
    });
    p->name = "removable";
    p = make_vert_space(padded_right, 4);
    p->name = "removable";

    std::vector<std::string> scales = {"Automatic"};
    int count = 10;
    for (int i = 0; i < count; i++) {
        int add = i * count;
        int amount = 100 + add;
        scales.push_back(fz("{}%", amount));
    }
    
    res = fz("{}%", std::round(m->scale * 100));
    p = make_dropdown_option(padded_right, "Scale", "Change the size of text, apps, and other items", "\ue93a", res, scales, [m](std::string selected) {
        current_to_wanted(m); 
        if (selected == "Automatic") {
            m->wanted_scale = -1;
        } else {
            selected.pop_back();
            float f = stof(selected);
            m->wanted_scale = f / 100.0f;
        }
        apply_wanted(m);
    });
    p->name = "removable";
    
    p = make_vert_space(padded_right, 4);
    p->name = "removable";

    std::vector<std::string> orientations= {"Landscape", "Vertical"};
    p = make_dropdown_option(padded_right, "Orientation", "Change the rotation of display", "", "Landscape", orientations, [m](std::string selected) {
        current_to_wanted(m); 
        if (selected == "Landscape") {
            m->wanted_transform = 0;
        } else if (selected == "Vertical") {
            m->wanted_transform = 1;
        }
        apply_wanted(m);
    });
    p->name = "removable";
}

static void fill_display_settings(Container *root) {
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
        c->type = ::vbox;
        layout(root, c, b);
        c->type = ::fullycustom;
        auto d = (RightData *) c->user_data;
        float overflow = -actual_true_height(c);
        d->scroll = std::min(std::max(overflow, d->scroll), 0.0f);

        for (auto child : c->children) {
            modify_all(child, 0, d->scroll);
        }
    };
    auto padded_right = right->child(FILL_SPACE, FILL_SPACE);

    make_section_title(padded_right, "Display Settings");
    
    make_vert_space(padded_right, 10);

    std::vector<MonitorOption *> options;
    auto data = execAndGet(std::string("hyprctl monitors").c_str());
    // no/checkin
    //auto data = execAndGet(std::string("cat /tmp/out").c_str());

    auto p = LineParser(data);
    MonitorOption *option = nullptr;
    while (p.has_another_line()) {
        defer(p.next_line());

        p.eat_blank();
        
        std::string_view s = p.eat_until_line_end();
        
        auto l = LineParser(s);

        try {
            if (s.starts_with("Monitor")) {
                option = new MonitorOption;
                options.push_back(option);
                
                l.eat_until(" ");
                option->name = trim(std::string(*l.eat_until(" ")));
            } else if (s.contains(" at ")) {
                option->w = std::stof(trim(std::string(*l.eat_until("x"))));
                option->h = std::stof(trim(std::string(*l.eat_until("@"))));
                option->fps = std::stof(trim(std::string(*l.eat_until(" "))));
                l.eat_until(" ");
                option->x = std::stoi(trim(std::string(*l.eat_until("x"))));
                option->y = std::stoi(trim(std::string(l.eat_until_line_end())));
            } else if (s.starts_with("scale")) {
                l.eat_until(": ");
                option->scale = std::stof(trim(std::string(l.eat_until_line_end())));
            } else if (s.starts_with("transform")) {
                l.eat_until(": ");
                option->transform = std::stoi(trim(std::string(l.eat_until_line_end())));
            } else if (s.starts_with("availableModes")) {
                l.eat_until(": ");
                while (l.not_end_of_line()) {
                    auto f = l.eat_until(" ");
                    option->option.push_back(std::string(*f));
                }
            }
        } catch (...) {
            main_thread([]() {
                notify("Failed to parse a monitor please report this issue on github!");
            });
            return;
        }
    }

    if (options.size() > 0) {
        make_screen_positioner(padded_right, options, [options](std::string monitor_name) {
            for (auto o : options) {
                if (o->name == monitor_name) {
                    change_display_options(o);
                    return;
                }
            }
        });
        
        make_vert_space(padded_right, 10);
    }
        
    for (auto m : options) {
        change_display_options(m);
        break;
    }

}

static void fill_desktop_settings(Container *root, Container *c) {
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
        c->type = ::vbox;
        layout(root, c, b);
        c->type = ::fullycustom;
        auto d = (RightData *) c->user_data;
        float overflow = -actual_true_height(c);
        d->scroll = std::min(std::max(overflow, d->scroll), 0.0f);

        for (auto child : c->children) {
            modify_all(child, 0, d->scroll);
        }
    };
    auto padded_right = right->child(FILL_SPACE, FILL_SPACE);

    make_section_title(padded_right, "Desktop Settings");
    
    make_vert_space(padded_right, 10);

    make_bool(padded_right, "Desktop Icons", "", set->desktop_icons, [](bool c) {
        set->desktop_icons = c;
        main_thread([]() {
            desktop_icons::stop();
            desktop_icons::start();
            damage_all();
        });
    });
    
    make_vert_space(padded_right, 4);
    
    make_button(padded_right, "Change folder", []() {
        static bool chooser_open = false;
        if (chooser_open)
            return;

        chooser_open = true;

        std::thread t([]() {
            const auto file = trim_newline(choose_file(true));
            main_thread([file]() {
                    notify(file);
            });
            
            if (!file.empty() && std::filesystem::exists(file) && std::filesystem::is_directory(file)) {
                main_thread([file]() {
                    set->desktop_folder = file;
                    desktop_icons::stop();
                    desktop_icons::start();
                    settings::load_save_settings(true, set); // save
                });
            }

            chooser_open = false;
        });

        t.detach();
    });
    
    make_vert_space(padded_right, 14);

    make_dropdown_option(padded_right, "Overview", "Change layout type", "", set->overview_layout_type, {"Grid", "Adaptive"}, [](std::string new_type) {
        set->overview_layout_type = new_type;
    });
}

static void fill_wallpaper_settings(Container *root, Container *c) {
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
        c->type = ::vbox;
        layout(root, c, b);
        c->type = ::fullycustom;
        auto d = (RightData *) c->user_data;
        float overflow = -actual_true_height(c);
        d->scroll = std::min(std::max(overflow, d->scroll), 0.0f);

        for (auto child : c->children) {
            modify_all(child, 0, d->scroll);
        }
    };
    auto padded_right = right->child(FILL_SPACE, FILL_SPACE);

    make_section_title(padded_right, "Wallpaper Settings");
    
    make_vert_space(padded_right, 10);

    make_bool(padded_right, "Draw wallpaper", "", set->draw_wallpaper, [](bool c) {
        set->draw_wallpaper = c;
        damage_all();
    });

    make_vert_space(padded_right, 4); 

    make_button(padded_right, "Choose file", []() {
        static bool chooser_open = false;
        if (chooser_open)
            return;

        chooser_open = true;

        std::thread t([]() {
            const auto file = trim_newline(choose_file());
            if (!file.empty() && std::filesystem::exists(file)) {
                auto mime = trim_newline(execAndGet(std::string("file --mime-type -b \"" + file + "\"").c_str()));

                if (mime.starts_with("image/")) {
                    const char* home = std::getenv("HOME");
                    std::filesystem::path filepath =
                        std::filesystem::path(home) / ".config/mylar/wall.png";
 
                    std::error_code ec;
                    std::filesystem::create_directories(filepath.parent_path(), ec);
                    std::filesystem::copy_file(file, filepath,
                                               std::filesystem::copy_options::overwrite_existing, ec);
                }
            }

            chooser_open = false;
        });

        t.detach();
    }); 
}


/*
        {
            auto b = c->real_bounds;
            float h = 8.5 * dpi;
            b.y += b.h * .5 - h * .5;
            b.h = h;

            drawRoundedRect(cr, b.x - std::round(5 * dpi), b.y, b.w + std::round(10 * dpi), b.h, h * .5, 1.0);
            set_argb(cr, slider_bg);
            cairo_fill(cr); 

            if (notches != 0) {
                float snap_percentage = 1.0f / ((float) notches);
                int count = std::round(1.0f / snap_percentage);
                float spacing = b.w * snap_percentage;
                float x_off = 0.0;
                for (int i = 0; i < count + 1; i++) {
                    set_rect(cr, {b.x + x_off - std::round(1 * dpi), b.y + std::round(10 * dpi), std::round(2 * dpi), std::round(6 * dpi)});
                    set_argb(cr, slider_bg);
                    cairo_fill(cr); 

                    x_off += spacing;
                }
            }
        }
        
        {
            auto data = (Field *) c->user_data;
            auto b = c->real_bounds;
            b.w = b.h;
            b.x += c->real_bounds.w * data->value - b.h * .5;
            drawRoundedRect(cr, b.x, b.y, b.w, b.h, b.h * .5, 1.0);
            set_argb(cr, {1, 1, 1, 1});
            cairo_fill(cr);

            //b.shrink(1.0);
            
            drawRoundedRect(cr, b.x, b.y, b.w, b.h, b.h * .5, 1.0);
            set_argb(cr, slider_bg);
            cairo_stroke(cr);

            b.shrink(5 * dpi);
            drawRoundedRect(cr, b.x, b.y, b.w, b.h, b.h * .5, 1.0);
            set_argb(cr, accent);
            cairo_fill(cr);
        }
        */


static void make_reset_textfield(Container *parent, std::string title, std::string description, bool only_numbers, std::string initial_value, std::string reset_value, std::function<void(std::string)> on_change) {
    auto p = make_self_height_sized_parent(parent);
    
    make_label_like(p, title, description);

    auto right = p->child(::hbox, FILL_SPACE, FILL_SPACE);
    right->alignment = container_alignment::ALIGN_CENTER;
    
    right->pre_layout = [](Container *root, Container *c, const Bounds &b) {
        auto mylar = (MylarWindow*)root->user_data;
        auto cr = mylar->raw_window->cr;
        auto dpi = mylar->raw_window->dpi;
        c->real_bounds.w = 250 * dpi;
        c->real_bounds.h = 70 * dpi;
        c->spacing = 5 * dpi;
    };

    auto field = make_field(right, only_numbers, initial_value, [](std::string latest_text) {
    });
    
    make_button(right, "Apply", [field, on_change]() {
        auto value = ((Field *) field->user_data)->text;
        on_change(value);
    });
    make_button(right, "Reset", [field, reset_value, on_change]() {
        ((Field *) field->user_data)->text = reset_value;
        auto value = ((Field *) field->user_data)->text;
        on_change(value);
    });
}

static void reset_conf() {
    static long last_time = 0;
    static bool already_queued = false;
    long current = get_current_time_in_ms();
    long delta = current - last_time;
    if (delta > 200) {
        last_time = current;
        main_thread([]() {
            hypriso->generate_mylar_hyprland_config();
        });
    } else {
        if (!already_queued) {
            already_queued = true;
            main_thread([]() {
                later(100, [](Timer *) {
                    hypriso->generate_mylar_hyprland_config();
                    already_queued = false;
                });
            });
        }
    }
}

static void fill_keyboard_settings(Container *root, Container *c) {
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
        c->type = ::vbox;
        layout(root, c, b);
        c->type = ::fullycustom;
        auto d = (RightData *) c->user_data;
        float overflow = -actual_true_height(c);
        d->scroll = std::min(std::max(overflow, d->scroll), 0.0f);

        for (auto child : c->children) {
            modify_all(child, 0, d->scroll);
        }
    };
    auto padded_right = right->child(FILL_SPACE, FILL_SPACE);

    make_section_title(padded_right, "Keyboard Settings");
    
    make_vert_space(padded_right, 10);

    bool only_numbers = true;
    make_reset_textfield(padded_right, "Repeat delay", "", only_numbers, std::to_string(set->repeat_delay), "600", [](std::string value) {
        set->repeat_delay = std::atoi(value.c_str());
        reset_conf();
    });

    make_vert_space(padded_right, 4); 
    
    make_reset_textfield(padded_right, "Repeat rate", "", only_numbers, std::to_string(set->repeat_rate), "25", [](std::string value) {
        set->repeat_rate = std::atoi(value.c_str());
        reset_conf();
    });
   
    make_vert_space(padded_right, 10); 
    
    make_section_title(padded_right, "Layouts");
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
        c->type = ::vbox;
        layout(root, c, b);
        c->type = ::fullycustom;
        auto d = (RightData *) c->user_data;
        float overflow = -actual_true_height(c);
        d->scroll = std::min(std::max(overflow, d->scroll), 0.0f);

        for (auto child : c->children) {
            modify_all(child, 0, d->scroll);
        }
    };
    auto padded_right = right->child(FILL_SPACE, FILL_SPACE);
    
    make_section_title(padded_right, "Mouse");
    
    make_vert_space(padded_right, 10); 

    make_bool(padded_right, "Natural scrolling", "Phone like scrolling behaviour", set->natural_scrolling_mouse, [](bool value) {
        set->natural_scrolling_mouse = value;
        main_thread([]() {
            hypriso->generate_mylar_hyprland_config();
        });
    });
    
    make_vert_space(padded_right, 4); 

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
    
    make_vert_space(padded_right, 4); 
    
    make_slider(padded_right, "Cursor speed", "", set->cursor_speed, [](float value) {
        set->cursor_speed = value;
        static long last_time = 0;
        static bool already_queued = false;
        long current = get_current_time_in_ms();
        long delta = current - last_time;
        if (delta > 200) {
            last_time = current;
            main_thread([]() {
                hypriso->generate_mylar_hyprland_config();
            });
        } else {
            if (!already_queued) {
                already_queued = true;
                main_thread([]() {
                    later(100, [](Timer *) {
                        hypriso->generate_mylar_hyprland_config();
                        already_queued = false;
                    });
                });
            }
        }
    });

    make_vert_space(padded_right, 10); 
    
    make_section_title(padded_right, "Touchpad");

    make_vert_space(padded_right, 10); 
    
    make_bool(padded_right, "Natural scrolling", "Phone like scrolling behaviour", set->natural_scrolling_touchpad, [](bool value) {
        set->natural_scrolling_touchpad = value;
        main_thread([]() {
            hypriso->generate_mylar_hyprland_config();
        });
    });
    
    make_vert_space(padded_right, 4); 

    make_bool(padded_right, "Disable while typing", "", set->touchpad_disable_while_typing, [](bool value) {
        set->touchpad_disable_while_typing = value;
        main_thread([]() {
            hypriso->generate_mylar_hyprland_config();
        });
    });

    make_vert_space(padded_right, 4); 

    make_button_group(padded_right, 
        "Touchpad acceleration", 
        "Acceleration makes precision clicks easier by understanding that if the movement is slower, the mouse should travel less distance, and if it’s faster, it should travel a further distance",
        {"Custom", "Adaptive", "Flat"}, 
        [](std::string selected) {
            set->touchpad_acceleration_curve = selected;
            main_thread([]() {
                hypriso->generate_mylar_hyprland_config();
            });
        }, set->touchpad_acceleration_curve);

    make_vert_space(padded_right, 4);

    make_bool(padded_right, "Hotcorners", "", set->hotcorners, [](bool value) {
        set->hotcorners = value;
        main_thread([]() {
            hypriso->generate_mylar_hyprland_config();
        });
    });
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
        } else if (label == "Keyboard") {
            fill_keyboard_settings(root, c);
        } else if (label == "Dock") {
            fill_dock_settings(root, c);
        } else if (label == "Wallpaper") {
            fill_wallpaper_settings(root, c);
        } else if (label == "Desktop") {
            fill_desktop_settings(root, c);
        } else if (label == "Display") {
            fill_display_settings(root);
        }
    };
}

void fill_left(Container *left) {
    create_tab_option(left, "Search");
    create_tab_option(left, "Display");
    create_tab_option(left, "Desktop");
    create_tab_option(left, "Mouse & Touchpad");
    create_tab_option(left, "Keyboard");
    create_tab_option(left, "Shortcuts");
    create_tab_option(left, "Time & Date");
    create_tab_option(left, "Audio");
    create_tab_option(left, "Wifi");
    create_tab_option(left, "Dock");
    create_tab_option(left, "Wallpaper");
}

void fill_root(Container *root) {
    root->when_paint = paint {
        auto mylar = (MylarWindow*)root->user_data;
        auto cr = mylar->raw_window->cr;
        set_argb(cr, right_color);
        set_rect(cr, c->real_bounds);
        cairo_fill(cr);
    };
    auto left_right = root->child(::hbox, FILL_SPACE, FILL_SPACE);
    
    auto left = left_right->child(::vbox, 300, FILL_SPACE);
    left->pre_layout = [](Container *root, Container *c, const Bounds &b) {
        auto mylar = (MylarWindow*)root->user_data;
        c->wanted_bounds.w = 200 * mylar->raw_window->dpi;
    };
    left->when_paint = paint {
        return;
        auto mylar = (MylarWindow*)root->user_data;
        auto cr = mylar->raw_window->cr;
        set_argb(cr, left_color);
        set_rect(cr, c->real_bounds);
        cairo_fill(cr);
    };
    fill_left(left);

    auto right = left_right->child(::fullycustom, FILL_SPACE, FILL_SPACE);
    right->name = "settings_right";
    auto d = new RightData;
    right->user_data = d;
    right->when_paint = paint {
        return;
        auto mylar = (MylarWindow*)root->user_data;
        auto cr = mylar->raw_window->cr;
        set_argb(cr, right_color);
        set_rect(cr, c->real_bounds);
        cairo_fill(cr);
    };
    right->receive_events_even_if_obstructed = true;
    right->when_fine_scrolled = [](Container* root, Container* c, double scroll_x, double scroll_y, bool came_from_touchpad) {
        auto d = (RightData *) c->user_data;
        auto mylar = (MylarWindow*)root->user_data;
        d->scroll += scroll_y * 3 * mylar->raw_window->dpi;
    };
}

void actual_start() {
    settings_app = windowing::open_app();
    RawWindowSettings settings;
    settings.pos.w = 1000;
    settings.pos.h = 760;
    //settings.pos.min_w = 900;
    //settings.pos.min_h = 700;
    settings.name = "Settings";
    auto mylar = open_mylar_window(settings_app, WindowType::NORMAL, settings);
    mylar->root->user_data = mylar;
    settings_mylar = mylar;
    fill_root(mylar->root);
    mylar->raw_window->on_close = [](RawWindow *w) {
        for (auto p : popups) {
            windowing::close_window(p->raw_window);
        }
        popups.clear();
        std::this_thread::sleep_for(std::chrono::milliseconds(40));
    };

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

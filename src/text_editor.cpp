#include "text_editor.h"

#include "client/raw_windowing.h"
#include "dock/dock.h"
#include "heart.h"
#include "hypriso.h"
#include "pango_font_cache.h"

#include <algorithm>
#include <cmath>
#include <pango/pangocairo.h>
#include <string_view>
#include <utility>
#include <xkbcommon/xkbcommon-keysyms.h>

STextEditorData::~STextEditorData() {
    if (layout)
        g_object_unref(layout);
}

// How long a blinking caret shows, and then hides.
static constexpr long caret_blink_ms = 530;

static bool editor_active(Container *c, const STextEditorData &data) {
    return c->active || (data.options.parent_activates && c->parent && c->parent->active);
}

static constexpr std::string_view password_bullet = "\u2022";

// The same close glyph the titlebar uses.
static constexpr std::string_view clear_glyph = "\uE8BB";

static bool editor_hovered(Container *c, const STextEditorData &data) {
    return c->state.mouse_hovering || (data.options.parent_activates && c->parent && c->parent->state.mouse_hovering);
}

static bool clear_button_visible(Container *c, const STextEditorData &data) {
    if (!data.options.clear_button || !data.options.editable || data.options.multiline || data.text.empty())
        return false;
    return editor_active(c, data) || editor_hovered(c, data) || data.clear_pressed;
}

// A square at the right end of the owner's viewport, reaching a little into its padding.
static Bounds clear_button_bounds(Container *root, Container *c, const STextEditorData &data) {
    auto viewport = data.options.viewport(root, c);
    double dpi = data.options.window(root)->dpi;
    double size = std::round(20 * dpi);
    double right = viewport.x + viewport.w + std::round(5 * dpi);
    return Bounds(right - size, std::round(viewport.y + (viewport.h - size) * .5), size, size);
}

static bool over_clear_button(Container *root, Container *c, const STextEditorData &data) {
    if (!clear_button_visible(c, data))
        return false;
    auto b = clear_button_bounds(root, c, data);
    double x = root->mouse_current_x;
    double y = root->mouse_current_y;
    return x >= b.x && x < b.x + b.w && y >= b.y && y < b.y + b.h;
}

// Where the text goes: the owner's viewport, minus the clear button while it shows.
static Bounds text_viewport(Container *root, Container *c, const STextEditorData &data) {
    auto viewport = data.options.viewport(root, c);
    if (clear_button_visible(c, data)) {
        auto button = clear_button_bounds(root, c, data);
        double dpi = data.options.window(root)->dpi;
        viewport.w = std::max(0.0, std::min(viewport.w, button.x - viewport.x - 2 * dpi));
    }
    return viewport;
}

// Password layouts hold one bullet per character, so byte offsets differ between the
// text and the layout. Every Pango index goes through these two conversions.
static int layout_index(const STextEditorData &data, int index) {
    if (!data.options.password)
        return index;
    return g_utf8_pointer_to_offset(data.text.data(), data.text.data() + index) * password_bullet.size();
}

static int text_index(const STextEditorData &data, int index) {
    if (!data.options.password)
        return index;
    long characters = g_utf8_strlen(data.text.data(), data.text.size());
    long offset = std::clamp<long>(index / static_cast<int>(password_bullet.size()), 0, characters);
    return g_utf8_offset_to_pointer(data.text.data(), offset) - data.text.data();
}

static PangoLayout *editor_layout(Container *root, STextEditorData &data) {
    auto window = data.options.window(root);
    if (!data.layout)
        data.layout = pango_cairo_create_layout(drawing::scratch_cairo());
    auto font = pango_font_description_new();
    pango_font_description_set_family(font, set->font.c_str());
    pango_font_description_set_size(font, static_cast<int>(data.options.font_size * window->dpi) * PANGO_SCALE);
    pango_font_description_set_weight(font, data.options.bold ? PANGO_WEIGHT_BOLD : PANGO_WEIGHT_NORMAL);
    pango_layout_set_font_description(data.layout, font);
    pango_font_description_free(font);
    pango_layout_set_width(data.layout, -1);
    pango_layout_set_single_paragraph_mode(data.layout, !data.options.multiline);
    if (data.options.password) {
        std::string masked;
        for (long i = g_utf8_strlen(data.text.data(), data.text.size()); i > 0; --i)
            masked += password_bullet;
        pango_layout_set_text(data.layout, masked.data(), masked.size());
    } else {
        pango_layout_set_text(data.layout, data.text.data(), data.text.size());
    }
    pango_cairo_update_layout(drawing::scratch_cairo(), data.layout);
    return data.layout;
}

static int previous_character(const std::string &text, int cursor) {
    if (cursor <= 0)
        return 0;
    return g_utf8_find_prev_char(text.data(), text.data() + cursor) - text.data();
}

static int next_character(const std::string &text, int cursor) {
    if (cursor >= static_cast<int>(text.size()))
        return text.size();
    return g_utf8_next_char(text.data() + cursor) - text.data();
}

enum class ETextMotion {
    Left,
    Right,
};

enum class ETextGroup {
    Edge,
    Space,
    Token,
    Normal,
};

static ETextGroup token_type(const std::string &text, int position) {
    if (position < 0 || position >= static_cast<int>(text.size()))
        return ETextGroup::Edge;
    char character = text[position];
    if (character == ' ' || character == '\t' || character == '\n' || character == '\r')
        return ETextGroup::Space;
    static constexpr std::string_view tokens = R"([]|(){};.!@#$%^&*-=+:'"<>?\/,`~)";
    if (tokens.find(character) != std::string_view::npos)
        return ETextGroup::Token;
    return ETextGroup::Normal;
}

static int seek_token(const std::string &text, int cursor, ETextMotion motion);

// Word motions would reveal where a password has spaces or symbols; jump to the ends.
static int seek_word(const STextEditorData &data, int cursor, ETextMotion motion) {
    if (data.options.password)
        return motion == ETextMotion::Right ? data.text.size() : 0;
    return seek_token(data.text, cursor, motion);
}

static int seek_token(const std::string &text, int cursor, ETextMotion motion) {
    // Unlike Kotlin's inclusive character ranges, caret offsets delimit [start, end).
    // Inspect the character on the side we are moving towards, even at a boundary.
    bool right = motion == ETextMotion::Right;
    int length = text.size();
    while (right ? cursor < length : cursor > 0) {
        int position = right ? cursor : previous_character(text, cursor);
        auto group = token_type(text, position);
        bool found_space = false;
        bool found_newline = false;
        do {
            char character = text[position];
            found_space |= character == ' ' || character == '\t';
            found_newline |= character == '\n' || character == '\r';
            cursor = right ? next_character(text, position) : position;
            position = right ? cursor : (cursor > 0 ? previous_character(text, cursor) : -1);
        } while (token_type(text, position) == group);

        // Skip a horizontal whitespace run and seek the next group, but stop on
        // whitespace containing a line break. Iterate instead of recursing.
        if (!found_space || found_newline)
            break;
    }
    return cursor;
}

static int line_start(const std::string &text, int cursor) {
    if (cursor <= 0)
        return 0;
    auto newline = text.rfind('\n', cursor - 1);
    return newline == std::string::npos ? 0 : newline + 1;
}

static int line_end(const std::string &text, int cursor) {
    auto newline = text.find('\n', cursor);
    return newline == std::string::npos ? text.size() : newline;
}

// Apply the same restrictions to typing, clipboard input, and initial/reset values.
static std::string filter_text(const STextEditorData &data, const std::string &text) {
    if (!g_utf8_validate(text.data(), text.size(), nullptr))
        return {};
    std::string result;
    for (size_t index = 0; index < text.size();) {
        auto character = g_utf8_get_char(text.data() + index);
        auto next = next_character(text, index);
        bool newline = character == '\n' || character == '\r' || character == 0x85 || character == 0x2028 || character == 0x2029;
        if (data.options.multiline || !newline)
            result.append(text, index, next - index);
        index = next;
    }
    if (data.options.only_numbers && !std::all_of(result.begin(), result.end(), [](unsigned char c) { return c >= '0' && c <= '9'; }))
        return {};
    return result;
}

static void break_edit_group(STextEditorData &data) {
    data.coalescing_edit = 0;
    data.last_text_cursor = -1;
}

static STextEditorHistoryEntry editor_snapshot(const STextEditorData &data) {
    return {data.text, data.cursor, data.selection, data.selecting};
}

static void record_edit(STextEditorData &data) {
    data.undo_stack.push_back(editor_snapshot(data));
    data.redo_stack.clear();
    break_edit_group(data);
}

static bool has_selection(const STextEditorData &data) {
    return data.selecting && data.cursor != data.selection;
}

static void erase_selection(STextEditorData &data) {
    if (has_selection(data)) {
        int start = std::min(data.cursor, data.selection);
        data.text.erase(start, std::abs(data.cursor - data.selection));
        data.cursor = start;
    }
    data.selecting = false;
}

static void insert_text(STextEditorData &data, const std::string &input, bool coalesce) {
    auto text = filter_text(data, input);
    if (data.options.max_length > 0) {
        int selected = data.selecting ? std::abs(data.cursor - data.selection) : 0;
        int room = std::max(0, data.options.max_length - static_cast<int>(data.text.size()) + selected);
        if (static_cast<int>(text.size()) > room) {
            // Cut on a character boundary.
            int end = room;
            while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xc0) == 0x80)
                --end;
            text.resize(end);
        }
    }
    if (text.empty())
        return;
    if (!coalesce || has_selection(data) || data.coalescing_edit != 1 || data.cursor != data.last_text_cursor)
        record_edit(data);
    erase_selection(data);
    data.text.insert(data.cursor, text);
    data.cursor += text.size();
    data.redo_stack.clear();
    data.coalescing_edit = coalesce ? 1 : 0;
    data.last_text_cursor = coalesce ? data.cursor : -1;
}

static void clamp_scroll(Container *root, Container *c, STextEditorData &data) {
    int width = 0;
    pango_layout_get_pixel_size(editor_layout(root, data), &width, nullptr);
    auto viewport = text_viewport(root, c, data);
    auto overflow = std::max(0.0, width + 1.0 - viewport.w);
    data.scroll_x = std::clamp<double>(data.scroll_x, 0, overflow);
}

static void keep_cursor_in_view(Container *root, Container *c, STextEditorData &data) {
    auto viewport = text_viewport(root, c, data);
    if (viewport.w <= 0)
        return;
    PangoRectangle cursor = {};
    pango_layout_get_cursor_pos(editor_layout(root, data), layout_index(data, data.cursor), &cursor, nullptr);
    double x = static_cast<double>(cursor.x) / PANGO_SCALE;
    double margin = std::min(60.0, viewport.w * .25);
    if (x < data.scroll_x + margin)
        data.scroll_x = x - margin;
    else if (x + 1 > data.scroll_x + viewport.w - margin)
        data.scroll_x = x + 1 - viewport.w + margin;
    clamp_scroll(root, c, data);
}

static void move_cursor(STextEditorData &data, int cursor, bool selecting) {
    if (selecting && !data.selecting)
        data.selection = data.cursor;
    data.selecting = selecting;
    data.cursor = cursor;
}

static void update_mouse_cursor(Container *root, Container *c, bool selecting) {
    auto &data = *static_cast<STextEditorData *>(c->user_data);
    auto layout = editor_layout(root, data);
    auto viewport = text_viewport(root, c, data);
    int height = 0;
    pango_layout_get_pixel_size(layout, nullptr, &height);
    double x = root->mouse_current_x - viewport.x + data.scroll_x;
    double y = data.options.multiline ? root->mouse_current_y - viewport.y - (viewport.h - height) * .5 : height * .5;
    int index = 0;
    int trailing = 0;
    pango_layout_xy_to_index(layout, x * PANGO_SCALE, y * PANGO_SCALE, &index, &trailing);
    index = text_index(data, index);
    while (trailing-- > 0)
        index = next_character(data.text, index);
    move_cursor(data, index, selecting);
    break_edit_group(data);
    if (!selecting)
        data.selection = data.cursor;
    else
        keep_cursor_in_view(root, c, data);
}

static void editor_key(Container *root, Container *c, xkb_keysym_t sym, int mods, bool is_text, const std::string &text) {
    auto &data = *static_cast<STextEditorData *>(c->user_data);
    bool control = mods & Modifier::MOD_CTRL;
    bool shift = mods & Modifier::MOD_SHIFT;
    if (control) {
        auto key = xkb_keysym_to_lower(sym);
        if (key == XKB_KEY_z || key == XKB_KEY_y) {
            bool redo = key == XKB_KEY_y || shift;
            auto &source = redo ? data.redo_stack : data.undo_stack;
            auto &destination = redo ? data.undo_stack : data.redo_stack;
            if (!source.empty()) {
                destination.push_back(editor_snapshot(data));
                auto state = std::move(source.back());
                source.pop_back();
                data.text = std::move(state.text);
                data.cursor = state.cursor;
                data.selection = state.selection;
                data.selecting = state.selecting;
            }
            break_edit_group(data);
            return;
        }
        break_edit_group(data);
        if (key == XKB_KEY_a) {
            data.selection = 0;
            data.cursor = data.text.size();
            data.selecting = true;
            return;
        }
        if (key == XKB_KEY_c || key == XKB_KEY_x) {
            if (!has_selection(data) || data.options.password)
                return;
            auto selected = data.text.substr(std::min(data.cursor, data.selection), std::abs(data.cursor - data.selection));
            if (windowing::set_clipboard(data.options.window(root), selected) && key == XKB_KEY_x) {
                record_edit(data);
                erase_selection(data);
            }
            return;
        }
        if (key == XKB_KEY_v) {
            auto window = data.options.window(root);
            std::weak_ptr<bool> lifetime = c->lifetime;
            std::weak_ptr<bool> root_lifetime = root->lifetime;
            windowing::get_clipboard(window, [root, c, window, lifetime, root_lifetime](std::string pasted) {
                if (lifetime.expired() || root_lifetime.expired() || !windowing::has_window(window))
                    return;
                auto &data = *static_cast<STextEditorData *>(c->user_data);
                auto before = data.text;
                insert_text(data, pasted, false);
                keep_cursor_in_view(root, c, data);
                windowing::redraw(window);
                if (before != data.text && data.options.on_change)
                    data.options.on_change(data.text);
            });
            return;
        }
    }
    if (sym == XKB_KEY_Return || sym == XKB_KEY_KP_Enter) {
        break_edit_group(data);
        if (data.options.multiline && !data.options.only_numbers)
            insert_text(data, "\n", false);
        return;
    }
    if (sym == XKB_KEY_Tab || sym == XKB_KEY_ISO_Left_Tab) {
        break_edit_group(data);
        if (data.options.on_tab)
            data.options.on_tab(root, c, shift || sym == XKB_KEY_ISO_Left_Tab);
        return;
    }
    if (sym == XKB_KEY_BackSpace || sym == XKB_KEY_Delete) {
        bool backspace = sym == XKB_KEY_BackSpace;
        int group = backspace ? 2 : 3;
        if (has_selection(data)) {
            record_edit(data);
            erase_selection(data);
        } else if ((backspace && data.cursor > 0) || (!backspace && data.cursor < static_cast<int>(data.text.size()))) {
            if (data.coalescing_edit != group || data.cursor != data.last_text_cursor)
                record_edit(data);
            int boundary = control ? seek_word(data, data.cursor, backspace ? ETextMotion::Left : ETextMotion::Right) :
                (backspace ? previous_character(data.text, data.cursor) : next_character(data.text, data.cursor));
            int start = std::min(data.cursor, boundary);
            int end = std::max(data.cursor, boundary);
            data.text.erase(start, end - start);
            data.cursor = start;
            data.selecting = false;
            data.redo_stack.clear();
            data.coalescing_edit = group;
            data.last_text_cursor = data.cursor;
            // Each token deletion is undoable on its own and must not merge
            // with subsequent character-by-character deletions.
            if (control)
                break_edit_group(data);
        }
        return;
    }
    if (is_text && !control && !(mods & (Modifier::MOD_ALT | Modifier::MOD_SUPER))) {
        insert_text(data, text, true);
        return;
    }
    break_edit_group(data);
    int cursor = data.cursor;
    if (sym == XKB_KEY_Left || sym == XKB_KEY_Right) {
        bool right = sym == XKB_KEY_Right;
        if (control)
            cursor = seek_word(data, cursor, right ? ETextMotion::Right : ETextMotion::Left);
        else if (!shift && has_selection(data))
            cursor = right ? std::max(data.cursor, data.selection) : std::min(data.cursor, data.selection);
        else
            cursor = right ? next_character(data.text, cursor) : previous_character(data.text, cursor);
    } else if (sym == XKB_KEY_Home)
        cursor = control ? 0 : line_start(data.text, cursor);
    else if (sym == XKB_KEY_End)
        cursor = control ? data.text.size() : line_end(data.text, cursor);
    else if (data.options.multiline && (sym == XKB_KEY_Up || sym == XKB_KEY_Down)) {
        int start = line_start(data.text, cursor);
        int end = line_end(data.text, cursor);
        int column = g_utf8_pointer_to_offset(data.text.data() + start, data.text.data() + cursor);
        if (sym == XKB_KEY_Up) {
            if (start == 0)
                return;
            end = start - 1;
            start = line_start(data.text, end);
        } else {
            if (end == static_cast<int>(data.text.size()))
                return;
            start = end + 1;
            end = line_end(data.text, start);
        }
        cursor = start;
        while (column-- > 0 && cursor < end)
            cursor = next_character(data.text, cursor);
    } else
        return;
    move_cursor(data, cursor, shift);
}

// The clear button: one undoable edit that empties the field.
static void clear_text_editor(Container *root, Container *c) {
    auto &data = *static_cast<STextEditorData *>(c->user_data);
    if (data.text.empty())
        return;
    record_edit(data);
    data.text.clear();
    data.cursor = 0;
    data.selection = 0;
    data.selecting = false;
    data.scroll_x = 0;
    data.clear_hovered = false;
    windowing::redraw(data.options.window(root));
    if (data.options.on_change)
        data.options.on_change(data.text);
}

void setup_text_editor(Container *c, std::string initial_value, STextEditorOptions options) {
    auto data = new STextEditorData;
    data->options = std::move(options);
    c->user_data = data;
    reset_text_editor(c, std::move(initial_value));
    if (!data->options.editable)
        return;
    c->when_drag_end_is_click = false;
    c->when_key_event = [](Container *root, Container *c, int key, bool pressed, xkb_keysym_t sym, int mods, bool is_text, std::string text) {
        auto &data = *static_cast<STextEditorData *>(c->user_data);
        if (!pressed || !editor_active(c, data))
            return;
        auto before = data.text;
        editor_key(root, c, sym, mods, is_text, text);
        keep_cursor_in_view(root, c, data);
        if (before != data.text && data.options.on_change)
            data.options.on_change(data.text);
    };
    c->when_mouse_down = [](Container *root, Container *c) {
        auto &data = *static_cast<STextEditorData *>(c->user_data);
        data.clear_pressed = over_clear_button(root, c, data);
        if (data.clear_pressed)
            return;
        update_mouse_cursor(root, c, false);
    };
    auto drag = [](Container *root, Container *c) {
        auto &data = *static_cast<STextEditorData *>(c->user_data);
        if (data.clear_pressed)
            return;
        update_mouse_cursor(root, c, true);
    };
    c->when_drag_start = drag;
    c->when_drag = drag;
    c->when_drag_end = [](Container *root, Container *c) {
        auto &data = *static_cast<STextEditorData *>(c->user_data);
        if (data.clear_pressed) {
            data.clear_pressed = false;
            return;
        }
        update_mouse_cursor(root, c, true);
    };
    c->when_mouse_motion = [](Container *root, Container *c) {
        auto &data = *static_cast<STextEditorData *>(c->user_data);
        bool hovered = over_clear_button(root, c, data);
        if (hovered != data.clear_hovered) {
            data.clear_hovered = hovered;
            windowing::redraw(data.options.window(root));
        }
    };
    c->when_mouse_leaves_container = [](Container *root, Container *c) {
        auto &data = *static_cast<STextEditorData *>(c->user_data);
        data.clear_hovered = false;
        windowing::redraw(data.options.window(root));
    };
    c->when_clicked = [](Container *root, Container *c) {
        auto &data = *static_cast<STextEditorData *>(c->user_data);
        if (data.clear_pressed) {
            data.clear_pressed = false;
            if (over_clear_button(root, c, data))
                clear_text_editor(root, c);
            return;
        }
        long current = get_current_time_in_ms();
        if (current - data.last_time < 400 && current - data.last_activation > 800) {
            data.last_activation = current;
            data.selecting = true;
            data.selection = 0;
            data.cursor = data.text.size();
            break_edit_group(data);
        } else
            data.last_time = current;
    };
    c->when_fine_scrolled = [](Container *root, Container *c, double x, double y, bool touchpad) {
        auto &data = *static_cast<STextEditorData *>(c->user_data);
        data.scroll_x -= x != 0 ? x : y;
        clamp_scroll(root, c, data);
    };
    c->when_active_status_changed = [](Container *root, Container *c) {
        auto &data = *static_cast<STextEditorData *>(c->user_data);
        break_edit_group(data);
        if (!editor_active(c, data))
            data.selecting = false;
    };
}

Bounds measure_text_editor(Container *root, Container *c) {
    auto &data = *static_cast<STextEditorData *>(c->user_data);
    int width = 0;
    int height = 0;
    pango_layout_get_pixel_size(editor_layout(root, data), &width, &height);
    return Bounds(0, 0, width, height);
}

void paint_text_editor(Container *root, Container *c, const RGBA &color, const RGBA &selection_color) {
    auto &data = *static_cast<STextEditorData *>(c->user_data);
    auto window = data.options.window(root);
    auto cr = window->drawing_context;
    auto layout = editor_layout(root, data);
    auto viewport = text_viewport(root, c, data);
    int height = 0;
    pango_layout_get_pixel_size(layout, nullptr, &height);
    double x = viewport.x - data.scroll_x;
    double y = viewport.y + (viewport.h - height) * .5;
    if (!editor_active(c, data))
        data.selecting = false;
    cr->save();
    set_rect(cr, viewport);
    cr->clip_path();
    if (has_selection(data)) {
        set_argb(cr, selection_color);
        int start = layout_index(data, std::min(data.cursor, data.selection));
        int end = layout_index(data, std::max(data.cursor, data.selection));
        auto iter = pango_layout_get_iter(layout);
        do {
            auto line = pango_layout_iter_get_line_readonly(iter);
            PangoRectangle logical = {};
            pango_layout_iter_get_line_extents(iter, nullptr, &logical);
            int *ranges = nullptr;
            int count = 0;
            pango_layout_line_get_x_ranges(line, start, end, &ranges, &count);
            for (int i = 0; i < count; ++i) {
                set_rect(cr, Bounds(x + ranges[i * 2] / static_cast<double>(PANGO_SCALE),
                    y + logical.y / static_cast<double>(PANGO_SCALE),
                    (ranges[i * 2 + 1] - ranges[i * 2]) / static_cast<double>(PANGO_SCALE),
                    logical.height / static_cast<double>(PANGO_SCALE)));
                cr->fill();
            }
            g_free(ranges);
        } while (pango_layout_iter_next_line(iter));
        pango_layout_iter_free(iter);
    }
    set_argb(cr, color);
    cr->move_to(x, y);
    cr->show_layout(layout);
    const bool active = editor_active(c, data);
    bool caret_showing = true;
    if (data.options.blink_caret && active) {
        const long now = get_current_time_in_ms();
        if (!data.blink_was_active || data.blink_cursor != data.cursor || data.blink_text_size != data.text.size()) {
            data.blink_cursor = data.cursor;
            data.blink_text_size = data.text.size();
            data.blink_start = now;
        }
        caret_showing = ((now - data.blink_start) / caret_blink_ms) % 2 == 0;
    }
    data.blink_was_active = active;
    if (data.options.editable && active && caret_showing) {
        PangoRectangle cursor = {};
        pango_layout_get_cursor_pos(layout, layout_index(data, data.cursor), &cursor, nullptr);
        set_rect(cr, Bounds(x + cursor.x / static_cast<double>(PANGO_SCALE),
            y + cursor.y / static_cast<double>(PANGO_SCALE), std::max(1.0f, std::round(window->dpi)),
            cursor.height / static_cast<double>(PANGO_SCALE)));
        cr->fill();
    }
    cr->restore();

    if (clear_button_visible(c, data)) {
        auto b = clear_button_bounds(root, c, data);
        bool hovered = over_clear_button(root, c, data);
        if (hovered || data.clear_pressed) {
            set_argb(cr, RGBA(color.r, color.g, color.b, color.a * (data.clear_pressed ? .2 : .1)));
            cr->new_path();
            cr->arc(b.x + b.w * .5, b.y + b.h * .5, b.w * .5, 0, 2 * M_PI);
            cr->fill();
        }
        auto glyph = get_cached_pango_font(cr, icon_font, std::round(8 * window->dpi), PANGO_WEIGHT_NORMAL, false);
        pango_layout_set_text(glyph, clear_glyph.data(), clear_glyph.size());
        pango_layout_set_width(glyph, -1);
        PangoRectangle ink = {};
        pango_layout_get_pixel_extents(glyph, &ink, nullptr);
        set_argb(cr, RGBA(color.r, color.g, color.b, color.a * (hovered ? .9 : .55)));
        cr->move_to(std::round(b.x + (b.w - ink.width) * .5 - ink.x), std::round(b.y + (b.h - ink.height) * .5 - ink.y));
        cr->show_layout(glyph);
    }

    // Repaint when the caret next flips, for as long as the field stays focused.
    if (data.options.blink_caret && data.options.editable && active && !data.blink_timer_pending) {
        data.blink_timer_pending = true;
        auto app = window->creator;
        std::weak_ptr<bool> lifetime = c->lifetime;
        windowing::timer(app, text_editor_ms_until_blink(c) + 1, [app, window, c, lifetime](void *) {
            std::lock_guard<std::mutex> lock(app->mutex);
            if (lifetime.expired())
                return;
            auto &data = *static_cast<STextEditorData *>(c->user_data);
            data.blink_timer_pending = false;
            if (windowing::has_window(window) && editor_active(c, data))
                windowing::redraw(window);
        }, nullptr);
    }
}

long text_editor_ms_until_blink(Container *c) {
    auto &data = *static_cast<STextEditorData *>(c->user_data);
    const long elapsed = std::max(0L, get_current_time_in_ms() - data.blink_start);
    return caret_blink_ms - elapsed % caret_blink_ms;
}

void reset_text_editor(Container *c, std::string value) {
    auto &data = *static_cast<STextEditorData *>(c->user_data);
    data.text = filter_text(data, value);
    data.cursor = data.text.size();
    data.selection = data.cursor;
    data.selecting = false;
    data.scroll_x = 0;
    data.undo_stack.clear();
    data.redo_stack.clear();
    break_edit_group(data);
}

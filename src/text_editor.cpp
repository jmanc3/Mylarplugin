#include "text_editor.h"

#include "client/raw_windowing.h"
#include "dock/dock.h"
#include "heart.h"

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

static bool editor_active(Container *c, const STextEditorData &data) {
    return c->active || (data.options.parent_activates && c->parent && c->parent->active);
}

static PangoLayout *editor_layout(Container *root, STextEditorData &data) {
    auto window = data.options.window(root);
    if (!data.layout)
        data.layout = pango_cairo_create_layout(window->cr);
    auto font = pango_font_description_new();
    pango_font_description_set_family(font, set->font.c_str());
    pango_font_description_set_size(font, static_cast<int>(data.options.font_size * window->dpi) * PANGO_SCALE);
    pango_font_description_set_weight(font, data.options.bold ? PANGO_WEIGHT_BOLD : PANGO_WEIGHT_NORMAL);
    pango_layout_set_font_description(data.layout, font);
    pango_font_description_free(font);
    pango_layout_set_width(data.layout, -1);
    pango_layout_set_single_paragraph_mode(data.layout, !data.options.multiline);
    pango_layout_set_text(data.layout, data.text.data(), data.text.size());
    pango_cairo_update_layout(window->cr, data.layout);
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
    auto viewport = data.options.viewport(root, c);
    auto overflow = std::max(0.0, width + 1.0 - viewport.w);
    data.scroll_x = std::clamp<double>(data.scroll_x, 0, overflow);
}

static void keep_cursor_in_view(Container *root, Container *c, STextEditorData &data) {
    auto viewport = data.options.viewport(root, c);
    if (viewport.w <= 0)
        return;
    PangoRectangle cursor = {};
    pango_layout_get_cursor_pos(editor_layout(root, data), data.cursor, &cursor, nullptr);
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
    auto viewport = data.options.viewport(root, c);
    int height = 0;
    pango_layout_get_pixel_size(layout, nullptr, &height);
    double x = root->mouse_current_x - viewport.x + data.scroll_x;
    double y = data.options.multiline ? root->mouse_current_y - viewport.y - (viewport.h - height) * .5 : height * .5;
    int index = 0;
    int trailing = 0;
    pango_layout_xy_to_index(layout, x * PANGO_SCALE, y * PANGO_SCALE, &index, &trailing);
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
            if (!has_selection(data))
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
            int boundary = control ? seek_token(data.text, data.cursor, backspace ? ETextMotion::Left : ETextMotion::Right) :
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
            cursor = seek_token(data.text, cursor, right ? ETextMotion::Right : ETextMotion::Left);
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
        update_mouse_cursor(root, c, false);
    };
    auto drag = [](Container *root, Container *c) {
        update_mouse_cursor(root, c, true);
    };
    c->when_drag_start = drag;
    c->when_drag = drag;
    c->when_drag_end = drag;
    c->when_clicked = [](Container *root, Container *c) {
        auto &data = *static_cast<STextEditorData *>(c->user_data);
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
    auto cr = window->cr;
    auto layout = editor_layout(root, data);
    auto viewport = data.options.viewport(root, c);
    int height = 0;
    pango_layout_get_pixel_size(layout, nullptr, &height);
    double x = viewport.x - data.scroll_x;
    double y = viewport.y + (viewport.h - height) * .5;
    if (!editor_active(c, data))
        data.selecting = false;
    cairo_save(cr);
    set_rect(cr, viewport);
    cairo_clip(cr);
    if (has_selection(data)) {
        set_argb(cr, selection_color);
        int start = std::min(data.cursor, data.selection);
        int end = std::max(data.cursor, data.selection);
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
                cairo_fill(cr);
            }
            g_free(ranges);
        } while (pango_layout_iter_next_line(iter));
        pango_layout_iter_free(iter);
    }
    set_argb(cr, color);
    cairo_move_to(cr, x, y);
    pango_cairo_show_layout(cr, layout);
    if (data.options.editable && editor_active(c, data)) {
        PangoRectangle cursor = {};
        pango_layout_get_cursor_pos(layout, data.cursor, &cursor, nullptr);
        set_rect(cr, Bounds(x + cursor.x / static_cast<double>(PANGO_SCALE),
            y + cursor.y / static_cast<double>(PANGO_SCALE), std::max(1.0f, std::round(window->dpi)),
            cursor.height / static_cast<double>(PANGO_SCALE)));
        cairo_fill(cr);
    }
    cairo_restore(cr);
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

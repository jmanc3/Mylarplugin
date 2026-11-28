#ifndef text_editor_h_INCLUDED
#define text_editor_h_INCLUDED

#include "container.h"

struct RawWindow;
struct RGBA;
struct _PangoLayout;

struct STextEditorHistoryEntry {
    std::string text;
    int cursor = 0;
    int selection = 0;
    bool selecting = false;
};

struct STextEditorOptions {
    bool multiline = true;
    bool only_numbers = false;
    bool editable = true;
    bool bold = false;
    bool parent_activates = false;
    // Draws one bullet per character and refuses to copy or cut the text.
    bool password = false;
    // Blink the caret. The editor schedules the repaints itself while focused.
    bool blink_caret = true;
    // Single-line editable fields show an 'x' at the right end that clears the text
    // while they hold text and are focused or hovered.
    bool clear_button = true;
    // Maximum text length in bytes (never splitting a character); 0 means unlimited.
    int max_length = 0;
    int font_size = 13;
    std::function<RawWindow *(Container *)> window;
    std::function<Bounds(Container *, Container *)> viewport;
    std::function<void(std::string)> on_change;
    std::function<void(Container *, Container *, bool)> on_tab;
};

struct STextEditorData : UserData {
    ~STextEditorData() override;

    std::string text;
    int cursor = 0;
    int selection = 0;
    bool selecting = false;
    std::vector<STextEditorHistoryEntry> undo_stack;
    std::vector<STextEditorHistoryEntry> redo_stack;
    int coalescing_edit = 0;
    int last_text_cursor = -1;
    long last_time = 0;
    long last_activation = 0;
    // The caret shows for the first half of every blink since blink_start,
    // which restarts when the caret moves, the text changes or the field is focused.
    long blink_start = 0;
    int blink_cursor = -1;
    size_t blink_text_size = 0;
    bool blink_was_active = false;
    bool blink_timer_pending = false;
    // The clear button was pressed and the release has not happened yet.
    bool clear_pressed = false;
    bool clear_hovered = false;
    float scroll_x = 0;
    _PangoLayout *layout = nullptr;
    STextEditorOptions options;
};

void setup_text_editor(Container *c, std::string initial_value, STextEditorOptions options);
Bounds measure_text_editor(Container *root, Container *c);
void paint_text_editor(Container *root, Container *c, const RGBA &color, const RGBA &selection_color);
void reset_text_editor(Container *c, std::string value);
void set_text_editor(Container *c, std::string value);
// Milliseconds until a blinking caret (options.blink_caret) next shows or hides.
long text_editor_ms_until_blink(Container *c);

#endif // text_editor_h_INCLUDED

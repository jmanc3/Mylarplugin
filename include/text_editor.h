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
    float scroll_x = 0;
    _PangoLayout *layout = nullptr;
    STextEditorOptions options;
};

void setup_text_editor(Container *c, std::string initial_value, STextEditorOptions options);
Bounds measure_text_editor(Container *root, Container *c);
void paint_text_editor(Container *root, Container *c, const RGBA &color, const RGBA &selection_color);
void reset_text_editor(Container *c, std::string value);

#endif // text_editor_h_INCLUDED

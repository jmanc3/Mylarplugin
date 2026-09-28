#ifndef edit_pin_h_INCLUDED
#define edit_pin_h_INCLUDED

#include <string>
#include <vector>
#include "container.h"

struct LabelHistoryEntry {
    std::string text;
    int cursor;
    int selection;
    bool selecting;
};

struct LabelData : UserData {
    int cursor = 0;
    
    int selection = 0;
    bool selecting = false;

    std::string text;
    std::vector<LabelHistoryEntry> undo_stack;
    std::vector<LabelHistoryEntry> redo_stack;
    int coalescing_edit = 0;
    int last_text_cursor = -1;

    long last_time = 0;
    long last_activation = 0;

    float scroll_x = 0;
    float scroll_y = 0;
};

namespace edit_pin {
    void open(std::string stacking_rule, std::string icon, std::string command);
};

#endif // edit_pin_h_INCLUDED

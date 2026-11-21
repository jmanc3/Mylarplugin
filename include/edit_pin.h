#ifndef edit_pin_h_INCLUDED
#define edit_pin_h_INCLUDED

#include "text_editor.h"

using LabelHistoryEntry = STextEditorHistoryEntry;
using LabelData = STextEditorData;

namespace edit_pin {
    inline constexpr const char *kAppId = "mylar-edit-pin";
    void open(std::string stacking_rule, std::string icon, std::string command);
    void stop();
};

#endif // edit_pin_h_INCLUDED

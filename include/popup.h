#pragma once

#include <string> 
#include <vector> 
#include <functional> 
#include "container.h"

struct PopupUserData : UserData {
    int cid = -1;
    int mid = -1;
    std::string root_uuid;
    std::string parent_uuid;
    std::string owner_row_uuid;
    std::string child_uuid;
    std::string requested_row_uuid;
    bool closing = false;
};

struct PopOption {
    bool seperator = false; // Is just a line
    
    std::string text;
    std::string icon_left;
    std::string icon_path;

    bool is_text_icon = false;

    bool has_attempted_loadin_icon = false;

    std::vector<PopOption> submenu;

    bool closes_on_click = true;
    bool checked = false;

    std::function<void()> on_clicked = nullptr;
};

namespace popup {
    // Coordinates are global logical pixels.
    void open(std::vector<PopOption> root, int x, int y, int cid = -1);
    void close(std::string container_uuid);
    bool dismiss_outside(int x, int y);
}

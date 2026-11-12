#pragma once

#include "client/raw_windowing.h"
#include "container.h"

#include <atomic>
#include <string>

struct MylarWindow;
struct Windows;

// Set when the dock is shutting down. Worker threads poll this instead of
// owning a reference to the dock app.
extern std::atomic<bool> finished;

struct STilingMenuState {
    bool is_tiling = false;
    bool all_workspaces = true;
    bool new_workspace_is_tiling = false;
    bool border_hint = true;
    int window_gap = 8;
};

struct Dock : UserData {
    RawApp *app = nullptr;
    // Set by dock::stop; the dock stays in `docks` until its thread exits.
    bool closing = false;
    MylarWindow *window = nullptr;

    MylarWindow *applications = nullptr;

    MylarWindow *volume = nullptr;

    MylarWindow *extra = nullptr;

    MylarWindow *brightness = nullptr;

    MylarWindow *battery = nullptr;
    MylarWindow *date_menu = nullptr;

    int date_view_month = 0;
    int date_view_year = 0;
    int selected_date_day = 0;
    int selected_date_month = 0;
    int selected_date_year = 0;

    MylarWindow *wifi = nullptr;

    MylarWindow *bluetooth = nullptr;
    std::string bluetooth_input;
    std::string bluetooth_prompt;
    unsigned long bluetooth_revision = 0;

    MylarWindow *tiling = nullptr;
    STilingMenuState tiling_settings;

    MylarWindow *projection = nullptr;

    MylarWindow *tray_menu = nullptr;

    Windows *collection = nullptr;
    bool first_fill = true;
    bool thumbnails_dirty = false;
    int startup_audio_update_count = 0;
    RawWindowSettings creation_settings;
};

#pragma once

#include <string>

struct Container;
struct Dock;

extern bool nightlight_on;
// Starts or stops hyprsunset's warmer colour temperature.
void toggle_nightlight();

inline constexpr int extra_popup_w = 370;
// Tall enough for the main page's grid of tiles and the tray under it.
int extra_popup_height();

// Builds the extra menu into a popup's root. `page` is "main" or "project".
// via_keyboard: opened by a shortcut, so the Project page starts with a row
// selected and Escape closes the menu instead of going back.
void fill_extra_container(Container *root, std::string page = "main", bool via_keyboard = false);
// Opens the extra menu from the dock's "extra" button.
void open_extra_popup(Dock *dock, std::string page = "main", bool via_keyboard = false);

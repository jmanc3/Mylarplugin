#pragma once

#include <string>
#include <vector>

struct ConfigSettings;
typedef struct _GtkWindow GtkWindow;

// The GTK file chooser settings uses to pick a wallpaper image. Blocks until the
// dialog closes and returns the chosen path, or "" when cancelled.
std::string choose_file(bool folder = false, GtkWindow *parent = nullptr);

namespace settings {
    // focus_search: put the keyboard cursor in the search field, ready for typing.
    void start(std::string page = "", bool focus_search = false);
    void stop();
    void load_save_settings(bool save, ConfigSettings* settings);
    // Call on the main thread right before changing set->monitors. The change
    // is rolled back unless the user keeps it from the display settings page.
    void display_change_begin();

    // A setting found by the settings search, usable outside the settings window.
    struct SearchResult {
        std::string page;
        std::string section;
        std::string title;
        std::string description;
        std::string breadcrumb; // "Page  ›  Section" as shown in the settings results
    };
    // Same matching and ranking as the search field in the settings window.
    // Safe to call from any thread.
    std::vector<SearchResult> search(const std::string &query, int limit = 8);
    // Opens settings on the result's page, scrolls to the row and flashes it.
    // Call on the main thread, like start.
    void open_search_result(const SearchResult &result);
    // Decodes the wallpaper files into memory ahead of the Display page.
    void preload_wallpapers();
    // Copies an image to the shared wallpaper (or target monitor's override)
    // and reloads it. Blocking (runs `file`), so call off the UI/main thread.
    bool set_wallpaper_from_file(const std::string &file, const std::string &target = "");
};

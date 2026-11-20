#pragma once

#include <string>
#include <vector>

struct Container;

struct ApplicationEntry {
    std::string name;
    std::string lowercase_name;
    std::string generic_name;
    std::vector<std::string> keywords;
    std::string full_path;
    std::string icon;
    bool is_desktop = false;
    bool other_desktop = false;
};

void fill_applications_container(Container *root);

// Starts (or keeps) the asynchronous application scan used by the launcher.
void preload_scripts();

// Returns the latest scanned .desktop files and $PATH executables (excluding
// Mylar's built-in entries and settings). Triggers an async refresh when
// `refresh` is true or no scan has run yet; `*scan_in_progress` is true while
// a background scan is still running.
std::vector<ApplicationEntry> get_application_entries(bool refresh = false, bool *scan_in_progress = nullptr);

// Match score shared by the applications menu and pin command picker.
// Lower is better, -1 means no match.
int match_application(const ApplicationEntry &entry, const std::string &text, const std::string &lowercase_text);

// Resolves the icon file path for an ApplicationEntry at `icon_size` pixels.
std::string resolve_application_icon(const ApplicationEntry &entry, int icon_size);

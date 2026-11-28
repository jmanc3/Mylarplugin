#pragma once

#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

struct Bounds;

namespace desktop_icons {
    void start();
    void stop();
    void deselect();
    void paint_overview(int monitor, const Bounds &bounds, float alpha);
    bool is_renaming();
    bool rename_bounds_contains(float x, float y);
    void finish_rename(bool commit);
    void reveal_in_file_manager(const std::vector<std::string> &paths);
}

// Small .desktop reader shared by the desktop icons and the dock's applications menu.
namespace desktop_file {
    struct Action {
        std::string name;
        std::string exec;
        std::string icon;
    };

    // Keys of the [Desktop Entry] group (localized ones keep their suffix, e.g. "Name[de]").
    // When `actions` is non-null, also parses [Desktop Action <id>] groups listed in Actions=
    // in the same pass. Empty when the file can't be read.
    std::unordered_map<std::string, std::string> read_entry(const std::filesystem::path &path,
                                                            std::vector<Action> *actions = nullptr);
    // Exec= without the %f, %U, ... field codes.
    std::string exec_without_field_codes(const std::string &exec);
}

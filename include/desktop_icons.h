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
}

// Small .desktop reader shared by the desktop icons and the dock's applications menu.
namespace desktop_file {
    struct Action {
        std::string name;
        std::string exec;
        std::string icon;
    };

    // Keys of the [Desktop Entry] group (localized ones keep their suffix, e.g. "Name[de]").
    // Empty when the file can't be read.
    std::unordered_map<std::string, std::string> read_entry(const std::filesystem::path &path);
    // Custom [Desktop Action <id>] entries listed in [Desktop Entry]'s Actions= key, in order.
    std::vector<Action> read_actions(const std::filesystem::path &path);
    // Exec= without the %f, %U, ... field codes.
    std::string exec_without_field_codes(const std::string &exec);
}

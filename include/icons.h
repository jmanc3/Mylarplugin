//
// Created by jmanc3 on 2/10/22.
//

#ifndef WINBAR_ICONS_H
#define WINBAR_ICONS_H

#include "container.h"
#include <algorithm>
#include <atomic>
#include <string>
#include <utility>
#include <vector>

static bool starts_with(const std::string& str, const std::string& prefix) {
    // Check if str is long enough to contain the prefix
    return str.size() >= prefix.size() && str.compare(0, prefix.size(), prefix) == 0;
}

extern std::atomic<bool> icons_loaded;

bool icon_cache_needs_update();
bool icon_cache_generate();
bool icon_cache_load();

// Remove all icons from memory
void unload_icons();

void generate_cache();

enum IconContext {
    Actions,
    Animations,
    Apps,
    Categories,
    Devices,
    Emblems,
    Emotes,
    Intl,
    Mimetypes,
    Places,
    Statuses,
    Panel,

    NotSet
};

struct Candidate {
    std::string parent_path;
    std::string filename;
    std::string theme;
    int extension;
    int size;
    int scale;
    IconContext context;

    [[nodiscard]] std::string full_path() const {
        std::string temp = std::string(parent_path).append("/").append(filename);
        if (extension == 0) {
            temp.append(".svg");
        } else if (extension == 1) {
            temp.append(".png");
        } else if (extension == 2) {
            temp.append(".xpm");
        }
        temp.erase(std::remove(temp.begin(), temp.end(), '\0'), temp.end());

        return temp;
    }

    int size_index = 10;
    bool is_part_of_current_theme = false;
    bool is_part_of_preferred_theme = false;
    bool is_part_of_target_context = false;
};

struct IconTarget {
    bool was_searched = false;

    std::string name;
    std::vector<Candidate> candidates;

    std::string best_full_path;

    void* user_data = nullptr;

    IconTarget(std::string name) : name(std::move(name)) {}

    IconTarget(std::string name, void* user_data) : name(std::move(name)), user_data(user_data) {}
};

void search_icons(std::vector<IconTarget>& targets);

void pick_best(std::vector<IconTarget>& targets, int size);

void pick_best(std::vector<IconTarget>& targets, int size, IconContext target_context);

std::string one_shot_icon(int size, const std::vector<std::string>& alt_names);

bool has_options(const std::string& name);

void get_options(std::vector<std::string>& names, const std::string& name, int max);

std::string c3ic_fix_desktop_file_icon(const std::string& given_name, const std::string& given_wm_class, const std::string& given_path, const std::string& given_icon);

std::string c3ic_fix_wm_class(const std::string& given_wm_class);

std::string single_shot_icon_live(std::string icon, int size); 

#endif // WINBAR_ICONS_H

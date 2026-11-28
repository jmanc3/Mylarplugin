#include "desktop_icons.h"
#include "client/raw_windowing.h"
#include "client/windowing.h"
#include "container.h"
#include "dock/dock_ui.h"
#include "events.h"
#include "heart.h"
#include "hypriso.h"
#include "icons.h"
#include "overview.h"
#include "pango_font_cache.h"
#include "popup.h"
#include "settings.h"
#include "text_editor.h"
#include "wallpaper_layout.h"

#include <gio/gio.h>
#include <xkbcommon/xkbcommon-keysyms.h>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <hyprutils/path/Path.hpp>
#include <linux/input-event-codes.h>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

static RGBA color_sel_color() {
    static RGBA default_color("99eeff25");
    return hypriso->get_varcolor("plugin:mylardesktop:sel_color", default_color);
}

static RGBA color_sel_border_color() {
    static RGBA default_color("99eeffff");
    return hypriso->get_varcolor("plugin:mylardesktop:sel_border_color", default_color);
}

static float conf_font_size() {
    return hypriso->get_varfloat("plugin:mylardesktop:desktop_font_size", 14);
}

static float conf_icon_size() {
    return hypriso->get_varfloat("plugin:mylardesktop:desktop_icon_size", 48);
}

static float conf_pad() {
    return hypriso->get_varfloat("plugin:mylardesktop:desktop_pad", 12);
}

static bool conf_vertical() {
    if (set->desktop_vertical_override == 0 || set->desktop_vertical_override == 1)
        return set->desktop_vertical_override == 1;
    return hypriso->get_varint("plugin:mylardesktop:desktop_vertical", 1) != 0;
}

static std::string conf_sort_by() {
    const auto& mode = set->desktop_sort_by;
    if (mode == "size" || mode == "type" || mode == "date_modified")
        return mode;
    return "name";
}

static std::string conf_desktop_folder() {
    return set->desktop_folder;
}

static float conf_total_w() {
    return 74;
}

static float conf_total_h() {
    return 84;
}

static float horiz_pad() {
    return 3;
}

static float vert_pad() {
    return 16;
}

// Label height in logical pixels; scale only when rendering.
static int two_line_height = 24;
 
struct DesktopItem {
    std::string full_filepath;
    std::string name;
    std::string extension;
    std::string icon;
    std::string exec;
    bool is_folder = false;
    std::vector<std::string> icons_for_mime;
    std::string name_sort_key;
    std::string type_sort_key;
    std::optional<goffset> sort_size;
    std::optional<std::pair<guint64, guint32>> sort_modified;
};

static std::vector<DesktopItem*> desktop_items;

static void clear_desktop_items() {
    for (auto* item : desktop_items)
        delete item;
    desktop_items.clear();
}

static std::string lowercase(std::string value) {
    std::ranges::transform(value, value.begin(), [](const unsigned char character) {
        return std::tolower(character);
    });
    return value;
}

static std::string natural_sort_key(const std::string& value) {
    auto *valid = g_utf8_make_valid(value.data(), value.size());
    auto *folded = g_utf8_casefold(valid, -1);
    auto *key = g_utf8_collate_key_for_filename(folded, -1);
    std::string result(key);
    g_free(key);
    g_free(folded);
    g_free(valid);
    return result;
}

static void refresh_sort_metadata(DesktopItem& item) {
    item.name_sort_key = natural_sort_key(item.name);
    item.type_sort_key.clear();
    item.sort_size.reset();
    item.sort_modified.reset();

    auto *file = g_file_new_for_path(item.full_filepath.c_str());
    auto *info = g_file_query_info(file,
        G_FILE_ATTRIBUTE_STANDARD_SIZE "," G_FILE_ATTRIBUTE_STANDARD_CONTENT_TYPE ","
        G_FILE_ATTRIBUTE_TIME_MODIFIED "," G_FILE_ATTRIBUTE_TIME_MODIFIED_USEC,
        G_FILE_QUERY_INFO_NONE, nullptr, nullptr);
    g_object_unref(file);
    if (!info)
        return;

    if (!item.is_folder && g_file_info_has_attribute(info, G_FILE_ATTRIBUTE_STANDARD_SIZE))
        item.sort_size = g_file_info_get_size(info);
    if (g_file_info_has_attribute(info, G_FILE_ATTRIBUTE_TIME_MODIFIED)) {
        item.sort_modified = std::pair{
            g_file_info_get_attribute_uint64(info, G_FILE_ATTRIBUTE_TIME_MODIFIED),
            g_file_info_get_attribute_uint32(info, G_FILE_ATTRIBUTE_TIME_MODIFIED_USEC)};
    }
    if (const auto *type = g_file_info_get_content_type(info)) {
        auto *description = g_content_type_get_description(type);
        if (description) {
            item.type_sort_key = natural_sort_key(description);
            g_free(description);
        }
    }
    g_object_unref(info);
}

static void sort_desktop_items() {
    const auto mode = conf_sort_by();
    const bool ascending = set->desktop_sort_ascending;
    std::ranges::sort(desktop_items, [&mode, ascending](const DesktopItem* lhs, const DesktopItem* rhs) {
        if (lhs->is_folder != rhs->is_folder)
            return lhs->is_folder;

        const auto ordered_before = [ascending](const auto& left, const auto& right) {
            return ascending ? left < right : right < left;
        };

        if (mode == "size" && !lhs->is_folder) {
            if (lhs->sort_size.has_value() != rhs->sort_size.has_value())
                return lhs->sort_size.has_value();
            if (lhs->sort_size != rhs->sort_size)
                return ordered_before(lhs->sort_size, rhs->sort_size);
        } else if (mode == "type") {
            if (lhs->type_sort_key.empty() != rhs->type_sort_key.empty())
                return !lhs->type_sort_key.empty();
            if (lhs->type_sort_key != rhs->type_sort_key)
                return ordered_before(lhs->type_sort_key, rhs->type_sort_key);
        } else if (mode == "date_modified") {
            if (lhs->sort_modified.has_value() != rhs->sort_modified.has_value())
                return lhs->sort_modified.has_value();
            if (lhs->sort_modified != rhs->sort_modified)
                return ordered_before(lhs->sort_modified, rhs->sort_modified);
        }

        if (lhs->name_sort_key != rhs->name_sort_key)
            return ordered_before(lhs->name_sort_key, rhs->name_sort_key);
        return ordered_before(lhs->full_filepath, rhs->full_filepath);
    });
}

static std::string trim_copy(std::string value);

static std::string remove_desktop_exec_field_codes(const std::string& exec) {
    std::string result;
    result.reserve(exec.size());

    for (size_t i = 0; i < exec.size(); ++i) {
        if (exec[i] != '%') {
            result += exec[i];
            continue;
        }

        if (i + 1 < exec.size()) {
            ++i;
            // "%%" is a literal percent sign.
            if (exec[i] == '%')
                result += '%';
        }
    }

    return trim_copy(std::move(result));
}

std::string desktop_file::exec_without_field_codes(const std::string& exec) {
    return remove_desktop_exec_field_codes(exec);
}

std::unordered_map<std::string, std::string> desktop_file::read_entry(const std::filesystem::path& path,
                                                                      std::vector<Action>* actions) {
    std::unordered_map<std::string, std::string> entry;
    if (actions)
        actions->clear();
    std::ifstream file(path);
    if (!file)
        return entry;

    std::unordered_map<std::string, std::unordered_map<std::string, std::string>> action_groups;
    static constexpr std::string_view kActionPrefix = "[Desktop Action ";

    std::string line;
    bool inDesktopEntry = false;
    std::string current_action_id;
    while (std::getline(file, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.empty() || line[0] == '#')
            continue;
        if (line.starts_with('[') && line.ends_with(']')) {
            inDesktopEntry = (line == "[Desktop Entry]");
            if (actions && line.starts_with(kActionPrefix) && line.size() > kActionPrefix.size() + 1)
                current_action_id = line.substr(kActionPrefix.size(), line.size() - kActionPrefix.size() - 1);
            else
                current_action_id.clear();
            continue;
        }
        if (!inDesktopEntry && current_action_id.empty())
            continue;

        const auto separator = line.find('=');
        if (separator == std::string::npos)
            continue;

        // Spaces around the '=' are not part of the key or the value.
        auto key = trim_copy(line.substr(0, separator));
        if (key.empty())
            continue;
        auto value = trim_copy(line.substr(separator + 1));

        if (inDesktopEntry)
            entry[std::move(key)] = std::move(value);
        else
            action_groups[current_action_id][std::move(key)] = std::move(value);
    }

    if (actions) {
        if (auto it = entry.find("Actions"); it != entry.end() && !it->second.empty()) {
            const auto& actions_list = it->second;
            size_t start = 0;
            while (start < actions_list.size()) {
                size_t end = actions_list.find(';', start);
                if (end == std::string::npos)
                    end = actions_list.size();
                auto id = trim_copy(actions_list.substr(start, end - start));
                start = end + 1;
                if (id.empty())
                    continue;

                auto group_it = action_groups.find(id);
                if (group_it == action_groups.end())
                    continue;
                const auto& group = group_it->second;
                auto name_it = group.find("Name");
                auto exec_it = group.find("Exec");
                if (name_it == group.end() || name_it->second.empty() || exec_it == group.end())
                    continue;
                auto exec = remove_desktop_exec_field_codes(exec_it->second);
                if (exec.empty())
                    continue;

                Action action;
                action.name = name_it->second;
                action.exec = std::move(exec);
                if (auto icon_it = group.find("Icon"); icon_it != group.end())
                    action.icon = icon_it->second;
                actions->push_back(std::move(action));
            }
        }
    }
    return entry;
}

static void read_desktop_file(DesktopItem& item, const std::filesystem::path& path) {
    auto entry = desktop_file::read_entry(path);
    if (auto it = entry.find("Name"); it != entry.end())
        item.name = it->second;
    if (auto it = entry.find("Icon"); it != entry.end())
        item.icon = it->second;
    if (auto it = entry.find("Exec"); it != entry.end())
        item.exec = remove_desktop_exec_field_codes(it->second);
}

static void refresh_desktop_item(DesktopItem& item, const std::filesystem::path& path, bool isFolder) {
    item.full_filepath = path.string();
    item.name = isFolder ? path.filename().string() : path.stem().string();
    item.extension = isFolder ? std::string{} : lowercase(path.extension().string());
    item.icon.clear();
    item.exec.clear();
    item.is_folder = isFolder;

    if (item.extension == ".desktop")
        read_desktop_file(item, path);
    refresh_sort_metadata(item);
}

static void add_icon_candidate(std::vector<std::string>& candidates, const std::string& candidate) {
    if (!candidate.empty())
        candidates.push_back(candidate);
}

static void add_desktop_file_icon_candidates(DesktopItem& item) {
    if (item.extension != ".desktop")
        return;

    const std::filesystem::path path(item.full_filepath);
    std::vector<std::string> candidates;
    add_icon_candidate(candidates, item.icon);
    add_icon_candidate(candidates, lowercase(item.icon));
    add_icon_candidate(candidates, item.name);
    add_icon_candidate(candidates, lowercase(item.name));
    add_icon_candidate(candidates, item.exec);
    add_icon_candidate(candidates, path.stem().string());
    add_icon_candidate(candidates, lowercase(path.stem().string()));
    item.icons_for_mime.insert(item.icons_for_mime.begin(), candidates.begin(), candidates.end());
}

static void add_mime_icon_candidates(DesktopItem& item) {
    auto file = g_file_new_for_path(item.full_filepath.c_str());
    GFileInfo *info = g_file_query_info(file, G_FILE_ATTRIBUTE_STANDARD_ICON, G_FILE_QUERY_INFO_NONE, NULL, nullptr);

    if (!info) {
        g_object_unref(file);
        return;
    }

    gchar **attrs = g_file_info_list_attributes(info, nullptr);

    for (int i = 0; attrs[i] != nullptr; ++i) {
        const char *name = attrs[i];
        GFileAttributeType type = g_file_info_get_attribute_type(info, name);

        if (type != G_FILE_ATTRIBUTE_TYPE_OBJECT)
            continue;

        GObject *obj = g_file_info_get_attribute_object(info, name);
        if (!G_IS_THEMED_ICON(obj))
            continue;

        const gchar * const *names = g_themed_icon_get_names(G_THEMED_ICON(obj));
        for (int j = 0; names[j] != nullptr; ++j)
            item.icons_for_mime.push_back(names[j]);
    }

    g_strfreev(attrs);
    g_object_unref(info);
    g_object_unref(file);
}

static int latest_desktop_watch = -1;

void on_change_in_desktop_folder() {
    //notify("change in desktop foldler");
    auto folder = Hyprutils::Path::resolvePath(conf_desktop_folder());
    if (!folder.has_value()) {
        clear_desktop_items();
        return;
    }

    std::error_code ec;
    if (!std::filesystem::is_directory(folder.value(), ec)) {
        clear_desktop_items();
        return;
    }

    std::vector<DesktopItem> scanned;
    std::unordered_set<std::string> seenPaths;
    std::filesystem::directory_iterator iterator(folder.value(), std::filesystem::directory_options::skip_permission_denied, ec);
    const std::filesystem::directory_iterator end;

    while (!ec && iterator != end) {
        const auto& entry = *iterator;
        const auto path = entry.path();
        if (path.filename().string().starts_with('.')) {
            iterator.increment(ec);
            continue;
        }

        const bool isFolder = entry.is_directory(ec);
        if (ec) {
            ec.clear();
            iterator.increment(ec);
            continue;
        }

        const auto fullPath = path.string();
        seenPaths.insert(fullPath);
        scanned.emplace_back();
        refresh_desktop_item(scanned.back(), path, isFolder);
        add_mime_icon_candidates(scanned.back());
        add_desktop_file_icon_candidates(scanned.back());

        iterator.increment(ec);
    }

    for (size_t i = 0; i < desktop_items.size();) {
        if (!seenPaths.contains(desktop_items[i]->full_filepath)) {
            delete desktop_items[i];
            desktop_items.erase(desktop_items.begin() + i);
            continue;
        }
        ++i;
    }

    std::unordered_map<std::string, DesktopItem*> existing;
    existing.reserve(desktop_items.size());
    for (auto* item : desktop_items)
        existing.emplace(item->full_filepath, item);

    for (const auto& scannedItem : scanned) {
        auto it = existing.find(scannedItem.full_filepath);
        if (it != existing.end()) {
            auto* item = it->second;
            *item = scannedItem;
            continue;
        }

        auto item = new DesktopItem(scannedItem);
        desktop_items.push_back(item);
    }

    sort_desktop_items();
}

void watch_desktop_folder() {
    auto folder = Hyprutils::Path::resolvePath(conf_desktop_folder());
    if (!folder.has_value()) {
        clear_desktop_items();
        return;
    }
    if (!std::filesystem::exists(folder.value()))
        return;
    static Timer *t = nullptr;
    static long time_made = 0;
    
    latest_desktop_watch = watch_file(folder.value(), [](FileWatchUpdate update, int fd) {
        if (t) {
            auto current = get_current_time_in_ms();
            if ((current - time_made) > 1200) { // allow a reset if timer still exists after a second
                t = nullptr;
            } else {
                return;
            }
        }
            
        t = later(1000, [](Timer *) {
            on_change_in_desktop_folder();
            
            damage_all();
            t = nullptr;
        });
        time_made = get_current_time_in_ms();
    });
}

struct IcoContainerData : UserData {
    std::string name;
    Container *c;
    long last_time_pressed = 0;
    bool is_selected = false;
    bool is_renaming = false;
    // Grid slot; (0,0) is top-left, (0,1) is the slot below. (-1,-1) = unset.
    int x_pos = -1;
    int y_pos = -1;
    float drag_offset_x = 0;
    float drag_offset_y = 0;
    bool is_dragging = false;
    bool is_settling = false;
    float settle_from_x = 0;
    float settle_from_y = 0;
    long settle_start_ms = 0;
    // Open feedback: icon and label punch in and spring back (see punch_scale).
    bool is_punching = false;
    long punch_start_ms = 0;
    TextureInfo icon_shadow;
    TextureInfo label_shadow;
    int icon_shadow_source = -1;
    
    float texture_scale = 0;

    void clear_textures() {
        for (const auto *key : {"label", "icon", "folder", "text-plain"}) {
            auto *texture = datum<TextureInfo>(c, key);
            free_text_texture(texture->id);
            *texture = TextureInfo();
        }
        free_text_texture(icon_shadow.id);
        free_text_texture(label_shadow.id);
        icon_shadow = TextureInfo();
        label_shadow = TextureInfo();
        icon_shadow_source = -1;
        *datum<bool>(c, "icon_attempted") = false;
    }

    ~IcoContainerData() {
        clear_textures();
    }
};

static constexpr float ICON_SETTLE_MS = 90.f;

static long long grid_key(int x, int y) {
    return (static_cast<long long>(x) << 32) ^ static_cast<unsigned int>(y);
}

static void damage_icon(Container *icon) {
    auto b = icon->real_bounds;
    b.grow(20);
    hypriso->damage_box(b);
}

// Taken from calibre's cover grid (gui2/library/alternate_views.py, CoverDelegate):
// a 500ms QPropertyAnimation with key values 1.0 -> 0.5 (at 0.5) -> 1.0 and
// QEasingCurve::OutInCirc applied to the whole progress before keyframe lookup.
static constexpr float ICON_PUNCH_MS = 500.f;
static constexpr float ICON_PUNCH_DEPTH = 0.5f;
// When false only the icon punches; the label stays put.
static constexpr bool include_label = true;

static float punch_scale(float t) {
    t = std::clamp(t, 0.f, 1.f);
    const float u = 2.f * t - 1.f;
    const float root = std::sqrt(std::max(0.f, 1.f - u * u));
    // Qt easeOutInCirc: easeOutCirc(2t)/2, then easeInCirc(2t-1)/2 + 0.5.
    const float e = t < .5f ? root * .5f : (1.f - root) * .5f + .5f;
    if (e < .5f)
        return 1.f + (ICON_PUNCH_DEPTH - 1.f) * (e / .5f);
    return ICON_PUNCH_DEPTH + (1.f - ICON_PUNCH_DEPTH) * ((e - .5f) / .5f);
}

static void begin_icon_punch(Container *icon) {
    auto *ico = (IcoContainerData *)icon->user_data;
    ico->is_punching = true;
    ico->punch_start_ms = get_current_time_in_ms();
    damage_icon(icon);
    request_refresh();
}

static void punch_icons_for_paths(const std::vector<std::string> &paths) {
    for (auto *desktop : actual_root->children) {
        if (desktop->custom_type != (int) TYPE::DESKTOP_ICONS)
            continue;
        for (auto *icon : desktop->children) {
            auto *item = *datum<DesktopItem *>(icon, "DesktopItem");
            if (item && std::find(paths.begin(), paths.end(), item->full_filepath) != paths.end())
                begin_icon_punch(icon);
        }
    }
}

static void begin_icon_settle(Container *icon) {
    auto *ico = (IcoContainerData *)icon->user_data;
    ico->is_settling = true;
    ico->settle_from_x = icon->real_bounds.x;
    ico->settle_from_y = icon->real_bounds.y;
    ico->settle_start_ms = get_current_time_in_ms();
    damage_icon(icon);
    request_refresh();
}

static std::string trim_copy(std::string value) {
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())))
        value.erase(value.begin());
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())))
        value.pop_back();
    return value;
}

static std::filesystem::path icon_positions_conf_path() {
    const char *home = std::getenv("HOME");
    if (!home)
        return {};
    return std::filesystem::path(home) / ".config/mylar/desktop_icons.conf";
}

static std::string current_desktop_section_key() {
    auto folder = Hyprutils::Path::resolvePath(conf_desktop_folder());
    if (folder.has_value())
        return folder.value();
    return conf_desktop_folder();
}

static std::string icon_entry_name(DesktopItem *item) {
    return std::filesystem::path(item->full_filepath).filename().string();
}

// section path -> entry name -> (x, y)
using IconPosMap = std::unordered_map<std::string, std::pair<int, int>>;
using IconPosSections = std::map<std::string, IconPosMap>;

static IconPosSections read_icon_position_sections() {
    IconPosSections sections;
    const auto path = icon_positions_conf_path();
    if (path.empty() || !std::filesystem::exists(path))
        return sections;

    std::ifstream in(path);
    if (!in)
        return sections;

    std::string line;
    std::string current;
    while (std::getline(in, line)) {
        line = trim_copy(std::move(line));
        if (line.empty() || line[0] == '#')
            continue;

        if (line.front() == '[' && line.back() == ']') {
            current = line.substr(1, line.size() - 2);
            sections.try_emplace(current);
            continue;
        }

        if (current.empty())
            continue;

        const auto eq = line.find('=');
        if (eq == std::string::npos)
            continue;

        const std::string name = trim_copy(line.substr(0, eq));
        const std::string rest = trim_copy(line.substr(eq + 1));
        if (name.empty())
            continue;

        const auto comma = rest.find(',');
        if (comma == std::string::npos)
            continue;

        try {
            const int x = std::stoi(trim_copy(rest.substr(0, comma)));
            const int y = std::stoi(trim_copy(rest.substr(comma + 1)));
            sections[current][name] = {x, y};
        } catch (...) {
            continue;
        }
    }

    return sections;
}

static void write_icon_position_sections(const IconPosSections &sections) {
    const auto path = icon_positions_conf_path();
    if (path.empty())
        return;

    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::trunc);
    if (!out)
        return;

    out << "# mylar desktop icon positions (x,y grid slots per desktop folder)\n";
    for (const auto &[section, entries] : sections) {
        out << '\n' << '[' << section << "]\n";
        for (const auto &[name, pos] : entries)
            out << name << '=' << pos.first << ',' << pos.second << '\n';
    }
}

static void save_icon_positions(Container *desktop) {
    if (!desktop)
        return;

    const auto section = current_desktop_section_key();
    if (section.empty())
        return;

    auto sections = read_icon_position_sections();
    IconPosMap current;
    current.reserve(desktop->children.size());
    for (auto *child : desktop->children) {
        auto *ico = (IcoContainerData *)child->user_data;
        if (ico->x_pos < 0 || ico->y_pos < 0)
            continue;
        DesktopItem *item = *datum<DesktopItem *>(child, "DesktopItem");
        if (!item)
            continue;
        current[icon_entry_name(item)] = {ico->x_pos, ico->y_pos};
    }
    sections[section] = std::move(current);
    write_icon_position_sections(sections);
}

// Loads persisted grid positions into icons (keyed by entry name within the desktop folder section).
static void load_icon_positions_from_save(Container *desktop) {
    if (!desktop)
        return;

    const auto section = current_desktop_section_key();
    if (section.empty())
        return;

    const auto sections = read_icon_position_sections();
    const auto it = sections.find(section);
    if (it == sections.end())
        return;

    std::unordered_set<long long> occupied;
    occupied.reserve(desktop->children.size());
    for (auto *child : desktop->children) {
        auto *ico = (IcoContainerData *)child->user_data;
        if (ico->x_pos < 0 || ico->y_pos < 0)
            continue;
        occupied.insert(grid_key(ico->x_pos, ico->y_pos));
    }

    for (auto *child : desktop->children) {
        auto *ico = (IcoContainerData *)child->user_data;
        if (ico->x_pos >= 0 && ico->y_pos >= 0)
            continue;

        DesktopItem *item = *datum<DesktopItem *>(child, "DesktopItem");
        if (!item)
            continue;

        const auto pit = it->second.find(icon_entry_name(item));
        if (pit == it->second.end())
            continue;

        const int x = pit->second.first;
        const int y = pit->second.second;
        if (x < 0 || y < 0)
            continue;
        if (occupied.contains(grid_key(x, y)))
            continue;

        ico->x_pos = x;
        ico->y_pos = y;
        occupied.insert(grid_key(x, y));
    }
}

static std::pair<int, int> next_available_slot(const std::unordered_set<long long> &occupied, int cols, int rows, bool vertical) {
    cols = std::max(1, cols);
    rows = std::max(1, rows);
    if (vertical) {
        for (int x = 0;; ++x) {
            for (int y = 0; y < rows; ++y) {
                if (!occupied.contains(grid_key(x, y)))
                    return {x, y};
            }
        }
    }
    for (int y = 0;; ++y) {
        for (int x = 0; x < cols; ++x) {
            if (!occupied.contains(grid_key(x, y)))
                return {x, y};
        }
    }
}

struct IconGridMetrics {
    float width = 0;
    float height = 0;
    float hpad = 0;
    float vpad = 0;
    float start_x = 0;
    float start_y = 0;
    int cols = 1;
    int rows = 1;
};

static IconGridMetrics icon_grid_metrics(Container *desktop, float s) {
    IconGridMetrics m;
    m.hpad = horiz_pad();
    m.vpad = vert_pad();
    auto ico_width = conf_icon_size();
    m.width = ico_width + (13 * 2);
    m.height = ico_width + two_line_height + 13;
    m.start_x = desktop->real_bounds.x + m.hpad;
    m.start_y = desktop->real_bounds.y + m.vpad * .3f;
    const float cell_w = m.width + m.hpad;
    const float cell_h = m.height + m.vpad;
    m.cols = std::max(1, static_cast<int>((desktop->real_bounds.w - m.hpad) / cell_w));
    m.rows = std::max(1, static_cast<int>((desktop->real_bounds.h - m.vpad * .3f) / cell_h));
    return m;
}

static Bounds bounds_for_grid_slot(const IconGridMetrics &m, int x_pos, int y_pos) {
    return Bounds(m.start_x + x_pos * (m.width + m.hpad),
                  m.start_y + y_pos * (m.height + m.vpad),
                  m.width,
                  m.height);
}

static void clear_desktop_selection(Container *desktop);

static std::pair<int, int> nearest_free_slot(const std::unordered_set<long long> &occupied, int prefer_x, int prefer_y) {
    prefer_x = std::max(0, prefer_x);
    prefer_y = std::max(0, prefer_y);
    if (!occupied.contains(grid_key(prefer_x, prefer_y)))
        return {prefer_x, prefer_y};

    for (int radius = 1; radius < 256; ++radius) {
        for (int dy = -radius; dy <= radius; ++dy) {
            for (int dx = -radius; dx <= radius; ++dx) {
                if (std::max(std::abs(dx), std::abs(dy)) != radius)
                    continue;
                const int x = prefer_x + dx;
                const int y = prefer_y + dy;
                if (x < 0 || y < 0)
                    continue;
                if (!occupied.contains(grid_key(x, y)))
                    return {x, y};
            }
        }
    }
    return {prefer_x, prefer_y};
}

// Updates grid slots only; visual position is animated via begin_icon_settle.
static void snap_icons_to_grid(Container *desktop, const std::vector<Container *> &icons) {
    if (icons.empty())
        return;

    auto monitor = *datum<int>(desktop, "monitor");
    auto m = icon_grid_metrics(desktop, scale(monitor));
    std::unordered_set<Container *> dragged(icons.begin(), icons.end());

    std::unordered_set<long long> occupied;
    occupied.reserve(desktop->children.size());
    for (auto *child : desktop->children) {
        if (dragged.contains(child))
            continue;
        auto *oico = (IcoContainerData *)child->user_data;
        if (oico->x_pos < 0 || oico->y_pos < 0)
            continue;
        occupied.insert(grid_key(oico->x_pos, oico->y_pos));
    }

    if (icons.size() == 1) {
        auto *icon = icons[0];
        auto *ico = (IcoContainerData *)icon->user_data;
        int gx = static_cast<int>(std::lround((icon->real_bounds.x - m.start_x) / (m.width + m.hpad)));
        int gy = static_cast<int>(std::lround((icon->real_bounds.y - m.start_y) / (m.height + m.vpad)));
        gx = std::max(0, gx);
        gy = std::max(0, gy);

        const int old_x = ico->x_pos;
        const int old_y = ico->y_pos;

        for (auto *other : desktop->children) {
            if (other == icon)
                continue;
            auto *oico = (IcoContainerData *)other->user_data;
            if (oico->x_pos == gx && oico->y_pos == gy) {
                damage_icon(other);
                oico->x_pos = old_x;
                oico->y_pos = old_y;
                begin_icon_settle(other);
                break;
            }
        }

        ico->x_pos = gx;
        ico->y_pos = gy;
        return;
    }

    for (auto *icon : icons) {
        auto *ico = (IcoContainerData *)icon->user_data;
        int gx = static_cast<int>(std::lround((icon->real_bounds.x - m.start_x) / (m.width + m.hpad)));
        int gy = static_cast<int>(std::lround((icon->real_bounds.y - m.start_y) / (m.height + m.vpad)));
        auto [nx, ny] = nearest_free_slot(occupied, gx, gy);
        ico->x_pos = nx;
        ico->y_pos = ny;
        occupied.insert(grid_key(nx, ny));
    }
}

static std::vector<Container *> icons_for_action(Container *desktop, Container *primary) {
    auto *pico = (IcoContainerData *)primary->user_data;
    std::vector<Container *> result;
    if (!pico->is_selected) {
        clear_desktop_selection(desktop);
        pico->is_selected = true;
        damage_icon(primary);
        result.push_back(primary);
        return result;
    }
    for (auto *child : desktop->children) {
        auto *ico = (IcoContainerData *)child->user_data;
        if (ico->is_selected)
            result.push_back(child);
    }
    if (result.empty())
        result.push_back(primary);
    return result;
}

static void clear_desktop_selection(Container* desktop) {
    for (auto* child : desktop->children) {
        auto* ico = (IcoContainerData*)(child->user_data);
        if (!ico->is_selected)
            continue;

        ico->is_selected = false;
        auto damage = child->real_bounds;
        damage.grow(2);
        hypriso->damage_box(damage);
    }
}

static void delete_selected_desktop_icons(Container *desktop) {
    std::vector<Container *> selected;
    selected.reserve(desktop->children.size());
    for (auto *child : desktop->children) {
        auto *ico = (IcoContainerData *)child->user_data;
        if (ico->is_selected)
            selected.push_back(child);
    }
    if (selected.empty())
        return;

    for (auto *icon : selected) {
        DesktopItem *item = *datum<DesktopItem *>(icon, "DesktopItem");
        if (!item)
            continue;

        GFile *file = g_file_new_for_path(item->full_filepath.c_str());
        gboolean trashed = g_file_trash(file, nullptr, nullptr);
        g_object_unref(file);

        if (trashed)
            continue;

        std::error_code ec;
        if (std::filesystem::is_symlink(item->full_filepath, ec))
            std::filesystem::remove(item->full_filepath, ec);
        else
            std::filesystem::remove_all(item->full_filepath, ec);
    }

    for (auto *icon : selected) {
        auto it = std::ranges::find(desktop->children, icon);
        if (it == desktop->children.end())
            continue;
        desktop->children.erase(it);
        delete icon;
    }

    on_change_in_desktop_folder();
    save_icon_positions(desktop);
    damage_all();
}

static void open_desktop_path(const std::string& path) {
    auto *quoted = g_shell_quote(path.c_str());
    launch_command(std::string("xdg-open ") + quoted);
    g_free(quoted);
}

static void open_desktop_item(const DesktopItem& item) {
    if (item.extension == ".desktop" && !item.exec.empty())
        launch_command(item.exec);
    else
        open_desktop_path(item.full_filepath);
}

static bool is_editable_desktop_item(const DesktopItem& item) {
    if (item.is_folder)
        return false;
    auto *file = g_file_new_for_path(item.full_filepath.c_str());
    auto *info = g_file_query_info(file,
        G_FILE_ATTRIBUTE_STANDARD_CONTENT_TYPE "," G_FILE_ATTRIBUTE_ACCESS_CAN_WRITE,
        G_FILE_QUERY_INFO_NONE, nullptr, nullptr);
    bool editable = false;
    if (info) {
        const auto *type = g_file_info_get_content_type(info);
        editable = type && g_content_type_is_a(type, "text/plain") &&
                   g_file_info_get_attribute_boolean(info, G_FILE_ATTRIBUTE_ACCESS_CAN_WRITE);
        g_object_unref(info);
    }
    g_object_unref(file);
    return editable;
}

struct DesktopRenameSession {
    std::string original_filepath;
    std::string original_extension;
    bool is_folder = false;
    bool is_desktop_file = false;
    int grid_x = -1;
    int grid_y = -1;
    Bounds box_bounds;

    RawApp *app = nullptr;
    MylarWindow *window = nullptr;
    Container *editor = nullptr;
    bool had_keyboard_focus = false;

    std::atomic<bool> finishing = false;
    std::atomic<bool> exited = false;
    std::thread thread;
};

static std::mutex desktop_rename_mutex;
static std::shared_ptr<DesktopRenameSession> active_desktop_rename;
static std::vector<std::shared_ptr<DesktopRenameSession>> finished_desktop_renames;
static std::string pending_select_filepath;

static void reap_desktop_renames_locked() {
    for (int i = (int) finished_desktop_renames.size() - 1; i >= 0; --i) {
        if (finished_desktop_renames[i]->exited.load()) {
            if (finished_desktop_renames[i]->thread.joinable())
                finished_desktop_renames[i]->thread.join();
            finished_desktop_renames.erase(finished_desktop_renames.begin() + i);
        }
    }
}

static void update_desktop_entry_name(const std::filesystem::path &path, const std::string &new_name) {
    std::ifstream in(path);
    if (!in)
        return;

    std::vector<std::string> lines;
    std::string line;
    bool in_desktop_entry = false;
    bool replaced = false;
    int desktop_entry_line = -1;

    while (std::getline(in, line)) {
        std::string clean = line;
        if (!clean.empty() && clean.back() == '\r')
            clean.pop_back();
        if (clean.starts_with('[') && clean.ends_with(']')) {
            in_desktop_entry = (clean == "[Desktop Entry]");
            if (in_desktop_entry)
                desktop_entry_line = static_cast<int>(lines.size());
        } else if (in_desktop_entry && !replaced) {
            const auto eq = clean.find('=');
            if (eq != std::string::npos && trim_copy(clean.substr(0, eq)) == "Name") {
                line = "Name=" + new_name;
                replaced = true;
            }
        }
        lines.push_back(std::move(line));
    }
    in.close();

    if (!replaced && desktop_entry_line >= 0)
        lines.insert(lines.begin() + desktop_entry_line + 1, "Name=" + new_name);
    else if (!replaced)
        return;

    std::ofstream out(path, std::ios::trunc);
    if (!out)
        return;
    for (const auto &l : lines)
        out << l << '\n';
}

static void clear_icon_renaming_flag(const std::string &filepath) {
    for (auto *desktop : actual_root->children) {
        if (desktop->custom_type != (int) TYPE::DESKTOP_ICONS)
            continue;
        for (auto *icon : desktop->children) {
            auto *ico = (IcoContainerData *) icon->user_data;
            auto *item = *datum<DesktopItem *>(icon, "DesktopItem");
            if (ico && ico->is_renaming && (!item || item->full_filepath == filepath)) {
                ico->is_renaming = false;
                damage_icon(icon);
            }
        }
    }
}

static void apply_desktop_icon_rename(const std::shared_ptr<DesktopRenameSession> &session, const std::string &raw_input) {
    const std::string new_base = trim_copy(raw_input);
    if (new_base.empty())
        return;

    if (new_base == "." || new_base == ".." ||
        new_base.find('/') != std::string::npos ||
        new_base.find('\0') != std::string::npos) {
        notify("Invalid file name.");
        return;
    }

    const std::filesystem::path old_path(session->original_filepath);
    const std::string new_filename = new_base + session->original_extension;
    const std::filesystem::path new_path = old_path.parent_path() / new_filename;

    if (new_path == old_path && !session->is_desktop_file)
        return;

    std::error_code ec;
    if (new_path != old_path) {
        const auto existing_status = std::filesystem::symlink_status(new_path, ec);
        if (!ec && std::filesystem::exists(existing_status)) {
            notify(fz("A file named \"{}\" already exists.", new_filename));
            return;
        }
        ec.clear();
        std::filesystem::rename(old_path, new_path, ec);
        if (ec) {
            notify(fz("Unable to rename: {}", ec.message()));
            return;
        }
    }

    if (session->is_desktop_file) {
        std::error_code sym_ec;
        if (!std::filesystem::is_symlink(new_path, sym_ec) && !sym_ec &&
            std::filesystem::is_regular_file(new_path, sym_ec) && !sym_ec) {
            update_desktop_entry_name(new_path, new_base);
        }
    }

    if (session->grid_x >= 0 && session->grid_y >= 0) {
        const auto section = current_desktop_section_key();
        if (!section.empty()) {
            auto sections = read_icon_position_sections();
            auto &saved = sections[section];
            saved.erase(old_path.filename().string());
            saved[new_path.filename().string()] = {session->grid_x, session->grid_y};
            write_icon_position_sections(sections);
        }
    }

    pending_select_filepath = new_path.string();
    on_change_in_desktop_folder();
    heart::layout_containers();
    damage_all();
    request_refresh();
}

static void finish_rename_from_client(const std::shared_ptr<DesktopRenameSession> &session, bool commit, std::string new_text) {
    if (session->finishing.exchange(true))
        return;

    if (session->window && session->window->raw_window)
        windowing::close_window(session->window->raw_window);
    if (session->app)
        windowing::close_app(session->app);

    main_thread([session, commit, new_text = std::move(new_text)]() {
        {
            std::lock_guard<std::mutex> lock(desktop_rename_mutex);
            if (active_desktop_rename == session) {
                finished_desktop_renames.push_back(std::move(active_desktop_rename));
                active_desktop_rename.reset();
            }
            reap_desktop_renames_locked();
        }
        clear_icon_renaming_flag(session->original_filepath);
        if (commit)
            apply_desktop_icon_rename(session, new_text);
        damage_all();
        request_refresh();
    });
}

static void run_desktop_rename_session(std::shared_ptr<DesktopRenameSession> session,
                                       int width, int height,
                                       int margin_left, int margin_top,
                                       float scale_hint,
                                       std::string monitor_name,
                                       int font_size,
                                       RGBA border_color) {
    auto *app = windowing::open_app();
    if (!app) {
        session->exited = true;
        finish_rename_from_client(session, false, {});
        return;
    }

    RawWindowSettings settings;
    settings.pos.w = width;
    settings.pos.h = height;
    settings.name = "DesktopRename";
    settings.monitor_name = std::move(monitor_name);
    settings.scale_hint = scale_hint;
    settings.layer_margins.left = margin_left;
    settings.layer_margins.top = margin_top;
    settings.keyboard_focus_on_click = true;

    auto *mylar = open_mylar_window(app, WindowType::OVERLAY, settings);
    if (!mylar || !mylar->raw_window) {
        windowing::close_app(app);
        windowing::main_loop(app);
        delete app;
        session->exited = true;
        finish_rename_from_client(session, false, {});
        return;
    }

    {
        std::lock_guard<std::mutex> lock(desktop_rename_mutex);
        session->app = app;
        session->window = mylar;
    }

    {
        std::lock_guard<std::mutex> lock(app->mutex);
        Container *root = mylar->root;
        root->type = ::hbox;

        Container *editor = root->child(FILL_SPACE, FILL_SPACE);
        session->editor = editor;

        STextEditorOptions options;
        options.multiline = false;
        options.editable = true;
        options.blink_caret = true;
        options.clear_button = false;
        options.font_size = font_size;
        options.window = [mylar](Container *) { return mylar->raw_window; };
        options.viewport = [mylar](Container *, Container *c) {
            const double dpi = mylar->raw_window->dpi;
            auto b = c->real_bounds;
            b.x += 6 * dpi;
            b.w = std::max(0.0, b.w - 12 * dpi);
            return b;
        };
        setup_text_editor(editor, "", std::move(options));

        editor->when_paint = [mylar, border_color](Container *root, Container *c) {
            auto *rw = mylar->raw_window;
            auto *cr = rw->drawing_context;
            const double dpi = rw->dpi;
            const auto b = c->real_bounds;
            if (b.w <= 0 || b.h <= 0)
                return;
            const double inset = std::max(1.0, std::round(dpi));
            const double radius = 4.0 * dpi;
            set_argb(cr, RGBA(0.11, 0.12, 0.14, 0.94));
            drawRoundedRect(cr, b.x + inset * 0.5, b.y + inset * 0.5,
                            std::max(0.0, b.w - inset), std::max(0.0, b.h - inset),
                            radius, 1);
            cr->fill_preserve();
            set_argb(cr, border_color);
            cr->set_line_width(inset);
            cr->stroke();
            paint_text_editor(root, c, RGBA(1, 1, 1, 1), RGBA(0.25, 0.55, 0.90, 0.65));
        };

        auto edit_key = std::move(editor->when_key_event);
        editor->when_key_event = [edit_key, session, mylar](Container *root, Container *c,
                                                            int key, bool pressed,
                                                            xkb_keysym_t sym, int mods,
                                                            bool is_text, std::string text) {
            if (pressed && (sym == XKB_KEY_Return || sym == XKB_KEY_KP_Enter)) {
                auto value = static_cast<STextEditorData *>(c->user_data)->text;
                finish_rename_from_client(session, true, std::move(value));
                return;
            }
            if (pressed && sym == XKB_KEY_Escape) {
                finish_rename_from_client(session, false, {});
                return;
            }
            edit_key(root, c, key, pressed, sym, mods, is_text, text);
            windowing::redraw(mylar->raw_window);
        };

        auto prev_focus = mylar->raw_window->on_keyboard_focus;
        mylar->raw_window->on_keyboard_focus = [prev_focus, session](RawWindow *rw, bool gained) {
            if (prev_focus)
                prev_focus(rw, gained);
            if (gained) {
                session->had_keyboard_focus = true;
                windowing::redraw(rw);
            } else if (session->had_keyboard_focus && !session->finishing.load()) {
                std::string value;
                {
                    std::lock_guard<std::mutex> lock(rw->creator->mutex);
                    if (session->editor && session->editor->user_data)
                        value = static_cast<STextEditorData *>(session->editor->user_data)->text;
                }
                finish_rename_from_client(session, true, std::move(value));
            }
            return false;
        };

        root->on_closed = [session](Container *) {
            finish_rename_from_client(session, false, {});
        };

        set_active(root, {editor}, root, true, false);
    }

    windowing::request_keyboard_focus(mylar->raw_window);
    windowing::redraw(mylar->raw_window);
    windowing::main_loop(app);

    {
        std::lock_guard<std::mutex> lock(desktop_rename_mutex);
        session->app = nullptr;
        session->window = nullptr;
        session->editor = nullptr;
    }

    delete mylar;
    cleanup_cached_pango_fonts();
    drawing::cleanup_thread_resources();
    session->exited = true;
}

static void start_desktop_icon_rename(Container *desktop, Container *icon) {
    if (!desktop || !icon)
        return;
    auto *item = *datum<DesktopItem *>(icon, "DesktopItem");
    auto *ico = (IcoContainerData *) icon->user_data;
    if (!item || !ico)
        return;

    desktop_icons::finish_rename(false);

    clear_desktop_selection(desktop);
    ico->is_selected = true;
    ico->is_renaming = true;
    ico->last_time_pressed = 0;
    damage_icon(icon);

    const int monitor = *datum<int>(desktop, "monitor");
    const Bounds mon = bounds_monitor(monitor);
    const float s = scale(monitor);
    const std::string monitor_name = hypriso->monitor_name(monitor);

    const double box_w = std::max(icon->real_bounds.w + 24.0, 128.0);
    const double box_h = std::max<double>(two_line_height + 4.0, 28.0);
    double box_x = std::round(icon->real_bounds.x + (icon->real_bounds.w - box_w) * 0.5);
    double box_y = std::round(icon->real_bounds.y + 2.0 + conf_icon_size() + 3.0);
    if (mon.w > box_w + 8.0)
        box_x = std::clamp(box_x, mon.x + 4.0, mon.x + mon.w - box_w - 4.0);
    if (mon.h > box_h + 8.0)
        box_y = std::clamp(box_y, mon.y + 4.0, mon.y + mon.h - box_h - 4.0);

    const int margin_left = static_cast<int>(std::lround(box_x - mon.x));
    const int margin_top = static_cast<int>(std::lround(box_y - mon.y));
    const int font_size = std::max(8, static_cast<int>(std::lround(conf_font_size() * 0.75f)));
    const RGBA border_color = color_sel_border_color();

    const std::filesystem::path orig_path(item->full_filepath);
    std::string raw_ext;
    if (!item->is_folder) {
        const std::string stem = orig_path.stem().string();
        const std::string ext = orig_path.extension().string();
        if (!stem.empty() && !ext.empty())
            raw_ext = ext;
    }

    auto session = std::make_shared<DesktopRenameSession>();
    session->original_filepath = item->full_filepath;
    session->original_extension = raw_ext;
    session->is_folder = item->is_folder;
    session->is_desktop_file = !item->is_folder && lowercase(raw_ext) == ".desktop";
    session->grid_x = ico->x_pos;
    session->grid_y = ico->y_pos;
    session->box_bounds = Bounds(box_x, box_y, box_w, box_h);

    {
        std::lock_guard<std::mutex> lock(desktop_rename_mutex);
        reap_desktop_renames_locked();
        active_desktop_rename = session;
    }

    session->thread = std::thread(run_desktop_rename_session,
                                  session,
                                  static_cast<int>(std::lround(box_w)),
                                  static_cast<int>(std::lround(box_h)),
                                  margin_left,
                                  margin_top,
                                  s,
                                  monitor_name,
                                  font_size,
                                  border_color);
}

static void start_rename_for_path(const std::string &filepath) {
    for (auto *desktop : actual_root->children) {
        if (desktop->custom_type != (int) TYPE::DESKTOP_ICONS)
            continue;
        for (auto *icon : desktop->children) {
            auto *item = *datum<DesktopItem *>(icon, "DesktopItem");
            if (item && item->full_filepath == filepath) {
                start_desktop_icon_rename(desktop, icon);
                return;
            }
        }
    }
}

static void stop_desktop_rename_sessions() {
    desktop_icons::finish_rename(false);
    std::vector<std::shared_ptr<DesktopRenameSession>> sessions;
    {
        std::lock_guard<std::mutex> lock(desktop_rename_mutex);
        if (active_desktop_rename) {
            sessions.push_back(std::move(active_desktop_rename));
            active_desktop_rename.reset();
        }
        for (auto &s : finished_desktop_renames)
            sessions.push_back(std::move(s));
        finished_desktop_renames.clear();
    }
    for (auto &s : sessions) {
        if (s->app)
            windowing::close_app(s->app);
        if (s->thread.joinable())
            s->thread.join();
    }
}

bool desktop_icons::is_renaming() {
    std::lock_guard<std::mutex> lock(desktop_rename_mutex);
    return active_desktop_rename && !active_desktop_rename->finishing.load();
}

bool desktop_icons::rename_bounds_contains(float x, float y) {
    std::lock_guard<std::mutex> lock(desktop_rename_mutex);
    if (!active_desktop_rename || active_desktop_rename->finishing.load())
        return false;
    return bounds_contains(active_desktop_rename->box_bounds, x, y);
}

void desktop_icons::finish_rename(bool commit) {
    std::shared_ptr<DesktopRenameSession> session;
    RawApp *app = nullptr;
    {
        std::lock_guard<std::mutex> lock(desktop_rename_mutex);
        session = active_desktop_rename;
        if (!session || session->finishing.load())
            return;
        app = session->app;
    }
    if (app) {
        windowing::run_on_app(app, [session, commit]() {
            std::string value;
            if (commit && session->app && session->editor && session->editor->user_data) {
                std::lock_guard<std::mutex> lock(session->app->mutex);
                if (session->editor && session->editor->user_data)
                    value = static_cast<STextEditorData *>(session->editor->user_data)->text;
            }
            finish_rename_from_client(session, commit, std::move(value));
        });
    } else {
        finish_rename_from_client(session, false, {});
    }
}

static void create_icon_popup(Container *icon) {
    // Folder watches can replace items while the popup is open; capture values.
    std::vector<DesktopItem> items;
    std::vector<std::string> editable_paths;
    for (auto *selected : icons_for_action(icon->parent, icon)) {
        auto *item = *datum<DesktopItem *>(selected, "DesktopItem");
        if (!item)
            continue;
        items.push_back(*item);
        if (is_editable_desktop_item(*item))
            editable_paths.push_back(item->full_filepath);
        ((IcoContainerData *)selected->user_data)->last_time_pressed = 0;
    }
    if (items.empty())
        return;

    std::vector<PopOption> options;
    PopOption open;
    open.text = items.size() == 1 ? "Open" : fz("Open {} Items", items.size());
    open.on_clicked = [items]() {
        std::vector<std::string> paths;
        for (const auto& item : items)
            paths.push_back(item.full_filepath);
        punch_icons_for_paths(paths);
        for (const auto& item : items)
            open_desktop_item(item);
    };
    options.push_back(open);

    if (!editable_paths.empty()) {
        PopOption edit;
        edit.text = items.size() == 1 ? "Edit" :
            (editable_paths.size() == 1 ? "Edit 1 Text File" : fz("Edit {} Text Files", editable_paths.size()));
        edit.on_clicked = [editable_paths]() {
            auto *editor = g_app_info_get_default_for_type("text/plain", FALSE);
            if (!editor) {
                notify("No default text editor is configured.");
                return;
            }
            GList *files = nullptr;
            for (const auto& path : editable_paths)
                files = g_list_prepend(files, g_file_new_for_path(path.c_str()));
            files = g_list_reverse(files);
            GError *error = nullptr;
            if (!g_app_info_launch(editor, files, nullptr, &error)) {
                notify(fz("Unable to edit desktop files: {}", error ? error->message : "Unknown error"));
                g_clear_error(&error);
            }
            g_list_free_full(files, g_object_unref);
            g_object_unref(editor);
        };
        options.push_back(edit);
    }

    PopOption manager;
    manager.text = items.size() == 1 ? "Open in File Manager" :
        fz("Open {} Items in File Manager", items.size());
    manager.on_clicked = [items]() {
        std::unordered_set<std::string> opened;
        for (const auto& item : items) {
            const auto path = item.is_folder ? item.full_filepath :
                std::filesystem::path(item.full_filepath).parent_path().string();
            if (opened.insert(path).second)
                open_desktop_path(path);
        }
    };
    options.push_back(manager);

    PopOption separator;
    separator.seperator = true;
    options.push_back(separator);
    if (items.size() == 1) {
        PopOption rename;
        rename.text = "Rename";
        const std::string target_path = items[0].full_filepath;
        rename.on_clicked = [target_path]() {
            start_rename_for_path(target_path);
        };
        options.push_back(rename);
    }
    PopOption remove;
    remove.text = items.size() == 1 ? "Remove" : fz("Remove {} Items", items.size());
    remove.on_clicked = [items]() {
        for (const auto& item : items) {
            auto *file = g_file_new_for_path(item.full_filepath.c_str());
            GError *error = nullptr;
            if (!g_file_trash(file, nullptr, &error)) {
                notify(fz("Unable to remove {}: {}", item.name, error ? error->message : "Unknown error"));
                g_clear_error(&error);
            }
            g_object_unref(file);
        }
        on_change_in_desktop_folder();
        damage_all();
    };
    options.push_back(remove);
    const auto m = mouse();
    popup::open(options, m.x - 1, m.y + 1);
}

static void update_desktop_selection(Container* desktop, const Bounds& selection) {
    for (auto* child : desktop->children) {
        auto* ico          = (IcoContainerData*)(child->user_data);
        const bool selected = overlaps(child->real_bounds, selection);
        if (ico->is_selected == selected)
            continue;

        ico->is_selected = selected;
        auto damage      = child->real_bounds;
        damage.grow(2);
        hypriso->damage_box(damage);
    }
}

static void paint_desktop_icon(Container *c, int monitor, const Bounds &bounds, float overview_alpha) {
    auto root = get_rendering_root();
    if (!root)
        return;
    const auto s = scale(monitor);
    renderfix
    const auto monitor_bounds = bounds_monitor(monitor);
    const double factor = bounds.w / (monitor_bounds.w * s);
    auto transform = [&](Bounds b) {
        return Bounds(bounds.x + b.x * factor, bounds.y + b.y * factor, b.w * factor, b.h * factor);
    };
    DesktopItem *item = *datum<DesktopItem *>(c, "DesktopItem");

    auto* ico = (IcoContainerData*)(c->user_data);

    // Icon and label shrink toward the icon cell's center while punching.
    float punch = 1.f;
    if (ico->is_punching)
        punch = punch_scale((get_current_time_in_ms() - ico->punch_start_ms) / ICON_PUNCH_MS);
    const double punch_cx = c->real_bounds.x + c->real_bounds.w * .5;
    // Without the label, punch toward the icon's own center so it doesn't drift down.
    const double punch_cy = include_label ? c->real_bounds.y + c->real_bounds.h * .5
                                          : c->real_bounds.y + 2 * s + conf_icon_size() * s * .5;
    auto paint_texture = [&](TextureInfo info, double x, double y, float alpha, bool punchable = true) {
        Bounds b(x, y, info.w, info.h);
        if (punch != 1.f && punchable)
            b = Bounds(punch_cx + (x - punch_cx) * punch, punch_cy + (y - punch_cy) * punch, info.w * punch, info.h * punch);
        draw_texture(info, transform(b), alpha);
    };
    if (ico->texture_scale != s) {
        ico->clear_textures();
        ico->texture_scale = s;
        for (const auto *key : {"folder", "text-plain"}) {
            const auto path = one_shot_icon(conf_icon_size() * s, {key});
            *datum<TextureInfo>(c, key) = gen_texture(path, conf_icon_size() * s);
        }
    }

    {
        TextureInfo info;

        if (!*datum<bool>(c, "icon_attempted")) {
            *datum<bool>(c, "icon_attempted") = true;
            auto path = one_shot_icon(conf_icon_size() * s, item->icons_for_mime);
            auto generated = gen_texture(path, conf_icon_size() * s);
            *datum<TextureInfo>(c, "icon") = generated;
        }

        info = *datum<TextureInfo>(c, "icon");

        if (info.id == -1) {
            if (item->is_folder) {
                info = *datum<TextureInfo>(c, "folder");
            } else {
                info = *datum<TextureInfo>(c, "text-plain");
            }
        }

        if (info.id != -1) {
            if (ico->icon_shadow_source != info.id) {
                free_text_texture(ico->icon_shadow.id);
                ico->icon_shadow = generate_dropshadow_texture(info.id, 8 * s, .5);
                ico->icon_shadow_source = info.id;
            }
            const int x = c->real_bounds.x + c->real_bounds.w * .5 - info.w * .5;
            const int y = c->real_bounds.y + 2 * s;
            if (ico->icon_shadow.id != -1) {
                const int padding = (ico->icon_shadow.w - info.w) / 2;
                paint_texture(ico->icon_shadow, x - padding, y - padding + 2 * s, overview_alpha * 0.6f);
            }
            paint_texture(info, x, y, overview_alpha);
        }
    }

    auto border_bounds = c->real_bounds;
    auto border_thickness = std::round(1 * s);
    border_bounds.shrink(border_thickness);
    auto sel_color = color_sel_color();
    auto sel_border_color = color_sel_border_color();
    sel_color.a *= overview_alpha;
    sel_border_color.a *= overview_alpha;

    if (c->state.mouse_pressing) {
        rect(transform(c->real_bounds), sel_color);
        border(transform(border_bounds), sel_border_color, border_thickness * factor);
    } else if (c->state.mouse_hovering) {
        rect(transform(c->real_bounds), sel_color);
        border(transform(border_bounds), sel_border_color, border_thickness * factor);
    } else if (ico->is_selected || ico->is_renaming) {
        rect(transform(c->real_bounds), sel_color);
        border(transform(border_bounds), sel_border_color, border_thickness * factor);
    }

    if (ico->is_renaming)
        return;

    if (ico->name != item->name) {
        ico->name = item->name;
        auto *label = datum<TextureInfo>(c, "label");
        free_text_texture(label->id);
        *label = TextureInfo();
        free_text_texture(ico->label_shadow.id);
        ico->label_shadow = TextureInfo();
    }

    TextureInfo text_img = *datum<TextureInfo>(c, "label");
    if (text_img.id == -1) {
        text_img = gen_text_texture(set->font, item->name, conf_font_size() * s, RGBA(1, 1, 1, 1), c->real_bounds.w, std::ceil(two_line_height * s), 1);
        *datum<TextureInfo>(c, "label") = text_img;
        free_text_texture(ico->label_shadow.id);
        ico->label_shadow = generate_dropshadow_texture(text_img.id, 3 * s, 2.0);
    }
    const int text_x = c->real_bounds.x;
    const int text_y = c->real_bounds.y + c->real_bounds.h - text_img.h - 6 * s;
    if (ico->label_shadow.id != -1) {
        const int padding = (ico->label_shadow.w - text_img.w) / 2;
        paint_texture(ico->label_shadow, text_x - padding + s, text_y - padding, overview_alpha * 0.7f, include_label);
    }
    paint_texture(text_img, text_x, text_y, overview_alpha, include_label);
}

void desktop_icons::paint_overview(int monitor, const Bounds &bounds, float alpha) {
    alpha = std::clamp(alpha, 0.0f, 1.0f);
    if (alpha == 0.0f)
        return;
    for (auto desktop : actual_root->children) {
        if (desktop->custom_type != (int) TYPE::DESKTOP_ICONS || *datum<int>(desktop, "monitor") != monitor)
            continue;
        for (auto icon : desktop->children)
            paint_desktop_icon(icon, monitor, bounds, alpha);
    }
}

void create_desktop_icon(Container *parent, DesktopItem *item) {
    auto c = parent->child(FILL_SPACE, FILL_SPACE);
    c->custom_type = (int) TYPE::DESKTOP_ICON;
    auto ico = new IcoContainerData;
    ico->name = item->name;
    ico->c = c;
    c->user_data = ico;
    *datum<DesktopItem *>(c, "DesktopItem") = item;
    for (const auto *key : {"label", "icon", "folder", "text-plain"})
        *datum<TextureInfo>(c, key) = TextureInfo();
    *datum<bool>(c, "icon_attempted") = false;

    c->when_mouse_motion = [](Container* actual_root, Container* c) {
        damage_icon(c);
    };
    // Entry does not dispatch motion, so both hover transitions must damage the icon.
    c->when_mouse_enters_container = c->when_mouse_motion;
    c->when_mouse_leaves_container = c->when_mouse_motion;
    c->when_drag_end_is_click = false;
    c->when_drag_start = [](Container* actual_root, Container* c) {
        if (c->state.mouse_button_pressed != BTN_LEFT)
            return;
        auto icons = icons_for_action(c->parent, c);
        for (auto *icon : icons) {
            auto *ico = (IcoContainerData *)icon->user_data;
            ico->is_settling = false;
            ico->is_dragging = true;
            ico->drag_offset_x = icon->real_bounds.x - actual_root->mouse_initial_x;
            ico->drag_offset_y = icon->real_bounds.y - actual_root->mouse_initial_y;
            icon->z_index = 1;
            damage_icon(icon);
        }
    };
    c->when_drag = [](Container* actual_root, Container* c) {
        actual_root->consumed_event = true;
        for (auto *icon : c->parent->children) {
            auto *ico = (IcoContainerData *)icon->user_data;
            if (!ico->is_dragging)
                continue;
            damage_icon(icon);
            Bounds next(actual_root->mouse_current_x + ico->drag_offset_x,
                        actual_root->mouse_current_y + ico->drag_offset_y,
                        icon->real_bounds.w,
                        icon->real_bounds.h);
            next.grow(20);
            hypriso->damage_box(next);
        }
    };
    c->when_drag_end = [](Container* actual_root, Container* c) {
        std::vector<Container *> dragged;
        for (auto *icon : c->parent->children) {
            auto *ico = (IcoContainerData *)icon->user_data;
            if (!ico->is_dragging)
                continue;
            damage_icon(icon);
            ico->is_dragging = false;
            icon->z_index = 0;
            icon->real_bounds = Bounds(actual_root->mouse_current_x + ico->drag_offset_x,
                                       actual_root->mouse_current_y + ico->drag_offset_y,
                                       icon->real_bounds.w,
                                       icon->real_bounds.h);
            dragged.push_back(icon);
        }
        snap_icons_to_grid(c->parent, dragged);
        for (auto *icon : dragged) {
            begin_icon_settle(icon);
        }
        save_icon_positions(c->parent);
    };
    c->when_clicked = [](Container* actual_root, Container* c) {
        actual_root->consumed_event = true;
        if (c->state.mouse_button_pressed == BTN_RIGHT) {
            create_icon_popup(c);
            return;
        }
        if (c->state.mouse_button_pressed != BTN_LEFT)
            return;
        auto ico = (IcoContainerData *) c->user_data;
        bool shift_down = *datum<bool>(c->parent, "shift_held");
        if (!shift_down)
            clear_desktop_selection(c->parent);
        auto current = get_current_time_in_ms();
        if ((current - ico->last_time_pressed) < 700) {
            DesktopItem *item = *datum<DesktopItem *>(c, "DesktopItem");
            begin_icon_punch(c);
            open_desktop_item(*item);
        } else {
            ico->is_selected = true;
        }
        ico->last_time_pressed = current;
    };
   
    c->when_paint = [](Container* actual_root, Container* c) {
        // Overview paints icons over its wallpaper cards with a per-frame fade.
        // Wallpaper captures must stay free of icons throughout the transition.
        if (overview::suppresses_desktop_icons())
            return;
        auto root = get_rendering_root();
        if (!root)
            return;
        auto [rid, s, stage, active_id] = roots_info(actual_root, root);
        if (stage != (int) STAGE::RENDER_POST_WALLPAPER || *datum<int>(c->parent, "monitor") != rid)
            return;
        const auto mb = bounds_monitor(rid);
        paint_desktop_icon(c, rid, Bounds(0, 0, mb.w * s, mb.h * s), 1.0f);
    };

}

static void arrange_desktop_icons() {
    const bool vertical = conf_vertical();
    for (auto *item : desktop_items)
        refresh_sort_metadata(*item);
    sort_desktop_items();

    for (auto *desktop : actual_root->children) {
        if (desktop->custom_type != (int)TYPE::DESKTOP_ICONS)
            continue;

        const auto metrics = icon_grid_metrics(desktop, scale(*datum<int>(desktop, "monitor")));
        std::unordered_set<long long> occupied;
        // Apply the chosen order without changing selection or relying on saved grid slots.
        for (auto *item : desktop_items) {
            for (auto *icon : desktop->children) {
                if (*datum<DesktopItem *>(icon, "DesktopItem") != item)
                    continue;
                auto *ico = (IcoContainerData *)icon->user_data;
                const auto [x, y] = next_available_slot(occupied, metrics.cols, metrics.rows, vertical);
                occupied.insert(grid_key(x, y));
                damage_icon(icon);
                ico->is_dragging = false;
                icon->z_index = 0;
                ico->x_pos = x;
                ico->y_pos = y;
                begin_icon_settle(icon);
                break;
            }
        }
        save_icon_positions(desktop);
    }
    damage_all();
    request_refresh();
}

static void create_root_popup(int monitor) {
    auto m = mouse();
    std::vector<PopOption> root;
    {
        PopOption view;
        view.text = "View";
        for (const bool vertical : {true, false}) {
            PopOption orientation;
            orientation.text = vertical ? "Vertical" : "Horizontal";
            orientation.checked = conf_vertical() == vertical;
            orientation.on_clicked = [vertical]() {
                set->desktop_vertical_override = vertical ? 1 : 0;
                settings::load_save_settings(true, set);
                arrange_desktop_icons();
            };
            view.submenu.push_back(orientation);
        }
        root.push_back(view);
    }
    {
        PopOption sort;
        sort.text = "Sort by";
        const std::pair<const char *, const char *> modes[] = {
            {"Name", "name"},
            {"Size", "size"},
            {"Item type", "type"},
            {"Date modified", "date_modified"},
        };
        for (const auto& [label, mode] : modes) {
            PopOption option;
            option.text = label;
            option.checked = conf_sort_by() == mode;
            option.on_clicked = [mode = std::string(mode)]() {
                set->desktop_sort_by = mode;
                settings::load_save_settings(true, set);
                arrange_desktop_icons();
            };
            sort.submenu.push_back(option);
        }
        PopOption separator;
        separator.seperator = true;
        sort.submenu.push_back(separator);
        for (const bool ascending : {true, false}) {
            PopOption direction;
            direction.text = ascending ? "Ascending" : "Descending";
            direction.checked = set->desktop_sort_ascending == ascending;
            direction.on_clicked = [ascending]() {
                set->desktop_sort_ascending = ascending;
                settings::load_save_settings(true, set);
                arrange_desktop_icons();
            };
            sort.submenu.push_back(direction);
        }
        root.push_back(sort);
    }
    {
        PopOption pop;
        pop.text = "Configure Display Settings...";   
        pop.on_clicked = []() {
            settings::start("Display");
        };
        root.push_back(pop);
    }
    {
        PopOption pop;
        pop.text = "Refresh Compositor...";   
        pop.on_clicked = []() {
            hypriso->dispatch("forcerendererreload", "");
        };
        root.push_back(pop);
    }

    PopOption magic_workspace;
    if (hypriso->is_magic_workspace_active(monitor)) {
        magic_workspace.text = "Exit magic workspace";
        magic_workspace.on_clicked = [monitor]() {
            hypriso->exit_magic_workspace(monitor);
        };
    } else {
        magic_workspace.text = "Enter magic workspace";
        magic_workspace.on_clicked = [monitor]() {
            hypriso->enter_magic_workspace(monitor);
        };
    }
    root.push_back(magic_workspace);
  
    PopOption pop;
    pop.seperator = true;
    root.push_back(pop);        

    {
        PopOption pop;
        pop.text = "Log out";   
        pop.on_clicked = []() {
            hypriso->logout();
        };
        root.push_back(pop);
    }

    popup::open(root, m.x - 1, m.y + 1);
}

enum class DesktopDropAction {
    Shortcut,
    Copy,
    Move,
};

static std::filesystem::path unique_destination_path(const std::filesystem::path& dest_dir,
                                                     const std::filesystem::path& src_path) {
    const auto filename = src_path.filename();
    auto candidate = dest_dir / filename;
    std::error_code ec;
    auto entry_exists = [&ec](const std::filesystem::path& p) {
        ec.clear();
        const auto status = std::filesystem::symlink_status(p, ec);
        return !ec && std::filesystem::exists(status);
    };
    if (!entry_exists(candidate))
        return candidate;

    ec.clear();
    const bool is_dir = std::filesystem::is_directory(src_path, ec);
    const std::string stem = is_dir ? filename.string() : filename.stem().string();
    const std::string ext = is_dir ? std::string{} : filename.extension().string();
    const std::string base = stem.empty() ? filename.string() : stem;
    const std::string suffix = stem.empty() ? std::string{} : ext;

    for (int i = 2; i < 10000; ++i) {
        candidate = dest_dir / (base + " (" + std::to_string(i) + ")" + suffix);
        if (!entry_exists(candidate))
            return candidate;
    }
    return dest_dir / filename;
}

static void place_new_desktop_entries_near(const std::vector<std::filesystem::path>& created_paths,
                                           float drop_x,
                                           float drop_y) {
    if (created_paths.empty())
        return;

    Container* target_desktop = nullptr;
    for (auto* child : actual_root->children) {
        if (child->custom_type != (int)TYPE::DESKTOP_ICONS)
            continue;
        if (bounds_contains(child->real_bounds, drop_x, drop_y)) {
            target_desktop = child;
            break;
        }
        if (!target_desktop)
            target_desktop = child;
    }

    if (target_desktop) {
        const auto section = current_desktop_section_key();
        if (!section.empty()) {
            const int monitor = *datum<int>(target_desktop, "monitor");
            const auto m = icon_grid_metrics(target_desktop, scale(monitor));
            auto sections = read_icon_position_sections();
            auto& saved = sections[section];

            std::unordered_set<std::string> created_names;
            for (const auto& created : created_paths)
                created_names.insert(created.filename().string());

            std::unordered_set<long long> occupied;
            occupied.reserve(target_desktop->children.size() + created_paths.size());
            for (auto* child : target_desktop->children) {
                auto* ico = (IcoContainerData*)child->user_data;
                if (ico->x_pos < 0 || ico->y_pos < 0)
                    continue;
                DesktopItem* item = *datum<DesktopItem*>(child, "DesktopItem");
                if (item && created_names.contains(icon_entry_name(item)))
                    continue;
                occupied.insert(grid_key(ico->x_pos, ico->y_pos));
            }

            const int prefer_x = std::max(0, static_cast<int>(std::lround((drop_x - m.width * 0.5f - m.start_x) / (m.width + m.hpad))));
            const int prefer_y = std::max(0, static_cast<int>(std::lround((drop_y - m.height * 0.5f - m.start_y) / (m.height + m.vpad))));

            for (const auto& created : created_paths) {
                const auto [nx, ny] = nearest_free_slot(occupied, prefer_x, prefer_y);
                occupied.insert(grid_key(nx, ny));
                const auto name = created.filename().string();
                saved[name] = {nx, ny};
                for (auto* child : target_desktop->children) {
                    DesktopItem* item = *datum<DesktopItem*>(child, "DesktopItem");
                    if (!item || icon_entry_name(item) != name)
                        continue;
                    auto* ico = (IcoContainerData*)child->user_data;
                    ico->x_pos = nx;
                    ico->y_pos = ny;
                    begin_icon_settle(child);
                    break;
                }
            }
            write_icon_position_sections(sections);
        }
    }

    on_change_in_desktop_folder();
    damage_all();
    request_refresh();
}

static void spawn_desktop_icons_worker(std::function<void()> fn);

static void perform_desktop_drop(std::vector<std::string> source_paths,
                                 DesktopDropAction action,
                                 float drop_x,
                                 float drop_y) {
    auto folder = Hyprutils::Path::resolvePath(conf_desktop_folder());
    if (!folder.has_value()) {
        notify("Desktop folder is not configured.");
        return;
    }

    const std::filesystem::path dest_dir(folder.value());
    std::error_code dir_ec;
    if (!std::filesystem::is_directory(dest_dir, dir_ec)) {
        notify("Desktop folder does not exist.");
        return;
    }

    spawn_desktop_icons_worker([source_paths = std::move(source_paths), dest_dir, action, drop_x, drop_y]() {
        std::vector<std::filesystem::path> created;
        std::vector<std::string> errors;

        for (const auto& raw_path : source_paths) {
            std::filesystem::path src(raw_path);
            std::error_code ec;
            const auto status = std::filesystem::symlink_status(src, ec);
            if (ec || !std::filesystem::exists(status))
                continue;

            if (action == DesktopDropAction::Move) {
                std::error_code eq_ec;
                if (std::filesystem::equivalent(src.parent_path(), dest_dir, eq_ec) && !eq_ec)
                    continue;
            }

            const auto dest = unique_destination_path(dest_dir, src);

            if (action == DesktopDropAction::Shortcut) {
                std::error_code abs_ec;
                auto target = std::filesystem::absolute(src, abs_ec);
                if (abs_ec)
                    target = src;
                std::error_code is_dir_ec;
                const bool is_dir = std::filesystem::is_directory(src, is_dir_ec);
                if (is_dir)
                    std::filesystem::create_directory_symlink(target, dest, ec);
                else
                    std::filesystem::create_symlink(target, dest, ec);
                if (ec) {
                    errors.push_back(fz("Shortcut {}: {}", src.filename().string(), ec.message()));
                    continue;
                }
                created.push_back(dest);
            } else if (action == DesktopDropAction::Copy) {
                std::filesystem::copy(
                    src,
                    dest,
                    std::filesystem::copy_options::recursive | std::filesystem::copy_options::copy_symlinks,
                    ec);
                if (ec) {
                    errors.push_back(fz("Copy {}: {}", src.filename().string(), ec.message()));
                    continue;
                }
                created.push_back(dest);
            } else if (action == DesktopDropAction::Move) {
                std::filesystem::rename(src, dest, ec);
                if (ec == std::errc::cross_device_link) {
                    ec.clear();
                    std::filesystem::copy(
                        src,
                        dest,
                        std::filesystem::copy_options::recursive | std::filesystem::copy_options::copy_symlinks,
                        ec);
                    if (!ec) {
                        std::error_code rm_ec;
                        if (std::filesystem::is_symlink(src, rm_ec))
                            std::filesystem::remove(src, rm_ec);
                        else
                            std::filesystem::remove_all(src, rm_ec);
                        if (rm_ec)
                            errors.push_back(fz("Remove original {}: {}", src.filename().string(), rm_ec.message()));
                    }
                }
                if (ec) {
                    errors.push_back(fz("Move {}: {}", src.filename().string(), ec.message()));
                    continue;
                }
                created.push_back(dest);
            }
        }

        main_thread([created = std::move(created), errors = std::move(errors), drop_x, drop_y]() {
            if (!created.empty())
                place_new_desktop_entries_near(created, drop_x, drop_y);
            if (!errors.empty())
                notify(errors.front());
        });
    });
}

struct DesktopIconsWorker {
    std::atomic<bool> done = false;
    std::thread thread;
};

static std::atomic<bool> desktop_icons_workers_stopping = false;
static std::mutex desktop_icons_workers_mutex;
static std::vector<std::shared_ptr<DesktopIconsWorker>> desktop_icons_workers;

static void spawn_desktop_icons_worker(std::function<void()> fn) {
    std::lock_guard<std::mutex> lock(desktop_icons_workers_mutex);
    for (int i = (int) desktop_icons_workers.size() - 1; i >= 0; i--) {
        if (desktop_icons_workers[i]->done.load()) {
            if (desktop_icons_workers[i]->thread.joinable())
                desktop_icons_workers[i]->thread.join();
            desktop_icons_workers.erase(desktop_icons_workers.begin() + i);
        }
    }
    if (desktop_icons_workers_stopping.load())
        return;
    auto worker = std::make_shared<DesktopIconsWorker>();
    worker->thread = std::thread([worker, fn = std::move(fn)]() {
        fn();
        worker->done = true;
    });
    desktop_icons_workers.push_back(std::move(worker));
}

static void stop_desktop_icons_workers() {
    std::vector<std::shared_ptr<DesktopIconsWorker>> workers;
    {
        std::lock_guard<std::mutex> lock(desktop_icons_workers_mutex);
        desktop_icons_workers_stopping = true;
        workers.swap(desktop_icons_workers);
    }
    for (auto &worker : workers) {
        if (worker->thread.joinable())
            worker->thread.join();
    }
    desktop_icons_workers_stopping = false;
}

static bool is_image_file(const std::string& path) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec) || ec)
        return false;
    auto *file = g_file_new_for_path(path.c_str());
    auto *info = g_file_query_info(file,
        G_FILE_ATTRIBUTE_STANDARD_CONTENT_TYPE,
        G_FILE_QUERY_INFO_NONE, nullptr, nullptr);
    bool is_image = false;
    if (info) {
        if (const auto *type = g_file_info_get_content_type(info)) {
            if (g_content_type_is_a(type, "image/*")) {
                is_image = true;
            } else if (auto *mime = g_content_type_get_mime_type(type)) {
                is_image = std::string_view(mime).starts_with("image/");
                g_free(mime);
            }
        }
        g_object_unref(info);
    }
    g_object_unref(file);
    return is_image;
}

static std::string wallpaper_target_for_drop(float drop_x, float drop_y) {
    auto overrides = wallpaper_parse_overrides(set->wallpaper_monitors);
    if (overrides.empty())
        return "";
    for (const auto& m : wallpaper_monitors_copy()) {
        if (drop_x >= m.x && drop_x < m.x + m.w && drop_y >= m.y && drop_y < m.y + m.h) {
            if (wallpaper_find_override(overrides, m.name))
                return m.name;
            break;
        }
    }
    return "";
}

static void open_drop_context_menu(const std::vector<std::string>& paths, float drop_x, float drop_y) {
    std::vector<std::string> valid_paths;
    valid_paths.reserve(paths.size());
    for (const auto& p : paths) {
        std::error_code ec;
        const auto status = std::filesystem::symlink_status(p, ec);
        if (!ec && std::filesystem::exists(status))
            valid_paths.push_back(p);
    }
    if (valid_paths.empty())
        return;

    std::vector<PopOption> options;
    {
        PopOption shortcut;
        shortcut.text = valid_paths.size() == 1 ? "Create Shortcut Here" : "Create Shortcuts Here";
        shortcut.on_clicked = [valid_paths, drop_x, drop_y]() {
            perform_desktop_drop(valid_paths, DesktopDropAction::Shortcut, drop_x, drop_y);
        };
        options.push_back(shortcut);
    }
    {
        PopOption copy;
        copy.text = "Copy Here";
        copy.on_clicked = [valid_paths, drop_x, drop_y]() {
            perform_desktop_drop(valid_paths, DesktopDropAction::Copy, drop_x, drop_y);
        };
        options.push_back(copy);
    }
    {
        PopOption move;
        move.text = "Move Here";
        move.on_clicked = [valid_paths, drop_x, drop_y]() {
            perform_desktop_drop(valid_paths, DesktopDropAction::Move, drop_x, drop_y);
        };
        options.push_back(move);
    }
    if (valid_paths.size() == 1 && is_image_file(valid_paths[0])) {
        PopOption wallpaper;
        wallpaper.text = "Set as wallpaper";
        const std::string image_path = valid_paths[0];
        wallpaper.on_clicked = [image_path, drop_x, drop_y]() {
            wallpaper_working_indicator_start();
            const std::string target = wallpaper_target_for_drop(drop_x, drop_y);
            spawn_desktop_icons_worker([image_path, target]() {
                if (!settings::set_wallpaper_from_file(image_path, target)) {
                    main_thread([]() {
                        notify("Unable to set wallpaper.");
                    });
                }
            });
        };
        options.push_back(wallpaper);
    }
    {
        PopOption sep;
        sep.seperator = true;
        options.push_back(sep);
    }
    {
        PopOption cancel;
        cancel.text = "Cancel";
        options.push_back(cancel);
    }

    popup::open(options, static_cast<int>(std::lround(drop_x)) - 1, static_cast<int>(std::lround(drop_y)) + 1);
    damage_all();
}

void desktop_icons::start() {
    auto in = gen_text_texture(set->font, "W\n", conf_font_size(), RGBA(1, 1, 1, 1));
    two_line_height = in.h;
    free_text_texture(in.id);
    // each monitor needs its own desktop pane possibly every workspace
    // assign each desktop pane a monitor id
    auto c = actual_root->child(FILL_SPACE, FILL_SPACE);
    c->custom_type = (int) TYPE::DESKTOP_ICONS;
    int monitor = hypriso->monitor_from_cursor();
    *datum<int>(c, "monitor") = monitor;

    c->pre_layout = [](Container *root, Container *c, const Bounds &b) {
        auto monitor = *datum<int>(c, "monitor");
        c->wanted_bounds = bounds_reserved_monitor(monitor);
        c->real_bounds = c->wanted_bounds;
        auto s = scale(monitor);

        if (overview::suppresses_desktop_icons() && desktop_icons::is_renaming())
            desktop_icons::finish_rename(false);

        if (set->desktop_icons) {
            merge_create<DesktopItem *>(c, desktop_items, [](Container *c) {
                return *datum<DesktopItem *>(c, "DesktopItem");
            }, [](Container *parent, DesktopItem *data) {
                create_desktop_icon(parent, data);
            });
        }

        if (!pending_select_filepath.empty()) {
            for (auto *child : c->children) {
                auto *item = *datum<DesktopItem *>(child, "DesktopItem");
                if (item && item->full_filepath == pending_select_filepath) {
                    clear_desktop_selection(c);
                    ((IcoContainerData *) child->user_data)->is_selected = true;
                    pending_select_filepath.clear();
                    break;
                }
            }
        }

        for (auto *child : c->children) {
            auto *ico = (IcoContainerData *)child->user_data;
            if (ico->x_pos < 0 || ico->y_pos < 0) {
                load_icon_positions_from_save(c);
                break;
            }
        }

        auto m = icon_grid_metrics(c, s);
        std::unordered_set<long long> occupied;
        occupied.reserve(c->children.size());

        for (auto *child : c->children) {
            auto *ico = (IcoContainerData *)child->user_data;
            if (ico->x_pos < 0 || ico->y_pos < 0)
                continue;
            occupied.insert(grid_key(ico->x_pos, ico->y_pos));
        }

        const bool vertical = conf_vertical();
        bool assigned_new = false;
        for (auto *child : c->children) {
            auto *ico = (IcoContainerData *)child->user_data;
            if (ico->x_pos >= 0 && ico->y_pos >= 0)
                continue;
            auto [sx, sy] = next_available_slot(occupied, m.cols, m.rows, vertical);
            ico->x_pos = sx;
            ico->y_pos = sy;
            occupied.insert(grid_key(sx, sy));
            assigned_new = true;
        }
        if (assigned_new)
            save_icon_positions(c);

        for (auto *child : c->children) {
            auto *ico = (IcoContainerData *)child->user_data;
            if (ico->is_dragging) {
                child->real_bounds = Bounds(root->mouse_current_x + ico->drag_offset_x,
                                            root->mouse_current_y + ico->drag_offset_y,
                                            m.width,
                                            m.height);
                continue;
            }

            if (ico->is_punching) {
                damage_icon(child);
                if (get_current_time_in_ms() - ico->punch_start_ms >= ICON_PUNCH_MS)
                    ico->is_punching = false;
                else
                    request_refresh();
            }

            const auto target = bounds_for_grid_slot(m, ico->x_pos, ico->y_pos);
            if (ico->is_settling) {
                const float t = (get_current_time_in_ms() - ico->settle_start_ms) / ICON_SETTLE_MS;
                if (t >= 1.f) {
                    ico->is_settling = false;
                    child->real_bounds = target;
                    damage_icon(child);
                } else {
                    damage_icon(child);
                    Bounds from(ico->settle_from_x, ico->settle_from_y, m.width, m.height);
                    child->real_bounds = lerp(from, target, t);
                    damage_icon(child);
                    request_refresh();
                }
                continue;
            }

            child->real_bounds = target;
        }
    };
    static bool dragging = false;
    dragging = false;
    c->when_drag_end_is_click = false;
    c->when_clicked = [](Container* actual_root, Container* c) {
        if (c->state.mouse_button_pressed == BTN_RIGHT) {
           create_root_popup(*datum<int>(c, "monitor"));
        } else {
            clear_desktop_selection(c);
        }
    };
    c->when_drag_start = [](Container* actual_root, Container* c) {
        dragging = true;
        const auto selection = fixed_box(actual_root->mouse_initial_x, actual_root->mouse_initial_y, actual_root->mouse_current_x, actual_root->mouse_current_y);
        update_desktop_selection(c, selection);
    };
    c->when_drag = [](Container *actual_root, Container *c) {
        actual_root->consumed_event = true;
        auto b = fixed_box(actual_root->mouse_initial_x, actual_root->mouse_initial_y, actual_root->mouse_current_x, actual_root->mouse_current_y);
        update_desktop_selection(c, b);
        static Bounds previousB = b;
        b.grow(20);
        hypriso->damage_box(b);
        hypriso->damage_box(previousB);
        previousB = b;
    };
    c->when_drag_end = [](Container* actual_root, Container* c) {
        dragging = false;
        auto damage = fixed_box(actual_root->mouse_initial_x, actual_root->mouse_initial_y, actual_root->mouse_current_x, actual_root->mouse_current_y);
        damage.grow(20);
        hypriso->damage_box(damage);
    };
    c->when_key_event = [](Container *root, Container *c, int key, bool pressed, xkb_keysym_t sym, int mods, bool is_text, std::string text) {
        (void)sym;
        (void)mods;
        (void)is_text;
        (void)text;
        if (desktop_icons::is_renaming())
            return;
        if (key == KEY_LEFTSHIFT)
            *datum<bool>(c, "shift_held") = pressed;

        if (key == KEY_F2 && pressed) {
            Container *only_selected = nullptr;
            int selected_count = 0;
            for (auto *child : c->children) {
                auto *ico = (IcoContainerData *) child->user_data;
                if (ico->is_selected) {
                    only_selected = child;
                    ++selected_count;
                }
            }
            if (selected_count == 1 && only_selected) {
                root->consumed_event = true;
                start_desktop_icon_rename(c, only_selected);
            }
        }

        if (key == KEY_DELETE && pressed)
            delete_selected_desktop_icons(c);
    };
    c->after_paint = [](Container* actual_root, Container* c) {
        if (overview::suppresses_desktop_icons())
            return;
        
        auto root = get_rendering_root();
        auto [rid, s, stage, active_id] = roots_info(actual_root, root);
        if (stage == (int)STAGE::RENDER_POST_WALLPAPER && dragging && c->state.mouse_button_pressed == BTN_LEFT) {
            auto b = fixed_box(actual_root->mouse_initial_x, actual_root->mouse_initial_y, actual_root->mouse_current_x, actual_root->mouse_current_y);

            // renderfix euivalent
            auto mb = bounds_monitor(rid);
            b.x -= mb.x;
            b.y -= mb.y;
            b.scale(s);
            b.round();

            auto col = color_sel_color();
            float rounding = 0.0f;
            auto shadow = b;
            shadow.grow(std::round(1.0f * s));
            render_drop_shadow(rid, 1.0, {0, 0, 0, .01f}, std::round(rounding * s), 2.0, shadow, std::round(6 * s));
            rect(b, RGBA(col.r, col.g, col.b, col.a), 0, std::round(rounding * s), 2.0f, true, 0.1);
            col = color_sel_border_color();
            border(b, RGBA(col.r, col.g, col.b, col.a), std::round(1.0f * s), 0, std::round(rounding * s), 2.0f, false, 1.0);


            // TEXT is center aligned label with trailing ... and max height of 2 ilnes
        }
    };

    hypriso->can_accept_desktop_drop = []() {
        if (!set->desktop_icons)
            return false;
        auto folder = Hyprutils::Path::resolvePath(conf_desktop_folder());
        if (!folder.has_value())
            return false;
        std::error_code ec;
        return std::filesystem::is_directory(folder.value(), ec);
    };
    hypriso->on_desktop_files_dropped = [](const std::vector<std::string>& paths, float x, float y) {
        open_drop_context_menu(paths, x, y);
    };

    on_change_in_desktop_folder();

    watch_desktop_folder();
}

void desktop_icons::stop() {
    stop_desktop_rename_sessions();
    hypriso->can_accept_desktop_drop = nullptr;
    hypriso->on_desktop_files_dropped = nullptr;
    for (int i = actual_root->children.size() - 1; i >= 0; i--) {
        auto c = actual_root->children[i];
        if (c->custom_type == (int) TYPE::DESKTOP_ICONS) {
            delete c;
            actual_root->children.erase(actual_root->children.begin() + i);
        }
    }
    clear_desktop_items();
    damage_all();
    if (latest_desktop_watch != -1)
        remove_watch(latest_desktop_watch);
    stop_desktop_icons_workers();
}

void desktop_icons::deselect() {
    for (int i = actual_root->children.size() - 1; i >= 0; i--) {
        auto child = actual_root->children[i];
        if (child->custom_type == (int) TYPE::DESKTOP_ICONS) {
            clear_desktop_selection(child);
        }
    }
}

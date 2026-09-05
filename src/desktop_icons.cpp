#include "desktop_icons.h"
#include "container.h"
#include "heart.h"
#include "hypriso.h"
#include "icons.h"
#include "overview.h"
#include "popup.h"
#include "settings.h"

#include <gio/gio.h>
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
    return hypriso->get_varfloat("plugin:mylardesktop:desktop_font_size", 12);
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

        if (i + 1 < exec.size())
            ++i;
    }

    return trim_copy(std::move(result));
}

static void read_desktop_file(DesktopItem& item, const std::filesystem::path& path) {
    std::ifstream file(path);
    if (!file)
        return;

    std::string line;
    bool inDesktopEntry = false;
    while (std::getline(file, line)) {
        if (line.starts_with('[') && line.ends_with(']')) {
            inDesktopEntry = line == "[Desktop Entry]";
            continue;
        }
        if (!inDesktopEntry)
            continue;

        const auto separator = line.find('=');
        if (separator == std::string::npos)
            continue;

        const std::string_view key(line.data(), separator);
        const std::string value = line.substr(separator + 1);
        if (key == "Name")
            item.name = value;
        else if (key == "Icon")
            item.icon = value;
        else if (key == "Exec")
            item.exec = remove_desktop_exec_field_codes(value);
    }
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

        const auto permissions = entry.status(ec).permissions();
        const bool isExecutable =
            !ec && (permissions & (std::filesystem::perms::owner_exec | std::filesystem::perms::group_exec | std::filesystem::perms::others_exec)) !=
                std::filesystem::perms::none;
        ec.clear();

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
    bool was_active_last_frame = false;
    long last_time_pressed = 0;
    bool is_selected = false;
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
        auto ico = (IcoContainerData *) c->user_data;
        bool is_active = c->state.mouse_hovering || c->state.mouse_pressing;
        auto b = c->real_bounds;
        b.grow(20);
        if (is_active) {
            hypriso->damage_box(b);
        } else if (ico->was_active_last_frame) {
            hypriso->damage_box(b);
        }
        ico->was_active_last_frame = is_active;
    };
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
            auto *ico = (IcoContainerData *)icon->user_data;
            ico->was_active_last_frame = icon->state.mouse_hovering || icon->state.mouse_pressing;
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
            open_desktop_item(*item);
        } else {
            ico->is_selected = true;
        }
        ico->last_time_pressed = current;
    };
   
    c->when_paint = [](Container* actual_root, Container* c) {
        if (!screenshotting_wallpaper && overview::is_showing())
            return;
        
        auto root = get_rendering_root();
        auto [rid, s, stage, active_id] = roots_info(actual_root, root);
        if (stage == (int)STAGE::RENDER_POST_WALLPAPER) {
            renderfix
            
            DesktopItem *item = *datum<DesktopItem *>(c, "DesktopItem");

            auto* ico = (IcoContainerData*)(c->user_data);
            if (ico->texture_scale != s) {
                ico->clear_textures();
                ico->texture_scale = s;
                for (const auto *key : {"folder", "text-plain"}) {
                    const auto path = one_shot_icon(conf_icon_size() * s, {key});
                    *datum<TextureInfo>(c, key) = gen_texture(path, conf_icon_size() * s);
                }
            }
            float overview_alpha = 1.0 - overview::get_openess();

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
                        ico->icon_shadow = generate_dropshadow_texture(info.id, 4 * s);
                        ico->icon_shadow_source = info.id;
                    }
                    const int x = c->real_bounds.x + c->real_bounds.w * .5 - info.w * .5;
                    const int y = c->real_bounds.y + 2 * s;
                    if (ico->icon_shadow.id != -1) {
                        const int padding = (ico->icon_shadow.w - info.w) / 2;
                        draw_texture(ico->icon_shadow, x - padding, y - padding + 2 * s, overview_alpha * 0.6f);
                    }
                    draw_texture(info, x, y, overview_alpha);
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
                rect(c->real_bounds, sel_color);
                border(border_bounds, sel_border_color, border_thickness);
            } else if (c->state.mouse_hovering) {
                rect(c->real_bounds, sel_color);
                border(border_bounds, sel_border_color, border_thickness);
            } else if (ico->is_selected) {
                rect(c->real_bounds, sel_color);
                border(border_bounds, sel_border_color, border_thickness);
            }
            
            
            TextureInfo text_img = *datum<TextureInfo>(c, "label");
            if (text_img.id == -1) {
                text_img = gen_text_texture(mylar_font, item->name, conf_font_size() * s, RGBA(1, 1, 1, 1), c->real_bounds.w, std::ceil(two_line_height * s), 1);
                *datum<TextureInfo>(c, "label") = text_img;
                free_text_texture(ico->label_shadow.id);
                ico->label_shadow = generate_dropshadow_texture(text_img.id, 2 * s);
            }
            const int text_x = c->real_bounds.x;
            const int text_y = c->real_bounds.y + c->real_bounds.h - text_img.h - 6 * s;
            if (ico->label_shadow.id != -1) {
                const int padding = (ico->label_shadow.w - text_img.w) / 2;
                draw_texture(ico->label_shadow, text_x - padding, text_y - padding + s, overview_alpha * 0.9f);
            }
            draw_texture(text_img, text_x, text_y, overview_alpha);
        }
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

static void create_root_popup() {
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
            settings::start();
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

void desktop_icons::start() {
    auto in = gen_text_texture(mylar_font, "W\n", conf_font_size(), RGBA(1, 1, 1, 1));
    two_line_height = in.h;
    free_text_texture(in.id);
    // each monitor needs its own desktop pane possibly every workspace
    // assign each desktop pane a monitor id
    auto c = actual_root->child(FILL_SPACE, FILL_SPACE);
    c->custom_type = (int) TYPE::DESKTOP_ICONS;
    int monitor = hypriso->monitor_from_cursor();
    auto monitor_scale = scale(monitor);
    *datum<int>(c, "monitor") = monitor;

    c->pre_layout = [](Container *root, Container *c, const Bounds &b) {
        auto monitor = *datum<int>(c, "monitor");
        c->wanted_bounds = bounds_reserved_monitor(monitor);
        c->real_bounds = c->wanted_bounds;
        auto s = scale(monitor);

        if (set->desktop_icons) {
            merge_create<DesktopItem *>(c, desktop_items, [](Container *c) {
                return *datum<DesktopItem *>(c, "DesktopItem");
            }, [](Container *parent, DesktopItem *data) {
                create_desktop_icon(parent, data);
            });
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
           create_root_popup();
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
        (void)root;
        (void)sym;
        (void)mods;
        (void)is_text;
        (void)text;
        if (key == KEY_LEFTSHIFT)
            *datum<bool>(c, "shift_held") = pressed;

        if (key == KEY_DELETE && pressed)
            delete_selected_desktop_icons(c);
    };
    c->after_paint = [](Container* actual_root, Container* c) {
        if (!screenshotting_wallpaper && overview::is_showing())
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
            float rounding = 9.0f;
            auto shadow = b;
            shadow.grow(std::round(1.0f * s));
            render_drop_shadow(rid, 1.0, {0, 0, 0, 1.0f}, std::round(rounding * s), 2.0, shadow);
            rect(b, RGBA(col.r, col.g, col.b, col.a), 0, std::round(rounding * s), 2.0f, true, 0.1);
            col = color_sel_border_color();
            border(b, RGBA(col.r, col.g, col.b, col.a), std::round(1.0f * s), 0, std::round(rounding * s), 2.0f, false, 1.0);


            // TEXT is center aligned label with trailing ... and max height of 2 ilnes
        }
    };

    on_change_in_desktop_folder();

    watch_desktop_folder();
}

void desktop_icons::stop() {
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
}

void desktop_icons::deselect() {
    for (int i = actual_root->children.size() - 1; i >= 0; i--) {
        auto child = actual_root->children[i];
        if (child->custom_type == (int) TYPE::DESKTOP_ICONS) {
            clear_desktop_selection(child);
        }
    }
}

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
    return hypriso->get_varint("plugin:mylardesktop:desktop_vertical", 1) != 0;
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

int two_line_height = 24;
 
struct DesktopItem {
    std::string full_filepath;
    std::string name;
    std::string extension;
    std::string icon;
    std::string exec;
    bool is_folder = false;
    std::vector<std::string> icons_for_mime;
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
            item->name = scannedItem.name;
            item->extension = scannedItem.extension;
            item->icon = scannedItem.icon;
            item->exec = scannedItem.exec;
            item->is_folder = scannedItem.is_folder;
            item->icons_for_mime = scannedItem.icons_for_mime;
            continue;
        }

        auto item = new DesktopItem(scannedItem);
        desktop_items.push_back(item);
    }

    std::ranges::sort(desktop_items, [](const DesktopItem* lhs, const DesktopItem* rhs) {
        if (lhs->is_folder != rhs->is_folder)
            return lhs->is_folder > rhs->is_folder;
        return lowercase(lhs->name) < lowercase(rhs->name);
    });
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
    
    ~IcoContainerData() {
        //notify(fz("{} was deleted", name));
        auto text_img = *datum<TextureInfo>(c, "label");
        free_text_texture(text_img.id);
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
    float shrink = 1 / s;
    m.height = ico_width + two_line_height * shrink + 13;
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

static std::vector<Container *> icons_for_drag(Container *desktop, Container *primary) {
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
    auto s = scale(hypriso->monitor_from_cursor());
    *datum<DesktopItem *>(c, "DesktopItem") = item;
    *datum<TextureInfo>(c, "label") = TextureInfo();
    {
        auto path = one_shot_icon(conf_icon_size() * s, {"folder"});
        *datum<TextureInfo>(c, "folder") = gen_texture(path, conf_icon_size() * s);
    }
    {
        auto path = one_shot_icon(conf_icon_size() * s, {"text-plain"});
        *datum<TextureInfo>(c, "text-plain") = gen_texture(path, conf_icon_size() * s);
    }
    {
        *datum<TextureInfo>(c, "icon") = TextureInfo();
        *datum<bool>(c, "icon_attempted") = false;
    }
     
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
        auto icons = icons_for_drag(c->parent, c);
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
        auto ico = (IcoContainerData *) c->user_data;
        bool shift_down = *datum<bool>(c->parent, "shift_held");
        if (!shift_down)
            clear_desktop_selection(c->parent);
        auto current = get_current_time_in_ms();
        if ((current - ico->last_time_pressed) < 700) {
            DesktopItem *item = *datum<DesktopItem *>(c, "DesktopItem");
            if (item->extension == ".desktop")
                launch_command(item->exec);
            else
                launch_command(fz("xdg-open \"{}\"", item->full_filepath));
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
                    draw_texture(info, 
                        c->real_bounds.x + c->real_bounds.w * .5 - info.w * .5, 
                        c->real_bounds.y + 2 * s, overview_alpha);
                }
            }
            
            auto* ico = (IcoContainerData*)(c->user_data);
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
                text_img = gen_text_texture(mylar_font, item->name, conf_font_size() * s, RGBA(1, 1, 1, 1), c->real_bounds.w, two_line_height, 1);
                *datum<TextureInfo>(c, "label") = text_img;
            }
            draw_texture(text_img, 
                c->real_bounds.x,
                c->real_bounds.y + c->real_bounds.h - text_img.h - 6 * s, overview_alpha);
        }
    };

}

static void create_root_popup() {
    auto m = mouse();
    std::vector<PopOption> root;
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
    auto in = gen_text_texture(mylar_font, "W\n", conf_font_size() * scale(hypriso->monitor_from_cursor()), RGBA(1, 1, 1, 1));
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

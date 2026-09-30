//
// Created by jmanc3 on 2/10/22.
//

#include "icons.h"

#include <cassert>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <math.h>
#include <optional>
#include <pango/pangocairo.h>
#include <sstream>
#include <string>
#include <stdexcept>
#include <sys/mman.h>
#include <sys/stat.h>
#include <thread>
#include <unordered_map>
#include <vector>
#include <zconf.h>

#ifdef TRACY_ENABLE
#include "tracy/Tracy.hpp"
#endif

static uint32_t cache_version = 3;
std::atomic<bool> icons_loaded = false;

int getExtension(unsigned short int i) {
    // return the top two bits
    return i >> 14;
}

int getParentIndex(unsigned short int i) {
    unsigned short int temp = i;
    // turn off the top two bits
    temp &= ~(1UL << (15));
    temp &= ~(1UL << (14));
    return temp;
}

struct Option {
    unsigned short int parentIndexAndExtension;
    unsigned char      themeIndex;
};

static long get_current_time_in_ms() {
    using namespace std::chrono;
    milliseconds currentTime = duration_cast<milliseconds>(system_clock::now().time_since_epoch());
    return currentTime.count();
}

// Should be serializable.
struct OptionsData {
    // full path
    std::vector<std::string> parentPaths;

    std::vector<std::string> themes;

    // key is the name
    // we don't use an unordered map because we need to search by key when user is
    // looking for icons
    std::map<std::string, std::vector<Option>> options;

    unsigned short int parentIndexOf(const std::string& path) {
        for (int i = parentPaths.size() - 1; i >= 0; --i) {
            if (parentPaths[i] == path) {
                return i;
            }
        }
        parentPaths.emplace_back(path);
        return parentPaths.size() - 1;
    }

    unsigned short int themeIndexOf(const std::string& path) {
        for (int i = themes.size() - 1; i >= 0; --i) {
            if (themes[i] == path) {
                return i;
            }
        }
        themes.emplace_back(path);
        return themes.size() - 1;
    }
};

// A background rescan must not change the live lookup thread's search paths.
static thread_local std::vector<std::string> icon_search_paths;
struct Range {
    unsigned long start  = -1;
    unsigned long length = -1;
};
struct IconCache {
    OptionsData data;
    std::vector<char> name_buffer;
    std::vector<char> option_buffer;
    std::unordered_map<std::string_view, Range> ranges;
};

static std::atomic<std::shared_ptr<const IconCache>> active_cache{std::make_shared<IconCache>()};
static std::mutex cache_update_mutex;

static void traverse_dir(const char* path, OptionsData *data, const std::vector<std::string>& icon_search_paths) {
    DIR* dir = opendir(path);
    if (dir == nullptr) {
        return;
    }
    auto close_directory = [](DIR *directory) { closedir(directory); };
    std::unique_ptr<DIR, decltype(close_directory)> directory(dir, close_directory);

    std::string path_as_string(path);
    std::string theme;
    for (const auto& item : icon_search_paths) {
        if (path_as_string.find(item) == 0) {
            theme = path_as_string.substr(item.size());
            if (theme.empty()) {
                theme = path;
            } else {
                // Remove the first slash
                theme = theme.substr(1);
                // Remove everything after slash
                theme = theme.substr(0, theme.find('/'));
            }
            break;
        }
    }
    unsigned short int current_theme_index  = data->themeIndexOf(theme);
    unsigned short int current_parent_index = data->parentIndexOf(path);

    struct dirent*     entry;
    struct stat        entryStat;
    while ((entry = readdir(dir)) != nullptr) {
        size_t name_len = strlen(entry->d_name);
        if (name_len > 5) {
            if (entry->d_name[name_len - 4] == '.') {
                size_t first  = name_len - 3;
                size_t second = name_len - 2;
                size_t third  = name_len - 1;

                int    svgs   = entry->d_name[first] == 's';
                int    svgv   = entry->d_name[second] == 'v';
                int    svgg   = entry->d_name[third] == 'g';
                int    svgsvg = svgs + svgv + svgg;

                if (svgsvg == 3) {
                    Option option                  = {};
                    option.parentIndexAndExtension = (current_parent_index & 0x3FFF) | (0 << 14);
                    option.themeIndex              = current_theme_index;
                    entry->d_name[name_len - 4]    = '\0';
                    (&data->options[entry->d_name])->push_back(option);
                    continue;
                }

                int pngp   = entry->d_name[first] == 'p';
                int pngv   = entry->d_name[second] == 'n';
                int pngg   = entry->d_name[third] == 'g';
                int pngpng = pngg + pngv + pngp;

                if (pngpng == 3) {
                    Option option                  = {};
                    option.parentIndexAndExtension = (current_parent_index & 0x3FFF) | (1 << 14);
                    option.themeIndex              = current_theme_index;
                    entry->d_name[name_len - 4]    = '\0';
                    (&data->options[entry->d_name])->push_back(option);
                    continue;
                }
            }
        }
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;

        char file[PATH_MAX];
        snprintf(file, PATH_MAX, "%s/%s", path, entry->d_name);
        if (stat(file, &entryStat) == -1)
            continue;
        if (S_ISDIR(entryStat.st_mode)) {
            traverse_dir(file, data, icon_search_paths);
        } else if (S_ISLNK(entryStat.st_mode)) {
            char    link[PATH_MAX];
            ssize_t len = readlink(file, link, sizeof(link));
            if (len != -1) {
                link[len] = '\0';
                traverse_dir(link, data, icon_search_paths);
            }
        }
    }
}

static void generate_data(OptionsData *data, const std::vector<std::string>& icon_search_paths) {
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif
    for (auto item : data->options)
        item.second.clear();
    data->options.clear();
    data->parentPaths.clear();
    data->themes.clear();

    const std::filesystem::directory_options searchOptions =
        (std::filesystem::directory_options::follow_directory_symlink | std::filesystem::directory_options::skip_permission_denied);
    struct stat st{};
    for (const auto& search_path : icon_search_paths) {
        if (stat(search_path.c_str(), &st) != 0)
            continue;

        traverse_dir(search_path.data(), data, icon_search_paths);
    }
}

//
//
// IF WM_NAME OR NAME SET ON WINDOW, CHECK THROUGH ALL .DESKTOP FILES FOR MATCH,
// AND USE ICON SPECIFIED or _KDE_NET_WM_DESKTOP_FILE property set or
// _GTK_APPLICATION_ID property set
//
//

static void save_data(const OptionsData *data) {
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif
    std::string icon_cache_path("/var/tmp/winbar_icon.cache");
    std::string icon_cache_temp_path("/var/tmp/winbar_icon.cache.tmp");
    
    std::ofstream cache_file;
    cache_file.open(icon_cache_temp_path, std::ios_base::out | std::ios_base::binary);
    if (!cache_file.is_open())
        throw std::runtime_error("Cannot open temporary icon cache");

    // version (string)
    cache_file << std::to_string(cache_version) << '\0';

#define WRITE_NUM(num) reinterpret_cast<const char*>(&num), sizeof(num)

    // We will use this number when loading, so we can pre-allocate a buffer that
    // will contain every option name We won't need to zero terminate the strings
    // in the buffer since we're going make a string view of the names when we
    // load them.
    unsigned long option_names_buffer_size = 0;
    for (const auto& item : data->options)
        option_names_buffer_size += item.first.size();
    cache_file.write(WRITE_NUM(option_names_buffer_size));

    // We will use this number to pre-allocate a buffer which will store the
    // parentIndexAndExtension and, themeIndex contiguously. We'll access the data
    // via a hash_table (std::unordered_map) which will take a string view and
    // return a Range{int index, int count}, into the pre-allocated buffer.
    unsigned long option_data_buffer_size = 0;
    for (const auto& item : data->options)
        option_data_buffer_size += item.second.size() * (sizeof(Option::parentIndexAndExtension) + sizeof(Option::themeIndex));
    cache_file.write(WRITE_NUM(option_data_buffer_size));

    // parent paths size (int)
    cache_file << std::to_string(data->parentPaths.size()) << '\0';
    for (const auto& item : data->parentPaths) {
        // parent paths (string)
        cache_file << item << '\0';
    }

    // themes paths size (int)
    cache_file << std::to_string(data->themes.size()) << '\0';
    for (const auto& item : data->themes) {
        // parent paths (string)
        cache_file << item << '\0';
    }

    // options size (int)
    cache_file << std::to_string(data->options.size()) << '\0';
    for (const auto& item : data->options) {
        // option name (string)
        cache_file << item.first << '\0';

        // option size (unsigned short int)
        unsigned short int optionsVectorSize = item.second.size();
        cache_file.write(WRITE_NUM(optionsVectorSize));

        for (const auto& option : item.second) {
            // (unsigned short int)
            cache_file.write(WRITE_NUM(option.parentIndexAndExtension));
            //            cache_file << std::to_string(option.parentIndexAndExtension)
            //            << '\0';
            // (char)
            cache_file.write(WRITE_NUM(option.themeIndex));
            //            cache_file << std::to_string(option.themeIndex) << '\0';
        }
    }

    cache_file.close();
    rename(icon_cache_temp_path.data(), icon_cache_path.data());
}

static void load_data() {
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif
    std::ifstream cache_file("/var/tmp/winbar_icon.cache", std::ios::binary | std::ios::ate);
    if (!cache_file)
        throw std::runtime_error("Cannot open icon cache");

    auto file_size = cache_file.tellg();
    if (file_size <= 0)
        throw std::runtime_error("Empty icon cache");
    std::vector<char> bytes(static_cast<size_t>(file_size));
    cache_file.seekg(0);
    if (!cache_file.read(bytes.data(), bytes.size()))
        throw std::runtime_error("Cannot read icon cache");

    size_t index = 0;
    auto read_string = [&]() {
        auto begin = bytes.data() + index;
        auto end = static_cast<const char *>(std::memchr(begin, '\0', bytes.size() - index));
        if (!end)
            throw std::runtime_error("Truncated icon cache string");
        index += end - begin + 1;
        return std::string(begin, end - begin);
    };
    auto read_number = [&](auto &number) {
        if (sizeof(number) > bytes.size() - index)
            throw std::runtime_error("Truncated icon cache number");
        std::memcpy(&number, bytes.data() + index, sizeof(number));
        index += sizeof(number);
    };
    auto read_count = [&]() {
        auto text = read_string();
        size_t consumed = 0;
        auto count = std::stoull(text, &consumed);
        if (consumed != text.size() || count > bytes.size())
            throw std::runtime_error("Invalid icon cache count");
        return static_cast<size_t>(count);
    };

    if (read_count() != cache_version)
        throw std::runtime_error("Unsupported icon cache version");

    auto next = std::make_shared<IconCache>();
    unsigned long names_size = 0;
    unsigned long options_size = 0;
    read_number(names_size);
    read_number(options_size);
    if (names_size > bytes.size() || options_size > bytes.size())
        throw std::runtime_error("Invalid icon cache buffer size");
    next->name_buffer.resize(names_size);
    next->option_buffer.resize(options_size);

    auto parents_count = read_count();
    for (size_t i = 0; i < parents_count; ++i)
        next->data.parentPaths.push_back(read_string());
    auto themes_count = read_count();
    for (size_t i = 0; i < themes_count; ++i)
        next->data.themes.push_back(read_string());

    auto entries_count = read_count();
    next->ranges.reserve(entries_count);
    size_t name_index = 0;
    size_t option_index = 0;
    for (size_t i = 0; i < entries_count; ++i) {
        auto name = read_string();
        unsigned short option_count = 0;
        read_number(option_count);
        size_t option_bytes = static_cast<size_t>(option_count) * 3;
        if (name.empty() || name.size() > names_size - name_index ||
            option_bytes > options_size - option_index || option_bytes > bytes.size() - index)
            throw std::runtime_error("Invalid icon cache entry");
        std::memcpy(next->name_buffer.data() + name_index, name.data(), name.size());
        std::string_view view(next->name_buffer.data() + name_index, name.size());
        if (!next->ranges.emplace(view, Range{option_index, option_bytes}).second)
            throw std::runtime_error("Duplicate icon cache entry");
        name_index += name.size();

        for (size_t j = 0; j < option_count; ++j) {
            unsigned short parent = 0;
            unsigned char theme = 0;
            read_number(parent);
            read_number(theme);
            if (getParentIndex(parent) >= next->data.parentPaths.size() || theme >= next->data.themes.size())
                throw std::runtime_error("Invalid icon cache path index");
            std::memcpy(next->option_buffer.data() + option_index, &parent, sizeof(parent));
            std::memcpy(next->option_buffer.data() + option_index + sizeof(parent), &theme, sizeof(theme));
            option_index += 3;
        }
    }
    if (name_index != names_size || option_index != options_size || index != bytes.size())
        throw std::runtime_error("Invalid icon cache size");

    // Readers retain their snapshot until the lookup finishes.
    std::shared_ptr<const IconCache> ready = std::move(next);
    active_cache.store(std::move(ready));
    icons_loaded = true;
}

void update_paths() {
    icon_search_paths.clear();

    // Setup search paths for icons
    //
    const char* h = getenv("HOME");
    std::string home;
    if (h)
        home = std::string(h);
    icon_search_paths.emplace_back("/usr/share/icons");
    icon_search_paths.emplace_back("/usr/local/share/icons");
    icon_search_paths.emplace_back(home + "/.icons");
    icon_search_paths.emplace_back(home + "/.local/share/icons");
    icon_search_paths.emplace_back("/var/lib/flatpak/exports/share/icons");
    icon_search_paths.emplace_back(home + "/.local/share/flatpak/exports/share/icons");
    const char* d = getenv("XDG_DATA_DIRS");
    std::string dirs;
    if (d)
        dirs = std::string(d);
    if (!dirs.empty()) {
        auto header_stream = std::stringstream{dirs};
        for (std::string dir; std::getline(header_stream, dir, ':');) {
            bool already_going_to_search = false;
            for (const auto& search_path : icon_search_paths) {
                if (dir == search_path) {
                    already_going_to_search = true;
                    break;
                }
            }
            if (!already_going_to_search)
                if (dir.find("icons") != std::string::npos)
                    icon_search_paths.emplace_back(dir);
        }
    }
    icon_search_paths.emplace_back("/usr/share/pixmaps");
}

void generate_cache() {
    std::lock_guard<std::mutex> lock(cache_update_mutex);
    update_paths();
    OptionsData generated;
    generate_data(&generated, icon_search_paths);
    save_data(&generated);
}

bool icon_cache_needs_update() {
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif
    update_paths();
    std::string path = "/var/tmp/winbar_icon.cache";

    if (std::filesystem::exists(path)) {
        // If cache version is not the same as modern cache version, icon cache needs update
        std::ifstream cache_file(path, std::ios::binary);
        std::string version;
        if (!std::getline(cache_file, version, '\0') || version != std::to_string(cache_version))
            return true;

        auto cache_time = std::filesystem::last_write_time(path);
        // If any icon folders are newer than cache
        for (auto p : icon_search_paths) {
            if (!std::filesystem::exists(p))
               continue; 
            auto icon_folder_time = std::filesystem::last_write_time(p);
            if (icon_folder_time > cache_time) {
                return true;
            }
        }

        return false;
    }
    
    return true;
}

bool icon_cache_generate() {
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif
    try {
        generate_cache();
        return true;
    } catch (const std::exception &error) {
        fprintf(stderr, "Icon cache generation failed: %s\n", error.what());
        return false;
    }
}

bool icon_cache_load() {
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif
    std::lock_guard<std::mutex> lock(cache_update_mutex);
    try {
        load_data();
        return true;
    } catch (const std::exception &error) {
        fprintf(stderr, "Icon cache loading failed: %s\n", error.what());
        return false;
    }
}

bool equals_case_insensitive(std::string_view a, std::string_view b) {
    if (a.size() != b.size())
        return false;

    for (size_t i = 0; i < a.size(); ++i) {
        unsigned char ca = static_cast<unsigned char>(a[i]);
        unsigned char cb = static_cast<unsigned char>(b[i]);

        if (std::tolower(ca) != std::tolower(cb))
            return false;
    }

    return true;
}

void search_icons(std::vector<IconTarget>& targets) {
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif
    auto cache = active_cache.load();
    const auto &ranges = cache->ranges;
    const auto *data = &cache->data;
    const auto *option_buffer = cache->option_buffer.data();
    for (int i = 0; i < targets.size(); ++i) {
        auto             &target = targets[i];
        target.was_searched = true;

        std::string_view target_name = target.name.c_str();
        if (target.name.size() > 2 && target.name[0] == ':' && target.name.find(':', 1) != std::string::npos) {
            int start   = target.name.find(':', 1);
            target_name = std::string_view(target.name.data() + start + 1, target.name.size() - start - 1);
        }

        bool easy_has = ranges.find(target_name) != ranges.end();
        Range range;
        bool found = false;
        std::string entry_name = std::string(target_name);

        if (!easy_has) {
            for (const auto &entry : ranges) {
                if (equals_case_insensitive(entry.first, target_name)) {
                    found = true;
                    easy_has = true;
                    range = entry.second;
                    entry_name = entry.first;
                    break;
                }
            }
        }

        if (!easy_has) {
            // Could be a path
            if (!target_name.empty() && target_name[0] == '/') {
                Candidate candidate;

                // Extract parent path
                size_t last_slash = target_name.find_last_of("/");
                if (last_slash != std::string::npos) {
                    candidate.parent_path = target_name.substr(0, last_slash);

                    // Extract filename
                    size_t last_dot = target_name.find_last_of(".");
                    if (last_dot != std::string::npos) {
                        candidate.filename = target_name.substr(last_slash + 1, last_dot - last_slash - 1);

                        // Determine extension
                        std::string extension_str = std::string(target_name.substr(last_dot));
                        std::transform(extension_str.begin(), extension_str.end(), extension_str.begin(), ::tolower);
                        if (extension_str == ".png") {
                            candidate.extension = 1;
                        } else if (extension_str == ".svg") {
                            candidate.extension = 0;
                        } else if (extension_str == ".xpm") {
                            candidate.extension = 2;
                        } else {
                            candidate.extension = 1; // Unknown extension
                        }

                        // Set default values for other fields
                        candidate.theme   = "hardcoded";
                        candidate.size    = 48; // Default size
                        candidate.scale   = 1;  // Default scale
                        candidate.context = IconContext::Apps;

                        targets[i].candidates.push_back(candidate);
                    }
                }
            }

            continue;
        }
        if (!found)
            range = ranges.at(target_name);

        std::vector<Candidate> candidates;
        for (int j = 0; j < (range.length / 3); ++j) {
            unsigned long      actual_index            = range.start + j * 3;
            unsigned short int parentIndexAndExtension = 0;
            std::memcpy(&parentIndexAndExtension, option_buffer + actual_index, sizeof(unsigned short int));
            unsigned char themeIndex = 0;
            std::memcpy(&themeIndex, option_buffer + actual_index + sizeof(unsigned short int), sizeof(unsigned char));

            Candidate candidate;
            candidate.parent_path = data->parentPaths[getParentIndex(parentIndexAndExtension)];
            candidate.filename    = entry_name;
            candidate.theme       = data->themes[themeIndex];
            candidate.extension   = getExtension(parentIndexAndExtension);
            candidate.context     = IconContext::NotSet;
            // The following is to set the icon 'context' based on the parent_path
            struct ICMap {
                std::string name;
                IconContext context;
            };
            std::vector<ICMap> ics = {
                {"/actions", IconContext::Actions},     {"/animations", IconContext::Animations}, {"/apps", IconContext::Apps},       {"/categories", IconContext::Categories},
                {"/devices", IconContext::Devices},     {"/emblems", IconContext::Emblems},       {"/emotes", IconContext::Emotes},   {"/intl", IconContext::Intl},
                {"/mimetypes", IconContext::Mimetypes}, {"/places", IconContext::Places},         {"/status", IconContext::Statuses}, {"/panel", IconContext::Panel}};
            std::string path_copy = candidate.parent_path;
            for (char& t : path_copy)
                t = std::tolower(t);
            for (const auto& item : ics)
                if (path_copy.find(item.name) != std::string::npos)
                    candidate.context = item.context;

            // The following is to determine the size and scale of the icon based on
            // the parent path
            unsigned long startIndex = candidate.parent_path.find(candidate.theme);
            if (startIndex == std::string::npos)
                startIndex = 0;
            startIndex += candidate.theme.size() + 1;

            char buffer[64];
            int  buffer_len = 0;
            bool found_at   = false;
            int  scale      = 0;
            int  size       = 0;

            // Iterate through the characters in the path string
            for (int i = startIndex; i < candidate.parent_path.length(); i++) {
                if (scale != 0 && size != 0)
                    break;

                char c = candidate.parent_path[i];
                if (isdigit(c)) {
                    // Save the digit character to the buffer
                    buffer[buffer_len] = c;
                    buffer_len++;
                } else if (c == '@') {
                    if (buffer_len != 0) {
                        buffer[buffer_len] = '\0';
                        size               = atoi(buffer);
                    }
                    found_at   = true;
                    buffer_len = 0;
                } else if (c == '/' || c == 'x' || c == 'X' || i == candidate.parent_path.length() - 1) {
                    if (found_at && buffer_len != 0) {
                        // Convert the buffer to an integer and save it to the scale
                        // variable
                        buffer[buffer_len] = '\0';
                        scale              = atoi(buffer);
                    } else if (buffer_len != 0) {
                        // Convert the buffer to an integer and save it to the size variable
                        buffer[buffer_len] = '\0';
                        size               = atoi(buffer);
                    }
                    // Reset the buffer and the found_at flag
                    buffer_len = 0;
                    found_at   = false;
                }
            }
            if (found_at && buffer_len != 0) {
                // Convert the buffer to an integer and save it to the scale variable
                buffer[buffer_len] = '\0';
                scale              = atoi(buffer);
            } else if (buffer_len != 0) {
                // Convert the buffer to an integer and save it to the size variable
                buffer[buffer_len] = '\0';
                size               = atoi(buffer);
            }
            candidate.size  = size;
            candidate.scale = scale;
            candidates.push_back(candidate);
        }

        for (const auto& item : candidates)
            targets[i].candidates.push_back(item);
    }
}

static std::string get_current_theme_name() {
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif
    static std::string current_theme;
    static long        previous_time_cached;
    if (!current_theme.empty()) {
        long current_time = get_current_time_in_ms();
        long elapsed_time = current_time - previous_time_cached;
        if (elapsed_time < 1000 * 20) {
            return current_theme;
        }
    }

    auto current_desktop = std::getenv("XDG_CURRENT_DESKTOP");
    if (current_desktop != nullptr) {
        if (strcmp(current_desktop, "KDE") == 0) {
            std::string kde_settings(getenv("HOME"));
            kde_settings += "/.config/kdeglobals";

            std::ifstream in(kde_settings);
            std::string   line;
            const char*   target_title     = "[Icons]";
            const char*   target_child     = "Theme=";
            bool          foundFirstTarget = false;
            while (std::getline(in, line)) {
                if (!foundFirstTarget) {
                    if (line.find(target_title) != std::string::npos) {
                        foundFirstTarget = true;
                    }
                } else {
                    if (line.find(target_child) != std::string::npos) {
                        previous_time_cached = get_current_time_in_ms();
                        current_theme        = line.substr(strlen(target_child));
                        return current_theme;
                    }
                }
            }
        }
    }

    std::string gtk_settings_file_path(getenv("HOME"));
    gtk_settings_file_path += "/.config/gtk-3.0/settings.ini";

    std::ifstream in(gtk_settings_file_path);

    std::string   line;
    const char*   target = "gtk-icon-theme-name=";
    while (std::getline(in, line)) {
        if (line.find(target) != std::string::npos) {
            previous_time_cached = get_current_time_in_ms();
            current_theme        = line.substr(strlen(target));
            return current_theme;
        }
    }
    previous_time_cached = get_current_time_in_ms();
    current_theme        = "hicolor";
    return current_theme;
}

static void c3ic_generate_sizes(int target_size, std::vector<int>& target_sizes) {
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif
    target_sizes.push_back(8);
    target_sizes.push_back(12);
    target_sizes.push_back(16);
    target_sizes.push_back(18);
    target_sizes.push_back(24);
    target_sizes.push_back(32);
    target_sizes.push_back(42);
    target_sizes.push_back(48);
    target_sizes.push_back(64);
    target_sizes.push_back(84);
    target_sizes.push_back(96);
    target_sizes.push_back(128);
    target_sizes.push_back(192);
    target_sizes.push_back(256);
    target_sizes.push_back(512);

    std::sort(target_sizes.begin(), target_sizes.end(), [target_size](int a, int b) {
        // Prefer higher pixel icons to lower ones
        long absolute_difference_between_a_and_the_target = std::abs(target_size - a);
        bool a_is_too_low                                 = a < target_size;
        long absolute_difference_between_b_and_the_target = std::abs(target_size - b);
        bool b_is_too_low                                 = b < target_size;

        if (a_is_too_low || b_is_too_low) {
            if (a_is_too_low && !b_is_too_low)
                return false;
            if (b_is_too_low && !a_is_too_low)
                return true;
            return absolute_difference_between_a_and_the_target < absolute_difference_between_b_and_the_target;
        }

        return absolute_difference_between_a_and_the_target < absolute_difference_between_b_and_the_target;
    });
}

// We should get rid of this function and be more specific at the calls sites
// with what they need
void pick_best(std::vector<IconTarget>& targets, int target_size) {
    pick_best(targets, target_size, IconContext::Apps);
}

void pick_best(std::vector<IconTarget>& targets, int target_size, IconContext target_context) {
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif
    auto             current_theme = get_current_theme_name();
    std::vector<int> strict_sizes;
    c3ic_generate_sizes(target_size, strict_sizes);
    std::vector<int> large_to_small;
    c3ic_generate_sizes(512, large_to_small);
    for (int ss = 0; ss < targets.size(); ss++) {
        IconTarget*      target              = &targets[ss];
        bool             has_preferred_theme = false;
        std::string_view preferred_theme;
        if (target->name.size() > 2 && target->name[0] == ':' && target->name.find(':', 1) != std::string::npos) {
            has_preferred_theme = true;
            int start           = target->name.find(':', 1);
            preferred_theme     = std::string_view(target->name.data() + 1, start - 1);
        }

        if (!target->name.empty() && target->name[0] == '/') {
            // If the target is just a full path, then just return the full path
            target->best_full_path = target->name;
        } else {
            //assert(target->was_searched);
            for (int i = 0; i < target->candidates.size(); i++) {
                Candidate* candidate                 = &target->candidates[i];
                candidate->is_part_of_current_theme  = current_theme == candidate->theme;
                candidate->is_part_of_target_context = candidate->context == target_context;
                if (has_preferred_theme) {
                    candidate->is_part_of_preferred_theme = candidate->theme == preferred_theme;
                }

                if (candidate->context == IconContext::NotSet)
                    candidate->is_part_of_target_context = false;
                candidate->size_index = strict_sizes.size() + 1;
                for (int size_index = 0; size_index < strict_sizes.size(); size_index++) {
                    if (starts_with(target->name, "steam_icon_")) {
                        if (candidate->size == large_to_small[size_index]) {
                            candidate->size_index = size_index;
                            break;
                        }
                    } else {
                        if (candidate->size == strict_sizes[size_index]) {
                            candidate->size_index = size_index;
                            break;
                        }
                    }
                }
                if (candidate->size == 0 && candidate->extension == 1) {
                    candidate->size_index = strict_sizes.size() + 1;
                }
            }
            // Sort vector based on quality and size, and current theme
            // Set best_full_path equal to best top option
            std::sort(target->candidates.begin(), target->candidates.end(), [current_theme](Candidate lhs, Candidate rhs) {
                if (lhs.is_part_of_preferred_theme == rhs.is_part_of_preferred_theme) {
                    if (lhs.is_part_of_current_theme == rhs.is_part_of_current_theme) {
                        if (lhs.is_part_of_target_context == rhs.is_part_of_target_context) {
                            if (lhs.size_index == rhs.size_index) {
                                if (lhs.extension == rhs.extension) {
                                    return lhs.scale < rhs.scale;
                                } else {
                                    return lhs.extension < rhs.extension;
                                }
                            } else {
                                return lhs.size_index < rhs.size_index;
                            }
                        } else {
                            return lhs.is_part_of_target_context > rhs.is_part_of_target_context;
                        }
                    }
                    return lhs.is_part_of_current_theme > rhs.is_part_of_current_theme;
                }
                return lhs.is_part_of_preferred_theme > rhs.is_part_of_preferred_theme;
            });

            if (!target->candidates.empty())
                target->best_full_path = target->candidates[0].full_path();
        }
    }
}

std::string one_shot_icon(int size, const std::vector<std::string>& alt_names) {
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif
    std::vector<IconTarget> targets;
    for (auto n : alt_names) {
        targets.emplace_back(n);
    }
    search_icons(targets);
    pick_best(targets, size);
    for (auto& t : targets) {
        if (!t.best_full_path.empty()) {
            return t.best_full_path;
        }
    }
    return "";
}

void unload_icons() {
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif
    std::lock_guard<std::mutex> lock(cache_update_mutex);
    std::shared_ptr<const IconCache> empty = std::make_shared<IconCache>();
    active_cache.store(std::move(empty));
    icons_loaded = false;
    icon_search_paths.clear();
}

std::string c3ic_fix_desktop_file_icon(const std::string& given_name, const std::string& given_wm_class, const std::string& given_path, const std::string& given_icon) {
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif
    // mmap tofix.csv file
    const char* home_directory = getenv("HOME");
    std::string to_fix_path(home_directory);
    to_fix_path += "/.config/winbar/tofix.csv";

    struct stat buffer{};
    int         cache_exists = stat(to_fix_path.c_str(), &buffer) == 0;

    // TODO: we have to compare modified time of the folders to see if we are up
    // to date
    if (!cache_exists) {
        //        printf("%s doesn't exists\n", to_fix_path.c_str());
        return given_icon;
    }

    // Attempt to mmap the file
    int         file_descriptor = open(to_fix_path.c_str(), O_RDWR, S_IRUSR | S_IWUSR);
    struct stat sb;
    if (fstat(file_descriptor, &sb) == -1) {
        //        printf("Couldn't get file size: %s\n", to_fix_path.c_str());
        close(file_descriptor);
        return given_icon;
    }

    char* to_fix_data = (char*)mmap(NULL, sb.st_size, PROT_READ | PROT_WRITE, MAP_SHARED, file_descriptor, 0);
    if (!to_fix_data) {
        //        printf("Couldn't mmap file.\n");
        close(file_descriptor);
        return given_icon;
    }

    unsigned long index_into_file = 0;

    // Eat the first line;
#define MNOT_DONE index_into_file < buffer.st_size
    while (MNOT_DONE && to_fix_data[index_into_file] != '\n') {
        index_into_file++;
    }
    index_into_file++;

    int         loading_type     = 0;
    int         offset           = 0;
    char        first[1024 * 6]  = {0};
    char        second[1024 * 6] = {0};
    char        third[1024 * 6]  = {0};
    char        fourth[1024 * 6] = {0};
    bool        should_change    = false;

    const char* given_name_c     = given_name.c_str();
    const char* given_wm_class_c = given_wm_class.c_str();

    char        given_path_c[1024 * 6] = {0};
    const char* temp_given_path_c      = given_path.c_str();

    for (int i = 0; i < given_path.size() + 1; i++) {
        char c = temp_given_path_c[i];
        if (c == '/') {
            offset = 0;
        } else {
            given_path_c[offset++] = c;
        }
    }
    offset = 0;

    while (MNOT_DONE) {
        if (to_fix_data[index_into_file] == ',') {
            switch (loading_type) {
                case 0: {
                    first[offset] = '\0';
                    if (strcmp(first, given_name_c) == 0) {
                        should_change = true;
                    }
                    break;
                }
                case 1: {
                    second[offset] = '\0';
                    if (strcmp(second, given_wm_class_c) == 0) {
                        should_change = true;
                    }
                    break;
                }
                case 2: {
                    third[offset] = '\0';

                    if (strcmp(third, given_path_c) == 0) {
                        should_change = true;
                    }
                    break;
                }
            }
            index_into_file++;
            loading_type++;
            offset = 0;
            continue;
        }
        if (to_fix_data[index_into_file] == '\n') {
            fourth[offset] = '\0';
            index_into_file++;
            if (should_change) {
                should_change = false;
                // return fourth
                munmap(to_fix_data, sb.st_size);
                close(file_descriptor);
                return fourth;
            }
            loading_type = 0;
            offset       = 0;
            continue;
        }
        switch (loading_type) {
            case 0: {
                first[offset++] = to_fix_data[index_into_file++];
                break;
            }
            case 1: {
                second[offset++] = to_fix_data[index_into_file++];
                break;
            }
            case 2: {
                // for the directory, we only want to save the file name not the entire
                // directory path
                char previous_char = to_fix_data[index_into_file];
                third[offset++]    = to_fix_data[index_into_file++];
                if (previous_char == '/') {
                    offset = 0;
                }
                break;
            }
            default: {
                fourth[offset++] = to_fix_data[index_into_file++];
                break;
            }
        }
    }

    munmap(to_fix_data, sb.st_size);
    close(file_descriptor);

    return given_icon;
}

std::string c3ic_fix_wm_class(const std::string& given_wm_class) {
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif
    
    if (given_wm_class == "steam")
        return given_wm_class;
    return c3ic_fix_desktop_file_icon(given_wm_class, given_wm_class, given_wm_class, given_wm_class);
}

bool has_options(const std::string& name) {
    auto cache = active_cache.load();
    const auto &ranges = cache->ranges;
    // Ignore preferred theme tag
    if (name.size() > 2 && name[0] == ':' && name.find(':', 1) != std::string::npos) {
        int              start          = name.find(':', 1);
        std::string_view icon_name_only = std::string_view(name.data() + start + 1, name.size() - start - 1);

        return ranges.find(icon_name_only) != ranges.end();
    }
    return ranges.find(name.c_str()) != ranges.end();
}

bool is_case_insensitive_substring(const std::string_view& str_view, const std::string_view& target) {
    return std::search(str_view.begin(), str_view.end(), target.begin(), target.end(), [](char a, char b) { return std::tolower(a) == std::tolower(b); }) != str_view.end();
}

void get_options(std::vector<std::string>& names, const std::string& name, int max) {
    auto cache = active_cache.load();
    const auto &ranges = cache->ranges;
    std::string_view icon_name_only = name.c_str();
    if (name.size() > 2 && name[0] == ':' && name.find(':', 1) != std::string::npos) {
        int start      = name.find(':', 1);
        icon_name_only = std::string_view(name.data() + start + 1, name.size() - start - 1);
    }
    for (const auto& entry : ranges) {
        if (is_case_insensitive_substring(entry.first, icon_name_only)) {
            bool only_print = true;
            for (auto c : entry.first) {
                if (!isprint(c))
                    only_print = false;
            }
            if (only_print) {
                names.emplace_back(entry.first);
                if (names.size() > max && max != 0)
                    return;
            }
        }
    }
}


std::vector<std::string> all_dirs;
std::vector<std::string> possible_dirs;

void gen_all_final_dirs() {
    namespace fs = std::filesystem;

    // go through each directory possible, enumerate every dir under it into a vector including root
    for (const auto& rootdir : icon_search_paths) {
        fs::path root(rootdir);

        if (!fs::exists(root))
            continue;

        if (fs::is_directory(root))
            all_dirs.push_back(root.string()); // include the root itself

        for (auto const& entry : fs::recursive_directory_iterator(root)) {
            if (entry.is_directory()) {
                all_dirs.push_back(entry.path().string());
            }
        }
    }
}

Candidate path_to_candidate(std::string path) {
    Candidate can;
    // Set default values for icons that are loose or in the root folder and not attached to a theme by default
    can.theme = "hardcoded";
    can.size = 48; // Default size
    can.scale = 1; // Default scale
    can.context = IconContext::Apps;

    std::string under = path;
    for (char &t: under)
        t = std::tolower(t);

    { // Context
        struct ICMap {
            std::string name;
            IconContext context;
        };
        std::vector<ICMap> ics = {{"/actions",    IconContext::Actions},
                                  {"/animations", IconContext::Animations},
                                  {"/apps",       IconContext::Apps},
                                  {"/categories", IconContext::Categories},
                                  {"/devices",    IconContext::Devices},
                                  {"/emblems",    IconContext::Emblems},
                                  {"/emotes",     IconContext::Emotes},
                                  {"/intl",       IconContext::Intl},
                                  {"/mimetypes",  IconContext::Mimetypes},
                                  {"/places",     IconContext::Places},
                                  {"/status",     IconContext::Statuses},
                                  {"/panel",      IconContext::Panel}};
        for (const auto &item: ics)
            if (under.find(item.name) != std::string::npos)
                can.context = item.context;
    }
    { // Extension
        if (under.find(".svg") != std::string::npos) {
            can.extension = 0;
        }
        if (under.find(".png") != std::string::npos) {
            can.extension = 1;
        }
        if (under.find(".xpm") != std::string::npos) {
            can.extension = 2;
        }
    }
    {
        size_t last_slash = path.find_last_of("/");
        if (last_slash != std::string::npos) {
            can.parent_path = path.substr(0, last_slash);

            // Extract filename
            size_t last_dot = path.find_last_of(".");
            if (last_dot != std::string::npos) {
                can.filename = path.substr(last_slash + 1, last_dot - last_slash - 1);
            }
        }
    }
    { // Theme
        for (auto f : icon_search_paths) {
            if (path.find(f) != std::string::npos) {
                auto s = path.substr(f.size() + 1);
                auto l = s.find("/");
                if (l != std::string::npos) {
                    can.theme = s.substr(0, l);

                    // extract size and scale from s if possible
                    char buffer[64];
                    int buffer_len = 0;
                    bool found_at = false;
                    int scale = 0;
                    int size = 0;

                    // Iterate through the characters in the path string
                    for (int i = 0; i < can.parent_path.length(); i++) {
                        if (scale != 0 && size != 0)
                            break;

                        char c = can.parent_path[i];
                        if (isdigit(c)) {
                            // Save the digit character to the buffer
                            buffer[buffer_len] = c;
                            buffer_len++;
                        } else if (c == '@') {
                            if (buffer_len != 0) {
                                buffer[buffer_len] = '\0';
                                size = atoi(buffer);
                            }
                            found_at = true;
                            buffer_len = 0;
                        } else if (c == '/' || c == 'x' || c == 'X' || i == can.parent_path.length() - 1) {
                            if (found_at && buffer_len != 0) {
                                // Convert the buffer to an integer and save it to the scale variable
                                buffer[buffer_len] = '\0';
                                scale = atoi(buffer);
                            } else if (buffer_len != 0) {
                                // Convert the buffer to an integer and save it to the size variable
                                buffer[buffer_len] = '\0';
                                size = atoi(buffer);
                            }
                            // Reset the buffer and the found_at flag
                            buffer_len = 0;
                            found_at = false;
                        }
                    }
                    if (found_at && buffer_len != 0) {
                        // Convert the buffer to an integer and save it to the scale variable
                        buffer[buffer_len] = '\0';
                        scale = atoi(buffer);
                    } else if (buffer_len != 0) {
                        // Convert the buffer to an integer and save it to the size variable
                        buffer[buffer_len] = '\0';
                        size = atoi(buffer);
                    }
                    can.size = size;
                    can.scale = scale;
                }

                // It's not visible here, but the case Where the icon is at the root path is being attended to here
                // as by default, We set the theme as hardcoded for that and sane defaults for the other aspects of the icon
            }
        }
    }

    return can;
}

std::string single_shot_icon_live(std::string icon, int size) {
    icon = c3ic_fix_wm_class(icon);
    {
#ifdef TRACY_ENABLE
        ZoneScopedN("single_shot one time cost");
#endif
        if (icon_search_paths.empty())
            update_paths();

        // One time cost (heavy)
        if (all_dirs.empty())
            gen_all_final_dirs();
    }

    {
#ifdef TRACY_ENABLE
        ZoneScopedN("single_shot possible");
#endif
        possible_dirs.clear();
        for (auto s : all_dirs) {
            auto svg = s + "/" + icon + ".svg";
            auto png = s + "/" + icon + ".png";
            auto xpm = s + "/" + icon + ".xpm";
            if (std::filesystem::exists(svg)) {
                possible_dirs.push_back(svg);
            }
            if (std::filesystem::exists(png)) {
                possible_dirs.push_back(png);
            }
            if (std::filesystem::exists(xpm)) {
                possible_dirs.push_back(xpm);
            }
        }
    }

    std::vector<IconTarget> targets;
    targets.push_back(IconTarget(icon));
    {
#ifdef TRACY_ENABLE
            ZoneScopedN("single_shot gen canidates");
#endif
        for (auto s : possible_dirs) {
            targets[0].candidates.push_back(path_to_candidate(s));
        }
        pick_best(targets, size);
        printf("%s\n", targets[0].best_full_path.c_str());
    }
    return targets[0].best_full_path;
}

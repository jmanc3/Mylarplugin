#include "desktop_icons.h"
#include "container.h"
#include "heart.h"
#include "hypriso.h"
#include "icons.h"

#include <linux/input-event-codes.h>
#include <algorithm>
#include <cctype>
#include <filesystem>
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

float conf_font_size = 11;
float conf_icon_size = 64;
float conf_pad = 10;
int two_line_height = conf_font_size * 2;
 
enum class DesktopItemType {
    DIRECTORY,
    IMAGE,
    VIDEO,
    AUDIO,
    DOCUMENT,
    TEXT,
    ARCHIVE,
    EXECUTABLE,
    DESKTOP_ENTRY,
    OTHER,
};

struct DesktopItem {
    std::string full_filepath;
    std::string name;
    std::string extension;
    bool is_folder = false;
    DesktopItemType type = DesktopItemType::OTHER;
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

static DesktopItemType desktop_item_type(const std::string& extension, const bool isFolder, const bool isExecutable) {
    static const std::unordered_set<std::string_view> imageExtensions = {
        ".avif", ".bmp", ".gif", ".heic", ".ico", ".jpeg", ".jpg", ".png", ".svg", ".tif", ".tiff", ".webp",
    };
    static const std::unordered_set<std::string_view> videoExtensions = {
        ".avi", ".m4v", ".mkv", ".mov", ".mp4", ".mpeg", ".mpg", ".webm",
    };
    static const std::unordered_set<std::string_view> audioExtensions = {
        ".aac", ".flac", ".m4a", ".mp3", ".ogg", ".opus", ".wav", ".wma",
    };
    static const std::unordered_set<std::string_view> documentExtensions = {
        ".doc", ".docx", ".epub", ".odf", ".odp", ".ods", ".odt", ".pdf", ".ppt", ".pptx", ".xls", ".xlsx",
    };
    static const std::unordered_set<std::string_view> textExtensions = {
        ".cfg", ".conf", ".csv", ".ini", ".json", ".log", ".md", ".rst", ".toml", ".txt", ".xml", ".yaml", ".yml",
    };
    static const std::unordered_set<std::string_view> archiveExtensions = {
        ".7z", ".bz2", ".gz", ".rar", ".tar", ".tgz", ".xz", ".zip", ".zst",
    };

    if (isFolder)
        return DesktopItemType::DIRECTORY;
    if (extension == ".desktop")
        return DesktopItemType::DESKTOP_ENTRY;
    if (imageExtensions.contains(extension))
        return DesktopItemType::IMAGE;
    if (videoExtensions.contains(extension))
        return DesktopItemType::VIDEO;
    if (audioExtensions.contains(extension))
        return DesktopItemType::AUDIO;
    if (documentExtensions.contains(extension))
        return DesktopItemType::DOCUMENT;
    if (textExtensions.contains(extension))
        return DesktopItemType::TEXT;
    if (archiveExtensions.contains(extension))
        return DesktopItemType::ARCHIVE;
    if (isExecutable)
        return DesktopItemType::EXECUTABLE;
    return DesktopItemType::OTHER;
}


static int latest_desktop_watch = -1;

void on_change_in_desktop_folder() {
    //notify("change in desktop foldler");

    const char* home = std::getenv("HOME");
    if (!home) {
        clear_desktop_items();
        return;
    }

    //const std::filesystem::path filepath = std::filesystem::path(home) / "Desktop";
    const std::filesystem::path filepath = std::filesystem::path(home);
    std::error_code ec;
    if (!std::filesystem::is_directory(filepath, ec)) {
        clear_desktop_items();
        return;
    }

    std::vector<DesktopItem> scanned;
    std::unordered_set<std::string> seenPaths;
    std::filesystem::directory_iterator iterator(filepath, std::filesystem::directory_options::skip_permission_denied, ec);
    const std::filesystem::directory_iterator end;

    while (!ec && iterator != end) {
        const auto& entry = *iterator;
        const auto path = entry.path();
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

        const auto extension = isFolder ? std::string{} : lowercase(path.extension().string());
        const auto fullPath = path.string();
        seenPaths.insert(fullPath);
        scanned.push_back(DesktopItem{
            .full_filepath = fullPath,
            .name          = isFolder ? path.filename().string() : path.stem().string(),
            .extension     = extension,
            .is_folder     = isFolder,
            .type          = desktop_item_type(extension, isFolder, isExecutable),
        });

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
            item->is_folder = scannedItem.is_folder;
            item->type = scannedItem.type;
            continue;
        }

        desktop_items.push_back(new DesktopItem{
            .full_filepath = scannedItem.full_filepath,
            .name          = scannedItem.name,
            .extension     = scannedItem.extension,
            .is_folder     = scannedItem.is_folder,
            .type          = scannedItem.type,
        });
    }

    std::ranges::sort(desktop_items, [](const DesktopItem* lhs, const DesktopItem* rhs) {
        if (lhs->is_folder != rhs->is_folder)
            return lhs->is_folder > rhs->is_folder;
        return lowercase(lhs->name) < lowercase(rhs->name);
    });
}

void watch_desktop_folder() {
    const char* home = std::getenv("HOME");
    //std::filesystem::path filepath = std::filesystem::path(home) / "Desktop";
    std::filesystem::path filepath = std::filesystem::path(home);
    if (!std::filesystem::exists(filepath))
        return;
    static Timer *t = nullptr;
    static long time_made = 0;
    
    latest_desktop_watch = watch_file(filepath.string(), [](FileWatchUpdate update, int fd) {
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

    ~IcoContainerData() {
        //notify(fz("{} was deleted", name));
        auto text_img = *datum<TextureInfo>(c, "label");
        free_text_texture(text_img.id);
    }
};

void create_desktop_icon(Container *parent, DesktopItem *item) {
    auto c = parent->child(FILL_SPACE, FILL_SPACE);
    auto ico = new IcoContainerData;
    ico->name = item->name;
    ico->c = c;
    c->user_data = ico;
    auto s = scale(hypriso->monitor_from_cursor());
    *datum<DesktopItem *>(c, "DesktopItem") = item;
    *datum<TextureInfo>(c, "label") = TextureInfo();
    {
        auto path = one_shot_icon(conf_icon_size * s, {"folder"});
        *datum<TextureInfo>(c, "folder") = gen_texture(path, conf_icon_size * s);
    }
    {
        auto path = one_shot_icon(conf_icon_size * s, {"text-plain"});
        *datum<TextureInfo>(c, "text-plain") = gen_texture(path, conf_icon_size * s);
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
    c->when_drag_end = c->when_mouse_motion;
   
    c->when_paint = [](Container* actual_root, Container* c) {
        auto root = get_rendering_root();
        auto [rid, s, stage, active_id] = roots_info(actual_root, root);
        if (stage == (int)STAGE::RENDER_POST_WALLPAPER) {
            renderfix
            
            DesktopItem *item = *datum<DesktopItem *>(c, "DesktopItem");

            {
                TextureInfo info;
                if (item->is_folder) {
                    info = *datum<TextureInfo>(c, "folder");
                } else {
                    info = *datum<TextureInfo>(c, "text-plain");
                }
                if (info.id != -1) {
                    draw_texture(info, c->real_bounds.x, c->real_bounds.y);
                }
            }
            
            if (c->state.mouse_pressing) {
                //rect(c->real_bounds, color_sel_color());
                border(c->real_bounds, color_sel_border_color(), std::round(1 * s));
            } else if (c->state.mouse_hovering) {
                rect(c->real_bounds, color_sel_color());
                border(c->real_bounds, color_sel_border_color(), std::round(1 * s));
            }
            
            
            TextureInfo text_img = *datum<TextureInfo>(c, "label");
            if (text_img.id == -1) {
                text_img = gen_text_texture(mylar_font, item->name, conf_font_size * s, RGBA(1, 1, 1, 1), c->real_bounds.w, two_line_height, 1);
                *datum<TextureInfo>(c, "label") = text_img;
            }
            draw_texture(text_img, 
                c->real_bounds.x,
                c->real_bounds.y + c->real_bounds.h + 4 * s);
        }
    };

}

void desktop_icons::start() {
    auto in = gen_text_texture(mylar_font, "W\n", conf_font_size * scale(hypriso->monitor_from_cursor()), RGBA(1, 1, 1, 1));
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

        merge_create<DesktopItem *>(c, desktop_items, [](Container *c) {
            return *datum<DesktopItem *>(c, "DesktopItem");
        }, [](Container *parent, DesktopItem *data) {
            create_desktop_icon(parent, data);
        });

        int pad = conf_pad * s;
        int start_x = c->real_bounds.x + pad;
        int start_y = c->real_bounds.y + pad;
        for (int i = 0; i < c->children.size(); i++) {
            auto child = c->children[i];
            child->real_bounds = Bounds(start_x, start_y, conf_icon_size, conf_icon_size);
            start_x += child->real_bounds.w + pad;
            if (start_x + child->real_bounds.w > (c->real_bounds.x + c->real_bounds.w)) {
                start_y +=  child->real_bounds.h + pad + two_line_height * .5;
                start_x = c->real_bounds.x + pad;
            }
        }
    };
    static bool dragging = false;
    dragging = false;
    c->when_drag_start = [](Container* actual_root, Container* c) {
        dragging = true;
    };
    c->when_drag = [](Container *actual_root, Container *c) {
        actual_root->consumed_event = true;
        auto b = fixed_box(actual_root->mouse_initial_x, actual_root->mouse_initial_y, actual_root->mouse_current_x, actual_root->mouse_current_y);
        static Bounds previousB = b;
        b.grow(20);
        hypriso->damage_box(b);
        hypriso->damage_box(previousB);
        previousB = b;
    };
    c->when_paint = [](Container* actual_root, Container* c) {
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
            render_drop_shadow(rid, 1.0, {0, 0, 0, .04f}, std::round(rounding * s), 2.0, shadow);
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

#include "desktop_icons.h"
#include "container.h"
#include "heart.h"
#include "hypriso.h"
#include "icons.h"
#include "popup.h"
#include "settings.h"

#include <gio/gio.h>
#include <algorithm>
#include <iostream>
#include <cctype>
#include <filesystem>
#include <hyprutils/path/Path.hpp>
#include <linux/input-event-codes.h>
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
    return hypriso->get_varfloat("plugin:mylardesktop:desktop_icon_size", 68);
}

static float conf_pad() {
    return hypriso->get_varfloat("plugin:mylardesktop:desktop_pad", 12);
}

static bool conf_vertical() {
    return hypriso->get_varint("plugin:mylardesktop:desktop_vertical", 1) != 0;
}

static std::string conf_desktop_folder() {
    return hypriso->get_varstring("plugin:mylardesktop:desktop_folder", "~/Desktop");
}

int two_line_height = 24;
 
struct DesktopItem {
    std::string full_filepath;
    std::string name;
    std::string extension;
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
            continue;
        }

        auto item = new DesktopItem{
            .full_filepath = scannedItem.full_filepath,
            .name          = scannedItem.name,
            .extension     = scannedItem.extension,
            .is_folder     = scannedItem.is_folder,
        };
        desktop_items.push_back(item);

        {
            auto file = g_file_new_for_path(item->full_filepath.c_str());
            GFileInfo *info = g_file_query_info(file, G_FILE_ATTRIBUTE_STANDARD_ICON, G_FILE_QUERY_INFO_NONE, NULL, nullptr);

            if (!info) {
                g_object_unref(file);
                continue;
            }

            gchar **attrs = g_file_info_list_attributes(info, nullptr);

            for (int i = 0; attrs[i] != nullptr; ++i) {
                const char *name = attrs[i];
                GFileAttributeType type = g_file_info_get_attribute_type(info, name);

                switch (type) {
                    case G_FILE_ATTRIBUTE_TYPE_OBJECT: {
                        GObject *obj = g_file_info_get_attribute_object(info, name);

                        if (G_IS_THEMED_ICON(obj)) {
                            const gchar * const *names =
                                g_themed_icon_get_names(G_THEMED_ICON(obj));

                            for (int j = 0; names[j] != nullptr; ++j) {
                                item->icons_for_mime.push_back(names[j]);
                            }
                        }
                        break;
                    }
                }
            }

            g_strfreev(attrs);
            g_object_unref(info);
            g_object_unref(file);
        }
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
    
    ~IcoContainerData() {
        //notify(fz("{} was deleted", name));
        auto text_img = *datum<TextureInfo>(c, "label");
        free_text_texture(text_img.id);
    }
};

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
    c->when_drag_end = c->when_mouse_motion;
    c->when_clicked = [](Container* actual_root, Container* c) {
        auto ico = (IcoContainerData *) c->user_data;
        clear_desktop_selection(c->parent);
        auto current = get_current_time_in_ms();
        if ((current - ico->last_time_pressed) < 700) {
            DesktopItem *item = *datum<DesktopItem *>(c, "DesktopItem");
            auto ran = fz("xdg-open \"{}\"", item->full_filepath);
            launch_command(ran);
        }
        ico->last_time_pressed = current;
    };
   
    c->when_paint = [](Container* actual_root, Container* c) {
        auto root = get_rendering_root();
        auto [rid, s, stage, active_id] = roots_info(actual_root, root);
        if (stage == (int)STAGE::RENDER_POST_WALLPAPER) {
            renderfix
            
            DesktopItem *item = *datum<DesktopItem *>(c, "DesktopItem");

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
                    draw_texture(info, c->real_bounds.x, c->real_bounds.y);
                }
            }
            
            auto* ico = (IcoContainerData*)(c->user_data);
            if (c->state.mouse_pressing) {
                //rect(c->real_bounds, color_sel_color());
                border(c->real_bounds, color_sel_border_color(), std::round(1 * s));
            } else if (c->state.mouse_hovering) {
                rect(c->real_bounds, color_sel_color());
                border(c->real_bounds, color_sel_border_color(), std::round(1 * s));
            } else if (ico->is_selected) {
                rect(c->real_bounds, color_sel_color());
                border(c->real_bounds, color_sel_border_color(), std::round(1 * s));
            }
            
            
            TextureInfo text_img = *datum<TextureInfo>(c, "label");
            if (text_img.id == -1) {
                text_img = gen_text_texture(mylar_font, item->name, conf_font_size() * s, RGBA(1, 1, 1, 1), c->real_bounds.w, two_line_height, 1);
                *datum<TextureInfo>(c, "label") = text_img;
            }
            draw_texture(text_img, 
                c->real_bounds.x,
                c->real_bounds.y + c->real_bounds.h + 4 * s);
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

        merge_create<DesktopItem *>(c, desktop_items, [](Container *c) {
            return *datum<DesktopItem *>(c, "DesktopItem");
        }, [](Container *parent, DesktopItem *data) {
            create_desktop_icon(parent, data);
        });

        int pad = conf_pad() * s;
        int start_x = c->real_bounds.x + pad;
        int start_y = c->real_bounds.y + pad;
        for (int i = 0; i < c->children.size(); i++) {
            auto child = c->children[i];
            child->real_bounds = Bounds(start_x, start_y, conf_icon_size(), conf_icon_size());
            if (conf_vertical()) {
                start_y += child->real_bounds.h + pad + two_line_height * .5;
                if (start_y + child->real_bounds.h > (c->real_bounds.y + c->real_bounds.h)) {
                    start_x += child->real_bounds.w + pad;
                    start_y = c->real_bounds.y + pad;
                }
            } else {
                start_x += child->real_bounds.w + pad;
                if (start_x + child->real_bounds.w > (c->real_bounds.x + c->real_bounds.w)) {
                    start_y += child->real_bounds.h + pad + two_line_height * .5;
                    start_x = c->real_bounds.x + pad;
                }
            }
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

void desktop_icons::deselect() {
    for (int i = actual_root->children.size() - 1; i >= 0; i--) {
        auto child = actual_root->children[i];
        if (child->custom_type == (int) TYPE::DESKTOP_ICONS) {
            clear_desktop_selection(child);
        }
    }
}

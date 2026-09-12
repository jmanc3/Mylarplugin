static void set_volume(const std::vector<std::string> &uuids, float scalar, bool is_muted) {
    if (scalar < 0)
        scalar = 0;
    if (scalar > 1)
        scalar = 1;

    if (uuids.empty())
        return;

    audio([uuids, scalar, is_muted]() {
        for (auto c : audio_clients) {
            if (std::find(uuids.begin(), uuids.end(), c->uuid) != uuids.end()) {
                if (is_muted)
                    c->set_mute(false);
                c->set_volume(scalar);
            }
        }
    });
    
}

static void set_volume(std::string uuid, float scalar, bool is_muted) {
    set_volume(std::vector<std::string>{uuid}, scalar, is_muted);
}

struct AudioData : UserData {
    std::string title; 
    std::string icon;
    float level = 100;
    bool muted = false;
    std::string uuid;
    std::vector<std::string> uuids;
    int pid = -1;
    bool merged = false;
    int indent_level = 0;

    bool attempted_to_load_icon_once = false;
    float old_dpi = 0.0;
    cairo_surface_t *icon_surface = nullptr;
};

static void paint_debug_us(Container *root, Container *c) {
    auto dock = (Dock *) root->user_data;
    auto cr = dock->volume->raw_window->cr;
    auto audio_data = (AudioData *) c->user_data;
    auto b = c->real_bounds;
    b.shrink(2);
    set_rect(cr, b);
    set_argb(cr, {1, 0, 1, 1});
    cairo_fill(cr);
}

static int audio_container = 3824729; 
static std::unordered_set<int> expanded_audio_pids;

static Container *fill_out_volume_slider(Container *c) {
    c->when_paint = [](Container *root, Container *c) {
        auto dock = (Dock *) root->user_data;
        auto cr = dock->volume->raw_window->cr;
        auto dpi = dock->volume->raw_window->dpi;
        auto audio_c = first_above_of(c, audio_container);
        auto audio_data = (AudioData *) audio_c->user_data;
        
        paint_slider(root, c, cr, dpi, audio_data->level);
        
    };

    c->when_clicked = [](Container *root, Container *c) {
        float scalar = (root->mouse_current_x - c->real_bounds.x) / c->real_bounds.w;
        auto audio_c = first_above_of(c, audio_container);
        auto audio_data = (AudioData *) audio_c->user_data;
        set_volume(audio_data->uuids, scalar, audio_data->muted);
    };
    c->when_drag_start = [](Container *root, Container *c) {
        float scalar = (root->mouse_current_x - c->real_bounds.x) / c->real_bounds.w;
        auto audio_c = first_above_of(c, audio_container);
        auto audio_data = (AudioData *) audio_c->user_data;
        set_volume(audio_data->uuids, scalar, audio_data->muted);
    };
    c->when_drag = [](Container *root, Container *c) {
        float scalar = (root->mouse_current_x - c->real_bounds.x) / c->real_bounds.w;
        auto audio_c = first_above_of(c, audio_container);
        auto audio_data = (AudioData *) audio_c->user_data;
        set_volume(audio_data->uuids, scalar, audio_data->muted);
    };
    c->when_drag_end = [](Container *root, Container *c) {
        float scalar = (root->mouse_current_x - c->real_bounds.x) / c->real_bounds.w;
        auto audio_c = first_above_of(c, audio_container);
        auto audio_data = (AudioData *) audio_c->user_data;
        set_volume(audio_data->uuids, scalar, audio_data->muted);
    };
    
    return nullptr;
}

static Container *add_volume_option(Container *parent) {
    auto line = parent->child(::vbox, FILL_SPACE, 40);
    line->custom_type = audio_container;
    auto audio_data = new AudioData;
    line->user_data = audio_data;

    static float total_h = volume_row_h;
    static float top_h = 30;
    
    line->pre_layout = [](Container *root, Container *c, const Bounds &b) {
        c->wanted_bounds.h = total_h * ((Dock *) root->user_data)->volume->raw_window->dpi;
    };

    auto label_icon = line->child(FILL_SPACE, FILL_SPACE);
    label_icon->pre_layout = [](Container *root, Container *c, const Bounds &b) {
        c->wanted_bounds.h = top_h * ((Dock *) root->user_data)->volume->raw_window->dpi;
    };
    label_icon->when_clicked = [](Container *root, Container *c) {
        auto dock = (Dock *) root->user_data;
        auto audio_c = first_above_of(c, audio_container);
        auto audio_data = (AudioData *) audio_c->user_data;
        if (!audio_data->merged || audio_data->pid <= 0)
            return;

        auto visible_row_count = audio_c->parent->children.size();
        auto child_row_count = audio_data->uuids.size();
        if (expanded_audio_pids.find(audio_data->pid) != expanded_audio_pids.end()) {
            expanded_audio_pids.erase(audio_data->pid);
            visible_row_count = visible_row_count > child_row_count
                ? visible_row_count - child_row_count
                : 1;
        } else {
            expanded_audio_pids.insert(audio_data->pid);
            visible_row_count += child_row_count;
        }

        resize_volume_popup_for_rows(dock, visible_row_count);
        dock::change_in_audio();
    };
    label_icon->when_paint = [](Container *root, Container *c) { 
        auto dock = (Dock *) root->user_data;
        auto cr = dock->volume->raw_window->cr;
        auto audio_c = first_above_of(c, audio_container);
        auto audio_data = (AudioData *) audio_c->user_data;
        auto dpi = dock->volume->raw_window->dpi;

        if (std::abs(audio_data->old_dpi - dpi) > EPSILON) {
            audio_data->old_dpi = dpi;
            if (audio_data->icon_surface) {
                cairo_surface_destroy(audio_data->icon_surface); 
                audio_data->icon_surface = nullptr;
            }
            audio_data->attempted_to_load_icon_once = false;
        }
        if (!audio_data->attempted_to_load_icon_once) {
            audio_data->attempted_to_load_icon_once = true;
            auto icon = audio_data->icon;
            auto full = one_shot_icon(24 * dpi, {icon, to_lower(icon), c3ic_fix_wm_class(icon), to_lower(icon)});
            if (!full.empty()) 
                load_icon_full_path(&audio_data->icon_surface, full, 24 * dpi);
        }

        int x_off = 0;
        if (audio_data->icon_surface) {
            auto width = cairo_image_surface_get_width(audio_data->icon_surface);
            auto height = cairo_image_surface_get_height(audio_data->icon_surface);
            cairo_set_source_surface(cr, audio_data->icon_surface, 
                c->real_bounds.x + 3 * dpi, c->real_bounds.y + c->real_bounds.h * .5 - height * .5);
            cairo_paint(cr);
            x_off += width + 6 * dpi;
        }

        //paint_debug_us(root, c);
        auto label = audio_data->title;
        if (audio_data->merged) {
            if (expanded_audio_pids.find(audio_data->pid) != expanded_audio_pids.end()) {
                label = "[-] " + label;
            } else {
                label = "[+] " + label;
            }
        } else if (audio_data->indent_level > 0) {
            label = "  " + label;
        }
        auto bounds = draw_text(cr, c, label, 12 * dpi, false, "Segoe Fluent Icons");
        auto b = draw_text(cr,
            c->real_bounds.x + 7 * dpi + x_off, c->real_bounds.y + c->real_bounds.h * .5 - bounds.h * .5,
            label, 12 * dpi, true, "Segoe Fluent Icons", 
            (c->real_bounds.w - 14 * dpi - x_off) * PANGO_SCALE, c->real_bounds.h * PANGO_SCALE, {0, 0, 0, 1});
    };
    
    auto volume_slider_parent = line->child(::hbox, FILL_SPACE, FILL_SPACE);
    volume_slider_parent->pre_layout = [](Container *root, Container *c, const Bounds &b) {
        c->wanted_bounds.h = (total_h - top_h) * ((Dock *) root->user_data)->volume->raw_window->dpi * .87;
    };

    auto left_volume_icon = volume_slider_parent->child(FILL_SPACE, FILL_SPACE);
    left_volume_icon->pre_layout = [](Container *root, Container *c, const Bounds &b) {
        float dpi = ((Dock *) root->user_data)->volume->raw_window->dpi;
        c->wanted_bounds.w = (total_h - top_h) * dpi * 1.1;
    };
    left_volume_icon->when_clicked = paint {
        auto dock = (Dock *) root->user_data;
        //windowing::set_popup_size(dock->volume->raw_window, 100, 300);
        
        auto audio_c = first_above_of(c, audio_container);
        auto audio_data = (AudioData *) audio_c->user_data;
        auto muted = audio_data->muted;
        auto uuids = audio_data->uuids;
        audio([uuids, muted]() {
            for (auto c : audio_clients) {
                if (std::find(uuids.begin(), uuids.end(), c->uuid) != uuids.end())
                   c->set_mute(!muted);
            }
        });
    };
    left_volume_icon->when_paint = paint {
        auto dock = (Dock *) root->user_data;
        auto cr = dock->volume->raw_window->cr;
        auto dpi = dock->volume->raw_window->dpi;
        auto audio_c = first_above_of(c, audio_container);
        auto audio_data = (AudioData *) audio_c->user_data;

        bool is_muted = audio_data->muted;
        int val = (int) std::round(audio_data->level * 100);
        
        if (!is_muted) {
            std::string background_bars = "\uE995";
            RGBA color = {.4, .4, .4, .4};
            auto b = draw_text(cr, 0, 0, background_bars, 14 * dpi, false, "Segoe Fluent Icons");
            draw_text(cr, center_x(c, b.w), center_y(c, b.h), background_bars, 14 * dpi, true, "Segoe Fluent Icons", -1, -1, color);
        }
        
        std::string text;
        if (is_muted) {
            text = "\uE74F";
        } else if (val == 0) {
            text = "\uE992";
        } else if (val < 33) {
            text = "\uE993";
        } else if (val < 66) {
            text = "\uE994";
        } else {
            text = "\uE995";
        }
        
        RGBA color = {0, 0, 0, 1};
        auto b = draw_text(cr, 0, 0, text, 14 * dpi, false, "Segoe Fluent Icons");
        draw_text(cr, center_x(c, b.w), center_y(c, b.h), text, 14 * dpi, true, "Segoe Fluent Icons", -1, -1, color);
    };
    
    auto slider = volume_slider_parent->child(FILL_SPACE, FILL_SPACE);
    fill_out_volume_slider(slider);
    auto right_volume_text = volume_slider_parent->child(FILL_SPACE, FILL_SPACE);
    right_volume_text->pre_layout = [](Container *root, Container *c, const Bounds &b) {
        float dpi = ((Dock *) root->user_data)->volume->raw_window->dpi;
        c->wanted_bounds.w = (total_h - top_h) * dpi * 1.2;
    };
    right_volume_text->when_paint = paint {
        auto dock = (Dock *) root->user_data;
        auto cr = dock->volume->raw_window->cr;
        auto dpi = dock->volume->raw_window->dpi;
        auto audio_c = first_above_of(c, audio_container);
        auto audio_data = (AudioData *) audio_c->user_data;
        int val = (int) std::round(audio_data->level * 100);
        std::string level = std::to_string(val);
        RGBA color = {0, 0, 0, 1};
        auto b = draw_text(cr, 0, 0, level, 12 * dpi, false, mylar_font);
        auto x = c->real_bounds.x + 17 * dpi;
        draw_text(cr, x, center_y(c, b.h), level, 12 * dpi, true, mylar_font, -1, -1, color);
    };

    auto right_pad = volume_slider_parent->child(FILL_SPACE, FILL_SPACE);
    right_pad->pre_layout = [](Container *root, Container *c, const Bounds &b) {
        c->wanted_bounds.w = b.h * .2;
    };
    

    return line;
}

static std::string make_unmerged_title(const AudioClient &client) {
    if (client.subtitle.empty())
        return client.title;
    return fz("{} : {}", client.title, client.subtitle);
}

static std::vector<VolumeRow> build_volume_rows(std::vector<AudioClient> &clients) {
    std::unordered_map<int, std::vector<AudioClient *>> clients_by_pid;
    for (auto &client : clients) {
        if (client.pid > 0) {
            clients_by_pid[client.pid].push_back(&client);
        }
    }

    std::vector<VolumeRow> rows;
    std::unordered_set<int> emitted_pids;
    std::unordered_set<std::string> emitted_uuids;

    for (auto &client : clients) {
        if (client.pid > 0) {
            auto &group = clients_by_pid[client.pid];
            if (group.size() > 1) {
                if (emitted_pids.find(client.pid) != emitted_pids.end())
                    continue;
                emitted_pids.insert(client.pid);

                VolumeRow merged;
                merged.id = fz("pid:{}", client.pid);
                merged.pid = client.pid;
                merged.merged = true;
                merged.level = 0;
                merged.muted = true;

                std::string subtitle_for_group;
                for (auto grouped_client : group) {
                    merged.uuids.push_back(grouped_client->uuid);
                    merged.level += grouped_client->get_volume();
                    merged.muted = merged.muted && grouped_client->is_muted();
                    if (merged.icon.empty() && !grouped_client->icon_name.empty()) {
                        merged.icon = grouped_client->icon_name;
                    }
                    if (subtitle_for_group.empty() && !grouped_client->subtitle.empty()) {
                        subtitle_for_group = grouped_client->subtitle;
                    }
                }
                merged.level /= (float) group.size();
                if (subtitle_for_group.empty()) {
                    subtitle_for_group = group.front()->title;
                }
                merged.title = subtitle_for_group;
                rows.push_back(std::move(merged));

                if (expanded_audio_pids.find(client.pid) != expanded_audio_pids.end()) {
                    for (auto grouped_client : group) {
                        VolumeRow child;
                        child.id = fz("uuid:{}:{}", client.pid, grouped_client->uuid);
                        child.title = make_unmerged_title(*grouped_client);
                        child.icon = grouped_client->icon_name;
                        child.level = grouped_client->get_volume();
                        child.muted = grouped_client->is_muted();
                        child.pid = grouped_client->pid;
                        child.indent_level = 1;
                        child.uuids.push_back(grouped_client->uuid);
                        rows.push_back(std::move(child));
                    }
                }
                continue;
            }
        }

        if (emitted_uuids.find(client.uuid) != emitted_uuids.end())
            continue;
        emitted_uuids.insert(client.uuid);
        VolumeRow single;
        single.id = fz("uuid:{}", client.uuid);
        single.title = make_unmerged_title(client);
        single.icon = client.icon_name;
        single.level = client.get_volume();
        single.muted = client.is_muted();
        single.pid = client.pid;
        single.uuids.push_back(client.uuid);
        rows.push_back(std::move(single));
    }

    return rows;
}

static void fill_volume_root(std::vector<AudioClient> clients, Container *root) {
    root->type = ::vbox;

    auto rows = build_volume_rows(clients);

    std::vector<std::string> row_ids;
    row_ids.reserve(rows.size());
    std::unordered_map<std::string, const VolumeRow *> rows_by_id;
    for (auto &row : rows) {
        row_ids.push_back(row.id);
        rows_by_id[row.id] = &row;
    }

    merge_create<std::string>(root, row_ids, [](Container *c) {
        return c->uuid;
    }, [](Container *parent, std::string id) {
        auto c = add_volume_option(parent);
        c->uuid = id;
    });

    {
        std::unordered_map<std::string, Container *> child_by_id;
        child_by_id.reserve(root->children.size());
        for (auto *child : root->children) {
            child_by_id[child->uuid] = child;
        }

        std::vector<Container *> ordered_children;
        ordered_children.reserve(root->children.size());
        std::unordered_set<Container *> seen;
        seen.reserve(root->children.size());

        for (const auto &id : row_ids) {
            auto it = child_by_id.find(id);
            if (it == child_by_id.end())
                continue;
            ordered_children.push_back(it->second);
            seen.insert(it->second);
        }
        for (auto *child : root->children) {
            if (seen.find(child) == seen.end()) {
                ordered_children.push_back(child);
            }
        }
        root->children = std::move(ordered_children);
    }

    auto dock = (Dock *) root->user_data;
    resize_volume_popup_for_rows(dock, rows.size());

    auto current = get_current_time_in_ms();
    for (auto client : clients) {
        if (client.is_master && (current - last_time_volume_adjusted) > 100) {
            volume_level = std::round(client.get_volume() * 100);
            is_master_muted = client.is_muted();
        }
    }

    for (auto c : root->children) {
        auto iter = rows_by_id.find(c->uuid);
        if (iter == rows_by_id.end())
            continue;
        auto row = iter->second;
        auto audio_data = (AudioData *) c->user_data;
        audio_data->uuid = row->id;
        audio_data->uuids = row->uuids;
        audio_data->level = row->level;
        audio_data->pid = row->pid;
        audio_data->icon = row->icon;
        audio_data->muted = row->muted;
        audio_data->title = row->title;
        audio_data->merged = row->merged;
        audio_data->indent_level = row->indent_level;
    }

    for (auto expanded_pid = expanded_audio_pids.begin(); expanded_pid != expanded_audio_pids.end();) {
        bool exists = false;
        for (auto c : root->children) {
            auto audio_data = (AudioData *) c->user_data;
            if (audio_data->merged && audio_data->pid == *expanded_pid) {
                exists = true;
                break;
            }
        }
        if (exists) {
            ++expanded_pid;
        } else {
            expanded_pid = expanded_audio_pids.erase(expanded_pid);
        }
    }
}


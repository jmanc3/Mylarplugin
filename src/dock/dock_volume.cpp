#include "dock/dock_volume.h"

#include "dock/dock_internal.h"
#include "settings.h"

#include <unistd.h>

float volume_level = 100;
bool is_master_muted = false;
long last_time_volume_adjusted = 0;

static std::unordered_map<Container *, int> volume_drag_blip_timers;

static void schedule_volume_drag_blip(Dock *dock, Container *slider) {
    volume_drag_blip_timers[slider] = windowing::timer(dock->app, 380, [dock, slider](void *) {
        volume_drag_blip_timers[slider] = -1;
        if (!slider->state.mouse_dragging)
            return;
        audio_play_blip();
        schedule_volume_drag_blip(dock, slider);
    }, nullptr);
}

static void stop_volume_drag_blip(Dock *dock, Container *slider) {
    auto timer = volume_drag_blip_timers.find(slider);
    if (timer == volume_drag_blip_timers.end())
        return;
    if (timer->second != -1)
        windowing::timer_stop(dock->app, timer->second);
    volume_drag_blip_timers.erase(timer);
}

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

// Which mixer node(s) a row's level meter follows.
struct MeterKey {
    bool mic = false;
    uint32_t node_index = UINT32_MAX;        // Microphones: the source index
    uint32_t pw_node_id = SPA_ID_INVALID;    // PipeWire: the node id of the stream or sink
    uint32_t pa_index = PA_INVALID_INDEX;    // PulseAudio: the sink input (or sink) index
    std::string pa_name;                     // PulseAudio: tells a sink apart from a sink input
};

static MeterKey meter_key_for(const AudioClient &client) {
    MeterKey key;
    key.pw_node_id = client.pw_node_id;
    key.pa_index = (uint32_t) client.index;
    key.pa_name = client.title;
    return key;
}

struct AudioData : UserData {
    std::string title;
    std::string icon;
    float level = 100;
    bool muted = false;
    bool is_master = false;
    std::string uuid;
    std::vector<std::string> uuids;
    int pid = -1;
    bool merged = false;
    int indent_level = 0;

    // Microphone rows (input devices) are driven through the mixer model instead of audio_clients.
    bool is_mic = false;
    uint32_t node_index = UINT32_MAX;
    long last_local_change = 0;
    float last_sent = -1;

    std::vector<MeterKey> meters;
    float meter = 0; // Shown level, 0..1 (falls off smoothly)

    std::string loaded_icon;
    float old_dpi = 0.0;
    cairo_surface_t *icon_surface = nullptr;

    ~AudioData() override {
        if (icon_surface)
            cairo_surface_destroy(icon_surface);
    }
};

struct VolumeRow {
    bool is_master = false;
    std::string id;
    std::string title;
    std::string icon;
    float level = 0;
    bool muted = false;
    int pid = -1;
    bool merged = false;
    int indent_level = 0;
    std::vector<std::string> uuids;
    bool is_mic = false;
    uint32_t node_index = UINT32_MAX;
    std::vector<MeterKey> meters;
};

// State of the open popup: which tab is showing and the slide between them.
struct VolumeMenuData : UserData {
    int tab = 0;     // 0 = Playback, 1 = Mics
    float from = 0;  // Slide position when the last tab switch started
    long anim_start = 0;
    size_t playback_rows = 1;
    size_t mic_rows = 0;
    unsigned long mic_revision = 0;
    int last_h = -1;

    VolumeMenuData() { audio_mixer_meters(true); }
    ~VolumeMenuData() override { audio_mixer_meters(false); }
};

static int audio_container = 3824729;
static std::unordered_set<int> expanded_audio_pids;

// All in logical pixels (multiplied by dpi when laid out).
static constexpr float volume_tabs_h = 46.0f;
static constexpr float volume_list_pad_top = 2.0f;
static constexpr float volume_list_pad_bottom = 8.0f;
static constexpr float volume_row_h = 62.0f;
static constexpr float volume_row_pad_x = 14.0f;
static constexpr float volume_row_pad_top = 6.0f;
static constexpr float volume_row_pad_bottom = 4.0f;
static constexpr float volume_row_top_h = 28.0f;
static constexpr float volume_icon_col = 24.0f;
static constexpr float volume_gap = 10.0f;
static constexpr float volume_value_col = 30.0f;
static constexpr float volume_indent = 16.0f;
static constexpr long volume_slide_ms = 240;

static const char *volume_tab_names[] = {"Playback", "Mics"};

int volume_popup_height_for_rows(size_t row_count) {
    auto rows = std::max<size_t>(1, row_count);
    return (int) std::round(volume_tabs_h + volume_list_pad_top + volume_list_pad_bottom + volume_row_h * (float) rows);
}

std::vector<AudioClient> snapshot_audio_clients() {
    std::vector<AudioClient> clients;
    audio_read([&]() {
        clients.reserve(audio_clients.size());
        for (auto client : audio_clients) {
            client->is_master = client->is_master_volume();
            clients.push_back(*client);
        }
    });
    return clients;
}

static VolumeMenuData *volume_menu(Dock *dock) {
    if (!dock || !dock->volume)
        return nullptr;
    if (auto body = container_by_name("volume_body", dock->volume->root))
        return (VolumeMenuData *) body->user_data;
    return nullptr;
}

static float volume_slide_t(VolumeMenuData *menu) {
    return std::clamp((float) (get_current_time_in_ms() - menu->anim_start) / (float) volume_slide_ms, 0.0f, 1.0f);
}

static float volume_slide_pos(VolumeMenuData *menu) {
    float t = volume_slide_t(menu);
    float e = 1.0f - std::pow(1.0f - t, 3.0f);
    return menu->from + ((float) menu->tab - menu->from) * e;
}

static bool volume_sliding(VolumeMenuData *menu) {
    return volume_slide_t(menu) < 1.0f;
}

static void volume_popup_update_size(Dock *dock) {
    if (!dock || !dock->volume || !dock->volume->raw_window)
        return;
    auto menu = volume_menu(dock);
    if (!menu)
        return;
    // Both pages share the popup, so size it for the taller one; it doesn't jump while sliding.
    int h = volume_popup_height_for_rows(std::max(menu->playback_rows, menu->mic_rows));
    if (h == menu->last_h)
        return;
    menu->last_h = h;
    windowing::set_popup_size(dock->volume->raw_window, volume_popup_w, h);
}

static void volume_row_set_level(AudioData *data, float scalar, bool finished) {
    scalar = std::clamp(scalar, 0.0f, 1.0f);
    if (!data->is_mic) {
        set_volume(data->uuids, scalar, data->muted);
        return;
    }
    data->level = scalar;
    data->last_local_change = get_current_time_in_ms();
    if (data->muted) {
        data->muted = false;
        audio_mixer_set_mute(AudioNodeKind::Source, data->node_index, false);
    }
    if (finished || std::abs(scalar - data->last_sent) > 0.004f) {
        data->last_sent = scalar;
        audio_mixer_set_volume(AudioNodeKind::Source, data->node_index, scalar);
    }
}

static Container *fill_out_volume_slider(Container *c) {
    c->when_paint = [](Container *root, Container *c) {
        auto dock = (Dock *) root->user_data;
        auto cr = dock->volume->raw_window->drawing_context;
        auto dpi = dock->volume->raw_window->dpi;
        auto audio_c = first_above_of(c, audio_container);
        auto audio_data = (AudioData *) audio_c->user_data;

        paint_slider(root, c, cr, dpi, std::clamp(audio_data->level, 0.0f, 1.0f),
                     audio_data->muted ? 0.0f : audio_data->meter);
    };

    c->when_drag_end_is_click = false;

    c->when_clicked = [](Container *root, Container *c) {
        float scalar = (root->mouse_current_x - c->real_bounds.x) / c->real_bounds.w;
        auto audio_c = first_above_of(c, audio_container);
        auto audio_data = (AudioData *) audio_c->user_data;
        volume_row_set_level(audio_data, scalar, true);

        if (audio_data->is_master)
            audio_play_blip();
    };
    c->when_drag_start = [](Container *root, Container *c) {
        auto dock = (Dock *) root->user_data;
        auto audio_c = first_above_of(c, audio_container);
        auto audio_data = (AudioData *) audio_c->user_data;

        if (audio_data->is_master) {
            stop_volume_drag_blip(dock, c);
            schedule_volume_drag_blip(dock, c);
        }

        float scalar = (root->mouse_current_x - c->real_bounds.x) / c->real_bounds.w;
        volume_row_set_level(audio_data, scalar, false);
    };
    c->when_drag = [](Container *root, Container *c) {
        float scalar = (root->mouse_current_x - c->real_bounds.x) / c->real_bounds.w;
        auto audio_c = first_above_of(c, audio_container);
        auto audio_data = (AudioData *) audio_c->user_data;
        volume_row_set_level(audio_data, scalar, false);
    };
    c->when_drag_end = [](Container *root, Container *c) {
        auto dock = (Dock *) root->user_data;
        auto audio_c = first_above_of(c, audio_container);
        auto audio_data = (AudioData *) audio_c->user_data;

        if (audio_data->is_master)
            stop_volume_drag_blip(dock, c);
        float scalar = (root->mouse_current_x - c->real_bounds.x) / c->real_bounds.w;

        volume_row_set_level(audio_data, scalar, true);
        if (audio_data->is_master)
            audio_play_blip();
    };

    return nullptr;
}

static Container *add_volume_option(Container *parent) {
    auto line = parent->child(::vbox, FILL_SPACE, 40);
    line->custom_type = audio_container;
    auto audio_data = new AudioData;
    line->user_data = audio_data;

    line->pre_layout = [](Container *root, Container *c, const Bounds &b) {
        auto dpi = ((Dock *) root->user_data)->volume->raw_window->dpi;
        c->wanted_bounds.h = volume_row_h * dpi;
        c->wanted_pad = Bounds(volume_row_pad_x * dpi, volume_row_pad_top * dpi,
                               volume_row_pad_x * dpi, volume_row_pad_bottom * dpi);
    };

    auto label_icon = line->child(FILL_SPACE, FILL_SPACE);
    label_icon->pre_layout = [](Container *root, Container *c, const Bounds &b) {
        c->wanted_bounds.h = volume_row_top_h * ((Dock *) root->user_data)->volume->raw_window->dpi;
    };
    label_icon->when_clicked = [](Container *root, Container *c) {
        auto dock = (Dock *) root->user_data;
        auto audio_c = first_above_of(c, audio_container);
        auto audio_data = (AudioData *) audio_c->user_data;
        if (audio_data->is_mic || !audio_data->merged || audio_data->pid <= 0)
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

        if (auto menu = volume_menu(dock)) {
            menu->playback_rows = visible_row_count;
            volume_popup_update_size(dock);
        }
        dock::change_in_audio();
    };
    label_icon->when_paint = [](Container *root, Container *c) {
        auto dock = (Dock *) root->user_data;
        auto cr = dock->volume->raw_window->drawing_context;
        auto audio_c = first_above_of(c, audio_container);
        auto audio_data = (AudioData *) audio_c->user_data;
        auto dpi = dock->volume->raw_window->dpi;

        if (audio_data->loaded_icon != audio_data->icon || std::abs(audio_data->old_dpi - dpi) > EPSILON) {
            audio_data->loaded_icon = audio_data->icon;
            audio_data->old_dpi = dpi;
            if (audio_data->icon_surface) {
                cairo_surface_destroy(audio_data->icon_surface);
                audio_data->icon_surface = nullptr;
            }
            // Same fallbacks as the Playback / Input Devices tabs of the settings audio page.
            auto icon = audio_data->icon;
            std::string fallback = audio_data->is_mic ? "audio-input-microphone" : "applications-multimedia";
            auto full = one_shot_icon(24 * dpi, {icon, to_lower(icon), c3ic_fix_wm_class(icon), fallback,
                                                 "audio-speakers", "multimedia-volume-control"});
            if (!full.empty())
                load_icon_full_path(&audio_data->icon_surface, full, 24 * dpi);
        }

        double x = c->real_bounds.x + (audio_data->indent_level > 0 ? volume_indent * dpi : 0);
        double icon_w = volume_icon_col * dpi;
        if (audio_data->icon_surface) {
            auto width = cairo_image_surface_get_width(audio_data->icon_surface);
            auto height = cairo_image_surface_get_height(audio_data->icon_surface);
            cr->set_source_surface(audio_data->icon_surface,
                std::round(x + icon_w * .5 - width * .5),
                std::round(c->real_bounds.y + c->real_bounds.h * .5 - height * .5));
            cr->paint_source();
        } else {
            std::string glyph = "";
            auto b = draw_text(cr, 0, 0, glyph, 14 * dpi, false, icon_font);
            draw_text(cr, x + icon_w * .5 - b.w * .5, c->real_bounds.y + c->real_bounds.h * .5 - b.h * .5,
                      glyph, 14 * dpi, true, icon_font, -1, -1, {0, 0, 0, 1});
        }
        x += icon_w + volume_gap * dpi;

        auto label = audio_data->title;
        if (audio_data->merged) {
            if (expanded_audio_pids.find(audio_data->pid) != expanded_audio_pids.end()) {
                label = "[-] " + label;
            } else {
                label = "[+] " + label;
            }
        }
        double text_w = c->real_bounds.x + c->real_bounds.w - x;
        auto bounds = draw_text(cr, c, label, 12 * dpi, false, icon_font);
        draw_text(cr,
            x, c->real_bounds.y + c->real_bounds.h * .5 - bounds.h * .5,
            label, 12 * dpi, true, icon_font,
            text_w * PANGO_SCALE, c->real_bounds.h * PANGO_SCALE, {0, 0, 0, 1});
    };

    auto volume_slider_parent = line->child(::hbox, FILL_SPACE, FILL_SPACE);
    volume_slider_parent->pre_layout = [](Container *root, Container *c, const Bounds &b) {
        c->spacing = volume_gap * ((Dock *) root->user_data)->volume->raw_window->dpi;
    };

    auto left_volume_icon = volume_slider_parent->child(FILL_SPACE, FILL_SPACE);
    left_volume_icon->pre_layout = [](Container *root, Container *c, const Bounds &b) {
        c->wanted_bounds.w = volume_icon_col * ((Dock *) root->user_data)->volume->raw_window->dpi;
    };
    left_volume_icon->when_clicked = [](Container *root, Container *c) {
        auto audio_c = first_above_of(c, audio_container);
        auto audio_data = (AudioData *) audio_c->user_data;
        auto muted = audio_data->muted;
        if (audio_data->is_mic) {
            audio_data->muted = !muted;
            audio_data->last_local_change = get_current_time_in_ms();
            audio_mixer_set_mute(AudioNodeKind::Source, audio_data->node_index, !muted);
            return;
        }
        auto uuids = audio_data->uuids;
        audio([uuids, muted]() {
            for (auto c : audio_clients) {
                if (std::find(uuids.begin(), uuids.end(), c->uuid) != uuids.end())
                    c->set_mute(!muted);
            }
        });
    };
    left_volume_icon->when_paint = [](Container *root, Container *c) {
        auto dock = (Dock *) root->user_data;
        auto cr = dock->volume->raw_window->drawing_context;
        auto dpi = dock->volume->raw_window->dpi;
        auto audio_c = first_above_of(c, audio_container);
        auto audio_data = (AudioData *) audio_c->user_data;

        bool is_muted = audio_data->muted;
        int val = (int) std::round(audio_data->level * 100);

        if (!is_muted) {
            std::string background_bars = "";
            RGBA color = {.4, .4, .4, .4};
            auto b = draw_text(cr, 0, 0, background_bars, 14 * dpi, false, icon_font);
            draw_text(cr, center_x(c, b.w), center_y(c, b.h), background_bars, 14 * dpi, true, icon_font, -1, -1, color);
        }

        std::string text;
        if (is_muted) {
            text = "";
        } else if (val == 0) {
            text = "";
        } else if (val < 33) {
            text = "";
        } else if (val < 66) {
            text = "";
        } else {
            text = "";
        }

        RGBA color = {0, 0, 0, 1};
        auto b = draw_text(cr, 0, 0, text, 14 * dpi, false, icon_font);
        draw_text(cr, center_x(c, b.w), center_y(c, b.h), text, 14 * dpi, true, icon_font, -1, -1, color);
    };

    auto slider = volume_slider_parent->child(FILL_SPACE, FILL_SPACE);
    fill_out_volume_slider(slider);
    auto right_volume_text = volume_slider_parent->child(FILL_SPACE, FILL_SPACE);
    right_volume_text->pre_layout = [](Container *root, Container *c, const Bounds &b) {
        c->wanted_bounds.w = volume_value_col * ((Dock *) root->user_data)->volume->raw_window->dpi;
    };
    right_volume_text->when_paint = [](Container *root, Container *c) {
        auto dock = (Dock *) root->user_data;
        auto cr = dock->volume->raw_window->drawing_context;
        auto dpi = dock->volume->raw_window->dpi;
        auto audio_c = first_above_of(c, audio_container);
        auto audio_data = (AudioData *) audio_c->user_data;
        int val = (int) std::round(audio_data->level * 100);
        std::string level = std::to_string(val);
        RGBA color = {0, 0, 0, 1};
        auto b = draw_text(cr, 0, 0, level, 12 * dpi, false, set->font);
        auto x = c->real_bounds.x + c->real_bounds.w - b.w;
        draw_text(cr, x, center_y(c, b.h), level, 12 * dpi, true, set->font, -1, -1, color);
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
        if (client.title.find("blip.wav") != std::string::npos)
            continue;
        if (client.pid > 0) {
            clients_by_pid[client.pid].push_back(&client);
        }
    }

    std::vector<VolumeRow> rows;
    std::unordered_set<int> emitted_pids;
    std::unordered_set<std::string> emitted_uuids;

    for (auto &client : clients) {
        if (client.title.find("blip.wav") != std::string::npos)
            continue;
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
                    merged.meters.push_back(meter_key_for(*grouped_client));
                    merged.is_master = merged.is_master || grouped_client->is_master_volume();
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
                        child.is_master = grouped_client->is_master_volume();
                        child.indent_level = 1;
                        child.uuids.push_back(grouped_client->uuid);
                        child.meters.push_back(meter_key_for(*grouped_client));
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
        single.is_master = client.is_master_volume();
        single.uuids.push_back(client.uuid);
        single.meters.push_back(meter_key_for(client));
        rows.push_back(std::move(single));
    }

    return rows;
}

// Input devices (microphones, line in), without the monitors of output devices. Default first.
static std::vector<VolumeRow> build_mic_rows(const std::vector<AudioNodeInfo> &nodes) {
    auto default_source = audio_mixer_default_source();
    std::vector<VolumeRow> rows;
    for (auto &n : nodes) {
        if (n.kind != AudioNodeKind::Source || n.monitor_of != UINT32_MAX)
            continue;
        VolumeRow row;
        row.id = fz("mic:{}", n.index);
        row.title = n.description.empty() ? n.name : n.description;
        row.icon = n.icon_name;
        row.level = (float) n.volume();
        row.muted = n.mute;
        row.is_mic = true;
        row.node_index = n.index;
        MeterKey key;
        key.mic = true;
        key.node_index = n.index;
        row.meters.push_back(key);
        if (!n.name.empty() && n.name == default_source)
            rows.insert(rows.begin(), std::move(row));
        else
            rows.push_back(std::move(row));
    }
    return rows;
}

int volume_popup_height_for_clients(const std::vector<AudioClient> &clients) {
    auto copy = clients;
    return volume_popup_height_for_rows(std::max(build_volume_rows(copy).size(), build_mic_rows(audio_mixer_nodes()).size()));
}

static void apply_volume_rows(Container *root, const std::vector<VolumeRow> &rows) {
    root->type = ::vbox;

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

    auto now = get_current_time_in_ms();
    for (auto c : root->children) {
        auto iter = rows_by_id.find(c->uuid);
        if (iter == rows_by_id.end())
            continue;
        auto row = iter->second;
        auto audio_data = (AudioData *) c->user_data;
        audio_data->uuid = row->id;
        audio_data->uuids = row->uuids;
        audio_data->pid = row->pid;
        audio_data->icon = row->icon;
        audio_data->is_master = row->is_master;
        audio_data->title = row->title;
        audio_data->merged = row->merged;
        audio_data->indent_level = row->indent_level;
        audio_data->is_mic = row->is_mic;
        audio_data->node_index = row->node_index;
        audio_data->meters = row->meters;
        // Keep what the user just set until the sound server catches up.
        if (now - audio_data->last_local_change > 600) {
            audio_data->level = row->level;
            audio_data->muted = row->muted;
        }
    }
}

void fill_volume_root(std::vector<AudioClient> clients, Container *root) {
    auto rows = build_volume_rows(clients);
    apply_volume_rows(root, rows);

    // `root` is the content of the playback scroll pane; the popup's top container holds the dock.
    Container *top = root;
    while (top->parent)
        top = top->parent;
    auto dock = (Dock *) top->user_data;
    if (auto menu = volume_menu(dock)) {
        menu->playback_rows = rows.size();
        volume_popup_update_size(dock);
    }

    auto current = get_current_time_in_ms();
    for (auto client : clients) {
        if (client.is_master_volume() && (current - last_time_volume_adjusted) > 100) {
            volume_level = std::round(client.get_volume() * 100);
            is_master_muted = client.is_muted();
        }
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

static const AudioNodeInfo *find_meter_node(const std::vector<AudioNodeInfo> &nodes, const MeterKey &key) {
    if (key.mic) {
        for (auto &n : nodes)
            if (n.kind == AudioNodeKind::Source && n.index == key.node_index)
                return &n;
        return nullptr;
    }
    if (key.pw_node_id != SPA_ID_INVALID) {
        for (auto &n : nodes)
            if ((n.kind == AudioNodeKind::SinkInput || n.kind == AudioNodeKind::Sink) && n.index == key.pw_node_id)
                return &n;
        return nullptr;
    }
    if (key.pa_index == PA_INVALID_INDEX)
        return nullptr;
    // Sinks and sink inputs have separate index spaces; a sink also has to match by name.
    for (auto &n : nodes)
        if (n.kind == AudioNodeKind::Sink && n.index == key.pa_index && n.name == key.pa_name)
            return &n;
    for (auto &n : nodes)
        if (n.kind == AudioNodeKind::SinkInput && n.index == key.pa_index)
            return &n;
    return nullptr;
}

// Peaks are linear amplitude; show them on a 60 dB scale (like the settings page) so quiet audio is visible.
static float volume_meter_level(float peak) {
    if (peak <= 0.0001f)
        return 0;
    return std::clamp(1.0f + 20.0f * std::log10(peak) / 60.0f, 0.0f, 1.0f);
}

// Returns true when a shown meter changed.
static bool update_meters(Container *scroll, const std::vector<AudioNodeInfo> &nodes) {
    if (!scroll)
        return false;
    bool changed = false;
    for (auto c : ((ScrollContainer *) scroll)->content->children) {
        auto data = (AudioData *) c->user_data;
        float target = 0;
        for (auto &key : data->meters)
            if (auto n = find_meter_node(nodes, key))
                if (n->peak_supported)
                    target = std::max(target, volume_meter_level(n->peak));
        float shown = target >= data->meter ? target : std::max(target, data->meter - 0.05f);
        if (std::abs(shown - data->meter) > 0.001f)
            changed = true;
        data->meter = shown;
    }
    return changed;
}

// Returns true when the microphone list was rebuilt.
static bool refresh_mics(Dock *dock, VolumeMenuData *menu, const std::vector<AudioNodeInfo> &nodes, bool force) {
    auto revision = audio_mixer_revision();
    if (!force && revision == menu->mic_revision)
        return false;
    menu->mic_revision = revision;
    auto scroll = (ScrollContainer *) container_by_name("volume_mics", dock->volume->root);
    if (!scroll)
        return false;
    auto rows = build_mic_rows(nodes);
    apply_volume_rows(scroll->content, rows);
    menu->mic_rows = rows.size();
    volume_popup_update_size(dock);
    return true;
}

static void volume_watch(Dock *dock, MylarWindow *window, bool fast) {
    windowing::timer(dock->app, fast ? 16 : 33, [dock, window](void *) {
        if (finished)
            return;
        RawWindow *popup_to_redraw = nullptr;
        bool sliding = false;
        {
            std::lock_guard<std::mutex> lock(dock->app->mutex);
            if (dock->volume != window)
                return; // Popup closed; stop polling.
            auto menu = volume_menu(dock);
            if (!menu)
                return;
            auto nodes = audio_mixer_nodes();
            bool redraw = refresh_mics(dock, menu, nodes, false);
            redraw |= update_meters(container_by_name("volume_root", window->root), nodes);
            redraw |= update_meters(container_by_name("volume_mics", window->root), nodes);
            sliding = volume_sliding(menu);
            // One more frame after the slide ends so it settles exactly on the page.
            redraw |= sliding || volume_slide_pos(menu) != menu->from;
            if (!sliding)
                menu->from = (float) menu->tab;
            if (redraw)
                popup_to_redraw = window->raw_window;
        }
        if (popup_to_redraw)
            windowing::redraw_now(popup_to_redraw);
        volume_watch(dock, window, sliding);
    }, nullptr);
}

static Container *make_volume_page(Container *body, float dpi, const std::string &scroll_name) {
    auto page = body->child(::vbox, FILL_SPACE, FILL_SPACE);
    page->pre_layout = [](Container *root, Container *c, const Bounds &b) {
        auto dpi = ((Dock *) root->user_data)->volume->raw_window->dpi;
        c->wanted_bounds.w = b.w;
        c->wanted_pad = Bounds(0, volume_list_pad_top * dpi, 0, volume_list_pad_bottom * dpi);
    };
    auto scroll = make_newscrollpane_as_child(page, ScrollPaneSettings(dpi), [](Container *root) {
        auto dock = ((Dock *) root->user_data);
        if (!dock || !dock->volume || !dock->volume->raw_window)
            return DrawContext{};

        auto window = dock->volume->raw_window;
        return DrawContext({window->drawing_context, window->dpi, [dock]() {
            if (!dock || !dock->volume || !dock->volume->raw_window)
                return;
            windowing::redraw(dock->volume->raw_window);
        }});
    });
    scroll->name = scroll_name;
    return page;
}

void build_volume_popup(Dock *dock) {
    auto window = dock->volume;
    auto root = window->root;
    root->type = ::vbox;

    auto menu = new VolumeMenuData;

    // Segmented "Playback | Mics" switcher with a pill that slides along with the pages.
    auto tabs = root->child(::hbox, FILL_SPACE, FILL_SPACE);
    tabs->pre_layout = [](Container *root, Container *c, const Bounds &b) {
        auto dpi = ((Dock *) root->user_data)->volume->raw_window->dpi;
        c->wanted_bounds.h = volume_tabs_h * dpi;
        c->wanted_pad = Bounds(volume_row_pad_x * dpi, 12 * dpi, volume_row_pad_x * dpi, 2 * dpi);
        c->spacing = 8 * dpi;
    };
    auto segment = tabs->child(::hbox, FILL_SPACE, FILL_SPACE);
    segment->pre_layout = [](Container *root, Container *c, const Bounds &b) {
        auto dpi = ((Dock *) root->user_data)->volume->raw_window->dpi;
        c->wanted_pad = Bounds(3 * dpi, 3 * dpi, 3 * dpi, 3 * dpi);
    };
    segment->when_paint = [menu](Container *root, Container *c) {
        auto dock = (Dock *) root->user_data;
        auto cr = dock->volume->raw_window->drawing_context;
        auto dpi = dock->volume->raw_window->dpi;
        auto b = c->real_bounds;
        drawRoundedRect(cr, b.x, b.y, b.w, b.h, 8 * dpi, 1.0);
        set_argb(cr, {0, 0, 0, .06});
        cr->fill();

        auto inner = c->children_bounds;
        double tab_w = inner.w / 2.0;
        Bounds pill(inner.x + tab_w * volume_slide_pos(menu), inner.y, tab_w, inner.h);
        drawRoundedRect(cr, pill.x, pill.y, pill.w, pill.h, 6 * dpi, 1.0);
        set_argb(cr, {1, 1, 1, 1});
        cr->fill();
        drawRoundedRect(cr, pill.x, pill.y, pill.w, pill.h, 6 * dpi, 1.0);
        set_argb(cr, {0, 0, 0, .1});
        cr->set_line_width(1.0);
        cr->stroke();
    };
    for (int i = 0; i < 2; i++) {
        auto tab = segment->child(FILL_SPACE, FILL_SPACE);
        tab->when_paint = [menu, i](Container *root, Container *c) {
            auto dock = (Dock *) root->user_data;
            auto cr = dock->volume->raw_window->drawing_context;
            auto dpi = dock->volume->raw_window->dpi;
            bool selected = menu->tab == i;
            float alpha = selected ? .9f : (c->state.mouse_hovering ? .75f : .55f);
            RGBA color = {0, 0, 0, alpha};
            auto b = draw_text(cr, 0, 0, volume_tab_names[i], 11 * dpi, false, set->font);
            draw_text(cr, center_x(c, b.w), center_y(c, b.h), volume_tab_names[i], 11 * dpi, true, set->font,
                      -1, -1, color);
        };
        tab->when_clicked = [menu, i](Container *root, Container *c) {
            auto dock = (Dock *) root->user_data;
            if (menu->tab == i)
                return;
            menu->from = volume_slide_pos(menu);
            menu->tab = i;
            menu->anim_start = get_current_time_in_ms();
            if (i == 1)
                refresh_mics(dock, menu, audio_mixer_nodes(), true);
            windowing::redraw(dock->volume->raw_window);
        };
    }

    // Gear: opens Settings on the Audio page (or focuses it and switches there if already open).
    auto gear = tabs->child(FILL_SPACE, FILL_SPACE);
    gear->pre_layout = [](Container *root, Container *c, const Bounds &b) {
        // Square, as tall as the segmented switcher.
        auto dpi = ((Dock *) root->user_data)->volume->raw_window->dpi;
        c->wanted_bounds.w = (volume_tabs_h - 14) * dpi;
    };
    gear->when_paint = [](Container *root, Container *c) {
        auto dock = (Dock *) root->user_data;
        auto cr = dock->volume->raw_window->drawing_context;
        auto dpi = dock->volume->raw_window->dpi;
        auto b = c->real_bounds;
        drawRoundedRect(cr, b.x, b.y, b.w, b.h, 8 * dpi, 1.0);
        float alpha = c->state.mouse_pressing ? .14f : (c->state.mouse_hovering ? .1f : .06f);
        set_argb(cr, {0, 0, 0, alpha});
        cr->fill();
        std::string glyph = "";
        auto t = draw_text(cr, 0, 0, glyph, 13 * dpi, false, icon_font);
        draw_text(cr, center_x(c, t.w), center_y(c, t.h), glyph, 13 * dpi, true, icon_font, -1, -1, {0, 0, 0, .8});
    };
    gear->when_clicked = [](Container *root, Container *c) {
        auto dock = (Dock *) root->user_data;
        windowing::close_window(dock->volume->raw_window);
        main_thread([]() {
            settings::start("Audio");
        });
    };

    // Both pages side by side; the negative left pad scrolls the row to the active one.
    auto body = root->child(::hbox, FILL_SPACE, FILL_SPACE);
    body->name = "volume_body";
    body->user_data = menu;
    body->pre_layout = [menu](Container *root, Container *c, const Bounds &b) {
        double offset = std::round(volume_slide_pos(menu) * b.w);
        c->wanted_pad = Bounds(-offset, 0, offset, 0);
    };
    body->when_paint = [](Container *root, Container *c) {
        auto dock = (Dock *) root->user_data;
        auto window = dock->volume->raw_window;
        window->drawing_context->save();
        clip_to_popup_background(window->drawing_context, root->real_bounds, window->dpi);
        window->drawing_context->rectangle(c->real_bounds.x, c->real_bounds.y, c->real_bounds.w, c->real_bounds.h);
        window->drawing_context->clip_path();
    };
    body->after_paint = [](Container *root, Container *c) {
        auto dock = (Dock *) root->user_data;
        dock->volume->raw_window->drawing_context->restore();
    };

    auto dpi = window->raw_window->dpi;
    make_volume_page(body, dpi, "volume_root");
    auto mics = make_volume_page(body, dpi, "volume_mics");
    mics->when_paint = [menu](Container *root, Container *c) {
        if (menu->mic_rows > 0)
            return;
        auto dock = (Dock *) root->user_data;
        auto cr = dock->volume->raw_window->drawing_context;
        auto dpi = dock->volume->raw_window->dpi;
        std::string text = "No microphones found";
        auto b = draw_text(cr, 0, 0, text, 11 * dpi, false, set->font);
        draw_text(cr, center_x(c, b.w), center_y(c, b.h), text, 11 * dpi, true, set->font, -1, -1, {0, 0, 0, .55});
    };

    refresh_mics(dock, menu, audio_mixer_nodes(), true);
    volume_watch(dock, window, false);
}

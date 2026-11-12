//
// Created by jmanc3 on 9/30/21.
//

#include "audio.h"
#include "defer.h"
#include "hypriso.h"
#include "icons.h"
#include "volume_mapping.h"

#ifdef TRACY_ENABLE

#include "../tracy/public/tracy/Tracy.hpp"

#endif

#include <pulse/mainloop.h>
#include <pulse/mainloop-api.h>
#include <alsa/asoundlib.h>
#include <iostream>
#include <algorithm>
#include <cstring>
#include <cmath>
#include <climits>
#include <cerrno>
#include <cstdlib>
#include <condition_variable>
#include <filesystem>
#include <sys/wait.h>
#include <signal.h>
#include <unistd.h>

#include <pipewire/pipewire.h>
#include <pipewire/extensions/metadata.h>
#include <pipewire/keys.h>
#include <spa/param/audio/raw.h>
#include <spa/param/props.h>
#include <spa/pod/builder.h>
#include <spa/pod/parser.h>
#include <spa/utils/dict.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/param.h>
#include <spa/param/profile.h>
#include <spa/param/route.h>
#include <pipewire/stream.h>
#include <pipewire/device.h>
#include <atomic>
#include <mutex>
#include <unordered_map>

// Global stuff
bool audio_running = false;
bool allow_audio_thread_creation = true;

enum AudioBackend {
    UNSET,
    PULSEAUDIO,
    PIPEWIRE,
    ALSA
};

// General stuff
static AudioBackend backend = AudioBackend::UNSET;
snd_mixer_t *alsa_handle = nullptr;
snd_mixer_elem_t *master_volume = nullptr;
snd_mixer_selem_id_t *master_sid = nullptr;
snd_mixer_elem_t *headphone_volume = nullptr;
snd_mixer_selem_id_t *headphone_sid = nullptr;
snd_mixer_elem_t *speaker_volume = nullptr;
snd_mixer_selem_id_t *speaker_sid = nullptr;

// Pulseaudio stuff
static bool ready = false;
static pa_mainloop *mainloop;
static pa_mainloop_api *mainloop_api;
static pa_context *context;
static std::atomic<bool> audio_thread_busy = false;
static std::mutex read_mutex;
static std::mutex signal_mutex;
static std::mutex free_to_continue_mutex;
static std::condition_variable needs_signal_condition;
static std::condition_variable free_to_continue_condition;
static std::atomic<bool> needs_block = false;
static bool signal_met = false;
static bool free_to_continue = true;
static std::string default_sink_name;
static std::thread::id audio_thread_id;

static void (*audio_change_callback)() = nullptr;
static std::mutex blip_mutex;
static std::condition_variable blip_condition;
static std::string pending_blip_path;
static bool blip_requested = false;
static bool blip_worker_started = false;
static bool blip_stop_requested = false;
static std::thread blip_worker_thread;

static void blip_playback_worker() {
    std::vector<pid_t> player_pids;
    std::unique_lock<std::mutex> lock(blip_mutex);

    while (true) {
        blip_condition.wait(lock, []() { return blip_requested || blip_stop_requested; });
        if (blip_stop_requested)
            break;
        auto path = pending_blip_path;
        blip_requested = false;

        player_pids.erase(std::remove_if(player_pids.begin(), player_pids.end(), [](pid_t pid) {
            int status;
            auto result = waitpid(pid, &status, WNOHANG);
            return result == pid || (result < 0 && errno != EINTR);
        }), player_pids.end());

        const char *player = nullptr;
        switch (backend) {
            case AudioBackend::PULSEAUDIO: player = "paplay"; break;
            case AudioBackend::PIPEWIRE: player = "pw-play"; break;
            case AudioBackend::ALSA: player = "aplay"; break;
            default: continue;
        }

        auto player_pid = fork();
        if (player_pid == 0) {
            execlp(player, player, path.c_str(), static_cast<char *>(nullptr));
            _exit(127);
        }
        if (player_pid > 0)
            player_pids.push_back(player_pid);
    }

    for (auto player_pid : player_pids)
        kill(player_pid, SIGTERM);
    lock.unlock();
    for (auto player_pid : player_pids) {
        while (waitpid(player_pid, nullptr, 0) < 0 && errno == EINTR) {}
    }
}

std::vector<AudioClient *> audio_clients;

// PipeWire stuff
struct PipeWireBoundNode {
    uint32_t id = SPA_ID_INVALID;
    bool is_master = false;
    pw_node *node = nullptr;
    spa_hook listener = {};
    AudioClient *client = nullptr;
};

static pw_thread_loop *pw_thread_loop_instance = nullptr;
static pw_context *pw_context_instance = nullptr;
static pw_core *pw_core_instance = nullptr;
static pw_registry *pw_registry_instance = nullptr;
static pw_metadata *pw_metadata_instance = nullptr;
static spa_hook pw_core_listener = {};
static spa_hook pw_registry_listener = {};
static spa_hook pw_metadata_listener = {};
static std::vector<PipeWireBoundNode *> pw_bound_nodes;
static int pw_sync_seq = 1;
static bool pw_initial_sync_done = false;

void sort();

static void mixer_clear_all();

static int parse_pid_value(const char *value) {
    if (!value || !*value)
        return -1;

    char *end = nullptr;
    errno = 0;
    long parsed = strtol(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0' || parsed <= 0 || parsed > INT_MAX)
        return -1;
    return static_cast<int>(parsed);
}

static void delete_all_audio_clients() {
    for (auto c: audio_clients)
        delete c;
    audio_clients.clear();
}

static bool pipewire_class_is_master(const char *media_class) {
    return media_class && strcmp(media_class, "Audio/Sink") == 0;
}

static bool pipewire_class_is_stream(const char *media_class) {
    if (!media_class)
        return false;
    return strcmp(media_class, "Stream/Output/Audio") == 0 ||
           strcmp(media_class, "Audio/Stream") == 0 ||
           strcmp(media_class, "Audio/Source") == 0;
}

static bool is_default_sink_client(const AudioClient *client) {
    if (!client || !client->is_master)
        return false;

    if (backend == AudioBackend::PIPEWIRE) {
        if (default_sink_name.empty())
            return false;
        if (!client->pw_node_name.empty() && client->pw_node_name == default_sink_name)
            return true;
        return client->title == default_sink_name;
    }

    return client->title == default_sink_name;
}

static std::string parse_pipewire_default_name(const char *value) {
    if (!value)
        return "";

    std::string s = value;
    auto name_pos = s.find("\"name\"");
    if (name_pos == std::string::npos)
        return "";
    auto colon = s.find(':', name_pos);
    if (colon == std::string::npos)
        return "";
    auto quote_start = s.find('"', colon);
    if (quote_start == std::string::npos)
        return "";
    quote_start++;
    auto quote_end = s.find('"', quote_start);
    if (quote_end == std::string::npos || quote_end <= quote_start)
        return "";
    return s.substr(quote_start, quote_end - quote_start);
}

static AudioClient *find_or_create_pipewire_client(uint32_t id, bool is_master) {
    for (auto *c: audio_clients) {
        if (c->pw_node_id == id && c->is_master == is_master)
            return c;
    }

    auto *client = new AudioClient();
    client->index = static_cast<int>(id);
    client->pw_node_id = id;
    client->is_master = is_master;
    if (is_master) {
        client->icon_name = "audio-card";
    }
    audio_clients.push_back(client);
    return client;
}

static PipeWireBoundNode *find_pipewire_bound_node(uint32_t id) {
    for (auto *bound: pw_bound_nodes) {
        if (bound->id == id)
            return bound;
    }
    return nullptr;
}

static void update_pipewire_client_volumes(AudioClient *client, const float *volumes, uint32_t n_values) {
    if (!client)
        return;

    client->pw_channel_volumes.clear();
    if (volumes && n_values > 0) {
        client->pw_channel_volumes.assign(volumes, volumes + n_values);
    } else {
        client->pw_channel_volumes.push_back(1.0f);
    }

    client->volume.channels = static_cast<uint8_t>(std::min<size_t>(client->pw_channel_volumes.size(), PA_CHANNELS_MAX));
    if (client->volume.channels == 0)
        client->volume.channels = 1;

    for (uint8_t i = 0; i < client->volume.channels; ++i) {
        float linear = client->pw_channel_volumes[i];
        if (linear < 0.0f)
            linear = 0.0f;
        if (linear > 1.0f)
            linear = 1.0f;
        // PipeWire volumes are linear amplitude; users (and pavucontrol/wpctl) see the cubic scale.
        client->volume.values[i] = static_cast<pa_volume_t>(std::round(std::cbrt(linear) * 65535.0f));
    }
}

static void cleanup_pipewire_state() {
    if (pw_thread_loop_instance)
        pw_thread_loop_lock(pw_thread_loop_instance);

    for (auto *bound: pw_bound_nodes) {
        spa_hook_remove(&bound->listener);
        if (bound->node) {
            pw_proxy_destroy(reinterpret_cast<pw_proxy *>(bound->node));
            bound->node = nullptr;
        }
        delete bound;
    }
    pw_bound_nodes.clear();
    mixer_clear_all();

    if (pw_metadata_instance) {
        spa_hook_remove(&pw_metadata_listener);
        pw_proxy_destroy(reinterpret_cast<pw_proxy *>(pw_metadata_instance));
        pw_metadata_instance = nullptr;
    }
    if (pw_registry_instance) {
        spa_hook_remove(&pw_registry_listener);
        pw_proxy_destroy(reinterpret_cast<pw_proxy *>(pw_registry_instance));
        pw_registry_instance = nullptr;
    }
    if (pw_core_instance) {
        spa_hook_remove(&pw_core_listener);
        pw_core_disconnect(pw_core_instance);
        pw_core_instance = nullptr;
    }
    if (pw_context_instance) {
        pw_context_destroy(pw_context_instance);
        pw_context_instance = nullptr;
    }
    if (pw_thread_loop_instance)
        pw_thread_loop_unlock(pw_thread_loop_instance);
    if (pw_thread_loop_instance) {
        pw_thread_loop_stop(pw_thread_loop_instance);
        pw_thread_loop_destroy(pw_thread_loop_instance);
        pw_thread_loop_instance = nullptr;
    }

    pw_initial_sync_done = false;
    pw_sync_seq = 1;
}

static void on_stream_suspend(pa_stream *s, void *data) {
    auto *client = (AudioClient *) data;

    if (pa_stream_is_suspended(s)) {
        if (client->peak != 0) {
            client->peak = 0;
            client->last_time_peak_changed = get_current_time_in_ms();
        }
        sort();
        if (audio_change_callback)
            audio_change_callback();
    }
}

static void on_stream_update(pa_stream *stream, size_t length, void *data) {
    auto *client = (AudioClient *) data;

    const void *buffer = nullptr;
    if (pa_stream_peek(stream, &buffer, &length) < 0) {
        fprintf(stderr, "pa_stream_peek() fail: %s", pa_strerror(pa_context_errno(pa_stream_get_context(stream))));
        return;
    }
    if (!buffer) {
        /* NULL data means either a hole or empty buffer. Only drop the stream when there is a hole (length > 0) */
        if (length)
            pa_stream_drop(stream);
        return;
    }

    double v = ((const float *) buffer)[length / sizeof(float) - 1];
    pa_stream_drop(stream);
    if (client->peak != std::min(std::max(v, 0.0), 1.0)) {
        client->last_time_peak_changed = get_current_time_in_ms();
    }
    client->peak = std::min(std::max(v, 0.0), 1.0);
    sort();
    if (audio_change_callback)
        audio_change_callback();
}

void sort() {
    auto time = get_current_time_in_ms();
    std::sort(audio_clients.begin(), audio_clients.end(), [time](AudioClient *a, AudioClient *b) {
        // order: master who is default sink, masters with older indexes first, sink inputs with lower index first
        if (a->is_master && b->is_master) {
            if (is_default_sink_client(a)) {
                return true;
            } else if (is_default_sink_client(b)) {
                return false;
            } else {
                return a->index < b->index;
            }
        } else if (a->is_master && !b->is_master) {
            return true;
        } else if (b->is_master && !a->is_master) {
            return false;
        } else {
            if ((a->peak == 0 && b->peak == 0) || (a->peak != 0 && b->peak != 0)) {
                if (a->peak != 0) {
                    if ((time - a->last_time_peak_changed < 1300) && (time - b->last_time_peak_changed > 1300)) {
                        return true;
                    } else if ((time - b->last_time_peak_changed < 1300) && (time - a->last_time_peak_changed > 1300)) {
                        return false;
                    }
                }
                return a->index < b->index;
            } else {
                return a->peak != 0;
            }
        }
    });
}

/*************************************************
 *
 * Mixer model (pavucontrol-style) shared by all backends
 *
 *************************************************/

struct MixerNode {
    AudioNodeInfo info;
    float peak_accum = 0;

    // PulseAudio
    pa_stream *pa_meter = nullptr;
    uint32_t pa_meter_device = UINT32_MAX;

    // PipeWire
    pw_node *pw_proxy = nullptr;
    spa_hook pw_listener = {};
    std::string pw_serial;
    uint32_t pw_device_id = SPA_ID_INVALID;
    int pw_profile_device = -1;
    std::vector<float> pw_linear;
    bool pw_subscribed = false;
    pw_stream *pw_meter = nullptr;
    spa_hook pw_meter_listener = {};

    // ALSA
    snd_mixer_t *alsa_mixer = nullptr;
    snd_mixer_elem_t *alsa_elem = nullptr;
};

struct MixerPwRoute {
    int index = -1;
    int direction = 0;
    std::string name;
    std::string description;
    bool available = true;
    uint32_t priority = 0;
    std::vector<int> devices;
};

struct MixerCard {
    AudioCardInfo info;

    // PipeWire
    pw_device *pw_proxy = nullptr;
    spa_hook pw_listener = {};
    bool pw_subscribed = false;
    std::vector<std::pair<int, AudioProfileInfo>> pw_profiles;
    int pw_active_profile = -1;
    std::vector<MixerPwRoute> pw_routes;
    std::unordered_map<int, int> pw_active_routes; // profile device -> route index

    // ALSA
    snd_mixer_t *alsa_mixer = nullptr;
};

static std::mutex mixer_mutex;
static std::vector<MixerNode *> mixer_nodes;
static std::vector<MixerCard *> mixer_cards;
static std::string mixer_default_sink;
static std::string mixer_default_source;
static std::atomic<unsigned long> mixer_rev{1};
static std::atomic<bool> mixer_meters_on{false};
static std::unordered_map<uint32_t, std::pair<uint32_t, uint32_t>> mixer_pw_links; // link id -> (output node, input node)

static const char *mixer_app_id = "winbar.volume";

static void mixer_changed() {
    mixer_rev++;
}

double AudioNodeInfo::volume() const {
    double v = 0;
    for (auto c: channel_volumes)
        v = std::max(v, c);
    return v;
}

static MixerNode *mixer_find(AudioNodeKind kind, uint32_t index) {
    for (auto n: mixer_nodes)
        if (n->info.kind == kind && n->info.index == index)
            return n;
    return nullptr;
}

static MixerNode *mixer_find_or_create(AudioNodeKind kind, uint32_t index) {
    if (auto n = mixer_find(kind, index))
        return n;
    auto n = new MixerNode;
    n->info.kind = kind;
    n->info.index = index;
    mixer_nodes.push_back(n);
    return n;
}

static MixerCard *mixer_find_card(uint32_t index) {
    for (auto c: mixer_cards)
        if (c->info.index == index)
            return c;
    return nullptr;
}

static std::string mixer_lower(std::string s) {
    for (auto &ch: s)
        ch = (char) tolower((unsigned char) ch);
    return s;
}

static std::string mixer_pick_icon(const std::string &icon, const std::string &app, const std::string &title,
                                   const char *fallback) {
    for (const auto &candidate: {icon, mixer_lower(icon), app, mixer_lower(app), title, mixer_lower(title)})
        if (!candidate.empty() && has_options(candidate))
            return candidate;
    if (!icon.empty())
        return icon;
    if (!app.empty())
        return mixer_lower(app);
    return fallback;
}

// --- PulseAudio meters ---

static void mixer_pa_meter_read(pa_stream *stream, size_t length, void *data) {
    auto n = (MixerNode *) data;
    const void *buffer = nullptr;
    if (pa_stream_peek(stream, &buffer, &length) < 0)
        return;
    if (!buffer) {
        if (length)
            pa_stream_drop(stream);
        return;
    }
    float v = 0;
    auto samples = (const float *) buffer;
    for (size_t i = 0; i < length / sizeof(float); i++)
        v = std::max(v, std::fabs(samples[i]));
    pa_stream_drop(stream);
    std::lock_guard<std::mutex> lock(mixer_mutex);
    n->peak_accum = std::max(n->peak_accum, std::min(v, 1.0f));
}

static void mixer_pa_meter_destroy(MixerNode *n) {
    if (!n->pa_meter)
        return;
    pa_stream_set_read_callback(n->pa_meter, nullptr, nullptr);
    pa_stream_disconnect(n->pa_meter);
    pa_stream_unref(n->pa_meter);
    n->pa_meter = nullptr;
    n->pa_meter_device = UINT32_MAX;
    n->peak_accum = 0;
}

// Called with mixer_mutex held, on the audio thread (or while it is blocked in audio()).
static void mixer_pa_meter_create(MixerNode *n) {
    if (!context || n->pa_meter)
        return;
    uint32_t source = UINT32_MAX;
    uint32_t monitor_stream = UINT32_MAX;
    bool device = true;
    switch (n->info.kind) {
        case AudioNodeKind::Sink:
            source = n->info.monitor_source;
            break;
        case AudioNodeKind::Source:
            source = n->info.index;
            break;
        case AudioNodeKind::SinkInput:
            if (auto sink = mixer_find(AudioNodeKind::Sink, n->info.device))
                source = sink->info.monitor_source;
            monitor_stream = n->info.index;
            device = false;
            break;
        case AudioNodeKind::SourceOutput:
            return; // Shown using the meter of the source it records from.
    }
    if (source == UINT32_MAX)
        return;

    pa_sample_spec spec;
    spec.format = PA_SAMPLE_FLOAT32;
    spec.rate = 25;
    spec.channels = 1;

    pa_buffer_attr attr;
    memset(&attr, 0, sizeof(attr));
    attr.fragsize = sizeof(float);
    attr.maxlength = (uint32_t) -1;

    auto stream = pa_stream_new(context, "Peak detect", &spec, nullptr);
    if (!stream)
        return;
    if (monitor_stream != UINT32_MAX)
        pa_stream_set_monitor_stream(stream, monitor_stream);
    pa_stream_set_read_callback(stream, mixer_pa_meter_read, n);
    auto flags = (pa_stream_flags_t) (PA_STREAM_DONT_MOVE | PA_STREAM_PEAK_DETECT | PA_STREAM_ADJUST_LATENCY |
                                      (device ? PA_STREAM_DONT_INHIBIT_AUTO_SUSPEND : PA_STREAM_NOFLAGS));
    auto source_name = std::to_string(source);
    if (pa_stream_connect_record(stream, source_name.c_str(), &attr, flags) < 0) {
        pa_stream_unref(stream);
        return;
    }
    n->pa_meter = stream;
    n->pa_meter_device = n->info.device;
}

// --- PipeWire meters ---

static void mixer_pw_meter_process(void *data) {
    auto n = (MixerNode *) data;
    if (!n->pw_meter)
        return;
    pw_buffer *b = pw_stream_dequeue_buffer(n->pw_meter);
    if (!b)
        return;
    float v = 0;
    spa_buffer *buf = b->buffer;
    if (buf && buf->n_datas > 0 && buf->datas[0].data && buf->datas[0].chunk) {
        auto &d = buf->datas[0];
        auto offset = std::min(d.chunk->offset, d.maxsize);
        auto size = std::min(d.chunk->size, d.maxsize - offset);
        auto samples = (const float *) SPA_PTROFF(d.data, offset, void);
        for (uint32_t i = 0; i < size / sizeof(float); i++)
            v = std::max(v, std::fabs(samples[i]));
    }
    pw_stream_queue_buffer(n->pw_meter, b);
    std::lock_guard<std::mutex> lock(mixer_mutex);
    n->peak_accum = std::max(n->peak_accum, std::min(v, 1.0f));
}

static pw_stream_events mixer_pw_meter_events_make() {
    pw_stream_events events = {};
    events.version = PW_VERSION_STREAM_EVENTS;
    events.process = mixer_pw_meter_process;
    return events;
}

static const pw_stream_events mixer_pw_meter_events = mixer_pw_meter_events_make();

static void mixer_pw_meter_destroy(MixerNode *n) {
    if (!n->pw_meter)
        return;
    spa_hook_remove(&n->pw_meter_listener);
    pw_stream_destroy(n->pw_meter);
    n->pw_meter = nullptr;
    n->peak_accum = 0;
}

// Called with the PipeWire thread loop locked and mixer_mutex held.
static void mixer_pw_meter_create(MixerNode *n) {
    if (!pw_core_instance || n->pw_meter)
        return;
    if (n->info.kind == AudioNodeKind::SourceOutput)
        return; // Shown using the meter of the source it records from.
    std::string target = !n->pw_serial.empty() ? n->pw_serial : n->info.name;
    if (target.empty())
        return;

    auto props = pw_properties_new(
            PW_KEY_MEDIA_TYPE, "Audio",
            PW_KEY_MEDIA_CATEGORY, "Capture",
            PW_KEY_MEDIA_ROLE, "DSP",
            PW_KEY_APP_ID, mixer_app_id,
            PW_KEY_APP_NAME, "Winbar Volume Control",
            PW_KEY_NODE_NAME, "winbar-peak-detect",
            PW_KEY_STREAM_MONITOR, "true",
            PW_KEY_NODE_PASSIVE, "true",
            PW_KEY_NODE_DONT_RECONNECT, "true",
            nullptr);
    pw_properties_set(props, PW_KEY_TARGET_OBJECT, target.c_str());
    if (n->info.kind == AudioNodeKind::Sink)
        pw_properties_set(props, PW_KEY_STREAM_CAPTURE_SINK, "true");

    auto stream = pw_stream_new(pw_core_instance, "Peak detect", props);
    if (!stream)
        return;

    uint8_t buffer[1024];
    spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
    spa_audio_info_raw raw = {};
    raw.format = SPA_AUDIO_FORMAT_F32;
    raw.channels = 1;
    raw.position[0] = SPA_AUDIO_CHANNEL_MONO;
    const spa_pod *params[1];
    params[0] = spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat, &raw);

    n->pw_meter = stream;
    pw_stream_add_listener(stream, &n->pw_meter_listener, &mixer_pw_meter_events, n);
    if (pw_stream_connect(stream, PW_DIRECTION_INPUT, PW_ID_ANY,
                          (pw_stream_flags) (PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS),
                          params, 1) < 0) {
        mixer_pw_meter_destroy(n);
    }
}

static void mixer_meter_create(MixerNode *n) {
    if (backend == AudioBackend::PULSEAUDIO)
        mixer_pa_meter_create(n);
    else if (backend == AudioBackend::PIPEWIRE)
        mixer_pw_meter_create(n);
}

static void mixer_meter_destroy(MixerNode *n) {
    mixer_pa_meter_destroy(n);
    mixer_pw_meter_destroy(n);
}

static void mixer_delete_node(MixerNode *n) {
    mixer_meter_destroy(n);
    if (n->pw_proxy) {
        spa_hook_remove(&n->pw_listener);
        pw_proxy_destroy((pw_proxy *) n->pw_proxy);
        n->pw_proxy = nullptr;
    }
    delete n;
}

static void mixer_remove(AudioNodeKind kind, uint32_t index) {
    for (size_t i = 0; i < mixer_nodes.size(); i++) {
        if (mixer_nodes[i]->info.kind == kind && mixer_nodes[i]->info.index == index) {
            mixer_delete_node(mixer_nodes[i]);
            mixer_nodes.erase(mixer_nodes.begin() + (long) i);
            mixer_changed();
            return;
        }
    }
}

static void mixer_delete_card(MixerCard *c) {
    if (c->pw_proxy) {
        spa_hook_remove(&c->pw_listener);
        pw_proxy_destroy((pw_proxy *) c->pw_proxy);
        c->pw_proxy = nullptr;
    }
    if (c->alsa_mixer) {
        snd_mixer_close(c->alsa_mixer);
        c->alsa_mixer = nullptr;
    }
    delete c;
}

// Must be called on the thread that owns the backend (or with the PipeWire loop locked).
static void mixer_clear_all() {
    std::lock_guard<std::mutex> lock(mixer_mutex);
    for (auto n: mixer_nodes)
        mixer_delete_node(n);
    mixer_nodes.clear();
    for (auto c: mixer_cards)
        mixer_delete_card(c);
    mixer_cards.clear();
    mixer_pw_links.clear();
    mixer_default_sink.clear();
    mixer_default_source.clear();
    mixer_changed();
}

// --- PulseAudio model ---

static std::string mixer_pa_prop(pa_proplist *p, const char *key) {
    if (!p)
        return "";
    const char *v = pa_proplist_gets(p, key);
    return v ? v : "";
}

static void mixer_pa_volumes(AudioNodeInfo &info, const pa_cvolume &v) {
    info.channel_volumes.clear();
    for (int i = 0; i < v.channels; i++)
        info.channel_volumes.push_back((double) v.values[i] / (double) PA_VOLUME_NORM);
}

template<typename PortInfo>
static void mixer_pa_ports(AudioNodeInfo &info, PortInfo **ports, uint32_t n_ports, PortInfo *active) {
    info.ports.clear();
    for (uint32_t i = 0; i < n_ports; i++) {
        if (!ports[i])
            continue;
        AudioPortInfo port;
        port.name = ports[i]->name ? ports[i]->name : "";
        port.description = ports[i]->description ? ports[i]->description : port.name;
        port.priority = ports[i]->priority;
        port.available = ports[i]->available != PA_PORT_AVAILABLE_NO;
        info.ports.push_back(port);
    }
    std::sort(info.ports.begin(), info.ports.end(), [](const AudioPortInfo &a, const AudioPortInfo &b) {
        return a.priority > b.priority;
    });
    info.active_port = active && active->name ? active->name : "";
}

static void mixer_pa_sink(const pa_sink_info *i) {
    if (!i || (i->name && strcmp(i->name, "auto_null") == 0))
        return;
    std::lock_guard<std::mutex> lock(mixer_mutex);
    auto n = mixer_find_or_create(AudioNodeKind::Sink, i->index);
    n->info.name = i->name ? i->name : "";
    n->info.description = i->description ? i->description : n->info.name;
    n->info.icon_name = mixer_pa_prop(i->proplist, "device.icon_name");
    if (n->info.icon_name.empty())
        n->info.icon_name = "audio-card";
    n->info.device = i->card;
    n->info.monitor_source = i->monitor_source;
    mixer_pa_volumes(n->info, i->volume);
    n->info.mute = i->mute;
    n->info.has_volume = true;
    n->info.corked = i->state == PA_SINK_SUSPENDED;
    mixer_pa_ports(n->info, i->ports, i->n_ports, i->active_port);
    n->info.peak_supported = true;
    if (mixer_meters_on)
        mixer_pa_meter_create(n);
    mixer_changed();
}

static void mixer_pa_source(const pa_source_info *i) {
    if (!i || (i->name && strcmp(i->name, "auto_null.monitor") == 0))
        return;
    std::lock_guard<std::mutex> lock(mixer_mutex);
    auto n = mixer_find_or_create(AudioNodeKind::Source, i->index);
    n->info.name = i->name ? i->name : "";
    n->info.description = i->description ? i->description : n->info.name;
    n->info.monitor_of = i->monitor_of_sink;
    n->info.icon_name = mixer_pa_prop(i->proplist, "device.icon_name");
    if (n->info.icon_name.empty())
        n->info.icon_name = n->info.monitor_of != PA_INVALID_INDEX ? "audio-card" : "audio-input-microphone";
    n->info.device = i->card;
    mixer_pa_volumes(n->info, i->volume);
    n->info.mute = i->mute;
    n->info.has_volume = true;
    n->info.corked = i->state == PA_SOURCE_SUSPENDED;
    mixer_pa_ports(n->info, i->ports, i->n_ports, i->active_port);
    n->info.peak_supported = true;
    if (mixer_meters_on)
        mixer_pa_meter_create(n);
    mixer_changed();
}

static void mixer_pa_stream_identity(AudioNodeInfo &info, const char *name, pa_proplist *p, const char *fallback_icon) {
    info.description = name ? name : "";
    std::string media = mixer_pa_prop(p, PA_PROP_MEDIA_NAME);
    if (!media.empty())
        info.description = media;
    info.name = info.description;
    info.app_name = mixer_pa_prop(p, PA_PROP_APPLICATION_NAME);
    info.pid = parse_pid_value(mixer_pa_prop(p, PA_PROP_APPLICATION_PROCESS_ID).c_str());
    if (info.icon_name.empty()) {
        info.icon_name = mixer_pick_icon(mixer_pa_prop(p, PA_PROP_APPLICATION_ICON_NAME), info.app_name,
                                         info.description, fallback_icon);
    }
}

static void mixer_pa_sink_input(const pa_sink_input_info *i) {
    if (!i)
        return;
    std::lock_guard<std::mutex> lock(mixer_mutex);
    auto n = mixer_find_or_create(AudioNodeKind::SinkInput, i->index);
    mixer_pa_stream_identity(n->info, i->name, i->proplist, "applications-multimedia");
    n->info.device = i->sink;
    mixer_pa_volumes(n->info, i->volume);
    n->info.mute = i->mute;
    n->info.has_volume = i->has_volume && i->volume_writable;
    n->info.corked = i->corked;
    n->info.peak_supported = true;
    if (n->pa_meter && n->pa_meter_device != n->info.device)
        mixer_pa_meter_destroy(n); // Moved to another sink; monitor the new one.
    if (mixer_meters_on)
        mixer_pa_meter_create(n);
    mixer_changed();
}

static void mixer_pa_source_output(const pa_source_output_info *i) {
    if (!i)
        return;
    std::string app_id = mixer_pa_prop(i->proplist, PA_PROP_APPLICATION_ID);
    // Hide peak meters (ours and other mixers').
    if (app_id == mixer_app_id || app_id == "org.PulseAudio.pavucontrol" || app_id == "org.gnome.VolumeControl" ||
        app_id == "org.kde.kmixd" || app_id == "org.kde.plasma-pa")
        return;
    std::lock_guard<std::mutex> lock(mixer_mutex);
    auto n = mixer_find_or_create(AudioNodeKind::SourceOutput, i->index);
    mixer_pa_stream_identity(n->info, i->name, i->proplist, "audio-input-microphone");
    n->info.device = i->source;
    mixer_pa_volumes(n->info, i->volume);
    n->info.mute = i->mute;
    n->info.has_volume = i->has_volume && i->volume_writable;
    n->info.corked = i->corked;
    n->info.peak_supported = true;
    mixer_changed();
}

static void mixer_pa_card(const pa_card_info *i) {
    if (!i)
        return;
    std::lock_guard<std::mutex> lock(mixer_mutex);
    auto c = mixer_find_card(i->index);
    if (!c) {
        c = new MixerCard;
        c->info.index = i->index;
        mixer_cards.push_back(c);
    }
    c->info.name = i->name ? i->name : "";
    c->info.description = mixer_pa_prop(i->proplist, PA_PROP_DEVICE_DESCRIPTION);
    if (c->info.description.empty())
        c->info.description = c->info.name;
    c->info.icon_name = mixer_pa_prop(i->proplist, "device.icon_name");
    if (c->info.icon_name.empty())
        c->info.icon_name = "audio-card";
    c->info.profiles.clear();
    for (uint32_t p = 0; p < i->n_profiles; p++) {
        auto profile = i->profiles2 ? i->profiles2[p] : nullptr;
        if (!profile)
            continue;
        AudioProfileInfo info;
        info.name = profile->name ? profile->name : "";
        info.description = profile->description ? profile->description : info.name;
        info.available = profile->available != 0;
        info.priority = profile->priority;
        info.n_sinks = profile->n_sinks;
        info.n_sources = profile->n_sources;
        c->info.profiles.push_back(info);
    }
    std::sort(c->info.profiles.begin(), c->info.profiles.end(), [](const AudioProfileInfo &a, const AudioProfileInfo &b) {
        return a.priority > b.priority;
    });
    c->info.active_profile = i->active_profile2 && i->active_profile2->name ? i->active_profile2->name : "";
    mixer_changed();
}

static void mixer_pa_remove_card(uint32_t index) {
    std::lock_guard<std::mutex> lock(mixer_mutex);
    for (size_t i = 0; i < mixer_cards.size(); i++) {
        if (mixer_cards[i]->info.index == index) {
            mixer_delete_card(mixer_cards[i]);
            mixer_cards.erase(mixer_cards.begin() + (long) i);
            mixer_changed();
            return;
        }
    }
}

static void mixer_pa_remove(AudioNodeKind kind, uint32_t index) {
    std::lock_guard<std::mutex> lock(mixer_mutex);
    mixer_remove(kind, index);
}

static void on_mixer_source(pa_context *, const pa_source_info *i, int eol, void *) {
    if (eol == 0)
        mixer_pa_source(i);
}

static void on_mixer_source_output(pa_context *, const pa_source_output_info *i, int eol, void *) {
    if (eol == 0)
        mixer_pa_source_output(i);
}

static void on_mixer_card(pa_context *, const pa_card_info *i, int eol, void *) {
    if (eol == 0)
        mixer_pa_card(i);
}

// --- PipeWire model ---

static AudioNodeKind mixer_pw_kind(const char *media_class, bool *ok) {
    *ok = true;
    if (media_class) {
        if (strcmp(media_class, "Audio/Sink") == 0 || strcmp(media_class, "Audio/Duplex") == 0)
            return AudioNodeKind::Sink;
        if (strcmp(media_class, "Audio/Source") == 0 || strcmp(media_class, "Audio/Source/Virtual") == 0)
            return AudioNodeKind::Source;
        if (strcmp(media_class, "Stream/Output/Audio") == 0)
            return AudioNodeKind::SinkInput;
        if (strcmp(media_class, "Stream/Input/Audio") == 0)
            return AudioNodeKind::SourceOutput;
    }
    *ok = false;
    return AudioNodeKind::Sink;
}

static MixerNode *mixer_pw_find_node(uint32_t id) {
    for (auto n: mixer_nodes)
        if (n->pw_proxy && n->info.index == id)
            return n;
    return nullptr;
}

static void mixer_pw_update_ports(MixerNode *n) {
    if (n->info.kind != AudioNodeKind::Sink && n->info.kind != AudioNodeKind::Source)
        return;
    n->info.ports.clear();
    n->info.active_port.clear();
    auto card = mixer_find_card(n->pw_device_id);
    if (!card)
        return;
    int direction = n->info.kind == AudioNodeKind::Sink ? SPA_DIRECTION_OUTPUT : SPA_DIRECTION_INPUT;
    for (auto &route: card->pw_routes) {
        if (route.direction != direction)
            continue;
        if (!route.devices.empty() && n->pw_profile_device >= 0 &&
            std::find(route.devices.begin(), route.devices.end(), n->pw_profile_device) == route.devices.end())
            continue;
        AudioPortInfo port;
        port.name = route.name;
        port.description = route.description.empty() ? route.name : route.description;
        port.available = route.available;
        port.priority = route.priority;
        n->info.ports.push_back(port);
    }
    std::sort(n->info.ports.begin(), n->info.ports.end(), [](const AudioPortInfo &a, const AudioPortInfo &b) {
        return a.priority > b.priority;
    });
    auto active = card->pw_active_routes.find(n->pw_profile_device);
    if (active != card->pw_active_routes.end()) {
        for (auto &route: card->pw_routes)
            if (route.index == active->second && route.direction == direction)
                n->info.active_port = route.name;
    }
}

static void mixer_pw_update_stream_devices() {
    for (auto n: mixer_nodes) {
        if (!n->pw_proxy)
            continue;
        if (n->info.kind != AudioNodeKind::SinkInput && n->info.kind != AudioNodeKind::SourceOutput)
            continue;
        uint32_t device = UINT32_MAX;
        for (auto &[id, link]: mixer_pw_links) {
            if (n->info.kind == AudioNodeKind::SinkInput && link.first == n->info.index) {
                if (auto target = mixer_pw_find_node(link.second))
                    if (target->info.kind == AudioNodeKind::Sink)
                        device = link.second;
            } else if (n->info.kind == AudioNodeKind::SourceOutput && link.second == n->info.index) {
                if (auto target = mixer_pw_find_node(link.first))
                    if (target->info.kind == AudioNodeKind::Source || target->info.kind == AudioNodeKind::Sink)
                        device = link.first;
            }
        }
        if (device != n->info.device) {
            n->info.device = device;
            mixer_changed();
        }
    }
}

static void mixer_pw_node_identity(MixerNode *n, const spa_dict *props) {
    if (!props)
        return;
    auto get = [props](const char *key) -> std::string {
        const char *v = spa_dict_lookup(props, key);
        return v ? v : "";
    };
    auto node_name = get(PW_KEY_NODE_NAME);
    if (!node_name.empty())
        n->info.name = node_name;
    auto serial = get(PW_KEY_OBJECT_SERIAL);
    if (!serial.empty())
        n->pw_serial = serial;
    auto device_id = get(PW_KEY_DEVICE_ID);
    if (!device_id.empty())
        n->pw_device_id = (uint32_t) strtoul(device_id.c_str(), nullptr, 10);
    auto profile_device = get("card.profile.device");
    if (!profile_device.empty())
        n->pw_profile_device = atoi(profile_device.c_str());

    if (n->info.kind == AudioNodeKind::Sink || n->info.kind == AudioNodeKind::Source) {
        auto description = get(PW_KEY_NODE_DESCRIPTION);
        if (description.empty())
            description = get(PW_KEY_NODE_NICK);
        if (!description.empty())
            n->info.description = description;
        if (n->info.description.empty())
            n->info.description = n->info.name;
        auto icon = get(PW_KEY_DEVICE_ICON_NAME);
        if (icon.empty())
            icon = get("device.icon_name");
        if (!icon.empty())
            n->info.icon_name = icon;
        if (n->info.icon_name.empty())
            n->info.icon_name = n->info.kind == AudioNodeKind::Sink ? "audio-card" : "audio-input-microphone";
        n->info.device = n->pw_device_id;
    } else {
        auto media = get(PW_KEY_MEDIA_NAME);
        if (!media.empty())
            n->info.description = media;
        else if (n->info.description.empty())
            n->info.description = node_name;
        auto app = get(PW_KEY_APP_NAME);
        if (!app.empty())
            n->info.app_name = app;
        auto pid = get(PW_KEY_APP_PROCESS_ID);
        if (pid.empty())
            pid = get("client.pid");
        if (!pid.empty())
            n->info.pid = parse_pid_value(pid.c_str());
        if (n->info.icon_name.empty() || n->info.icon_name == "applications-multimedia" ||
            n->info.icon_name == "audio-input-microphone") {
            n->info.icon_name = mixer_pick_icon(get(PW_KEY_APP_ICON_NAME), n->info.app_name, n->info.description,
                n->info.kind == AudioNodeKind::SinkInput ? "applications-multimedia" : "audio-input-microphone");
        }
    }
    n->info.peak_supported = true;
}

static void mixer_pw_node_info(void *data, const pw_node_info *info) {
    auto n = (MixerNode *) data;
    if (!info)
        return;
    std::lock_guard<std::mutex> lock(mixer_mutex);
    mixer_pw_node_identity(n, info->props);
    n->info.corked = info->state != PW_NODE_STATE_RUNNING;
    if (!n->pw_subscribed) {
        n->pw_subscribed = true;
        uint32_t ids[] = {SPA_PARAM_Props};
        pw_node_subscribe_params(n->pw_proxy, ids, 1);
    }
    mixer_pw_update_ports(n);
    if (mixer_meters_on)
        mixer_pw_meter_create(n);
    mixer_changed();
}

static void mixer_pw_node_param(void *data, int, uint32_t id, uint32_t, uint32_t, const spa_pod *param) {
    auto n = (MixerNode *) data;
    if (!param || id != SPA_PARAM_Props)
        return;
    std::lock_guard<std::mutex> lock(mixer_mutex);
    bool mute = n->info.mute;
    float single = -1;
    uint32_t value_size = 0, value_type = SPA_ID_INVALID, n_values = 0;
    void *values = nullptr;
    if (spa_pod_parse_object(param, SPA_TYPE_OBJECT_Props, nullptr,
                             SPA_PROP_mute, SPA_POD_OPT_Bool(&mute),
                             SPA_PROP_channelVolumes, SPA_POD_OPT_Array(&value_size, &value_type, &n_values, &values),
                             SPA_PROP_volume, SPA_POD_OPT_Float(&single)) < 0)
        return;
    n->info.mute = mute;
    if (values && value_size == sizeof(float) && value_type == SPA_TYPE_Float && n_values > 0) {
        n->pw_linear.assign((float *) values, (float *) values + std::min<uint32_t>(n_values, SPA_AUDIO_MAX_CHANNELS));
    } else if (single >= 0) {
        n->pw_linear.assign(1, single);
    } else {
        return;
    }
    n->info.channel_volumes.clear();
    for (auto v: n->pw_linear)
        n->info.channel_volumes.push_back(std::cbrt(std::max(0.0f, v)));
    mixer_changed();
}

static const pw_node_events mixer_pw_node_events = {
        PW_VERSION_NODE_EVENTS,
        mixer_pw_node_info,
        mixer_pw_node_param,
};

static void mixer_pw_card_refresh(MixerCard *c) {
    c->info.profiles.clear();
    c->info.active_profile.clear();
    for (auto &[index, profile]: c->pw_profiles) {
        c->info.profiles.push_back(profile);
        if (index == c->pw_active_profile)
            c->info.active_profile = profile.name;
    }
    std::sort(c->info.profiles.begin(), c->info.profiles.end(), [](const AudioProfileInfo &a, const AudioProfileInfo &b) {
        return a.priority > b.priority;
    });
    for (auto n: mixer_nodes)
        if (n->pw_proxy && n->pw_device_id == c->info.index)
            mixer_pw_update_ports(n);
    mixer_changed();
}

static void mixer_pw_device_info(void *data, const pw_device_info *info) {
    auto c = (MixerCard *) data;
    if (!info)
        return;
    std::lock_guard<std::mutex> lock(mixer_mutex);
    if (info->props) {
        auto get = [info](const char *key) -> std::string {
            const char *v = spa_dict_lookup(info->props, key);
            return v ? v : "";
        };
        c->info.name = get(PW_KEY_DEVICE_NAME);
        c->info.description = get(PW_KEY_DEVICE_DESCRIPTION);
        if (c->info.description.empty())
            c->info.description = get(PW_KEY_DEVICE_NICK);
        if (c->info.description.empty())
            c->info.description = c->info.name;
        c->info.icon_name = get(PW_KEY_DEVICE_ICON_NAME);
        if (c->info.icon_name.empty())
            c->info.icon_name = "audio-card";
    }
    if (!c->pw_subscribed) {
        c->pw_subscribed = true;
        uint32_t ids[] = {SPA_PARAM_EnumProfile, SPA_PARAM_Profile, SPA_PARAM_EnumRoute, SPA_PARAM_Route};
        pw_device_subscribe_params(c->pw_proxy, ids, SPA_N_ELEMENTS(ids));
    }
    mixer_changed();
}

static std::vector<int> mixer_pw_int_array(const spa_pod *pod) {
    std::vector<int> result;
    if (!pod)
        return result;
    uint32_t n_values = 0;
    auto values = (int32_t *) spa_pod_get_array(pod, &n_values);
    if (!values || SPA_POD_ARRAY_VALUE_TYPE(pod) != SPA_TYPE_Int)
        return result;
    result.assign(values, values + n_values);
    return result;
}

static void mixer_pw_device_param(void *data, int, uint32_t id, uint32_t, uint32_t, const spa_pod *param) {
    auto c = (MixerCard *) data;
    if (!param)
        return;
    std::lock_guard<std::mutex> lock(mixer_mutex);
    if (id == SPA_PARAM_EnumProfile) {
        int32_t index = -1, priority = 0;
        uint32_t available = SPA_PARAM_AVAILABILITY_unknown;
        const char *name = nullptr, *description = nullptr;
        if (spa_pod_parse_object(param, SPA_TYPE_OBJECT_ParamProfile, nullptr,
                                 SPA_PARAM_PROFILE_index, SPA_POD_Int(&index),
                                 SPA_PARAM_PROFILE_name, SPA_POD_String(&name),
                                 SPA_PARAM_PROFILE_description, SPA_POD_OPT_String(&description),
                                 SPA_PARAM_PROFILE_priority, SPA_POD_OPT_Int(&priority),
                                 SPA_PARAM_PROFILE_available, SPA_POD_OPT_Id(&available)) < 0)
            return;
        AudioProfileInfo profile;
        profile.name = name ? name : "";
        profile.description = description ? description : profile.name;
        profile.priority = (uint32_t) std::max(0, priority);
        profile.available = available != SPA_PARAM_AVAILABILITY_no;
        bool replaced = false;
        for (auto &existing: c->pw_profiles) {
            if (existing.first == index) {
                existing.second = profile;
                replaced = true;
            }
        }
        if (!replaced)
            c->pw_profiles.emplace_back(index, profile);
    } else if (id == SPA_PARAM_Profile) {
        int32_t index = -1;
        if (spa_pod_parse_object(param, SPA_TYPE_OBJECT_ParamProfile, nullptr,
                                 SPA_PARAM_PROFILE_index, SPA_POD_Int(&index)) < 0)
            return;
        c->pw_active_profile = index;
    } else if (id == SPA_PARAM_EnumRoute) {
        int32_t index = -1, priority = 0;
        uint32_t direction = 0, available = SPA_PARAM_AVAILABILITY_unknown;
        const char *name = nullptr, *description = nullptr;
        const spa_pod *devices = nullptr;
        if (spa_pod_parse_object(param, SPA_TYPE_OBJECT_ParamRoute, nullptr,
                                 SPA_PARAM_ROUTE_index, SPA_POD_Int(&index),
                                 SPA_PARAM_ROUTE_direction, SPA_POD_Id(&direction),
                                 SPA_PARAM_ROUTE_name, SPA_POD_String(&name),
                                 SPA_PARAM_ROUTE_description, SPA_POD_OPT_String(&description),
                                 SPA_PARAM_ROUTE_priority, SPA_POD_OPT_Int(&priority),
                                 SPA_PARAM_ROUTE_available, SPA_POD_OPT_Id(&available),
                                 SPA_PARAM_ROUTE_devices, SPA_POD_OPT_Pod(&devices)) < 0)
            return;
        MixerPwRoute route;
        route.index = index;
        route.direction = (int) direction;
        route.name = name ? name : "";
        route.description = description ? description : route.name;
        route.priority = (uint32_t) std::max(0, priority);
        route.available = available != SPA_PARAM_AVAILABILITY_no;
        route.devices = mixer_pw_int_array(devices);
        bool replaced = false;
        for (auto &existing: c->pw_routes) {
            if (existing.index == index && existing.direction == route.direction) {
                existing = route;
                replaced = true;
            }
        }
        if (!replaced)
            c->pw_routes.push_back(route);
    } else if (id == SPA_PARAM_Route) {
        int32_t index = -1, device = -1;
        if (spa_pod_parse_object(param, SPA_TYPE_OBJECT_ParamRoute, nullptr,
                                 SPA_PARAM_ROUTE_index, SPA_POD_Int(&index),
                                 SPA_PARAM_ROUTE_device, SPA_POD_Int(&device)) < 0)
            return;
        c->pw_active_routes[device] = index;
    } else {
        return;
    }
    mixer_pw_card_refresh(c);
}

static const pw_device_events mixer_pw_device_events = {
        PW_VERSION_DEVICE_EVENTS,
        mixer_pw_device_info,
        mixer_pw_device_param,
};

// Called from the registry listener (PipeWire loop thread, lock held).
static void mixer_pw_global(uint32_t id, const char *type, uint32_t version, const spa_dict *props) {
    if (!type || !props || !pw_registry_instance)
        return;
    if (strcmp(type, PW_TYPE_INTERFACE_Link) == 0) {
        const char *out = spa_dict_lookup(props, PW_KEY_LINK_OUTPUT_NODE);
        const char *in = spa_dict_lookup(props, PW_KEY_LINK_INPUT_NODE);
        if (!out || !in)
            return;
        std::lock_guard<std::mutex> lock(mixer_mutex);
        mixer_pw_links[id] = {(uint32_t) strtoul(out, nullptr, 10), (uint32_t) strtoul(in, nullptr, 10)};
        mixer_pw_update_stream_devices();
        return;
    }
    if (strcmp(type, PW_TYPE_INTERFACE_Device) == 0) {
        const char *media_class = spa_dict_lookup(props, PW_KEY_MEDIA_CLASS);
        if (!media_class || strcmp(media_class, "Audio/Device") != 0)
            return;
        std::lock_guard<std::mutex> lock(mixer_mutex);
        if (mixer_find_card(id))
            return;
        auto proxy = (pw_device *) pw_registry_bind(pw_registry_instance, id, PW_TYPE_INTERFACE_Device,
                                                    std::min(version, (uint32_t) PW_VERSION_DEVICE), 0);
        if (!proxy)
            return;
        auto c = new MixerCard;
        c->info.index = id;
        c->pw_proxy = proxy;
        const char *description = spa_dict_lookup(props, PW_KEY_DEVICE_DESCRIPTION);
        const char *name = spa_dict_lookup(props, PW_KEY_DEVICE_NAME);
        c->info.name = name ? name : "";
        c->info.description = description ? description : c->info.name;
        c->info.icon_name = "audio-card";
        mixer_cards.push_back(c);
        pw_device_add_listener(proxy, &c->pw_listener, &mixer_pw_device_events, c);
        mixer_changed();
        return;
    }
    if (strcmp(type, PW_TYPE_INTERFACE_Node) != 0)
        return;

    bool ok = false;
    auto kind = mixer_pw_kind(spa_dict_lookup(props, PW_KEY_MEDIA_CLASS), &ok);
    if (!ok)
        return;
    if (kind == AudioNodeKind::SourceOutput) {
        const char *monitor = spa_dict_lookup(props, PW_KEY_STREAM_MONITOR);
        const char *app_id = spa_dict_lookup(props, PW_KEY_APP_ID);
        const char *node_name = spa_dict_lookup(props, PW_KEY_NODE_NAME);
        if ((monitor && strcmp(monitor, "true") == 0) || (app_id && strcmp(app_id, mixer_app_id) == 0) ||
            (node_name && strcmp(node_name, "winbar-peak-detect") == 0))
            return;
    }

    std::lock_guard<std::mutex> lock(mixer_mutex);
    if (mixer_find(kind, id))
        return;
    auto proxy = (pw_node *) pw_registry_bind(pw_registry_instance, id, PW_TYPE_INTERFACE_Node,
                                              std::min(version, (uint32_t) PW_VERSION_NODE), 0);
    if (!proxy)
        return;
    auto n = mixer_find_or_create(kind, id);
    n->pw_proxy = proxy;
    mixer_pw_node_identity(n, props);
    pw_node_add_listener(proxy, &n->pw_listener, &mixer_pw_node_events, n);
    mixer_pw_update_stream_devices();
    mixer_changed();
}

static void mixer_pw_global_remove(uint32_t id) {
    std::lock_guard<std::mutex> lock(mixer_mutex);
    if (mixer_pw_links.erase(id)) {
        mixer_pw_update_stream_devices();
        return;
    }
    for (size_t i = 0; i < mixer_nodes.size(); i++) {
        if (mixer_nodes[i]->pw_proxy && mixer_nodes[i]->info.index == id) {
            mixer_delete_node(mixer_nodes[i]);
            mixer_nodes.erase(mixer_nodes.begin() + (long) i);
            mixer_pw_update_stream_devices();
            mixer_changed();
            return;
        }
    }
    for (size_t i = 0; i < mixer_cards.size(); i++) {
        if (mixer_cards[i]->pw_proxy && mixer_cards[i]->info.index == id) {
            mixer_delete_card(mixer_cards[i]);
            mixer_cards.erase(mixer_cards.begin() + (long) i);
            mixer_changed();
            return;
        }
    }
}

static void mixer_pw_set_props(MixerNode *n, const bool *mute, const std::vector<float> *linear) {
    if (!n->pw_proxy)
        return;
    uint8_t buffer[1024];
    spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
    spa_pod_frame f;
    spa_pod_builder_push_object(&b, &f, SPA_TYPE_OBJECT_Props, SPA_PARAM_Props);
    if (mute) {
        spa_pod_builder_prop(&b, SPA_PROP_mute, 0);
        spa_pod_builder_bool(&b, *mute);
    }
    if (linear && !linear->empty()) {
        spa_pod_builder_prop(&b, SPA_PROP_channelVolumes, 0);
        spa_pod_builder_array(&b, sizeof(float), SPA_TYPE_Float, (uint32_t) linear->size(), linear->data());
    }
    auto pod = (const spa_pod *) spa_pod_builder_pop(&b, &f);
    pw_node_set_param(n->pw_proxy, SPA_PARAM_Props, 0, pod);
}

// --- ALSA model ---

static const char *alsa_playback_preference[] = {"Master", "PCM", "Speaker", "Headphone", "Front"};
static const char *alsa_capture_preference[] = {"Capture", "Mic", "Internal Mic", "Front Mic", "Rear Mic", "Line"};

static std::string alsa_elem_name(snd_mixer_elem_t *elem) {
    std::string name = snd_mixer_selem_get_name(elem);
    if (auto index = snd_mixer_selem_get_index(elem))
        name += " " + std::to_string(index);
    return name;
}

static snd_mixer_elem_t *alsa_find_elem(snd_mixer_t *mixer, bool playback, const std::string &wanted) {
    snd_mixer_elem_t *first = nullptr;
    snd_mixer_elem_t *preferred = nullptr;
    int preferred_rank = INT_MAX;
    for (auto elem = snd_mixer_first_elem(mixer); elem; elem = snd_mixer_elem_next(elem)) {
        if (!snd_mixer_selem_is_active(elem))
            continue;
        bool has = playback ? snd_mixer_selem_has_playback_volume(elem) : snd_mixer_selem_has_capture_volume(elem);
        if (!has)
            continue;
        if (!wanted.empty() && alsa_elem_name(elem) == wanted)
            return elem;
        if (!first)
            first = elem;
        auto name = snd_mixer_selem_get_name(elem);
        int rank = 0;
        for (auto candidate: playback ? std::vector<const char *>(std::begin(alsa_playback_preference), std::end(alsa_playback_preference))
                                      : std::vector<const char *>(std::begin(alsa_capture_preference), std::end(alsa_capture_preference))) {
            if (strcmp(name, candidate) == 0 && rank < preferred_rank) {
                preferred = elem;
                preferred_rank = rank;
            }
            rank++;
        }
    }
    return preferred ? preferred : first;
}

// Re-reads volume/mute of every ALSA node; called with mixer_mutex held.
static void alsa_mixer_refresh() {
    bool changed = false;
    for (auto c: mixer_cards)
        if (c->alsa_mixer)
            snd_mixer_handle_events(c->alsa_mixer);
    for (auto n: mixer_nodes) {
        if (!n->alsa_elem)
            continue;
        bool playback = n->info.kind == AudioNodeKind::Sink;
        double volume = playback ? get_normalized_playback_volume(n->alsa_elem, SND_MIXER_SCHN_FRONT_LEFT)
                                 : get_normalized_capture_volume(n->alsa_elem, SND_MIXER_SCHN_FRONT_LEFT);
        int on = 1;
        bool has_switch = playback ? snd_mixer_selem_has_playback_switch(n->alsa_elem)
                                   : snd_mixer_selem_has_capture_switch(n->alsa_elem);
        if (has_switch) {
            if (playback)
                snd_mixer_selem_get_playback_switch(n->alsa_elem, SND_MIXER_SCHN_FRONT_LEFT, &on);
            else
                snd_mixer_selem_get_capture_switch(n->alsa_elem, SND_MIXER_SCHN_FRONT_LEFT, &on);
        }
        bool mute = !on;
        if (n->info.channel_volumes.size() != 1 || std::abs(n->info.channel_volumes[0] - volume) > 0.0001 ||
            n->info.mute != mute) {
            n->info.channel_volumes = {volume};
            n->info.mute = mute;
            changed = true;
        }
    }
    if (changed)
        mixer_changed();
}

static void alsa_mixer_add_node(MixerCard *card, int card_number, bool playback, const std::string &card_name) {
    auto elem = alsa_find_elem(card->alsa_mixer, playback, "");
    if (!elem)
        return;
    auto n = new MixerNode;
    n->info.kind = playback ? AudioNodeKind::Sink : AudioNodeKind::Source;
    n->info.index = (uint32_t) card_number;
    n->info.name = card->info.name;
    n->info.description = card_name;
    n->info.icon_name = playback ? "audio-card" : "audio-input-microphone";
    n->info.device = (uint32_t) card_number;
    n->alsa_mixer = card->alsa_mixer;
    n->alsa_elem = elem;
    // ALSA has no ports in the PulseAudio sense; expose the volume controls instead so the user
    // can pick which one the slider drives.
    for (auto e = snd_mixer_first_elem(card->alsa_mixer); e; e = snd_mixer_elem_next(e)) {
        if (!snd_mixer_selem_is_active(e))
            continue;
        if (playback ? !snd_mixer_selem_has_playback_volume(e) : !snd_mixer_selem_has_capture_volume(e))
            continue;
        AudioPortInfo port;
        port.name = alsa_elem_name(e);
        port.description = port.name;
        n->info.ports.push_back(port);
    }
    n->info.active_port = alsa_elem_name(elem);
    mixer_nodes.push_back(n);
}

static void alsa_mixer_scan() {
    std::lock_guard<std::mutex> lock(mixer_mutex);
    int card_number = -1;
    while (snd_card_next(&card_number) >= 0 && card_number >= 0) {
        auto hw = "hw:" + std::to_string(card_number);
        snd_mixer_t *mixer = nullptr;
        if (snd_mixer_open(&mixer, 0) != 0)
            continue;
        if (snd_mixer_attach(mixer, hw.c_str()) != 0 || snd_mixer_selem_register(mixer, nullptr, nullptr) != 0 ||
            snd_mixer_load(mixer) != 0) {
            snd_mixer_close(mixer);
            continue;
        }
        auto card = new MixerCard;
        card->info.index = (uint32_t) card_number;
        card->info.name = hw;
        card->info.icon_name = "audio-card";
        card->alsa_mixer = mixer;
        char *name = nullptr;
        char *long_name = nullptr;
        if (snd_card_get_name(card_number, &name) == 0 && name)
            card->info.description = name;
        if (snd_card_get_longname(card_number, &long_name) == 0 && long_name) {
            if (card->info.description.empty())
                card->info.description = long_name;
        }
        free(name);
        free(long_name);
        if (card->info.description.empty())
            card->info.description = hw;
        mixer_cards.push_back(card);

        alsa_mixer_add_node(card, card_number, true, card->info.description);
        alsa_mixer_add_node(card, card_number, false, card->info.description);
    }
    for (auto n: mixer_nodes) {
        if (n->info.kind == AudioNodeKind::Sink && mixer_default_sink.empty())
            mixer_default_sink = n->info.name;
        if (n->info.kind == AudioNodeKind::Source && mixer_default_source.empty())
            mixer_default_source = n->info.name;
    }
    alsa_mixer_refresh();
    mixer_changed();
}

// --- Public mixer API ---

std::vector<AudioNodeInfo> audio_mixer_nodes() {
    std::vector<AudioNodeInfo> result;
    std::lock_guard<std::mutex> lock(mixer_mutex);
    if (backend == AudioBackend::ALSA)
        alsa_mixer_refresh();
    result.reserve(mixer_nodes.size());
    for (auto n: mixer_nodes) {
        result.push_back(n->info);
        result.back().peak = n->peak_accum;
        n->peak_accum = 0;
        result.back().peak_supported = n->info.peak_supported && (n->pa_meter || n->pw_meter ||
            n->info.kind == AudioNodeKind::SourceOutput);
    }
    std::sort(result.begin(), result.end(), [](const AudioNodeInfo &a, const AudioNodeInfo &b) {
        if (a.kind != b.kind)
            return a.kind < b.kind;
        return a.index < b.index;
    });
    return result;
}

std::vector<AudioCardInfo> audio_mixer_cards() {
    std::vector<AudioCardInfo> result;
    std::lock_guard<std::mutex> lock(mixer_mutex);
    for (auto c: mixer_cards)
        result.push_back(c->info);
    std::sort(result.begin(), result.end(), [](const AudioCardInfo &a, const AudioCardInfo &b) {
        return a.index < b.index;
    });
    return result;
}

std::string audio_mixer_default_sink() {
    std::lock_guard<std::mutex> lock(mixer_mutex);
    return mixer_default_sink;
}

std::string audio_mixer_default_source() {
    std::lock_guard<std::mutex> lock(mixer_mutex);
    return mixer_default_source;
}

unsigned long audio_mixer_revision() {
    return mixer_rev;
}

std::string audio_backend_name() {
    if (!audio_running || !ready)
        return "";
    switch (backend) {
        case AudioBackend::PULSEAUDIO: return "PulseAudio";
        case AudioBackend::PIPEWIRE: return "PipeWire";
        case AudioBackend::ALSA: return "ALSA";
        default: return "";
    }
}

void audio_mixer_set_volume(AudioNodeKind kind, uint32_t index, double volume) {
    volume = std::clamp(volume, 0.0, 1.5);
    audio([kind, index, volume]() {
        std::lock_guard<std::mutex> lock(mixer_mutex);
        auto n = mixer_find(kind, index);
        if (!n)
            return;
        // Keep the balance between channels; scale them so the loudest one hits `volume`.
        auto current = n->info.channel_volumes;
        if (current.empty())
            current.push_back(volume);
        double loudest = n->info.volume();
        std::vector<double> wanted;
        for (auto v: current)
            wanted.push_back(loudest > 0.0001 ? v / loudest * volume : volume);

        if (backend == AudioBackend::PULSEAUDIO) {
            pa_cvolume cv;
            pa_cvolume_init(&cv);
            cv.channels = (uint8_t) std::min<size_t>(wanted.size(), PA_CHANNELS_MAX);
            for (int i = 0; i < cv.channels; i++)
                cv.values[i] = (pa_volume_t) std::round(wanted[i] * PA_VOLUME_NORM);
            pa_operation *o = nullptr;
            switch (kind) {
                case AudioNodeKind::Sink: o = pa_context_set_sink_volume_by_index(context, index, &cv, nullptr, nullptr); break;
                case AudioNodeKind::Source: o = pa_context_set_source_volume_by_index(context, index, &cv, nullptr, nullptr); break;
                case AudioNodeKind::SinkInput: o = pa_context_set_sink_input_volume(context, index, &cv, nullptr, nullptr); break;
                case AudioNodeKind::SourceOutput: o = pa_context_set_source_output_volume(context, index, &cv, nullptr, nullptr); break;
            }
            if (o)
                pa_operation_unref(o);
        } else if (backend == AudioBackend::PIPEWIRE) {
            std::vector<float> linear;
            for (auto v: wanted)
                linear.push_back((float) (v * v * v));
            mixer_pw_set_props(n, nullptr, &linear);
        } else if (backend == AudioBackend::ALSA) {
            if (!n->alsa_elem)
                return;
            double v = std::min(volume, 1.0);
            if (kind == AudioNodeKind::Sink)
                set_normalized_playback_volume(n->alsa_elem, v, 0);
            else
                set_normalized_capture_volume(n->alsa_elem, v, 0);
            alsa_mixer_refresh();
        }
    });
}

void audio_mixer_set_mute(AudioNodeKind kind, uint32_t index, bool mute) {
    audio([kind, index, mute]() {
        std::lock_guard<std::mutex> lock(mixer_mutex);
        auto n = mixer_find(kind, index);
        if (!n)
            return;
        if (backend == AudioBackend::PULSEAUDIO) {
            pa_operation *o = nullptr;
            switch (kind) {
                case AudioNodeKind::Sink: o = pa_context_set_sink_mute_by_index(context, index, mute, nullptr, nullptr); break;
                case AudioNodeKind::Source: o = pa_context_set_source_mute_by_index(context, index, mute, nullptr, nullptr); break;
                case AudioNodeKind::SinkInput: o = pa_context_set_sink_input_mute(context, index, mute, nullptr, nullptr); break;
                case AudioNodeKind::SourceOutput: o = pa_context_set_source_output_mute(context, index, mute, nullptr, nullptr); break;
            }
            if (o)
                pa_operation_unref(o);
        } else if (backend == AudioBackend::PIPEWIRE) {
            mixer_pw_set_props(n, &mute, nullptr);
        } else if (backend == AudioBackend::ALSA) {
            if (!n->alsa_elem)
                return;
            if (kind == AudioNodeKind::Sink) {
                if (snd_mixer_selem_has_playback_switch(n->alsa_elem))
                    snd_mixer_selem_set_playback_switch_all(n->alsa_elem, !mute);
            } else {
                if (snd_mixer_selem_has_capture_switch(n->alsa_elem))
                    snd_mixer_selem_set_capture_switch_all(n->alsa_elem, !mute);
            }
            alsa_mixer_refresh();
        }
    });
}

void audio_mixer_set_default(AudioNodeKind kind, uint32_t index) {
    if (kind != AudioNodeKind::Sink && kind != AudioNodeKind::Source)
        return;
    audio([kind, index]() {
        std::lock_guard<std::mutex> lock(mixer_mutex);
        auto n = mixer_find(kind, index);
        if (!n)
            return;
        if (backend == AudioBackend::PULSEAUDIO) {
            pa_operation *o = kind == AudioNodeKind::Sink
                ? pa_context_set_default_sink(context, n->info.name.c_str(), nullptr, nullptr)
                : pa_context_set_default_source(context, n->info.name.c_str(), nullptr, nullptr);
            if (o)
                pa_operation_unref(o);
        } else if (backend == AudioBackend::PIPEWIRE) {
            if (!pw_metadata_instance)
                return;
            std::string value = "{ \"name\": \"" + n->info.name + "\" }";
            pw_metadata_set_property(pw_metadata_instance, PW_ID_CORE,
                kind == AudioNodeKind::Sink ? "default.configured.audio.sink" : "default.configured.audio.source",
                "Spa:String:JSON", value.c_str());
        } else if (backend == AudioBackend::ALSA) {
            // The ALSA default device lives in asoundrc; only remember the choice for the UI.
            if (kind == AudioNodeKind::Sink)
                mixer_default_sink = n->info.name;
            else
                mixer_default_source = n->info.name;
            mixer_changed();
        }
    });
}

void audio_mixer_move_stream(AudioNodeKind kind, uint32_t index, uint32_t device) {
    if (kind != AudioNodeKind::SinkInput && kind != AudioNodeKind::SourceOutput)
        return;
    audio([kind, index, device]() {
        std::lock_guard<std::mutex> lock(mixer_mutex);
        auto n = mixer_find(kind, index);
        if (!n)
            return;
        if (backend == AudioBackend::PULSEAUDIO) {
            pa_operation *o = kind == AudioNodeKind::SinkInput
                ? pa_context_move_sink_input_by_index(context, index, device, nullptr, nullptr)
                : pa_context_move_source_output_by_index(context, index, device, nullptr, nullptr);
            if (o)
                pa_operation_unref(o);
        } else if (backend == AudioBackend::PIPEWIRE) {
            if (!pw_metadata_instance)
                return;
            MixerNode *target = nullptr;
            for (auto candidate: mixer_nodes)
                if (candidate->pw_proxy && candidate->info.index == device)
                    target = candidate;
            if (!target)
                return;
            if (!target->pw_serial.empty()) {
                pw_metadata_set_property(pw_metadata_instance, index, "target.object", "Spa:Id",
                                         target->pw_serial.c_str());
            } else {
                pw_metadata_set_property(pw_metadata_instance, index, "target.object", nullptr,
                                         target->info.name.c_str());
            }
        }
    });
}

void audio_mixer_set_port(AudioNodeKind kind, uint32_t index, const std::string &port) {
    if (kind != AudioNodeKind::Sink && kind != AudioNodeKind::Source)
        return;
    audio([kind, index, port]() {
        std::lock_guard<std::mutex> lock(mixer_mutex);
        auto n = mixer_find(kind, index);
        if (!n)
            return;
        if (backend == AudioBackend::PULSEAUDIO) {
            pa_operation *o = kind == AudioNodeKind::Sink
                ? pa_context_set_sink_port_by_index(context, index, port.c_str(), nullptr, nullptr)
                : pa_context_set_source_port_by_index(context, index, port.c_str(), nullptr, nullptr);
            if (o)
                pa_operation_unref(o);
        } else if (backend == AudioBackend::PIPEWIRE) {
            auto card = mixer_find_card(n->pw_device_id);
            if (!card || !card->pw_proxy || n->pw_profile_device < 0)
                return;
            int direction = kind == AudioNodeKind::Sink ? SPA_DIRECTION_OUTPUT : SPA_DIRECTION_INPUT;
            for (auto &route: card->pw_routes) {
                if (route.name != port || route.direction != direction)
                    continue;
                uint8_t buffer[1024];
                spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
                auto pod = (const spa_pod *) spa_pod_builder_add_object(&b,
                    SPA_TYPE_OBJECT_ParamRoute, SPA_PARAM_Route,
                    SPA_PARAM_ROUTE_index, SPA_POD_Int(route.index),
                    SPA_PARAM_ROUTE_device, SPA_POD_Int(n->pw_profile_device),
                    SPA_PARAM_ROUTE_save, SPA_POD_Bool(true));
                pw_device_set_param(card->pw_proxy, SPA_PARAM_Route, 0, pod);
                return;
            }
        } else if (backend == AudioBackend::ALSA) {
            if (!n->alsa_mixer)
                return;
            if (auto elem = alsa_find_elem(n->alsa_mixer, kind == AudioNodeKind::Sink, port)) {
                if (alsa_elem_name(elem) == port) {
                    n->alsa_elem = elem;
                    n->info.active_port = port;
                    n->info.channel_volumes.clear();
                    alsa_mixer_refresh();
                    mixer_changed();
                }
            }
        }
    });
}

void audio_mixer_set_card_profile(uint32_t card_index, const std::string &profile) {
    audio([card_index, profile]() {
        std::lock_guard<std::mutex> lock(mixer_mutex);
        auto card = mixer_find_card(card_index);
        if (!card)
            return;
        if (backend == AudioBackend::PULSEAUDIO) {
            if (auto o = pa_context_set_card_profile_by_index(context, card_index, profile.c_str(), nullptr, nullptr))
                pa_operation_unref(o);
        } else if (backend == AudioBackend::PIPEWIRE) {
            if (!card->pw_proxy)
                return;
            for (auto &[index, info]: card->pw_profiles) {
                if (info.name != profile)
                    continue;
                uint8_t buffer[1024];
                spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
                auto pod = (const spa_pod *) spa_pod_builder_add_object(&b,
                    SPA_TYPE_OBJECT_ParamProfile, SPA_PARAM_Profile,
                    SPA_PARAM_PROFILE_index, SPA_POD_Int(index),
                    SPA_PARAM_PROFILE_save, SPA_POD_Bool(true));
                pw_device_set_param(card->pw_proxy, SPA_PARAM_Profile, 0, pod);
                return;
            }
        }
    });
}

void audio_mixer_meters(bool on) {
    // Reference counted: the settings audio page and the dock volume popup can both want meters.
    static std::mutex users_mutex;
    static int users = 0;
    {
        std::lock_guard<std::mutex> lock(users_mutex);
        users = std::max(0, users + (on ? 1 : -1));
        on = users > 0;
    }
    if (mixer_meters_on == on)
        return;
    mixer_meters_on = on;
    if (backend != AudioBackend::PULSEAUDIO && backend != AudioBackend::PIPEWIRE)
        return;
    audio([on]() {
        std::lock_guard<std::mutex> lock(mixer_mutex);
        for (auto n: mixer_nodes) {
            if (on)
                mixer_meter_create(n);
            else
                mixer_meter_destroy(n);
        }
    });
}

static void on_sink(pa_context *, const pa_sink_info *l, int eol, void *) {
    if (eol == 0 && l)
        mixer_pa_sink(l);
    if (eol != 0 || l == nullptr)
        return;
    defer(if (audio_change_callback) audio_change_callback());

    auto data = (pa_sink_info *) l;

    // PulseAudio may expose a synthetic fallback sink named auto_null.
    // Keep it out of the client list so it doesn't appear as a real device.
    if (data->name && strcmp(data->name, "auto_null") == 0) {
        for (int i = 0; i < audio_clients.size(); i++) {
            if (audio_clients[i]->index == data->index && audio_clients[i]->is_master) {
                delete audio_clients[i];
                audio_clients.erase(audio_clients.begin() + i);
                sort();
                break;
            }
        }
        return;
    }

    AudioClient *client = nullptr;
    bool create = true;
    for (auto c: audio_clients) {
        if (c->is_master && c->index == data->index) {
            client = c;
            create = false;
            break;
        }
    }
    if (client == nullptr)
        client = new AudioClient;
    client->title = data->name;
    client->subtitle = data->description;
    client->index = data->index;
    client->icon_name = "audio-card";
    client->is_master = true;
    client->monitor_source_name = l->monitor_source_name;
    client->volume = data->volume;
    client->mute = data->mute;
    if (create) {
        audio_clients.push_back(client);
        sort();
    }
}

static void on_sink_input(pa_context *, const pa_sink_input_info *l, int eol, void *) {
    if (eol == 0 && l)
        mixer_pa_sink_input(l);
    if (eol != 0 || l == nullptr)
        return;
    defer(if (audio_change_callback) audio_change_callback());

    auto data = (pa_sink_input_info *) l;

    AudioClient *client = nullptr;
    bool create = true;
    for (auto c: audio_clients) {
        if (!c->is_master && c->index == data->index) {
            client = c;
            create = false;
            break;
        }
    }
    if (client == nullptr)
        client = new AudioClient;
    client->title = data->name;
    client->index = data->index;
    client->pid = -1;

    if (l->proplist) {
        size_t nbytes;
        const void *data = nullptr;
        pa_proplist_get(l->proplist, PA_PROP_APPLICATION_NAME, &data, &nbytes);
        if (data) {
            client->subtitle = std::string((const char *) data, nbytes);
            data = nullptr;
        }
        pa_proplist_get(l->proplist, PA_PROP_APPLICATION_PROCESS_ID, &data, &nbytes);
        if (data) {
            std::string pid_str((const char *) data, nbytes);
            client->pid = parse_pid_value(pid_str.c_str());
            data = nullptr;
        }

        if (client->icon_name.empty()) {
            pa_proplist_get(l->proplist, PA_PROP_APPLICATION_ICON_NAME, &data, &nbytes);
            if (data) {
                std::string possible_icon_name = std::string((const char *) data, nbytes);
                data = nullptr;
                if (has_options(possible_icon_name))
                    client->icon_name = possible_icon_name;
                if (client->icon_name.empty()) {
                    for (char &i: possible_icon_name)
                        i = tolower(i);
                    if (has_options(possible_icon_name))
                        client->icon_name = possible_icon_name;

                    if (client->icon_name.empty()) {
                        possible_icon_name = client->title;
                        if (!client->subtitle.empty())
                            possible_icon_name = client->subtitle;
                        if (has_options(possible_icon_name))
                            client->icon_name = possible_icon_name;
                        if (client->icon_name.empty()) {
                            for (char &i: possible_icon_name)
                                i = tolower(i);
                            if (has_options(possible_icon_name))
                                client->icon_name = possible_icon_name;
                        }
                    }
                }
            } else {
                std::string possible_icon_name = client->title;
                if (!client->subtitle.empty())
                    possible_icon_name = client->subtitle;
                if (has_options(possible_icon_name))
                    client->icon_name = possible_icon_name;
                if (client->icon_name.empty()) {
                    for (char &i: possible_icon_name)
                        i = tolower(i);
                    if (has_options(possible_icon_name))
                        client->icon_name = possible_icon_name;
                }
            }
        }
    }
    client->volume = data->volume;
    client->mute = data->mute;
    if (create) {
        audio_clients.push_back(client);
        sort();
    }
}

static void on_server(pa_context *, const pa_server_info *i, void *) {
    if (i == nullptr)
        return;
    defer(if (audio_change_callback) audio_change_callback());
    default_sink_name = i->default_sink_name ? i->default_sink_name : "";
    {
        std::lock_guard<std::mutex> lock(mixer_mutex);
        mixer_default_sink = default_sink_name;
        mixer_default_source = i->default_source_name ? i->default_source_name : "";
        mixer_changed();
    }
    sort();
}

// Callback function to handle volume events
static void change_callback(pa_context *c, pa_subscription_event_type_t event_type, uint32_t index, void *userdata) {
    pa_subscription_event_type_t event_type_masked;
    pa_operation *o;

    event_type_masked = static_cast<pa_subscription_event_type_t>(event_type & PA_SUBSCRIPTION_EVENT_TYPE_MASK);

    switch (event_type & PA_SUBSCRIPTION_EVENT_FACILITY_MASK) {
        case PA_SUBSCRIPTION_EVENT_SINK: {
            if (event_type_masked == PA_SUBSCRIPTION_EVENT_REMOVE) {
                mixer_pa_remove(AudioNodeKind::Sink, index);
                bool removed = false;
                for (int i = 0; i < audio_clients.size(); i++) {
                    if (audio_clients[i]->index == index && audio_clients[i]->is_master) {
                        delete audio_clients[i];
                        audio_clients.erase(audio_clients.begin() + i);
                        removed = true;
                        sort();
                        break;
                    }
                }
                if (removed && audio_change_callback)
                    audio_change_callback();
            } else {
                if (!(o = pa_context_get_sink_info_by_index(c, index, on_sink, userdata))) {
                    fprintf(stderr, "ERROR: pa_context_get_sink_info_by_index() failed\n");
                    break;
                }
                pa_operation_unref(o);
            }
            break;
        }
        case PA_SUBSCRIPTION_EVENT_SINK_INPUT: {
            if (event_type_masked == PA_SUBSCRIPTION_EVENT_REMOVE) {
                mixer_pa_remove(AudioNodeKind::SinkInput, index);
                bool removed = false;
                for (int i = 0; i < audio_clients.size(); i++) {
                    if (audio_clients[i]->index == index && !audio_clients[i]->is_master) {
                        delete audio_clients[i];
                        audio_clients.erase(audio_clients.begin() + i);
                        removed = true;
                        sort();
                        break;
                    }
                }
                if (removed && audio_change_callback)
                    audio_change_callback();
            } else {
                if (!(o = pa_context_get_sink_input_info(c, index, on_sink_input, userdata))) {
                    fprintf(stderr, "ERROR: pa_context_get_sink_input_info() failed\n");
                    break;
                }
                pa_operation_unref(o);
            }
            break;
        }
        case PA_SUBSCRIPTION_EVENT_SOURCE: {
            if (event_type_masked == PA_SUBSCRIPTION_EVENT_REMOVE) {
                mixer_pa_remove(AudioNodeKind::Source, index);
            } else if ((o = pa_context_get_source_info_by_index(c, index, on_mixer_source, userdata))) {
                pa_operation_unref(o);
            }
            break;
        }
        case PA_SUBSCRIPTION_EVENT_SOURCE_OUTPUT: {
            if (event_type_masked == PA_SUBSCRIPTION_EVENT_REMOVE) {
                mixer_pa_remove(AudioNodeKind::SourceOutput, index);
            } else if ((o = pa_context_get_source_output_info(c, index, on_mixer_source_output, userdata))) {
                pa_operation_unref(o);
            }
            break;
        }
        case PA_SUBSCRIPTION_EVENT_CARD: {
            if (event_type_masked == PA_SUBSCRIPTION_EVENT_REMOVE) {
                mixer_pa_remove_card(index);
            } else if ((o = pa_context_get_card_info_by_index(c, index, on_mixer_card, userdata))) {
                pa_operation_unref(o);
            }
            break;
        }
        case PA_SUBSCRIPTION_EVENT_SERVER: {
            if (!(o = pa_context_get_server_info(c, on_server, userdata))) {
                fprintf(stderr, "ERROR: pa_context_get_server_info() failed\n");
                break;
            }
            pa_operation_unref(o);
            break;
        }
    }
}

static void iterate_pulseaudio_mainloop() {
    if (pa_mainloop_prepare(mainloop, -1) < 0) {
        audio_running = false;
        return;
    }
    if (pa_mainloop_poll(mainloop) < 0) {
        audio_running = false;
        return;
    }

    std::unique_lock<std::mutex> read_lock(read_mutex);

    audio_thread_busy = true;
    defer(audio_thread_busy = false);
    if (pa_mainloop_dispatch(mainloop) < 0) {
        audio_running = false;
        return;
    }
    if (needs_block) {
        needs_block = false;

        std::unique_lock<std::mutex> free_to_continue_lock(free_to_continue_mutex);
        {
            std::unique_lock<std::mutex> lock(signal_mutex);
            signal_met = true;
            needs_signal_condition.notify_all();
        }
        free_to_continue_condition.wait_for(free_to_continue_lock, std::chrono::milliseconds(70),
                                            []() { return free_to_continue; });
    }
}

static bool try_establishing_connection_with_pulseaudio() {
    mainloop = pa_mainloop_new();
    if (!mainloop) return false;
    mainloop_api = pa_mainloop_get_api(mainloop);
    if (!mainloop_api) return false;

    pa_proplist *prop_list = pa_proplist_new();
    if (!prop_list) return false;
    defer(pa_proplist_free(prop_list));
    pa_proplist_sets(prop_list, PA_PROP_APPLICATION_NAME, ("Winbar Volume Control"));
    pa_proplist_sets(prop_list, PA_PROP_APPLICATION_ID, "winbar.volume");
    pa_proplist_sets(prop_list, PA_PROP_APPLICATION_ICON_NAME, "audio-card");
    pa_proplist_sets(prop_list, PA_PROP_APPLICATION_VERSION, "1");
    context = pa_context_new_with_proplist(mainloop_api, "Winbar Volume Control", prop_list);
    if (!context) {
        pa_mainloop_free(mainloop);
        return false;
    }

    if (pa_context_connect(context, NULL, PA_CONTEXT_NOAUTOSPAWN, NULL) < 0) {
        return false;
    }

    pa_context_set_state_callback(context, [](pa_context *c, void *) {
        pa_context_state_t state = pa_context_get_state(c);
        if (state == PA_CONTEXT_READY) {
            pa_context_set_subscribe_callback(c, change_callback, nullptr);
            auto mask = (pa_subscription_mask_t) (PA_SUBSCRIPTION_MASK_SINK | PA_SUBSCRIPTION_MASK_SINK_INPUT |
                                                  PA_SUBSCRIPTION_MASK_SOURCE | PA_SUBSCRIPTION_MASK_SOURCE_OUTPUT |
                                                  PA_SUBSCRIPTION_MASK_CARD | PA_SUBSCRIPTION_MASK_SERVER);
            pa_operation *pa_op = pa_context_subscribe(c, mask, nullptr, nullptr);
            pa_operation_unref(pa_op);

            pa_op = pa_context_get_server_info(c, on_server, nullptr);
            pa_operation_unref(pa_op);
            pa_op = pa_context_get_sink_info_list(c, on_sink, nullptr);
            pa_operation_unref(pa_op);
            pa_op = pa_context_get_sink_input_info_list(c, on_sink_input, nullptr);
            pa_operation_unref(pa_op);
            pa_op = pa_context_get_source_info_list(c, on_mixer_source, nullptr);
            pa_operation_unref(pa_op);
            pa_op = pa_context_get_source_output_info_list(c, on_mixer_source_output, nullptr);
            pa_operation_unref(pa_op);
            pa_op = pa_context_get_card_info_list(c, on_mixer_card, nullptr);
            pa_operation_unref(pa_op);
            ready = true;
        }
        if (!PA_CONTEXT_IS_GOOD(state)) {
            audio_running = false;
            ready = false;
        }
    }, nullptr);

    audio_running = true;
    while (audio_running && !ready) {
        iterate_pulseaudio_mainloop();
    }
    return ready;
}

static void pipewire_update_client_identity(AudioClient *client, const spa_dict *props) {
    if (!client || !props)
        return;

    const char *node_name = spa_dict_lookup(props, PW_KEY_NODE_NAME);
    const char *node_description = spa_dict_lookup(props, PW_KEY_NODE_DESCRIPTION);
    const char *media_name = spa_dict_lookup(props, PW_KEY_MEDIA_NAME);
    const char *app_name = spa_dict_lookup(props, PW_KEY_APP_NAME);
    const char *icon_name = spa_dict_lookup(props, PW_KEY_APP_ICON_NAME);
    const char *process_id = spa_dict_lookup(props, "application.process.id");
    if ((!process_id || !*process_id))
        process_id = spa_dict_lookup(props, "client.pid");
    client->pid = parse_pid_value(process_id);
    if (node_name && *node_name)
        client->pw_node_name = node_name;

    if (client->is_master) {
        if (node_name && *node_name)
            client->title = node_name;
        if (node_description && *node_description)
            client->subtitle = node_description;
        if (client->title.empty())
            client->title = "Master";
        client->icon_name = "audio-card";
    } else {
        if (media_name && *media_name) {
            client->title = media_name;
        } else if (node_name && *node_name) {
            client->title = node_name;
        }
        if (app_name && *app_name)
            client->subtitle = app_name;

        if (icon_name && has_options(icon_name)) {
            client->icon_name = icon_name;
        } else if (!client->subtitle.empty() && has_options(client->subtitle)) {
            client->icon_name = client->subtitle;
        } else if (!client->title.empty() && has_options(client->title)) {
            client->icon_name = client->title;
        } else if (!icon_name) {
            std::string lowered = !client->subtitle.empty() ? client->subtitle : client->title;
            for (auto &ch: lowered)
                ch = static_cast<char>(tolower(static_cast<unsigned char>(ch)));
            if (has_options(lowered))
                client->icon_name = lowered;
        }
    }
}

static void pipewire_handle_node_param(AudioClient *client, uint32_t id, const spa_pod *param) {
    if (!client || !param || id != SPA_PARAM_Props)
        return;

    bool mute = client->mute;
    float single_volume = client->pw_channel_volumes.empty() ? 1.0f : client->pw_channel_volumes[0];
    float channel_volumes[SPA_AUDIO_MAX_CHANNELS] = {};
    uint32_t channel_value_size = 0;
    uint32_t channel_value_type = SPA_ID_INVALID;
    uint32_t n_channel_volumes = 0;
    void *channel_values = nullptr;

    int parsed = spa_pod_parse_object(param,
                                      SPA_TYPE_OBJECT_Props, nullptr,
                                      SPA_PROP_mute, SPA_POD_OPT_Bool(&mute),
                                      SPA_PROP_channelVolumes, SPA_POD_OPT_Array(&channel_value_size,
                                                                                  &channel_value_type,
                                                                                  &n_channel_volumes,
                                                                                  &channel_values),
                                      SPA_PROP_volume, SPA_POD_OPT_Float(&single_volume));
    if (parsed < 0)
        return;

    client->mute = mute;
    if (channel_values && channel_value_size == sizeof(float) && channel_value_type == SPA_TYPE_Float &&
        n_channel_volumes > 0) {
        uint32_t clamped = std::min(n_channel_volumes, static_cast<uint32_t>(SPA_AUDIO_MAX_CHANNELS));
        memcpy(channel_volumes, channel_values, clamped * sizeof(float));
        n_channel_volumes = clamped;
        update_pipewire_client_volumes(client, channel_volumes, n_channel_volumes);
    } else {
        update_pipewire_client_volumes(client, &single_volume, 1);
    }
}

static void pipewire_node_info_callback(void *data, const pw_node_info *info) {
    auto *bound = static_cast<PipeWireBoundNode *>(data);
    if (!bound || !info)
        return;

    auto *client = bound->client;
    if (!client)
        return;

    if (info->props) {
        pipewire_update_client_identity(client, info->props);
        const char *node_name = spa_dict_lookup(info->props, PW_KEY_NODE_NAME);
        if (client->is_master && node_name && *node_name && default_sink_name.empty()) {
            default_sink_name = node_name;
        }
    }

    client->index = static_cast<int>(bound->id);
    client->pw_node_id = bound->id;
    client->pw_node_ref = bound->node;
    client->is_master = bound->is_master;

    uint32_t ids[] = {SPA_PARAM_Props};
    pw_node_subscribe_params(bound->node, ids, 1);
    pw_node_enum_params(bound->node, 0, SPA_PARAM_Props, 0, UINT32_MAX, nullptr);

    sort();
    if (audio_change_callback)
        audio_change_callback();
}

static void pipewire_node_param_callback(void *data, int, uint32_t id, uint32_t, uint32_t, const spa_pod *param) {
    auto *bound = static_cast<PipeWireBoundNode *>(data);
    if (!bound || !bound->client)
        return;
    pipewire_handle_node_param(bound->client, id, param);
    sort();
    if (audio_change_callback)
        audio_change_callback();
}

static const pw_node_events pipewire_node_events = {
        PW_VERSION_NODE_EVENTS,
        pipewire_node_info_callback,
        pipewire_node_param_callback,
};

static int pipewire_metadata_property_callback(void *, uint32_t, const char *key, const char *, const char *value) {
    if (!key)
        return 0;
    if (strcmp(key, "default.audio.sink") == 0 || strcmp(key, "default.audio.source") == 0) {
        std::lock_guard<std::mutex> lock(mixer_mutex);
        auto name = parse_pipewire_default_name(value);
        if (strcmp(key, "default.audio.sink") == 0)
            mixer_default_sink = name;
        else
            mixer_default_source = name;
        mixer_changed();
    }
    if (strcmp(key, "default.audio.sink") != 0 && strcmp(key, "default.configured.audio.sink") != 0)
        return 0;

    std::string new_default = parse_pipewire_default_name(value);
    if (!new_default.empty()) {
        default_sink_name = new_default;
        sort();
        if (audio_change_callback)
            audio_change_callback();
    }
    return 0;
}

static const pw_metadata_events pipewire_metadata_events = {
        PW_VERSION_METADATA_EVENTS,
        pipewire_metadata_property_callback,
};

static void pipewire_registry_global_callback(void *, uint32_t id, uint32_t, const char *type, uint32_t version,
                                              const spa_dict *props) {
    if (!type)
        return;

    mixer_pw_global(id, type, version, props);

    if (strcmp(type, PW_TYPE_INTERFACE_Metadata) == 0) {
        if (pw_metadata_instance)
            return;
        // Defaults live in the "default" metadata; others (settings, route-settings...) don't matter here.
        const char *metadata_name = props ? spa_dict_lookup(props, "metadata.name") : nullptr;
        if (metadata_name && strcmp(metadata_name, "default") != 0)
            return;

        pw_metadata_instance = static_cast<pw_metadata *>(pw_registry_bind(
                pw_registry_instance, id, PW_TYPE_INTERFACE_Metadata,
                std::min(version, static_cast<uint32_t>(PW_VERSION_METADATA)), 0));
        if (!pw_metadata_instance)
            return;
        pw_metadata_add_listener(pw_metadata_instance, &pw_metadata_listener, &pipewire_metadata_events, nullptr);
        return;
    }

    if (strcmp(type, PW_TYPE_INTERFACE_Node) != 0 || !props)
        return;

    const char *media_class = spa_dict_lookup(props, PW_KEY_MEDIA_CLASS);
    const bool is_master = pipewire_class_is_master(media_class);
    const bool is_stream = pipewire_class_is_stream(media_class);
    if (!is_master && !is_stream)
        return;

    if (find_pipewire_bound_node(id))
        return;

    auto *bound = new PipeWireBoundNode();
    bound->id = id;
    bound->is_master = is_master;
    bound->node = static_cast<pw_node *>(pw_registry_bind(
            pw_registry_instance, id, PW_TYPE_INTERFACE_Node,
            std::min(version, static_cast<uint32_t>(PW_VERSION_NODE)), 0));
    if (!bound->node) {
        delete bound;
        return;
    }

    bound->client = find_or_create_pipewire_client(id, is_master);
    bound->client->pw_node_ref = bound->node;
    bound->client->pw_node_id = id;
    bound->client->is_master = is_master;
    pipewire_update_client_identity(bound->client, props);
    pw_node_add_listener(bound->node, &bound->listener, &pipewire_node_events, bound);

    pw_bound_nodes.push_back(bound);
    sort();
    if (audio_change_callback)
        audio_change_callback();
}

static void pipewire_registry_global_remove_callback(void *, uint32_t id) {
    mixer_pw_global_remove(id);
    for (size_t i = 0; i < pw_bound_nodes.size(); ++i) {
        auto *bound = pw_bound_nodes[i];
        if (bound->id != id)
            continue;

        for (size_t c = 0; c < audio_clients.size(); ++c) {
            if (audio_clients[c] == bound->client) {
                delete audio_clients[c];
                audio_clients.erase(audio_clients.begin() + static_cast<long>(c));
                break;
            }
        }

        spa_hook_remove(&bound->listener);
        if (bound->node)
            pw_proxy_destroy(reinterpret_cast<pw_proxy *>(bound->node));
        delete bound;
        pw_bound_nodes.erase(pw_bound_nodes.begin() + static_cast<long>(i));
        sort();
        if (audio_change_callback)
            audio_change_callback();
        return;
    }
}

static const pw_registry_events pipewire_registry_events = {
        PW_VERSION_REGISTRY_EVENTS,
        pipewire_registry_global_callback,
        pipewire_registry_global_remove_callback,
};

static void pipewire_core_done_callback(void *, uint32_t id, int seq) {
    if (id == PW_ID_CORE && seq == pw_sync_seq) {
        pw_initial_sync_done = true;
        pw_thread_loop_signal(pw_thread_loop_instance, false);
    }
}

static void pipewire_core_error_callback(void *, uint32_t id, int, int res, const char *) {
    // Most errors are about a single object, e.g. "unknown resource" for a stream that just went away
    // (like the volume blip) while we still had requests in flight for it. Only a broken connection is fatal.
    if (id != PW_ID_CORE || res != -EPIPE)
        return;
    ready = false;
    audio_running = false;
    pw_thread_loop_signal(pw_thread_loop_instance, false);
}

static const pw_core_events pipewire_core_events = {
        PW_VERSION_CORE_EVENTS,
        nullptr,
        pipewire_core_done_callback,
        nullptr,
        pipewire_core_error_callback,
};

static bool try_establishing_connection_with_pipewire() {
    pw_init(nullptr, nullptr);

    pw_thread_loop_instance = pw_thread_loop_new("winbar-audio-pipewire", nullptr);
    if (!pw_thread_loop_instance)
        return false;

    if (pw_thread_loop_start(pw_thread_loop_instance) != 0) {
        pw_thread_loop_destroy(pw_thread_loop_instance);
        pw_thread_loop_instance = nullptr;
        return false;
    }

    pw_thread_loop_lock(pw_thread_loop_instance);

    pw_context_instance = pw_context_new(pw_thread_loop_get_loop(pw_thread_loop_instance), nullptr, 0);
    if (!pw_context_instance) {
        pw_thread_loop_unlock(pw_thread_loop_instance);
        cleanup_pipewire_state();
        return false;
    }

    pw_core_instance = pw_context_connect(pw_context_instance, nullptr, 0);
    if (!pw_core_instance) {
        pw_thread_loop_unlock(pw_thread_loop_instance);
        cleanup_pipewire_state();
        return false;
    }

    pw_core_add_listener(pw_core_instance, &pw_core_listener, &pipewire_core_events, nullptr);

    pw_registry_instance = pw_core_get_registry(pw_core_instance, PW_VERSION_REGISTRY, 0);
    if (!pw_registry_instance) {
        pw_thread_loop_unlock(pw_thread_loop_instance);
        cleanup_pipewire_state();
        return false;
    }

    pw_registry_add_listener(pw_registry_instance, &pw_registry_listener, &pipewire_registry_events, nullptr);

    pw_initial_sync_done = false;
    pw_sync_seq = pw_core_sync(pw_core_instance, PW_ID_CORE, pw_sync_seq);

    audio_running = true;
    int wait_rounds = 0;
    while (audio_running && !pw_initial_sync_done && wait_rounds < 8) {
        pw_thread_loop_timed_wait(pw_thread_loop_instance, 1);
        wait_rounds++;
    }

    ready = audio_running && pw_initial_sync_done;
    pw_thread_loop_unlock(pw_thread_loop_instance);
    if (!ready)
        cleanup_pipewire_state();
    return ready;
}

void alsa_event_pumping_required_callback(PF *) {
    snd_mixer_handle_events(alsa_handle);
}

static int alsa_state_change_callback(snd_mixer_elem_t *elem, unsigned int) {
    if (!audio_change_callback) return 0;
    if (master_volume != elem) return 0;
    if (!audio_running) return 0;

    double volume = get_normalized_playback_volume(master_volume, SND_MIXER_SCHN_FRONT_LEFT);
    int mute;
    snd_mixer_selem_get_playback_switch(elem, snd_mixer_selem_channel_id_t::SND_MIXER_SCHN_FRONT_LEFT, &mute);

    if (!audio_clients.empty()) {
        audio_clients[0]->alsa_volume = volume;
        audio_clients[0]->mute = !((bool) mute);
    }
    audio_change_callback();
    return 0;
}

bool try_establishing_connection_with_alsa() {
    if (snd_mixer_open(&alsa_handle, 0) != 0)
        return false;

    const char *card = "default"; // can be replaced with hw:0
    if (snd_mixer_attach(alsa_handle, card) != 0) {
        snd_mixer_close(alsa_handle);
        return false;
    }
    if (snd_mixer_selem_register(alsa_handle, NULL, NULL) != 0) {
        snd_mixer_close(alsa_handle);
        return false;
    }
    if (snd_mixer_load(alsa_handle) != 0) {
        snd_mixer_close(alsa_handle);
        return false;
    }

    snd_mixer_selem_id_malloc(&master_sid);
    snd_mixer_selem_id_set_index(master_sid, 0);

    const char *selem_name = "Master";
    snd_mixer_selem_id_set_name(master_sid, selem_name);
    if ((master_volume = snd_mixer_find_selem(alsa_handle, master_sid)) == nullptr) {
        snd_mixer_selem_id_free(master_sid);
        master_sid = nullptr;
        return false;
    }

    // The reason we can't just use Master is because when you mute the master, all the children get muted, but when you
    // un-mute, they don't get un-muted.
    { // Headphone (optional)
        snd_mixer_selem_id_malloc(&headphone_sid);
        snd_mixer_selem_id_set_index(headphone_sid, 0);

        const char *name = "Headphone";
        snd_mixer_selem_id_set_name(headphone_sid, name);
        if ((headphone_volume = snd_mixer_find_selem(alsa_handle, headphone_sid)) == nullptr) {
            headphone_sid = nullptr;
            snd_mixer_selem_id_free(headphone_sid);
        }
    }
    { // Speaker Front (optional)
        snd_mixer_selem_id_malloc(&speaker_sid);
        snd_mixer_selem_id_set_index(speaker_sid, 0);

        const char *name = "Speaker Front";
        snd_mixer_selem_id_set_name(speaker_sid, name);
        if ((speaker_volume = snd_mixer_find_selem(alsa_handle, speaker_sid)) == nullptr) {
            speaker_sid = nullptr;
            snd_mixer_selem_id_free(speaker_sid);
        }
    }

    // Watch for events and pump them when required
    int i, nfds = snd_mixer_poll_descriptors_count(alsa_handle);
    struct pollfd pfds[nfds];
    if (snd_mixer_poll_descriptors(alsa_handle, pfds, nfds) < 0)
        return false;
    {
        // std::lock_guard<std::mutex> lock(app->running_mutex);
        for (i = 0; i < nfds; i++) {
            poll_descriptor(pfds[i].fd, alsa_event_pumping_required_callback, nullptr,
                            "try_establishing_connection_with_alsa");
        }
    }

    snd_mixer_elem_set_callback(master_volume, alsa_state_change_callback);

    auto c = new AudioClient();
    audio_clients.push_back(c);
    c->is_master = true;
    c->title = "Master";
    c->icon_name = "audio-card";
    c->index = snd_mixer_selem_get_index(master_volume);
    default_sink_name = "Master";

    alsa_state_change_callback(master_volume, 0);

    long min, max;
    snd_mixer_selem_get_playback_volume_range(master_volume, &min, &max);
    long current_volume;
    snd_mixer_selem_get_playback_volume(master_volume, SND_MIXER_SCHN_FRONT_LEFT, &current_volume);
    double volume_percentage = ((double) (current_volume - min)) / (double) (max - min);
    c->cached_volume = volume_percentage;
    c->alsa_volume = volume_percentage;

    alsa_mixer_scan();

    return true;
}

static std::vector<std::thread> threads;

void audio_sort() {
    sort();
}

void audio_start() {
    if (!threads.empty() && !audio_running) {
        audio_stop();
        audio_join();
    }
    auto t = std::thread([]() -> void {
        if (backend != AudioBackend::UNSET)
            return;
        if (set->prefer_pipewire_audio_backend) {
            if (try_establishing_connection_with_pipewire()) {
                backend = AudioBackend::PIPEWIRE;
                audio_thread_id = std::this_thread::get_id();
                while (audio_running) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(20));
                }
                cleanup_pipewire_state();
                delete_all_audio_clients();
                audio_running = false;
                backend = AudioBackend::UNSET;
            } else if (try_establishing_connection_with_pulseaudio()) {
                backend = AudioBackend::PULSEAUDIO;
                audio_thread_id = std::this_thread::get_id();
                while (audio_running) {
                    iterate_pulseaudio_mainloop();
                }
                mixer_clear_all();
                pa_context_disconnect(context);
                pa_context_unref(context);
                pa_mainloop_free(mainloop);
                context = nullptr;
                mainloop = nullptr;
                delete_all_audio_clients();
                audio_running = false;
                backend = AudioBackend::UNSET;
            } else if (try_establishing_connection_with_alsa()) {
                backend = AudioBackend::ALSA;
                audio_running = true;
                ready = true;
                audio_thread_id = std::this_thread::get_id();
            }
        } else {
        	if (try_establishing_connection_with_pulseaudio()) {
        	    backend = AudioBackend::PULSEAUDIO;
        	    audio_thread_id = std::this_thread::get_id();
        	    while (audio_running) {
            		iterate_pulseaudio_mainloop();
        	    }
        	    mixer_clear_all();
        	    pa_context_disconnect(context);
        	    pa_context_unref(context);
        	    pa_mainloop_free(mainloop);
        	    context = nullptr;
        	    mainloop = nullptr;
        	    delete_all_audio_clients();
                audio_running = false;
                backend = AudioBackend::UNSET;
        	} else if (try_establishing_connection_with_pipewire()) {
        	    backend = AudioBackend::PIPEWIRE;
        	    audio_thread_id = std::this_thread::get_id();
        	    while (audio_running) {
            		std::this_thread::sleep_for(std::chrono::milliseconds(20));
        	    }
        	    cleanup_pipewire_state();
        	    delete_all_audio_clients();
                audio_running = false;
                backend = AudioBackend::UNSET;
        	} else if (try_establishing_connection_with_alsa()) {
        	    backend = AudioBackend::ALSA;
        	    audio_running = true;
        	    ready = true;
        	    audio_thread_id = std::this_thread::get_id();
        	}
        }
    });
    threads.push_back(std::move(t));
}

void audio_stop() {
    //if (audio_change_callback)
        //audio_change_callback();
    ready = false;
    audio_running = false;
    if (backend == AudioBackend::PULSEAUDIO) {
        pa_mainloop_wakeup(mainloop);
    } else if (backend == AudioBackend::PIPEWIRE) {
        if (pw_thread_loop_instance)
            pw_thread_loop_signal(pw_thread_loop_instance, false);
    } else if (backend == AudioBackend::ALSA) {
        if (alsa_handle) snd_mixer_close(alsa_handle);
        if (master_sid) snd_mixer_selem_id_free(master_sid);
        if (headphone_sid) snd_mixer_selem_id_free(headphone_sid);
        if (speaker_sid) snd_mixer_selem_id_free(speaker_sid);
        alsa_handle = nullptr;
        master_sid = nullptr;
        headphone_sid = nullptr;
        speaker_sid = nullptr;
        delete_all_audio_clients();
        mixer_clear_all();
    }
    backend = AudioBackend::UNSET;
}

void audio_join() {
    for (int i = 0; i < threads.size(); ++i)
        threads[i].join();
    threads.clear();
}

void audio(const std::function<void()> &callback) {
#ifdef  TRACY_ENABLE
    ZoneScoped;
#endif
    if (!ready || !audio_running) return;

    if (backend == AudioBackend::ALSA) {
        callback();
        return;
    } else if (backend == AudioBackend::PIPEWIRE) {
        if (callback == nullptr)
            return;
        if (!pw_thread_loop_instance)
            return;
        pw_thread_loop_lock(pw_thread_loop_instance);
        callback();
        pw_thread_loop_unlock(pw_thread_loop_instance);
        return;
    } else if (backend != AudioBackend::PULSEAUDIO) {
        return;
    }

    // We handle the case where this function is called from the audio thread which would obviously lock us up.
    if (std::this_thread::get_id() == audio_thread_id) {
        // re-call the function passed in but on another thread
        std::thread temp([callback]() {
            audio(callback);
        });
        temp.detach();
        return;
    }

    while (audio_thread_busy && audio_running); // If audio thread is doing something, wait for it to finish

    if (callback == nullptr) {
        pa_mainloop_wakeup(mainloop);
        return;
    }

    needs_block = true;
    signal_met = false;
    free_to_continue = false;
    // First we grab a lock on signal_mutex because after calling 'audio_wakeup', audio thread will try to lock signal_mutex
    // It won't be able to because we have the lock. That gives us time to set up the conditional which will unlock the lock,
    // and will wait for the audio thread to notify us that it has woken up and dispatched, and we are free to use the pa_mainloop.
    {
        std::unique_lock<std::mutex> lock(signal_mutex);
        pa_mainloop_wakeup(mainloop);
        needs_signal_condition.wait_for(lock, std::chrono::milliseconds(70), []() { return signal_met; });
        callback();
    }

    // At this point the audio thread is waiting for us to notify it that it is free to continue, so we try to gain a lock
    // Set the variable to true, and notify the conditional variable in the audio thread
    {
        std::unique_lock<std::mutex> free_to_continue_lock(free_to_continue_mutex);
        free_to_continue = true;
        free_to_continue_condition.notify_all();
    }
}

void audio_play_blip() {
    const std::string path = "/usr/share/mylardesktop/audio/blip.wav";
    if (!std::filesystem::is_regular_file(path))
        return;

    std::lock_guard<std::mutex> lock(blip_mutex);
    if (blip_stop_requested)
        return;
    pending_blip_path = path;
    blip_requested = true;
    if (!blip_worker_started) {
        blip_worker_started = true;
        blip_worker_thread = std::thread(blip_playback_worker);
    }
    blip_condition.notify_one();
}

void audio_stop_blip() {
    {
        std::lock_guard<std::mutex> lock(blip_mutex);
        blip_stop_requested = true;
        blip_condition.notify_one();
    }
    if (blip_worker_thread.joinable())
        blip_worker_thread.join();
}

void audio_read(const std::function<void()> &callback) {
#ifdef  TRACY_ENABLE
    ZoneScoped;
#endif
    if (!ready || !audio_running || !callback) return;

    if (backend == AudioBackend::ALSA) {
        callback();
        return;
    } else if (backend == AudioBackend::PIPEWIRE) {
        if (!pw_thread_loop_instance)
            return;
        pw_thread_loop_lock(pw_thread_loop_instance);
        callback();
        pw_thread_loop_unlock(pw_thread_loop_instance);
        return;
    } else if (backend != AudioBackend::PULSEAUDIO) {
        return;
    }

    while (audio_thread_busy && audio_running); // If audio thread is doing something, wait for it to finish

    std::unique_lock<std::mutex> read_lock(read_mutex);

    callback();
}

static bool opened = false;

void meter_watching_start() {
    if (!set->meter_animations)
        return;
    opened = true;
#ifdef  TRACY_ENABLE
    ZoneScoped;
#endif
    if (!ready || !audio_running || backend != AudioBackend::PULSEAUDIO) return;
    for (const auto &audio_client: audio_clients) {
        if (!audio_client->stream) {
            pa_sample_spec spec;
            spec.channels = 1;
            spec.format = PA_SAMPLE_FLOAT32;
            spec.rate = 120;

            pa_buffer_attr attr;
            memset(&attr, 0, sizeof(attr));
            attr.fragsize = sizeof(float);
            attr.maxlength = (uint32_t) -1;

            audio_client->stream = pa_stream_new(context, (audio_client->title + " Peak").c_str(), &spec, NULL);

            auto flags = (pa_stream_flags_t) (10752); // exact number set by pulseaudio
            pa_stream_set_read_callback(audio_client->stream, on_stream_update, audio_client);
            pa_stream_set_suspended_callback(audio_client->stream, on_stream_suspend, audio_client);

            if (audio_client->monitor_source_name.empty() && audio_client->index != PA_INVALID_INDEX) {
                pa_stream_set_monitor_stream(audio_client->stream, audio_client->index);
                pa_stream_connect_record(audio_client->stream, NULL, &attr, flags);
            } else {
                pa_stream_connect_record(audio_client->stream, audio_client->monitor_source_name.c_str(), &attr, flags);
            }
        }
    }
}

void meter_watching_stop() {
    if (!set->meter_animations)
        return;
    if (!opened)
        return;
    opened = false;
#ifdef  TRACY_ENABLE
    ZoneScoped;
#endif
    if (!ready || !audio_running || backend != AudioBackend::PULSEAUDIO) return;
    for (const auto &audio_client: audio_clients) {
        if (audio_client->stream) {
            pa_stream_disconnect(audio_client->stream);
            pa_stream_unref(audio_client->stream);
            audio_client->stream = nullptr;
        }
    }
}

void AudioClient::set_mute(bool mute_on, bool user_initiated) {
#ifdef  TRACY_ENABLE
    ZoneScoped;
#endif
    if (user_initiated)
        user_muted = mute_on;
    else if (!mute_on && user_muted)
        return;

    if (backend == AudioBackend::PULSEAUDIO) {
        pa_operation *pa_op;
        if (this->is_master) {
            pa_op = pa_context_set_sink_mute_by_index(context, this->index, (int) mute_on, NULL, NULL);
        } else {
            pa_op = pa_context_set_sink_input_mute(context, this->index, (int) mute_on, NULL, NULL);
        }
        pa_operation_unref(pa_op);
    } else if (backend == AudioBackend::PIPEWIRE) {
        if (!pw_node_ref)
            return;
        pw_thread_loop_lock(pw_thread_loop_instance);
        this->mute = mute_on;

        float channel_volumes[SPA_AUDIO_MAX_CHANNELS];
        uint32_t n_channels = static_cast<uint32_t>(std::min<size_t>(pw_channel_volumes.size(), SPA_AUDIO_MAX_CHANNELS));
        if (n_channels == 0) {
            n_channels = 1;
            channel_volumes[0] = static_cast<float>(std::pow(this->get_volume(), 3.0));
        } else {
            for (uint32_t i = 0; i < n_channels; ++i)
                channel_volumes[i] = pw_channel_volumes[i];
        }

        uint8_t buffer[1024];
        spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
        const spa_pod *pod = reinterpret_cast<const spa_pod *>(spa_pod_builder_add_object(
                &b,
                SPA_TYPE_OBJECT_Props,
                SPA_PARAM_Props,
                SPA_PROP_mute, SPA_POD_Bool(mute_on),
                SPA_PROP_channelVolumes, SPA_POD_Array(sizeof(float), SPA_TYPE_Float, n_channels, channel_volumes)
        ));
        pw_node_set_param(pw_node_ref, SPA_PARAM_Props, 0, pod);
        pw_thread_loop_unlock(pw_thread_loop_instance);
    } else if (backend == AudioBackend::ALSA) {
        if (mute_on) { // mute
            snd_mixer_selem_set_playback_switch_all(master_volume, 0);
            if (headphone_volume)
                snd_mixer_selem_set_playback_switch_all(headphone_volume, 0);
            if (speaker_volume)
                snd_mixer_selem_set_playback_switch_all(speaker_volume, 0);
        } else {
            snd_mixer_selem_set_playback_switch_all(master_volume, 1);
            if (headphone_volume)
                snd_mixer_selem_set_playback_switch_all(headphone_volume, 1);
            if (speaker_volume)
                snd_mixer_selem_set_playback_switch_all(speaker_volume, 1);
        }
        alsa_state_change_callback(master_volume, 0);
    }
}

void AudioClient::set_volume(double value) {
#ifdef  TRACY_ENABLE
    ZoneScoped;
#endif
    if (value > 1)
        value = 1;
    if (value < 0)
        value = 0;
    if (backend == AudioBackend::PULSEAUDIO) {
        pa_cvolume copy = this->volume;
        for (int i = 0; i < this->volume.channels; i++)
            copy.values[i] = std::round(65536 * value);

        pa_operation *pa_op;
        if (this->is_master) {
            pa_op = pa_context_set_sink_volume_by_index(context, index, &copy, NULL, NULL);
        } else {
            pa_op = pa_context_set_sink_input_volume(context, index, &copy, NULL, NULL);
        }
        pa_operation_unref(pa_op);
    } else if (backend == AudioBackend::PIPEWIRE) {
        if (!pw_node_ref)
            return;
        pw_thread_loop_lock(pw_thread_loop_instance);

        // Slider position is on the cubic scale; PipeWire wants linear amplitude.
        float new_volume = static_cast<float>(value * value * value);
        float channel_volumes[SPA_AUDIO_MAX_CHANNELS];
        uint32_t n_channels = static_cast<uint32_t>(std::min<size_t>(pw_channel_volumes.size(), SPA_AUDIO_MAX_CHANNELS));
        if (n_channels == 0) {
            n_channels = 1;
        }
        for (uint32_t i = 0; i < n_channels; ++i)
            channel_volumes[i] = new_volume;

        uint8_t buffer[1024];
        spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
        const spa_pod *pod = reinterpret_cast<const spa_pod *>(spa_pod_builder_add_object(
                &b,
                SPA_TYPE_OBJECT_Props,
                SPA_PARAM_Props,
                SPA_PROP_mute, SPA_POD_Bool(mute),
                SPA_PROP_channelVolumes, SPA_POD_Array(sizeof(float), SPA_TYPE_Float, n_channels, channel_volumes)
        ));

        pw_node_set_param(pw_node_ref, SPA_PARAM_Props, 0, pod);
        update_pipewire_client_volumes(this, channel_volumes, n_channels);
        pw_thread_loop_unlock(pw_thread_loop_instance);
    } else if (backend == AudioBackend::ALSA) {
        this->alsa_volume = value;
        // will determine the bounds? or how the number is round? something like that. it doesn't matter too much from what I can tell
        auto dir = this->get_volume() == value ? 0 : this->get_volume() > value ? -1 : 1;
        set_normalized_playback_volume(master_volume, value, dir);
        alsa_state_change_callback(master_volume, 0);
    }
}

bool AudioClient::is_muted() {
    return mute;
}

double AudioClient::get_volume() {
    if (backend == AudioBackend::PULSEAUDIO) {
        return ((double) volume.values[0]) / ((double) 65535);
    } else if (backend == AudioBackend::PIPEWIRE) {
        if (!pw_channel_volumes.empty())
            return std::cbrt(std::max(0.0f, pw_channel_volumes[0]));
        return ((double) volume.values[0]) / ((double) 65535);
    } else if (backend == AudioBackend::ALSA) {
        return alsa_volume;
    }
    return 0;
}

bool AudioClient::is_master_volume() {
    return is_default_sink_client(this);
}

void audio_state_change_callback(void (*callback)()) {
    audio_change_callback = callback;
}

#include <random>

static std::string get_uuid() {
    std::random_device              rd;
    std::mt19937                    gen(rd());
    std::uniform_int_distribution<> dis(0, 15);
    std::uniform_int_distribution<> dis2(8, 11); // for variant

    std::stringstream               ss;
    ss << std::hex;
    for (int i = 0; i < 8; i++)
        ss << dis(gen);
    ss << "-";
    for (int i = 0; i < 4; i++)
        ss << dis(gen);
    ss << "-4"; // UUID version 4
    for (int i = 0; i < 3; i++)
        ss << dis(gen);
    ss << "-";
    ss << dis2(gen); // UUID variant
    for (int i = 0; i < 3; i++)
        ss << dis(gen);
    ss << "-";
    for (int i = 0; i < 12; i++)
        ss << dis(gen);
    return ss.str();
}

AudioClient::AudioClient() {
    uuid = get_uuid();
}

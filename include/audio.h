//
// Created by jmanc3 on 9/30/21.
//

#ifndef WINBAR_AUDIO_H
#define WINBAR_AUDIO_H

#include <pulse/mainloop.h>
#include <thread>
#include <pulse/pulseaudio.h>
#include <vector>
#include <string>
#include <cstdint>
#include <functional>
#include <pipewire/node.h>

extern bool audio_running;
extern bool allow_audio_thread_creation;

struct AudioClient {
    std::string uuid = "";
    int pid = -1;
    
    int index = PA_INVALID_INDEX;
    uint32_t pw_node_id = SPA_ID_INVALID;
    pw_node *pw_node_ref = nullptr;
    std::string pw_node_name;
    std::string monitor_source_name;
    std::string title;
    std::string subtitle;
    std::string icon_name;
    bool is_master = false;
    pa_cvolume volume = {0};
    bool mute = 0;
    bool user_muted = false;
    double peak = 0;
    pa_stream *stream = nullptr; // for showing uv meter
    std::vector<float> pw_channel_volumes;

    long last_time_peak_changed = 0;
    double cached_volume = 1;
    double alsa_volume = 0;

    void set_mute(bool mute_on, bool user_initiated = true);

    void set_volume(double value);
    bool is_muted();

    double get_volume();

    bool is_master_volume();

    AudioClient();
};

extern std::vector<AudioClient *> audio_clients;

void audio_start();

void audio_stop();

void audio_sort();

void audio_join();

void audio(const std::function<void()> &callback);

void audio_play_blip();

void audio_stop_blip();

void audio_read(const std::function<void()> &callback);

void meter_watching_start();

void meter_watching_stop();

// For updating UI when other programs change audio state that are not us.
void audio_state_change_callback(void (*callback)());

/*
 * Mixer model (what pavucontrol shows): output devices (sinks), input devices (sources),
 * playback streams (sink inputs), recording streams (source outputs) and cards with their
 * profiles. It is kept separately from `audio_clients` (which the dock uses) and is safe to
 * read from any thread through the snapshot functions below. Setters may be called from any
 * thread except the audio thread itself.
 *
 * Volumes are normalized so that 1.0 is 100% (PA_VOLUME_NORM). For PipeWire the linear
 * channel volumes are mapped through a cube root, which is what pavucontrol and wpctl display.
 */
enum class AudioNodeKind {
    Sink = 0,     // Output device
    Source,       // Input device (microphone, line in, monitor of a sink)
    SinkInput,    // Playback stream
    SourceOutput, // Recording stream
};

struct AudioPortInfo {
    std::string name;
    std::string description;
    bool available = true; // false when known to be unplugged
    uint32_t priority = 0;
};

struct AudioProfileInfo {
    std::string name;
    std::string description;
    bool available = true;
    uint32_t priority = 0;
    uint32_t n_sinks = 0;
    uint32_t n_sources = 0;
};

struct AudioCardInfo {
    uint32_t index = UINT32_MAX;
    std::string name;
    std::string description;
    std::string icon_name;
    std::vector<AudioProfileInfo> profiles;
    std::string active_profile;
};

struct AudioNodeInfo {
    AudioNodeKind kind = AudioNodeKind::Sink;
    uint32_t index = UINT32_MAX;
    std::string name;        // Internal name (sink/source name, stream media name)
    std::string description; // Human readable device description, or the stream's media name
    std::string app_name;    // Streams only
    std::string icon_name;
    int pid = -1;
    // Streams: index of the sink/source they are connected to. Devices: index of their card.
    uint32_t device = UINT32_MAX;
    // Sources only: index of the sink this is the monitor of.
    uint32_t monitor_of = UINT32_MAX;
    // Sinks only: index of the monitor source.
    uint32_t monitor_source = UINT32_MAX;
    std::vector<double> channel_volumes; // 1.0 == 100%
    bool mute = false;
    bool has_volume = true;
    bool corked = false;
    std::vector<AudioPortInfo> ports;
    std::string active_port;
    float peak = 0;          // 0..1, only updated while meters are running
    bool peak_supported = false;

    double volume() const;
};

std::vector<AudioNodeInfo> audio_mixer_nodes();

std::vector<AudioCardInfo> audio_mixer_cards();

std::string audio_mixer_default_sink();

std::string audio_mixer_default_source();

// Increments whenever anything in the mixer model (other than peaks) changes.
unsigned long audio_mixer_revision();

// "PulseAudio", "PipeWire", "ALSA" or "" when not connected.
std::string audio_backend_name();

void audio_mixer_set_volume(AudioNodeKind kind, uint32_t index, double volume);

void audio_mixer_set_mute(AudioNodeKind kind, uint32_t index, bool mute);

// kind must be Sink or Source.
void audio_mixer_set_default(AudioNodeKind kind, uint32_t index);

// kind must be SinkInput or SourceOutput; device is the target sink/source index.
void audio_mixer_move_stream(AudioNodeKind kind, uint32_t index, uint32_t device);

// kind must be Sink or Source.
void audio_mixer_set_port(AudioNodeKind kind, uint32_t index, const std::string &port);

void audio_mixer_set_card_profile(uint32_t card, const std::string &profile);

// Starts or stops peak metering of every node. Reference counted: every call with true must be
// matched by one with false; meters run while at least one user wants them.
void audio_mixer_meters(bool on);


#endif //WINBAR_AUDIO_H

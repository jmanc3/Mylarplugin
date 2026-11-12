#pragma once

#include "audio.h"

#include <cstddef>
#include <vector>

struct Container;
struct Dock;

inline constexpr int volume_popup_w = 330;

int volume_popup_height_for_rows(size_t row_count);
int volume_popup_height_for_clients(const std::vector<AudioClient> &clients);
std::vector<AudioClient> snapshot_audio_clients();

void fill_volume_root(std::vector<AudioClient> clients, Container *root);

// Builds the Playback / Mics tabs and pages inside dock->volume (which must already be open).
void build_volume_popup(Dock *dock);

extern float volume_level;
extern bool is_master_muted;
extern long last_time_volume_adjusted;

#include "thumbnail_capture.h"
#include "heart.h"

#include <algorithm>
#include <cmath>

namespace thumbnail_capture {

Workspaces active_workspaces() {
    Workspaces workspaces;
    for (auto m : actual_monitors) {
        const int mid = *datum<int>(m, "cid");
        workspaces[mid] = hypriso->get_active_workspace_id(mid);
    }
    return workspaces;
}

Batch::Batch(History &history, int monitor, bool initial, const Workspaces &workspaces)
    : Batch(history, hypriso->last_render_ms(monitor), 1000.0 / std::max(1.0, (double) hypriso->fps(monitor)),
            initial, preview_interval_ms, workspaces) {}

Batch::Batch(History &history, double render_ms, double frame_ms, bool initial, long interval, const Workspaces &workspaces)
    : now(get_current_time_in_ms()), history(history), initial(initial) {
    const double load = (render_ms + history.smoothed_ms) / std::max(1.0, frame_ms);
    scale = 1.0 + std::max(0.0, load - 0.35) * 4.0;
    fast_ms = scaled_interval(interval, 250);
    slow_ms = scaled_interval(400, 2000);
    budget_ms = initial ? 6.0 : std::clamp(frame_ms * 0.35 - render_ms * 0.5, 1.5, 6.0);

    std::map<int, int> counts;
    const auto stacking = get_window_stacking_order();
    for (auto it = stacking.rbegin(); it != stacking.rend(); ++it) {
        const int mid = get_monitor(*it);
        const auto ws = workspaces.find(mid);
        if (ws == workspaces.end() || counts[mid] >= 2 || is_slept(*it) || !get_cid_container(*it))
            continue;
        if (hypriso->get_active_workspace_id_client(*it) == ws->second) {
            priority.insert(*it);
            ++counts[mid];
            if (priority.size() == workspaces.size() * 2)
                break;
        }
    }
}

long Batch::scaled_interval(long base, long maximum) const {
    return std::clamp(std::lround(base * scale), base, maximum);
}

bool Batch::add_preview(int cid, bool hovered, const Bounds &bounds) {
    return add(cid, hovered, bounds, !hypriso->has_thumbnail(cid), hypriso->is_thumb_dirty(cid), true);
}

bool Batch::add(int cid, bool hovered, const Bounds &bounds, bool missing, bool dirty, bool allow_fallback, long preloaded) {
    const bool fast = hovered || priority.contains(cid);
    if (!missing) {
        auto it = history.windows.find(cid);
        const long last = it == history.windows.end() || !it->second ? preloaded : it->second;
        const long elapsed = last ? now - last : slow_ms * 4;
        if (elapsed < (fast ? fast_ms : slow_ms))
            return false;
        if (!fast && !dirty && (!allow_fallback || elapsed < slow_ms * 3))
            return false;
    }
    candidates.push_back({cid, missing ? 0 : hovered ? 1 : fast ? 2 : 3, bounds});
    return true;
}

const std::vector<Batch::Candidate> &Batch::ordered_candidates() {
    std::ranges::stable_sort(candidates, {}, &Candidate::tier);
    // Leave missing and hovered thumbnails first, then rotate the rest across ticks.
    auto first = std::ranges::find_if(candidates, [](const Candidate &c) { return c.tier > 1; });
    const auto span = candidates.end() - first;
    if (span > 1)
        std::rotate(first, first + history.cursor % span, candidates.end());
    return candidates;
}

double Batch::elapsed_ms() const {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

bool Batch::defer_capture(bool missing) {
    if (missing || !captured || elapsed_ms() < budget_ms)
        return false;
    deferred = true;
    return true;
}

void Batch::record(int cid, long timestamp) {
    history.windows[cid] = timestamp;
    ++captured;
}

long Batch::finish() {
    history.cursor += captured;
    std::erase_if(history.windows, [](const auto &entry) { return !get_cid_container(entry.first); });
    if (!initial)
        history.smoothed_ms = history.smoothed_ms * 0.75 + elapsed_ms() * 0.25;
    return deferred ? std::min(fast_ms, 50L) : fast_ms;
}

long Batch::capture_previews() {
    for (const auto &candidate : ordered_candidates()) {
        if (defer_capture(candidate.tier == 0))
            break;
        hypriso->screenshot(candidate.cid);
        record(candidate.cid, get_current_time_in_ms());
        if (!initial && candidate.bounds.w > 0 && candidate.bounds.h > 0) {
            auto damage = candidate.bounds;
            damage.grow(4.0);
            hypriso->damage_box(damage);
        }
    }
    return finish();
}

}

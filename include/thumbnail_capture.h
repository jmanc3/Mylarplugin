#pragma once

#include "container.h"

#include <chrono>
#include <map>
#include <set>

namespace thumbnail_capture {

constexpr long preview_interval_ms = 20;
using Workspaces = std::map<int, int>;
Workspaces active_workspaces();

struct History {
    std::map<int, long> windows;
    double smoothed_ms = 0;
    size_t cursor = 0;
};

class Batch {
public:
    struct Candidate {
        int cid;
        int tier;
        Bounds bounds;
    };

    Batch(History &history, int monitor, bool initial, const Workspaces &workspaces);
    Batch(History &history, double render_ms, double frame_ms, bool initial, long interval, const Workspaces &workspaces);

    long scaled_interval(long base, long maximum) const;
    bool add_preview(int cid, bool hovered, const Bounds &bounds);
    bool add(int cid, bool hovered, const Bounds &bounds, bool missing, bool dirty, bool allow_fallback, long preloaded = 0);
    const std::vector<Candidate> &ordered_candidates();
    bool defer_capture(bool missing);
    void record(int cid, long timestamp);
    long finish();
    long capture_previews();

    long now;
    long fast_ms;
    long slow_ms;

private:
    History &history;
    bool initial;
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    double scale;
    double budget_ms;
    size_t captured = 0;
    bool deferred = false;
    std::set<int> priority;
    std::vector<Candidate> candidates;
    double elapsed_ms() const;
};

}

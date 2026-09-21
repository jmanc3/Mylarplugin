#pragma once

#include <algorithm>
#include <cmath>

namespace desktop_gesture {
    constexpr float sensitivity = .42f;
    constexpr float activation_offset = 2.0f * sensitivity;
    constexpr float maximum_offset = 250.0f * sensitivity;
    constexpr float maximum_delta = 22.0f;
    constexpr float intent_offset = 12.0f * sensitivity;

    enum class Owner { None, Overview, ShowDesktop };

    struct State {
        Owner owner = Owner::None;
        float initial_progress = 0.0f;
        float y_offset = 0.0f;

        void begin(bool overview_open, bool desktop_open, float overview_progress, float desktop_progress) {
            owner = overview_open ? Owner::Overview : desktop_open ? Owner::ShowDesktop : Owner::None;
            initial_progress = std::clamp(overview_open ? overview_progress : desktop_open ? desktop_progress : 0.0f, 0.0f, 1.0f);
            y_offset = 0.0f;
        }

        void update(float delta_y) {
            y_offset = std::clamp(y_offset + std::clamp(delta_y, -maximum_delta, maximum_delta) * sensitivity,
                                  -maximum_offset, maximum_offset);
            if (owner == Owner::None && std::abs(y_offset) > activation_offset)
                owner = y_offset < 0.0f ? Owner::Overview : Owner::ShowDesktop;
        }

        float progress() const {
            const float travel = std::max(std::abs(y_offset) - activation_offset, 0.0f) / (maximum_offset - activation_offset);
            const float direction = owner == Owner::Overview ? -1.0f : 1.0f;
            return std::clamp(initial_progress + direction * std::copysign(travel, y_offset), 0.0f, 1.0f);
        }
    };

    // Follow the latest deliberate vertical stroke, ignoring small reversals.
    struct VerticalStroke {
        float offset = 0.0f;
        float origin = 0.0f;
        float extreme = 0.0f;
        int direction = 0;
        long start = 0;
        long extreme_time = 0;

        void update(float delta_y, long now) {
            if (extreme_time == 0)
                start = extreme_time = now;
            offset += std::clamp(delta_y, -maximum_delta, maximum_delta) * sensitivity;
            if (direction == 0) {
                if (std::abs(offset) < intent_offset)
                    return;
                direction = offset > 0.0f ? 1 : -1;
            } else if ((extreme - offset) * direction >= intent_offset) {
                origin = extreme;
                start = extreme_time;
                direction = -direction;
            }
            if ((offset - extreme) * direction >= 0.0f) {
                extreme = offset;
                extreme_time = now;
            }
        }

        float displacement() const {
            return direction == 0 ? 0.0f : offset - origin;
        }
    };

    struct Release {
        float target;
        float velocity;
    };

    // Positive offset opens the selected view; overview reverses its physical Y offset.
    inline Release release(long start, long end, float opening_offset, float progress) {
        const double duration = std::max(end - start, 1L) / 1000.0;
        const double velocity = std::abs(opening_offset) / (250.0 * .42) / duration;
        const bool flick = velocity >= 0.6;

        float target;
        if (flick && opening_offset > 0.0f)
            target = progress >= .01f ? 1.0f : 0.0f;
        else if (flick && opening_offset < 0.0f)
            target = progress <= .99f ? 0.0f : 1.0f;
        else
            target = progress < .5f ? 0.0f : 1.0f;

        return {target, static_cast<float>((target < progress ? -velocity : velocity) * .8)};
    }
}

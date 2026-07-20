#include "snap_preview.h"

#include "drag.h"
#include "heart.h"
#include "spring.h"
#include <iterator>
#include <sys/types.h>
#include <xkbcommon/xkbcommon-keysyms.h>

static Timer* timer = nullptr;

static float total_time = 170.0f;
static float fade_time = 300.0f;
static float pad = 10.0f;
static float circumvent_time = 220.0f;

struct Pos {
    int x;
    int y;
    long time;

    int vel_x = 0;
    int vel_y = 0;
};

static std::vector<Pos> positions;

struct SnapPreview {
    bool show = false;

    long start_drag_time = 0;
    SnapPosition start_snap_type = SnapPosition::NONE;
    SnapPosition end_snap_type = SnapPosition::NONE;
    bool start_was_snapped = 0;
    bool circumventing = false;

    Bounds start;
    Bounds current;
    Bounds previous_box; // for damage
    Bounds end;

    float rounding = 0.0;
    RGBA color = {0, 0, 0, 1};
    
    long time_since_change = 0;
    float scalar = 0.0; // 0->1
    float velx_at_change = 0;
    float vely_at_change = 0;

    SnapPosition previous_snap_type = SnapPosition::NONE;

    long keep_showing_until = 0;
    long end_drag_time = 0;

    int cid = -1;
};

static SnapPreview* preview = new SnapPreview;

enum transition_type {
    unknown,
    none_to_none,
    none_to_some,
    some_to_none,
    some_to_same_some,
    some_to_other_some,
};

void calculate_current() {
    preview->current = {
        preview->start.x + ((preview->end.x - preview->start.x) * preview->scalar),
        preview->start.y + ((preview->end.y - preview->start.y) * preview->scalar),
        preview->start.w + ((preview->end.w - preview->start.w) * preview->scalar),
        preview->start.h + ((preview->end.h - preview->start.h) * preview->scalar),
    };
}

void update_preview(Timer* t) {
    t->keep_running = true;
    long current = get_current_time_in_ms();
    float dt = (float) ((long) (current - preview->time_since_change));
    if (preview->end_drag_time != 0) {
        if ((current - preview->end_drag_time) > total_time * 1.3) {
            t->keep_running= false;
            timer = nullptr;
        }
    }
    float change = std::abs(preview->velx_at_change) / 65.0f;
    if (change > 1.0)
        change = 1.0;

    auto state = springEvaluate(dt, 0, 1, change * .03, {total_time, 1.0});

    preview->scalar = state.value;

    calculate_current();

    auto b = preview->current;
    auto p = preview->previous_box;
    b.grow(20);
    p.grow(20);
    hypriso->damage_box(b);
    hypriso->damage_box(p);
    preview->previous_box = preview->current;
}

void snap_preview::on_drag_start(int cid, int x, int y) {
    preview->cid = cid;
    preview->show = true;
    preview->end_drag_time = 0;
    preview->previous_snap_type = SnapPosition::NONE;
    preview->rounding = hypriso->get_rounding(cid);
    preview->color = hypriso->get_shadow_color(cid);
    
    if (!timer) {
        timer = later(nullptr, 1000.0f / 165.0f, update_preview);
        timer->keep_running = true;
    }
    if (auto c = get_cid_container(cid)) {
        bool is_snapped = *datum<bool>(c, "snapped");
        int snap_type = *datum<int>(c, "snap_type");
        preview->circumventing = is_snapped;
        
        preview->start_drag_time = get_current_time_in_ms();
        preview->start_was_snapped = is_snapped;
        preview->start_snap_type = (SnapPosition) snap_type;
        
        snap_preview::on_drag(cid, x, y);
    }
}

Bounds bounds_client_fixed_for_titlebar(int cid) {
    auto b = bounds_client(cid);
    if (hypriso->has_decorations(cid)) {
        b.y -= titlebar_h;
        b.h += titlebar_h;
    }
    return b;
}

void snap_preview::on_drag(int cid, int x, int y) {
    preview->cid = cid;
    auto monitor_cursor_on = hypriso->monitor_from_cursor();
    auto pos = mouse_to_snap_position(monitor_cursor_on, x, y);
    //defer();
    long current = get_current_time_in_ms();
    auto client_bounds = bounds_client_fixed_for_titlebar(cid);

    preview->show = true;
    if (pos != preview->previous_snap_type)  {
        // fixes snap preview opening when dragging titlebar from a region that would otherwise start a preview
        if (preview->circumventing && preview->start_was_snapped && ((current - preview->start_drag_time) < circumvent_time)) {
            preview->show = false;
            return;
        }
        preview->circumventing = false;

        
        preview->time_since_change = current;
        preview->scalar = 0;
        preview->velx_at_change = 0;
        preview->vely_at_change = 0;
        if (positions.size() > 6) {
            for (int i = 0; i < 6; i++) {
                float scalar = ((float ) (6.0f - i)) / 6.0f;
                preview->velx_at_change += positions[positions.size() - 1 - i].vel_x * scalar; 
                preview->vely_at_change += positions[positions.size() - 1 - i].vel_y * scalar;                 
            }
        }

        if (pos == SnapPosition::NONE) {
            preview->start = preview->current;
            preview->end = client_bounds;
            preview->velx_at_change = 50.0f;
            preview->vely_at_change = 50.0f;
        } else {
            if (preview->previous_snap_type == SnapPosition::NONE) {
                preview->start = client_bounds;
                preview->end = snap_position_to_bounds(monitor_cursor_on, pos);
            } else {
                preview->start = preview->current;
                preview->end = snap_position_to_bounds(monitor_cursor_on, pos);
            }
        }
        {
            auto mbounds = bounds_monitor(monitor_cursor_on);
            if (preview->start.x < mbounds.x) {
                preview->start.w -= mbounds.x - preview->start.x + pad;
                preview->start.x = mbounds.x;
            }
            if (preview->start.y < mbounds.y) {
                preview->start.h -= mbounds.y - preview->start.y + pad;
                preview->start.y = mbounds.y;
            }
            if (preview->start.x + preview->start.w > mbounds.x + mbounds.w) {
                preview->start.w -= ((preview->start.x + preview->start.w) - (mbounds.x + mbounds.w));
            }
            if (preview->start.y + preview->start.h > mbounds.y + mbounds.h) {
                preview->start.h -= ((preview->start.y + preview->start.h) - (mbounds.y + mbounds.h));
            }
        }
        preview->end.shrink(pad); 
    }

    if (pos == SnapPosition::NONE) {
        preview->end = client_bounds;
        preview->end.shrink(3);
    }

    calculate_current();

    preview->previous_snap_type = pos;
}

void snap_preview::on_drag_end(int cid, int x, int y, int snap_type) {
    preview->show = false;
    preview->keep_showing_until = get_current_time_in_ms() + 300;
    preview->end_drag_time = get_current_time_in_ms();
    preview->end_snap_type = (SnapPosition) snap_type;
    if ((preview->scalar < .7 && preview->previous_snap_type == SnapPosition::MAX) || 
        (preview->scalar < .6 && preview->previous_snap_type != SnapPosition::MAX)) {
        preview->start = snap_position_to_bounds(hypriso->monitor_from_cursor(), preview->previous_snap_type);
        preview->end = snap_position_to_bounds(hypriso->monitor_from_cursor(), preview->previous_snap_type);
    } else {
        preview->start = preview->current;
        preview->end = snap_position_to_bounds(hypriso->monitor_from_cursor(), preview->previous_snap_type);
    }
    
    bool snapped = snap_type != (int)SnapPosition::NONE;
    if (preview->circumventing && snapped) {
        //preview->end = snap_position_to_bounds(hypriso->monitor_from_cursor(), (SnapPosition) snap_type);
        //preview->start = snap_position_to_bounds(hypriso->monitor_from_cursor(), (SnapPosition) snap_type);
        //preview->scalar = 1.0;
        //calculate_current();
    }
    
    preview->time_since_change = get_current_time_in_ms();
}

#undef	MAX
#define MAX(a, b)  (((a) > (b)) ? (a) : (b))

#undef	MIN
#define MIN(a, b)  (((a) < (b)) ? (a) : (b))

bool overlaps(double ax, double ay, double aw, double ah,
              double bx, double by, double bw, double bh) {
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif
    if (ax > (bx + bw) || bx > (ax + aw))
        return false;
    return !(ay > (by + bh) || by > (ay + ah));
}

double calculate_overlap_percentage(double ax, double ay, double aw, double ah,
                                    double bx, double by, double bw, double bh) {
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif
    double result = 0.0;
    //trivial cases
    if (!overlaps(ax, ay, aw, ah, bx, by, bw, bh)) return 0.0;
    if (ax == bx && ay == by && aw == bw && ah == bh) return 100.0;
    
    //# overlap between A and B
    double SA = aw * ah;
    double SB = bw * bh;
    double SI = MAX(0, MIN(ax + aw, bx + bw) - MAX(ax, bx)) *
                MAX(0, MIN(ay + ah, by + bh) - MAX(ay, by));
    double SU = SA + SB - SI;
    result = SI / SU; //ratio
    result *= 100.0; //percentage
    return result;
}


// c is a ::CLIENT
void snap_preview::draw(Container* actual_root, Container* c) {
    auto root = get_rendering_root();
    if (!root)
        return;
    auto [rid, s, stage, active_id] = roots_info(actual_root, root);
    auto cid = *datum<int>(c, "cid");
    /*if (!(active_id == cid && stage == (int)STAGE::RENDER_POST_WINDOW)) {
        if (cid == drag::drag_window()) {
            renderfix
            render_drop_shadow(rid, 1.0, {0, 0, 0, 1}, 10, 2.0f, {100, 100, 300, 300});
        }
    }*/

    if (!(active_id == cid && stage == (int)STAGE::RENDER_PRE_WINDOW))
        return;
    if (cid != preview->cid)
        return;
    renderfix
    
    auto current = get_current_time_in_ms();
    if (preview->show || (current < preview->keep_showing_until)) {
        float fade_scalar = ((float)(current - preview->end_drag_time)) / fade_time;
        if (fade_scalar > 1.0)
            fade_scalar = 1.0;
        if (preview->end_drag_time == 0)
            fade_scalar = 0.0;
        float fade_amount = 1.0 - fade_scalar;
        if (preview->end_drag_time != 0) {
            if (preview->previous_snap_type == SnapPosition::NONE) {
                fade_amount = 1.0f;
                auto client_bounds = bounds_client_fixed_for_titlebar(preview->cid);
                preview->end = client_bounds;
                calculate_current();
            }
        }
        Bounds b = preview->current;
        if (preview->circumventing && preview->end_drag_time != 0 && preview->end_snap_type != SnapPosition::NONE) {
            fade_amount = 0.0;
            b = snap_position_to_bounds(hypriso->monitor_from_cursor(), preview->end_snap_type);
        }
        auto cb = bounds_client_fixed_for_titlebar(preview->cid);
        auto r = calculate_overlap_percentage(b.x, b.y, b.w, b.h, cb.x, cb.y, cb.w, cb.h);
        if (r < 93) {
            b.x -= root->real_bounds.x;
            b.y -= root->real_bounds.y;
            b.scale(s);
            b.round();
            if (preview->previous_snap_type != SnapPosition::NONE || (preview->previous_snap_type == SnapPosition::NONE && preview->scalar < .8)) {
                render_drop_shadow(rid, 1.0, {0, 0, 0, .05f * fade_amount}, preview->rounding * fade_amount, 2.0f, b);
            }
            rect(b, {.98, .98, .98, .30f}, 0, std::round(preview->rounding * s * fade_amount), 2.0f, true, 1.0);
            b.shrink(std::round(1.0f * s));
            border(b, {1.0, 1.0, 1.0, 0.1f}, std::round(1.0f * s), 0, std::round(preview->rounding * s * fade_amount), 2.0f, false, 1.0);
        }
    }
}

Pos velocity_last(const std::vector<Pos>& s) {
    if (s.size() < 2)
        return {0, 0};
    auto& a = s[s.size() - 2];
    auto& b = s[s.size() - 1];
    Pos p;
    p.x = (b.x - a.x) / ((double)(b.time - a.time));
    p.y = (b.y - a.y) / ((double)(b.time - a.time));
    return p;
}

void snap_preview::on_mouse_move(int x, int y) {
    Pos pos;
    pos.x = x;
    pos.y = y;
    pos.time = get_current_time_in_ms();

    positions.push_back(pos);

    if (positions.size() > 50)
        positions.erase(positions.begin()); // remove oldest

    if (positions.size() > 2) {
        auto p = velocity_last(positions);
        positions[positions.size() - 1].vel_x = p.x;
        positions[positions.size() - 1].vel_y = p.y;
    }
}

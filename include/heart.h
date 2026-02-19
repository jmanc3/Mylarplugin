#ifndef heart_h_INCLUDED
#define heart_h_INCLUDED

#include "container.h"
#include "hypriso.h"
#include "defer.h"

#include <any>
#include <assert.h>
#include <vector>

#ifdef TRACY_ENABLE
#include "tracy/Tracy.hpp"
#endif

#define paint [](Container *root, Container *c)
#define fz std::format
#define nz notify

#define center_y(c, in_h) std::round(c->real_bounds.y + c->real_bounds.h * .5) - std::ceil(in_h * .5)
#define center_x(c, in_w) c->real_bounds.x + c->real_bounds.w * .5 - in_w * .5

#define clip(clipbounds, s) bool _before = hypriso->clip; \
    Bounds _beforebox = hypriso->clipbox; \
    hypriso->clip = true; \
    auto _parentbox = clipbounds; \
    _parentbox.scale(s); \
    hypriso->clipbox = _parentbox; \
    defer(hypriso->clipbox = _beforebox); \
    defer(hypriso->clip = _before);

static Bounds to_parent(Container *rendering_root, Container *c) {
    auto pb = c->parent->real_bounds;
    pb.x -= rendering_root->real_bounds.x;
    pb.y -= rendering_root->real_bounds.y; 
    return pb; 
};

struct ClientInfo : UserData {
    std::vector<int> grouped_with;
};

void clear_snap_groups(int id);

static void render_fix(Container *root, Container *c);

// We need to make unscaled coords, into the fully scaled coord via monitor scale
#define renderfix auto c_backup = c->real_bounds; \
                  render_fix(root, c); \
                  defer(c->real_bounds = c_backup);

static std::string to_lower(const std::string& str) {
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif
    std::string result;
    result.reserve(str.size()); // avoid reallocations

    std::transform(str.begin(), str.end(), std::back_inserter(result), [](unsigned char c) { return std::tolower(c); });
    return result;
}

static bool enough_time_since_last_check(long reattempt_timeout, long last_time_checked) {
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif
    
    return (get_current_time_in_ms() - last_time_checked) > reattempt_timeout;
}

enum struct TYPE : uint8_t {
    NONE = 0,
    RESIZE_HANDLE, // The handle that exists between two snapped winodws
    CLIENT_RESIZE, // The resize that exists around a window
    CLIENT, // Windows
    LAYER, // Layer: dock, lockscreens, notifications
    ALT_TAB,
    SNAP_HELPER,
    SNAP_THUMB,
    WORKSPACE_SWITCHER,
    WORKSPACE_THUMB,
    TEST,
    OUR_POPUP,
    POPUP,
    OVERVIEW,
    WORKSPACE_CHANGE_INDICATOR,
};

extern std::vector<Container *> actual_monitors;
extern Container *actual_root;

struct Datas {
    std::unordered_map<std::string, std::any> datas;
};
extern std::unordered_map<std::string, Datas> datas;

template<typename T>
static T *get_data(const std::string& uuid, const std::string& name) {
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif

    // Locate uuid
    auto it_uuid = datas.find(uuid);
    if (it_uuid == datas.end())
        return nullptr;

    // Locate name
    auto it_name = it_uuid->second.datas.find(name);
    if (it_name == it_uuid->second.datas.end())
        return nullptr;

    // Attempt safe cast
    if (auto ptr = std::any_cast<T>(&it_name->second))
        return ptr;

    return nullptr; // type mismatch
}

template<typename T>
static void set_data(const std::string& uuid, const std::string& name, T&& value) {
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif
     datas[uuid].datas[name] = std::forward<T>(value);
}

template<typename T>
static T *get_or_create(const std::string& uuid, const std::string& name) {
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif
     T *data = get_data<T>(uuid, name);
    if (!data) {
        set_data<T>(uuid, name, T());
        data = get_data<T>(uuid, name);
    }
    return data;
}

static void remove_data(const std::string& uuid) {
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif
     datas.erase(uuid);
}

class FunctionTimer {
public:
    FunctionTimer(const std::string& name) : name_(name), start_time_(std::chrono::high_resolution_clock::now()) {}

    ~FunctionTimer() {
        auto end_time = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time_);
        notify(std::to_string(duration.count()));
        //std::cout << name_ << ": " << duration.count() << " ms" << std::endl;
    }

private:
    std::string name_;
    std::chrono::high_resolution_clock::time_point start_time_;
};

/*
template<typename T, typename C, typename N>
auto datum(C&& container, N&& needle) {
    //FunctionTimer timer("datum"); // Timer starts here
    //assert(container && "passed nullptr container to datum");
    auto a = get_or_create<T>(std::forward<C>(container)->uuid, std::forward<N>(needle));
    return a;
}
*/

struct SD {
    std::string needle;
    void *data; 
};

struct DD {
    std::string name;
    std::vector<SD *> sds;
};

static std::vector<DD *> dds;

template<typename T, typename C, typename N>
auto datum(C&& container, N&& needle) {
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif
     //FunctionTimer timer("datum"); // Timer starts here
    //assert(container && "passed nullptr container to datum");
    auto a = get_or_create<T>(std::forward<C>(container)->uuid, std::forward<N>(needle));
    //dds->push_back();
    return a;
}

static std::tuple<int, float, int, int> roots_info(Container *actual_root, Container *rendering_root) {
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif
    auto rid = *datum<int>(rendering_root, "cid");
    auto s = scale(rid);
    auto stage = *datum<int>(actual_root, "stage");
    auto active_id = *datum<int>(actual_root, "active_id");
    return {rid, s, stage, active_id};
}

static std::tuple<int, float, int, int> from_root(Container *c) {
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif
    int rid = current_rendering_monitor(); 
    float s = scale(rid);
    int stage = 0; 
    int active_id = 0; 
    for (auto m : actual_monitors) {
        auto mrid = *datum<int>(m, "cid");
        if (mrid == rid) {
            stage = *datum<int>(m, "stage");
            active_id = *datum<int>(m, "active_id");
            break;
        }
    }
    
    return {rid, s, stage, active_id};
}

static Container *first_above_of(Container *c, TYPE type) {
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif
    Container *client_above = nullptr; 
    Container *current = c;
    while (current->parent != nullptr) {
        if (current->parent->custom_type == (int) type) {
            return current->parent;
        }
        current = current->parent;
    }
    //assert(client_above && fz("Did not find container of type {} above, probably logic bug introduced", (int) type).c_str());
    return nullptr; 
}

static Container *get_cid_container(int id) {
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif
    for (auto child : actual_root->children) {
        if (child->custom_type == (int) TYPE::CLIENT) {
            if (*datum<int>(child, "cid") == id) {
                return child;
            }
        }
    }
    return nullptr;
}


static void paint_debug(Container *root, Container *c) {
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif
     border(c->real_bounds, {1, 0, 1, 1}, 4);
}

void damage_all();

static void request_damage(Container *root, Container *c) {
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif
    auto [rid, s, stage, active_id] = from_root(root);
    auto b = c->real_bounds;
    //b.scale(s);
    //b.x += root->real_bounds.x;
    //b.y += root->real_bounds.y;
    //b.scale(1.0 / s);
    b.grow(1);
    hypriso->damage_box(b);
}

void consume_event(Container *root, Container *c);

static void render_fix(Container *root, Container *c) {
    // Containers are in raw unscaled coordinates, but rendering needs to be per monitor based
    // So first, if the monitor is positioned at 1000 1000 and this container is at 1100 and 1500, we want it's new coordinates to be 100 500
    auto rendering_monitor = current_rendering_monitor();
    auto root_real_bounds = bounds_monitor(rendering_monitor);

    c->real_bounds.x -= root_real_bounds.x;
    c->real_bounds.y -= root_real_bounds.y;
    
    // Then we also need to scale our raw unscaled coordianates, to scaled ones (scaled by the current scale set by the monitor)
    c->real_bounds.scale(scale(rendering_monitor));
    c->real_bounds.round();
}

struct WindowRestoreLocation {
    Bounds box; // all values are 0-1 and supposed to be scaled to monitor

    bool keep_above = false;
    bool fake_fullscreen = false;
    bool remove_titlebar = false;
    
    bool remember_size = false;
    bool remember_workspace = false;
    int remembered_workspace = -1;

    Bounds actual_size_on_monitor(Bounds m) {
        Bounds b = {box.x * m.w, box.y * m.h, box.w * m.w, box.h * m.h};
        if (b.w < 5)
            b.w = 5;
        if (b.h < 5)
            b.h = 5;
        return b;
    }
};

extern std::unordered_map<std::string, WindowRestoreLocation> restore_infos;

void log(const std::string& msg);

Container *get_rendering_root();

SnapPosition opposite_snap_position(SnapPosition pos);
Bounds snap_position_to_bounds(int mon, SnapPosition pos);
SnapPosition mouse_to_snap_position(int mon, int x, int y);
bool double_clicked(Container *c, std::string needle);
void consume_everything(Container *c);
void update_restore_info_for(int id);
void launch_command(std::string command);
void add_to_snap_group(int id, int other, const std::vector<int> &grouped);
bool groupable(SnapPosition position, const std::vector<int> ids);
bool groupable_types(SnapPosition a, SnapPosition b);

static void set_argb(cairo_t *cr, RGBA color) {
    cairo_set_source_rgba(cr, color.r, color.g, color.b, color.a);
}

static void set_rect(cairo_t *cr, Bounds bounds) {
    cairo_rectangle(cr, bounds.x, bounds.y, bounds.w, bounds.h);
}

struct SnapLimits {
    float left_middle = .5f;
    float right_middle = .5f;
    float middle_middle = .5f;
};

SnapLimits get_snap_limits(int monitor);
Bounds snap_position_to_bounds_limited(int mon, SnapPosition pos, SnapLimits limits);

namespace heart {    
    void begin();
    void end();
    void layout_containers();
}

#endif // heart_h_INCLUDED


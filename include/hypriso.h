#ifndef hypriso_h_INCLUDED
#define hypriso_h_INCLUDED

#include <hyprland/src/SharedDefs.hpp>

#include "container.h"
#include <ranges>
#include <string>
#include <vector>
#include <chrono>
#include <functional>
#include <regex>

#ifdef TRACY_ENABLE
#include "tracy/Tracy.hpp"
#endif

static int titlebar_h = 28;
//static std::string mylar_font = "Noto Sans";
//static std::string mylar_font = "SF Pro Rounded";
static std::string mylar_font = "Segoe UI Variable";
static long minimize_anim_time = 100;

struct SurfacePassInfo {
    double pos_x;
    double pos_y;
    double local_pos_x;
    double local_pos_y;
    double w;
    double h;
    double cbx;
    double cby;
    double cbw;
    double cbh;
};

enum struct STAGE : uint8_t {
    RENDER_PRE = eRenderStage::RENDER_PRE,        /* Before binding the gl context */
    RENDER_BEGIN = eRenderStage::RENDER_BEGIN,          /* Just when the rendering begins, nothing has been rendered yet. Damage, current render data in opengl valid. */
    RENDER_POST_WALLPAPER = eRenderStage::RENDER_POST_WALLPAPER, /* After background layer, but before bottom and overlay layers */
    RENDER_PRE_WINDOWS = eRenderStage::RENDER_PRE_WINDOWS,    /* Pre windows, post bottom and overlay layers */
    RENDER_POST_WINDOWS = eRenderStage::RENDER_POST_WINDOWS,   /* Post windows, pre top/overlay layers, etc */
    RENDER_LAST_MOMENT = eRenderStage::RENDER_LAST_MOMENT,    /* Last moment to render with the gl context */
    RENDER_POST = eRenderStage::RENDER_POST,           /* After rendering is finished, gl context not available anymore */
    RENDER_POST_MIRROR = eRenderStage::RENDER_POST_MIRROR,    /* After rendering a mirror */
    RENDER_PRE_WINDOW = eRenderStage::RENDER_PRE_WINDOW,     /* Before rendering a window (any pass) Note some windows (e.g. tiled) may have 2 passes (main & popup) */
    RENDER_POST_WINDOW = eRenderStage::RENDER_POST_WINDOW,    /* After rendering a window (any pass) */
};

enum class SnapPosition {
  NONE,
  MAX,
  LEFT,
  RIGHT,
  TOP_LEFT,
  TOP_RIGHT,
  BOTTOM_RIGHT,
  BOTTOM_LEFT
};

enum class RESIZE_TYPE {
  NONE,
  TOP,
  RIGHT,
  BOTTOM,
  LEFT,
  TOP_RIGHT,
  TOP_LEFT,
  BOTTOM_LEFT,
  BOTTOM_RIGHT,
};

static bool parse_hex(std::string hex, double *a, double *r, double *g, double *b) {
    while (hex[0] == '#') { // remove leading pound sign
        hex.erase(0, 1);
    }
    std::regex pattern("([0-9a-fA-F]{2})([0-9a-fA-F]{2})([0-9a-fA-F]{2})([0-9a-fA-F]{2})");
    
    std::smatch match;
    if (std::regex_match(hex, match, pattern)) {
        double t_a = std::stoul(match[4].str(), nullptr, 16);
        double t_r = std::stoul(match[1].str(), nullptr, 16);
        double t_g = std::stoul(match[2].str(), nullptr, 16);
        double t_b = std::stoul(match[3].str(), nullptr, 16);
        
        *a = t_a / 255;
        *r = t_r / 255;
        *g = t_g / 255;
        *b = t_b / 255;
        return true;
    }
    
    return false;
}

struct RGBA {
    double r, g, b, a;
    
    RGBA(float r, float g, float b, float a) : r(r), g(g), b(b), a(a) {
        
    }

    RGBA(std::string hex) {
        parse_hex(hex, &this->a, &this->r, &this->g, &this->b);
    }

    RGBA () {
        
    }

    bool operator==(const RGBA& other) const {
        constexpr double eps = 1e-9;
        return std::fabs(r - other.r) < eps &&
               std::fabs(g - other.g) < eps &&
               std::fabs(b - other.b) < eps &&
               std::fabs(a - other.a) < eps;
    }

    bool operator!=(const RGBA& other) const {
        return !(*this == other);
    }
};

struct Timer {
    wl_event_source *source = nullptr;
    std::function<void(Timer *)> func = nullptr;
    void *data = nullptr;
    bool keep_running = false;
    float delay = 0;
};

struct TextureInfo {
    int id = -1;
    int w = 0;
    int h = 0;

    std::string cached_text;
    RGBA cached_color;
    
    long last_reattempt_time = 0;
    int reattempts_count = 0;
    int cached_h = 0;
};

struct ThinClient {
    int id; // unique id which maps to a hyprland window

    int initial_x = 0; // before drag start
    int initial_y = 0; // before drag start

    bool snapped = false;
    SnapPosition snap_type = SnapPosition::NONE;
    Bounds pre_snap_bounds;
    float drag_initial_mouse_percentage = 0;

    bool resizing = false;
    int resize_type = 0;
    Bounds initial_win_box;

    std::string uuid;

    ThinClient(int _id) : id(_id) {}
};

struct ThinMonitor {
    int id; // unique id which maps to a hyprland monitor

    ThinMonitor(int _id) : id(_id) {}
};

struct HyprIso {
    bool no_render = false;

    bool dragging = false;
    int dragging_id = -1;
    long drag_stop_time = 0;
    Bounds drag_initial_mouse_pos;
    Bounds drag_initial_window_pos;

    std::string last_cursor_set = "";
    
    bool resizing = false;
    int resizing_id = false;

    bool whitelist_on = false;
    std::vector<int> render_whitelist;

    float get_varfloat(std::string target, float default_float = 1.0);    
    RGBA get_varcolor(std::string target, RGBA default_color = {1.0, 0.0, 1.0, 1.0});

    int get_varint(std::string target, int default_int = 0);

    Bounds getTexBox(int id);

    void create_config_variables();
    void overwrite_animation_speed(float speed);    
    
    // The main workhorse of the program which pumps events from hyprland to mylar
    void create_hooks();
    void create_callbacks();

    uint32_t keycode_to_keysym(int keycode);

    // So things can be cleaned
    void end(); 
    
    std::function<void(int id)> on_workspace_change = nullptr;
    
    std::function<bool(int id, float x, float y)> on_mouse_move = nullptr;

    std::function<bool(int id, int button, int state, float x, float y)> on_mouse_press = nullptr;

    std::function<bool(int id, int source, int axis, int direction, double delta, int discrete, bool mouse)> on_scrolled = nullptr;

    std::function<bool(int id, int key, int state, bool update_mods)> on_key_press = nullptr;
    
    std::function<void(int id)> on_window_open = nullptr;
    
    std::function<void(int id)> on_window_closed = nullptr;
    
    std::function<void(int id)> on_title_change = nullptr;

    std::function<void()> on_layer_change = nullptr;

    std::function<void(int id)> on_monitor_open = nullptr;
    
    std::function<void(int id)> on_monitor_closed = nullptr;

    std::function<void(int id)> on_layer_open = nullptr;
    
    std::function<void(int id)> on_layer_closed = nullptr;
    
    std::function<void(int id)> on_popup_open = nullptr;
    
    std::function<void(int id)> on_popup_closed = nullptr;
     
    std::function<void(int id)> on_activated = nullptr;
    
    std::function<void(std::string name, int monitor, int w, float a)> on_draw_decos = nullptr;
    
    std::function<void(int id, int stage)> on_render = nullptr;

    std::function<bool(int id)> is_snapped = nullptr;

    std::function<void(int id)> on_drag_start_requested = nullptr;
    std::function<void(int id, RESIZE_TYPE type)> on_resize_start_requested = nullptr;
    std::function<void()> on_drag_or_resize_cancel_requested = nullptr;

    std::function<void()> on_config_reload = nullptr;
    void reload();

    std::function<void(int cid, int want)> on_requests_max_or_min = nullptr;

    void add_hyprctl_dispatcher(std::string command, std::function<bool(std::string)> func);

    //std::vector<ThinClient *> windows;
    //std::vector<ThinMonitor *> monitors;

    bool wants_titlebar(int id);
    void reserve_titlebar(int id, int size);

    float get_rounding(int id);
    RGBA get_shadow_color(int id);

    int get_pid(int client);

    std::string class_name(int id);
    std::string title_name(int id);
    std::string monitor_name(int id);
    
    void set_corner_rendering_mask_for_window(int id, int mask);
    
    void move(int id, int x, int y);
    void move_resize(int id, int x, int y, int w, int h, bool instant = true);
    void move_resize(int id, Bounds b, bool instant = true);
    
    int monitor_from_cursor();
    
    bool requested_client_side_decorations(int cid);

    void send_key(uint32_t key);

    bool being_animated(int cid);

    Bounds floating_offset(int id);
    Bounds workspace_offset(int id);

    Bounds min_size(int id);
    bool is_x11(int id);
    bool is_fullscreen(int id);
    bool is_opaque(int id);
    bool has_decorations(int id);
    void remove_decorations(int id);
    
    void bring_to_front(int id, bool focus = true);
    void set_hidden(int id, bool state, bool animate_to_dock = false);
    
    bool has_focus(int client);
    void all_lose_focus();
    void all_gain_focus();

    bool is_mapped(int id);
    bool is_hidden(int id);
    bool resizable(int id);
    
    void set_float_state(int id, bool should_float);

    bool alt_tabbable(int id);

    void should_round(int id, bool state);

    void damage_entire(int monitor);
    void damage_box(Bounds b);

    void screenshot_all();
    void screenshot(int id);
    void screenshot_deco(int id);
    void screenshot_space(int mon, int id);
    void screenshot_wallpaper(int mon);

    Bounds thumbnail_size(int id);

    void draw_thumbnail(int id, Bounds b, int rounding = 0, float roundingPower = 2.0f, int cornermask = 0, float alpha = 1.0);
    void draw_deco_thumbnail(int id, Bounds b, int rounding = 0, float roundingPower = 2.0f, int cornermask = 0);
    void draw_raw_deco_thumbnail(int id, Bounds b, int rounding = 0, float roundingPower = 2.0f, int cornermask = 0);
    void draw_raw_min_thumbnail(int id, Bounds b, float scalar);
    void draw_workspace(int mon, int id, Bounds b, int rounding = 0);
    void draw_wallpaper(int mon, Bounds b, int rounding = 0, float alpha = 1.0);

    void send_false_position(int x, int y);
    void send_false_click();

    void set_zoom_factor(float amount, bool instant = false);
    int parent(int id);

    void logout();

    void show_desktop();
    void hide_desktop();
    void move_to_workspace(int id, int workspace);
    void move_to_workspace(int workspace);
    void move_to_workspace_id(int workspace_id);

    bool is_pinned(int id);
    void pin(int id, bool state);
    
    bool is_fake_fullscreen(int id);
    void fake_fullscreen(int id, bool state);

    SurfacePassInfo pass_info(int cid);

    std::vector<int> get_workspace_ids(int monitor);
    std::vector<int> get_workspaces(int monitor);
    
    int get_active_workspace(int monitor);
    int get_active_workspace_id(int monitor);
    int get_active_workspace_id_client(int client);
    int get_workspace(int client);

    float zoom_progress(int monitor);

    bool is_space_tiling(int space);
    void set_space_tiling(int space, bool state);

    void add_float_rule();
    void overwrite_defaults();

    void simulateMouseMovement();
    bool has_popup_at(int cid, Bounds b);

    void login_animation();
    
    void do_default_drag(int cid);
    void do_default_resize(int cid);
    bool is_floating(int cid);

    bool clip = false;
    Bounds clipbox;

    void generate_mylar_hyprland_config();
};

extern HyprIso *hypriso;

void rect(Bounds box, RGBA color, int conrnermask = 0, float round = 0.0, float roundingPower = 2.0, bool blur = false, float blurA = 1.0);
void border(Bounds box, RGBA color, float size, int cornermask = 0, float round = 0.0, float roundingPower = 2.0, bool blur = false, float blurA = 1.0);
void shadow(Bounds box, RGBA color, float rounding, float roundingPower, float size);
void render_drop_shadow(int mon, float const& a, RGBA m_realShadowColor, float ROUNDINGBASE, float ROUNDINGPOWER, Bounds fullBox, float size = 0);
void testDraw();

static long get_current_time_in_ms() {
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif
     using namespace std::chrono;
    milliseconds currentTime = duration_cast<milliseconds>(system_clock::now().time_since_epoch());
    return currentTime.count();
}

// ThinClient props
Bounds bounds(ThinClient *w);
Bounds real_bounds(ThinClient *w);
Bounds bounds_full(ThinClient *w);
std::string class_name(ThinClient *w);
std::string title_name(ThinClient *w);

// ThinMonitor props
Bounds bounds(ThinMonitor *m);
Bounds bounds_reserved(ThinMonitor *m);

Bounds bounds_monitor(int id);
Bounds bounds_reserved_monitor(int id);

Bounds bounds_client(int id);
Bounds bounds_client_final(int id);
Bounds bounds_layer(int id);
Bounds real_bounds_client(int id);
Bounds bounds_full_client(int id);

int current_rendering_monitor();
int current_rendering_window();

float scale(int id);

std::vector<int> get_window_stacking_order();

void notify(std::string text);

void set_window_corner_mask(int id, int cornermask);

void free_text_texture(int id);
TextureInfo gen_text_texture(std::string font, std::string text, float h, RGBA color);
TextureInfo gen_texture(std::string path, float h);

void draw_texture(TextureInfo info, int x, int y, float a = 1.0, float clip_w = 0.0);

void setCursorImageUntilUnset(std::string cursor);
void unsetCursorImage(bool force = false);

int get_monitor(int client);

void close_window(int id);

Bounds mouse();

Timer* later(void* data, float time_ms, const std::function<void(Timer*)>& fn);

Timer* later(float time_ms, const std::function<void(Timer*)>& fn);

Timer* later_immediate(const std::function<void(Timer*)>& fn);

void request_refresh();
void request_refresh_only();

void main_thread(std::function<void()> func);

void load_icon_full_path(cairo_surface_t** surface, std::string path, int target_size);

Bounds lerp(Bounds start, Bounds end, float scalar);

float pull(std::vector<float>& fls, float scalar);

void animate(float *value, float target, float time_ms, std::shared_ptr<bool> lifetime, std::function<void(bool)> on_completion = nullptr, std::function<float(float)> lerp_func = nullptr);
bool is_being_animating(float *value);
bool is_being_animating_to(float *value, float target);
 
#endif // hypriso_h_INCLUDED

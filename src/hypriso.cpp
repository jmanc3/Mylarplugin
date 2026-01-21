/*
 *  The hyprland isolation file.
 *
 *  This file is the sole place where we anything hyprland specific is allowed to be included.
 *
 *  The purpose is to minimize our interaction surface so that our program stays as functional as possible on new updates, and we only need to fix up this file for new versions. 
 */

#include "hypriso.h"
#include "heart.h"

#include <cairo.h>
#include <fstream>
#include <glib.h>
#include <iostream>
#include <bits/types/idtype_t.h>
#include <cmath>
#include <cstring>
#include <drm_fourcc.h>
#include <glib-object.h>
//#include <hyprland/src/desktop/Popup.hpp>
#include <hyprland/src/desktop/view/Popup.hpp>
#include <hyprland/src/managers/SeatManager.hpp>
#include <unordered_map>
#include <wlr-layer-shell-unstable-v1.hpp>

#include <wayland-server-core.h>
#include <wayland-server-protocol.h>
#include <wlr-layer-shell-unstable-v1.hpp>
#include <xcb/xproto.h>
#include <xkbcommon/xkbcommon.h>
#include <filesystem>

#ifdef TRACY_ENABLE
#include "tracy/Tracy.hpp"
#endif

#include "container.h"
#include "first.h"
#include <cassert>
#include <hyprland/src/helpers/time/Time.hpp>

#include <algorithm>
#include <hyprutils/math/Vector2D.hpp>
#include <librsvg/rsvg.h>
#include <any>

#define private public
#include <hyprland/src/render/OpenGL.hpp>
#undef private

#include <hyprland/src/helpers/Color.hpp>
#include <kde-server-decoration.hpp>
//#include <hyprland/protocols/kde-server-decoration.hpp>
//#include <hyprland/protocols/wlr-layer-shell-unstable-v1.hpp>

#include <hyprland/src/render/pass/ShadowPassElement.hpp>
#include <hyprland/src/render/pass/RendererHintsPassElement.hpp>
//#include <hyprland/src/desktop/LayerSurface.hpp>
#include <hyprland/src/desktop/view/LayerSurface.hpp>
#include <hyprland/src/protocols/core/DataDevice.hpp>
#include <hyprland/src/protocols/PointerConstraints.hpp>
#include <hyprland/src/protocols/RelativePointer.hpp>
#include <hyprland/src/protocols/SessionLock.hpp>
#include <hyprland/src/protocols/LayerShell.hpp>

#define private public
#include <hyprland/src/render/pass/SurfacePassElement.hpp>
#include <hyprland/src/render/decorations/CHyprDropShadowDecoration.hpp>
#include <hyprland/src/protocols/ServerDecorationKDE.hpp>
#include <hyprland/src/protocols/XDGDecoration.hpp>
#include <hyprland/src/protocols/XDGShell.hpp>
#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/managers/KeybindManager.hpp>
#include <hyprland/src/managers/input/InputManager.hpp>
#include <hyprland/src/xwayland/XWM.hpp>
#include <hyprland/src/config/ConfigManager.hpp>
#include <hyprland/src/render/OpenGL.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#undef private

#include <hyprland/src/xwayland/XWayland.hpp>
#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/devices/IPointer.hpp>
#include <hyprland/src/render/pass/TexPassElement.hpp>
#include <hyprland/src/render/pass/RectPassElement.hpp>
#include <hyprland/src/render/pass/BorderPassElement.hpp>
#include <hyprland/src/managers/HookSystemManager.hpp>
#include <hyprland/src/managers/PointerManager.hpp>
#include <hyprland/src/managers/LayoutManager.hpp>
#include <hyprland/src/managers/cursor/CursorShapeOverrideController.hpp>
#include <hyprland/src/SharedDefs.hpp>
#include <hyprland/src/desktop/DesktopTypes.hpp>
#include <hyprland/src/render/decorations/DecorationPositioner.hpp>
#include <hyprland/src/render/decorations/IHyprWindowDecoration.hpp>
#include <hyprland/src/desktop/DesktopTypes.hpp>
#include <hyprland/src/render/Framebuffer.hpp>
#include <hyprland/src/desktop/state/FocusState.hpp>

#include <hyprutils/utils/ScopeGuard.hpp>

#include <hyprlang.hpp>

HyprIso *hypriso = new HyprIso;

static int unique_id = 0;

static bool next_check = false;
static std::string previously_seen_instance_signature = "";

void* pRenderWindow = nullptr;
void* pRenderLayer = nullptr;
void* pRenderMonitor = nullptr;
void* pRenderWorkspace = nullptr;
void* pRenderWorkspaceWindows = nullptr;
void* pRenderWorkspaceWindowsFullscreen = nullptr;
typedef void (*tRenderWindow)(void *, PHLWINDOW, PHLMONITOR, const Time::steady_tp&, bool decorate, eRenderPassMode, bool ignorePosition, bool standalone);
typedef void (*tRenderMonitor)(void *, PHLMONITOR pMonitor, bool commit);
typedef void (*tRenderWorkspace)(void *, PHLMONITOR, PHLWORKSPACE, const Time::steady_tp &, const CBox &geom);
typedef void (*tRenderWorkspaceWindows)(void *, PHLMONITOR, PHLWORKSPACE, const Time::steady_tp &);
typedef void (*tRenderWorkspaceWindowsFullscreen)(void *, PHLMONITOR, PHLWORKSPACE, const Time::steady_tp &);

typedef enum
{
    OB_CLIENT_TYPE_DESKTOP, /*!< A desktop (bottom-most window) */
    OB_CLIENT_TYPE_DOCK,    /*!< A dock bar/panel window */
    OB_CLIENT_TYPE_TOOLBAR, /*!< A toolbar window, pulled off an app */
    OB_CLIENT_TYPE_MENU,    /*!< An unpinned menu from an app */
    OB_CLIENT_TYPE_UTILITY, /*!< A small utility window such as a palette */
    OB_CLIENT_TYPE_SPLASH,  /*!< A splash screen window */
    OB_CLIENT_TYPE_DIALOG,  /*!< A dialog window */
    OB_CLIENT_TYPE_NORMAL   /*!< A normal application window */
} ObClientType;

typedef struct _ObMwmHints ObMwmHints;

/*! The MWM Hints as retrieved from the window property
  This structure only contains 3 elements, even though the Motif 2.0
  structure contains 5. We only use the first 3, so that is all gets
  defined.
*/
struct _ObMwmHints
{
    /*! A bitmask of ObMwmFlags values */
    guint flags;
    /*! A bitmask of ObMwmFunctions values */
    guint functions;
    /*! A bitmask of ObMwmDecorations values */
    guint decorations;
};

/*! The number of elements in the ObMwmHints struct */
#define OB_MWM_ELEMENTS 3

/*! Possible flags for MWM Hints (defined by Motif 2.0) */
typedef enum
{
    OB_MWM_FLAG_FUNCTIONS   = 1 << 0, /*!< The MMW Hints define funcs */
    OB_MWM_FLAG_DECORATIONS = 1 << 1  /*!< The MWM Hints define decor */
} ObMwmFlags;

/*! Possible functions for MWM Hints (defined by Motif 2.0) */
typedef enum
{
    OB_MWM_FUNC_ALL      = 1 << 0, /*!< All functions */
    OB_MWM_FUNC_RESIZE   = 1 << 1, /*!< Allow resizing */
    OB_MWM_FUNC_MOVE     = 1 << 2, /*!< Allow moving */
    OB_MWM_FUNC_ICONIFY  = 1 << 3, /*!< Allow to be iconfied */
    OB_MWM_FUNC_MAXIMIZE = 1 << 4  /*!< Allow to be maximized */
#if 0
    OM_MWM_FUNC_CLOSE    = 1 << 5  /*!< Allow to be closed */
#endif
} ObMwmFunctions;

/*! Possible decorations for MWM Hints (defined by Motif 2.0) */
typedef enum
{
    OB_MWM_DECOR_ALL      = 1 << 0, /*!< All decorations */
    OB_MWM_DECOR_BORDER   = 1 << 1, /*!< Show a border */
    OB_MWM_DECOR_HANDLE   = 1 << 2, /*!< Show a handle (bottom) */
    OB_MWM_DECOR_TITLE    = 1 << 3, /*!< Show a titlebar */
#if 0
    OB_MWM_DECOR_MENU     = 1 << 4, /*!< Show a menu */
#endif
    OB_MWM_DECOR_ICONIFY  = 1 << 5, /*!< Show an iconify button */
    OB_MWM_DECOR_MAXIMIZE = 1 << 6  /*!< Show a maximize button */
} ObMwmDecorations;

typedef enum {
    OB_FRAME_DECOR_TITLEBAR    = 1 << 0, /*!< Display a titlebar */
    OB_FRAME_DECOR_HANDLE      = 1 << 1, /*!< Display a handle (bottom) */
    OB_FRAME_DECOR_GRIPS       = 1 << 2, /*!< Display grips in the handle */
    OB_FRAME_DECOR_BORDER      = 1 << 3, /*!< Display a border */
    OB_FRAME_DECOR_ICON        = 1 << 4, /*!< Display the window's icon */
    OB_FRAME_DECOR_ICONIFY     = 1 << 5, /*!< Display an iconify button */
    OB_FRAME_DECOR_MAXIMIZE    = 1 << 6, /*!< Display a maximize button */
    /*! Display a button to toggle the window's placement on
      all desktops */
    OB_FRAME_DECOR_ALLDESKTOPS = 1 << 7,
    OB_FRAME_DECOR_SHADE       = 1 << 8, /*!< Display a shade button */
    OB_FRAME_DECOR_CLOSE       = 1 << 9  /*!< Display a close button */
} ObFrameDecorations;

/*! The things the user can do to the client window */
typedef enum
{
    OB_CLIENT_FUNC_RESIZE     = 1 << 0, /*!< Allow user resizing */
    OB_CLIENT_FUNC_MOVE       = 1 << 1, /*!< Allow user moving */
    OB_CLIENT_FUNC_ICONIFY    = 1 << 2, /*!< Allow to be iconified */
    OB_CLIENT_FUNC_MAXIMIZE   = 1 << 3, /*!< Allow to be maximized */
    OB_CLIENT_FUNC_SHADE      = 1 << 4, /*!< Allow to be shaded */
    OB_CLIENT_FUNC_FULLSCREEN = 1 << 5, /*!< Allow to be made fullscreen */
    OB_CLIENT_FUNC_CLOSE      = 1 << 6, /*!< Allow to be closed */
    OB_CLIENT_FUNC_ABOVE      = 1 << 7, /*!< Allow to be put in lower layer */
    OB_CLIENT_FUNC_BELOW      = 1 << 8, /*!< Allow to be put in higher layer */
    OB_CLIENT_FUNC_UNDECORATE = 1 << 9  /*!< Allow to be undecorated */
} ObFunctions;


struct Anim {
    float *value = nullptr;
    float start_value;
    float target;
    long start_time;
    float time_ms;
    std::weak_ptr<bool> lifetime;
    std::function<void(bool)> on_completion = nullptr;
    std::function<float(float)> lerp_func = nullptr;
};

static std::vector<Anim *> anims;


struct HyprWindow {
    int id;  
    PHLWINDOW w;

    SurfacePassInfo pass_info;
    
    bool checked_resizable = false;
    bool resizable = true;

    bool was_workspace_visible = false;

    bool is_hidden = false; // used in show/hide desktop
    bool was_hidden = false; // used in show/hide desktop

    CFramebuffer *fb = nullptr;
    Bounds w_bounds_raw; // 0 -> 1, percentage of fb taken up by the actual window used for drawing
    Bounds w_size; // 0 -> 1, percentage of fb taken up by the actual window used for drawing
    
    CFramebuffer *deco_fb = nullptr;
    Bounds w_deco_raw; // 0 -> 1, percentage of fb taken up by the actual window used for drawing
    Bounds w_decos_size; // 0 -> 1, percentage of fb taken up by the actual window used for drawing

    CFramebuffer *min_fb = nullptr;
    Bounds w_min_mon;
    Bounds w_min_raw;
    Bounds w_min_size;
    long unminize_start = 0;
    bool animate_to_dock = false;

    int cornermask = 0; // when rendering the surface, what corners should be rounded
    bool no_rounding = false;

    ObClientType type = (ObClientType) -1;
    bool transient = false;
    ObMwmHints mwmhints;
    guint decorations;
    gboolean undecorated;
    guint functions;
};

static std::vector<HyprWindow *> hyprwindows;

struct HyprMonitor {
    int id;  
    PHLMONITOR m;
    long creation_time = get_current_time_in_ms();
    bool first = true;

    CFramebuffer *wallfb = nullptr;
    Bounds wall_size; // 0 -> 1, percentage of fb taken up by the actual window used for drawing
};

static std::vector<HyprMonitor *> hyprmonitors;

// Similar to windows (docks, locksavers, and stuff like that)
struct HyprLayer {
    int id;  
    PHLLS l;
};

static std::vector<HyprLayer *> hyprlayers;

struct HyprWorkspaces {
    int id;
    PHLWORKSPACEREF w;
    CFramebuffer *buffer = nullptr;

    bool is_tiling = false;
};

static std::vector<HyprWorkspaces *> hyprspaces;

struct Texture {
    SP<CTexture> texture;
    TextureInfo info;
};

static std::vector<Texture *> hyprtextures;

class AnyPass : public IPassElement {
public:
    struct AnyData {
        std::function<void(AnyPass*)> draw = nullptr;
        CBox box = {};
    };
    AnyData* m_data = nullptr;

    AnyPass(const AnyData& data) {
        m_data       = new AnyData;
        m_data->draw = data.draw;
    }
    virtual ~AnyPass() {
        delete m_data;
    }

    virtual void draw(const CRegion& damage) {
        // here we can draw anything
        if (m_data->draw) {
            m_data->draw(this);
        }
    }
    virtual bool needsLiveBlur() {
        return false;
    }
    virtual bool needsPrecomputeBlur() {
        return false;
    }
    //virtual std::optional<CBox> boundingBox() {
        //return {};
    //}
    
    virtual const char* passName() {
        return "CAnyPassElement";
    }
};

void set_rounding(int mask) {
    //return; //possibly the slow bomb
    if (!g_pHyprOpenGL || !g_pHyprOpenGL->m_shaders) {
        return;
    }
    // todo set shader mask to 3, and then to 0 afterwards
    g_pHyprOpenGL->useProgram(g_pHyprOpenGL->m_shaders->m_shQUAD.program);
    GLint loc = glGetUniformLocation(g_pHyprOpenGL->m_shaders->m_shQUAD.program, "cornerDisableMask");
    glUniform1i(loc, mask);
    GLint value;

    g_pHyprOpenGL->useProgram(g_pHyprOpenGL->m_shaders->m_shRGBA.program);
    loc = glGetUniformLocation(g_pHyprOpenGL->m_shaders->m_shRGBA.program, "cornerDisableMask");
    glUniform1i(loc, mask);

    g_pHyprOpenGL->useProgram(g_pHyprOpenGL->m_shaders->m_shRGBX.program);
    loc = glGetUniformLocation(g_pHyprOpenGL->m_shaders->m_shRGBX.program, "cornerDisableMask");
    glUniform1i(loc, mask);

    g_pHyprOpenGL->useProgram(g_pHyprOpenGL->m_shaders->m_shEXT.program);
    loc = glGetUniformLocation(g_pHyprOpenGL->m_shaders->m_shEXT.program, "cornerDisableMask");
    glUniform1i(loc, mask);

    g_pHyprOpenGL->useProgram(g_pHyprOpenGL->m_shaders->m_shCM.program);
    loc = glGetUniformLocation(g_pHyprOpenGL->m_shaders->m_shCM.program, "cornerDisableMask");
    glUniform1i(loc, mask);

    g_pHyprOpenGL->useProgram(g_pHyprOpenGL->m_shaders->m_shPASSTHRURGBA.program);
    loc = glGetUniformLocation(g_pHyprOpenGL->m_shaders->m_shPASSTHRURGBA.program, "cornerDisableMask");
    glUniform1i(loc, mask);

    g_pHyprOpenGL->useProgram(g_pHyprOpenGL->m_shaders->m_shBORDER1.program);
    loc = glGetUniformLocation(g_pHyprOpenGL->m_shaders->m_shBORDER1.program, "cornerDisableMask");
    glUniform1i(loc, mask);
}

PHLWINDOW get_window_from_mouse() {
    const auto      MOUSECOORDS = g_pInputManager->getMouseCoordsInternal();
    const PHLWINDOW PWINDOW     = g_pCompositor->vectorToWindowUnified(MOUSECOORDS, Desktop::View::RESERVED_EXTENTS | Desktop::View::INPUT_EXTENTS | Desktop::View::ALLOW_FLOATING);
    return PWINDOW;
}

void on_open_monitor(PHLMONITOR m);
void interleave_floating_and_tiled_windows();

// TODO: need to see how performance intensive this is
void HyprIso::overwrite_animation_speed(float speed) {
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif
 
    //const std::string& nodeName, int enabled, float speed, const std::string& bezier, const std::string& style = ""
    g_pConfigManager->m_animationTree.setConfigForNode("zoomFactor", true, speed, "quick", "");
}

static void change_float_state(PHLWINDOW PWINDOW, bool should_float) {
    if (!PWINDOW)
        return;
    if (PWINDOW->m_isFloating == should_float)
        return;

    // remove drag status
    if (!g_pInputManager->m_currentlyDraggedWindow.expired())
        g_pKeybindManager->changeMouseBindMode(MBIND_INVALID);

    if (PWINDOW->m_groupData.pNextWindow.lock() && PWINDOW->m_groupData.pNextWindow.lock() != PWINDOW) {
        const auto PCURRENT = PWINDOW->getGroupCurrent();

        PCURRENT->m_isFloating = should_float;
        g_pLayoutManager->getCurrentLayout()->changeWindowFloatingMode(PCURRENT);

        PHLWINDOW curr = PCURRENT->m_groupData.pNextWindow.lock();
        while (curr != PCURRENT) {
            curr->m_isFloating = PCURRENT->m_isFloating;
            curr               = curr->m_groupData.pNextWindow.lock();
        }
    } else {
        PWINDOW->m_isFloating = should_float;

        g_pLayoutManager->getCurrentLayout()->changeWindowFloatingMode(PWINDOW);
    }

    if (PWINDOW->m_workspace) {
        PWINDOW->m_workspace->updateWindows();
        PWINDOW->m_workspace->updateWindowData();
    }

    g_pLayoutManager->getCurrentLayout()->recalculateMonitor(PWINDOW->monitorID());
    g_pCompositor->updateAllWindowsAnimatedDecorationValues();

    return;
}

struct MotifHints {
    uint32_t flags;
    uint32_t functions;
    uint32_t decorations;
    int32_t  input_mode;
    uint32_t status;
};

// Motif flags
constexpr uint32_t MWM_HINTS_FUNCTIONS   = 1 << 0;
constexpr uint32_t MWM_FUNC_ALL      = 1 << 0; // enable all functions
constexpr uint32_t MWM_FUNC_RESIZE   = 1 << 1; // allow user to resize window
constexpr uint32_t MWM_FUNC_MOVE     = 1 << 2;
constexpr uint32_t MWM_FUNC_MINIMIZE = 1 << 3;
constexpr uint32_t MWM_FUNC_MAXIMIZE = 1 << 4;
constexpr uint32_t MWM_FUNC_CLOSE    = 1 << 5;
constexpr uint32_t MWM_HINTS_DECORATIONS = 1 << 1;

// Returns motif hints if they exist, otherwise std::nullopt
std::optional<MotifHints> getMotifHints(xcb_connection_t* conn, xcb_window_t win)
{
    if (!conn || !win)
        return std::nullopt;

    const char motifName[] = "_MOTIF_WM_HINTS";

    // Resolve atom
    xcb_intern_atom_cookie_t cookie =
        xcb_intern_atom(conn, 0, sizeof(motifName) - 1, motifName);

    xcb_intern_atom_reply_t* reply =
        xcb_intern_atom_reply(conn, cookie, nullptr);

    if (!reply)
        return std::nullopt;

    xcb_atom_t motifAtom = reply->atom;
    free(reply);

    // Request up to 5 longs (sizeof(MotifHints))
    xcb_get_property_cookie_t propCookie =
        xcb_get_property(conn, 0, win, motifAtom,
                         XCB_GET_PROPERTY_TYPE_ANY, 0, 5);

    xcb_get_property_reply_t* propReply =
        xcb_get_property_reply(conn, propCookie, nullptr);

    if (!propReply || xcb_get_property_value_length(propReply) < sizeof(MotifHints)) {
        free(propReply);
        return std::nullopt;
    }

    MotifHints hints{};
    memcpy(&hints, xcb_get_property_value(propReply), sizeof(hints));
    free(propReply);

    return hints; // full structure
}

bool HyprIso::requested_client_side_decorations(int cid) {
    for (auto hw : hyprwindows) {
        if (hw->id != cid)
            continue;
        auto w = hw->w;
            //for (auto hw : hyprwindows) {
        //for (const auto &s : NProtocols::serverDecorationKDE->m_decos) {
            //if (s->m_surf == hw->w->m_xdgSurface) {
 
        /*for (const auto &s : NProtocols::serverDecorationKDE->m_decos) {
            if (w->m_xdgSurface && s->m_surf == w->m_xdgSurface) {
                //notify(std::to_string((int) s->m_mostRecentlyRequested));
                if (s->m_mostRecentlyRequested == ORG_KDE_KWIN_SERVER_DECORATION_MODE_CLIENT) {
                    notify(fz("1 {}", class_name(hw->id)));
                    return true;
                }
            }
        }
        for (const auto &[a, b] : NProtocols::xdgDecoration->m_decorations) {
            if (w->m_xdgSurface && w->m_xdgSurface->m_toplevel && w->m_xdgSurface->m_toplevel->m_resource && b->m_resource == w->m_xdgSurface->m_toplevel->m_resource) {
                if (b->mostRecentlyRequested == ZXDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE) {
                    return true;
                    notify("2");
                }
            }
        }
        */

        if (w->m_isX11 && w->m_xwaylandSurface) {
            auto win = w->m_xwaylandSurface->m_xID;
            if (g_pXWayland && g_pXWayland->m_wm) {
                auto connection = g_pXWayland->m_wm->getConnection();

                auto hintsOpt = getMotifHints(connection, win);
                if (hintsOpt) {
                    auto& h = *hintsOpt;
                    bool wantsNoDecorations =
                        (h.flags & MWM_HINTS_DECORATIONS) && (h.decorations == 0);
                    if (wantsNoDecorations) {
                        return true;
                    }
                }
            }
        }
    }
    
    return false;
}

void on_open_window(PHLWINDOW w) {
    for (auto m : g_pCompositor->m_monitors) {
        on_open_monitor(m);
    }
    if (auto surface = w->m_xdgSurface) {
        if (auto toplevel = surface->m_toplevel.lock()) {
            auto resource = toplevel->m_resource;
            if (resource) {
                resource->setMove([](CXdgToplevel*, wl_resource*, uint32_t) {
                    if (hypriso->on_drag_start_requested) {
                        if (auto w = get_window_from_mouse()) {
                            for (auto hw : hyprwindows) {
                                if (w == hw->w) {
                                    hypriso->on_drag_start_requested(hw->id);
                                }
                            }
                        }
                    }
                });
                resource->setResize([](CXdgToplevel* t, wl_resource*, uint32_t, xdgToplevelResizeEdge e) {
                    for (auto w : g_pCompositor->m_windows) {
                        if (auto surf = w->m_xdgSurface.lock()) {
                            if (auto top = surf->m_toplevel.lock()) {
                                auto resource = top->m_resource;
                                if (resource.get() == t) {
                                    auto type = RESIZE_TYPE::NONE;
                                    if (e == XDG_TOPLEVEL_RESIZE_EDGE_NONE) {
                                        type = RESIZE_TYPE::NONE;
                                    } else if (e == XDG_TOPLEVEL_RESIZE_EDGE_TOP) {
                                        type = RESIZE_TYPE::TOP;
                                    } else if (e == XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM) {
                                        type = RESIZE_TYPE::BOTTOM;
                                    } else if (e == XDG_TOPLEVEL_RESIZE_EDGE_LEFT) {
                                        type = RESIZE_TYPE::LEFT;
                                    } else if (e == XDG_TOPLEVEL_RESIZE_EDGE_TOP_LEFT) {
                                        type = RESIZE_TYPE::TOP_LEFT;
                                    } else if (e == XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM_LEFT) {
                                        type = RESIZE_TYPE::BOTTOM_LEFT;
                                    } else if (e == XDG_TOPLEVEL_RESIZE_EDGE_RIGHT) {
                                        type = RESIZE_TYPE::RIGHT;
                                    } else if (e == XDG_TOPLEVEL_RESIZE_EDGE_TOP_RIGHT) {
                                        type = RESIZE_TYPE::TOP_RIGHT;
                                    } else if (e == XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM_RIGHT) {
                                        type = RESIZE_TYPE::BOTTOM_RIGHT;
                                    }
                                    int id = 0;
                                    for (auto hw : hyprwindows)
                                        if (hw->w == w)
                                            id = hw->id;
                                    hypriso->on_resize_start_requested(id, type);
                                }
                            }
                        }
                    }
                });
            }
        }
    }

    auto hw = new HyprWindow;
    hw->id = unique_id++;
    hw->w = w;
    hyprwindows.push_back(hw);
    hypriso->on_window_open(hw->id);
}

void on_close_window(PHLWINDOW w) {
    HyprWindow *hw = nullptr;
    int target_index = 0;
    for (int i = 0; i < hyprwindows.size(); i++) {
        auto hr = hyprwindows[i];
        if (hr->w == w) {
            target_index = i;
            hw = hr;
        }
    }
    if (hw) {
        hypriso->on_window_closed(hw->id);
        delete hw;
        hyprwindows.erase(hyprwindows.begin() + target_index);
    }
}

void on_open_monitor(PHLMONITOR m) {
    for (auto mons : hyprmonitors) {
        if (mons->m == m)
            return;
    }
    auto hm = new HyprMonitor;
    hm->id = unique_id++;
    hm->m = m;
    hyprmonitors.push_back(hm);
    hypriso->on_monitor_open(hm->id);
    //notify("monitor open");
}

void on_close_monitor(PHLMONITOR m) {
    HyprMonitor *hm = nullptr;
    int target_index = 0;
    for (int i = 0; i < hyprmonitors.size(); i++) {
        auto hr = hyprmonitors[i];
        if (hr->m == m) {
            target_index = i;
            hm = hr;
        }
    }
    if (hm) {
        hypriso->on_monitor_closed(hm->id);
        delete hm;
        hyprmonitors.erase(hyprmonitors.begin() + target_index);
    }
}
SurfacePassInfo HyprIso::pass_info(int cid) {
    for (auto h : hyprwindows) {
        if (h->id == cid) {
            return h->pass_info;
        }
    }
        
    return SurfacePassInfo();
}

CSurfacePassElement::SRenderData collect_mdata(PHLWINDOW w) {
    //CSurfacePassElement::SRenderData renderdata;
    // m_data.pMonitor probably render mon
    //auto current = g_pHyprOpenGL->m_renderData.currentWindow;
    //
    auto pWindow = w;
    auto pMonitor = g_pHyprOpenGL->m_renderData.pMonitor;
    if (!pMonitor) {
        pMonitor = w->m_monitor;
    }
    const auto PWORKSPACE = pWindow->m_workspace;
    const auto REALPOS = pWindow->m_realPosition->value() + (pWindow->m_pinned ? Vector2D{} : PWORKSPACE->m_renderOffset->value());
    
    static auto time = Time::steadyNow();
    CSurfacePassElement::SRenderData renderdata = {pMonitor, time};
    CBox textureBox = {REALPOS.x, REALPOS.y, std::max(pWindow->m_realSize->value().x, 5.0), std::max(pWindow->m_realSize->value().y, 5.0)};

    renderdata.w = textureBox.w;
    renderdata.h = textureBox.h;
    // m_data.w,h
    renderdata.pWindow = w; // m_data.pWindow

    renderdata.surface = pWindow->wlSurface()->resource(); // m_data.surface
    // m_data.mainSurface
    renderdata.pos.x = textureBox.x; // m_data.pos.x,y
    renderdata.pos.y = textureBox.y;
    renderdata.pos.x += pWindow->m_floatingOffset.x;
    renderdata.pos.y += pWindow->m_floatingOffset.y;
    renderdata.surfaceCounter = 0;
    pWindow->wlSurface()->resource()->breadthfirst(
        [&renderdata, &pWindow](SP<CWLSurfaceResource> s, const Vector2D& offset, void* data) {
            // m_data.localPos
            renderdata.localPos = offset;
            renderdata.texture = s->m_current.texture;
            renderdata.surface = s;
            renderdata.mainSurface = s == pWindow->wlSurface()->resource();
            renderdata.surfaceCounter++;
            //notify(std::to_string(renderdata.surfaceCounter));
        },
        nullptr);

    // m_data.squishOversized left default
    return renderdata;
}

Bounds tobounds(CBox box);

Bounds HyprIso::getTexBox(int id) {
#ifdef FORK_WARN
    static_assert(false, "[Function Body] Make sure our `CSurfacePassElement::getTexBox()` and Hyprland's are synced!");
    //src/render/pass/SurfacePassElement.cpp
    //CBox CSurfacePassElement::getTexBox() {
#endif

    PHLWINDOW w;
    bool found = false;
    for (auto h : hyprwindows) {
        if (h->id == id) {
            w = h->w;
            found = true;
            break;
        }
    }

    if (!found)
        return {};
    auto m_data = collect_mdata(w); 

    // ===========================
    
    const double outputX = -m_data.pMonitor->m_position.x, outputY = -m_data.pMonitor->m_position.y;

    const auto   INTERACTIVERESIZEINPROGRESS = m_data.pWindow && g_pInputManager->m_currentlyDraggedWindow && g_pInputManager->m_dragMode == MBIND_RESIZE;
    auto         PSURFACE                    = Desktop::View::CWLSurface::fromResource(m_data.surface);

    CBox         windowBox;
    if (m_data.surface && m_data.mainSurface) {
        windowBox = {sc<int>(outputX) + m_data.pos.x + m_data.localPos.x, sc<int>(outputY) + m_data.pos.y + m_data.localPos.y, m_data.w, m_data.h};

        // however, if surface buffer w / h < box, we need to adjust them
        const auto PWINDOW = PSURFACE ? Desktop::View::CWindow::fromView(PSURFACE->view()) : nullptr;

        // center the surface if it's smaller than the viewport we assign it
        if (PSURFACE && !PSURFACE->m_fillIgnoreSmall && PSURFACE->small() /* guarantees PWINDOW */) {
            const auto CORRECT  = PSURFACE->correctSmallVec();
            const auto SIZE     = PSURFACE->getViewporterCorrectedSize();
            const auto REPORTED = PWINDOW->getReportedSize();

            if (!INTERACTIVERESIZEINPROGRESS) {
                windowBox.translate(CORRECT);

                windowBox.width  = SIZE.x * (PWINDOW->m_realSize->value().x / REPORTED.x);
                windowBox.height = SIZE.y * (PWINDOW->m_realSize->value().y / REPORTED.y);
            } else {
                windowBox.width  = SIZE.x;
                windowBox.height = SIZE.y;
            }
        }
    } else { //  here we clamp to 2, these might be some tiny specks

        const auto SURFSIZE = m_data.surface->m_current.size;

        windowBox = {sc<int>(outputX) + m_data.pos.x + m_data.localPos.x, sc<int>(outputY) + m_data.pos.y + m_data.localPos.y, std::max(sc<float>(SURFSIZE.x), 2.F),
                     std::max(sc<float>(SURFSIZE.y), 2.F)};
        if (m_data.pWindow && m_data.pWindow->m_realSize->isBeingAnimated() && m_data.surface && !m_data.mainSurface && m_data.squishOversized /* subsurface */) {
            // adjust subsurfaces to the window
            const auto REPORTED = m_data.pWindow->getReportedSize();
            if (REPORTED.x != 0 && REPORTED.y != 0) {
                windowBox.width  = (windowBox.width / REPORTED.x) * m_data.pWindow->m_realSize->value().x;
                windowBox.height = (windowBox.height / REPORTED.y) * m_data.pWindow->m_realSize->value().y;
            }
        }
    }

    if (m_data.squishOversized) {
        if (m_data.localPos.x + windowBox.width > m_data.w)
            windowBox.width = m_data.w - m_data.localPos.x;
        if (m_data.localPos.y + windowBox.height > m_data.h)
            windowBox.height = m_data.h - m_data.localPos.y;
    }

    return tobounds(windowBox);
}


inline CFunctionHook* g_pOnSurfacePassDraw = nullptr;
typedef void (*origSurfacePassDraw)(CSurfacePassElement *, const CRegion& damage);
void hook_onSurfacePassDraw(void* thisptr, const CRegion& damage) {
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif
 
    auto  spe = (CSurfacePassElement *) thisptr;
    SurfacePassInfo i;
    i.pos_x = spe->m_data.pos.x;
    i.pos_y = spe->m_data.pos.y;
    i.local_pos_x = spe->m_data.localPos.y;
    i.local_pos_y = spe->m_data.localPos.y;
    i.w = spe->m_data.w;
    i.h = spe->m_data.h;
    auto bb = spe->getTexBox();
    bb.scale(spe->m_data.pMonitor->m_scale);
    bb.round();
    i.cbx = bb.x;
    i.cby = bb.y;
    i.cbw = bb.w;
    i.cbh = bb.h;
    
    auto window = spe->m_data.pWindow;
    //notify("alo");
    int cornermask = 0;
    for (auto hw: hyprwindows) {
        if (hw->w == window) {
            cornermask = hw->cornermask;
            hw->pass_info = i;
        }
    }
    set_rounding(cornermask); // only top rounding
    (*(origSurfacePassDraw)g_pOnSurfacePassDraw->m_original)(spe, damage);
    set_rounding(0);
}

void fix_window_corner_rendering() {
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif
 
    //return;
    static const auto METHODS = HyprlandAPI::findFunctionsByName(globals->api, "draw");
    // TODO: check if m.address is same as set_rounding even though signature is SurfacePassElement
    for (auto m : METHODS) {
        if (m.signature.find("SurfacePassElement") != std::string::npos) {
            //notify(m.demangled);
            g_pOnSurfacePassDraw = HyprlandAPI::createFunctionHook(globals->api, m.address, (void*)&hook_onSurfacePassDraw);
            g_pOnSurfacePassDraw->hook();
            return;
        }
    }
}


inline CFunctionHook* g_pOnReadProp = nullptr;
typedef void (*origOnReadProp)(void*, SP<CXWaylandSurface> XSURF, uint32_t atom, xcb_get_property_reply_t* reply);
    //void CXWM::readProp(SP<CXWaylandSurface> XSURF, uint32_t atom, xcb_get_property_reply_t* reply) {
void hook_OnReadProp(void* thisptr, SP<CXWaylandSurface> XSURF, uint32_t atom, xcb_get_property_reply_t* reply) {
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif
 
    (*(origOnReadProp)g_pOnReadProp->m_original)(thisptr, XSURF, atom, reply);

    /*
    const auto* value    = sc<const char*>(xcb_get_property_value(reply));

    auto handleMotifs = [&]() {
        // Motif hints are 5 longs: flags, functions, decorations, input_mode, status
        const uint32_t* hints = rc<const uint32_t*>(value);

        std::vector<uint32_t> m_motifs;
        m_motifs.assign(hints, hints + std::min<size_t>(reply->value_len, 5));

        for (const auto &w : g_pCompositor->m_windows) {
            if (w->m_xwaylandSurface == XSURF) {
                w->m_X11DoesntWantBorders = false;
                g_pXWaylandManager->checkBorders(w);

                const uint32_t flags       = m_motifs[0];
                const uint32_t decorations = m_motifs.size() > 2 ? m_motifs[2] : 1;
                const uint32_t MWM_HINTS_DECORATIONS = (1L << 1);

                if ((flags & MWM_HINTS_DECORATIONS) && decorations == 0) {
                    w->m_X11DoesntWantBorders = true;
                }

                // has decora
                bool has_decorations = false;
                for (auto& wd : w->m_windowDecorations)
                    if (wd->getDisplayName() == "MylarBar")
                        has_decorations = true;

                if (w->m_X11DoesntWantBorders && has_decorations) {
                    for (auto it = w.get()->m_windowDecorations.rbegin(); it != w.get()->m_windowDecorations.rend(); ++it) {
                        auto bar = (IHyprWindowDecoration *) it->get();
                        if (bar->getDisplayName() == "MylarBar" || bar->getDisplayName() == "MylarResize") {
                            HyprlandAPI::removeWindowDecoration(globals->api, bar);
                            for (auto hw : hyprwindows) {
                                if (hw->w == w && hypriso->on_window_closed) {
                                    hypriso->on_window_closed(hw->id);
                                }
                            }
                        }
                    }
                } else if (!w->m_X11DoesntWantBorders && !has_decorations) {
                    // add
                    for (auto hw : hyprwindows) {
                        if (hw->w == w && hypriso->on_window_open) {
                           hypriso->on_window_open(hw->id);
                        }
                    }
                }
            }
        }
    };
    if (atom == HYPRATOMS["_MOTIF_WM_HINTS"])
        handleMotifs();
    */
}

static wl_event_source *source = nullptr;

void remove_request_listeners() {
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif
    
    for (auto& w : g_pCompositor->m_windows) {
        if (auto surface = w->m_xdgSurface) {
            if (auto toplevel = surface->m_toplevel.lock()) {
                auto resource = toplevel->m_resource;
                if (resource) {
                    resource->setMove(nullptr);
                    resource->setResize(nullptr);
                }
            }
        }
    }
}

inline CFunctionHook* g_pOnRMS = nullptr;
typedef Vector2D (*origOnRMS)(void*);
Vector2D hook_OnRMS(void* thisptr) {
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif
    //recheck_csd_for_all_wayland_windows();
    //notify("min");
    return Vector2D(4, 4);
    //return (*(origOnRMS)g_pOnRMS->m_original)(thisptr);
}

void overwrite_min() {
    return;
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif
    {
        static const auto METHODS = HyprlandAPI::findFunctionsByName(globals->api, "requestedMinSize");
        g_pOnRMS = HyprlandAPI::createFunctionHook(globals->api, METHODS[0].address, (void*)&hook_OnRMS);
        g_pOnRMS->hook();
    }
    
}

void recheck_csd_for_all_wayland_windows() {
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif
    if (source)    
        return;
    source = wl_event_loop_add_timer(g_pCompositor->m_wlEventLoop, [](void *) {
        source = nullptr;

        for (auto w : g_pCompositor->m_windows) {
            if (w->m_isX11)
                continue;
            
            bool remove_csd = false;
            for (const auto &s : NProtocols::serverDecorationKDE->m_decos) {
                if (w->m_xdgSurface && s->m_surf == w->m_xdgSurface) {
                    //notify(std::to_string((int) s->m_mostRecentlyRequested));
                    if (s->m_mostRecentlyRequested == ORG_KDE_KWIN_SERVER_DECORATION_MODE_CLIENT) {
                        remove_csd = true;
                    }
                }
            }

            for (const auto &[a, b] : NProtocols::xdgDecoration->m_decorations) {
                if (w->m_xdgSurface && w->m_xdgSurface->m_toplevel && w->m_xdgSurface->m_toplevel->m_resource && b->m_resource == w->m_xdgSurface->m_toplevel->m_resource) {
                    if (b->mostRecentlyRequested == ZXDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE) {
                        remove_csd = true;
                    }
                }
            }


            bool has_csd = false;
            for (auto& wd : w->m_windowDecorations)
                if (wd->getDisplayName() == "MylarBar")
                    has_csd = true;
            if (has_csd && remove_csd) {
                for (auto hw : hyprwindows) {
                    if (hw->w == w) {
                        hypriso->on_window_closed(hw->id);
                    }
                }
            } else if (!has_csd && !remove_csd) {
                for (auto hw : hyprwindows) {
                    if (hw->w == w) {
                        hypriso->on_window_open(hw->id);
                    }
                }
            }
        }
        
        return 0; // 0 means stop timer, >0 means retry in that amount of ms
    }, nullptr);
    wl_event_source_timer_update(source, 10); // 10ms
}

inline CFunctionHook* g_pOnKDECSD = nullptr;
typedef uint32_t (*origOnKDECSD)(void*);
uint32_t hook_OnKDECSD(void* thisptr) {
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif
    //notify(fz("hook"));

    auto ptr = (CServerDecorationKDE *) thisptr;
    for (auto hw : hyprwindows) {
        for (const auto &s : NProtocols::serverDecorationKDE->m_decos) {
            if (s->m_surf == hw->w->m_xdgSurface) {
                //notify(fz("hook_OnKDECSD {}", hypriso->class_name(hw->id)));
                break;
            }
        }
    } 
    //ptr->m_resource
    //recheck_csd_for_all_wayland_windows();
    //
    return ORG_KDE_KWIN_SERVER_DECORATION_MANAGER_MODE_CLIENT;
}

inline CFunctionHook* g_pOnKDERequestCSD = nullptr;
typedef uint32_t (*origOnKDERequestCSD)(void*, uint32_t mode);
uint32_t hook_OnKDERequestCSD(void* thisptr, uint32_t mode) {
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif
    //notify("ehllo");
    //notify(fz("mode {}", mode));
    //recheck_csd_for_all_wayland_windows();
    for (auto hw : hyprwindows) {
        for (const auto &s : NProtocols::serverDecorationKDE->m_decos) {
            if (s->m_surf == hw->w->m_xdgSurface) {
                //notify(fz("hook_OnKDERequestCSD {} {}", hypriso->class_name(hw->id), mode));
                break;
            }
        }
    } 
    
    return ORG_KDE_KWIN_SERVER_DECORATION_MANAGER_MODE_CLIENT;
}

inline CFunctionHook* g_pOnKDEReleaseCSD = nullptr;
typedef uint32_t (*origOnKDEReleaseCSD)(void*);
uint32_t hook_OnKDEReleaseCSD(void* thisptr) {
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif
    //notify(fz("released"));

    //recheck_csd_for_all_wayland_windows();
    for (auto hw : hyprwindows) {
        for (const auto &s : NProtocols::serverDecorationKDE->m_decos) {
            if (s->m_surf == hw->w->m_xdgSurface) {
                //notify(fz("hook_OnKDEReleaseCSD {}", hypriso->class_name(hw->id)));
                break;
            }
        }
    } 
    return ORG_KDE_KWIN_SERVER_DECORATION_MANAGER_MODE_CLIENT;
}



inline CFunctionHook* g_pOnXDGCSD = nullptr;
typedef zxdgToplevelDecorationV1Mode (*origOnXDGCSD)(void*);
zxdgToplevelDecorationV1Mode hook_OnXDGCSD(void* thisptr) {
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif
    recheck_csd_for_all_wayland_windows();
    return (*(origOnXDGCSD)g_pOnXDGCSD->m_original)(thisptr);
}

void detect_csd_request_change() {
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif
    //notify("detect");
    // hook xdg and kde csd request mode, then set timeout for 25 ms, 5 times which checks and updates csd for current windows based on most recent requests
    {
        static const auto METHODS = HyprlandAPI::findFunctionsByName(globals->api, "kdeDefaultModeCSD");
        g_pOnKDECSD = HyprlandAPI::createFunctionHook(globals->api, METHODS[0].address, (void*)&hook_OnKDECSD);
        g_pOnKDECSD->hook();
    }
    {
        static const auto METHODS = HyprlandAPI::findFunctionsByName(globals->api, "kdeModeOnRequestCSD");
        g_pOnKDERequestCSD = HyprlandAPI::createFunctionHook(globals->api, METHODS[0].address, (void*)&hook_OnKDERequestCSD);
        g_pOnKDERequestCSD->hook();
    }
    {
        static const auto METHODS = HyprlandAPI::findFunctionsByName(globals->api, "kdeModeOnReleaseCSD");
        g_pOnKDEReleaseCSD = HyprlandAPI::createFunctionHook(globals->api, METHODS[0].address, (void*)&hook_OnKDEReleaseCSD);
        g_pOnKDEReleaseCSD->hook();
    }

    /*{
        static const auto METHODS = HyprlandAPI::findFunctionsByName(globals->api, "xdgDefaultModeCSD");
        g_pOnXDGCSD = HyprlandAPI::createFunctionHook(globals->api, METHODS[0].address, (void*)&hook_OnXDGCSD);
        g_pOnXDGCSD->hook();
    }*/

    // hook props change xwayland function, parse motifs, set or remove decorations as needed
    {
        static const auto METHODS = HyprlandAPI::findFunctionsByName(globals->api, "readProp");
        g_pOnReadProp = HyprlandAPI::createFunctionHook(globals->api, METHODS[0].address, (void*)&hook_OnReadProp);
        g_pOnReadProp->hook();
    }

}


#define _NET_WM_MOVERESIZE_SIZE_TOPLEFT     0
#define _NET_WM_MOVERESIZE_SIZE_TOP         1
#define _NET_WM_MOVERESIZE_SIZE_TOPRIGHT    2
#define _NET_WM_MOVERESIZE_SIZE_RIGHT       3
#define _NET_WM_MOVERESIZE_SIZE_BOTTOMRIGHT 4
#define _NET_WM_MOVERESIZE_SIZE_BOTTOM      5
#define _NET_WM_MOVERESIZE_SIZE_BOTTOMLEFT  6
#define _NET_WM_MOVERESIZE_SIZE_LEFT        7
#define _NET_WM_MOVERESIZE_MOVE             8  /* movement only */
#define _NET_WM_MOVERESIZE_SIZE_KEYBOARD    9  /* size via keyboard */
#define _NET_WM_MOVERESIZE_MOVE_KEYBOARD    10 /* move via keyboard */
#define _NET_WM_MOVERESIZE_CANCEL           11 /* cancel operation */

std::string get_atom_name(xcb_connection_t* conn, xcb_atom_t atom) {
    if (atom == XCB_ATOM_NONE)
        return "XCB_ATOM_NONE";

    xcb_get_atom_name_cookie_t cookie = xcb_get_atom_name(conn, atom);
    xcb_get_atom_name_reply_t* reply  = xcb_get_atom_name_reply(conn, cookie, nullptr);
    if (!reply)
        return "(failed to get atom)";

    std::string name(xcb_get_atom_name_name(reply), xcb_get_atom_name_name_length(reply));
    free(reply);
    return name;
}

PHLWINDOW winref_from_x11(xcb_window_t window) {
    for (auto w : g_pCompositor->m_windows) {
        if (w->m_isX11 && w->m_xwaylandSurface->m_xID == window) {
            return w;
        }
    }
    return nullptr;
}

void resizing_start(RESIZE_TYPE type, PHLWINDOW w, Vector2D mouse) {
    if (hypriso->on_resize_start_requested) {
        for (auto hw : hyprwindows) {
            if (hw->w == w) {
                hypriso->on_resize_start_requested(hw->id, type);
                return;                
            }
        }
    }
}

inline CFunctionHook* g_pOnClientMessageHook = nullptr;
typedef void (*origOnClientMessage)(void*, xcb_client_message_event_t* e);
void hook_onClientMessage(void* thisptr, xcb_client_message_event_t* e) {
    (*(origOnClientMessage)g_pOnClientMessageHook->m_original)(thisptr, e);
    auto spe = (CXWM*)thisptr;

    xcb_connection_t* conn = spe->getConnection(); // calls the private getConnection()
    if (conn && e->type != HYPRATOMS["WM_PROTOCOLS"]) {
        //auto name = get_atom_name(conn, e->type);
        if (e->type == HYPRATOMS["_NET_WM_MOVERESIZE"]) {
            auto direction = e->data.data32[2];
            //n(f("move resize: {}", direction));
            if (auto w = winref_from_x11(e->window)) {
                auto mouse = g_pInputManager->getMouseCoordsInternal();
                if (direction == _NET_WM_MOVERESIZE_SIZE_TOPLEFT) {
                    resizing_start(RESIZE_TYPE::TOP_LEFT, w, mouse);
                } else if (direction == _NET_WM_MOVERESIZE_SIZE_TOP) {
                    resizing_start(RESIZE_TYPE::TOP, w, mouse);
                } else if (direction == _NET_WM_MOVERESIZE_SIZE_TOPRIGHT) {
                    resizing_start(RESIZE_TYPE::TOP_RIGHT, w, mouse);
                } else if (direction == _NET_WM_MOVERESIZE_SIZE_RIGHT) {
                    resizing_start(RESIZE_TYPE::RIGHT, w, mouse);
                } else if (direction == _NET_WM_MOVERESIZE_SIZE_BOTTOMRIGHT) {
                    resizing_start(RESIZE_TYPE::BOTTOM_RIGHT, w, mouse);
                } else if (direction == _NET_WM_MOVERESIZE_SIZE_BOTTOM) {
                    resizing_start(RESIZE_TYPE::BOTTOM, w, mouse);
                } else if (direction == _NET_WM_MOVERESIZE_SIZE_BOTTOMLEFT) {
                    resizing_start(RESIZE_TYPE::BOTTOM_LEFT, w, mouse);
                } else if (direction == _NET_WM_MOVERESIZE_SIZE_LEFT) {
                    resizing_start(RESIZE_TYPE::LEFT, w, mouse);
                } else if (direction == _NET_WM_MOVERESIZE_MOVE) {
                    if (hypriso->on_drag_start_requested) {
                        for (auto hw : hyprwindows) {
                            if (hw->w == w) {
                                hypriso->on_drag_start_requested(hw->id);
                                break;
                            }
                        }
                    }
                } else if (_NET_WM_MOVERESIZE_CANCEL) {
                    if (hypriso->on_drag_or_resize_cancel_requested) {
                        hypriso->on_drag_or_resize_cancel_requested();
                    }
                }
            }
        } 
/*
        else if (e->type == HYPRATOMS["WM_CHANGE_STATE"]) { // visibility change
            int type = e->data.data32[0];
            if (auto w = winref_from_x11(e->window)) {
                if (type == 3) { // iconify
                    hypriso->set_hidden(w, true);
                } else if (type == 1) { // show
                    hypriso->set_hidden(w, false);
                }
            }
        }
        */

        /*
		if (get_atom_name(conn, e->type) == "WM_PROTOCOLS") {
            xcb_atom_t protocol = e->data.data32[0]; // first element usually
            xcb_get_atom_name_cookie_t proto_cookie = xcb_get_atom_name(conn, protocol);
            xcb_get_atom_name_reply_t* proto_reply = xcb_get_atom_name_reply(conn, proto_cookie, nullptr);
            if (proto_reply) {
                int len = xcb_get_atom_name_name_length(proto_reply);
                char* name = xcb_get_atom_name_name(proto_reply);
                std::string proto_name(name, len);
                //n(f("Protocol: {}", proto_name));
                free(proto_reply);
            }
		}
		*/
    }
}

void detect_x11_move_resize_requests() {
    {
        static const auto METHODS = HyprlandAPI::findFunctionsByName(globals->api, "handleClientMessage");
        if (!METHODS.empty()) {
            g_pOnClientMessageHook = HyprlandAPI::createFunctionHook(globals->api, METHODS[0].address, (void*)&hook_onClientMessage);
            g_pOnClientMessageHook->hook();
        } else {
            notify("Couldn't hook handleClientMessage");
        }
    }
}

static void configHandleGradientDestroy(void** data) {
    if (*data)
        delete sc<CGradientValueData*>(*data);
}

inline CFunctionHook* g_pRenderWindowHook = nullptr;

typedef void (*origRenderWindowFunc)(CHyprRenderer *, PHLWINDOW pWindow, PHLMONITOR pMonitor, const Time::steady_tp& time, bool decorate, eRenderPassMode mode, bool ignorePosition, bool standalone);

void hook_RenderWindow(void* thisptr, PHLWINDOW pWindow, PHLMONITOR pMonitor, const Time::steady_tp& time, bool decorate, eRenderPassMode mode, bool ignorePosition, bool standalone) {
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif
    if (!hypriso->render_whitelist.empty() || hypriso->whitelist_on) {
        for (auto hw : hyprwindows) {
            if (hw->w == pWindow) {
                bool found = false;
                for (auto white : hypriso->render_whitelist) {
                    if (white == hw->id) {
                        found = true;
                    }
                }
                if (!found) {
                    if (!g_pHyprRenderer->m_bRenderingSnapshot) {
                        return;
                    }
                }
            }
        }
    }

    Hyprlang::INT* rounding_amount = nullptr;
    int initial_value = 0;

    Hyprlang::INT* border_size = nullptr;
    int initial_border_size = 0;

    Hyprlang::CConfigCustomValueType* active_border = nullptr; 
    Hyprlang::CConfigCustomValueType* initial_active_border;
    auto current = get_current_time_in_ms();
    
    for (auto hw : hyprwindows) {
        if (hw->w == pWindow) {
            if (current - hw->unminize_start < minimize_anim_time && hw->animate_to_dock)
                return;
        }
        if (hw->w == pWindow && hw->no_rounding) {
            {
                Hyprlang::CConfigValue* val = g_pConfigManager->getHyprlangConfigValuePtr("decoration:rounding");
                rounding_amount = (Hyprlang::INT*)val->dataPtr();
                initial_value = *rounding_amount;
                *rounding_amount = 0;
            }

            Hyprlang::CConfigValue* val2 = g_pConfigManager->getHyprlangConfigValuePtr("general:border_size");
            border_size = (Hyprlang::INT*)val2->dataPtr();
            initial_border_size = *border_size;
            *border_size = 0;
        }
    }
    (*(origRenderWindowFunc)g_pRenderWindowHook->m_original)((CHyprRenderer *) thisptr, pWindow, pMonitor, Time::steadyNow(), decorate, mode, ignorePosition, standalone);
    if (rounding_amount) {
        *rounding_amount = initial_value;
        *border_size = initial_border_size;
        //if (active_border) {
            //active_border = initial_active_border;
        //}
    }
}

inline CFunctionHook* g_pWindowRoundingHook = nullptr;
typedef float (*origWindowRoundingFunc)(Desktop::View::CWindow *);
float hook_WindowRounding(void* thisptr) {
#ifdef FORK_WARN
//float CWindow::rounding() {
    static_assert(false, "[Function Body] Make sure our `roundingPower` and Hyprland's are synced!");
#endif
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif
    float result = (*(origWindowRoundingFunc)g_pWindowRoundingHook->m_original)((Desktop::View::CWindow *)thisptr);
    return result;
}

inline CFunctionHook* g_pWindowRoundingPowerHook = nullptr;
typedef float (*origWindowRoundingPowerFunc)(Desktop::View::CWindow *);
float hook_WindowRoundingPower(void* thisptr) {
#ifdef FORK_WARN
//float CWindow::roundingPower() {
    static_assert(false, "[Function Body] Make sure our `roundingPower` and Hyprland's are synced!");
#endif
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif
    float result = (*(origWindowRoundingPowerFunc)g_pWindowRoundingPowerHook->m_original)((Desktop::View::CWindow *)thisptr);
    return result;
}

void hook_render_functions() {
    //return;
    /*
    {
        static const auto METHODS = HyprlandAPI::findFunctionsByName(globals->api, "rounding");
        for (auto m : METHODS) {
            if (m.signature.find("CWindow") != std::string::npos) {
                g_pWindowRoundingHook       = HyprlandAPI::createFunctionHook(globals->api, m.address, (void*)&hook_WindowRounding);
                g_pWindowRoundingHook->hook();
                break;
            }
        }
    }
    {
        static const auto METHODS = HyprlandAPI::findFunctionsByName(globals->api, "roundingPower");
        for (auto m : METHODS) {
            if (m.signature.find("CWindow") != std::string::npos) {
                g_pWindowRoundingPowerHook       = HyprlandAPI::createFunctionHook(globals->api, m.address, (void*)&hook_WindowRoundingPower);
                g_pWindowRoundingPowerHook->hook();
                break;
            }
        }
    }
    */
 
    {
        static const auto METHODS = HyprlandAPI::findFunctionsByName(globals->api, "renderWindow");
        g_pRenderWindowHook       = HyprlandAPI::createFunctionHook(globals->api, METHODS[0].address, (void*)&hook_RenderWindow);
        g_pRenderWindowHook->hook();
        pRenderWindow = METHODS[0].address;
    }
    {
        static const auto METHODS = HyprlandAPI::findFunctionsByName(globals->api, "renderLayer");
        pRenderLayer = METHODS[0].address;
    }
    {
        static const auto METHODS = HyprlandAPI::findFunctionsByName(globals->api, "renderWorkspace");
        pRenderWorkspace = METHODS[0].address;
    }
    {
        static const auto METHODS = HyprlandAPI::findFunctionsByName(globals->api, "renderMonitor");
        pRenderMonitor = METHODS[0].address;
    }
    
}

inline CFunctionHook* g_pOnCircleNextHook = nullptr;
typedef bool (*origOnCircleNextFunc)(void*, const IPointer::SButtonEvent&);
SDispatchResult hook_onCircleNext(void* thisptr, std::string arg) {
    // we don't call the original function because we want to remove it
    return {};
}

void disable_default_alt_tab_behaviour() {
    //return;
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif
    {
        static const auto METHODS = HyprlandAPI::findFunctionsByName(globals->api, "circleNext");
        g_pOnCircleNextHook       = HyprlandAPI::createFunctionHook(globals->api, METHODS[0].address, (void*)&hook_onCircleNext);
        g_pOnCircleNextHook->hook();
    }
}

void set_window_corner_mask(int id, int cornermask) {
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif
    notify("deprecated(set_window_corner_mask): use set_corner_rendering_mask_for_window");
    hypriso->set_corner_rendering_mask_for_window(id, cornermask);
}

std::string HyprIso::class_name(int id) {
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif
    for (auto hyprwindow : hyprwindows) {
        if (hyprwindow->id == id) {
            if (auto w = hyprwindow->w.get()) {
                return w->m_class;
            }
        }
    }
 
    return "";
}

float HyprIso::get_rounding(int id) {
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif
    for (auto hw: hyprwindows)
        if (hw->id == id)
            return hw->w->rounding();
    return 0;
}

RGBA HyprIso::get_shadow_color(int id) {
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif
    for (auto hw: hyprwindows)
        if (hw->id == id) {
            auto c = hw->w->m_realShadowColor->value();
            return RGBA(c.r, c.g, c.b, c.a);
        }
    return {0, 0, 0, 1};
}

void HyprIso::set_float_state(int id, bool should_float) {
    for (auto hw: hyprwindows)
        if (hw->id == id)    
            change_float_state(hw->w, should_float);
}

int HyprIso::get_varint(std::string target, int default_float) {
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif
    //return default_float;

    auto confval = HyprlandAPI::getConfigValue(globals->api, target);
    if (!confval) {
        return default_float;
    }

    auto VAR = (Hyprlang::INT* const*)confval->getDataStaticPtr();
    return **VAR; 
}

float HyprIso::get_varfloat(std::string target, float default_float) {
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif
    //return default_float;
    
    auto confval = HyprlandAPI::getConfigValue(globals->api, target);
    if (!confval)
        return default_float;

    auto VAR = (Hyprlang::FLOAT* const*)confval->getDataStaticPtr();
    return **VAR; 
}

RGBA HyprIso::get_varcolor(std::string target, RGBA default_color) {
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif
    //return default_color;
    
    auto confval = HyprlandAPI::getConfigValue(globals->api, target);
    if (!confval)
        return default_color;

    auto VAR = (Hyprlang::INT* const*)confval->getDataStaticPtr();
    auto color = CHyprColor(**VAR);
    return RGBA(color.r, color.g, color.b, color.a);         
}

void HyprIso::create_config_variables() {
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif
    
    HyprlandAPI::addConfigValue(globals->api, "plugin:mylardesktop:titlebar_button_bg_hovered_color", Hyprlang::INT{*configStringToInt("rgba(202020ff)")});
    HyprlandAPI::addConfigValue(globals->api, "plugin:mylardesktop:titlebar_button_bg_pressed_color", Hyprlang::INT{*configStringToInt("rgba(111111ff)")});
    HyprlandAPI::addConfigValue(globals->api, "plugin:mylardesktop:titlebar_closed_button_bg_hovered_color", Hyprlang::INT{*configStringToInt("rgba(dd1111ff)")});
    HyprlandAPI::addConfigValue(globals->api, "plugin:mylardesktop:titlebar_closed_button_bg_pressed_color", Hyprlang::INT{*configStringToInt("rgba(880000ff)")});
    HyprlandAPI::addConfigValue(globals->api, "plugin:mylardesktop:titlebar_closed_button_icon_color_hovered_pressed", Hyprlang::INT{*configStringToInt("rgba(ffffffff)")});

    HyprlandAPI::addConfigValue(globals->api, "plugin:mylardesktop:titlebar_focused_color", Hyprlang::INT{*configStringToInt("rgba(000000ff)")});
    HyprlandAPI::addConfigValue(globals->api, "plugin:mylardesktop:titlebar_unfocused_color", Hyprlang::INT{*configStringToInt("rgba(222222ff)")});
    HyprlandAPI::addConfigValue(globals->api, "plugin:mylardesktop:titlebar_focused_text_color", Hyprlang::INT{*configStringToInt("rgba(ffffffff)")});
    HyprlandAPI::addConfigValue(globals->api, "plugin:mylardesktop:titlebar_unfocused_text_color", Hyprlang::INT{*configStringToInt("rgba(999999ff)")});
    
    HyprlandAPI::addConfigValue(globals->api, "plugin:mylardesktop:thumb_to_position_time", Hyprlang::FLOAT{355});
    HyprlandAPI::addConfigValue(globals->api, "plugin:mylardesktop:snap_helper_fade_in", Hyprlang::FLOAT{400});

    HyprlandAPI::addConfigValue(globals->api, "plugin:mylardesktop:titlebar_button_ratio", Hyprlang::FLOAT{1.4375});
    HyprlandAPI::addConfigValue(globals->api, "plugin:mylardesktop:titlebar_text_h", Hyprlang::FLOAT{15});
    HyprlandAPI::addConfigValue(globals->api, "plugin:mylardesktop:titlebar_icon_h", Hyprlang::FLOAT{21});
    HyprlandAPI::addConfigValue(globals->api, "plugin:mylardesktop:titlebar_button_icon_h", Hyprlang::FLOAT{13});

    HyprlandAPI::addConfigValue(globals->api, "plugin:mylardesktop:resize_edge_size", Hyprlang::FLOAT{10});
    
    HyprlandAPI::addConfigValue(globals->api, "plugin:mylardesktop:dock", Hyprlang::INT{3});
    HyprlandAPI::addConfigValue(globals->api, "plugin:mylardesktop:dock_color", Hyprlang::INT{*configStringToInt("rgba(00000088)")});
    HyprlandAPI::addConfigValue(globals->api, "plugin:mylardesktop:dock_sel_active_color", Hyprlang::INT{*configStringToInt("rgba(ffffff44)")});
    HyprlandAPI::addConfigValue(globals->api, "plugin:mylardesktop:dock_sel_press_color", Hyprlang::INT{*configStringToInt("rgba(ffffff44)")});
    HyprlandAPI::addConfigValue(globals->api, "plugin:mylardesktop:dock_sel_hover_color", Hyprlang::INT{*configStringToInt("rgba(ffffff44)")});
    HyprlandAPI::addConfigValue(globals->api, "plugin:mylardesktop:dock_sel_accent_color", Hyprlang::INT{*configStringToInt("rgba(ffffff88)")});

    HyprlandAPI::addConfigValue(globals->api, "plugin:mylardesktop:sel_color", Hyprlang::INT{*configStringToInt("rgba(ffffff44)")});
    HyprlandAPI::addConfigValue(globals->api, "plugin:mylardesktop:sel_border_color", Hyprlang::INT{*configStringToInt("rgba(ffffff44)")});
}

static void on_open_layer(PHLLS l) {
    for (auto hl : hyprlayers) {
        if (hl->l == l)
            return;
    }
    if (l->m_layer == ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND) {
        return;
    }
    auto hl = new HyprLayer;
    hl->id = unique_id++;
    //notify(fz("{} {}", l->m_layer, hl->id));
    hl->l = l;
    //log(fz("{} {} {} {} {} {}", l->m_position.x, l->m_position.y, l->m_realSize->goal().x, l->m_realSize->goal().y, l->m_realPosition->goal().x, l->m_realPosition->goal().y));
    hyprlayers.push_back(hl);

    if (hypriso->on_layer_open) {
       hypriso->on_layer_open(hl->id); 
    }
}

void on_layer_close(PHLLS l) {
    HyprLayer *hl = nullptr;
    int target_index = 0;
    for (int i = 0; i < hyprlayers.size(); i++) {
        auto hlt = hyprlayers[i];
        if (hlt->l == l) {
            target_index = i;
            hl = hlt;
        }
    }
    if (hl) {
        hypriso->on_layer_closed(hl->id);
        delete hl;
        hyprlayers.erase(hyprlayers.begin() + target_index);
    }
}

static int main_wake_pipe[2];
static std::vector<std::function<void()>> funcs;

void main_thread(std::function<void()> func) {
    funcs.push_back(func);
    write(main_wake_pipe[1], "x", 1);
}

void setup_wake_main_thread() {
    pipe2(main_wake_pipe, O_CLOEXEC | O_NONBLOCK);
    int fd = main_wake_pipe[0];
    uint32_t mask = WL_EVENT_READABLE;
    wl_event_loop_add_fd(g_pCompositor->m_wlEventLoop, fd, mask, [](int fd, uint32_t mask, void *data){
        char buf[64];
        read(main_wake_pipe[0], buf, sizeof buf);
        for (auto f : funcs)
            f();
        funcs.clear();
        return 0;
    }, nullptr);
}

void HyprIso::create_callbacks() {
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif    
    setup_wake_main_thread();

    for (auto m : g_pCompositor->m_monitors) {
        on_open_monitor(m);
    }

    for (auto w : g_pCompositor->m_windows) {
        on_open_window(w);
    }
    
    for (auto l : g_pCompositor->m_layers) {
        on_open_layer(l);
    }

    static auto openWindow  = HyprlandAPI::registerCallbackDynamic(globals->api, "openWindow", [](void* self, SCallbackInfo& info, std::any data) {
        if (hypriso->on_window_open) {
            auto w = std::any_cast<PHLWINDOW>(data); // todo getorcreate ref on our side
            on_open_window(w);
        }
    });
    static auto closeWindow1 = HyprlandAPI::registerCallbackDynamic(globals->api, "closeWindow", [](void* self, SCallbackInfo& info, std::any data) {
        if (hypriso->on_window_closed) {
            auto w = std::any_cast<PHLWINDOW>(data); // todo getorcreate ref on our side
            on_close_window(w);
        }
    });
    static auto windowTitle = HyprlandAPI::registerCallbackDynamic(globals->api, "windowTitle", [](void* self, SCallbackInfo& info, std::any data) {
        if (hypriso->on_title_change) {
            auto w = std::any_cast<PHLWINDOW>(data); // todo getorcreate ref on our side
            for (auto hw : hyprwindows) {
                if (hw->w == w) {
                    hypriso->on_title_change(hw->id);
                    break;
                }
            }
        }
    });

    static auto openLayer  = HyprlandAPI::registerCallbackDynamic(globals->api, "openLayer", [](void* self, SCallbackInfo& info, std::any data) {
        try {
            auto l = std::any_cast<PHLLS>(data); 
            on_open_layer(l);
        } catch (...) {
            notify("openLayer cast failed");
        }

        if (hypriso->on_layer_change) {
            hypriso->on_layer_change();
        }
    });
    static auto closeLayer = HyprlandAPI::registerCallbackDynamic(globals->api, "closeLayer", [](void* self, SCallbackInfo& info, std::any data) {
        try {
            auto l = std::any_cast<PHLLS>(data); 
            on_layer_close(l);
        } catch (...) {
            notify("closeLayer cast failed");
        }

        if (hypriso->on_layer_change) {
            hypriso->on_layer_change();
        }
    });
    
    static auto render = HyprlandAPI::registerCallbackDynamic(globals->api, "render", [](void* self, SCallbackInfo& info, std::any data) {
        //return;
        auto stage = std::any_cast<eRenderStage>(data);
        if (stage == eRenderStage::RENDER_PRE) {
            #ifdef TRACY_ENABLE
                FrameMarkStart("Render");
            #endif        
        }
        if (hypriso->on_render) {
            for (auto m : hyprmonitors) {
                if (m->m == g_pHyprOpenGL->m_renderData.pMonitor) {
                    hypriso->on_render(m->id, (int)stage);
                }
            }
        }
        if (stage == eRenderStage::RENDER_LAST_MOMENT) {
            #ifdef TRACY_ENABLE
                FrameMarkEnd("Render");
            #endif
        }
    });
    
    static auto mouseMove = HyprlandAPI::registerCallbackDynamic(globals->api, "mouseMove", [](void* self, SCallbackInfo& info, std::any data) {
        //return;
        auto consume = false;
        if (hypriso->on_mouse_move) {
            auto mouse = g_pInputManager->getMouseCoordsInternal();
            auto m     = g_pCompositor->getMonitorFromCursor();
            consume    = hypriso->on_mouse_move(0, mouse.x * m->m_scale, mouse.y * m->m_scale);
        }
        info.cancelled = consume;
    });

    static auto mouseButton = HyprlandAPI::registerCallbackDynamic(globals->api, "mouseButton", [](void* self, SCallbackInfo& info, std::any data) {
        auto e       = std::any_cast<IPointer::SButtonEvent>(data);
        auto consume = false;
        if (hypriso->on_mouse_press) {
            auto mouse = g_pInputManager->getMouseCoordsInternal();
            auto s     = g_pCompositor->getMonitorFromCursor()->m_scale;
            consume    = hypriso->on_mouse_press(e.mouse, e.button, e.state, mouse.x * s, mouse.y * s);
        }
        info.cancelled = consume;
    });

    static auto mouseAxis = HyprlandAPI::registerCallbackDynamic(globals->api, "mouseAxis", [](void* self, SCallbackInfo& info, std::any data) {
        bool consume = false;
        auto p       = std::any_cast<std::unordered_map<std::string, std::any>>(data);
        for (std::pair<const std::string, std::any> pair : p) {
            if (pair.first == "event") {
                auto axisevent = std::any_cast<IPointer::SAxisEvent>(pair.second);
                if (hypriso->on_scrolled) {
                    consume = hypriso->on_scrolled(0, axisevent.source, axisevent.axis, axisevent.relativeDirection, axisevent.delta, axisevent.deltaDiscrete, axisevent.mouse);
                }
            }
        }
        info.cancelled = consume;
    });

    static auto keyPress = HyprlandAPI::registerCallbackDynamic(globals->api, "keyPress", [](void* self, SCallbackInfo& info, std::any data) {
        auto consume = false;
        if (hypriso->on_key_press) {
            auto p = std::any_cast<std::unordered_map<std::string, std::any>>(data);
            for (std::pair<const std::string, std::any> pair : p) {
                if (pair.first == "event") {
                    auto skeyevent = std::any_cast<IKeyboard::SKeyEvent>(pair.second);
                    consume        = hypriso->on_key_press(0, skeyevent.keycode, skeyevent.state, skeyevent.updateMods);
                } else if (pair.first == "keyboard") {
                    auto ikeyboard = std::any_cast<Hyprutils::Memory::CSharedPointer<IKeyboard>>(pair.second);
                }
            }
        }
        info.cancelled = consume;
    });
    /*
    static auto monitorAdded = HyprlandAPI::registerCallbackDynamic(globals->api, "monitorAdded", [](void* self, SCallbackInfo& info, std::any data) {
        if (hypriso->on_monitor_open) {
            auto m = std::any_cast<PHLMONITOR>(data); // todo getorcreate ref on our side
            on_open_monitor(m);
        }
    });
    
    static auto monitorRemoved = HyprlandAPI::registerCallbackDynamic(globals->api, "monitorRemoved", [](void* self, SCallbackInfo& info, std::any data) {
        if (hypriso->on_monitor_closed) {
            auto m = std::any_cast<PHLMONITOR>(data); // todo getorcreate ref on our side
            on_close_monitor(m);
        }
    });
    */
    

    static auto configReloaded = HyprlandAPI::registerCallbackDynamic(globals->api, "configReloaded", [](void* self, SCallbackInfo& info, std::any data) {
        if (hypriso->on_config_reload) {
            hypriso->on_config_reload();
            
            Hyprlang::CConfigValue* val = g_pConfigManager->getHyprlangConfigValuePtr("input:follow_mouse");
            auto f = (Hyprlang::INT*)val->dataPtr();
            //initial_value = *f;
            *f = 2;
        }
    });

    for (auto e : g_pCompositor->m_workspaces) {
        auto hs = new HyprWorkspaces;
        hs->w = e;
        hs->id = unique_id++;
        hs->buffer = new CFramebuffer;
        hyprspaces.push_back(hs);
    }

    static auto createWorkspace = HyprlandAPI::registerCallbackDynamic(globals->api, "createWorkspace", [](void* self, SCallbackInfo& info, std::any data) {
        auto s = std::any_cast<CWorkspace*>(data)->m_self.lock();
        auto hs = new HyprWorkspaces;
        hs->w = s;
        hs->id = unique_id++;
        hs->buffer = new CFramebuffer;
        hyprspaces.push_back(hs);
    });

    static auto destroyWorkspace = HyprlandAPI::registerCallbackDynamic(globals->api, "destroyWorkspace", [](void* self, SCallbackInfo& info, std::any data) {
        auto s = std::any_cast<CWorkspace*>(data);
        for (int i = hyprspaces.size() - 1; i >= 0; i--) {
            auto hs = hyprspaces[i];
            bool remove = false;
            if (!hs->w.lock()) {
                remove = true;
            } else if (hs->w.get() == s) {
                remove = true;
            }
            if (remove) {
                delete hs->buffer;
                hyprspaces.erase(hyprspaces.begin() + i);
            }
        }
    });
    static auto workspace = HyprlandAPI::registerCallbackDynamic(globals->api, "workspace", [](void* self, SCallbackInfo& info, std::any data) {
        //notify("workspace");
        if (hypriso->on_mouse_move) {
            auto mouse = g_pInputManager->getMouseCoordsInternal();
            auto m = g_pCompositor->getMonitorFromCursor();
            hypriso->on_mouse_move(0, mouse.x * m->m_scale, mouse.y * m->m_scale);
        }
        if (hypriso->on_workspace_change) {
            auto w = std::any_cast<PHLWORKSPACE>(data); 
            for (auto space : hyprspaces) {
                if (space->w == w) {
                    hypriso->on_workspace_change(space->id);
                }
            }
        }
    });

    static auto activeWindow = HyprlandAPI::registerCallbackDynamic(globals->api, "activeWindow", [](void* self, SCallbackInfo& info, std::any data) {
        auto p = std::any_cast<PHLWINDOW>(data);
        if (hypriso->on_activated) {
            for (auto h : hyprwindows) {
                if (h->w == p) {
                    hypriso->on_activated(h->id);
                }
            }
        }
    });
}

inline CFunctionHook* g_pOnArrangeMonitors = nullptr;
typedef void (*origArrangeMonitors)(CCompositor *);
void hook_onArrangeMonitors(void* thisptr) {
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif
    auto spe = (CCompositor *) thisptr;
    (*(origArrangeMonitors) g_pOnArrangeMonitors->m_original)(spe);
    //notify("Some change in monitors");
    // Go through all and check any that no longer exist or that need to exist

    //return;
    for (const auto &m : g_pCompositor->m_monitors) {
        bool already_in = false;
        for (auto hm : hyprmonitors) {
            if (hm->m == m) {
                already_in = true;
            }
        }

        if (!already_in) {
            on_open_monitor(m);
        }
    }
    for (int i = hyprmonitors.size() - 1; i >= 0; i--) {
        auto hm = hyprmonitors[i];
        bool in = false;
        for (const auto &m : g_pCompositor->m_monitors) {
            if (hm->m == m) {
                in = true;
            }
        }

        if (!in) {
            on_close_monitor(hm->m);
        }
    }
}

void hook_monitor_arrange() {
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif
    return;
    static const auto METHODS = HyprlandAPI::findFunctionsByName(globals->api, "arrangeMonitors");
    for (auto m : METHODS) {
        if (m.signature.find("CCompositor") != std::string::npos) {
            g_pOnArrangeMonitors = HyprlandAPI::createFunctionHook(globals->api, m.address, (void*)&hook_onArrangeMonitors);
            g_pOnArrangeMonitors->hook();
            return;
        }
    }
}

inline CFunctionHook* g_pOnArrangeLayers = nullptr;
typedef void (*origArrangeLayers)(CHyprRenderer *, const MONITORID& monitor);
void hook_onArrangeLayers(void* thisptr, const MONITORID& monitor) {
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif
    auto spe = (CHyprRenderer *) thisptr;
    (*(origArrangeLayers)g_pOnArrangeLayers->m_original)(spe, monitor);
    //notify("dock added");
}

void hook_dock_change() {
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif
    //g_pHyprRenderer->arrangeLayersForMonitor(m_id);
    static const auto METHODS = HyprlandAPI::findFunctionsByName(globals->api, "arrangeLayersForMonitor");
    // TODO: check if m.address is same as set_rounding even though signature is SurfacePassElement
    for (auto m : METHODS) {
        if (m.signature.find("CHyprRenderer") != std::string::npos) {
            //notify(m.demangled);
            //notify(m.demangled);
            //g_pOnArrangeLayers = HyprlandAPI::createFunctionHook(globals->api, m.address, (void*)&hook_onArrangeLayers);
            //g_pOnArrangeLayers->hook();
            return;
        }
    }
}

//void CHyprDropShadowDecoration::render(PHLMONITOR pMonitor, float const& a) {
//const auto PWINDOW = m_window.lock();
inline CFunctionHook* g_pOnShadowRender = nullptr;
typedef void (*origShadowRender)(CHyprDropShadowDecoration *, PHLMONITOR pMonitor, float const& a);
void hook_onShadowRender(void* thisptr, PHLMONITOR pMonitor, float const& a) {
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif
    auto ptr = (CHyprDropShadowDecoration *) thisptr;
    
    static auto PSHADOWSIZE = CConfigValue<Hyprlang::INT>("decoration:shadow:range");
    static auto PSHADOWS = CConfigValue<Hyprlang::INT>("decoration:shadow:enabled");

    int before = *PSHADOWSIZE;
    bool before_shadows = *PSHADOWS;
    if (auto w = ptr->m_window.lock()) {
        bool has_focus = w == Desktop::focusState()->window();
        if (has_focus) {
            *PSHADOWSIZE.ptr() = (int) (before * 2.3);
        }
        if (hypriso->is_snapped) {
            for (auto h : hyprwindows) {
                if (h->w == w) {
                    if (hypriso->is_snapped(h->id)) {
                        *PSHADOWS.ptr() = 0;
                    }
                }
            }
        }
    }
    (*(origShadowRender)g_pOnShadowRender->m_original)(ptr, pMonitor, a);
    *PSHADOWSIZE.ptr() = before;
    *PSHADOWS.ptr() = before_shadows;
}
 
void hook_shadow_decorations() {
    static const auto METHODS = HyprlandAPI::findFunctionsByName(globals->api, "render");
    for (auto m : METHODS) {
        if (m.signature.find("CHyprDropShadowDecoration") != std::string::npos) {
            g_pOnShadowRender = HyprlandAPI::createFunctionHook(globals->api, m.address, (void*)&hook_onShadowRender);
            g_pOnShadowRender->hook();
            return;
        }
    }
    
}

void hook_popup_creation_and_destruction();

void onUpdateState(Desktop::View::CWindow *ptr) {
    std::optional<bool>      requestsFS = ptr->m_xdgSurface ? ptr->m_xdgSurface->m_toplevel->m_state.requestsFullscreen : ptr->m_xwaylandSurface->m_state.requestsFullscreen;
    std::optional<MONITORID> requestsID = ptr->m_xdgSurface ? ptr->m_xdgSurface->m_toplevel->m_state.requestsFullscreenMonitor : MONITOR_INVALID;
    std::optional<bool>      requestsMX = ptr->m_xdgSurface ? ptr->m_xdgSurface->m_toplevel->m_state.requestsMaximize : ptr->m_xwaylandSurface->m_state.requestsMaximize;
    std::optional<bool>      requestsMin = ptr->m_xdgSurface ? ptr->m_xdgSurface->m_toplevel->m_state.requestsMinimize : ptr->m_xwaylandSurface->m_state.requestsMinimize;
    

    if (requestsFS.has_value() && !(ptr->m_suppressedEvents & Desktop::View::eSuppressEvents::SUPPRESS_FULLSCREEN)) {
        if (requestsID.has_value() && (requestsID.value() != MONITOR_INVALID) && !(ptr->m_suppressedEvents & Desktop::View::eSuppressEvents::SUPPRESS_FULLSCREEN_OUTPUT)) {
            if (ptr->m_isMapped) {
                const auto monitor = g_pCompositor->getMonitorFromID(requestsID.value());
                g_pCompositor->moveWindowToWorkspaceSafe(ptr->m_self.lock(), monitor->m_activeWorkspace);
                Desktop::focusState()->rawMonitorFocus(monitor);
            }

            if (!ptr->m_isMapped)
                ptr->m_wantsInitialFullscreenMonitor = requestsID.value();
        }

        bool fs = requestsFS.value();
        if (ptr->m_isMapped)
            g_pCompositor->changeWindowFullscreenModeClient(ptr->m_self.lock(), FSMODE_FULLSCREEN, requestsFS.value());

        if (!ptr->m_isMapped)
            ptr->m_wantsInitialFullscreen = fs;
    }

    if (requestsMX.has_value() && !(ptr->m_suppressedEvents & Desktop::View::eSuppressEvents::SUPPRESS_MAXIMIZE)) {
        if (ptr->m_isMapped) {
            //auto window    = ptr->m_self.lock();
            //auto state     = sc<int8_t>(window->m_fullscreenState.client);
            //bool maximized = (state & sc<uint8_t>(FSMODE_MAXIMIZED)) != 0;
            //g_pCompositor->changeWindowFullscreenModeClient(window, FSMODE_MAXIMIZED, !maximized);
        }
    }

    if (requestsMX.has_value() || requestsMin.has_value()) {
        if (hypriso->on_requests_max_or_min) {
            for (auto hw : hyprwindows) {
                if (hw->w.get() == ptr) {
                    int want = 0; // max
                    if (requestsMin.has_value())
                        want = 1;
                    hypriso->on_requests_max_or_min(hw->id, want);
                    break;
                }
            }
        }
    }
}

inline CFunctionHook* g_pOnUpdateStateHook = nullptr;
typedef void (*origUpdateState)(Desktop::View::CWindow *);
void hook_onUpdateState(void* thisptr) {
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif
#ifdef FORK_WARN
    //void CWindow::onUpdateState() {
    static_assert(false, "[Function Body] Make sure our `onUpdateState` and Hyprland's are synced!");
#endif
    onUpdateState((Desktop::View::CWindow *) thisptr);
}

void hook_maximize_minimize() {
    {
        static const auto METHODS = HyprlandAPI::findFunctionsByName(globals->api, "onUpdateState");
        for (auto m : METHODS) {
            if (m.signature.find("CWindow") != std::string::npos) {
                g_pOnUpdateStateHook = HyprlandAPI::createFunctionHook(globals->api, m.address, (void*)&hook_onUpdateState);
                g_pOnUpdateStateHook->hook();
                break;
            }
        }
    }
}

// {"anchors":[{"x":0,"y":1},{"x":0.525,"y":0.4},{"x":1,"y":0}],"controls":[{"x":0.45237856557239087,"y":0.8590353902180988},{"x":0.7485689587985109,"y":0.05514647589789487}]}
static std::vector<float> fadein = { 0, 0.0050000000000000044, 0.01100000000000001, 0.017000000000000015, 0.02300000000000002, 0.030000000000000027, 0.03700000000000003, 0.04500000000000004, 0.052000000000000046, 0.061000000000000054, 0.06999999999999995, 0.07899999999999996, 0.08899999999999997, 0.09899999999999998, 0.10999999999999999, 0.122, 0.135, 0.14800000000000002, 0.16300000000000003, 0.17800000000000005, 0.19399999999999995, 0.21199999999999997, 0.23099999999999998, 0.252, 0.275, 0.30000000000000004, 0.32699999999999996, 0.359, 0.394, 0.43600000000000005, 0.487, 0.554, 0.613, 0.638, 0.661, 0.6839999999999999, 0.7070000000000001, 0.728, 0.748, 0.768, 0.786, 0.804, 0.821, 0.838, 0.853, 0.868, 0.882, 0.895, 0.907, 0.919, 0.9299999999999999, 0.94, 0.949, 0.958, 0.966, 0.973, 0.98, 0.986, 0.991, 0.996 };

float pull(std::vector<float>& fls, float scalar) {
    if (fls.empty())
        return 0.0f; // or throw an exception

    // Clamp scalar between 0 and 1
    scalar = std::clamp(scalar, 0.0f, 1.0f);

    float fIndex = scalar * (fls.size() - 1); // exact position
    int   i0     = static_cast<int>(std::floor(fIndex));
    int   i1     = static_cast<int>(std::ceil(fIndex));

    if (i0 == i1 || i1 >= fls.size()) {
        return fls[i0];
    }

    float t = fIndex - i0; // fraction between the two indices
    return fls[i0] * (1.0f - t) + fls[i1] * t;
}

// {"anchors":[{"x":0,"y":1},{"x":1,"y":0}],"controls":[{"x":0.29521329248987305,"y":-0.027766935560437862}]}
static std::vector<float> zoomin = { 0, 0.05600000000000005, 0.10899999999999999, 0.15800000000000003, 0.20499999999999996, 0.249, 0.29000000000000004, 0.32899999999999996, 0.366, 0.402, 0.43500000000000005, 0.46699999999999997, 0.497, 0.526, 0.554, 0.5800000000000001, 0.605, 0.628, 0.651, 0.673, 0.6930000000000001, 0.7130000000000001, 0.732, 0.749, 0.766, 0.782, 0.798, 0.812, 0.8260000000000001, 0.84, 0.852, 0.864, 0.875, 0.886, 0.896, 0.906, 0.915, 0.923, 0.931, 0.938, 0.945, 0.952, 0.958, 0.963, 0.969, 0.973, 0.978, 0.982, 0.985, 0.988, 0.991, 0.993, 0.995, 0.997, 0.998, 1, 1, 1.001, 1.001, 1.001 };



bool rendered_splash_screen(CBox &monbox, PHLMONITORREF mon) {
    if (previously_seen_instance_signature == g_pCompositor->m_instanceSignature)
        return false;
    for (auto h : hyprmonitors) {
        if (h->m == mon) {
            auto current = get_current_time_in_ms();
            long delta = current - h->creation_time;
            if (h->first && delta > 2000) {
                h->first = false;
                const char* home = std::getenv("HOME");
                if (home) {
                    std::filesystem::path filepath = std::filesystem::path(home) / ".config/mylar/chime.wav";
                    system(fz("mpv \"{}\" &", filepath.string()).c_str());
                }
            }
            float scalar = (delta - 3000.0f) / 1000.0f;
            if (scalar < 1.0) {
                CBox box = {0, 0, h->m->m_transformedSize.x, h->m->m_transformedSize.x};
                CHyprColor color = {1, 1, 1, .04f * (1.0f - pull(fadein, scalar))};
                CHyprOpenGLImpl::SRectRenderData rectdata;
                auto region = new CRegion(box);
                rectdata.damage = region;
                rectdata.blur = false;
                rectdata.blurA = 1.0;
                rectdata.round = 0;
                rectdata.roundingPower = 2.0f;
                rectdata.xray = false;
                color = {0, 0, 0, 1.0f * (1.0f - pull(fadein, scalar))};
                g_pHyprOpenGL->renderRect(box, color, rectdata);
                /*auto pancake_scalar = pull(zoomin, scalar * 3);
                auto sfactor = 200;
                monbox.y += sfactor * (1.0f - pancake_scalar);
                monbox.h -= sfactor * 2 * (1.0f - pancake_scalar);
                */
                monbox.scaleFromCenter(1.0 + (0.05 * (1.0 - pull(zoomin, scalar)))); 
                hypriso->damage_entire(h->id);
                return true;
            }
        }
    }

    return false;
}

void monitor_finish(CHyprOpenGLImpl *ptr) {
    static auto PZOOMDISABLEAA = CConfigValue<Hyprlang::INT>("cursor:zoom_disable_aa");

    TRACY_GPU_ZONE("RenderEnd");

    // end the render, copy the data to the main framebuffer
    if (ptr->m_offloadedFramebuffer) {
        ptr->m_renderData.damage = ptr->m_renderData.finalDamage;
        ptr->pushMonitorTransformEnabled(true);

        CBox monbox = {0, 0, ptr->m_renderData.pMonitor->m_transformedSize.x, ptr->m_renderData.pMonitor->m_transformedSize.y};

        bool rendering_splash = false;
        if (!rendered_splash_screen(monbox, ptr->m_renderData.pMonitor)) {
            if (g_pHyprRenderer->m_renderMode == RENDER_MODE_NORMAL && ptr->m_renderData.mouseZoomFactor == 1.0f)
                ptr->m_renderData.pMonitor->m_zoomController.m_resetCameraState = true;
            ptr->m_renderData.pMonitor->m_zoomController.applyZoomTransform(monbox, ptr->m_renderData);            
        } else {
            rendering_splash = true;
        }

        ptr->m_applyFinalShader = !ptr->m_renderData.blockScreenShader;
        if (ptr->m_renderData.mouseZoomUseMouse && *PZOOMDISABLEAA)
            ptr->m_renderData.useNearestNeighbor = true;
        
        if (rendering_splash)
            ptr->m_renderData.useNearestNeighbor = false;

        // copy the damaged areas into the mirror buffer
        // we can't use the offloadFB for mirroring, as it contains artifacts from blurring
        if (!ptr->m_renderData.pMonitor->m_mirrors.empty() && !ptr->m_fakeFrame)
            ptr->saveBufferForMirror(monbox);

        ptr->m_renderData.outFB->bind();
        ptr->blend(false);

        if (ptr->m_finalScreenShader.program < 1 && !g_pHyprRenderer->m_crashingInProgress)
            ptr->renderTexturePrimitive(ptr->m_renderData.pCurrentMonData->offloadFB.getTexture(), monbox);
        else
            ptr->renderTexture(ptr->m_renderData.pCurrentMonData->offloadFB.getTexture(), monbox, {});

        ptr->blend(true);

        ptr->m_renderData.useNearestNeighbor = false;
        ptr->m_applyFinalShader              = false;
        ptr->popMonitorTransformEnabled();
    }

    // reset our data
    ptr->m_renderData.pMonitor.reset();
    ptr->m_renderData.mouseZoomFactor   = 1.f;
    ptr->m_renderData.mouseZoomUseMouse = true;
    ptr->m_renderData.blockScreenShader = false;
    ptr->m_renderData.currentFB         = nullptr;
    ptr->m_renderData.mainFB            = nullptr;
    ptr->m_renderData.outFB             = nullptr;
    ptr->popMonitorTransformEnabled();

    // if we dropped to offMain, release it now.
    // if there is a plugin constantly using it, this might be a bit slow,
    // but I haven't seen a single plugin yet use these, so it's better to drop a bit of vram.
    if (ptr->m_renderData.pCurrentMonData->offMainFB.isAllocated())
        ptr->m_renderData.pCurrentMonData->offMainFB.release();

    // check for gl errors
    const GLenum ERR = glGetError();

    if (ERR == GL_CONTEXT_LOST) /* We don't have infra to recover from this */
        RASSERT(false, "glGetError at Opengl::end() returned GL_CONTEXT_LOST. Cannot continue until proper GPU reset handling is implemented.");
}

inline CFunctionHook* g_pOnMonitorEndHook = nullptr;
typedef void (*origMonitorEnd)(CHyprOpenGLImpl *);
void hook_onMonitorEnd(void* thisptr) {
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif
#ifdef FORK_WARN
    static_assert(false, "[Function Body] Make sure our `CHyprOpenGLImpl::end` and Hyprland's are synced!");
#endif
    monitor_finish((CHyprOpenGLImpl *) thisptr);
}

void hook_monitor_render() {
    if (true) {
        static const auto METHODS = HyprlandAPI::findFunctionsByName(globals->api, "end");
        for (auto m : METHODS) {
            if (m.demangled.find("CHyprOpenGLImpl::end") != std::string::npos) {
                g_pOnMonitorEndHook = HyprlandAPI::createFunctionHook(globals->api, m.address, (void*)&hook_onMonitorEnd);
                g_pOnMonitorEndHook->hook();
                break;
            }
        }
    }
}

std::string get_previous_instance_signature() {
    std::string previous = "";
    // Resolve $HOME
    const char* home = std::getenv("HOME");
    if (!home)
        return previous;

    std::filesystem::path filepath = std::filesystem::path(home) / ".config/mylar/last_seen_instance_signature.txt";
    std::filesystem::create_directories(filepath.parent_path());

    {
        std::ifstream file(filepath);
        if (file) {
            std::string line;
            std::getline(file, line);
            previous = line;
        }
    }

    {
        std::ofstream out(filepath, std::ios::trunc);
        if (!out)
            return previous;
        out << g_pCompositor->m_instanceSignature << "\n";
    }

    return previous; 
}

void screenshot_window_with_decos(CFramebuffer* buffer, PHLWINDOW w);

inline CFunctionHook* g_pOnSetHiddenHook = nullptr;
typedef void (*origSetHidden)(Desktop::View::CWindow *, bool);
void hook_onSetHidden(void* thisptr, bool state) {
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif
#ifdef FORK_WARN
    static_assert(false, "[Function Body] Make sure our `CHyprOpenGLImpl::end` and Hyprland's are synced!");
#endif
    auto w = (Desktop::View::CWindow *) thisptr;
    for (auto hw : hyprwindows) {
        if  (hw->w.get() == w) {
            if (state) {
                // set to hide
                if (!hw->min_fb)
                    hw->min_fb = new CFramebuffer;
                screenshot_window_with_decos(hw->min_fb, hw->w);
                hw->w_min_mon = {0, 0, hw->w->m_monitor->m_pixelSize.x, hw->w->m_monitor->m_pixelSize.y};
                hw->w_min_size = tobounds(w->getFullWindowBoundingBox());
                hw->w_min_size.x -= hw->w->m_monitor->m_position.x;
                hw->w_min_size.y -= hw->w->m_monitor->m_position.y;
                hw->w_min_size.scale(w->m_monitor->m_scale);
                hw->w_min_raw = tobounds(w->getFullWindowBoundingBox());
                hw->w_min_raw.x -= hw->w->m_monitor->m_position.x;
                hw->w_min_raw.y -= hw->w->m_monitor->m_position.y;
            } else {
                // set to show
                hw->unminize_start = get_current_time_in_ms(); 
            }
        }
    }
    (*(origSetHidden)g_pOnSetHiddenHook->m_original)(w, state);
}

void hook_hidden_state_change() {
#ifdef FORK_WARN
    static_assert(false, "[Function Body] Make sure our `CWindow::setHidden(bool)` and Hyprland's are synced!");
#endif

    static const auto METHODS = HyprlandAPI::findFunctionsByName(globals->api, "setHidden");
    for (auto m : METHODS) {
        if (m.demangled.find("CWindow::setHidden") != std::string::npos) {
            g_pOnSetHiddenHook = HyprlandAPI::createFunctionHook(globals->api, m.address, (void*)&hook_onSetHidden);
            g_pOnSetHiddenHook->hook();
            break;
        }
    }
}

static std::string default_conf = R"(
#
# ----THIS FILE IS AUTOGENERATED, DO NOT MODIFY---
#
# ----USE ~/.config/mylar/user.conf---
#

monitor=,preferred,auto,auto

$terminal = alacritty
$fileManager = dolphin
$menu = rofi -show run

general:gaps_in = 5;
general:gaps_out = 20;
general:border_size = 1
general:col.active_border = rgba(888888ff)
general:col.inactive_border = rgba(595959ff)
general:resize_on_border = false # Hyprland border, not Mylar border
general:allow_tearing = false
general:layout = master

dwindle {
    pseudotile = true # Master switch for pseudotiling. Enabling is bound to mainMod + P in the keybinds section below
    preserve_split = true # You probably want this
}

master {
    new_status = master
    mfact = .55
}

misc {
    force_default_wallpaper = 0 # Set to 0 or 1 to disable the anime mascot wallpapers
    disable_hyprland_logo = true # If true disables the random hyprland logo / anime girl background. :(
}

decoration:rounding = 9
decoration:rounding_power = 2.0

decoration:shadow:enabled = true
decoration:shadow:sharp = false
decoration:shadow:range = 12
decoration:shadow:render_power = 2
decoration:shadow:color = rgba(00000020)

decoration:blur:enabled = true
decoration:blur:size = 3
decoration:blur:passes = 5
decoration:blur:noise = .05
decoration:blur:vibrancy = 0.4696

layerrule = match:class Dock, blur on

input:follow_mouse = 2

gesture = 3, horizontal, workspace

cursor:no_warps = true

ecosystem:no_update_news = true
ecosystem:no_donation_nag = true

cursor:zoom_detached_camera = 1
cursor:zoom_disable_aa = true

windowrule = match:class .*, float on

$mainMod = Alt
bind = $mainMod, Q, exec, $terminal
bind = Alt SHIFT, Return, exec, $terminal
bind = Alt SHIFT, C, killactive,
bind = $mainMod, F4, killactive,
bind = $mainMod, M, exit,
bind = $mainMod, V, togglefloating,
bind = $mainMod, P, pseudo, # dwindle
bind = $mainMod, J, togglesplit, # dwindle

bind = $mainMod, 1, workspace, 1
bind = $mainMod, 2, workspace, 2
bind = $mainMod, 3, workspace, 3
bind = $mainMod, 4, workspace, 4
bind = $mainMod, 5, workspace, 5
bind = $mainMod, 6, workspace, 6
bind = $mainMod, 7, workspace, 7
bind = $mainMod, 8, workspace, 8
bind = $mainMod, 9, workspace, 9
bind = $mainMod, 0, workspace, 10

# Move active window to a workspace with mainMod + SHIFT + [0-9]
bind = $mainMod SHIFT, 1, movetoworkspace, 1
bind = $mainMod SHIFT, 2, movetoworkspace, 2
bind = $mainMod SHIFT, 3, movetoworkspace, 3
bind = $mainMod SHIFT, 4, movetoworkspace, 4
bind = $mainMod SHIFT, 5, movetoworkspace, 5
bind = $mainMod SHIFT, 6, movetoworkspace, 6
bind = $mainMod SHIFT, 7, movetoworkspace, 7
bind = $mainMod SHIFT, 8, movetoworkspace, 8
bind = $mainMod SHIFT, 9, movetoworkspace, 9
bind = $mainMod SHIFT, 0, movetoworkspace, 10

# scratchpad
bind = $mainMod SHIFT, S, movetoworkspace, special:magic
bind = $mainMod, S, togglespecialworkspace, magic

bind = $mainMod SHIFT, P, exec, rofi -show run

bind = $mainMod, Z, exec, hyprctl dispatch layoutmsg swapwithmaster
binde = $mainMod, H, exec, hyprctl dispatch layoutmsg mfact -0.05
binde = $mainMod, L, exec, hyprctl dispatch layoutmsg mfact 0.05
bind = $mainMod, j, layoutmsg, cyclenext
bind = $mainMod, k, layoutmsg, cycleprev

# Move/resize windows with mainMod + LMB/RMB and dragging
bindm = $mainMod, mouse:272, movewindow
bindm = $mainMod, mouse:273, resizewindow

bindel = ,XF86AudioRaiseVolume, exec, wpctl set-volume -l 1 @DEFAULT_AUDIO_SINK@ 5%+
bindel = ,XF86AudioLowerVolume, exec, wpctl set-volume @DEFAULT_AUDIO_SINK@ 5%-
bindel = ,XF86AudioMute, exec, wpctl set-mute @DEFAULT_AUDIO_SINK@ toggle
bindel = ,XF86AudioMicMute, exec, wpctl set-mute @DEFAULT_AUDIO_SOURCE@ toggle
bindel = ,XF86MonBrightnessUp, exec, brightnessctl -e4 -n2 set 5%+
bindel = ,XF86MonBrightnessDown, exec, brightnessctl -e4 -n2 set 5%-

# Requires playerctl
bindl = , XF86AudioNext, exec, playerctl next
bindl = , XF86AudioPause, exec, playerctl play-pause
bindl = , XF86AudioPlay, exec, playerctl play-pause
bindl = , XF86AudioPrev, exec, playerctl previous

xwayland {
  force_zero_scaling = true
}

debug:disable_scale_checks = true


# https://wiki.hypr.land/Configuring/Variables/#animations
animations {
    enabled = yes, please :)

    # Default curves, see https://wiki.hypr.land/Configuring/Animations/#curves
    #        NAME,           X0,   Y0,   X1,   Y1
    bezier = bangercurve,   0.51, 0.55, 0.51, 0.92
    #bezier = bangercurve,   0.51, 0.55, 0.51, 0.92
    bezier = easeOutSine,   0.61, 1, 0.88, 1
    bezier = easeOutCirc,   0, 0.55, 0.45, 1
    bezier = easeInOutSine,   0.37, 0, 0.63, 1
    bezier = easeOutQuint,   0.23, 1,    0.32, 1
    bezier = easeInOutCubic, 0.65, 0.05, 0.36, 1
    bezier = linear,         0,    0,    1,    1
    bezier = almostLinear,   0.5,  0.5,  0.75, 1
    bezier = quick,          0.15, 0,    0.1,  1

    # Default animations, see https://wiki.hypr.land/Configuring/Animations/
    #           NAME,          ONOFF, SPEED, CURVE,        [STYLE]
    animation = global,        1,     20,    default
    animation = border,        1,     5.39,  easeOutQuint
    animation = windows,       1,     4.60, easeOutQuint
    animation = windowsIn,     1,     0.0001,   easeOutQuint, popin 96%
    animation = windowsOut,    1,     2.49,  linear,       popin 96%
    animation = fadeIn,        1,     0.0001,  easeInOutSine
    animation = fadeOut,       1,     1.46,  almostLinear
    animation = fade,          1,     0.001,  almostLinear 
    animation = layers,        1,     3.81,  easeOutQuint
    animation = layersIn,      1,     4,     easeOutQuint, fade
    animation = layersOut,     1,     1.5,   linear,       fade
    animation = fadeLayersIn,  1,     1.79,  almostLinear
    animation = fadeLayersOut, 1,     1.39,  almostLinear
    animation = workspaces,    1,     5.01,  bangercurve, slide
    animation = workspacesIn,  1,     5.01,  bangercurve, slide
    animation = workspacesOut, 1,     5.01,  bangercurve, slide
    animation = zoomFactor,    1,     2,     linear
    animation = monitorAdded,  1,     9,    easeOutQuint
}


# snap helper
plugin:mylardesktop:snap_helper_fade_in = 173
plugin:mylardesktop:thumb_to_position_time = 355

# resize
plugin:mylardesktop:resize_edge_size = 20

# titlebar
plugin:mylardesktop:titlebar_focused_color = rgba(000000ff)
plugin:mylardesktop:titlebar_unfocused_color = rgba(222222ff)
plugin:mylardesktop:titlebar_focused_text_color = rgba(ffffffff)
plugin:mylardesktop:titlebar_unfocused_text_color = rgba(ffffffaa)

plugin:mylardesktop:titlebar_button_bg_hovered_color = rgba(303030ff)
plugin:mylardesktop:titlebar_button_bg_pressed_color = rgba(202020ff)

plugin:mylardesktop:titlebar_closed_button_bg_hovered_color = rgba(dd1111ff)
plugin:mylardesktop:titlebar_closed_button_bg_pressed_color = rgba(880000ff)
plugin:mylardesktop:titlebar_closed_button_icon_color_hovered_pressed = rgba(ffffffff)

plugin:mylardesktop:titlebar_button_ratio = 1.4375
plugin:mylardesktop:titlebar_text_h = 13
plugin:mylardesktop:titlebar_icon_h = 19
plugin:mylardesktop:titlebar_button_icon_h = 12

plugin:mylardesktop:dock = 3
#plugin:mylardesktop:dock_color = rgba(78228877)
plugin:mylardesktop:dock_color = rgba(00000022)
plugin:mylardesktop:dock_sel_active_color = rgba(ffffff33)
plugin:mylardesktop:dock_sel_hover_color = rgba(ffffff50)
plugin:mylardesktop:dock_sel_press_color = rgba(ffffff44)
plugin:mylardesktop:dock_sel_accent_color = rgba(ffffffff)

plugin:mylardesktop:sel_color = rgba(ffffff11)
plugin:mylardesktop:sel_border_color = rgba(ffffff11)



source = ~/.config/mylar/user.conf

)";

static bool return_default_config = false;

inline CFunctionHook* g_pOnGetMainConfigPathHook = nullptr;
typedef std::string (*origGetMainConfigPath)(CConfigManager *);
std::string hook_onGetMainConfigPath(void* thisptr) {
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif
#ifdef FORK_WARN
    static_assert(false, "[Function Body] Make sure our `CConfigManager::getMainConfigPath` and Hyprland's are synced!");
#endif
    const char* home = std::getenv("HOME");
    if (!home || return_default_config)
        return (*(origGetMainConfigPath)g_pOnGetMainConfigPathHook->m_original)((CConfigManager *) thisptr);

    std::filesystem::path filepath = std::filesystem::path(home) / ".config/mylar/default.conf";
    std::filesystem::create_directories(filepath.parent_path());
    
    {
        std::ofstream out(filepath, std::ios::trunc);
        out << default_conf << std::endl;
    }
    
    {
        std::filesystem::path f = std::filesystem::path(home) / ".config/mylar/user.conf";
        std::filesystem::create_directories(f.parent_path());
        if (!std::filesystem::exists(f)) {
            std::ofstream out(f, std::ios::trunc);
            out << std::endl << std::endl;
        }
    }

    return filepath.string();
}

void hook_default_config() {
    static const auto METHODS = HyprlandAPI::findFunctionsByName(globals->api, "getMainConfigPath");
    for (auto m : METHODS) {
        if (m.demangled.find("CConfigManager::getMainConfigPath") != std::string::npos) {
            g_pOnGetMainConfigPathHook = HyprlandAPI::createFunctionHook(globals->api, m.address, (void*)&hook_onGetMainConfigPath);
            g_pOnGetMainConfigPathHook->hook();
            break;
        }
    }
}

void HyprIso::create_hooks() {
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif
    previously_seen_instance_signature = get_previous_instance_signature();
    //return;
    detect_csd_request_change();
    fix_window_corner_rendering();
    hook_shadow_decorations();
    //disable_default_alt_tab_behaviour();
    detect_x11_move_resize_requests();    
    overwrite_min();
    hook_render_functions();
    overwrite_defaults();
    interleave_floating_and_tiled_windows();
    hook_maximize_minimize();
    hook_dock_change();
    hook_monitor_arrange();
    hook_popup_creation_and_destruction();
    hook_monitor_render();
    hook_hidden_state_change();
    hook_default_config();
}

bool xcb_get_transient_for(xcb_connection_t* conn, xcb_window_t window, xcb_window_t* out) {
    xcb_get_property_cookie_t cookie = xcb_get_property(conn, 0, window, XCB_ATOM_WM_TRANSIENT_FOR, XCB_ATOM_WINDOW, 0, 1);
    xcb_get_property_reply_t* reply = xcb_get_property_reply(conn, cookie, NULL);

    if (!reply)
        return false;

    bool ok = false;
    if (xcb_get_property_value_length(reply) == sizeof(xcb_window_t)) {
        if (out)
            *out = *(xcb_window_t*)xcb_get_property_value(reply);
        ok = true;
    }

    free(reply);
    return ok;
}

void client_get_type_and_transientness(HyprWindow* self) {
    guint num, i;
    guint32 *val;

    self->type = (ObClientType) -1;
    self->transient = false;
    
    for (const auto& a : self->w->m_xwaylandSurface->m_atoms) {
        if (a == HYPRATOMS["NET_WM_WINDOW_TYPE_DESKTOP"])
            self->type = OB_CLIENT_TYPE_DESKTOP;
        else if (a == HYPRATOMS["NET_WM_WINDOW_TYPE_DOCK"])
            self->type = OB_CLIENT_TYPE_DOCK;
        else if (a == HYPRATOMS["NET_WM_WINDOW_TYPE_TOOLBAR"])
            self->type = OB_CLIENT_TYPE_TOOLBAR;
        else if (a == HYPRATOMS["NET_WM_WINDOW_TYPE_MENU"])
            self->type = OB_CLIENT_TYPE_MENU;
        else if (a == HYPRATOMS["NET_WM_WINDOW_TYPE_UTILITY"])
            self->type = OB_CLIENT_TYPE_UTILITY;
        else if (a == HYPRATOMS["NET_WM_WINDOW_TYPE_SPLASH"])
            self->type = OB_CLIENT_TYPE_SPLASH;
        else if (a == HYPRATOMS["NET_WM_WINDOW_TYPE_DIALOG"])
            self->type = OB_CLIENT_TYPE_DIALOG;
        else if (a == HYPRATOMS["NET_WM_WINDOW_TYPE_NORMAL"])
            self->type = OB_CLIENT_TYPE_NORMAL;
        else if (a == HYPRATOMS["KDE_NET_WM_WINDOW_TYPE_OVERRIDE"])
        {
            self->mwmhints.flags &= (OB_MWM_FLAG_FUNCTIONS |
                                     OB_MWM_FLAG_DECORATIONS);
            self->mwmhints.decorations = 0;
            self->mwmhints.functions = 0;
        }
        if (self->type != (ObClientType) -1)
            break; 
    }

    xcb_window_t t;
    auto connection = g_pXWayland->m_wm->getConnection();
    if (xcb_get_transient_for(connection, self->w->m_xwaylandSurface->m_xID, &t))
        self->transient = TRUE;

    if (self->type == (ObClientType) -1) {
        if (self->transient)
            self->type = OB_CLIENT_TYPE_DIALOG;
        else
            self->type = OB_CLIENT_TYPE_NORMAL;
    }

    if (self->type == OB_CLIENT_TYPE_DIALOG ||
        self->type == OB_CLIENT_TYPE_TOOLBAR ||
        self->type == OB_CLIENT_TYPE_MENU ||
        self->type == OB_CLIENT_TYPE_UTILITY)
    {
        self->transient = TRUE;
    }
}

static void client_setup_default_decor_and_functions(HyprWindow* self) {
    self->decorations =
        (OB_FRAME_DECOR_TITLEBAR |
         OB_FRAME_DECOR_HANDLE |
         OB_FRAME_DECOR_GRIPS |
         OB_FRAME_DECOR_BORDER |
         OB_FRAME_DECOR_ICON |
         OB_FRAME_DECOR_ALLDESKTOPS |
         OB_FRAME_DECOR_ICONIFY |
         OB_FRAME_DECOR_MAXIMIZE |
         OB_FRAME_DECOR_SHADE |
         OB_FRAME_DECOR_CLOSE);
    self->functions =
        (OB_CLIENT_FUNC_RESIZE |
         OB_CLIENT_FUNC_MOVE |
         OB_CLIENT_FUNC_ICONIFY |
         OB_CLIENT_FUNC_MAXIMIZE |
         OB_CLIENT_FUNC_SHADE |
         OB_CLIENT_FUNC_CLOSE |
         OB_CLIENT_FUNC_BELOW |
         OB_CLIENT_FUNC_ABOVE |
         OB_CLIENT_FUNC_UNDECORATE);

    //if (!(self->min_size.width < self->max_size.width ||
          //self->min_size.height < self->max_size.height))
        //self->functions &= ~OB_CLIENT_FUNC_RESIZE;

    switch (self->type) {
    case OB_CLIENT_TYPE_NORMAL:
        self->functions |= OB_CLIENT_FUNC_FULLSCREEN;
        break;

    case OB_CLIENT_TYPE_DIALOG:
        self->functions |= OB_CLIENT_FUNC_FULLSCREEN;
        break;

    case OB_CLIENT_TYPE_UTILITY:
        break;

    case OB_CLIENT_TYPE_MENU:
    case OB_CLIENT_TYPE_TOOLBAR:
        self->decorations &= ~(OB_FRAME_DECOR_ICONIFY |
                               OB_FRAME_DECOR_MAXIMIZE);
        self->functions &= ~(OB_CLIENT_FUNC_ICONIFY |
                             OB_CLIENT_FUNC_MAXIMIZE);
        break;

    case OB_CLIENT_TYPE_SPLASH:
        self->decorations = 0;
        self->functions = OB_CLIENT_FUNC_MOVE;
        break;

    case OB_CLIENT_TYPE_DESKTOP:
        self->decorations = 0;
        self->functions = 0;
        break;

    case OB_CLIENT_TYPE_DOCK:
        self->decorations = 0;
        self->functions = OB_CLIENT_FUNC_BELOW;
        break;
    }

    if (self->decorations == 0)
        self->functions &= ~OB_CLIENT_FUNC_UNDECORATE;

    if (self->mwmhints.flags & OB_MWM_FLAG_DECORATIONS) {
        if (! (self->mwmhints.decorations & OB_MWM_DECOR_ALL)) {
            if (! ((self->mwmhints.decorations & OB_MWM_DECOR_HANDLE) ||
                   (self->mwmhints.decorations & OB_MWM_DECOR_TITLE)))
            {
                if (self->mwmhints.decorations & OB_MWM_DECOR_BORDER)
                    self->decorations = OB_FRAME_DECOR_BORDER;
                else
                    self->decorations = 0;
            }
        }
    }

    if (self->mwmhints.flags & OB_MWM_FLAG_FUNCTIONS) {
        if (! (self->mwmhints.functions & OB_MWM_FUNC_ALL)) {
            if (! (self->mwmhints.functions & OB_MWM_FUNC_RESIZE))
                self->functions &= ~OB_CLIENT_FUNC_RESIZE;
            if (! (self->mwmhints.functions & OB_MWM_FUNC_MOVE))
                self->functions &= ~OB_CLIENT_FUNC_MOVE;
        }
    }

    if (!(self->functions & OB_CLIENT_FUNC_SHADE))
        self->decorations &= ~OB_FRAME_DECOR_SHADE;
    if (!(self->functions & OB_CLIENT_FUNC_ICONIFY))
        self->decorations &= ~OB_FRAME_DECOR_ICONIFY;
    if (!(self->functions & OB_CLIENT_FUNC_RESIZE))
        self->decorations &= ~(OB_FRAME_DECOR_GRIPS | OB_FRAME_DECOR_HANDLE);

    if (!((self->functions & OB_CLIENT_FUNC_MAXIMIZE) &&
          (self->functions & OB_CLIENT_FUNC_MOVE) &&
          (self->functions & OB_CLIENT_FUNC_RESIZE))) {
        self->functions &= ~OB_CLIENT_FUNC_MAXIMIZE;
        self->decorations &= ~OB_FRAME_DECOR_MAXIMIZE;
    }
}

bool HyprIso::alt_tabbable(int id) {
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif
    for (auto h : hyprwindows) {
        if (h->id == id) {
            bool found = false;
            bool canX11 = false;
            
            if (h->w->m_isX11) {
                client_get_type_and_transientness(h);
                if (h->type == OB_CLIENT_TYPE_NORMAL) {
                    canX11 = true;
                }
            } else {
                canX11 = true;
            }
            for (const auto &w : g_pCompositor->m_windows) {
                if (w == h->w && w->m_isMapped && canX11) {
                    found = true;
                }
            }
            return found;
        }
    }

    return false; 
}

void change_root_config_path(std::string path, bool force = true) {
    return_default_config = true;
    auto default_path = g_pConfigManager->getMainConfigPath();
    return_default_config = false;
    if (!force) {
        if (default_path.find("/mylar/default.conf") != std::string::npos) {
            return;
        }
    }
    g_pConfigManager->m_config->changeRootPath(path.c_str());
}

void HyprIso::end() {
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif
    g_pHyprRenderer->m_renderPass.removeAllOfType("CRectPassElement");
    g_pHyprRenderer->m_renderPass.removeAllOfType("CBorderPassElement");
    g_pHyprRenderer->m_renderPass.removeAllOfType("CTexPassElement");
    g_pHyprRenderer->m_renderPass.removeAllOfType("CAnyPassElement");
    remove_request_listeners(); 
    // reset to default config
    return_default_config = true;
    change_root_config_path(g_pConfigManager->getMainConfigPath());
    g_pConfigManager->reload();
    for (auto a : anims)
        delete a;
    anims.clear();
}

CBox tocbox(Bounds b) {
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif
    return {b.x, b.y, b.w, b.h};
}

Bounds tobounds(CBox box) {
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif
    return {box.x, box.y, box.w, box.h};
}

void rect(Bounds box, RGBA color, int cornermask, float round, float roundingPower, bool blur, float blurA) {
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif
 
    //return;
    if (box.h <= 0 || box.w <= 0)
        return;
    bool clip = hypriso->clip;
    Bounds clipbox = hypriso->clipbox;
    if (clip && !tocbox(clipbox).overlaps(tocbox(box))) {
        return; 
    }
    if (cornermask == 16)
        round = 0;
    AnyPass::AnyData anydata([box, color, cornermask, round, roundingPower, blur, blurA, clip, clipbox](AnyPass* pass) {
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif
 
        CHyprOpenGLImpl::SRectRenderData rectdata;
        auto region = new CRegion(tocbox(box));
        rectdata.damage        = region;
        rectdata.blur          = blur;
        rectdata.blurA         = blurA;
        rectdata.round         = std::round(round);
        rectdata.roundingPower = roundingPower;
        rectdata.xray = false;

        if (clip)
            g_pHyprOpenGL->m_renderData.clipBox = tocbox(clipbox);
        
        // TODO: who is responsible for cleaning up this damage?
        set_rounding(cornermask); // only top side
        g_pHyprOpenGL->renderRect(tocbox(box), CHyprColor(color.r, color.g, color.b, color.a), rectdata);
        set_rounding(0);
        if (clip)
            g_pHyprOpenGL->m_renderData.clipBox = CBox();
    });
    anydata.box = tocbox(box);
    g_pHyprRenderer->m_renderPass.add(makeUnique<AnyPass>(std::move(anydata)));
}

void border(Bounds box, RGBA color, float size, int cornermask, float round, float roundingPower, bool blur, float blurA) {
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif
 
    if (box.h <= 0 || box.w <= 0)
        return;
    CBorderPassElement::SBorderData rectdata;
    rectdata.grad1         = CHyprColor(color.r, color.g, color.b, color.a);
    rectdata.grad2         = CHyprColor(color.r, color.g, color.b, color.a);
    rectdata.box           = tocbox(box);
    rectdata.round         = round;
    rectdata.outerRound    = round;
    rectdata.borderSize    = size;
    rectdata.roundingPower = roundingPower;
    g_pHyprRenderer->m_renderPass.add(makeUnique<CBorderPassElement>(rectdata));
}

void shadow(Bounds box, RGBA color, float rounding, float roundingPower, float size) {
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif
 
    AnyPass::AnyData anydata([box, color, rounding, roundingPower, size](AnyPass* pass) {
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif
 
        if (Desktop::focusState()->window().get()) {
            auto current = g_pHyprOpenGL->m_renderData.currentWindow;
            g_pHyprOpenGL->m_renderData.currentWindow = Desktop::focusState()->window();
            g_pHyprOpenGL->renderRoundedShadow(tocbox(box), rounding, roundingPower, size, CHyprColor(color.r, color.g, color.b, color.a), 1.0);
            g_pHyprOpenGL->m_renderData.currentWindow = current;
        }
    });
    g_pHyprRenderer->m_renderPass.add(makeUnique<AnyPass>(std::move(anydata)));
}


struct MylarBar : public IHyprWindowDecoration {
    PHLWINDOW m_window;
    int m_size;
    
    MylarBar(PHLWINDOW w, int size) : IHyprWindowDecoration(w) {
        m_window = w;
        m_size = size;
    }
    
    SDecorationPositioningInfo getPositioningInfo() { 
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif
        
        SDecorationPositioningInfo info;
        info.policy         = DECORATION_POSITION_STICKY;
        info.edges          = DECORATION_EDGE_TOP;
        info.priority       = 10005;
        info.reserved       = true;
        info.desiredExtents = {{0, m_size}, {0, 0}};
        return info;
    }
    void onPositioningReply(const SDecorationPositioningReply& reply) {
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif
        
        //g_pHyprRenderer->damageMonitor(m_window->m_monitor.lock());
        //draw(m_window->m_monitor.lock(), 1.0);
    }
    void draw(PHLMONITOR monitor, float const& a) { 
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif
        
        if (!hypriso->on_draw_decos)
           return; 
        for (auto m : hyprmonitors) {
            if (m->m == monitor) {
                for (auto w : hyprwindows) {
                   if (w->w == m_window)  {
                       hypriso->on_draw_decos(getDisplayName(), m->id, w->id, a);
                       return;
                   }
                }
            }
        }
    }
    eDecorationType getDecorationType() { return eDecorationType::DECORATION_GROUPBAR; }
    void updateWindow(PHLWINDOW) { 
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif
        
        //g_pHyprRenderer->damageMonitor(m_window->m_monitor.lock());
        //draw(m_window->m_monitor.lock(), 1.0);
    }
    void damageEntire() { 
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif
        
        //g_pHyprRenderer->damageMonitor(m_window->m_monitor.lock());
        //hypriso->damage_entire(m_window->m_monitorMovedFrom);
        //draw(m_window->m_monitor.lock(), 1.0);
    } 
    bool onInputOnDeco(const eInputType, const Vector2D&, std::any = {}) { 
        if (next_check) {
            next_check = false;
            return true;
        }
        return true; 
    }
    eDecorationLayer getDecorationLayer() { return eDecorationLayer::DECORATION_LAYER_BOTTOM; }
    uint64_t getDecorationFlags() { return DECORATION_ALLOWS_MOUSE_INPUT; }
    std::string getDisplayName() { return "MylarBar"; }
};

bool HyprIso::wants_titlebar(int id) {
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif
    for (auto hyprwindow : hyprwindows) {
        if (hyprwindow->id == id) {
            if (hyprwindow->w->m_X11DoesntWantBorders) {
                return false;
            }
        }
    }
    
    if (requested_client_side_decorations(id))
        return false;  

    return true;
}

void HyprIso::reserve_titlebar(int id, int size) {
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif
    
    for (auto hyprwindow : hyprwindows) {
        if (hyprwindow->id == id) {
            if (auto w = hyprwindow->w.get()) {
                for (auto& wd : w->m_windowDecorations)
                    if (wd->getDisplayName() == "MylarBar")
                        return;

                auto m = makeUnique<MylarBar>(hyprwindow->w, size);
                HyprlandAPI::addWindowDecoration(globals->api, hyprwindow->w, std::move(m));
            }
        }
    }
}

void request_refresh() {
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif
    
    for (auto m : g_pCompositor->m_monitors) {
        g_pHyprRenderer->damageMonitor(m);
        g_pCompositor->scheduleFrameForMonitor(m);
    }
}

void request_refresh_only() {
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif
    
    for (auto m : g_pCompositor->m_monitors) {
        g_pCompositor->scheduleFrameForMonitor(m);
    }
}

Bounds bounds_full(ThinClient *w) {
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif
    
    notify("deprecated(bounds_full): use bounds_full_client");
    return bounds_full_client(w->id);
}

Bounds bounds(ThinClient *w) {
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif
    
    notify("deprecated(bounds): use bounds_client");
    return bounds_client(w->id);
}

Bounds real_bounds(ThinClient *w) {
    notify("deprecated(real_bounds): use real_bounds_client");
    return real_bounds_client(w->id);
}

std::string class_name(ThinClient *w) {
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif
    
    for (auto hyprwindow : hyprwindows) {
        if (hyprwindow->id == w->id) {
            if (auto w = hyprwindow->w.get()) {
                return w->m_class;
            }
        }
    }
 
   return ""; 
}

std::string HyprIso::title_name(int id) {
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif
    
    for (auto hyprwindow : hyprwindows) {
        if (hyprwindow->id == id) {
            if (auto w = hyprwindow->w.get()) {
                return w->m_title;
            }
        }
    }
 
    return "";
}

std::string title_name(ThinClient *w) {
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif
    
    for (auto hyprwindow : hyprwindows) {
        if (hyprwindow->id == w->id) {
            if (auto w = hyprwindow->w.get()) {
                return w->m_title;
            }
        }
    }
 
    return "";
}

Bounds bounds(ThinMonitor *m) {
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif
    
    notify("deprecated(bounds): use bounds_monitor");
    return bounds_monitor(m->id);
}

Bounds bounds_reserved(ThinMonitor *m) {
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif
    
    notify("deprecated(bounds_reserved): use bounds_reserved_monitor");
    return bounds_reserved_monitor(m->id);
}

void notify(std::string text) {
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif
    
    HyprlandAPI::addNotification(globals->api, text, {1, 1, 1, 1}, 4000);
}

int current_rendering_monitor() {
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif
    
    if (auto m = g_pHyprOpenGL->m_renderData.pMonitor.lock()) {
        for (auto hyprmonitor : hyprmonitors) {
            if (hyprmonitor->m == m) {
                return hyprmonitor->id;
            }
        } 
    }
    return -1;
}

int current_rendering_window() {
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif
    
    if (auto c = g_pHyprOpenGL->m_renderData.currentWindow.lock()) {
        for (auto hyprwindow : hyprwindows) {
            if (hyprwindow->w == c) {
                return hyprwindow->id; 
            }
        }         
    }
    return -1;
}

float scale(int id) {
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif
    for (auto hyprmonitor : hyprmonitors) {
        if (hyprmonitor->id == id) {
            return hyprmonitor->m->m_scale;
        }
    }
    return 1.0;
}

// TODO: this is wrong order
std::vector<int> HyprIso::get_workspace_ids(int monitor) {
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif
    struct Pair {
        WORKSPACEID raw_id;
        int id;
    };
    std::vector<Pair> vec;
    for (auto hm : hyprmonitors) {
        if (hm->id == monitor) {
            for (auto e : g_pCompositor->m_workspaces) {
                if (hm->m == e->m_monitor) {
                    for (auto hs : hyprspaces) {
                        if (hs->w == e) {
                            vec.push_back({hs->w->m_id, hs->id});
                        }
                    }
                }
            }
        }
    }
    std::sort(vec.begin(), vec.end(), [](Pair a, Pair b) {
        return a.raw_id < b.raw_id; 
    });
    std::vector<int> vecint;
    for (auto v : vec)
        vecint.push_back(v.id);

    return vecint;
}

// TODO: this is wrong order
std::vector<int> HyprIso::get_workspaces(int monitor) {
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif
    struct Pair {
        WORKSPACEID raw_id;
        int id;
    };
    std::vector<Pair> vec;
    for (auto hm : hyprmonitors) {
        if (hm->id == monitor) {
            for (auto e : g_pCompositor->m_workspaces) {
                if (hm->m == e->m_monitor) {
                    for (auto hs : hyprspaces) {
                        if (hs->w == e) {
                            vec.push_back({hs->w->m_id, hs->id});
                        }
                    }
                }
            }
        }
    }
    std::sort(vec.begin(), vec.end(), [](Pair a, Pair b) {
        return a.raw_id < b.raw_id; 
    });
    std::vector<int> vecint;
    for (auto v : vec)
        vecint.push_back(v.raw_id);

    return vecint;
}

int HyprIso::get_active_workspace(int monitor) {
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif
    for (auto hm : hyprmonitors) {
        if (hm->id == monitor) {
            if (hm->m->m_activeWorkspace.get()) {
                return hm->m->m_activeWorkspace->m_id;
            }
        }
    }
    return -1;
}

int HyprIso::get_active_workspace_id(int monitor) {
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif
    for (auto hm : hyprmonitors) {
        if (hm->id == monitor) {
            for (auto s : hyprspaces) {
                if (s->w == hm->m->m_activeWorkspace) {
                    return s->id;
                }
            }
        }
    }
    return -1;
}

int HyprIso::get_active_workspace_id_client(int client) {
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif
    for (auto hw : hyprwindows) {
        if (hw->id == client) {
            for (auto s : hyprspaces) {
                if (s->w == hw->w->m_workspace) {
                    return s->id;
                }
            }
        }
    }
    return -1;
}

bool HyprIso::is_space_tiling(int space) {
    for (auto &hs : hyprspaces) {
        if (hs->id == space) {
            return hs->is_tiling;
        }
    }
    return false;
}

void HyprIso::set_space_tiling(int space, bool state) {
    for (auto &hs : hyprspaces) {
        if (hs->id == space) {
            hs->is_tiling = state;
        }
    }
}

void HyprIso::pin(int id, bool state) {
    for (auto hw : hyprwindows) {
        if (hw->id == id) {
            hw->w->m_pinned = state;
        }
    }
}

bool HyprIso::is_fake_fullscreen(int id) {
    for (auto hw : hyprwindows) {
        if (hw->id == id) {
            auto w = hw->w;
            if (w->m_fullscreenState.internal == 0 && w->m_fullscreenState.client == 2) {
                return true;
            }
        }
    }

    return false;
}

void HyprIso::fake_fullscreen(int id, bool state) {
    for (auto hw : hyprwindows) {
        if (hw->id == id) {
            hw->w->m_ruleApplicator->syncFullscreenOverride(Desktop::Types::COverridableVar(false, Desktop::Types::PRIORITY_SET_PROP));

            //hw->w->m_windowData.syncFullscreen = CWindowOverridableVar(false, PRIORITY_SET_PROP);
            
            if (state) {
                g_pCompositor->setWindowFullscreenState(hw->w, Desktop::View::SFullscreenState{.internal = (eFullscreenMode) 0, .client = (eFullscreenMode) 2});
            } else {
                g_pCompositor->setWindowFullscreenState(hw->w, Desktop::View::SFullscreenState{.internal = (eFullscreenMode) 0, .client = (eFullscreenMode) 0});
            }
            hw->w->m_ruleApplicator->syncFullscreenOverride(
                Desktop::Types::COverridableVar(hw->w->m_fullscreenState.internal == hw->w->m_fullscreenState.client, Desktop::Types::PRIORITY_SET_PROP));
            //hw->w->m_windowData.syncFullscreen = CWindowOverridableVar(hw->w->m_fullscreenState.internal == hw->w->m_fullscreenState.client, PRIORITY_SET_PROP);
        }
    }
}


int HyprIso::get_workspace(int client) {
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif
    for (auto hw : hyprwindows) {
        if (hw->id == client) {
            if (hw->w->m_workspace.get()) {
                return hw->w->m_workspace->m_id;
            }
        }
    }
    return -1;
}


std::vector<int> get_window_stacking_order() {
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif
    std::vector<int> vec;
    for (auto w : g_pCompositor->m_windows) {
        for (auto hyprwindow : hyprwindows) {
            if (hyprwindow->w == w) {
                vec.push_back(hyprwindow->id);
            }
        }        
    }
    
    return vec;
}

void HyprIso::move(int id, int x, int y) {
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif
    for (auto c : hyprwindows) {
        if (c->id == id) {
            c->w->m_realPosition->setValueAndWarp({x, y});
        }
    }
}

void HyprIso::move_resize(int id, int x, int y, int w, int h, bool instant) {
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif
    for (auto c : hyprwindows) {
        if (c->id == id) {
            auto scaling_factor = c->w->m_monitor->m_scale;
            /*
            {
                float x_scaled = ((float) x) * scaling_factor;
                auto rounded = std::round(x_scaled);
                auto top = std::ceil(x_scaled);
                auto bottom = std::floor(x_scaled);
                // detect a position that is going to experience a problem 
                if (top == rounded && bottom != rounded) { 
                    // hack workaround
                    x = x - 1; 
                }
            }
            {
                float x_scaled = ((float) x + w) * scaling_factor;
                auto rounded = std::round(x_scaled);
                auto top = std::ceil(x_scaled);
                auto bottom = std::floor(x_scaled);
                // detect a position that is going to experience a problem 
                if (top == rounded && bottom != rounded) { 
                    // hack workaround
                    w = w + 1; 
                }
            }
            */
            {
                if (instant) {
                    #ifdef TRACY_ENABLE
                        ZoneScopedN("Move warp");
                    #endif
                    c->w->m_realPosition->setValueAndWarp({x, y});
                    c->w->m_realSize->setValueAndWarp({w, h});
                } else {
                    #ifdef TRACY_ENABLE
                        ZoneScopedN("Move regular");
                    #endif
                    *c->w->m_realPosition = {x, y};
                    *c->w->m_realSize = {w, h};
                }
            }
            {
                #ifdef TRACY_ENABLE
                    ZoneScopedN("Move regular");
                #endif
                c->w->sendWindowSize(false);
            }
            {
                #ifdef TRACY_ENABLE
                    ZoneScopedN("Update decos");
                #endif
                c->w->updateWindowDecos();
            }
        }
    }
}
void HyprIso::move_resize(int id, Bounds b, bool instant) {
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif
    move_resize(id, b.x, b.y, b.w, b.h, instant);
}


bool paint_svg_to_surface(cairo_surface_t* surface, std::string path, int target_size) {
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif
    GFile* gfile = g_file_new_for_path(path.c_str());
    if (gfile == nullptr)
        return false;
    RsvgHandle* handle = rsvg_handle_new_from_gfile_sync(gfile, RSVG_HANDLE_FLAGS_NONE, NULL, NULL);

    // TODO: is this correct?
    if (handle == nullptr)
        return false;

    auto* temp_context = cairo_create(surface);
    cairo_save(temp_context);
    cairo_set_operator(temp_context, CAIRO_OPERATOR_CLEAR);
    cairo_paint(temp_context);
    cairo_restore(temp_context);

    cairo_save(temp_context);
    const RsvgRectangle viewport{0, 0, (double)target_size, (double)target_size};
    rsvg_handle_render_layer(handle, temp_context, NULL, &viewport, nullptr);
    cairo_restore(temp_context);
    cairo_destroy(temp_context);

    g_object_unref(gfile);
    g_object_unref(handle);

    return true;
}

bool paint_png_to_surface(cairo_surface_t* surface, std::string path, int target_size) {
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif
    auto* png_surface = cairo_image_surface_create_from_png(path.c_str());

    if (cairo_surface_status(png_surface) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(png_surface);
        return false;
    }

    auto* temp_context = cairo_create(surface);
    int   w            = cairo_image_surface_get_width(png_surface);
    int   h            = cairo_image_surface_get_height(png_surface);

    cairo_save(temp_context);
    cairo_set_operator(temp_context, CAIRO_OPERATOR_CLEAR);
    cairo_paint(temp_context);
    cairo_restore(temp_context);

    cairo_save(temp_context);
    if (target_size != w) {
        double scale = ((double)target_size) / ((double)w);
        cairo_scale(temp_context, scale, scale);
    }
    cairo_set_source_surface(temp_context, png_surface, 0, 0);
    cairo_paint(temp_context);
    cairo_restore(temp_context);
    cairo_destroy(temp_context);
    cairo_surface_destroy(png_surface);

    return true;
}

cairo_surface_t* cairo_image_surface_create_from_xpm(const std::string& path) {
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif
    std::ifstream file(path);
    if (!file) {
        std::cerr << "Failed to open XPM file: " << path << std::endl;
        return nullptr;
    }

    std::string              line;
    std::vector<std::string> lines;

    // Read lines into a buffer
    while (std::getline(file, line)) {
        size_t start = line.find('"');
        size_t end   = line.rfind('"');
        if (start != std::string::npos && end != std::string::npos && end > start) {
            lines.push_back(line.substr(start + 1, end - start - 1));
        }
    }

    if (lines.empty())
        return nullptr;

    // Parse header
    std::istringstream header(lines[0]);
    int                width, height, num_colors, chars_per_pixel;
    header >> width >> height >> num_colors >> chars_per_pixel;

    // Parse color map
    std::unordered_map<std::string, uint32_t> color_map;
    for (int i = 1; i <= num_colors; ++i) {
        std::string entry     = lines[i];
        std::string key       = entry.substr(0, chars_per_pixel);
        std::string color_str = entry.substr(entry.find("c ") + 2);

        uint32_t    color = 0x00000000; // Default to transparent

        if (color_str == "None") {
            color = 0x00000000;
        } else if (color_str[0] == '#') {
            // Parse hex color
            color_str      = color_str.substr(1); // skip '#'
            unsigned int r = 0, g = 0, b = 0;

            if (color_str.length() == 6) {
                std::istringstream(color_str.substr(0, 2)) >> std::hex >> r;
                std::istringstream(color_str.substr(2, 2)) >> std::hex >> g;
                std::istringstream(color_str.substr(4, 2)) >> std::hex >> b;
            }

            color = (0xFF << 24) | (r << 16) | (g << 8) | b; // ARGB
        }

        color_map[key] = color;
    }

    // Create surface
    cairo_surface_t* surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
    unsigned char*   data    = cairo_image_surface_get_data(surface);
    int              stride  = cairo_image_surface_get_stride(surface);

    // Parse pixels
    for (int y = 0; y < height; ++y) {
        const std::string& row = lines[y + 1 + num_colors];
        for (int x = 0; x < width; ++x) {
            std::string    key   = row.substr(x * chars_per_pixel, chars_per_pixel);
            uint32_t       color = color_map.count(key) ? color_map[key] : 0x00000000;

            unsigned char* pixel = data + y * stride + x * 4;
            pixel[0]             = (color >> 0) & 0xFF;  // B
            pixel[1]             = (color >> 8) & 0xFF;  // G
            pixel[2]             = (color >> 16) & 0xFF; // R
            pixel[3]             = (color >> 24) & 0xFF; // A
        }
    }

    cairo_surface_mark_dirty(surface);
    return surface;
}

bool paint_xpm_to_surface(cairo_surface_t* surface, std::string path, int target_size) {
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif
    auto* xpm_surface = cairo_image_surface_create_from_xpm(path.c_str());
    if (!xpm_surface)
        return false;

    if (cairo_surface_status(xpm_surface) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(xpm_surface);
        return false;
    }

    auto* temp_context = cairo_create(surface);
    int   w            = cairo_image_surface_get_width(xpm_surface);

    cairo_save(temp_context);
    cairo_set_operator(temp_context, CAIRO_OPERATOR_CLEAR);
    cairo_paint(temp_context);
    cairo_restore(temp_context);

    cairo_save(temp_context);
    if (target_size != w) {
        double scale = ((double)target_size) / ((double)w);
        cairo_scale(temp_context, scale, scale);
    }
    cairo_set_source_surface(temp_context, xpm_surface, 0, 0);
    cairo_paint(temp_context);
    cairo_restore(temp_context);
    cairo_destroy(temp_context);
    cairo_surface_destroy(xpm_surface);

    return true;
}

bool paint_surface_with_image(cairo_surface_t* surface, std::string path, int target_size, void (*upon_completion)(bool)) {
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif
    bool success = false;
    if (path.find(".svg") != std::string::npos) {
        success = paint_svg_to_surface(surface, path, target_size);
    } else if (path.find(".png") != std::string::npos) {
        success = paint_png_to_surface(surface, path, target_size);
    } else if (path.find(".xpm") != std::string::npos) {
        success = paint_xpm_to_surface(surface, path, target_size);
    }
    if (upon_completion != nullptr) {
        upon_completion(success);
    }
    return success;
}

cairo_surface_t* accelerated_surface(int w, int h) {
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif

    cairo_surface_t* raw_surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);

    /*
  cairo_surface_t *fast_surface = cairo_surface_create_similar_image(
          cairo_get_target(client_entity->cr), CAIRO_FORMAT_ARGB32, w, h);
          */

    if (cairo_surface_status(raw_surface) != CAIRO_STATUS_SUCCESS)
        return nullptr;

    return raw_surface;
}

void load_icon_full_path(cairo_surface_t** surface, std::string path, int target_size) {
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif
    if (path.find("svg") != std::string::npos) {
        *surface = accelerated_surface(target_size, target_size);
        paint_svg_to_surface(*surface, path, target_size);
    } else if (path.find("png") != std::string::npos) {
        *surface = accelerated_surface(target_size, target_size);
        paint_png_to_surface(*surface, path, target_size);
    } else if (path.find("xpm") != std::string::npos) {
        *surface = accelerated_surface(target_size, target_size);
        paint_xpm_to_surface(*surface, path, target_size);
    }
}

SP<CTexture> missingTexure(int size) {
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif
    SP<CTexture> tex = makeShared<CTexture>();
    tex->allocate();

    const auto CAIROSURFACE = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 512, 512);
    const auto CAIRO        = cairo_create(CAIROSURFACE);

    cairo_set_antialias(CAIRO, CAIRO_ANTIALIAS_NONE);
    cairo_save(CAIRO);
    cairo_set_source_rgba(CAIRO, 0, 0, 0, 1);
    cairo_set_operator(CAIRO, CAIRO_OPERATOR_SOURCE);
    cairo_paint(CAIRO);
    cairo_set_source_rgba(CAIRO, 1, 0, 1, 1);
    cairo_rectangle(CAIRO, 256, 0, 256, 256);
    cairo_fill(CAIRO);
    cairo_rectangle(CAIRO, 0, 256, 256, 256);
    cairo_fill(CAIRO);
    cairo_restore(CAIRO);

    cairo_surface_flush(CAIROSURFACE);

    tex->m_size = {512, 512};

    // copy the data to an OpenGL texture we have
    const GLint glFormat = GL_RGBA;
    const GLint glType   = GL_UNSIGNED_BYTE;

    const auto  DATA = cairo_image_surface_get_data(CAIROSURFACE);
    tex->bind();
    tex->setTexParameter(GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    tex->setTexParameter(GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    tex->setTexParameter(GL_TEXTURE_SWIZZLE_R, GL_BLUE);
    tex->setTexParameter(GL_TEXTURE_SWIZZLE_B, GL_RED);
    glTexImage2D(GL_TEXTURE_2D, 0, glFormat, tex->m_size.x, tex->m_size.y, 0, glFormat, glType, DATA);

    cairo_surface_destroy(CAIROSURFACE);
    cairo_destroy(CAIRO);

    return tex;
}

SP<CTexture> loadAsset(const std::string& filename, int target_size) {
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif
    cairo_surface_t* icon = nullptr;
    load_icon_full_path(&icon, filename, target_size);
    if (!icon)
        return {};

    const auto CAIROFORMAT = cairo_image_surface_get_format(icon);
    auto       tex         = makeShared<CTexture>();

    tex->allocate();
    tex->m_size = {cairo_image_surface_get_width(icon), cairo_image_surface_get_height(icon)};

    const GLint glIFormat = CAIROFORMAT == CAIRO_FORMAT_RGB96F ? GL_RGB32F : GL_RGBA;
    const GLint glFormat  = CAIROFORMAT == CAIRO_FORMAT_RGB96F ? GL_RGB : GL_RGBA;
    const GLint glType    = CAIROFORMAT == CAIRO_FORMAT_RGB96F ? GL_FLOAT : GL_UNSIGNED_BYTE;

    const auto  DATA = cairo_image_surface_get_data(icon);
    tex->bind();
    tex->setTexParameter(GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    tex->setTexParameter(GL_TEXTURE_MIN_FILTER, GL_LINEAR);

    if (CAIROFORMAT != CAIRO_FORMAT_RGB96F) {
        tex->setTexParameter(GL_TEXTURE_SWIZZLE_R, GL_BLUE);
        tex->setTexParameter(GL_TEXTURE_SWIZZLE_B, GL_RED);
    }

    glTexImage2D(GL_TEXTURE_2D, 0, glIFormat, tex->m_size.x, tex->m_size.y, 0, glFormat, glType, DATA);

    cairo_surface_destroy(icon);

    return tex;
}

void free_text_texture(int id) {
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif
    for (int i = 0; i < hyprtextures.size(); i++) {
        auto h = hyprtextures[i];
        if (h->info.id == id) {
            h->texture.reset();
            printf("free: %d\n", h->info.id);
            hyprtextures.erase(hyprtextures.begin() + i);
            delete h;
        }
    }
}

TextureInfo gen_texture(std::string path, float h) {
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif
    //log("gen texture");
    //notify("gen texture");
    auto tex = loadAsset(path, h);
    if (tex.get()) {
        auto t = new Texture;
        t->texture = tex;
        TextureInfo info;
        info.id = unique_id++;
        info.w = t->texture->m_size.x;
        info.h = t->texture->m_size.y;
        printf("generate pic: %d\n", info.id);
        t->info = info;
        hyprtextures.push_back(t);
        return t->info;
    }
    return {};
}

TextureInfo gen_gradient_texture(RGBA center, RGBA edge, float wh) {
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif

    const int size = static_cast<int>(wh);
    if (size <= 0)
        return {};

    // Create Cairo surface
    cairo_surface_t* surface =
        cairo_image_surface_create(CAIRO_FORMAT_ARGB32, size, size);
    cairo_t* cr = cairo_create(surface);

    // Clear surface
    cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

    // Create radial gradient
    cairo_pattern_t* pattern = cairo_pattern_create_radial(
        size / 2.0, size / 2.0, 0.0,
        size / 2.0, size / 2.0, size / 2.0);

    cairo_pattern_add_color_stop_rgba(
        pattern, 0.0,
        center.r, center.g, center.b, center.a);

    cairo_pattern_add_color_stop_rgba(
        pattern, 1.0,
        edge.r, edge.g, edge.b, edge.a);

    // Draw gradient
    cairo_set_source(cr, pattern);
    cairo_arc(cr, size / 2.0, size / 2.0, size / 2.0, 0, 2 * M_PI);
    cairo_fill(cr);

    cairo_pattern_destroy(pattern);
    cairo_destroy(cr);

    // Upload to OpenGL
    auto tex = g_pHyprOpenGL->texFromCairo(surface);
    cairo_surface_destroy(surface);

    if (!tex.get())
        return {};

    auto t = new Texture;
    t->texture = tex;

    TextureInfo info;
    info.id = unique_id++;
    info.w = t->texture->m_size.x;
    info.h = t->texture->m_size.y;

    t->info = info;
    hyprtextures.push_back(t);

    return info;
}

TextureInfo gen_text_texture(std::string font, std::string text, float h, RGBA color) {
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif
    //log("gen text texture");
    //notify("gen text");
    auto tex = g_pHyprOpenGL->renderText(text, CHyprColor(color.r, color.g, color.b, color.a), h, false, font, 0);
    if (tex.get()) {
        auto t = new Texture;
        t->texture = tex;
        TextureInfo info;
        info.id = unique_id++;
        info.w = t->texture->m_size.x;
        info.h = t->texture->m_size.y;
        t->info = info;
        hyprtextures.push_back(t);
        printf("generate text: %d\n", info.id);
        return t->info;
    }
    return {};
}

void draw_texture(TextureInfo info, int x, int y, float a, float clip_w) {
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif
 
    //return;
    for (auto t : hyprtextures) {
        
       if (t->info.id == info.id) {
            CTexPassElement::SRenderData data;
            data.tex = t->texture;
            data.box = {(float) x, (float) y, data.tex->m_size.x, data.tex->m_size.y};
            data.box.x = x;
            data.box.round();
            auto inter = data.box;
            if (hypriso->clip) {
                inter = tocbox(hypriso->clipbox);
                //.intersection(data.box)
                if (data.box.inside(inter)) {
                    inter = tocbox(hypriso->clipbox).intersection(data.box);
                }
            }
            
            data.clipBox = inter;
            if (clip_w != 0.0) {
                data.clipBox.w = clip_w;
            }
            data.a = 1.0 * a;
            g_pHyprRenderer->m_renderPass.add(makeUnique<CTexPassElement>(std::move(data)));
            
       }
    }
}

void setCursorImageUntilUnset(std::string cursor) {
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif
    if (hypriso->last_cursor_set == cursor) {
        return; 
    }
    hypriso->last_cursor_set = cursor;
    // pre 52
    //g_pInputManager->setCursorImageUntilUnset(cursor);
    Cursor::overrideController->setOverride(cursor, Cursor::CURSOR_OVERRIDE_SPECIAL_ACTION);
}

void unsetCursorImage(bool force) {
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif
    if (hypriso->last_cursor_set == "none" && !force)
        return;
    hypriso->last_cursor_set = "none";
    Cursor::overrideController->unsetOverride(Cursor::CURSOR_OVERRIDE_SPECIAL_ACTION);
    // pre 52
    //g_pInputManager->unsetCursorImage();
}

int get_monitor(int client) {
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif
    for (auto hw : hyprwindows) {
       if (hw->id == client) {
           for (auto hm : hyprmonitors) {
              if (hm->m == hw->w->m_monitor) {
                  return hm->id;
              } 
           }
       } 
    }
    return -1; 
}

Bounds mouse() {
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif
    auto mouse = g_pInputManager->getMouseCoordsInternal();
    return {mouse.x, mouse.y, mouse.x, mouse.y};
}

void close_window(int id) {
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif
    for (auto hw : hyprwindows) {
        if (hw->id == id) {
            g_pCompositor->closeWindow(hw->w);
        }
    }
}

int HyprIso::monitor_from_cursor() {
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif
    auto m = g_pCompositor->getMonitorFromCursor();
    for (auto hm : hyprmonitors) {
        if (hm->m == m) {
            return hm->id;   
        }
    }
    return -1;
}

bool HyprIso::is_mapped(int id) {
    for (auto hw : hyprwindows) {
        if (hw->id == id) {
            return hw->w->m_isMapped;
        }
    }
    return false; 
}

bool x11MotifDisallowsResize(const MotifHints& h) {
    // If no function hints are provided, assume resizing is allowed
    if (!(h.flags & MWM_HINTS_FUNCTIONS))
        return false;

    // If all functions are allowed, resizing is allowed
    if (h.functions & MWM_FUNC_ALL)
        return false;

    // Otherwise, check explicitly
    return !(h.functions & MWM_FUNC_RESIZE);
}

bool HyprIso::resizable(int id) {
    for (auto hw : hyprwindows) {
        if (hw->id == id) {
            if (!hw->checked_resizable) {
                hw->checked_resizable = true;
                if (hw->w->m_isX11 && hw->w->m_xwaylandSurface) {
                    auto win = hw->w->m_xwaylandSurface->m_xID;
                    if (g_pXWayland && g_pXWayland->m_wm) {
                        auto connection = g_pXWayland->m_wm->getConnection();

                        auto hintsOpt = getMotifHints(connection, win);
                        if (hintsOpt) {
                            auto& h = *hintsOpt;
                            hw->resizable = !x11MotifDisallowsResize(h);
                        }
                    }
                }            
            }
            return hw->resizable;
        }
    }
    return true;
}

bool HyprIso::is_hidden(int id) {
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif
    for (auto hw : hyprwindows) {
        if (hw->id == id) {
            return hw->is_hidden;
        }
    }
    return false; 
}

void HyprIso::set_hidden(int id, bool state, bool animate_to_dock) {
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif
    for (auto hw : hyprwindows) {
        if (hw->id == id) {
            hw->w->updateWindowDecos();
            hw->w->setHidden(state);
            hw->animate_to_dock = animate_to_dock;
            hw->is_hidden = state;
        }
    }
    for (auto m : hyprmonitors) {
        hypriso->damage_entire(m->id);
    }
}

void HyprIso::bring_to_front(int id, bool focus) {
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif
    for (auto hw : hyprwindows) {
        if (hw->id == id) {
            g_pKeybindManager->switchToWindow(hw->w, true);
            g_pCompositor->changeWindowZOrder(hw->w, true);
        }
    }
}

Bounds HyprIso::min_size(int id) {
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif
    for (auto hw : hyprwindows) {
        if (hw->id == id) {
            //auto s = hw->w->requestedMinSize();
            //return {s.x, s.y, s.x, s.y};
            return {20, 20, 20, 20};
        }
    }
    return {20, 20, 20, 20};
}

void HyprIso::remove_decorations(int id) {
    for (auto hyprwindow : hyprwindows) {
        if (hyprwindow->id == id) {
            if (auto w = hyprwindow->w.get()) {
                for (auto& wd : w->m_windowDecorations) {
                    if (wd->getDisplayName() == "MylarBar")  {
                        HyprlandAPI::removeWindowDecoration(globals->api, wd.get());
                        return;
                    }
                }
            }
        }
    }
    
}

bool HyprIso::has_decorations(int id) {
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif
    for (auto hw : hyprwindows) {
        if (hw->id == id) {
            for (const auto &decos : hw->w->m_windowDecorations) {
               if (decos->getDisplayName() == "MylarBar") {
                   return true;
               } 
            }
        }
    }
    return false;
}


bool HyprIso::is_x11(int id) {
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif
    for (auto hw : hyprwindows) {
        if (hw->id == id) {
            return hw->w->m_isX11;
        }
    }
    return false;
}

bool HyprIso::is_opaque(int id) {
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif
    for (auto hw : hyprwindows) {
        if (hw->id == id) {
            return hw->w->opaque();
        }
    }
    return true;
}

void HyprIso::send_key(uint32_t key) {
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif
    for (auto k : g_pInputManager->m_keyboards) {
        IKeyboard::SKeyEvent event;
        event.timeMs     = get_current_time_in_ms();
        event.updateMods = false;
        event.keycode    = key;
        event.state      = WL_KEYBOARD_KEY_STATE_PRESSED;
        g_pInputManager->onKeyboardKey(event, k);
        event.timeMs = get_current_time_in_ms();
        event.state  = WL_KEYBOARD_KEY_STATE_RELEASED;
        g_pInputManager->onKeyboardKey(event, k);
    }
}

bool HyprIso::is_fullscreen(int id) {
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif
    for (auto hw : hyprwindows) {
        if (hw->id == id) {
            return hw->w->isFullscreen();
        }
    }
    return false;
}

void HyprIso::should_round(int id, bool state) {
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif
    for (auto hw : hyprwindows) {
        if (hw->id == id) {
            hw->no_rounding = !state;
        }
    }
}

void HyprIso::damage_entire(int monitor) {
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif
    for (auto hm : hyprmonitors) {
        if (hm->id == monitor) {
            g_pHyprRenderer->damageMonitor(hm->m);
        }
    }
}

void HyprIso::damage_box(Bounds b) {
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif
    g_pHyprRenderer->damageBox(b.x, b.y, b.w, b.h);
}

int later_action(void* user_data) {
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif
    auto timer = (Timer*)user_data;
    if (timer->func)
        timer->func(timer);
    if (!timer->keep_running) {
        // remove from vec
        wl_event_source_remove(timer->source);
        delete timer;
    } else {
        wl_event_source_timer_update(timer->source, timer->delay);
    }
    return 0;
}

Timer* later(void* data, float time_ms, const std::function<void(Timer*)>& fn) {
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif
    auto timer    = new Timer;
    timer->func   = fn;
    timer->data   = data;
    timer->delay  = time_ms;
    timer->source = wl_event_loop_add_timer(g_pCompositor->m_wlEventLoop, &later_action, timer);
    wl_event_source_timer_update(timer->source, time_ms);
    return timer;
}

Timer* later(float time_ms, const std::function<void(Timer*)>& fn) {
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif
    auto timer    = new Timer;
    timer->func   = fn;
    timer->delay  = time_ms;
    timer->source = wl_event_loop_add_timer(g_pCompositor->m_wlEventLoop, &later_action, timer);
    wl_event_source_timer_update(timer->source, time_ms);
    return timer;
}

Timer* later_immediate(const std::function<void(Timer*)>& fn) {
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif
    return later(1, fn);
}

void screenshot_monitor(CFramebuffer* buffer, PHLMONITOR m) {
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif
    if (!buffer || !pRenderMonitor)
        return;
    if (!m || !m->m_output || m->m_pixelSize.x <= 0 || m->m_pixelSize.y <= 0)
        return;
    CRegion fakeDamage{0, 0, INT16_MAX, INT16_MAX};
    g_pHyprRenderer->makeEGLCurrent();
    buffer->alloc(m->m_pixelSize.x, m->m_pixelSize.y, DRM_FORMAT_ABGR8888);
    g_pHyprRenderer->beginRender(m, fakeDamage, RENDER_MODE_FULL_FAKE, nullptr, buffer);
    g_pHyprRenderer->m_bRenderingSnapshot = true;
    g_pHyprOpenGL->clear(CHyprColor(0, 0, 0, 0)); // JIC
    g_pHyprOpenGL->m_renderData.pMonitor = m;
    (*(tRenderMonitor)pRenderMonitor)(g_pHyprRenderer.get(), m, false);
    g_pHyprOpenGL->m_renderData.pMonitor  = m;
    g_pHyprOpenGL->m_renderData.outFB     = buffer;
    g_pHyprOpenGL->m_renderData.currentFB = buffer;
    g_pHyprRenderer->endRender();
    g_pHyprRenderer->m_bRenderingSnapshot = false;
}


void render_wallpaper(PHLMONITOR pMonitor, const Time::steady_tp& time, const Vector2D& translate, const float& scale) {
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif
    static auto PDIMSPECIAL      = CConfigValue<Hyprlang::FLOAT>("decoration:dim_special");
    static auto PBLURSPECIAL     = CConfigValue<Hyprlang::INT>("decoration:blur:special");
    static auto PBLUR            = CConfigValue<Hyprlang::INT>("decoration:blur:enabled");
    static auto PXPMODE          = CConfigValue<Hyprlang::INT>("render:xp_mode");
    static auto PSESSIONLOCKXRAY = CConfigValue<Hyprlang::INT>("misc:session_lock_xray");

    //g_pHyprRenderer->renderBackground(pMonitor);
    g_pHyprOpenGL->clearWithTex();

    for (auto const& ls : pMonitor->m_layerSurfaceLayers[ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND]) {
        g_pHyprRenderer->renderLayer(ls.lock(), pMonitor, time);
    }

    //EMIT_HOOK_EVENT("render", RENDER_POST_WALLPAPER);

    /*
    for (auto const& ls : pMonitor->m_layerSurfaceLayers[ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM]) {
        g_pHyprRenderer->renderLayer(ls.lock(), pMonitor, time);
    }

    for (auto const& ls : pMonitor->m_layerSurfaceLayers[ZWLR_LAYER_SHELL_V1_LAYER_TOP]) {
        g_pHyprRenderer->renderLayer(ls.lock(), pMonitor, time);
    }

    for (auto const& ls : pMonitor->m_layerSurfaceLayers[ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY]) {
        g_pHyprRenderer->renderLayer(ls.lock(), pMonitor, time);
    }
    */
}

void actual_screenshot_wallpaper(CFramebuffer* buffer, PHLMONITOR m) {
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif
    if (!buffer || !pRenderMonitor)
        return;
    if (!m || !m->m_output || m->m_pixelSize.x <= 0 || m->m_pixelSize.y <= 0)
        return;
    CRegion fakeDamage{0, 0, INT16_MAX, INT16_MAX};
    g_pHyprRenderer->makeEGLCurrent();
    buffer->alloc(m->m_pixelSize.x, m->m_pixelSize.y, DRM_FORMAT_ABGR8888);
    g_pHyprRenderer->beginRender(m, fakeDamage, RENDER_MODE_FULL_FAKE, nullptr, buffer);
    g_pHyprOpenGL->clear(CHyprColor(0, 0, 1, 1)); // JIC

    const auto NOW = Time::steadyNow();
    render_wallpaper(m, NOW, {0.0, 0.0}, 1.0);
    
    g_pHyprRenderer->endRender();
}

HyprWindow *get_window(PHLWINDOW w) {
    for (auto hw : hyprwindows) {
        if (hw->w == w) {
            return hw;
        }
    }
    return nullptr;
}

void screenshot_workspace(CFramebuffer* buffer, PHLWORKSPACEREF w, PHLMONITOR m, bool include_cursor) {
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif
    //return;
    if (!buffer || pRenderWorkspace == nullptr)
        return;
    if (!m || !m->m_output || m->m_pixelSize.x <= 0 || m->m_pixelSize.y <= 0)
        return;
    CRegion fakeDamage{0, 0, INT16_MAX, INT16_MAX};
    g_pHyprRenderer->makeEGLCurrent();
    buffer->alloc(m->m_pixelSize.x, m->m_pixelSize.y, DRM_FORMAT_ABGR8888);
    g_pHyprRenderer->beginRender(m, fakeDamage, RENDER_MODE_FULL_FAKE, nullptr, buffer);
    g_pHyprOpenGL->clear(CHyprColor(0, 0, 0, 0)); // JIC
    g_pHyprOpenGL->m_renderData.pMonitor = m;
    const auto NOW = Time::steadyNow();

    g_pHyprRenderer->renderWorkspace(m, w.lock(), Time::steadyNow(), m->logicalBox());
    //generateFrame(m, w.lock(), Time::steadyNow(), m->logicalBox());

    g_pHyprRenderer->endRender();
}

void makeSnapshot(PHLWINDOW pWindow, CFramebuffer *PFRAMEBUFFER) {
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif
    // we trust the window is valid.
    const auto PMONITOR = pWindow->m_monitor.lock();

    if (!PMONITOR || !PMONITOR->m_output || PMONITOR->m_pixelSize.x <= 0 || PMONITOR->m_pixelSize.y <= 0)
        return;

    if (!g_pHyprRenderer->shouldRenderWindow(pWindow))
        return; // ignore, window is not being rendered

    // we need to "damage" the entire monitor
    // so that we render the entire window
    // this is temporary, doesn't mess with the actual damage
    CRegion      fakeDamage{0, 0, PMONITOR->m_transformedSize.x, PMONITOR->m_transformedSize.y};

    PHLWINDOWREF ref{pWindow};

    g_pHyprRenderer->makeEGLCurrent();

    //const auto PFRAMEBUFFER = &g_pHyprOpenGL->m_windowFramebuffers[ref];

    PFRAMEBUFFER->alloc(PMONITOR->m_pixelSize.x, PMONITOR->m_pixelSize.y, DRM_FORMAT_ABGR8888);

    g_pHyprRenderer->beginRender(PMONITOR, fakeDamage, RENDER_MODE_FULL_FAKE, nullptr, PFRAMEBUFFER);

    g_pHyprRenderer->m_bRenderingSnapshot = true;

    g_pHyprOpenGL->clear(CHyprColor(0, 0, 0, 0)); // JIC

    g_pHyprRenderer->renderWindow(pWindow, PMONITOR, Time::steadyNow(), true, RENDER_PASS_ALL, true);

    g_pHyprRenderer->endRender();

    g_pHyprRenderer->m_bRenderingSnapshot = false;
}

void ourRenderWindow(PHLWINDOW pWindow, PHLMONITOR pMonitor, const Time::steady_tp& time, bool decorate, eRenderPassMode mode, bool ignorePosition, bool standalone) {
#ifdef FORK_WARN
//void CHyprRenderer::renderWindow(PHLWINDOW pWindow, PHLMONITOR pMonitor, const Time::steady_tp& time, bool decorate, eRenderPassMode mode, bool ignorePosition, bool standalone) {
    static_assert(true, "[Function Body] Make sure our `CHyprRenderer::renderWindow` and Hyprland's are synced!");
#endif
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif
    if (pWindow->m_fadingOut) {
        if (pMonitor == pWindow->m_monitor) // TODO: fix this
            g_pHyprRenderer->renderSnapshot(pWindow);
        return;
    }

    if (!pWindow->m_isMapped)
        return;

    TRACY_GPU_ZONE("RenderWindow");

    const auto                       PWORKSPACE = pWindow->m_workspace;
    const auto                       REALPOS    = pWindow->m_realPosition->value() + (pWindow->m_pinned ? Vector2D{} : PWORKSPACE->m_renderOffset->value());
    static auto                      PDIMAROUND = CConfigValue<Hyprlang::FLOAT>("decoration:dim_around");

    CSurfacePassElement::SRenderData renderdata = {pMonitor, time};
    CBox                             textureBox = {REALPOS.x, REALPOS.y, std::max(pWindow->m_realSize->value().x, 5.0), std::max(pWindow->m_realSize->value().y, 5.0)};

    renderdata.pos.x = textureBox.x;
    renderdata.pos.y = textureBox.y;
    renderdata.w     = textureBox.w;
    renderdata.h     = textureBox.h;

    if (ignorePosition) {
        renderdata.pos.x = pMonitor->m_position.x;
        renderdata.pos.y = pMonitor->m_position.y;
    } else {
        const bool ANR = pWindow->isNotResponding();
        if (ANR && pWindow->m_notRespondingTint->goal() != 0.2F)
            *pWindow->m_notRespondingTint = 0.2F;
        else if (!ANR && pWindow->m_notRespondingTint->goal() != 0.F)
            *pWindow->m_notRespondingTint = 0.F;
    }

    //if (standalone)
        //decorate = false;

    // whether to use m_fMovingToWorkspaceAlpha, only if fading out into an invisible ws
    const bool USE_WORKSPACE_FADE_ALPHA = pWindow->m_monitorMovedFrom != -1 && (!PWORKSPACE || !PWORKSPACE->isVisible());

    renderdata.surface   = pWindow->wlSurface()->resource();
    renderdata.dontRound = pWindow->isEffectiveInternalFSMode(FSMODE_FULLSCREEN);
    renderdata.fadeAlpha = pWindow->m_alpha->value() * (pWindow->m_pinned || USE_WORKSPACE_FADE_ALPHA ? 1.f : PWORKSPACE->m_alpha->value()) *
        (USE_WORKSPACE_FADE_ALPHA ? pWindow->m_movingToWorkspaceAlpha->value() : 1.F) * pWindow->m_movingFromWorkspaceAlpha->value();
    renderdata.alpha         = pWindow->m_activeInactiveAlpha->value();
    renderdata.decorate      = decorate && !pWindow->m_X11DoesntWantBorders && !pWindow->isEffectiveInternalFSMode(FSMODE_FULLSCREEN);
    renderdata.rounding      = standalone || renderdata.dontRound ? 0 : pWindow->rounding() * pMonitor->m_scale;
    renderdata.roundingPower = standalone || renderdata.dontRound ? 2.0f : pWindow->roundingPower();
    //renderdata.blur          = !standalone && g_pHyprRenderer->shouldBlur(pWindow);
    renderdata.blur          = false;
    renderdata.pWindow       = pWindow;

    if (standalone) {
        renderdata.alpha     = 1.f;
        renderdata.fadeAlpha = 1.f;
    }

    // apply opaque
    if (pWindow->m_ruleApplicator->opaque().valueOrDefault())
        renderdata.alpha = 1.f;

    renderdata.pWindow = pWindow;

    // for plugins
    g_pHyprOpenGL->m_renderData.currentWindow = pWindow;

    EMIT_HOOK_EVENT("render", RENDER_PRE_WINDOW);

    const auto fullAlpha = renderdata.alpha * renderdata.fadeAlpha;

    if (*PDIMAROUND && pWindow->m_ruleApplicator->dimAround().valueOrDefault() && !g_pHyprRenderer->m_bRenderingSnapshot && mode != RENDER_PASS_POPUP) {
        CBox                        monbox = {0, 0, g_pHyprOpenGL->m_renderData.pMonitor->m_transformedSize.x, g_pHyprOpenGL->m_renderData.pMonitor->m_transformedSize.y};
        CRectPassElement::SRectData data;
        data.color = CHyprColor(0, 0, 0, *PDIMAROUND * fullAlpha);
        data.box   = monbox;
        g_pHyprRenderer->m_renderPass.add(makeUnique<CRectPassElement>(data));
    }

    renderdata.pos.x += pWindow->m_floatingOffset.x;
    renderdata.pos.y += pWindow->m_floatingOffset.y;

    // if window is floating and we have a slide animation, clip it to its full bb
    if (!ignorePosition && pWindow->m_isFloating && !pWindow->isFullscreen() && PWORKSPACE->m_renderOffset->isBeingAnimated() && !pWindow->m_pinned) {
        CRegion rg =
            pWindow->getFullWindowBoundingBox().translate(-pMonitor->m_position + PWORKSPACE->m_renderOffset->value() + pWindow->m_floatingOffset).scale(pMonitor->m_scale);
        renderdata.clipBox = rg.getExtents();
    }

    // render window decorations first, if not fullscreen full
    if (mode == RENDER_PASS_ALL || mode == RENDER_PASS_MAIN) {

        const bool TRANSFORMERSPRESENT = !pWindow->m_transformers.empty();

        if (TRANSFORMERSPRESENT) {
            g_pHyprOpenGL->bindOffMain();

            for (auto const& t : pWindow->m_transformers) {
                t->preWindowRender(&renderdata);
            }
        }

        if (renderdata.decorate) {
            for (auto const& wd : pWindow->m_windowDecorations) {
                if (wd->getDecorationLayer() != DECORATION_LAYER_BOTTOM)
                    continue;

                wd->draw(pMonitor, fullAlpha);
            }

            for (auto const& wd : pWindow->m_windowDecorations) {
                if (wd->getDecorationLayer() != DECORATION_LAYER_UNDER)
                    continue;

                wd->draw(pMonitor, fullAlpha);
            }
        }

        static auto PXWLUSENN = CConfigValue<Hyprlang::INT>("xwayland:use_nearest_neighbor");
        if ((pWindow->m_isX11 && *PXWLUSENN) || pWindow->m_ruleApplicator->nearestNeighbor().valueOrDefault())
            renderdata.useNearestNeighbor = true;

        if (pWindow->wlSurface()->small() && !pWindow->wlSurface()->m_fillIgnoreSmall && renderdata.blur) {
            CBox wb = {renderdata.pos.x - pMonitor->m_position.x, renderdata.pos.y - pMonitor->m_position.y, renderdata.w, renderdata.h};
            wb.scale(pMonitor->m_scale).round();
            CRectPassElement::SRectData data;
            data.color = CHyprColor(0, 0, 0, 0);
            data.box   = wb;
            data.round = renderdata.dontRound ? 0 : renderdata.rounding - 1;
            data.blur  = true;
            data.blurA = renderdata.fadeAlpha;
            data.xray  = g_pHyprOpenGL->shouldUseNewBlurOptimizations(nullptr, pWindow);
            g_pHyprRenderer->m_renderPass.add(makeUnique<CRectPassElement>(data));
            renderdata.blur = false;
        }

        renderdata.surfaceCounter = 0;
        pWindow->wlSurface()->resource()->breadthfirst(
            [&renderdata, &pWindow](SP<CWLSurfaceResource> s, const Vector2D& offset, void* data) {
                if (!s->m_current.texture)
                    return;

                if (s->m_current.size.x < 1 || s->m_current.size.y < 1)
                    return;

                renderdata.localPos    = offset;
                renderdata.texture     = s->m_current.texture;
                renderdata.surface     = s;
                renderdata.mainSurface = s == pWindow->wlSurface()->resource();
                g_pHyprRenderer->m_renderPass.add(makeUnique<CSurfacePassElement>(renderdata));
                renderdata.surfaceCounter++;
            },
            nullptr);

        renderdata.useNearestNeighbor = false;

        if (renderdata.decorate) {
            for (auto const& wd : pWindow->m_windowDecorations) {
                if (wd->getDecorationLayer() != DECORATION_LAYER_OVER)
                    continue;

                wd->draw(pMonitor, fullAlpha);
            }
        }

        if (TRANSFORMERSPRESENT) {
            CFramebuffer* last = g_pHyprOpenGL->m_renderData.currentFB;
            for (auto const& t : pWindow->m_transformers) {
                last = t->transform(last);
            }

            g_pHyprOpenGL->bindBackOnMain();
            g_pHyprOpenGL->renderOffToMain(last);
        }
    }

    g_pHyprOpenGL->m_renderData.clipBox = CBox();

    if (mode == RENDER_PASS_ALL || mode == RENDER_PASS_POPUP) {
        if (!pWindow->m_isX11) {
            CBox geom = pWindow->m_xdgSurface->m_current.geometry;

            renderdata.pos -= geom.pos();
            renderdata.dontRound       = true; // don't round popups
            renderdata.pMonitor        = pMonitor;
            renderdata.squishOversized = false; // don't squish popups
            renderdata.popup           = true;

            static CConfigValue PBLURIGNOREA = CConfigValue<Hyprlang::FLOAT>("decoration:blur:popups_ignorealpha");

            renderdata.blur = g_pHyprRenderer->shouldBlur(pWindow->m_popupHead);

            if (renderdata.blur) {
                renderdata.discardMode |= DISCARD_ALPHA;
                renderdata.discardOpacity = *PBLURIGNOREA;
            }

            if (pWindow->m_ruleApplicator->nearestNeighbor().valueOrDefault())
                renderdata.useNearestNeighbor = true;

            renderdata.surfaceCounter = 0;

            pWindow->m_popupHead->breadthfirst(
                [&renderdata](WP<Desktop::View::CPopup> popup, void* data) {
                    if (popup->m_fadingOut) {
                        g_pHyprRenderer->renderSnapshot(popup);
                        return;
                    }

                    if (!popup->aliveAndVisible())
                        return;

                    const auto     pos    = popup->coordsRelativeToParent();
                    const Vector2D oldPos = renderdata.pos;
                    renderdata.pos += pos;
                    renderdata.fadeAlpha = popup->m_alpha->value();

                    popup->wlSurface()->resource()->breadthfirst(
                        [&renderdata](SP<CWLSurfaceResource> s, const Vector2D& offset, void* data) {
                            if (!s->m_current.texture)
                                return;

                            if (s->m_current.size.x < 1 || s->m_current.size.y < 1)
                                return;

                            renderdata.localPos    = offset;
                            renderdata.texture     = s->m_current.texture;
                            renderdata.surface     = s;
                            renderdata.mainSurface = false;
                            g_pHyprRenderer->m_renderPass.add(makeUnique<CSurfacePassElement>(renderdata));
                            renderdata.surfaceCounter++;
                        },
                        data);

                    renderdata.pos = oldPos;
                },
                &renderdata);

            renderdata.alpha = 1.F;
        }

        if (decorate) {
            for (auto const& wd : pWindow->m_windowDecorations) {
                if (wd->getDecorationLayer() != DECORATION_LAYER_OVERLAY)
                    continue;

                wd->draw(pMonitor, fullAlpha);
            }
        }
    }

    EMIT_HOOK_EVENT("render", RENDER_POST_WINDOW);

    g_pHyprOpenGL->m_renderData.currentWindow.reset();
}

void screenshot_window_with_decos(CFramebuffer* buffer, PHLWINDOW w) {
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif
    if (!buffer || !pRenderWindow || !w)
        return;
    const auto m = w->m_monitor.lock();
    if (!m || !m->m_output || m->m_pixelSize.x <= 0 || m->m_pixelSize.y <= 0)
        return;
    CRegion fakeDamage{0, 0, INT16_MAX, INT16_MAX};

    g_pHyprRenderer->makeEGLCurrent();

    auto ex = g_pDecorationPositioner->getWindowDecorationExtents(w, false);
    buffer->alloc(m->m_pixelSize.x + ex.topLeft.x + ex.bottomRight.x, m->m_pixelSize.y + ex.topLeft.y + ex.bottomRight.y, DRM_FORMAT_ABGR8888);
    g_pHyprRenderer->beginRender(m, fakeDamage, RENDER_MODE_FULL_FAKE, nullptr, buffer);

    g_pHyprRenderer->m_bRenderingSnapshot = true;
    g_pHyprOpenGL->clear(CHyprColor(0, 0, 0, 0)); // JIC

    auto fo = w->m_floatingOffset;
    auto before = w->m_hidden;
    w->m_hidden = false;

    ourRenderWindow(w, m, Time::steadyNow(), true, RENDER_PASS_ALL, false, true);
    w->m_hidden = before;
    
    w->m_floatingOffset = fo;

    g_pHyprRenderer->endRender();

    g_pHyprRenderer->m_bRenderingSnapshot = false;
}

void screenshot_window(HyprWindow *hw, PHLWINDOW w, bool include_decorations) {
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif
    
    //return;
    if (!pRenderWindow || !w.get())
        return;
    const auto m = w->m_monitor.lock();
    if (!m || !m->m_output || m->m_pixelSize.x <= 0 || m->m_pixelSize.y <= 0)
        return;
    if (include_decorations) {
        bool h = w->m_hidden;
        w->m_hidden = false;
        screenshot_window_with_decos(hw->deco_fb, w);
       //m->m_scale 
        hw->w_decos_size = tobounds(w->getFullWindowBoundingBox());
        hw->w_decos_size.scale(m->m_scale);
        hw->w_deco_raw = tobounds(w->getFullWindowBoundingBox());

        w->m_hidden = h;

        return;
    }

    // we need to "damage" the entire monitor
    // so that we render the entire window
    // this is temporary, doesnt mess with the actual damage
    CRegion fakeDamage{0, 0, INT16_MAX, INT16_MAX};
    g_pHyprRenderer->makeEGLCurrent();
    hw->fb->alloc(m->m_pixelSize.x, m->m_pixelSize.y, DRM_FORMAT_ABGR8888);
    g_pHyprRenderer->beginRender(m, fakeDamage, RENDER_MODE_FULL_FAKE, nullptr, hw->fb);
    g_pHyprRenderer->m_bRenderingSnapshot = true;
    g_pHyprOpenGL->clear(CHyprColor(0, 0, 0, 0)); // JIC
    auto const NOW = Time::steadyNow();
    (*(tRenderWindow)pRenderWindow)(g_pHyprRenderer.get(), w, m, NOW, false, RENDER_PASS_MAIN, true, true);
    g_pHyprRenderer->endRender();
    g_pHyprRenderer->m_bRenderingSnapshot = false;

    hw->w_size = Bounds(0, 0, (w->m_realSize->value().x * m->m_scale), (w->m_realSize->value().y * m->m_scale));
    hw->w_bounds_raw = Bounds(0, 0, (w->m_realSize->value().x), (w->m_realSize->value().y));
}

void HyprIso::screenshot_all() {
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif
    for (auto w : g_pCompositor->m_windows) {
        bool has_mylar_bar = false;
        for (const auto &decos : w->m_windowDecorations) 
            if (decos->getDisplayName() == "MylarBar")
                has_mylar_bar = true;
            
        if (true) {
            for (auto hw : hyprwindows) {
                if (hw->w == w) {
                    if (!hw->fb)
                        hw->fb = new CFramebuffer;
                    screenshot_window(hw, w, false);
                }
            }
        }
    }
}

void HyprIso::screenshot(int id) {
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif
    for (auto w : g_pCompositor->m_windows) {
        bool has_mylar_bar = false;
        for (const auto &decos : w->m_windowDecorations) 
            if (decos->getDisplayName() == "MylarBar")
                has_mylar_bar = true;
            
        if (true) {
            for (auto hw : hyprwindows) {
                if (hw->w == w && hw->id == id) {
                    if (!hw->fb)
                        hw->fb = new CFramebuffer;
                    screenshot_window(hw, w, false);
                }
            }
        }
    }
}

void HyprIso::draw_workspace(int mon, int id, Bounds b, int rounding) {
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif
 
    //return;
    for (auto hs : hyprspaces) {
        if (hs->w->m_id == id) {
            if (!hs->buffer->isAllocated())
                continue;
            //notify("draw space " + std::to_string(id));
            AnyPass::AnyData anydata([b, hs, rounding](AnyPass* pass) {
    #ifdef TRACY_ENABLE
        ZoneScoped;
    #endif
     
                //notify("draw");
                auto roundingPower = 2.0f;
                auto cornermask = 0;
                auto tex = hs->buffer->getTexture();
                //notify(std::to_string(hs->w->m_id) + " " + std::to_string((unsigned long long) hs->buffer));
                
                auto box = tocbox(b);

                CHyprOpenGLImpl::STextureRenderData data;
                data.allowCustomUV = true;

                data.round = rounding;
                data.noAA = true;
                data.roundingPower = roundingPower;
                g_pHyprOpenGL->m_renderData.primarySurfaceUVTopLeft     = Vector2D(0, 0);
                g_pHyprOpenGL->m_renderData.primarySurfaceUVBottomRight = Vector2D(
                    std::min(1.0, 1.0),
                    std::min(1.0, 1.0)
                );
                set_rounding(cornermask);
                g_pHyprOpenGL->renderTexture(tex, box, data);
                set_rounding(0);
                g_pHyprOpenGL->m_renderData.primarySurfaceUVTopLeft     = Vector2D(-1, -1);
                g_pHyprOpenGL->m_renderData.primarySurfaceUVBottomRight = Vector2D(-1, -1);
            });
            g_pHyprRenderer->m_renderPass.add(makeUnique<AnyPass>(std::move(anydata)));
        }
    }
};

void HyprIso::draw_wallpaper(int mon, Bounds b, int rounding, float alpha) {
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif
 
    //return;
    for (auto hm : hyprmonitors) {
        if (hm->id != mon)
            continue;
        if (!hm->wallfb)
            continue;
        AnyPass::AnyData anydata([hm, mon, b, rounding, alpha](AnyPass* pass) {
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif
             //notify("draw");
            auto roundingPower = 2.0f;
            auto cornermask = 0;
            auto tex = hm->wallfb->getTexture();
            auto box = tocbox(b);

            CHyprOpenGLImpl::STextureRenderData data;
            data.allowCustomUV = true;

            data.round = rounding;
            data.roundingPower = roundingPower;
            data.a = alpha;
            g_pHyprOpenGL->m_renderData.primarySurfaceUVTopLeft     = Vector2D(0, 0);
            g_pHyprOpenGL->m_renderData.primarySurfaceUVBottomRight = Vector2D(
                std::min(1.0, 1.0),
                std::min(1.0, 1.0)
            );
            set_rounding(cornermask);
            g_pHyprOpenGL->renderTexture(tex, box, data);
            set_rounding(0);
            g_pHyprOpenGL->m_renderData.primarySurfaceUVTopLeft     = Vector2D(-1, -1);
            g_pHyprOpenGL->m_renderData.primarySurfaceUVBottomRight = Vector2D(-1, -1);
        });
        g_pHyprRenderer->m_renderPass.add(makeUnique<AnyPass>(std::move(anydata)));
    }
};

void HyprIso::screenshot_wallpaper(int mon) {
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif
   for (auto hm : hyprmonitors) {
       if (hm->id == mon) {
           if (!hm->wallfb)
               hm->wallfb = new CFramebuffer;
           actual_screenshot_wallpaper(hm->wallfb, hm->m);
           hm->wall_size = Bounds(hm->m->m_position.x, hm->m->m_position.y, 
               hm->m->m_pixelSize.x, hm->m->m_pixelSize.y);
       }
   }
}

void HyprIso::screenshot_space(int mon, int id) {
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif
    //notify("attempt screenshot " + std::to_string(id));
    for (auto hs : hyprspaces) {
        //notify("against " + std::to_string(hs->w->m_id));
        if (hs->w->m_id == id) {
            if (!hs->buffer)
                hs->buffer = new CFramebuffer;
            
            //notify("screenshot " + std::to_string(id));
            screenshot_workspace(hs->buffer, hs->w, hs->w->m_monitor.lock(), false);
            break;
        }
        // for (auto hm : hyprmonitors) {
        //     if (hs->w.lock() && hs->w->m_monitor == hm->m && mon == hm->id) {
        //         if (hs->w->m_id == id) {
        //             screenshot_workspace(hs->buffer, hs->w, hm->m, false);
        //             return;
        //         }
        //     }
        // }
    }
}

void HyprIso::screenshot_deco(int id) {
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif
    for (auto w : g_pCompositor->m_windows) {
        for (auto hw : hyprwindows) {
            if (hw->w == w && hw->id == id) {
                if (!hw->deco_fb)
                    hw->deco_fb = new CFramebuffer;
                screenshot_window(hw, w, true);
            }
        }
    }
}

Bounds HyprIso::thumbnail_size(int id) {
    for (auto hw : hyprwindows) {
        if (hw->id == id) {
            return hw->w_bounds_raw;
        }
    }
    return {1, 1, 1, 1};
}

// Will stretch the thumbnail if the aspect ratio passed in is different from thumbnail
void HyprIso::draw_thumbnail(int id, Bounds b, int rounding, float roundingPower, int cornermask, float alpha) {
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif
 
    // return;
    for (auto hw : hyprwindows) {
        if (hw->id == id) {
            if (hw->fb && hw->fb->isAllocated()) {
                bool clip = this->clip;
                Bounds clipbox = this->clipbox;
                AnyPass::AnyData anydata([id, b, hw, rounding, roundingPower, cornermask, alpha, clip, clipbox](AnyPass* pass) {
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif
 
                    auto tex = hw->fb->getTexture();
                    auto box = tocbox(b);
                    CHyprOpenGLImpl::STextureRenderData data;
                    data.allowCustomUV = true;
                    data.round = rounding;
                    data.roundingPower = roundingPower;
                    data.a = alpha;
                    g_pHyprOpenGL->m_renderData.primarySurfaceUVTopLeft     = Vector2D(0, 0);
                    g_pHyprOpenGL->m_renderData.primarySurfaceUVBottomRight = Vector2D(
                        std::min(hw->w_size.w / hw->fb->m_size.x, 1.0), 
                        std::min(hw->w_size.h / hw->fb->m_size.y, 1.0) 
                    );
                    set_rounding(cornermask);
                    if (clip)
                        g_pHyprOpenGL->m_renderData.clipBox = tocbox(clipbox);
                    g_pHyprOpenGL->renderTexture(tex, box, data);
                    set_rounding(0);
                    g_pHyprOpenGL->m_renderData.primarySurfaceUVTopLeft     = Vector2D(-1, -1);
                    g_pHyprOpenGL->m_renderData.primarySurfaceUVBottomRight = Vector2D(-1, -1);
                    if (clip)
                        g_pHyprOpenGL->m_renderData.clipBox = tocbox(Bounds());
                });
                g_pHyprRenderer->m_renderPass.add(makeUnique<AnyPass>(std::move(anydata)));
            }
        }
    }
}

void HyprIso::draw_deco_thumbnail(int id, Bounds b, int rounding, float roundingPower, int cornermask) {
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif
    // return;
    for (auto hw : hyprwindows) {
        if (hw->id == id) {
            if (hw->deco_fb && hw->deco_fb->isAllocated()) {
                AnyPass::AnyData anydata([id, b, hw, rounding, roundingPower, cornermask](AnyPass* pass) {
                    auto tex = hw->deco_fb->getTexture();
                    auto box = tocbox(b);
                    CHyprOpenGLImpl::STextureRenderData data;
                    data.allowCustomUV = true;
                    data.round = rounding;
                    data.roundingPower = roundingPower;
                    g_pHyprOpenGL->m_renderData.primarySurfaceUVTopLeft     = Vector2D(0, 0);
                    g_pHyprOpenGL->m_renderData.primarySurfaceUVBottomRight = Vector2D(
                        std::min(hw->w_decos_size.w / hw->deco_fb->m_size.x, 1.0), 
                        std::min(hw->w_decos_size.h / hw->deco_fb->m_size.y, 1.0) 
                    );
                    set_rounding(cornermask);
                    g_pHyprOpenGL->renderTexture(tex, box, data);
                    set_rounding(0);
                    g_pHyprOpenGL->m_renderData.primarySurfaceUVTopLeft     = Vector2D(-1, -1);
                    g_pHyprOpenGL->m_renderData.primarySurfaceUVBottomRight = Vector2D(-1, -1);
                });
                g_pHyprRenderer->m_renderPass.add(makeUnique<AnyPass>(std::move(anydata)));
            }
        }
    }
}

Bounds lerp(Bounds start, Bounds end, float scalar) {
    return {
        start.x + (end.x - start.x) * scalar,
        start.y + (end.y - start.y) * scalar,
        start.w + (end.w - start.w) * scalar,
        start.h + (end.h - start.h) * scalar
    };
    return end; 
}

double easeIn(double x) {
    return x * x;
}

void HyprIso::draw_raw_min_thumbnail(int id, Bounds b, float scalar) {
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif
    for (auto hw : hyprwindows) {
        if (hw->id == id) {
            if (hw->min_fb && hw->min_fb->isAllocated()) {
                if (!hw->animate_to_dock)
                    return;
                AnyPass::AnyData anydata([id, b, hw, scalar](AnyPass* pass) {
                    auto tex = hw->min_fb->getTexture();
                    auto sss = hw->w_min_mon;
                    auto ex = g_pDecorationPositioner->getWindowDecorationExtents(hw->w, false);
                    Bounds bounds = {0.0f, 0.0f, sss.w + ex.topLeft.x + ex.bottomRight.x, sss.h + ex.bottomRight.y + ex.topLeft.y};
                    auto lerped = lerp(bounds, b, scalar);
                    if (!hw->w->m_hidden)
                        lerped = lerp(b, bounds, scalar);
                    auto box = tocbox(lerped);
                    CHyprOpenGLImpl::STextureRenderData data;
                    data.allowCustomUV = false;
                    data.round = 0.0;
                    if (hw->w->m_hidden) {
                        data.a = 1.0 - easeIn(scalar);
                    } else {
                        data.a = scalar;
                    }
                    data.roundingPower = 2.0;
                    g_pHyprOpenGL->renderTexture(tex, box, data);
                    g_pHyprOpenGL->m_renderData.primarySurfaceUVTopLeft     = Vector2D(-1, -1);
                    g_pHyprOpenGL->m_renderData.primarySurfaceUVBottomRight = Vector2D(-1, -1);
                });
                g_pHyprRenderer->m_renderPass.add(makeUnique<AnyPass>(std::move(anydata)));
                
                //border(hw->w_min_size, {1, 0, 0, 1}, 10);
            }
        }
    }
}

void HyprIso::draw_raw_deco_thumbnail(int id, Bounds b, int rounding, float roundingPower, int cornermask) {
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif
    // return;
    for (auto hw : hyprwindows) {
        if (hw->id == id) {
            if (hw->deco_fb && hw->deco_fb->isAllocated()) {
                AnyPass::AnyData anydata([id, b, hw, rounding, roundingPower, cornermask](AnyPass* pass) {
                    auto tex = hw->deco_fb->getTexture();
                    auto box = tocbox(b);
                    CHyprOpenGLImpl::STextureRenderData data;
                    data.allowCustomUV = true;
                    data.round = rounding;
                    data.roundingPower = roundingPower;
                    //g_pHyprOpenGL->m_renderData.primarySurfaceUVTopLeft     = Vector2D(0, 0);
                    //g_pHyprOpenGL->m_renderData.primarySurfaceUVBottomRight = Vector2D(
                        //std::min(hw->w_decos_size.w / hw->deco_fb->m_size.x, 1.0), 
                        //std::min(hw->w_decos_size.h / hw->deco_fb->m_size.y, 1.0) 
                    //);
                    set_rounding(cornermask);
                    g_pHyprOpenGL->renderTexture(tex, box, data);
                    set_rounding(0);
                    g_pHyprOpenGL->m_renderData.primarySurfaceUVTopLeft     = Vector2D(-1, -1);
                    g_pHyprOpenGL->m_renderData.primarySurfaceUVBottomRight = Vector2D(-1, -1);
                });
                g_pHyprRenderer->m_renderPass.add(makeUnique<AnyPass>(std::move(anydata)));
            }
        }
    }
}


void HyprIso::set_zoom_factor(float amount, bool instant) {
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif
    Hyprlang::CConfigValue* val = g_pConfigManager->getHyprlangConfigValuePtr("cursor:zoom_factor");
    auto zoom_amount = (Hyprlang::FLOAT*)val->dataPtr();
    *zoom_amount = amount;

    for (auto const& m : g_pCompositor->m_monitors) {
        if (m->m_cursorZoom) {
            if (instant)
                m->m_cursorZoom->setValueAndWarp(amount);
            else 
                *(m->m_cursorZoom) = amount;
            g_pLayoutManager->getCurrentLayout()->recalculateMonitor(m->m_id);
        }
    }    
}

int HyprIso::parent(int id) {
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif
    for (auto hw : hyprwindows) {
        if (hw->id == id) {
            if (auto w = hw->w->parent()) {
                for (auto hw : hyprwindows) {
                    if (hw->w == w) {
                        return hw->id;
                    }
                }
            }            
        }
    }
    return -1;
}

void HyprIso::show_desktop() {
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif
    for (auto hw : hyprwindows) {
        hypriso->set_hidden(hw->id, hw->was_hidden);
    }
    for (auto r : hyprmonitors) {
        hypriso->damage_entire(r->id);
    }
}

void HyprIso::hide_desktop() {
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif
    for (auto hw : hyprwindows) {
        hw->was_hidden = hw->is_hidden;
        hypriso->set_hidden(hw->id, true);
    }
    for (auto r : hyprmonitors) {
        hypriso->damage_entire(r->id);
    }
}

static void updateRelativeCursorCoords() {
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif
    static auto PNOWARPS = CConfigValue<Hyprlang::INT>("cursor:no_warps");

    if (*PNOWARPS)
        return;

    if (Desktop::focusState()->window())
        Desktop::focusState()->window()->m_relativeCursorCoordsOnLastWarp = g_pInputManager->getMouseCoordsInternal() - Desktop::focusState()->window()->m_position;
}

void HyprIso::move_to_workspace(int workspace) {
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif
    g_pKeybindManager->changeworkspace(std::to_string(workspace));
}

void HyprIso::move_to_workspace_id(int workspace) {
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif
    for (auto s : hyprspaces) {
        if (s->id == workspace) {
            g_pKeybindManager->changeworkspace(std::to_string(s->w->m_id));
        }
    }
}


void HyprIso::move_to_workspace(int id, int workspace) {
#ifdef FORK_WARN
    static_assert(false, "[Function Body] Make sure our `CSurfacePassElement::getTexBox()` and Hyprland's are synced!");
#endif
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif
    PHLWINDOW PWINDOW;
    for (auto hw : hyprwindows) {
        if (hw->id == id) {
            PWINDOW = hw->w;
            break;
        }
    }

    if (!PWINDOW)
        return;
    std::string args = std::to_string(workspace);

    const auto& [WORKSPACEID, workspaceName, isAutoID] = getWorkspaceIDNameFromString(args);
    if (WORKSPACEID == WORKSPACE_INVALID) {
        Log::logger->log(Log::DEBUG, "Invalid workspace in moveActiveToWorkspace");
        return;
    }

    if (WORKSPACEID == PWINDOW->workspaceID()) {
        Log::logger->log(Log::DEBUG, "Not moving to workspace because it didn't change.");
        return;
    }

    auto       pWorkspace = g_pCompositor->getWorkspaceByID(WORKSPACEID);
    PHLMONITOR pMonitor   = nullptr;
    const auto POLDWS     = PWINDOW->m_workspace;

    updateRelativeCursorCoords();

    g_pHyprRenderer->damageWindow(PWINDOW);

    if (pWorkspace) {
        const auto FULLSCREENMODE = PWINDOW->m_fullscreenState.internal;
        g_pCompositor->moveWindowToWorkspaceSafe(PWINDOW, pWorkspace);
        pMonitor = pWorkspace->m_monitor.lock();
        Desktop::focusState()->rawMonitorFocus(pMonitor);
        g_pCompositor->setWindowFullscreenInternal(PWINDOW, FULLSCREENMODE);
    } else {
        pWorkspace = g_pCompositor->createNewWorkspace(WORKSPACEID, PWINDOW->monitorID(), workspaceName, false);
        pMonitor   = pWorkspace->m_monitor.lock();
        g_pCompositor->moveWindowToWorkspaceSafe(PWINDOW, pWorkspace);
    }

    POLDWS->m_lastFocusedWindow = POLDWS->getFirstWindow();

    if (pWorkspace->m_isSpecialWorkspace)
        pMonitor->setSpecialWorkspace(pWorkspace);
    else if (POLDWS->m_isSpecialWorkspace)
        POLDWS->m_monitor.lock()->setSpecialWorkspace(nullptr);

    pMonitor->changeWorkspace(pWorkspace);

    Desktop::focusState()->fullWindowFocus(PWINDOW);
    PWINDOW->warpCursor();
}

void HyprIso::reload() {
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif
    const char* home = std::getenv("HOME");
    if (home) {
        std::filesystem::path filepath = std::filesystem::path(home) / ".config/mylar/default.conf";
        change_root_config_path(filepath, false);
    }
    
    g_pConfigManager->reload(); 
    for (auto w : g_pCompositor->m_windows)
        w->updateDecorationValues();
    //g_pHyprOpenGL->initShaders();
}

void HyprIso::add_float_rule() {
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif
    return;
    g_pConfigManager->handleWindowrule("windowrule", "fmatch:class:.*, float on");
}

void HyprIso::overwrite_defaults() {
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif
    return;
/*
    {
        Hyprlang::CConfigValue* val = g_pConfigManager->getHyprlangConfigValuePtr("decoration:blur:enabled");
        auto target = (Hyprlang::INT*)val->dataPtr();
        *target = 1;
    }
    {
        Hyprlang::CConfigValue* val = g_pConfigManager->getHyprlangConfigValuePtr("decoration:blur:size");
        auto target = (Hyprlang::INT*)val->dataPtr();
        *target = 6;
    }
    {
        Hyprlang::CConfigValue* val = g_pConfigManager->getHyprlangConfigValuePtr("decoration:blur:passes");
        auto target = (Hyprlang::INT*)val->dataPtr();
        *target = 3;
    }
    {
        Hyprlang::CConfigValue* val = g_pConfigManager->getHyprlangConfigValuePtr("decoration:blur:noise");
        auto target = (Hyprlang::FLOAT*)val->dataPtr();
        *target = .04;
    }
    {
        Hyprlang::CConfigValue* val = g_pConfigManager->getHyprlangConfigValuePtr("decoration:blur:vibrancy");
        auto target = (Hyprlang::FLOAT*)val->dataPtr();
        *target = .4696;
    }
    */
    {
        Hyprlang::CConfigValue* val = g_pConfigManager->getHyprlangConfigValuePtr("cursor:zoom_rigid");
        auto target = (Hyprlang::INT*)val->dataPtr();
        *target = false;
    }
    {
        Hyprlang::CConfigValue* val = g_pConfigManager->getHyprlangConfigValuePtr("cursor:zoom_disable_aa");
        auto target = (Hyprlang::INT*)val->dataPtr();
        *target = true;
    }

    //g_pConfigManager->handleWindowRule("windowrulev2", "float, class:.*");
}

Bounds HyprIso::floating_offset(int id) {
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif
    for (auto hw : hyprwindows) {
        if (hw->id == id) {
            return {hw->w->m_floatingOffset.x, hw->w->m_floatingOffset.y, 0, 0};
        }
    }

    return {};
}
Bounds HyprIso::workspace_offset(int id) {
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif
    for (auto hw : hyprwindows) {
        if (hw->id == id) {
            if (hw->w->m_workspace) {
                if (!hw->w->m_pinned) {
                    auto off = hw->w->m_workspace->m_renderOffset->value();
                    return {off.x, off.y, 0, 0};
                }
            }
        }
    }
    return {};
}

bool HyprIso::has_focus(int client) {
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif
    for (auto hw: hyprwindows) {
        if (hw->id == client) {
            return hw->w == Desktop::focusState()->window();
        }
    }
    return false;
}

/*
PHLWINDOW vectorToWindowUnified(const Vector2D& pos, uint8_t properties, PHLWINDOW pIgnoreWindow) {
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif
    const auto  PMONITOR          = g_pCompositor->getMonitorFromVector(pos);
    static auto PRESIZEONBORDER   = CConfigValue<Hyprlang::INT>("general:resize_on_border");
    static auto PBORDERSIZE       = CConfigValue<Hyprlang::INT>("general:border_size");
    static auto PBORDERGRABEXTEND = CConfigValue<Hyprlang::INT>("general:extend_border_grab_area");
    static auto PSPECIALFALLTHRU  = CConfigValue<Hyprlang::INT>("input:special_fallthrough");
    const auto  BORDER_GRAB_AREA  = *PRESIZEONBORDER ? *PBORDERSIZE + *PBORDERGRABEXTEND : 0;
    const bool  ONLY_PRIORITY     = properties & FOCUS_PRIORITY;

    // pinned windows on top of floating regardless
    if (properties & ALLOW_FLOATING) {
        for (auto const& w : g_pCompositor->m_windows | std::views::reverse) {
            if (ONLY_PRIORITY && !w->priorityFocus())
                continue;

            if (w->m_isFloating && w->m_isMapped && !w->isHidden() && !w->m_X11ShouldntFocus && w->m_pinned && !w->m_ruleApplicator->noFocus().valueOrDefault() && w != pIgnoreWindow) {
                const auto BB  = w->getWindowBoxUnified(properties);
                CBox       box = BB.copy().expand(!w->isX11OverrideRedirect() ? BORDER_GRAB_AREA : 0);
                if (box.containsPoint(g_pPointerManager->position()))
                    return w;

                if (!w->m_isX11) {
                    if (w->hasPopupAt(pos))
                        return w;
                }
            }
        }
    }

    auto windowForWorkspace = [&](bool special) -> PHLWINDOW {
        auto floating = [&](bool aboveFullscreen) -> PHLWINDOW {
            for (auto const& w : g_pCompositor->m_windows | std::views::reverse) {

                if (special && !w->onSpecialWorkspace()) // because special floating may creep up into regular
                    continue;

                if (!w->m_workspace)
                    continue;

                if (ONLY_PRIORITY && !w->priorityFocus())
                    continue;

                const auto PWINDOWMONITOR = w->m_monitor.lock();

                // to avoid focusing windows behind special workspaces from other monitors
                if (!*PSPECIALFALLTHRU && PWINDOWMONITOR && PWINDOWMONITOR->m_activeSpecialWorkspace && w->m_workspace != PWINDOWMONITOR->m_activeSpecialWorkspace) {
                    const auto BB = w->getWindowBoxUnified(properties);
                    if (BB.x >= PWINDOWMONITOR->m_position.x && BB.y >= PWINDOWMONITOR->m_position.y &&
                        BB.x + BB.width <= PWINDOWMONITOR->m_position.x + PWINDOWMONITOR->m_size.x && BB.y + BB.height <= PWINDOWMONITOR->m_position.y + PWINDOWMONITOR->m_size.y)
                        continue;
                }

                if (w->m_isMapped && w->m_workspace->isVisible() && !w->isHidden() && !w->m_ruleApplicator->noFocus().valueOrDefault() &&
+                    w != pIgnoreWindow) {
                    // OR windows should add focus to parent
                    if (w->m_X11ShouldntFocus && !w->isX11OverrideRedirect())
                        continue;

                    const auto BB  = w->getWindowBoxUnified(properties);
                    CBox       box = BB.copy().expand(!w->isX11OverrideRedirect() ? BORDER_GRAB_AREA : 0);
                    if (box.containsPoint(g_pPointerManager->position())) {

                        if (w->m_isX11 && w->isX11OverrideRedirect() && !w->m_xwaylandSurface->wantsFocus()) {
                            // Override Redirect
                            return g_pCompositor->m_lastWindow.lock(); // we kinda trick everything here.
                            // TODO: this is wrong, we should focus the parent, but idk how to get it considering it's nullptr in most cases.
                        }

                        return w;
                    }

                    if (!w->m_isX11) {
                        if (w->hasPopupAt(pos))
                            return w;
                    }
                }
            }

            return nullptr;
        };

        if (properties & ALLOW_FLOATING) {
            // first loop over floating cuz they're above, m_lWindows should be sorted bottom->top, for tiled it doesn't matter.
            auto found = floating(true);
            if (found)
                return found;
        }

        if (properties & FLOATING_ONLY) {
            //return floating(false);
            return nullptr;
        }

        const WORKSPACEID WSPID      = special ? PMONITOR->activeSpecialWorkspaceID() : PMONITOR->activeWorkspaceID();
        const auto        PWORKSPACE = g_pCompositor->getWorkspaceByID(WSPID);

        if (PWORKSPACE->m_hasFullscreenWindow && !(properties & SKIP_FULLSCREEN_PRIORITY) && !ONLY_PRIORITY)
            return PWORKSPACE->getFullscreenWindow();

        auto found = floating(false);
        if (found)
            return found;

        // for windows, we need to check their extensions too, first.
        for (auto const& w : g_pCompositor->m_windows) {
            if (ONLY_PRIORITY && !w->priorityFocus())
                continue;

            if (special != w->onSpecialWorkspace())
                continue;

            if (!w->m_workspace)
                continue;

            if (!w->m_isX11 && !w->m_isFloating && w->m_isMapped && w->workspaceID() == WSPID && !w->isHidden() && !w->m_X11ShouldntFocus &&
                !w->m_windowData->noFocus().valueOrDefault() && w != pIgnoreWindow) {
                if (w->hasPopupAt(pos))
                    return w;
            }
        }

        for (auto const& w : g_pCompositor->m_windows) {
            if (ONLY_PRIORITY && !w->priorityFocus())
                continue;

            if (special != w->onSpecialWorkspace())
                continue;

            if (!w->m_workspace)
                continue;

            if (!w->m_isFloating && w->m_isMapped && w->workspaceID() == WSPID && !w->isHidden() && !w->m_X11ShouldntFocus && !w->m_windowData().noFocus.valueOrDefault() &&
                w != pIgnoreWindow) {
                CBox box = (properties & USE_PROP_TILED) ? w->getWindowBoxUnified(properties) : CBox{w->m_position, w->m_size};
                if (box.containsPoint(pos))
                    return w;
            }
        }

        return nullptr;
    };

    // special workspace
    if (PMONITOR->m_activeSpecialWorkspace && !*PSPECIALFALLTHRU)
        return windowForWorkspace(true);

    if (PMONITOR->m_activeSpecialWorkspace) {
        const auto PWINDOW = windowForWorkspace(true);

        if (PWINDOW)
            return PWINDOW;
    }

    return windowForWorkspace(false);
}

void mouseMoveUnified(uint32_t time, bool refocus, bool mouse, std::optional<Vector2D> overridePos) {
    assert(false && "Update to latest version 52");
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif
    g_pInputManager->m_lastInputMouse = mouse;

    if (!g_pCompositor->m_readyToProcess || g_pCompositor->m_isShuttingDown || g_pCompositor->m_unsafeState)
        return;

    Vector2D const mouseCoords        = overridePos.value_or(g_pInputManager->getMouseCoordsInternal());
    auto const     MOUSECOORDSFLOORED = mouseCoords.floor();

    if (MOUSECOORDSFLOORED == g_pInputManager->m_lastCursorPosFloored && !refocus)
        return;

    static auto PFOLLOWMOUSE          = CConfigValue<Hyprlang::INT>("input:follow_mouse");
    static auto PFOLLOWMOUSETHRESHOLD = CConfigValue<Hyprlang::FLOAT>("input:follow_mouse_threshold");
    static auto PMOUSEREFOCUS         = CConfigValue<Hyprlang::INT>("input:mouse_refocus");
    static auto PFOLLOWONDND          = CConfigValue<Hyprlang::INT>("misc:always_follow_on_dnd");
    static auto PFLOATBEHAVIOR        = CConfigValue<Hyprlang::INT>("input:float_switch_override_focus");
    static auto PMOUSEFOCUSMON        = CConfigValue<Hyprlang::INT>("misc:mouse_move_focuses_monitor");
    static auto PRESIZEONBORDER       = CConfigValue<Hyprlang::INT>("general:resize_on_border");
    static auto PRESIZECURSORICON     = CConfigValue<Hyprlang::INT>("general:hover_icon_on_border");

//CWLDataDeviceProtocol

    const auto  FOLLOWMOUSE = *PFOLLOWONDND && PROTO::data->dndActive() ? 1 : *PFOLLOWMOUSE;

    if (FOLLOWMOUSE == 1 && g_pInputManager->m_lastCursorMovement.getSeconds() < 0.5)
        g_pInputManager->m_mousePosDelta += MOUSECOORDSFLOORED.distance(g_pInputManager->m_lastCursorPosFloored);
    else
        g_pInputManager->m_mousePosDelta = 0;

    g_pInputManager->m_foundSurfaceToFocus.reset();
    g_pInputManager->m_foundLSToFocus.reset();
    g_pInputManager->m_foundWindowToFocus.reset();
    SP<CWLSurfaceResource> foundSurface;
    Vector2D               surfaceCoords;
    Vector2D               surfacePos = Vector2D(-1337, -1337);
    PHLWINDOW              pFoundWindow;
    PHLLS                  pFoundLayerSurface;

    EMIT_HOOK_EVENT_CANCELLABLE("mouseMove", MOUSECOORDSFLOORED);

    g_pInputManager->m_lastCursorPosFloored = MOUSECOORDSFLOORED;

    const auto PMONITOR = g_pInputManager->isLocked() && g_pCompositor->m_lastMonitor ? g_pCompositor->m_lastMonitor.lock() : g_pCompositor->getMonitorFromCursor();

    // this can happen if there are no displays hooked up to Hyprland
    if (PMONITOR == nullptr)
        return;

    if (PMONITOR->m_cursorZoom->value() != 1.f)
        g_pHyprRenderer->damageMonitor(PMONITOR);

    bool skipFrameSchedule = PMONITOR->shouldSkipScheduleFrameOnMouseEvent();

    if (!PMONITOR->m_solitaryClient.lock() && g_pHyprRenderer->shouldRenderCursor() && g_pPointerManager->softwareLockedFor(PMONITOR->m_self.lock()) && !skipFrameSchedule)
        g_pCompositor->scheduleFrameForMonitor(PMONITOR, Aquamarine::IOutput::AQ_SCHEDULE_CURSOR_MOVE);

    // constraints
    if (!g_pSeatManager->m_mouse.expired() && g_pInputManager->isConstrained()) {
        const auto SURF       = CWLSurface::fromResource(g_pCompositor->m_lastFocus.lock());
        const auto CONSTRAINT = SURF ? SURF->constraint() : nullptr;

        if (CONSTRAINT) {
            if (CONSTRAINT->isLocked()) {
                const auto HINT = CONSTRAINT->logicPositionHint();
                g_pCompositor->warpCursorTo(HINT, true);
            } else {
                const auto RG           = CONSTRAINT->logicConstraintRegion();
                const auto CLOSEST      = RG.closestPoint(mouseCoords);
                const auto BOX          = SURF->getSurfaceBoxGlobal();
                const auto CLOSESTLOCAL = (CLOSEST - (BOX.has_value() ? BOX->pos() : Vector2D{})) * (SURF->getWindow() ? SURF->getWindow()->m_X11SurfaceScaledBy : 1.0);

                g_pCompositor->warpCursorTo(CLOSEST, true);
                g_pSeatManager->sendPointerMotion(time, CLOSESTLOCAL);
                PROTO::relativePointer->sendRelativeMotion(sc<uint64_t>(time) * 1000, {}, {});
            }

            return;

        } else
            Debug::log(ERR, "BUG THIS: Null SURF/CONSTRAINT in mouse refocus. Ignoring constraints. {:x} {:x}", rc<uintptr_t>(SURF.get()), rc<uintptr_t>(CONSTRAINT.get()));
    }

    if (PMONITOR != g_pCompositor->m_lastMonitor && (*PMOUSEFOCUSMON || refocus) && g_pInputManager->m_forcedFocus.expired())
        g_pCompositor->setActiveMonitor(PMONITOR);

    // check for windows that have focus priority like our permission popups
    pFoundWindow = g_pCompositor->vectorToWindowUnified(mouseCoords, FOCUS_PRIORITY);
    if (pFoundWindow)
        foundSurface = g_pCompositor->vectorWindowToSurface(mouseCoords, pFoundWindow, surfaceCoords);

    if (!foundSurface && g_pSessionLockManager->isSessionLocked()) {

        // set keyboard focus on session lock surface regardless of layers
        const auto PSESSIONLOCKSURFACE = g_pSessionLockManager->getSessionLockSurfaceForMonitor(PMONITOR->m_id);
        const auto foundLockSurface    = PSESSIONLOCKSURFACE ? PSESSIONLOCKSURFACE->surface->surface() : nullptr;

        g_pCompositor->focusSurface(foundLockSurface);

        // search for interactable abovelock surfaces for pointer focus, or use session lock surface if not found
        for (auto& lsl : PMONITOR->m_layerSurfaceLayers | std::views::reverse) {
            foundSurface = g_pCompositor->vectorToLayerSurface(mouseCoords, &lsl, &surfaceCoords, &pFoundLayerSurface, true);

            if (foundSurface)
                break;
        }

        if (!foundSurface) {
            surfaceCoords = mouseCoords - PMONITOR->m_position;
            foundSurface  = foundLockSurface;
        }

        if (refocus) {
            g_pInputManager->m_foundLSToFocus      = pFoundLayerSurface;
            g_pInputManager->m_foundWindowToFocus  = pFoundWindow;
            g_pInputManager->m_foundSurfaceToFocus = foundSurface;
        }

        g_pSeatManager->setPointerFocus(foundSurface, surfaceCoords);
        g_pSeatManager->sendPointerMotion(time, surfaceCoords);

        return;
    }

    PHLWINDOW forcedFocus = g_pInputManager->m_forcedFocus.lock();

    if (!forcedFocus)
        forcedFocus = g_pCompositor->getForceFocus();

    if (forcedFocus && !foundSurface) {
        pFoundWindow = forcedFocus;
        surfacePos   = pFoundWindow->m_realPosition->value();
        foundSurface = pFoundWindow->m_wlSurface->resource();
    }

    // if we are holding a pointer button,
    // and we're not dnd-ing, don't refocus. Keep focus on last surface.
    if (!PROTO::data->dndActive() && !g_pInputManager->m_currentlyHeldButtons.empty() && g_pCompositor->m_lastFocus && g_pCompositor->m_lastFocus->m_mapped &&
        g_pSeatManager->m_state.pointerFocus && !g_pInputManager->m_hardInput) {
        foundSurface = g_pSeatManager->m_state.pointerFocus.lock();

        // IME popups aren't desktop-like elements
        // TODO: make them.
        CInputPopup* foundPopup = g_pInputManager->m_relay.popupFromSurface(foundSurface);
        if (foundPopup) {
            surfacePos             = foundPopup->globalBox().pos();
            g_pInputManager->m_focusHeldByButtons   = true;
            g_pInputManager->m_refocusHeldByButtons = refocus;
        } else {
            auto HLSurface = CWLSurface::fromResource(foundSurface);

            if (HLSurface) {
                const auto BOX = HLSurface->getSurfaceBoxGlobal();

                if (BOX) {
                    const auto PWINDOW = HLSurface->getWindow();
                    surfacePos         = BOX->pos();
                    pFoundLayerSurface = HLSurface->getLayer();
                    if (!pFoundLayerSurface)
                        pFoundWindow = !PWINDOW || PWINDOW->isHidden() ? g_pCompositor->m_lastWindow.lock() : PWINDOW;
                } else // reset foundSurface, find one normally
                    foundSurface = nullptr;
            } else // reset foundSurface, find one normally
                foundSurface = nullptr;
        }
    }

    g_pLayoutManager->getCurrentLayout()->onMouseMove(g_pInputManager->getMouseCoordsInternal());

    // forced above all
    if (!g_pInputManager->m_exclusiveLSes.empty()) {
        if (!foundSurface)
            foundSurface = g_pCompositor->vectorToLayerSurface(mouseCoords, &g_pInputManager->m_exclusiveLSes, &surfaceCoords, &pFoundLayerSurface);

        if (!foundSurface) {
            foundSurface = (*g_pInputManager->m_exclusiveLSes.begin())->m_surface->resource();
            surfacePos   = (*g_pInputManager->m_exclusiveLSes.begin())->m_realPosition->goal();
        }
    }

    if (!foundSurface)
        foundSurface = g_pCompositor->vectorToLayerPopupSurface(mouseCoords, PMONITOR, &surfaceCoords, &pFoundLayerSurface);

    // overlays are above fullscreen
    if (!foundSurface)
        foundSurface = g_pCompositor->vectorToLayerSurface(mouseCoords, &PMONITOR->m_layerSurfaceLayers[ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY], &surfaceCoords, &pFoundLayerSurface);

    // also IME popups
    if (!foundSurface) {
        auto popup = g_pInputManager->m_relay.popupFromCoords(mouseCoords);
        if (popup) {
            foundSurface = popup->getSurface();
            surfacePos   = popup->globalBox().pos();
        }
    }

    // also top layers
    if (!foundSurface)
        foundSurface = g_pCompositor->vectorToLayerSurface(mouseCoords, &PMONITOR->m_layerSurfaceLayers[ZWLR_LAYER_SHELL_V1_LAYER_TOP], &surfaceCoords, &pFoundLayerSurface);

    // then, we check if the workspace doesn't have a fullscreen window
    const auto PWORKSPACE   = PMONITOR->m_activeSpecialWorkspace ? PMONITOR->m_activeSpecialWorkspace : PMONITOR->m_activeWorkspace;
    const auto PWINDOWIDEAL = g_pCompositor->vectorToWindowUnified(mouseCoords, RESERVED_EXTENTS | INPUT_EXTENTS | ALLOW_FLOATING);
    if (PWORKSPACE->m_hasFullscreenWindow && !foundSurface && PWORKSPACE->m_fullscreenMode == FSMODE_FULLSCREEN) {
        pFoundWindow = PWORKSPACE->getFullscreenWindow();

        if (!pFoundWindow) {
            // what the fuck, somehow happens occasionally??
            PWORKSPACE->m_hasFullscreenWindow = false;
            return;
        }

        if (PWINDOWIDEAL &&
            ((PWINDOWIDEAL->m_isFloating && (PWINDOWIDEAL->m_createdOverFullscreen || PWINDOWIDEAL->m_pinned))
             || (PMONITOR->m_activeSpecialWorkspace == PWINDOWIDEAL->m_workspace)))
            pFoundWindow = PWINDOWIDEAL;

        if (!pFoundWindow->m_isX11) {
            foundSurface = g_pCompositor->vectorWindowToSurface(mouseCoords, pFoundWindow, surfaceCoords);
            surfacePos   = Vector2D(-1337, -1337);
        } else {
            foundSurface = pFoundWindow->m_wlSurface->resource();
            surfacePos   = pFoundWindow->m_realPosition->value();
        }
    }

    // then windows
    if (!foundSurface) {
        if (PWORKSPACE->m_hasFullscreenWindow && PWORKSPACE->m_fullscreenMode == FSMODE_MAXIMIZED) {
            if (!foundSurface) {
                if (PMONITOR->m_activeSpecialWorkspace) {
                    if (pFoundWindow != PWINDOWIDEAL)
                        pFoundWindow = g_pCompositor->vectorToWindowUnified(mouseCoords, RESERVED_EXTENTS | INPUT_EXTENTS | ALLOW_FLOATING);

                    if (pFoundWindow && !pFoundWindow->onSpecialWorkspace()) {
                        pFoundWindow = PWORKSPACE->getFullscreenWindow();
                    }
                } else {
                    // if we have a maximized window, allow focusing on a bar or something if in reserved area.
                    if (g_pCompositor->isPointOnReservedArea(mouseCoords, PMONITOR)) {
                        foundSurface = g_pCompositor->vectorToLayerSurface(mouseCoords, &PMONITOR->m_layerSurfaceLayers[ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM], &surfaceCoords,
                                                                           &pFoundLayerSurface);
                    }

                    if (!foundSurface) {
                        if (pFoundWindow != PWINDOWIDEAL)
                            pFoundWindow = g_pCompositor->vectorToWindowUnified(mouseCoords, RESERVED_EXTENTS | INPUT_EXTENTS | ALLOW_FLOATING);

                        if (!(pFoundWindow && (pFoundWindow->m_isFloating && (pFoundWindow->m_createdOverFullscreen || pFoundWindow->m_pinned))))
                            pFoundWindow = PWORKSPACE->getFullscreenWindow();
                    }
                }
            }

        } else {
            if (pFoundWindow != PWINDOWIDEAL)
                pFoundWindow = g_pCompositor->vectorToWindowUnified(mouseCoords, RESERVED_EXTENTS | INPUT_EXTENTS | ALLOW_FLOATING);
        }

        if (pFoundWindow) {
            if (!pFoundWindow->m_isX11) {
                foundSurface = g_pCompositor->vectorWindowToSurface(mouseCoords, pFoundWindow, surfaceCoords);
                if (!foundSurface) {
                    foundSurface = pFoundWindow->m_wlSurface->resource();
                    surfacePos   = pFoundWindow->m_realPosition->value();
                }
            } else {
                foundSurface = pFoundWindow->m_wlSurface->resource();
                surfacePos   = pFoundWindow->m_realPosition->value();
            }
        }
    }

    // then surfaces below
    if (!foundSurface)
        foundSurface = g_pCompositor->vectorToLayerSurface(mouseCoords, &PMONITOR->m_layerSurfaceLayers[ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM], &surfaceCoords, &pFoundLayerSurface);

    if (!foundSurface)
        foundSurface = g_pCompositor->vectorToLayerSurface(mouseCoords, &PMONITOR->m_layerSurfaceLayers[ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND], &surfaceCoords, &pFoundLayerSurface);

    if (g_pPointerManager->softwareLockedFor(PMONITOR->m_self.lock()) > 0 && !skipFrameSchedule)
        g_pCompositor->scheduleFrameForMonitor(g_pCompositor->m_lastMonitor.lock(), Aquamarine::IOutput::AQ_SCHEDULE_CURSOR_MOVE);

    // FIXME: This will be disabled during DnD operations because we do not exactly follow the spec
    // xdg-popup grabs should be keyboard-only, while they are absolute in our case...
    if (g_pSeatManager->m_seatGrab && !g_pSeatManager->m_seatGrab->accepts(foundSurface) && !PROTO::data->dndActive()) {
        if (g_pInputManager->m_hardInput || refocus) {
            g_pSeatManager->setGrab(nullptr);
            return; // setGrab will refocus
        } else {
            // we need to grab the last surface.
            foundSurface = g_pSeatManager->m_state.pointerFocus.lock();

            auto HLSurface = CWLSurface::fromResource(foundSurface);

            if (HLSurface) {
                const auto BOX = HLSurface->getSurfaceBoxGlobal();

                if (BOX.has_value())
                    surfacePos = BOX->pos();
            }
        }
    }

    if (!foundSurface) {
        if (!g_pInputManager->m_emptyFocusCursorSet) {
            if (*PRESIZEONBORDER && *PRESIZECURSORICON && g_pInputManager->m_borderIconDirection != BORDERICON_NONE) {
                g_pInputManager->m_borderIconDirection = BORDERICON_NONE;
                unsetCursorImage();
            }

            // TODO: maybe wrap?
            //if (g_pInputManager->m_clickBehavior == CLICKMODE_KILL)
                //g_pInputManager->setCursorImageOverride("crosshair");
            //else
                //g_pInputManager->setCursorImageOverride("left_ptr");

            g_pInputManager->m_emptyFocusCursorSet = true;
        }

        g_pSeatManager->setPointerFocus(nullptr, {});

        if (refocus || g_pCompositor->m_lastWindow.expired()) // if we are forcing a refocus, and we don't find a surface, clear the kb focus too!
            g_pCompositor->focusWindow(nullptr);

        return;
    }

    g_pInputManager->m_emptyFocusCursorSet = false;

    Vector2D surfaceLocal = surfacePos == Vector2D(-1337, -1337) ? surfaceCoords : mouseCoords - surfacePos;

    if (pFoundWindow && !pFoundWindow->m_isX11 && surfacePos != Vector2D(-1337, -1337)) {
        // calc for oversized windows... fucking bullshit.
        CBox geom = pFoundWindow->m_xdgSurface->m_current.geometry;

        surfaceLocal = mouseCoords - surfacePos + geom.pos();
    }

    if (pFoundWindow && pFoundWindow->m_isX11) // for x11 force scale zero
        surfaceLocal = surfaceLocal * pFoundWindow->m_X11SurfaceScaledBy;

    bool allowKeyboardRefocus = true;

    if (!refocus && g_pCompositor->m_lastFocus) {
        const auto PLS = g_pCompositor->getLayerSurfaceFromSurface(g_pCompositor->m_lastFocus.lock());

        if (PLS && PLS->m_layerSurface->m_current.interactivity == ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE)
            allowKeyboardRefocus = false;
    }

    // set the values for use
    if (refocus) {
        g_pInputManager->m_foundLSToFocus      = pFoundLayerSurface;
        g_pInputManager->m_foundWindowToFocus  = pFoundWindow;
        g_pInputManager->m_foundSurfaceToFocus = foundSurface;
    }

    if (g_pInputManager->m_currentlyDraggedWindow.lock() && pFoundWindow != g_pInputManager->m_currentlyDraggedWindow) {
        g_pSeatManager->setPointerFocus(foundSurface, surfaceLocal);
        return;
    }

    if (pFoundWindow && foundSurface == pFoundWindow->m_wlSurface->resource() && !g_pInputManager->m_cursorImageOverridden) {
        const auto BOX = pFoundWindow->getWindowMainSurfaceBox();
        //if (VECNOTINRECT(mouseCoords, BOX.x, BOX.y, BOX.x + BOX.width, BOX.y + BOX.height))
            //g_pInputManager->setCursorImageOverride("left_ptr");
        //else
            //g_pInputManager->restoreCursorIconToApp();
    }

    if (pFoundWindow) {
        // change cursor icon if hovering over border
        if (*PRESIZEONBORDER && *PRESIZECURSORICON) {
            if (!pFoundWindow->isFullscreen() && !pFoundWindow->hasPopupAt(mouseCoords)) {
                g_pInputManager->setCursorIconOnBorder(pFoundWindow);
            } else if (g_pInputManager->m_borderIconDirection != BORDERICON_NONE) {
                unsetCursorImage();
            }
        }

        if (FOLLOWMOUSE != 1 && !refocus) {
            if (pFoundWindow != g_pCompositor->m_lastWindow.lock() && g_pCompositor->m_lastWindow.lock() &&
                ((pFoundWindow->m_isFloating && *PFLOATBEHAVIOR == 2) || (g_pCompositor->m_lastWindow->m_isFloating != pFoundWindow->m_isFloating && *PFLOATBEHAVIOR != 0))) {
                // enter if change floating style
                if (FOLLOWMOUSE != 3 && allowKeyboardRefocus)
                    g_pCompositor->focusWindow(pFoundWindow, foundSurface);
                g_pSeatManager->setPointerFocus(foundSurface, surfaceLocal);
            } else if (FOLLOWMOUSE == 2 || FOLLOWMOUSE == 3)
                g_pSeatManager->setPointerFocus(foundSurface, surfaceLocal);

            if (pFoundWindow == g_pCompositor->m_lastWindow)
                g_pSeatManager->setPointerFocus(foundSurface, surfaceLocal);

            if (FOLLOWMOUSE != 0 || pFoundWindow == g_pCompositor->m_lastWindow)
                g_pSeatManager->setPointerFocus(foundSurface, surfaceLocal);

            if (g_pSeatManager->m_state.pointerFocus == foundSurface)
                g_pSeatManager->sendPointerMotion(time, surfaceLocal);

            g_pInputManager->m_lastFocusOnLS = false;
            return; // don't enter any new surfaces
        } else {
            if (time == 0 && refocus) {
                g_pInputManager->m_lastMouseFocus = pFoundWindow;

                // TODO: this looks wrong. When over a popup, it constantly is switching.
                // Temp fix until that's figured out. Otherwise spams windowrule lookups and other shit.
                if (g_pInputManager->m_lastMouseFocus.lock() != pFoundWindow || g_pCompositor->m_lastWindow.lock() != pFoundWindow) {
                    if (g_pInputManager->m_mousePosDelta > *PFOLLOWMOUSETHRESHOLD || refocus) {
                        const bool hasNoFollowMouse = pFoundWindow && pFoundWindow->m_windowData.noFollowMouse.valueOrDefault();

                        if (refocus || !hasNoFollowMouse)
                            g_pCompositor->focusWindow(pFoundWindow, foundSurface);
                    }
                } else
                    g_pCompositor->focusSurface(foundSurface, pFoundWindow);
            }
            if (allowKeyboardRefocus && ((FOLLOWMOUSE != 3 && (*PMOUSEREFOCUS || m_lastMouseFocus.lock() != pFoundWindow)) || refocus)) {
                if (m_lastMouseFocus.lock() != pFoundWindow || g_pCompositor->m_lastWindow.lock() != pFoundWindow || g_pCompositor->m_lastFocus != foundSurface || refocus) {
                    m_lastMouseFocus = pFoundWindow;

                    // TODO: this looks wrong. When over a popup, it constantly is switching.
                    // Temp fix until that's figured out. Otherwise spams windowrule lookups and other shit.
                    if (m_lastMouseFocus.lock() != pFoundWindow || g_pCompositor->m_lastWindow.lock() != pFoundWindow) {
                        if (m_mousePosDelta > *PFOLLOWMOUSETHRESHOLD || refocus) {
                            const bool hasNoFollowMouse = pFoundWindow && pFoundWindow->m_windowData.noFollowMouse.valueOrDefault();

                            if (refocus || !hasNoFollowMouse)
                                g_pCompositor->focusWindow(pFoundWindow, foundSurface);
                        }
                    } else
                        g_pCompositor->focusSurface(foundSurface, pFoundWindow);
                }
            }
        }

        if (g_pSeatManager->m_state.keyboardFocus == nullptr)
            g_pCompositor->focusWindow(pFoundWindow, foundSurface);

        g_pInputManager->m_lastFocusOnLS = false;
    } else {
        if (*PRESIZEONBORDER && *PRESIZECURSORICON && g_pInputManager->m_borderIconDirection != BORDERICON_NONE) {
            g_pInputManager->m_borderIconDirection = BORDERICON_NONE;
            unsetCursorImage();
        }

        if (pFoundLayerSurface && (pFoundLayerSurface->m_layerSurface->m_current.interactivity != ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE) && FOLLOWMOUSE != 3 &&
            (allowKeyboardRefocus || pFoundLayerSurface->m_layerSurface->m_current.interactivity == ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE)) {
            g_pCompositor->focusSurface(foundSurface);
        }

        if (pFoundLayerSurface)
            g_pInputManager->m_lastFocusOnLS = true;
    }

    g_pSeatManager->setPointerFocus(foundSurface, surfaceLocal);
    g_pSeatManager->sendPointerMotion(time, surfaceLocal);
}
*/

void renderWorkspaceWindows(PHLMONITOR pMonitor, PHLWORKSPACE pWorkspace, const Time::steady_tp& time) {
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif
    PHLWINDOW lastWindow;

    EMIT_HOOK_EVENT("render", RENDER_PRE_WINDOWS);

    std::vector<PHLWINDOWREF> windows, fadingOut, pinned;
    windows.reserve(g_pCompositor->m_windows.size());

    // collect renderable windows
    for (auto const& w : g_pCompositor->m_windows) {
        if (w->isHidden() || (!w->m_isMapped && !w->m_fadingOut))
            continue;
        if (!g_pHyprRenderer->shouldRenderWindow(w, pMonitor))
            continue;

        windows.emplace_back(w);
    }

    // categorize + interleave
    for (auto& wref : windows) {
        auto w = wref.lock();
        if (!w)
            continue;

        // pinned go to separate pass (still above everything)
        if (w->m_pinned) {
            pinned.emplace_back(w);
            continue;
        }

        // some things may force us to ignore the special/not special disparity
        const bool IGNORE_SPECIAL_CHECK = w->m_monitorMovedFrom != -1 &&
                                          (w->m_workspace && !w->m_workspace->isVisible());

        if (!IGNORE_SPECIAL_CHECK && pWorkspace->m_isSpecialWorkspace != w->onSpecialWorkspace())
            continue;

        if (pWorkspace->m_isSpecialWorkspace && w->m_monitor != pWorkspace->m_monitor)
            continue; // special on another monitor drawn elsewhere

        // last window drawn after others
        if (w == Desktop::focusState()->window()) {
            lastWindow = w;
            continue;
        }

        if (w->m_fadingOut) {
            fadingOut.emplace_back(w);
            continue;
        }

        // main pass (interleaved tiled/floating)
        g_pHyprRenderer->renderWindow(w, pMonitor, time, true, RENDER_PASS_MAIN);

        // popup directly after main
        g_pHyprRenderer->renderWindow(w, pMonitor, time, true, RENDER_PASS_POPUP);
    }

    // render last focused window after the rest
    if (lastWindow) {
        g_pHyprRenderer->renderWindow(lastWindow, pMonitor, time, true, RENDER_PASS_MAIN);
        g_pHyprRenderer->renderWindow(lastWindow, pMonitor, time, true, RENDER_PASS_POPUP);
    }

    // fading out (tiled or floating) — after main windows
    for (auto& wref : fadingOut) {
        auto w = wref.lock();
        if (w)
            g_pHyprRenderer->renderWindow(w, pMonitor, time, true, RENDER_PASS_MAIN);
    }

    // pinned last, above everything
    for (auto& wref : pinned) {
        auto w = wref.lock();
        if (w)
            g_pHyprRenderer->renderWindow(w, pMonitor, time, true, RENDER_PASS_ALL);
    }
}

// for interleaving tiled and floating windows
//void CInputManager::mouseMoveUnified(uint32_t time, bool refocus, bool mouse, std::optional<Vector2D> overridePos) {
//PHLWINDOW CCompositor::vectorToWindowUnified(const Vector2D& pos, uint8_t properties, PHLWINDOW pIgnoreWindow) {
//void CHyprRenderer::renderWorkspaceWindows(PHLMONITOR pMonitor, PHLWORKSPACE pWorkspace, const Time::steady_tp& time) {

inline CFunctionHook* g_pOnRenderWorkspaceWindows = nullptr;
typedef void (*origRenderWorkspaceWindows)(CHyprRenderer *, PHLMONITOR pMonitor, PHLWORKSPACE pWorkspace, const Time::steady_tp& time);
void hook_onRenderWorkspaceWindows(void* thisptr,  PHLMONITOR pMonitor, PHLWORKSPACE pWorkspace, const Time::steady_tp& time) {
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif

    //auto chr = (CHyprRenderer *) thisptr;
    //(*(origRenderWorkspaceWindows)g_pOnRenderWorkspaceWindows->m_original)(chr, pMonitor, pWorkspace, time);
    renderWorkspaceWindows(pMonitor, pWorkspace, time);
}

inline CFunctionHook* g_pOnVectorToWindowUnified = nullptr;
typedef PHLWINDOW (*origVectorToWindowUnified)(CCompositor *, const Vector2D& pos, uint8_t properties, PHLWINDOW pIgnoreWindow);
PHLWINDOW hook_onVectorToWindowUnified(void* thisptr, const Vector2D& pos, uint8_t properties, PHLWINDOW pIgnoreWindow) {
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif
    return (*(origVectorToWindowUnified)g_pOnVectorToWindowUnified->m_original)((CCompositor *) thisptr, pos, properties, pIgnoreWindow);
    //return vectorToWindowUnified(pos, properties, pIgnoreWindow);
}

inline CFunctionHook* g_pOnMouseMoveUnified = nullptr;
typedef void (*origMouseMoveUnifiedd)(CInputManager *, uint32_t time, bool refocus, bool mouse, std::optional<Vector2D> overridePos);
void hook_onMouseMoveUnified(void* thisptr, uint32_t time, bool refocus, bool mouse, std::optional<Vector2D> overridePos) {
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif

    (*(origMouseMoveUnifiedd)g_pOnMouseMoveUnified->m_original)((CInputManager *) thisptr, time, refocus, mouse, overridePos);
    //mouseMoveUnified(time, refocus, mouse, overridePos);
}

void interleave_floating_and_tiled_windows() {
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif
    {
        static const auto METHODS = HyprlandAPI::findFunctionsByName(globals->api, "renderWorkspaceWindows");
        for (auto m : METHODS) {
            if (m.demangled.find("CHyprRenderer::renderWorkspaceWindows(") != std::string::npos) {
                pRenderWorkspaceWindows = m.address;
                //g_pOnRenderWorkspaceWindows = HyprlandAPI::createFunctionHook(globals->api, m.address, (void*)&hook_onRenderWorkspaceWindows);
                //g_pOnRenderWorkspaceWindows->hook();
            }
        }
    }
    return;
    {
        static auto METHODS = HyprlandAPI::findFunctionsByName(globals->api, "renderWorkspaceWindowsFullscreen");
        pRenderWorkspaceWindowsFullscreen = METHODS[0].address;
    }    
    {
        static const auto METHODS = HyprlandAPI::findFunctionsByName(globals->api, "vectorToWindowUnified");
        for (auto m : METHODS) {
            if (m.signature.find("CCompositor") != std::string::npos) {
                g_pOnVectorToWindowUnified = HyprlandAPI::createFunctionHook(globals->api, m.address, (void*)&hook_onVectorToWindowUnified);
                g_pOnVectorToWindowUnified->hook();
            }
        }
    }
    {
        static const auto METHODS = HyprlandAPI::findFunctionsByName(globals->api, "mouseMoveUnified");
        for (auto m : METHODS) {
            if (m.signature.find("CInputManager") != std::string::npos) {
                g_pOnMouseMoveUnified = HyprlandAPI::createFunctionHook(globals->api, m.address, (void*)&hook_onMouseMoveUnified);
                g_pOnMouseMoveUnified->hook();
            }
        }
    }

    
}

static PHLWINDOWREF prev;

void HyprIso::all_lose_focus() {
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif
    prev = Desktop::focusState()->window();
    g_pSeatManager->setPointerFocus(nullptr, {});
    Desktop::focusState()->fullWindowFocus(nullptr);
}

void HyprIso::all_gain_focus() {
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif
    if (auto p = prev.lock()) {
        //g_pSeatManager->setPointerFocus(p, {});
        Desktop::focusState()->fullWindowFocus(p);        
    }
}

void HyprIso::set_corner_rendering_mask_for_window(int id, int mask) {
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif
    for (auto hw: hyprwindows)
        if (hw->id == id)
            hw->cornermask = mask;
}

Bounds bounds_monitor(int id);
Bounds bounds_reserved_monitor(int id);

Bounds bounds_full_client(int wid) {
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif
    for (auto hyprwindow : hyprwindows) {
        if (hyprwindow->id == wid) {
            if (auto w = hyprwindow->w.get()) {
                return tobounds(w->getFullWindowBoundingBox());
            }
        }
    }    
    return {0, 0, 0, 0};
}

Bounds bounds_client(int wid) {
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif
    for (auto hyprwindow : hyprwindows) {
        if (hyprwindow->id == wid) {
            if (auto w = hyprwindow->w.get()) {
                //return w->getFullWindowBoundingBox();
                return tobounds(w->getWindowMainSurfaceBox());
            }
        }
    }    
    return {0, 0, 0, 0};
}

Bounds bounds_client_final(int wid) {
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif
    for (auto hyprwindow : hyprwindows) {
        if (hyprwindow->id == wid) {
            if (auto w = hyprwindow->w.get()) {
                return {w->m_realPosition->goal().x, w->m_realPosition->goal().y, w->m_realSize->goal().x, w->m_realSize->goal().y};
            }
        }
    }    
    return {0, 0, 0, 0};
}

Bounds bounds_layer(int wid) {
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif
    for (auto hl : hyprlayers) {
        if (hl->id == wid) {
            if (auto w = hl->l.get()) {
                return tobounds(CBox(w->m_realPosition->goal().x, w->m_realPosition->goal().y, w->m_realSize->goal().x, w->m_realSize->goal().y));
            }
        }
    }    
    return {0, 0, 0, 0};
}

Bounds real_bounds_client(int wid) {
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif
    for (auto hyprwindow : hyprwindows) {
        if (hyprwindow->id == wid) {
            if (auto w = hyprwindow->w.get()) {
                //return w->getFullWindowBoundingBox();
                return tobounds({
                    w->m_realPosition->goal().x, w->m_realPosition->goal().y,
                    w->m_realSize->goal().x, w->m_realSize->goal().y
                });
            }
        }
    }    
    return {0, 0, 0, 0};
}

Bounds bounds_monitor(int id) {
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif
    for (auto hyprmonitor : hyprmonitors) {
        if (hyprmonitor->id == id) {
            if (auto m = hyprmonitor->m.get()) {
                return tobounds(m->logicalBox());
            }
        }
    }    
    return {0, 0, 0, 0};
}

Bounds bounds_reserved_monitor(int id) {
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif
    for (auto hyprmonitor : hyprmonitors) {
        if (hyprmonitor->id == id) {
            if (auto m = hyprmonitor->m.get()) {
                auto b = m->logicalBox();
                b.x += m->m_reservedArea.left();
                b.y += m->m_reservedArea.top();
                b.w -= (m->m_reservedArea.left() + m->m_reservedArea.right());
                b.h -= (m->m_reservedArea.top() + m->m_reservedArea.bottom());
                return tobounds(b);
            }
        }
    }    
    return {0, 0, 0, 0};
}

bool HyprIso::being_animated(int cid) {
    for (auto hw : hyprwindows) {
        if (hw->id == cid) {
            return hw->w->m_realPosition->isBeingAnimated() || hw->w->m_realSize->isBeingAnimated() ;
        }
    }
    return false;
}

bool HyprIso::is_pinned(int id) {
   for (auto hw : hyprwindows) {
        if (hw->id == id) {
            return hw->w->m_pinned;
        }
   }
   return false;
}

void updateWindow(CHyprDropShadowDecoration *ds, PHLWINDOW pWindow) {
    const auto PWINDOW = ds->m_window.lock();

    ds->m_lastWindowPos  = PWINDOW->m_realPosition->value();
    ds->m_lastWindowSize = PWINDOW->m_realSize->value();

    ds->m_lastWindowBox          = {ds->m_lastWindowPos.x, ds->m_lastWindowPos.y, ds->m_lastWindowSize.x, ds->m_lastWindowSize.y};
    ds->m_lastWindowBoxWithDecos = g_pDecorationPositioner->getBoxWithIncludedDecos(pWindow);
}

void drawShadowInternal(const CBox& box, int round, float roundingPower, int range, CHyprColor color, float a, bool sharp) {
    static auto PSHADOWSHARP = sharp;

    if (box.w < 1 || box.h < 1)
        return;

    g_pHyprOpenGL->blend(true);

    color.a *= a;

    if (PSHADOWSHARP)
        g_pHyprOpenGL->renderRect(box, color, {.round = round, .roundingPower = roundingPower});
    else
        g_pHyprOpenGL->renderRoundedShadow(box, round, roundingPower, range, color, 1.F);
}

void testDraw() {
    AnyPass::AnyData anydata([](AnyPass* pass) {
        static bool once = true;
        static CFramebuffer *otherFB = nullptr;
        PHLMONITOR m;
        auto mid = hypriso->monitor_from_cursor();
        for (auto s : hyprmonitors) {
            if (s->id == mid) {
                m = s->m;
            }
        }
        if (!m)
            return;
        if (!otherFB) {
            otherFB = new CFramebuffer;
            otherFB->alloc(m->m_pixelSize.x, m->m_pixelSize.y, DRM_FORMAT_ABGR8888);
        }
        if (once) {
            once = false;
            auto* LASTFB = g_pHyprOpenGL->m_renderData.currentFB;
            otherFB->bind();

            for (auto h : hyprwindows) {
                if (hypriso->alt_tabbable(h->id)) {
                    g_pHyprRenderer->renderWindow(h->w, h->w->m_monitor.lock(), Time::steadyNow(), false, eRenderPassMode::RENDER_PASS_ALL);
                    break;
                }
            }

            //g_pHyprRenderer->renderWorkspace(m, m->m_activeWorkspace, Time::steadyNow(), m->logicalBox());

            //g_pHyprOpenGL->renderRect({0, 0, 200, 200}, CHyprColor(0, 1, 0, 1), {.round = 0});
            //g_pHyprOpenGL->renderRect({0, 0, 100, 50}, CHyprColor(1, 1, 0, 1), {.round = 0});
            
            LASTFB->bind();
        }

        CHyprOpenGLImpl::STextureRenderData data;

        CRegion saveDamage = g_pHyprOpenGL->m_renderData.damage;
        g_pHyprOpenGL->m_renderData.damage = {0, 0, m->m_pixelSize.x, m->m_pixelSize.y};
        CRegion texDamage{g_pHyprOpenGL->m_renderData.damage};
        data.damage = &texDamage;
        g_pHyprOpenGL->renderTextureInternal(otherFB->getTexture(), {0, 0, m->m_pixelSize.x, m->m_pixelSize.y}, data);
        g_pHyprOpenGL->m_renderData.damage = saveDamage;
        //g_pHyprOpenGL->renderTextureMatte(alphaSwapFB.getTexture(), monbox, alphaFB);
    });
    //g_pHyprOpenGL->m_renderData.damage;
    g_pHyprRenderer->m_renderPass.add(makeUnique<AnyPass>(std::move(anydata)));
}

void drawDropShadow(PHLMONITOR pMonitor, float const& a, CHyprColor b, float ROUNDINGBASE, float ROUNDINGPOWER, CBox fullBox, int range, bool sharp) {
    AnyPass::AnyData anydata([pMonitor, a, b, ROUNDINGBASE, ROUNDINGPOWER, fullBox, range, sharp](AnyPass* pass) {
        CHyprColor m_realShadowColor = CHyprColor(b.r, b.g, b.b, b.a);
        if (g_pCompositor->m_windows.empty())
            return;
        PHLWINDOW fake_window = g_pCompositor->m_windows[0]; // there is a faulty assert that exists that would otherwise be hit without a fake window target
        static auto PSHADOWSIZE = range;
        const auto ROUNDING = ROUNDINGBASE;
        auto allBox = fullBox;
        allBox.expand(PSHADOWSIZE);
        allBox.round();
        
        if (fullBox.width < 1 || fullBox.height < 1)
            return; // don't draw invisible shadows

        g_pHyprOpenGL->scissor(nullptr);
        auto before_window = g_pHyprOpenGL->m_renderData.currentWindow;
        g_pHyprOpenGL->m_renderData.currentWindow = fake_window;

        // we'll take the liberty of using this as it should not be used rn
        CFramebuffer& alphaFB = g_pHyprOpenGL->m_renderData.pCurrentMonData->mirrorFB;
        CFramebuffer& alphaSwapFB = g_pHyprOpenGL->m_renderData.pCurrentMonData->mirrorSwapFB;
        auto* LASTFB = g_pHyprOpenGL->m_renderData.currentFB;

        CRegion saveDamage = g_pHyprOpenGL->m_renderData.damage;

        g_pHyprOpenGL->m_renderData.damage = allBox;
        g_pHyprOpenGL->m_renderData.damage.subtract(fullBox.copy().expand(-ROUNDING)).intersect(saveDamage);
        g_pHyprOpenGL->m_renderData.renderModif.applyToRegion(g_pHyprOpenGL->m_renderData.damage);

        alphaFB.bind();

        // build the matte
        // 10-bit formats have dogshit alpha channels, so we have to use the matte to its fullest.
        // first, clear region of interest with black (fully transparent)
        g_pHyprOpenGL->renderRect(allBox, CHyprColor(0, 0, 0, 1), {.round = 0});

        // render white shadow with the alpha of the shadow color (otherwise we clear with alpha later and shit it to 2 bit)
        drawShadowInternal(allBox, ROUNDING, ROUNDINGPOWER, PSHADOWSIZE, CHyprColor(1, 1, 1, m_realShadowColor.a), a, sharp);

        // render black window box ("clip")
        g_pHyprOpenGL->renderRect(fullBox, CHyprColor(0, 0, 0, 1.0), {.round = (int) (ROUNDING), .roundingPower = ROUNDINGPOWER});

        alphaSwapFB.bind();

        // alpha swap just has the shadow color. It will be the "texture" to render.
        g_pHyprOpenGL->renderRect(allBox, m_realShadowColor.stripA(), {.round = 0});

        LASTFB->bind();

        CBox monbox = {0, 0, pMonitor->m_transformedSize.x, pMonitor->m_transformedSize.y};

        g_pHyprOpenGL->pushMonitorTransformEnabled(true);
        g_pHyprOpenGL->setRenderModifEnabled(false);
        g_pHyprOpenGL->renderTextureMatte(alphaSwapFB.getTexture(), monbox, alphaFB);
        g_pHyprOpenGL->setRenderModifEnabled(true);
        g_pHyprOpenGL->popMonitorTransformEnabled();

        g_pHyprOpenGL->m_renderData.damage = saveDamage;

        g_pHyprOpenGL->m_renderData.currentWindow = before_window;
    });
    g_pHyprRenderer->m_renderPass.add(makeUnique<AnyPass>(std::move(anydata)));
}

void render_drop_shadow(int mon, float const& a, RGBA b, float ROUNDINGBASE, float ROUNDINGPOWER, Bounds fullB, float size) {
    PHLMONITOR pMonitor;
    for (auto hm : hyprmonitors) {
        if (hm->id == mon) {
            pMonitor = hm->m;
        }
    }
    if (!pMonitor)
        return;
    CHyprColor colorb = CHyprColor(b.r, b.g, b.b, b.a);
    static auto PSHADOWSIZE = CConfigValue<Hyprlang::INT>("decoration:shadow:range");
    static auto PSHADOWSCALE = CConfigValue<Hyprlang::FLOAT>("decoration:shadow:scale");

    if (size != 0) {
        drawDropShadow(pMonitor, a, colorb, ROUNDINGBASE, ROUNDINGPOWER, tocbox(fullB), size, false);
    } else {
        drawDropShadow(pMonitor, a, colorb, ROUNDINGBASE, ROUNDINGPOWER, tocbox(fullB), *PSHADOWSIZE, false);
    }
}


void HyprIso::logout() {
    if (g_pKeybindManager->m_dispatchers.contains("exit")) {
       g_pKeybindManager->m_dispatchers["exit"]("");
    } else {
        notify("dispatch `exit` no longer exists, report this issue if encountered");
    }
}

void HyprIso::send_false_position(int x, int y) {
    g_pSeatManager->sendPointerMotion(Time::millis(Time::steadyNow()), {x, y});
}

void HyprIso::send_false_click() {
    g_pSeatManager->sendPointerButton(Time::millis(Time::steadyNow()), BTN_LEFT, WL_POINTER_BUTTON_STATE_PRESSED);
    g_pSeatManager->sendPointerButton(Time::millis(Time::steadyNow()), BTN_LEFT, WL_POINTER_BUTTON_STATE_RELEASED);
}

uint32_t HyprIso::keycode_to_keysym(int keycode) {
    //const xkb_keysym_t keysym = xkb_state_key_get_one_sym(nullptr, keycode);
    //const xkb_keysym_t internalKeysym = xkb_state_key_get_one_sym(pKeyboard->m_xkbState, KEYCODE);

   return 0;
}

void HyprIso::simulateMouseMovement() {
    g_pInputManager->simulateMouseMovement();
}

bool HyprIso::has_popup_at(int cid, Bounds b) {
    for (auto h : hyprwindows) {
        if (h->id == cid) {
            return h->w->hasPopupAt({b.x, b.y});
        }
    }
     
    return false; 
}

struct HyprPopup {
    int id;  
    Desktop::View::CPopup *p;
};

std::vector<HyprPopup *> hyprpopus;

// src/desktop/Popup.cpp
// UP<CPopup> CPopup::create(PHLWINDOW pOwner) {
// UP<CPopup> CPopup::create(PHLLS pOwner) {
// child of other popup
// UP<CPopup> CPopup::create(SP<CXDGPopupResource> resource, WP<CPopup> pOwner) {
// void CPopup::fullyDestroy() {

void popup_created(Desktop::View::CPopup *popup) {
    auto hp = new HyprPopup;
    hp->p = popup;
    hp->id = unique_id++;
    hyprpopus.push_back(hp);

    if (hypriso->on_popup_open) {
        hypriso->on_popup_open(hp->id);
    }
}

void popup_destroyed(int id, Desktop::View::CPopup *popup) {
    for (int i = 0; i < hyprpopus.size(); i++) {
        auto hp = hyprpopus[i];
        if (hp->id == id) {
            if (hypriso->on_popup_closed) {
                hypriso->on_popup_closed(id);
            }
            delete hp;
            hyprpopus.erase(hyprpopus.begin() + i);
        }
    }
}

void hook_popup_creation_and_destruction() {
    
}

void HyprIso::do_default_drag(int cid) {
    next_check = true;
    g_pKeybindManager->changeMouseBindMode(MBIND_MOVE);
}

void HyprIso::do_default_resize(int cid) {
    next_check = true;
    g_pKeybindManager->changeMouseBindMode(MBIND_RESIZE);
}

bool HyprIso::is_floating(int cid) {
    for (auto hw: hyprwindows)
        if (hw->id == cid)
            return hw->w->m_isFloating;
    return false;
}


// hook notify mfact
// SDispatchResult CKeybindManager::layoutmsg(std::string msg) {
// it's valid to only care about layoutmsg mfact and not config change because we are basically treating it as a snap window resizer


void HyprIso::add_hyprctl_dispatcher(std::string command, std::function<bool(std::string)> func) {
    HyprlandAPI::addDispatcherV2(globals->api, command, [func](std::string in) {
        SDispatchResult result;
        result.success = func(in);
        return result;
    });
}


std::string HyprIso::monitor_name(int id) {
    for (auto hm : hyprmonitors) {
        if (hm->id == id) {
            return hm->m->m_name;
        }
    }
    return "";
}

float HyprIso::zoom_progress(int monitor) {
    for (auto hm : hyprmonitors) {
        if (hm->id == monitor) {
            if (hm->m) {
                return hm->m->m_zoomAnimProgress->value();
            }
        }
    }
    return 1.0f;
}

int HyprIso::get_pid(int client) {
    for (auto h : hyprwindows) {
        if (h->id == client) {
            return h->w->getPID();
        }
    }
     
    return -1;
}

void HyprIso::login_animation() {
    for (auto mon : hyprmonitors) {
       if (mon->m) {
           mon->creation_time = get_current_time_in_ms();
           mon->first = true;
           get_previous_instance_signature();
           previously_seen_instance_signature = "";
       }
    }
}

void HyprIso::generate_mylar_hyprland_config() {
    const char* home = std::getenv("HOME");
    if (!home) {
        notify("$HOME environment variable not set");
        return;
    }

    {
        std::filesystem::path filepath = std::filesystem::path(home) / ".config/mylar/default.conf";
        std::filesystem::create_directories(filepath.parent_path());
        
        std::ofstream out(filepath, std::ios::trunc);
        out << default_conf << std::endl;
    }
    
    {
        std::filesystem::path f = std::filesystem::path(home) / ".config/mylar/user.conf";
        std::filesystem::create_directories(f.parent_path());
        if (!std::filesystem::exists(f)) {
            std::ofstream out(f, std::ios::trunc);
            out << std::endl << std::endl;
        }
    }
    

    //g_pConfigManager->handleSource("source", "~/.config/mylar/default.conf");
}

void animate(float *value, float target, float time_ms, std::shared_ptr<bool> lifetime, std::function<void(bool)> on_completion, std::function<float(float)> lerp_func) {
    
    for (auto anim : anims) {
        if (anim->value == value) {
            anim->start_value = *value;
            anim->target = target;
            anim->start_time = get_current_time_in_ms();
            anim->time_ms = time_ms;
            anim->lifetime = lifetime;
            anim->on_completion = on_completion;
            anim->lerp_func = lerp_func;
            return;
        }
    }
    
    auto anim = new Anim;
    anim->value = value;
    anim->start_value = *value;
    anim->target = target;
    anim->start_time = get_current_time_in_ms();
    anim->time_ms = time_ms;
    anim->lifetime = lifetime;
    anim->on_completion = on_completion;
    anim->lerp_func = lerp_func;
    
    anims.push_back(anim);
    
    // TODO: this creates a later per animation which is dumb, they should all be combined into one that calls over a vec of anims
    later(1000.0f / 165.0f, [anim](Timer *t) {
        t->keep_running = true;
        
        if (anim->lifetime.lock()) {
            long delta = get_current_time_in_ms() - anim->start_time;
            float delta_ms = (float) delta;
            float scalar = delta_ms / anim->time_ms;
            if (scalar > 1.0) {
                t->keep_running = false;
                scalar = 1.0;
            }
            if (anim->lerp_func)
                scalar = anim->lerp_func(scalar);
            
            auto diff = (anim->target - anim->start_value) * scalar;
            *anim->value = anim->start_value + diff;
            if (!t->keep_running ) {
                *anim->value = anim->target;
                if (anim->on_completion) {
                    anim->on_completion(true);
                }
                for (int i = anims.size() - 1; i >= 0; i--) {
                    if (anims[i] == anim) {
                        anims.erase(anims.begin() + i);
                    }
                }
                delete anim;
            }
        } else {
            for (int i = anims.size() - 1; i >= 0; i--) {
                if (anims[i] == anim) {
                    anims.erase(anims.begin() + i);
                }
            }
            delete anim;
            t->keep_running = false;
            if (anim->on_completion)
                anim->on_completion(false);
        }
    });
}

bool is_being_animating(float *value) {
    for (auto a : anims)
        if (value == a->value)
            return true;
    return false;
}

bool is_being_animating_to(float *value, float target) {
    for (auto a : anims)
        if (value == a->value && a->target == target)
            return true;
    return false;
}



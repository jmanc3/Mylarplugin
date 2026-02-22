
#include "events.h"

#include "container.h"
#include <algorithm>
#include <format>

#include "json.hpp"

#ifdef TRACY_ENABLE
#include "tracy/Tracy.hpp"
#endif

#define BTN_LEFT		0x110
#define BTN_RIGHT		0x111
#define BTN_MIDDLE		0x112

void log(std::string) {}
#define fz std::format

void fill_list_with_concerned(std::vector<Container*>& containers, Container* parent) {
    if (parent->type == ::newscroll) {
        auto s = (ScrollContainer*)parent;
        for (auto child : s->content->children)
            fill_list_with_concerned(containers, child);
        if (s->right)
            fill_list_with_concerned(containers, s->right);
        if (s->bottom)
            fill_list_with_concerned(containers, s->bottom);
    } else {
        for (auto child : parent->children)
            fill_list_with_concerned(containers, child);
    }

    if (parent->state.concerned) {
        containers.push_back(parent);
    }
}

std::vector<Container*> concerned_containers(Container* root) {
    std::vector<Container*> containers;

    fill_list_with_concerned(containers, root);

    return containers;
}

void fill_list_with_pierced(std::vector<Container*>& containers, Container* parent, int x, int y) {
    if (!parent->exists)
        return;
    if (parent->type == ::newscroll) {
        auto s = (ScrollContainer*)parent;
        for (auto child : s->content->children) {
            if (child->interactable) {
                // parent->real_bounds w and h need to be subtracted by right and bottom
                // if they exist
                auto real_bounds_copy = parent->real_bounds;
                if (s->right && s->right->exists)
                    real_bounds_copy.w -= s->right->real_bounds.w;
                if (s->bottom && s->bottom->exists)
                    real_bounds_copy.h -= s->bottom->real_bounds.h;
                if (!bounds_contains(real_bounds_copy, x, y))
                    continue;
                fill_list_with_pierced(containers, child, x, y);
            }
        }
        if (s->right && s->right->exists)
            fill_list_with_pierced(containers, s->right, x, y);
        if (s->bottom && s->bottom->exists)
            fill_list_with_pierced(containers, s->bottom, x, y);
    } else {
        for (auto child : parent->children) {
            if (child->interactable) {
                // check if parent is scrollpane and if so, check if the child is in
                // bounds before calling
                if (parent->type >= ::scrollpane && parent->type <= ::scrollpane_b_never)
                    if (!bounds_contains(parent->real_bounds, x, y))
                        continue;
                fill_list_with_pierced(containers, child, x, y);
            }
        }
    }

    if (parent->exists) {
        if (parent->handles_pierced) {
            if (parent->handles_pierced(parent, x, y))
                containers.push_back(parent);
        } else if (bounds_contains(parent->real_bounds, x, y)) {
            if (parent->parent_bounds_limit_input_bounds && parent->parent) {
                if (bounds_contains(parent->parent->real_bounds, x, y)) {
                    containers.push_back(parent);
                }
            } else {
                containers.push_back(parent);
            }
        }
    }
}

// Should return the list of containers directly underneath the x and y with
// deepest children first in the list
std::vector<Container*> pierced_containers(Container* root, int x, int y) {
    std::vector<Container*> containers;

    fill_list_with_pierced(containers, root, x, y);

    return containers;
}

bool is_pierced(Container* c, std::vector<Container*>& pierced) {
    for (auto container : pierced) {
        if (container == c) {
            return true;
        }
    }
    return false;
}

void handle_mouse_motion(Container* root, int x, int y) {
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif
    // Disabled because of scrolls
    root->previous_x                  = root->mouse_current_x;
    root->previous_y                  = root->mouse_current_y;
    root->mouse_current_x             = x;
    root->mouse_current_y             = y;
    std::vector<Container*> pierced   = pierced_containers(root, x, y);
    std::vector<Container*> concerned = concerned_containers(root);

    // pierced   are ALL the containers under the mouse
    // concerned are all the containers which have concerned state on
    // mouse_motion can be the catalyst for sending out
    // when_mouse_enters_container, when_mouse_motion,
    // when_mouse_leaves_container, when_drag_start, and when_drag

    // If container in concerned but not in pierced means
    // when_mouse_leaves_container needs to be called and mouse_hovering needs to
    // be set to false and if the container is not doing anything else like being
    // dragged or pressed then concerned can be set to false
    for (int i = 0; i < concerned.size(); i++) {
        auto c = concerned[i];

        // TODO we need to remove from concered if not on top as well not just pierced
        bool in_pierced = false;
        bool top = false;
        for (int j = 0; j < pierced.size(); j++) {
            auto p = pierced[j];
            if (p == c) {
                in_pierced = true;
                if (c->receive_events_even_if_obstructed) {
                    top = true;
                } else if (c->receive_events_even_if_obstructed_by_one && j == 1) {
                    top = true;
                } else if (j == 0) {
                    top = true;
                }
                break;
            }
        }
        if (!c->exists) {
            in_pierced = false;
            top = false;
        }

        if (c->state.mouse_pressing || c->state.mouse_dragging) {
            if (c->state.mouse_dragging) {
                auto move_distance_x = abs(root->mouse_initial_x - root->mouse_current_x);
                auto move_distance_y = abs(root->mouse_initial_y - root->mouse_current_y);
                if (move_distance_x >= c->minimum_x_distance_to_move_before_drag_begins || move_distance_y >= c->minimum_y_distance_to_move_before_drag_begins) {
                    // handle when_drag
                    if (c->when_drag) {
                        c->when_drag(root, c);
                    }
                }
            } else if (c->state.mouse_pressing) {
                auto move_distance_x = abs(root->mouse_initial_x - root->mouse_current_x);
                auto move_distance_y = abs(root->mouse_initial_y - root->mouse_current_y);
                if (move_distance_x >= c->minimum_x_distance_to_move_before_drag_begins || move_distance_y >= c->minimum_y_distance_to_move_before_drag_begins) {
                    c->state.mouse_dragging = true;
                    if (c->when_drag_start) {
                        c->when_drag_start(root, c);
                    }
                }
            }
        } else if (in_pierced && top) {
            // handle when_mouse_motion
            if (c->when_mouse_motion) {
                c->when_mouse_motion(root, c);
            }
        } else {
            // handle when_mouse_leaves_container
            c->state.mouse_hovering = false;
            if (c->when_mouse_leaves_container) {
                c->when_mouse_leaves_container(root, c);
            }
            c->state.reset();
        }
    }

    bool a_concerned_container_mouse_pressed = false;
    for (int i = 0; i < concerned.size(); i++) {
        auto c = concerned[i];

        if (c->state.mouse_pressing || c->state.mouse_dragging)
            a_concerned_container_mouse_pressed = true;
    }

    for (int i = 0; i < pierced.size(); i++) {
        auto p = pierced[i];
        bool non_top = i != 0;
        //if (c->when_mouse_leaves_container && !p) {
            //c->when_mouse_leaves_container(root, c);
        //}
 
        if (a_concerned_container_mouse_pressed && !p->state.concerned)
            continue;

        if (non_top && (!p->receive_events_even_if_obstructed_by_one && !p->receive_events_even_if_obstructed)) {
            continue;
        }
        if (non_top) {
            if (p->receive_events_even_if_obstructed) {

            } else if (p->receive_events_even_if_obstructed_by_one && p != pierced[0]->parent) {
                continue;
            }
        }

        if (p->state.concerned)
            continue;

        // handle when_mouse_enters_container
        p->state.concerned      = true;
        p->state.mouse_hovering = true;
        if (p->when_mouse_enters_container) {
            p->when_mouse_enters_container(root, p);
        }
    }
}

void set_active(Container* root, const std::vector<Container*>& active_containers, Container* c, bool state, bool viakey) {
    if (c->type == ::newscroll) {
        auto s = (ScrollContainer*)c;
        for (auto child : s->content->children) {
            set_active(root, active_containers, child, state, viakey);
        }

        bool will_be_activated = false;
        for (auto active_container : active_containers) {
            if (active_container == s->content) {
                will_be_activated = true;
            }
        }
        if (will_be_activated) {
            if (!s->content->active) {
                s->content->active = true;
                s->content->viakey = viakey;
                if (s->content->when_active_status_changed) {
                    s->content->when_active_status_changed(root, s->content);
                }
            }
        } else {
            if (s->content->active) {
                s->content->active = false;
                s->content->viakey = viakey;
                if (s->content->when_active_status_changed) {
                    s->content->when_active_status_changed(root, s->content);
                }
            }
        }
    } else {
        for (auto child : c->children) {
            set_active(root, active_containers, child, state, viakey);
        }

        bool will_be_activated = false;
        for (auto active_container : active_containers) {
            if (active_container == c) {
                will_be_activated = true;
            }
        }
        if (will_be_activated) {
            if (!c->active) {
                c->active = true;
                c->viakey = viakey;
                if (c->when_active_status_changed) {
                    c->when_active_status_changed(root, c);
                }
            }
        } else {
            if (c->active) {
                c->active = false;
                c->viakey = viakey;
                if (c->when_active_status_changed) {
                    c->when_active_status_changed(root, c);
                }
            }
        }
    }
}

void handle_mouse_button_press(Container* root, const Event& e) {
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif
    // TODOO wrong check
    if (e.button == BTN_LEFT)
        root->left_mouse_down = true;

    root->mouse_initial_x = e.x;
    root->mouse_initial_y = e.y;
    root->mouse_current_x = e.x;
    root->mouse_current_y = e.y;
    std::vector<Container*> pierced   = pierced_containers(root, e.x, e.y);
    std::vector<Container*> concerned = concerned_containers(root);

    // pierced   are ALL the containers under the mouse
    // concerned are all the containers which have concerned state on
    // handle_mouse_button can be the catalyst for sending out when_mouse_down and
    // when_scrolled

    std::vector<Container*> mouse_downed;

    //notify(std::format("{} {} {} {}", e.scroll, pierced.size(), root->mouse_current_x, root->mouse_current_y));

    for (int i = 0; i < pierced.size(); i++) {
        auto p = pierced[i];

        // pierced[0] will always be the top or "real" container we are actually
        // clicking on therefore everyone else in the list needs to set
        // receive_events_even_if_obstructed to true to receive events
        if (i != 0 && (!p->receive_events_even_if_obstructed_by_one && !p->receive_events_even_if_obstructed)) {
            continue;
        }
        if (i != 0) {
            if (p->receive_events_even_if_obstructed) {

            } else if (p->receive_events_even_if_obstructed_by_one && p != pierced[0]->parent) {
                continue;
            }
        }

        p->state.concerned = true; // Make sure this container is concerned

        // Check if its a scroll event and call when_scrolled if so
        if (e.scroll) {
            if (p->when_fine_scrolled) {
                p->when_fine_scrolled(root, p, 0, -e.delta, !e.from_mouse);
                handle_mouse_motion(root, e.x, e.y);
            }
            continue;
        }

        if (e.button != BTN_LEFT && e.button != BTN_RIGHT && e.button != BTN_MIDDLE) {
            continue;
        }

        // Update state and call when_mouse_down
        p->state.mouse_hovering = true; // If this container is pressed then clearly
                                        // we are hovered as well
        p->state.mouse_pressing       = true;
        p->state.mouse_button_pressed = e.button;

        if (p->when_mouse_down || p->when_key_event) {
            mouse_downed.push_back(p);
            if (p->when_mouse_down) {
                p->when_mouse_down(root, p);
            }
        }
    }
    if (!e.scroll)
        set_active(root, mouse_downed, root, false, false);
}

bool handle_mouse_button_release(Container* root, const Event& e) {
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif
    if (e.button != BTN_LEFT && e.button != BTN_RIGHT && e.button != BTN_MIDDLE) {
        return true;
    }

    if (e.button == BTN_LEFT)
        root->left_mouse_down = false;

    root->mouse_current_x             = e.x;
    root->mouse_current_y             = e.y;
    std::vector<Container*> concerned = concerned_containers(root);
    std::vector<Container*> pierced   = pierced_containers(root, e.x, e.y);

    // pierced   are ALL the containers under the mouse
    // concerned are all the containers which have concerned state on
    // handle_mouse_button_release can be the catalyst for sending out
    // when_mouse_up, when_drag_end, and_when_clicked

    for (int i = 0; i < concerned.size(); i++) {
        auto                c           = concerned[i];
        std::weak_ptr<bool> still_alive = c->lifetime;
        bool                p           = is_pierced(c, pierced);

        if (c->when_mouse_leaves_container && !p) {
            c->when_mouse_leaves_container(root, c);
        }
        if (!still_alive.lock())
            continue;

        if (c->when_drag_end) {
            if (c->state.mouse_dragging) {
                c->when_drag_end(root, c);
            }
        }
        if (!still_alive.lock())
            continue;

        if (c->when_clicked) {
            if (c->when_drag_end_is_click && c->state.mouse_dragging && p) {
                c->when_clicked(root, c);
            } else if ((!c->state.mouse_dragging)) {
                c->when_clicked(root, c);
            }
        }
        if (!still_alive.lock())
            continue;

        // TODO: when_clicked could've delete'd 'c' so recheck for it
        c->state.mouse_pressing = false;
        c->state.mouse_button_pressed = 0;
        c->state.mouse_dragging = false;
        c->state.mouse_hovering = false;
        c->state.concerned      = false;
    }

    handle_mouse_motion(root, e.x, e.y);

    return false;
}

#include <fstream>
#include <string>
#include <mutex>

void log_json(const std::string& msg) {
    //return;
    static bool firstCall = true;
    static std::ofstream ofs;
    static std::mutex writeMutex;
    static long num = 0;

    std::lock_guard<std::mutex> lock(writeMutex);

    if (firstCall) {
        ofs.open("/tmp/log.json", std::ios::out | std::ios::trunc);
        firstCall = false;

        // Replace "program" with something that displays a live-updating file.
        // Example choices:
        //   - `xterm -e "tail -f /tmp/log"`
        //   - `gedit /tmp/log`
        //   - `glow /tmp/log`
        //std::thread t([]() {
            //system("alacritty -e tail -f /tmp/log");
        //});
        //t.detach();
    } else if (!ofs.is_open()) {
        // If log is called after close, recover
        ofs.open("/tmp/log.json", std::ios::out | std::ios::app);
    }

    ofs << msg << '\n';
    ofs.flush(); // force write so GUI viewer always shows latest content
}

nlohmann::ordered_json output_container(Container *c) {
    nlohmann::ordered_json data = nlohmann::ordered_json::object();

    data["id"] = c->uuid;
    data["x"] = c->real_bounds.x;
    data["y"] = c->real_bounds.y;
    data["w"] = c->real_bounds.w;
    data["h"] = c->real_bounds.h;
    data["active"] = c->active;
    data["concerned"] = c->state.concerned;
    data["exists"] = c->exists;
    data["mouse_hovering"] = c->state.mouse_hovering;
    data["mouse_pressing"] = c->state.mouse_pressing;
    data["mouse_dragging"] = c->state.mouse_dragging;
    data["mouse_button_pressed"] = c->state.mouse_button_pressed;
    data["spacing"] = c->spacing;
    data["scroll_h_real"] = c->scroll_h_real;
    data["scroll_v_real"] = c->scroll_v_real;

    // children
    data["children"] = nlohmann::json::array();
    data["children_count"] = c->children.size();

    for (auto *child : c->children) {
        data["children"].push_back(output_container(child));
    }

    return data;
}

void log_layout(Container *root) {
#ifdef DEBUGCONTAINERS
    auto j = output_container(root);
    log_json(j.dump());
#endif
}

void mouse_entered(Container* root, const Event& e) {
    log(fz("mouse entered {} {}", e.x, e.y));
    log_layout(root);
    handle_mouse_motion(root, e.x, e.y);
    log_layout(root);
}

void mouse_left(Container* root, const Event& e) {
    log(fz("mouse left {} {}", -1000, -1000));
    log_layout(root);
    handle_mouse_motion(root, -1000, -1000);
    log_layout(root);
}

void move_event(Container* root, const Event& e) {
    log(fz("mouse motion {} {}", e.x, e.y));
    log_layout(root);
    handle_mouse_motion(root, e.x, e.y);
    log_layout(root);
}

void mouse_event(Container* root, const Event& e) {
    log(fz("mouse event {} {} but:{} state:{} scroll:{}", e.x, e.y, e.button, e.state, e.scroll));
    log_layout(root);
    if (e.state || e.scroll) {
        handle_mouse_button_press(root, e);
    } else {
        handle_mouse_button_release(root, e);
    }
    log_layout(root);
}

void paint_outline(Container* root, Container* c) {
    if (!c->exists)
        return;
    
    if (c->when_paint) {
        // normalize render position to monitor because of how terrible confusing hyprland rendering works
        //auto b = c->real_bounds;
        //c->real_bounds.x -= root->real_bounds.x;
        //c->real_bounds.y -= root->real_bounds.y;
        c->when_paint(root, c);
        //c->real_bounds = b;
    }
    if (!c->automatically_paint_children) {
        return;
    }
 
    if (c->type == ::newscroll) {
        auto s = (ScrollContainer *) c;
        std::vector<int> render_order;
        for (int i = 0; i < s->content->children.size(); i++) {
            render_order.push_back(i);
        }
        std::sort(render_order.begin(), render_order.end(), [s](int a, int b) -> bool {
            return s->content->children[a]->z_index < s->content->children[b]->z_index;
        });
        
        for (auto index: render_order) {
            if (overlaps(s->content->children[index]->real_bounds, s->real_bounds)) {
                paint_outline(root, s->content->children[index]);
            }
        }
        
        if (s->right && s->right->exists)
            paint_outline(root, s->right);
        if (s->bottom && s->bottom->exists)
            paint_outline(root, s->bottom);
    } else {
        std::vector<int> render_order;
        for (int i = 0; i < c->children.size(); i++) {
            render_order.push_back(i);
        }
        std::sort(render_order.begin(), render_order.end(), [c](int a, int b) -> bool {
            return c->children[a]->z_index < c->children[b]->z_index;
        });
        for (auto index: render_order) {
            paint_outline(root, c->children[index]);
        }
    }
   
    if (c->after_paint) {
        c->after_paint(root, c);
    }
    c->first_paint = false;
}

void paint_root(Container* c) {
    paint_outline(c, c);
}

static void
send_key_actual(Container *root, Container* container, int key, bool pressed, xkb_keysym_t sym, int mods, bool is_text, std::string text) {
    if (container->type == ::newscroll) {
        auto *s = (ScrollContainer *) container;
        for (auto c: s->content->children) {
            send_key_actual(root, c, key, pressed, sym, mods, is_text, text);
        }
        if (s->content->when_key_event) {
            s->content->when_key_event(root, container, key, pressed, sym, mods, is_text, text);
        }
    } else {
        for (auto c: container->children) {
            send_key_actual(root, c, key, pressed, sym, mods, is_text, text);
        }
        if (container->when_key_event) {
            container->when_key_event(root, container, key, pressed, sym, mods, is_text, text);
        }
    }
}

void key_press(Container* container, int key, bool pressed, xkb_keysym_t sym, int mods, bool is_text, std::string text) {
    send_key_actual(container, container, key, pressed, sym, mods, is_text, text);
}


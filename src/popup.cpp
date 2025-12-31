
#include "popup.h"

#include "second.h"
#include "icons.h"

#include <linux/input-event-codes.h>

int index_within_parent(Container *parent, Container *c) {
    if (!parent || !c)
        return 0;
    for (auto i = 0; i < parent->children.size(); i++) {
        if (parent->children[i] == c) {
            return i;
        }
    }
    return 0;
}

void popup::close(std::string uuid) {
    later_immediate([uuid](Timer *) {
        for (int i = 0; i < actual_root->children.size(); i++) {
           auto child = actual_root->children[i];
           if (child->uuid == uuid) {
               delete child;
               actual_root->children.erase(actual_root->children.begin() + i);
               break;
           }
        }
        for (auto m : actual_monitors) {
            hypriso->damage_entire(*datum<int>(m, "cid"));
        }
    });    
}

struct PopOptionData : UserData {
    PopOption p;
};

void popup::open(std::vector<PopOption> root, int x, int y, int cid) {
    static const float option_height = 24;
    static const float seperator_size = 5;
    auto mid = hypriso->monitor_from_cursor();
    float s = scale(mid);

    float height = 0;
    for (auto pop_option : root) {
        if (pop_option.seperator) {
            height += seperator_size;
        } else {
            height += option_height * s;
        }
    }
    auto p = actual_root->child(::vbox, 277, height);
    consume_everything(p);
    p->receive_events_even_if_obstructed = true;
    p->custom_type = (int) TYPE::OUR_POPUP;
    auto pud = new PopupUserData;
    pud->mid = mid;
    pud->cid = cid;
    p->user_data = pud;
    p->real_bounds = Bounds(x, y, p->wanted_bounds.w, p->wanted_bounds.h);
    
    { // keep on screen
        auto mb = bounds_reserved_monitor(hypriso->monitor_from_cursor());
        if (p->real_bounds.x + p->real_bounds.w > mb.x + mb.w) {
            int overflow_x = (p->real_bounds.x + p->real_bounds.w) - (mb.x + mb.w);
            p->real_bounds.x -= overflow_x;
        }
        if (p->real_bounds.y + p->real_bounds.h > mb.y + mb.h) {
            int overflow_y = (p->real_bounds.y + p->real_bounds.h) - (mb.y + mb.h);
            p->real_bounds.y -= overflow_y;
        }
        if (mb.y > p->real_bounds.y) {
            int overflow_y = mb.y - p->real_bounds.y;
            p->real_bounds.y += overflow_y;
        }
        if (mb.x > p->real_bounds.x) {
            int overflow_x = mb.x - p->real_bounds.x;
            p->real_bounds.x += overflow_x;
        }
    }
    
    p->when_mouse_enters_container = paint {
        //hypriso->all_lose_focus();
        setCursorImageUntilUnset("default");
        hypriso->send_false_position(-1, -1);
        consume_event(root, c);
    };
    p->when_mouse_leaves_container = paint {
        //hypriso->all_gain_focus();
        unsetCursorImage(true);
        consume_event(root, c);
    };
    p->when_mouse_down = paint {
        consume_event(root, c);
    };
    p->when_mouse_up = paint {
        consume_event(root, c);
    };

    //p->wanted_pad = Bounds(7, 7, 7, 7);
    p->when_paint = [](Container *actual_root, Container *c) {
        auto root = get_rendering_root();
        if (!root) return;
        auto [rid, s, stage, active_id] = roots_info(actual_root, root);
        if (stage == (int) STAGE::RENDER_POST_WINDOWS) {
            renderfix
            auto pud = (PopupUserData *) c->user_data;
            auto b = c->real_bounds;
            render_drop_shadow(rid, 1.0, {0, 0, 0, .07}, 7 * s, 2.0f, b);
            rect(b, {1, 1, 1, .75}, 0, 7 * s);
        }
    };
    p->after_paint = [](Container *actual_root, Container *c) {
        auto root = get_rendering_root();
        if (!root) return;
        auto [rid, s, stage, active_id] = roots_info(actual_root, root);
        if (stage == (int) STAGE::RENDER_POST_WINDOWS) {
            renderfix
            auto b = c->real_bounds;
            b.shrink(1);
            border(b, {0, 0, 0, .2}, 1, 0, 7 * s);
        }
    };
    
    p->pre_layout = [](Container *actual_root, Container *c, const Bounds &b) {
        ::layout(actual_root, c, c->real_bounds);
    };
    //p->child(FILL_SPACE, 2 * s);
    for (auto pop_option : root) {
        auto option = p->child(FILL_SPACE, option_height * s);
        if (pop_option.seperator) {
            option->wanted_bounds.h = seperator_size;
            option->when_paint = [](Container *actual_root, Container *c) {
                auto root = get_rendering_root();
                if (!root)
                    return;
                auto [rid, s, stage, active_id] = roots_info(actual_root, root);
                if (stage == (int)STAGE::RENDER_POST_WINDOWS) {
                    renderfix
                    auto b = c->real_bounds;
                    b.y += std::floor(b.h * .5);
                    b.h = 1.0;
                    b.x += 8 * s;
                    b.w -= 16 * s;
                    rect(b, {0, 0, 0, 0.3}, 0, 1 * s, 2.0, false);
                }
            };
            continue;
        }
        auto popdata = new PopOptionData;
        popdata->p = pop_option;
        option->user_data = popdata;
        option->when_paint = [](Container *actual_root, Container *c) {
            auto root = get_rendering_root();
            if (!root) return;
            auto [rid, s, stage, active_id] = roots_info(actual_root, root);
            if (stage == (int) STAGE::RENDER_POST_WINDOWS) {
                renderfix

                int index = index_within_parent(c->parent, c);
                bool first = index == 0;
                bool last = index == c->parent->children.size();
                
                if (c->state.mouse_hovering) {
                    auto b = c->real_bounds;
                    rect(b, {0, 0, 0, .1}, 0, 7 * s, 2.0f, false);
                }
                if (c->state.mouse_button_pressed == BTN_LEFT) {
                    if (c->state.mouse_pressing) {
                        auto b = c->real_bounds;
                        rect(b, {0, 0, 0, .2}, 0, 7 * s, 2.0, false);
                    }
                }

                auto popdata = (PopOptionData*)c->user_data;
                auto pop_option = popdata->p;

                if (!pop_option.has_attempted_loadin_icon) {
                    pop_option.has_attempted_loadin_icon = true;
                    if (!pop_option.icon_left.empty()) {
                        if (pop_option.is_text_icon) {

                        } else {
                            auto icon = pop_option.icon_left;
                            pop_option.icon_path= one_shot_icon(14 * s, {icon, to_lower(icon), c3ic_fix_wm_class(icon), to_lower(icon)});
                        }
                    }
                }
                if (pop_option.is_text_icon) {
                    auto info = gen_text_texture("Segoe Fluent Icons", pop_option.icon_left, 14 * s, {0, 0, 0, 1});
                    draw_texture(info, c->real_bounds.x + (40 * s * .5) - info.w * .5, center_y(c, info.h));
                    free_text_texture(info.id);
                } else if (!pop_option.icon_path.empty()) {
                    auto info = gen_texture(pop_option.icon_path, 18 * s);
                    draw_texture(info, c->real_bounds.x + (40 * s * .5) - info.w * .5, center_y(c, info.h));
                    free_text_texture(info.id);
                }

                auto info = gen_text_texture(mylar_font, pop_option.text, 14 * s, {0, 0, 0, 1});
                draw_texture(info, c->real_bounds.x + 40 * s, center_y(c, info.h));
                free_text_texture(info.id);
            }
        };
        option->when_clicked = paint {
            if (c->state.mouse_button_pressed != BTN_LEFT)
                return;
            auto popdata = (PopOptionData *) c->user_data;
            auto pop_option = popdata->p;
            if (pop_option.on_clicked) {
                pop_option.on_clicked();
            }

            for (auto c : actual_root->children) {
                if (c->custom_type == (int) TYPE::OUR_POPUP) {
                    popup::close(c->uuid);
                }
            }
        };
    }
    //p->child(FILL_SPACE, 2 * s);
    
    if (hypriso->on_mouse_move) {
        auto m = mouse();
        hypriso->on_mouse_move(0, m.x, m.y);
    }

    for (auto m : actual_monitors)
        hypriso->damage_entire(*datum<int>(m, "cid"));
}





#include "container.h"

#include <cassert>
#include <cmath>
#include <iostream>

#include <random>
#include <sstream>

#ifdef TRACY_ENABLE
#include <tracy/Tracy.hpp>
#endif

std::function<void(Container *)> on_any_container_close = nullptr;

// Sum of non filler child height and spacing
double reserved_height(Container* box) {
    double space = 0;
    for (auto child : box->children) {
        if (!child->exists)
            continue;
        if (child->wanted_bounds.h == FILL_SPACE) {
            space += child->wanted_pad.y + child->wanted_pad.h;
        } else if (child->wanted_bounds.h == USE_CHILD_SIZE) {
            double child_height = 0;
            for (auto grandchild : child->children) {
                if (!grandchild->exists)
                    continue;
                if (grandchild->wanted_bounds.h > child_height)
                    child_height = grandchild->wanted_bounds.h;
            }
            space += child_height;
        } else {
            space += child->wanted_bounds.h;
        }
        space += box->spacing;
    }
    space -= box->spacing; // Remove spacing after last child
    return space;
}

// The reserved height plus the relevant padding
double true_height(Container* box) {
    return reserved_height(box) + box->wanted_pad.y + box->wanted_pad.h - box->real_bounds.h;
}

double actual_true_height(Container* box) {
    // set space to the distance between the lowest y of child and the hightest
    // y+h of child
    double lowest_y      = 0;
    double highest_y     = 0;
    bool   lowest_y_set  = false;
    bool   highest_y_set = false;
    for (auto child : box->children) {
        if (!child->exists)
            continue;
        if (!lowest_y_set || child->real_bounds.y < lowest_y) {
            lowest_y     = child->real_bounds.y;
            lowest_y_set = true;
        }
        if (!highest_y_set || child->real_bounds.y + child->real_bounds.h > highest_y) {
            highest_y     = child->real_bounds.y + std::max((child->real_bounds.h - 1), 0.0);
            highest_y_set = true;
        }
    }
    return (highest_y - lowest_y) + box->wanted_pad.y + box->wanted_pad.h;
}

double actual_true_width(Container* box) {
    // set space to the distance between the lowest x of child and the hightest
    // x+w of child
    double lowest_x      = 0;
    double highest_x     = 0;
    bool   lowest_x_set  = false;
    bool   highest_x_set = false;
    for (auto child : box->children) {
        if (!child->exists)
            continue;
        if (!lowest_x_set || child->real_bounds.x < lowest_x) {
            lowest_x     = child->real_bounds.x;
            lowest_x_set = true;
        }
        if (!highest_x_set || child->real_bounds.x + child->real_bounds.w > highest_x) {
            highest_x     = child->real_bounds.x + std::max((child->real_bounds.w - 1), 0.0);
            highest_x_set = true;
        }
    }
    return (highest_x - lowest_x) + box->wanted_pad.x + box->wanted_pad.w;
}

// returns the height filler children should be
double single_filler_height(Container* container) {
    double reserved_h       = reserved_height(container);
    double available_h      = container->children_bounds.h - reserved_h;
    double single_fill_size = 0;
    if (available_h > 0) {
        double filler_children_count = 0;
        for (auto child : container->children) {
            if (child->wanted_bounds.h == FILL_SPACE) {
                filler_children_count++;
            }
        }
        if (filler_children_count > 0) {
            single_fill_size = available_h / filler_children_count;
        }
    }
    return single_fill_size;
}

void modify_all(Container* container, double x_change, double y_change) {
    for (auto child : container->children) {
        modify_all(child, x_change, y_change);
    }

    container->real_bounds.x += x_change;
    container->real_bounds.y += y_change;
}


void layout_absolute(Container* root, Container* container, const Bounds& bounds) {
    if (container->pre_layout) {
        container->pre_layout(root, container, bounds);
    }
    for (auto child : container->children) {
        if (child && child->pre_layout) {
            child->pre_layout(root, child, bounds);
        }
    }
    for (auto child : container->children) {
        if (child && child->exists) {
            layout(root, child, Bounds(child->real_bounds.x, child->real_bounds.y, child->real_bounds.w, child->real_bounds.h));
        }
    }
}

void layout_fullycustom(Container* root, Container* container, const Bounds& bounds) {
    if (container->pre_layout)
        container->pre_layout(root, container, bounds);
}

void layout_vbox(Container* root, Container* container, const Bounds& bounds) {
    for (auto child : container->children) {
        if (child && child->pre_layout) {
            child->pre_layout(root, child, bounds);
        }
    }

    double fill_h = single_filler_height(container);

    double offset = 0;
    for (auto child : container->children) {
        if (child && child->exists) {
            double target_w = child->wanted_pad.x + child->wanted_pad.w;
            double target_h = child->wanted_pad.y + child->wanted_pad.h;

            if (child->before_layout)
                child->before_layout(root, child, bounds, &target_w, &target_h);

            if (child->wanted_bounds.w == FILL_SPACE) {
                target_w = container->children_bounds.w;
            } else if (child->wanted_bounds.w == USE_CHILD_SIZE) {
                target_w += reserved_width(child);
            } else {
                target_w += child->wanted_bounds.w;
            }
            if (child->wanted_bounds.h == FILL_SPACE) {
                target_h += fill_h;
            } else if (child->wanted_bounds.h == USE_CHILD_SIZE) {
                target_h += reserved_height(child);
            } else {
                target_h += child->wanted_bounds.h;
            }
            if (child->wanted_bounds.w == DYNAMIC || child->wanted_bounds.h == DYNAMIC) {
                child->when_layout(root, child, bounds, &target_w, &target_h);
            }

            // Keep within horizontal bounds
            if (container->scroll_h_real > 0)
                container->scroll_h_real = 0;
            if (container->scroll_h_real != 0) {
                double overhang = target_w - container->real_bounds.w;
                if (-container->scroll_h_real > overhang) {
                    container->scroll_h_real = -overhang;
                }
                if (overhang < 0) {
                    container->scroll_h_real = 0;
                }
            }

            // Keep within vertical bounds
            if (container->scroll_v_real > 0)
                container->scroll_v_real = 0;
            if (container->scroll_v_real != 0) {
                double overhang = target_h - container->real_bounds.h;
                if (-container->scroll_v_real > overhang) {
                    container->scroll_v_real = -overhang;
                }
                if (overhang < 0) {
                    container->scroll_v_real = 0;
                }
            }

            // Keep within horizontal bounds
            if (container->scroll_h_visual > 0)
                container->scroll_h_visual = 0;
            if (container->scroll_h_visual != 0) {
                double overhang = target_w - container->real_bounds.w;
                if (-container->scroll_h_visual > overhang) {
                    container->scroll_h_visual = -overhang;
                }
                if (overhang < 0) {
                    container->scroll_h_visual = 0;
                }
            }

            // Keep within vertical bounds
            if (container->scroll_v_visual > 0)
                container->scroll_v_visual = 0;
            if (container->scroll_v_visual != 0) {
                double overhang = target_h - container->real_bounds.h;
                if (-container->scroll_v_visual > overhang) {
                    container->scroll_v_visual = -overhang;
                }
                if (overhang < 0) {
                    container->scroll_v_visual = 0;
                }
            }

            layout(root, child,
                   Bounds(container->children_bounds.x + container->scroll_h_visual, container->children_bounds.y + offset + container->scroll_v_visual, target_w, target_h));

            offset += child->real_bounds.h + container->spacing;
        }
    }

    if (container->wanted_bounds.w == USE_CHILD_SIZE) {
        container->real_bounds.w = reserved_width(container);
    }
    if (container->wanted_bounds.h == USE_CHILD_SIZE) {
        container->real_bounds.h = reserved_height(container);
    }

    if (container->alignment & ALIGN_CENTER) {
        // Get height, divide by two, subtract that by parent y - h / 2
        double full_height  = offset;
        double align_offset = bounds.h / 2 - full_height / 2;

        modify_all(container, 0, align_offset);
    }
}

// Sum of non filler child widths and spacing
double reserved_width(Container* box) {
    double space = 0;
    for (auto child : box->children) {
        if (child) {
            if (!child->exists)
                continue;
            if (child->wanted_bounds.w == FILL_SPACE) {
                space += child->wanted_pad.x + child->wanted_pad.w;
            } else if (child->wanted_bounds.w == USE_CHILD_SIZE) {
                space += reserved_width(child);
            } else {
                space += child->wanted_bounds.w;
            }
            space += box->spacing;
        }
    }
    space -= box->spacing; // Remove spacing after last child
    return space;
}

// The reserved width plus the relevant padding
double true_width(Container* box) {
    return reserved_width(box) + box->wanted_pad.x + box->wanted_pad.w - box->real_bounds.w;
}

// returns the width filler children should be
double single_filler_width(Container* container, const Bounds bounds) {
    double reserved_w       = reserved_width(container);
    double available_w      = container->children_bounds.w - reserved_w;
    double single_fill_size = 0;
    if (available_w > 0) {
        double filler_children_count = 0;
        for (auto child : container->children) {
            if (child) {
                if (child->wanted_bounds.w == FILL_SPACE) {
                    filler_children_count++;
                }
            }
        }
        if (filler_children_count > 0) {
            single_fill_size = available_w / filler_children_count;
        }
    }
    return single_fill_size;
}

void layout_hbox(Container* root, Container* container, const Bounds& bounds) {
    for (auto child : container->children) {
        if (child && child->pre_layout) {
            child->pre_layout(root, child, bounds);
        }
    }

    double fill_w = single_filler_width(container, bounds);

    double offset = 0;
    for (auto child : container->children) {
        if (child && child->exists) {
            double target_w = child->wanted_pad.x + child->wanted_pad.w;
            double target_h = child->wanted_pad.y + child->wanted_pad.h;

            if (child->before_layout)
                child->before_layout(root, child, bounds, &target_w, &target_h);

            if (child->wanted_bounds.w == FILL_SPACE) {
                target_w += fill_w;
            } else if (child->wanted_bounds.w == USE_CHILD_SIZE) {
                target_w += reserved_width(child);
            } else {
                target_w += child->wanted_bounds.w;
            }
            if (child->wanted_bounds.h == FILL_SPACE) {
                target_h = container->children_bounds.h;
            } else if (child->wanted_bounds.h == USE_CHILD_SIZE) {
                target_h += reserved_height(child);
            } else {
                target_h += child->wanted_bounds.h;
            }
            if (child->wanted_bounds.w == DYNAMIC || child->wanted_bounds.h == DYNAMIC) {
                child->when_layout(root, child, bounds, &target_w, &target_h);
            }

            // Keep within horizontal bounds
            if (container->scroll_h_real > 0)
                container->scroll_h_real = 0;
            if (container->scroll_h_real != 0) {
                double overhang = target_w - container->real_bounds.w;
                if (-container->scroll_h_real > overhang) {
                    container->scroll_h_real = -overhang;
                }
                if (overhang < 0) {
                    container->scroll_h_real = 0;
                }
            }

            // Keep within vertical bounds
            if (container->scroll_v_real > 0)
                container->scroll_v_real = 0;
            if (container->scroll_v_real != 0) {
                double overhang = target_h - container->real_bounds.h;
                if (-container->scroll_v_real > overhang) {
                    container->scroll_v_real = -overhang;
                }
                if (overhang < 0) {
                    container->scroll_v_real = 0;
                }
            }

            // Keep within horizontal bounds
            if (container->scroll_h_visual > 0)
                container->scroll_h_visual = 0;
            if (container->scroll_h_visual != 0) {
                double overhang = target_w - container->real_bounds.w;
                if (-container->scroll_h_visual > overhang) {
                    container->scroll_h_visual = -overhang;
                }
                if (overhang < 0) {
                    container->scroll_h_visual = 0;
                }
            }

            // Keep within vertical bounds
            if (container->scroll_v_visual > 0)
                container->scroll_v_visual = 0;
            if (container->scroll_v_visual != 0) {
                double overhang = target_h - container->real_bounds.h;
                if (-container->scroll_v_visual > overhang) {
                    container->scroll_v_visual = -overhang;
                }
                if (overhang < 0) {
                    container->scroll_v_visual = 0;
                }
            }

            layout(root, child,
                   Bounds(container->children_bounds.x + offset + container->scroll_h_visual, container->children_bounds.y + container->scroll_v_visual, target_w, target_h));

            offset += child->real_bounds.w + container->spacing;
        }
    }

    if (container->wanted_bounds.w == USE_CHILD_SIZE) {
        container->real_bounds.w = reserved_width(container);
    }
    if (container->wanted_bounds.h == USE_CHILD_SIZE) {
        container->real_bounds.h = reserved_height(container);
    }

    if (container->alignment & ALIGN_CENTER) {
        for (auto c : container->children) {
            if (c->wanted_bounds.h != FILL_SPACE) {
                // Get height, divide by two, subtract that by parent y - h / 2
                double full_height  = c->real_bounds.h;
                double align_offset = bounds.h / 2 - full_height / 2;
                modify_all(c, 0, align_offset);
            }
        }
    }
    if (container->alignment & ALIGN_RIGHT) {
        if (!container->children.empty()) {
            Container* first            = container->children[0];
            Container* last             = container->children[container->children.size() - 1];
            double     total_children_w = (last->real_bounds.x + last->real_bounds.w) - first->real_bounds.x;

            for (auto c : container->children) {
                modify_all(c, (container->real_bounds.w - container->wanted_pad.w - total_children_w), 0);
            }
        }
    }
    if (container->alignment & ALIGN_CENTER_HORIZONTALLY) {
        if (!container->children.empty()) {
            Bounds     real_bounds      = container->real_bounds;
            Container* first            = container->children[0];
            Container* last             = container->children[container->children.size() - 1];
            double     total_children_w = (last->real_bounds.x + last->real_bounds.w) - first->real_bounds.x;

            for (auto c : container->children) {
                modify_all(c, (container->real_bounds.w - total_children_w) * .5, 0);
            }
            // guarantee first is greater than real_bounds.x
            if (first->real_bounds.x < real_bounds.x) {
                auto diff = real_bounds.x - first->real_bounds.x;
                for (auto c : container->children) {
                    modify_all(c, diff, 0);
                }
            }
        }
    }
    if (container->alignment & ALIGN_GLOBAL_CENTER_HORIZONTALLY) {
        if (!container->children.empty()) {
            Container* root = container;
            while (true) {
                if (root->parent == nullptr)
                    break;
                root = root->parent;
            }

            Bounds     real_bounds      = container->real_bounds;
            Container* first            = container->children[0];
            Container* last             = container->children[container->children.size() - 1];
            double     total_children_w = (last->real_bounds.x + last->real_bounds.w) - first->real_bounds.x;
            double     target_x         = root->real_bounds.w / 2 - total_children_w / 2;
            double     initial_x        = first->real_bounds.x;
            for (auto c : container->children) {
                modify_all(c, target_x - initial_x, 0);
            }
            // guarantee first is greater than real_bounds.x
            if (first->real_bounds.x < real_bounds.x) {
                auto diff = real_bounds.x - first->real_bounds.x;
                for (auto c : container->children) {
                    modify_all(c, diff, 0);
                }
            }
            // guarantee last is less than real_bounds.x
            if (last->real_bounds.x + last->real_bounds.w > real_bounds.x + real_bounds.w) {
                auto diff = (real_bounds.x + real_bounds.w) - (last->real_bounds.x + last->real_bounds.w);
                for (auto c : container->children) {
                    modify_all(c, diff, 0);
                }
            }
            // guarantee first is greater than real_bounds.x
            if (first->real_bounds.x < real_bounds.x) {
                auto diff = real_bounds.x - first->real_bounds.x;
                for (auto c : container->children) {
                    modify_all(c, diff, 0);
                }
            }
        }
    }
}

void layout_stack(Container* root, Container* container, const Bounds& bounds) {
    for (auto child : container->children) {
        layout(root, child, bounds);
    }
}

// Expected container children:
// [required] right_box
// [required] bottom_box
// [required] content_area
void layout_scrollpane(Container* root, Container* container, const Bounds& bounds) { 
    assert(container->children.size() == 3 && !container->children[2]->children.empty());

    auto* r_bar        = container->children[0];
    auto* b_bar        = container->children[1];
    auto* content_area = container->children[2];
    auto* content      = container->children[2]->children[0];

    int   rv                      = content_area->scroll_v_real;
    int   rh                      = content_area->scroll_h_real;
    content_area->scroll_v_real   = 0;
    content_area->scroll_h_real   = 0;
    int vv                        = content_area->scroll_v_visual;
    int vh                        = content_area->scroll_h_visual;
    content_area->scroll_v_visual = 0;
    content_area->scroll_h_visual = 0;
    layout(root, content_area, Bounds(bounds.x, bounds.y, bounds.w, bounds.h));
    content_area->scroll_v_real   = rv;
    content_area->scroll_h_real   = rh;
    content_area->scroll_v_visual = vv;
    content_area->scroll_h_visual = vh;

    int    options = container->type;

    double r_w      = r_bar->wanted_bounds.w;
    double b_h      = b_bar->wanted_bounds.h;
    double target_w = content->wanted_bounds.w;
    double target_h = content->wanted_bounds.h;

    if (options & scrollpane_r_always) {

    } else if (options & scrollpane_r_never) {
        r_w = 0;
    } else if (options & scrollpane_r_sometimes) { // sometimes
        if (options & scrollpane_inline_r) {
            if (target_h <= container->real_bounds.h) {
                r_w = 0;
            }
        } else {
            int future_b_h = b_h;

            if (options & scrollpane_b_always) {

            } else if (options & scrollpane_b_never) {
                future_b_h = 0;
            } else { // sometimes
                if (options & scrollpane_inline_b) {
                    if (target_w <= container->real_bounds.w) {
                        future_b_h = 0;
                    }
                } else {
                    if (target_w + r_w <= container->real_bounds.w && b_h != 0) {
                        future_b_h = 0;
                    }
                }
            }

            if (target_h + future_b_h <= container->real_bounds.h) {
                r_w = 0;
            }
        }
    }
    if (options & scrollpane_b_always) {

    } else if (options & scrollpane_b_never) {
        b_h = 0;
    } else if (options & scrollpane_b_sometimes) { // sometimes
        if (options & scrollpane_inline_b) {
            if (target_w <= container->real_bounds.w) {
                b_h = 0;
            }
        } else {
            if (target_w + r_w <= container->real_bounds.w && b_h != 0) {
                b_h = 0;
            }
        }
    }
    r_bar->exists = (r_w != 0);
    b_bar->exists = (b_h != 0);

    if (!(options & ::scrollpane_inline_r) && !(options & ::scrollpane_inline_b)) {
        layout(root, content_area, Bounds(bounds.x, bounds.y, bounds.w - r_w, bounds.h - b_h));
    } else if (!(options & ::scrollpane_inline_r)) {
        layout(root, content_area, Bounds(bounds.x, bounds.y, bounds.w - r_w, bounds.h));
    } else if (!(options & ::scrollpane_inline_b)) {
        layout(root, content_area, Bounds(bounds.x, bounds.y, bounds.w, bounds.h - b_h));
    } else {
        layout(root, content_area, Bounds(bounds.x, bounds.y, bounds.w, bounds.h));
    }

    if (r_bar->exists) {
        layout(root, r_bar, Bounds(bounds.x + bounds.w - r_w, bounds.y, r_w, bounds.h - b_h));
    }
    if (b_bar->exists)
        layout(root, b_bar, Bounds(bounds.x, bounds.y + bounds.h - b_h, bounds.w - r_w, b_h));
}

void clamp_scroll(ScrollContainer* scrollpane) {
    double true_height = actual_true_height(scrollpane->content);
    // add to true_height to account for bottom if it exists and not inline
    if (scrollpane->bottom && scrollpane->bottom->exists && !scrollpane->settings.bottom_inline_track)
        true_height += scrollpane->bottom->real_bounds.h;
    scrollpane->scroll_v_real   = -std::max(0.0, std::min(-scrollpane->scroll_v_real, true_height - scrollpane->real_bounds.h));
    scrollpane->scroll_v_visual = -std::max(0.0, std::min(-scrollpane->scroll_v_visual, true_height - scrollpane->real_bounds.h));

    double true_width = actual_true_width(scrollpane->content);
    // add to true_width to account for right if it exists and not inline
    if (scrollpane->right && scrollpane->right->exists && !scrollpane->settings.right_inline_track)
        true_width += scrollpane->right->real_bounds.w;
    scrollpane->scroll_h_real   = -std::max(0.0, std::min(-scrollpane->scroll_h_real, true_width - scrollpane->real_bounds.w));
    scrollpane->scroll_h_visual = -std::max(0.0, std::min(-scrollpane->scroll_h_visual, true_width - scrollpane->real_bounds.w));
}

void layout_newscrollpane_content(Container* root, ScrollContainer* scroll, const Bounds& bounds, bool right_scroll_bar_needed, bool bottom_scroll_bar_needed) {
    double             w        = scroll->content->wanted_bounds.w;
    double             h        = scroll->content->wanted_bounds.h;
    ScrollPaneSettings settings = scroll->settings;

    if (w == FILL_SPACE) {
        w = bounds.w - (settings.right_inline_track ? 0 : right_scroll_bar_needed ? settings.right_width : 0);
        if (settings.right_show_amount == 0) {
            w = bounds.w - settings.right_width;
        } else if (settings.right_show_amount == 2) {
            w = bounds.w;
        }
    } else {
        w = true_width(scroll->content);
    }
    if (h == FILL_SPACE) {
        h = bounds.h - (settings.bottom_inline_track ? 0 : bottom_scroll_bar_needed ? settings.bottom_height : 0);
        if (settings.bottom_show_amount == 0) {
            h = bounds.h - settings.bottom_height;
        } else if (settings.bottom_show_amount == 2) {
            h = bounds.h;
        }
    } else {
        h = true_height(scroll->content);
    }

    layout(root, scroll->content, Bounds(bounds.x + scroll->scroll_h_visual, bounds.y + scroll->scroll_v_visual, w, h));

    scroll->content->real_bounds.h = actual_true_height(scroll->content);
    scroll->content->real_bounds.w = actual_true_width(scroll->content);
}

void layout_newscrollpane(Container* root, ScrollContainer* scroll, const Bounds& bounds) {
    if (scroll->pre_layout)
        scroll->pre_layout(root, scroll, bounds);
    ScrollPaneSettings settings = scroll->settings;

    // layout the content as if the scroll bars were needed, and then if the size
    // exceeds the bounds, layout again but with only the needed scroll bars
    layout_newscrollpane_content(root, scroll, bounds, true, true);

    bool right_scroll_bar_needed  = scroll->content->real_bounds.h > scroll->real_bounds.h;
    bool bottom_scroll_bar_needed = scroll->content->real_bounds.w > scroll->real_bounds.w;

    bool create_right_scrollbar = right_scroll_bar_needed;
    if (settings.right_show_amount == 2) {
        create_right_scrollbar = false;
    } else if (settings.right_show_amount == 0) {
        create_right_scrollbar = true;
    }
    bool create_bottom_scrollbar = bottom_scroll_bar_needed;
    if (settings.bottom_show_amount == 2) {
        create_bottom_scrollbar = false;
    } else if (settings.bottom_show_amount == 0) {
        create_bottom_scrollbar = true;
    }

    clamp_scroll(scroll);

    layout_newscrollpane_content(root, scroll, bounds, right_scroll_bar_needed, bottom_scroll_bar_needed);

    if (create_right_scrollbar) {
        layout(root, scroll->right, Bounds(bounds.x + bounds.w - settings.right_width, bounds.y, settings.right_width, bounds.h));
    } else {
        scroll->right->exists = false;
    }
    if (create_bottom_scrollbar) {
        layout(root, scroll->bottom, Bounds(bounds.x, bounds.y + bounds.h - settings.bottom_height, bounds.w - settings.right_width, settings.bottom_height));
    } else {
        scroll->bottom->exists = false;
    }
}

void layout(Container* root, Container* container, const Bounds& bounds) {
    container->real_bounds.x = bounds.x;
    container->real_bounds.y = bounds.y;

    bool fill_w              = container->wanted_bounds.w == FILL_SPACE;
    bool fill_h              = container->wanted_bounds.h == FILL_SPACE;
    container->real_bounds.w = (fill_w) ? bounds.w : container->wanted_bounds.w;
    container->real_bounds.h = (fill_h) ? bounds.h : container->wanted_bounds.h;

    container->children_bounds.x = container->real_bounds.x + container->wanted_pad.x;
    container->children_bounds.y = container->real_bounds.y + container->wanted_pad.y;
    container->children_bounds.w = container->real_bounds.w - container->wanted_pad.x - container->wanted_pad.w;
    container->children_bounds.h = container->real_bounds.h - container->wanted_pad.y - container->wanted_pad.h;

    if (container->type & layout_type::newscroll) {
        auto s = (ScrollContainer*)container;
        if (s->content->children.empty()) {
            s->content->exists = false;
            s->right->exists   = false;
            s->bottom->exists  = false;
            return;
        }
        s->content->exists = true;
        s->right->exists   = true;
        s->bottom->exists  = true;
    } else if (container->children.empty()) {
        return;
    }
    if (!container->should_layout_children)
        return;

    if (container->type & layout_type::hbox) {
        layout_hbox(root, container, container->children_bounds);
    } else if (container->type & layout_type::vbox) {
        layout_vbox(root, container, container->children_bounds);
    } else if (container->type & layout_type::stack) {
        layout_stack(root, container, container->children_bounds);
    } else if (container->type & layout_type::scrollpane) {
        layout_scrollpane(root, container, container->children_bounds);
    } else if (container->type & layout_type::transition) {
        for (int i = 0; i < container->children.size(); i++) {
            auto child = container->children[i];
            if (i == 0) {
                child->exists = true;
                layout(root, child, bounds);
            } else {
                child->exists = false;
            }
        }
    } else if (container->type & layout_type::newscroll) {
        layout_newscrollpane(root, (ScrollContainer*)container, container->children_bounds);
    } else if (container->type & layout_type::editable_label) {
    } else if (container->type & layout_type::absolute) {
        layout_absolute(root, container, container->children_bounds);
    } else if (container->type & layout_type::fullycustom) {
        layout_fullycustom(root, container, container->children_bounds);
    }

    // TODO: this only covers the first layer and not all of them
    // for sharp pixel boundaries
    for (auto child : container->children) {
        if (child) {
            child->real_bounds.x = round(child->real_bounds.x);
            child->real_bounds.y = round(child->real_bounds.y);
            child->real_bounds.w = round(child->real_bounds.w);
            child->real_bounds.h = round(child->real_bounds.h);

            child->children_bounds.x = round(child->children_bounds.x);
            child->children_bounds.y = round(child->children_bounds.y);
            child->children_bounds.w = round(child->children_bounds.w);
            child->children_bounds.h = round(child->children_bounds.h);
        }
    }

    if (container->distribute_overflow_to_children) {
        if (!container->children.empty()) {
            double overflow = 0;
            int    i        = 0;
            do {
                auto last  = container->children[container->children.size() - 1];
                auto right = (last->real_bounds.x + last->real_bounds.w) - container->real_bounds.x;
                overflow   = right - container->real_bounds.w;
                overflow--;
                container->children[i]->real_bounds.w -= 1;
                for (int l = i + 1; l < container->children.size(); l++)
                    container->children[l]->real_bounds.x -= 1;
                i++;
                if (i == container->children.size())
                    i = 0;
            } while (overflow > 0);
        }
    }

    //    if (generate_event) {
    //        if (container->when_layout) {
    //            container->when_layout(container);
    //        }
    //    }
}

Container* container_by_name(std::string name, Container* root) {
    if (!root) {
        return nullptr;
    }

    if (root->name == name) {
        return root;
    }

    if (root->type == layout_type::newscroll) {
        auto scroll   = (ScrollContainer*)root;
        auto possible = container_by_name(name, scroll->content);
        if (possible)
            return possible;
        possible = container_by_name(name, scroll->right);
        if (possible)
            return possible;
        possible = container_by_name(name, scroll->bottom);
        if (possible)
            return possible;
        for (auto child : scroll->children) {
            possible = container_by_name(name, child);
            if (possible)
                return possible;
        }
    } else {
        for (auto child : root->children) {
            auto possible = container_by_name(name, child);
            if (possible)
                return possible;
        }
    }

    return nullptr;
}

Container* container_by_name_up(std::string name, Container* root) {
    auto parent = root->parent;
    if (!parent)
        return nullptr;
    if (parent->name == name) {
        return parent;
    }

        /*
    if (parent->type == layout_type::newscroll) {
        auto scroll   = (ScrollContainer*)parent;
        auto possible = container_by_name(name, scroll->content);
        if (possible)
            return possible;
        possible = container_by_name(name, scroll->right);
        if (possible)
            return possible;
        possible = container_by_name(name, scroll->bottom);
        if (possible)
            return possible;
        for (auto child : scroll->children) {
            possible = container_by_name(name, child);
            if (possible)
                return possible;
        }
    } else {
        for (auto child : parent->children) {
            auto possible = container_by_name(name, child);
            if (possible)
                return possible;
        }
    }
        */

    return container_by_name_up(name, parent);
}



Container* container_by_container(Container* target, Container* root) {
    if (root == target) {
        return root;
        ;
    }

    if (root->type == layout_type::newscroll) {
        auto scroll   = (ScrollContainer*)root;
        auto possible = container_by_container(target, scroll->content);
        if (possible)
            return possible;
        possible = container_by_container(target, scroll->right);
        if (possible)
            return possible;
        possible = container_by_container(target, scroll->bottom);
        if (possible)
            return possible;
        for (auto child : scroll->children) {
            possible = container_by_container(target, child);
            if (possible)
                return possible;
        }
    } else {
        for (auto child : root->children) {
            auto possible = container_by_container(target, child);
            if (possible)
                return possible;
        }
    }

    return nullptr;
}

bool overlaps(Bounds a, Bounds b) {
    if (a.x > (b.x + b.w) || b.x > (a.x + a.w))
        return false;

    return !(a.y > (b.y + b.h) || b.y > (a.y + a.h));
}

bool bounds_contains(const Bounds& bounds, int x, int y) {
    int bounds_x = std::round(bounds.x);
    int bounds_y = std::round(bounds.y);
    int bounds_w = std::round(bounds.w);
    int bounds_h = std::round(bounds.h);

    return x >= bounds_x && x <= bounds_x + bounds_w && y >= bounds_y && y <= bounds_y + bounds_h;
}

Bounds::Bounds(double x, double y, double w, double h) {
    this->x = x;
    this->y = y;
    this->w = w;
    this->h = h;
}

Bounds::Bounds(const Bounds& b) {
    x = b.x;
    y = b.y;
    w = b.w;
    h = b.h;
}

Bounds::Bounds() {
    x = 0;
    y = 0;
    w = 0;
    h = 0;
}

bool Bounds::non_zero() {
    return (x != 0) || (y != 0) || (w == 0) || (h == 0);
}

void Bounds::shrink(double amount) {
    this->x += amount;
    this->y += amount;
    this->w -= amount * 2;
    this->h -= amount * 2;
}

void Bounds::grow(double amount) {
    this->x -= amount;
    this->y -= amount;
    this->w += amount * 2;
    this->h += amount * 2;
}

Bounds Bounds::scale(double amount) {
    this->x *= amount;
    this->y *= amount;
    this->w *= amount;
    this->h *= amount;
    return *this;
}

Bounds Bounds::scale_from_center(double scale) {
    double oldW = w, oldH = h;

    w *= scale;
    h *= scale;

    x -= (w - oldW) * .5;
    y -= (h - oldH) * .5;

    return *this;
}

Bounds Bounds::round() {
    double roundedX = std::round(x);
    double roundedY = std::round(y);
    double newW     = x + w - roundedX;
    double newH     = y + h - roundedY;

    x = roundedX;
    y = roundedY;
    w = std::round(newW);
    h = std::round(newH);
    return *this;
}

static std::string get_uuid() {
    std::random_device              rd;
    std::mt19937                    gen(rd());
    std::uniform_int_distribution<> dis(0, 15);
    std::uniform_int_distribution<> dis2(8, 11); // for variant

    std::stringstream               ss;
    ss << std::hex;
    for (int i = 0; i < 8; i++)
        ss << dis(gen);
    ss << "-";
    for (int i = 0; i < 4; i++)
        ss << dis(gen);
    ss << "-4"; // UUID version 4
    for (int i = 0; i < 3; i++)
        ss << dis(gen);
    ss << "-";
    ss << dis2(gen); // UUID variant
    for (int i = 0; i < 3; i++)
        ss << dis(gen);
    ss << "-";
    for (int i = 0; i < 12; i++)
        ss << dis(gen);
    return ss.str();
}

Container* Container::child(int wanted_width, int wanted_height) {
    Container* child_container = new Container(wanted_width, wanted_height);
    child_container->parent    = this;
    this->children.push_back(child_container);
    return child_container;
}

Container* Container::child(int type, int wanted_width, int wanted_height) {
    Container* child_container = new Container(wanted_width, wanted_height);
    child_container->type      = type;
    child_container->parent    = this;
    this->children.push_back(child_container);
    return child_container;
}

Container::Container(layout_type type, double wanted_width, double wanted_height) {
    this->type      = type;
    wanted_bounds.w = wanted_width;
    wanted_bounds.h = wanted_height;
    uuid            = get_uuid();
}

Container::Container(double wanted_width, double wanted_height) {
    wanted_bounds.w = wanted_width;
    wanted_bounds.h = wanted_height;
    uuid            = get_uuid();
}

Container::Container(const Container& c) {
    parent = c.parent;
    name   = c.name;
    uuid   = c.uuid;

    for (auto child : c.children) {
        children.push_back(new Container(*child));
    }

    type    = c.type;
    z_index = c.z_index;
    spacing = c.spacing;

    wanted_bounds   = c.wanted_bounds;
    wanted_pad      = c.wanted_pad;
    real_bounds     = c.real_bounds;
    children_bounds = c.children_bounds;
    interactable    = c.interactable;
    alignment       = c.alignment;

    should_layout_children = c.should_layout_children;
    clip_children          = c.clip_children;

    when_paint                      = c.when_paint;
    when_layout                     = c.when_layout;
    when_mouse_enters_container     = c.when_mouse_enters_container;
    when_mouse_leaves_container     = c.when_mouse_leaves_container;
    when_scrolled                   = c.when_scrolled;
    when_drag_start                 = c.when_drag_start;
    when_drag                       = c.when_drag;
    when_drag_end                   = c.when_drag_end;
    when_mouse_down                 = c.when_mouse_down;
    when_mouse_up                   = c.when_mouse_up;
    when_clicked                    = c.when_clicked;
    distribute_overflow_to_children = c.distribute_overflow_to_children;
}

Container::~Container() {
    for (auto child : children) {
        if (child->type == layout_type::newscroll) {
            delete (ScrollContainer*)child;
        } else {
            delete child;
        }
    }
    if (this->on_closed)
       this->on_closed(this);
    if (on_any_container_close) {
        on_any_container_close(this);
    }
    //    auto data = static_cast<UserData *>(user_data);
    if (skip_delete)
        return;
    auto data = (UserData*)user_data;
    if (data != nullptr) {
        data->destroy();
    }

    // clear_data_for(this);
    //    ((UserData *) user_data)->destroy();
}

Container::Container() {
    parent                 = nullptr;
    type                   = layout_type::hbox;
    z_index                = 0;
    spacing                = 0;
    should_layout_children = true;
    user_data              = nullptr;
    uuid                   = get_uuid();
}

ScrollContainer* Container::scrollchild(const ScrollPaneSettings& scroll_pane_settings) {
    return new ScrollContainer(scroll_pane_settings);
    // return make_newscrollpane_as_child(this, scroll_pane_settings);
}

ScrollPaneSettings::ScrollPaneSettings(float scale) {
    this->right_width        = this->right_width * scale;
    this->bottom_height      = this->bottom_height * scale;
    this->right_arrow_height = this->right_arrow_height * scale;
    this->bottom_arrow_width = this->bottom_arrow_width * scale;
}



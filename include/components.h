//
// Created by jmanc3 on 6/14/20.
//

#ifndef SCROLL_COMPONENTS_H
#define SCROLL_COMPONENTS_H

#include <stack>
#include "container.h"
#include "hypriso.h"

struct DrawContext {
    cairo_t *cr = nullptr;
    float dpi = 1.0;
    std::function<void ()> on_needs_frame = nullptr;
};

struct ScrollData : UserData {
    std::function<DrawContext (Container *root)> func = nullptr;
};

ScrollContainer *
make_newscrollpane_as_child(Container *parent, const ScrollPaneSettings &settings, std::function<DrawContext (Container *root)> func);

#endif// SCROLL_COMPONENTS_H

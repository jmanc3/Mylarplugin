static void fill_projection_container(Dock *dock) {
    dock->projection->root->user_data = dock;
    dock->projection->root->wanted_bounds.w = FILL_SPACE;
    dock->projection->root->wanted_bounds.h = FILL_SPACE;
    auto scroll = make_newscrollpane_as_child(dock->projection->root, ScrollPaneSettings(1.0), [](Container *root) {
        auto dock = ((Dock *) root->user_data);
        return DrawContext({dock->projection->raw_window->cr, dock->projection->raw_window->dpi, [dock]() {
            windowing::redraw(dock->projection->raw_window);
        }});
    });
    dock->projection->root->when_paint = [](Container *root, Container *c) {
        auto dock = (Dock *) root->user_data;
        auto cr = dock->projection->raw_window->cr;
        set_argb(cr, {1, 1, 1, 1});
        drawRoundedRect(cr, c->real_bounds.x, c->real_bounds.y, c->real_bounds.w, c->real_bounds.h, 10 * dock->projection->raw_window->dpi, 1.0);
        cairo_fill(cr);
        set_argb(cr, border_color);
        drawRoundedRect(cr, c->real_bounds.x, c->real_bounds.y, c->real_bounds.w, c->real_bounds.h, 10 * dock->projection->raw_window->dpi, 1.0);
        cairo_stroke(cr);
    };

    auto parent = scroll->content;
    scroll->pre_layout = [](Container *root, Container *c, const Bounds &b) {
        auto scroll = (ScrollContainer *) c;
        auto dock = (Dock *) root->user_data;
        auto dpi = dock->projection->raw_window->dpi;
        scroll->content->wanted_pad = Bounds(10 * dpi, 10 * dpi, 10 * dpi, 10 * dpi);
        scroll->content->spacing = 10 * dpi;
    };
    make_self_sizing_label(parent, "Mirror this screen to", 12, [](Dock *d) {
        return d->projection;
    });
    auto icon_label_create = [](Container *parent, std::string left, std::string right, bool left_bright, bool right_bright, std::string text, int size, RGBA color) {
        auto label = parent->child(FILL_SPACE, FILL_SPACE);
        label->pre_layout = [](Container *root, Container *c, const Bounds &b) {
            auto dock = (Dock *) root->user_data;
            auto dpi = dock->projection->raw_window->dpi;
            c->wanted_bounds.h = 40 * dpi;
        };
        label->when_paint = [text, size, color, left, right, left_bright, right_bright](Container *root, Container *c) {
            auto dock = (Dock *) root->user_data;
            auto dpi = dock->projection->raw_window->dpi;
            auto cr = dock->projection->raw_window->cr;
            auto bounds_text = draw_text(cr, 0, 0, text, size * dpi, false, mylar_font, -1, -1, color);
            auto left_color = color;
            auto right_color = color;
            auto bounds_left = draw_text(cr, 0, 0, left, size * dpi, false, "Segoe Fluent Icons", -1, -1, left_color);
            auto bounds_right = draw_text(cr, 0, 0, right, size * dpi, false, "Segoe Fluent Icons", -1, -1, right_color);
            if (!left_bright)
                left_color.a = .4;
            if (!right_bright)
                right_color.a = .4;

            float xoff = c->real_bounds.x + 8 * dpi;

            draw_text(cr, 
                xoff, 
                c->real_bounds.y + c->real_bounds.h * .5 - bounds_left.h * .5, 
                left, size * dpi, true, mylar_font, -1, -1, left_color);
            
            xoff += bounds_left.w;
            
            draw_text(cr, 
                xoff, 
                c->real_bounds.y + c->real_bounds.h * .5 - bounds_right.h * .5, 
                right, size * dpi, true, mylar_font, -1, -1, right_color);

            xoff += bounds_right.w;

            draw_text(cr, 
                xoff + 8 * dpi, 
                c->real_bounds.y + c->real_bounds.h * .5 - bounds_text.h * .5, 
                text, size * dpi, true, mylar_font, -1, -1, color);
            
            if (c->state.mouse_hovering) {
                if (c->state.mouse_pressing) {
                    set_argb(cr, RGBA(0, 0, 0, .4));
                } else {
                    set_argb(cr, RGBA(0, 0, 0, 1));
                }
                drawRoundedRect(cr, c->real_bounds.x, c->real_bounds.y, c->real_bounds.w, c->real_bounds.h, 8 * dpi, std::round(1.0 * dpi));
                cairo_stroke(cr);
            }
        };
    };

    auto flowpane = [](Container *parent, std::vector<std::string> options, std::function<void (std::string)> on_clicked, std::function<MylarWindow * (Container *)> get_window) {
        auto label = parent->child(::absolute, FILL_SPACE, FILL_SPACE);
        static float pad = 14;
        static float size = 12;
        static float rowh = 20;
        static RGBA color = RGBA(0, 0, 0, 1);
        for (int i = 0; i < options.size(); i++) {
            auto option = options[i];
            auto option_label = label->child(FILL_SPACE, FILL_SPACE);
            option_label->parent_bounds_limit_input_bounds = false;
            option_label->name = option;
            option_label->when_clicked = [on_clicked](Container *root, Container *c) {
                on_clicked(c->name);
            };
            option_label->when_paint = [get_window, option](Container *root, Container *c) {
                auto dock = (Dock *) root->user_data;
                auto mylar = get_window(root);
                auto dpi = mylar->raw_window->dpi;
                auto cr = mylar->raw_window->cr;
                if (c->state.mouse_pressing || c->state.mouse_hovering) {
                    if (c->state.mouse_pressing) {
                        set_argb(cr, RGBA(.6, .6, .6, 1));
                    } else {
                        set_argb(cr, RGBA(.8, .8, .8, 1));
                    }
                    drawRoundedRect(cr, c->real_bounds.x, c->real_bounds.y, c->real_bounds.w, c->real_bounds.h, 4 * dpi, std::round(1.0 * dpi));
                    cairo_fill(cr);
                }
                set_argb(cr, RGBA(.6, .6, .6, 1));
                drawRoundedRect(cr, c->real_bounds.x, c->real_bounds.y, c->real_bounds.w, c->real_bounds.h, 4 * dpi, std::round(1.0 * dpi));
                cairo_stroke(cr);


                auto b = draw_text(cr, 0, 0, option, size * dpi, false, mylar_font, -1, -1, color);
                draw_text(cr, 
                    center_x(c, b.w), center_y(c, b.h), 
                    option, size * dpi, true, mylar_font, -1, -1, color);
            };
        }
        
        label->pre_layout = [get_window, options](Container *root, Container *c, const Bounds &b) {
            auto dock = (Dock *) root->user_data;
            auto mylar = get_window(root);
            auto dpi = mylar->raw_window->dpi;
            auto cr = mylar->raw_window->cr;
            int lines = 1;
            float xoff = c->real_bounds.x;
            float layout_yoff = c->real_bounds.y;
            for (int i = 0; i < options.size(); i++) {
                auto bounds_text = draw_text(cr, 0, 0, options[i], size * dpi, false, mylar_font, -1, -1, color);
                if ((xoff + bounds_text.w) > (b.w + c->real_bounds.x)) {
                    xoff = c->real_bounds.x;
                    lines++;
                    layout_yoff += rowh * dpi;  
                }
                
                auto option_label = c->children[i];
                float visual_y = layout_yoff + (rowh * .5) * dpi - bounds_text.h * .5;
                visual_y -= 0 * dpi;
                float visual_x = xoff += 10 * dpi;
                option_label->real_bounds = Bounds(visual_x, visual_y, bounds_text.w, bounds_text.h);
                option_label->real_bounds.grow(4 * dpi);
                xoff += bounds_text.w + pad * dpi;
            }
            c->wanted_bounds.h = rowh * dpi * ((float) lines);
            c->wanted_bounds.w = FILL_SPACE;
        };
    };

    dock->app->update_monitor_information();
    
    static std::vector<std::string> mons;
    mons.clear();
    //mons.push_back("All");
    auto ours = dock->creation_settings.monitor_name;

    for (auto m : hypriso->all_monitors()) {
        if (m.from != ours) {
            mons.push_back(m.from);
        }
    }

    flowpane(parent, mons, [ours](std::string option) {
        main_thread([ours, option] {
            monitor_rule_mirror_from_to_toggle(ours, option);
        });
    }, [](Container *c) { return ((Dock *) c->user_data)->projection; });

    make_self_sizing_label(parent, "Disable other screens", 12, [](Dock *d) {
        return d->projection;
    });

    flowpane(parent, mons, [ours](std::string option) {
        main_thread([ours, option] {
            monitor_rule_disable_toggle(ours, option);
        });
    }, [](Container *c) { return ((Dock *) c->user_data)->projection; });
}


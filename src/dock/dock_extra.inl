static void fill_extra_container(Container *root) {
    static auto get_window = [](Dock *dock) {
        return dock->extra;
    };
    root->type = ::vbox;
    root->when_paint = [](Container *root, Container *c) {
        auto dock = (Dock *) root->user_data;
        auto window = get_window(dock);
        auto cr = window->raw_window->cr;
        set_argb(cr, {1, 1, 1, 1});
        drawRoundedRect(cr, c->real_bounds.x, c->real_bounds.y, c->real_bounds.w, c->real_bounds.h, 10 * dock->extra->raw_window->dpi, 1.0);
        cairo_fill(cr);
        set_argb(cr, border_color);
        drawRoundedRect(cr, c->real_bounds.x, c->real_bounds.y, c->real_bounds.w, c->real_bounds.h, 10 * dock->extra->raw_window->dpi, 1.0);
        cairo_stroke(cr);
    };
    auto top = root->child(::hbox, FILL_SPACE, FILL_SPACE);
    static auto button_pad = 10;
    for (int i = 0; i < 3; i++) {
        auto l = top->child(FILL_SPACE, FILL_SPACE);
        l->pre_layout = [](Container *root, Container *c, const Bounds &b) {
            auto dock = (Dock *) root->user_data;
            auto window = get_window(dock);
            auto s = window->raw_window->dpi;
            c->wanted_pad = Bounds(button_pad * s, button_pad * s, button_pad * s, button_pad * s);
        };
        static std::vector<std::string> argsa = {"\uE702", "\uE708", "\uE087"};
        auto b = l->child(FILL_SPACE, FILL_SPACE);
        if (i == 1) {
            b->when_clicked = [](Container *root, Container *c) {
               if (nightlight_on)  {
                   system("killall hyprsunset");
               } else {
                   std::thread t([]() {
                       system("hyprsunset -t 5000");
                   });
                   t.detach();
               }
               nightlight_on = !nightlight_on;
            };
        }
        if (i == 0) {
            b->when_clicked = [](Container *root, Container *c) {
                auto dock = (Dock *) root->user_data;
                auto window = get_window(dock);
                windowing::close_window(window->raw_window);

                auto mylar = dock->window;
                auto dpi = mylar->raw_window->dpi;

                struct Reformed {
                    MylarWindow *window = nullptr;
                    Dock *dock = nullptr;
                    float dpi = 1.0;
                };
                auto reformed = new Reformed;
                reformed->dpi = dpi;
                reformed->window = mylar;
                reformed->dock = dock;

                windowing::timer(dock->app, 40, [](void *data) {
                    auto r = (Reformed *) data;
                    auto mylar = r->window;
                    auto dock = r->dock;
                    auto dpi = r->dpi;
                    auto c = container_by_name("extra", dock->window->root);
                    
                    RawWindowSettings settings = make_icon_anchored_popup_settings(c, dpi, volume_popup_w, volume_popup_w * 1.6);

                    dock->bluetooth = open_mylar_popup(mylar, settings);
                    if (!dock->bluetooth)
                        return;
                    dock->bluetooth->root->on_closed = [](Container* root) {
                        auto dock = (Dock*)root->user_data;
                        dock->bluetooth = nullptr;
                    };
                    dock->bluetooth->root->user_data = dock;
                    dock->bluetooth->root->wanted_bounds.w = FILL_SPACE;
                    dock->bluetooth->root->wanted_bounds.h = FILL_SPACE;
                    fill_bluetooth_container(dock);
                    windowing::redraw(dock->bluetooth->raw_window);
                }, reformed);
            };
        }
        b->when_paint = [i](Container *root, Container *c) {
            auto dock = (Dock *) root->user_data;
            auto window = get_window(dock);
            auto cr = window->raw_window->cr;
            if (c->state.mouse_pressing || c->state.mouse_hovering) {
                if (c->state.mouse_pressing) {
                    set_argb(cr, {0, 0, 0, .2});
                } else if (c->state.mouse_hovering) {
                    set_argb(cr, {0, 0, 0, .1});
                }
                drawRoundedRect(cr, c->real_bounds.x, c->real_bounds.y, c->real_bounds.w, c->real_bounds.h, 10 * dock->extra->raw_window->dpi, 1.0);
                cairo_fill(cr);
            }
            set_argb(cr, {0, 0, 0, .2});
            drawRoundedRect(cr, c->real_bounds.x, c->real_bounds.y, c->real_bounds.w, c->real_bounds.h, 10 * dock->extra->raw_window->dpi, 1.0);
            cairo_stroke(cr);
//static Bounds draw_text(cairo_t *cr, int x, int y, std::string text, int size = 10, bool draw = true, std::string font = mylar_font, int wrap = -1, int h = -1, RGBA color = {1, 1, 1, 1}) {

            auto b = draw_text(cr, 0, 0, argsa[i], 36, false, "Segoe Fluent Icons", -1, -1, {0, 0, 0, 1});
            draw_text(cr, 
                center_x(c, b.w), center_y(c, b.h),
                argsa[i], 36, true, "Segoe Fluent Icons", -1, -1, {0, 0, 0, 1});
        };
    } 

    auto middle = root->child(::hbox, FILL_SPACE, FILL_SPACE);
    for (int i = 0; i < 3; i++) {
        auto l = middle->child(FILL_SPACE, FILL_SPACE);
        l->pre_layout = [](Container *root, Container *c, const Bounds &b) {
            auto dock = (Dock *) root->user_data;
            auto window = get_window(dock);
            auto s = window->raw_window->dpi;
            c->wanted_pad = Bounds(button_pad * s, button_pad * s, button_pad * s, button_pad * s);
        };
        static std::vector<std::string> argsa = {"\uEBC6", "\uE722", "\uF140"};
        auto b = l->child(FILL_SPACE, FILL_SPACE);
        if (i == 1) {
            b->when_clicked = [](Container *root, Container *c) {
                auto dock = (Dock *) root->user_data;
                auto window = get_window(dock);
                windowing::close_window(window->raw_window);

                windowing::timer(dock->app, 100, [](void *data) {
                    system("hyprctl dispatch \"hl.plugin.mylar.screenshot_tool()\"");
                }, nullptr);
            };
        }
        if (i == 2) {
            b->when_clicked = [](Container *root, Container *c) {
                auto dock = (Dock *) root->user_data;
                auto window = get_window(dock);
                windowing::close_window(window->raw_window);
                
                system("hyprctl kill");
            };
        }
        if (i == 0) {
            b->when_clicked = [](Container *root, Container *c) {
                auto dock = (Dock *) root->user_data;
                auto window = get_window(dock);
                windowing::close_window(window->raw_window);

                auto mylar = dock->window;
                auto dpi = mylar->raw_window->dpi;

                struct Reformed {
                    MylarWindow *window = nullptr;
                    Dock *dock = nullptr;
                    float dpi = 1.0;
                };
                auto reformed = new Reformed;
                reformed->dpi = dpi;
                reformed->window = mylar;
                reformed->dock = dock;

                windowing::timer(dock->app, 40, [](void *data) {
                    main_thread([data]() {
                        auto r = (Reformed *) data;
                        auto mylar = r->window;
                        auto dock = r->dock;
                        auto dpi = r->dpi;
                        auto c = container_by_name("extra", dock->window->root);
                        
                        RawWindowSettings settings = make_icon_anchored_popup_settings(c, dpi, volume_popup_w, volume_popup_w * .5);

                        dock->projection = open_mylar_popup(mylar, settings);
                        if (!dock->projection)
                            return;
                        dock->projection->root->on_closed = [](Container* root) {
                            auto dock = (Dock*)root->user_data;
                            dock->projection = nullptr;
                        };
                        dock->projection->root->user_data = dock;
                        dock->projection->root->wanted_bounds.w = FILL_SPACE;
                        dock->projection->root->wanted_bounds.h = FILL_SPACE;
                        fill_projection_container(dock);
                        windowing::redraw(dock->projection->raw_window);
                    });
                }, reformed);
            };
        }
        b->when_paint = [i](Container *root, Container *c) {
            auto dock = (Dock *) root->user_data;
            auto window = get_window(dock);
            auto cr = window->raw_window->cr;
            if (c->state.mouse_pressing || c->state.mouse_hovering) {
                if (c->state.mouse_pressing) {
                    set_argb(cr, {0, 0, 0, .2});
                } else if (c->state.mouse_hovering) {
                    set_argb(cr, {0, 0, 0, .1});
                }
                drawRoundedRect(cr, c->real_bounds.x, c->real_bounds.y, c->real_bounds.w, c->real_bounds.h, 10 * dock->extra->raw_window->dpi, 1.0);
                cairo_fill(cr);
            }
            set_argb(cr, {0, 0, 0, .2});
            drawRoundedRect(cr, c->real_bounds.x, c->real_bounds.y, c->real_bounds.w, c->real_bounds.h, 10 * dock->extra->raw_window->dpi, 1.0);
            cairo_stroke(cr);

            auto b = draw_text(cr, 0, 0, argsa[i], 36, false, "Segoe Fluent Icons", -1, -1, {0, 0, 0, 1});
            draw_text(cr, 
                center_x(c, b.w), center_y(c, b.h),
                argsa[i], 36, true, "Segoe Fluent Icons", -1, -1, {0, 0, 0, 1});
        };
    }    
    
    auto bottom = root->child(FILL_SPACE, FILL_SPACE);
    bottom->pre_layout = [](Container *root, Container *c, const Bounds &b) {
        auto dock = (Dock *) root->user_data;
        auto window = get_window(dock);
        auto s = window->raw_window->dpi;
        c->wanted_bounds = Bounds(0, 0, FILL_SPACE, 60 * s);
    };
    bottom->when_paint = [](Container *root, Container *c) {
        auto dock = (Dock *) root->user_data;
        auto window = get_window(dock);
        auto cr = window->raw_window->cr;
        set_argb(cr, {0, 0, 0, .08});
        drawRoundedRect(cr, c->real_bounds.x, c->real_bounds.y, c->real_bounds.w, c->real_bounds.h, 10 * dock->extra->raw_window->dpi, 1.0);
        cairo_fill(cr);
    };
}


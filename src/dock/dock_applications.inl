static Container *make_field(Container *parent, bool only_numbers, std::string initial_value, std::function<void(std::string)> on_change) {
    auto pad = parent->child(FILL_SPACE, FILL_SPACE);
    STextEditorOptions options;
    options.multiline = false;
    options.only_numbers = only_numbers;
    options.font_size = 11;
    options.window = [](Container *root) { return static_cast<Dock *>(root->user_data)->applications->raw_window; };
    options.viewport = [](Container *root, Container *c) {
        auto dpi = static_cast<Dock *>(root->user_data)->applications->raw_window->dpi;
        auto bounds = c->real_bounds;
        bounds.x += 11 * dpi;
        bounds.w = std::max(0.0, bounds.w - 22 * dpi);
        return bounds;
    };
    options.on_change = std::move(on_change);
    setup_text_editor(pad, std::move(initial_value), std::move(options));
    pad->pre_layout = [](Container *root, Container *c, const Bounds &b) {
        auto dpi = static_cast<Dock *>(root->user_data)->applications->raw_window->dpi;
        auto bounds = measure_text_editor(root, c);
        c->wanted_bounds.w = FILL_SPACE;
        c->wanted_bounds.h = bounds.h + 11 * dpi * 2 * .8;
    };
  
    pad->when_paint = [](Container *root, Container *c) {
        auto dock = (Dock*)root->user_data;
        auto cr = dock->applications->raw_window->cr;
        auto dpi = dock->applications->raw_window->dpi;

        set_argb(cr, c->active ? accent : RGBA(.8, .8, .8, 1));
        auto b = c->real_bounds;
        drawRoundedRect(cr, b.x, b.y, b.w, b.h, 10 * dpi, 1.0 * dpi);
        cairo_stroke(cr);

        set_argb(cr, {1, 1, 1, 1});
        auto minus_border = c->real_bounds;
        minus_border.shrink(std::round(1 * dpi));
        b = minus_border;
        drawRoundedRect(cr, b.x, b.y, b.w, b.h, 10 * dpi, 1.0);
        //set_rect(cr, minus_border);
        cairo_fill(cr);
        
        paint_text_editor(root, c, RGBA(0, 0, 0, 1), accent);
    };
    return pad;
}

void scripts_load(std::vector<Script *> &temp_scripts) {
    temp_scripts.clear();

    // go through every directory in $PATH environment variable
    // add to our scripts list if the files we check are executable
    std::string paths = std::string(getenv("PATH"));

    std::replace(paths.begin(), paths.end(), ':', ' ');

    std::stringstream ss(paths);
    std::string string_path;
    while (ss >> string_path) {
        if (auto *dir = opendir(string_path.c_str())) {
            struct dirent *dp;
            while ((dp = readdir(dir)) != NULL) {
                struct stat st, ln;

                // This is what determines if its an executable and its from dmenu and
                // its not good. oh well
                static int flag[26];
#define FLAG(x) (flag[(x) - 'a'])

                const char *path = string_path.c_str();
                if ((!stat(path, &st) &&
                     (FLAG('a') || dp->d_name[0] != '.') /* hidden files      */
                     && (!FLAG('b') || S_ISBLK(st.st_mode)) /* block special     */
                     && (!FLAG('c') || S_ISCHR(st.st_mode)) /* character special */
                     && (!FLAG('d') || S_ISDIR(st.st_mode)) /* directory         */
                     && (!FLAG('e') || access(path, F_OK) == 0) /* exists            */
                     && (!FLAG('f') || S_ISREG(st.st_mode)) /* regular file      */
                     && (!FLAG('g') || st.st_mode & S_ISGID) /* set-group-id flag */
                     && (!FLAG('h') ||
                         (!lstat(path, &ln) && S_ISLNK(ln.st_mode))) /* symbolic link */
                     && (!FLAG('p') || S_ISFIFO(st.st_mode)) /* named pipe        */
                     && (!FLAG('r') || access(path, R_OK) == 0) /* readable          */
                     && (!FLAG('s') || st.st_size > 0) /* not empty         */
                     && (!FLAG('u') || st.st_mode & S_ISUID) /* set-user-id flag  */
                     && (!FLAG('w') || access(path, W_OK) == 0) /* writable          */
                     && (!FLAG('x') || access(path, X_OK) == 0)) !=
                    FLAG('v')) {
                    /* executable        */

                    if (!(FLAG('q'))) {
                        bool already_have_this_script = false;
                        std::string name = std::string(dp->d_name);
                        for (auto *script: temp_scripts) {
                            if (script->name == name) {
                                already_have_this_script = true;
                                break;
                            }
                        }
                        if (already_have_this_script)
                            continue;

                        auto *script = new Script();
                        script->name = name;
                        script->lowercase_name = script->name;
                        std::transform(script->lowercase_name.begin(),
                                       script->lowercase_name.end(),
                                       script->lowercase_name.begin(), ::tolower);

                        script->full_path = path;
                        script->full_path += "/" + name;
                        script->path = path;
                        if (!script->path.empty()) {
                            if (script->path[script->path.length() - 1] == '/' ||
                                script->path[script->path.length() - 1] == '\\') {
                                script->path.erase(script->path.begin() +
                                                   (script->path.length() - 1));
                            }
                        }

                        temp_scripts.push_back(script);
                    }
                }
            }
            closedir(dir);
        }
    }

    if (temp_scripts.empty())
        return;
}

static void fill_applications_container(Container *root) {
    root->when_paint = [](Container *root, Container *c) {
        auto dock = (Dock *) root->user_data;
        auto cr = dock->applications->raw_window->cr;
        paint_popup_background(cr, c->real_bounds, dock->applications->raw_window->dpi);
    };

    static const float pad_amount = 16;
    auto padded = root->child(FILL_SPACE, FILL_SPACE);
    padded->pre_layout = [](Container *root, Container *c, const Bounds &b) {
        auto dock = (Dock *) root->user_data;
        auto dpi = dock->applications->raw_window->dpi;
        c->wanted_pad = Bounds(pad_amount, pad_amount, pad_amount, pad_amount).scale(dpi);
        c->spacing = 8 * dpi;
    };

    static std::string field_text;
    field_text = "";
    static int active_option = 0;
    active_option = 0;
    
    auto field = make_field(padded, false, "", [](std::string text) {
        field_text = std::move(text);
        active_option = 0;
    });
    auto edit_key = std::move(field->when_key_event);
    field->when_key_event = [edit_key](Container *root, Container *c, int key, bool pressed, xkb_keysym_t sym,
                                     int mods, bool is_text, std::string text) {
        if (!c->active)
            return;
        edit_key(root, c, key, pressed, sym, mods, is_text, text);
        if (pressed) {
            auto dock = (Dock *) root->user_data;
            if (sym == XKB_KEY_Escape) {
                windowing::close_window(dock->applications->raw_window);
                active_option = 0;
            } else if (sym == XKB_KEY_Return || sym == XKB_KEY_KP_Enter) {
                if (auto o_ = container_by_name("scroll_content", root)) {
                    auto o = ((ScrollContainer *) o_)->content;
                    bool none_matched = true;
                    for (auto ch: o->children) {
                        if (ch->exists) {
                            auto s = (Script *) ch->user_data;
                            if (s->selected) {
                                launch_command(s->full_path);
                                active_option = 0;
                                none_matched = false;
                            }
                        }
                    }
                    if (none_matched) {
                        if (!field_text.empty())
                            launch_command(field_text);
                    }
                }
                windowing::close_window(dock->applications->raw_window);
            } else if (sym == XKB_KEY_Up) {
                active_option--;
            } else if (sym == XKB_KEY_Down) {
                active_option++;
            }
        }
    };

    auto scroll_parent = padded->child(FILL_SPACE, FILL_SPACE);
    auto scroll = make_newscrollpane_as_child(scroll_parent, ScrollPaneSettings(1.0), [](Container *root) {
        auto dock = ((Dock *) root->user_data);
        return DrawContext({dock->applications->raw_window->cr, dock->applications->raw_window->dpi, [dock]() {
            windowing::redraw(dock->applications->raw_window);
        }});
    });
    auto scroll_content = scroll->content;
    scroll->name = "scroll_content";
    scroll->pre_layout = [](Container *root, Container *c_, const Bounds &b) {
        auto scroll = ((ScrollContainer *) c_);
        auto data = (ScrollData *) scroll->user_data;
        auto c = scroll->content;
        if (data->func) {
            DrawContext ctx = data->func(root);
            int amount = std::round(12.0f * (ctx.dpi));
            scroll->right->children[0]->wanted_bounds.h = amount;
            scroll->right->children[2]->wanted_bounds.h = amount;
            scroll->bottom->children[0]->wanted_bounds.w = amount;
            scroll->bottom->children[2]->wanted_bounds.w = amount;
            scroll->settings.right_width = amount; 
            scroll->settings.right_arrow_height = amount; 
            scroll->settings.bottom_height = amount; 
            scroll->settings.bottom_arrow_width = amount; 
        }
        auto dock = (Dock *) root->user_data;
        auto dpi = dock->applications->raw_window->dpi;
        c->spacing = 5 * dpi;
        int alive_count = 0;
        for (auto ch: c->children) {
            auto s = (Script *) ch->user_data;
            s->selected = false;
            auto name = s->name;
            auto full = s->full_path;
            ch->exists = name.starts_with(field_text);
            if (ch->exists)
                alive_count++;
        }
        if (alive_count != 0) {
            if (active_option < 0)
                active_option = 0;
            if (active_option > alive_count - 1)
                active_option = alive_count - 1;
        }
        int index_of_alive_child = 0;
        for (auto ch: c->children) {
            auto s = (Script *) ch->user_data;
            if (ch->exists) {
                if (index_of_alive_child == active_option)
                    s->selected = true;
                index_of_alive_child++;
            }
        }
    };    

    for (auto s: scripts) {
        auto o = scroll_content->child(FILL_SPACE, FILL_SPACE);
        o->user_data = s;
        o->pre_layout = [](Container *root, Container *c, const Bounds &b) {
            auto dock = (Dock *) root->user_data;
            auto dpi = dock->applications->raw_window->dpi;
            c->wanted_bounds.h = 36 * dpi;
        };
        o->when_paint = [](Container *root, Container *c) {
            auto dock = (Dock *) root->user_data;
            auto s = (Script *) c->user_data;
            auto name = s->name;
            auto full = s->full_path;
            auto dpi = dock->applications->raw_window->dpi;
            auto cr = dock->applications->raw_window->cr;
            if (s->selected) {
                set_argb(cr, accent);
            } else {
                set_argb(cr, {0, 0, 0, .15});
            }
            drawRoundedRect(cr, c->real_bounds.x, c->real_bounds.y, c->real_bounds.w,
                            c->real_bounds.h,
                            10 * dock->applications->raw_window->dpi,
                            std::round(1.0 * dpi));
            cairo_stroke(cr);

            // static Bounds draw_text(cairo_t *cr, int x, int y, std::string text,
            // int size, bool draw, std::string font, int wrap, int h, RGBA color,
            // bool bold, int align = 0) {
            std::string text = fz("{} ({})", name, full);
            auto b = draw_text(cr, c->real_bounds.x, c->real_bounds.y, text, 13 * dpi,
                               false, set->font, -1, -1, RGBA(0, 0, 0, 1), false, 0);
            draw_text(cr, std::round(c->real_bounds.x + (c->real_bounds.h - b.h) * .5),
                      std::round(center_y(c, b.h)), text, std::round(13 * dpi), true, set->font, -1, -1,
                      RGBA(0, 0, 0, 1), false, 0);
        };
        o->when_clicked = [](Container *root, Container *c) {
            auto s = (Script *) c->user_data;
            auto full = s->full_path;
            launch_command(full);
        };
    }

    set_active(root, {field}, root, true, false);

    auto bottom = padded->child(::hbox, FILL_SPACE, 32);
    bottom->alignment = ALIGN_RIGHT;
    bottom->pre_layout = [](Container *root, Container *c, const Bounds &b) {
        auto dock = (Dock *) root->user_data;
        auto dpi = dock->applications->raw_window->dpi;
        c->wanted_bounds.h = 32 * dpi;
        c->wanted_pad = Bounds(pad_amount, pad_amount, pad_amount, pad_amount).scale(dpi);
        c->spacing = 8 * dpi;
        for (auto ch : c->children) {
            ch->wanted_bounds.w = c->wanted_bounds.h;
            ch->wanted_bounds.h = c->wanted_bounds.h;
        }
    };
    static std::vector<std::string> icon = { "\uE713", "\uE7E8" };
    for (int i = 0; i < 2; i++) {
        auto b = bottom->child(32, 32);
        b->when_paint = [i](Container *root, Container *c) {
            auto dock = (Dock *) root->user_data;
            auto dpi = dock->applications->raw_window->dpi;
            auto cr = dock->applications->raw_window->cr;
            if (c->state.mouse_pressing) {
                set_argb(cr, {.5, .5, .5, 1});
                set_rect(cr, c->real_bounds);
                cairo_fill(cr);
            } else if (c->state.mouse_hovering) {
                set_argb(cr, {.65, .65, .65, 1});
                set_rect(cr, c->real_bounds);
                cairo_fill(cr);
            }

            
            auto b = draw_text(cr, 0, 0, icon[i], 12 * dpi, false, icon_font);
            draw_text(cr, 
                c->real_bounds.x + c->real_bounds.w * .5 - b.w * .5, 
                c->real_bounds.y + c->real_bounds.h * .5 - b.h * .5, icon[i], 12 * dpi, true, icon_font, -1, -1, {0, 0, 0, 1});
        };
    }
}

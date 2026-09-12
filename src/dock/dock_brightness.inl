static void fill_brightness_container(Dock *dock) {
    dock->brightness->root->when_paint = [](Container *root, Container *c) {
        auto dock = (Dock *) root->user_data;
        auto cr = dock->brightness->raw_window->cr;
        set_argb(cr, {1, 1, 1, 1});
        drawRoundedRect(cr, c->real_bounds.x, c->real_bounds.y, c->real_bounds.w, c->real_bounds.h, 10 * dock->brightness->raw_window->dpi, 1.0);
        cairo_fill(cr);
        set_argb(cr, border_color);
        drawRoundedRect(cr, c->real_bounds.x, c->real_bounds.y, c->real_bounds.w, c->real_bounds.h, 10 * dock->brightness->raw_window->dpi, 1.0);
        cairo_stroke(cr);
    };
    auto parent = dock->brightness->root->child(::vbox, FILL_SPACE, FILL_SPACE);
    parent->pre_layout = [](Container *root, Container *c, const Bounds &b) {
        auto dock = (Dock *) root->user_data;
        auto s = dock->brightness->raw_window->dpi;
        c->wanted_pad = Bounds(8 * s, 8 * s, 8 * s, 8 * s);
    };
    
    //make_self_sizing_label(parent, "Screen brightness", 12, [](Dock *d) { return d->brightness; });
    
    //make_vert_space(parent, 4, [](Dock *d) { return d->brightness; });

    struct SliderInfo : UserData {
        float amount = 0;
    };
    auto slider = make_self_sizing_slider(parent, [](Container *c) {
        auto slider = c->parent;
        auto slider_info = (SliderInfo *) slider->user_data;
        return "\uE706";
    }, [](Container *c) {
        auto slider = c->parent;
        auto slider_info = (SliderInfo *) slider->user_data;
        return std::to_string((int) std::round(slider_info->amount * 100));
    }, [](Container *c, float amount) {
        auto slider = c->parent;
        auto slider_info = (SliderInfo *) slider->user_data;
        slider_info->amount = std::max(0.0f, std::min(1.0f, amount));
        set_brightness(std::round(slider_info->amount * 100.0f));
    }, [](Container *c) {
        auto slider = c->parent;
        auto slider_info = (SliderInfo *) slider->user_data;
        return slider_info->amount;
    },
    [](Dock *d) { return d->brightness; });
    auto slider_info = new SliderInfo;
    slider_info->amount = get_brightness() / 100.0f;
    slider->user_data = slider_info;
}


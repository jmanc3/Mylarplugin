static void fill_bluetooth_container(Dock *dock) {
    dock->bluetooth->root->when_paint = [](Container *root, Container *c) {
        auto dock = (Dock *) root->user_data;
        auto cr = dock->bluetooth->raw_window->cr;
        set_argb(cr, {1, 1, 1, 1});
        drawRoundedRect(cr, c->real_bounds.x, c->real_bounds.y, c->real_bounds.w, c->real_bounds.h, 10 * dock->bluetooth->raw_window->dpi, 1.0);
        cairo_fill(cr);
        set_argb(cr, border_color);
        drawRoundedRect(cr, c->real_bounds.x, c->real_bounds.y, c->real_bounds.w, c->real_bounds.h, 10 * dock->bluetooth->raw_window->dpi, 1.0);
        cairo_stroke(cr);
    };
}

static void fill_bluetooth_container(Dock *dock) {
    dock->bluetooth->root->when_paint = [](Container *root, Container *c) {
        auto dock = (Dock *) root->user_data;
        auto cr = dock->bluetooth->raw_window->cr;
        paint_popup_background(cr, c->real_bounds, dock->bluetooth->raw_window->dpi);
    };
}

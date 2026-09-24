static void fill_wifi_container(Dock *dock) {
    dock->wifi->root->when_paint = [](Container *root, Container *c) {
        auto dock = (Dock *) root->user_data;
        auto cr = dock->wifi->raw_window->cr;
        paint_popup_background(cr, c->real_bounds, dock->wifi->raw_window->dpi);
    };
}


#ifndef overview_h_INCLUDED
#define overview_h_INCLUDED

namespace overview {
    void open(int monitor);
    void close(bool focus = true);
    void instant_close();
    void click(int id, int button, int state, float x, float y);

    bool is_showing();
};

#endif // overview_h_INCLUDED

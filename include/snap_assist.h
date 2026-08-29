#ifndef snap_assist_h_INCLUDED
#define snap_assist_h_INCLUDED

namespace snap_assist {
    void open(int monitor, int cid);
    void close(bool force = false);
    void click(int id, int button, int state, float x, float y);

    void fix_order();
    bool is_showing();
};

#endif // snap_assist_h_INCLUDED

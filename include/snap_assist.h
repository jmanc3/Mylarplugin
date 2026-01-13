#ifndef snap_assist_h_INCLUDED
#define snap_assist_h_INCLUDED

namespace snap_assist {
    void open(int monitor, int cid);
    void close();
    void click(int id, int button, int state, float x, float y);

    void fix_order();
};

#endif // snap_assist_h_INCLUDED

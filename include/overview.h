#ifndef overview_h_INCLUDED
#define overview_h_INCLUDED

namespace overview {
    void open(int monitor);
    void close(bool focus = true);
    void instant_close();
    void click(int id, int button, int state, float x, float y);

    // When screenshoting a workspace, we need to fake paint the final output because the actual overview has too much state
    // in animating in and out and draggging that we don't want to deal with recreating every time we simply want a screenshot
    // of a certain workspace.
    void fake_paint(int id);
    
    void should_draw(bool state);

    bool is_showing();
};

#endif // overview_h_INCLUDED

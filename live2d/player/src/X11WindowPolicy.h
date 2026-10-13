#pragma once
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <cstring>

namespace x11pet {
inline bool Supports(Display* dpy, Atom atom) {
    Atom type = None;
    int format = 0;
    unsigned long count = 0, after = 0;
    unsigned char* data = NULL;
    bool supported = false;
    if (XGetWindowProperty(dpy, DefaultRootWindow(dpy), XInternAtom(dpy, "_NET_SUPPORTED", False),
                           0, 4096, False, XA_ATOM, &type, &format, &count, &after, &data) == Success
        && type == XA_ATOM && format == 32 && data != NULL) {
        const Atom* atoms = (const Atom*)data;
        for (unsigned long n = 0; n < count; ++n) if (atoms[n] == atom) supported = true;
    }
    if (data != NULL) XFree(data);
    return supported;
}
// GNOME constrains NORMAL/UTILITY geometry even for _NET_MOVERESIZE_WINDOW.
// A desktop widget needs an unconstrained type in both layers: DOCK when in front,
// DESKTOP when behind applications. It never publishes struts or reserves workarea.
inline void ModelType(Display* dpy, Window window, bool top) {
    const Atom type = XInternAtom(dpy, top ? "_NET_WM_WINDOW_TYPE_DOCK" : "_NET_WM_WINDOW_TYPE_DESKTOP", False);
    XChangeProperty(dpy, window, XInternAtom(dpy, "_NET_WM_WINDOW_TYPE", False), XA_ATOM, 32,
                    PropModeReplace, (const unsigned char*)&type, 1);
}
inline void RestorePosition(Display* dpy, Window window, int x, int y) {
    XMoveWindow(dpy, window, x, y);
    XFlush(dpy);
}
inline void NoFocusOnMap(Display* dpy, Window window) {
    XWMHints* existing = XGetWMHints(dpy, window);
    XWMHints hints;
    std::memset(&hints, 0, sizeof(hints));
    if (existing != NULL) { hints = *existing; XFree(existing); }
    hints.flags |= InputHint;
    hints.flags &= ~XUrgencyHint;
    hints.input = False;
    XSetWMHints(dpy, window, &hints);
    const unsigned long time = 0;
    XChangeProperty(dpy, window, XInternAtom(dpy, "_NET_WM_USER_TIME", False), XA_CARDINAL, 32,
                    PropModeReplace, (const unsigned char*)&time, 1);
}
// A bubble is a passive tooltip, not an application window belonging in Dock overlap/focus logic.
inline void BubbleHints(Display* dpy, Window window, Window model, bool top) {
    NoFocusOnMap(dpy, window);
    const Atom type = XInternAtom(dpy, top ? "_NET_WM_WINDOW_TYPE_TOOLTIP" : "_NET_WM_WINDOW_TYPE_DESKTOP", False);
    XChangeProperty(dpy, window, XInternAtom(dpy, "_NET_WM_WINDOW_TYPE", False), XA_ATOM, 32,
                    PropModeReplace, (const unsigned char*)&type, 1);
    const char cls[] = "l2dpet-bubble\0l2dpet-bubble";
    XChangeProperty(dpy, window, XInternAtom(dpy, "WM_CLASS", False), XA_STRING, 8,
                    PropModeReplace, (const unsigned char*)cls, sizeof(cls));
    XSetTransientForHint(dpy, window, model);
}
}

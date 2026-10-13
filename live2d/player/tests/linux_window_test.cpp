// Integration test: run only in an isolated X11 session with an EWMH window manager.
#include <bits/stdc++.h>
#define private public
#include "../src/PortX11Pet.cpp"
#undef private
#include <cassert>

static void Settle(PortX11Pet& pet) {
    for (int i=0; i<40; ++i) { pet.PumpEvents(); usleep(20000); }
}
static long Property(Display* dpy, Window window, const char* key) {
    Atom type; int format; unsigned long count, left; unsigned char* data=NULL;
    assert(XGetWindowProperty(dpy,window,XInternAtom(dpy,key,False),0,16,False,AnyPropertyType,
                              &type,&format,&count,&left,&data)==Success);
    const long value=count && format==32 ? ((long*)data)[0] : -1;
    if(data) XFree(data);
    return value;
}
static bool IsBelow(Display* dpy, Window lower, Window upper) {
    Atom type; int format; unsigned long count,left; unsigned char* data=NULL;
    XGetWindowProperty(dpy,DefaultRootWindow(dpy),XInternAtom(dpy,"_NET_CLIENT_LIST_STACKING",False),
                       0,4096,False,XA_WINDOW,&type,&format,&count,&left,&data);
    long lo=-1,hi=-1;
    if(data && format==32) for(unsigned long i=0;i<count;++i) {
        if(((Window*)data)[i]==lower) lo=i;
        if(((Window*)data)[i]==upper) hi=i;
    }
    if(data) XFree(data);
    return lo>=0 && hi>=0 && lo<hi;
}
int main() {
    Display* dpy=XOpenDisplay(NULL); assert(dpy);
    for(int n=0;n<150 && !x11pet::Supports(dpy,XInternAtom(dpy,"_NET_MOVERESIZE_WINDOW",False));++n) usleep(100000);
    assert(x11pet::Supports(dpy,XInternAtom(dpy,"_NET_MOVERESIZE_WINDOW",False)));
    const Window root=DefaultRootWindow(dpy);
    const int sh=DisplayHeight(dpy,DefaultScreen(dpy));
    const Window settings=XCreateSimpleWindow(dpy,root,100,100,300,180,0,0,0);
    XStoreName(dpy,settings,"l2dpet-test-settings"); XMapWindow(dpy,settings); XSync(dpy,False); usleep(800000);
    XSetInputFocus(dpy,settings,RevertToPointerRoot,CurrentTime); XSync(dpy,False);
    for(int restart=0;restart<2;++restart) {
        PortX11Pet pet;
        pet._uiTried=true; // No real UI child or user configuration in this test.
        assert(pet.Initialize(PortConfig{100,100,60,NULL}));
        pet.SetSavedPlacement(-40,-50,true);
        pet.SetZoom(1);
        Settle(pet);
        assert(pet._winX==-40 && pet._winY==sh-50);
        pet.ShowBubble("test bubble",NULL,0,10);
        pet.UpdateBubble(); Settle(pet);
        Window focus; int revert; XGetInputFocus(dpy,&focus,&revert);
        assert(focus==settings);
        assert(Property(dpy,pet._bw,"_NET_WM_USER_TIME")==0);
        assert(Property(dpy,pet._bw,"_NET_WM_WINDOW_TYPE")==long(XInternAtom(dpy,"_NET_WM_WINDOW_TYPE_TOOLTIP",False)));
        pet.SetPanelState("{\"config\":{\"top\":0}}"); Settle(pet);
        assert(pet._winX==-40 && pet._winY==sh-50);
        assert(IsBelow(dpy,pet._win,settings) && IsBelow(dpy,pet._bw,settings));
        pet.HideBubble(); pet.ShowBubble("second bubble",NULL,0,10); pet.UpdateBubble(); Settle(pet);
        XGetInputFocus(dpy,&focus,&revert); assert(focus==settings);
        assert(IsBelow(dpy,pet._bw,settings));
        pet.SetWindowVisible(false); pet.SetWindowVisible(true); Settle(pet);
        assert(pet._winX==-40 && pet._winY==sh-50);
        // Programmatic drag requests must also remain unconstrained in the background layer.
        XMoveWindow(pet._dpy,pet._win,-50,sh-40); XFlush(pet._dpy); Settle(pet);
        assert(pet._winX==-50 && pet._winY==sh-40);
        pet.SavePlacement(); assert(pet._portCmds.back().find("\"pos_b\":-60")!=std::string::npos);
    }
    {
        PortX11Pet pet; pet._uiTried=true;
        assert(pet.Initialize(PortConfig{100,100,60,NULL}));
        pet.SetZoom(1); Settle(pet);
        pet.SetSavedPlacement(-40,-50,true); Settle(pet);
        assert(pet._winX==-40 && pet._winY==sh-50); // late config delivery
    }
    XDestroyWindow(dpy,settings); XCloseDisplay(dpy);
    puts("PASS real X11 port: restart/late restore, half-offscreen drag, hidden/show, passive bubbles, top-off stacking");
}

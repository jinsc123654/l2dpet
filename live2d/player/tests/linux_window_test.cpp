// Integration test: run only in an isolated X11 session with an EWMH window manager.
#include <bits/stdc++.h>
#define private public
#include "../src/PortX11Pet.cpp"
#undef private
#include <cassert>

static void Settle(PortX11Pet& pet) {
    for (int i=0; i<40; ++i) { pet.PumpEvents(); usleep(20000); }
}
static bool Mapped(PortX11Pet& pet) { return pet._winMapped; }
// 等窗口真的变成 viewable：XMapWindow 只是**请求**，重定向给 WM 之后要等它把窗口映射出来。
// 固定 sleep 在空闲机器上够用、在负载高的机器（CI）上就会让后面的 XSetInputFocus 吃 BadMatch。
static void WaitViewable(Display* dpy, Window w, int maxMs=12000) {
    for(int waited=0; waited<maxMs; waited+=20) {
        XWindowAttributes attrs;
        if(XGetWindowAttributes(dpy,w,&attrs) && attrs.map_state==IsViewable) return;
        usleep(20000);
    }
    fprintf(stderr,"[test] 窗口 0x%lx 12s 内没变成 viewable（WM 没管它？）\n",(unsigned long)w);
    exit(1);
}
// 等条件成立（毫秒上限）：CI 的 GNOME 会话比本地慢得多，固定次数的 Settle 不够用。
static bool WaitFor(PortX11Pet& pet, bool (*cond)(PortX11Pet&), int maxMs=12000) {
    for(int waited=0; waited<maxMs; waited+=20) { pet.PumpEvents(); usleep(20000); if(cond(pet)) return true; }
    return cond(pet);
}
// Xlib 默认的错误处理是**直接 exit(1)**：出错时连是哪个请求都看不到
//（CI 上踩过：气泡窗口没建出来时 XGetWindowProperty(0x0)，进程静默死掉，只剩一行 BadWindow）。
static int OnXError(Display*, XErrorEvent* err) {
    fprintf(stderr,"[test] X error: opcode=%d resource=0x%lx serial=%lu\n",
            (int)err->request_code, err->resourceid, err->serial);
    exit(70);
    return 0;   // exit 之后到不了，凑 XErrorHandler 的返回类型
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
    XSetErrorHandler(OnXError);
    Display* dpy=XOpenDisplay(NULL); assert(dpy);
    for(int n=0;n<150 && !x11pet::Supports(dpy,XInternAtom(dpy,"_NET_MOVERESIZE_WINDOW",False));++n) usleep(100000);
    assert(x11pet::Supports(dpy,XInternAtom(dpy,"_NET_MOVERESIZE_WINDOW",False)));
    const Window root=DefaultRootWindow(dpy);
    const int sh=DisplayHeight(dpy,DefaultScreen(dpy));
    const Window settings=XCreateSimpleWindow(dpy,root,100,100,300,180,0,0,0);
    XStoreName(dpy,settings,"l2dpet-test-settings"); XMapWindow(dpy,settings); XSync(dpy,False);
    WaitViewable(dpy,settings);
    XSetInputFocus(dpy,settings,RevertToPointerRoot,CurrentTime); XSync(dpy,False);
    for(int restart=0;restart<2;++restart) {
        PortX11Pet pet;
        pet._uiTried=true; // No real UI child or user configuration in this test.
        assert(pet.Initialize(PortConfig{100,100,60,NULL}));
        pet.SetSavedPlacement(-40,-50,true);
        pet.SetZoom(1);
        Settle(pet);
        assert(pet._winX==-40 && pet._winY==sh-50);
        // 气泡只在窗口**在屏幕上**时才弹（override-redirect 孤儿气泡，见 UpdateBubble 的说明）。
        // 窗口先得被 WM 映射出来：本地是瞬时的，CI 的降级会话里会晚很多 → 等，别用固定次数猜。
        if (!WaitFor(pet,Mapped)) {
            Window droot=0,dparent=0,*kids=NULL; unsigned nkids=0;
            XQueryTree(dpy,pet._win,&droot,&dparent,&kids,&nkids);
            if(kids) XFree(kids);
            fprintf(stderr,"[test] 桌宠窗口 12s 内没被映射（_win=0x%lx parent=0x%lx kids=%u）\n",
                    (unsigned long)pet._win,(unsigned long)dparent,nkids);
            return 1;
        }
        pet.ShowBubble("test bubble",NULL,0,10);
        pet.UpdateBubble(); Settle(pet);
        assert(pet._bw!=0);   // 没建出来 = ShowBubble 提前返回（窗口没映射 / Xft 打不开字体）
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

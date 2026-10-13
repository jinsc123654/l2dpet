/**
 * PortX11.cpp — 桌面 X11 端口：窗口显示 + 鼠标交互（本机开发/调试用）
 *
 * 说明：ARGB8888（0xAARRGGBB）与 X11 depth-24 真彩的像素布局在
 * little-endian 上一致（低 24 位 = RGB），可直接包装为零拷贝 XImage。
 * 编译期可选：CMake 检测到 X11 时定义 L2D_WITH_X11 才编入。
 */

#include "Port.hpp"
#include "Platform.h"
#include "l2d_config.h"

#include <cstddef> // NULL（stub 分支亦需要）

#ifdef L2D_WITH_X11

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <time.h>
#include <unistd.h>

#include <X11/Xlib.h>
#include <X11/Xutil.h>

namespace
{

class PortX11 : public IPort
{
public:
    PortX11()
        : _dpy(NULL)
        , _win(0)
        , _gc(NULL)
        , _wmDelete(0)
        , _w(480)
        , _h(800)
        , _frameNs(0)
        , _btnDown(false)
        , _quit(false)
        , _scaled(NULL)
        , _scaledW(0)
        , _scaledH(0)
    {
        memset(&_nextFrame, 0, sizeof(_nextFrame));
    }

    ~PortX11()
    {
        free(_scaled);
        if (_dpy != NULL)
        {
            if (_gc != NULL)
            {
                XFreeGC(_dpy, _gc);
            }
            if (_win != 0)
            {
                XDestroyWindow(_dpy, _win);
            }
            XCloseDisplay(_dpy);
        }
    }

    bool Initialize(const PortConfig& cfg) override
    {
        _dpy = XOpenDisplay(NULL);
        if (_dpy == NULL)
        {
            return false; // 无 X 环境（板上）→ 让上层尝试下一个 port
        }

        _w = cfg.width > 0 ? cfg.width : L2D_SCREEN_WIDTH;
        _h = cfg.height > 0 ? cfg.height : L2D_SCREEN_HEIGHT;

        const int screen = DefaultScreen(_dpy);
        _win = XCreateSimpleWindow(_dpy, RootWindow(_dpy, screen), 0, 0,
                                   (unsigned)_w, (unsigned)_h, 0,
                                   BlackPixel(_dpy, screen), BlackPixel(_dpy, screen));
        XStoreName(_dpy, _win, "l2dtest (X11 port)");
        XSelectInput(_dpy, _win,
                     ExposureMask | ButtonPressMask | ButtonReleaseMask |
                     PointerMotionMask | StructureNotifyMask);
        _wmDelete = XInternAtom(_dpy, "WM_DELETE_WINDOW", False);
        XSetWMProtocols(_dpy, _win, &_wmDelete, 1);
        XMapWindow(_dpy, _win);
        _gc = XCreateGC(_dpy, _win, 0, NULL);
        XFlush(_dpy);

        _frameNs = cfg.maxFps > 0 ? (1000000000L / cfg.maxFps) : 0;
        clock_gettime(CLOCK_MONOTONIC, &_nextFrame);
        printf("[port:x11] 窗口 %dx%d（鼠标拖动=跟随，单击=切表情）\n", _w, _h);
        fflush(stdout);
        return true;
    }

    const char* Name() const override { return "x11"; }
    int Width() const override { return _w; }
    int Height() const override { return _h; }

    bool Present(const uint32_t* fb, int width, int height) override
    {
        if (_dpy == NULL)
        {
            return false;
        }

        // 尺寸相等：零拷贝包装；不等（降采样）：CPU 最近邻放大到内部缓冲
        const uint32_t* src = fb;
        if (width != _w || height != _h)
        {
            if (_scaledW != _w || _scaledH != _h || _scaled == NULL)
            {
                free(_scaled);
                _scaled = (uint32_t*)malloc((size_t)_w * _h * 4);
                _scaledW = _w;
                _scaledH = _h;
            }
            SoftScaleNearest(fb, width, height, _scaled, _w, _h);
            src = _scaled;
        }

        XImage img;
        memset(&img, 0, sizeof(img));
        img.width = _w;
        img.height = _h;
        img.xoffset = 0;
        img.format = ZPixmap;
        img.data = (char*)src;
        img.byte_order = LSBFirst;
        img.bitmap_unit = 32;
        img.bitmap_bit_order = LSBFirst;
        img.bitmap_pad = 32;
        img.depth = 24;
        img.bytes_per_line = _w * 4;
        img.bits_per_pixel = 32;
        img.red_mask = 0xff0000;
        img.green_mask = 0xff00;
        img.blue_mask = 0xff;
        XPutImage(_dpy, _win, _gc, &img, 0, 0, 0, 0, (unsigned)_w, (unsigned)_h);
        XFlush(_dpy);
        return true;
    }

    bool PollInput(int* x, int* y, bool* down) override
    {
        if (_dpy == NULL)
        {
            return false;
        }
        while (XPending(_dpy) > 0)
        {
            XEvent e;
            XNextEvent(_dpy, &e);
            switch (e.type)
            {
            case ButtonPress:
                if (e.xbutton.button == Button1)
                {
                    _btnDown = true;
                    *x = e.xbutton.x;
                    *y = e.xbutton.y;
                    *down = true;
                    return true;
                }
                break;
            case ButtonRelease:
                if (e.xbutton.button == Button1)
                {
                    _btnDown = false;
                    *x = e.xbutton.x;
                    *y = e.xbutton.y;
                    *down = false;
                    return true;
                }
                break;
            case MotionNotify:
                if (_btnDown)
                {
                    *x = e.xmotion.x;
                    *y = e.xmotion.y;
                    *down = true;
                    return true;
                }
                break;
            case ClientMessage:
                if ((Atom)e.xclient.data.l[0] == _wmDelete)
                {
                    _quit = true;
                }
                break;
            default:
                break;
            }
        }
        return false;
    }

    void WaitFrame() override
    {
        if (_frameNs <= 0)
        {
            return;
        }
        _nextFrame.tv_nsec += _frameNs;
        while (_nextFrame.tv_nsec >= 1000000000L)
        {
            _nextFrame.tv_nsec -= 1000000000L;
            _nextFrame.tv_sec += 1;
        }
        plat::SleepUntil(_nextFrame);
    }

    bool ShouldQuit() override { return _quit; }

private:
    Display* _dpy;
    Window _win;
    GC _gc;
    Atom _wmDelete;
    int _w;
    int _h;
    long _frameNs;
    struct timespec _nextFrame;
    bool _btnDown;
    bool _quit;
    uint32_t* _scaled;
    int _scaledW;
    int _scaledH;
};

} // namespace

IPort* CreatePortX11()
{
    return new PortX11();
}

#else // !L2D_WITH_X11

IPort* CreatePortX11()
{
    return NULL; // 未启用 X11 支持
}

#endif

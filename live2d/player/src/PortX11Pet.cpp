/**
 * PortX11Pet.cpp — 本机桌宠端口（X11 + 合成器）：透明、无边框、置顶的角色窗口
 *
 * 这里**只有角色渲染窗口和输入**，没有任何 UI 控件：
 *   - 32 位 ARGB visual：渲染器出的是预乘 alpha 帧、背景全 0 → 合成器下背景透明，只看到角色；
 *   - 无边框（_MOTIF_WM_HINTS）、置顶（_NET_WM_STATE_ABOVE）、不占任务栏/切换器；
 *   - 输入：左键拖动 = 移动窗口；左键单击（几乎没移动）= 点击角色（PollClick）；滚轮 = 缩放；Esc = 退出；
 *   - 视线跟随**全局鼠标**（PollLook 返回窗口坐标，可在窗口外）；
 *   - 输入区域按角色轮廓收窄（XShape ShapeInput，8px 块）：点到透明处穿透到桌面。
 *
 * UI（托盘、右键菜单、说话气泡、状态与设置）全部在 l2dpet-ui 进程（live2d/panel/，EUI-NEO）：
 *   本端口把"右键了 / 要说话 / 气泡锚点动了"变成 {"event":"ui_*"} 事件（PollUiEvent → 主循环广播），
 *   UI 进程的操作则以协议命令回来（choice / window / quit / config …）。见 live2d/DESIGN-PET-LIFE.md §13。
 * 编译期可选：CMake 检测到 X11 + Xext（XShape）时定义 L2D_WITH_X11_PET 才编入。
 */

#include "Port.hpp"
#include "l2d_config.h"
#include "Platform.h"
#include "JsonLite.h"

#include <cstddef>

#ifdef L2D_WITH_X11_PET

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <time.h>
#include <vector>

#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/shape.h>
#include <X11/keysym.h>
#include <X11/Xft/Xft.h>

#include <algorithm>
#include <string>

namespace
{

std::string JsonStr(const std::string& s)
{
    std::string o = "\"";
    for (size_t i = 0; i < s.size(); ++i)
    {
        const char c = s[i];
        if (c == '"' || c == '\\') { o += '\\'; o += c; }
        else if ((unsigned char)c < 0x20) { char b[8]; snprintf(b, sizeof(b), "\\u%04x", c); o += b; }
        else o += c;
    }
    return o + "\"";
}

class PortX11Pet : public IPort
{
public:
    PortX11Pet() { memset(&_nextFrame, 0, sizeof(_nextFrame)); }

    ~PortX11Pet()
    {
        if (_uiPid > 0)
        {
            plat::TerminateChild(_uiPid);   // 桌宠退出：UI 进程（托盘/菜单/气泡/设置）一起退
            plat::WaitChildExit(_uiPid, 2000);
        }
        free(_scaled);
        free(_fxBuf);
        if (_dpy != NULL)
        {
            if (_font != NULL) XftFontClose(_dpy, _font);
            if (_bxft != NULL) XftDrawDestroy(_bxft);
            if (_bgc != NULL) XFreeGC(_dpy, _bgc);
            if (_bw != 0) XDestroyWindow(_dpy, _bw);
            if (_gc != NULL) XFreeGC(_dpy, _gc);
            if (_win != 0) XDestroyWindow(_dpy, _win);
            if (_cmap != 0) XFreeColormap(_dpy, _cmap);
            XCloseDisplay(_dpy);
        }
    }

    bool Initialize(const PortConfig& cfg) override
    {
        _dpy = XOpenDisplay(NULL);
        if (_dpy == NULL)
        {
            return false;
        }
        const int screen = DefaultScreen(_dpy);
        XVisualInfo vi;

        if (!XMatchVisualInfo(_dpy, screen, 32, TrueColor, &vi))
        {
            fprintf(stderr, "[port:pet] 没有 32 位 ARGB visual（没开合成器？）——无法透明\n");
            XCloseDisplay(_dpy);
            _dpy = NULL;
            return false;
        }
        // 高分屏：桌面缩放（Xft.dpi/96，如 GNOME 200% → 2）乘进窗口尺寸——渲染分辨率 = 窗口尺寸
        _ui = DetectUiScale(_dpy);
        _w = (int)((cfg.width > 0 ? cfg.width : 360) * _ui + 0.5f) & ~1;
        _h = (int)((cfg.height > 0 ? cfg.height : 600) * _ui + 0.5f) & ~1;
        _winX = 0;                                      // 真正的位置在首次映射时定（MapInitial：存档位置 / 工作区左下角）
        _winY = std::max(0, DisplayHeight(_dpy, screen) - _h);

        _cmap = XCreateColormap(_dpy, RootWindow(_dpy, screen), vi.visual, AllocNone);
        _vis = vi.visual;   // 气泡窗口/Xft 用
        XSetWindowAttributes swa;
        memset(&swa, 0, sizeof(swa));
        swa.colormap = _cmap;
        swa.background_pixel = 0;   // 全透明
        swa.border_pixel = 0;
        swa.event_mask = ExposureMask | ButtonPressMask | ButtonReleaseMask | PointerMotionMask | KeyPressMask
                         | StructureNotifyMask;
        _win = XCreateWindow(_dpy, RootWindow(_dpy, screen), _winX, _winY, (unsigned)_w, (unsigned)_h, 0, vi.depth,
                             InputOutput, vi.visual, CWColormap | CWBackPixel | CWBorderPixel | CWEventMask, &swa);
        XStoreName(_dpy, _win, "Live2D 桌宠");
        {
            // 中文标题必须同时设 EWMH 的 _NET_WM_NAME（UTF8_STRING）——WM_NAME 的类型是
            // XA_STRING，dock/顶栏按 latin-1 解读会显示成乱码（"æ¡å®"）
            const Atom netName = XInternAtom(_dpy, "_NET_WM_NAME", False);
            const Atom utf8 = XInternAtom(_dpy, "UTF8_STRING", False);
            const char* title = "Live2D 桌宠";
            XChangeProperty(_dpy, _win, netName, utf8, 8, PropModeReplace,
                            (const unsigned char*)title, (int)strlen(title));
        }
        {
            // WM_CLASS 必须是 ASCII（ICCCM："instance\0class\0"）——dock 用它匹配 .desktop/显示应用名
            const char cls[] = "l2dpet\0l2dpet";
            XChangeProperty(_dpy, _win, XInternAtom(_dpy, "WM_CLASS", False), XA_STRING, 8, PropModeReplace,
                            (const unsigned char*)cls, (int)sizeof(cls));
        }
        {
            struct { unsigned long flags, functions, decorations; long inputMode; unsigned long status; } hints;
            memset(&hints, 0, sizeof(hints));
            hints.flags = 2;          // MWM_HINTS_DECORATIONS：无边框
            hints.decorations = 0;
            const Atom mwm = XInternAtom(_dpy, "_MOTIF_WM_HINTS", False);
            XChangeProperty(_dpy, _win, mwm, mwm, 32, PropModeReplace, (unsigned char*)&hints, 5);
        }
        {
            const Atom state = XInternAtom(_dpy, "_NET_WM_STATE", False);
            Atom vals[3] = { XInternAtom(_dpy, "_NET_WM_STATE_ABOVE", False),
                             XInternAtom(_dpy, "_NET_WM_STATE_SKIP_TASKBAR", False),
                             XInternAtom(_dpy, "_NET_WM_STATE_SKIP_PAGER", False) };
            XChangeProperty(_dpy, _win, state, XA_ATOM, 32, PropModeReplace, (unsigned char*)vals, 3);
            const Atom type = XInternAtom(_dpy, "_NET_WM_WINDOW_TYPE", False);
            // 类型用 DOCK，不用 UTILITY：桌宠是"桌面挂件"，要的是**按显示桌面（Super+D）时也留在桌面上**。
            // WM 不会最小化 dock 类窗口（GNOME/Mutter 上 Ubuntu 的 Dock 就是这么活下来的），而 UTILITY
            // 会被一起最小化——只剩 override-redirect 的气泡孤零零挂在桌面上（实测到的现象）。
            // 代价：dock 在多数 WM 里总是置顶、跟随所有工作区，"总在最前"开关在 GNOME 上不再有区别。
            Atom dock = XInternAtom(_dpy, "_NET_WM_WINDOW_TYPE_DOCK", False);
            XChangeProperty(_dpy, _win, type, XA_ATOM, 32, PropModeReplace, (unsigned char*)&dock, 1);
        }
        {
            XSizeHints sh;
            memset(&sh, 0, sizeof(sh));
            sh.flags = PPosition | USPosition | PSize;
            sh.x = _winX;
            sh.y = _winY;
            sh.width = _w;
            sh.height = _h;
            XSetWMNormalHints(_dpy, _win, &sh);
        }
        _wmDelete = XInternAtom(_dpy, "WM_DELETE_WINDOW", False);
        XSetWMProtocols(_dpy, _win, &_wmDelete, 1);
        // 先不映射：等 SetZoom（启动时按保存的大小）定好尺寸和位置再一次性显示——
        // 否则会先以 100% 大小出现、再缩小，底边中点锚定导致离开左下角
        _mapPending = true;
        _gc = XCreateGC(_dpy, _win, 0, NULL);
        XFlush(_dpy);

        _frameNs = cfg.maxFps > 0 ? (1000000000L / cfg.maxFps) : 0;
        clock_gettime(CLOCK_MONOTONIC, &_nextFrame);
        printf("[port:pet] 桌宠窗口 %dx%d @(%d,%d)（屏幕缩放 %.2f×）：左键拖动=移动，单击=点角色，滚轮=缩放，右键=菜单，Esc=退出\n",
               _w, _h, _winX, _winY, _ui);
        fflush(stdout);
        return true;
    }

    const char* Name() const override { return "pet"; }
    int Width() const override { return _w; }
    int Height() const override { return _h; }
    bool Hidden() const override { return _hidden; }

    // ------------------------------------------------------------ 点击 / 滑动特效（叠画在角色帧上）
    // 坐标 = 窗口像素（与 _w×_h 缓冲同一空间）；缓冲是预乘 ARGB，Over 按预乘写
    struct Fx
    {
        int kind;                    // 0=扩散圆环 1=飞散星星 2=滑动闪光
        float x, y, vx, vy, age, life, r0, r1;
    };
    std::vector<Fx> _fxv;
    uint32_t* _fxBuf = NULL;
    int _fxBufW = 0, _fxBufH = 0;   // _fxBuf 的尺寸（跟随窗口缩放重分配）
    double _fxLast = 0.0;
    float _fxAccum = 0.0f;
    int _fxLastX = -1, _fxLastY = -1;

    static unsigned char FxClamp(int v) { return (unsigned char)(v < 0 ? 0 : (v > 255 ? 255 : v)); }

    void FxOver(uint32_t* p, int r, int g, int b, float a)
    {
        if (a <= 0.0f) return;
        if (a > 1.0f) a = 1.0f;
        const uint32_t d = *p;
        const float da = (float)((d >> 24) & 0xFF) / 255.0f;
        const float ia = 1.0f - a;
        const int R = (int)((float)r * a + (float)((d >> 16) & 0xFF) * ia);
        const int G = (int)((float)g * a + (float)((d >> 8) & 0xFF) * ia);
        const int B = (int)((float)b * a + (float)(d & 0xFF) * ia);
        const int A = (int)(255.0f * (a + da * ia));
        *p = ((uint32_t)FxClamp(A) << 24) | ((uint32_t)FxClamp(R) << 16) | ((uint32_t)FxClamp(G) << 8) | (uint32_t)FxClamp(B);
    }

    void FxDisc(uint32_t* buf, float cx, float cy, float r, int cr, int cg, int cb, float a)
    {
        const int x0 = (int)(cx - r - 1), x1 = (int)(cx + r + 1);
        const int y0 = (int)(cy - r - 1), y1 = (int)(cy + r + 1);
        for (int y = std::max(0, y0); y <= std::min(_h - 1, y1); ++y)
        {
            for (int x = std::max(0, x0); x <= std::min(_w - 1, x1); ++x)
            {
                const float dx = (float)x + 0.5f - cx, dy = (float)y + 0.5f - cy;
                const float d = sqrtf(dx * dx + dy * dy);
                if (d < r + 1.0f) FxOver(&buf[(size_t)y * _w + x], cr, cg, cb, a * (d < r - 1.0f ? 1.0f : (r + 1.0f - d) * 0.5f));
            }
        }
    }

    void FxRingPix(uint32_t* buf, float cx, float cy, float r, float w, int cr, int cg, int cb, float a)
    {
        const int x0 = (int)(cx - r - w - 1), x1 = (int)(cx + r + w + 1);
        const int y0 = (int)(cy - r - w - 1), y1 = (int)(cy + r + w + 1);
        for (int y = std::max(0, y0); y <= std::min(_h - 1, y1); ++y)
        {
            for (int x = std::max(0, x0); x <= std::min(_w - 1, x1); ++x)
            {
                const float dx = (float)x + 0.5f - cx, dy = (float)y + 0.5f - cy;
                const float d = sqrtf(dx * dx + dy * dy) - r;
                if (fabsf(d) < w) FxOver(&buf[(size_t)y * _w + x], cr, cg, cb, a * (1.0f - fabsf(d) / w));
            }
        }
    }

    /** 一颗四芒星：亮心 + 四条短光线（光线用渐小的圆点串成）*/
    void FxStar(uint32_t* buf, float x, float y, float s, float a)
    {
        FxDisc(buf, x, y, s * 0.9f, 255, 252, 240, a);
        FxDisc(buf, x, y, s * 0.45f, 255, 255, 255, a);
        for (int k = 0; k < 4; ++k)
        {
            const float ang = 1.5707964f * (float)k + 0.35f;
            for (int j = 1; j <= 3; ++j)
            {
                const float rr = s * (0.9f + 1.15f * (float)j);
                FxDisc(buf, x + cosf(ang) * rr, y + sinf(ang) * rr, s * (0.42f - 0.1f * (float)j), 255, 236, 170, a * (1.0f - 0.22f * (float)j));
            }
        }
    }

    void FxBurst(float x, float y)
    {
        const float s = _ui;
        _fxv.push_back(Fx{ 0, x, y, 0, 0, 0.0f, 0.40f, 8 * s, 34 * s });
        _fxv.push_back(Fx{ 0, x, y, 0, 0, -0.09f, 0.32f, 5 * s, 20 * s });
        for (int i = 0; i < 7; ++i)
        {
            const float ang = (float)rand() / (float)RAND_MAX * 6.28318f;
            const float sp = (35.0f + (float)rand() / (float)RAND_MAX * 75.0f) * s;
            _fxv.push_back(Fx{ 1, x, y, cosf(ang) * sp, sinf(ang) * sp - 26.0f * s,
                               0.0f, 0.45f + (float)rand() / (float)RAND_MAX * 0.2f, 1.4f * s + 1.6f * s * ((float)rand() / (float)RAND_MAX), 0 });
        }
        if (_fxv.size() > 96) _fxv.erase(_fxv.begin(), _fxv.begin() + (int)(_fxv.size() - 96));
    }

    void FxTrail(float x, float y)
    {
        const float s = _ui;
        _fxv.push_back(Fx{ 2, x + ((float)(rand() % 100) / 100.0f - 0.5f) * 6 * s,
                           y + ((float)(rand() % 100) / 100.0f - 0.5f) * 6 * s,
                           (float)(rand() % 40 - 20) * 0.6f * s, -14.0f * s,
                           0.0f, 0.28f + (float)(rand() % 100) / 100.0f * 0.14f, 2.4f * s, 0.8f * s });
        if (_fxv.size() > 96) _fxv.erase(_fxv.begin(), _fxv.begin() + (int)(_fxv.size() - 96));
    }

    /** 推进并叠画（Present 每帧调用）；返回叠加后的缓冲 */
    const uint32_t* FxComposite(const uint32_t* src)
    {
        const double now = NowSec();
        float dt = (float)(now - _fxLast);
        if (_fxLast <= 0.0 || dt > 0.2f) dt = 1.0f / 30.0f;
        _fxLast = now;
        // 缩放（滚轮）会改 _w×_h：缓冲必须跟着重分配，否则 memcpy 越界崩（libc）
        if (_fxBuf == NULL || _fxBufW != _w || _fxBufH != _h)
        {
            free(_fxBuf);
            _fxBuf = (uint32_t*)malloc((size_t)_w * _h * 4);
            _fxBufW = _w;
            _fxBufH = _h;
        }
        if (_fxBuf == NULL)
        {
            _fxv.clear();
            return src;
        }
        memcpy(_fxBuf, src, (size_t)_w * _h * 4);
        size_t w = 0;
        for (size_t i = 0; i < _fxv.size(); ++i)
        {
            Fx& f = _fxv[i];
            f.age += dt;
            if (f.age < 0.0f)
            {
                _fxv[w++] = f;   // 还没出生（延迟环）
                continue;
            }
            const float t = f.age / f.life;
            if (t >= 1.0f) continue;   // 死了
            f.x += f.vx * dt;
            f.y += f.vy * dt;
            f.vx *= (1.0f - 2.4f * dt);
            f.vy *= (1.0f - 2.4f * dt);
            if (f.kind == 0)
            {
                const float r = f.r0 + (f.r1 - f.r0) * t;
                FxRingPix(_fxBuf, f.x, f.y, r, 2.6f * _ui, 255, 205, 130, 0.85f * (1.0f - t) * (1.0f - t));
            }
            else if (f.kind == 1)
            {
                FxStar(_fxBuf, f.x, f.y, f.r0, (1.0f - t));
            }
            else
            {
                FxDisc(_fxBuf, f.x, f.y, f.r0 * (1.0f - t * 0.5f), 255, 240, 190, 0.75f * (1.0f - t));
                FxDisc(_fxBuf, f.x, f.y, f.r0 * 0.4f, 255, 255, 255, 0.8f * (1.0f - t));
            }
            _fxv[w++] = f;
        }
        _fxv.resize(w);
        return _fxBuf;
    }

    bool Present(const uint32_t* fb, int width, int height) override
    {
        if (_dpy == NULL)
        {
            return false;
        }
        if (_mapPending)
        {
            // 启动时前几帧可能先于配置（SetZoom）到来：再等一会儿，1.5s 还没等到才按当前尺寸显示
            if (_mapWaitSince <= 0.0) _mapWaitSince = NowSec();
            if (NowSec() - _mapWaitSince < 1.5) return true;
            MapInitial();
        }
        if (_hidden)
        {
            XFlush(_dpy);
            return true;
        }
        const uint32_t* src = fb;

        _srcW = width;
        _srcH = height;
        if (width != _w || height != _h)
        {
            if (_scaledW != _w || _scaledH != _h || _scaled == NULL)
            {
                free(_scaled);
                _scaled = (uint32_t*)malloc((size_t)_w * _h * 4);
                _scaledW = _w;
                _scaledH = _h;
            }
            SoftScaleBilinear(fb, width, height, _scaled, _w, _h);
            src = _scaled;
        }
        static const char* dumpPath = getenv("L2D_PET_DUMP");   // 调试：第 60 帧存 PAM（含 alpha）
        static int dumpFrame = 0;
        if (dumpPath != NULL && ++dumpFrame == 60)
        {
            if (FILE* f = fopen(dumpPath, "wb"))
            {
                fprintf(f, "P7\nWIDTH %d\nHEIGHT %d\nDEPTH 4\nMAXVAL 255\nTUPLTYPE RGB_ALPHA\nENDHDR\n", _w, _h);
                for (int i = 0; i < _w * _h; ++i)
                {
                    const uint32_t c = src[i];
                    const unsigned char px[4] = { (unsigned char)(c >> 16), (unsigned char)(c >> 8), (unsigned char)c,
                                                  (unsigned char)(c >> 24) };
                    fwrite(px, 1, 4, f);
                }
                fclose(f);
            }
        }
        XImage img;
        memset(&img, 0, sizeof(img));
        img.width = _w;
        img.height = _h;
        img.format = ZPixmap;
        img.data = (char*)src;
        img.byte_order = LSBFirst;
        img.bitmap_unit = 32;
        img.bitmap_bit_order = LSBFirst;
        img.bitmap_pad = 32;
        img.depth = 32;              // ARGB（预乘 alpha，合成器按预乘混合）
        img.bytes_per_line = _w * 4;
        img.bits_per_pixel = 32;
        img.red_mask = 0xff0000;
        img.green_mask = 0xff00;
        img.blue_mask = 0xff;
        if (!_fxv.empty())
        {
            src = FxComposite(src);   // 有特效：拷一份再叠画（不动渲染器缓冲）
            img.data = (char*)src;
        }
        XPutImage(_dpy, _win, _gc, &img, 0, 0, 0, 0, (unsigned)_w, (unsigned)_h);
        if (--_shapeCountdown <= 0 && !_pressed)   // 输入区域跟角色轮廓（~0.5s 一次；拖动中不改）
        {
            UpdateInputShape(src);
            _shapeCountdown = 15;
        }
        UpdateBubble();
        XFlush(_dpy);
        return true;
    }

    bool PollInput(int* x, int* y, bool* down) override
    {
        (void)x; (void)y; (void)down;
        PumpEvents();
        return false;
    }

    bool PollClick(int* x, int* y) override
    {
        PumpEvents();
        if (!_clickPending)
        {
            return false;
        }
        _clickPending = false;
        *x = _clickX;
        *y = _clickY;
        return true;
    }

    bool PollWheel(int* delta) override
    {
        PumpEvents();
        if (_wheel == 0)
        {
            return false;
        }
        *delta = _wheel;
        _wheel = 0;
        return true;
    }

    void SetSavedPlacement(int left, int bottomGap) override
    {
        if (!_mapPending) return;   // 只管第一次显示
        _savedL = left;
        _savedB = bottomGap;
    }

    /** 第一次显示：存档位置（钳进工作区）/ 没存过 → 贴工作区左下角；尺寸此时已是最终尺寸 */
    void MapInitial()
    {
        _mapPending = false;
        int wa[4];
        WorkAreaOrScreen(wa);
        const int sh = DisplayHeight(_dpy, DefaultScreen(_dpy));
        int x = wa[0], y = wa[1] + wa[3] - _h;   // 默认：工作区左下角（任务栏/Dock 之上）
        if (_savedL >= 0 && _savedB >= 0)
        {
            x = _savedL;
            y = sh - _savedB - _h;
        }
        _winX = std::max(wa[0], std::min(x, wa[0] + wa[2] - _w));
        _winY = std::max(wa[1], std::min(y, wa[1] + wa[3] - _h));
        XResizeWindow(_dpy, _win, (unsigned)_w, (unsigned)_h);
        XSizeHints hints;
        memset(&hints, 0, sizeof(hints));
        hints.flags = PPosition | USPosition | PSize;
        hints.x = _winX;
        hints.y = _winY;
        hints.width = _w;
        hints.height = _h;
        XSetWMNormalHints(_dpy, _win, &hints);
        XMapWindow(_dpy, _win);
        XMoveWindow(_dpy, _win, _winX, _winY);
        _shapeCountdown = 0;
        XFlush(_dpy);
        printf("[port:pet] 首次显示 %dx%d @ %d,%d（%s）\n", _w, _h, _winX, _winY,
               _savedL >= 0 && _savedB >= 0 ? "上次的位置" : "工作区左下角");
        fflush(stdout);
    }

    /** 记住位置：左边 x + 底边离屏幕底的距离（缩放以底边为基准，所以存"底"而不是"顶"） */
    void SavePlacement()
    {
        const int sh = DisplayHeight(_dpy, DefaultScreen(_dpy));
        char b[128];
        snprintf(b, sizeof(b), "{\"cmd\":\"config\",\"set\":{\"pos_l\":%d,\"pos_b\":%d}}", _winX,
                 std::max(0, sh - (_winY + _h)));
        _portCmds.push_back(b);
    }

    /** 缩放整个视窗：窗口 = 渲染帧 × zoom，底边中点不动；超屏按比例回退 */
    void SetZoom(float zoom) override
    {
        if (_dpy == NULL)
        {
            return;
        }
        zoom = std::min(2.0f, std::max(0.5f, zoom));
        const int srcW = _srcW > 0 ? _srcW : _w, srcH = _srcH > 0 ? _srcH : _h;
        const int sw = DisplayWidth(_dpy, DefaultScreen(_dpy)), sh = DisplayHeight(_dpy, DefaultScreen(_dpy));
        int nw = (int)(srcW * zoom + 0.5f), nh = (int)(srcH * zoom + 0.5f);
        const int maxW = std::max(160, sw - 40), maxH = std::max(160, sh - 120);
        if (nw > maxW || nh > maxH)
        {
            zoom *= std::min((float)maxW / nw, (float)maxH / nh);
            nw = (int)(srcW * zoom + 0.5f);
            nh = (int)(srcH * zoom + 0.5f);
        }
        nw = std::max(80, nw);
        nh = std::max(80, nh);
        _zoom = zoom;
        if (_mapPending)
        {
            _w = nw;
            _h = nh;
            MapInitial();
            return;
        }
        if (nw == _w && nh == _h)
        {
            return;
        }
        // 锚点：底边不动；水平方向——贴着左边就左边不动、贴着右边就右边不动，否则中点不动
        int wa[4];
        WorkAreaOrScreen(wa);
        const bool atLeft = _winX <= wa[0] + 8, atRight = _winX + _w >= wa[0] + wa[2] - 8;
        const int cx = _winX + _w / 2, by = _winY + _h, rx = _winX + _w;
        const int nx = atLeft ? _winX : (atRight ? rx - nw : cx - nw / 2);
        _w = nw;
        _h = nh;
        _winX = std::max(0, std::min(nx, sw - _w));
        _winY = std::max(0, std::min(by - nh, sh - _h));
        XMoveResizeWindow(_dpy, _win, _winX, _winY, (unsigned)_w, (unsigned)_h);
        SavePlacement();
        _shapeCountdown = 0;     // 下一帧重扫轮廓（头位 → 气泡重定位跟上）
        _bubReposAt = (float)(NowSec() + 0.08);
        XFlush(_dpy);
    }

    // ------------------------------------------------------------ 说话气泡（旧版设计：本进程 X11 直绘。
    // 椭圆鼓泡 + 深青描边 + 下弯浅蓝 + 右上高光 + 两个小圆点沿法线指向角色；
    // SDF 像素级渲染（预乘 ARGB）+ Xft 文字；0.18s 淡入 / 0.22s 淡出）
    static int Utf8Len(unsigned char c)
    {
        if (c < 0x80) return 1;
        if ((c & 0xE0) == 0xC0) return 2;
        if ((c & 0xF0) == 0xE0) return 3;
        if ((c & 0xF8) == 0xF0) return 4;
        return 1;
    }

    /** ARGB 预乘颜色（32 位 visual 上 GC/Xft 都按预乘解释）*/
    static unsigned long PreMult(unsigned char a, unsigned char r, unsigned char g, unsigned char b)
    {
        return ((unsigned long)a << 24) | ((unsigned long)((int)r * a / 255) << 16)
             | ((unsigned long)((int)g * a / 255) << 8) | (unsigned long)((int)b * a / 255);
    }

    /** 气泡缩放同步（zoom 变了 → 字号重开、正显示的气泡重排）。不乘 _ui：Xft 字号在
        高分屏上本来就按 DPI 放大，再乘就双重缩放了（旧版语义 = 只跟 zoom）*/
    void SyncBs()
    {
        const float bs = std::min(2.0f, std::max(0.5f, _zoom));
        if (fabsf(bs - _bs) <= 0.01f) return;
        _bs = bs;
        if (_bubActive && !_bubText.empty() && EnsureFont())
        {
            std::vector<const char*> op;
            const std::vector<std::string> keep = _bubOpts;
            for (size_t i = 0; i < keep.size(); ++i) op.push_back(keep[i].c_str());
            LayoutBubble(_bubText.c_str(), op.empty() ? NULL : &op[0], (int)op.size());
        }
    }

    bool EnsureFont()
    {
        // 气泡跟着角色缩放：字号 = 14pt × 缩放（量化到 0.5pt，缩放变了才重开字体）
        const float pt = roundf(14.0f * _bs * 2.0f) * 0.5f;
        if (_font != NULL && fabsf(pt - _fontPt) < 0.01f)
        {
            return true;
        }
        if (_font != NULL)
        {
            XftFontClose(_dpy, _font);
            _font = NULL;
            _fontTried = false;
        }
        if (_fontTried)
        {
            return false;
        }
        _fontTried = true;
        char spec[48];
        snprintf(spec, sizeof(spec), "sans:size=%.1f", pt);
        _font = XftFontOpenName(_dpy, DefaultScreen(_dpy), spec);
        _fontPt = pt;
        if (_font == NULL)
        {
            fprintf(stderr, "[port:pet] Xft 打不开字体 sans:size=14——气泡不可用\n");
            fflush(stderr);
            return false;
        }
        return true;
    }

    int TextW(const std::string& s) const
    {
        XGlyphInfo gi;
        XftTextExtentsUtf8(_dpy, _font, (const FcChar8*)s.data(), (int)s.size(), &gi);
        return gi.xOff;
    }

    /** 不能出现在行首的标点（中文排版"避头点"）*/
    static bool NoLineStart(const std::string& ch)
    {
        static const char* const kNo[] = { "，", "。", "！", "？", "、", "；", "：", "）", "」", "』", "】", "》",
                                          "…", "—", "～", "”", "’", ",", ".", "!", "?", ";", ":", ")", "]", "~" };
        for (size_t i = 0; i < sizeof(kNo) / sizeof(kNo[0]); ++i)
        {
            if (ch == kNo[i]) return true;
        }
        return false;
    }

    /** 气泡窗口：override-redirect 的 ARGB 小窗（不进窗口管理、不抢焦点），按需映射 */
    bool EnsureBubbleWindow()
    {
        if (_bw != 0)
        {
            return true;
        }
        XSetWindowAttributes swa;
        memset(&swa, 0, sizeof(swa));
        swa.colormap = _cmap;
        swa.background_pixel = 0;
        swa.border_pixel = 0;
        swa.override_redirect = True;
        swa.event_mask = ButtonPressMask | PointerMotionMask | LeaveWindowMask | ExposureMask;
        _bw = XCreateWindow(_dpy, RootWindow(_dpy, DefaultScreen(_dpy)), 0, 0, 16, 16, 0, 32, InputOutput, _vis,
                            CWColormap | CWBackPixel | CWBorderPixel | CWOverrideRedirect | CWEventMask, &swa);
        XStoreName(_dpy, _bw, "l2dpet-bubble");   // 便于测试/工具定位
        _bgc = XCreateGC(_dpy, _bw, 0, NULL);
        _bxft = XftDrawCreate(_dpy, _bw, _vis, _cmap);
        return _bw != 0;
    }

    /** 文本折行（逐码点按字宽；避头点：标点不落行首）+ 选项胶囊布局（内容块坐标：左上角 = 0,0）*/
    void LayoutBubble(const char* text, const char* const* options, int n)
    {
        _bubLines.clear();
        _bubOpts.clear();
        _bubRects.clear();
        const float s = _bs;
        const int maxTextW = (int)(210 * s);
        const std::string t(text);
        std::string line;
        int maxW = 0;
        size_t i = 0;
        while (i < t.size())
        {
            if (t[i] == '\n')
            {
                maxW = std::max(maxW, TextW(line));
                _bubLines.push_back(line);
                line.clear();
                ++i;
                continue;
            }
            int cl = Utf8Len((unsigned char)t[i]);
            if (i + (size_t)cl > t.size())
            {
                cl = 1;
            }
            const std::string ch = t.substr(i, (size_t)cl);
            const std::string cand = line + ch;
            if (TextW(cand) > maxTextW && !line.empty() && !NoLineStart(ch))
            {
                maxW = std::max(maxW, TextW(line));
                _bubLines.push_back(line);
                line.clear();
                continue;
            }
            line = cand;   // 避头点：标点宁可略超宽也留在行尾
            i += (size_t)cl;
        }
        if (!line.empty())
        {
            maxW = std::max(maxW, TextW(line));
            _bubLines.push_back(line);
        }
        const int lineH = _font->height + (int)(4 * s);
        const int pillH = _font->height + (int)(12 * s);
        const int gap = (int)(8 * s);
        std::vector<int> widths;
        int rowW = 0;
        for (int k = 0; k < n; ++k)
        {
            _bubOpts.push_back(std::string(options[k]));
            widths.push_back(TextW(_bubOpts[(size_t)k]) + (int)(28 * s));
            rowW += widths[(size_t)k] + (k ? gap : 0);
        }
        _cw = std::max(maxW, std::min(rowW, (int)(260 * s)));
        int y = (int)_bubLines.size() * lineH - (int)(4 * s);
        if (n > 0)
        {
            y += (int)(12 * s);
            int x = 0, rowStart = 0;
            for (int k = 0; k <= n; ++k)
            {
                const bool end = (k == n);
                if (!end && (x + widths[(size_t)k] <= _cw || k == rowStart))
                {
                    XRectangle r = { (short)x, (short)y, (unsigned short)widths[(size_t)k], (unsigned short)pillH };
                    _bubRects.push_back(r);
                    x += widths[(size_t)k] + gap;
                    continue;
                }
                const int shift = (_cw - (x - gap)) / 2;   // 这一行整行居中
                for (int j = rowStart; j < k; ++j)
                {
                    _bubRects[(size_t)j].x = (short)(_bubRects[(size_t)j].x + shift);
                }
                if (end)
                {
                    y += pillH;
                    break;
                }
                y += pillH + gap;
                x = 0;
                rowStart = k;
                --k;
            }
        }
        _ch = y;
        _bubDirty = true;
    }

    // ---- 软件绘制（预乘 ARGB，抗锯齿：有符号距离场）----
    static float SdRoundBox(float px, float py, float x, float y, float w, float h, float r)
    {
        const float cx = x + w * 0.5f, cy = y + h * 0.5f;
        const float qx = fabsf(px - cx) - (w * 0.5f - r), qy = fabsf(py - cy) - (h * 0.5f - r);
        const float ox = qx > 0 ? qx : 0, oy = qy > 0 ? qy : 0;
        return sqrtf(ox * ox + oy * oy) + std::min(std::max(qx, qy), 0.0f) - r;
    }

    /** 椭圆（近似 SDF，边缘几个像素内足够准）；r 随角度轻微起伏 = 手绘感 */
    static float SdBlob(float px, float py, float cx, float cy, float a, float b)
    {
        const float dx = px - cx, dy = py - cy;
        const float th = atan2f(dy * a, dx * b);
        const float w = 1.0f + 0.018f * sinf(2.0f * th + 0.6f) + 0.008f * sinf(3.0f * th + 1.3f);   // 轻微蛋形
        const float A = a * w, B = b * w;
        const float k0 = sqrtf((dx * dx) / (A * A) + (dy * dy) / (B * B));
        const float k1 = sqrtf((dx * dx) / (A * A * A * A) + (dy * dy) / (B * B * B * B));
        return k1 > 1e-6f ? k0 * (k0 - 1.0f) / k1 : -std::min(A, B);
    }

    static float SdCapsule(float px, float py, float ax, float ay, float bx, float by, float r)
    {
        const float pax = px - ax, pay = py - ay, bax = bx - ax, bay = by - ay;
        float h = (pax * bax + pay * bay) / (bax * bax + bay * bay);
        h = h < 0 ? 0 : (h > 1 ? 1 : h);
        const float dx = pax - bax * h, dy = pay - bay * h;
        return sqrtf(dx * dx + dy * dy) - r;
    }

    /** 预乘"over"：dst = src + dst*(1-a) */
    static void Over(float* d, float r, float g, float b, float a)
    {
        d[0] = r * a + d[0] * (1 - a);
        d[1] = g * a + d[1] * (1 - a);
        d[2] = b * a + d[2] * (1 - a);
        d[3] = a + d[3] * (1 - a);
    }

    static float Cov(float d) { return std::min(std::max(0.5f - d, 0.0f), 1.0f); }

    /** 思考气泡：白色圆鼓椭圆 + 深青粗描边 + 下半部浅蓝阴影 + 右上两道高光 + 两个小圆点指向角色；
     *  选项 = 白底深青描边胶囊（悬停浅蓝）。a = 整体不透明度 */
    void RenderBubbleBg(float a)
    {
        const float olR = 27 / 255.f, olG = 78 / 255.f, olB = 96 / 255.f;   // 深青描边
        const float s = _bs;                                              // 跟角色缩放
        const float lw = 3.4f * s;                                        // 描边宽
        float ea = (_cw * 0.5f + 16.0f * s) * 1.32f, eb = (_ch * 0.5f + 12.0f * s) * 1.32f;
        eb = std::max(eb, std::max(ea * 0.55f, 44.0f * s));   // 圆鼓：高不低于宽的 0.55
        const int M = (int)(8 * s), dotsW = (int)(60 * s), dotsH = (int)(56 * s);
        const int W = (int)(2 * ea) + 2 * M + dotsW, H = (int)(2 * eb) + 2 * M + dotsH;
        const float ecx = (_side < 0) ? M + ea : W - M - ea;   // 椭圆靠离角色远的一侧，圆点朝角色
        const float ecy = M + eb;
        const float dir = (_side < 0) ? 1.0f : -1.0f;
        const float th = 0.70f;   // 椭圆参数角（约 40°，偏下方）
        const float bx = ecx + dir * ea * cosf(th), by = ecy + eb * sinf(th);
        float nx = dir * cosf(th) / ea, ny = sinf(th) / eb;
        const float nl = sqrtf(nx * nx + ny * ny);
        nx /= nl;
        ny /= nl;
        const float d1r = 11.0f * s, d2r = 6.0f * s;
        const float d1x = bx + nx * (lw + d1r + 7.0f * s), d1y = by + ny * (lw + d1r + 7.0f * s);
        const float d2x = d1x + nx * (d1r + d2r + 9.0f * s), d2y = d1y + ny * (d1r + d2r + 9.0f * s);
        _contentX = (int)(ecx - _cw * 0.5f);
        _contentY = (int)(ecy - _ch * 0.5f);
        _tipX = (int)(d2x + dir * d2r);
        _tipY = (int)(d2y + d2r);
        _bbuf.assign((size_t)W * H, 0u);
        for (int y = 0; y < H; ++y)
        {
            for (int x = 0; x < W; ++x)
            {
                const float px = x + 0.5f, py = y + 0.5f;
                float c[4] = { 0, 0, 0, 0 };
                const float d = SdBlob(px, py, ecx, ecy, ea, eb);
                const float outer = Cov(d - lw * 0.5f);
                if (outer > 0)
                {
                    const float inner = Cov(d + lw * 0.5f);
                    const float up = SdBlob(px, py, ecx, ecy - eb * 0.30f, ea * 0.98f, eb * 0.98f);
                    const float shade = Cov(-up);   // 在上移椭圆之外（= 下方那一弯）为 1
                    float fr = 1.0f, fg = 1.0f, fb = 1.0f;
                    fr -= shade * (1.0f - 222 / 255.f);
                    fg -= shade * (1.0f - 238 / 255.f);
                    fb -= shade * (1.0f - 250 / 255.f);
                    Over(c, olR, olG, olB, outer);                 // 先整块描边色
                    Over(c, fr, fg, fb, inner * 0.94f / 1.0f);     // 再盖白底（略透明）
                }
                {
                    const float hx = ecx + ea * 0.55f, hy = ecy - eb * 0.64f;
                    const float h1 = SdCapsule(px, py, hx - ea * 0.13f, hy - eb * 0.05f, hx + ea * 0.05f, hy + eb * 0.07f, 1.6f * s);
                    const float h2 = SdCapsule(px, py, hx + ea * 0.12f, hy + eb * 0.13f, hx + ea * 0.17f, hy + eb * 0.22f, 1.6f * s);
                    const float hc = std::max(Cov(h1), Cov(h2));
                    const float mg = 6.0f * s;
                    const bool inContent = px > _contentX - mg && px < _contentX + _cw + mg
                                           && py > _contentY - mg && py < _contentY + _ch + mg;
                    if (hc > 0 && d < -lw - 2.0f * s && !inContent)   // 高光不压文字、不贴描边
                    {
                        Over(c, olR, olG, olB, hc * 0.9f);
                    }
                }
                const float dd[2][3] = { { d1x, d1y, d1r }, { d2x, d2y, d2r } };
                for (int k = 0; k < 2; ++k)
                {
                    const float dq = sqrtf((px - dd[k][0]) * (px - dd[k][0]) + (py - dd[k][1]) * (py - dd[k][1])) - dd[k][2];
                    const float o = Cov(dq - 1.6f * s), in = Cov(dq + 1.6f * s);   // 和主体描边一样粗
                    if (o > 0)
                    {
                        Over(c, olR, olG, olB, o);
                        Over(c, 1.0f, 1.0f, 1.0f, in * 0.94f);
                    }
                }
                for (size_t r = 0; r < _bubRects.size(); ++r)
                {
                    const XRectangle q = _bubRects[r];
                    const float qx = (float)(_contentX + q.x), qy = (float)(_contentY + q.y), qw = q.width, qh = q.height;
                    if (px < qx - 3 || px > qx + qw + 3 || py < qy - 3 || py > qy + qh + 3)
                    {
                        continue;
                    }
                    const float dq = SdRoundBox(px, py, qx, qy, qw, qh, qh * 0.5f);
                    const bool hov = (int)r == _hover;
                    Over(c, olR, olG, olB, Cov(dq - std::max(1.0f, s)));
                    Over(c, hov ? 0.86f : 1.0f, hov ? 0.93f : 1.0f, 1.0f, Cov(dq + std::max(1.0f, s)));
                }
                const unsigned A = (unsigned)(c[3] * a * 255.f + 0.5f), R = (unsigned)(c[0] * a * 255.f + 0.5f),
                               G = (unsigned)(c[1] * a * 255.f + 0.5f), B = (unsigned)(c[2] * a * 255.f + 0.5f);
                _bbuf[(size_t)y * W + x] = (A << 24) | (R << 16) | (G << 8) | B;
            }
        }
        _bwW = W;
        _bwH = H;
    }

    static XftColor XftCol(unsigned char r, unsigned char g, unsigned char b, float a)
    {
        XftColor c;
        memset(&c, 0, sizeof(c));
        const unsigned char A = (unsigned char)(a * 255.f);
        c.pixel = PreMult(A, r, g, b);
        c.color.red = (unsigned short)(r * 257);
        c.color.green = (unsigned short)(g * 257);
        c.color.blue = (unsigned short)(b * 257);
        c.color.alpha = (unsigned short)(a * 65535.f);   // Xft 走 XRender：分量必须填，只填 pixel 会全透明
        return c;
    }

    /** 每帧：淡入/淡出、悬停高亮；位置在弹出时定一次（相对桌宠窗口固定，不跟模型晃动，只随窗口拖动）*/
    void UpdateBubble()
    {
        SyncBs();
        if (_bw == 0)
        {
            return;
        }
        // 桌宠窗口不在屏幕上（被窗口管理器最小化、"显示桌面"Super+D、托盘隐藏）→ 气泡一起收掉。
        // 气泡是 override-redirect 窗口，WM 不会连它一起最小化，不收就会剩一个气泡孤零零挂在桌面上
        // （实测：按"显示桌面"后模型没了、气泡还在）。窗口回来后由下一条消息重新弹。
        if (_hidden || !_winMapped)
        {
            if (_bubActive || _bwMapped)
            {
                _bubActive = false;
                _bubFadeOut = -1.0;
                XUnmapWindow(_dpy, _bw);
                _bwMapped = false;
            }
            return;
        }
        if (!_bubActive)
        {
            if (_bwMapped)
            {
                XUnmapWindow(_dpy, _bw);
                _bwMapped = false;
            }
            return;
        }
        const double now = NowSec();
        if (_bubReposAt > 0.0f && now >= (double)_bubReposAt)
        {
            _bubReposAt = 0.0f;
            _bubDirty = true;
        }
        if (_bubFadeOut < 0.0 && now >= _bubUntil)
        {
            _bubFadeOut = now;   // 到期：开始淡出
        }
        float a = (float)std::min(1.0, (now - _bubShownAt) / 0.18);   // 淡入 0.18s
        a = a * a * (3.0f - 2.0f * a);
        if (_bubFadeOut >= 0.0)
        {
            const float f = (float)((now - _bubFadeOut) / 0.22);
            if (f >= 1.0f)
            {
                _bubActive = false;
                UpdateBubble();   // 取消映射
                return;
            }
            a *= 1.0f - f;
        }
        if (_bubDirty || fabsf(a - _bubLastA) > 0.02f || _hover != _bubLastHover
            || (a >= 1.0f && _bubLastA < 1.0f))
        {
            const bool resized = _bubDirty;
            RenderBubbleBg(a);
            _bubLastA = a;
            _bubLastHover = _hover;
            _bubDirty = false;
            _bubRedraw = true;
            if (resized)
            {
                // 锚点 = 小圆点末端（tip）落在**头部带中线那一行的轮廓边缘**：气泡挂在角色
                // 左上/右上角，圆点正好从头发/发带那儿"长出来"（不是吊在正头顶上方）。没有轮廓
                // （空帧）时退回窗口近似值。
                // 位置在这里定一次就**冻结**（存成窗口相对偏移）：之后只随窗口拖动，模型自己晃头
                // 不带气泡乱动——每帧按实时轮廓重算会让气泡跟着呼吸/眨眼抖。
                // gap 是逻辑像素：× _bs（缩放）× _ui（屏幕缩放）——角色大小随这两者变。
                const float gap = BubbleGapPx() * _bs * _ui;
                int faceX, faceY;
                if (_charBoxOk)
                {
                    const int edge = (_side < 0) ? _bandMidMinX : _bandMidMaxX;
                    faceX = edge + _side * (int)(gap + 0.5f);
                    faceY = _charTop + (int)(_headBandH * 0.5f);
                }
                else
                {
                    faceX = _w / 2 + _side * (int)(30 * _bs);
                    faceY = (int)(_h * 0.14f);
                }
                _bubOffX = faceX - _tipX;
                _bubOffY = faceY - _tipY;
                // 兜底：窗口贴着屏幕边、气泡被钳回角色身上（压到头带）时，整体顶到头顶上方，
                // 再不行挪到外侧——正常情况（锚点在头发边上）不会走到这里。
                const int bx = _winX + _bubOffX, by = _winY + _bubOffY;
                const int sw0 = DisplayWidth(_dpy, DefaultScreen(_dpy)), sh0 = DisplayHeight(_dpy, DefaultScreen(_dpy));
                const int cx = std::max(0, std::min(bx, sw0 - _bwW));
                const int cy = std::max(0, std::min(by, sh0 - _bwH));
                if (_charBoxOk && (cx != bx || cy != by))
                {
                    const int hTop = _winY + _charTop, hBot = hTop + _headBandH;
                    const int hMinX = _winX + _headMinX, hMaxX = _winX + _headMaxX;
                    const int pad = (int)(gap + 0.5f);
                    if (cx < hMaxX + pad && cx + _bwW > hMinX - pad && cy < hBot + pad && cy + _bwH > hTop - pad)
                    {
                        const int up = hTop - pad - _bwH;
                        if (up >= 0)
                        {
                            _bubOffY = up - _winY;
                        }
                        else if (_side < 0)
                        {
                            _bubOffX = std::max(0, hMinX - pad - _bwW) - _winX;
                        }
                        else
                        {
                            _bubOffX = std::min(sw0 - _bwW, hMaxX + pad) - _winX;
                        }
                    }
                }
            }
        }
        // 每帧只做"别跑出屏幕"的钳制：锚点已冻结，模型怎么动气泡都不动，只随窗口拖。
        const int sw = DisplayWidth(_dpy, DefaultScreen(_dpy)), shh = DisplayHeight(_dpy, DefaultScreen(_dpy));
        int x = _winX + _bubOffX, y = _winY + _bubOffY;
        x = std::max(0, std::min(x, sw - _bwW));
        y = std::max(0, std::min(y, shh - _bwH));
        if (!_bwMapped)
        {
            XMoveResizeWindow(_dpy, _bw, x, y, (unsigned)_bwW, (unsigned)_bwH);
            XMapRaised(_dpy, _bw);
            _bwMapped = true;
            _bwX = x;
            _bwY = y;
        }
        else if (x != _bwX || y != _bwY || _bubRedraw)
        {
            XMoveResizeWindow(_dpy, _bw, x, y, (unsigned)_bwW, (unsigned)_bwH);
            _bwX = x;
            _bwY = y;
        }
        if (_bubRedraw)
        {
            if (_font == NULL)
            {
                _bubRedraw = false;   // 字体没开出来（EnsureFont 失败过）：只画背景不画字
                return;
            }
            XImage img;
            memset(&img, 0, sizeof(img));
            img.width = _bwW;
            img.height = _bwH;
            img.format = ZPixmap;
            img.data = (char*)&_bbuf[0];
            img.byte_order = LSBFirst;
            img.bitmap_unit = 32;
            img.bitmap_bit_order = LSBFirst;
            img.bitmap_pad = 32;
            img.depth = 32;
            img.bytes_per_line = _bwW * 4;
            img.bits_per_pixel = 32;
            img.red_mask = 0xff0000;
            img.green_mask = 0xff00;
            img.blue_mask = 0xff;
            XPutImage(_dpy, _bw, _bgc, &img, 0, 0, 0, 0, (unsigned)_bwW, (unsigned)_bwH);
            const int lineH = _font->height + (int)(4 * _bs);
            XftColor tc = XftCol(38, 48, 58, a);   // 正文深灰
            for (size_t li = 0; li < _bubLines.size(); ++li)
            {
                if (!_bubLines[li].empty())
                {
                    const int lw = TextW(_bubLines[li]);
                    XftDrawStringUtf8(_bxft, &tc, _font, _contentX + (_cw - lw) / 2,
                                      _contentY + (int)li * lineH + _font->ascent,
                                      (const FcChar8*)_bubLines[li].data(), (int)_bubLines[li].size());
                }
            }
            for (size_t r = 0; r < _bubRects.size() && r < _bubOpts.size(); ++r)
            {
                const XRectangle q = _bubRects[r];
                XftColor oc = (int)r == _hover ? XftCol(15, 60, 80, a) : XftCol(27, 78, 96, a);   // 选项深青
                const int tw = TextW(_bubOpts[r]);
                XftDrawStringUtf8(_bxft, &oc, _font, _contentX + q.x + (q.width - tw) / 2,
                                  _contentY + q.y + (q.height - _font->height) / 2 + _font->ascent,
                                  (const FcChar8*)_bubOpts[r].data(), (int)_bubOpts[r].size());
            }
            _bubRedraw = false;
        }
    }

    /** 气泡窗口坐标 → 选项下标（-1 = 不在选项上）*/
    int OptionAt(int x, int y) const
    {
        for (size_t r = 0; r < _bubRects.size(); ++r)
        {
            const XRectangle q = _bubRects[r];
            const int qx = _contentX + q.x, qy = _contentY + q.y;
            if (x >= qx && x < qx + q.width && y >= qy && y < qy + q.height)
            {
                return (int)r;
            }
        }
        return -1;
    }

    /** 气泡窗口的输入事件；返回 true = 已处理 */
    bool HandleBubbleEvent(const XEvent& e)
    {
        if (_bw == 0 || e.xany.window != _bw)
        {
            return false;
        }
        if (e.type == MotionNotify)
        {
            _hover = OptionAt(e.xmotion.x, e.xmotion.y);
        }
        else if (e.type == LeaveNotify)
        {
            _hover = -1;
        }
        else if (e.type == Expose)
        {
            _bubRedraw = true;
        }
        else if (e.type == ButtonPress && e.xbutton.button == Button1 && _bubActive)
        {
            const int k = OptionAt(e.xbutton.x, e.xbutton.y);
            if (k >= 0)
            {
                _choicePending = k;
                _bubFadeOut = NowSec();   // 选中后淡出（按钮一起清掉，免得淡出期间再被点）
                _bubOpts.clear();
                _bubRects.clear();
                _bubDirty = true;
            }
            else if (_bubOpts.empty())
            {
                _bubFadeOut = NowSec();   // 点普通气泡 = 收起
            }
        }
        return true;
    }

    void ShowBubble(const char* text, const char* const* options, int optionCount, float seconds) override
    {
        if (_dpy == NULL || text == NULL || text[0] == '\0' || seconds <= 0.0f)
        {
            HideBubble();
            return;
        }
        // 窗口不在屏幕上（最小化/显示桌面/托盘隐藏）就不弹：气泡是 override-redirect 窗口，
        // 会孤零零留在桌面上（见 UpdateBubble 的说明）
        if (_hidden || !_winMapped)
        {
            return;
        }
        SyncBs();
        if (!EnsureFont() || !EnsureBubbleWindow())
        {
            return;
        }
        // 限长：协议 say / LLM 台词都走这里；超长文本会排出几万像素高的气泡窗 → X 服务器 BadAlloc/BadValue，
        // Xlib 默认错误处理直接结束进程。正常台词 ≤ 60 字，这里截到 120 字（按 UTF-8 字符边界）+ "…"
        std::string clipped = text;
        {
            size_t i = 0, chars = 0;
            while (i < clipped.size() && chars < 120)
            {
                const unsigned char c = (unsigned char)clipped[i];
                i += c < 0x80 ? 1 : ((c & 0xE0) == 0xC0 ? 2 : ((c & 0xF0) == 0xE0 ? 3 : 4));
                ++chars;
            }
            if (i < clipped.size()) clipped = clipped.substr(0, i) + "…";
        }
        text = clipped.c_str();
        // 选项同理：最多 4 个、每个 ≤16 字（选项是一排胶囊，超长会把气泡撑到屏幕外）
        std::vector<std::string> optStore;
        std::vector<const char*> optPtr;
        for (int k = 0; options != NULL && k < optionCount && k < 4; ++k)
        {
            std::string o = options[k] != NULL ? options[k] : "";
            size_t i = 0, chars = 0;
            while (i < o.size() && chars < 16)
            {
                const unsigned char c = (unsigned char)o[i];
                i += c < 0x80 ? 1 : ((c & 0xE0) == 0xC0 ? 2 : ((c & 0xF0) == 0xE0 ? 3 : 4));
                ++chars;
            }
            optStore.push_back(o.substr(0, std::min(i, o.size())));
        }
        for (size_t k = 0; k < optStore.size(); ++k) optPtr.push_back(optStore[k].c_str());
        options = optPtr.empty() ? NULL : &optPtr[0];
        optionCount = (int)optPtr.size();
        _bubText = text;
        LayoutBubble(text, options, optionCount < 0 ? 0 : optionCount);
        // 左右：角色在屏幕右半边 → 气泡弹在头部左上方（反之右上方），离屏幕边远的一侧
        const int sw = DisplayWidth(_dpy, DefaultScreen(_dpy));
        _side = (_winX + _headX > sw / 2) ? -1 : +1;
        const double now = NowSec();
        _bubShownAt = now;
        _bubUntil = now + (double)seconds;
        _bubFadeOut = -1.0;
        _bubActive = true;
        _hover = -1;
        _choicePending = -1;
        _bubDirty = true;
    }

    void HideBubble() override
    {
        // 收起：走 0.22s 淡出（UpdateBubble 里完成并取消映射）
        if (_bubActive && _bubFadeOut < 0.0)
        {
            _bubFadeOut = NowSec();
        }
        _bubOpts.clear();
        _bubRects.clear();
        _bubDirty = true;
    }

    /** 当前是否正挂着一个"带选项、还没选"的气泡（播放器据此不让后续台词冲掉提问）*/
    bool BubbleAwaitingChoice() const override
    {
        return _bubActive && !_bubOpts.empty() && _bubFadeOut < 0.0 && NowSec() < _bubUntil;
    }

    bool PollChoice(int* index) override
    {
        PumpEvents();
        if (_choicePending < 0)
        {
            return false;
        }
        *index = _choicePending;
        _choicePending = -1;
        return true;
    }

    bool PollLook(int* x, int* y) override
    {
        if (_dpy == NULL || _pressed)
        {
            return false;
        }
        Window rootRet, childRet;
        int rx, ry, wx, wy;
        unsigned int mask;
        if (!XQueryPointer(_dpy, _win, &rootRet, &childRet, &rx, &ry, &wx, &wy, &mask))
        {
            return false;
        }
        *x = wx;
        *y = wy;
        return true;
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
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (now.tv_sec > _nextFrame.tv_sec + 1)
        {
            _nextFrame = now;   // 落后太多就重新对齐，不追帧
        }
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &_nextFrame, NULL);
    }

    /** 播放器 ~2Hz 推的面板状态：这里只取窗口类配置（不透明度/置顶/穿透/锁定），并确保 UI 进程在跑 */
    void SetPanelState(const std::string& json) override
    {
        jsonlite::Value v;
        if (jsonlite::parse(json.c_str(), json.size(), v, NULL) && v.isObj())
        {
            const jsonlite::Value& c = v.get("config");
            _cfgOpacity = (float)c.get("opacity").numOr(1.0);
            _cfgTop = c.get("top").numOr(1.0) > 0.5;
            _cfgPass = c.get("passthrough").numOr(0.0) > 0.5;
            // 锁定大小和位置：角色窗口不再被拖走（滚轮缩放由主循环拦，见 l2dtest）
            const bool lockNow = c.get("lock").numOr(0.0) > 0.5;
            if (lockNow != _cfgLock)
            {
                _cfgLock = lockNow;
                _dragging = false;   // 锁定瞬间中止进行中的拖动
            }
            ApplyWindowConfig();
        }
        if (!_uiTried)
        {
            _uiTried = true;
            LaunchUi(-1);   // 常驻 UI 进程：主窗口先隐藏，托盘/菜单/气泡由它负责
        }
        ReapUi();
    }

    /** 打开状态与设置第 page 页（menu=true：在头部旁弹右键菜单；page<0：收起设置窗口与菜单）*/
    void OpenPanel(int page, bool menu) override
    {
        if (menu)
        {
            PushMenuEvent(_winX + _headX, _winY + _headY + (int)(40 * _ui));
        }
        else if (page < 0)
        {
            _uiEvents.push_back("{\"event\":\"ui_close\"}");   // UI 进程隐藏主窗口 + 收起菜单
        }
        else
        {
            LaunchUi(page);
        }
    }

    bool PollPanelCommand(std::string* line) override
    {
        if (_portCmds.empty()) return false;
        *line = _portCmds.front();
        _portCmds.pop_front();
        return true;
    }

    bool PollUiEvent(std::string* json) override
    {
        if (_uiEvents.empty()) return false;
        *json = _uiEvents.front();
        _uiEvents.pop_front();
        return true;
    }

    void SetWindowVisible(bool v) override
    {
        if (_dpy == NULL || v == !_hidden) return;
        _hidden = !v;
        if (_hidden)
        {
            HideBubble();
            _bubActive = false;   // Present 隐藏时不再跑：气泡立即取消映射，别留在屏幕上
            if (_bw != 0 && _bwMapped)
            {
                XUnmapWindow(_dpy, _bw);
                _bwMapped = false;
            }
            XUnmapWindow(_dpy, _win);
            _fxv.clear();   // 隐藏时清掉特效（Present 不再跑）
        }
        else if (_mapPending)
        {
            MapInitial();
        }
        else
        {
            XMapWindow(_dpy, _win);
            XMoveWindow(_dpy, _win, _winX, _winY);
            _appliedTop = -1;   // 重新映射后置顶状态要再发一次
            _shapeCountdown = 0;
        }
        printf("[port:pet] %s\n", _hidden ? "隐藏" : "显示");
        fflush(stdout);
        XFlush(_dpy);
    }

    void RequestQuit() override { _quit = true; }

    bool ShouldQuit() override
    {
        PumpEvents();
        return _quit;
    }

private:
    // ------------------------------------------------------------ UI 进程（l2dpet-ui，EUI-NEO）
    /** 找 UI 程序：L2DPET_UI > 本程序同目录 > ../build-panel/ > ~/l2d-build/build-panel/ */
    static std::string FindUiBinary()
    {
        std::vector<std::string> cands;
        if (const char* e = getenv("L2DPET_UI")) cands.push_back(e);
        const std::string self = plat::SelfExePath();
        if (!self.empty())
        {
            const std::string dir = self.substr(0, self.rfind('/'));
            cands.push_back(dir + "/l2dpet-ui");
            cands.push_back(dir.substr(0, dir.rfind('/')) + "/build-panel/l2dpet-ui");
        }
        cands.push_back(plat::HomeDir() + "/l2d-build/build-panel/l2dpet-ui");
        for (size_t i = 0; i < cands.size(); ++i)
        {
            if (plat::IsFile(cands[i]) && plat::Exists(cands[i]))
            {
                return cands[i];
            }
        }
        return std::string();
    }

    void ReapUi()
    {
        if (_uiPid <= 0)
        {
            if (_uiTried && _uiRestartAt > 0.0 && NowSec() >= _uiRestartAt)
            {
                _uiRestartAt = 0.0;
                LaunchUi(-1);
            }
            return;
        }
        bool exited = false;
        int status = 0;
        plat::ReapChild(_uiPid, &exited, &status);
        if (exited)
        {
            _uiPid = -1;
            // 自动重启（退避 2s → 4s → … ≤60s）：UI 进程管着托盘和说话气泡，崩了不拉起来，
            // LLM 的台词就全丢、托盘图标也没了。正常运行满 60s 再崩就把退避清零
            const double now = NowSec();
            _uiBackoff = (now - _uiStartedAt > 60.0) ? 2.0 : std::min(_uiBackoff * 2.0, 60.0);
            _uiRestartAt = now + _uiBackoff;
            fprintf(stderr, "[port:pet] UI 进程退出了（%.0fs 后自动重启）\n", _uiBackoff);
        }
    }

    /** 确保 UI 进程在跑；page ≥ 0 时让它显示状态与设置第 page 页 */
    void LaunchUi(int page)
    {
        ReapUi();
        if (page >= 0)
        {
            _uiEvents.push_back("{\"event\":\"ui_open\",\"page\":" + std::to_string(page) + "}");
        }
        if (_uiPid > 0)
        {
            return;
        }
        const std::string bin = FindUiBinary();
        if (bin.empty())
        {
            fprintf(stderr, "[port:pet] 找不到 l2dpet-ui（运行 live2d/build-panel.sh；或 L2DPET_UI 指定）——没有托盘/菜单/气泡\n");
            return;
        }
        char scale[32], pg[16];
        snprintf(scale, sizeof(scale), "%.2f", _ui);
        snprintf(pg, sizeof(pg), "%d", page);
        plat::SetEnv("L2DPET_UI_SCALE", scale);
        plat::SetEnv("L2DPET_PAGE", pg);   // -1 = 启动时不显示设置窗口
        const std::string dir = bin.substr(0, bin.rfind('/'));
        const long pid = plat::SpawnDetached(bin, std::vector<std::string>(), dir);
        if (pid > 0)
        {
            _uiPid = (int)pid;
            _uiStartedAt = NowSec();
            _uiRestartAt = 0.0;
            printf("[port:pet] 启动 UI 进程 %s（pid %d）\n", bin.c_str(), (int)pid);
            fflush(stdout);
        }
    }

    /** time = 触发它的 X 事件时间戳（UI 进程设成弹出窗的 _NET_WM_USER_TIME，过 WM 的防抢焦点）*/
    void PushMenuEvent(int x, int y, unsigned long time = 0)
    {
        int wa[4];
        WorkAreaOrScreen(wa);
        char b[240];
        snprintf(b, sizeof(b), "{\"event\":\"ui_menu\",\"x\":%d,\"y\":%d,\"scale\":%.2f,\"time\":%lu,\"wa\":[%d,%d,%d,%d]}", x, y,
                 _ui, time, wa[0], wa[1], wa[2], wa[3]);
        _uiEvents.push_back(b);
        LaunchUi(-1);   // UI 进程没在跑就拉起来（这一次菜单会稍慢）
    }

    // ------------------------------------------------------------ 屏幕 / 工作区 / 缩放
    void WorkAreaOrScreen(int* wa)
    {
        wa[0] = 0;
        wa[1] = 0;
        wa[2] = DisplayWidth(_dpy, DefaultScreen(_dpy));
        wa[3] = DisplayHeight(_dpy, DefaultScreen(_dpy));
        const double now = NowSec();
        if (now - _waAt > 5.0)
        {
            _waAt = now;
            _waOk = false;
            Atom type;
            int fmt;
            unsigned long n = 0, after = 0;
            unsigned char* data = NULL;
            const Atom a = XInternAtom(_dpy, "_NET_WORKAREA", True);
            if (a != None && XGetWindowProperty(_dpy, RootWindow(_dpy, DefaultScreen(_dpy)), a, 0, 4, False, XA_CARDINAL,
                                                &type, &fmt, &n, &after, &data) == Success && data != NULL)
            {
                if (n >= 4 && fmt == 32)
                {
                    const long* v = (const long*)data;
                    for (int i = 0; i < 4; ++i) _wa[i] = (int)v[i];
                    _waOk = _wa[2] > 0 && _wa[3] > 0;
                }
                XFree(data);
            }
        }
        if (_waOk)
        {
            for (int i = 0; i < 4; ++i) wa[i] = _wa[i];
        }
    }

    /** 桌面缩放倍数：L2D_UI_SCALE > Xft.dpi/96 > 1；钳到 [1, 3] */
    static float DetectUiScale(Display* dpy)
    {
        float s = 1.0f;
        const char* env = getenv("L2D_UI_SCALE");
        if (env != NULL && atof(env) > 0.0)
        {
            s = (float)atof(env);
        }
        else if (const char* dpi = XGetDefault(dpy, "Xft", "dpi"))
        {
            if (atof(dpi) > 0.0) s = (float)(atof(dpi) / 96.0);
        }
        return s < 1.0f ? 1.0f : (s > 3.0f ? 3.0f : s);
    }

    /** 窗口类配置：不透明度 / 置顶 / 鼠标穿透（变了才动）*/
    void ApplyWindowConfig()
    {
        bool pass = _cfgPass;
        if (_appliedPass < 0 && pass)
        {
            // 启动时不继承"穿透"：否则一启动就点不到她、也打不开菜单
            _portCmds.push_back("{\"cmd\":\"config\",\"set\":{\"passthrough\":0}}");
            pass = false;
        }
        if (fabsf(_cfgOpacity - _appliedOpacity) > 0.004f)
        {
            _appliedOpacity = _cfgOpacity;
            const Atom a = XInternAtom(_dpy, "_NET_WM_WINDOW_OPACITY", False);
            const unsigned long v = (unsigned long)((double)_cfgOpacity * 0xffffffffu);
            XChangeProperty(_dpy, _win, a, XA_CARDINAL, 32, PropModeReplace, (const unsigned char*)&v, 1);
        }
        const int top = _cfgTop ? 1 : 0;
        if (top != _appliedTop)
        {
            _appliedTop = top;
            XEvent ev;
            memset(&ev, 0, sizeof(ev));
            ev.xclient.type = ClientMessage;
            ev.xclient.window = _win;
            ev.xclient.message_type = XInternAtom(_dpy, "_NET_WM_STATE", False);
            ev.xclient.format = 32;
            ev.xclient.data.l[0] = top;   // _NET_WM_STATE_ADD / REMOVE
            ev.xclient.data.l[1] = (long)XInternAtom(_dpy, "_NET_WM_STATE_ABOVE", False);
            ev.xclient.data.l[3] = 1;
            XSendEvent(_dpy, RootWindow(_dpy, DefaultScreen(_dpy)), False, SubstructureRedirectMask | SubstructureNotifyMask,
                       &ev);
        }
        if ((pass ? 1 : 0) != _appliedPass)
        {
            _appliedPass = pass ? 1 : 0;
            _shapeCountdown = 0;   // 下一帧重算输入区域
        }
        XFlush(_dpy);
    }

    void PumpEvents()
    {
        if (_dpy == NULL)
        {
            return;
        }
        while (XPending(_dpy) > 0)
        {
            XEvent e;
            XNextEvent(_dpy, &e);
            if (HandleBubbleEvent(e))
            {
                continue;   // 气泡窗口的事件（选项点击 / 悬停）
            }
            switch (e.type)
            {
            case ButtonPress:
                if (e.xbutton.button == Button1)
                {
                    _uiEvents.push_back("{\"event\":\"ui_close_menu\"}");   // 在她身上左键 = 收起菜单（兜底）
                    _pressed = true;
                    _dragging = false;
                    _pressRootX = e.xbutton.x_root;
                    _pressRootY = e.xbutton.y_root;
                    _pressWinX = _winX;
                    _pressWinY = _winY;
                    _clickX = e.xbutton.x;
                    _clickY = e.xbutton.y;
                }
                else if (e.xbutton.button == Button3)
                {
                    PushMenuEvent(e.xbutton.x_root, e.xbutton.y_root, e.xbutton.time);   // 右键 = 菜单（UI 进程画）
                }
                else if (e.xbutton.button == Button4)
                {
                    ++_wheel;
                }
                else if (e.xbutton.button == Button5)
                {
                    --_wheel;
                }
                break;
            case MotionNotify:
                if (_pressed && !_cfgLock)   // 锁定大小和位置：拖动不生效（按下抬起仍算一次点击）
                {
                    const int dx = e.xmotion.x_root - _pressRootX, dy = e.xmotion.y_root - _pressRootY;
                    if (!_dragging && dx * dx + dy * dy > 25)
                    {
                        _dragging = true;
                    }
                    if (_dragging)
                    {
                        _winX = _pressWinX + dx;
                        _winY = _pressWinY + dy;
                        XMoveWindow(_dpy, _win, _winX, _winY);
                    }
                }
                else
                {
                    // 滑动特效：指针滑过模型（窗口 shape 保证事件只在模型上到达），每 ~10 逻辑像素撒一颗闪光
                    if (_fxLastX >= 0)
                    {
                        const float ddx = (float)(e.xmotion.x - _fxLastX), ddy = (float)(e.xmotion.y - _fxLastY);
                        _fxAccum += sqrtf(ddx * ddx + ddy * ddy);
                        if (_fxAccum >= 10.0f * _ui)
                        {
                            _fxAccum = 0.0f;
                            FxTrail((float)e.xmotion.x, (float)e.xmotion.y);
                        }
                    }
                    _fxLastX = e.xmotion.x;
                    _fxLastY = e.xmotion.y;
                }
                break;
            case ButtonRelease:
                if (e.xbutton.button == Button1 && _pressed)
                {
                    _pressed = false;
                    if (!_dragging)
                    {
                        _clickPending = true;   // 几乎没移动 = 一次点击
                        FxBurst((float)_clickX, (float)_clickY);   // 点击特效：扩散圆环 + 飞散星星
                    }
                    else if (_winX != _pressWinX || _winY != _pressWinY)
                    {
                        SavePlacement();   // 拖动挪了窗口：记住位置，下次启动还在这儿
                    }
                    _dragging = false;
                }
                break;
            case ConfigureNotify:
                if (e.xconfigure.window == _win)
                {
                    _winX = e.xconfigure.x;
                    _winY = e.xconfigure.y;
                }
                break;
            // 窗口在不在屏幕上：WM 最小化/显示桌面会 Unmap 我们（客户端自己 hide 也走同一条），
            // 记下来给气泡用（气泡是 override-redirect 窗口，WM 不会连它一起收）
            case MapNotify:
                if (e.xmap.window == _win) _winMapped = true;
                break;
            case UnmapNotify:
                if (e.xunmap.window == _win) _winMapped = false;
                break;
            case KeyPress:
                if (XLookupKeysym(&e.xkey, 0) == XK_Escape)
                {
                    printf("[port:pet] Esc → 退出\n");
                    _quit = true;
                }
                break;
            case ClientMessage:
                if (e.xclient.message_type == XInternAtom(_dpy, "WM_PROTOCOLS", False) && (Atom)e.xclient.data.l[0] == _wmDelete)
                {
                    printf("[port:pet] 窗口管理器要求关闭 → 退出\n");
                    _quit = true;
                }
                break;
            default:
                break;
            }
        }
    }

    static double NowSec()
    {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
    }

    /** 气泡与角色轮廓的最小间距（像素，未乘缩放）：L2DPET_BUBBLE_GAP 由桌宠从 config 的
     *  bubble_gap 推过来（与字号同一套环境变量通道，见 PortGlfwPet::BubbleBasePx）。*/
    static float BubbleGapPx()
    {
        if (const char* e = getenv("L2DPET_BUBBLE_GAP"))
        {
            const float v = (float)atof(e);
            if (v >= 0.0f && v <= 120.0f) return v;
        }
        return 0.0f;
    }

    /** 输入区域 = 有像素（alpha > 0x20）的 8x8 块（按行合并成矩形）；顺便估头部位置（气泡锚点）*/
    void UpdateInputShape(const uint32_t* px)
    {
        const int B = 8;
        std::vector<XRectangle> rects;
        for (int by = 0; by < _h; by += B)
        {
            int runStart = -1;
            for (int bx = 0; bx <= _w; bx += B)
            {
                bool solid = false;
                if (bx < _w)
                {
                    for (int y = by; y < by + B && y < _h && !solid; y += 2)
                    {
                        const uint32_t* row = px + (size_t)y * _w;
                        for (int x = bx; x < bx + B && x < _w; x += 2)
                        {
                            if ((row[x] >> 24) > 0x20)
                            {
                                solid = true;
                                break;
                            }
                        }
                    }
                }
                if (solid && runStart < 0)
                {
                    runStart = bx;
                }
                else if (!solid && runStart >= 0)
                {
                    XRectangle r;
                    r.x = (short)runStart;
                    r.y = (short)by;
                    r.width = (unsigned short)(bx - runStart);
                    r.height = (unsigned short)(by + B <= _h ? B : _h - by);
                    rects.push_back(r);
                    runStart = -1;
                }
            }
        }
        if (rects.empty())
        {
            XRectangle r = { (short)(_w / 4), (short)(_h / 4), (unsigned short)(_w / 2), (unsigned short)(_h / 2) };
            rects.push_back(r);
        }
        if (rects.size() > 1)
        {
            int top = 1 << 30, bot = -1, minX = 1 << 30, maxX = -1;
            for (size_t i = 0; i < rects.size(); ++i)
            {
                top = std::min(top, (int)rects[i].y);
                bot = std::max(bot, (int)rects[i].y + (int)rects[i].height);
                minX = std::min(minX, (int)rects[i].x);
                maxX = std::max(maxX, (int)rects[i].x + (int)rects[i].width);
            }
            const int charH = bot - top;
            long sx = 0, n = 0;
            const int bandH = std::max(1, charH / 8);
            const int bandY = top + bandH;
            const int midY = top + bandH / 2;     // 头部带中线：气泡小圆点从这一行"长出来"
            int hMinX = 1 << 30, hMaxX = -1;      // 头部带（顶部 1/8）的左右
            int mMinX = 1 << 30, mMaxX = -1;      // 中线那一行的轮廓左右（= 头发/发带边缘）
            for (size_t i = 0; i < rects.size(); ++i)
            {
                if (rects[i].y < bandY)
                {
                    sx += rects[i].x + rects[i].width / 2;
                    ++n;
                    hMinX = std::min(hMinX, (int)rects[i].x);
                    hMaxX = std::max(hMaxX, (int)rects[i].x + (int)rects[i].width);
                }
                if ((int)rects[i].y <= midY && midY < (int)rects[i].y + (int)rects[i].height)
                {
                    mMinX = std::min(mMinX, (int)rects[i].x);
                    mMaxX = std::max(mMaxX, (int)rects[i].x + (int)rects[i].width);
                }
            }
            _headX = n > 0 ? (int)(sx / n) : (minX + maxX) / 2;
            _headY = top + charH * 11 / 100;
            _charTop = top;          // 轮廓顶：气泡锚点按它算
            _headMinX = n > 0 ? hMinX : minX;
            _headMaxX = n > 0 ? hMaxX : maxX;
            _headBandH = bandH;
            _bandMidMinX = mMaxX >= mMinX ? mMinX : _headMinX;
            _bandMidMaxX = mMaxX >= mMinX ? mMaxX : _headMaxX;
            if (!_charBoxOk && _bubActive) _bubDirty = true;   // 首次扫到轮廓：把气泡锚点补算一次
            _charBoxOk = true;
        }
        else
        {
            _charBoxOk = false;      // 没有可用轮廓（空帧）：气泡退回窗口近似锚点
        }
        if (_appliedPass == 1)
        {
            XShapeCombineRectangles(_dpy, _win, ShapeInput, 0, 0, NULL, 0, ShapeSet, Unsorted);   // 穿透：输入区域清空
            return;
        }
        XShapeCombineRectangles(_dpy, _win, ShapeInput, 0, 0, &rects[0], (int)rects.size(), ShapeSet, Unsorted);
    }

    Display* _dpy = NULL;
    Window _win = 0;
    GC _gc = NULL;
    Colormap _cmap = 0;
    Atom _wmDelete = 0;
    int _w = 360, _h = 600;
    long _frameNs = 0;
    struct timespec _nextFrame;
    bool _pressed = false, _dragging = false;
    int _pressRootX = 0, _pressRootY = 0, _pressWinX = 0, _pressWinY = 0;
    int _winX = 0, _winY = 0;
    bool _clickPending = false;
    int _clickX = 0, _clickY = 0;
    int _wheel = 0;
    bool _quit = false;
    uint32_t* _scaled = NULL;
    int _scaledW = 0, _scaledH = 0;
    int _shapeCountdown = 0;
    float _ui = 1.0f;                      // 屏幕缩放倍数（Xft.dpi/96；L2D_UI_SCALE 覆盖）
    float _zoom = 1.0f;
    bool _mapPending = false;              // 窗口还没第一次显示（等尺寸/位置定好）
    double _mapWaitSince = 0.0;            // 开始等 SetZoom 的时刻（Present 兜底用）
    int _savedL = -1, _savedB = -1;        // 存档位置（SetSavedPlacement）                    // 整窗缩放
    int _srcW = 0, _srcH = 0;              // 播放器渲染帧尺寸
    bool _hidden = false;
    bool _winMapped = false;               // 窗口在屏幕上（Map/UnmapNotify 维护）：气泡跟着它收放
    int _headX = 180, _headY = 80;         // 角色头部（窗口坐标；输入 shape 扫描时估）
    // 窗口类配置
    float _cfgOpacity = 1.0f, _appliedOpacity = -1.0f;
    bool _cfgTop = true, _cfgPass = false, _cfgLock = false;
    int _appliedTop = -1, _appliedPass = -1;
    int _wa[4] = { 0, 0, 0, 0 };
    bool _waOk = false;
    double _waAt = -10.0;
    // UI 进程与事件
    pid_t _uiPid = -1;
    bool _uiTried = false;
    double _uiStartedAt = 0.0;   // 本次 UI 进程启动时刻（判断是不是"刚起就崩"）
    double _uiRestartAt = 0.0;   // >0：到这个时刻自动重启 UI 进程
    double _uiBackoff = 1.0;
    std::deque<std::string> _uiEvents;     // → 主循环广播给 UI 进程
    std::deque<std::string> _portCmds;     // 端口自己要发的协议命令（如启动时关掉穿透）
    // 气泡状态（本进程 X11 直绘，旧版设计）
    float _bs = 1.0f;                      // 气泡缩放（= BubbleScale，SyncBs 同步）
    float _bubReposAt = 0.0f;              // 缩放后强制重定位气泡的时刻（0 = 无待办）
    float _fontPt = 0.0f;
    std::string _bubText;
    XftDraw* _bxft = NULL;
    XftFont* _font = NULL;
    bool _fontTried = false;
    bool _bubActive = false;
    double _bubUntil = 0.0;
    double _bubShownAt = 0.0, _bubFadeOut = -1.0;
    int _cw = 0, _ch = 0;                  // 内容块（文字 + 选项）尺寸
    int _contentX = 0, _contentY = 0;      // 内容块在气泡窗口里的左上角
    int _bubOffX = 0, _bubOffY = 0;        // 气泡相对桌宠窗口的偏移（弹出时定，之后不随模型动）
    std::vector<std::string> _bubLines;
    std::vector<std::string> _bubOpts;
    std::vector<XRectangle> _bubRects;     // 选项胶囊（气泡本体坐标）
    int _choicePending = -1;
    std::vector<unsigned int> _bbuf;       // 气泡背景像素（ARGB）
    Window _bw = 0;                        // 气泡窗口（override-redirect ARGB）
    GC _bgc = NULL;
    bool _bwMapped = false;
    int _bwX = 0, _bwY = 0, _bwW = 0, _bwH = 0;
    int _side = -1;                        // -1 = 气泡在头部左侧，+1 = 右侧
    int _tipX = 0, _tipY = 0;              // 小圆点末端（气泡窗口坐标）
    int _charTop = 0;                      // 角色轮廓顶（窗口坐标；UpdateInputShape 扫）
    int _headMinX = 0, _headMaxX = 0, _headBandH = 1; // 头部带（顶部 1/8）的左右与高度
    int _bandMidMinX = 0, _bandMidMaxX = 0;           // 头部带中线那一行的轮廓左右：小圆点从这里出来
    bool _charBoxOk = false;               // bbox 是否有效（空帧时 false）
    int _hover = -1, _bubLastHover = -2;
    float _bubLastA = -1.0f;
    bool _bubDirty = true, _bubRedraw = false;
    Visual* _vis = NULL;
};

} // namespace

IPort* CreatePortX11Pet()
{
    return new PortX11Pet();
}

#else // !L2D_WITH_X11_PET

IPort* CreatePortX11Pet()
{
    return NULL;
}

#endif

/**
 * Port.hpp — 平台移植接口（多平台输出/输入抽象）
 *
 * 设计：Live2D 解算 + 软渲染与"输出到哪块屏/用什么输入"彻底解耦。
 * 任何平台最差都能走 Headless（纯软件解算，画面落 PPM 文件）——
 * 即"最差最差也是纯软件解算"。
 *
 * 已有实现（CreatePort）：
 *   "x11"      — 桌面 X11 窗口 + 鼠标（Linux 开发/调试用）
 *   "pet"      — 桌宠：X11 透明无边框置顶窗口（需合成器；拖动移窗、单击点角色）
 *   "glfwpet"  — 跨平台桌宠端口（Windows/macOS/纯 Wayland 会话）
 *   "sdl"      — SDL2 窗口 + 鼠标（跨平台桌面；需 SDL2 开发包，编译期可选）
 *   "headless" — 无显示纯软兜底（任何环境可跑；配合 --render 出帧）
 *   NULL       — 自动探测：x11 → sdl → headless 依次尝试
 *
 * 坐标约定：PollInput 返回**port 输出表面坐标**（0..Width()-1）；调用方负责
 * 映射到渲染目标坐标（两者在无降采样时相同）。
 */

#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string>

struct PortConfig
{
    int width;          // 期望输出宽（port 可据实际设备调整，见 Width()）
    int height;         // 期望输出高
    int maxFps;         // 最大帧率（0 = 不节流）
    const char* device; // 平台相关（可 NULL 用默认）
};

class IPort
{
public:
    virtual ~IPort() {}

    /** 打开平台设备/窗口；失败返回 false（调用方可回退到其它 port） */
    virtual bool Initialize(const PortConfig& cfg) = 0;

    /** port 名称（日志用） */
    virtual const char* Name() const = 0;

    /**
     * Initialize 失败是否因"屏幕已被其它程序独占"。调用方据此区分"环境不支持"
     * （可回退其它 port）与"屏被占用、应让位"。
     */
    virtual bool ScreenBusy() const { return false; }

    /** 输出表面尺寸（渲染目标以此为基准 × scale） */
    virtual int Width() const = 0;
    virtual int Height() const = 0;

    /**
     * 输入轮询：返回一次事件（表面坐标系）。
     * @param x,y   事件位置（0..Width()-1 / 0..Height()-1）
     * @param down  按压状态（true=按下/拖动，false=抬起）
     * @return 是否有新事件
     */
    virtual bool PollInput(int* x, int* y, bool* down)
    {
        (void)x; (void)y; (void)down;
        return false;
    }

    /**
     * 宿主给的注视目标轮询（表面坐标系）——非交互端口用：外部工具/宿主把注视位置
     * 写进端口，这里读出来。语义是**状态**不是事件：有目标就每次返回 true（最新值）；
     * 没有则 false（调用方保持现状）。与 PollInput 的区别：不携带按下/抬起，不触发点击语义。
     */
    virtual bool PollLook(int* x, int* y)
    {
        (void)x; (void)y;
        return false;
    }

    /** 点击事件（桌宠：几乎没移动的左键单击；表面坐标）。拖动类输入不走这里 */
    virtual bool PollClick(int* x, int* y)
    {
        (void)x; (void)y;
        return false;
    }

    /** 滚轮（+1 = 向上一格；桌宠用来缩放）*/
    virtual bool PollWheel(int* delta)
    {
        (void)delta;
        return false;
    }

    /**
     * 缩放（桌宠：**缩放整个视窗**——窗口跟着变大小、模型永远完整不裁剪；可选接口）。
     * zoom=1 时窗口 = 渲染尺寸；port 自行决定范围与窗口锚点（桌宠 = 底边中点不动）。
     */
    virtual void SetZoom(float zoom)
    {
        (void)zoom;
    }

    /** 上次保存的窗口位置（桌宠）：left = 窗口左边 x，bottomGap = 窗口底边到屏幕底的距离（像素，<0 = 没存过）。
     *  只在窗口第一次显示之前生效（启动时先知道大小和位置再映射，不会"先大后小、跑偏"）。 */
    virtual void SetSavedPlacement(int left, int bottomGap)
    {
        (void)left;
        (void)bottomGap;
    }

    /**
     * 说话气泡（桌宠用；其它 port 默认忽略）。text 为 UTF-8（可含 \n）；
     * options/count = 可点击的选项（count=0 表示无选项）；seconds 后自动消失（<=0 = 隐藏）。
     * 选项被点中后经 PollChoice 返回下标（0 起）。
     */
    virtual void ShowBubble(const char* text, const char* const* options, int optionCount, float seconds)
    {
        (void)text; (void)options; (void)optionCount; (void)seconds;
    }

    /** 立即隐藏气泡 */
    virtual void HideBubble() {}

    /** 是否正挂着一个"带选项、还没选"的气泡（播放器据此不让后续台词冲掉提问）*/
    virtual bool BubbleAwaitingChoice() const { return false; }

    /**
     * 桌宠面板（右键菜单 + 状态/设置窗口；只有 pet port 实现）：
     * 播放器约每 0.5s 推一份状态 JSON（心情/情绪/配置/表情与动作清单…）；
     * 面板上的操作以**协议命令行**（与 LLM 同一套 JSON，PROTOCOL-LLM.md）经 PollPanelCommand 交回。
     */
    virtual void SetPanelState(const std::string& json) { (void)json; }
    /** UI 进程要的事件（右键菜单 / 气泡 / 打开设置…；{"event":"ui_*"}），主循环取走广播给订阅者 */
    virtual bool PollUiEvent(std::string* json)
    {
        (void)json;
        return false;
    }
    /** 显示 / 隐藏角色窗口（UI 进程的托盘/菜单经 {"cmd":"window"} 调过来）*/
    virtual void SetWindowVisible(bool visible) { (void)visible; }
    /** 请求退出（{"cmd":"quit"}）*/
    virtual void RequestQuit() {}
    /** 窗口是否隐藏（桌宠隐藏到托盘时 true：主循环跳过渲染）*/
    virtual bool Hidden() const { return false; }
    /** 打开设置窗口第 page 页（menu=true：在角色旁弹右键菜单）*/
    virtual void OpenPanel(int page, bool menu) { (void)page; (void)menu; }
    virtual bool PollPanelCommand(std::string* line)
    {
        (void)line;
        return false;
    }
    /** 用户点了气泡里的选项（表面坐标由 port 自己消化；这里只报下标） */
    virtual bool PollChoice(int* index)
    {
        (void)index;
        return false;
    }

    /**
     * 输出一帧。fb 为 ARGB8888（预乘 alpha），尺寸 width×height
     * （可能小于输出表面——port 自行放大）。
     */
    virtual bool Present(const uint32_t* fb, int width, int height) = 0;

    /** 帧率节流：阻塞到下一帧时刻（maxFps=0 时为空实现） */
    virtual void WaitFrame() {}

    /** 窗口被关闭等要求退出（X11/SDL 用） */
    virtual bool ShouldQuit() { return false; }

    /** 可选：抓取"当前显示内容"为 PPM（支持读回的 port 用；其它 port 返回 false） */
    virtual bool SaveScreenshot(const char* path)
    {
        (void)path;
        return false;
    }
};

/**
 * 创建 port。
 * @param name "pet" / "glfwpet" / "x11" / "sdl" / "headless"；NULL = 自动探测
 *             （x11 → sdl → headless，首个成功者为准）
 * @return 未初始化的 port 实例；名称无效返回 NULL
 */
IPort* CreatePort(const char* name);

/**
 * 纯 CPU 最近邻放大（无任何硬件加速平台的通用兜底；x11/sdl 等 port 复用）。
 * dst 尺寸 dw×dh；src 尺寸 sw×sh。
 */
/** 双线性缩放（预乘 ARGB 逐通道插值是正确的；桌宠整窗缩放用——最近邻放大会出锯齿/马赛克）*/
inline void SoftScaleBilinear(const uint32_t* src, int sw, int sh, uint32_t* dst, int dw, int dh)
{
    if (sw < 2 || sh < 2 || dw < 1 || dh < 1)
    {
        return;
    }
    const float fx = (float)sw / (float)dw, fy = (float)sh / (float)dh;
    for (int y = 0; y < dh; ++y)
    {
        float sy = (y + 0.5f) * fy - 0.5f;
        if (sy < 0) sy = 0;
        int y0 = (int)sy;
        if (y0 > sh - 2) y0 = sh - 2;
        const uint32_t wy = (uint32_t)((sy - y0) * 256.0f);
        const uint32_t* r0 = src + (size_t)y0 * sw;
        const uint32_t* r1 = r0 + sw;
        uint32_t* drow = dst + (size_t)y * dw;
        for (int x = 0; x < dw; ++x)
        {
            float sx = (x + 0.5f) * fx - 0.5f;
            if (sx < 0) sx = 0;
            int x0 = (int)sx;
            if (x0 > sw - 2) x0 = sw - 2;
            const uint32_t wx = (uint32_t)((sx - x0) * 256.0f);
            const uint32_t a = r0[x0], b = r0[x0 + 1], c = r1[x0], d = r1[x0 + 1];
            uint32_t out = 0;
            for (int sh8 = 0; sh8 < 32; sh8 += 8)
            {
                const uint32_t top = ((a >> sh8) & 255) * (256 - wx) + ((b >> sh8) & 255) * wx;
                const uint32_t bot = ((c >> sh8) & 255) * (256 - wx) + ((d >> sh8) & 255) * wx;
                out |= (((top * (256 - wy) + bot * wy) >> 16) & 255) << sh8;
            }
            drow[x] = out;
        }
    }
}

inline void SoftScaleNearest(const uint32_t* src, int sw, int sh,
                             uint32_t* dst, int dw, int dh)
{
    for (int y = 0; y < dh; ++y)
    {
        const int sy = (int)((int64_t)y * sh / dh);
        const uint32_t* srow = src + (size_t)sy * sw;
        uint32_t* drow = dst + (size_t)y * dw;
        for (int x = 0; x < dw; ++x)
        {
            drow[x] = srow[(int)((int64_t)x * sw / dw)];
        }
    }
}

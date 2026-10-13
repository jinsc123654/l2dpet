/**
 * PortGlfwPet.cpp — 跨平台桌宠端口（GLFW 3.4 + OpenGL + freetype 文字）
 *
 * 三端（Linux/Windows/macOS）同一份代码——替代 PortX11Pet 的平台专属实现：
 *   窗口  ：GLFW_TRANSPARENT_FRAMEBUFFER + FLOATING + 无边框（透明/置顶/无边框）
 *   出帧  ：软件 ARGB（预乘）→ glDrawPixels 直写帧缓冲（legacy 上下文，三端可用）
 *   点击  ：Linux = XShape 输入区域（角色轮廓，非角色处点击穿到桌面）；
 *           Win/Mac = 整窗 GLFW_MOUSE_PASSTHROUGH（v1）+ 软件命中判定
 *   输入  ：全局鼠标轮询（Linux XQueryPointer / Win GetCursorPos+GetAsyncKeyState /
 *           Mac CGEventSource——窗外也跟随）+ 本窗口滚轮/按键事件
 *   气泡  ：第二个 GLFW 窗口（透明/置顶/不抢焦点）——SDF 鼓泡像素绘制 + freetype 文字
 *   子进程：l2dpet-ui 由 plat::SpawnDetached 拉起（三端）
 *
 * 依赖：GLFW 3.4（仓库 vendored）+ glad（vendored）；freetype 由 PetText 引入。
 */
#include "Port.hpp"
#include "Platform.h"

#ifdef L2D_WITH_GLFW_PET

#include "PetText.h"
#include "PetPaint.h"

#define GLFW_INCLUDE_NONE   // GL 函数由 glad 提供（避免 GLFW 自带 GL 头抢先）
#include <GLFW/glfw3.h>
#include <glad/glad.h>
#if defined(_WIN32)
#define GLFW_EXPOSE_NATIVE_WIN32
#elif defined(__APPLE__)
// macOS 无额外 native 需求
#else
#define GLFW_EXPOSE_NATIVE_X11
#endif
#include <GLFW/glfw3native.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#elif defined(__APPLE__)
// 全局鼠标：CGEventSource（macOS）
#include <ApplicationServices/ApplicationServices.h>
#else
#include <X11/Xlib.h>
#include "X11WindowPolicy.h"
#include <X11/Xatom.h>
#include <X11/extensions/shape.h>
#include <X11/keysym.h>
#endif

namespace
{

std::string JsonStr(const std::string& s)
{
    std::string o = "\"";
    for (size_t i = 0; i < s.size(); ++i)
    {
        const unsigned char c = (unsigned char)s[i];
        if (c == '"') o += "\\\"";
        else if (c == '\\') o += "\\\\";
        else if (c == '\n') o += "\\n";
        else if (c < 0x20) { char b[8]; snprintf(b, sizeof(b), "\\u%04x", c); o += b; }
        else o += (char)c;
    }
    return o + "\"";
}

std::string JsonNum(double v)
{
    char b[32];
    snprintf(b, sizeof(b), "%.4g", v);
    return b;
}

/** 读一行 JSON 里 "key": 后面的字符串（简版；气泡/菜单协议只用到简单字段） */
std::string JsonGetStr(const std::string& line, const char* key)
{
    const std::string k = std::string("\"") + key + "\"";
    size_t p = line.find(k);
    if (p == std::string::npos) return "";
    p = line.find(':', p + k.size());
    if (p == std::string::npos) return "";
    ++p;
    while (p < line.size() && (line[p] == ' ' || line[p] == '\t')) ++p;
    if (p >= line.size()) return "";
    if (line[p] != '"') return "";
    ++p;
    std::string out;
    for (; p < line.size(); ++p)
    {
        const char c = line[p];
        if (c == '\\' && p + 1 < line.size())
        {
            const char n = line[p + 1];
            if (n == 'n') { out += '\n'; ++p; continue; }
            if (n == 't') { out += '\t'; ++p; continue; }
            if (n == '"') { out += '"'; ++p; continue; }
            if (n == '\\') { out += '\\'; ++p; continue; }
            if (n == 'u' && p + 5 < line.size())
            {
                const unsigned cp = (unsigned)strtoul(line.substr(p + 2, 4).c_str(), NULL, 16);
                p += 5;
                if (cp < 0x80) out += (char)cp;
                else if (cp < 0x800) { out += (char)(0xC0 | (cp >> 6)); out += (char)(0x80 | (cp & 0x3F)); }
                else
                {
                    out += (char)(0xE0 | (cp >> 12));
                    out += (char)(0x80 | ((cp >> 6) & 0x3F));
                    out += (char)(0x80 | (cp & 0x3F));
                }
                continue;
            }
        }
        if (c == '"') break;
        out += c;
    }
    return out;
}

}   // namespace

class PortGlfwPet : public IPort
{
public:
    PortGlfwPet() {}
    ~PortGlfwPet();

    bool Initialize(const PortConfig& cfg) override;
    const char* Name() const override { return "glfwpet"; }
    int Width() const override { return _w; }
    int Height() const override { return _h; }
    bool Present(const uint32_t* fb, int width, int height) override;
    void WaitFrame() override;
    bool ShouldQuit() override;

    bool PollInput(int* x, int* y, bool* down) override { return false; }
    bool PollLook(int* x, int* y) override;
    bool PollClick(int* x, int* y) override;
    bool PollWheel(int* delta) override;
    void SetZoom(float zoom) override;
    void SetSavedPlacement(int left, int bottomGap, bool valid) override;
    void EnsureOnScreen() override;

    void ShowBubble(const char* text, const char* const* options, int optionCount, float seconds) override;
    void HideBubble() override;
    bool BubbleAwaitingChoice() const override;
    bool PollChoice(int* index) override;

    void SetPanelState(const std::string& json) override
    {
        _panelState = json;
        RefreshFromState();
        if (!_uiTried)
        {
            _uiTried = true;
            LaunchUi(-1);   // 常驻 UI 进程：主窗口先隐藏，托盘/菜单/气泡由它负责
        }
        ReapUi();
    }
    bool PollUiEvent(std::string* json) override;
    void OpenPanel(int page, bool menu) override;
    bool PollPanelCommand(std::string* line) override;
    void SetWindowVisible(bool visible) override;
    void RequestQuit() override { _quit = true; }
    bool Hidden() const override { return _hidden; }

private:
    void RefreshFromState();
    void ApplyZoom(float zoom);
    void MoveToDefaultSpot();
    void ApplySavedPlacement();                    // 按存档位置落位（启动时 / 配置迟到时）
    void SavePlacement();                          // 拖动/缩放/找回之后把位置写回 config
    void MonitorBoundsAt(int x, int y, int* mx, int* my, int* mw, int* mh);   // 含该点的显示器整屏范围
    void MonitorBounds(int* mx, int* my, int* mw, int* mh);                   // 窗口所在显示器的整屏范围
    static int ClampKeepVisible(int pos, int lo, int hi, int size);
    void UpdateInputShape(const uint32_t* px);   // Linux：XShape 输入区域 = 角色轮廓
    bool PointOnCharacter(int winX, int winY) const;   // 软件命中（帧 alpha 轮廓）
#if defined(_WIN32)
    static LRESULT CALLBACK HitTestWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);  // 逐像素命中（WM_NCHITTEST）
    static PortGlfwPet* s_hitPort;
    static WNDPROC s_origProc;
#endif
    void GlobalMouse(int* rootX, int* rootY, bool* down, bool* downR) const;
    void PumpGlobalInput();                       // 全局鼠标轮询 → 点击/拖动/滑动特效
    void UpdateEffects(double now);               // 特效推进（叠画在帧上）
    const uint32_t* FxComposite(const uint32_t* src);

    // ---- 特效（共享像素库 petpaint：点击圆环+星星、滑动闪光）
    std::vector<petpaint::Fx> _fxv;
    uint32_t* _fxBuf = NULL;
    int _fxBufW = 0, _fxBufH = 0;
    double _fxLast = 0.0;
    float _fxAccum = 0.0f;
    int _fxLastX = -1, _fxLastY = -1;
    void FxBurst(float x, float y);
    void FxTrail(float x, float y);

    // ---- 气泡（SDF 鼓泡共享库 petpaint；文字用 freetype）
    petpaint::BubbleGeom _bg;
    int BubbleOptionAt(double x, double y) const;
    static void BubbleKeyCb(GLFWwindow* w, int key, int sc, int action, int mods);
    static void BubbleMouseBtnCb(GLFWwindow* w, int button, int action, int mods);

    // ---- 窗口 / GL
    GLFWwindow* _win = NULL;
    GLFWwindow* _bw = NULL;
    int _w = 360, _h = 600;
    int _renderW = 360, _renderH = 600;
    int _winX = 0, _winY = 0;
    // 存档位置（config pos_l = 窗口左边 x，pos_b = 窗口底边距显示器底边的距离；两个都允许是负数）
    int _savedL = 0, _savedB = 0;
    bool _savedValid = false;      // 存过位置
    bool _placementDone = false;   // 已按存档落位：配置再变也不往回拽
    bool _movedByUser = false;     // 用户自己拖过：不再套用存档
    bool _shownOnce = false;       // 首帧已上屏
    bool _sizeSettled = false;     // 真实渲染尺寸已定（首个 Present 之后的首次缩放之后）
    float _ui = 1.0f;
    float _zoom = 1.0f;
    bool _quit = false;
    bool _hidden = false;
    float _frameDt = 1.0f / 30.0f;
    double _nextFrame = 0.0;
    // GL 资源（主窗 + 气泡窗各自的纹理/程序/VAO）
    struct GlSurface
    {
        GLuint tex = 0, prog = 0, vao = 0, vbo = 0;
        int texW = 0, texH = 0;
    };
    GlSurface _surf, _bubSurf;
    uint32_t* _scaled = NULL;
    int _scaledW = 0, _scaledH = 0;
    static bool GlInitSurface(GlSurface* s);
    static void GlUploadSurface(GlSurface* s, const uint32_t* argb, int w, int h, int winW, int winH);

    // ---- 输入状态（全局轮询 + 软件命中）
    bool _pressed = false, _dragging = false;
    int _pressRootX = 0, _pressRootY = 0, _pressWinX = 0, _pressWinY = 0;
    int _clickX = 0, _clickY = 0;
    bool _clickPending = false, _wasDown = false, _wasDownR = false;
    int _wheel = 0;
    const uint32_t* _lastFrame = NULL;
    int _shapeCountdown = 0;
    // 角色轮廓 bbox（窗口坐标）：从 _lastFrame 的 alpha 粗扫（对齐 X11 端口的 UpdateInputShape，
    // 每 0.5s 一次）。气泡锚点要用它——按"窗口高度百分比"算会随模型在窗口里的占比漂。
    int _cmTop = 0;
    int _cmHeadMinX = 0, _cmHeadMaxX = 0, _cmBandH = 1;   // 头部带（顶部 1/8）
    int _cmMidMinX = 0, _cmMidMaxX = 0;                   // 带中线那一行的轮廓左右：小圆点从这里出来
    bool _cmOk = false;
    double _cmAt = -10.0;

    // ---- 气泡状态
    std::vector<uint32_t> _bubbleBuf;
    pet::PetFont _font;
    bool _bubActive = false;
    double _bubShownAt = 0.0, _bubUntil = 0.0, _bubFadeOut = -1.0;
    int _side = -1;
    float _bs = 1.0f;
    int _hover = -1;
    int _choicePending = -1;

    // ---- 面板 / UI 进程
    std::string _panelState;
    std::deque<std::string> _uiEvents;
    std::deque<std::string> _portCmds;
    long _uiPid = -1;
    bool _uiTried = false;
    bool _uiStarted = false;
    std::string _pendingUiPage;
    bool _appliedTop = true;
    bool _passthrough = false;
    bool _lock = false;          // 锁定大小和位置（config lock）：拖动不生效
    double _menuAt = 0.0;
    bool _menuOpen = false;
    double _menuClearAt = 0.0;
    void ReapUi();
    void LaunchUi(int page);
    static std::string FindUiBinary();
    void PushRightMenu(int rootX, int rootY);   // 右键：工作区 + 时间戳（UI 进程弹菜单）
    void KeepBelowTaskbar();
    void BubbleTick();                          // 气泡淡入淡出/跟随/呈现（每帧）
    void AnchorBubble();                        // 重算/应用气泡锚点（冻结后在每帧只随窗口平移）
    bool CharMetrics(int* top, int* hMinX, int* hMaxX, int* bandH);   // 轮廓顶 + 头部带（扫 _lastFrame）
    void WindowWorkarea(int* mx, int* my, int* mw, int* mh);        // 窗口所在显示器的工作区（不是主屏）
    bool EnsureBubbleWindow(int w, int h);
    int _bubW = 0, _bubH = 0, _bubX = 0, _bubY = 0;
    int _bubLastX = -9999999, _bubLastY = -9999999;   // 最近下发的气泡位置（判"要不要跟着走"）
    // 气泡锚点（窗口相对；只在弹出/缩放时算一次就冻结——模型晃头不带气泡乱动）
    bool _anchorOk = false;
    int _anchorX = 0, _anchorY = 0;
    bool _anchorBox = false;                          // 冻结时是否拿到了头部带 bbox（兜底判定用）
    int _aTop = 0, _aBot = 0, _aMinX = 0, _aMaxX = 0;
    bool _bubMapped = false;
    float _bubFontPx = 0.0f;   // 气泡布局时用的字号：绘制时按它复位，否则缩放一变就对不上
    float _bubLastA = -1.0f;
    int _bubLastHover = -2;
    std::string _bubText;
};

// ---------------------------------------------------------------- GL 表面（纹理 + 全屏 quad；预乘混合）

namespace
{

const char* kVsSrc =
    "#version 330 core\n"
    "layout(location=0) in vec2 aPos;\n"
    "layout(location=1) in vec2 aUV;\n"
    "out vec2 vUV;\n"
    "void main(){ vUV=aUV; gl_Position=vec4(aPos,0.0,1.0); }\n";

const char* kFsSrc =
    "#version 330 core\n"
    "in vec2 vUV;\n"
    "uniform sampler2D uTex;\n"
    "out vec4 oCol;\n"
    "void main(){ oCol=texture(uTex,vUV); }\n";

GLuint CompileShader(GLenum type, const char* src)
{
    const GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok)
    {
        char log[512] = { 0 };
        glGetShaderInfoLog(s, sizeof(log) - 1, NULL, log);
        fprintf(stderr, "[glfwpet] shader 编译失败：%s\n", log);
        glDeleteShader(s);
        return 0;
    }
    return s;
}

}   // namespace

// 角色窗/气泡窗不进任务栏（定义见文件后段 GlobalMouse 上方）
static void PetHideFromTaskbar(GLFWwindow* w);

#if defined(_WIN32)
PortGlfwPet* PortGlfwPet::s_hitPort = NULL;
WNDPROC PortGlfwPet::s_origProc = NULL;

/** WM_NCHITTEST：把"整窗穿透"换成"逐像素穿透"。
 *  角色轮廓内 → HTCLIENT：这次点击归宠物（不再传给下面的窗口）；
 *  轮廓外     → HTTRANSPARENT：交给下面的窗口（= Linux 侧 XShape 输入区域之外）。
 *  用的就是软件命中同一张轮廓（PointOnCharacter 查上一帧的 alpha）。*/
LRESULT CALLBACK PortGlfwPet::HitTestWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_NCHITTEST && s_hitPort != NULL)
    {
        // 鼠标穿透：整个角色窗口都不吃鼠标，点角色也穿给下面的窗口（等价于 Linux 侧清空 XShape
        // 输入区域）。说话气泡是**另一个窗口**，不受这里影响 —— 所以穿透开着时，带选项的气泡
        // 照样能点（要的就是这个：穿透不该连带把选项也穿掉）。
        if (s_hitPort->_passthrough)
        {
            return HTTRANSPARENT;
        }
        POINT pt;
        pt.x = (int)(short)LOWORD(lp);   // WM_NCHITTEST 的坐标是屏幕坐标
        pt.y = (int)(short)HIWORD(lp);
        ScreenToClient(hwnd, &pt);
        if (!s_hitPort->PointOnCharacter(pt.x, pt.y))
        {
            return HTTRANSPARENT;
        }
    }
    return s_origProc != NULL ? CallWindowProcW(s_origProc, hwnd, msg, wp, lp)
                              : DefWindowProcW(hwnd, msg, wp, lp);
}
#endif

bool PortGlfwPet::GlInitSurface(GlSurface* s)
{
    const GLuint vs = CompileShader(GL_VERTEX_SHADER, kVsSrc);
    const GLuint fs = CompileShader(GL_FRAGMENT_SHADER, kFsSrc);
    if (!vs || !fs) return false;
    s->prog = glCreateProgram();
    glAttachShader(s->prog, vs);
    glAttachShader(s->prog, fs);
    glLinkProgram(s->prog);
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint ok = 0;
    glGetProgramiv(s->prog, GL_LINK_STATUS, &ok);
    if (!ok) return false;
    glUseProgram(s->prog);
    glUniform1i(glGetUniformLocation(s->prog, "uTex"), 0);
    // 全屏 quad（pos + uv；uv 上下翻转：我们的缓冲是自上而下、GL 纹理自下而上）
    const float verts[] = {
        -1, -1, 0, 1,
         1, -1, 1, 1,
         1,  1, 1, 0,
        -1, -1, 0, 1,
         1,  1, 1, 0,
        -1,  1, 0, 0,
    };
    glGenVertexArrays(1, &s->vao);
    glGenBuffers(1, &s->vbo);
    glBindVertexArray(s->vao);
    glBindBuffer(GL_ARRAY_BUFFER, s->vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));
    glGenTextures(1, &s->tex);
    glBindTexture(GL_TEXTURE_2D, s->tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    return true;
}

void PortGlfwPet::GlUploadSurface(GlSurface* s, const uint32_t* argb, int w, int h, int winW, int winH)
{
    glViewport(0, 0, winW, winH);
    glClearColor(0, 0, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT);
    if (argb == NULL || w <= 0 || h <= 0) return;
    glBindTexture(GL_TEXTURE_2D, s->tex);
    if (s->texW != w || s->texH != h)
    {
        // 缓冲是 0xAARRGGBB（小端内存序 = B,G,R,A）→ GL_BGRA 正配
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_BGRA, GL_UNSIGNED_BYTE, argb);
        s->texW = w;
        s->texH = h;
    }
    else
    {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_BGRA, GL_UNSIGNED_BYTE, argb);
    }
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);   // 预乘 over
    glUseProgram(s->prog);
    glBindVertexArray(s->vao);
    glDrawArrays(GL_TRIANGLES, 0, 6);
}

// ---------------------------------------------------------------- 初始化

PortGlfwPet::~PortGlfwPet()
{
    if (_uiPid > 0)
    {
        plat::TerminateChild(_uiPid);
        plat::WaitChildExit(_uiPid, 2000);
    }
    free(_scaled);
    free(_fxBuf);
    if (_bw != NULL) glfwDestroyWindow(_bw);
    if (_win != NULL) glfwDestroyWindow(_win);
    glfwTerminate();
}

bool PortGlfwPet::Initialize(const PortConfig& cfg)
{
    glfwSetErrorCallback([](int code, const char* desc) {
        fprintf(stderr, "[glfwpet] GLFW 错误 %d：%s\n", code, desc);
    });
    if (!glfwInit())
    {
        fprintf(stderr, "[glfwpet] glfwInit 失败（无显示环境？）\n");
        return false;
    }
    // 桌面缩放：GLFW 内容缩放（X11 = Xft.dpi/96；Win = DPI；Mac = backing scale）
    {
        float xs = 1.0f, ys = 1.0f;
        glfwGetMonitorContentScale(glfwGetPrimaryMonitor(), &xs, &ys);
        if (xs > 0.05f) _ui = xs;
        if (const char* e = getenv("L2D_UI_SCALE"))
        {
            const float v = (float)atof(e);
            if (v >= 0.5f && v <= 4.0f) _ui = v;
        }
    }
    const int baseW = cfg.width > 0 ? cfg.width : 360;
    const int baseH = cfg.height > 0 ? cfg.height : 600;
    _w = (int)(baseW * _ui + 0.5f) & ~1;
    _h = (int)(baseH * _ui + 0.5f) & ~1;

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
#if defined(__APPLE__)
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
#endif
    glfwWindowHint(GLFW_RESIZABLE, GLFW_FALSE);
    glfwWindowHint(GLFW_DECORATED, GLFW_FALSE);
    glfwWindowHint(GLFW_FLOATING, GLFW_TRUE);
    glfwWindowHint(GLFW_TRANSPARENT_FRAMEBUFFER, GLFW_TRUE);
    glfwWindowHint(GLFW_FOCUS_ON_SHOW, GLFW_FALSE);
    glfwWindowHint(GLFW_SCALE_TO_MONITOR, GLFW_FALSE);
    glfwWindowHint(GLFW_VISIBLE, GLFW_TRUE);
#if defined(__linux__)
    // WM_CLASS 固定 ASCII（GLFW 默认用窗口标题兜底 WM_CLASS，中文标题会让 dock 显示乱码）
    glfwWindowHint(GLFW_X11_CLASS_NAME, (int)(intptr_t) "l2dpet");
    glfwWindowHint(GLFW_X11_INSTANCE_NAME, (int)(intptr_t) "l2dpet");
#endif
#if defined(__APPLE__)
    // macOS v1：整窗穿透 + 软件命中（见 PointOnCharacter）。
    // Windows **不用**整窗穿透：那样点角色时这一次点击会同时穿到下面的窗口（"点了模型，
    // 后面的应用也收到了点击"）。Windows 改由 WM_NCHITTEST 逐像素判定，等价于 Linux 的 XShape 输入区域。
    glfwWindowHint(GLFW_MOUSE_PASSTHROUGH, GLFW_TRUE);
#endif
    _win = glfwCreateWindow(_w, _h, "Live2D 桌宠", NULL, NULL);
    if (_win == NULL)
    {
        fprintf(stderr, "[glfwpet] 创建窗口失败\n");
        glfwTerminate();
        return false;
    }
    PetHideFromTaskbar(_win);   // 角色窗口不进任务栏/Alt-Tab（同 Linux 的 SKIP_TASKBAR）
#if defined(_WIN32)
    // 逐像素命中 + 不抢焦点：
    //   · WM_NCHITTEST 子类化（见 HitTestWndProc）：角色轮廓内吃掉点击、轮廓外穿给下面；
    //   · WS_EX_NOACTIVATE：点她不该把当前应用的前台焦点抢走（桌宠该有的行为）。
    {
        HWND hwnd = glfwGetWin32Window(_win);
        if (hwnd != NULL)
        {
            LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
            SetWindowLongPtrW(hwnd, GWL_EXSTYLE, ex | (LONG_PTR)WS_EX_NOACTIVATE);
            s_hitPort = this;
            s_origProc = (WNDPROC)SetWindowLongPtrW(hwnd, GWLP_WNDPROC, (LONG_PTR)&PortGlfwPet::HitTestWndProc);
        }
    }
#endif
    glfwMakeContextCurrent(_win);
    if (gladLoadGLLoader((GLADloadproc)glfwGetProcAddress) == 0)
    {
        fprintf(stderr, "[glfwpet] glad 加载 GL 函数失败\n");
        return false;
    }
    glfwSwapInterval(0);   // 自管节流（WaitFrame）
    if (!GlInitSurface(&_surf))
    {
        fprintf(stderr, "[glfwpet] GL 资源初始化失败\n");
        return false;
    }

    // 位置：屏幕右下角（离任务栏留边）
    {
        int mx = 0, my = 0, mw = 0, mh = 0;
        WindowWorkarea(&mx, &my, &mw, &mh);
        _winX = mx + mw - _w - (int)(40 * _ui);
        _winY = my + mh - _h - (int)(60 * _ui);
    }
    glfwSetWindowPos(_win, _winX, _winY);
    glfwSetWindowPosCallback(_win, [](GLFWwindow* w, int x, int y) {
        PortGlfwPet* p = (PortGlfwPet*)glfwGetWindowUserPointer(w);
        if (p != NULL)
        {
            p->_winX = x;
            p->_winY = y;
        }
    });
    glfwSetWindowUserPointer(_win, this);
    glfwSetScrollCallback(_win, [](GLFWwindow* w, double dx, double dy) {
        PortGlfwPet* p = (PortGlfwPet*)glfwGetWindowUserPointer(w);
        if (p != NULL && !p->_passthrough && dy != 0.0) p->_wheel += dy > 0 ? 1 : -1;
    });
    // 鼠标按键（Linux：主输入源；XShape 保证只有点到角色才会到达本窗口）
    glfwSetMouseButtonCallback(_win, [](GLFWwindow* w, int button, int action, int mods) {
        (void)mods;
        PortGlfwPet* p = (PortGlfwPet*)glfwGetWindowUserPointer(w);
        if (p == NULL || p->_passthrough) return;
        double cx = 0, cy = 0;
        glfwGetCursorPos(w, &cx, &cy);
        const int rx = p->_winX + (int)cx, ry = p->_winY + (int)cy;
        if (button == GLFW_MOUSE_BUTTON_LEFT)
        {
            if (action == GLFW_PRESS)
            {
                // "点在她身上 = 收菜单"：Linux 走窗口事件、Windows 走 WM_NCHITTEST 命中后的
                // 真实事件，两边都到得了这里，所以放在平台分支外面。
                p->_uiEvents.push_back("{\"event\":\"ui_close_menu\"}");   // 点她身上 = 收菜单（兜底）
                p->_menuOpen = false;
#if defined(__linux__) || defined(_WIN32)
                // Windows 按下也以真实窗口事件为准；全局位置只用于持续拖动。
                if (p->_passthrough) return;
                p->_pressed = true;
                fprintf(stderr, "[glfwpet] model press lock=%d passthrough=%d\n", (int)p->_lock, (int)p->_passthrough);
                p->_dragging = false;
                p->_pressRootX = rx;
                p->_pressRootY = ry;
                p->_pressWinX = p->_winX;
                p->_pressWinY = p->_winY;
                p->_clickX = (int)cx;
                p->_clickY = (int)cy;
#endif
            }
            else if (action == GLFW_RELEASE)
            {
#if defined(__linux__)
                if (!p->_pressed || p->_passthrough) return;
                if (p->_dragging)
                {
                    p->SavePlacement();
                    fprintf(stderr, "[glfwpet] drag finished at %d,%d\n", p->_winX, p->_winY);
                }
                else
                {
                    p->_clickPending = true;
                    p->_clickX = (int)cx;
                    p->_clickY = (int)cy;
                    petpaint::Burst(&p->_fxv, (float)cx, (float)cy, p->_ui);
                }
                p->_pressed = false;
                p->_dragging = false;
#endif
            }
        }
#if defined(__linux__) || defined(_WIN32)
        else if (button == GLFW_MOUSE_BUTTON_RIGHT && action == GLFW_PRESS)
        {
            p->PushRightMenu(rx, ry);
        }
#else
        (void)rx;
        (void)ry;
#endif
    });
    glfwSetKeyCallback(_win, [](GLFWwindow* w, int key, int sc, int action, int mods) {
        if (key == GLFW_KEY_ESCAPE && action == GLFW_PRESS)
        {
            PortGlfwPet* p = (PortGlfwPet*)glfwGetWindowUserPointer(w);
            if (p != NULL)
            {
                printf("[glfwpet] Esc → 退出\n");
                p->_quit = true;
            }
        }
    });

#if defined(__linux__)
    // X11：跳过任务栏/分页器 + 工具窗口类型 + 记 _NET_WM_PID（与 X11 端口同款）
    {
        Display* dpy = glfwGetX11Display();
        const Window xw = glfwGetX11Window(_win);
        if (dpy != NULL && xw != 0)
        {
            x11pet::ModelType(dpy, xw, _appliedTop);
            x11pet::NoFocusOnMap(dpy, xw);
            const Atom state = XInternAtom(dpy, "_NET_WM_STATE", False);
            Atom states[3] = { XInternAtom(dpy, "_NET_WM_STATE_SKIP_TASKBAR", False),
                               XInternAtom(dpy, "_NET_WM_STATE_SKIP_PAGER", False),
                               XInternAtom(dpy, "_NET_WM_STATE_ABOVE", False) };
            XChangeProperty(dpy, xw, state, XA_ATOM, 32, PropModeReplace, (unsigned char*)states, _appliedTop ? 3 : 2);
            const Atom pidAtom = XInternAtom(dpy, "_NET_WM_PID", False);
            const unsigned long pid = (unsigned long)plat::CurrentPid();
            XChangeProperty(dpy, xw, pidAtom, XA_CARDINAL, 32, PropModeReplace, (unsigned char*)&pid, 1);
            XFlush(dpy);
        }
    }
#elif defined(_WIN32)
    // Win：工具窗口（不进 Alt-Tab/任务栏）
    {
        HWND hwnd = glfwGetWin32Window(_win);
        LONG_PTR ex = GetWindowLongPtrA(hwnd, GWL_EXSTYLE);
        SetWindowLongPtrA(hwnd, GWL_EXSTYLE, ex | WS_EX_TOOLWINDOW);
    }
#endif

    printf("[glfwpet] 桌宠窗口 %dx%d（屏幕缩放 %.2f×，GLFW 跨平台端口）：左键拖动=移动，单击=点角色，滚轮=缩放，右键=菜单，Esc=退出\n",
           _w, _h, _ui);
    fflush(stdout);
    _nextFrame = plat::MonoNow();
    return true;
}

// ---------------------------------------------------------------- 出帧

bool PortGlfwPet::Present(const uint32_t* fb, int width, int height)
{
    if (_quit) return true;
    if (_hidden)
    {
        return true;   // 隐藏时不渲染（窗口已 hide）
    }
    _renderW = width;
    _renderH = height;
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
        if (_scaled != NULL)
        {
            SoftScaleBilinear(fb, width, height, _scaled, _w, _h);
            src = _scaled;
        }
    }
    _lastFrame = src;

    if (_menuOpen && plat::MonoNow() > _menuClearAt) _menuOpen = false;   // 菜单超期兜底
    PumpGlobalInput();
    if (!_fxv.empty())
    {
        src = FxComposite(src);
    }

    glfwMakeContextCurrent(_win);
    GlUploadSurface(&_surf, src, _w, _h, _w, _h);
    glfwSwapBuffers(_win);
    glfwPollEvents();
    _shownOnce = true;   // 首帧上屏：存档落位/启动缩放到此结束，之后才允许记录位置（见 SavePlacement）

#if defined(__linux__)
    if (--_shapeCountdown <= 0 && !_pressed)
    {
        UpdateInputShape(_lastFrame);   // 输入区域只取模型，排除悬停/点击特效
        _shapeCountdown = 15;
    }
#endif
    BubbleTick();
    KeepBelowTaskbar();
    ReapUi();
    return true;
}

void PortGlfwPet::WaitFrame()
{
    _nextFrame += _frameDt;
    const double now = plat::MonoNow();
    if (_nextFrame < now) _nextFrame = now;   // 落后就追平，不补帧
    const double waitMs = (_nextFrame - now) * 1000.0;
    if (waitMs > 0.5) plat::SleepMs((int)(waitMs + 0.5));
}

bool PortGlfwPet::ShouldQuit()
{
    if (_quit) return true;
    if (_win != NULL && glfwWindowShouldClose(_win)) return true;
    return false;
}

// ---------------------------------------------------------------- 输入（全局鼠标轮询 + 软件命中）

/** 桌宠窗口/气泡窗口都不该出现在任务栏与 Alt-Tab 里——Linux 侧靠 _NET_WM_STATE 的
 *  SKIP_TASKBAR / SKIP_PAGER 达到同样效果，Win32 的对应开关是 WS_EX_TOOLWINDOW。
 *  注意 GLFW 给**无边框**窗口默认加了 WS_EX_APPWINDOW（否则它不上任务栏），
 *  两者互斥：要摘掉任务栏入口就得同时去掉 APPWINDOW。
 *  改完扩展样式后任务栏按钮不会立刻消失，藏一下再显示让它重建（启动期调用，看不出闪）。*/
static void PetHideFromTaskbar(GLFWwindow* w)
{
#if defined(_WIN32)
    HWND hwnd = glfwGetWin32Window(w);
    if (hwnd == NULL)
    {
        return;
    }
    LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    SetWindowLongPtrW(hwnd, GWL_EXSTYLE,
                      (ex & ~(LONG_PTR)WS_EX_APPWINDOW) | (LONG_PTR)WS_EX_TOOLWINDOW);
    ShowWindow(hwnd, SW_HIDE);
    ShowWindow(hwnd, SW_SHOWNA);
#else
    (void)w;
#endif
}

/** 气泡字号基准（像素）：布局里那些留白/行高/宽度预算都是按这个字号定的，
 *  字号一变就整体按比例推挤（见 ShowBubble 的 _bs）。*/
static const float kBubbleBaseFontPx = 14.0f;

/** 气泡基准字号（像素）：L2DPET_BUBBLE_PX 可覆盖（桌宠从 config 的 bubble_px 推过来）。
 *  Windows 上默认 14px 的雅黑常显得偏小发虚，做成可调省得改代码重编。*/
static float BubbleBasePx()
{
    if (const char* e = getenv("L2DPET_BUBBLE_PX"))
    {
        const float v = (float)atof(e);
        if (v >= 8.0f && v <= 64.0f) return v;
    }
    return kBubbleBaseFontPx;
}

/** 气泡与角色轮廓的最小间距（像素，未乘缩放）：L2DPET_BUBBLE_GAP 由桌宠从 config 的
 *  bubble_gap 推过来，与字号同一套环境变量通道。默认 0：小圆点末端正好落在轮廓边缘，
 *  看着就是"从头发里长出来"；调大则整段往角色外侧挪。*/
static float BubbleGapPx()
{
    if (const char* e = getenv("L2DPET_BUBBLE_GAP"))
    {
        const float v = (float)atof(e);
        if (v >= 0.0f && v <= 120.0f) return v;
    }
    return 0.0f;
}

void PortGlfwPet::GlobalMouse(int* rootX, int* rootY, bool* down, bool* downR) const
{
    *down = false;
    *downR = false;
#if defined(_WIN32)
    POINT pt;
    GetCursorPos(&pt);
    *rootX = pt.x;
    *rootY = pt.y;
    *down = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
    *downR = (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0;
#elif defined(__APPLE__)
    CGEventRef ev = CGEventCreate(NULL);
    const CGPoint loc = CGEventGetLocation(ev);
    CFRelease(ev);
    *rootX = (int)loc.x;
    *rootY = (int)loc.y;
    *down = CGEventSourceButtonState(kCGEventSourceStateCombinedSessionState, kCGMouseButtonLeft);
    *downR = CGEventSourceButtonState(kCGEventSourceStateCombinedSessionState, kCGMouseButtonRight);
#else
    Display* dpy = glfwGetX11Display();
    if (dpy == NULL)
    {
        *rootX = 0;
        *rootY = 0;
        return;
    }
    Window rootRet, childRet;
    int rx = 0, ry = 0, wx = 0, wy = 0;
    unsigned mask = 0;
    const Window xw = glfwGetX11Window(_win);
    if (XQueryPointer(dpy, xw, &rootRet, &childRet, &rx, &ry, &wx, &wy, &mask))
    {
        *rootX = rx;
        *rootY = ry;
        *down = (mask & Button1Mask) != 0;
        *downR = (mask & Button3Mask) != 0;
    }
    else
    {
        *rootX = 0;
        *rootY = 0;
    }
#endif
}

bool PortGlfwPet::PointOnCharacter(int winX, int winY) const
{
    if (_lastFrame == NULL) return false;
    if (winX < 0 || winY < 0 || winX >= _w || winY >= _h) return false;
    // 采样周边 3×3 的 alpha（边缘容错）
    for (int dy = -1; dy <= 1; ++dy)
    {
        for (int dx = -1; dx <= 1; ++dx)
        {
            const int x = winX + dx, y = winY + dy;
            if (x < 0 || y < 0 || x >= _w || y >= _h) continue;
            if (((_lastFrame[(size_t)y * _w + x] >> 24) & 0xFF) > 0x20) return true;
        }
    }
    return false;
}

void PortGlfwPet::PumpGlobalInput()
{
    int rx = 0, ry = 0;
    bool down = false, downR = false;
#if defined(__linux__)
    // Linux：鼠标按键走 GLFW 窗口事件（精确——点菜单/其它窗口不会误算，XShape 已把
    // 窗口输入区域收窄到角色轮廓）；这里只取全局位置做滑动特效
    {
        double cx = 0, cy = 0;
        glfwGetCursorPos(_win, &cx, &cy);
        rx = _winX + (int)cx;
        ry = _winY + (int)cy;
        down = _pressed;          // 由窗口回调维护
        downR = false;
    }
#else
    // Windows：按下走窗口事件，轮询全局位置和释放；macOS 同时轮询按下。
    GlobalMouse(&rx, &ry, &down, &downR);
    // 抑制都在轮询**之后**判（GlobalMouse 会覆盖 down/downR，写在前面就是死代码）：
    //   · macOS 菜单开着时抑制全局点击；Windows 由窗口事件区分接收者；
    //   · 鼠标穿透：她整个不吃鼠标（拖不动、点不到，右键菜单也不弹）——Linux 侧靠把输入区域清空
    //     达到同样效果（见 UpdateInputShape）。
#if !defined(_WIN32)
    if (_menuOpen) { down = false; downR = false; }
#endif
    if (_passthrough) { down = false; downR = false; }
#endif
    const int wx = rx - _winX;
    const int wy = ry - _winY;

#if defined(__APPLE__)
    // 按下边沿（仅 Win/Mac：Linux 走窗口回调）
    // 整窗穿透 + 全局轮询下，**任何**窗口上的按下都会走到这里，所以按下时先判一次
    // "这次算不算落在角色轮廓上"（Linux 侧靠 XShape 把输入区域收窄到轮廓，天然没有这个问题）。
    // 但原点（_pressRoot* / _pressWin*）**不管归不归宠物都要刷新**：
    // 它们只在按下沿更新，若这次按在别处就跳过更新，拖动段会拿**上一次**的原点算增量，
    // 窗口会被整体甩到光标附近（症状："点到哪她跳到哪"）。
    if (down && !_wasDown)
    {
        _pressed = PointOnCharacter(wx, wy);
        _dragging = false;
        _pressRootX = rx;
        _pressRootY = ry;
        _pressWinX = _winX;
        _pressWinY = _winY;
        _clickX = wx;
        _clickY = wy;
    }
    if (downR && !_wasDownR && PointOnCharacter(wx, wy))
    {
        PushRightMenu(rx, ry);
    }
    _wasDown = down;
    _wasDownR = downR;
#endif
    // 松开左键：一次拖动结束 → 记住位置（写回 config，下次启动回到这里）
    if (!down && _dragging)
    {
        _dragging = false;
        _pressed = false;   // 拖动释放不能再算一次点击
        SavePlacement();
        fprintf(stderr, "[glfwpet] drag finished at %d,%d\n", _winX, _winY);
    }
    // 拖动 / 滑动 / 抬起（两条输入源共用这段）；锁定大小和位置时拖动不生效（按下抬起仍算一次点击）
    if (down && _pressed && !_lock)
    {
        const int dx = rx - _pressRootX, dy = ry - _pressRootY;
        if (!_dragging && dx * dx + dy * dy > 25 * (int)(_ui * _ui)) _dragging = true;
        if (_dragging)
        {
            const int nx = _pressWinX + dx, ny = _pressWinY + dy;
            if (nx != _winX || ny != _winY)
            {
                _winX = nx;
                _winY = ny;
                _movedByUser = true;
                glfwSetWindowPos(_win, _winX, _winY);
            }
        }
    }
    else if (!down && !_pressed)
    {
        // 未按住：滑过角色撒闪光（每 ~10 逻辑像素）
        if (wx != _fxLastX || wy != _fxLastY)
        {
            const bool onChar = PointOnCharacter(wx, wy);
            if (_fxLastX >= 0 && onChar)
            {
                const float ddx = (float)(wx - _fxLastX), ddy = (float)(wy - _fxLastY);
                _fxAccum += sqrtf(ddx * ddx + ddy * ddy);
                if (_fxAccum >= 10.0f * _ui)
                {
                    _fxAccum = 0.0f;
                    petpaint::Trail(&_fxv, (float)wx, (float)wy, _ui);
                }
            }
            _fxLastX = wx;
            _fxLastY = wy;
        }
    }
    // 抬起 / 松手：`_pressed` 无条件清掉。
    // 只按 "!down && _wasDown" 判会漏掉**在两帧之间完成的快按**（按下与抬起同落在两次
    // 轮询之间，下一帧只看到 down=false、_wasDown=false）——于是 _pressed 一直挂着，
    // 后面任何一次按下都会被当成"还在拖上一次"，配合陈旧原点就把窗口甩飞。
    if (!down && _pressed)
    {
        if (!_dragging && PointOnCharacter(wx, wy))
        {
            _clickPending = true;
            _clickX = wx;
            _clickY = wy;
            petpaint::Burst(&_fxv, (float)wx, (float)wy, _ui);
        }
        _pressed = false;
        _dragging = false;
    }
    _wasDown = down;
}

bool PortGlfwPet::PollLook(int* x, int* y)
{
    int rx = 0, ry = 0;
    bool down = false, downR = false;
    GlobalMouse(&rx, &ry, &down, &downR);
    if (down) return false;   // 按住拖动中不给视线目标（与 X11 端口一致）
    *x = rx - _winX;
    *y = ry - _winY;
    return true;
}

bool PortGlfwPet::PollClick(int* x, int* y)
{
    if (!_clickPending) return false;
    _clickPending = false;
    *x = _clickX;
    *y = _clickY;
    return true;
}

bool PortGlfwPet::PollWheel(int* delta)
{
    if (_wheel == 0) return false;
    *delta = _wheel;
    _wheel = 0;
    return true;
}

// ---------------------------------------------------------------- 特效推进（叠画）

const uint32_t* PortGlfwPet::FxComposite(const uint32_t* src)
{
    const double now = plat::MonoNow();
    float dt = (float)(now - _fxLast);
    if (_fxLast <= 0.0 || dt > 0.2f) dt = 1.0f / 30.0f;
    _fxLast = now;
    // 缩放会改 _w×_h：缓冲必须跟着重分配
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
    petpaint::Advance(&_fxv, _fxBuf, _w, _h, dt, _ui);
    return _fxBuf;
}

// ---------------------------------------------------------------- 缩放 / 显示隐藏 / 输入区域

/** 至少留在屏幕里的像素：只要一小条就够（**尽量按用户拖的原样记录**——有人就是要把她挂出去）。
 *  真拖到完全看不见了，面板「窗口」页有「拉回屏幕内」一键找回，所以这里不保守（与 X11 端口同规则） */
int PortGlfwPet::ClampKeepVisible(int pos, int lo, int hi, int size)
{
    const int keep = 8;
    const int lowest = lo - size + keep;
    const int highest = hi - keep;
    if (highest <= lowest) return lo;   // 窗口比 keep 还小（极端情况）：退回屏幕起点
    return std::max(lowest, std::min(pos, highest));
}

/** 记住位置：左边 x + 底边距**显示器底边**的距离（缩放以底边为基准，所以存"底"而不是"顶"）。
 *  两个值都允许是负数（挂在屏幕外时左边可以为负、底边距为负 = 有一截在屏幕下方）。
 *  与 X11 端口共用同一份 config 键（pos_l / pos_b / pos_set），两边随时可以接着用。 */
void PortGlfwPet::SavePlacement()
{
    // 首帧显示之前不记录：启动路径里窗口位置/尺寸还要被配置改写（存档落位、缩放），
    // 那会儿记下的是默认位，会把用户上次的位置覆盖掉——实测就是这样把存档顶掉的
    if (!_shownOnce) return;
    int mx = 0, my = 0, mw = 0, mh = 0;
    MonitorBounds(&mx, &my, &mw, &mh);
    if (mw <= 0 || mh <= 0) return;
    char b[160];
    snprintf(b, sizeof(b), "{\"cmd\":\"config\",\"set\":{\"pos_l\":%d,\"pos_b\":%d,\"pos_set\":1}}",
             _winX, (my + mh) - (_winY + _h));
    _portCmds.push_back(b);   // 交给主循环按协议执行（和 X11 端口同一条路）
}

void PortGlfwPet::SetSavedPlacement(int left, int bottomGap, bool valid)
{
    _savedL = left;
    _savedB = bottomGap;
    _savedValid = valid;
    if (!valid || _placementDone || _movedByUser || _win == NULL)
    {
        return;   // 没存过 / 已经落位过 / 用户自己拖过了
    }
    // 窗口在 Initialize 里就已按默认位显示（当初她永远停在默认位），配置一到这里就补上
    ApplySavedPlacement();
}

/** 把窗口挪到存档位置（和启动恢复同一套钳制：允许伸出屏幕，只保证留一块抓得到的）。
 *  选显示器按存档的左边 x（水平坐标是绝对的）：多屏时她挂在哪块屏上就回哪块屏。 */
void PortGlfwPet::ApplySavedPlacement()
{
    if (!_savedValid || _win == NULL) return;
    int mx = 0, my = 0, mw = 0, mh = 0;
    MonitorBoundsAt(_savedL + _w / 2, _winY + _h / 2, &mx, &my, &mw, &mh);
    if (mw <= 0 || mh <= 0) return;
    _placementDone = true;
    const int nx = ClampKeepVisible(_savedL, mx, mx + mw, _w);
    const int ny = ClampKeepVisible(my + mh - _savedB - _h, my, my + mh, _h);
    if (nx == _winX && ny == _winY) return;
    _winX = nx;
    _winY = ny;
#if defined(__linux__)
    Display* dpy = glfwGetX11Display();
    const Window xw = glfwGetX11Window(_win);
    if (dpy != NULL && xw != 0) x11pet::RestorePosition(dpy, xw, _winX, _winY);
    else glfwSetWindowPos(_win, _winX, _winY);
#else
    glfwSetWindowPos(_win, _winX, _winY);
#endif
    _shapeCountdown = 0;   // 位置变了：输入区域下一帧重算
    _anchorOk = false;     // 气泡锚点跟着重算
    fprintf(stderr, "[glfwpet] 还原存档位置 → %d,%d\n", _winX, _winY);   // stderr：GUI 子系统构建下 stdout 会丢
}

/** 面板/托盘「拉回屏幕内」：把她整个挪回工作区（明确的找回动作，放回整块可见）。
 *  与启动恢复的区别：启动按存档原样放、允许伸在屏幕外，见 ApplySavedPlacement。 */
void PortGlfwPet::EnsureOnScreen()
{
    if (_win == NULL) return;
    int mx = 0, my = 0, mw = 0, mh = 0;
    WindowWorkarea(&mx, &my, &mw, &mh);
    if (mw <= 0 || mh <= 0) return;
    const int nx = std::max(mx, std::min(_winX, mx + mw - _w));
    const int ny = std::max(my, std::min(_winY, my + mh - _h));
    if (nx == _winX && ny == _winY) return;
    _winX = nx;
    _winY = ny;
    glfwSetWindowPos(_win, _winX, _winY);
    _shapeCountdown = 0;
    _anchorOk = false;
    SavePlacement();   // 找回后的位置也要记住
    fprintf(stderr, "[glfwpet] 拉回屏幕内 → %d,%d\n", _winX, _winY);
}

void PortGlfwPet::MoveToDefaultSpot()
{
    int mx = 0, my = 0, mw = 0, mh = 0;
    WindowWorkarea(&mx, &my, &mw, &mh);
    _winX = mx + mw - _w - (int)(40 * _ui);
    _winY = my + mh - _h - (int)(60 * _ui);
    glfwSetWindowPos(_win, _winX, _winY);
}

void PortGlfwPet::ApplyZoom(float zoom)
{
    if (zoom < 0.5f) zoom = 0.5f;
    if (zoom > 2.0f) zoom = 2.0f;
    _zoom = zoom;
    // 启动阶段 = 真实渲染尺寸还没定下来：窗口先按 cfg 的尺寸建（360x600），首个 Present 才知道
    // 真实渲染尺寸（480x800），那之后的首次缩放才算把尺寸定下来。这段里存过位置就一律按存档落位，
    // 不能走锚点——否则窗口尺寸一变，她就从存档位平移出去（用户看到的"开机时向右偏移"）。
    const bool settling = _savedValid && !_movedByUser && !_sizeSettled;
    if (_shownOnce && !_sizeSettled) _sizeSettled = true;
    int nw = (int)(_renderW * _zoom + 0.5f) & ~1;
    int nh = (int)(_renderH * _zoom + 0.5f) & ~1;
    {
        int wx = 0, wy = 0, ww = 0, wh = 0;
        WindowWorkarea(&wx, &wy, &ww, &wh);   // 尺寸上限按工作区（别大到盖住任务栏）
        if (ww > 0 && nw > ww) { const float k = (float)ww / (float)nw; nw = ww; nh = (int)(nh * k); }
        if (wh > 0 && nh > wh) { const float k = (float)wh / (float)nh; nh = wh; nw = (int)(nw * k); }
    }
    if (nw < 2) nw = 2;
    if (nh < 2) nh = 2;
    if (nw == _w && nh == _h) return;
    // 位置钳制一律按"至少留一块抓得到"，不夹进工作区：挂在屏幕边上的她不该因为缩放被拽回屏内
    int mx = 0, my = 0, mw = 0, mh = 0;
    MonitorBounds(&mx, &my, &mw, &mh);
    // 锚点一律用**旧**尺寸算（贴边/中点都是相对改尺寸之前的位置），再换到新尺寸
    const int oldW = _w, oldH = _h;
    const bool atLeft = _winX <= mx + 8, atRight = _winX + oldW >= mx + mw - 8;
    const int cx = _winX + oldW / 2, by = _winY + oldH, rx = _winX + oldW;
    _w = nw;
    _h = nh;
    if (settling)
    {
        // 按存档落位（左边 + 底边距，与尺寸无关）：尺寸怎么变都停在同一个位置
        _placementDone = true;
        _winX = ClampKeepVisible(_savedL, mx, mx + mw, _w);
        _winY = ClampKeepVisible(my + mh - _savedB - _h, my, my + mh, _h);
        fprintf(stderr, "[glfwpet] 按存档落位（尺寸定下来后）→ %d,%d\n", _winX, _winY);
    }
    else
    {
        // 锚点：底边不动；水平——贴左边就左边不动、贴右边就右边不动，否则中点不动（与 X11 端口同语义）
        const int nx = atLeft ? _winX : (atRight ? rx - nw : cx - nw / 2);
        _winX = ClampKeepVisible(nx, mx, mx + mw, _w);
        _winY = ClampKeepVisible(by - nh, my, my + mh, _h);
    }
    glfwSetWindowSize(_win, _w, _h);
    glfwSetWindowPos(_win, _winX, _winY);
    _shapeCountdown = 0;
    _anchorOk = false;   // 窗口尺寸变了：气泡锚点得按新的轮廓重算
    _fxBufW = 0;   // 特效/缩放缓冲下一帧重分配
    SavePlacement();   // 用户改缩放：位置跟着变了，一并记下来（首帧之前会被 SavePlacement 自己挡掉）
}

void PortGlfwPet::SetZoom(float zoom)
{
    ApplyZoom(zoom);
}

void PortGlfwPet::SetWindowVisible(bool visible)
{
    if (_win == NULL || visible == !_hidden) return;
    _hidden = !visible;
    if (_hidden)
    {
        HideBubble();
        _bubActive = false;
        if (_bw != NULL) glfwHideWindow(_bw);
        _bubMapped = false;
        glfwHideWindow(_win);
        _fxv.clear();
    }
    else
    {
        glfwShowWindow(_win);
        glfwSetWindowPos(_win, _winX, _winY);
        _shapeCountdown = 0;
    }
    printf("[glfwpet] %s\n", _hidden ? "隐藏" : "显示");
    fflush(stdout);
}

void PortGlfwPet::UpdateInputShape(const uint32_t* px)
{
#if defined(__linux__)
    static const bool off = getenv("L2D_NO_SHAPE") != NULL;   // 调试：跳过输入区域
    if (off) return;
    Display* dpy = glfwGetX11Display();
    if (dpy == NULL || px == NULL) return;
    const Window xw = glfwGetX11Window(_win);
    if (xw == 0) return;
    // 8x8 块：块内有 alpha 的算"可点"；按行合并成矩形（与 X11 端口同款）
    const int BS = 8;
    std::vector<XRectangle> rects;
    for (int by = 0; by < _h; by += BS)
    {
        int runStart = -1;
        for (int bx = 0; bx <= _w; bx += BS)
        {
            bool hit = false;
            if (bx < _w)
            {
                const int x1 = std::min(bx + BS, _w), y1 = std::min(by + BS, _h);
                for (int y = by; y < y1 && !hit; ++y)
                {
                    for (int x = bx; x < x1; ++x)
                    {
                        if (((px[(size_t)y * _w + x] >> 24) & 0xFF) > 0x20)
                        {
                            hit = true;
                            break;
                        }
                    }
                }
            }
            if (hit && runStart < 0) runStart = bx;
            if (!hit && runStart >= 0)
            {
                XRectangle r = { (short)runStart, (short)by, (unsigned short)(bx - runStart), (unsigned short)BS };
                rects.push_back(r);
                runStart = -1;
            }
        }
    }
    if (_passthrough)
    {
        XShapeCombineRectangles(dpy, xw, ShapeInput, 0, 0, NULL, 0, ShapeSet, Unsorted);
    }
    else
    {
        XShapeCombineRectangles(dpy, xw, ShapeInput, 0, 0, rects.empty() ? NULL : &rects[0], (int)rects.size(),
                                ShapeSet, Unsorted);
    }
    if (getenv("L2D_DBG_SHAPE") != NULL)
    {
        fprintf(stderr, "[glfwpet] 输入区域: %zu 块（第 1 块 %d,%d %dx%d）passthrough=%d\n", rects.size(),
                rects.empty() ? -1 : rects[0].x, rects.empty() ? -1 : rects[0].y,
                rects.empty() ? -1 : rects[0].width, rects.empty() ? -1 : rects[0].height, (int)_passthrough);
        fflush(stderr);
    }
    XFlush(dpy);
#else
    (void)px;
#endif
}

// ---------------------------------------------------------------- 面板状态 / 配置

namespace
{

/** 从 JSON 文本 pos 起读一个值（bool 或数字）→ 布尔；pos 传引用前进到 ':' 后 */
bool JsonReadBool(const std::string& s, size_t pos, bool* found)
{
    *found = false;
    if (pos >= s.size()) return false;
    while (pos < s.size() && (s[pos] == ' ' || s[pos] == '\t')) ++pos;
    if (pos >= s.size()) return false;
    if (s.compare(pos, 5, "false") == 0) { *found = true; return false; }
    if (s.compare(pos, 4, "true") == 0) { *found = true; return true; }
    if (s[pos] == '0' || s[pos] == '1' || s[pos] == '-' || (s[pos] >= '0' && s[pos] <= '9'))
    {
        *found = true;
        return atof(s.c_str() + pos) != 0.0;
    }
    return false;
}

}   // namespace

void PortGlfwPet::RefreshFromState()
{
    // 只解析 panel_state 里的 config 段（避免误读 llm 日志/记忆里的同名字段）
    const std::string& all = _panelState;
    if (all.empty()) return;
    size_t p = all.find("\"config\"");
    if (p == std::string::npos) return;
    p = all.find('{', p);
    if (p == std::string::npos) return;
    int depth = 0;
    size_t end = p;
    for (; end < all.size(); ++end)
    {
        if (all[end] == '{') ++depth;
        else if (all[end] == '}')
        {
            --depth;
            if (depth == 0) break;
        }
    }
    const std::string cfg = all.substr(p, end > p ? end - p + 1 : 0);
    // top / passthrough：true/false 或 0/1；zoom：数字
    {
        size_t q = cfg.find("\"top\"");
        if (q != std::string::npos)
        {
            q = cfg.find(':', q);
            bool found = false;
            const bool v = JsonReadBool(cfg, q == std::string::npos ? cfg.size() : q + 1, &found);
            if (found && v != _appliedTop)
            {
                _appliedTop = v;
                glfwSetWindowAttrib(_win, GLFW_FLOATING, v ? GLFW_TRUE : GLFW_FALSE);
#if defined(__linux__)
                Display* modelDpy = glfwGetX11Display();
                const Window modelXw = glfwGetX11Window(_win);
                if (modelDpy != NULL && modelXw != 0) x11pet::ModelType(modelDpy, modelXw, v);
#endif
                if (_bw != NULL)
                {
                    glfwSetWindowAttrib(_bw, GLFW_FLOATING, v ? GLFW_TRUE : GLFW_FALSE);
#if defined(__linux__)
                    Display* dpy = glfwGetX11Display();
                    const Window xw = glfwGetX11Window(_bw);
                    if (dpy != NULL && xw != 0)
                        x11pet::BubbleHints(dpy, xw, glfwGetX11Window(_win), v);
#endif
                }
                fprintf(stderr, "[glfwpet] top=%d (model+bubble)\n", (int)v);
            }
        }
        q = cfg.find("\"passthrough\"");
        if (q != std::string::npos)
        {
            q = cfg.find(':', q);
            bool found = false;
            const bool v = JsonReadBool(cfg, q == std::string::npos ? cfg.size() : q + 1, &found);
            if (found && v != _passthrough)
            {
                if (_dragging) SavePlacement();
                _passthrough = v;
                _pressed = false;
                _dragging = false;
                _wasDown = false;
                _wasDownR = false;
                fprintf(stderr, "[glfwpet] model passthrough=%d\n", (int)v);
                // 穿透要的是"整个角色窗口不吃鼠标，点她穿给**下面的程序**"。WM_NCHITTEST 返回
                // HTTRANSPARENT 只对同一个线程的窗口之间有效，跨进程穿不过去（实测：点她时后面的
                // 资源管理器/记事本收不到），所以这里用 Windows 的整窗穿透 WS_EX_TRANSPARENT
                // （GLFW 的 MOUSE_PASSTHROUGH：Win32 上就是设/清这个扩展样式）。
                // 逐像素命中（HitTestWndProc）只在"没穿透"时用；穿透时它也会返回 HTTRANSPARENT，
                // 两条路一起保证同线程/跨进程都不吃鼠标。
#if !defined(__linux__) && !defined(__APPLE__)
                glfwSetWindowAttrib(_win, GLFW_MOUSE_PASSTHROUGH, v ? GLFW_TRUE : GLFW_FALSE);
#endif
#if defined(__linux__)
                if (v)
                {
                    Display* dpy = glfwGetX11Display();
                    XUngrabPointer(dpy, CurrentTime);   // 清除拖动时的隐式 grab，否则清空 shape 仍会收到鼠标事件
                    XShapeCombineRectangles(dpy, glfwGetX11Window(_win), ShapeInput, 0, 0,
                                            NULL, 0, ShapeSet, Unsorted);
                    XFlush(dpy);
                }
#endif
                // Linux 使用 XShape，不能再叠加 GLFW 整窗穿透；macOS 保留软件命中所需的整窗穿透。
                _shapeCountdown = 0;   // 下一帧重扫输入区域
            }
        }
        q = cfg.find("\"lock\"");
        if (q != std::string::npos)
        {
            q = cfg.find(':', q);
            bool found = false;
            const bool v = JsonReadBool(cfg, q == std::string::npos ? cfg.size() : q + 1, &found);
            if (found && v != _lock)
            {
                _lock = v;
                _dragging = false;   // 锁定瞬间中止进行中的拖动
            }
        }
        q = cfg.find("\"zoom\"");
        if (q != std::string::npos)
        {
            q = cfg.find(':', q);
            if (q != std::string::npos)
            {
                const float z = (float)atof(cfg.c_str() + q + 1);
                if (z >= 0.5f && z <= 2.0f && fabsf(z - _zoom) > 0.01f) ApplyZoom(z);
            }
        }
    }
}

// ---------------------------------------------------------------- UI 进程（l2dpet-ui）

std::string PortGlfwPet::FindUiBinary()
{
    std::vector<std::string> cands;
    if (const char* e = getenv("L2DPET_UI")) cands.push_back(e);
    const std::string self = plat::SelfExePath();
    if (!self.empty())
    {
        const size_t slash = self.find_last_of("/\\");
        if (slash != std::string::npos)
        {
            const std::string dir = self.substr(0, slash);
            cands.push_back(plat::Join(dir, "l2dpet-ui"));
#ifdef _WIN32
            cands.push_back(plat::Join(dir, "l2dpet-ui.exe"));
#endif
            const size_t up = dir.find_last_of("/\\");
            if (up != std::string::npos)
            {
                const std::string base = dir.substr(0, up);
                cands.push_back(plat::Join(plat::Join(base, "build-panel"), "l2dpet-ui"));
            }
        }
    }
    cands.push_back(plat::Join(plat::Join(plat::HomeDir(), "l2d-build/build-panel"), "l2dpet-ui"));
    for (size_t i = 0; i < cands.size(); ++i)
    {
        if (plat::IsFile(cands[i]) && plat::Exists(cands[i])) return cands[i];
    }
    return std::string();
}

void PortGlfwPet::ReapUi()
{
    if (_uiPid <= 0) return;
    bool exited = false;
    int status = 0;
    plat::ReapChild(_uiPid, &exited, &status);
    if (exited)
    {
        _uiPid = -1;
        fprintf(stderr, "[glfwpet] UI 进程退出了（下次打开设置/右键时重启）\n");
    }
}

void PortGlfwPet::LaunchUi(int page)
{
    ReapUi();
    if (page >= 0)
    {
        _uiEvents.push_back("{\"event\":\"ui_open\",\"page\":" + std::to_string(page) + "}");
    }
    if (_uiPid > 0) return;
    const std::string bin = FindUiBinary();
    if (bin.empty())
    {
        fprintf(stderr, "[glfwpet] 找不到 l2dpet-ui（运行 live2d/build-panel.sh；或 L2DPET_UI 指定）——没有托盘/菜单/气泡\n");
        return;
    }
    char scale[32], pg[16];
    snprintf(scale, sizeof(scale), "%.2f", _ui);
    snprintf(pg, sizeof(pg), "%d", page);
    plat::SetEnv("L2DPET_UI_SCALE", scale);
    plat::SetEnv("L2DPET_PAGE", pg);
    const size_t slash = bin.find_last_of("/\\");
    const std::string dir = slash == std::string::npos ? std::string(".") : bin.substr(0, slash);
    const long pid = plat::SpawnDetached(bin, std::vector<std::string>(), dir);
    if (pid > 0)
    {
        _uiPid = pid;
        printf("[glfwpet] 启动 UI 进程 %s（pid %d）\n", bin.c_str(), (int)pid);
        fflush(stdout);
    }
}

void PortGlfwPet::PushRightMenu(int rootX, int rootY)
{
    int mx = 0, my = 0, mw = 0, mh = 0;
    WindowWorkarea(&mx, &my, &mw, &mh);
    const unsigned long t = (unsigned long)(plat::MonoNow() * 1000.0);
    char b[240];
    snprintf(b, sizeof(b), "{\"event\":\"ui_menu\",\"x\":%d,\"y\":%d,\"scale\":%.2f,\"time\":%lu,\"wa\":[%d,%d,%d,%d]}",
             rootX, rootY, _ui, t, mx, my, mw, mh);
    _uiEvents.push_back(b);
    LaunchUi(-1);
    _menuAt = plat::MonoNow();
    _menuOpen = true;
    _menuClearAt = _menuAt + 6.0;   // 兜底：菜单最多活 6s（UI 侧失焦/选项点击都会提前收）
}

void PortGlfwPet::OpenPanel(int page, bool menu)
{
    if (menu)
    {
        PushRightMenu(_winX + _w / 2, _winY + (int)(_h * 0.2f));
    }
    else if (page < 0)
    {
        _uiEvents.push_back("{\"event\":\"ui_close\"}");
    }
    else
    {
        LaunchUi(page);
    }
}

bool PortGlfwPet::PollUiEvent(std::string* json)
{
    if (_uiEvents.empty()) return false;
    *json = _uiEvents.front();
    _uiEvents.pop_front();
    return true;
}

bool PortGlfwPet::PollPanelCommand(std::string* line)
{
    if (_portCmds.empty()) return false;
    *line = _portCmds.front();
    _portCmds.pop_front();
    return true;
}

// ---------------------------------------------------------------- 气泡（GLFW 子窗 + SDF + freetype）

int PortGlfwPet::BubbleOptionAt(double x, double y) const
{
    return petpaint::OptionAt(_bg, x, y, _bs);
}

void PortGlfwPet::BubbleKeyCb(GLFWwindow* w, int key, int sc, int action, int mods)
{
    (void)sc;
    (void)mods;
    PortGlfwPet* p = (PortGlfwPet*)glfwGetWindowUserPointer(w);
    if (p != NULL && action == GLFW_PRESS)
    {
        if (key == GLFW_KEY_ESCAPE) p->HideBubble();
    }
}

void PortGlfwPet::BubbleMouseBtnCb(GLFWwindow* w, int button, int action, int mods)
{
    (void)mods;
    PortGlfwPet* p = (PortGlfwPet*)glfwGetWindowUserPointer(w);
    if (p == NULL || action != GLFW_PRESS) return;
    if (button != GLFW_MOUSE_BUTTON_LEFT) return;
    double cx = 0, cy = 0;
    glfwGetCursorPos(w, &cx, &cy);
    const int k = p->BubbleOptionAt(cx, cy);
    if (k >= 0)
    {
        p->_choicePending = k;
        p->_bubFadeOut = plat::MonoNow();
        p->_bg.opts.clear();
        p->_bg.rectXW.clear();
        p->_bg.rectY.clear();
    }
    else if (p->_bg.opts.empty())
    {
        p->_bubFadeOut = plat::MonoNow();   // 点普通气泡 = 收起
    }
}

bool PortGlfwPet::EnsureBubbleWindow(int w, int h)
{
    if (_bw != NULL)
    {
        glfwSetWindowSize(_bw, w, h);
        return true;
    }
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
#if defined(__APPLE__)
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
#endif
    glfwWindowHint(GLFW_RESIZABLE, GLFW_FALSE);
    glfwWindowHint(GLFW_DECORATED, GLFW_FALSE);
    glfwWindowHint(GLFW_FLOATING, _appliedTop ? GLFW_TRUE : GLFW_FALSE);
    glfwWindowHint(GLFW_MOUSE_PASSTHROUGH, GLFW_FALSE);
    glfwWindowHint(GLFW_TRANSPARENT_FRAMEBUFFER, GLFW_TRUE);
    glfwWindowHint(GLFW_FOCUS_ON_SHOW, GLFW_FALSE);
    glfwWindowHint(GLFW_SCALE_TO_MONITOR, GLFW_FALSE);
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    _bw = glfwCreateWindow(w, h, "l2dpet-bubble", NULL, _win);
    if (_bw == NULL) return false;
    PetHideFromTaskbar(_bw);   // 气泡窗口同样不进任务栏
#if defined(_WIN32)
    // 作为模型的 owned window，一起升降，并保持气泡在模型上方。
    SetWindowLongPtrW(glfwGetWin32Window(_bw), GWLP_HWNDPARENT,
                      (LONG_PTR)glfwGetWin32Window(_win));
#endif
    glfwSetWindowUserPointer(_bw, this);
    glfwSetKeyCallback(_bw, &PortGlfwPet::BubbleKeyCb);
    glfwSetMouseButtonCallback(_bw, &PortGlfwPet::BubbleMouseBtnCb);
    glfwMakeContextCurrent(_bw);
    if (!GlInitSurface(&_bubSurf))
    {
        glfwDestroyWindow(_bw);
        _bw = NULL;
        return false;
    }
#if defined(__linux__)
    {
        Display* dpy = glfwGetX11Display();
        const Window xw = glfwGetX11Window(_bw);
        if (dpy != NULL && xw != 0)
        {
            x11pet::BubbleHints(dpy, xw, glfwGetX11Window(_win), _appliedTop);
            const Atom state = XInternAtom(dpy, "_NET_WM_STATE", False);
            Atom states[3] = { XInternAtom(dpy, "_NET_WM_STATE_SKIP_TASKBAR", False),
                               XInternAtom(dpy, "_NET_WM_STATE_SKIP_PAGER", False),
                               XInternAtom(dpy, "_NET_WM_STATE_ABOVE", False) };
            XChangeProperty(dpy, xw, state, XA_ATOM, 32, PropModeReplace, (unsigned char*)states, _appliedTop ? 3 : 2);
            XFlush(dpy);
        }
    }
#endif
    return true;
}

/** 含该点的显示器；没有就主屏。
 *  原来各处都写 glfwGetPrimaryMonitor()：双屏时角色能拖到第二块屏（拖动不做钳制），
 *  但气泡/缩放/菜单全被按主屏工作区钳回去——气泡就"留在主屏"跟不上。窗口模式没有
 *  glfwGetWindowMonitor（返回 NULL），按窗口中心落在哪块显示器来选。*/
static GLFWmonitor* GlfwMonitorAtPoint(int x, int y)
{
    int n = 0;
    GLFWmonitor** mons = glfwGetMonitors(&n);
    for (int i = 0; i < n; ++i)
    {
        int px = 0, py = 0;
        glfwGetMonitorPos(mons[i], &px, &py);
        const GLFWvidmode* vm = glfwGetVideoMode(mons[i]);
        if (vm != NULL && x >= px && x < px + vm->width && y >= py && y < py + vm->height)
        {
            return mons[i];
        }
    }
    return glfwGetPrimaryMonitor();
}

void PortGlfwPet::WindowWorkarea(int* mx, int* my, int* mw, int* mh)
{
    GLFWmonitor* mon = glfwGetWindowMonitor(_win);
    if (mon == NULL) mon = GlfwMonitorAtPoint(_winX + _w / 2, _winY + _h / 2);
    if (mon == NULL)
    {
        *mx = 0; *my = 0; *mw = 0; *mh = 0;
        return;
    }
    glfwGetMonitorWorkarea(mon, mx, my, mw, mh);
}

/** 含该点的显示器**整屏**范围（不是工作区）：存档位置存的是"底边距屏幕底"，
 *  要按整屏算才对得上（工作区少了任务栏那一条，位置会整体偏移） */
void PortGlfwPet::MonitorBoundsAt(int x, int y, int* mx, int* my, int* mw, int* mh)
{
    GLFWmonitor* mon = GlfwMonitorAtPoint(x, y);
    if (mon == NULL)
    {
        *mx = 0; *my = 0; *mw = 0; *mh = 0;
        return;
    }
    glfwGetMonitorPos(mon, mx, my);
    const GLFWvidmode* vm = glfwGetVideoMode(mon);
    *mw = vm != NULL ? vm->width : 0;
    *mh = vm != NULL ? vm->height : 0;
}

void PortGlfwPet::MonitorBounds(int* mx, int* my, int* mw, int* mh)
{
    MonitorBoundsAt(_winX + _w / 2, _winY + _h / 2, mx, my, mw, mh);
}

/** 角色轮廓（窗口坐标）：top = 最顶不透明行；hMinX/hMaxX/bandH = 头部带（顶部 1/8，含
 *  头发/双马尾）的左右与高度——气泡锚点钉在这条带中线那一行的轮廓边缘上。
 *  算法与 X11 端口的 UpdateInputShape 对齐（那边有 8x8 块矩形可复用，这边直接扫 _lastFrame
 *  的 alpha）。0.5s 缓存一次——只在气泡要锚定时用，逐帧全扫没必要。*/
bool PortGlfwPet::CharMetrics(int* top, int* hMinX, int* hMaxX, int* bandH)
{
    const double now = plat::MonoNow();
    if (!_cmOk || now - _cmAt >= 0.5)
    {
        _cmAt = now;
        _cmOk = false;
        if (_lastFrame != NULL && _w > 0 && _h > 0)
        {
            int t = -1, b = -1;
            for (int y = 0; y < _h; y += 2)
            {
                int rowMin = -1, rowMax = -1;
                const uint32_t* row = _lastFrame + (size_t)y * _w;
                for (int x = 0; x < _w; x += 2)
                {
                    if (((row[x] >> 24) & 0xFF) > 0x20)
                    {
                        if (rowMin < 0) rowMin = x;
                        rowMax = x;
                    }
                }
                if (rowMin < 0) continue;
                if (t < 0) t = y;
                b = y;
            }
            if (t >= 0 && b > t)
            {
                const int band = t + (b - t) / 8;   // 顶部 1/8 = 头部带
                const int midY = t + (b - t) / 16;  // 带的中线：小圆点从这一行的轮廓边缘"长出来"
                int hm = _w, hx = -1, mm = _w, mx2 = -1;
                for (int y = t; y <= band && y < _h; y += 2)
                {
                    const uint32_t* row = _lastFrame + (size_t)y * _w;
                    for (int x = 0; x < _w; x += 2)
                    {
                        if (((row[x] >> 24) & 0xFF) > 0x20)
                        {
                            if (x < hm) hm = x;
                            if (x > hx) hx = x;
                            if (y <= midY + 1 && y + 1 >= midY)
                            {
                                if (x < mm) mm = x;
                                if (x > mx2) mx2 = x;
                            }
                        }
                    }
                }
                _cmTop = t;
                _cmHeadMinX = hm <= hx ? hm : 0;
                _cmHeadMaxX = hm <= hx ? hx : _w;
                _cmBandH = std::max(1, (b - t) / 8);
                _cmMidMinX = mx2 >= mm ? mm : _cmHeadMinX;
                _cmMidMaxX = mx2 >= mm ? mx2 : _cmHeadMaxX;
                _cmOk = true;
            }
        }
    }
    if (!_cmOk) return false;
    *top = _cmTop;
    *hMinX = _cmHeadMinX;
    *hMaxX = _cmHeadMaxX;
    *bandH = _cmBandH;
    return true;
}

/** 气泡锚点：小圆点末端（tipX/tipY）落在**头部带中线那一行的轮廓边缘**——气泡挂在角色
 *  左上/右上角，圆点正好从头发/发带那儿"长出来"（不是吊在正头顶上方）。
 *  锚点只在 ShowBubble / 缩放后算一次就**冻结**（存成窗口相对坐标 `_anchorX/_anchorY`），
 *  之后每帧只随窗口位置平移：拖动角色气泡跟着走，模型自己晃头不带气泡乱抖。
 *  没有轮廓（还没有帧、或纯色帧）时不冻结，下一帧再试——不然会永远停在近似位置。*/
void PortGlfwPet::AnchorBubble()
{
    int mx = 0, my = 0, mw = 0, mh = 0;
    WindowWorkarea(&mx, &my, &mw, &mh);
    if (!_anchorOk)
    {
        int top = 0, hMinX = 0, hMaxX = 0, bandH = 0;
        const bool box = CharMetrics(&top, &hMinX, &hMaxX, &bandH);
        const int gap = (int)(BubbleGapPx() * _bs + 0.5f);
        if (box)
        {
            const int edge = (_side < 0) ? _cmMidMinX : _cmMidMaxX;   // 头部带中线那一行的轮廓边缘
            _anchorX = edge + _side * gap;                            // 圆点末端正好从头发/发带那儿出来
            _anchorY = top + bandH / 2;
            _aTop = top;
            _aBot = top + bandH;
            _aMinX = hMinX;
            _aMaxX = hMaxX;
            _anchorOk = true;
        }
        else
        {
            _anchorX = _w / 2 + _side * (int)(30 * _bs);   // 没有轮廓：退回窗口近似（每帧再试）
            _anchorY = (int)(_h * 0.14f);
        }
    }
    _anchorBox = _anchorOk;
    // 冻结的是"窗口相对"锚点：拖动/缩放时它自己不动，位置由窗口位置带出来
    const int wantX = _winX + _anchorX - _bg.tipX;
    const int wantY = _winY + _anchorY - _bg.tipY;
    _bubX = std::max(mx, std::min(wantX, mx + mw - _bubW));
    _bubY = std::max(my, std::min(wantY, my + mh - _bubH));
    // 兜底：只有被屏幕边钳回来（贴着工作区边缘）才启用——这时可能压到头上，就整体顶到
    // 头顶上方（再不行挪到外侧）。正常位置（锚点在头发边上）不动，锚点也不会因此横跳。
    if (_anchorBox && (_bubX != wantX || _bubY != wantY))
    {
        const int gap = (int)(BubbleGapPx() * _bs + 0.5f);
        const int cTop = _winY + _aTop, cBot = _winY + _aBot;
        const int cMinX = _winX + _aMinX, cMaxX = _winX + _aMaxX;
        if (_bubX < cMaxX + gap && _bubX + _bubW > cMinX - gap && _bubY < cBot + gap && _bubY + _bubH > cTop - gap)
        {
            const int up = cTop - gap - _bubH;
            if (up >= my)
            {
                _bubY = up;
            }
            else if (_side < 0)
            {
                _bubX = std::max(mx, cMinX - gap - _bubW);
            }
            else
            {
                _bubX = std::min(mx + mw - _bubW, cMaxX + gap);
            }
        }
    }
}

void PortGlfwPet::ShowBubble(const char* text, const char* const* options, int optionCount, float seconds)
{
    if (text == NULL || text[0] == '\0' || seconds <= 0.0f)
    {
        HideBubble();
        return;
    }
    // 窗口最小化/隐藏时不弹（同 BubbleTick）：否则会有一个气泡孤零零留在桌面上
    if (_hidden || glfwGetWindowAttrib(_win, GLFW_ICONIFIED))
    {
        HideBubble();
        return;
    }
    int mx = 0, my = 0, mw = 0, mh = 0;
    WindowWorkarea(&mx, &my, &mw, &mh);
    // 缩放只用来定字号；**气泡几何整体跟字号走**（_bs = 字号 / 基准字号）：
    // bubble_px 调大时行高、胶囊、留白、宽度预算一起长，不会"字大气球小"（字挤出行外）。
    _bubFontPx = BubbleBasePx() * std::min(2.0f, std::max(0.5f, _zoom)) * _ui;
    // 极端组合（大字号 × 大缩放）下气泡别长出屏幕：W ≈ 461×s（见 LayoutBubble），
    // 超了就把字号和几何一起收回——两边同步，仍是"字多大、气球多大"。
    {
        const float fitS = (float)mw * 0.9f / 461.0f;
        if (fitS > 0.05f && _bubFontPx / kBubbleBaseFontPx > fitS)
        {
            _bubFontPx = fitS * kBubbleBaseFontPx;
        }
    }
    if (!_font.Ready() && !_font.Load(_bubFontPx))
    {
        return;   // 没字体：气泡画不出文字，索性不弹
    }
    _font.SetPixelSize(_bubFontPx);
    _bs = _bubFontPx / kBubbleBaseFontPx;   // 布局/绘制/命中共用这一号缩放
    _bubText = text;
    petpaint::TextWidthFn tw = [this](const std::string& s) { return _font.TextWidth(s); };
    // 传字体实测行高：行高/胶囊高由它推挤（字号调大 → 气泡跟着长高；写死常量会叠行）
    petpaint::LayoutBubble(&_bg, text, options, optionCount < 0 ? 0 : optionCount, tw, _bs, _font.LineHeight());
    // 左右：角色在屏幕右半边 → 气泡在左（圆点朝右下）
    _side = (_winX + _w / 2 > mx + mw / 2) ? -1 : +1;
    if (_side > 0) petpaint::MirrorBubble(&_bg, _bs);
    if (!EnsureBubbleWindow(_bg.W, _bg.H))
    {
        return;
    }
    _bubW = _bg.W;
    _bubH = _bg.H;
    _anchorOk = false;   // 新气泡：锚点重新算一次（几何/缩放可能变了），再冻结
    AnchorBubble();      // 位置：小圆点末端钉在头部带中线那一行的轮廓边缘（角色左上/右上角）
    const double now = plat::MonoNow();
    _bubShownAt = now;
    _bubUntil = now + (double)seconds;
    _bubFadeOut = -1.0;
    _bubActive = true;
    _hover = -1;
    _choicePending = -1;
    _bubLastA = -1.0f;
}

void PortGlfwPet::HideBubble()
{
    if (_bubActive && _bubFadeOut < 0.0)
    {
        _bubFadeOut = plat::MonoNow();
    }
    _bg.opts.clear();
    _bg.rectXW.clear();
    _bg.rectY.clear();
}

bool PortGlfwPet::BubbleAwaitingChoice() const
{
    return _bubActive && !_bg.opts.empty() && _bubFadeOut < 0.0 && plat::MonoNow() < _bubUntil;
}

bool PortGlfwPet::PollChoice(int* index)
{
    if (_choicePending < 0) return false;
    *index = _choicePending;
    _choicePending = -1;
    return true;
}

void PortGlfwPet::KeepBelowTaskbar()
{
#if defined(_WIN32)
    if (!_appliedTop || _hidden || _win == nullptr) return;
    HWND model = glfwGetWin32Window(_win);
    HWND bubble = _bw != nullptr && _bubMapped ? glfwGetWin32Window(_bw) : nullptr;
    // 只在模型所在显示器的任务栏确实被宠物盖住时调整，不抢焦点。
    const HMONITOR monitor = MonitorFromWindow(model, MONITOR_DEFAULTTONEAREST);
    for (HWND bar = GetTopWindow(nullptr); bar != nullptr; bar = GetWindow(bar, GW_HWNDNEXT))
    {
        wchar_t cls[64] = {};
        GetClassNameW(bar, cls, 64);
        if (wcscmp(cls, L"Shell_TrayWnd") != 0 && wcscmp(cls, L"Shell_SecondaryTrayWnd") != 0) continue;
        if (!IsWindowVisible(bar) || MonitorFromWindow(bar, MONITOR_DEFAULTTONEAREST) != monitor) continue;
        if (!(GetWindowLongPtrW(bar, GWL_EXSTYLE) & WS_EX_TOPMOST)) continue;
        bool above = false;
        for (HWND w = GetWindow(bar, GW_HWNDPREV); w != nullptr; w = GetWindow(w, GW_HWNDPREV))
            if (w == model || w == bubble) { above = true; break; }
        if (!above) return;
        const UINT flags = SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER;
        if (bubble != nullptr) SetWindowPos(bubble, bar, 0, 0, 0, 0, flags);
        SetWindowPos(model, bubble != nullptr ? bubble : bar, 0, 0, 0, 0, flags);
        return;
    }
#endif
}

void PortGlfwPet::BubbleTick()
{
    if (_bw == NULL) return;
    // 窗口被最小化（Windows 的 Win+D / 任务栏最小化；Linux 下 WM 的"显示桌面"= WM_STATE Iconic）
    // 或托盘隐藏 → 气泡一起收掉。气泡是没有任务栏项的工具窗，"显示桌面"不动它，不收就会剩一个
    // 气泡孤零零挂在桌面上（实测）。窗口回来后由下一条消息重新弹。
    if (_hidden || glfwGetWindowAttrib(_win, GLFW_ICONIFIED))
    {
        if (_bubActive || _bubMapped)
        {
            _bubActive = false;
            _bubFadeOut = -1.0;
            _bubMapped = false;
            glfwHideWindow(_bw);
        }
        return;
    }
    const double now = plat::MonoNow();
    if (!_bubActive)
    {
        if (_bubMapped)
        {
            glfwHideWindow(_bw);
            _bubMapped = false;
        }
        return;
    }
    if (_bubFadeOut < 0.0 && now >= _bubUntil) _bubFadeOut = now;   // 到期：开始淡出
    float a = (float)std::min(1.0, (now - _bubShownAt) / 0.18);
    a = a * a * (3.0f - 2.0f * a);
    if (_bubFadeOut >= 0.0)
    {
        const float f = (float)((now - _bubFadeOut) / 0.22);
        if (f >= 1.0f)
        {
            _bubActive = false;
            glfwHideWindow(_bw);
            _bubMapped = false;
            return;
        }
        a *= 1.0f - f;
    }
    // 锚点每帧重算：角色被拖动/缩放后，即使气泡内容与透明度都没变，窗口位置也要跟上
    AnchorBubble();
    const bool bubMoved = (_bubX != _bubLastX || _bubY != _bubLastY);
    if (_bubLastA >= 0.0f && fabsf(a - _bubLastA) < 0.02f && _hover == _bubLastHover && !bubMoved) return;   // 没变化不重画
    _bubLastA = a;
    _bubLastHover = _hover;
    _bubLastX = _bubX;
    _bubLastY = _bubY;
    // 字号必须复位到布局时那一号：滚轮缩放会改 _bs，而 _bg 的胶囊几何是按布局时的 _bs 烘焙的，
    // 不复位就会出现"字比胶囊大"——选项文字互相压在一起（有时能看到的渲染异常）。
    if (_bubFontPx > 0.0f) _font.SetPixelSize(_bubFontPx);   // SetPixelSize 自身幂等（同号直接返回）
    // 背景（SDF）+ 文字（freetype）
    petpaint::RenderBubbleBg(_bg, _bs, _hover, a, &_bubbleBuf);
    {
        const int lineH = _bg.lineH > 0 ? _bg.lineH : (int)(18 * _bs + 0.5f) + (int)(4 * _bs);
        for (size_t i = 0; i < _bg.lines.size(); ++i)
        {
            const int lw = _font.TextWidth(_bg.lines[i]);
            _font.Draw(&_bubbleBuf[0], _bg.W, _bg.H,
                       _bg.contentX + ((int)_bg.cw - lw) / 2, _bg.contentY + (int)(i * lineH), _bg.lines[i],
                       38, 48, 58, a);
        }
        for (size_t r = 0; r < _bg.rectXW.size() && r < _bg.opts.size(); ++r)
        {
            const int ox = _bg.contentX + _bg.rectXW[r].second;
            const int oy = _bg.contentY + petpaint::PillTopY(_bg, r, _bs);   // 每个选项自己的行（换行后各行不同）
            const int tw2 = _font.TextWidth(_bg.opts[r]);
            const unsigned char cr = (int)r == _hover ? 15 : 27;
            const unsigned char cg = (int)r == _hover ? 60 : 78;
            const unsigned char cb = (int)r == _hover ? 80 : 96;
            _font.Draw(&_bubbleBuf[0], _bg.W, _bg.H, ox + (_bg.rectXW[r].first - tw2) / 2,
                       oy + (_bg.pillH - (_bg.fontH > 0 ? _bg.fontH : (int)(18 * _bs))) / 2, _bg.opts[r], cr, cg, cb, a);
        }
    }
    if (!_bubMapped)
    {
        glfwSetWindowPos(_bw, _bubX, _bubY);
        glfwSetWindowSize(_bw, _bubW, _bubH);
#if defined(__linux__)
        Display* dpy = glfwGetX11Display();
        const Window xw = glfwGetX11Window(_bw);
        if (dpy != NULL && xw != 0) x11pet::NoFocusOnMap(dpy, xw);
#endif
        glfwShowWindow(_bw);
        _bubMapped = true;
    }
    else
    {
        glfwSetWindowPos(_bw, _bubX, _bubY);
    }
    glfwMakeContextCurrent(_bw);
    GlUploadSurface(&_bubSurf, _bubbleBuf.empty() ? NULL : &_bubbleBuf[0], _bg.W, _bg.H, _bubW, _bubH);
    glfwSwapBuffers(_bw);
    glfwMakeContextCurrent(_win);   // 切回主窗上下文
}

IPort* CreatePortGlfwPet()
{
    return new PortGlfwPet();
}

#else   // !L2D_WITH_GLFW_PET

IPort* CreatePortGlfwPet()
{
    return NULL;   // 未编入 GLFW 端口
}

#endif

#include "DesktopStartup.h"
// app.cpp — l2dpet-ui：桌宠的全部 UI（EUI-NEO）——托盘、右键菜单、说话气泡、状态与设置窗口
//
// 常驻进程（桌宠启动时拉起）：主窗口 = 状态与设置（启动隐藏、关窗隐藏）；托盘 = EUI 托盘（补丁加了自定义菜单）；
// 右键菜单 / 气泡 = 无边框透明弹出子窗口（补丁加的能力，见 eui-patches/）。桌宠进程只剩角色渲染窗口。
// 桌宠推 ui_* 事件（ui_menu / ui_open / ui_close / ui_close_menu）→ 本进程画；气泡由桌宠进程自己直绘；
// 本进程的操作 → 协议命令（choice / window / quit / config / touch / mood / model …）。
//
// 纯协议客户端：所有数据来自 {"cmd":"panel_state"}，所有操作都是桌宠协议命令（config / touch / mood /
// emotion / model / llm_config …，见 PROTOCOL-LLM.md），和 LLM agent 走同一条路。
// 响应式：窗口宽 ≥760 逻辑像素时侧栏展开、状态页双列；更窄时侧栏收成图标、卡片单列堆叠；控件宽度随窗口走。
// 启动参数走环境变量（EUI 的入口不暴露 argv）：L2DPET_PORT（默认 23333）、L2DPET_PAGE（初始页）、
// L2DPET_UI_SCALE（初始窗口按此放大；桌宠按 Xft.dpi 传过来）。
#include "eui_neo.h"
#include "JsonLite.h"
#include "i18n.h"
#include "petlink.h"
#include "core/window/window_backend.h"
#include "Platform.h"   // plat::LocalTime（localtime_r 的跨平台包装）

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <functional>
#include <mutex>
#include <thread>
#include <map>
#include <string>
#include <vector>

#if !defined(_WIN32) && !defined(__APPLE__)
#include <fcntl.h>      // O_WRONLY（fc-match 子进程的 stderr → /dev/null）
#include <spawn.h>      // 原生文件选择（zenity / kdialog）；fc-match 也走 posix_spawn（不经 shell）
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#elif defined(_WIN32)
// 菜单"点空白处收起"要轮询全局左键（见 Tick）；NOMINMAX 防 min/max 宏顶掉 std::min/std::max
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <mbctype.h>  // _setmbcp / _MB_CP_UTF8
// 注意：这两个必须放在 windows.h **之后**——它们要 windef.h 里的 CALLBACK / LPARAM 等类型，
// 先包含会在 prsht.h 里炸出一堆"CALLBACK 未声明"。WIN32_LEAN_AND_MEAN 又把它们排除了，所以得显式包含。
#include <commdlg.h>   // GetOpenFileNameW / GetSaveFileNameW
#include <shlobj.h>    // SHBrowseForFolderW / SHGetPathFromIDListW
#include <shellapi.h>  // ShellExecuteW（"打开配置/模型目录"）
#endif

namespace app {

namespace {
#if defined(_WIN32)
// 让 CRT 的窄字符文件 API 按 UTF-8 解释路径（同桌宠 main 里那处，见 player/src/Platform.cpp 的说明）。
// 面板的 main 在 EUI 里插不进代码，用静态构造在 main 之前做掉。
namespace { struct WinUtf8Startup { WinUtf8Startup() { _setmbcp(_MB_CP_UTF8); } } g_winUtf8Startup; }
#endif


float EnvF(const char* k, float d)
{
    const char* v = std::getenv(k);
    return (v != nullptr && std::atof(v) > 0.0) ? (float)std::atof(v) : d;
}

int EnvI(const char* k, int d)
{
    // 空值按"没设置"算：atoi("") = 0，会把 L2DPET_PAGE="" 误当成"打开设置窗口"（开机自启时弹面板），
    // 也会把 L2DPET_PORT="" 变成端口 0。
    const char* v = std::getenv(k);
    return (v != nullptr && *v != '\0') ? std::atoi(v) : d;
}

#if !defined(_WIN32) && !defined(__APPLE__)
/** 跑 `fc-match -f '%{file}' <pattern>` 取字体路径。
 *  **不经 shell**：posix_spawn + 管道，参数是数组（与文件选择器同一套做法）；失败/异常都当"没有"。 */
std::string FcMatchPath(const char* pattern)
{
    int fds[2];
    if (pipe(fds) != 0) return {};
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, fds[1], 1);      // 子进程 stdout → 管道
    posix_spawn_file_actions_addclose(&fa, fds[0]);
    posix_spawn_file_actions_addclose(&fa, fds[1]);
    posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);   // stderr 丢掉
    const char* argv[] = { "fc-match", "-f", "%{file}", pattern, nullptr };
    // 环境收窄到 fontconfig 需要的几项（与文件选择器同一套做法；顺便别把整个环境塞给子进程）
    std::vector<std::string> envs;
    const char* kKeep[] = { "HOME", "XDG_CACHE_HOME", "XDG_DATA_HOME", "XDG_CONFIG_HOME", "FONTCONFIG_FILE",
                            "FONTCONFIG_PATH", "LANG", "LC_ALL" };
    for (size_t i = 0; i < sizeof(kKeep) / sizeof(kKeep[0]); ++i)
    {
        const char* v = getenv(kKeep[i]);
        if (v != NULL && *v != '\0') envs.push_back(std::string(kKeep[i]) + "=" + v);
    }
    envs.push_back("PATH=/usr/local/bin:/usr/bin:/bin");
    std::vector<char*> envp;
    for (size_t i = 0; i < envs.size(); ++i) envp.push_back(const_cast<char*>(envs[i].c_str()));
    envp.push_back(NULL);
    pid_t pid = 0;
    const int rc = posix_spawnp(&pid, "fc-match", &fa, nullptr, (char* const*)argv, &envp[0]);
    posix_spawn_file_actions_destroy(&fa);
    close(fds[1]);
    if (rc != 0)
    {
        close(fds[0]);
        return {};
    }
    std::string out;
    char buf[256];
    for (;;)
    {
        const ssize_t n = read(fds[0], buf, sizeof(buf));
        if (n <= 0) break;
        out.append(buf, (size_t)n);
        if (out.size() > 1024) break;   // 防失控
    }
    close(fds[0]);
    int st = 0;
    waitpid(pid, &st, 0);
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
    return out;
}
#endif

/** 正文用系统 CJK 字体（EUI 自带的是标题装饰字体）；找不到就用 EUI 默认 */
std::string SystemCjkFont()
{
    const char* env = std::getenv("L2DPET_FONT");
    if (env != nullptr && *env) return env;
#if defined(_WIN32)
    return i18n::Get() == i18n::Lang::Ja ? "C:/Windows/Fonts/YuGothM.ttc" : "C:/Windows/Fonts/msyh.ttc";
#elif defined(__APPLE__)
    return "/System/Library/Fonts/PingFang.ttc";   // 苹方中日常用同一份
#else
    // 日语用 lang=ja 去挑（kana 字形与汉字写法才是日语的），中文按 zh-cn。
    // 不用 popen/shell：posix_spawn + 管道、参数是数组（见 FcMatchPath）——安全写法，
    // 也不怕以后把语言/字体名做成可配置（现在 L2DPET_FONT 只走上面的 env 分支，不进这里）。
    return FcMatchPath(i18n::Get() == i18n::Lang::Ja ? "sans-serif:lang=ja" : "sans-serif:lang=zh-cn");
#endif
}

} // namespace

namespace { void Tick(); void OnMainWindowReady(core::window::Handle window); }

/** 面板偏好（theme / lang）在启动时先读一次桌宠的 config.json：窗口标题和首帧主题要在
 *  连上桌宠之前就定好（协议里的值连上后会覆盖它，见 ApplyPrefs） */
struct BootPrefs { std::string lang = "zh", theme = "light"; };

const BootPrefs& Boot()
{
    static const BootPrefs b = [] {
        BootPrefs o;
#if defined(_WIN32)
        const char* home = std::getenv("USERPROFILE");
#else
        const char* home = std::getenv("HOME");
#endif
        if (home == nullptr || *home == 0) return o;
        FILE* f = std::fopen((std::string(home) + "/.config/l2dpet/config.json").c_str(), "rb");
        if (f == nullptr) return o;
        std::string text;
        char buf[1024];
        size_t n;
        while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) text.append(buf, n);
        std::fclose(f);
        jsonlite::Value v;
        if (!jsonlite::parse(text.c_str(), text.size(), v, nullptr) || !v.isObj()) return o;
        const jsonlite::Value& str = v.get("str");
        const std::string lang = str.get("lang").strOr(""), theme = str.get("theme").strOr("");
        if (!lang.empty()) o.lang = lang;
        if (theme == "dark" || theme == "auto" || theme == "light") o.theme = theme;
        return o;
    }();
    return b;
}

const DslAppConfig& dslAppConfig()
{
    static const float s = EnvF("L2DPET_UI_SCALE", 1.0f);
    // 语言要在 SystemCjkFont() 之前定：日语挑 lang=ja 的字体（kana 字形才正），中文按 zh-cn
    static const bool langInit = [] { i18n::Set(i18n::FromCode(Boot().lang)); return true; }();
    static const std::string font = SystemCjkFont();
    static const DslAppConfig config = [] {
        const BootPrefs& b = Boot();   // 标题就按存下来的语言出（S.themeMode 由 ApplyPrefs 在首帧前设好）
        DslAppConfig c;
        c.title(i18n::T("桌宠 · 状态与设置"))
            .pageId("l2dpet_panel")
            .clearColor([&b] {   // 首帧清屏色：深色下先闪一下白底太刺眼
                bool dark = b.theme == "dark";
                if (b.theme == "auto")
                {
                    const time_t t = std::time(nullptr);
                    struct tm lt;
                    plat::LocalTime(t, &lt);
                    dark = lt.tm_hour >= 19 || lt.tm_hour < 7;
                }
                return dark ? eui::Color{ 0.082f, 0.090f, 0.133f, 1.0f }      // #151722
                            : eui::Color{ 0.941f, 0.957f, 0.976f, 1.0f };      // #F0F4F9
            }())
            .windowSize((int)(900 * s), (int)(720 * s))
            .minWindowSize((int)(360 * s), (int)(420 * s))
            .fps(60.0)
            .startHidden()                       // 常驻：设置窗口按需显示
            .hideOnClose()                       // 关窗 = 收起，不退出
            .onTick([] { Tick(); }, 0.03)        // 收桌宠事件 / 开关菜单与气泡 / 托盘（主窗口隐藏时也跑）
            // 主窗口就绪：拿到原生句柄，把系统画的标题栏也刷成当前主题（深色下那条白条很扎眼）
            .onWindowReady([](core::window::Handle h) { OnMainWindowReady(h); })
            .tray(true)
            .trayTitle("Live2D 桌宠")
            // 托盘图标按平台给：宿主解析口径不一样，给错了就是"图标不显示"——
            //  · Windows：tray.h 走 ExtractIconEx 取图标 → 必须是 .ico 文件；
            //  · Linux/macOS：SNI 的 IconName 是**主题图标名**，宿主在 IconThemePath 里找 <名字>.png/.svg——
            //    .ico 根本不在它的查找范围内；而且 Linux 包里只有 .png（不带 .ico），
            //    EUI 的 resolveIconPath 也就解析不出绝对路径，只能退回相对名 "."（宿主按它自己的工作目录找，找不到）。
            //    PNG 能被解析成 exe 旁边的绝对路径 → IconName="l2dpet" + 绝对 IconThemePath → 正常显示。
#if defined(_WIN32)
            .trayIcon("l2dpet.ico")
#else
            .trayIcon("l2dpet.png")
#endif
            // 窗口/任务栏/Alt-Tab 的图标是 EUI 运行期从这张 PNG 加载的（跟 exe 内嵌的 .ico 是两条路）：
            // 不显式指定就会用 EUI 自带的 assets/icon.png（蓝色花生），跟我们的图标对不上。
            .iconPath("l2dpet.png");
        if (!font.empty()) c.textFont(font);
        return c;
    }();
    return config;
}

namespace {

using jsonlite::Value;
using Clock = std::chrono::steady_clock;

// 「尝试连接」的结果：后台线程发请求、UI 线程渲染 → 用互斥保护（不塞进 AppState，免得跨线程碰界面状态）
std::mutex g_llmTestMu;
int g_llmTestState = 0;        // 0 没试过 / 1 正在试 / 2 成功 / 3 失败
std::string g_llmTestInfo;     // 成功：协议 + 耗时 + 模型回复样本；失败：错误原文
std::string g_llmTestUrl;      // 实际请求的完整地址（地址填错时一眼看出拼成了什么）

// ---------------------------------------------------------------- 状态
// 页序：0 状态 / 1 行为 / 2 窗口 / 3 模型 / 4 对话 / 5 记忆 / 6 关于 / 7 日志（P_LOG，对话页的子页）
//       8 外观 / 9 外部接入（追加页号 → 外部 agent 用 {"cmd":"panel","page":N} 打开的老页号不受影响）
enum Page { P_STATUS, P_BEHAVIOR, P_WINDOW, P_MODEL, P_LLM, P_MEMORY, P_ABOUT, P_LOG, P_APPEARANCE, P_EXTERNAL };
const int kPageMax = (int)P_EXTERNAL;

struct Local { float v; Clock::time_point at; };
struct LocalStr { std::string v; Clock::time_point at; };

// 主题/语言（面板自己的偏好，存在桌宠配置 config.json 的 str 段：
//   theme = auto / light / dark，lang = zh / en / auto）
// 顺序 = 界面上"日间 / 夜间 / 跟随时间"的分段控件与托盘菜单项 顺序（两处都用这个下标）
enum { THEME_LIGHT = 0, THEME_DARK = 1, THEME_AUTO = 2 };

struct AppState
{
    PetLink link;
    bool started = false;
    std::string raw;          // 上次解析的 panel_state 原文
    Value st;                 // 解析结果（ok:true 时）
    bool connected = false;
    int page = P_STATUS;
    long lastReqSeq = -1;
    std::map<std::string, Local> local;   // 刚改过的配置：本地值优先 1.5s（不等桌宠回推，手感跟手）
    std::map<std::string, LocalStr> localStr;   // 同上，字符串配置（theme / lang / weather_loc）
    std::map<std::string, Clock::time_point> sentAt;
    // 主题 / 语言（面板偏好；"auto" 解析后存在这两个字段里）
    int themeMode = THEME_LIGHT;
    std::string langCode = "zh";
    void* hwnd = nullptr;     // 主窗口原生句柄（onWindowReady 时填；Windows 设深色标题栏用）
    std::map<int, float> pageH;           // 各页内容高度（上一帧排版结果）
    eui::Signal<float> scroll[10];
    bool externalInit = false;
    eui::Signal<std::string> externalPort{ "23333" };
#if defined(_WIN32)
    eui::Signal<std::string> externalPython{ "python" };
#else
    eui::Signal<std::string> externalPython{ "python3" };
#endif
    eui::Signal<std::string> key{ "" }, model{ "" }, url{ "" }, impPath{ "" }, weatherLoc{ "" }, backupPath{ "" };
    std::string hot;            // 当前悬停的演示卡片 id（Interactive Demo 风格：悬停上浮）
    int secN = 0;               // 本页小节序号（PageTitle 清零，Section 递增 → 编号徽章）
    Clock::time_point tryAt;    // 上一次"试一试"的时刻（卡片上的"已发送 ✓"反馈）
    std::string tryId;
    // 名字与称呼（模型页）：输入框；切到别的模型时从状态重新填
    eui::Signal<std::string> nmName{ "" }, nmNick{ "" }, nmCall{ "" };
    eui::Signal<std::string> personaGuidance{ "" };
    std::string personaGuidanceFor;
    std::string nmFor;          // 输入框当前对应的模型指纹
    bool importArmed = false;   // 「导入」二次确认（防手滑覆盖记忆）
    bool fieldsInit = false;
    // LLM 供应商编辑器（对话页）：工作副本 + 当前编辑项 + 字段信号 + 新键入的密钥（按 id）
    struct LlmProvEd
    {
        std::string id, name, url, keyTail, model;
        bool keySet = false;
        std::vector<std::string> models;
    };
    std::vector<LlmProvEd> llmProvs;
    std::string llmEditId;
    bool llmInit = false;
    int llmSeq = 0;
    std::string modelDelName;   // 模型页"删除"已架起待确认的模型名（按名匹配，列表变动不误删）
    std::map<std::string, std::string> llmNewKeys;   // 编辑中新键入的密钥（保存时随请求下发，不落面板）
    eui::Signal<std::string> lpName{ "" }, lpUrl{ "" }, lpKey{ "" }, lpNewModel{ "" };
    std::string toast;
    Clock::time_point toastAt;
    // 右键菜单（弹出窗口 "menu"）
    int menuLevel = 0, menuHover = -1;
    int menuX = 0, menuY = 0;
    int menuW = 0, menuH = 0;   // 菜单窗口尺寸（Win32 判"点在菜单外"用）
    float menuScale = 1.0f;
    unsigned long menuTime = 0;
    Clock::time_point menuOpenedAt;
    int wa[4] = { 0, 0, 1920, 1080 };
    // 气泡：已回到桌宠进程直绘（PortX11Pet.cpp，旧版设计），本进程无气泡状态
    // 托盘
    std::string traySig;
    bool everConnected = false;
    Clock::time_point lostAt;
};

AppState S;

eui::Color C(unsigned v, float a = 1.0f)
{
    return { ((v >> 16) & 255) / 255.0f, ((v >> 8) & 255) / 255.0f, (v & 255) / 255.0f, a };
}

// ---------------------------------------------------------------- 主题（日间 / 夜间 / 跟随时间）
/** 现在是不是深色：跟随时间 = 本地时间 19:00–7:00（日出前/日落后都算夜里） */
bool DarkNow()
{
    if (S.themeMode == THEME_DARK) return true;
    if (S.themeMode == THEME_LIGHT) return false;
    const time_t t = std::time(nullptr);
    struct tm lt;
    plat::LocalTime(t, &lt);
    return lt.tm_hour >= 19 || lt.tm_hour < 7;
}

/** 标题栏（系统画的那条）跟着主题走：深色时用 DWM 的"沉浸式深色标题栏"，否则恢复系统色。
 *  dwmapi 动态取（老系统没这个属性就静默跳过，标题栏保持系统色，面板本身不受影响）。
 *  属性号：20 = Win10 1903+ / Win11；19 = 更早的 1809。两个都试，成功即止。 */
void ApplyTitleBarTheme(void* nativeWindow, bool dark)
{
#if defined(_WIN32)
    if (nativeWindow == NULL) return;
    typedef HRESULT(WINAPI * SetWindowAttributeFn)(HWND, DWORD, LPCVOID, DWORD);
    static SetWindowAttributeFn setAttribute = NULL;
    static bool probed = false;
    if (!probed)
    {
        probed = true;
        if (HMODULE dwm = LoadLibraryW(L"dwmapi.dll"))
        {
            setAttribute = (SetWindowAttributeFn)GetProcAddress(dwm, "DwmSetWindowAttribute");
        }
    }
    if (setAttribute == NULL) return;
    const BOOL on = dark ? TRUE : FALSE;
    if (FAILED(setAttribute((HWND)nativeWindow, 20, &on, sizeof(on))))
    {
        setAttribute((HWND)nativeWindow, 19, &on, sizeof(on));
    }
#else
    (void)nativeWindow;
    (void)dark;
#endif
}

/** 主题一变（含"跟随时间"到点）就刷一次标题栏；没变就不碰（DWM 调用每 30ms 一次没必要） */
int g_titleBarDark = -1;   // -1 = 还没设过
void SyncTitleBar(bool dark)
{
    if (g_titleBarDark == (int)dark) return;
    g_titleBarDark = dark ? 1 : 0;
    ApplyTitleBarTheme(S.hwnd, dark);
}

/** 主窗口就绪（EUI 的 onWindowReady）：记下原生句柄，并把标题栏刷成当前主题。
 *  定义在这儿是因为要用 S / DarkNow；注册处（dslAppConfig 在上文）只转发一下。 */
void OnMainWindowReady(core::window::Handle window)
{
    S.hwnd = core::window::nativeWindowInfo(window).platformWindow;
    SyncTitleBar(DarkNow());
}

/** EUI 组件的主题令牌（输入框/按钮/开关/滑块/滚动条/导航栏都吃它） */
components::theme::ThemeColorTokens Th()
{
    // Interactive Product Demo 风格：品牌色 = 紫罗兰（与粉→紫→蓝的品牌渐变同色系），背景微带紫调
    if (!DarkNow())
    {
        auto t = components::theme::light();
        t.primary = { 0.486f, 0.424f, 1.0f, 1.0f };        // #7C6CFF
        t.background = { 0.957f, 0.961f, 0.988f, 1.0f };   // #F4F5FC
        return t;
    }
    auto t = components::theme::dark();
    t.primary = { 0.545f, 0.478f, 1.0f, 1.0f };            // #8B7AFF（深色底上略提亮）
    t.background = C(0x151722);
    t.surface = C(0x1E2130);
    t.surfaceHover = C(0x262B3F);
    t.surfaceActive = C(0x31374F);
    t.text = C(0xE9EBF5);
    t.border = C(0x2E3348);
    return t;
}

// 调色板（每帧由 ApplyTheme() 按 DarkNow() 刷一遍；见下方两套值）
eui::Color kText, kMuted, kPink, kGreen, kOrange, kPurple, kDanger, kAccent,
           kCard, kBorder, kField, kTrack, kHover, kHoverDanger, kMenuBg, kMenuBorder,
           kCardBorder, kTimeline, kBadgeBase, kWarn, kToastBg, kToastText, kDangerBg, kDangerBorder;
// 品牌渐变（主视觉：粉 → 紫 → 蓝）与投影色——两套主题共用
const eui::Color kBrandA = C(0xFF7EB3), kBrandB = C(0x8B7CFF), kBrandC = C(0x58B4FF), kViolet = C(0x7C6CFF);
eui::Color kShadow, kShadowHot;
// 睡觉回满精力的秒数：与桌宠 PlayerLife.cpp 的 dt / 1200.0f 是同一个数，
// 只用于估算"大约还有多久会自己醒"（两边改一个就要改另一个）
const float kSleepFullSecs = 1200.0f;

void ApplyTheme()
{
    if (!DarkNow())
    {
        kText = C(0x182230); kMuted = C(0x6D7888); kPink = C(0xF77DAA); kGreen = C(0x3CC48A);
        kOrange = C(0xFF9F43); kPurple = C(0x9B8CFF); kDanger = C(0xFF4D6D); kAccent = C(0x54AEFF);
        kCard = C(0xFFFFFF); kBorder = C(0xE6E8F2); kField = C(0xF5F6FC);
        kTrack = C(0xEEF0F7); kHover = C(0xEFF7FE); kHoverDanger = C(0xFFECF0);
        kMenuBg = C(0xFFFFFF); kMenuBorder = C(0xDAE1EA); kCardBorder = C(0xE6E8F2, 0.8f);
        kTimeline = C(0xE6E8F2); kBadgeBase = C(0xFFFFFF); kWarn = C(0xD97706);
        kToastBg = C(0x182230, 0.9f); kToastText = C(0xFFFFFF);
        kDangerBg = C(0xFF4D6D, 0.08f); kDangerBorder = C(0xFF4D6D, 0.30f);
        kShadow = C(0x2A2F6B, 0.08f); kShadowHot = C(0x5B4BDB, 0.20f);
        return;
    }
    // 夜间：紫调深底 + 提亮的彩色（对比度够、又不刺眼）
    kText = C(0xE9EBF5); kMuted = C(0x99A1B8); kPink = C(0xFF8FBB); kGreen = C(0x4ED9A0);
    kOrange = C(0xFFAE5C); kPurple = C(0xAD9FFF); kDanger = C(0xFF6B85); kAccent = C(0x6BBCFF);
    kCard = C(0x1E2130); kBorder = C(0x2E3348); kField = C(0x232739);
    kTrack = C(0x2A2F42); kHover = C(0x262B3F); kHoverDanger = C(0x3A2430);
    kMenuBg = C(0x1B1E2B); kMenuBorder = C(0x30354A); kCardBorder = C(0x30354A, 0.9f);
    kTimeline = C(0x2A2F42); kBadgeBase = C(0x1E2130); kWarn = C(0xF0B45C);
    kToastBg = C(0xF0F2F8, 0.94f); kToastText = C(0x1B1E2B);
    kDangerBg = C(0xFF6B85, 0.16f); kDangerBorder = C(0xFF6B85, 0.45f);
    kShadow = C(0x000000, 0.45f); kShadowHot = C(0x000000, 0.55f);
}


std::string JStr(const std::string& s)
{
    std::string o = "\"";
    for (char c : s)
    {
        if (c == '"' || c == '\\') { o += '\\'; o += c; }
        else if ((unsigned char)c < 0x20) { char b[8]; std::snprintf(b, sizeof(b), "\\u%04x", c); o += b; }
        else o += c;
    }
    return o + "\"";
}

std::string Num(float v)
{
    char b[32];
    std::snprintf(b, sizeof(b), "%.3g", v);
    return b;
}

/** 一位小数（统计里的"平均 x.x 秒"） */
std::string Num1(double v)
{
    char b[32];
    std::snprintf(b, sizeof(b), "%.1f", v);
    return b;
}

void Toast(const std::string& t);   // 定义在下面（SetLangCode 切换语言时要用它提示）

/** 字符串配置读（theme / lang / weather_loc…）：本地刚改过的优先——
 *  桌宠回推了同样的值就撤掉本地覆盖；它一直不回（没连/旧版桌宠）就一直用本地值 */
std::string CfgStr(const char* key, const std::string& def)
{
    const std::string sv = S.st.get("config").get("str").get(key).strOr("");
    auto it = S.localStr.find(key);
    if (it != S.localStr.end())
    {
        if (it->second.v == sv) S.localStr.erase(it);   // 桌宠已经落盘并回推 → 撤掉覆盖
        else return it->second.v;
    }
    return sv.empty() ? def : sv;
}

void SetCfgStr(const std::string& key, const std::string& v)
{
    S.localStr[key] = { v, Clock::now() };
    S.link.send("{\"cmd\":\"config\",\"setstr\":{" + JStr(key) + ":" + JStr(v) + "}}");
}

/** 日间 / 夜间 / 跟随时间 */
void SetThemeMode(int mode)
{
    S.themeMode = std::clamp(mode, 0, 2);
    SetCfgStr("theme", mode == THEME_DARK ? "dark" : (mode == THEME_AUTO ? "auto" : "light"));
    S.traySig.clear();   // 托盘菜单里的勾选项要跟着变
    SyncTitleBar(DarkNow());   // 标题栏立刻跟上，不等下一次 tick
    app::requestUpdate();
}

/** zh / en / auto（auto = 按系统 locale 认） */
void SetLangCode(const std::string& code)
{
    const std::string before = S.langCode;
    S.langCode = code;
    i18n::Set(i18n::FromCode(code));
    SetCfgStr("lang", code);
    S.traySig.clear();
    // 换了语言：她会用新语言重写记忆展示、性格、台词库、题库、笑话（agent 侧检测到 lang 变化就做；
    // 睡着/没 Key 时排队，醒来补做）——这里用**新语言**提示一句
    if (before != code) Toast(i18n::T("已切换语言：她会用新语言重写记忆、性格、台词库、题库和笑话（要配好 API Key，且她醒着时）"));
    app::requestUpdate();
}

/** 每帧对账一次：配置（theme/lang）→ 生效值 */
void ApplyPrefs()
{
    // 还没拿到 panel_state（没连上/刚启动）→ 用启动时从 config.json 读到的值兜底；
    // 拿到配置之后就以配置为准，键不在 = 出厂默认（否则用户删掉配置项后面板会一直停在启动值）
    const BootPrefs& b = Boot();
    const bool live = S.st.isObj() && S.st.get("config").isObj();
    const std::string th = CfgStr("theme", live ? "light" : b.theme);
    S.themeMode = th == "dark" ? THEME_DARK : (th == "auto" ? THEME_AUTO : THEME_LIGHT);
    const std::string lg = CfgStr("lang", live ? "zh" : b.lang);
    S.langCode = lg;
    const i18n::Lang want = i18n::FromCode(lg);
    if (want != i18n::Get()) i18n::Set(want);
}

float Cfg(const char* key, float def)
{
    auto it = S.local.find(key);
    if (it != S.local.end() && Clock::now() - it->second.at < std::chrono::milliseconds(1500)) return it->second.v;
    const Value& v = S.st.get("config").get(key);
    return v.isNum() ? (float)v.num : def;
}

void SetCfg(const std::string& key, float v, bool throttle = false)
{
    S.local[key] = { v, Clock::now() };
    auto& last = S.sentAt[key];
    if (throttle && Clock::now() - last < std::chrono::milliseconds(120)) return;
    last = Clock::now();
    S.link.send("{\"cmd\":\"config\",\"set\":{" + JStr(key) + ":" + Num(v) + "}}");
}

void Toast(const std::string& t)
{
    S.toast = t;
    S.toastAt = Clock::now();
}

#if defined(_WIN32)
/** UTF-8 <-> UTF-16：Win32 的 W 版 API 要宽字符，工程里的字符串一律 UTF-8。 */
static std::wstring WideFromUtf8(const std::string& s)
{
    if (s.empty()) return std::wstring();
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), NULL, 0);
    if (n <= 0) return std::wstring();
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}

static std::string Utf8FromWide(const wchar_t* w)
{
    if (w == NULL || w[0] == 0) return std::string();
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    if (n <= 0) return std::string();
    std::string s((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, &s[0], n, NULL, NULL);
    s.pop_back();   // 去掉结尾的 '\0'
    return s;
}
#endif

/** 用系统文件管理器打开一个目录（配置目录 / 模型库）：Windows 走 W 版 API——用户名是中文时
 *  窄字符 ShellExecuteA 打不开；macOS/Linux 交给 EUI 的 openUrl（open / xdg-open）。*/
static void OpenFolder(const std::string& dir)
{
    if (dir.empty()) return;
#if defined(_WIN32)
    const std::wstring w = WideFromUtf8(dir);
    if (!w.empty()) ShellExecuteW(nullptr, L"open", w.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#else
    eui::platform::openUrl(dir);
#endif
}

/** 桌宠的配置目录 / 模型库目录：优先用运行中桌宠上报的路径（panel_state.dirs），
 *  没连上时按同一套约定自己拼（见 player/src/L2DPlayer.h 的 ConfigDirPath / ModelsLibDir）。*/
static std::string PetHomeDir()
{
#if defined(_WIN32)
    const char* home = std::getenv("USERPROFILE");
#else
    const char* home = std::getenv("HOME");
#endif
    return std::string(home != nullptr && *home ? home : ".");
}

static std::string PetConfigDir()
{
    const std::string fromPet = S.st.get("dirs").get("config").strOr("");
    return fromPet.empty() ? PetHomeDir() + "/.config/l2dpet" : fromPet;
}

static std::string PetModelsDir()
{
    const std::string fromPet = S.st.get("dirs").get("models").strOr("");
    return fromPet.empty() ? PetHomeDir() + "/.local/share/l2dpet/models" : fromPet;
}

/** Absolute-path test: '/...' (POSIX), 'X:\' or 'X:/' (drive), '\\server\share' (UNC).
 *  Only '/'-leading used to count, so on Windows a typed drive path - and a UNC network path
 *  in particular - was rejected as "not absolute".
 *  (char)92 = backslash, written this way to keep the source free of stray escapes. */
static bool IsAbsPath(const std::string& p)
{
    if (p.empty()) return false;
    if (p[0] == '/') return true;
#if defined(_WIN32)
    const char B = (char)92;
    if (p.size() >= 3 && p[1] == ':' && (p[2] == B || p[2] == '/') &&
        ((p[0] >= 'A' && p[0] <= 'Z') || (p[0] >= 'a' && p[0] <= 'z')))
    {
        return true;
    }
    if (p.size() >= 2 && p[0] == B && p[1] == B) return true;
#endif
    return false;
}

// ---------------------------------------------------------------- 原生文件选择（导出/导入/选模型包）
// Linux：优先 zenity（GNOME 原生 GTK 文件管理器），退回 kdialog（KDE）；两者都没装 → -1，
// 调用方回退成"用输入框里手填的路径"。Win/Mac 暂时返回 -1（以后接 IFileOpenDialog / NSOpenPanel）。
// 用 posix_spawn + 管道取 stdout，不经 shell（路径里有引号/空格/分号都安全）。
// 返回：1 = 选好了（*out 是路径）；0 = 用户取消；-1 = 没有可用的选择器/启动失败。
int PickPathNative(bool save, bool dirOnly, const std::string& suggested, const std::string& title, std::string* out)
{
#if defined(_WIN32)
    // 系统原生对话框：文件用 comdlg32 的 Get(Open|Save)FileNameW，选目录用 shell32 的 SHBrowseForFolderW。
    // 这段跑在后台线程里（见 PickPathAsync）——它们都是模态阻塞的，正好不卡 UI 线程。
    // OFN_NOCHANGEDIR 很重要：别让对话框把进程当前目录改掉（桌宠按工作目录找 assets/l2dpet.png）。
    const std::wstring wtitle = WideFromUtf8(title);
    const int coInit = (CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED) == S_OK) ? 1 : 0;
    int rc = 0;   // 默认 = 取消
    if (dirOnly)
    {
        BROWSEINFOW bi;
        memset(&bi, 0, sizeof(bi));
        bi.lpszTitle = wtitle.empty() ? NULL : wtitle.c_str();
        bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;   // 新式外观：可拉宽、可手输路径
        LPITEMIDLIST idl = SHBrowseForFolderW(&bi);
        if (idl != NULL)
        {
            wchar_t buf[MAX_PATH] = { 0 };
            if (SHGetPathFromIDListW(idl, buf) && buf[0] != 0)
            {
                *out = Utf8FromWide(buf);
                rc = 1;
            }
            CoTaskMemFree(idl);
        }
    }
    else
    {
        wchar_t buf[4096] = { 0 };   // 长路径留足余量
        const std::wstring ws = WideFromUtf8(suggested);
        if (!ws.empty()) wcsncpy(buf, ws.c_str(), (sizeof(buf) / sizeof(buf[0])) - 1);
        OPENFILENAMEW ofn;
        memset(&ofn, 0, sizeof(ofn));
        ofn.lStructSize = sizeof(ofn);
        ofn.lpstrFilter = L"模型包 (*.zip)\0*.zip\0所有文件 (*.*)\0*.*\0";
        ofn.lpstrFile = buf;
        ofn.nMaxFile = (DWORD)(sizeof(buf) / sizeof(buf[0]));
        ofn.lpstrTitle = wtitle.empty() ? NULL : wtitle.c_str();
        ofn.Flags = OFN_EXPLORER | OFN_NOCHANGEDIR | OFN_PATHMUSTEXIST |
                    (save ? OFN_OVERWRITEPROMPT : OFN_FILEMUSTEXIST);
        const BOOL ok = save ? GetSaveFileNameW(&ofn) : GetOpenFileNameW(&ofn);
        if (ok)
        {
            *out = Utf8FromWide(buf);
            rc = 1;
        }
    }
    if (coInit) CoUninitialize();
    return rc;
#elif defined(__APPLE__)
    (void)save; (void)dirOnly; (void)suggested; (void)title; (void)out;
    return -1;   // macOS：以后接 NSOpenPanel。
#else
    const char* kZenity = "/usr/bin/zenity";
    const char* kKdialog = "/usr/bin/kdialog";
    std::vector<std::string> args;
    if (access(kZenity, X_OK) == 0)
    {
        args.push_back(kZenity);
        args.push_back("--file-selection");
        if (save) args.push_back("--save");
        if (dirOnly) args.push_back("--directory");
        args.push_back("--title=" + (title.empty() ? std::string("选择文件") : title));
        if (!suggested.empty()) args.push_back("--filename=" + suggested);
    }
    else if (access(kKdialog, X_OK) == 0)
    {
        args.push_back(kKdialog);
        args.push_back(dirOnly ? "--getexistingdirectory" : (save ? "--getsavefilename" : "--getopenfilename"));
        args.push_back(suggested.empty() ? std::string(".") : suggested);
    }
    else
    {
        return -1;
    }
    int fds[2];
    if (pipe(fds) != 0) return -1;
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, fds[1], 1);      // 子进程 stdout → 管道
    posix_spawn_file_actions_addclose(&fa, fds[0]);
    posix_spawn_file_actions_addclose(&fa, fds[1]);
    std::vector<char*> argv;
    for (size_t i = 0; i < args.size(); ++i) argv.push_back(const_cast<char*>(args[i].c_str()));
    argv.push_back(NULL);
    // 环境：只挑 GTK 真需要的几项（家里各种 LC_*/GTK_*/GSETTINGS_* 会让 zenity 起不来——
    // 实测本机带完整环境时它立刻退出、给干净环境才能正常弹窗）
    std::vector<std::string> envs;
    const char* kKeep[] = { "DISPLAY", "XAUTHORITY", "XDG_RUNTIME_DIR", "DBUS_SESSION_BUS_ADDRESS", "HOME" };
    for (size_t i = 0; i < sizeof(kKeep) / sizeof(kKeep[0]); ++i)
    {
        const char* v = getenv(kKeep[i]);
        if (v != NULL && *v != '\0') envs.push_back(std::string(kKeep[i]) + "=" + v);
    }
    envs.push_back("PATH=/usr/local/bin:/usr/bin:/bin");
    envs.push_back("LC_ALL=C.UTF-8");      // 固定 locale：中文 locale 缺数据时 GTK 会直接退出
    std::vector<char*> envp;
    for (size_t i = 0; i < envs.size(); ++i) envp.push_back(const_cast<char*>(envs[i].c_str()));
    envp.push_back(NULL);
    pid_t pid = 0;
    const int rc = posix_spawn(&pid, args[0].c_str(), &fa, NULL, &argv[0], &envp[0]);
    posix_spawn_file_actions_destroy(&fa);
    close(fds[1]);
    if (rc != 0)
    {
        close(fds[0]);
        std::fprintf(stderr, "[picker] posix_spawn 失败 rc=%d\n", rc);
        return -1;
    }
    std::string picked;
    char buf[1024];
    ssize_t n;
    while ((n = read(fds[0], buf, sizeof(buf))) > 0) picked.append(buf, (size_t)n);
    close(fds[0]);
    int status = 0;
    waitpid(pid, &status, 0);
    while (!picked.empty() && (picked[picked.size() - 1] == '\n' || picked[picked.size() - 1] == '\r'))
    {
        picked.erase(picked.size() - 1);
    }
    const bool exitedOk = WIFEXITED(status) && WEXITSTATUS(status) == 0;
    std::fprintf(stderr, "[picker] %s 退出 status=%d 选中=%s\n", args[0].c_str(), status,
                 picked.empty() ? "(空)" : picked.c_str());
    if (!exitedOk || picked.empty() || picked[0] != '/')
    {
        return 0;   // 取消（zenity 取消时 rc=1）
    }
    *out = picked;
    return 1;
#endif
}

// 文件对话框放后台线程：zenity 是模态的、等主人点完才返回——放在 UI 线程上会让托盘/右键菜单/说话气泡
// 全部卡住（事件堆着不处理）。结果由 Tick() 里的 PollPick() 在 UI 线程回调。同一时间只开一个。
struct PickJob
{
    std::mutex mu;
    bool busy = false, done = false;
    int r = 0;
    std::string path;
    std::function<void(int, const std::string&)> then;
};
PickJob g_pick;

void PickPathAsync(bool save, bool dirOnly, const std::string& suggested, const std::string& title,
                   std::function<void(int, const std::string&)> then)
{
    {
        std::lock_guard<std::mutex> lk(g_pick.mu);
        if (g_pick.busy)
        {
            Toast("文件对话框已经开着");
            return;
        }
        g_pick.busy = true;
        g_pick.done = false;
        g_pick.then = std::move(then);
    }
    std::thread([save, dirOnly, suggested, title] {
        std::string out;
        const int r = PickPathNative(save, dirOnly, suggested, title, &out);
        {
            std::lock_guard<std::mutex> lk(g_pick.mu);
            g_pick.r = r;
            g_pick.path = out;
            g_pick.done = true;
        }
        app::requestUpdate();
    }).detach();
}

void PollPick()
{
    std::function<void(int, const std::string&)> fn;
    int r = 0;
    std::string path;
    {
        std::lock_guard<std::mutex> lk(g_pick.mu);
        if (!g_pick.done) return;
        g_pick.done = false;
        g_pick.busy = false;
        fn.swap(g_pick.then);
        r = g_pick.r;
        path = g_pick.path;
    }
    if (fn) fn(r, path);
    app::requestUpdate();
}

std::string EmotionZh(const std::string& e)
{
    static const std::map<std::string, std::string> k = {
        { "neutral", "平静" }, { "calm", "安心" }, { "happy", "开心" }, { "excited", "兴奋" }, { "shy", "害羞" },
        { "affectionate", "亲昵" }, { "curious", "好奇" }, { "confused", "困惑" }, { "tired", "疲惫" },
        { "sad", "难过" }, { "anxious", "紧张" }, { "angry", "生气" }, { "surprised", "惊讶" },
        { "sulky", "赌气" }, { "content", "愉快" }, { "uneasy", "不安" }, { "low", "有点低落" } };
    auto it = k.find(e);
    return i18n::T(it != k.end() ? it->second : e);
}

std::string StateZh(const std::string& s)
{
    if (s == "drowsy") return i18n::T("犯困");
    if (s == "asleep") return i18n::T("睡着了");
    if (s == "sulk") return i18n::T("在生闷气");
    return i18n::T("醒着");
}

// ---------------------------------------------------------------- 排版小工具（逻辑像素；y 游标自上而下）
struct Pen
{
    eui::Ui& ui;
    float x, w, y;
};

/** 估算文字宽度（像素）：CJK ≈ 1em/字、ASCII ≈ 0.56em/字，再乘 1.06 的松量——
 *  与原来"字节数 × 4.6"对中文完全一致（1 汉字 3 字节 = 13.8px @13px 字号），
 *  但英文不再按字节低估（英文界面里按钮/胶囊宽度靠它） */
float TextW(const std::string& str, float fs)
{
    float units = 0.0f;
    for (size_t i = 0; i < str.size();)
    {
        const unsigned char c = (unsigned char)str[i];
        const size_t n = c < 0x80 ? 1 : ((c & 0xE0) == 0xC0 ? 2 : ((c & 0xF0) == 0xE0 ? 3 : 4));
        units += n == 1 ? 0.56f : 1.0f;
        i += n;
    }
    return units * fs * 1.06f;
}

float TextH(const std::string& raw, float width, float fs, float lh)
{
    const std::string s = i18n::T(raw);
    // 粗估折行高度（CJK 约 1em/字，ASCII 约 0.55em）
    float units = 0.0f;
    for (size_t i = 0; i < s.size();)
    {
        const unsigned char c = (unsigned char)s[i];
        const size_t n = c < 0x80 ? 1 : ((c & 0xE0) == 0xC0 ? 2 : ((c & 0xF0) == 0xE0 ? 3 : 4));
        if (s[i] == '\n') units = std::ceil(units / std::max(1.0f, width / fs)) * (width / fs);
        units += n == 1 ? 0.56f : 1.0f;
        i += n;
    }
    const float perLine = std::max(1.0f, width / fs);
    return std::max(1.0f, std::ceil(units / perLine)) * lh;
}

void Text(eui::Ui& ui, const std::string& id, float x, float y, float w, float h, const std::string& t, float fs,
          eui::Color col, int weight = 400, bool wrap = false, core::HorizontalAlign al = core::HorizontalAlign::Left)
{
    ui.text(id).position(x, y).size(w, h).text(i18n::T(t)).fontSize(fs).lineHeight(fs * 1.4f).fontWeight(weight)
        .color(col).wrap(wrap).horizontalAlign(al).build();
}

void Card(eui::Ui& ui, const std::string& id, float x, float y, float w, float h)
{
    ui.rect(id).position(x, y).size(w, h).color(kCard).radius(18.0f).border(1.0f, kCardBorder)
        .shadow(28.0f, 0.0f, 10.0f, kShadow).build();
}

eui::Color Mix(eui::Color a, eui::Color b, float t)
{
    return { a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t };
}

/** 图标字形（Font Awesome 7 Solid） */
void Icon(eui::Ui& ui, const std::string& id, float x, float y, float box, unsigned cp, float fs, eui::Color col)
{
    const float lh = fs * 1.25f;   // 字形按行盒顶对齐：自己算偏移让它在 box 里垂直居中
    ui.text(id).position(x, y + (box - lh) / 2.0f).size(box, lh).icon(cp).fontSize(fs).lineHeight(lh).color(col)
        .horizontalAlign(core::HorizontalAlign::Center).build();
}

/** 圆角图标徽章：浅色底 + 彩色图标 */
void IconBadge(eui::Ui& ui, const std::string& id, float x, float y, float box, unsigned cp, eui::Color col)
{
    ui.rect(id + ".bg").position(x, y).size(box, box).gradient(Mix(col, kBadgeBase, 0.78f), Mix(col, kBadgeBase, 0.9f))
        .radius(box * 0.32f).build();
    Icon(ui, id + ".i", x, y, box, cp, box * 0.44f, col);
}

void Chip(eui::Ui& ui, const std::string& id, float x, float y, const std::string& raw, eui::Color col, float* outW = nullptr)
{
    const std::string t = i18n::T(raw);
    const float w = 22.0f + TextW(t, 13.0f);
    ui.rect(id + ".bg").position(x, y).size(w, 26.0f).color({ col.r, col.g, col.b, 0.12f }).radius(13.0f)
        .border(1.0f, { col.r, col.g, col.b, 0.22f }).build();
    Text(ui, id + ".t", x, y + 3.0f, w, 20.0f, t, 13.0f, Mix(col, kText, 0.25f), 600, false, core::HorizontalAlign::Center);
    if (outW) *outW = w;
}

void Section(Pen& p, const std::string& id, const std::string& t)
{
    // 产品演示式小节头：渐变编号徽章 + 标题（"01 反应"）
    char n[8];
    std::snprintf(n, sizeof(n), "%02d", ++S.secN);
    p.ui.rect(id + ".no.bg").position(p.x, p.y).size(34.0f, 24.0f).gradient(kBrandA, kBrandB, core::GradientDirection::Horizontal)
        .radius(12.0f).build();
    Text(p.ui, id + ".no", p.x, p.y + 3.0f, 34.0f, 18.0f, n, 12.0f, C(0xFFFFFF), 800, false, core::HorizontalAlign::Center);
    Text(p.ui, id, p.x + 44.0f, p.y + 1.0f, p.w - 44.0f, 24.0f, t, 16.0f, kText, 700);
    p.y += 38.0f;
}

void Divider(eui::Ui& ui, const std::string& id, float x, float y, float w)
{
    ui.rect(id).position(x + 16.0f, y).size(w - 32.0f, 1.0f).color(kBorder).build();
}

/** 开关行（卡片内，高 56） */
void ToggleRow(Pen& p, const std::string& id, const std::string& label, const std::string& hint, const char* key)
{
    const bool on = Cfg(key, 0.0f) > 0.5f;
    Text(p.ui, id + ".l", p.x + 18.0f, p.y + (hint.empty() ? 17.0f : 9.0f), p.w - 110.0f, 22.0f, label, 15.0f, kText, 500);
    if (!hint.empty())
        Text(p.ui, id + ".h", p.x + 18.0f, p.y + 31.0f, p.w - 110.0f, 20.0f, hint, 12.5f, kMuted);
    const std::string k = key;
    p.ui.stack(id + ".sw.wrap").position(p.x + p.w - 74.0f, p.y + 14.0f).size(56.0f, 28.0f).content([&] {
        components::toggleSwitch(p.ui, id + ".sw").theme(Th()).size(56.0f, 28.0f).checked(on)
            .onChange([k](bool v) { SetCfg(k, v ? 1.0f : 0.0f); }).build();
    }).build();
    p.y += 56.0f;
}

/** 滑块行（卡片内，高 56）：值域 [lo,hi]，step>0 取整到步长；fmt 生成右侧数值文字 */
void SliderRow(Pen& p, const std::string& id, const std::string& label, const char* key, float lo, float hi, float step,
               const std::function<std::string(float)>& fmt)
{
    const float v = Cfg(key, lo);
    const bool narrow = p.w < 520.0f;
    const float labelW = narrow ? p.w - 36.0f : std::min(220.0f, p.w * 0.38f);
    Text(p.ui, id + ".l", p.x + 18.0f, p.y + (narrow ? 6.0f : 17.0f), labelW, 22.0f, label, 15.0f, kText, 500);
    const float sx = narrow ? p.x + 18.0f : p.x + 18.0f + labelW + 12.0f;
    const float sw = narrow ? p.w - 36.0f - 72.0f : p.w - (sx - p.x) - 18.0f - 72.0f;
    const float sy = narrow ? p.y + 30.0f : p.y + 14.0f;
    const std::string k = key;
    p.ui.stack(id + ".s.wrap").position(sx, sy).size(std::max(80.0f, sw), 28.0f).content([&] {
        components::slider(p.ui, id + ".s").theme(Th()).size(std::max(80.0f, sw), 28.0f)
            .value((v - lo) / (hi - lo))
            .onChange([k, lo, hi, step](float t) {
                float val = lo + (hi - lo) * t;
                if (step > 0.0f) val = std::round(val / step) * step;
                SetCfg(k, val, true);
            })
            .build();
    }).build();
    Text(p.ui, id + ".v", sx + std::max(80.0f, sw) + 10.0f, sy + 3.0f, 64.0f, 22.0f, fmt(v), 14.0f, kMuted, 500,
         false, core::HorizontalAlign::Right);
    p.y += narrow ? 68.0f : 56.0f;
}

/** 行高（卡片要先按这个算好高度再画——同 id 画两次会把后画的盖到内容上） */
float ToggleH() { return 56.0f; }
float SliderH(float w) { return w < 520.0f ? 68.0f : 56.0f; }

/** 按钮流：一行放不下自动换行（响应式） */
struct ButtonFlow
{
    eui::Ui& ui;
    float x0, x, y, maxX;
    void add(const std::string& id, const std::string& labelRaw, std::function<void()> fn, bool primary = false,
             float h = 38.0f)
    {
        const std::string label = i18n::T(labelRaw);
        const float w = 30.0f + TextW(label, 14.0f);
        if (x + w > maxX && x > x0)
        {
            x = x0;
            y += h + 10.0f;
        }
        components::button(ui, id).theme(Th(), primary).position(x, y).size(w, h).text(label).fontSize(14.0f)
            .onClick(std::move(fn)).build();
        x += w + 10.0f;
    }
};

void Bar(eui::Ui& ui, const std::string& id, float x, float y, float w, const std::string& label, float v, eui::Color col)
{
    Text(ui, id + ".l", x, y, w - 50.0f, 22.0f, label, 14.5f, kText, 500);
    char b[16];
    std::snprintf(b, sizeof(b), "%d", (int)std::lround(std::clamp(v, 0.0f, 1.0f) * 100.0f));
    Text(ui, id + ".n", x + w - 50.0f, y, 50.0f, 22.0f, b, 14.0f, kMuted, 500, false, core::HorizontalAlign::Right);
    ui.rect(id + ".track").position(x, y + 28.0f).size(w, 10.0f).color(kTrack).radius(5.0f).build();
    if (v > 0.005f)
        ui.rect(id + ".fill").position(x, y + 28.0f).size(std::max(10.0f, w * std::clamp(v, 0.0f, 1.0f)), 10.0f)
            .gradient(Mix(col, C(0xFFFFFF), 0.35f), col, core::GradientDirection::Horizontal).radius(5.0f)
            .shadow(8.0f, 0.0f, 2.0f, { col.r, col.g, col.b, 0.30f })
            .transition(0.45f).animate(core::AnimProperty::Frame).build();   // 数值变化时平滑滑过去
}

// ---------------------------------------------------------------- 页面
void PageTitle(Pen& p, const std::string& t, const std::string& sub = "")
{
    S.secN = 0;
    // 眉题（品牌色小字）+ 大标题 + 渐变短线 + 副标题
    Text(p.ui, "eyebrow", p.x, p.y, p.w, 18.0f, "LIVE2D 桌宠 · 设置", 12.0f, kViolet, 700);
    p.y += 22.0f;
    Text(p.ui, "title", p.x, p.y, p.w, 40.0f, t, 30.0f, kText, 800);
    p.y += 44.0f;
    p.ui.rect("title.bar").position(p.x, p.y).size(44.0f, 4.0f)
        .gradient(kBrandA, kBrandC, core::GradientDirection::Horizontal).radius(2.0f).build();
    p.y += 14.0f;
    if (!sub.empty())
    {
        Text(p.ui, "subtitle", p.x, p.y, p.w, 22.0f, sub, 14.5f, kMuted);
        p.y += 30.0f;
    }
    p.y += 14.0f;
}

/** 演示卡片（Interactive Product Demo）：图标徽章 + 标题 + 一句说明 + "试一试 →"；悬停上浮加深投影，
 *  点一下立即发命令，右下角变成"已发送 ✓"1.6 秒——桌面上的她同时有反应，形成"所见即所得"的演示感 */
void DemoCard(eui::Ui& ui, const std::string& id, float x, float y, float w, float h, unsigned icon,
              const std::string& title, const std::string& desc, eui::Color col, const std::string& cmd)
{
    const bool hot = S.hot == id;
    const bool sent = S.tryId == id && Clock::now() - S.tryAt < std::chrono::milliseconds(1600);
    const float dy = hot ? -4.0f : 0.0f;
    ui.rect(id + ".bg").position(x, y + dy).size(w, h).color(kCard).radius(18.0f)
        .border(1.0f, hot ? Mix(col, C(0xFFFFFF), 0.55f) : kCardBorder)
        .shadow(hot ? 30.0f : 22.0f, 0.0f, hot ? 14.0f : 8.0f, hot ? eui::Color{ col.r, col.g, col.b, 0.22f } : kShadow)
        .build();
    // 左上角一抹彩色光（卡片的"主题色"）
    ui.rect(id + ".glow").position(x + 1.0f, y + dy + 1.0f).size(w - 2.0f, 46.0f)
        .gradient(eui::Color{ col.r, col.g, col.b, hot ? 0.12f : 0.06f }, eui::Color{ col.r, col.g, col.b, 0.0f })
        .radius(17.0f).build();
    IconBadge(ui, id + ".ic", x + 18.0f, y + dy + 18.0f, 42.0f, icon, col);
    Text(ui, id + ".t", x + 72.0f, y + dy + 18.0f, w - 90.0f, 22.0f, title, 16.0f, kText, 700);
    Text(ui, id + ".d", x + 72.0f, y + dy + 42.0f, w - 90.0f, 20.0f, desc, 12.5f, kMuted);
    Text(ui, id + ".go", x + 18.0f, y + dy + h - 32.0f, w - 36.0f, 20.0f, sent ? "已发送 ✓  看看桌面上的她" : "试一试  →",
         13.0f, sent ? kGreen : (hot ? col : Mix(col, kMuted, 0.35f)), 700, false, core::HorizontalAlign::Right);
    components::mouseArea(ui, id + ".hit").position(x, y + dy).size(w, h)
        .onEnter([id] { S.hot = id; app::requestUpdate(); })
        .onLeave([id] { if (S.hot == id) { S.hot.clear(); app::requestUpdate(); } })
        .onTap([id, cmd] {
            S.link.send(cmd);
            S.tryId = id;
            S.tryAt = Clock::now();
            app::requestUpdate();
        })
        .build();
}

/** 数据卡（KPI）：图标 + 大数字 + 标签 */
void KpiTile(eui::Ui& ui, const std::string& id, float x, float y, float w, float h, unsigned icon, const std::string& value,
             const std::string& unit, const std::string& label, eui::Color col)
{
    Card(ui, id + ".card", x, y, w, h);
    IconBadge(ui, id + ".ic", x + 16.0f, y + 16.0f, 36.0f, icon, col);
    const float vw = 4.0f + (float)value.size() * 15.5f;   // 数字约 0.6em 宽
    Text(ui, id + ".v", x + 16.0f, y + 58.0f, vw + 20.0f, 34.0f, value, 26.0f, kText, 800);
    Text(ui, id + ".u", x + 16.0f + vw + 6.0f, y + 70.0f, w - vw - 30.0f, 20.0f, unit, 13.0f, kMuted, 600);
    Text(ui, id + ".l", x + 62.0f, y + 24.0f, w - 74.0f, 20.0f, label, 13.0f, kMuted, 600);
}

/** 裁到给定像素宽（UTF-8 码点边界），超出加"…"；宽度用 TextW 估（中英混排都对得上） */
std::string CutWidth(const std::string& s, float w, float fs)
{
    float acc = 0.0f;
    size_t i = 0;
    while (i < s.size())
    {
        const unsigned char c = (unsigned char)s[i];
        const size_t n = c < 0x80 ? 1 : ((c & 0xE0) == 0xC0 ? 2 : ((c & 0xF0) == 0xE0 ? 3 : 4));
        const float step = (n == 1 ? 0.56f : 1.0f) * fs * 1.06f;
        if (acc + step > w) return s.substr(0, i) + "…";
        acc += step;
        i += n;
    }
    return s;
}

std::string Ago(double t)
{
    const long d = (long)std::max(0.0, (double)std::time(nullptr) - t);
    if (d < 60) return i18n::T("刚刚");
    if (d < 3600) return std::to_string(d / 60) + i18n::T(" 分钟前");
    if (d < 86400) return std::to_string(d / 3600) + i18n::T(" 小时前");
    return std::to_string(d / 86400) + i18n::T(" 天前");
}

std::string Dur(float sec)
{
    const int s0 = (int)std::max(0.0f, sec);
    if (s0 < 60) return std::to_string(s0) + i18n::T(" 秒");
    if (s0 < 3600) return std::to_string(s0 / 60) + i18n::T(" 分钟");
    return std::to_string(s0 / 3600) + i18n::T(" 小时 ") + std::to_string(s0 / 60 % 60) + i18n::T(" 分");
}

/** 关系等级（亲密度 → 阶段名 + 段内进度） */
std::string BondName(float a, int* lv, float* prog)
{
    static const float kT[] = { 0.0f, 0.3f, 0.5f, 0.7f, 0.85f, 0.95f, 1.0001f };
    static const char* const kN[] = { "陌生", "认识", "熟悉", "亲近", "亲密", "挚友" };
    int i = 0;
    while (i < 5 && a >= kT[i + 1]) ++i;
    *lv = i + 1;
    *prog = std::clamp((a - kT[i]) / (kT[i + 1] - kT[i]), 0.0f, 1.0f);
    return i18n::T(kN[i]);
}

/** 卡内小标题：图标 + 标题 + 右侧注释 */
void CardHead(eui::Ui& ui, const std::string& id, float x, float y, float w, unsigned icon, const std::string& t,
              eui::Color col, const std::string& note = "")
{
    IconBadge(ui, id + ".ic", x, y, 30.0f, icon, col);
    Text(ui, id + ".t", x + 40.0f, y + 4.0f, w - 40.0f, 22.0f, t, 15.5f, kText, 700);
    if (!note.empty())
        Text(ui, id + ".n", x + w - 200.0f, y + 6.0f, 200.0f, 20.0f, note, 12.0f, kMuted, 500, false, core::HorizontalAlign::Right);
}

/** 进度行（接下来会发生什么）：标题 + 说明 + 细进度条 */
void NextRow(eui::Ui& ui, const std::string& id, float x, float y, float w, const std::string& t, const std::string& d,
             float v, eui::Color col)
{
    Text(ui, id + ".t", x, y, w, 20.0f, t, 14.0f, kText, 600);
    Text(ui, id + ".d", x, y + 22.0f, w, 18.0f, d, 12.0f, kMuted);
    ui.rect(id + ".tr").position(x, y + 46.0f).size(w, 6.0f).color(kTrack).radius(3.0f).build();
    if (v > 0.005f)
        ui.rect(id + ".f").position(x, y + 46.0f).size(std::max(6.0f, w * std::clamp(v, 0.0f, 1.0f)), 6.0f)
            .gradient(Mix(col, C(0xFFFFFF), 0.35f), col, core::GradientDirection::Horizontal).radius(3.0f)
            .transition(0.45f).animate(core::AnimProperty::Frame).build();
}

/** 她的显示名：面板设的名字 > LLM 生成的名字 > 模型目录名 */
std::string PetDisplayName()
{
    const std::string n = S.st.get("persona").get("name").strOr("");
    return n.empty() ? std::string(S.st.get("model").strOr("桌宠")) : n;
}

/** LLM 连接状态（只有"最近一次调用成功"才算已连接；见桌宠 LlmStateJson） */
std::string LlmStateLabel(bool* good)
{
    const std::string st = S.st.get("llm_state").strOr("no_agent");
    *good = st == "ok";
    if (st == "ok") return "LLM 已连接";
    if (st == "error") return "LLM 连接失败";
    if (st == "no_key") return "LLM 未配置";
    if (st == "pending") return "LLM 待验证";
    return "LLM 未连接";
}

void PageStatus(Pen& p)
{
    S.secN = 0;
    const Value& mood = S.st.get("mood");
    const Value& emo = mood.get("emotion");
    const std::string st = mood.get("state").strOr("awake");
    const eui::Color white = C(0xFFFFFF);

    // ================= 主视觉（Hero）：品牌渐变 + 实时徽标 + 名字 + 此刻心情 + VAD 玻璃面板
    const bool wide = p.w >= 560.0f;              // 够宽才放 VAD 玻璃面板
    const float heroH = 232.0f;
    const float plotW = wide ? heroH - 48.0f : 0.0f;
    const float hx = p.x, hy = p.y, hw = p.w;
    p.ui.rect("hero.bg").position(hx, hy).size(hw, heroH).gradient(kBrandA, kBrandB, core::GradientDirection::Horizontal)
        .radius(26.0f).shadow(36.0f, 0.0f, 16.0f, C(0x7C6CFF, 0.28f)).build();
    p.ui.rect("hero.bg2").position(hx + hw * 0.45f, hy).size(hw * 0.55f, heroH)
        .gradient(eui::Color{ kBrandC.r, kBrandC.g, kBrandC.b, 0.0f }, eui::Color{ kBrandC.r, kBrandC.g, kBrandC.b, 0.85f },
                  core::GradientDirection::Horizontal)
        .radius(26.0f).build();
    // 装饰光斑
    p.ui.rect("hero.o1").position(hx + hw - 150.0f, hy - 60.0f).size(220.0f, 220.0f).color(C(0xFFFFFF, 0.10f)).radius(110.0f).build();
    p.ui.rect("hero.o2").position(hx + hw * 0.38f, hy + heroH - 70.0f).size(120.0f, 120.0f).color(C(0xFFFFFF, 0.07f)).radius(60.0f).build();
    {
        const float x = hx + 28.0f;
        float y = hy + 24.0f;
        // LIVE 徽标
        const bool live = S.connected;
        p.ui.rect("hero.live.bg").position(x, y).size(136.0f, 28.0f).color(C(0xFFFFFF, 0.20f)).radius(14.0f)
            .border(1.0f, C(0xFFFFFF, 0.35f)).build();
        p.ui.rect("hero.live.halo").position(x + 9.0f, y + 7.0f).size(14.0f, 14.0f).color(C(live ? 0x5CFFB0 : 0xFFD166, 0.35f)).radius(7.0f).build();
        p.ui.rect("hero.live.dot").position(x + 12.0f, y + 10.0f).size(8.0f, 8.0f).color(C(live ? 0x5CFFB0 : 0xFFD166)).radius(4.0f).build();
        Text(p.ui, "hero.live.t", x + 28.0f, y + 4.0f, 116.0f, 20.0f, live ? "LIVE · 实时同步" : "等待连接…", 12.5f, white, 700);
        y += 44.0f;
        Text(p.ui, "hero.name", x, y, hw * 0.6f, 44.0f, PetDisplayName(), 34.0f, white, 800);
        y += 50.0f;
        // 状态 / LLM 胶囊（白色玻璃）
        // 睡着后 LLM 暂停（省 token），只有点她一下才恢复——在这里明示
        const Value& ai = S.st.get("agent_info");
        const bool paused = ai.get("paused").isBool() && ai.get("paused").b;
        bool llmGood = false;
        const std::string llmLab = LlmStateLabel(&llmGood);
        const std::string s1 = StateZh(st), s2 = (llmGood && paused) ? "LLM 休眠 · 点她唤醒" : llmLab;
        const float w1 = 30.0f + TextW(s1, 13.0f), w2 = 30.0f + TextW(i18n::T(s2), 13.0f);
        p.ui.rect("hero.p1").position(x, y).size(w1, 28.0f).color(C(0xFFFFFF, 0.24f)).radius(14.0f).build();
        Text(p.ui, "hero.p1.t", x, y + 5.0f, w1, 20.0f, s1, 13.0f, white, 700, false, core::HorizontalAlign::Center);
        p.ui.rect("hero.p2").position(x + w1 + 8.0f, y).size(w2, 28.0f).color(C(0xFFFFFF, 0.24f)).radius(14.0f).build();
        Text(p.ui, "hero.p2.t", x + w1 + 8.0f, y + 5.0f, w2, 20.0f, s2, 13.0f, white, 700, false, core::HorizontalAlign::Center);
        {
            // 关系等级胶囊：亲密度不再画成永远 99 的条，而是"挚友 Lv.6"+ 认识天数
            int lv = 1;
            float pr = 0.0f;
            const std::string bn = BondName((float)mood.get("affection").numOr(0), &lv, &pr);
            const double fs = S.st.get("life").get("first_seen").numOr(0);
            long days = 1;   // 按日历日算（昨天认识 = 第 2 天），不是按满 24 小时
            if (fs > 0)
            {
                auto midnight = [](time_t t) { struct tm lt; plat::LocalTime(t, &lt); lt.tm_hour = 12; lt.tm_min = lt.tm_sec = 0; return std::mktime(&lt); };
                days = (long)std::lround(std::difftime(midnight(std::time(nullptr)), midnight((time_t)fs)) / 86400.0) + 1;
            }
            const std::string s3 = i18n::TF("♥ {} Lv.{} · 认识第 {} 天",
                                            { bn, std::to_string(lv), std::to_string(days) });
            const float w3 = 30.0f + TextW(s3, 13.0f), x3 = x + w1 + w2 + 16.0f;
            if (x3 + w3 < hx + hw - plotW - 40.0f)
            {
                p.ui.rect("hero.p3").position(x3, y).size(w3, 28.0f).color(C(0xFFFFFF, 0.92f)).radius(14.0f).build();
                Text(p.ui, "hero.p3.t", x3, y + 5.0f, w3, 20.0f, s3, 13.0f, kViolet, 700, false, core::HorizontalAlign::Center);
            }
        }
        y += 44.0f;
        // 此刻心情 + 强度条（白色）
        const float inten = std::clamp((float)emo.get("intensity").numOr(0), 0.0f, 1.0f);
        // 强度条宽：留出右侧"强度 xx%"标签（80）和 VAD 面板，不能伸出主视觉
        const float bw = std::max(120.0f, std::min(340.0f, hw - 56.0f - 90.0f - (plotW > 0.0f ? plotW + 28.0f : 0.0f)));
        Text(p.ui, "hero.emo.c", x, y, 90.0f, 20.0f, "此刻的心情", 13.0f, C(0xFFFFFF, 0.85f), 600);
        Text(p.ui, "hero.emo", x + 90.0f, y - 6.0f, bw - 90.0f, 30.0f, EmotionZh(emo.get("name").strOr("neutral")), 22.0f, white, 800);
        y += 32.0f;
        p.ui.rect("hero.int.t").position(x, y).size(bw, 8.0f).color(C(0xFFFFFF, 0.25f)).radius(4.0f).build();
        if (inten > 0.01f)
            p.ui.rect("hero.int.f").position(x, y).size(std::max(8.0f, bw * inten), 8.0f).color(white).radius(4.0f)
                .transition(0.45f).animate(core::AnimProperty::Frame).build();
        const std::string b = i18n::TF("强度 {}%", { std::to_string((int)std::lround(inten * 100.0f)) });
        Text(p.ui, "hero.int.n", x + bw + 10.0f, y - 7.0f, 80.0f, 20.0f, b, 12.5f, C(0xFFFFFF, 0.9f), 600);
    }
    // VAD 玻璃面板（横 = 愉悦，纵 = 唤醒）
    {
        const Value& V0 = emo.get("v");
        const float V = (float)V0.numOr(0), A = (float)emo.get("a").numOr(0), D = (float)emo.get("d").numOr(0);
        const float plot = plotW;
        if (plot > 0.0f)
        {
            const float px = hx + hw - 28.0f - plot, py = hy + (heroH - plot) / 2.0f;
            p.ui.rect("vad.bg").position(px, py).size(plot, plot).color(C(0xFFFFFF, 0.16f)).radius(20.0f)
                .border(1.0f, C(0xFFFFFF, 0.40f)).build();
            p.ui.rect("vad.h").position(px + 12.0f, py + plot / 2.0f).size(plot - 24.0f, 1.0f).color(C(0xFFFFFF, 0.35f)).build();
            p.ui.rect("vad.v").position(px + plot / 2.0f, py + 12.0f).size(1.0f, plot - 24.0f).color(C(0xFFFFFF, 0.35f)).build();
            const float dx = px + plot / 2.0f + std::clamp(V, -1.0f, 1.0f) * (plot / 2.0f - 18.0f);
            const float dy = py + plot / 2.0f - std::clamp(A, -1.0f, 1.0f) * (plot / 2.0f - 18.0f);
            p.ui.rect("vad.halo").position(dx - 16.0f, dy - 16.0f).size(32.0f, 32.0f).color(C(0xFFFFFF, 0.28f)).radius(16.0f)
                .transition(0.6f).animate(core::AnimProperty::Frame).build();
            p.ui.rect("vad.dot").position(dx - 8.0f, dy - 8.0f).size(16.0f, 16.0f).color(white).radius(8.0f)
                .shadow(10.0f, 0.0f, 2.0f, C(0x5B4BDB, 0.45f)).transition(0.6f).animate(core::AnimProperty::Frame).build();
            Text(p.ui, "vad.t1", px + plot / 2.0f + 6.0f, py + 10.0f, 60.0f, 16.0f, "兴奋", 11.5f, C(0xFFFFFF, 0.85f), 600);
            Text(p.ui, "vad.t2", px + plot - 44.0f, py + plot / 2.0f - 20.0f, 40.0f, 16.0f, "愉悦", 11.5f, C(0xFFFFFF, 0.85f), 600);
            Text(p.ui, "vad.t3", px + 12.0f, py + plot - 26.0f, plot - 24.0f, 16.0f, D >= 0 ? "姿态 · 比较主动" : "姿态 · 有点退缩",
                 11.5f, C(0xFFFFFF, 0.85f), 600);
        }
    }
    p.y += heroH + 20.0f;

    // ================= KPI 数据卡
    {
        const int cols = p.w >= 760.0f ? 4 : 2;
        const float gap = 14.0f, tw = (p.w - gap * (cols - 1)) / cols, th = 100.0f;
        const int up = (int)S.st.get("uptime").numOr(0);
        char hm[32];
        if (up >= 3600) std::snprintf(hm, sizeof(hm), "%d:%02d", up / 3600, up / 60 % 60);
        else std::snprintf(hm, sizeof(hm), "%d", up / 60);
        const struct { const char* id; unsigned ic; std::string v; const char* u; const char* l; eui::Color c; } k[] = {
            { "k.sess", 0xF25A, std::to_string((int)S.st.get("session_touches").numOr(0)), "次", "本次互动", kPink },
            { "k.all", 0xF004, std::to_string((int)S.st.get("touches").numOr(0)), "次", "累计互动", kBrandB },
            { "k.up", 0xF017, hm, up >= 3600 ? "时:分" : "分钟", "已陪伴", kBrandC },
            { "k.rps", 0xF11B, std::to_string((int)S.st.get("life").get("rps").arr.size() >= 3
                                                  ? (int)S.st.get("life").get("rps").arr[1].numOr(0) : 0)
                                   + ":" + std::to_string((int)S.st.get("life").get("rps").arr.size() >= 3
                                                  ? (int)S.st.get("life").get("rps").arr[2].numOr(0) : 0),
              "你:她", "猜拳战绩", kOrange } };
        for (int i = 0; i < 4; ++i)
        {
            const float x = p.x + (i % cols) * (tw + gap), y = p.y + (i / cols) * (th + gap);
            KpiTile(p.ui, k[i].id, x, y, tw, th, k[i].ic, k[i].v, k[i].u, k[i].l, k[i].c);
        }
        p.y += (4 / cols) * (th + gap) + 14.0f;
    }

    // ================= 内在状态
    Section(p, "sec.inner", "她的内在状态");
    {
        const bool two = p.w >= 560.0f;
        const float rowH = 74.0f, h = 20.0f + (two ? 2 : 4) * rowH;
        Card(p.ui, "inner.card", p.x, p.y, p.w, h);
        const float x = p.x + 22.0f, w = p.w - 44.0f, bw = two ? (w - 36.0f) / 2.0f : w;
        // 每条下面写"什么会让它变"——状态页不只是看数，而是告诉你怎么影响她
        const struct { const char* id; const char* l; const char* k; eui::Color c; const char* hint; } bars[] = {
            { "b.energy", "精力", "energy", kGreen, "睡觉回满 · 醒着最多回到 65% · 互动消耗" },
            { "b.aff", "亲密度", "affection", kPink, "摸头、牵手会涨 · 她累了还闹、久别会掉" },
            { "b.annoy", "恼火", "annoy", kOrange, "连戳会涨 · 摸头消气，也会自己消" },
            { "b.lonely", "寂寞", "lonely", kPurple, "被晾着会涨 · 理她、陪她玩就降" } };
        for (int i = 0; i < 4; ++i)
        {
            const float bx = two ? x + (i % 2) * (bw + 36.0f) : x;
            const float by = p.y + 14.0f + (two ? (i / 2) : i) * rowH;
            Bar(p.ui, bars[i].id, bx, by, bw, bars[i].l, (float)mood.get(bars[i].k).numOr(0), bars[i].c);
            // 累了（精力 < 30%）：精力那条的说明换成红色警告
            // 只有醒着/犯困时互动才会扣亲密度（睡着戳不醒、生闷气时不扣），警告也只在这两种状态出现
            const bool warn = i == 0 && (float)mood.get("energy").numOr(1) < 0.3f && (st == "awake" || st == "drowsy");
            Text(p.ui, std::string(bars[i].id) + ".h", bx, by + 44.0f, bw, 18.0f,
                 CutWidth(i18n::T(warn ? "她累了！再闹会掉亲密度，让她睡一会儿" : bars[i].hint), bw, 12.0f),
                 12.0f, warn ? kDanger : kMuted, warn ? 700 : 400);
        }
        p.y += h + 26.0f;
    }

    // ================= 试一试（演示卡片）
    Section(p, "sec.try", p.w >= 600.0f ? "试一试 —— 点一下，桌面上的她马上有反应" : "试一试（点一下她马上有反应）");
    if ((float)mood.get("energy").numOr(1) < 0.3f && (st == "awake" || st == "drowsy"))
    {
        // 累了的提示条：互动会消耗精力，精力低时继续互动会掉亲密度
        p.ui.rect("try.warn").position(p.x, p.y).size(p.w, 44.0f).color(kDangerBg).radius(12.0f)
            .border(1.0f, kDangerBorder).build();
        Icon(p.ui, "try.warn.i", p.x + 12.0f, p.y + 7.0f, 30.0f, 0xF186, 15.0f, kDanger);
        Text(p.ui, "try.warn.t", p.x + 48.0f, p.y + 12.0f, p.w - 60.0f, 20.0f,
             CutWidth(i18n::T("她现在很累（精力 < 30%）：每次互动都会掉亲密度，先让她睡一会儿吧"), p.w - 60.0f, 13.5f),
             13.5f, kDanger, 600);
        p.y += 56.0f;
    }
    {
        struct D { const char* id; unsigned ic; const char* t; const char* d; eui::Color c; std::string cmd; };
        std::vector<D> ds;
        if (st == "asleep" || st == "drowsy")
            ds.push_back({ "try.wake", 0xF185, "叫醒她", "她现在困了，轻轻把她叫醒", kOrange, "{\"cmd\":\"mood\",\"state\":\"awake\"}" });
        ds.push_back({ "try.pat", 0xF256, "摸摸头", "最喜欢被摸头，会眯眼蹭你", kPink, "{\"cmd\":\"touch\",\"zone\":\"head\",\"kind\":\"rub\"}" });
        ds.push_back({ "try.hand", 0xF2B5, "牵手", "伸出手，看她怎么回应", kBrandC, "{\"cmd\":\"touch\",\"zone\":\"hand.r\",\"kind\":\"click\"}" });
        ds.push_back({ "try.happy", 0xF118, "逗她开心", "推一把开心情绪，表情跟着变", kOrange, "{\"cmd\":\"emotion\",\"name\":\"happy\",\"intensity\":0.8}" });
        ds.push_back({ "try.joke", 0xF59A, "讲个笑话", "让她讲一个（笑话池里没讲过的）", kPurple, "{\"cmd\":\"joke\"}" });
        ds.push_back({ "try.rps", 0xF11B, "猜拳", "石头剪刀布，在气泡里出拳", kGreen, "{\"cmd\":\"game\",\"kind\":\"rps\"}" });
        if (st != "asleep" && st != "drowsy")
            ds.push_back({ "try.sleep", 0xF186, "哄她睡觉", "让她犯困，慢慢睡着", kViolet, "{\"cmd\":\"mood\",\"state\":\"drowsy\"}" });
        const int cols = p.w >= 760.0f ? 3 : (p.w >= 480.0f ? 2 : 1);
        const float gap = 14.0f, cw = (p.w - gap * (cols - 1)) / cols, ch = 104.0f;
        for (size_t i = 0; i < ds.size(); ++i)
        {
            const float x = p.x + (i % cols) * (cw + gap), y = p.y + (i / cols) * (ch + gap);
            DemoCard(p.ui, ds[i].id, x, y, cw, ch, ds[i].ic, ds[i].t, ds[i].d, ds[i].c, ds[i].cmd);
        }
        p.y += ((ds.size() + cols - 1) / cols) * (ch + gap) + 12.0f;
    }

    const Value& life = S.st.get("life");
    const bool two = p.w >= 760.0f;
    const float gap = 16.0f, colW = two ? (p.w - gap) / 2.0f : p.w;

    // ================= 接下来会发生什么 + 情绪曲线
    Section(p, "sec.next", "她接下来会怎样");
    {
        const float hA = 318.0f, hB = 318.0f;   // 三行进度 + 底部"已持续"一行，留足底边距
        // ---- A：倒计时/进度
        {
            const float cx = p.x, cy = p.y, iw = colW - 40.0f, ix = cx + 20.0f;
            Card(p.ui, "next.card", cx, cy, colW, hA);
            CardHead(p.ui, "next.h", ix, cy + 18.0f, iw, 0xF254, "接下来", kBrandB, "实时");
            float y = cy + 66.0f;
            const float idle = (float)life.get("idle").numOr(0), sinceT = (float)life.get("since_touch").numOr(0);
            const float after = std::max(1.0f, (float)life.get("sleep_after").numOr(180));
            const float energy = (float)mood.get("energy").numOr(0);
            // 1) 睡觉相关
            const float annoyNow = (float)mood.get("annoy").numOr(0);
            if (st == "asleep" && energy < 0.3f)
            {
                const float left = std::max(0.0f, (0.95f - energy) * kSleepFullSecs);
                NextRow(p.ui, "nx.sleep", ix, y, iw, "累趴了，在补觉（戳不醒）",
                        i18n::TF("精力回到 30% 前怎么戳都不会醒；大约 {} 后自己醒", { Dur(left) }),
                        energy / 0.95f, kPurple);
            }
            else if (st == "sulk")
            {
                NextRow(p.ui, "nx.sleep", ix, y, iw, "在生气，不理你",
                        energy < 0.25f ? "摸头能慢慢哄好；她已经很累了，20 秒没人碰就会气着睡着"
                                       : "摸头能慢慢哄好（恼火降到 30% 以下）；戳她只会更气",
                        1.0f - annoyNow, kDanger);
            }
            else if (st != "asleep" && st != "drowsy" && energy < 0.3f)
            {
                NextRow(p.ui, "nx.sleep", ix, y, iw, "她累了：再闹就要出事",
                        "每次互动亲密度 -2；恼火到 70% 会生气不理你；精力耗尽会直接累趴睡着",
                        annoyNow / 0.7f, kDanger);
            }
            else if (st == "asleep")
            {
                const float left = std::max(0.0f, (0.95f - energy) * kSleepFullSecs);
                NextRow(p.ui, "nx.sleep", ix, y, iw, "睡着了 · 精力回到 95% 会自己醒",
                        left > 1.0f ? i18n::TF("大约还要 {}（摸摸头她会在梦里笑，戳两下会醒）", { Dur(left) })
                                    : i18n::T("快醒了"),
                        energy / 0.95f, kPurple);
            }
            else if (st == "drowsy")
            {
                NextRow(p.ui, "nx.sleep", ix, y, iw, "正在犯困", "再没人理她就会睡着；碰她一下会醒过来", 0.8f, kOrange);
            }
            else if (life.get("sleep_on").numOr(1) < 0.5f)
            {
                NextRow(p.ui, "nx.sleep", ix, y, iw, "不会犯困", "「行为」里关掉了睡觉", 0.0f, kMuted);
            }
            else
            {
                const float q = std::min(idle, sinceT);
                NextRow(p.ui, "nx.sleep", ix, y, iw, "犯困倒计时",
                        idle < 5.0f
                            ? i18n::TF("你在用电脑，她就不会犯困（你离开 {} 后才会）", { Dur(after) })
                            : i18n::TF("再过 {} 没人理，她就要打瞌睡了", { Dur(after - q) }),
                        q / after, kOrange);
            }
            y += 74.0f;
            // 2) 休息提醒
            const float work = (float)life.get("work").numOr(0), wl = (float)life.get("work_limit").numOr(0);
            if (wl >= 60.0f)
                NextRow(p.ui, "nx.work", ix, y, iw, "提醒你休息",
                        i18n::TF("你已连续活动 {}，再 {} 她会提醒你起来走走", { Dur(work), Dur(wl - work) }),
                        work / wl, kBrandC);
            else
                NextRow(p.ui, "nx.work", ix, y, iw, "提醒你休息", "已关闭（在「行为 → 互动小功能」里设置）", 0.0f, kMuted);
            y += 74.0f;
            // 3) 主动找你
            const float lonely = (float)mood.get("lonely").numOr(0);
            if (life.get("seek_on").numOr(1) > 0.5f)
                NextRow(p.ui, "nx.seek", ix, y, iw, "主动找你搭话",
                        lonely >= 0.6f ? "她已经很寂寞了——你一动鼠标她就会来找你" : "寂寞到 60% 且你在电脑前时，她会主动挥手搭话",
                        lonely / 0.6f, kPink);
            else
                NextRow(p.ui, "nx.seek", ix, y, iw, "主动找你搭话", "已关闭", 0.0f, kMuted);
            y += 74.0f;
            Text(p.ui, "nx.for", ix, y - 6.0f, iw, 18.0f,
                 i18n::TF("她{}已经 {}了", { StateZh(st), Dur((float)life.get("state_for").numOr(0)) }), 12.0f, kMuted);
        }
        // ---- B：情绪曲线（愉悦 = 柱，向上暖/向下冷；唤醒 = 点）
        {
            const float cx = two ? p.x + colW + gap : p.x, cy = two ? p.y : p.y + hA + gap;
            const float ix = cx + 20.0f, iw = colW - 40.0f;
            Card(p.ui, "curve.card", cx, cy, colW, hB);
            CardHead(p.ui, "curve.h", ix, cy + 18.0f, iw, 0xF201, "情绪曲线", kPink, "近 15 分钟 · 10 秒一格");
            const float gx = ix, gy = cy + 66.0f, gw = iw, gh = 170.0f, mid = gy + gh / 2.0f;
            p.ui.rect("cv.bg").position(gx, gy).size(gw, gh).color(kField).radius(12.0f).build();
            p.ui.rect("cv.mid").position(gx + 8.0f, mid).size(gw - 16.0f, 1.0f).color(kBorder).build();
            const Value& hist = life.get("emo_hist");
            const size_t n = hist.isArr() ? hist.arr.size() : 0;
            const int slots = 90;
            const float sw = (gw - 16.0f) / slots;
            for (size_t i = 0; i < n; ++i)
            {
                const Value& pt = hist.arr[i];
                if (!pt.isArr() || pt.arr.size() < 2) continue;
                const float v = std::clamp((float)pt.arr[0].numOr(0), -1.0f, 1.0f);
                const float a = std::clamp((float)pt.arr[1].numOr(0), -1.0f, 1.0f);
                const float bx = gx + 8.0f + (float)(slots - (int)n + (int)i) * sw;
                const float bh = std::max(2.0f, std::fabs(v) * (gh / 2.0f - 10.0f));
                const eui::Color c = v >= 0 ? kPink : kBrandC;
                p.ui.rect("cv.b" + std::to_string(i)).position(bx + sw * 0.15f, v >= 0 ? mid - bh : mid)
                    .size(std::max(1.5f, sw * 0.7f), bh).color({ c.r, c.g, c.b, 0.75f }).radius(1.0f).build();
                const float ay = mid - a * (gh / 2.0f - 10.0f);
                p.ui.rect("cv.a" + std::to_string(i)).position(bx + sw * 0.5f - 2.0f, ay - 2.0f).size(4.0f, 4.0f)
                    .color(kViolet).radius(2.0f).build();
            }
            if (n == 0)
                Text(p.ui, "cv.empty", gx, mid - 10.0f, gw, 20.0f, "刚启动，正在记录…", 12.5f, kMuted, 500, false,
                     core::HorizontalAlign::Center);
            Text(p.ui, "cv.l1", gx + 10.0f, gy + 6.0f, 80.0f, 16.0f, "开心", 11.0f, kPink, 600);
            Text(p.ui, "cv.l2", gx + 10.0f, gy + gh - 22.0f, 80.0f, 16.0f, "低落", 11.0f, kBrandC, 600);
            const float ly = gy + gh + 12.0f;
            p.ui.rect("cv.k1").position(gx, ly + 4.0f).size(10.0f, 10.0f).color(kPink).radius(2.0f).build();
            Text(p.ui, "cv.k1t", gx + 16.0f, ly, 120.0f, 18.0f, "柱 = 愉悦度", 12.0f, kMuted);
            p.ui.rect("cv.k2").position(gx + 120.0f, ly + 5.0f).size(8.0f, 8.0f).color(kViolet).radius(4.0f).build();
            Text(p.ui, "cv.k2t", gx + 134.0f, ly, 140.0f, 18.0f, "点 = 兴奋度", 12.0f, kMuted);
            Text(p.ui, "cv.now", gx + gw - 60.0f, ly, 60.0f, 18.0f, "现在 →", 12.0f, kMuted, 500, false, core::HorizontalAlign::Right);
        }
        p.y += (two ? std::max(hA, hB) : hA + gap + hB) + 26.0f;
    }

    // ================= 最近动态 + 被摸的地方
    Section(p, "sec.feed", "刚才发生了什么");
    {
        const Value& feed = life.get("feed");
        const size_t fn = feed.isArr() ? feed.arr.size() : 0;
        const int show = (int)std::min<size_t>(fn, 8);
        const float rowH = 44.0f, hA = 72.0f + std::max(1, show) * rowH + 8.0f;
        const Value& zs = life.get("zones");
        std::vector<std::pair<long, int>> order;   // (累计, 下标)
        for (size_t i = 0; zs.isArr() && i < zs.arr.size(); ++i)
        {
            const Value& z = zs.arr[i];
            if (z.isArr() && z.arr.size() >= 5) order.push_back({ (long)(z.arr[1].numOr(0) + z.arr[2].numOr(0)), (int)i });
        }
        std::sort(order.begin(), order.end(), [](const std::pair<long, int>& a, const std::pair<long, int>& b) { return a.first > b.first; });
        const int zshow = (int)std::min<size_t>(order.size(), 6);
        const float hB = 72.0f + std::max(1, zshow) * 50.0f + 34.0f;
        const float hRow = two ? std::max(hA, hB) : 0.0f;
        // ---- A：动态时间线
        {
            const float cx = p.x, cy = p.y, ix = cx + 20.0f, iw = colW - 40.0f;
            Card(p.ui, "feed.card", cx, cy, colW, two ? hRow : hA);
            CardHead(p.ui, "feed.h", ix, cy + 18.0f, iw, 0xF075, "最近动态", kBrandC,
                     fn ? i18n::TF("共 {} 条", { std::to_string(fn) }) : "");
            if (show == 0)
                Text(p.ui, "feed.empty", ix, cy + 76.0f, iw, 20.0f, "还没有动态——去「试一试」点一下，或者在桌面上摸摸她", 12.5f, kMuted);
            for (int k = 0; k < show; ++k)
            {
                const Value& it = feed.arr[fn - 1 - (size_t)k];   // 最新在上
                if (!it.isArr() || it.arr.size() < 3) continue;
                const std::string kind = it.arr[1].strOr(""), raw = it.arr[2].strOr("");
                unsigned ic = 0xF075;
                eui::Color c = kBrandC;
                std::string txt = raw;
                if (kind == "state") { ic = 0xF186; c = kPurple; txt = i18n::T("她") + raw; }
                else if (kind == "say") { ic = 0xF075; c = kPink; txt = i18n::T("她说：") + raw; }
                else if (kind == "llm") { ic = 0xF0E7; c = kViolet; txt = i18n::T("她（LLM）：") + raw; }
                else if (kind == "touch") { ic = 0xF256; c = kOrange; }
                else if (kind == "game") { ic = 0xF11B; c = kGreen; }
                else if (kind == "emo") { ic = 0xF118; c = kPink; txt = i18n::TF("心情变成了「{}」", { EmotionZh(raw) }); }
                const float ry = cy + 66.0f + k * rowH;
                // 时间线竖线 + 节点
                if (k + 1 < show)
                    p.ui.rect("fd.ln" + std::to_string(k)).position(ix + 13.0f, ry + 28.0f).size(2.0f, rowH - 28.0f)
                        .color(kTimeline).build();
                IconBadge(p.ui, "fd.ic" + std::to_string(k), ix, ry, 28.0f, ic, c);
                const float tw = iw - 40.0f - 80.0f;
                Text(p.ui, "fd.t" + std::to_string(k), ix + 40.0f, ry + 4.0f, tw, 20.0f,
                     CutWidth(txt, tw, 13.5f), 13.5f, kText, 500);
                Text(p.ui, "fd.a" + std::to_string(k), ix + iw - 80.0f, ry + 5.0f, 80.0f, 18.0f, Ago(it.arr[0].numOr(0)),
                     12.0f, kMuted, 500, false, core::HorizontalAlign::Right);
            }
        }
        // ---- B：被摸的地方（累计，点/搓分色；右侧"本次 +n"）
        {
            const float cx = two ? p.x + colW + gap : p.x, cy = two ? p.y : p.y + hA + gap;
            const float ix = cx + 20.0f, iw = colW - 40.0f;
            Card(p.ui, "zone.card", cx, cy, colW, two ? hRow : hB);
            CardHead(p.ui, "zone.h", ix, cy + 18.0f, iw, 0xF256, "她被摸的地方", kOrange, "累计");
            if (zshow == 0)
                Text(p.ui, "zn.empty", ix, cy + 76.0f, iw, 20.0f, "还没有记录（升级后从现在开始统计）", 12.5f, kMuted);
            const long top = zshow ? std::max(1L, order[0].first) : 1;
            for (int k = 0; k < zshow; ++k)
            {
                const Value& z = zs.arr[(size_t)order[(size_t)k].second];
                const long ck = (long)z.arr[1].numOr(0), rb = (long)z.arr[2].numOr(0);
                const long sess = (long)(z.arr[3].numOr(0) + z.arr[4].numOr(0));
                const float ry = cy + 70.0f + k * 50.0f, bw = iw - 150.0f;
                const std::string id = "zn" + std::to_string(k);
                Text(p.ui, id + ".l", ix, ry + 2.0f, 70.0f, 20.0f, z.arr[0].strOr("?"), 13.5f, kText, 600);
                const float wr = bw * (float)rb / (float)top, wc = bw * (float)ck / (float)top;
                p.ui.rect(id + ".tr").position(ix + 76.0f, ry + 6.0f).size(bw, 12.0f).color(kTrack).radius(6.0f).build();
                if (rb > 0)
                    p.ui.rect(id + ".r").position(ix + 76.0f, ry + 6.0f).size(std::max(6.0f, wr), 12.0f).color(kPink).radius(6.0f).build();
                if (ck > 0)
                    p.ui.rect(id + ".c").position(ix + 76.0f + wr, ry + 6.0f).size(std::max(6.0f, wc), 12.0f).color(kOrange).radius(6.0f).build();
                Text(p.ui, id + ".n", ix + 76.0f + bw + 8.0f, ry + 2.0f, 66.0f, 20.0f,
                     std::to_string(ck + rb) + (sess > 0 ? "  +" + std::to_string(sess) : ""), 12.5f,
                     sess > 0 ? kGreen : kMuted, 600, false, core::HorizontalAlign::Right);
            }
            const float ly = cy + 70.0f + std::max(1, zshow) * 50.0f + 4.0f;
            p.ui.rect("zn.k1").position(ix, ly + 4.0f).size(10.0f, 10.0f).color(kPink).radius(5.0f).build();
            Text(p.ui, "zn.k1t", ix + 16.0f, ly, 60.0f, 18.0f, "摸/揉", 12.0f, kMuted);
            p.ui.rect("zn.k2").position(ix + 70.0f, ly + 4.0f).size(10.0f, 10.0f).color(kOrange).radius(5.0f).build();
            Text(p.ui, "zn.k2t", ix + 86.0f, ly, 90.0f, 18.0f, "戳/牵/捏", 12.0f, kMuted);
            Text(p.ui, "zn.k3t", ix + 170.0f, ly, iw - 170.0f, 18.0f, "绿色 +n = 本次", 12.0f, kGreen);
        }
        p.y += (two ? hRow : hA + gap + hB) + 20.0f;
    }
}

void PageBehavior(Pen& p)
{
    PageTitle(p, "行为", "她怎么反应、什么时候犯困、会不会主动找你");
    Section(p, "sec.react", "反应");
    Card(p.ui, "react.card", p.x, p.y, p.w, 2 * ToggleH() + 2 * SliderH(p.w));
    ToggleRow(p, "t.lines", "即时台词气泡", "被戳、被摸时立刻冒一句（不等 LLM）", "lines");
    Divider(p.ui, "d1", p.x, p.y, p.w);
    ToggleRow(p, "t.native", "借用模型自带表情", "情绪强时套用模型里的开心/害羞/生气等表情", "native_expr");
    SliderRow(p, "s.bubble.min", "气泡最短显示时间", "bubble_min_secs", 1.0f, 10.0f, 0.5f, [](float v) { return Num(v) + "s"; });
    Divider(p.ui, "d2", p.x, p.y, p.w);
    SliderRow(p, "s.react", "情绪敏感度", "reactivity", 0.3f, 2.0f, 0.1f, [](float v) { char b[16]; std::snprintf(b, 16, "%.1f×", v); return std::string(b); });
    Divider(p.ui, "d3", p.x, p.y, p.w);
    SliderRow(p, "s.gain", "表情幅度", "expr_gain", 0.0f, 2.0f, 0.05f, [](float v) { return std::to_string((int)std::lround(v * 100)) + "%"; });
    p.y += 22.0f;

    Section(p, "sec.life", "生活节律");
    Card(p.ui, "life.card", p.x, p.y, p.w, 2 * ToggleH() + 2 * SliderH(p.w));
    ToggleRow(p, "t.sleep", "会犯困、睡觉", "没人理一阵子会打瞌睡，最后睡着（戳醒会有点起床气）", "sleep");
    Divider(p.ui, "d4", p.x, p.y, p.w);
    SliderRow(p, "s.after", "无人理会多久后犯困", "sleep_after", 60.0f, 900.0f, 30.0f,
              [](float v) { return std::to_string((int)v) + i18n::T(" 秒"); });
    Divider(p.ui, "d5", p.x, p.y, p.w);
    ToggleRow(p, "t.seek", "寂寞时主动找你", "你在电脑前却很久不理她，她会挥手搭话", "seek");
    Divider(p.ui, "d6", p.x, p.y, p.w);
    SliderRow(p, "s.idle", "自主小动作", "idle", 0.0f, 3.0f, 1.0f, [](float v) {
        static const char* k[] = { "关", "少", "适中", "多" };
        return i18n::T(k[std::clamp((int)std::lround(v), 0, 3)]);
    });
    p.y += 22.0f;

    // ---- 互动小功能：工作时长提醒 / 天气闲聊 / 玩（笑话、猜拳在右键菜单里）
    Section(p, "sec.fun", "互动小功能（笑话、猜拳在右键菜单里）");
    Card(p.ui, "fun.card", p.x, p.y, p.w, ToggleH() + SliderH(p.w) + 96.0f);
    SliderRow(p, "s.work", "连续工作多久提醒休息", "work_remind", 0.0f, 240.0f, 15.0f, [](float v) {
        return v < 1.0f ? i18n::T("关") : (std::to_string((int)v) + i18n::T(" 分钟"));
    });
    Divider(p.ui, "d7", p.x, p.y, p.w);
    ToggleRow(p, "t.weather", "天气闲聊", "闲时偶尔聊一句天气（联网查真实天气；查不到就只关心/提问）", "weather");
    Divider(p.ui, "d8", p.x, p.y, p.w);
    {
        const Value& strv = S.st.get("config").get("str");
        const std::string loc = strv.get("weather_loc").strOr("");
        // 回填已保存的城市：以前只写不回读，输入框永远是占位符，看着像"没保存/重启就没了"，
        // 再点一次「保存位置」还会把存着的城市覆盖成空。规则：首次进页面填一次；之后只有当输入框
        // 还是"干净"的（等于上次看到的已保存值）才接受外部变化，避免边输边被覆盖。
        static bool locFilled = false;
        static std::string locSeen;
        if (!locFilled)
        {
            S.weatherLoc.set(loc);
            locFilled = true;
        }
        else if (S.weatherLoc.get() == locSeen && S.weatherLoc.get() != loc)
        {
            S.weatherLoc.set(loc);
        }
        locSeen = loc;
        Text(p.ui, "loc.l", p.x + 18.0f, p.y + 9.0f, 120.0f, 22.0f, "位置（可选）", 15.0f, kText, 500);
        components::input(p.ui, "loc.in").theme(Th()).position(p.x + 140.0f, p.y).size(p.w - 300.0f, 40.0f)
            .placeholder(i18n::T("如：北京 / 上海浦东（留空=不提位置）")).bind(S.weatherLoc).fontSize(13.5f)
            .onEnter([] {
                S.link.send("{\"cmd\":\"config\",\"setstr\":{\"weather_loc\":" + JStr(S.weatherLoc.get()) + "}}");
                Toast("位置已保存");
            }).build();
        components::button(p.ui, "loc.save").theme(Th(), false).position(p.x + p.w - 150.0f, p.y).size(96.0f, 40.0f)
            .text(i18n::T("保存位置")).fontSize(13.0f)
            .onClick([] {
                S.link.send("{\"cmd\":\"config\",\"setstr\":{\"weather_loc\":" + JStr(S.weatherLoc.get()) + "}}");
                Toast("位置已保存");
            }).build();
        p.y += 52.0f;
        Text(p.ui, "fun.h", p.x + 18.0f, p.y, p.w - 36.0f, 40.0f,
             "工作时长提醒：你一直在动（用鼠标）累计到设定分钟数，她会关心一句让你起来活动；"
             "离开超过 5 分钟重新计时。笑话/猜拳/聊天气在右键菜单里点。", 12.5f, kMuted, 400, true);
        p.y += 44.0f;
    }
    p.y += 20.0f;
}

void PageWindow(Pen& p)
{
    PageTitle(p, "窗口", "桌宠窗口的显示方式（模型与能力见「模型」页）");
    Section(p, "sec.disp", "显示");
    Card(p.ui, "disp.card", p.x, p.y, p.w, 3 * ToggleH() + 5 * SliderH(p.w));
    SliderRow(p, "s.zoom", "大小（也可在角色上滚滚轮）", "zoom", 0.5f, 2.0f, 0.05f, [](float v) { return std::to_string((int)std::lround(v * 100)) + "%"; });
    Divider(p.ui, "d1", p.x, p.y, p.w);
    SliderRow(p, "s.op", "不透明度", "opacity", 0.2f, 1.0f, 0.05f, [](float v) { return std::to_string((int)std::lround(v * 100)) + "%"; });
    SliderRow(p, "s.bubble", "气泡字号", "bubble_px", 10.0f, 40.0f, 1.0f, [](float v) { return std::to_string((int)std::lround(v)) + "px"; });
    SliderRow(p, "s.bubble.gap", "气泡与角色间距", "bubble_gap", 0.0f, 60.0f, 1.0f, [](float v) { return std::to_string((int)std::lround(v)) + "px"; });
    Divider(p.ui, "d2", p.x, p.y, p.w);
    ToggleRow(p, "t.top", "总在最前", "", "top");
    Divider(p.ui, "d3", p.x, p.y, p.w);
    ToggleRow(p, "t.pass", "鼠标穿透", "开启后点不到她（不能拖动/右键），在这里或托盘里关掉", "passthrough");
    Divider(p.ui, "d4", p.x, p.y, p.w);
    ToggleRow(p, "t.lock", "锁定大小和位置", "开启后拖动/滚轮不再挪动或缩放她（防误操作）；这里的滑条仍可改", "lock");
    p.y += 22.0f;

    // 位置：显示存档坐标（以前存了却看不到），并给一个"被拖丢了"时的找回按钮
    const Value& wcfg = S.st.get("config");
    const int posL = (int)wcfg.get("pos_l").numOr(0);
    const int posB = (int)wcfg.get("pos_b").numOr(0);
    const bool posSet = wcfg.get("pos_set").numOr(0) > 0.5
        || (posL >= 0 && posB >= 0);   // 兼容旧配置（没有 pos_set 时，一对非负坐标就是存过的）
    const std::string posLine = !posSet
        ? i18n::T("还没记录位置（默认停在屏幕左下角）")
        : i18n::TF("左边距 {} px · 底边距 {} px（负数表示伸到屏幕外）；重启按原样恢复",
                   { std::to_string(posL), std::to_string(posB) });
    Section(p, "sec.place", "位置");
    const float posH = TextH(posLine, p.w - 36.0f, 12.5f, 20.0f);
    Card(p.ui, "place.card", p.x, p.y, p.w, posH + 76.0f);
    Text(p.ui, "place.line", p.x + 18.0f, p.y + 14.0f, p.w - 36.0f, posH, posLine, 12.5f, kMuted, 400, true);
    components::button(p.ui, "b.onscreen").theme(Th(), false).position(p.x + 18.0f, p.y + posH + 24.0f).size(150.0f, 36.0f)
        .text(i18n::T("拉回屏幕内")).fontSize(14.0f)
        .onClick([] {
            S.link.send("{\"cmd\":\"window\",\"on_screen\":true}");
            Toast("已把她拉回屏幕内");
        }).build();
    p.y += posH + 98.0f;
}

// ---------------------------------------------------------------- 模型页
/** 一行 chips（自动换行）；返回占用高度。draw=false 时只量高度 */
float ChipFlow(eui::Ui& ui, const std::string& id, float x, float w, float y, const std::vector<std::string>& items,
               const std::vector<eui::Color>& cols, bool draw)
{
    const float h = 30.0f, gap = 8.0f;
    float cx = x, cy = y;
    for (size_t i = 0; i < items.size(); ++i)
    {
        const float cw = 22.0f + TextW(i18n::T(items[i]), 13.0f);
        if (cx > x && cx + cw > x + w)
        {
            cx = x;
            cy += h + gap;
        }
        if (draw)
        {
            Chip(ui, id + "." + std::to_string(i), cx, cy, items[i], cols[i % cols.size()]);
        }
        cx += cw + gap;
    }
    return items.empty() ? 0.0f : (cy - y + h);
}

void PageModel(Pen& p)
{
    const Value& mi = S.st.get("model_info");
    PageTitle(p, "模型", "解算出的能力 / 模型库切换 / 导入新模型");
    if (!mi.isObj())
    {
        Text(p.ui, "mi.none", p.x, p.y, p.w, 40.0f, "还没读到模型信息（桌宠刚启动？稍后自动刷新）", 14.0f, kMuted);
        p.y += 56.0f;
        return;
    }

    const std::string name = mi.get("name").strOr("?");
    const std::string dir = mi.get("dir").strOr("");
    const std::string skel = mi.get("skeleton").strOr("none");
    const bool rig = mi.get("rig").isBool() && mi.get("rig").b;
    const bool profile = mi.get("profile").isBool() && mi.get("profile").b;
    const bool caps = mi.get("caps").isBool() && mi.get("caps").b;
    const std::string zmode = mi.get("zone_mode").strOr("basic");
    const int zcount = (int)mi.get("zone_count").numOr(2);

    // ---- 当前模型 · 解算结果
    Section(p, "sec.cur", "当前模型 · 解算结果");
    const Value& exprs = S.st.get("expressions");
    const Value& motions = mi.get("motions");
    const Value& body = mi.get("body");

    std::vector<std::string> mChips;
    for (size_t i = 0; i < motions.arr.size(); ++i)
    {
        const Value& g = motions.arr[i];
        if (g.arr.size() < 2) continue;
        mChips.push_back(std::string(g.arr[0].strOr("?")) + " ×" + std::to_string((int)g.arr[1].numOr(0)));
    }
    std::vector<std::string> eChips;
    for (size_t i = 0; i < exprs.arr.size(); ++i) eChips.push_back(exprs.arr[i].strOr(""));

    std::vector<std::string> rigChips, authChips;
    for (size_t i = 0; i < body.arr.size(); ++i)
    {
        const Value& b = body.arr[i];
        if (b.arr.size() < 2) continue;
        const std::string cn = b.arr[0].strOr("");
        const bool rigid = (int)b.arr[1].numOr(0) != 0;
        (rigid ? rigChips : authChips).push_back(cn + i18n::T(rigid ? " ·骨架" : " ·作者"));
    }

    // 高度先量后排（ChipFlow 折行高度由同一函数给出，绘制用同一组变量推进 y）
    const float innerW = p.w - 36.0f;
    const float flowM = mChips.empty() ? 22.0f : ChipFlow(p.ui, "q.m", 0.0f, innerW, 0.0f, mChips, {}, false);
    const float flowE = eChips.empty() ? 22.0f : ChipFlow(p.ui, "q.e", 0.0f, innerW, 0.0f, eChips, {}, false);
    const float flowR = rigChips.empty() ? 0.0f : ChipFlow(p.ui, "q.b1", 0.0f, innerW, 0.0f, rigChips, {}, false);
    const float flowA = authChips.empty() ? 0.0f : ChipFlow(p.ui, "q.b2", 0.0f, innerW, 0.0f, authChips, {}, false);
    const float hHead = 16.0f + 34.0f + 22.0f + 30.0f;
    const float hM = 24.0f + flowM + 10.0f;
    const float hE = 24.0f + flowE + 10.0f;
    const float hB = 24.0f + flowR + (flowR > 0.0f && flowA > 0.0f ? 8.0f : 0.0f) + flowA + 8.0f;
    const float hTip = (profile && caps) ? 22.0f : 48.0f;
    const float curH = hHead + hM + hE + hB + hTip + 12.0f;
    Card(p.ui, "cur.card", p.x, p.y, p.w, curH);

    float y = p.y + 16.0f;
    Text(p.ui, "cur.name", p.x + 18.0f, y, p.w - 260.0f, 30.0f, name, 20.0f, kText, 700);
    {
        // 骨架/分区徽章（右上）
        const std::string skelZh = skel == "full" ? "骨架 full" : (skel == "chain" ? "骨架 chain"
                                 : (skel == "swing" ? "骨架 swing" : "无骨架"));
        const std::string zoneZh = i18n::TF(zmode == "rig"    ? "交互 {} 区（含肘）"
                                            : zmode == "bound" ? "交互 {} 区（作者绑定）"
                                                               : "交互 {} 区（基础）",
                                            { std::to_string(zcount) });
        float bw = 0.0f;
        Chip(p.ui, "cur.sk", p.x + p.w - 300.0f, y + 2.0f, skelZh, skel == "none" ? kMuted : kAccent, &bw);
        Chip(p.ui, "cur.zn", p.x + p.w - 300.0f + bw + 8.0f, y + 2.0f, zoneZh,
             zmode == "rig" ? kAccent : (zmode == "bound" ? C(0xE8A33D) : kMuted));
    }
    y += 34.0f;
    Text(p.ui, "cur.fv", p.x + 18.0f, y, p.w - 36.0f, 20.0f,
         i18n::TF("指纹 {}", { mi.get("fv").strOr("?") }) + i18n::T(rig ? "　·　臂骨架 ✓" : "　·　臂骨架 —"),
         12.5f, kMuted);
    y += 22.0f;
    Text(p.ui, "cur.dir", p.x + 18.0f, y, p.w - 36.0f, 20.0f, dir, 12.0f, kMuted);
    y += 30.0f;

    Text(p.ui, "cur.ml", p.x + 18.0f, y, 200.0f, 22.0f, "动作（motion 组）", 14.0f, kText, 600);
    y += 24.0f;
    if (mChips.empty())
    {
        Text(p.ui, "cur.ml0", p.x + 18.0f, y, p.w - 36.0f, 22.0f, "（模型未带动作）", 13.0f, kMuted);
    }
    else
    {
        ChipFlow(p.ui, "cur.m", p.x + 18.0f, innerW, y, mChips, { kAccent, C(0x7BD389) }, true);
    }
    y += flowM + 10.0f;

    Text(p.ui, "cur.el", p.x + 18.0f, y, 200.0f, 22.0f,
         i18n::TF("表情（{}）", { std::to_string((int)eChips.size()) }), 14.0f, kText, 600);
    y += 24.0f;
    if (eChips.empty())
    {
        Text(p.ui, "cur.el0", p.x + 18.0f, y, p.w - 36.0f, 22.0f, "（模型未带表情）", 13.0f, kMuted);
    }
    else
    {
        ChipFlow(p.ui, "cur.e", p.x + 18.0f, innerW, y, eChips, { C(0xE86FA8), C(0xB07BE8), C(0x54AEFF) }, true);
    }
    y += flowE + 10.0f;

    Text(p.ui, "cur.bl", p.x + 18.0f, y, 360.0f, 22.0f,
         i18n::TF("躯干通道（BODY：骨架驱动 {} / 作者绑定 {}）",
                  { std::to_string((int)rigChips.size()), std::to_string((int)authChips.size()) }),
         14.0f, kText, 600);
    y += 24.0f;
    if (!rigChips.empty())
    {
        ChipFlow(p.ui, "cur.b1", p.x + 18.0f, innerW, y, rigChips, { kAccent }, true);
        y += flowR;
    }
    if (flowR > 0.0f && flowA > 0.0f) y += 8.0f;
    if (!authChips.empty())
    {
        ChipFlow(p.ui, "cur.b2", p.x + 18.0f, innerW, y, authChips, { C(0xE8A33D) }, true);
        y += flowA;
    }
    y += 8.0f;
    if (profile && caps)
    {
        Text(p.ui, "cur.ok", p.x + 18.0f, y, p.w - 36.0f, 20.0f, "profile ✓　能力缓存 ✓　（能力解算完整；可重新探测）", 12.5f, kMuted);
    }
    else
    {
        Text(p.ui, "cur.warn", p.x + 18.0f, y, p.w - 36.0f, 40.0f,
             i18n::TF("缺少 {}：点击分区会退化为头/身两带。点「能力探测」自动解算（约 30 秒，完成后自动重载）",
                      { (profile ? std::string() : std::string("l2d.profile.json "))
                        + (caps ? std::string() : i18n::T("能力缓存 ")) }),
             13.0f, kWarn, 500, true);
    }
    p.y += curH + 12.0f;

    // ---- 性格（这个模型是谁：能力解算 → LLM 生成 → 每模型一份）
    Section(p, "sec.persona", "性格（每个模型一份，由能力解算生成）");
    {
        const Value& per = S.st.get("persona");
        const std::string ptext = per.get("text").strOr("");
        const std::string pby = per.get("by").strOr("");
        const float inner = p.w - 36.0f;
        const std::string body = ptext.empty()
            ? std::string("还没有这个模型的性格。内置 agent 配好 API Key 后会自动按模型能力（表情/道具/动作/可交互区域）"
                          "解算生成——也可以用下面的按钮手动触发生成；没配 Key 时会先用本地兜底性格。")
            : ptext;
        const float th = TextH(body, inner, 13.0f, 19.0f);
        const std::string fv = S.st.get("model_info").get("fv").strOr("");
        if (S.personaGuidanceFor != fv && !ptext.empty())
        {
            S.personaGuidanceFor = fv;
            S.personaGuidance.set(per.get("guidance").strOr(""));
        }
        const float inputY = p.y + 16.0f + th + 12.0f;
        const float perH = 16.0f + th + 12.0f + 176.0f + 40.0f + 16.0f;
        Card(p.ui, "per.card", p.x, p.y, p.w, perH);
        Text(p.ui, "per.t", p.x + 18.0f, p.y + 16.0f, inner, th, body, 13.0f, ptext.empty() ? kMuted : kText, 400, true);
        Text(p.ui, "per.guidance.label", p.x + 18.0f, inputY, inner, 22.0f, "你希望的性格方向（可选）", 14.0f, kText, 600);
        components::input(p.ui, "per.guidance").theme(Th()).position(p.x + 18.0f, inputY + 30.0f).size(inner, 100.0f)
            .placeholder(i18n::T("例如：嘴硬心软、爱吐槽但会鼓励我，少聊天气；喜欢音乐，有一点傲娇。"))
            .bind(S.personaGuidance).multiline().fontSize(13.5f).build();
        Text(p.ui, "per.guidance.hint", p.x + 18.0f, inputY + 138.0f, inner, 30.0f,
             "最多 1000 字；生成时结合你的文字与模型能力。留空由模型自由生成，名字与称呼仍保留。", 12.0f, kMuted, 400, true);
        {
            ButtonFlow bf{ p.ui, p.x + 18.0f, p.x + 18.0f, inputY + 176.0f, p.x + p.w - 18.0f };
            bf.add("per.regen", ptext.empty() ? "生成性格" : "重新生成性格", [&] {
                const std::string guidance = S.personaGuidance.get();
                if (guidance.size() > 4000 || std::count_if(guidance.begin(), guidance.end(),
                    [](unsigned char c) { return (c & 0xC0) != 0x80; }) > 1000)
                {
                    Toast("性格方向过长，请缩短到 1000 字以内");
                    return;
                }
                S.link.send("{\"cmd\":\"persona\",\"regen\":true,\"guidance\":" + JStr(guidance) + "}");
                Toast("正在按模型能力生成性格（约 3~10 秒）…");
            }, true, 36.0f);
            if (!ptext.empty())
            {
                Text(p.ui, "per.by", p.x + 18.0f + 200.0f, inputY + 186.0f, std::max(100.0f, inner - 200.0f), 20.0f,
                     pby == "llm" ? "来源：LLM 按能力解算（可手改 ~/.config/l2dpet/personas/）"
                                  : "来源：本地兜底（配好 API Key 再点生成即可换成 LLM 版）",
                     12.0f, kMuted);
            }
        }
        p.y += perH + 12.0f;
    }

    // ---- 名字与称呼（主人手设：她叫什么、小名、她怎么叫你；每个模型一份，重新生成性格也保留）
    Section(p, "sec.names", "名字与称呼");
    {
        const Value& per = S.st.get("persona");
        const std::string fv = S.st.get("model_info").get("fv").strOr("");
        // 换了模型：输入框换成这个模型的设定（等 agent 上报了性格再填，否则会拿空值盖掉、一按保存就清空）
        if (S.nmFor != fv && !std::string(per.get("text").strOr("")).empty())
        {
            S.nmFor = fv;
            S.nmName.set(per.get("user_name").strOr(""));
            S.nmNick.set(per.get("nick").strOr(""));
            S.nmCall.set(per.get("call").strOr(""));
        }
        const float rowH = 52.0f, cardH = 16.0f + 3 * rowH + 48.0f + 40.0f;
        Card(p.ui, "nm.card", p.x, p.y, p.w, cardH);
        const float lx = p.x + 18.0f, ix = p.x + 150.0f, iw = std::max(160.0f, p.w - 168.0f);
        const std::string genName = per.get("name").strOr("");
        const struct { const char* id; const char* label; eui::Signal<std::string>* sig; std::string ph; } rows[] = {
            { "nm.name", "她的名字", &S.nmName, genName.empty() ? i18n::T("留空 = 用性格里生成的名字") : i18n::TF("留空 = 用「{}」", { genName }) },
            { "nm.nick", "小名", &S.nmNick, i18n::T("如：芙芙（可留空）") },
            { "nm.call", "她怎么叫你", &S.nmCall, i18n::T("如：阿杰 / 哥哥（留空 = 按性格和记忆）") } };
        for (int i = 0; i < 3; ++i)
        {
            const float ry = p.y + 16.0f + i * rowH;
            Text(p.ui, std::string(rows[i].id) + ".l", lx, ry + 10.0f, 130.0f, 22.0f, rows[i].label, 14.5f, kText, 600);
            components::input(p.ui, rows[i].id).theme(Th()).position(ix, ry).size(iw, 40.0f)
                .placeholder(rows[i].ph).bind(*rows[i].sig).fontSize(13.5f).build();
        }
        const float by = p.y + 16.0f + 3 * rowH + 4.0f;
        components::button(p.ui, "nm.save").theme(Th(), true).position(ix, by).size(120.0f, 38.0f)
            .text(i18n::T("保存")).fontSize(14.0f)
            .onClick([] {
                S.link.send("{\"cmd\":\"names\",\"name\":" + JStr(S.nmName.get()) + ",\"nick\":" + JStr(S.nmNick.get())
                            + ",\"call_user\":" + JStr(S.nmCall.get()) + "}");
                Toast(i18n::T("已保存：她会马上用新的名字和称呼说话（本地台词稍后重写）"));
            }).build();
        components::button(p.ui, "nm.clear").theme(Th(), false).position(ix + 132.0f, by).size(120.0f, 38.0f)
            .text(i18n::T("恢复默认")).fontSize(14.0f)
            .onClick([] {
                S.nmName.set("");
                S.nmNick.set("");
                S.nmCall.set("");
                S.link.send("{\"cmd\":\"names\",\"name\":\"\",\"nick\":\"\",\"call_user\":\"\"}");
                Toast(i18n::T("已恢复：名字用性格里生成的，称呼按性格和记忆"));
            }).build();
        Text(p.ui, "nm.h", lx, by + 48.0f, p.w - 36.0f, 20.0f,
             "每个模型一份（存在它的性格文件里，重新生成性格也保留）；对话、即时台词、笑话和提问都会用这里的名字与称呼。",
             12.0f, kMuted, 400, true);
        p.y += cardH + 12.0f;
    }

    // ---- 模型库 · 切换 / 删除
    Section(p, "sec.lib", "模型库 · 切换 / 删除（~/.local/share/l2dpet/models/）");
    const Value& ms = S.st.get("models");
    const int cur = (int)S.st.get("model_index").numOr(0);
    if (!S.modelDelName.empty())   // 架起的删除目标不在了（删完/列表刷新）→ 收起确认态
    {
        bool found = false;
        for (size_t i = 0; i < ms.arr.size() && !found; ++i) found = ms.arr[i].str == S.modelDelName;
        if (!found) S.modelDelName.clear();
    }
    Card(p.ui, "lib.card", p.x, p.y, p.w, 64.0f + (float)ms.arr.size() * 48.0f);
    {
        float ly = p.y + 14.0f;
        for (size_t i = 0; i < ms.arr.size(); ++i)
        {
            const int idx = (int)i;
            const bool curM = idx == cur;
            const std::string nm = ms.arr[i].str;
            const bool armed = !curM && S.modelDelName == nm;   // 删除要按两次（防误删）
            if (curM)
            {
                p.ui.rect("lib.hl." + std::to_string(i)).position(p.x + 10.0f, ly - 4.0f).size(p.w - 20.0f, 44.0f)
                    .color(kHover).radius(10.0f).build();
            }
            Text(p.ui, "lib.n." + std::to_string(i), p.x + 26.0f, ly + 9.0f, p.w - 300.0f, 24.0f,
                 (curM ? "● " : "") + nm, 15.0f, curM ? kAccent : kText, curM ? 700 : 500);
            if (curM)
            {
                Text(p.ui, "lib.cur", p.x + p.w - 116.0f, ly + 12.0f, 88.0f, 22.0f, "使用中", 13.0f, kAccent, 600,
                     false, core::HorizontalAlign::Center);
            }
            else
            {
                components::button(p.ui, "lib.b." + std::to_string(i)).theme(Th(), false)
                    .position(p.x + p.w - 116.0f, ly + 3.0f).size(88.0f, 36.0f).text(i18n::T("切换")).fontSize(13.5f)
                    .onClick([idx] {
                        S.modelDelName.clear();
                        S.link.send("{\"cmd\":\"model\",\"index\":" + std::to_string(idx) + "}");
                        Toast("正在切换模型…");
                    }).build();
                components::button(p.ui, "lib.d." + std::to_string(i)).theme(Th(), armed)
                    .position(p.x + p.w - 210.0f, ly + 3.0f).size(88.0f, 36.0f)
                    .text(i18n::T(armed ? "确认删除？" : "删除")).fontSize(13.0f)
                    .onClick([nm] {
                        if (S.modelDelName != nm)
                        {
                            S.modelDelName = nm;   // 第一次只"架起来"
                            Toast(i18n::TF("再点一次「确认删除」会删除「{}」", { nm }));
                            return;
                        }
                        S.modelDelName.clear();
                        S.link.send("{\"cmd\":\"delete_model\",\"name\":" + JStr(nm) + "}");
                        Toast(i18n::TF("正在删除「{}」…", { nm }));
                    }).build();
            }
            ly += 48.0f;
        }
    }
    p.y += 64.0f + (float)ms.arr.size() * 48.0f + 12.0f;

    // ---- 导入 / 探测
    Section(p, "sec.imp", "导入新模型 / 能力探测");
    Card(p.ui, "imp.card", p.x, p.y, p.w, 176.0f);
    Text(p.ui, "imp.l", p.x + 18.0f, p.y + 16.0f, 200.0f, 22.0f, "模型路径（.zip 或目录）", 14.0f, kText, 600);
    components::input(p.ui, "imp.path").theme(Th()).position(p.x + 18.0f, p.y + 44.0f).size(p.w - 460.0f, 40.0f)
        .placeholder(i18n::T("点右边「选 zip…/选目录…」挑一个，或手填绝对路径")).bind(S.impPath).fontSize(13.5f).build();
    components::button(p.ui, "imp.pickzip").theme(Th(), false).position(p.x + p.w - 340.0f, p.y + 44.0f).size(100.0f, 40.0f)
        .text(i18n::T("选 zip…")).fontSize(13.0f)
        .onClick([] {
            PickPathAsync(false, false, S.impPath.get(), i18n::T("桌宠：选择模型 zip"), [](int r, const std::string& picked) {
                if (r == 1) { S.impPath.set(picked); Toast(i18n::TF("已选择：{}", { picked })); }
                else if (r == -1) Toast(i18n::T("打不开系统文件对话框：请在输入框里填路径"));
            });
        }).build();
    components::button(p.ui, "imp.pickdir").theme(Th(), false).position(p.x + p.w - 232.0f, p.y + 44.0f).size(100.0f, 40.0f)
        .text(i18n::T("选目录…")).fontSize(13.0f)
        .onClick([] {
            PickPathAsync(false, true, S.impPath.get(), i18n::T("桌宠：选择模型目录（含 model3.json）"), [](int r, const std::string& picked) {
                if (r == 1) { S.impPath.set(picked); Toast(i18n::TF("已选择：{}", { picked })); }
                else if (r == -1) Toast(i18n::T("打不开系统文件对话框：请在输入框里填路径"));
            });
        }).build();
    components::button(p.ui, "imp.go").theme(Th(), true).position(p.x + p.w - 122.0f, p.y + 44.0f).size(104.0f, 40.0f)
        .text(i18n::T("导入并识别")).fontSize(13.5f)
        .onClick([&] {
            const std::string v = S.impPath.get();
            if (!IsAbsPath(v))
            {
                Toast("给绝对路径（.zip 或含 model3.json 的目录）");
                return;
            }
            S.link.send("{\"cmd\":\"import_model\",\"path\":" + JStr(v) + "}");
            Toast("正在导入…（大模型要等几秒）");
        }).build();
    Text(p.ui, "imp.h", p.x + 18.0f, p.y + 92.0f, p.w - 36.0f, 40.0f,
         "zip 会解压到模型库并按目录名命名（重名自动加 -2）；目录会被拷贝（原文件夹不动）；导入后自动切换并识别能力（指纹/表情/动作/躯干/分区）。",
         12.5f, kMuted, 400, true);
    {
        ButtonFlow bf{ p.ui, p.x + 18.0f, p.x + 18.0f, p.y + 128.0f, p.x + p.w - 18.0f };
        bf.add("imp.probe", "能力探测（约 30 秒）", [] {
            S.link.send("{\"cmd\":\"probe\"}");
            Toast("能力探测已开始，完成后自动重载模型…");
        }, !caps);
    }
    p.y += 176.0f + 20.0f;
}

void PageLLM(Pen& p)
{
    PageTitle(p, "对话（LLM）");
    const Value& llm = S.st.get("llm");
    bool llmGood = false;
    const std::string llmLab = LlmStateLabel(&llmGood);
    const std::string llmSt = S.st.get("llm_state").strOr("no_agent");
    Chip(p.ui, "chip.agent", p.x + 170.0f, p.y - 50.0f, llmLab, llmGood ? kGreen : (llmSt == "error" ? kDanger : kMuted));
    if (llmSt == "error")   // 失败原因直接写出来（Key 错 / 网络 / 端点不对），不用去翻日志
    {
        const std::string why = S.st.get("llm_err").strOr("");
        Text(p.ui, "llm.err", p.x + 2.0f, p.y - 4.0f, p.w, 18.0f, i18n::TF("最近一次调用失败：{}", { why }), 12.0f, kDanger);
        p.y += 22.0f;
    }
    // 供应商编辑器：首次从 panel_state 载入工作副本
    if (!S.llmInit && S.st.isObj())
    {
        for (const Value& pr : llm.get("providers").arr)
        {
            AppState::LlmProvEd e;
            e.id = pr.get("id").strOr("");
            e.name = pr.get("name").strOr("");
            e.url = pr.get("base_url").strOr("");
            e.keyTail = pr.get("key_tail").strOr("");
            e.keySet = pr.get("key_set").isBool() && pr.get("key_set").b;
            e.model = pr.get("model").strOr("");
            for (const Value& m : pr.get("models").arr) e.models.push_back(m.strOr(""));
            if (!e.id.empty()) S.llmProvs.push_back(e);
        }
        S.llmEditId = llm.get("active").strOr("");
        if (S.llmEditId.empty() && !S.llmProvs.empty()) S.llmEditId = S.llmProvs.front().id;
        S.llmInit = true;
    }
    // 编辑项查找 + 字段回流/外发
    auto findEdit = [&]() -> AppState::LlmProvEd* {
        for (auto& e : S.llmProvs) if (e.id == S.llmEditId) return &e;
        return S.llmProvs.empty() ? nullptr : &S.llmProvs.front();
    };
    auto flushEdit = [&]() {   // 把输入框内容写回工作副本（切换/保存前调用）
        AppState::LlmProvEd* e = findEdit();
        if (e == nullptr) return;
        e->name = S.lpName.get();
        e->url = S.lpUrl.get();
        if (!S.lpKey.get().empty())
        {
            S.llmNewKeys[e->id] = S.lpKey.get();
            S.lpKey.set("");
        }
    };
    auto loadEdit = [&]() {    // 工作副本 → 输入框
        AppState::LlmProvEd* e = findEdit();
        if (e == nullptr) { S.lpName.set(""); S.lpUrl.set(""); return; }
        S.lpName.set(e->name);
        S.lpUrl.set(e->url);
        S.lpKey.set("");
    };
    static bool editLoaded = false;
    if (S.llmInit && !editLoaded)
    {
        loadEdit();
        editLoaded = true;
    }

    Section(p, "sec.prov", "供应商（同一时间只用激活的那一个）");
    AppState::LlmProvEd* cur = findEdit();
    float testCardH = 0.0f;   // 「尝试连接」结果卡占的高度（下面算卡片高度时要算进去）
    // 供应商 chips 行
    const float chipH = 40.0f;
    float cx0 = p.x + 18.0f, cy0 = p.y + 14.0f;
    const float cardH =
        (14.0f + chipH + 14.0f)                                       // chips 行
        + (cur != nullptr ? 3 * 60.0f + 52.0f : 60.0f)                 // 字段区
        + (cur != nullptr ? 40.0f + std::max(1.0f, (float)cur->models.size()) * 40.0f + 14.0f : 0.0f)   // 模型列表
        + 52.0f + 30.0f;                                               // 按钮行 + 提示
    Card(p.ui, "prov.card", p.x, p.y, p.w, cardH);
    for (size_t i = 0; i < S.llmProvs.size(); ++i)
    {
        const bool active = S.llmProvs[i].id == S.llmEditId;
        char id[32];
        snprintf(id, sizeof(id), "pchip%d", (int)i);
        const std::string title = S.llmProvs[i].name.empty() ? "（未命名）" : S.llmProvs[i].name;
        const float w = std::min(200.0f, 26.0f + title.size() * 8.5f + 26.0f);
        const std::string pid = S.llmProvs[i].id;
        components::button(p.ui, id).theme(Th(), active).position(cx0, cy0).size(w, chipH)
            .text((active ? "● " : "") + title).fontSize(14.0f)
            .onClick([&, pid] {
                flushEdit();
                S.llmEditId = pid;
                loadEdit();
                app::requestUpdate();
            }).build();
        cx0 += w + 8.0f;
    }
    {
        components::button(p.ui, "padd").theme(Th(), false).position(cx0, cy0).size(96.0f, chipH)
            .text(i18n::T("+ 添加")).fontSize(14.0f)
            .onClick([&] {
                flushEdit();
                AppState::LlmProvEd e;
                char idbuf[32];
                snprintf(idbuf, sizeof(idbuf), "p%d", ++S.llmSeq + (int)time(NULL) % 1000);
                e.id = idbuf;
                e.name = "新供应商";
                e.url = "";
                e.model = "";
                S.llmProvs.push_back(e);
                S.llmEditId = e.id;
                loadEdit();
                S.llmInit = true;
                app::requestUpdate();
            }).build();
    }
    float py = p.y + 14.0f + chipH + 14.0f;
    if (cur != nullptr)
    {
        auto field = [&](const char* id, const std::string& label, eui::Signal<std::string>& sig, const std::string& ph) {
            Text(p.ui, std::string(id) + ".l", p.x + 18.0f, py + 9.0f, 92.0f, 22.0f, label, 15.0f, kText, 500);
            components::input(p.ui, id).theme(Th()).position(p.x + 118.0f, py).size(p.w - 136.0f, 40.0f)
                .placeholder(i18n::T(ph)).bind(sig).fontSize(14.0f).build();
            py += 60.0f;
        };
        const std::string keyPh = cur->keySet
            ? i18n::TF("已保存（末 4 位 {}）；粘贴新的可替换", { cur->keyTail })
            : i18n::T("未设置（必填）");
        field("lp.name", "名称", S.lpName, "DeepSeek / 智谱 / 本地 Ollama…");
        field("lp.url", "接口地址", S.lpUrl, "https://api.deepseek.com（默认）");
        field("lp.key", "API Key", S.lpKey, keyPh);
        // 模型列表
        Divider(p.ui, "prov.d1", p.x + 18.0f, py - 12.0f, p.w - 36.0f);
        Text(p.ui, "prov.ml", p.x + 18.0f, py + 6.0f, 200.0f, 22.0f, "模型列表", 15.0f, kText, 600);
        {
            components::input(p.ui, "lp.nm").theme(Th()).position(p.x + p.w - 300.0f, py).size(190.0f, 36.0f)
                .placeholder(i18n::T("新模型名，如 deepseek-chat")).bind(S.lpNewModel).fontSize(13.5f)
                .onEnter([&] {
                    const std::string m = S.lpNewModel.get();
                    if (!m.empty() && findEdit() != nullptr)
                    {
                        AppState::LlmProvEd* e = findEdit();
                        e->models.push_back(m);
                        e->model = m;
                        S.lpNewModel.set("");
                        app::requestUpdate();
                    }
                }).build();
            components::button(p.ui, "lp.addm").theme(Th(), false).position(p.x + p.w - 102.0f, py).size(84.0f, 36.0f)
                .text(i18n::T("+ 添加模型")).fontSize(13.0f)
                .onClick([&] {
                    const std::string m = S.lpNewModel.get();
                    AppState::LlmProvEd* e = findEdit();
                    if (!m.empty() && e != nullptr)
                    {
                        e->models.push_back(m);
                        e->model = m;
                        S.lpNewModel.set("");
                        app::requestUpdate();
                    }
                }).build();
        }
        py += 40.0f;
        if (cur->models.empty())
        {
            Text(p.ui, "prov.nomodel", p.x + 24.0f, py + 6.0f, p.w - 48.0f, 22.0f,
                 "还没有模型：在上面输入模型名（如 deepseek-chat）后回车/点添加", 13.0f, kMuted);
            py += 40.0f;
        }
        for (size_t i = 0; i < cur->models.size(); ++i)
        {
            const bool use = cur->models[i] == cur->model;
            char id[32];
            snprintf(id, sizeof(id), "pm%d", (int)i);
            Text(p.ui, std::string(id) + ".t", p.x + 34.0f, py + 9.0f, p.w - 240.0f, 22.0f, cur->models[i], 14.0f,
                 use ? kAccent : kText, use ? 600 : 500);
            if (use) Text(p.ui, std::string(id) + ".ck", p.x + 18.0f, py + 8.0f, 18.0f, 20.0f, "✓", 14.0f, kAccent, 700);
            const std::string mv = cur->models[i];
            components::button(p.ui, std::string(id) + ".use").theme(Th(), false).position(p.x + p.w - 180.0f, py).size(72.0f, 34.0f)
                .text(i18n::T(use ? "使用中" : "选用")).fontSize(13.0f)
                .onClick([&, mv] {
                    AppState::LlmProvEd* e = findEdit();
                    if (e != nullptr) { e->model = mv; app::requestUpdate(); }
                }).build();
            components::button(p.ui, std::string(id) + ".del").theme(Th(), false).position(p.x + p.w - 100.0f, py).size(72.0f, 34.0f)
                .text(i18n::T("删除")).fontSize(13.0f)
                .onClick([&, i] {
                    AppState::LlmProvEd* e = findEdit();
                    if (e != nullptr && i < e->models.size())
                    {
                        if (e->model == e->models[i]) e->model = "";
                        e->models.erase(e->models.begin() + (long)i);
                        if (e->model.empty() && !e->models.empty()) e->model = e->models.front();
                        app::requestUpdate();
                    }
                }).build();
            py += 40.0f;
        }
        py += 14.0f;
        // 保存并激活 / 删除该供应商
        ButtonFlow bf{ p.ui, p.x + 18.0f, p.x + 18.0f, py, p.x + p.w - 18.0f };
        bf.add("prov.save", "保存并激活", [&] {
            flushEdit();
            AppState::LlmProvEd* e = findEdit();
            if (e == nullptr) return;
            if (e->name.empty()) { Toast("供应商要有名字"); return; }
            if (!e->url.empty() && e->url.rfind("http://", 0) != 0 && e->url.rfind("https://", 0) != 0)
            {
                Toast("接口地址要以 http:// 或 https:// 开头");
                return;
            }
            std::string cmd = "{\"cmd\":\"llm_providers\",\"active\":" + JStr(S.llmEditId) + ",\"providers\":[";
            std::map<std::string, std::string> keyedTails;   // 本次新下发的密钥（id → 末 4 位）
            for (size_t i = 0; i < S.llmProvs.size(); ++i)
            {
                const AppState::LlmProvEd& pr = S.llmProvs[i];
                cmd += i ? ",{" : "{";
                cmd += "\"id\":" + JStr(pr.id) + ",\"name\":" + JStr(pr.name) + ",\"base_url\":" + JStr(pr.url)
                       + ",\"model\":" + JStr(pr.model) + ",\"models\":[";
                for (size_t k = 0; k < pr.models.size(); ++k)
                {
                    if (k) cmd += ",";
                    cmd += JStr(pr.models[k]);
                }
                cmd += "]";
                std::map<std::string, std::string>::const_iterator nk = S.llmNewKeys.find(pr.id);
                if (nk != S.llmNewKeys.end() && !nk->second.empty())
                {
                    cmd += ",\"api_key\":" + JStr(nk->second);   // 只在新填了密钥时下发（其余按 id 保留）
                    const std::string& k = nk->second;
                    keyedTails[pr.id] = k.size() >= 4 ? k.substr(k.size() - 4) : k;
                }
                cmd += "}";
            }
            cmd += "]}";
            S.link.send(cmd);
            // 工作副本只在启动时从桌宠状态载入一次：保存后不就地更新"已保存（末 4 位 …）"提示的话，
            // 输入框会一直提示"未设置（必填）"，看着像密钥没存上（其实已经写进 llm.json 了）。
            for (size_t i = 0; i < S.llmProvs.size(); ++i)
            {
                std::map<std::string, std::string>::const_iterator t = keyedTails.find(S.llmProvs[i].id);
                if (t != keyedTails.end())
                {
                    S.llmProvs[i].keySet = true;
                    S.llmProvs[i].keyTail = t->second;
                }
            }
            S.llmNewKeys.clear();
            S.lpKey.set("");
            Toast(i18n::TF("已保存：{} 生效（agent 下一次调用就用它）", { e->name }));
        }, true);
        // 试连接：不保存也能测当前表单（地址/模型/刚粘进来的密钥），走桌宠真实调用那条路
        bf.add("prov.test", "尝试连接", [&] {
            flushEdit();
            AppState::LlmProvEd* e = findEdit();
            if (e == nullptr) { Toast("先加一个供应商"); return; }
            if (e->url.empty()) { Toast("先填接口地址"); return; }
            if (e->model.empty()) { Toast("先在下面「模型列表」里点「选用」挑一个模型"); return; }
            std::string typed;
            std::map<std::string, std::string>::const_iterator nk = S.llmNewKeys.find(e->id);
            if (nk != S.llmNewKeys.end()) typed = nk->second;   // 刚键入还没保存的密钥也拿来试
            const std::string url = e->url, model = e->model;
            {
                std::lock_guard<std::mutex> lk(g_llmTestMu);
                g_llmTestState = 1;
                g_llmTestInfo.clear();
                g_llmTestUrl = url;
            }
            app::requestUpdate();
            std::thread([url, model, typed] {
                std::string cmd = "{\"cmd\":\"llm_test\",\"base_url\":" + JStr(url)
                                  + ",\"llm_model\":" + JStr(model) + "}";
                if (!typed.empty())
                {
                    cmd = cmd.substr(0, cmd.size() - 1) + ",\"api_key\":" + JStr(typed) + "}";
                }
                std::string reply, info, shownUrl = url;
                int state = 3;
                if (!S.link.request(cmd, &reply))
                {
                    info = i18n::T("连不上桌宠：它没在跑，或控制端口不是面板连的那个（看「状态」页）");
                }
                else
                {
                    Value v;
                    if (jsonlite::parse(reply.c_str(), reply.size(), v, nullptr) && v.isObj()
                        && v.get("result").isObj())
                    {
                        const Value& r = v.get("result");
                        const bool ok = r.get("ok").isBool() && r.get("ok").b;
                        shownUrl = r.get("url").strOr(url.c_str());
                        if (ok)
                        {
                            state = 2;
                            info = i18n::TF("连上了：{} 协议 · 用时 {} 秒 · 模型回「{}」",
                                            { r.get("protocol").strOr(""), Num1((double)r.get("ms").numOr(0) / 1000.0),
                                              r.get("sample").strOr("") });
                        }
                        else
                        {
                            info = i18n::TF("试连接失败：{}", { r.get("error").strOr("") });
                        }
                    }
                    else
                    {
                        info = i18n::T("桌宠的应答看不懂（版本对不上？）");
                    }
                }
                std::lock_guard<std::mutex> lk(g_llmTestMu);
                g_llmTestState = state;
                g_llmTestInfo = info;
                g_llmTestUrl = shownUrl;
                app::requestUpdate();
            }).detach();
        });
        if (S.llmProvs.size() > 1)
        {
            bf.add("prov.del", "删除该供应商", [&] {
                const std::string delId = S.llmEditId;
                for (size_t i = 0; i < S.llmProvs.size(); ++i)
                {
                    if (S.llmProvs[i].id == delId)
                    {
                        S.llmProvs.erase(S.llmProvs.begin() + (long)i);
                        break;
                    }
                }
                S.llmNewKeys.erase(delId);
                S.llmEditId = S.llmProvs.empty() ? "" : S.llmProvs.front().id;
                loadEdit();
                // 立刻全量保存（保持"同一时间只有一个激活"的约束）
                std::string cmd = "{\"cmd\":\"llm_providers\",\"active\":" + JStr(S.llmEditId) + ",\"providers\":[";
                for (size_t i = 0; i < S.llmProvs.size(); ++i)
                {
                    const AppState::LlmProvEd& pr = S.llmProvs[i];
                    cmd += i ? ",{" : "{";
                    cmd += "\"id\":" + JStr(pr.id) + ",\"name\":" + JStr(pr.name) + ",\"base_url\":" + JStr(pr.url)
                           + ",\"model\":" + JStr(pr.model) + ",\"models\":[";
                    for (size_t k = 0; k < pr.models.size(); ++k)
                    {
                        if (k) cmd += ",";
                        cmd += JStr(pr.models[k]);
                    }
                    cmd += "]}";
                }
                cmd += "]}";
                S.link.send(cmd);
                Toast("已删除该供应商");
            });
        }
        py = bf.y + 38.0f;
        const std::string hint = i18n::TF("存于 {}（仅本用户可读，0600）；环境变量优先",
                                          { std::string(llm.get("path").strOr("")) });
        Text(p.ui, "prov.hint", p.x + 18.0f, py + 4.0f, p.w - 36.0f, 20.0f, hint, 12.5f, kMuted);
        // 「尝试连接」的结果：状态 + 详情 + 实际请求地址（地址填错时一眼看出拼成了什么）
        int testState = 0;
        std::string testInfo, testUrl;
        {
            std::lock_guard<std::mutex> lk(g_llmTestMu);
            testState = g_llmTestState;
            testInfo = g_llmTestInfo;
            testUrl = g_llmTestUrl;
        }
        if (testState != 0)
        {
            const std::string testHead = testState == 1 ? i18n::T("正在连接…")
                                        : (testState == 2 ? i18n::T("试连接成功") : i18n::T("试连接失败"));
            const float infoH = testInfo.empty() ? 0.0f : TextH(testInfo, p.w - 36.0f, 12.5f, 20.0f);
            const std::string urlLine = i18n::TF("实际请求：{}", { testUrl });
            const float urlH = TextH(urlLine, p.w - 36.0f, 12.0f, 18.0f);
            const float cardH = infoH + urlH + 46.0f;
            Card(p.ui, "lt.card", p.x, py + 30.0f, p.w, cardH);
            Text(p.ui, "lt.head", p.x + 18.0f, py + 42.0f, p.w - 36.0f, 20.0f, testHead, 13.5f,
                 testState == 2 ? kAccent : (testState == 3 ? kDanger : kMuted), 700);
            if (!testInfo.empty())
            {
                Text(p.ui, "lt.info", p.x + 18.0f, py + 66.0f, p.w - 36.0f, infoH, testInfo, 12.5f,
                     testState == 3 ? kDanger : kText, 400, true);
            }
            Text(p.ui, "lt.url", p.x + 18.0f, py + 70.0f + infoH, p.w - 36.0f, urlH, urlLine, 12.0f, kMuted, 400, true);
            testCardH = cardH + 34.0f;
        }
    }
    else
    {
        Text(p.ui, "prov.empty", p.x + 18.0f, py + 6.0f, p.w - 36.0f, 22.0f,
             "还没有供应商：点上面「+ 添加」新建一个（名称 / 接口地址 / API Key / 模型列表）", 13.5f, kMuted);
    }
    p.y += 14.0f + chipH + 14.0f + (cur != nullptr ? 3 * 60.0f + 52.0f + 40.0f + std::max(1.0f, (float)cur->models.size()) * 40.0f + 14.0f + 52.0f + 30.0f : 60.0f) + testCardH;
    p.y += 22.0f;

    Section(p, "sec.tokens", "Token 节省");
    const int tier = std::max(0, std::min(3, (int)std::lround(Cfg("llm_token_tier", 1))));
    const char* tokenHints[] = {
        "完整：最近 6 条消息，按原间隔互动，回复使用供应商默认上限。",
        "均衡：最近 4 条消息，小剧场间隔 ×1.5，回复上限 1536 Token。",
        "节省：最近 2 条消息，小剧场间隔 ×3，回复上限 1024 Token。",
        "极省：仅当前问题，关闭自动小剧场，回复上限 768 Token。"
    };
    const std::string tokenHint = i18n::T(tokenHints[tier]);
    const std::string tokenNote = i18n::T("不删除已保存的记忆；性格、台词库和记忆整理保留独立预算。完整档可提前补题，其余档题库问完再更新。实际节省量取决于使用情况。");
    const float tokenHintH = TextH(tokenHint, p.w - 36.0f, 12.5f, 20.0f);
    const float tokenNoteH = TextH(tokenNote, p.w - 36.0f, 12.5f, 20.0f);
    Card(p.ui, "tokens.card", p.x, p.y, p.w, SliderH(p.w) + tokenHintH + tokenNoteH + 24.0f);
    SliderRow(p, "s.tokens", "节省档位", "llm_token_tier", 0.0f, 3.0f, 1.0f, [](float v) {
        const char* names[] = { "完整", "均衡", "节省", "极省" };
        return i18n::T(names[std::max(0, std::min(3, (int)std::lround(v)))]);
    });
    Text(p.ui, "tokens.hint", p.x + 18.0f, p.y + 4.0f, p.w - 36.0f, tokenHintH, tokenHint, 12.5f, kMuted, 400, true);
    Text(p.ui, "tokens.note", p.x + 18.0f, p.y + tokenHintH + 10.0f, p.w - 36.0f, tokenNoteH, tokenNote, 12.5f, kMuted, 400, true);
    p.y += tokenHintH + tokenNoteH + 46.0f;

    Section(p, "sec.part", "参与度");
    Card(p.ui, "part.card", p.x, p.y, p.w, 2 * ToggleH() + SliderH(p.w));
    ToggleRow(p, "t.idle", "闲时小剧场", "没人互动时，LLM 偶尔安排一段小日常", "llm_idle");
    Divider(p.ui, "d1", p.x, p.y, p.w);
    SliderRow(p, "s.period", "小剧场平均间隔", "llm_period", 20.0f, 600.0f, 10.0f,
              [](float v) { return std::to_string((int)v) + i18n::T(" 秒"); });
    Divider(p.ui, "d2", p.x, p.y, p.w);
    ToggleRow(p, "t.q", "允许提问（气泡选项）", "LLM 偶尔在气泡里给你几个选项", "llm_questions");
    p.y += 22.0f;

    // 统计 + 日志入口
    const Value& stt = S.st.get("llm_stats");
    const Value& k = stt.get("kinds");
    const std::string l1 = i18n::TF("交互 {} 次：回应 {} · 闲时 {} · 续戏 {} · 重逢 {} · 台词库 {} · 记忆 {} · 提问 {}",
                                    { std::to_string((int)stt.get("calls").numOr(0)), std::to_string((int)k.get("回应").numOr(0)),
                                      std::to_string((int)k.get("闲时").numOr(0)), std::to_string((int)k.get("续戏").numOr(0)),
                                      std::to_string((int)k.get("重逢").numOr(0)), std::to_string((int)k.get("台词库").numOr(0)),
                                      std::to_string((int)k.get("记忆").numOr(0)),
                                      std::to_string((int)stt.get("questions").numOr(0)) });
    const std::string l2 = i18n::TF("失败 {} · 用量 {}→{} tokens · 平均 {} 秒",
                                    { std::to_string((int)stt.get("errors").numOr(0)),
                                      std::to_string((int)stt.get("tok_in").numOr(0)),
                                      std::to_string((int)stt.get("tok_out").numOr(0)),
                                      Num1(stt.get("avg_ms").numOr(0) / 1000.0) });
    const float th = TextH(l1, p.w - 36.0f, 14.0f, 20.0f);
    const std::string tokenStats = i18n::TF("最近一次 {}→{} Token · 本次运行对话与维护累计 {}→{} Token",
        { std::to_string((long long)stt.get("last_tok_in").numOr(0)),
          std::to_string((long long)stt.get("last_tok_out").numOr(0)),
          std::to_string((long long)stt.get("session_tok_in").numOr(0)),
          std::to_string((long long)stt.get("session_tok_out").numOr(0)) });
    const float tokenStatsH = TextH(tokenStats, p.w - 36.0f, 12.5f, 20.0f);
    // 台词库：最近一次成功更新时间（+ 组数/句数），有失败的话再列一条原因——
    // "到底更新了没有"要一眼看得出来（0.0 秒/超时都让人怀疑）
    const std::string linesAt = stt.get("lines_at").strOr("");
    const std::string linesNote = stt.get("lines_note").strOr("");
    const std::string linesFailAt = stt.get("lines_fail_at").strOr("");
    const std::string linesFailErr = stt.get("lines_fail_err").strOr("");
    const std::string linesLine = linesAt.empty()
        ? i18n::T("台词库：本次运行还没成功更新过（用内置台词兜底）")
        : i18n::TF("台词库：{} 更新 · {}", { linesAt, linesNote });
    const std::string linesFailLine = linesFailAt.empty()
        ? std::string()
        : i18n::TF("最近一次失败：{} · {}", { linesFailAt, linesFailErr });
    const float linesH = TextH(linesLine, p.w - 36.0f, 12.5f, 20.0f);
    const float linesFailH = linesFailLine.empty() ? 0.0f : TextH(linesFailLine, p.w - 36.0f, 12.5f, 20.0f);
    Card(p.ui, "stat.card", p.x, p.y, p.w, th + tokenStatsH + linesH + linesFailH + 104.0f);
    Text(p.ui, "stat.l1", p.x + 18.0f, p.y + 14.0f, p.w - 36.0f, th, l1, 14.0f, kText, 500, true);
    Text(p.ui, "stat.l2", p.x + 18.0f, p.y + 18.0f + th, p.w - 36.0f, 20.0f, l2, 12.5f, (int)stt.get("errors").numOr(0) > 0 ? kDanger : kMuted);
    Text(p.ui, "stat.tokens", p.x + 18.0f, p.y + th + 42.0f, p.w - 36.0f, tokenStatsH, tokenStats, 12.5f, kMuted, 400, true);
    Text(p.ui, "stat.lines", p.x + 18.0f, p.y + th + tokenStatsH + 48.0f, p.w - 36.0f, linesH, linesLine, 12.5f,
         linesAt.empty() ? kMuted : kText, 400, true);
    if (!linesFailLine.empty())
    {
        Text(p.ui, "stat.linesfail", p.x + 18.0f, p.y + th + tokenStatsH + linesH + 54.0f, p.w - 36.0f, linesFailH,
             linesFailLine, 12.5f, kDanger, 400, true);
    }
    components::button(p.ui, "b.log").theme(Th(), false)
        .position(p.x + 18.0f, p.y + th + tokenStatsH + linesH + linesFailH + 56.0f).size(120.0f, 36.0f)
        .text(i18n::T("交互日志 ›")).fontSize(14.0f).onClick([] { S.page = P_LOG; S.scroll[P_LOG].set(0.0f); }).build();
    p.y += th + tokenStatsH + linesH + linesFailH + 124.0f;
}

void PageLog(Pen& p)
{
    components::button(p.ui, "b.back").theme(Th(), false).position(p.x, p.y).size(84.0f, 34.0f).text(i18n::T("‹ 返回")).fontSize(14.0f)
        .onClick([] { S.page = P_LLM; }).build();
    Text(p.ui, "title", p.x + 98.0f, p.y, p.w - 98.0f, 34.0f, "交互日志", 24.0f, kText, 800);
    p.y += 44.0f;
    const Value& lg = S.st.get("llm_log");
    const std::string sub = i18n::TF("最近 {} 条（本次运行）· 完整记录在 ~/.local/state/l2dpet/llm-log.jsonl",
                                     { std::to_string(lg.arr.size()) });
    Text(p.ui, "sub", p.x, p.y, p.w, TextH(sub, p.w, 13.0f, 18.0f), sub, 13.0f, kMuted, 400, true);
    p.y += TextH(sub, p.w, 13.0f, 18.0f) + 14.0f;
    if (lg.arr.empty())
    {
        Text(p.ui, "empty", p.x, p.y, p.w, 22.0f, "还没有记录：agent 每调用一次 LLM 会在这里记一条。", 14.0f, kMuted);
        p.y += 40.0f;
        return;
    }
    for (int i = (int)lg.arr.size() - 1, n = 0; i >= 0 && n < 60; --i, ++n)
    {
        const Value& e = lg.arr[(size_t)i];
        const bool ok = !(e.get("ok").isBool() && !e.get("ok").b);
        const std::string id = "log." + std::to_string(n);
        std::string note = e.get("note").strOr("");
        const size_t rb = note.find("】");
        if (rb != std::string::npos && rb < 200) note = note.substr(rb + 3);
        std::string out;
        if (!ok) out = i18n::TF("错误：{}", { std::string(e.get("err").strOr("")) });
        else
        {
            for (size_t j = 0; j < e.get("say").arr.size(); ++j)
                out += (j ? " / " : i18n::T("她说：「")) + e.get("say").arr[j].str;
            if (!e.get("say").arr.empty()) out += i18n::T("」");
            std::string tags;
            for (size_t j = 0; j < e.get("emotions").arr.size(); ++j) tags += (tags.empty() ? "" : " · ") + e.get("emotions").arr[j].str;
            for (size_t j = 0; j < e.get("actions").arr.size(); ++j) tags += (tags.empty() ? "" : " · ") + e.get("actions").arr[j].str;
            if (!tags.empty()) out += (out.empty() ? "" : "  ") + std::string("[") + tags + "]";
            if (out.empty()) out = "（没说话）";
        }
        const float tw = p.w - 32.0f;
        const std::string nline = i18n::TF("情境：{}", { note });
        const float nh = TextH(nline, tw, 13.0f, 19.0f), oh = TextH(out, tw, 13.0f, 19.0f);
        const float h = 44.0f + nh + oh + 16.0f;
        Card(p.ui, id + ".c", p.x, p.y, p.w, h);
        Text(p.ui, id + ".t", p.x + 16.0f, p.y + 12.0f, 80.0f, 20.0f, e.get("t").strOr(""), 13.0f, kMuted);
        const std::string kind = e.get("kind").strOr("?");
        const eui::Color kc = !ok ? kDanger : (kind == "回应" ? kPink : (kind == "闲时" ? kAccent : kPurple));
        Chip(p.ui, id + ".k", p.x + 90.0f, p.y + 9.0f, kind, kc);
        const std::string meta = ok
            ? i18n::TF("{} 秒 · {}→{} tok{}",
                       { Num1(e.get("ms").numOr(0) / 1000.0), std::to_string((int)e.get("tok_in").numOr(0)),
                         std::to_string((int)e.get("tok_out").numOr(0)),
                         (e.get("question").isBool() && e.get("question").b) ? i18n::T(" · 提问") : std::string() })
            : i18n::T("失败");
        Text(p.ui, id + ".m", p.x + p.w - 216.0f, p.y + 12.0f, 200.0f, 20.0f, meta, 13.0f, ok ? kMuted : kDanger, 400,
             false, core::HorizontalAlign::Right);
        Text(p.ui, id + ".n", p.x + 16.0f, p.y + 42.0f, tw, nh, nline, 13.0f, kText, 400, true);
        Text(p.ui, id + ".o", p.x + 16.0f, p.y + 46.0f + nh, tw, oh, out, 13.0f, ok ? kMuted : kDanger, 400, true);
        p.y += h + 10.0f;
    }
    p.y += 10.0f;
}

void PageMemory(Pen& p)
{
    PageTitle(p, "记忆", "她记得的关于你、以及你们之间的事（人可读、可手改：~/.config/l2dpet/memory/）");
    // ---- 记忆维护：立即压缩 + 备份导出/导入（放在最上面：这两件事常要用）
    {
        const Value& mm = S.st.get("memory");
        const std::string cmp = mm.get("compressed").strOr("");
        const std::string outDefault = mm.get("backup_default").strOr("");
        Section(p, "sec.memops", "维护");
        Card(p.ui, "memops.card", p.x, p.y, p.w, 42.0f + 52.0f + 46.0f + 16.0f);
        {
            ButtonFlow bf{ p.ui, p.x + 18.0f, p.x + 18.0f, p.y + 12.0f, p.x + p.w - 18.0f };
            bf.add("mm.cmp", "立即压缩记忆", [] {
                S.link.send("{\"cmd\":\"mem_compress\"}");
                Toast("正在压缩记忆（合并去重、旧事折进「更早」，几秒）…");
            }, false, 36.0f);
            Text(p.ui, "mm.cmpt", p.x + 18.0f + 210.0f, p.y + 22.0f, 420.0f, 20.0f,
                 cmp.empty() ? i18n::T("每天会自动压缩一次（她有 API Key 且醒着时）")
                             : i18n::TF("上次压缩：{}", { cmp }), 12.0f, kMuted);
        }
        {
            const float iy = p.y + 12.0f + 42.0f;
            Text(p.ui, "bk.l", p.x + 18.0f, iy + 9.0f, 92.0f, 22.0f, "备份文件", 15.0f, kText, 500);
            if (!outDefault.empty() && S.backupPath.get().empty()) S.backupPath.set(outDefault);
            components::input(p.ui, "bk.in").theme(Th()).position(p.x + 110.0f, iy).size(p.w - 460.0f, 40.0f)
                .placeholder(i18n::T("绝对路径，或用右边「导出…/导入…」直接选文件")).bind(S.backupPath).fontSize(13.0f).build();
            // 导出…：打开原生文件对话框（另存为）→ 选好就导；没有 zenity/kdialog 时回退输入框里的路径
            components::button(p.ui, "bk.pickout").theme(Th(), false).position(p.x + p.w - 340.0f, iy).size(100.0f, 40.0f)
                .text(i18n::T("导出…")).fontSize(13.5f)
                .onClick([outDefault] {
                    PickPathAsync(true, false, S.backupPath.get().empty() ? outDefault : S.backupPath.get(),
                                  i18n::T("桌宠：导出备份到"), [](int r, const std::string& picked) {
                        std::string path = S.backupPath.get();
                        if (r == 1)
                        {
                            path = picked;
                            S.backupPath.set(picked);
                        }
                        else if (r == -1)
                        {
                            Toast(i18n::T("打不开系统文件对话框：用输入框里的路径导出"));
                        }
                        else
                        {
                            return;   // 取消
                        }
                        if (!IsAbsPath(path)) { Toast(i18n::T("给绝对路径，或用「导出…」选文件")); return; }
                        S.link.send("{\"cmd\":\"export\",\"path\":" + JStr(path) + "}");
                        Toast(i18n::T("正在导出（记忆+性格+题库+笑话+配置）…"));
                    });
                }).build();
            // 导入…：打开原生对话框（打开）→ 选中后按钮变「确认导入？」，再点一次才真导入
            const bool arm = S.importArmed;
            components::button(p.ui, "bk.in2").theme(Th(), arm).position(p.x + p.w - 232.0f, iy).size(110.0f, 40.0f)
                .text(i18n::T(arm ? "确认导入？" : "导入…")).fontSize(13.5f)
                .onClick([&] {
                    const std::string cur = S.backupPath.get();
                    if (!S.importArmed)
                    {
                        PickPathAsync(false, false, cur, i18n::T("桌宠：从备份导入"), [cur](int r, const std::string& picked) {
                            if (r == 1)
                            {
                                S.backupPath.set(picked);
                            }
                            else if (r == -1)
                            {
                                if (cur.empty() || cur[0] != '/') { Toast(i18n::T("打不开系统文件对话框：请在输入框里填备份文件绝对路径")); return; }
                            }
                            else
                            {
                                return;   // 取消
                            }
                            S.importArmed = true;
                            Toast(i18n::T("再点一次「确认导入」会用备份覆盖当前记忆/性格/题库/笑话"));
                        });
                        return;
                    }
                    S.importArmed = false;
                    const std::string v = S.backupPath.get();
                    if (!IsAbsPath(v)) { Toast(i18n::T("给绝对路径，或用「导入…」选文件")); return; }
                    S.link.send("{\"cmd\":\"import\",\"path\":" + JStr(v) + "}");
                    Toast(i18n::T("正在导入…（会重载她的记忆与性格）"));
                }).build();
        }
        Text(p.ui, "bk.h", p.x + 18.0f, p.y + 12.0f + 42.0f + 52.0f, p.w - 36.0f, 34.0f,
             "导出：把记忆/性格/题库/笑话/配置打成一个 JSON（**不含模型文件和 API Key**）；导入：覆盖回来并立即重载。",
             12.0f, kMuted, 400, true);
        p.y += 156.0f + 12.0f;
    }
    const std::string text = S.st.get("memory").get("text").strOr("");
    if (text.empty())
    {
        Text(p.ui, "empty", p.x, p.y, p.w, 22.0f, "还没有记忆（需要 LLM agent 在跑，互动后会慢慢整理）。", 14.0f, kMuted);
        p.y += 40.0f;
        return;
    }
    const std::string at = S.st.get("memory").get("at").strOr("");
    if (!at.empty())
    {
        Text(p.ui, "at", p.x, p.y - 8.0f, p.w, 18.0f, i18n::TF("更新于 {}", { at }), 12.5f, kMuted);
        p.y += 16.0f;
    }
    // 记忆只有"## 小标题 / - 列表 / 普通行"三种：自己排（EUI 的 markdown 组件对中文按字断词，字距发散）
    struct Ln { int kind; std::string t; float h; };   // 0 标题 1 列表 2 正文
    std::vector<Ln> lines;
    const float tw = p.w - 40.0f;
    float total = 0.0f;
    size_t a = 0;
    while (a <= text.size())
    {
        size_t b = text.find('\n', a);
        if (b == std::string::npos) b = text.size();
        std::string ln = text.substr(a, b - a);
        a = b + 1;
        if (ln.empty()) continue;
        Ln L;
        if (ln.rfind("## ", 0) == 0) { L.kind = 0; L.t = ln.substr(3); L.h = 38.0f; }
        else if (ln.rfind("- ", 0) == 0) { L.kind = 1; L.t = ln.substr(2); L.h = TextH(L.t, tw - 18.0f, 14.5f, 22.0f) + 8.0f; }
        else { L.kind = 2; L.t = ln; L.h = TextH(ln, tw, 14.5f, 22.0f) + 8.0f; }
        total += L.h;
        lines.push_back(L);
    }
    Card(p.ui, "mem.card", p.x, p.y, p.w, total + 36.0f);
    float y = p.y + 18.0f;
    for (size_t i = 0; i < lines.size(); ++i)
    {
        const Ln& L = lines[i];
        const std::string id = "mem." + std::to_string(i);
        if (L.kind == 0)
        {
            Text(p.ui, id, p.x + 20.0f, y + 6.0f, tw, 26.0f, L.t, 17.0f, kAccent, 800);
        }
        else if (L.kind == 1)
        {
            p.ui.rect(id + ".dot").position(p.x + 24.0f, y + 9.0f).size(6.0f, 6.0f).color(kPink).radius(3.0f).build();
            Text(p.ui, id, p.x + 38.0f, y, tw - 18.0f, L.h - 8.0f, L.t, 14.5f, kText, 400, true);
        }
        else
        {
            Text(p.ui, id, p.x + 20.0f, y, tw, L.h - 8.0f, L.t, 14.5f, kMuted, 400, true);
        }
        y += L.h;
    }
    p.y += total + 56.0f;
}

/** 外观页：主题（日间/夜间/跟随时间）+ 语言（中文/English/跟随系统） */
void PageAppearance(Pen& p)
{
    PageTitle(p, "外观", "界面的配色与语言（只影响面板自己；她说的话由 LLM 按语言生成）");

    const float segH = 40.0f;
    Section(p, "sec.theme", "主题");
    Card(p.ui, "theme.card", p.x, p.y, p.w, 130.0f);
    Text(p.ui, "theme.l", p.x + 18.0f, p.y + 16.0f, 200.0f, 22.0f, "主题", 15.0f, kText, 500);
    {
        const float segW = std::min(430.0f, p.w - 36.0f);
        p.ui.stack("theme.seg.wrap").position(p.x + 18.0f, p.y + 46.0f).size(segW, segH).content([&] {
            components::segmented(p.ui, "theme.seg").theme(Th()).size(segW, segH).fontSize(14.0f)
                .items({ i18n::T("日间"), i18n::T("夜间"), i18n::T("跟随时间") })
                .selected(S.themeMode)
                .onChange([](int v) { SetThemeMode(v); })
                .build();
        }).build();
    }
    Text(p.ui, "theme.h", p.x + 18.0f, p.y + 98.0f, p.w - 36.0f, 20.0f,
         "白天浅色、19:00–7:00 深色；也可以固定其中一种", 12.5f, kMuted);
    p.y += 130.0f + 26.0f;

    Section(p, "sec.lang", "语言");
    Card(p.ui, "lang.card", p.x, p.y, p.w, 130.0f);
    Text(p.ui, "lang.l", p.x + 18.0f, p.y + 16.0f, 200.0f, 22.0f, "语言", 15.0f, kText, 500);
    {
        const float segW = std::min(430.0f, p.w - 36.0f);
        p.ui.stack("lang.seg.wrap").position(p.x + 18.0f, p.y + 46.0f).size(segW, segH).content([&] {
            components::segmented(p.ui, "lang.seg").theme(Th()).size(segW, segH).fontSize(14.0f)
                .items({ "中文", "English", "日本語", i18n::T("跟随系统") })
                .selected(S.langCode == "en" ? 1 : (S.langCode == "ja" ? 2 : (S.langCode == "auto" ? 3 : 0)))
                .onChange([](int v) {
                    SetLangCode(v == 1 ? "en" : (v == 2 ? "ja" : (v == 3 ? "auto" : "zh")));
                })
                .build();
        }).build();
    }
    Text(p.ui, "lang.h", p.x + 18.0f, p.y + 98.0f, p.w - 36.0f, 20.0f,
         "界面文字（导航/页面/菜单/托盘）立即切换", 12.5f, kMuted);
    p.y += 130.0f + 26.0f;

    Card(p.ui, "pref.card", p.x, p.y, p.w, 56.0f);
    Text(p.ui, "pref.h", p.x + 18.0f, p.y + 18.0f, p.w - 36.0f, 20.0f,
         "主题与语言存在桌宠配置里（config.json 的 str 段），随备份导出/导入", 12.5f, kMuted);
    p.y += 56.0f + 20.0f;
}


// External integrations share the existing loopback TCP JSON-lines control service.
int SavedExternalPort()
{
    return (int)S.st.get("config").get("ctl_port").numOr(23333);
}

void SaveExternalPort()
{
    const std::string text = S.externalPort.get();
    if (text.empty() || text.find_first_not_of("0123456789") != std::string::npos || text.size() > 5)
    {
        Toast("端口必须是 1024–65535 的整数");
        return;
    }
    const int port = std::atoi(text.c_str());
    if (port < 1024 || port > 65535)
    {
        Toast("端口必须是 1024–65535 的整数");
        return;
    }
    S.link.send("{\"cmd\":\"config\",\"set\":{\"ctl_port\":" + std::to_string(port) + "}}");
    Toast("端口设置已发送；重启桌宠后生效");
}

std::string ExternalScript()
{
    const std::string exe = plat::SelfExePath();
    const std::string folder = exe.substr(0, exe.find_last_of("/\\"));
    const std::string packaged = folder + "/tools/pet_bridge.py";
    if (plat::IsFile(packaged)) return packaged;
    return L2D_BRIDGE_SOURCE;
}

void ExternalParagraph(Pen& p, const std::string& id, const std::string& raw)
{
    const float height = TextH(raw, p.w - 36.0f, 14.0f, 23.0f) + 12.0f;
    Text(p.ui, id, p.x + 18.0f, p.y, p.w - 36.0f, height, raw, 14.0f, kMuted, 400, true);
    p.y += height + 8.0f;
}

void ExternalCode(Pen& p, const std::string& id, const std::string& code, const char* copyLabel = "复制配置")
{
    float height = 16.0f;
    size_t start = 0;
    do
    {
        const size_t end = code.find('\n', start);
        const std::string line = code.substr(start, end == std::string::npos ? end : end - start);
        height += TextH(line.empty() ? " " : line, p.w - 36.0f, 13.0f, 21.0f) + 3.0f;
        if (end == std::string::npos) break;
        start = end + 1;
    } while (start <= code.size());
    Card(p.ui, id + ".bg", p.x, p.y, p.w, height);
    Text(p.ui, id, p.x + 18.0f, p.y + 8.0f, p.w - 36.0f, height - 8.0f, code, 13.0f, kText, 400, true);
    p.y += height + 12.0f;
    ButtonFlow bf{ p.ui, p.x, p.x, p.y, p.x + p.w };
    bf.add(id + ".copy", copyLabel, [code] {
        core::window::setClipboardText(code);
        Toast("配置已复制");
    });
    p.y = bf.y + 58.0f;
}

void PageExternal(Pen& p)
{
    PageTitle(p, "外部接入", "连接编程助手，让工作消息变成温暖的陪伴");
    if (!S.externalInit)
    {
        S.externalPort.set(std::to_string(SavedExternalPort()));
        S.externalInit = true;
    }
    const int current = EnvI("L2DPET_PORT", 23333);
    const int saved = SavedExternalPort();

    Section(p, "ext.companion.sec", "工作消息陪伴");
    Card(p.ui, "ext.companion.card", p.x, p.y, p.w, 2 * ToggleH() + SliderH(p.w));
    ToggleRow(p, "ext.companion.enable", "陪伴模式", "用俏皮的本地文案回应任务状态，不额外调用模型", "companion");
    ToggleRow(p, "ext.companion.motion", "温和的小动作", "桌宠自行选择支持的动作；不会打断正在等待选择的气泡", "companion_motion");
    SliderRow(p, "ext.companion.interval", "进度提醒间隔", "companion_interval", 10.0f, 120.0f, 5.0f,
        [](float v) { return std::to_string((int)v) + "s"; });
    p.y += 20.0f;
    ExternalParagraph(p, "ext.companion.info", "开始、工作中、取得进展、等待确认、出错、结束和暂停，都有不同的陪伴表达。重复消息会去重，进度提醒会限频；结束只表示本轮回复已就绪。");
    ButtonFlow companionButtons{ p.ui, p.x + 18.0f, p.x + 18.0f, p.y, p.x + p.w - 18.0f };
    companionButtons.add("ext.companion.demo", "试试陪伴消息", [] {
        S.link.send("{\"cmd\":\"companion_event\",\"kind\":\"completed\",\"source\":\"Codex\",\"message\":" + JStr(i18n::T("陪伴模式已准备好")) + "}");
    });
    p.y = companionButtons.y + 58.0f;

    Section(p, "ext.port.sec", "本地接入端口");
    ExternalParagraph(p, "ext.port.info", "默认端口：23333；地址：127.0.0.1。Hook 和 MCP 都连接此端口；这是 TCP JSON-Lines 服务，不是 HTTP 地址。");
    ExternalParagraph(p, "ext.port.active", i18n::TF("当前端口：{} · 已保存端口：{}", {std::to_string(current), std::to_string(saved)}));
    components::input(p.ui, "ext.port.input").theme(Th()).position(p.x + 18.0f, p.y)
        .size(std::min(230.0f, p.w - 36.0f), 40.0f).bind(S.externalPort).fontSize(14.0f)
        .placeholder("23333").onEnter(SaveExternalPort).build();
    p.y += 52.0f;
    ButtonFlow ports{ p.ui, p.x + 18.0f, p.x + 18.0f, p.y, p.x + p.w - 18.0f };
    ports.add("ext.port.save", "保存端口", SaveExternalPort, true);
    ports.add("ext.port.reset", "恢复默认端口", [] { S.externalPort.set("23333"); SaveExternalPort(); });
    ports.add("ext.test", "测试气泡", [] {
        S.link.send("{\"cmd\":\"say\",\"text\":" + JStr(i18n::T("外部接入测试成功")) + "}");
        Toast("测试消息已发送，请查看桌宠气泡");
    });
    p.y = ports.y + 58.0f;
    ExternalParagraph(p, "ext.port.restart", "保存后需退出并重新启动桌宠。启动参数可覆盖设置；接入文件会记录实际端口，助手应读取该文件。端口被占用时请选择其他端口。");

    const Value& integration = S.st.get("integration");
    const std::string directory = integration.get("directory").strOr("");
    Section(p, "ext.discovery.sec", "接入说明文件");
    ExternalParagraph(p, "ext.discovery.info", "把接入文件路径交给编程助手。助手读取当前端口、模型能力和性格，自行生成适合自己的 Hook / MCP 配置，并登记到注册目录。");
    ExternalCode(p, "ext.discovery.path", integration.get("descriptor").strOr(""), "复制接入文件路径");
    ExternalParagraph(p, "ext.discovery.readme", directory + "/README.md");
    Section(p, "ext.clients.sec", "已注册的编程助手");
    const auto& clients = integration.get("clients").arr;
    if (clients.empty()) ExternalParagraph(p, "ext.clients.empty", "还没有助手注册。每个助手在 clients/<助手ID>/registration.json 登记，配置文件也可放在自己的目录里。");
    for (const auto& client : clients)
    {
        const std::string clientId = client.get("id").strOr("");
        const std::string rowId = "ext.client." + clientId;
        Card(p.ui, rowId + ".card", p.x, p.y, p.w, 60.0f);
        Text(p.ui, rowId + ".name", p.x + 18.0f, p.y + 8.0f, p.w - 110.0f, 22.0f, client.get("name").strOr(""), 15.0f, kText, 600);
        Text(p.ui, rowId + ".id", p.x + 18.0f, p.y + 32.0f, p.w - 110.0f, 20.0f, clientId, 12.0f, kMuted);
        p.ui.stack(rowId + ".wrap").position(p.x + p.w - 74.0f, p.y + 16.0f).size(56.0f, 28.0f).content([&, clientId, rowId] {
            components::toggleSwitch(p.ui, rowId + ".toggle").theme(Th()).size(56.0f, 28.0f).checked(client.get("enabled").isBool() && client.get("enabled").b)
                .onChange([clientId](bool enabled) {
                    S.link.send("{\"cmd\":\"integration_client\",\"action\":\"set_enabled\",\"client_id\":" + JStr(clientId)
                        + ",\"enabled\":" + (enabled ? "true" : "false") + "}");
                }).build();
        }).build();
        p.y += 72.0f;
    }
    ExternalParagraph(p, "ext.clients.root", directory + "/clients");
    ExternalParagraph(p, "ext.clients.help", "关闭某个助手后，它的 Hook 和 MCP 陪伴通知都会被桌宠拦截；更新注册文件不会重置你的通知选择。注册文件只声明身份，不会执行其中的命令。");

    // 最近的工作消息：放页尾（它是"记录"，不是设置项）。上限跟着桌宠的缓冲走——PetCompanion
    // 只留最近 60 条、只存于本次运行（不落盘），这里就全量显示，最新在最上面。
    const auto& companionEvents = S.st.get("companion_events").arr;
    if (!companionEvents.empty())
    {
        Section(p, "ext.companion.history", "最近的工作消息");
        for (int i = (int)companionEvents.size() - 1, n = 0; i >= 0 && n < 60; --i, ++n)
        {
            const Value& event = companionEvents[(size_t)i];
            const std::string id = "ext.companion.event." + std::to_string(n);
            ExternalParagraph(p, id + ".speech", event.get("speech").strOr(""));
            const std::string message = event.get("message").strOr("");
            if (!message.empty()) ExternalParagraph(p, id + ".message", message);
        }
        ExternalParagraph(p, "ext.companion.memory", "最新在最上面，最多保留 60 条，只存于本次运行（重启即清空）；详细错误和完整成果仍以编程助手的对话为准。");
    }
}


void PageAbout(Pen& p)
{
    PageTitle(p, "关于");
    // 只做"开发者 + 程序版本"：logo、作者、版本、主页，居中。
    // 端口/路径/操作提示这类说明不放这里（要查去看别的页/文档）。
    // 版本号来自仓库根 version.txt（编译期 -DL2D_VERSION 带进来，见 panel/CMakeLists.txt）。
#ifndef L2D_VERSION
#define L2D_VERSION "dev"
#endif
    const float logo = 148.0f;
    const float cardH = 24.0f + logo + 18.0f + 28.0f + 6.0f + 20.0f + 2.0f + 20.0f + 2.0f + 20.0f + 22.0f;
    Card(p.ui, "about.card", p.x, p.y, p.w, cardH);

    p.ui.image("about.logo")
        .position(p.x + (p.w - logo) * 0.5f, p.y + 24.0f)
        .size(logo, logo)
        .source("l2dpet-logo.png")
        .contain()
        .build();

    const float yText = p.y + 24.0f + logo + 18.0f;
    Text(p.ui, "about.author", p.x, yText, p.w, 28.0f, "第九个下弦月", 19.0f, kText, 600, false,
         core::HorizontalAlign::Center);
    Text(p.ui, "about.ver", p.x, yText + 34.0f, p.w, 20.0f, i18n::TF("开发者 · 版本 {}", { L2D_VERSION }), 13.0f, kMuted,
         400, false, core::HorizontalAlign::Center);
    // 个人主页（点一下用系统浏览器打开）
    const std::string home = "https://space.bilibili.com/34165842";
    p.ui.text("about.home")
        .position(p.x, yText + 56.0f)
        .size(p.w, 20.0f)
        .text(i18n::TF("视频网站个人主页 · {}", { home }))
        .fontSize(13.0f)
        .lineHeight(20.0f)
        .color(kAccent)
        .horizontalAlign(core::HorizontalAlign::Center)
        .onClick([home] { eui::platform::openUrl(home); })
        .build();
    // 开源协议：本项目（自己的代码）是 MIT；第三方组件各自许可（Live2D Core 是专有的），
    // 见仓库根 LICENSE。这里只显示一行，不做说明。
    Text(p.ui, "about.license", p.x, yText + 76.0f, p.w, 20.0f, "开源协议 · MIT", 13.0f, kMuted, 400, false,
         core::HorizontalAlign::Center);
    p.y += cardH + 20.0f;

    // 数据目录：一键用系统文件管理器打开（配置 / 模型库）。路径由运行中的桌宠上报
    //（panel_state.dirs），没连上时按同一套约定自己拼。
    Section(p, "about.dirs", "数据目录");
    {
        const std::string cfgDir = PetConfigDir();
        const std::string modelsDir = PetModelsDir();
        const float top = p.y + 2.0f;
        const float cardH2 = 16.0f + 40.0f + 8.0f + 20.0f + 20.0f + 10.0f;
        Card(p.ui, "about.dirs.card", p.x, top, p.w, cardH2);
        ButtonFlow dirs{ p.ui, p.x + 18.0f, p.x + 18.0f, top + 14.0f, p.x + p.w - 18.0f };
        dirs.add("about.dirs.cfg", "打开配置目录", [] { OpenFolder(PetConfigDir()); });
        dirs.add("about.dirs.models", "打开模型目录", [] { OpenFolder(PetModelsDir()); });
        Text(p.ui, "about.dirs.cfg.path", p.x + 18.0f, top + 60.0f, p.w - 36.0f, 20.0f,
             i18n::TF("配置 · {}", { cfgDir }), 12.0f, kMuted);
        Text(p.ui, "about.dirs.models.path", p.x + 18.0f, top + 80.0f, p.w - 36.0f, 20.0f,
             i18n::TF("模型库 · {}", { modelsDir }), 12.0f, kMuted);
        p.y = top + cardH2 + 18.0f;
    }

    // 开机自启：写当前用户的启动项（Windows = HKCU 的 Run 键；Linux = ~/.config/autostart 的 .desktop）。
    // 开关状态**以系统为准**（每次排版都读一次）：用户可能在任务管理器/文件里改过，别显示成另一种真相。
    Section(p, "about.boot", "启动");
    {
        const bool on = desktop::AutostartEnabled();
        const float top = p.y + 2.0f;
        Card(p.ui, "about.boot.card", p.x, top, p.w, 56.0f);
        Text(p.ui, "about.boot.l", p.x + 18.0f, top + 9.0f, p.w - 110.0f, 22.0f, "开机自启", 15.0f, kText, 500);
#if defined(_WIN32)
        const char* hint = "登录后自动把她放出来（当前用户级启动项，不需要管理员）";
#else
        const char* hint = "登录后自动把她放出来（写 ~/.config/autostart 的用户级自启项）";
#endif
        Text(p.ui, "about.boot.h", p.x + 18.0f, top + 31.0f, p.w - 110.0f, 20.0f, hint, 12.5f, kMuted);
        p.ui.stack("about.boot.sw.wrap").position(p.x + p.w - 74.0f, top + 14.0f).size(56.0f, 28.0f).content([&] {
            components::toggleSwitch(p.ui, "about.boot.sw").theme(Th()).size(56.0f, 28.0f).checked(on)
                .onChange([](bool v) {
                    const bool ok = desktop::SetAutostart(v);
                    Toast(ok ? i18n::T(v ? "已设为开机自启" : "已取消开机自启")
                             : i18n::T("改不了启动项（权限或系统限制）"));
                    app::requestUpdate();   // 下一帧重新读系统状态，开关跟着回到实际值
                }).build();
        }).build();
        p.y = top + 56.0f + 18.0f;
    }
}

void Refresh()
{
    bool conn = false;
    const std::string raw = S.link.snapshot(&conn);
    S.connected = conn;
    if (raw != S.raw)
    {
        S.raw = raw;
        Value v;
        if (jsonlite::parse(raw.c_str(), raw.size(), v, nullptr) && v.isObj() && v.get("ok").isBool() && v.get("ok").b)
        {
            S.st = v.get("state");
            // 桌宠要求切页（右键菜单/托盘点了"状态与设置"某项）
            const Value& rq = v.get("panel_req");
            const long seq = (long)rq.get("seq").numOr(-1);
            if (seq != S.lastReqSeq)
            {
                if (S.lastReqSeq != -1 || seq > 0) S.page = std::clamp((int)rq.get("page").numOr(0), 0, kPageMax);
                S.lastReqSeq = seq;
            }
        }
    }
}

} // namespace


// ================================================================ 常驻：事件 / 菜单 / 气泡 / 托盘
namespace {

// ---- 右键菜单
struct MItem { int kind; std::string label; std::string cmd; int sub = 0; bool checked = false; };
enum { MI_ACTION, MI_TOGGLE, MI_SUB, MI_BACK, MI_SEP, MI_HEADER, MI_QUIT, MI_PREFS, MI_HIDE, MI_PREF };
const float kMenuW = 240.0f, kRowH = 34.0f, kSepH = 9.0f, kMenuPad = 8.0f;

std::vector<MItem> MenuItems()
{
    std::vector<MItem> v;
    const Value& mood = S.st.get("mood");
    const std::string st = mood.get("state").strOr("awake");
    auto add = [&](int k, const std::string& l, const std::string& c = "", int sub = 0, bool chk = false) {
        v.push_back(MItem{ k, l, c, sub, chk });
    };
    add(MI_HEADER, PetDisplayName() + " · " + StateZh(st) + " · "
                   + EmotionZh(mood.get("emotion").get("name").strOr("neutral")));
    add(MI_PREFS, "状态与设置…", "", P_STATUS);
    add(MI_SEP, "");
    if (st == "asleep" || st == "drowsy")
        add(MI_ACTION, "叫醒她", "{\"cmd\":\"mood\",\"state\":\"awake\"}");
    add(MI_ACTION, "摸摸头", "{\"cmd\":\"touch\",\"zone\":\"head\",\"kind\":\"rub\"}");
    add(MI_ACTION, "牵手", "{\"cmd\":\"touch\",\"zone\":\"hand.r\",\"kind\":\"click\"}");
    add(MI_ACTION, "逗她开心", "{\"cmd\":\"emotion\",\"name\":\"happy\",\"intensity\":0.8}");
    add(MI_ACTION, "捏捏脸", "{\"cmd\":\"touch\",\"zone\":\"face\",\"kind\":\"click\"}");
    if (st == "sulk") add(MI_ACTION, "哄哄她", "{\"cmd\":\"touch\",\"zone\":\"head\",\"kind\":\"rub\"}");
    if (st != "asleep" && st != "drowsy")
        add(MI_ACTION, "哄她睡觉", "{\"cmd\":\"mood\",\"state\":\"drowsy\"}");
    add(MI_SEP, "");
    add(MI_ACTION, "讲个笑话", "{\"cmd\":\"joke\"}");
    add(MI_ACTION, "猜拳！", "{\"cmd\":\"game\",\"kind\":\"rps\"}");
    if (Cfg("weather", 0) > 0.5f) add(MI_ACTION, "聊聊天气", "{\"cmd\":\"weather\"}");
    add(MI_SEP, "");
    add(MI_TOGGLE, "锁定大小和位置", "lock", 0, Cfg("lock", 0) > 0.5f);
    add(MI_ACTION, "拉回屏幕内", "{\"cmd\":\"window\",\"on_screen\":true}");   // 拖到屏幕外找不着时用
    add(MI_TOGGLE, "总是在前面", "top", 0, Cfg("top", 1) > 0.5f);
    add(MI_TOGGLE, "鼠标穿透", "passthrough", 0, Cfg("passthrough", 0) > 0.5f);
    add(MI_HIDE, "隐藏到托盘");
    add(MI_QUIT, "退出");
    return v;
}

float MenuHeight(const std::vector<MItem>& v)
{
    float h = kMenuPad * 2.0f;
    for (const MItem& it : v) h += it.kind == MI_SEP ? kSepH : kRowH;
    return h;
}

void ComposeMenu(eui::Ui& ui, const eui::Screen& screen)
{
    Refresh();
    ApplyPrefs();
    ApplyTheme();
    const std::vector<MItem> items = MenuItems();
    const float w = screen.width, h = screen.height;
    ui.rect("m.bg").position(0, 0).size(w, h).color(kMenuBg).radius(12.0f).border(1.0f, kMenuBorder).build();
    float y = kMenuPad;
    for (size_t i = 0; i < items.size(); ++i)
    {
        const MItem& it = items[i];
        const std::string id = "m." + std::to_string(i);
        if (it.kind == MI_SEP)
        {
            ui.rect(id).position(14.0f, y + kSepH / 2.0f).size(w - 28.0f, 1.0f).color(kBorder).build();
            y += kSepH;
            continue;
        }
        const bool hot = (int)i == S.menuHover && it.kind != MI_HEADER;
        if (hot)
            ui.rect(id + ".hl").position(6.0f, y + 1.0f).size(w - 12.0f, kRowH - 2.0f)
                .color(it.kind == MI_QUIT ? kHoverDanger : kHover).radius(7.0f).build();
        if (it.checked) Text(ui, id + ".ck", 12.0f, y + 7.0f, 20.0f, 20.0f, "✓", 14.0f, kAccent, 700);
        if (it.kind == MI_SUB) Text(ui, id + ".ar", w - 28.0f, y + 6.0f, 16.0f, 20.0f, "›", 16.0f, kMuted);
        if (it.kind == MI_BACK) Text(ui, id + ".bk", 12.0f, y + 6.0f, 16.0f, 20.0f, "‹", 16.0f, kMuted);
        const eui::Color col = it.kind == MI_HEADER ? kMuted
                             : (it.kind == MI_QUIT && hot ? kDanger : (hot ? kAccent : kText));
        Text(ui, id + ".t", 34.0f, y + (it.kind == MI_HEADER ? 9.0f : 7.0f), w - 60.0f, 20.0f, it.label,
             it.kind == MI_HEADER ? 12.5f : 14.5f, col, it.kind == MI_HEADER ? 400 : 500);
        if (it.kind != MI_HEADER)
        {
            const int idx = (int)i;
            const MItem copy = it;
            components::mouseArea(ui, id + ".hit").position(4.0f, y).size(w - 8.0f, kRowH)
                .onEnter([idx] { S.menuHover = idx; app::requestUpdate(); })
                .onLeave([idx] { if (S.menuHover == idx) { S.menuHover = -1; app::requestUpdate(); } })
                .onTap([copy] {
                    switch (copy.kind)
                    {
                    case MI_ACTION: S.link.send(copy.cmd); app::closeWindow("menu"); break;
                    case MI_PREF:
                        if (copy.cmd.rfind("theme:", 0) == 0)
                            SetThemeMode(copy.cmd == "theme:dark" ? THEME_DARK
                                                                  : (copy.cmd == "theme:auto" ? THEME_AUTO : THEME_LIGHT));
                        else if (copy.cmd.rfind("lang:", 0) == 0) SetLangCode(copy.cmd.substr(5));
                        app::closeWindow("menu");
                        break;
                    case MI_TOGGLE: SetCfg(copy.cmd, copy.checked ? 0.0f : 1.0f);
                        app::closeWindow("menu"); break;
                    case MI_SUB: case MI_BACK: {
                        S.menuLevel = copy.kind == MI_BACK ? 0 : copy.sub;
                        S.menuHover = -1;
                        const float lh = MenuHeight(MenuItems());
                        app::resizeWindow("menu", (int)std::lround(kMenuW * S.menuScale), (int)std::lround(lh * S.menuScale));
                        break;
                    }
                    case MI_PREFS: S.page = copy.sub; app::showMainWindow(); app::closeWindow("menu"); break;
                    case MI_HIDE: S.link.send("{\"cmd\":\"window\",\"visible\":false}"); app::closeWindow("menu"); break;
                    case MI_QUIT: S.link.send("{\"cmd\":\"quit\"}"); app::closeWindow("menu"); break;
                    default: break;
                    }
                    app::requestUpdate();
                })
                .build();
        }
        y += kRowH;
    }
}

void OpenMenu(int x, int y, float scale)
{
    S.menuOpenedAt = Clock::now();
    S.menuLevel = 0;
    S.menuHover = -1;
    S.menuScale = scale;
    const int W = (int)std::lround(kMenuW * scale), H = (int)std::lround(MenuHeight(MenuItems()) * scale);
    int px = x + 2, py = y + 2;
    if (px + W > S.wa[0] + S.wa[2]) px = x - W - 2;          // 右边放不下 → 往左弹
    if (py + H > S.wa[1] + S.wa[3]) py = S.wa[1] + S.wa[3] - H; // 下边放不下 → 往上收
    px = std::max(S.wa[0], px);
    py = std::max(S.wa[1], py);
    S.menuX = px;
    S.menuY = py;
    S.menuW = W;
    S.menuH = H;
    if (app::isWindowOpen("menu")) app::closeWindow("menu");
    if (std::getenv("L2DPET_UI_DEBUG")) std::fprintf(stderr, "[ui] 打开菜单 %dx%d @(%d,%d)\n", W, H, px, py);
    app::openWindow(DslWindowConfig{}
                        .title(i18n::T("桌宠菜单")).pageId("menu").windowSize(W, H).position(px, py)
                        .decorated(false).alwaysOnTop(true).transparent(true).resizable(false)
                        .windowType(1).focusOnShow(true).clearColor({ 0, 0, 0, 0 }).userTime(S.menuTime)
                        .onFocusChange([](bool f) {
                            if (std::getenv("L2DPET_UI_DEBUG")) std::fprintf(stderr, "[ui] 菜单焦点 %d\n", (int)f);
                            // 点别处 = 关；刚弹出 0.3s 内的失焦不算（WM 映射时的焦点抖动）
                            if (!f && Clock::now() - S.menuOpenedAt > std::chrono::milliseconds(300)) app::closeWindow("menu");
                        })
                        .onKeyEvent([](const eui::KeyEvent&) { app::closeWindow("menu"); }),
                    ComposeMenu);
}

// ---- 气泡已回到桌宠进程直绘（旧版设计：椭圆 + 双圆点；见 PortX11Pet.cpp）。
//      本进程不再处理 ui_bubble / ui_bubble_anchor / ui_bubble_hide。

/** 事件里的错误消息（没带 msg 时给"未知错误"） */
std::string EvMsg(const Value& e)
{
    const std::string m = e.get("msg").strOr("");
    return m.empty() ? i18n::T("未知错误") : m;
}

void ReadWa(const Value& e)
{
    const Value& wa = e.get("wa");
    if (wa.isArr() && wa.arr.size() >= 4)
        for (int i = 0; i < 4; ++i) S.wa[i] = (int)wa.arr[(size_t)i].numOr(S.wa[i]);
}

void HandleEvent(const std::string& line)
{
    static const bool dbg = std::getenv("L2DPET_UI_DEBUG") != nullptr;
    if (dbg) std::fprintf(stderr, "[ui] 事件 %s\n", line.c_str());
    Value e;
    if (!jsonlite::parse(line.c_str(), line.size(), e, nullptr) || !e.isObj()) return;
    const std::string ev = e.get("event").strOr("");
    if (ev == "ui_menu")
    {
        ReadWa(e);
        S.menuTime = (unsigned long)e.get("time").numOr(0);
        OpenMenu((int)e.get("x").numOr(0), (int)e.get("y").numOr(0), (float)e.get("scale").numOr(EnvF("L2DPET_UI_SCALE", 1.0f)));
    }
    else if (ev == "ui_close_menu")
    {
        app::closeWindow("menu");
    }
    else if (ev == "ui_close")   // panel page:-1：收起设置窗口（顺带收起菜单）
    {
        app::hideMainWindow();
        app::closeWindow("menu");
    }
    else if (ev == "ui_open")
    {
        S.page = std::clamp((int)e.get("page").numOr(0), 0, kPageMax);
        app::showMainWindow();
    }
    else if (ev == "model_import")   // 模型页：导入结果
    {
        const bool ok = e.get("ok").isBool() && e.get("ok").b;
        Toast(ok ? i18n::TF("已导入「{}」，正在切换识别…", { e.get("name").strOr("") })
                 : i18n::TF("导入失败：{}", { EvMsg(e) }));
    }
    else if (ev == "model_delete")   // 模型页：删除结果
    {
        const bool ok = e.get("ok").isBool() && e.get("ok").b;
        S.modelDelName.clear();
        Toast(ok ? i18n::TF("已删除「{}」", { e.get("name").strOr("") })
                 : i18n::TF("删除失败：{}", { EvMsg(e) }));
    }
    else if (ev == "probe")          // 模型页：能力探测开始/完成
    {
        if (e.get("started").isBool() && e.get("started").b) Toast(i18n::T("能力探测已开始（约 30 秒，完成自动重载）…"));
        else if (e.get("done").isBool() && e.get("done").b)
            Toast(e.get("saved").isBool() && e.get("saved").b ? i18n::T("能力探测完成，已识别并重载模型")
                                                              : i18n::T("探测完成（缓存写盘失败，仅本次有效）"));
    }
    else if (ev == "mem_export")     // 记忆页：导出结果
    {
        const bool ok = e.get("ok").isBool() && e.get("ok").b;
        Toast(ok ? i18n::TF("已导出 {} 个文件 → {}", { std::to_string((int)e.get("files").numOr(0)),
                                                       std::string(e.get("path").strOr("")) })
                 : i18n::TF("导出失败：{}", { EvMsg(e) }));
    }
    else if (ev == "mem_import")     // 记忆页：导入结果
    {
        const bool ok = e.get("ok").isBool() && e.get("ok").b;
        S.importArmed = false;
        Toast(ok ? i18n::TF("已导入 {} 个文件，正在重载她的记忆…", { std::to_string((int)e.get("files").numOr(0)) })
                 : i18n::TF("导入失败：{}", { EvMsg(e) }));
    }
    else if (ev == "mem_compress_done")   // 记忆页：压缩结果
    {
        const bool ok = e.get("ok").isBool() && e.get("ok").b;
        const std::string cmsg = e.get("msg").strOr("");
        Toast(ok ? i18n::TF("记忆已压缩：{}", { std::string(e.get("report").strOr("")) })
                 : i18n::TF("压缩失败：{}", { cmsg.empty() ? i18n::T("（她睡着或没配 Key 时不压）") : cmsg }));
    }
    else if (ev == "persona")        // 模型页：性格生成结果（内置 agent 上报）
    {
        const bool ok = e.get("ok").isBool() && e.get("ok").b;
        // 注意：strOr 返回 const char*，跟字面量 == 比的是指针（恒假）——必须转 std::string
        const std::string by = e.get("by").strOr("");
        Toast(ok ? i18n::TF("已为「{}」准备好性格（{}）",
                            { e.get("name").strOr(""), by == "llm" ? i18n::T("LLM 按能力解算") : i18n::T("本地兜底") })
                 : i18n::TF("性格生成失败：{}", { EvMsg(e) }));
    }
    // ui_bubble / ui_bubble_anchor / ui_bubble_hide：气泡已回到桌宠进程直绘，本进程忽略
}

// ---- 托盘（EUI 托盘 + 补丁的自定义菜单）
enum { T_VIS = 1, T_PREFS = 2, T_TOP = 5, T_PASS = 6, T_LOCK = 7, T_ONSCREEN = 8, T_QUIT = 9, T_THEME0 = 20, T_LANG0 = 30, T_MODEL0 = 100 };

void UpdateTray()
{
    const bool visible = !(S.st.get("visible").isBool() && !S.st.get("visible").b);
    const int cur = (int)S.st.get("model_index").numOr(0);
    std::string sig = std::string(visible ? "v" : "h") + (Cfg("top", 1) > 0.5f ? "t" : "-") + (Cfg("passthrough", 0) > 0.5f ? "p" : "-")
                      + (Cfg("lock", 0) > 0.5f ? "l" : "-")
                      + std::to_string(cur) + (S.connected ? "c" : "n");
    for (const Value& m : S.st.get("models").arr) sig += "|" + m.str;
    if (sig == S.traySig) return;
    S.traySig = sig;
    std::vector<eui::platform::TrayMenuItem> items;
    auto item = [](int id, const std::string& l, bool chk = false, bool en = true) {
        eui::platform::TrayMenuItem it;
        it.id = id; it.label = l; it.checked = chk; it.enabled = en;
        return it;
    };
    eui::platform::TrayMenuItem sep;
    sep.separator = true;
    items.push_back(item(T_VIS, i18n::T(visible ? "隐藏桌宠" : "显示桌宠"), false, S.connected));
    items.push_back(item(T_PREFS, i18n::T("状态与设置…")));
    const Value& ms = S.st.get("models");
    if (ms.arr.size() > 1)
    {
        eui::platform::TrayMenuItem models = item(0, i18n::T("模型"));
        for (size_t i = 0; i < ms.arr.size(); ++i)
        {
            eui::platform::TrayMenuItem m = item(T_MODEL0 + (int)i, ms.arr[i].str, (int)i == cur);
            m.radio = true;
            models.children.push_back(m);
        }
        items.push_back(models);
    }
    items.push_back(sep);
    // 外观 / 语言：托盘里也能切（改了就把 sig 清掉，下次 Tick 重推菜单）
    {
        eui::platform::TrayMenuItem app = item(0, i18n::T("外观"));
        app.children.push_back(item(T_THEME0, i18n::T("日间"), S.themeMode == THEME_LIGHT, S.connected));
        app.children.push_back(item(T_THEME0 + 1, i18n::T("夜间"), S.themeMode == THEME_DARK, S.connected));
        app.children.push_back(item(T_THEME0 + 2, i18n::T("跟随时间"), S.themeMode == THEME_AUTO, S.connected));
        eui::platform::TrayMenuItem sep2;
        sep2.separator = true;
        app.children.push_back(sep2);
        app.children.push_back(item(T_LANG0, "中文", S.langCode == "zh", S.connected));
        app.children.push_back(item(T_LANG0 + 1, "English", S.langCode == "en", S.connected));
        app.children.push_back(item(T_LANG0 + 2, "日本語", S.langCode == "ja", S.connected));
        app.children.push_back(item(T_LANG0 + 3, i18n::T("跟随系统"), S.langCode == "auto", S.connected));
        for (size_t i = 0; i < app.children.size(); ++i)
            if (!app.children[i].separator) app.children[i].radio = true;   // 三选一：全是单选点，别让第一个成勾选
        items.push_back(app);
        eui::platform::TrayMenuItem sep1;
        sep1.separator = true;
        items.push_back(sep1);
    }
    items.push_back(item(T_TOP, i18n::T("总在最前"), Cfg("top", 1) > 0.5f, S.connected));
    items.push_back(item(T_PASS, i18n::T("鼠标穿透"), Cfg("passthrough", 0) > 0.5f, S.connected));
    items.push_back(item(T_LOCK, i18n::T("锁定大小和位置"), Cfg("lock", 0) > 0.5f, S.connected));
    for (size_t i = items.size() - 3; i < items.size(); ++i) items[i].checkable = true;
    items.push_back(item(T_ONSCREEN, i18n::T("拉回屏幕内"), false, S.connected));   // 拖到屏幕外找不着时用
    items.push_back(sep);
    items.push_back(item(T_QUIT, i18n::T("退出")));
    eui::platform::setTrayMenu(items);
}

void HandleTrayClicks()
{
    int id = 0;
    while (eui::platform::consumeTrayMenuClick(&id))
    {
        const bool visible = !(S.st.get("visible").isBool() && !S.st.get("visible").b);
        if (id == T_VIS) S.link.send(std::string("{\"cmd\":\"window\",\"visible\":") + (visible ? "false" : "true") + "}");
        else if (id == T_PREFS) { S.page = P_STATUS; app::showMainWindow(); }
        else if (id == T_TOP) SetCfg("top", Cfg("top", 1) > 0.5f ? 0.0f : 1.0f);
        else if (id == T_PASS) SetCfg("passthrough", Cfg("passthrough", 0) > 0.5f ? 0.0f : 1.0f);
        else if (id == T_LOCK) SetCfg("lock", Cfg("lock", 0) > 0.5f ? 0.0f : 1.0f);
        else if (id == T_ONSCREEN) S.link.send("{\"cmd\":\"window\",\"on_screen\":true}");
        else if (id == T_QUIT) { S.link.send("{\"cmd\":\"quit\"}"); }
        else if (id >= T_THEME0 && id < T_THEME0 + 3) SetThemeMode(id - T_THEME0);
        else if (id >= T_LANG0 && id < T_LANG0 + 4)
        {
            static const char* const kLang[] = { "zh", "en", "ja", "auto" };
            SetLangCode(kLang[id - T_LANG0]);
        }
        else if (id >= T_MODEL0) S.link.send("{\"cmd\":\"model\",\"index\":" + std::to_string(id - T_MODEL0) + "}");
        S.traySig.clear();
    }
}

void Tick()
{
    if (!S.started)
    {
        S.started = true;
        const int pg = EnvI("L2DPET_PAGE", -1);
        S.page = std::clamp(pg, 0, kPageMax);
        S.link.start(EnvI("L2DPET_PORT", 23333), [] { app::requestUpdate(); });
        if (pg >= 0) app::showMainWindow();
    }
    Refresh();
    ApplyPrefs();
    ApplyTheme();   // 主窗口可能一直没显示过：调色板必须在常驻 tick 里也刷
    SyncTitleBar(DarkNow());   // "跟随时间"到点/别处改了主题：标题栏也跟着翻（没变时不动）
#if defined(_WIN32)
    // 菜单开着时，点到菜单外的空白处应当收起。
    // Linux 靠弹窗失焦（上面 onFocusChange）判"点别处"；Windows 上前台锁让弹窗根本拿不到焦点
    // （glfwFocusWindow → SetForegroundWindow 失败），那条路永远不触发——表现就是
    // "必须点菜单项才关得掉"。这里自己轮询全局左键补上：按下沿落在菜单矩形外 → 收起。
    {
        static bool s_menuBtnDown = false;
        const bool down = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
        if (down && !s_menuBtnDown && app::isWindowOpen("menu") &&
            Clock::now() - S.menuOpenedAt > std::chrono::milliseconds(300))
        {
            POINT pt;
            GetCursorPos(&pt);
            const bool inside = pt.x >= S.menuX && pt.x < S.menuX + S.menuW &&
                                pt.y >= S.menuY && pt.y < S.menuY + S.menuH;
            if (!inside) app::closeWindow("menu");
        }
        s_menuBtnDown = down;
    }
#endif
    std::string line;
    while (S.link.popEvent(&line)) HandleEvent(line);
    PollPick();
    UpdateTray();
    HandleTrayClicks();
    // 桌宠没了（连上过、又断开 10 秒）→ 本进程也退出（正常情况下桌宠退出时会直接 SIGTERM 本进程）
    if (S.connected) { S.everConnected = true; S.lostAt = Clock::now(); }
    else if (S.everConnected && Clock::now() - S.lostAt > std::chrono::seconds(10)) std::exit(0);
}

} // namespace

void compose(eui::Ui& ui, const eui::Screen& screen)
{
    Refresh();
    ApplyPrefs();    // 配置 → 主题/语言（每帧对账，改了立刻生效）
    ApplyTheme();    // 调色板（"跟随时间"到点自动换）

    const bool compact = screen.width < 760.0f;
    const float navW = compact ? 84.0f : 210.0f;
    const float contentW = std::max(0.0f, screen.width - navW);
    const int navSel = S.page == P_LOG ? P_LLM : S.page;

    ui.row("root").size(screen.width, screen.height).content([&] {
        components::navbar(ui, "nav").theme(Th()).size(navW, screen.height).compact(compact)
            .brand(i18n::T("Live2D 桌宠"), 0xF004).subtitle(i18n::T(S.connected ? "已连接" : "未连接"))
            .selected(navSel)
            .items({ { "status", i18n::T("状态"), 0xF004, P_STATUS },
                     { "behavior", i18n::T("行为"), 0xF1DE, P_BEHAVIOR },
                     { "window", i18n::T("窗口"), 0xF2D0, P_WINDOW },
                     { "appearance", i18n::T("外观"), 0xF53F, P_APPEARANCE },
                     { "model", i18n::T("模型"), 0xF1B2, P_MODEL },
                     { "llm", i18n::T("对话"), 0xF086, P_LLM },
                     { "memory", i18n::T("记忆"), 0xF02D, P_MEMORY },
                      { "external", i18n::T("外部接入"), 0xF1E6, P_EXTERNAL },
                     { "about", i18n::T("关于"), 0xF05A, P_ABOUT } })
            .onChange([](int v) { S.page = v; })
            .build();

        ui.stack("host").size(contentW, screen.height).content([&] {
            ui.rect("host.bg").size(contentW, screen.height).color(Th().background).build();
            if (!S.connected || !S.st.isObj())
            {
                const float w = std::min(520.0f, contentW - 48.0f);
                Text(ui, "nc.t", 24.0f, 40.0f, w, 34.0f, S.connected ? "正在读取桌宠状态…" : "没有连上桌宠", 24.0f, kText, 800);
                const std::string m = i18n::TF("请先启动桌宠（l2dtest --pet --ctl {}）。连上后这里会自动刷新；每秒重试一次。",
                                               { std::to_string(EnvI("L2DPET_PORT", 23333)) });
                Text(ui, "nc.m", 24.0f, 84.0f, w, TextH(m, w, 14.0f, 20.0f), m, 14.0f, kMuted, 400, true);
                return;
            }
            const int pg = S.page;
            const float pad = compact ? 18.0f : 30.0f;
            const float h = std::max(screen.height, S.pageH.count(pg) ? S.pageH[pg] : screen.height);
            components::scrollView(ui, "page.scroll." + std::to_string(pg)).theme(Th()).size(contentW, screen.height)
                .bind(S.scroll[pg]).step(56.0f).scrollbarWidth(8.0f).scrollbarGap(6.0f)
                .content([&](eui::Ui& cu, float cw, float) {
                    cu.stack("page.canvas." + std::to_string(pg)).size(cw, h).content([&] {
                        // 内容最宽 1200（大屏/最大化时不至于一行字拉满屏），多出来的宽度两边均分 = 居中
                        const float maxW = 1200.0f, useW = std::min(maxW, cw - pad * 2.0f);
                        Pen p{ cu, pad + std::max(0.0f, (cw - pad * 2.0f - useW) / 2.0f), useW, 28.0f };
                        switch (pg)
                        {
                        case P_STATUS: PageStatus(p); break;
                        case P_BEHAVIOR: PageBehavior(p); break;
                        case P_WINDOW: PageWindow(p); break;
                        case P_MODEL: PageModel(p); break;
                        case P_LLM: PageLLM(p); break;
                        case P_MEMORY: PageMemory(p); break;
                        case P_LOG: PageLog(p); break;
                        case P_APPEARANCE: PageAppearance(p); break;
                        case P_EXTERNAL: PageExternal(p); break;
                        default: PageAbout(p); break;
                        }
                        S.pageH[pg] = p.y + 20.0f;   // 下一帧按这个高度给滚动区
                    }).build();
                })
                .build();
            // 提示条（3 秒）
            if (!S.toast.empty() && Clock::now() - S.toastAt < std::chrono::seconds(3))
            {
                const float tw = std::min(contentW - 40.0f, 44.0f + TextW(S.toast, 14.0f));
                ui.rect("toast.bg").position((contentW - tw) / 2.0f, screen.height - 70.0f).size(tw, 40.0f)
                    .color(kToastBg).radius(20.0f).build();
                Text(ui, "toast.t", (contentW - tw) / 2.0f, screen.height - 60.0f, tw, 22.0f, S.toast, 14.0f,
                     kToastText, 600, false, core::HorizontalAlign::Center);
            }
        }).build();
    }).build();
}

} // namespace app

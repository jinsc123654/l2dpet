#pragma once

#include "eui/dsl.h"
#include "eui/types.h"
#include "eui/window.h"

#include <functional>
#include <string>
#include <vector>

namespace app {

using DslWindowCompose = std::function<void(eui::Ui&, const eui::Screen&)>;

struct DslWindowRequest {
    std::string title = "Window";
    std::string pageId = "window";
    eui::Color clearColor = {0.16f, 0.18f, 0.20f, 1.0f};
    int width = 640;
    int height = 420;
    bool modal = false;
    std::function<void(const eui::KeyEvent&)> onKeyEvent;
    DslWindowCompose compose;
    // l2dpet 补丁：弹出窗口 / 气泡
    int x = 0;
    int y = 0;
    bool positionSet = false;
    bool decorated = true;
    bool alwaysOnTop = false;
    bool transparent = false;
    bool focusOnShow = true;
    bool resizable = true;
    int windowType = 0;                        // 0 普通 1 弹出菜单 2 提示/气泡 3 工具窗
    unsigned long userTime = 0;                // 触发它的用户操作时间戳（X11 防抢焦点）
    std::function<void(bool)> onFocusChange;   // 失焦 = false（弹出菜单据此自动关闭）
};

// l2dpet 补丁：运行中控制窗口（主线程队列，由后端主循环执行）
struct DslWindowControl {
    enum Kind { Close, Move, Resize, ShowMain, HideMain } kind = Close;
    std::string pageId;
    int a = 0;
    int b = 0;
};
void closeWindow(const std::string& pageId);
void moveWindow(const std::string& pageId, int x, int y);
void resizeWindow(const std::string& pageId, int width, int height);
void showMainWindow();
void hideMainWindow();
bool isWindowOpen(const std::string& pageId);
std::vector<DslWindowControl> consumeWindowControls();
void markWindowOpen(const std::string& pageId, bool open);
void runTick();             // 每圈主循环（主窗口隐藏时也跑）调一次 DslAppConfig::onTick
double tickInterval();      // >0：主循环最多等这么久就醒（onTick / 托盘点击要及时处理）
bool startHidden();
bool hideOnClose();

const char* windowTitle();
bool showDebugStatsInTitle();
double debugTitleUpdateInterval();
bool showDebugOverlay();
double frameRateLimit();
int initialWindowWidth();
int initialWindowHeight();
int initialWindowX();
int initialWindowY();
bool initialWindowPositionSet();
int minimumWindowWidth();
int minimumWindowHeight();
int maximumWindowWidth();
int maximumWindowHeight();
bool windowResizable();
bool windowHighDpi();
bool windowDecorated();
bool windowAlwaysOnTop();
bool windowMaximized();
float uiScale();
bool trayEnabled();
const char* trayTitle();
const char* trayIconPath();
void requestUpdate();
bool initialize(eui::window::Handle window);
bool update(eui::window::Handle window, float deltaSeconds, int windowWidth, int windowHeight, float dpiScale, float pointerScale);
bool update(eui::window::Handle window, float deltaSeconds, int windowWidth, int windowHeight, float dpiScale, float pointerScale, bool updateRequested);
bool update(eui::window::Handle window, float deltaSeconds, int windowWidth, int windowHeight, float dpiScale, float pointerScale, bool updateRequested, bool inputEnabled);
bool isAnimating();
void render(int windowWidth, int windowHeight, float dpiScale);
void releaseGraphicsResources();
void shutdown();
std::vector<DslWindowRequest> consumeWindowRequests();

namespace detail {
void requestFullPaint();
}

} // namespace app

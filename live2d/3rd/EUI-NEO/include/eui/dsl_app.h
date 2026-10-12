#pragma once

#include "eui/app.h"
#include "eui/async.h"

#include <cmath>
#include <functional>
#include <string>
#include <utility>

namespace app {

struct DslAppConfig {
    std::string titleValue = "App";
    std::string pageIdValue = "app";
    eui::Color clearColorValue = {0.16f, 0.18f, 0.20f, 1.0f};
    int windowWidthValue = 800;
    int windowHeightValue = 600;
    int windowXValue = 0;
    int windowYValue = 0;
    bool windowPositionSetValue = false;
    int minWindowWidthValue = 0;
    int minWindowHeightValue = 0;
    int maxWindowWidthValue = 0;
    int maxWindowHeightValue = 0;
    bool resizableValue = true;
    bool highDpiValue = true;
    bool decoratedValue = true;
    bool alwaysOnTopValue = false;
    bool maximizedValue = false;
    float uiScaleValue = 1.0f;
#if defined(EUI_DEBUG_BUILD)
    bool showDebugStatsInTitleValue = true;
    bool showDebugOverlayValue = true;
#else
    bool showDebugStatsInTitleValue = false;
    bool showDebugOverlayValue = false;
#endif
    double debugTitleIntervalValue = 1.0;
    std::function<void(eui::Ui&, const eui::Screen&)> debugOverlayCompose;
    double fpsValue = 90.0;
    std::string iconPathValue = "assets/icon.png";
    std::string textFontFileValue;
    std::string iconFontFileValue;
    bool trayEnabledValue = false;
    std::string trayTitleValue;
    std::string trayIconPathValue;
    std::function<void(const eui::KeyEvent&)> keyEventHandler;
    std::function<void()> shutdownHandler;
    // l2dpet 补丁：常驻 UI 进程
    std::function<void()> tickHandler;
    double tickIntervalValue = 0.0;
    bool startHiddenValue = false;
    bool hideOnCloseValue = false;
    // l2dpet 补丁：主窗口就绪回调——应用拿原生句柄用（面板靠它设 Windows 的深色标题栏）
    std::function<void(eui::window::Handle)> windowReadyHandler;

    DslAppConfig& title(std::string value) { titleValue = std::move(value); return *this; }
    DslAppConfig& pageId(std::string value) { pageIdValue = std::move(value); return *this; }
    DslAppConfig& clearColor(const eui::Color& value) { clearColorValue = value; return *this; }
    DslAppConfig& background(const eui::Color& value) { return clearColor(value); }
    DslAppConfig& windowSize(int width, int height) {
        windowWidthValue = width;
        windowHeightValue = height;
        return *this;
    }
    DslAppConfig& windowWidth(int value) { windowWidthValue = value; return *this; }
    DslAppConfig& windowHeight(int value) { windowHeightValue = value; return *this; }
    DslAppConfig& windowPosition(int x, int y) {
        windowXValue = x;
        windowYValue = y;
        windowPositionSetValue = true;
        return *this;
    }
    DslAppConfig& centerWindow() {
        windowPositionSetValue = false;
        return *this;
    }
    DslAppConfig& minWindowSize(int width, int height) {
        minWindowWidthValue = width;
        minWindowHeightValue = height;
        return *this;
    }
    DslAppConfig& maxWindowSize(int width, int height) {
        maxWindowWidthValue = width;
        maxWindowHeightValue = height;
        return *this;
    }
    DslAppConfig& resizable(bool value = true) { resizableValue = value; return *this; }
    DslAppConfig& highDpi(bool value = true) { highDpiValue = value; return *this; }
    DslAppConfig& decorated(bool value = true) { decoratedValue = value; return *this; }
    DslAppConfig& alwaysOnTop(bool value = true) { alwaysOnTopValue = value; return *this; }
    DslAppConfig& maximized(bool value = true) { maximizedValue = value; return *this; }
    DslAppConfig& uiScale(float value) {
        uiScaleValue = value > 0.0f ? value : 1.0f;
        return *this;
    }
    DslAppConfig& showDebugStatsInTitle(bool value = true) {
        showDebugStatsInTitleValue = value;
        return *this;
    }
    DslAppConfig& debugTitleInterval(double value) {
        debugTitleIntervalValue = std::isfinite(value) && value > 0.0 ? value : 1.0;
        return *this;
    }
    DslAppConfig& showDebugOverlay(bool value = true) {
        showDebugOverlayValue = value;
        return *this;
    }
    DslAppConfig& onDebugOverlay(std::function<void(eui::Ui&, const eui::Screen&)> callback) {
        debugOverlayCompose = std::move(callback);
        return *this;
    }
    DslAppConfig& fps(double value) { fpsValue = value; return *this; }
    DslAppConfig& iconPath(std::string value) { iconPathValue = std::move(value); return *this; }
    DslAppConfig& textFont(std::string value) { textFontFileValue = std::move(value); return *this; }
    DslAppConfig& iconFont(std::string value) { iconFontFileValue = std::move(value); return *this; }
    DslAppConfig& fonts(std::string textFont, std::string iconFont = {}) {
        textFontFileValue = std::move(textFont);
        iconFontFileValue = std::move(iconFont);
        return *this;
    }
    DslAppConfig& tray(bool value = true) {
        trayEnabledValue = value;
        return *this;
    }
    DslAppConfig& trayTitle(std::string value) {
        trayTitleValue = std::move(value);
        return *this;
    }
    DslAppConfig& trayIcon(std::string value) {
        trayIconPathValue = std::move(value);
        return *this;
    }
    DslAppConfig& onKeyEvent(std::function<void(const eui::KeyEvent&)> handler) {
        keyEventHandler = std::move(handler);
        return *this;
    }
    /** @brief UI/渲染线程退出回调，在主窗口 GPU 设备销毁前释放应用资源引用。 */
    /** 每圈主循环调用（主窗口隐藏时也调）；interval 秒 = 主循环最长等待（及时处理外部事件/托盘点击） */
    DslAppConfig& onTick(std::function<void()> handler, double interval = 0.05) {
        tickHandler = std::move(handler);
        tickIntervalValue = interval;
        return *this;
    }
    /** 启动时主窗口隐藏（常驻进程：用 app::showMainWindow() 按需显示） */
    DslAppConfig& startHidden(bool value = true) { startHiddenValue = value; return *this; }
    /** 点关闭按钮 = 隐藏主窗口（不退出进程） */
    DslAppConfig& hideOnClose(bool value = true) { hideOnCloseValue = value; return *this; }
    /** l2dpet 补丁：主窗口建好后调一次（窗口隐藏时也会调）——拿原生句柄改窗口级属性用 */
    DslAppConfig& onWindowReady(std::function<void(eui::window::Handle)> handler) {
        windowReadyHandler = std::move(handler);
        return *this;
    }
    DslAppConfig& onShutdown(std::function<void()> handler) {
        shutdownHandler = std::move(handler);
        return *this;
    }
};

struct DslWindowConfig {
    std::string titleValue = "Window";
    std::string pageIdValue = "window";
    eui::Color clearColorValue = {0.16f, 0.18f, 0.20f, 1.0f};
    int windowWidthValue = 640;
    int windowHeightValue = 420;
    bool modalValue = false;
    std::function<void(const eui::KeyEvent&)> keyEventHandler;
    // l2dpet 补丁：弹出窗口 / 气泡
    int xValue = 0, yValue = 0;
    bool positionSetValue = false, decoratedValue = true, alwaysOnTopValue = false, transparentValue = false;
    bool focusOnShowValue = true, resizableValue = true;
    int windowTypeValue = 0;
    unsigned long userTimeValue = 0;
    std::function<void(bool)> focusHandler;
    /** 触发这个窗口的用户操作时间戳（X11 事件 time）：让 WM 的防抢焦点放行 */
    DslWindowConfig& userTime(unsigned long value) { userTimeValue = value; return *this; }
    DslWindowConfig& position(int x, int y) { xValue = x; yValue = y; positionSetValue = true; return *this; }
    DslWindowConfig& decorated(bool value = true) { decoratedValue = value; return *this; }
    DslWindowConfig& alwaysOnTop(bool value = true) { alwaysOnTopValue = value; return *this; }
    DslWindowConfig& transparent(bool value = true) { transparentValue = value; return *this; }
    DslWindowConfig& focusOnShow(bool value = true) { focusOnShowValue = value; return *this; }
    DslWindowConfig& resizable(bool value = true) { resizableValue = value; return *this; }
    /** 0 普通 1 弹出菜单 2 提示/气泡 3 工具窗（非 0 = 不进任务栏） */
    DslWindowConfig& windowType(int value) { windowTypeValue = value; return *this; }
    DslWindowConfig& onFocusChange(std::function<void(bool)> handler) { focusHandler = std::move(handler); return *this; }

    DslWindowConfig& title(std::string value) { titleValue = std::move(value); return *this; }
    DslWindowConfig& pageId(std::string value) { pageIdValue = std::move(value); return *this; }
    DslWindowConfig& clearColor(const eui::Color& value) { clearColorValue = value; return *this; }
    DslWindowConfig& background(const eui::Color& value) { return clearColor(value); }
    DslWindowConfig& windowSize(int width, int height) {
        windowWidthValue = width;
        windowHeightValue = height;
        return *this;
    }
    DslWindowConfig& windowWidth(int value) { windowWidthValue = value; return *this; }
    DslWindowConfig& windowHeight(int value) { windowHeightValue = value; return *this; }
    DslWindowConfig& modal(bool value = true) { modalValue = value; return *this; }
    DslWindowConfig& onKeyEvent(std::function<void(const eui::KeyEvent&)> handler) {
        keyEventHandler = std::move(handler);
        return *this;
    }
};

const DslAppConfig& dslAppConfig();
void compose(eui::Ui& ui, const eui::Screen& screen);

void openWindow(const DslWindowConfig& config, DslWindowCompose composeFn);
void openWindow(const char* title, int width, int height, DslWindowCompose composeFn);

} // namespace app

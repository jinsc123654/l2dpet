#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <mmsystem.h>
#define GLFW_EXPOSE_NATIVE_WIN32
#endif

#include <GLFW/glfw3.h>
#ifdef _WIN32
#include <GLFW/glfw3native.h>
#endif

#include "eui/app.h"
#include "eui/detail/dsl_app_impl.h"
#include "core/app/app_runner.h"
#include "core/app/dsl_window_manager.h"
#include "core/app/dsl_window_runtime.h"
#include "core/app/frame_pacing.h"
#include "core/app/main_window_runtime.h"
#include "core/input/input_state.h"
#include "core/platform/async.h"
#include "core/platform/platform.h"
#include "core/window/window_backend.h"
#include "core/render/render_backend.h"

#include <algorithm>
#include <chrono>
#include <functional>
#include <cstdio>
#include <memory>
#include <thread>
#include <vector>

struct WindowState : app::AppRunner {
    bool hideToTrayRequested = false;
    bool forceClose = false;
    bool iconified = false;
    GLFWwindow* modalChildWindow = nullptr;
    std::function<void(bool)> onFocus;   // l2dpet 补丁：子窗口失焦回调
};

struct ManagedWindow {
    GLFWwindow* window = nullptr;
    WindowState state;
    app::DslWindowRuntime content;
    std::unique_ptr<core::render::RenderBackend> renderBackend;
};

struct TimerResolutionGuard {
    TimerResolutionGuard() {
#ifdef _WIN32
        timeBeginPeriod(1);
#endif
    }

    ~TimerResolutionGuard() {
#ifdef _WIN32
        timeEndPeriod(1);
#endif
    }
};

float getDpiScale(GLFWwindow* window) {
    float scaleX = 1.0f;
    float scaleY = 1.0f;
    glfwGetWindowContentScale(window, &scaleX, &scaleY);
    return (scaleX + scaleY) * 0.5f;
}

float getPointerScale(GLFWwindow* window) {
    int windowWidth = 0;
    int windowHeight = 0;
    int framebufferWidth = 0;
    int framebufferHeight = 0;
    glfwGetWindowSize(window, &windowWidth, &windowHeight);
    glfwGetFramebufferSize(window, &framebufferWidth, &framebufferHeight);

    if (windowWidth <= 0 || windowHeight <= 0) {
        return 1.0f;
    }

    const float scaleX = static_cast<float>(framebufferWidth) / static_cast<float>(windowWidth);
    const float scaleY = static_cast<float>(framebufferHeight) / static_cast<float>(windowHeight);
    return (scaleX + scaleY) * 0.5f;
}

GLFWmonitor* getWindowMonitor(GLFWwindow* window) {
    if (GLFWmonitor* monitor = glfwGetWindowMonitor(window)) {
        return monitor;
    }

    int windowX = 0;
    int windowY = 0;
    int windowWidth = 0;
    int windowHeight = 0;
    glfwGetWindowPos(window, &windowX, &windowY);
    glfwGetWindowSize(window, &windowWidth, &windowHeight);

    int monitorCount = 0;
    GLFWmonitor** monitors = glfwGetMonitors(&monitorCount);
    GLFWmonitor* bestMonitor = glfwGetPrimaryMonitor();
    int bestArea = 0;

    for (int i = 0; i < monitorCount; ++i) {
        GLFWmonitor* monitor = monitors[i];
        int monitorX = 0;
        int monitorY = 0;
        glfwGetMonitorPos(monitor, &monitorX, &monitorY);
        const GLFWvidmode* mode = glfwGetVideoMode(monitor);
        if (!mode) {
            continue;
        }

        const int overlapLeft = std::max(windowX, monitorX);
        const int overlapTop = std::max(windowY, monitorY);
        const int overlapRight = std::min(windowX + windowWidth, monitorX + mode->width);
        const int overlapBottom = std::min(windowY + windowHeight, monitorY + mode->height);
        const int overlapWidth = std::max(0, overlapRight - overlapLeft);
        const int overlapHeight = std::max(0, overlapBottom - overlapTop);
        const int overlapArea = overlapWidth * overlapHeight;
        if (overlapArea > bestArea) {
            bestArea = overlapArea;
            bestMonitor = monitor;
        }
    }

    return bestMonitor;
}

double getWindowRefreshRate(GLFWwindow* window) {
#ifdef _WIN32
    HWND hwnd = glfwGetWin32Window(window);
    HMONITOR nativeMonitor = hwnd != nullptr ? MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST) : nullptr;
    if (nativeMonitor != nullptr) {
        MONITORINFOEXW monitorInfo{};
        monitorInfo.cbSize = sizeof(monitorInfo);
        if (GetMonitorInfoW(nativeMonitor, &monitorInfo)) {
            DEVMODEW mode{};
            mode.dmSize = sizeof(mode);
            if (EnumDisplaySettingsW(monitorInfo.szDevice, ENUM_CURRENT_SETTINGS, &mode) &&
                mode.dmDisplayFrequency > 1) {
                return static_cast<double>(mode.dmDisplayFrequency);
            }
        }
    }
#endif
    GLFWmonitor* monitor = getWindowMonitor(window);
    const GLFWvidmode* mode = monitor ? glfwGetVideoMode(monitor) : nullptr;
    if (mode && mode->refreshRate > 0) {
        return static_cast<double>(mode->refreshRate);
    }
    return 60.0;
}

void updateFrameInterval(GLFWwindow* window, WindowState& windowState, double now, bool force = false) {
    windowState.updateFrameInterval(getWindowRefreshRate(window), now, force);
}

void waitForNextFrame(GLFWwindow* window, const WindowState& windowState) {
    while (!glfwWindowShouldClose(window)) {
        const double remaining = windowState.nextFrameTime - glfwGetTime();
        if (remaining <= 0.0) {
            break;
        }

        app::detail::waitForFrameDuration(remaining);
    }
}

void hideWindowToTray(GLFWwindow* window, WindowState& windowState, core::render::RenderBackend& renderBackend) {
    if ((!windowState.trayAvailable && !app::hideOnClose() && !app::startHidden()) || windowState.hiddenToTray) {
        return;
    }

    core::render::ScopedRenderBackend scopedRenderBackend(renderBackend);
    app::releaseGraphicsResources();
    core::cancelInput(window);
    glfwHideWindow(window);
    windowState.hiddenToTray = true;
    windowState.hideToTrayRequested = false;
    windowState.paintRequested = false;
    windowState.renderedFrames = 0;
    windowState.nextFrameTime = glfwGetTime();
}

// l2dpet 补丁 0002：把主窗口抬到 Z 序最前。
// Windows 上 glfwShowWindow 走 SW_SHOWNA（只显示、不激活、不动 Z 序），
// glfwFocusWindow 的 SetForegroundWindow 又常被前台锁挡掉（本进程不是前台进程时直接失败）——
// 结果是窗口"显示"了却压在其他窗口后面，用户看起来像没打开。SetWindowPos(HWND_TOP) 不受前台锁限制。
void raiseWindowToFront(GLFWwindow* window) {
#if defined(_WIN32)
    HWND hwnd = glfwGetWin32Window(window);
    if (hwnd != nullptr) {
        SetWindowPos(hwnd, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    }
#else
    (void)window;
#endif
}

void restoreWindowFromTray(GLFWwindow* window, WindowState& windowState) {
    if (!windowState.hiddenToTray) {
        return;
    }

    glfwRestoreWindow(window);
    glfwShowWindow(window);
    raiseWindowToFront(window);
    glfwFocusWindow(window);
    windowState.hiddenToTray = false;
    windowState.hideToTrayRequested = false;
    windowState.paintRequested = true;
    app::detail::requestFullPaint();
    windowState.nextFrameTime = glfwGetTime();
}

void installWindowCallbacks(GLFWwindow* window, WindowState& windowState) {
    glfwSetWindowUserPointer(window, &windowState);
    glfwSetFramebufferSizeCallback(window, [](GLFWwindow* currentWindow, int w, int h) {
        static_cast<WindowState*>(glfwGetWindowUserPointer(currentWindow))->paintRequested = true;
        if (w > 0 && h > 0) {
            app::detail::requestFullPaint();
        }
    });
    glfwSetWindowRefreshCallback(window, [](GLFWwindow* currentWindow) {
        static_cast<WindowState*>(glfwGetWindowUserPointer(currentWindow))->paintRequested = true;
        app::detail::requestFullPaint();
    });
    glfwSetWindowContentScaleCallback(window, [](GLFWwindow* currentWindow, float, float) {
        static_cast<WindowState*>(glfwGetWindowUserPointer(currentWindow))->paintRequested = true;
        app::detail::requestFullPaint();
    });
    glfwSetWindowFocusCallback(window, [](GLFWwindow* currentWindow, int focused) {
        WindowState* state = static_cast<WindowState*>(glfwGetWindowUserPointer(currentWindow));
        if (!state) {
            return;
        }
        if (focused != GLFW_TRUE) {
            core::cancelInput(currentWindow);
        }
        state->paintRequested = true;
        if (focused && state->modalChildWindow != nullptr && !glfwWindowShouldClose(state->modalChildWindow)) {
            glfwFocusWindow(state->modalChildWindow);
        }
        if (state->onFocus) {
            state->onFocus(focused == GLFW_TRUE);   // l2dpet 补丁：弹出菜单失焦即关
        }
    });
    glfwSetWindowIconifyCallback(window, [](GLFWwindow* currentWindow, int iconified) {
        WindowState* state = static_cast<WindowState*>(glfwGetWindowUserPointer(currentWindow));
        if (!state) {
            return;
        }
        state->iconified = iconified == GLFW_TRUE;
        if (state->iconified) {
            core::cancelInput(currentWindow);
        } else {
            state->paintRequested = true;
        }
    });
}

std::unique_ptr<ManagedWindow> createManagedWindow(const app::DslWindowRequest& request,
                                                   GLFWwindow* parentWindow,
                                                   core::render::RenderBackend& shareBackend) {
    core::window::WindowCreateRequest windowRequest;
    windowRequest.width = request.width;
    windowRequest.height = request.height;
    windowRequest.title = request.title.c_str();
    windowRequest.parent = parentWindow;
    windowRequest.renderApi = core::render::windowRenderApi();
    windowRequest.x = request.x;
    windowRequest.y = request.y;
    windowRequest.positionSet = request.positionSet;
    windowRequest.decorated = request.decorated;
    windowRequest.alwaysOnTop = request.alwaysOnTop;
    windowRequest.transparent = request.transparent;
    windowRequest.focusOnShow = request.focusOnShow;
    windowRequest.resizable = request.resizable;
    windowRequest.windowType = request.windowType;
    windowRequest.userTime = request.userTime;
    GLFWwindow* childWindow = static_cast<GLFWwindow*>(core::window::createWindow(windowRequest));
    if (!childWindow) {
        return {};
    }

    auto managed = std::make_unique<ManagedWindow>();
    managed->window = childWindow;
    managed->renderBackend = core::render::createRenderBackend(childWindow, &shareBackend);
    if (!managed->renderBackend) {
        core::window::destroyWindow(childWindow);
        return {};
    }
    if (!managed->renderBackend->initialize()) {
        core::window::destroyWindow(childWindow);
        return {};
    }
    managed->state.lastTitleUpdate = glfwGetTime();
    managed->state.nextFrameTime = managed->state.lastTitleUpdate;
    installWindowCallbacks(childWindow, managed->state);

    if (!managed->content.initialize(childWindow, request)) {
        managed->renderBackend.reset();
        core::releaseInputQueue(childWindow);
        core::window::destroyWindow(childWindow);
        return {};
    }

    managed->state.paintRequested = true;
    managed->state.onFocus = request.onFocusChange;
    app::markWindowOpen(request.pageId, true);
    if (managed->content.request().modal) {
        glfwFocusWindow(childWindow);
    }
    return managed;
}

void destroyManagedWindow(std::unique_ptr<ManagedWindow>& managed) {
    if (!managed || managed->window == nullptr) {
        managed.reset();
        return;
    }

    GLFWwindow* windowToDestroy = managed->window;
    app::markWindowOpen(managed->content.request().pageId, false);
    if (managed->renderBackend) {
        managed->renderBackend->makeCurrent();
        managed->renderBackend->releaseRenderCache();
    }
    core::releaseInputQueue(windowToDestroy);
    if (managed->renderBackend) {
        core::render::ScopedRenderBackend scopedRenderBackend(*managed->renderBackend);
        managed->content.shutdown(false);
    } else {
        managed->content.shutdown(false);
    }
    managed->renderBackend.reset();
    core::window::destroyWindow(windowToDestroy);
    managed.reset();
}

bool updateManagedWindow(ManagedWindow& managed, float deltaSeconds, bool updateRequested) {
    if (managed.window == nullptr || glfwWindowShouldClose(managed.window)) {
        return false;
    }

    managed.renderBackend->makeCurrent();

    managed.state.iconified = glfwGetWindowAttrib(managed.window, GLFW_ICONIFIED) == GLFW_TRUE;
    if (managed.state.iconified) {
        managed.renderBackend->releaseRenderCache();
        managed.state.paintRequested = true;
        managed.content.requestFullPaint();
        managed.state.resetTiming(glfwGetTime());
        return true;
    }

    int framebufferWidth = 0;
    int framebufferHeight = 0;
    glfwGetFramebufferSize(managed.window, &framebufferWidth, &framebufferHeight);
    if (framebufferWidth <= 0 || framebufferHeight <= 0) {
        managed.renderBackend->releaseRenderCache();
        managed.state.paintRequested = true;
        managed.content.requestFullPaint();
        managed.state.resetTiming(glfwGetTime());
        return true;
    }

    const float dpiScale = getDpiScale(managed.window);
    const float pointerScale = getPointerScale(managed.window);
    const float logicalWidth = static_cast<float>(framebufferWidth) / dpiScale;
    const float logicalHeight = static_cast<float>(framebufferHeight) / dpiScale;

    core::render::ScopedRenderBackend scopedRenderBackend(*managed.renderBackend);
    if (managed.content.update(managed.window, deltaSeconds, logicalWidth, logicalHeight, pointerScale, dpiScale, updateRequested)) {
        managed.state.paintRequested = true;
    }

    if (managed.state.paintRequested || managed.content.paintRequested()) {
        managed.renderBackend->beginFrame({
            managed.window,
            core::window::nativeWindowInfo(managed.window),
            framebufferWidth,
            framebufferHeight,
            dpiScale
        });
        managed.content.render(*managed.renderBackend, framebufferWidth, framebufferHeight, dpiScale);
        managed.renderBackend->present();
        managed.state.paintRequested = false;
        ++managed.state.renderedFrames;
    }
    return true;
}

bool isManagedWindowClosed(const ManagedWindow& managed) {
    return managed.window == nullptr || glfwWindowShouldClose(managed.window);
}

bool isManagedWindowRenderable(const ManagedWindow& managed) {
    if (managed.window == nullptr || glfwWindowShouldClose(managed.window)) {
        return false;
    }
    if (managed.state.iconified || glfwGetWindowAttrib(managed.window, GLFW_ICONIFIED) == GLFW_TRUE) {
        return false;
    }

    int framebufferWidth = 0;
    int framebufferHeight = 0;
    glfwGetFramebufferSize(managed.window, &framebufferWidth, &framebufferHeight);
    return framebufferWidth > 0 && framebufferHeight > 0;
}

bool anyRenderableManagedWindowAnimating(const app::DslWindowManager<ManagedWindow>& windows) {
    return windows.anyAnimating(isManagedWindowRenderable);
}

void pruneClosedWindows(app::DslWindowManager<ManagedWindow>& windows) {
    windows.pruneClosed(isManagedWindowClosed, destroyManagedWindow);
}

void createRequestedWindows(app::DslWindowManager<ManagedWindow>& windows,
                            GLFWwindow* shareWindow,
                            core::render::RenderBackend& shareBackend,
                            const std::vector<app::DslWindowRequest>& requests) {
    windows.createPending(requests, [&](const app::DslWindowRequest& request) {
        return createManagedWindow(request, shareWindow, shareBackend);
    });
    shareBackend.makeCurrent();
}

GLFWwindow* findModalChildWindow(app::DslWindowManager<ManagedWindow>& windows) {
    ManagedWindow* managed = windows.modalWindow(isManagedWindowClosed);
    return managed != nullptr ? managed->window : nullptr;
}

// l2dpet 补丁：执行 app::closeWindow / moveWindow / resizeWindow / showMainWindow / hideMainWindow
void applyWindowControls(app::DslWindowManager<ManagedWindow>& windows, GLFWwindow* mainWindow,
                         WindowState& mainState, core::render::RenderBackend& mainBackend) {
    for (const app::DslWindowControl& c : app::consumeWindowControls()) {
        if (c.kind == app::DslWindowControl::ShowMain) {
            if (mainState.hiddenToTray) {
                restoreWindowFromTray(mainWindow, mainState);
            } else {
                glfwShowWindow(mainWindow);
                raiseWindowToFront(mainWindow);
                glfwFocusWindow(mainWindow);
            }
            continue;
        }
        if (c.kind == app::DslWindowControl::HideMain) {
            mainBackend.releaseRenderCache();
            hideWindowToTray(mainWindow, mainState, mainBackend);
            continue;
        }
        // l2dpet 补丁：同 pageId 可能短暂存在多个窗口（事件竞态：新气泡开出来、
        // 旧的还没销毁）。Close/Move/Resize 作用于**全部**匹配窗口——否则每次只关
        // 最老的一个，bubOpen 复位后没人再调 closeWindow，多出来的窗口永久泄漏。
        windows.updateAll([&](ManagedWindow& m) {
            if (m.window == nullptr || m.content.request().pageId != c.pageId) {
                return;
            }
            if (c.kind == app::DslWindowControl::Close) {
                glfwSetWindowShouldClose(m.window, GLFW_TRUE);
            } else if (c.kind == app::DslWindowControl::Move) {
                glfwSetWindowPos(m.window, c.a, c.b);
            } else if (c.kind == app::DslWindowControl::Resize) {
                glfwSetWindowSize(m.window, std::max(1, c.a), std::max(1, c.b));
                m.state.paintRequested = true;
            }
        });
    }
}

int eui_app_run() {
    core::platform::repairCurrentWorkingDirectory();
    core::render::initializeRenderBackendLoader();
    if (!glfwInit()) {
        return -1;
    }
    TimerResolutionGuard timerResolution;

    core::window::WindowCreateRequest windowRequest;
    windowRequest.width = app::initialWindowWidth();
    windowRequest.height = app::initialWindowHeight();
    windowRequest.x = app::initialWindowX();
    windowRequest.y = app::initialWindowY();
    windowRequest.positionSet = app::initialWindowPositionSet();
    windowRequest.minWidth = app::minimumWindowWidth();
    windowRequest.minHeight = app::minimumWindowHeight();
    windowRequest.maxWidth = app::maximumWindowWidth();
    windowRequest.maxHeight = app::maximumWindowHeight();
    windowRequest.resizable = app::windowResizable();
    windowRequest.highDpi = app::windowHighDpi();
    windowRequest.decorated = app::windowDecorated();
    windowRequest.alwaysOnTop = app::windowAlwaysOnTop();
    windowRequest.maximized = app::windowMaximized();
    windowRequest.title = app::windowTitle();
    windowRequest.renderApi = core::render::windowRenderApi();
    GLFWwindow* window = static_cast<GLFWwindow*>(core::window::createWindow(windowRequest));
    if (!window) {
        glfwTerminate();
        return -1;
    }

    WindowState windowState;
    windowState.resetTiming(glfwGetTime());
    updateFrameInterval(window, windowState, windowState.lastTitleUpdate, true);
    if (app::showDebugStatsInTitle()) {
        char title[128];
        std::snprintf(title, sizeof(title), "%s - 0 FPS", app::windowTitle());
        glfwSetWindowTitle(window, title);
    }
    installWindowCallbacks(window, windowState);

    const auto cleanupMainWindow = [&] {
        core::releaseInputQueue(window);
        core::window::destroyWindow(window);
        glfwTerminate();
    };

    auto renderBackend = core::render::createRenderBackend(window);
    if (!renderBackend) {
        cleanupMainWindow();
        return -1;
    }
    if (!renderBackend->initialize()) {
        cleanupMainWindow();
        return -1;
    }

    if (!app::initialize(window)) {
        app::shutdown();
        renderBackend.reset();
        cleanupMainWindow();
        return -1;
    }
    app::MainWindowRuntime mainWindowRuntime(windowState);
    windowState.initializeTray();
    glfwSetWindowCloseCallback(window, [](GLFWwindow* currentWindow) {
        WindowState* state = static_cast<WindowState*>(glfwGetWindowUserPointer(currentWindow));
        if (state && state->modalChildWindow != nullptr && !glfwWindowShouldClose(state->modalChildWindow)) {
            glfwFocusWindow(state->modalChildWindow);
            glfwSetWindowShouldClose(currentWindow, GLFW_FALSE);
            return;
        }
        if (state && (state->trayAvailable || app::hideOnClose()) && !state->forceClose) {
            state->hideToTrayRequested = true;
            glfwSetWindowShouldClose(currentWindow, GLFW_FALSE);
        }
    });
    glfwSetWindowIconifyCallback(window, [](GLFWwindow* currentWindow, int iconified) {
        WindowState* state = static_cast<WindowState*>(glfwGetWindowUserPointer(currentWindow));
        if (!state) {
            return;
        }
        state->iconified = iconified == GLFW_TRUE;
        if (iconified) {
            core::cancelInput(currentWindow);
        } else {
            state->paintRequested = true;
            app::detail::requestFullPaint();
        }
    });

    app::DslWindowManager<ManagedWindow> childWindows;
    if (app::startHidden()) {
        hideWindowToTray(window, windowState, *renderBackend);   // l2dpet 补丁：常驻进程启动即隐藏主窗口
    }
    double lastHiddenTick = glfwGetTime();

    while (!glfwWindowShouldClose(window)) {
        renderBackend->makeCurrent();
        app::runTick();                                            // l2dpet 补丁
        applyWindowControls(childWindows, window, windowState, *renderBackend);
        windowState.pollTray(false);
        if (windowState.consumeTrayExitRequested()) {
            windowState.forceClose = true;
            glfwSetWindowShouldClose(window, GLFW_TRUE);
            break;
        }
        if (windowState.consumeTrayShowRequested()) {
            restoreWindowFromTray(window, windowState);
        }
        pruneClosedWindows(childWindows);
        windowState.modalChildWindow = findModalChildWindow(childWindows);
        if (windowState.hideToTrayRequested && windowState.modalChildWindow != nullptr) {
            windowState.hideToTrayRequested = false;   // l2dpet 补丁：只有模态子窗口挡住隐藏（弹出菜单/气泡不挡）
        }
        if (windowState.hideToTrayRequested) {
            renderBackend->releaseRenderCache();
            hideWindowToTray(window, windowState, *renderBackend);
        }
        if (windowState.hiddenToTray) {
            // l2dpet 补丁：主窗口隐藏时子窗口（菜单/气泡）照常创建、更新、渲染
            const double now = glfwGetTime();
            const float dt = static_cast<float>(std::min(0.1, now - lastHiddenTick));
            lastHiddenTick = now;
            const bool upd = core::platform::consumeUiUpdate() || core::async::dispatchReady();
            createRequestedWindows(childWindows, window, *renderBackend, app::consumeWindowRequests());
            childWindows.updateAll([&](ManagedWindow& managed) { updateManagedWindow(managed, dt, upd); });
            pruneClosedWindows(childWindows);
            renderBackend->makeCurrent();
            const bool animating = anyRenderableManagedWindowAnimating(childWindows);
            const double tick = app::tickInterval();
            glfwWaitEventsTimeout(animating ? 1.0 / 60.0 : (tick > 0.0 ? tick : 0.10));
            windowState.resetTiming(glfwGetTime());
            continue;
        }
        lastHiddenTick = glfwGetTime();

        windowState.iconified = glfwGetWindowAttrib(window, GLFW_ICONIFIED) == GLFW_TRUE;
        if (windowState.iconified) {
            renderBackend->releaseRenderCache();
            windowState.paintRequested = false;
            windowState.consumeFrameRequest();
            windowState.resetTiming(glfwGetTime());
            glfwWaitEventsTimeout(0.25);
            continue;
        }

        if (windowState.anyAnimating(anyRenderableManagedWindowAnimating(childWindows))) {
            waitForNextFrame(window, windowState);
        }

        const double currentFrameTime = glfwGetTime();

        int framebufferWidth = 0;
        int framebufferHeight = 0;
        glfwGetFramebufferSize(window, &framebufferWidth, &framebufferHeight);
        if (framebufferWidth <= 0 || framebufferHeight <= 0) {
            renderBackend->releaseRenderCache();
            windowState.paintRequested = true;
            app::detail::requestFullPaint();
            windowState.consumeFrameRequest();
            glfwWaitEvents();
            mainWindowRuntime.markUnavailableFrame(glfwGetTime());
            continue;
        }

        const float dpiScale = getDpiScale(window);
        const float pointerScale = getPointerScale(window);
        const bool mainInputEnabled = windowState.modalChildWindow == nullptr;

        mainWindowRuntime.runFrame(
            window,
            *renderBackend,
            {framebufferWidth, framebufferHeight, dpiScale, pointerScale},
            currentFrameTime,
            getWindowRefreshRate(window),
            mainInputEnabled,
            [&] {
                createRequestedWindows(childWindows, window, *renderBackend, app::consumeWindowRequests());
                pruneClosedWindows(childWindows);
                windowState.modalChildWindow = findModalChildWindow(childWindows);
            },
            [&](float frameDelta, bool updateRequested) {
                childWindows.updateAll([&](ManagedWindow& managed) {
                    updateManagedWindow(managed, frameDelta, updateRequested);
                });

                createRequestedWindows(childWindows, window, *renderBackend, app::consumeWindowRequests());
                pruneClosedWindows(childWindows);
                windowState.modalChildWindow = findModalChildWindow(childWindows);
            },
            [&](const char* title) {
                glfwSetWindowTitle(window, title);
            },
            [&] {
                return anyRenderableManagedWindowAnimating(childWindows);
            });

        const bool anyAnimating = windowState.anyAnimating(anyRenderableManagedWindowAnimating(childWindows));
        if (anyAnimating) {
            glfwPollEvents();
        } else if (app::tickInterval() > 0.0) {
            glfwWaitEventsTimeout(app::tickInterval());   // l2dpet 补丁：onTick / 托盘点击要及时
        } else {
            glfwWaitEvents();
        }
    }

    childWindows.destroyAll(destroyManagedWindow);
    core::releaseInputQueue(window);
    renderBackend->makeCurrent();
    renderBackend->releaseRenderCache();
    core::platform::shutdownTray();
    {
        core::render::ScopedRenderBackend scopedRenderBackend(*renderBackend);
        app::shutdown();
    }
    renderBackend.reset();
    glfwTerminate();
    return 0;
}

#ifndef EUI_APP_RUNNER_LIBRARY
int main() {
    return eui_app_run();
}
#endif

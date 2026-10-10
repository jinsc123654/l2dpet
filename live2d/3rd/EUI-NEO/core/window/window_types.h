#pragma once

namespace core::window {

using Handle = void*;
using ContextKey = void*;
using CursorHandle = void*;

enum class CursorType {
    Arrow,
    Hand
};

enum class RenderApi {
    OpenGL,
    Vulkan
};

struct WindowCreateRequest {
    int width = 0;
    int height = 0;
    int x = 0;
    int y = 0;
    bool positionSet = false;
    int minWidth = 0;
    int minHeight = 0;
    int maxWidth = 0;
    int maxHeight = 0;
    const char* title = "";
    bool resizable = true;
    bool highDpi = true;
    bool decorated = true;
    bool alwaysOnTop = false;
    bool maximized = false;
    bool modal = false;
    // l2dpet 补丁：弹出窗口 / 气泡（菜单、提示）
    bool transparent = false;   // 透明帧缓冲（GLFW_TRANSPARENT_FRAMEBUFFER；clearColor 的 alpha 生效）
    bool focusOnShow = true;    // false = 显示时不抢焦点（气泡）
    int windowType = 0;         // 0 普通 1 弹出菜单 2 提示/气泡 3 工具窗（X11：_NET_WM_WINDOW_TYPE + 不进任务栏）
    unsigned long userTime = 0; // 触发它的用户操作时间戳（X11 _NET_WM_USER_TIME：过 WM 的防抢焦点）
    Handle parent = nullptr;
    RenderApi renderApi = RenderApi::OpenGL;
};

struct NativeWindowInfo {
    Handle handle = nullptr;
    void* platformWindow = nullptr;
    void* platformDisplay = nullptr;
    void* platformView = nullptr;
};

} // namespace core::window

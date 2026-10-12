#include "core/window/window_backend.h"
#include "core/platform/native_bridge.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>
#if defined(EUI_WINDOW_BACKEND_SDL2)

#include <SDL.h>
#if defined(__linux__) && !defined(__ANDROID__) && defined(SDL_VIDEO_DRIVER_X11)
#include <SDL_syswm.h>
#include <X11/Xresource.h>
#endif
#ifdef None
#undef None
#endif
#ifdef Bool
#undef Bool
#endif
#ifdef Status
#undef Status
#endif
#ifdef CursorShape
#undef CursorShape
#endif
#ifdef Success
#undef Success
#endif
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <imm.h>

#include <new>
#include <unordered_map>
#endif
#if defined(_WIN32) || defined(__APPLE__)
#include <SDL_syswm.h>
#endif
namespace core::window {

namespace {

void configureOpenGLWindowAttributes() {
    SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 16);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 0);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
}

#if defined(_WIN32)

struct SdlImeFilterState {
    WNDPROC previousProc = nullptr;
    SDL_Rect rect{};
    bool hasRect = false;
    bool applying = false;
    UINT_PTR reapplyTimer = 0;
};

std::unordered_map<HWND, SdlImeFilterState*> gSdlImeFilters;

SdlImeFilterState* sdlImeState(HWND hwnd) {
    const auto iterator = gSdlImeFilters.find(hwnd);
    return iterator != gSdlImeFilters.end() ? iterator->second : nullptr;
}

HWND hwndForSdlWindow(SDL_Window* window) {
    if (window == nullptr) {
        return nullptr;
    }

    SDL_SysWMinfo info{};
    SDL_VERSION(&info.version);
    if (SDL_GetWindowWMInfo(window, &info) != SDL_TRUE ||
        info.subsystem != SDL_SYSWM_WINDOWS) {
        return nullptr;
    }
    return info.info.win.window;
}

void applySdlImeRect(HWND hwnd, const SDL_Rect& rect) {
    HIMC context = ImmGetContext(hwnd);
    if (context == nullptr) {
        return;
    }

    LOGFONTW font{};
    const HFONT defaultFont = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
    if (defaultFont != nullptr &&
        GetObjectW(defaultFont, sizeof(font), &font) == sizeof(font)) {
        font.lfHeight = -std::max(12, rect.h);
        font.lfQuality = CLEARTYPE_QUALITY;
        ImmSetCompositionFontW(context, &font);
    }

    COMPOSITIONFORM composition{};
    composition.dwStyle = CFS_FORCE_POSITION;
    composition.ptCurrentPos.x = rect.x;
    composition.ptCurrentPos.y = rect.y;
    composition.rcArea.left = rect.x;
    composition.rcArea.top = rect.y;
    composition.rcArea.right = rect.x + rect.w;
    composition.rcArea.bottom = rect.y + rect.h;
    ImmSetCompositionWindow(context, &composition);

    CANDIDATEFORM candidate{};
    candidate.dwIndex = 0;
    candidate.dwStyle = CFS_EXCLUDE;
    candidate.ptCurrentPos = composition.ptCurrentPos;
    candidate.rcArea = composition.rcArea;
    ImmSetCandidateWindow(context, &candidate);

    ImmReleaseContext(hwnd, context);
}

void reapplySdlImeRect(HWND hwnd, SdlImeFilterState* state) {
    if (state == nullptr || !state->hasRect || state->applying) {
        return;
    }

    state->applying = true;
    applySdlImeRect(hwnd, state->rect);
    if (sdlImeState(hwnd) == state) {
        state->applying = false;
    }
}

LRESULT CALLBACK sdlImeWindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* state = sdlImeState(hwnd);
    if (message == WM_TIMER && state != nullptr &&
        state->reapplyTimer != 0 && wParam == state->reapplyTimer) {
        KillTimer(hwnd, state->reapplyTimer);
        state->reapplyTimer = 0;
        reapplySdlImeRect(hwnd, state);
        return 0;
    }

    const bool placementChanged = message == WM_IME_STARTCOMPOSITION ||
        message == WM_IME_COMPOSITION ||
        (message == WM_IME_NOTIFY &&
         (wParam == IMN_OPENCANDIDATE || wParam == IMN_CHANGECANDIDATE));

    if (placementChanged) {
        reapplySdlImeRect(hwnd, state);
    }

    const LRESULT result = state != nullptr && state->previousProc != nullptr
        ? CallWindowProcW(state->previousProc, hwnd, message, wParam, lParam)
        : DefWindowProcW(hwnd, message, wParam, lParam);

    if (placementChanged) {
        state = sdlImeState(hwnd);
        reapplySdlImeRect(hwnd, state);
        if (state != nullptr) {
            if (state->reapplyTimer != 0) {
                KillTimer(hwnd, state->reapplyTimer);
            }
            state->reapplyTimer = SetTimer(hwnd, 0, USER_TIMER_MINIMUM, nullptr);
        }
    }
    return result;
}

void installSdlImeFilter(SDL_Window* window) {
    HWND hwnd = hwndForSdlWindow(window);
    if (hwnd == nullptr || sdlImeState(hwnd) != nullptr) {
        return;
    }

    auto* state = new (std::nothrow) SdlImeFilterState{};
    if (state == nullptr) {
        return;
    }
    state->previousProc = reinterpret_cast<WNDPROC>(GetWindowLongPtrW(hwnd, GWLP_WNDPROC));
    if (state->previousProc == nullptr) {
        delete state;
        return;
    }

    gSdlImeFilters.emplace(hwnd, state);
    SetLastError(ERROR_SUCCESS);
    const LONG_PTR previous = SetWindowLongPtrW(
        hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(sdlImeWindowProc));
    if (previous == 0 && GetLastError() != ERROR_SUCCESS) {
        gSdlImeFilters.erase(hwnd);
        delete state;
    }
}

void uninstallSdlImeFilter(SDL_Window* window) {
    HWND hwnd = hwndForSdlWindow(window);
    if (hwnd == nullptr) {
        return;
    }

    auto* state = sdlImeState(hwnd);
    if (state == nullptr) {
        return;
    }

    if (state->reapplyTimer != 0) {
        KillTimer(hwnd, state->reapplyTimer);
    }
    SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(state->previousProc));
    gSdlImeFilters.erase(hwnd);
    delete state;
}

#endif
#if defined(__linux__) && !defined(__ANDROID__) && defined(SDL_VIDEO_DRIVER_X11)
struct X11ResourceApi {
    using Initialize = void (*)();
    using ResourceManagerString = char* (*)(Display*);
    using GetStringDatabase = XrmDatabase (*)(const char*);
    using GetResource = int (*)(XrmDatabase, const char*, const char*, char**, XrmValue*);
    using DestroyDatabase = void (*)(XrmDatabase);

    void* library = nullptr;
    Initialize initialize = nullptr;
    ResourceManagerString resourceManagerString = nullptr;
    GetStringDatabase getStringDatabase = nullptr;
    GetResource getResource = nullptr;
    DestroyDatabase destroyDatabase = nullptr;

    bool available() const {
        return library != nullptr && initialize != nullptr &&
               resourceManagerString != nullptr && getStringDatabase != nullptr &&
               getResource != nullptr && destroyDatabase != nullptr;
    }
};

X11ResourceApi loadX11ResourceApi() {
    X11ResourceApi api;
    void* library = SDL_LoadObject("libX11.so.6");
    if (library == nullptr) {
        library = SDL_LoadObject("libX11.so");
    }
    if (library == nullptr) {
        return api;
    }

    api.library = library;
    api.initialize = reinterpret_cast<X11ResourceApi::Initialize>(
        SDL_LoadFunction(library, "XrmInitialize"));
    api.resourceManagerString = reinterpret_cast<X11ResourceApi::ResourceManagerString>(
        SDL_LoadFunction(library, "XResourceManagerString"));
    api.getStringDatabase = reinterpret_cast<X11ResourceApi::GetStringDatabase>(
        SDL_LoadFunction(library, "XrmGetStringDatabase"));
    api.getResource = reinterpret_cast<X11ResourceApi::GetResource>(
        SDL_LoadFunction(library, "XrmGetResource"));
    api.destroyDatabase = reinterpret_cast<X11ResourceApi::DestroyDatabase>(
        SDL_LoadFunction(library, "XrmDestroyDatabase"));
    if (!api.available()) {
        SDL_UnloadObject(library);
        return {};
    }
    return api;
}

const X11ResourceApi& x11ResourceApi() {
    static const X11ResourceApi api = loadX11ResourceApi();
    return api;
}
#endif


} // namespace


float x11ContentScale(Handle window) {
    SDL_Window* sdlWindow = static_cast<SDL_Window*>(window);
    if (sdlWindow == nullptr) {
        return 0.0f;
    }
#if defined(__linux__) && !defined(__ANDROID__) && defined(SDL_VIDEO_DRIVER_X11)
    SDL_SysWMinfo info{};
    SDL_VERSION(&info.version);
    if (SDL_GetWindowWMInfo(sdlWindow, &info) != SDL_TRUE ||
        info.subsystem != SDL_SYSWM_X11 ||
        info.info.x11.display == nullptr) {
        return 0.0f;
    }

    const X11ResourceApi& api = x11ResourceApi();
    if (!api.available()) {
        return 0.0f;
    }

    static Display* cachedDisplay = nullptr;
    static float cachedScale = 1.0f;
    if (cachedDisplay == info.info.x11.display) {
        return cachedScale;
    }
    cachedDisplay = info.info.x11.display;
    cachedScale = 1.0f;

    api.initialize();
    char* resources = api.resourceManagerString(info.info.x11.display);
    if (resources == nullptr) {
        return cachedScale;
    }
    XrmDatabase database = api.getStringDatabase(resources);
    if (database == nullptr) {
        return cachedScale;
    }

    char* type = nullptr;
    XrmValue value{};
    if (api.getResource(database, "Xft.dpi", "Xft.Dpi", &type, &value) &&
        value.addr != nullptr) {
        char* end = nullptr;
        const float dpi = std::strtof(value.addr, &end);
        if (end != value.addr && std::isfinite(dpi) && dpi > 0.0f) {
            cachedScale = dpi / 96.0f;
        }
    }
    api.destroyDatabase(database);
    return cachedScale;
#else
    (void)sdlWindow;
    return 0.0f;
#endif
}

Handle createWindow(const WindowCreateRequest& request) {
    if (request.renderApi == RenderApi::OpenGL) {
        configureOpenGLWindowAttributes();
    }

    Uint32 flags = 0;
    if (request.highDpi) {
        flags |= SDL_WINDOW_ALLOW_HIGHDPI;
    }
    if (request.resizable) {
        flags |= SDL_WINDOW_RESIZABLE;
    }
    if (!request.decorated) {
        flags |= SDL_WINDOW_BORDERLESS;
    }
    if (request.alwaysOnTop) {
        flags |= SDL_WINDOW_ALWAYS_ON_TOP;
    }
    if (request.maximized) {
        flags |= SDL_WINDOW_MAXIMIZED;
    }
    flags |= request.renderApi == RenderApi::Vulkan ? SDL_WINDOW_VULKAN : SDL_WINDOW_OPENGL;

    SDL_Window* window = SDL_CreateWindow(
        request.title != nullptr ? request.title : "",
        request.positionSet ? request.x : SDL_WINDOWPOS_CENTERED,
        request.positionSet ? request.y : SDL_WINDOWPOS_CENTERED,
        request.width,
        request.height,
        flags);
    if (window != nullptr) {
        if (request.minWidth > 0 || request.minHeight > 0) {
            SDL_SetWindowMinimumSize(window,
                                     request.minWidth > 0 ? request.minWidth : 1,
                                     request.minHeight > 0 ? request.minHeight : 1);
        }
        if (request.maxWidth > 0 || request.maxHeight > 0) {
            const int minimumWidth = request.minWidth > 0 ? request.minWidth : 1;
            const int minimumHeight = request.minHeight > 0 ? request.minHeight : 1;
            constexpr int unboundedSize = std::numeric_limits<int>::max() / 4;
            SDL_SetWindowMaximumSize(window,
                                     request.maxWidth > 0
                                         ? std::max(minimumWidth, request.maxWidth)
                                         : unboundedSize,
                                     request.maxHeight > 0
                                         ? std::max(minimumHeight, request.maxHeight)
                                         : unboundedSize);
        }
    }
#if defined(__linux__) && !defined(__ANDROID__) && defined(SDL_VIDEO_DRIVER_X11)
    if (window != nullptr && request.highDpi) {
        const float scale = x11ContentScale(window);
        if (scale > 0.0f && scale != 1.0f) {
            SDL_SetWindowSize(
                window,
                static_cast<int>(std::lround(static_cast<float>(request.width) * scale)),
                static_cast<int>(std::lround(static_cast<float>(request.height) * scale)));
            SDL_SetWindowPosition(
                window,
                request.positionSet ? request.x : SDL_WINDOWPOS_CENTERED,
                request.positionSet ? request.y : SDL_WINDOWPOS_CENTERED);
        }
    }
#endif
#if defined(_WIN32)
    installSdlImeFilter(window);
#endif
    return window;
}

void destroyWindow(Handle window) {
    if (window != nullptr) {
        auto* sdlWindow = static_cast<SDL_Window*>(window);
#if defined(_WIN32)
        uninstallSdlImeFilter(sdlWindow);
#endif
        SDL_DestroyWindow(sdlWindow);
    }
}

NativeWindowInfo nativeWindowInfo(Handle window) {
    NativeWindowInfo result;
    result.handle = window;
#if defined(_WIN32) || defined(__APPLE__)
    if (window == nullptr) {
        return result;
    }
    SDL_SysWMinfo info;
    SDL_VERSION(&info.version);
    if (SDL_GetWindowWMInfo(static_cast<SDL_Window*>(window), &info) != SDL_TRUE) {
        return result;
    }
#if defined(_WIN32)
    if (info.subsystem == SDL_SYSWM_WINDOWS) {
        result.platformWindow = info.info.win.window;
    }
#elif defined(__APPLE__)
    if (info.subsystem == SDL_SYSWM_COCOA) {
        result.platformWindow = info.info.cocoa.window;
    }
#endif
#endif
    return result;
}

ContextKey currentContextKey() {
    return SDL_GL_GetCurrentContext();
}

double timeSeconds() {
    const Uint64 frequency = SDL_GetPerformanceFrequency();
    return frequency > 0
        ? static_cast<double>(SDL_GetPerformanceCounter()) / static_cast<double>(frequency)
        : 0.0;
}

void postEmptyEvent() {
    SDL_Event event{};
    event.type = SDL_USEREVENT;
    SDL_PushEvent(&event);
}

std::string clipboardText(Handle) {
    char* text = SDL_GetClipboardText();
    if (text == nullptr) {
        return {};
    }
    std::string result(text);
    SDL_free(text);
    return result;
}

void setClipboardText(const std::string& text) {
    SDL_SetClipboardText(text.c_str());
}

CursorHandle createStandardCursor(CursorType type) {
    return SDL_CreateSystemCursor(type == CursorType::Hand ? SDL_SYSTEM_CURSOR_HAND : SDL_SYSTEM_CURSOR_ARROW);
}

void setCursor(Handle, CursorHandle cursor) {
    SDL_SetCursor(static_cast<SDL_Cursor*>(cursor));
}

void destroyCursor(CursorHandle cursor) {
    SDL_FreeCursor(static_cast<SDL_Cursor*>(cursor));
}

void setWindowIcon(Handle window, int width, int height, unsigned char* pixels) {
    if (window == nullptr || pixels == nullptr || width <= 0 || height <= 0) {
        return;
    }
    SDL_Surface* surface = SDL_CreateRGBSurfaceWithFormatFrom(
        pixels, width, height, 32, width * 4, SDL_PIXELFORMAT_RGBA32);
    if (surface != nullptr) {
        SDL_SetWindowIcon(static_cast<SDL_Window*>(window), surface);
        SDL_FreeSurface(surface);
    }
    eui_set_application_icon_rgba(width, height, pixels);
}

void setImeCursorRect(Handle window, float x, float y, float width, float height) {
#if defined(_WIN32)
    SDL_Rect rect{
        static_cast<int>(x + 0.5f),
        static_cast<int>(y + 0.5f),
        static_cast<int>(width + 0.5f),
        static_cast<int>(height + 0.5f)
    };
    HWND hwnd = hwndForSdlWindow(static_cast<SDL_Window*>(window));
    if (hwnd != nullptr) {
        auto* state = sdlImeState(hwnd);
        if (state != nullptr) {
            state->rect = rect;
            state->hasRect = true;
        }
    }
    SDL_SetTextInputRect(&rect);
#else
    int windowWidth = 0;
    int windowHeight = 0;
    int drawableWidth = 0;
    int drawableHeight = 0;
    SDL_GetWindowSize(static_cast<SDL_Window*>(window), &windowWidth, &windowHeight);
    SDL_GL_GetDrawableSize(static_cast<SDL_Window*>(window), &drawableWidth, &drawableHeight);
    const float scaleX = windowWidth > 0 && drawableWidth > 0
        ? static_cast<float>(drawableWidth) / static_cast<float>(windowWidth)
        : 1.0f;
    const float scaleY = windowHeight > 0 && drawableHeight > 0
        ? static_cast<float>(drawableHeight) / static_cast<float>(windowHeight)
        : 1.0f;
    SDL_Rect rect{
        static_cast<int>(x / scaleX),
        static_cast<int>(y / scaleY),
        static_cast<int>(width / scaleX),
        static_cast<int>(height / scaleY)
    };
    SDL_SetTextInputRect(&rect);
#endif
}

} // namespace core::window

#else

#ifndef GLFW_INCLUDE_NONE
#define GLFW_INCLUDE_NONE
#endif
#include <GLFW/glfw3.h>

#include "core/platform/ime_bridge.h"

#if defined(__linux__) && !defined(__ANDROID__)
#define GLFW_EXPOSE_NATIVE_X11
#include <GLFW/glfw3native.h>
#include <X11/Xatom.h>
#include <dlfcn.h>
#endif

#if defined(_WIN32)
// l2dpet 补丁：Windows 侧"不进任务栏"要改扩展样式（WS_EX_TOOLWINDOW），
// 需要 glfwGetWin32Window / GetWindowLongPtrW。
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>
#endif

namespace core::window {

namespace {

void configureOpenGLWindowHints() {
    glfwWindowHint(GLFW_CLIENT_API, GLFW_OPENGL_API);
    glfwWindowHint(GLFW_SAMPLES, 0);
    glfwWindowHint(GLFW_RED_BITS, 8);
    glfwWindowHint(GLFW_GREEN_BITS, 8);
    glfwWindowHint(GLFW_BLUE_BITS, 8);
    glfwWindowHint(GLFW_ALPHA_BITS, 8);
    glfwWindowHint(GLFW_DEPTH_BITS, 16);
    glfwWindowHint(GLFW_STENCIL_BITS, 0);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
}

} // namespace

Handle createWindow(const WindowCreateRequest& request) {
    GLFWwindow* shareContext = nullptr;
    if (request.renderApi == RenderApi::Vulkan) {
        glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    } else {
        configureOpenGLWindowHints();
        shareContext = static_cast<GLFWwindow*>(request.parent);
    }
    glfwWindowHint(GLFW_RESIZABLE, request.resizable ? GLFW_TRUE : GLFW_FALSE);
    glfwWindowHint(GLFW_DECORATED, request.decorated ? GLFW_TRUE : GLFW_FALSE);
    glfwWindowHint(GLFW_FLOATING, request.alwaysOnTop ? GLFW_TRUE : GLFW_FALSE);
    glfwWindowHint(GLFW_MAXIMIZED, request.maximized ? GLFW_TRUE : GLFW_FALSE);
    // l2dpet 补丁：透明 / 不抢焦点 / 先隐藏（定好位置和窗口类型再显示，避免闪到默认位置）
    glfwWindowHint(GLFW_TRANSPARENT_FRAMEBUFFER, request.transparent ? GLFW_TRUE : GLFW_FALSE);
    glfwWindowHint(GLFW_FOCUS_ON_SHOW, request.focusOnShow ? GLFW_TRUE : GLFW_FALSE);
    glfwWindowHint(GLFW_FOCUSED, request.focusOnShow ? GLFW_TRUE : GLFW_FALSE);
    // 弹出窗（windowType != 0）都是隐藏创建、定好样式再显示——Windows 侧"不进任务栏"
    // 的扩展样式必须在显示前改好（见下面 _WIN32 那段的注释）。
    const bool deferShow = request.windowType != 0 || request.positionSet;
    glfwWindowHint(GLFW_VISIBLE, deferShow ? GLFW_FALSE : GLFW_TRUE);

    GLFWwindow* window = glfwCreateWindow(
        request.width,
        request.height,
        request.title != nullptr ? request.title : "",
        nullptr,
        shareContext);
    if (window == nullptr) {
        return nullptr;
    }

    const int minimumWidth = request.minWidth > 0 ? request.minWidth : GLFW_DONT_CARE;
    const int minimumHeight = request.minHeight > 0 ? request.minHeight : GLFW_DONT_CARE;
    const int maximumWidth = request.maxWidth > 0
        ? std::max(request.maxWidth, request.minWidth > 0 ? request.minWidth : 1)
        : GLFW_DONT_CARE;
    const int maximumHeight = request.maxHeight > 0
        ? std::max(request.maxHeight, request.minHeight > 0 ? request.minHeight : 1)
        : GLFW_DONT_CARE;
    if (minimumWidth != GLFW_DONT_CARE || minimumHeight != GLFW_DONT_CARE ||
        maximumWidth != GLFW_DONT_CARE || maximumHeight != GLFW_DONT_CARE) {
        glfwSetWindowSizeLimits(window, minimumWidth, minimumHeight, maximumWidth, maximumHeight);
    }
    if (request.positionSet) {
        glfwSetWindowPos(window, request.x, request.y);
    }
#if defined(GLFW_EXPOSE_NATIVE_X11)
    // l2dpet 补丁：X11 窗口类型 + 不进任务栏/切换器（弹出菜单、气泡不该出现在 Alt-Tab 里）
    // GLFW 是 dlopen 的 libX11、EUI 不链接 X11：同样用 dlsym 取这三个函数（不引入新的链接依赖）
    using InternAtomFn = Atom (*)(Display*, const char*, Bool);
    using ChangePropFn = int (*)(Display*, ::Window, Atom, Atom, int, int, const unsigned char*, int);
    using FlushFn = int (*)(Display*);
    static void* x11 = nullptr;
    // l2dpet 补丁：所有窗口都要（WM_CLASS 修正对主窗口同样必要，不只弹出窗）
    if (x11 == nullptr && glfwGetPlatform() == GLFW_PLATFORM_X11) {
        x11 = dlopen("libX11.so.6", RTLD_LAZY | RTLD_NOLOAD);
        if (x11 == nullptr) x11 = dlopen("libX11.so.6", RTLD_LAZY);
    }
    const auto XInternAtom = x11 ? reinterpret_cast<InternAtomFn>(dlsym(x11, "XInternAtom")) : nullptr;
    const auto XChangeProperty = x11 ? reinterpret_cast<ChangePropFn>(dlsym(x11, "XChangeProperty")) : nullptr;
    const auto XFlush = x11 ? reinterpret_cast<FlushFn>(dlsym(x11, "XFlush")) : nullptr;
    if (request.windowType != 0 && glfwGetPlatform() == GLFW_PLATFORM_X11 && XInternAtom && XChangeProperty && XFlush) {
        Display* dpy = glfwGetX11Display();
        ::Window xw = glfwGetX11Window(window);
        // A quick menu is an unmanaged popup, not a second top-level application
        // window. Some WMs reject activation of a managed POPUP_MENU while our
        // settings window is already visible/focused.
        if (request.windowType == 1) {
            using ChangeAttributesFn = int (*)(Display*, ::Window, unsigned long, XSetWindowAttributes*);
            const auto changeAttributes = reinterpret_cast<ChangeAttributesFn>(dlsym(x11, "XChangeWindowAttributes"));
            if (changeAttributes) {
                XSetWindowAttributes attributes{};
                attributes.override_redirect = True;
                changeAttributes(dpy, xw, CWOverrideRedirect, &attributes);
            }
        }
        const char* typeName = request.windowType == 1 ? "_NET_WM_WINDOW_TYPE_POPUP_MENU"
                             : request.windowType == 2 ? "_NET_WM_WINDOW_TYPE_TOOLTIP"
                                                       : "_NET_WM_WINDOW_TYPE_UTILITY";
        Atom type = XInternAtom(dpy, typeName, False);
        XChangeProperty(dpy, xw, XInternAtom(dpy, "_NET_WM_WINDOW_TYPE", False), XA_ATOM, 32, PropModeReplace,
                        reinterpret_cast<unsigned char*>(&type), 1);
        Atom states[3] = { XInternAtom(dpy, "_NET_WM_STATE_SKIP_TASKBAR", False),
                           XInternAtom(dpy, "_NET_WM_STATE_SKIP_PAGER", False),
                           XInternAtom(dpy, "_NET_WM_STATE_ABOVE", False) };
        XChangeProperty(dpy, xw, XInternAtom(dpy, "_NET_WM_STATE", False), XA_ATOM, 32, PropModeReplace,
                        reinterpret_cast<unsigned char*>(states), request.alwaysOnTop ? 3 : 2);
        if (request.userTime != 0) {
            // 防抢焦点：带上触发它的用户操作时间（右键那一下），WM 才肯把焦点给这个新窗口
            unsigned long t = request.userTime;
            XChangeProperty(dpy, xw, XInternAtom(dpy, "_NET_WM_USER_TIME", False), XA_CARDINAL, 32, PropModeReplace,
                            reinterpret_cast<unsigned char*>(&t), 1);
        }
        XFlush(dpy);
    }
    // l2dpet 补丁：WM_CLASS 固定 ASCII——GLFW 用"窗口标题"兜底 WM_CLASS，中文标题会被
    // dock/顶栏按 latin-1 解码显示成乱码；标题显示走 _NET_WM_NAME 不受影响。
    // （所有窗口都设，不只弹出窗；XA_STRING 原子值恒为 31）
    if (glfwGetPlatform() == GLFW_PLATFORM_X11 && XInternAtom && XChangeProperty && XFlush) {
        Display* dpy = glfwGetX11Display();
        ::Window xw = glfwGetX11Window(window);
        static const char kClass[] = "l2dpet-ui\0l2dpet-ui";   // instance\0class\0
        XChangeProperty(dpy, xw, XInternAtom(dpy, "WM_CLASS", False), 31, 8, PropModeReplace,
                        reinterpret_cast<const unsigned char*>(kClass), (int)sizeof(kClass));
        XFlush(dpy);
    }
#endif
#if defined(_WIN32)
    // l2dpet 补丁：**弹出窗口**（右键菜单 windowType=1 / 气泡 2 / 工具窗 3）不进任务栏与
    // Alt-Tab——它们是附属窗，冒出来一个任务栏按钮很突兀。主窗口（状态与设置）保持原样：
    // 用户本来就要在任务栏里看到它、能切回来。GLFW 给窗口默认带 WS_EX_APPWINDOW，
    // 必须一并去掉，否则工具窗样式不生效。此刻窗口还没显示（见上面的 deferShow）。
    if (request.windowType != 0) {
        HWND hwnd = glfwGetWin32Window(window);
        if (hwnd != nullptr) {
            const LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
            SetWindowLongPtrW(hwnd, GWL_EXSTYLE, (ex | WS_EX_TOOLWINDOW) & ~WS_EX_APPWINDOW);
        }
    }
#endif
    if (deferShow) {
        glfwShowWindow(window);
        if (request.positionSet) {
            glfwSetWindowPos(window, request.x, request.y);   // 有些 WM 映射时会重放置：再定一次
        }
#if defined(GLFW_EXPOSE_NATIVE_X11)
        // l2dpet 补丁：弹出窗（菜单/气泡）显式抬到最上。桌宠进程的角色窗口是另一个进程的
        // 置顶窗口，被隐藏再显示（重新映射）后会排到这些窗口上面——气泡/菜单其实开了却看不见。
        using RaiseFn = int (*)(Display*, ::Window);
        const auto XRaiseWindowFn = x11 ? reinterpret_cast<RaiseFn>(dlsym(x11, "XRaiseWindow")) : nullptr;
        if (request.windowType != 0 && XRaiseWindowFn && glfwGetPlatform() == GLFW_PLATFORM_X11) {
            XRaiseWindowFn(glfwGetX11Display(), glfwGetX11Window(window));
        }
        if (request.windowType == 1 && request.focusOnShow && glfwGetPlatform() == GLFW_PLATFORM_X11) {
            using SetInputFocusFn = int (*)(Display*, ::Window, int, Time);
            const auto setFocus = x11 ? reinterpret_cast<SetInputFocusFn>(dlsym(x11, "XSetInputFocus")) : nullptr;
            if (setFocus) {
                setFocus(glfwGetX11Display(), glfwGetX11Window(window), RevertToParent,
                         request.userTime ? request.userTime : CurrentTime);
                if (XFlush) XFlush(glfwGetX11Display());
            }
        }
#endif
#if defined(GLFW_EXPOSE_NATIVE_X11)
        // Unmanaged menus take input focus directly above; do not ask the WM
        // to activate them as a managed application window.
#endif
    }
    return window;
}

void destroyWindow(Handle window) {
    if (window != nullptr) {
        glfwDestroyWindow(static_cast<GLFWwindow*>(window));
    }
}

NativeWindowInfo nativeWindowInfo(Handle window) {
    NativeWindowInfo result;
    result.handle = window;
    // l2dpet 补丁：GLFW 后端的这个函数原来是"只回 handle、不回原生窗口"的桩（SDL 那版才填），
    // 于是应用拿不到 HWND（面板设深色标题栏要它）。X11 侧暂不需要（要 NativeWindowInfo 的
    // 应用只有面板，而深色标题栏是 Windows 的事），所以这里只补 Win32。
#if defined(_WIN32)
    if (window != nullptr) {
        result.platformWindow = glfwGetWin32Window(static_cast<GLFWwindow*>(window));
    }
#endif
    return result;
}

ContextKey currentContextKey() {
    return glfwGetCurrentContext();
}

double timeSeconds() {
    return glfwGetTime();
}

void postEmptyEvent() {
    glfwPostEmptyEvent();
}

std::string clipboardText(Handle window) {
    const char* text = glfwGetClipboardString(static_cast<GLFWwindow*>(window));
    return text != nullptr ? text : "";
}

void setClipboardText(const std::string& text) {
    glfwSetClipboardString(glfwGetCurrentContext(), text.c_str());
}

CursorHandle createStandardCursor(CursorType type) {
    return glfwCreateStandardCursor(type == CursorType::Hand ? GLFW_HAND_CURSOR : GLFW_ARROW_CURSOR);
}

void setCursor(Handle window, CursorHandle cursor) {
    glfwSetCursor(static_cast<GLFWwindow*>(window), static_cast<GLFWcursor*>(cursor));
}

void destroyCursor(CursorHandle cursor) {
    glfwDestroyCursor(static_cast<GLFWcursor*>(cursor));
}

void setWindowIcon(Handle window, int width, int height, unsigned char* pixels) {
    if (window == nullptr || pixels == nullptr || width <= 0 || height <= 0) {
        return;
    }
    GLFWimage image{};
    image.width = width;
    image.height = height;
    image.pixels = pixels;
    glfwSetWindowIcon(static_cast<GLFWwindow*>(window), 1, &image);
    eui_set_application_icon_rgba(width, height, pixels);
}

void setImeCursorRect(Handle window, float x, float y, float width, float height) {
    eui_ime_set_cursor_rect_with_font(static_cast<GLFWwindow*>(window), x, y, width, height, height);
}

} // namespace core::window

#endif

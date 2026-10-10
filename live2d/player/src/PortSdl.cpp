/**
 * PortSdl.cpp — SDL2 端口：跨平台桌面窗口 + 鼠标交互
 *
 * 编译期可选：CMake 检测到 SDL2 开发包时定义 L2D_WITH_SDL 才编入；
 * 否则 CreatePortSdl 返回 NULL（自动探测时被跳过）。
 */

#include "Port.hpp"
#include "l2d_config.h"

#include <cstddef> // NULL（stub 分支亦需要）

#ifdef L2D_WITH_SDL

#include <cstdio>
#include <cstring>
#include <time.h>

#include <SDL.h>

namespace
{

class PortSdl : public IPort
{
public:
    PortSdl()
        : _win(NULL)
        , _ren(NULL)
        , _tex(NULL)
        , _w(480)
        , _h(800)
        , _frameNs(0)
        , _btnDown(false)
        , _quit(false)
    {
        memset(&_nextFrame, 0, sizeof(_nextFrame));
    }

    ~PortSdl()
    {
        if (_tex != NULL) SDL_DestroyTexture(_tex);
        if (_ren != NULL) SDL_DestroyRenderer(_ren);
        if (_win != NULL) SDL_DestroyWindow(_win);
        if (_win != NULL) SDL_Quit();
    }

    bool Initialize(const PortConfig& cfg) override
    {
        if (SDL_Init(SDL_INIT_VIDEO) != 0)
        {
            return false;
        }
        _w = cfg.width > 0 ? cfg.width : L2D_SCREEN_WIDTH;
        _h = cfg.height > 0 ? cfg.height : L2D_SCREEN_HEIGHT;

        _win = SDL_CreateWindow("l2dtest (SDL port)", SDL_WINDOWPOS_CENTERED,
                                SDL_WINDOWPOS_CENTERED, _w, _h, 0);
        if (_win == NULL)
        {
            SDL_Quit();
            return false;
        }
        _ren = SDL_CreateRenderer(_win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
        if (_ren == NULL)
        {
            _ren = SDL_CreateRenderer(_win, -1, 0); // 软件渲染兜底
        }
        if (_ren == NULL)
        {
            SDL_DestroyWindow(_win);
            SDL_Quit();
            return false;
        }
        _tex = SDL_CreateTexture(_ren, SDL_PIXELFORMAT_ARGB8888,
                                 SDL_TEXTUREACCESS_STREAMING, _w, _h);
        if (_tex == NULL)
        {
            SDL_DestroyRenderer(_ren);
            SDL_DestroyWindow(_win);
            SDL_Quit();
            return false;
        }
        SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1");

        _frameNs = cfg.maxFps > 0 ? (1000000000L / cfg.maxFps) : 0;
        clock_gettime(CLOCK_MONOTONIC, &_nextFrame);
        printf("[port:sdl] 窗口 %dx%d（鼠标拖动=跟随，单击=切表情）\n", _w, _h);
        fflush(stdout);
        return true;
    }

    const char* Name() const override { return "sdl"; }
    int Width() const override { return _w; }
    int Height() const override { return _h; }

    bool Present(const uint32_t* fb, int width, int height) override
    {
        (void)width;
        (void)height;
        SDL_UpdateTexture(_tex, NULL, fb, _w * 4);
        SDL_RenderClear(_ren);
        SDL_RenderCopy(_ren, _tex, NULL, NULL);
        SDL_RenderPresent(_ren);
        return true;
    }

    bool PollInput(int* x, int* y, bool* down) override
    {
        SDL_Event e;
        while (SDL_PollEvent(&e))
        {
            switch (e.type)
            {
            case SDL_QUIT:
                _quit = true;
                break;
            case SDL_MOUSEBUTTONDOWN:
                if (e.button.button == SDL_BUTTON_LEFT)
                {
                    _btnDown = true;
                    *x = e.button.x;
                    *y = e.button.y;
                    *down = true;
                    return true;
                }
                break;
            case SDL_MOUSEBUTTONUP:
                if (e.button.button == SDL_BUTTON_LEFT)
                {
                    _btnDown = false;
                    *x = e.button.x;
                    *y = e.button.y;
                    *down = false;
                    return true;
                }
                break;
            case SDL_MOUSEMOTION:
                if (_btnDown)
                {
                    *x = e.motion.x;
                    *y = e.motion.y;
                    *down = true;
                    return true;
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
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &_nextFrame, NULL);
    }

    bool ShouldQuit() override { return _quit; }

private:
    SDL_Window* _win;
    SDL_Renderer* _ren;
    SDL_Texture* _tex;
    int _w;
    int _h;
    long _frameNs;
    struct timespec _nextFrame;
    bool _btnDown;
    bool _quit;
};

} // namespace

IPort* CreatePortSdl()
{
    return new PortSdl();
}

#else // !L2D_WITH_SDL

IPort* CreatePortSdl()
{
    return NULL; // 未启用 SDL 支持
}

#endif

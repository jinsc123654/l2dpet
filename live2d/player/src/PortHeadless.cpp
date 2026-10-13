/**
 * PortHeadless.cpp — 纯软件兜底端口：无显示、无输入
 *
 * "最差最差也是纯软件解算"——任何环境（无 DRM、无 X11、无 SDL）都能跑：
 * 渲染照常进行（配合 --render 可按帧落 PPM 文件），只是不上屏。
 */

#include "Port.hpp"
#include "Platform.h"      // clock_gettime / clock_nanosleep（MSVC 垫片）
#include "l2d_config.h"

#include <cstdio>
#include <cstring>
#include <time.h>

namespace
{

class PortHeadless : public IPort
{
public:
    PortHeadless()
        : _w(480)
        , _h(800)
        , _frameNs(0)
    {
        memset(&_nextFrame, 0, sizeof(_nextFrame));
    }

    bool Initialize(const PortConfig& cfg) override
    {
        _w = cfg.width > 0 ? cfg.width : L2D_SCREEN_WIDTH;
        _h = cfg.height > 0 ? cfg.height : L2D_SCREEN_HEIGHT;
        _frameNs = cfg.maxFps > 0 ? (1000000000L / cfg.maxFps) : 0;
        clock_gettime(CLOCK_MONOTONIC, &_nextFrame);
        printf("[port:headless] 纯软解算 %dx%d（无显示输出；--render 可落帧）\n", _w, _h);
        fflush(stdout);
        return true;
    }

    const char* Name() const override { return "headless"; }
    int Width() const override { return _w; }
    int Height() const override { return _h; }

    bool Present(const uint32_t* fb, int width, int height) override
    {
        (void)fb;
        (void)width;
        (void)height;
        return true; // 无显示：帧由上层按需落盘
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
        plat::SleepUntil(_nextFrame);
    }

private:
    int _w;
    int _h;
    long _frameNs;
    struct timespec _nextFrame;
};

} // namespace

IPort* CreatePortHeadless()
{
    return new PortHeadless();
}

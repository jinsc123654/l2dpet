/**
 * PetBootstrap.h — 命令行选项与解析（原 l2dtest.cpp 的 main 前半段）。
 *
 * 从 l2dtest.cpp 拆出（纯搬移，逻辑未改）。选项默认值仍来自 l2d_config.h。
 */
#pragma once

#include <CubismFramework.hpp>   // csmFloat32 / csmBool（选项类型）
#include "l2d_config.h"

#include <string>

extern bool g_uiOff;   // 关掉自带 UI（定义在 l2dtest.cpp）

/** 命令行选项（原来散在 main 里的一批局部量） */
using namespace Live2D::Cubism::Framework;   // csmFloat32 / csmBool 未限定使用

struct PetOptions
{
    const char* model3Path = NULL;
    // 默认值全部来自 l2d_config.h（可被命令行覆盖）
    int frameCount = L2D_DEFAULT_FRAMES;
    csmFloat32 dt = 1.0f / (csmFloat32)L2D_DEFAULT_FPS;
    bool noMotion = false;
    int reportEvery = L2D_REPORT_EVERY;
    int dumpVertsFrame = -1;
    int dumpParamsFrame = -1;   // --dump-params <n>：导出第 n 帧全部参数值（id=value；诊断用）
    int renderWidth = L2D_SCREEN_WIDTH;
    int renderHeight = L2D_SCREEN_HEIGHT;
    const char* renderDir = NULL;
    const char* playPpmDir = NULL; // --play-ppm：离线帧序列上屏（对照/验收用）
    int renderEvery = L2D_REPORT_EVERY;
    const char* portRequest = L2D_PORT_DEFAULT; // auto|x11|sdl|headless|pet
    int ctlPort = 23333;                        // --ctl <端口>：本机协议端口（默认 23333，与面板一致；0 = 不开）
    bool ctlExplicit = false;                  // --ctl overrides environment and saved port
    int cmdFd = -1;                             // --cmd-fd <n>：指令环（表情/动作/手势/演出脚本）
    const char* modelDir = NULL;                // --model-dir（模型库目录；底部箭头切换）
    int targetFps = L2D_DEFAULT_FPS;
    int rasterThreads = L2D_DEFAULT_THREADS;
    float renderScale = L2D_DEFAULT_SCALE;
    float renderZoom = L2D_DEFAULT_ZOOM;
    bool autoFit = L2D_DEFAULT_AUTOFIT;     // 等比最大填充（自动 zoom）
    float fitFill = L2D_AUTOFIT_FILL;
    bool followEnabled = L2D_DEFAULT_FOLLOW; // 触摸拖动跟随（眼睛/头部注视手指）
    bool watermarkOff = true;                // --watermark on|off（默认 off：模型自带"水印"表情=作者开关，默认应用它）
    bool staticOrder = true;                 // --order static|dynamic（桌面默认 static：首帧冻结绘制顺序；--order dynamic 恢复官方动态序）
    int lookX = 0, lookY = 0;                // --look x,y：固定注视目标（像素坐标；对拍/调试用）
    bool hasLook = false;
    bool followTest = false;  // --follow-test：目标自动循环四角（验证用）
    bool exprTest = false;    // --expr-test：每 2 秒自动切表情（调试）
    bool modelSwitchTest = false; // --model-switch-test：每 3 秒自动切模型（调试）
    const char* shotPath = NULL; // --shot：屏幕截图输出（PPM）
    int shotFrame = 30;
};

/** 解析命令行。返回 <0 = 可以继续跑；否则返回值就是进程退出码（0 = 只看了用法）。 */
int ParseArgs(int argc, char** argv, PetOptions* o);

/** PetBootstrap.cpp — 见 PetBootstrap.h（从 l2dtest.cpp 拆出，纯搬移） */
#include "PetBootstrap.h"

#include "PpmPlayer.h"    // --play-ppm 的 RunPpmPlayer
#include "FileUtil.h"
#include "Platform.h"

#include <cstring>        // strlen / strncmp（MSVC 由其它头间接带入，gcc/clang 不会）

/** 取 --name=value / --name value 的值（原 l2dtest.cpp 的解析助手，随解析搬过来） */
bool OptValue(const char* arg, int* i, int argc, char** argv,
                     const char* name, const char** outValue)
{
    const size_t n = strlen(name);
    if (strncmp(arg, name, n) != 0)
    {
        return false;
    }
    if (arg[n] == '=')
    {
        *outValue = arg + n + 1;
        return true;
    }
    if (arg[n] == '\0' && (*i + 1) < argc)
    {
        *outValue = argv[++(*i)];
        return true;
    }
    return false;
}
#include <cstdio>
#include <cstdlib>
#include <csignal>
#include <cstring>

// 退出信号的处理函数定义在 l2dtest.cpp（main 之外那个文件）
extern void HandleQuitSignal(int);

int ParseArgs(int argc, char** argv, PetOptions* o)
{
    {
        int positional = 0;
        for (int i = 1; i < argc; ++i)
        {
            const char* opt = NULL; // 选项值（--name=value / --name value 通用）
            if (strcmp(argv[i], "--no-motion") == 0)
            {
                o->noMotion = true;
                continue;
            }
            if (OptValue(argv[i], &i, argc, argv, "--port", &opt))
            {
                o->portRequest = opt;
                continue;
            }
            if (OptValue(argv[i], &i, argc, argv, "--cmd-fd", &opt))
            {
                o->cmdFd = atoi(opt); // 指令环（宿主给的 memfd；表情/动作/手势/脚本）
                continue;
            }
            if (strcmp(argv[i], "--pet") == 0)
            {
                // 显式 --port 优先（o->portRequest 默认值是 "auto"，不是 NULL）
                if (o->portRequest == NULL || strcmp(o->portRequest, "auto") == 0)
                {
#if defined(__linux__)
                    o->portRequest = "pet";       // Linux：X11 原生端口（最成熟）
#else
                    o->portRequest = "glfwpet";   // Windows/macOS：GLFW 跨平台端口
#endif
                }
                g_uiOff = true;        // 帧里只留角色
                continue;
            }
            if (OptValue(argv[i], &i, argc, argv, "--ctl", &opt))
            {
                o->ctlPort = atoi(opt);
                continue;
            }
            if (strcmp(argv[i], "--no-ui") == 0)
            {
                g_uiOff = true;   // 显式关自带 UI（宿主是别的图形栈时用）
                continue;
            }
            if (OptValue(argv[i], &i, argc, argv, "--model-dir", &opt))
            {
                o->modelDir = opt;
                continue;
            }
            if (OptValue(argv[i], &i, argc, argv, "--threads", &opt))
            {
                o->rasterThreads = atoi(opt);
                if (o->rasterThreads < 1)
                {
                    o->rasterThreads = 1;
                }
                continue;
            }
            if (OptValue(argv[i], &i, argc, argv, "--scale", &opt))
            {
                o->renderScale = (float)atof(opt);
                if (o->renderScale <= 0.0f)
                {
                    o->renderScale = 1.0f;
                }
                if (o->renderScale > 1.0f)
                {
                    o->renderScale = 1.0f;
                }
                continue;
            }
            if (OptValue(argv[i], &i, argc, argv, "--zoom", &opt))
            {
                o->renderZoom = (float)atof(opt);
                if (o->renderZoom <= 0.0f)
                {
                    o->renderZoom = 1.0f;
                }
                continue;
            }
            if (strcmp(argv[i], "--fit") == 0)
            {
                o->autoFit = true;
                continue;
            }
            if (OptValue(argv[i], &i, argc, argv, "--fit-fill", &opt))
            {
                o->fitFill = (float)atof(opt);
                if (o->fitFill <= 0.0f || o->fitFill > 1.0f)
                {
                    o->fitFill = 0.95f;
                }
                continue;
            }
            if (strcmp(argv[i], "--follow") == 0)
            {
                o->followEnabled = true;
                continue;
            }
            if (strcmp(argv[i], "--no-follow") == 0)
            {
                o->followEnabled = false;
                continue;
            }
            if (strcmp(argv[i], "--follow-test") == 0)
            {
                o->followEnabled = true;
                o->followTest = true;
                continue;
            }
            if (OptValue(argv[i], &i, argc, argv, "--watermark", &opt))
            {
                // on=保留模型自带水印；off（默认）=应用模型自带的"水印"表情（作者开关）
                if (strcmp(opt, "on") == 0)
                    o->watermarkOff = false;
                else if (strcmp(opt, "off") != 0)
                    fprintf(stderr, "[l2dtest] --watermark 只认 on/off（收到 %s），按 off 处理\n", opt);
                continue;
            }
            if (OptValue(argv[i], &i, argc, argv, "--order", &opt))
            {
                // static = 绘制顺序首帧冻结：关掉 5.x 模型"参数驱动绘制顺序"的弹层效果
                //（实测 shuflle 看向右上角/呼吸抬头时把胸口皮肤层弹到裙子上面）；
                // dynamic（默认）= 官方 Core 语义（每帧按 csmGetDrawableDrawOrders 重排）
                // ——官方语义对多数模型是正确的（如 Haru 提手动作要把双手画到裙子前面）。
                if (strcmp(opt, "static") == 0)
                    o->staticOrder = true;
                else if (strcmp(opt, "dynamic") != 0)
                    fprintf(stderr, "[l2dtest] --order 只认 static/dynamic（收到 %s），按 dynamic 处理\n", opt);
                continue;
            }
            if (OptValue(argv[i], &i, argc, argv, "--look", &opt))
            {
                // 固定注视目标（像有一根手指一直按在 (x,y)）：对拍/复现用，比触碰更确定
                if (sscanf(opt, "%d,%d", &o->lookX, &o->lookY) == 2)
                    o->hasLook = true;
                else
                    fprintf(stderr, "[l2dtest] --look 需要 x,y（渲染目标像素坐标）\n");
                continue;
            }
            if (strcmp(argv[i], "--expr-test") == 0)
            {
                o->exprTest = true;
                continue;
            }
            if (strcmp(argv[i], "--model-switch-test") == 0)
            {
                o->modelSwitchTest = true;
                continue;
            }
            if (OptValue(argv[i], &i, argc, argv, "--shot", &opt))
            {
                o->shotPath = opt;
                continue;
            }
            if (OptValue(argv[i], &i, argc, argv, "--shot-frame", &opt))
            {
                o->shotFrame = atoi(opt);
                continue;
            }
            if (OptValue(argv[i], &i, argc, argv, "--fps", &opt))
            {
                o->targetFps = atoi(opt);
                continue;
            }
            if (OptValue(argv[i], &i, argc, argv, "--every", &opt))
            {
                o->reportEvery = atoi(opt);
                if (o->reportEvery < 1)
                {
                    o->reportEvery = 1;
                }
                continue;
            }
            if (OptValue(argv[i], &i, argc, argv, "--dump-verts", &opt))
            {
                o->dumpVertsFrame = atoi(opt);
                continue;
            }
            if (OptValue(argv[i], &i, argc, argv, "--dump-params", &opt))
            {
                o->dumpParamsFrame = atoi(opt);
                continue;
            }
            if (OptValue(argv[i], &i, argc, argv, "--size", &opt))
            {
                int w = 0, h = 0;
                if (sscanf(opt, "%dx%d", &w, &h) == 2 && w > 0 && h > 0)
                {
                    o->renderWidth = w;
                    o->renderHeight = h;
                }
                continue;
            }
            if (strcmp(argv[i], "--render") == 0 && i + 1 < argc)
            {
                o->renderDir = argv[++i];
                continue;
            }
            if (OptValue(argv[i], &i, argc, argv, "--play-ppm", &opt))
            {
                o->playPpmDir = opt; // 离线帧序列上屏（不加载模型；见 RunPpmPlayer）
                continue;
            }
            if (OptValue(argv[i], &i, argc, argv, "--render-every", &opt))
            {
                o->renderEvery = atoi(opt);
                if (o->renderEvery < 1)
                {
                    o->renderEvery = 1;
                }
                continue;
            }
            switch (positional++)
            {
            case 0:
                // 未给模型路径时，位置 0 可为帧数（纯数字开头）
                if (argv[i][0] >= '0' && argv[i][0] <= '9')
                {
                    o->frameCount = atoi(argv[i]);
                }
                else
                {
                    o->model3Path = argv[i];
                }
                break;
            case 1: o->frameCount = atoi(argv[i]); break;
            case 2: o->dt = (csmFloat32)atof(argv[i]); break;
            default: break;
            }
        }
    }

    signal(SIGTERM, HandleQuitSignal);
    signal(SIGINT, HandleQuitSignal);

    // --- --play-ppm：离线帧序列上屏（不加载模型、不初始化框架，最小路径）---
    if (o->playPpmDir != NULL)
    {
        return RunPpmPlayer(o->playPpmDir, o->targetFps, o->portRequest,
                            o->renderWidth, o->renderHeight);
    }

    // 双击运行（一个参数都没有）= 桌面桌宠模式：等价于 --pet，并优先把程序自带的 ./models 当模型库。
    // 便携包里双击 l2dtest.exe 就能用——不用 run.bat，也不用记参数。
    if (argc == 1)
    {
#if defined(__linux__)
        o->portRequest = "pet";        // Linux：X11 原生端口
#else
        o->portRequest = "glfwpet";    // Windows/macOS：GLFW 跨平台端口
#endif
        g_uiOff = true;               // 帧里只留角色
        o->frameCount = 0;               // 一直跑：双击进来不是为了跑满默认帧数就退出
        static std::string s_exeModels;   // 生命周期要长于 o->modelDir（后者只存 const char*）
        s_exeModels = DirName(plat::SelfExePath()) + "/models";
        if (plat::IsDir(s_exeModels)) o->modelDir = s_exeModels.c_str();
    }


    return -1;   // 可以继续
}

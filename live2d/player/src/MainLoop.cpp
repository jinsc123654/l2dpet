/** MainLoop.cpp — 见 MainLoop.h（从 l2dtest.cpp 拆出，纯搬移） */
#include "MainLoop.h"

#include "L2DPlayer.h"
#include "FileUtil.h"
#include "Platform.h"
#include "Port.hpp"
#include "CtlServer.hpp"
#include "L2dCmd.h"
#include "PpmPlayer.h"    // g_quit（主循环的退出标志）

#include <CubismFramework.hpp>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#if !defined(_WIN32)
#include <sys/mman.h>   // mmap（指令环）
#if defined(__GLIBC__)
#include <malloc.h>     // malloc_trim（glibc 专属）
#endif
#endif

using namespace Live2D::Cubism::Framework;

// 定义在 l2dtest.cpp（指令环 / 证据输出），这里给跨文件声明
extern l2d_cmd_ring_t* g_cmdRing;
extern csmUint64 Fnv64VertexChecksum(const CubismModel* model, csmInt32* outVertexCount);
extern void PrintSampleVertices(const CubismModel* model);
extern void DrainCmdRing(L2DPlayer& player);
extern void ExecuteCmdLine(L2DPlayer& player, const char* line);

namespace
{

/** 桌宠运行期状态：原来是 main 里的局部量，现在升级成成员（名字不变，便于对照） */
struct PetRuntime
{
    PetOptions* opt = NULL;

    int Run();
};

}   // namespace

int PetRuntime::Run()
{
    // --- 指令环映射（--cmd-fd：宿主给的 memfd；没有就跳过，本进程仍可独立跑）---
    // 宿主形态专用（fd 传递 + mmap）；其它平台走协议端口
#if !defined(_WIN32)
    if (opt->cmdFd >= 0)
    {
        void* p = mmap(NULL, l2d_cmd_size(), PROT_READ | PROT_WRITE, MAP_SHARED, opt->cmdFd, 0);

        if (p == MAP_FAILED)
        {
            fprintf(stderr, "[l2dtest] 指令环 mmap 失败：%s\n", strerror(errno));
        }
        else if (((l2d_cmd_ring_t*)p)->magic != L2D_CMD_MAGIC)
        {
            fprintf(stderr, "[l2dtest] 指令环 magic 不符（拿错 fd？）\n");
        }
        else
        {
            g_cmdRing = (l2d_cmd_ring_t*)p;
            printf("[l2dtest] 指令环就绪（%zu 字节）\n", l2d_cmd_size());
            fflush(stdout);
        }
    }
#endif   // !_WIN32

    // --- Framework 启动 ---
    static L2DAllocator allocator;
    CubismFramework::Option option;
    option.LogFunction = [](const csmChar* message) { fprintf(stderr, "[csm] %s", message); };
    option.LoggingLevel = CubismFramework::Option::LogLevel_Warning;
    CubismFramework::StartUp(&allocator, &option);
    CubismFramework::Initialize();

    // --- 模型库（底部 ◀/▶ 箭头循环切换）---
    // 目录优先级：--model-dir > 位置参数的所在目录 > config L2D_MODEL_DIR
    std::string scanDir;
    if (opt->modelDir != NULL)
    {
        scanDir = opt->modelDir;
    }
    else if (opt->model3Path != NULL)
    {
        scanDir = DirName(RealPath(opt->model3Path));
    }
    else if (L2D_MODEL_DIR[0] != '\0')
    {
        scanDir = L2D_MODEL_DIR;
    }
    std::vector<std::string> modelList;
    ScanModelDir(scanDir.c_str(), &modelList);
    // 桌宠：再并入用户模型库 ~/.local/share/l2dpet/models/<名字>/<x>.model3.json（托盘/右键菜单里可选）
    if (opt->portRequest != NULL && (strcmp(opt->portRequest, "pet") == 0 || strcmp(opt->portRequest, "glfwpet") == 0))
    {
        std::vector<std::string> lib;
        ScanModelDir(L2DPlayer::ModelsLibDir().c_str(), &lib);
        for (size_t i = 0; i < lib.size(); ++i)
        {
            const std::string rp = RealPath(lib[i].c_str());
            bool dup = false;
            for (size_t k = 0; k < modelList.size() && !dup; ++k) dup = RealPath(modelList[k].c_str()) == rp;
            if (!dup) modelList.push_back(lib[i]);
        }
    }
    int currentModel = 0;
    if (opt->model3Path == NULL)
    {
        if (modelList.empty())
        {
            fprintf(stderr, "[l2dtest] 模型库为空: %s（也没有位置参数指定模型）\n", scanDir.c_str());
            CubismFramework::Dispose();
            return 1;
        }
    }
    else
    {
        const std::string want = RealPath(opt->model3Path);
        bool found = false;
        for (size_t i = 0; i < modelList.size(); ++i)
        {
            if (modelList[i] == want || RealPath(modelList[i].c_str()) == want)
            {
                currentModel = (int)i;
                found = true;
                break;
            }
        }
        if (!found)
        {
            modelList.insert(modelList.begin(), want); // 指定的模型不在库里：放第一个
            currentModel = 0;
        }
    }
    printf("[models] 模型库 %s：%d 个模型\n", scanDir.c_str(), (int)modelList.size());
    for (size_t i = 0; i < modelList.size() && i < 12; ++i)
    {
        printf("  [%2d] %s\n", (int)i, BaseName(DirName(modelList[i])).c_str());
    }

    // --- 加载模型（支持运行时切换：placement new 重建）---
    L2DPlayer player;
    // 恢复上次选中的模型（config 的 str.model）：不记的话每次启动都回到扫描到的第一个，
    // 用户切到某个模型后一重启就"变回 Hiyori"。按目录名匹配（与面板按名字切换同一套）。
    // 气泡字号（config 的 bubble_px）推给桌宠端口：端口不读配置，用环境变量传（见 PortGlfwPet::ShowBubble）
    {
        player.CfgLoad();   // 幂等；确保读到的是磁盘上的最新配置
        const csmFloat32 bp = player.Cfg("bubble_px");
        if (bp >= 8.0f && bp <= 64.0f)
        {
            char bpx[32];
            snprintf(bpx, sizeof(bpx), "%d", (int)(bp + 0.5f));
            plat::SetEnv("L2DPET_BUBBLE_PX", bpx);
        }
        // 气泡与角色的间距（config 的 bubble_gap）：与字号同一套环境变量通道
        const csmFloat32 bg = player.Cfg("bubble_gap");
        if (bg >= 0.0f && bg <= 120.0f)
        {
            char bgp[32];
            snprintf(bgp, sizeof(bgp), "%d", (int)(bg + 0.5f));
            plat::SetEnv("L2DPET_BUBBLE_GAP", bgp);
        }
    }

    if (opt->model3Path == NULL)
    {
        player.CfgLoad();   // 幂等；确保读到的是磁盘上的最新配置
        const std::string wantModel = player.CfgStr("model");
        if (!wantModel.empty())
        {
            for (size_t i = 0; i < modelList.size(); ++i)
            {
                if (BaseName(DirName(modelList[i])) == wantModel)
                {
                    currentModel = (int)i;
                    printf("[models] 按配置选中上次的模型：%s\n", wantModel.c_str());
                    break;
                }
            }
        }
    }
    CubismModel* model = NULL;
    csmInt32 drawableCount = 0;
    csmInt32 parameterCount = 0;
    std::vector<std::string> paramIds;
    std::vector<csmFloat32> baseline;

    /** 重建每模型缓存（切换后必须调用） */
    auto refreshModelState = [&]()
    {
        model = player.GetModel();
        drawableCount = model->GetDrawableCount();
        parameterCount = model->GetParameterCount();
        paramIds.assign((size_t)parameterCount, std::string());
        baseline.assign((size_t)parameterCount, 0.0f);
        for (csmInt32 i = 0; i < parameterCount; ++i)
        {
            paramIds[i] = model->GetParameterId(i)->GetString().GetRawString();
            baseline[i] = model->GetParameterValue(i);
        }
    };

    /** 加载模型资产（不含渲染装配；切换模型时复用） */
    auto loadModelAssets = [&](const std::string& path) -> bool
    {
        const csmFloat32 keepZoom = player.RenderZoom(); // 保持用户当前缩放
        player.DeleteRenderer();
        player.~L2DPlayer();
#if defined(__GLIBC__)
        malloc_trim(0); // 把旧模型的空闲页交还系统（内存受限机器：防切换峰值翻倍）
#endif
        new (&player) L2DPlayer();
        player.SetNoMotion(opt->noMotion);
        player.SetRenderZoom(keepZoom > 0.0f ? keepZoom : opt->renderZoom);
        // 切换后固定用"当前缩放"（不重新自动适配；用户可拖缩放条再调）
        player.SetTouchFollow(opt->followEnabled);
        player.SetFollowTest(opt->followTest);
        player.SetExprTest(opt->exprTest);
    player.SetModelSwitchTest(opt->modelSwitchTest);
        player.SetWatermarkOff(opt->watermarkOff);
        if (!player.Load(path.c_str()))
        {
            return false;
        }
        refreshModelState();
        player.SetModelName(BaseName(DirName(path)).c_str()); // 目录名 = 模型名（toast）
        return true;
    };

    /** 渲染装配（首次在 port 定尺寸后调用；切换时沿用现有尺寸） */
    auto setupRenderForModel = [&]() -> bool
    {
        if (!player.SetupRender((csmUint32)opt->renderWidth, (csmUint32)opt->renderHeight))
        {
            return false;
        }
        CubismRenderer_Software* rr = player.SoftwareRenderer();
        if (rr != NULL)
        {
            if (opt->rasterThreads > 0)
            {
                rr->SetRasterThreads(opt->rasterThreads);
            }
            // 绘制顺序：默认静态（首帧冻结；--order dynamic 恢复官方动态序）
            rr->SetStaticDrawOrder(opt->staticOrder);
            printf("[ui] 绘制顺序：%s\n", opt->staticOrder ? "静态（首帧冻结；--order dynamic 恢复官方动态序）" : "动态（官方语义）");
            fflush(stdout);
        }
        return true;
    };

    if (!loadModelAssets(modelList[(size_t)currentModel]))
    {
        fprintf(stderr, "[l2dtest] 模型加载失败\n");
        CubismFramework::Dispose();
        return 1;
    }

    csmInt32 vertexCount = 0;
    Fnv64VertexChecksum(model, &vertexCount);

    printf("== l2dtest: Live2D Framework 最小播放器（headless）==\n");
    printf("model: %s\n", modelList[(size_t)currentModel].c_str());
    printf("规模: drawable=%d 顶点=%d 参数=%d 动作组=%d 表情=%d\n",
           drawableCount, vertexCount, parameterCount,
           player.Setting()->GetMotionGroupCount(), player.Setting()->GetExpressionCount());
    printf("动作组[0] '%s': %d 个 motion\n", player.FirstMotionGroupName(),
           player.Setting()->GetMotionCount(player.FirstMotionGroupName()));
    WriteCaps(player);   // 能力表写进指令环（宿主 LIST 用）
    printf("管线: 眨眼=%s 呼吸=1 物理=%s 姿势=%s\n",
           player.Setting()->GetEyeBlinkParameterCount() > 0 ? "1" : "0",
           strcmp(player.Setting()->GetPhysicsFileName(), "") != 0 ? "1" : "0",
           strcmp(player.Setting()->GetPoseFileName(), "") != 0 ? "1" : "0");

    // --- 平台端口（多平台：x11/sdl/headless/pet；--port 指定或自动探测）---
    // 注意：--render 纯落帧模式（未显式指定 port）**不探测 platform**——
    // 它只渲染落 PPM、无节流无窗口，是性能测试路径；给了 port 才走平台输出。
    IPort* port = NULL;
    const bool explicitPort = (opt->portRequest != NULL) && (strcmp(opt->portRequest, "auto") != 0);
    const bool wantPort = explicitPort || (opt->renderDir == NULL);
    if (wantPort)
    {
        PortConfig pcfg;
        pcfg.width = opt->renderWidth;
        pcfg.height = opt->renderHeight;
        pcfg.maxFps = opt->targetFps;
        pcfg.device = NULL;

        // config 默认 "auto" = 自动探测
        const char* portName = explicitPort ? opt->portRequest : NULL;
        const char* candidates[5];
        int candidateCount = 0;
        if (portName != NULL)
        {
            candidates[candidateCount++] = portName;
        }
        else
        {
            // 自动探测顺序：X11 → SDL
            // headless 不放这里：它是"无显示"兜底，见下面两段式探测（否则"屏被占用"
            // 会被 headless 悄悄接住 → 变成看不见的宠物空转）
            candidates[candidateCount++] = "x11";
            candidates[candidateCount++] = "sdl";
        }

        bool screenBusy = false;
        for (int i = 0; i < candidateCount && port == NULL; ++i)
        {
            IPort* candidate = CreatePort(candidates[i]);
            if (candidate == NULL)
            {
                if (portName != NULL)
                {
                    fprintf(stderr, "[l2dtest] 未知 port: %s（可用 x11/sdl/headless/pet）\n", candidates[i]);
                    CubismFramework::Dispose();
                    return 1;
                }
                continue;
            }
            if (candidate->Initialize(pcfg))
            {
                port = candidate;
            }
            else
            {
                if (candidate->ScreenBusy())
                {
                    screenBusy = true;
                }
                delete candidate;
            }
        }
        if (port == NULL && !screenBusy && portName == NULL)
        {
            // 无显示端口可用、且并非"屏被占用" → 退回 headless 纯软兜底
            IPort* fallback = CreatePort("headless");
            if (fallback != NULL)
            {
                if (fallback->Initialize(pcfg))
                {
                    port = fallback;
                }
                else
                {
                    delete fallback;
                }
            }
        }
        if (port == NULL)
        {
            if (screenBusy)
            {
                // 屏被其它程序独占（调试时"换程序用屏"）：让位并显式退出；
                // 退出码 3 → 监管脚本判定"不重试"，避免 3 秒重起循环抢屏
                fprintf(stderr, "[l2dtest] 屏幕已被其它程序占用——本程序退出（退出码 3）\n");
                CubismFramework::Dispose();
                return 3;
            }
            fprintf(stderr, "[l2dtest] 无可用的输出端口\n");
            CubismFramework::Dispose();
            return 1;
        }

        // 渲染分辨率 = 输出表面 × scale（降采样时上屏由 port 放大）
        const int screenW = port->Width();
        const int screenH = port->Height();
        opt->renderWidth = ((int)(screenW * opt->renderScale)) & ~1;
        opt->renderHeight = ((int)(screenH * opt->renderScale)) & ~1;
        if (opt->renderWidth < 2) opt->renderWidth = 2;
        if (opt->renderHeight < 2) opt->renderHeight = 2;
        // 本机桌宠：高分屏按物理像素渲染（2× 屏 = 4 倍像素），旧"最多 4 线程"的默认不够用——
        // 没显式 --threads 时用 min(核数, 8)（实测 3200×2000/200%：4 线程 16fps → 8 线程 30fps 封顶）
        if (opt->rasterThreads <= 0 && (strcmp(port->Name(), "pet") == 0 || strcmp(port->Name(), "glfwpet") == 0))
        {
            const int online = plat::CpuCount();
            opt->rasterThreads = online > 8 ? 8 : (online > 0 ? online : 1);
        }

        if (!setupRenderForModel())
        {
            fprintf(stderr, "[l2dtest] 渲染装配失败\n");
            delete port;
            CubismFramework::Dispose();
            return 1;
        }
        printf("输出: port=%s 表面 %dx%d @%dfps | 渲染 %dx%d（scale=%.2f%s）%s\n",
               port->Name(), screenW, screenH, opt->targetFps, opt->renderWidth, opt->renderHeight, opt->renderScale,
               opt->renderWidth != screenW ? "，上屏时 port 放大" : "",
               opt->frameCount <= 0 ? "（无限播放，Ctrl-C 退出）" : "");
    }

    // --- 无 port 但需要渲染（--render 纯落帧模式）---
    const bool renderEnabled = (opt->renderDir != NULL) || (port != NULL);
    if (port == NULL && opt->renderDir != NULL)
    {
        if (!setupRenderForModel())
        {
            fprintf(stderr, "[l2dtest] 渲染装配失败\n");
            CubismFramework::Dispose();
            return 1;
        }
    }

    if (renderEnabled && opt->renderDir != NULL)
    {
        printf("渲染: %dx%d → %s/（每 %d 帧存一帧 PPM）\n",
               opt->renderWidth, opt->renderHeight, opt->renderDir, opt->renderEvery);
    }

    // （参数快照已由 refreshModelState 在加载/切换时建立）
    if (opt->frameCount > 0)
    {
        player.Update(opt->dt); // 预热一帧，让 motion 跑起来
    }

    // --- 主循环 ---
    int distinctFnv = 0;
    csmUint64 lastFnv = 0;
    csmFloat32 eyeOpenMin = 1.0f;
    csmFloat32 breathMin = 1.0f, breathMax = 0.0f;
    bool hasEyeBlinkParam = false;
    bool hasBreathParam = false;
    for (csmInt32 i = 0; i < parameterCount; ++i)
    {
        if (paramIds[i] == "ParamEyeLOpen") hasEyeBlinkParam = true;
        if (paramIds[i] == "ParamBreath") hasBreathParam = true;
    }
    // 端口模式：按 opt->targetFps 推导 opt->dt 并做绝对时基节流
    if (port != NULL && opt->targetFps > 0)
    {
        opt->dt = 1.0f / (csmFloat32)opt->targetFps;
    }

    struct timespec ts0, ts1;
    clock_gettime(CLOCK_MONOTONIC, &ts0);
    struct timespec tPrev = ts0;
    const bool realtimeDt = (port != NULL && getenv("L2D_FIXED_DT") == NULL);
    struct timespec tUpd = ts0;

    // 本机协议端口（--ctl）：桌宠 / LLM 用（PROTOCOL-LLM.md）
    CtlServer* ctl = NULL;
    player.SetViewSize(opt->renderWidth, opt->renderHeight);
    player.SetPort(port);   // 气泡输出到输出端口（NULL=纯渲染，只动口型）
    player.SetModelLibrary(modelList, currentModel);
    float petAwayT = 0.0f;  // 桌宠：鼠标离开人物多久了（-1 = 已放开视线）
    const bool isPetPort = (port != NULL && (strcmp(port->Name(), "pet") == 0
                                             || strcmp(port->Name(), "glfwpet") == 0));
    if (!opt->ctlExplicit)
    {
        player.CfgLoad();   // Model loading reconstructs the player; reload configuration before binding.
        const float savedValue = player.Cfg("ctl_port");
        const int saved = std::isfinite(savedValue) && savedValue >= 1024 && savedValue <= 65535 && savedValue == std::floor(savedValue) ? (int)savedValue : 23333;
        opt->ctlPort = saved >= 1024 && saved <= 65535 ? saved : 23333;
        const char* envPort = getenv("L2DPET_PORT");
        if (envPort && *envPort)
        {
            char* end = NULL;
            const long value = strtol(envPort, &end, 10);
            if (*end || value < 1 || value > 65535)
            {
                fprintf(stderr, "[ctl] invalid L2DPET_PORT\n");
                return 2;
            }
            opt->ctlPort = (int)value;
        }
    }
    if (opt->ctlPort > 0)
    {
        ctl = new CtlServer();
        if (!ctl->Start(opt->ctlPort))
        {
            // 端口被占（多半是已有一只桌宠在跑）：直接退出。否则这只没有控制端口——
            // 托盘/菜单的"退出"走 23333 发不进去，会变成一只"关不掉的桌宠"。
            fprintf(stderr, "[ctl] --ctl %d 绑定失败（已有桌宠在跑？），退出。\n", opt->ctlPort);
            fflush(stderr);
            delete ctl;
            return 1;
        }
        else
        {
            char pv[16];
            snprintf(pv, sizeof(pv), "%d", opt->ctlPort);
            player._integration.activePort = opt->ctlPort;
            player.PanelStateJson(0); // Publish discovery before any assistant connects.
            plat::SetEnv("L2DPET_PORT", pv);   // 子进程（l2dpet-ui）继承：连这个端口
            // 内置 agent：桌宠自带 LLM 大脑（线程内调 LlmClient；命令经 g_builtinAgent
            // 队列回到主循环执行 PetCommand——与 TCP/面板同一入口）。L2D_NO_AGENT=1 关闭。
            if (isPetPort && getenv("L2D_NO_AGENT") == NULL)
            {
                g_builtinAgent = new PetAgent();
                g_builtinAgent->Start();
            }
        }
    }

    int executedFrames = 0;
    double updateSec = 0.0;
    double renderSec = 0.0;
    for (int frame = 0; !g_quit && (opt->frameCount <= 0 || frame < opt->frameCount); ++frame)
    {
        ++executedFrames;

        DrainCmdRing(player);   // 宿主指令（表情/动作/手势/脚本）——在 Update 前消费

        // --- 底部 ◀/▶ 箭头请求：运行时切换模型 ---
        csmInt32 switchReq = player.TakeModelSwitchRequest();
        // --- 模型页导入：重扫模型库（并在需要时切到刚导入的模型）---
        // 另外每 ~2s 温和重扫一次：用户直接往模型库拷/删目录也能被发现（面板列表不再留死条目）
        {
            static double libScanAt = -1e9;
            {
                const double libNow = (double)(tPrev.tv_sec - ts0.tv_sec)
                                      + (double)(tPrev.tv_nsec - ts0.tv_nsec) * 1e-9;
                if (isPetPort && libNow - libScanAt > 2.0)
                {
                    libScanAt = libNow;
                    std::vector<std::string> lib;
                    ScanModelDir(L2DPlayer::ModelsLibDir().c_str(), &lib);
                    std::vector<std::string> merged;
                    ScanModelDir(scanDir.c_str(), &merged);
                    for (size_t i = 0; i < lib.size(); ++i)
                    {
                        const std::string rp = RealPath(lib[i].c_str());
                        bool dup = false;
                        for (size_t k = 0; k < merged.size() && !dup; ++k) dup = RealPath(merged[k].c_str()) == rp;
                        if (!dup) merged.push_back(lib[i]);
                    }
                    if (merged.size() != modelList.size())
                    {
                        player.RequestModelRescanRefresh();
                    }
                    else
                    {
                        bool same = true;
                        for (size_t i = 0; i < merged.size() && same; ++i) same = merged[i] == modelList[i];
                        if (!same) player.RequestModelRescanRefresh();
                    }
                }
            }
            std::string wantName;
            if (player.TakeModelRescan(&wantName))
            {
                std::vector<std::string> list;
                ScanModelDir(scanDir.c_str(), &list);
                if (isPetPort)
                {
                    std::vector<std::string> lib;
                    ScanModelDir(L2DPlayer::ModelsLibDir().c_str(), &lib);
                    for (size_t i = 0; i < lib.size(); ++i)
                    {
                        const std::string rp = RealPath(lib[i].c_str());
                        bool dup = false;
                        for (size_t k = 0; k < list.size() && !dup; ++k) dup = RealPath(list[k].c_str()) == rp;
                        if (!dup) list.push_back(lib[i]);
                    }
                }
                if (!list.empty())
                {
                    // 重扫会改变排序/下标：先按"当前加载的模型路径"找回它的新下标，
                    // 否则下标会指向别的模型（面板高亮错位、切换判定也会被跳过）
                    const std::string curPath = (currentModel >= 0 && currentModel < (int)modelList.size())
                                                ? RealPath(modelList[(size_t)currentModel].c_str()) : std::string();
                    modelList = list;
                    currentModel = 0;
                    for (size_t i = 0; i < modelList.size(); ++i)
                    {
                        if (!curPath.empty() && RealPath(modelList[i].c_str()) == curPath)
                        {
                            currentModel = (int)i;
                            break;
                        }
                    }
                    player.SetModelLibrary(modelList, currentModel);
                    printf("[ui] 模型库重扫：%d 个（%s）\n", (int)modelList.size(), wantName.empty() ? "导入后刷新" : wantName.c_str());
                    fflush(stdout);
                    if (!wantName.empty() && BaseName(DirName(modelList[(size_t)currentModel])) != wantName)
                    {
                        for (size_t i = 0; i < modelList.size(); ++i)
                        {
                            if (BaseName(DirName(modelList[i])) == wantName)
                            {
                                player.RequestModelSwitchTo((int)i);
                                break;
                            }
                        }
                    }
                }
            }
        }
        const int switchTo = player.TakeModelSwitchTo();   // 桌宠托盘/菜单/协议：按序号切
        if (switchTo >= 0 && switchTo < (int)modelList.size() && switchTo != currentModel)
        {
            switchReq = switchTo - currentModel;
        }
        const bool reloadReq = player.TakeModelReload();   // 探测完成后强制重载（学到的部件→臂骨架/分区生效）
        if (reloadReq || (switchReq != 0 && modelList.size() > 1))
        {
            const int prevModel = currentModel;   // 新模型加载失败时退回它
            if (switchReq != 0)
            {
                currentModel = (currentModel + switchReq + (int)modelList.size()) % (int)modelList.size();
            }
            printf("[ui] 加载模型 %d/%d: %s%s\n", currentModel + 1, (int)modelList.size(),
                   BaseName(DirName(modelList[(size_t)currentModel])).c_str(), reloadReq ? "（重载）" : "");
            fflush(stdout);
            if (loadModelAssets(modelList[(size_t)currentModel]) && setupRenderForModel())
            {
                Fnv64VertexChecksum(model, &vertexCount);
                printf("规模: drawable=%d 顶点=%d 参数=%d 动作组=%d 表情=%d\n",
                       drawableCount, vertexCount, parameterCount,
                       player.Setting()->GetMotionGroupCount(),
                       player.Setting()->GetExpressionCount());
                fflush(stdout);
                WriteCaps(player);   // 切模型后刷新能力表（表情/动作/手势都变了）
                // 记下选中的模型：下次启动直接回到它（见上面「按配置选中上次的模型」）
                player.CfgStrSet("model", BaseName(DirName(modelList[(size_t)currentModel])));
                player.CfgSave();
                // 播放器是原地重建的：桌宠接线（输出端口/视口/模型库）要补回；通知订阅者重新 query
                player.SetViewSize(opt->renderWidth, opt->renderHeight);
                player.SetPort(port);
                player.SetModelLibrary(modelList, currentModel);
                player.PushPetEvent("{\"event\":\"model_changed\",\"model\":\"" + player.ModelFv() + "\",\"name\":\""
                                    + BaseName(DirName(modelList[(size_t)currentModel])) + "\"}");
            }
            else
            {
                // 切换时旧播放器已经析构重建：不能"保持当前显示"（model 指针已悬空 → 下一帧 use-after-free）。
                // 退回原模型重新加载；连原模型都加载不了就干净退出，绝不带着悬空指针跑
                fprintf(stderr, "[ui] 模型加载失败：%s —— 退回原模型\n", modelList[(size_t)currentModel].c_str());
                currentModel = prevModel;
                if (loadModelAssets(modelList[(size_t)currentModel]) && setupRenderForModel())
                {
                    player.SetViewSize(opt->renderWidth, opt->renderHeight);
                    player.SetPort(port);
                    player.SetModelLibrary(modelList, currentModel);
                    player.PushPetEvent("{\"event\":\"model_load_failed\"}");
                }
                else
                {
                    fprintf(stderr, "[ui] 原模型也加载不了，退出\n");
                    break;
                }
                fflush(stdout);
            }
        }

        // 能力探测队列（非阻塞）：在正式解算前跑一点——探测的中间姿态会被本帧
        // 解算覆盖，屏幕不受影响；物理档会给物理链留过渡状态，期间轻微晃动属正常
        player.ProbeTick();

        // 气泡点选必须先于本帧解算消费：PetTick（在 Update 里）换气泡时会重写 _bubbleOptions，
        // 排在它后面会把"刚点的选项"错算到下一条气泡头上——提问到期后还有 0.22s 淡出期可点中，
        // 队列里正好有排队气泡时就会踩到。端口的鼠标事件在上一帧 Present 里已经收好（glfwPollEvents），
        // 这里取到的就是该次点击。
        if (isPetPort && port != NULL)
        {
            int choiceIdx = 0;
            while (port->PollChoice(&choiceIdx))
            {
                player.PetChoiceMade(choiceIdx);
            }
        }

        if (frame > 0)
        {
            struct timespec ta, tb;
            const bool profOn = L2DPlayer::UiProfOn();
            if (profOn) { clock_gettime(CLOCK_MONOTONIC, &ta); }
            // 实时口径（有输出端口 = 窗口跑）：opt->dt = 实测帧间隔——重模型 7fps 时动作/
            // 呼吸/演出也按墙钟走（原来固定 1/30 → 舒芙蕾动画只有 0.23× 速）。
            // 钳到 ≤0.2s（切模型/卡顿后不让物理一步跨太大）；物理由 SDK 按 physics3 的
            // Fps 分步，弹簧也自行分步。离线渲染/对拍（无端口）保持固定 opt->dt、逐位可复现；
            // L2D_FIXED_DT=1 强制固定口径。
            if (realtimeDt)
            {
                struct timespec now;

                clock_gettime(CLOCK_MONOTONIC, &now);
                csmFloat32 wall = (csmFloat32)((double)(now.tv_sec - tUpd.tv_sec)
                                               + (double)(now.tv_nsec - tUpd.tv_nsec) * 1e-9);
                tUpd = now;
                if (wall < 0.005f) wall = 0.005f;
                if (wall > 0.2f) wall = 0.2f;
                opt->dt = wall;
            }
            player.Update(opt->dt);
            if (profOn)
            {
                clock_gettime(CLOCK_MONOTONIC, &tb);
                updateSec += (double)(tb.tv_sec - ta.tv_sec) + (double)(tb.tv_nsec - ta.tv_nsec) * 1e-9;
            }
        }

        // 输入事件：port 表面坐标 → 渲染目标坐标（scale<1 时两者不同）
        if (port != NULL)
        {
            int tx = 0;
            int ty = 0;
            bool tdown = false;
            while (port->PollInput(&tx, &ty, &tdown))
            {
                const int rx = port->Width() > 0 ? tx * opt->renderWidth / port->Width() : tx;
                const int ry = port->Height() > 0 ? ty * opt->renderHeight / port->Height() : ty;
                player.HandleTouch(rx, ry, tdown);
            }

            // 宿主/端口给的注视目标：只更新跟随目标（不参与点击/缩放条
            // 判定——那是本地输入的事）；速度平滑仍由调度器里的 CubismTargetPoint 完成。
            // 桌宠顺带做指针手势（hover 驻留 / rub 左右搓）：同一份鼠标位置喂给区域分类器。
            int lx = 0;
            int ly = 0;
            bool hasPointer = port->PollLook(&lx, &ly);
            const int prx = port->Width() > 0 ? lx * opt->renderWidth / port->Width() : lx;
            const int pry = port->Height() > 0 ? ly * opt->renderHeight / port->Height() : ly;

            // 桌宠：鼠标只在人物附近（包围盒外扩 40px）才算"在逗它"——跟随视线 + 手势判定；
            // 离开 1 秒后放开视线，交回自主待机（东张西望/小动作），不再一直盯着鼠标
            const bool rawPointer = hasPointer;

            if (hasPointer && isPetPort && !player.PointNearModel((csmFloat32)prx, (csmFloat32)pry, 40.0f * player.PxK()))
            {
                hasPointer = false;
            }
            if (isPetPort)
            {
                player.PetLifeTick(opt->dt, rawPointer, (csmFloat32)prx, (csmFloat32)pry, hasPointer);
            }
            if (hasPointer)
            {
                petAwayT = 0.0f;
                if (!player.LookOverridden())
                {
                    player.SetLookTargetPixels((csmFloat32)prx, (csmFloat32)pry);
                }
                if (isPetPort)
                {
                    player.PetPointerTick((csmFloat32)prx, (csmFloat32)pry, true, opt->dt);
                }
            }
            else if (isPetPort)
            {
                player.PetPointerTick(0.0f, 0.0f, false, opt->dt);
                if (petAwayT >= 0.0f)
                {
                    petAwayT += opt->dt;
                    if (petAwayT >= 1.0f && !player.LookOverridden())
                    {
                        player.ReleaseLook();
                        petAwayT = -1.0f;   // 已放开；鼠标回来前不再重复
                    }
                }
            }

            // 桌宠：单击角色 = 点击反应 + 连击计数（click 事件防抖后发）；滚轮 = 缩放
            //（气泡选项 = choice 事件，在帧首 Update 之前消费，见上面那段）
            int cx = 0;
            int cy = 0;
            while (port->PollClick(&cx, &cy))
            {
                const int rx = port->Width() > 0 ? cx * opt->renderWidth / port->Width() : cx;
                const int ry = port->Height() > 0 ? cy * opt->renderHeight / port->Height() : cy;
                const char* zoneKey = NULL;

                player.HandleTapZ(rx, ry, &zoneKey);
                if (isPetPort)
                {
                    player.NotePetClick(zoneKey);
                }
            }
            int wheel = 0;
            if (port->PollWheel(&wheel))
            {
                if (isPetPort)
                {
                    // 桌宠：缩放**整个视窗**（窗口跟 z 变大小、模型不裁剪；渲染保持满幅）
                    // 锁定大小和位置时忽略滚轮（防误缩放；面板滑条仍可改）
                    if (player.Cfg("lock") > 0.5f)
                    {
                        wheel = 0;
                    }
                    if (wheel != 0)
                    {
                        csmFloat32 z = player.UiZoom();

                        z = z > 0.0f ? z : 1.0f;
                        z *= powf(1.1f, (csmFloat32)wheel);
                        z = z < 0.5f ? 0.5f : (z > 2.0f ? 2.0f : z);
                        port->SetZoom(z);
                        player.NoteZoom(z);   // 存进配置（下次启动保持）
                    }
                }
                else
                {
                    csmFloat32 z = player.RenderZoom() > 0.0f ? player.RenderZoom() : 1.0f;

                    z *= powf(1.1f, (csmFloat32)wheel);
                    z = z < 0.3f ? 0.3f : (z > 3.0f ? 3.0f : z);
                    player.SetRenderZoom(z);
                }
            }
        }

        // 协议端口：请求在主循环里执行（播放器状态单线程访问）；事件推给订阅者
        // 内置 agent 的协议命令：主线程执行（PetCommand 与 TCP/面板同一入口）后唤醒它
        if (g_builtinAgent != NULL)
        {
            PetAgent::RpcPtr rpc;
            while (g_builtinAgent->PopPending(&rpc))
            {
                bool subDummy = false;
                rpc->reply = player.PetCommand(rpc->line, (csmFloat32)player.MeasuredFps(), &subDummy);
                g_builtinAgent->FinishPending(rpc);
            }
        }

        if (ctl != NULL)
        {
            int conn = 0;
            std::string line;

            while (ctl->Poll(&conn, &line))
            {
                bool sub = false;
                const std::string reply = player.PetCommand(line, (csmFloat32)player.MeasuredFps(), &sub);

                if (sub)
                {
                    ctl->Subscribe(conn);
                    if (line.find("\"role\":\"ui\"") != std::string::npos)
                    {
                        ctl->MarkUi(conn);   // UI 进程的订阅：不算 LLM agent
                    }
                }
                ctl->Reply(conn, reply);
            }
            // 桌宠面板：状态下发（2Hz）+ 面板操作（同一套协议命令）
            if (isPetPort && port != NULL)
            {
                static double panelAt = 0.0;
                struct timespec pts;
                clock_gettime(CLOCK_MONOTONIC, &pts);
                const double pnow = (double)pts.tv_sec + (double)pts.tv_nsec * 1e-9;

                if (pnow - panelAt > 0.5)
                {
                    panelAt = pnow;
                    // 内置 agent 不走 TCP 订阅（进程内直调）——面板/status 的"LLM 已连接"要把
                    // 它算进去，否则一直显示"未连接"
                    int agentCount = ctl->SubscriberCount();
                    if (g_builtinAgent != NULL && g_builtinAgent->Running()) ++agentCount;
                    player.SetAgentCount(agentCount);
                    port->SetPanelState(player.PanelStateJson(agentCount));
                }
                std::string pline;
                while (port->PollPanelCommand(&pline))
                {
                    bool psub = false;
                    const std::string prep = player.PetCommand(pline, (csmFloat32)player.MeasuredFps(), &psub);

                    printf("[panel] %s → %s\n",
                           pline.find("api_key") != std::string::npos ? "{\"cmd\":\"llm_config\",…（密钥不打日志）}" : pline.c_str(),
                           prep.substr(0, 160).c_str());
                    fflush(stdout);
                    panelAt = 0.0;   // 操作后立刻刷新面板
                }
            }
            std::string uiev;
            while (port != NULL && port->PollUiEvent(&uiev))
            {
                ctl->Broadcast(uiev);   // 菜单/气泡/打开设置 → UI 进程
            }
            const std::vector<std::string> evs = player.TakePetEvents();

            for (size_t e = 0; e < evs.size(); ++e)
            {
                ctl->Broadcast(evs[e]);
                if (g_builtinAgent != NULL) g_builtinAgent->PushEvent(evs[e]);
            }
        }

        // --look：固定注视目标（对拍/调试；每帧强制，port 为 NULL 的纯 --render 跑法也生效）
        if (opt->hasLook)
        {
            player.SetLookTargetPixels((csmFloat32)opt->lookX, (csmFloat32)opt->lookY);
        }

        if (renderEnabled && !(port != NULL && port->Hidden()))   // 桌宠隐藏到托盘：不渲染（解算照跑，状态/情绪继续）
        {
            struct timespec ta, tb;
            const bool profOn = L2DPlayer::UiProfOn();
            if (profOn) { clock_gettime(CLOCK_MONOTONIC, &ta); }
            player.Render();
            if (profOn)
            {
                clock_gettime(CLOCK_MONOTONIC, &tb);
                renderSec += (double)(tb.tv_sec - ta.tv_sec) + (double)(tb.tv_nsec - ta.tv_nsec) * 1e-9;
            }
            if (opt->renderDir != NULL && frame % opt->renderEvery == 0)
            {
                CubismRenderer_Software* renderer = player.SoftwareRenderer();
                if (renderer != NULL)
                {
                    char path[512];
                    snprintf(path, sizeof(path), "%s/frame_%04d.ppm", opt->renderDir, frame);
                    WritePpm(path, renderer->GetFramebuffer(),
                             (int)renderer->GetFramebufferWidth(),
                             (int)renderer->GetFramebufferHeight());
                }
            }
            if (port != NULL)
            {
                CubismRenderer_Software* renderer = player.SoftwareRenderer();
                if (renderer != NULL)
                {
                    // 尺寸相等时 port 内部直接拷贝；小于表面时 port 自行放大
                    port->Present(renderer->GetFramebuffer(),
                                  (int)renderer->GetFramebufferWidth(),
                                  (int)renderer->GetFramebufferHeight());
                }
                port->WaitFrame(); // 帧率节流（maxFps 由 PortConfig 配置）
                if (opt->shotPath != NULL && frame == opt->shotFrame)
                {
                    if (port->SaveScreenshot(opt->shotPath))
                    {
                        printf("[shot] 屏幕内容已存 %s（frame %d）\n", opt->shotPath, frame);
                    }
                }
                if (port->ShouldQuit())
                {
                    printf("[port] 窗口关闭，退出\n");
                    break;
                }
            }
        }

        const csmUint64 fnv = Fnv64VertexChecksum(model, &vertexCount);
        if (frame == 0 || fnv != lastFnv)
        {
            ++distinctFnv;
        }
        lastFnv = fnv;

        // 活性指标
        for (csmInt32 i = 0; i < parameterCount; ++i)
        {
            const csmFloat32 v = model->GetParameterValue(i);
            if (paramIds[i] == "ParamEyeLOpen" && v < eyeOpenMin)
            {
                eyeOpenMin = v;
            }
            if (paramIds[i] == "ParamBreath")
            {
                if (v < breathMin) breathMin = v;
                if (v > breathMax) breathMax = v;
            }
        }

        // 全量参数导出（诊断：不透明度绑定/物理输出这类"非几何"状态靠它定位）
        if (frame == opt->dumpParamsFrame)
        {
            for (csmInt32 i = 0; i < parameterCount; ++i)
            {
                printf("[pd] %s %.6f\n", paramIds[i].c_str(), model->GetParameterValue(i));
            }
        }

        // 全量顶点导出（跨平台逐点比对用）
        if (frame == opt->dumpVertsFrame)
        {
            for (csmInt32 d = 0; d < drawableCount; ++d)
            {
                const csmInt32 vertexCount = model->GetDrawableVertexCount(d);
                const Live2D::Cubism::Core::csmVector2* positions = model->GetDrawableVertexPositions(d);
                for (csmInt32 v = 0; v < vertexCount; ++v)
                {
                    printf("[vd] %d %d %.9f %.9f\n", d, v, positions[v].X, positions[v].Y);
                }
            }
        }

        // 逐帧证据
        if (frame % opt->reportEvery == 0)
        {
            printf("[f %3d] t=%.3f fnv=0x%016llx | 变化:", frame, frame * opt->dt, (unsigned long long)fnv);
            int changed = 0;
            for (csmInt32 i = 0; i < parameterCount && changed < 8; ++i)
            {
                const csmFloat32 v = model->GetParameterValue(i);
                const csmFloat32 d = v - baseline[i];
                if (d > 1e-4f || d < -1e-4f)
                {
                    printf(" %s=%.4f", paramIds[i].c_str(), v);
                    ++changed;
                }
            }
            if (changed == 0)
            {
                printf(" (无)");
            }
            printf("\n");
            PrintSampleVertices(model);
        }

        // 真实帧间隔（右上角 FPS 显示用）
        {
            struct timespec tn;
            clock_gettime(CLOCK_MONOTONIC, &tn);
            const csmFloat32 realDt = (csmFloat32)((tn.tv_sec - tPrev.tv_sec)
                                                   + (tn.tv_nsec - tPrev.tv_nsec) / 1e9);
            player.NoteFrameTime(realDt);
            tPrev = tn;
        }
    }

    clock_gettime(CLOCK_MONOTONIC, &ts1);
    const double elapsed = (ts1.tv_sec - ts0.tv_sec) + (ts1.tv_nsec - ts0.tv_nsec) / 1e9;

    printf("--- 汇总 ---\n");
    printf("模式=%s 帧数=%d opt->dt=%.4f 耗时=%.3fs (%.2f ms/帧)%s\n",
           opt->noMotion ? "无动作(眨眼+呼吸)" : "动作播放", executedFrames, opt->dt, elapsed,
           executedFrames > 0 ? elapsed * 1000.0 / executedFrames : 0.0,
           g_quit ? "（收到退出信号，优雅停止）" : "");
    printf("顶点在动: FNV 不同值 %d/%d 帧\n", distinctFnv, executedFrames);
    if (!hasEyeBlinkParam)
    {
        printf("眨眼活性: (模型无 ParamEyeLOpen 参数)\n");
    }
    else if (eyeOpenMin < 0.5f)
    {
        printf("眨眼活性: 有闭合 (min=%.4f)\n", eyeOpenMin);
    }
    else
    {
        printf("眨眼活性: 未见闭合（动作播放期间官方管线暂停眨眼，可加 --no-motion 复验）\n");
    }

    if (!hasBreathParam)
    {
        printf("呼吸活性: (模型无 ParamBreath 参数——呼吸作用于 Angle 参数)\n");
    }
    else if ((breathMax - breathMin) > 0.01f)
    {
        printf("呼吸活性: ParamBreath %.4f→%.4f (有起伏)\n", breathMin, breathMax);
    }
    else
    {
        printf("呼吸活性: 未见起伏 (ParamBreath 恒定)\n");
    }
    printf("motion 播放中=%s\n", player.IsPlayingMotion() ? "是" : "否");
    printf("本轮合计: update=%.2fms/帧 render=%.2fms/帧（帧数=%d）\n",
           executedFrames > 0 ? updateSec * 1000.0 / executedFrames : 0.0,
           executedFrames > 0 ? renderSec * 1000.0 / executedFrames : 0.0,
           executedFrames);
    if (renderEnabled)
    {
        CubismRenderer_Software* renderer = player.SoftwareRenderer();
        if (renderer != NULL)
        {
            printf("渲染统计: 可见 drawable=%u 三角形/帧=%u 扫描像素/帧=%llu 告警=%u\n",
                   renderer->GetLastVisibleDrawableCount(),
                   renderer->GetLastRasterTriangleCount(),
                   (unsigned long long)renderer->GetLastScanPixelCount(),
                   renderer->GetLastMaskWarnCount());
            renderer->PrintProfile();
        }
        L2DPlayer::PrintUiStats();
    }

    // --- 清理 ---
    if (g_builtinAgent != NULL)
    {
        g_builtinAgent->Stop();
        delete g_builtinAgent;
        g_builtinAgent = NULL;
    }
    delete port;
    player.DeleteRenderer();
    CubismFramework::Dispose();

    return 0;
}

int RunMainLoop(PetOptions& opt)
{
    PetRuntime rt;
    rt.opt = &opt;
    return rt.Run();
}

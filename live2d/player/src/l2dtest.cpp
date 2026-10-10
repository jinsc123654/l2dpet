/**
 * l2dtest.cpp — Live2D Cubism Framework 最小播放器（headless，无窗口）
 *
 * 目的（L-C ①）：验证 Framework 能加载完整模型资产（model3.json / moc3 /
 * motion3.json / exp3.json / physics3.json / pose3.json），并按官方更新管线
 * （motion → 眨眼/呼吸/表情/物理 → Core 顶点变形）驱动模型。
 *
 * 与 moc3test.c 的关系：moc3test 只验证 Core（模型解析 + 变形链）；本程序验证
 * Framework 层（资产解析 + 播放管线），是 L-C ②（软渲染器）的宿主骨架。
 *
 * 用法：l2dtest <模型目录>/<X.model3.json> [帧数=180] [dt=1/30] [--no-motion] [--every=N]
 *              [--dump-verts=N] [--size WxH] [--render <目录>] [--render-every=N]
 *   --no-motion：不播动作组，让眨眼/呼吸独立驱动参数（官方管线中动作播放
 *                期间会暂停眨眼，循环动作下眨眼永远不触发——此开关用于
 *                验证眨眼/呼吸子系统本身）。
 *   --every=N  ：证据打印间隔（默认 30；=1 输出全部帧，供双平台 diff）
 *   --dump-verts=N：把第 N 帧的全部顶点以 %.9f 全精度导出（[vd] 行），供跨平台逐点比对
 *   --size WxH ：渲染目标尺寸（默认 480x800）
 *   --render d ：启用软件光栅化（L-C ②），每 --render-every 帧把画面存为 PPM 到 d/
 *   --render-every=N：存帧间隔（默认 30）
 *   --fps=N    ：帧率上限（输出端口节流，默认 30）
 * 输出：逐帧参数/顶点证据（FNV 校验和 + 定点采样值，可跨平台比对）
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdarg>
#include <deque>
#include <string>
#include <map>
#include <vector>
#include <algorithm>
#include <set>
#include <ctime>
#include <time.h>
#include <csignal>
#include <limits.h>
#include <cerrno>
#include <fstream>
#if !defined(_WIN32)
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/mman.h>
#include <sys/stat.h>
#if defined(__GLIBC__)
#include <malloc.h>   // malloc_trim（glibc 专属）
#endif
#else
// MSVC 的重入分词器叫 strtok_s（与 strtok_r 同签名，故直接别名）
#define strtok_r strtok_s
#include <malloc.h>   // _aligned_malloc / _aligned_free（L2DAllocator 的对齐分配）
#include <mbctype.h>  // _setmbcp / _MB_CP_UTF8
#endif

#include <CubismFramework.hpp>
#include <CubismDefaultParameterId.hpp>
#include <CubismModelSettingJson.hpp>
#include <ICubismModelSetting.hpp>
#include <Id/CubismIdManager.hpp>
#include <Model/CubismUserModel.hpp>
#include <Model/CubismModel.hpp>
#include <Motion/CubismMotion.hpp>
#include <Motion/CubismMotionManager.hpp>
#include <Motion/CubismExpressionMotionManager.hpp>
#include <Motion/ACubismMotion.hpp>
#include <Motion/CubismBreathUpdater.hpp>
#include <Motion/CubismEyeBlinkUpdater.hpp>
#include <Motion/CubismExpressionUpdater.hpp>
#include <Motion/CubismPhysicsUpdater.hpp>
#include <Motion/CubismPoseUpdater.hpp>
#include <Motion/CubismLookUpdater.hpp>
#include <Effect/CubismBreath.hpp>
#include <Effect/CubismEyeBlink.hpp>
#include <Effect/CubismPose.hpp>
#include <Effect/CubismLook.hpp>
#include <Physics/CubismPhysics.hpp>
#include <Math/CubismMatrix44.hpp>
#include <Math/CubismModelMatrix.hpp>
#include <Rendering/CubismRenderer.hpp>
#include <Utils/CubismString.hpp>
#include <Type/csmString.hpp>

// 软渲染后端（构建脚本注入到 Framework/src/Rendering/Software，见 player/CMakeLists）
#include "CubismRenderer_Software.hpp"
#include "SoftTexture.hpp"

// 平台端口抽象（多平台：x11/sdl/pet/glfwpet/headless；见 Port.hpp）
#include "Port.hpp"

// 指令环契约（宿主 → 播放器：表情/动作/手势/演出脚本）
#include "L2dCmd.h"

// 小 JSON 解析器（profile 与演出脚本用；SDK 自带的 CubismJson 对手写/紧凑格式有 bug）
#include "JsonLite.h"
#include "CtlServer.hpp"   // 本机桌宠的协议端口（PROTOCOL-LLM.md）
#include "Platform.h"      // 跨平台小工具层（时间/路径/目录/子进程）

#include "SysStats.h"       // 系统状态采样（拆出）
#include "PlayerAllocator.h"  // Cubism 分配器（拆出）
#include "FileUtil.h"         // 文件/路径工具（拆出）
#include "ModelImport.h"      // 模型导入（拆出）
#include "Backup.h"           // 备份导出/导入（拆出）
#include "PpmPlayer.h"        // PPM 帧序列上屏（拆出）
#include "PetI18n.h"
#include "PetVerbs.h"
#include "PetAgent.h"      // 内置 agent（LLM 大脑；L2D_NO_AGENT=1 关闭）

// 统一配置（Port 平台层 + 软件配置全集中于此）
#include "l2d_config.h"

using namespace Live2D::Cubism::Framework;
using Rendering::CubismRenderer_Software;
using Rendering::SoftTexture;

// ---------------------------------------------------------------- 参数工具

/**
 * 统一取选项值：同时支持 "--name=value" 与 "--name value" 两种写法。
 * 匹配时返回 true；"--name value" 形式下推进循环下标 i 越过 value。
 */

// ---------------------------------------------------------------- 信号

// SIGTERM/SIGINT → 主循环优雅退出（rc=0；供 run.sh 监管语义：正常退出不重启）
volatile sig_atomic_t g_quit = 0;
PetAgent* g_builtinAgent = NULL;   // 内置 agent（主循环喂事件/执行它的协议命令）

/**
 * 关掉播放器自带的全部 UI（状态栏/缩放条/箭头/圆环）。
 *
 * 为什么要有：UI 会污染"纯角色帧"——
 * **实测教训**：状态栏的 CPU/RAM/FPS 读数每跑一次都不一样，于是"同一模型同一帧"
 * 的 PPM 逐字节基线永远对不上（一度被误判成渲染器不确定）。
 * --no-ui 显式关闭（宿主是别的图形栈时用）；--pet 也置本开关（帧里只留角色）。
 */
bool g_uiOff = false;

void HandleQuitSignal(int)
{
    g_quit = 1;
}

// ------------------------------------------------------ 播放器（Framework 宿主）

// 指令环回复：写一条单行结果到 L2dCmd.h 的 reply 区（定义在文件后段）
void SetReply(const char* fmt, ...);

// 能力表/参数表重建（写指令环 caps/params/data 区；定义在文件后段）
class L2DPlayer;
void WriteCaps(L2DPlayer& player);

#include "L2DPlayer.h"   // 播放器主体（从本文件拆出；实现见 Player*.cpp）

// ---------------------------------------------------------------- 证据输出

csmUint64 Fnv64VertexChecksum(const CubismModel* model, csmInt32* outVertexCount)
{
    csmUint64 hash = 1469598103934665603ULL; // FNV-1a offset basis
    csmInt32 total = 0;

    const csmInt32 drawableCount = model->GetDrawableCount();
    for (csmInt32 d = 0; d < drawableCount; ++d)
    {
        const csmInt32 vertexCount = model->GetDrawableVertexCount(d);
        const Live2D::Cubism::Core::csmVector2* positions = model->GetDrawableVertexPositions(d);
        total += vertexCount;

        for (csmInt32 v = 0; v < vertexCount; ++v)
        {
            csmUint32 bits[2];
            memcpy(&bits[0], &positions[v].X, 4);
            memcpy(&bits[1], &positions[v].Y, 4);
            hash ^= bits[0];
            hash *= 1099511628211ULL;
            hash ^= bits[1];
            hash *= 1099511628211ULL;
        }
    }
    *outVertexCount = total;
    return hash;
}

void PrintSampleVertices(const CubismModel* model)
{
    const csmInt32 drawableCount = model->GetDrawableCount();
    for (csmInt32 d = 0; d < drawableCount; ++d)
    {
        const csmInt32 vertexCount = model->GetDrawableVertexCount(d);
        if (vertexCount <= 0)
        {
            continue;
        }
        const Live2D::Cubism::Core::csmVector2* positions = model->GetDrawableVertexPositions(d);
        const csmInt32 sampleCount = vertexCount < 3 ? vertexCount : 3;
        printf("[v 采样] drawable %d:", d);
        for (csmInt32 v = 0; v < sampleCount; ++v)
        {
            printf(" (%.6f,%.6f)", positions[v].X, positions[v].Y);
        }
        printf("\n");
        return;
    }
}

// ---------------------------------------------------------------- 指令环（宿主 → 本进程）
// 契约见 L2dCmd.h：SPSC 文本环，一行一条指令（表情/动作/手势/脚本 JSON/复位）。
// 解析与执行都只在本进程（宿主保持"哑"）；caps 区由本进程装载模型后写入。

l2d_cmd_ring_t* g_cmdRing = NULL;   // --cmd-fd 映射（未给则为 NULL）

/** 写一条单行回复到指令环 reply 区（先写文本、后递增 replySeq（release）——
 *  宿主按 "读 seq → 读文本 → 再读 seq" 判稳定，见 L2dCmd.h）*/
void SetReply(const char* fmt, ...)
{
    if (g_cmdRing == NULL)
    {
        return;
    }
    char text[L2D_CMD_REPLY_SIZE];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(text, sizeof(text), fmt, ap);
    va_end(ap);

    size_t n = strlen(text);

    if (n >= sizeof(g_cmdRing->reply))
    {
        n = sizeof(g_cmdRing->reply) - 1;
    }
    memcpy(g_cmdRing->reply, text, n);
    g_cmdRing->reply[n] = '\0';
    __atomic_add_fetch(&g_cmdRing->replySeq, 1u, __ATOMIC_RELEASE);
}

/** 把能力表写进指令环 caps 区（模型装载/切换后调用；宿主据此应答 LIST）*/
void WriteCaps(L2DPlayer& player)
{
    if (g_cmdRing == NULL)
    {
        return;
    }
    __atomic_store_n(&g_cmdRing->capsReady, 0u, __ATOMIC_RELEASE);
    snprintf(g_cmdRing->fingerprint, sizeof(g_cmdRing->fingerprint), "0x%016llx",
             (unsigned long long)player.Fingerprint());
    player.BuildParamTable(g_cmdRing->params, L2D_CMD_PARAMS_SIZE);      // 全量参数表
    player.BuildCapabilityText(g_cmdRing->caps, L2D_CMD_CAPS_SIZE);      // 能力表（含 FP/PARAMGROUPS）
    __atomic_store_n(&g_cmdRing->capsReady, 1u, __ATOMIC_RELEASE);
}

/** 执行一条指令行（行协议见 main 分支 DESIGN-LLM-CONTROL.md §5；桌面协议见 PROTOCOL-LLM.md）*/
void ExecuteCmdLine(L2DPlayer& player, const char* line)
{
    while (*line == ' ' || *line == '\t')
    {
        ++line;
    }
    if (*line == '\0')
    {
        return;
    }
    player.NoteExternalActivity();

    // 防串扰：可选行首 "@<fp> " 前缀（十六进制 16 位，可带 0x）——不匹配则拒执。
    // fp = caps 里 FP= 那个值；换模型/换模型包后旧指令自然全部失效。
    if (*line == '@')
    {
        csmUint64 given = 0;
        const char* p = line + 1;

        if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X'))
        {
            p += 2;
        }
        int digits = 0;

        while (digits < 16)
        {
            const char c = *p;
            int v = -1;

            if (c >= '0' && c <= '9') { v = c - '0'; }
            else if (c >= 'a' && c <= 'f') { v = c - 'a' + 10; }
            else if (c >= 'A' && c <= 'F') { v = c - 'A' + 10; }
            else { break; }
            given = (given << 4) | (csmUint64)v;
            ++p;
            ++digits;
        }
        while (*p == ' ' || *p == '\t')
        {
            ++p;
        }
        if (digits == 0 || *p == '\0')
        {
            fprintf(stderr, "[cmd] @ 前缀格式不对（应为 @<16位hex> <指令>）\n");
            SetReply("REJECT @ 前缀格式不对");
            return;
        }
        if (given != player.Fingerprint())
        {
            fprintf(stderr, "[cmd] 拒绝：指纹不匹配（指令写给别的模型？）\n");
            SetReply("REJECT 指纹不匹配（本机 %s）", player.FingerprintText().c_str());
            return;
        }
        line = p;
    }

    const uint32_t replyMark = (g_cmdRing != NULL) ? g_cmdRing->replySeq : 0u;

    if (*line == '{' || *line == '[')   // 演出脚本（JSON：对象=单个，数组=队列）
    {
        char err[128] = {0};

        if (player.PlayScriptJson(line, (csmInt32)strlen(line), err, sizeof(err)))
        {
            printf("[cmd] 脚本入队（队列 %d）\n", (int)player.PendingSceneCount());
            SetReply("OK 脚本入队（队列 %d）", (int)player.PendingSceneCount());
        }
        else
        {
            fprintf(stderr, "[cmd] 脚本被拒：%s\n", err);
            SetReply("REJECT 脚本被拒：%s", err);
        }
        fflush(stdout);
        return;
    }

    char word[32] = {0};
    int used = 0;

    if (sscanf(line, "%31s%n", word, &used) != 1)
    {
        return;
    }
    const char* rest = line + used;

    while (*rest == ' ' || *rest == '\t')
    {
        ++rest;
    }

    if (!strcmp(word, "EXPR"))
    {
        if (!strcmp(rest, "next"))
        {
            player.StartExpression(-1);
        }
        else if (rest[0] >= '0' && rest[0] <= '9')
        {
            if (!player.StartExpression(atoi(rest)))
            {
                fprintf(stderr, "[cmd] 表情序号越界：%s\n", rest);
                SetReply("REJECT 表情序号越界：%s", rest);
            }
        }
        else if (*rest != '\0' && !player.StartExpressionByName(rest))
        {
            fprintf(stderr, "[cmd] 表情不存在：%s\n", rest);
            SetReply("REJECT 表情不存在：%s（LIST 查名字清单）", rest);
        }
    }
    else if (!strcmp(word, "MOTION"))
    {
        char gname[48] = {0};
        int idx = 0;
        const int n = sscanf(rest, "%47s %d", gname, &idx);
        int g = -1;

        for (csmInt32 k = 0; k < player.MotionGroupCount(); ++k)
        {
            if (strcmp(player.MotionGroupName(k), gname) == 0)
            {
                g = (int)k;
                break;
            }
        }
        if (n >= 1 && g >= 0)
        {
            if (!player.StartMotionByGroupIndex(g, idx))
            {
                fprintf(stderr, "[cmd] 动作不存在：%s %d\n", gname, idx);
                SetReply("REJECT 动作不存在：%s %d", gname, idx);
            }
        }
        else
        {
            fprintf(stderr, "[cmd] 动作组不存在：%s\n", gname);
            SetReply("REJECT 动作组不存在：%s（LIST 查 MOTION=）", gname);
        }
    }
    else if (!strcmp(word, "GESTURE"))
    {
        if (*rest != '\0' && !player.StartGesture(rest))
        {
            fprintf(stderr, "[cmd] 手势不存在：%s\n", rest);
            SetReply("REJECT 手势不存在：%s（LIST 查 GESTURE=）", rest);
        }
    }
    else if (!strcmp(word, "TAP"))
    {
        int tx = 0, ty = 0;

        if (sscanf(rest, "%d %d", &tx, &ty) == 2)
        {
            player.HandleTap(tx, ty);   // 区域判定在播放器里（用模型包围盒）
        }
    }
    else if (!strcmp(word, "PROBE"))
    {
        if (*rest != '\0' && strcmp(rest, "ALL") != 0)
        {
            player.SetProbePhys(false);
            player.ProbeParam(rest);    // PROBE <param>：只测一个（同步，便宜）
            SetReply("OK 单参数探测 %s", rest);
        }
        else
        {
            player.SetProbePhys(false);
            player.ProbeEnqueue(*rest != '\0');   // PROBE / PROBE ALL：入队（非阻塞）
            SetReply("OK 探测开跑（直连档%s）", (*rest != '\0') ? "，全量" : "");
        }
    }
    else if (!strcmp(word, "PROBEPHYS"))
    {
        if (*rest != '\0' && strcmp(rest, "ALL") != 0)
        {
            player.SetProbePhys(true);
            player.ProbeParam(rest);   // PROBEPHYS <param>：单参数（同步，物理稳态档）
            SetReply("OK 单参数探测 %s（物理稳态档）", rest);
        }
        else
        {
            player.SetProbePhys(true);   // 物理稳态档（美术绑在物理链输出上的模型靠它才动）
            player.ProbeEnqueue(*rest != '\0');   // PROBEPHYS / PROBEPHYS ALL：入队（非阻塞）
            SetReply("OK 探测开跑（物理稳态档%s）", (*rest != '\0') ? "，全量" : "");
        }
    }
    else if (!strcmp(word, "BODY"))
    {
        // 躯体通道：BODY head.x=20 head.y=-8 arm.r=0.6 ／ BODY reset ／ BODY（查询）
        if (*rest == '\0' || !strcmp(rest, "?"))
        {
            char st[192];
            char hs[160];

            player.BodyStateText(st, sizeof(st));
            player.HandStateText(hs, sizeof(hs));
            if (*hs == '\0')
            {
                player.ArmSetText(hs, sizeof(hs), true);
            }
            SetReply("OK BODY %s%s%s", (*st != '\0' || *hs != '\0') ? st : "(未设定)", (*st && *hs) ? " " : "", hs);
        }
        else if (!strcmp(rest, "reset") || !strcmp(rest, "clear"))
        {
            player.ResetBodyChannels();
            SetReply("OK BODY 已释放（回呼吸/视线自动行为）");
        }
        else
        {
            // 两遍解析：先全部校验（原子：有一个不认就整条拒，LLM 好纠错），再全部设定
            char pass1[256];
            const int p1 = snprintf(pass1, sizeof(pass1), "%s", rest);
            (void)p1;

            char bad[160] = {0};
            bool ok = true;
            char* save = NULL;

            for (char* tok = strtok_r(pass1, " ", &save); tok != NULL; tok = strtok_r(NULL, " ", &save))
            {
                char* eq = strchr(tok, '=');

                if (eq == NULL || eq == tok || eq[1] == '\0')
                {
                    snprintf(bad, sizeof(bad), "%s", tok);
                    ok = false;
                    break;
                }
                *eq = '\0';
                const size_t tl = strlen(tok);

                if (tl > 4 && !strcmp(tok + tl - 4, ".set"))
                {
                    // 作者手臂套：arm.r.set=B / auto
                    const std::string err = player.SetArmSet(tok, eq + 1, true);

                    if (!err.empty())
                    {
                        snprintf(bad, sizeof(bad), "%s", err.c_str());
                        ok = false;
                        break;
                    }
                    continue;
                }
                if (strncmp(tok, "hand.", 5) == 0)
                {
                    // IK 目标：hand.r=mouth / hand.r=0.1,0.5 / hand.r=off
                    const std::string err = player.SetHandTarget(tok, eq + 1, true);

                    if (!err.empty())
                    {
                        snprintf(bad, sizeof(bad), "%s", err.c_str());
                        ok = false;
                        break;
                    }
                    continue;
                }
                if (strcmp(tok, "t") != 0 && !player.HasBodyChannel(tok))
                {
                    snprintf(bad, sizeof(bad), "%s", tok);
                    ok = false;
                    break;
                }
            }
            if (!ok)
            {
                fprintf(stderr, "[cmd] 躯体通道不认：%s\n", bad);
                SetReply("REJECT BODY：%s（通道见 LIST 的 BODYCHANNELS，锚点见 ANCHORS）", bad);
            }
            else
            {
                char pass2[256];
                const int p2 = snprintf(pass2, sizeof(pass2), "%s", rest);
                (void)p2;

                char* save2 = NULL;
                // t=<秒>：这条指令的过渡时间（"0.2 秒甩上去" vs "2 秒慢慢抬"）；缺省=通道默认
                csmFloat32 trans = 0.0f;
                {
                    const char* tp = strstr(rest, "t=");

                    while (tp != NULL && tp != rest && tp[-1] != ' ')
                    {
                        tp = strstr(tp + 1, "t=");
                    }
                    if (tp != NULL)
                    {
                        trans = (csmFloat32)atof(tp + 2);
                    }
                }

                for (char* tok = strtok_r(pass2, " ", &save2); tok != NULL; tok = strtok_r(NULL, " ", &save2))
                {
                    char* eq = strchr(tok, '=');

                    *eq = '\0';
                    const size_t tl = strlen(tok);

                    if (tl > 4 && !strcmp(tok + tl - 4, ".set"))
                    {
                        player.SetArmSet(tok, eq + 1, false);
                    }
                    else if (strncmp(tok, "hand.", 5) == 0)
                    {
                        player.SetHandTarget(tok, eq + 1, false);
                    }
                    else if (strcmp(tok, "t") != 0)
                    {
                        player.SetBodyChannel(tok, (csmFloat32)atof(eq + 1), trans);
                    }
                }
                {
                    char st[192];

                    char hs[160];

                    player.BodyStateText(st, sizeof(st));
                    player.HandStateText(hs, sizeof(hs));
                    if (*hs == '\0')
                    {
                        player.ArmSetText(hs, sizeof(hs), true);
                    }
                    SetReply("OK BODY %s%s%s", st, (*st && *hs) ? " " : "", hs);
                }
            }
        }
    }
    else if (!strcmp(word, "ACTION"))
    {
        // 动作片段：ACTION <名> [speed=1] [scale=1] [repeat=1] ／ ACTION stop
        char name[64] = {0};
        float speed = 1.0f, scale = 1.0f;
        int repeat = 1;

        sscanf(rest, "%63s", name);
        const char* p;
        if ((p = strstr(rest, "speed=")) != NULL) speed = (float)atof(p + 6);
        if ((p = strstr(rest, "scale=")) != NULL) scale = (float)atof(p + 6);
        if ((p = strstr(rest, "repeat=")) != NULL) repeat = atoi(p + 7);
        if (name[0] == '\0')
        {
            SetReply("REJECT ACTION 缺动作名（LIST 查 ACTIONS=）");
        }
        else if (!strcmp(name, "stop"))
        {
            player.StopActions();
            SetReply("OK ACTION 已停");
        }
        else
        {
            std::string missing;
            csmFloat32 total = 0.0f;

            if (!player.StartAction(name, speed, scale, repeat, &missing, &total))
            {
                SetReply("REJECT 动作不存在：%s（LIST 查 ACTIONS=）", name);
            }
            else
            {
                // 回复带时长（LLM 排节奏）+ 本模型缺的通道（知道哪部分做不出来）
                SetReply("OK ACTION %s %.1fs%s%s", name, (double)total,
                         missing.empty() ? "" : " 缺:", missing.c_str());
            }
        }
    }
    else if (!strcmp(word, "PART"))
    {
        // 部件不透明度：PART PartArmAR=0 PartArmBR=1 ／ PART off（切作者画的另一套手臂/道具）
        const std::string err = player.SetPartHold(rest);

        if (err.empty())
        {
            SetReply("OK PART %s", rest);
        }
        else
        {
            SetReply("REJECT PART：%s", err.c_str());
        }
    }
    else if (!strcmp(word, "MOCAP"))
    {
        // 动捕感强度：MOCAP 0..3（0=关：纯指令值；1=默认微动+跟随；2 活泼；3 夸张）
        if (*rest != '\0')
        {
            player.SetMocapLevel(atoi(rest));
            WriteCaps(player);   // LIST 的 MOCAP= 跟着变
        }
        SetReply("OK MOCAP=%d", (int)player.MocapLevel());
    }
    else if (!strcmp(word, "TALK"))
    {
        // TALK <秒> 或 TALK <文本…>（文本按字数估时长）；口型叠在表情上（举话筒说话）
        if (*rest != '\0' && rest[0] >= '0' && rest[0] <= '9')
        {
            player.StartTalk((csmFloat32)atof(rest), NULL);
            SetReply("OK 说话 %.1fs", atof(rest));
        }
        else if (*rest != '\0')
        {
            player.StartTalk(L2DPlayer::EstimateTalkDur(rest), rest);
            SetReply("OK 说话（按文本估时）");
        }
        else
        {
            SetReply("REJECT TALK 需要秒数或文本");
        }
    }
    else if (!strcmp(word, "LOOK"))
    {
        float x = 0, y = 0;

        if (sscanf(rest, "%f %f", &x, &y) == 2)
        {
            player.SetLookTargetPixels(x, y);
        }
    }
    else if (!strcmp(word, "IDLE"))
    {
        player.SetIdleLevel(atoi(rest));
        printf("[cmd] 自主待机强度 → %d\n", (int)player.IdleLevel());
    }
    else if (!strcmp(word, "RESET"))
    {
        player.PerformanceReset();
    }
    else if (!strcmp(word, "LIST") || !strcmp(word, "STATUS"))
    {
        // 由宿主应答（caps 区在宿主侧可读），本进程不处理
    }
    else
    {
        fprintf(stderr, "[cmd] 未识别指令：%s\n", word);
        SetReply("REJECT 未识别指令：%s", word);
        fflush(stdout);
        return;
    }
    // 分支里没专门回复的，统一回一句（工具靠 replySeq 变化判"执行到没执行"）
    if (g_cmdRing != NULL && g_cmdRing->replySeq == replyMark)
    {
        SetReply("OK %s", word);
    }
    fflush(stdout);
}

/** 排空指令环：攒行 → 逐行执行（每帧调用；SPSC：只有本进程写 tail）*/
void DrainCmdRing(L2DPlayer& player)
{
    static std::string acc;

    if (g_cmdRing == NULL)
    {
        return;
    }
    for (;;)
    {
        const uint32_t head = __atomic_load_n(&g_cmdRing->head, __ATOMIC_ACQUIRE);
        uint32_t tail = __atomic_load_n(&g_cmdRing->tail, __ATOMIC_RELAXED);

        if (tail == head)
        {
            break;
        }
        while (tail != head)
        {
            const char c = g_cmdRing->data[tail % L2D_CMD_RING_SIZE];

            ++tail;
            if (c == '\n')
            {
                __atomic_store_n(&g_cmdRing->tail, tail, __ATOMIC_RELEASE);
                __atomic_add_fetch(&g_cmdRing->lines, 1u, __ATOMIC_RELAXED);
                ExecuteCmdLine(player, acc.c_str());
                acc.clear();
            }
            else if (acc.size() < 16384)
            {
                acc.push_back(c);
            }
            else
            {
                acc.clear(); // 超长行保护（正常不会有）
            }
        }
        __atomic_store_n(&g_cmdRing->tail, tail, __ATOMIC_RELEASE);
    }
}

// ---------------------------------------------------------------- main

#include "PetBootstrap.h"   // 命令行选项与解析（从本文件拆出）
#include "MainLoop.h"        // 运行期 + 主循环（从本文件拆出）

#if defined(_WIN32)
#include <windows.h>   // GetConsoleWindow / ShowWindow / _wfreopen：藏控制台 + 日志转文件

/** 双击启动时（控制台子系统下 Windows 会**给这个进程新开一个控制台**）：
 *   1) 把那个控制台窗口立刻藏掉——桌宠不需要黑窗口；
 *   2) 日志改写到 %TEMP%\l2dpet.log（藏起来的控制台没人看，文件更好找）。
 *  从终端启动时控制台是**调用方的**（GetConsoleProcessList > 1）：窗口不动，日志照旧进终端。
 *
 *  **调试用**：设 L2DPET_DEBUG_CONSOLE=1 就不藏控制台、日志也不转文件 → 那个黑窗口
 *  就是实时日志窗（单文件启动器对这个变量同样生效：见 tools/portable/portable.cpp）。
 *
 *  为什么不用 GUI 子系统（双击根本不开控制台）？实测在这个进程里 CRT 自己的
 *  stdout/stderr 过一会儿就写不出去（静态 CRT + 无控制台 + GLFW/线程：freopen 到文件、
 *  甚至 _dup2 接管 fd 1/2 都一样，日志只剩启动头几行）。宁可要控制台子系统 + 立刻隐藏。*/
static void SetupConsoleAndLog()
{
    if (GetEnvironmentVariableW(L"L2DPET_DEBUG_CONSOLE", NULL, 0) > 0) return;   // 调试：控制台留着

    const HWND con = GetConsoleWindow();
    DWORD pids[4] = { 0 };
    const DWORD attached = (con != NULL) ? GetConsoleProcessList(pids, 4) : 0;
    if (con == NULL || attached > 1) return;   // 没有控制台 / 是调用方终端共享的：原样不动

    ShowWindow(con, SW_HIDE);

    wchar_t tmp[MAX_PATH] = L"";
    if (GetTempPathW(MAX_PATH, tmp) == 0) return;
    const std::wstring path = std::wstring(tmp) + L"l2dpet.log";
    if (_wfreopen(path.c_str(), L"w", stdout) == NULL) return;
    if (_wfreopen(path.c_str(), L"a", stderr) == NULL) return;
}
#else
static void SetupConsoleAndLog() {}
#endif

int main(int argc, char** argv)
{
#if defined(_WIN32)
    // 让 CRT 的窄字符文件/进程 API（fopen / _access / _spawnv …）按 UTF-8 解释路径。
    // 平台层自己的路径操作已全部改走 W 版 API；这里兜住应用里剩下的那些窄调用。
    _setmbcp(_MB_CP_UTF8);
    // 输出不缓冲：桌宠是常驻进程，日志得能实时看（重定向到文件、双击时尤其明显——
    // 默认块缓冲会把日志憋在 4KB 里，崩溃/排查时什么都看不到）。
    // 必须在任何输出之前调：freopen 之后再调 setvbuf 会 fail-fast（实测 0xC0000409）。
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    SetupConsoleAndLog();
#endif
    PetOptions opt;
    const int rc = ParseArgs(argc, argv, &opt);
    if (rc >= 0)
    {
        return rc;   // 只看了用法 / 参数不认
    }
    return RunMainLoop(opt);
}

#if defined(_WIN32) && defined(L2D_GUI_SUBSYSTEM)
/** GUI 子系统（双击不带黑窗口）下的入口：CRT 走 WinMainCRTStartup → WinMain。
 *  GLFW 不带这个 shim，自己写一行转给 main。
 *  （别改成 /ENTRY:mainCRTStartup 硬指入口：那样绕开 CRT 的 GUI 启动路径，
 *    静态 CRT + 无控制台时 stdout/stderr 会写不出去——日志只剩零星几行，实测。）*/
int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int)
{
    return main(__argc, __argv);
}
#endif

/**
 * PlayerProbe.cpp — L2DPlayer 的能力探测实现（从 l2dtest.cpp 拆出，纯搬移，逻辑未改）。
 * 声明与数据成员见 L2DPlayer.h。
 */
#include "L2DPlayer.h"

#include "FileUtil.h"
#include "ModelImport.h"
#include "Backup.h"
#include "Platform.h"
#include "Port.hpp"
#include "JsonLite.h"
#include "CtlServer.hpp"
#include "PetI18n.h"
#include "l2d_config.h"

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

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <set>
#include <string>
#include <vector>

using namespace Live2D::Cubism::Framework;
using Rendering::CubismRenderer_Software;
using Rendering::SoftTexture;

/** 物理跑到"稳定"：20 步一组推进，组间**全部参数**变化都小于阈值就停（上限 maxSteps）。
 *  物理是积分器，固定 40 步对不同模型不保证收敛——不收敛时测到的"效果"其实是
 *  残余摆动（老矩阵就有这种假阳性）。返回实际步数（诊断）。*/
csmInt32 L2DPlayer::SettlePhysicsStable(csmInt32 maxSteps)
{
    if (_physics == NULL)
    {
        _model->Update();
        return 0;
    }
    const csmInt32 pc = _model->GetParameterCount();
    std::vector<csmFloat32> prev((size_t)pc, 0.0f), cur((size_t)pc, 0.0f);
    csmInt32 used = 0;

    SnapshotParams(&prev);
    while (used < maxSteps)
    {
        for (int t = 0; t < 20 && used < maxSteps; ++t, ++used)
        {
            _physics->Evaluate(_model, 1.0f / 30.0f);
        }
        SnapshotParams(&cur);
        bool settled = true;

        for (csmInt32 i = 0; i < pc && settled; ++i)
        {
            const csmFloat32 d = fabsf(cur[(size_t)i] - prev[(size_t)i]);

            if (d > 1e-4f * (1.0f + fabsf(cur[(size_t)i])))
            {
                settled = false;
            }
        }
        if (settled)
        {
            break;
        }
        prev.swap(cur);
    }
    _model->Update();
    return used;
}

/** 测"某个取值"下的顶点位移；physMode=false 直连（不跑物理）、true 走物理稳态 */
L2DPlayer::ProbeEffect L2DPlayer::ProbeSide(csmInt32 idx, csmFloat32 val, csmBool physMode)
{
    std::vector<std::vector<csmFloat32> > base;

    // 基线：默认参数（同一档：可选物理稳态）→ 快照
    ResetToDefaults();
    if (physMode)
    {
        SettlePhysics();
    }
    else
    {
        _model->Update();
    }
    SnapshotVerts(&base);

    ResetToDefaults();
    _model->SetParameterValue(idx, val);
    if (physMode)
    {
        SettlePhysics();
    }
    else
    {
        _model->Update();
    }

    ProbeEffect e;
    const csmInt32 dc = _model->GetDrawableCount();

    e.x0 = 1e9f;
    e.x1 = -1e9f;
    e.y0 = 1e9f;
    e.y1 = -1e9f;

    for (csmInt32 d = 0; d < dc; ++d)
    {
        const csmInt32 vc = _model->GetDrawableVertexCount(d);
        const Live2D::Cubism::Core::csmVector2* pos = _model->GetDrawableVertexPositions(d);
        const std::vector<csmFloat32>& brow = base[(size_t)d];
        csmInt32 touched = 0;

        if (pos == NULL || brow.size() < (size_t)vc * 2)
        {
            continue;
        }
        for (csmInt32 v = 0; v < vc; ++v)
        {
            const csmFloat32 cx = pos[v].X, cy = pos[v].Y;
            const csmFloat32 bx = brow[(size_t)v * 2], by = brow[(size_t)v * 2 + 1];
            const csmFloat32 dd = (fabsf(cx - bx) > fabsf(cy - by)) ? fabsf(cx - bx) : fabsf(cy - by);

            if (dd > 1e-4f)
            {
                if (cx < e.x0) e.x0 = cx;
                if (cx > e.x1) e.x1 = cx;
                if (cy < e.y0) e.y0 = cy;
                if (cy > e.y1) e.y1 = cy;
                touched = 1;
            }
            if (dd > e.delta)
            {
                e.delta = dd;
            }
        }
        e.moved += touched;
    }
    return e;
}

/** 探测单个参数（按序号）；结果并入能力表 _cap，打印 O/P 两行（P 行尾带 eff=）*/
void L2DPlayer::ProbeParamByIdx(csmInt32 idx)
{
    if (idx < 0 || idx >= _model->GetParameterCount())
    {
        printf("[probe] 参数序号越界：%d（共 %d）\n", (int)idx, (int)_model->GetParameterCount());
        fflush(stdout);
        return;
    }
    const char* paramCode = _model->GetParameterId((csmUint32)idx)->GetString().GetRawString();
    const csmFloat32 dmin = _model->GetParameterMinimumValue((csmUint32)idx);
    const csmFloat32 dmax = _model->GetParameterMaximumValue((csmUint32)idx);
    const csmFloat32 ddef = _model->GetParameterDefaultValue((csmUint32)idx);
    std::vector<std::vector<csmFloat32> > base;

    ResetToDefaults();
    if (_probePhys)
    {
        SettlePhysics();
    }
    _model->Update();
    SnapshotVerts(&base);

    /* 不透明度基线：有的能力是"换美术"（眨眼/嘴型/道具靠 drawable 不透明度
     * 切换、顶点不动）——只测顶点会漏（实测 Wanko 全走这条） */
    std::vector<csmFloat32> baseOp;
    {
        const csmUint32 dc0 = (csmUint32)_model->GetDrawableCount();

        baseOp.assign((size_t)dc0, 0.0f);
        for (csmUint32 d0 = 0; d0 < dc0; ++d0)
        {
            baseOp[(size_t)d0] = _model->GetDrawableOpacity(d0);
        }
    }

    csmFloat32 best = -1.0f, bestVal = ddef;
    csmInt32 bestMoved = 0;
    csmInt32 bestOpMoved = 0;
    csmFloat32 bx0 = 0, bx1 = 0, by0 = 0, by1 = 0;
    const char* side = "max";
    std::vector<csmInt32> bestSet;   // 动过的 drawable 集（手臂部件学习用）

    for (int s = 0; s < 2; ++s)
    {
        const csmFloat32 val = (s == 0) ? (ddef + 0.8f * (dmax - ddef))
                                        : (ddef + 0.8f * (dmin - ddef));

        ResetToDefaults();
        _model->SetParameterValue(idx, val);
        if (_probePhys)
        {
            SettlePhysics();   /* 物理稳态档：美术绑在物理链输出上的模型靠它才动 */
            /* 运行时语义校准：轨道写值发生在 scheduler（物理）**之后**（ApplySceneTracks），
             * 所以"物理输出参数"也能被驱动——这里再写一次，否则会被物理覆盖成全零 */
            _model->SetParameterValue(idx, val);
        }
        _model->Update();

        csmFloat32 delta = 0.0f;
        csmInt32 moved = 0;
        csmInt32 opMoved = 0;
        csmFloat32 x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f;
        const csmInt32 dc = _model->GetDrawableCount();
        std::vector<csmInt32> sideSet;   // 本侧动过的 drawable（手臂部件学习用）

        for (csmInt32 d = 0; d < dc; ++d)
        {
            const csmInt32 vc = _model->GetDrawableVertexCount(d);
            const Live2D::Cubism::Core::csmVector2* pos = _model->GetDrawableVertexPositions(d);
            const std::vector<csmFloat32>& brow = base[(size_t)d];
            csmInt32 touched = 0;

            /* 不透明度也看：有的能力是"换美术"（顶点不动、切 drawable 不透明度）*/
            if (fabsf(_model->GetDrawableOpacity((csmUint32)d) - baseOp[(size_t)d]) > 0.01f)
            {
                ++opMoved;
            }
            if (pos == NULL || brow.size() < (size_t)vc * 2)
            {
                continue;
            }
            for (csmInt32 v = 0; v < vc; ++v)
            {
                const csmFloat32 cx = pos[v].X, cy = pos[v].Y;
                const csmFloat32 bx = brow[(size_t)v * 2], by = brow[(size_t)v * 2 + 1];
                const csmFloat32 dd = (fabsf(cx - bx) > fabsf(cy - by)) ? fabsf(cx - bx) : fabsf(cy - by);

                if (dd > 1e-4f)
                {
                    if (cx < x0) x0 = cx;
                    if (cx > x1) x1 = cx;
                    if (cy < y0) y0 = cy;
                    if (cy > y1) y1 = cy;
                    touched = 1;
                }
                if (dd > delta)
                {
                    delta = dd;
                }
            }
            moved += touched;
            if (touched)
            {
                sideSet.push_back(d);   // 部件学习：本侧动过的 drawable（每件一次）
            }
        }
        if (opMoved > bestOpMoved)
        {
            bestOpMoved = opMoved;
        }
        if (delta > best)
        {
            best = delta;
            bestVal = val;
            bestMoved = moved;
            bx0 = x0; bx1 = x1; by0 = y0; by1 = y1;
            side = (s == 0) ? "max" : "min";
            bestSet = sideSet;   // 记住"更有效那侧"动过的 drawable（部件学习）
        }
    }
    /* 手臂部件学习：参数名像"手臂"（armR/ArmR…）就把它带动的 drawable 归到 arm.r/arm.l
     * ——运行时"纸片人"刚性抬手靠这份名单（不依赖作者绑定） */
    {
        const char* partKey = NULL;
        const char* ah = strstr(paramCode, "Arm");

        if (ah == NULL)
        {
            ah = strstr(paramCode, "arm");
        }
        if (ah == NULL)
        {
            ah = strstr(paramCode, "Hand");   // 有的模型只有 Hand* 段
        }
        if (ah == NULL)
        {
            ah = strstr(paramCode, "hand");
        }
        if (ah != NULL)
        {
            // "Arm" 之后第一个 L/R 定左右：ParamarmR1、ParamArmAL01、ParamArmBRHand01、
            // ParamArmDLHand03…（多臂链模型都能兜住）
            for (const char* p = ah + 3; *p != '\0'; ++p)
            {
                if (*p == 'L' || *p == 'l')
                {
                    partKey = "arm.l";
                    break;
                }
                if (*p == 'R' || *p == 'r')
                {
                    partKey = "arm.r";
                    break;
                }
            }
        }
        if (partKey != NULL && !bestSet.empty())
        {
            std::vector<csmInt32>& dst = _parts[partKey];

            for (size_t i = 0; i < bestSet.size(); ++i)
            {
                if (std::find(dst.begin(), dst.end(), bestSet[i]) == dst.end())
                {
                    dst.push_back(bestSet[i]);
                }
            }
        }
    }
    printf("O opacity=%d\n", (int)bestOpMoved);
    fflush(stdout);
    ResetToDefaults();

    /* 结果并入能力表：真动了才置"直连/物理"位；"换美术"从不透明度被改动观测 */
    csmUint8 shown = 0;

    if (_cap.size() == (size_t)_model->GetParameterCount())
    {
        ParamCap& cap = _cap[(size_t)idx];
        csmUint8 add = kCapKnown;

        if (bestMoved > 0)
        {
            if (_probePhys == false)
            {
                add |= kCapDirect;      // 直连档：不跑物理也能动
            }
            else if ((cap.flags & kCapDirect) == 0)
            {
                // 物理档动、直连档不动 ⇒ 靠物理链生效（先测直连、后测物理的约定下成立）
                add |= kCapPhys;
            }
        }
        if (bestOpMoved > 0)
        {
            add |= kCapArt;
        }
        cap.flags |= add;
        if (best > cap.delta)
        {
            cap.delta = best;
        }
        shown = cap.flags;
    }
    printf("P %s exists=1", paramCode);
    printf(" range=%.3f..%.3f def=%.3f side=%s val=%.3f", dmin, dmax, ddef, side, bestVal);
    printf(" maxDelta=%.4f moved=%d", best, bestMoved);
    printf(" bbox=%.2f,%.2f,%.2f,%.2f eff=%s\n", bx0, bx1, by0, by1, CapCode(shown));
    fflush(stdout);
}

void L2DPlayer::ProbeEnqueue(csmBool all)
{
    static const char* const kStd[] = {
        "ParamAngleX", "ParamAngleY", "ParamAngleZ",
        "ParamBodyAngleX", "ParamBodyAngleY", "ParamBodyAngleZ",
        "ParamEyeBallX", "ParamEyeBallY",
        "ParamEyeLOpen", "ParamEyeROpen", "ParamEyeLSmile", "ParamEyeRSmile",
        "ParamBrowLY", "ParamBrowRY", "ParamBrowLForm", "ParamBrowRForm",
        "ParamMouthOpenY", "ParamMouthForm", "ParamMouthX",
        "ParamBreath",
        "ParamArmLA", "ParamArmLB", "ParamArmRA", "ParamArmRB",
        "ParamArmL", "ParamArmR",
        NULL,
    };
    const csmInt32 pc = _model->GetParameterCount();

    _probeQueue.clear();
    if (all)
    {
        for (csmInt32 i = 0; i < pc; ++i)
        {
            _probeQueue.push_back(i);
        }
    }
    else
    {
        for (int k = 0; kStd[k] != NULL; ++k)
        {
            const csmInt32 idx = _model->GetParameterIndex(
                CubismFramework::GetIdManager()->GetId(kStd[k]));

            // 存在性判据同 ProbeParam：合成序号（≥count）不算存在（见函数注释）
            if (idx >= 0 && idx < pc)
            {
                _probeQueue.push_back(idx);
            }
        }
        // 自动族：名字里含这些子串的参数也测（各家作者命名不同，靠这里兜住）
        static const char* const kFam[] = { "arm", "Arm", "hand", "Hand", "wrist", "Wrist",
                                            "Leg", "leg", "Foot", "foot", "Knee", "knee", NULL };

        for (csmInt32 i = 0; i < pc; ++i)
        {
            const char* pid = _model->GetParameterId((csmUint32)i)->GetString().GetRawString();
            bool dup = false;

            for (int k = 0; kStd[k] != NULL && !dup; ++k)
            {
                dup = (strcmp(kStd[k], pid) == 0);
            }
            if (dup)
            {
                continue;
            }
            for (int k = 0; kFam[k] != NULL && !dup; ++k)
            {
                if (strstr(pid, kFam[k]) != NULL)
                {
                    _probeQueue.push_back(i);
                    dup = true;
                }
            }
        }
    }
    _probeActive = true;
    _probeDone = 0;
    _probeTotal = (csmInt32)_probeQueue.size();
    printf("[probe] 开跑 %d 项（%s档；每帧按预算推进，完事自动写能力缓存）\n",
           (int)_probeTotal, _probePhys ? "物理稳态" : "直连");
    fflush(stdout);
}

/** 每帧推进一步（主循环在正式解算前调用：探测的中间姿态会被本帧解算覆盖，
 *  屏幕不受影响；物理档会给物理链留过渡状态，探测期间模型轻微晃动属正常）*/
void L2DPlayer::ProbeTick()
{
    if (!_probeActive)
    {
        return;
    }
    if (_probeQueue.empty())
    {
        ProbeFinish();   // 上一帧跑完 → 收尾
        return;
    }
    static const char* envBudget = getenv("L2D_PROBE_BUDGET");
    const double budgetMs = (envBudget != NULL) ? atof(envBudget) : 120.0;
    struct timespec t0, t1;

    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (;;)
    {
        ProbeParamByIdx(_probeQueue.back());
        _probeQueue.pop_back();
        ++_probeDone;
        clock_gettime(CLOCK_MONOTONIC, &t1);
        const double ms = (double)(t1.tv_sec - t0.tv_sec) * 1000.0
                        + (double)(t1.tv_nsec - t0.tv_nsec) * 1e-6;

        if (_probeQueue.empty() || ms >= budgetMs)
        {
            break;
        }
    }
    if (_probeQueue.empty())
    {
        ProbeFinish();
    }
    else if ((_probeDone % 25) == 0)
    {
        printf("[probe] 进度 %d/%d\n", (int)_probeDone, (int)_probeTotal);
        fflush(stdout);
    }
}

/**
 * "画出部分"的屏幕包围盒（顶点级；比 canvas 包围盒紧——点空白背景不算戳到人）。
 * 每次点击算一次（几万顶点变换 ≈ 1~2ms，点击频率下无所谓）。
 */
void L2DPlayer::ComputeArtBBox(csmFloat32* bb)
{
    bb[0] = bb[1] = 1e9f;
    bb[2] = bb[3] = -1e9f;
    if (_targetW < 8.0f || _targetH < 8.0f)
    {
        return;
    }
    // 优先：真正画出来的像素（帧缓冲 alpha）。网格顶点包围盒带透明留白/辅助件，
    // 实测舒芙蕾下沿比脚底低一大截 → 腿部分段整体下移、"脚"根本点不到。0.5s 缓存一次。
    if (SilhouetteBBox(bb))
    {
        return;
    }
    CubismMatrix44 projection;

    BuildBaseProjection(&projection, (csmUint32)_targetW, (csmUint32)_targetH);
    if (_renderZoom != 1.0f && _renderZoom > 0.0f)
    {
        projection.ScaleRelative(_renderZoom, _renderZoom);
    }
    projection.MultiplyByMatrix(GetModelMatrix());

    const csmFloat32* mm = projection.GetArray();
    const csmInt32 dc = _model->GetDrawableCount();

    for (csmInt32 d = 0; d < dc; ++d)
    {
        if (_model->GetDrawableOpacity(d) <= 0.001f)
        {
            continue; // 不可见（隐藏辅助件）不算
        }
        const csmInt32 vc = _model->GetDrawableVertexCount(d);
        const Live2D::Cubism::Core::csmVector2* pos = _model->GetDrawableVertexPositions(d);

        if (pos == NULL)
        {
            continue;
        }
        for (csmInt32 v = 0; v < vc; ++v)
        {
            const csmFloat32 px = pos[v].X * mm[0] + pos[v].Y * mm[4] + mm[12];
            const csmFloat32 py = pos[v].X * mm[1] + pos[v].Y * mm[5] + mm[13];
            const csmFloat32 pw = pos[v].X * mm[3] + pos[v].Y * mm[7] + mm[15];
            const csmFloat32 iw = (pw != 0.0f) ? 1.0f / pw : 1.0f;
            const csmFloat32 sx = (px * iw + 1.0f) * 0.5f * _targetW;
            const csmFloat32 sy = (1.0f - py * iw) * 0.5f * _targetH;

            if (sx < bb[0]) bb[0] = sx;
            if (sx > bb[2]) bb[2] = sx;
            if (sy < bb[1]) bb[1] = sy;
            if (sy > bb[3]) bb[3] = sy;
        }
    }
}

/**
 * 点击（渲染目标坐标）：区域判定 → 反应。
 * 区域＝模型屏幕包围盒内按高度切：上 32% 算"头部"，其余算"身体"；框外忽略。
 * 反应优先级：① 模型自带 Tap* 动作素材（Haru/Mao/Natori 等有 TapBody，官方示例行为）
 *            ② profile 的 reactions（head/body/any：手势+表情）
 *            ③ 内置兜底（头→tilt 歪头；身→surprise 惊讶）
 * 带 1.2s 冷却（连点不刷屏）。
 */
/** 返回点到的区域：0=头 1=身 -1=没点到人/冷却中 */
/**
 * 点击（渲染目标坐标）：16 区判定 → 反应。
 * 区域＝锚点投影 + 包围盒分带（见 ClassifyZone）；反应映射：头/脸/颈 = "头部反应"，其余 = "身体反应"。
 * 反应优先级：① 模型自带 Tap* 动作素材（Haru/Mao/Natori 等有 TapBody，官方示例行为）
 *            ② profile 的 reactions（head/body/any：手势+表情）
 *            ③ 内置兜底（头→tilt 歪头；身→surprise 惊讶）
 * 带 1.2s 冷却（连点不刷屏）。zoneOut（可空）带回区域 key 给协议层。
 */
// ===================== 身体区域的本地即时反应（不经 LLM；PROTOCOL-LLM.md 的 16 区）=====================
// 原来只分"头/身"两类，点手、点腿、点脚反应都一样 → 体感"不灵敏"。现在每个区有自己的反应：
// 先看向点到的位置，再按部位做动作 + 表情；同区连点 ≥3 次升级成"生气"；搓（rub）另一套。
// profile 可覆盖："zoneReactions": {"head": {"action":"shy","expr":"害羞"}, "rub.head": {...}}

/** 情绪 → 表情：先 profile 别名，再按常见命名找（中/日/英各家叫法不同）*/
bool L2DPlayer::StartEmotion(const char* emo)
{
    if (emo == NULL || *emo == '\0')
    {
        return false;
    }
    if (StartExpressionByName(emo))
    {
        return true;
    }
    static const struct { const char* emo; const char* names[6]; } kTab[] = {
        { "开心", { "爱心", "Smile", "smile", "Happy", "happy", "星星" } },
        { "害羞", { "脸红", "Blushing", "blush", "Shy", "shy", "照れ" } },
        { "生气", { "Angry", "angry", "怒", "黑脸", "Mad", "mad" } },
        { "惊讶", { "Surprised", "surprised", "Surprise", "星星", "驚き", "惊" } },
        { "难过", { "Sad", "sad", "哭哭", "Cry", "cry", "悲しみ" } },
    };
    for (size_t i = 0; i < sizeof(kTab) / sizeof(kTab[0]); ++i)
    {
        if (strcmp(kTab[i].emo, emo) != 0)
        {
            continue;
        }
        for (int k = 0; k < 6; ++k)
        {
            if (StartExpressionByName(kTab[i].names[k]))
            {
                return true;
            }
        }
    }
    return false;
}

/**
 * PlayerBody.cpp — L2DPlayer 的躯体通道 / 手臂骨架实现（从 l2dtest.cpp 拆出，纯搬移，逻辑未改）。
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

// ===================== 躯体通道（BODY：像控制身体一样控制模型） =====================
// 通用通道名 → 本模型参数：标准名自动映射（各家同名参数直接可用），profile 的
// "body" 段可覆盖/追加（如舒芙蕾的 5 段手臂链 → 一条 arm.r 通道）。
// 语义：**增量**（写值叠加在呼吸/视线等自动行为之上，身体仍然"活着"）；
// 设定后每帧保持 + 一阶平滑（看起来像身体在动过去），直到改值或 `BODY reset`。

/** 建通道（参数全部不存在则整条通道不建——跨模型自动降级）。
 *  preOverride：-1=按能力数据自动判层（eff 含 p → 物理前），0=强制 post，1=强制 pre */
void L2DPlayer::AddBodyChannel(const char* name, const std::vector<std::pair<const char*, csmFloat32> >& maps, csmFloat32 slew, csmInt32 preOverride, csmInt32 rigidOverride)
{
    BodyChannel ch;

    ch.name = name;
    ch.slew = slew;
    ch.omega = slew * kBodyOmegaK;
    for (size_t i = 0; i < maps.size(); ++i)
    {
        const csmInt32 idx = _model->GetParameterIndex(
            CubismFramework::GetIdManager()->GetId(maps[i].first));

        if (idx >= 0 && idx < _model->GetParameterCount())
        {
            ch.params.push_back(std::make_pair(idx, maps[i].second));
        }
    }
    if (ch.params.empty())
    {
        return;
    }
    // 已知"死参数"（能效探测为 -，既无直连也无物理/美术效果）→ 整条通道不建：
    // 别让 LLM 往没有布线的部位使劲（舒芙蕾的 body.x 就是这种）
    if (!_cap.empty())
    {
        bool anyAlive = false;

        for (size_t i = 0; i < ch.params.size(); ++i)
        {
            const size_t idx = (size_t)ch.params[i].first;

            if (idx >= _cap.size() || (_cap[idx].flags & kCapKnown) == 0
                || (_cap[idx].flags & (kCapDirect | kCapPhys | kCapArt)) != 0)
            {
                anyAlive = true;   // 未知也算活着（探测数据不全时不误杀）
                break;
            }
        }
        if (!anyAlive)
        {
            return;
        }
    }
    if (rigidOverride >= 0)
    {
        ch.rigid = (rigidOverride != 0);
    }
    if (preOverride >= 0)
    {
        ch.pre = (preOverride != 0);
    }
    else if (!_cap.empty() && ch.params[0].first < (csmInt32)_cap.size())
    {
        // 自动判层：该参数"仅/兼经物理"（能效里带 p）→ 物理必须看得到 → 写在物理前
        ch.pre = (_cap[(size_t)ch.params[0].first].flags & kCapPhys) != 0;
    }
    _channels.push_back(ch);
}

/** 装载时建标准通道表 + profile 覆盖 */
void L2DPlayer::BuildBodyChannels()
{
    _channels.clear();

    struct Std { const char* ch; const char* param; csmFloat32 slew; };
    static const Std kStd[] = {
        { "head.x",     "ParamAngleX",      6.0f },
        { "head.y",     "ParamAngleY",      6.0f },
        { "head.z",     "ParamAngleZ",      6.0f },
        { "body.x",     "ParamBodyAngleX",  3.0f },
        { "body.y",     "ParamBodyAngleY",  3.0f },
        { "body.z",     "ParamBodyAngleZ",  3.0f },
        { "gaze.x",     "ParamEyeBallX",   10.0f },
        { "gaze.y",     "ParamEyeBallY",   10.0f },
        { "eye.l",      "ParamEyeLOpen",   14.0f },
        { "eye.r",      "ParamEyeROpen",   14.0f },
        { "brow.l",     "ParamBrowLY",      8.0f },
        { "brow.r",     "ParamBrowRY",      8.0f },
        { "brow.l.form","ParamBrowLForm",   8.0f },
        { "brow.r.form","ParamBrowRForm",   8.0f },
        { "mouth.open", "ParamMouthOpenY", 12.0f },
        { "mouth.form", "ParamMouthForm",  12.0f },
        { "cheek",      "ParamCheek",       6.0f },
        // 标准手臂细通道：官方示例模型是两段式 ParamArmLA/LB（L/R 各一段链）
        { "arm.l.a",    "ParamArmLA",       4.0f },
        { "arm.l.b",    "ParamArmLB",       4.0f },
        { "arm.r.a",    "ParamArmRA",       4.0f },
        { "arm.r.b",    "ParamArmRB",       4.0f },
    };
    for (size_t i = 0; i < sizeof(kStd) / sizeof(kStd[0]); ++i)
    {
        std::vector<std::pair<const char*, csmFloat32> > m;

        m.push_back(std::make_pair(kStd[i].param, 1.0f));
        AddBodyChannel(kStd[i].ch, m, kStd[i].slew);
    }
    // 命名回退（各作者不同：yumi 用 ParamEyeOpenL/R、ParamBrowYL/YR…）——
    // 同名通道给多个候选参数，AddBodyChannel 只收下存在的那些
    {
        static const struct { const char* ch; const char* a; const char* b; csmFloat32 slew; } kAlt[] = {
            { "eye.l",      "ParamEyeOpenL",   "ParamEyeOpenLeft",  14.0f },
            { "eye.r",      "ParamEyeOpenR",   "ParamEyeOpenRight", 14.0f },
            { "brow.l",     "ParamBrowYL",     "ParamBrowLY2",       8.0f },
            { "brow.r",     "ParamBrowYR",     "ParamBrowRY2",       8.0f },
        };
        for (size_t i = 0; i < sizeof(kAlt) / sizeof(kAlt[0]); ++i)
        {
            std::vector<std::pair<const char*, csmFloat32> > m;

            m.push_back(std::make_pair(kAlt[i].a, 1.0f));
            m.push_back(std::make_pair(kAlt[i].b, 1.0f));
            AddBodyChannel(kAlt[i].ch, m, kAlt[i].slew);
        }
    }
    // 整臂通道 arm.l/arm.r：**有部件数据走关节骨架**（BuildArmRigs/ApplyArmRigs），否则退回参数
    // （官方两段链一起驱动；定制模型由 profile 覆盖成自己的链，如舒芙蕾 5 段）
    {
        std::vector<std::pair<const char*, csmFloat32> > m;

        m.push_back(std::make_pair("ParamArmRA", 1.0f));
        m.push_back(std::make_pair("ParamArmRB", 1.0f));
        AddBodyChannel("arm.r", m, 4.0f);
        m.clear();
        m.push_back(std::make_pair("ParamArmLA", 1.0f));
        m.push_back(std::make_pair("ParamArmLB", 1.0f));
        AddBodyChannel("arm.l", m, 4.0f);
    }
    LoadBodyChannelProfile();   // profile 的 "body" 段：覆盖/追加（如手臂链）
    LoadAuthorArmSets();        // 作者手臂套（有则本侧走作者关节链，外加骨架让位）
    BuildArmRigs();             // 手臂关节骨架 + 关节通道 + IK 锚点（有部件数据的模型）
    printf("[body] 通道 %d 条：", (int)_channels.size());
    for (size_t i = 0; i < _channels.size(); ++i)
    {
        printf("%s%s", i ? "," : "", _channels[i].name.c_str());
    }
    printf("\n");
    fflush(stdout);
}

/** profile 里的 "body" 段：{ "arm.r": { "slew":4, "params":[{"param":"ParamarmR1","scale":-1}, …] } }
 *  同名通道 = 覆盖参数映射；新名字 = 追加通道 */
void L2DPlayer::LoadBodyChannelProfile()
{
    if (_dir.empty())
    {
        return;
    }
    std::string path = _dir + "/l2d.profile.json";
    FILE* f = fopen(path.c_str(), "rb");

    if (f == NULL)
    {
        return;
    }
    std::string text;
    char buf[4096];
    size_t got;

    while ((got = fread(buf, 1, sizeof(buf), f)) > 0)
    {
        text.append(buf, got);
        if (text.size() > (1u << 20)) { break; }
    }
    fclose(f);

    jsonlite::Value root;
    std::string perr;

    if (!jsonlite::parse(text.c_str(), text.size(), root, &perr))
    {
        return;   // profile 解析失败：手势那段会报，这里静默降级
    }
    const jsonlite::Value& body = root.get("body");

    if (!body.isObj())
    {
        return;
    }
    for (size_t i = 0; i < body.obj.size(); ++i)
    {
        const std::string& chName = body.obj[i].first;
        const jsonlite::Value& cv = body.obj[i].second;

        if (!cv.isObj() || !cv.get("params").isArr())
        {
            continue;
        }
        std::vector<std::pair<const char*, csmFloat32> > maps;
        std::vector<std::string> keep;   // 保 id 字符串生命周期

        const jsonlite::Value& arr = cv.get("params");

        for (size_t k = 0; k < arr.arr.size(); ++k)
        {
            const char* pn = arr.arr[k].get("param").strOr("");

            if (*pn == '\0')
            {
                continue;
            }
            keep.push_back(pn);
            maps.push_back(std::make_pair(keep.back().c_str(),
                                          (csmFloat32)arr.arr[k].get("scale").numOr(1.0)));
        }
        if (maps.empty())
        {
            continue;
        }
        const csmFloat32 slew = (csmFloat32)cv.get("slew").numOr(4.0);
        csmInt32 preOverride = -1;
        csmInt32 rigidOverride = -1;

        if (cv.has("rigid"))
        {
            const jsonlite::Value& rv = cv.get("rigid");

            rigidOverride = rv.isBool() ? (rv.b ? 1 : 0) : -1;
        }
        if (cv.has("pre"))
        {
            preOverride = cv.get("pre").isBool() ? (cv.get("pre").b ? 1 : 0) : -1;
        }

        bool replaced = false;

        for (size_t c = 0; c < _channels.size(); ++c)
        {
            if (_channels[c].name == chName)
            {
                BodyChannel ch;

                ch.name = chName;
                ch.slew = slew;
                ch.omega = slew * kBodyOmegaK;
                for (size_t m = 0; m < maps.size(); ++m)
                {
                    const csmInt32 idx = _model->GetParameterIndex(
                        CubismFramework::GetIdManager()->GetId(maps[m].first));

                    if (idx >= 0 && idx < _model->GetParameterCount())
                    {
                        ch.params.push_back(std::make_pair(idx, maps[m].second));
                    }
                }
                if (!ch.params.empty())
                {
                    ch.rigid = (rigidOverride != 0);   // 默认 true（-1 也 true）
                    if (preOverride >= 0)
                    {
                        ch.pre = (preOverride != 0);
                    }
                    else if (!_cap.empty() && ch.params[0].first < (csmInt32)_cap.size())
                    {
                        ch.pre = (_cap[(size_t)ch.params[0].first].flags & kCapPhys) != 0;
                    }
                    _channels[c] = ch;
                    replaced = true;
                }
                break;
            }
        }
        if (!replaced)
        {
            AddBodyChannel(chName.c_str(), maps, slew, preOverride, rigidOverride);
        }
    }
}

/** 通道说明（caps 的 BODYINFO=，给 LLM 定幅度用）："名:下限..上限:方式"。
 *  方式：joint=渲染器肩关节旋转（0.45≈胸/肩 0.6≈斜上 1≈平举；负=外展）、
 *        p=经物理链（头/身，会带动头发衣服）、d=直连参数。范围=相对默认值的增量 */
void L2DPlayer::BodyInfoText(char* out, size_t cap) const
{
    size_t n = 0;

    out[0] = '\0';
    for (size_t i = 0; i < _channels.size(); ++i)
    {
        const BodyChannel& ch = _channels[i];
        const bool joint = ch.rig >= 0 || ch.rigid && _parts.find(ch.name) != _parts.end()
                          && !_parts.find(ch.name)->second.empty();
        csmFloat32 lo = -1.0f, hi = 1.0f;

        if (ch.params.empty() && ch.rig >= 0)
        {
            lo = ch.lo;   // 关节通道（度）
            hi = ch.hi;
        }
        else if (!joint && !ch.params.empty())
        {
            const csmUint32 idx = (csmUint32)ch.params[0].first;
            const csmFloat32 sc = ch.params[0].second;
            const csmFloat32 def = _model->GetParameterDefaultValue(idx);
            const csmFloat32 a = (_model->GetParameterMinimumValue(idx) - def) / (sc != 0.0f ? sc : 1.0f);
            const csmFloat32 b = (_model->GetParameterMaximumValue(idx) - def) / (sc != 0.0f ? sc : 1.0f);

            lo = a < b ? a : b;
            hi = a < b ? b : a;
        }
        const int w = snprintf(out + n, cap > n ? cap - n : 0, "%s%s:%g..%g:%s",
                               i ? "," : "", ch.name.c_str(), (double)lo, (double)hi,
                               joint ? "joint" : (ch.pre ? "p" : "d"));

        if (w < 0 || (size_t)w >= cap - n)
        {
            break;
        }
        n += (size_t)w;
    }
}

/** post 层（作者动作/scheduler 之后）调用：写关节最终值 */
void L2DPlayer::ApplyJointHolds(csmFloat32 dt)
{
    for (size_t i = 0; i < _jointHolds.size();)
    {
        JointHold& h = _jointHolds[i];
        const csmFloat32 cur = _model->GetParameterValue(h.param);

        if (!h.started)
        {
            h.value = cur;   // 从当前姿态出发
            h.started = true;
        }
        const int nSub = 1 + (int)(dt * h.omega / 0.25f);
        const csmFloat32 sub = dt / (csmFloat32)nSub;

        for (int s = 0; s < nSub; ++s)
        {
            const csmFloat32 acc = h.omega * h.omega * (h.target - h.value) - 2.0f * 0.8f * h.omega * h.vel;

            h.vel += acc * sub;
            h.value += h.vel * sub;
        }
        if (h.releasing)
        {
            h.weight -= dt / 0.35f;
            if (h.weight <= 0.0f)
            {
                _jointHolds.erase(_jointHolds.begin() + (ptrdiff_t)i);
                continue;
            }
        }
        _model->SetParameterValue(h.param, cur + (h.value - cur) * h.weight);
        ++i;
    }
    if (_poseReleaseAt > 0.0f && _perfClock >= _poseReleaseAt)
    {
        _poseReleaseAt = 0.0f;
        ReleasePose();
    }
}

/** 采样一套手臂：肩×肘 5×5 网格，每个姿势下这套手臂（根部件下所有 drawable）的轮廓 */
void L2DPlayer::SampleArmSet(ArmSet& st)
{
    st.sampled = true;
    st.samp.clear();
    if (st.show.empty())
    {
        return;
    }
    const csmInt32 root = st.show[0];
    std::vector<csmInt32> draws;

    for (csmInt32 d = 0; d < _model->GetDrawableCount(); ++d)
    {
        for (csmInt32 q = _model->GetDrawableParentPartIndex((csmUint32)d); q >= 0;
             q = _model->GetPartParentPartIndex((csmUint32)q))
        {
            if (q == root)
            {
                draws.push_back(d);
                break;
            }
        }
    }
    if (draws.empty())
    {
        return;
    }
    const csmInt32 pc = _model->GetParameterCount();
    std::vector<csmFloat32> saved((size_t)pc);

    for (csmInt32 i = 0; i < pc; ++i)
    {
        saved[(size_t)i] = _model->GetParameterValue(i);
    }
    const int N = 5;
    const csmInt32 js = st.joint[0], je = st.joint[1];

    for (int a = 0; a < (js >= 0 ? N : 1); ++a)
    {
        for (int b = 0; b < (je >= 0 ? N : 1); ++b)
        {
            csmFloat32 vs = 0.0f, ve = 0.0f;

            if (js >= 0)
            {
                const csmFloat32 lo = _model->GetParameterMinimumValue((csmUint32)js), hi = _model->GetParameterMaximumValue((csmUint32)js);
                vs = lo + (hi - lo) * (csmFloat32)a / (csmFloat32)(N - 1);
                _model->SetParameterValue(js, vs);
            }
            if (je >= 0)
            {
                const csmFloat32 lo = _model->GetParameterMinimumValue((csmUint32)je), hi = _model->GetParameterMaximumValue((csmUint32)je);
                ve = lo + (hi - lo) * (csmFloat32)b / (csmFloat32)(N - 1);
                _model->SetParameterValue(je, ve);
            }
            _model->Update();
            double cx = 0.0, cy = 0.0;
            long n = 0;
            csmFloat32 x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;

            for (size_t k = 0; k < draws.size(); ++k)
            {
                const Live2D::Cubism::Core::csmVector2* pos = _model->GetDrawableVertexPositions(draws[k]);

                for (csmInt32 v = 0; v < _model->GetDrawableVertexCount(draws[k]); ++v)
                {
                    cx += pos[v].X;
                    cy += pos[v].Y;
                    ++n;
                    x0 = pos[v].X < x0 ? pos[v].X : x0;
                    y0 = pos[v].Y < y0 ? pos[v].Y : y0;
                    x1 = pos[v].X > x1 ? pos[v].X : x1;
                    y1 = pos[v].Y > y1 ? pos[v].Y : y1;
                }
            }
            if (n == 0)
            {
                continue;
            }
            const csmFloat32 f[8] = { vs, ve, (csmFloat32)(cx / n), (csmFloat32)(cy / n), x0, y0, x1, y1 };

            st.samp.insert(st.samp.end(), f, f + 8);
        }
    }
    for (csmInt32 i = 0; i < pc; ++i)
    {
        _model->SetParameterValue(i, saved[(size_t)i]);
    }
    _model->Update();
}

/** 开始换套过渡（to = 目标套；toAuto = 过渡完交回自动）。T = 每段移动时长 */
void L2DPlayer::BeginArmTransition(size_t a, int to, const std::vector<std::pair<csmInt32, csmFloat32> >& targets, bool toAuto, csmFloat32 T)
{
    AuthorArm& arm = _authorArms[a];
    const int from = arm.shown >= 0 ? arm.shown : VisibleSet(arm);
    const ArmSet& dst = arm.sets[(size_t)to];

    arm.cur = toAuto ? -1 : to;
    RemapArmChannels(arm, to);
    T = T < 0.3f ? 0.3f : (T > 1.5f ? 1.5f : T);
    // 同一套 / 两臂共用一套（Hiyori）/ 认不出当前套：不过桥，直接关节平滑 + pose 淡化
    if (from == to || from < 0 || dst.bilateral || arm.sets[(size_t)from].show.empty() || dst.show.empty())
    {
        arm.tr.active = false;
        arm.shown = to;
        for (size_t i = 0; i < targets.size(); ++i)
        {
            HoldJoint(targets[i].first, targets[i].second, T);
        }
        if (toAuto)
        {
            for (int j = 0; j < 2; ++j)
            {
                if (dst.joint[j] >= 0) ReleaseJoint(dst.joint[j]);
            }
            ArmToAuto(arm);
        }
        return;
    }
    ArmSet& src = arm.sets[(size_t)from];
    ArmSet& dstm = arm.sets[(size_t)to];

    if (!src.sampled) SampleArmSet(src);
    if (!dstm.sampled) SampleArmSet(dstm);
    // 旧套当前关节、新套目标关节（归一化到 0..1 量程，用来罚"绕远路"）
    auto norm = [this](csmInt32 p, csmFloat32 v) {
        if (p < 0) return 0.0f;
        const csmFloat32 lo = _model->GetParameterMinimumValue((csmUint32)p), hi = _model->GetParameterMaximumValue((csmUint32)p);
        return hi > lo ? (v - lo) / (hi - lo) : 0.0f;
    };
    csmFloat32 curS = src.joint[0] >= 0 ? _model->GetParameterValue(src.joint[0]) : 0.0f;
    csmFloat32 curE = src.joint[1] >= 0 ? _model->GetParameterValue(src.joint[1]) : 0.0f;
    csmFloat32 tgtS = dst.joint[0] >= 0 ? _model->GetParameterDefaultValue((csmUint32)dst.joint[0]) : 0.0f;
    csmFloat32 tgtE = dst.joint[1] >= 0 ? _model->GetParameterDefaultValue((csmUint32)dst.joint[1]) : 0.0f;

    for (size_t i = 0; i < targets.size(); ++i)
    {
        if (targets[i].first == dst.joint[0]) tgtS = targets[i].second;
        if (targets[i].first == dst.joint[1]) tgtE = targets[i].second;
    }
    // 尺度：旧套样本包围盒对角线
    csmFloat32 L = 1.0f;

    if (src.samp.size() >= 8)
    {
        L = sqrtf((src.samp[6] - src.samp[4]) * (src.samp[6] - src.samp[4]) + (src.samp[7] - src.samp[5]) * (src.samp[7] - src.samp[5]));
        L = L > 1e-4f ? L : 1.0f;
    }
    csmFloat32 best = 1e30f;
    size_t bi = 0, bj = 0;

    for (size_t i = 0; i + 8 <= src.samp.size(); i += 8)
    {
        for (size_t j = 0; j + 8 <= dstm.samp.size(); j += 8)
        {
            const csmFloat32* p = &src.samp[i];
            const csmFloat32* q = &dstm.samp[j];
            const csmFloat32 dc = sqrtf((p[2] - q[2]) * (p[2] - q[2]) + (p[3] - q[3]) * (p[3] - q[3]));
            const csmFloat32 db = fabsf(p[4] - q[4]) + fabsf(p[5] - q[5]) + fabsf(p[6] - q[6]) + fabsf(p[7] - q[7]);
            const csmFloat32 detour = fabsf(norm(src.joint[0], p[0]) - norm(src.joint[0], curS))
                                    + fabsf(norm(src.joint[1], p[1]) - norm(src.joint[1], curE))
                                    + fabsf(norm(dst.joint[0], q[0]) - norm(dst.joint[0], tgtS))
                                    + fabsf(norm(dst.joint[1], q[1]) - norm(dst.joint[1], tgtE));
            const csmFloat32 c = (dc + 0.5f * db) / L + 0.08f * detour;

            if (c < best)
            {
                best = c;
                bi = i;
                bj = j;
            }
        }
    }
    AuthorArm::Trans& tr = arm.tr;

    tr.active = true;
    tr.from = from;
    tr.to = to;
    tr.phase = 1;
    tr.t0 = _perfClock;
    tr.T = T;
    tr.toAuto = toAuto;
    tr.targets = targets;
    tr.bridgeFrom[0] = src.samp.empty() ? curS : src.samp[bi];
    tr.bridgeFrom[1] = src.samp.empty() ? curE : src.samp[bi + 1];
    tr.bridgeTo[0] = dstm.samp.empty() ? tgtS : dstm.samp[bj];
    tr.bridgeTo[1] = dstm.samp.empty() ? tgtE : dstm.samp[bj + 1];
    arm.shown = from;
    // 第 1 段：旧手臂转到交接姿势
    if (src.joint[0] >= 0) HoldJoint(src.joint[0], tr.bridgeFrom[0], T);
    if (src.joint[1] >= 0) HoldJoint(src.joint[1], tr.bridgeFrom[1], T);
    printf("[armset] %s 过渡 %s→%s：交接姿势 旧(肩%.0f 肘%.0f) 新(肩%.0f 肘%.0f)，轮廓差 %.2f\n",
           arm.ch.c_str(), src.name.c_str(), dstm.name.c_str(), tr.bridgeFrom[0], tr.bridgeFrom[1],
           tr.bridgeTo[0], tr.bridgeTo[1], best);
    fflush(stdout);
}

/** 每帧推进过渡（pre 层、压部件之前调用）*/
void L2DPlayer::ArmTransTick()
{
    static const csmFloat32 kSwap = 0.25f;   // 交叉淡化时长（SDK pose 的 FadeInTime 一般 0.2~0.5）

    for (size_t a = 0; a < _authorArms.size(); ++a)
    {
        AuthorArm& arm = _authorArms[a];
        AuthorArm::Trans& tr = arm.tr;

        if (!tr.active)
        {
            continue;
        }
        const ArmSet& src = arm.sets[(size_t)tr.from];
        const ArmSet& dst = arm.sets[(size_t)tr.to];

        if (tr.phase == 1 && _perfClock >= tr.t0 + tr.T)
        {
            // 第 2 段：在交接姿势换手——新手臂直接就位于交接姿势，旧手臂随淡化放开
            if (dst.joint[0] >= 0) HoldJointSnap(dst.joint[0], tr.bridgeTo[0]);
            if (dst.joint[1] >= 0) HoldJointSnap(dst.joint[1], tr.bridgeTo[1]);
            arm.shown = tr.to;
            tr.phase = 2;
            tr.t0 = _perfClock;
        }
        else if (tr.phase == 2 && _perfClock >= tr.t0 + kSwap)
        {
            if (src.joint[0] >= 0) ReleaseJoint(src.joint[0]);
            if (src.joint[1] >= 0) ReleaseJoint(src.joint[1]);
            // 第 3 段：新手臂从交接姿势转到目标（没给目标的关节回作者默认值）
            bool hasS = false, hasE = false;

            for (size_t i = 0; i < tr.targets.size(); ++i)
            {
                HoldJoint(tr.targets[i].first, tr.targets[i].second, tr.T);
                hasS = hasS || tr.targets[i].first == dst.joint[0];
                hasE = hasE || tr.targets[i].first == dst.joint[1];
            }
            if (!hasS && dst.joint[0] >= 0) HoldJoint(dst.joint[0], _model->GetParameterDefaultValue((csmUint32)dst.joint[0]), tr.T);
            if (!hasE && dst.joint[1] >= 0) HoldJoint(dst.joint[1], _model->GetParameterDefaultValue((csmUint32)dst.joint[1]), tr.T);
            tr.phase = 3;
            tr.t0 = _perfClock;
        }
        else if (tr.phase == 3 && _perfClock >= tr.t0 + tr.T)
        {
            tr.active = false;
            if (tr.toAuto)
            {
                // 收回：新手臂已经回到默认姿势 → 放开关节、交回作者动作
                if (dst.joint[0] >= 0) ReleaseJoint(dst.joint[0]);
                if (dst.joint[1] >= 0) ReleaseJoint(dst.joint[1]);
                ArmToAuto(arm);
            }
        }
    }
}

/** PART 部件=不透明度 …（每帧在 pose 之前压住；切作者画的另一套手臂用）／ PART off。
 *  pose 按"组内第一个不透明度 > 0 的部件"选可见套，所以切套时要把同组其他套压成 0。
 *  返回错误说明（空=成功）*/
std::string L2DPlayer::SetPartHold(const char* args)
{
    if (!strcmp(args, "off"))
    {
        _partHold.clear();
        return std::string();
    }
    std::vector<std::pair<csmInt32, csmFloat32> > add;
    char buf[512];

    snprintf(buf, sizeof(buf), "%s", args);
    char* save = NULL;

    for (char* tok = strtok_r(buf, " ", &save); tok != NULL; tok = strtok_r(NULL, " ", &save))
    {
        char* eq = strchr(tok, '=');

        if (eq == NULL)
        {
            return std::string("格式：PART 部件=不透明度 …（或 PART off）：") + tok;
        }
        *eq = '\0';
        const csmInt32 idx = _model->GetPartIndex(CubismFramework::GetIdManager()->GetId(tok));

        if (idx < 0 || idx >= _model->GetPartCount())
        {
            return std::string("部件不存在：") + tok;
        }
        if (!strcmp(eq + 1, "off"))
        {
            add.push_back(std::make_pair(idx, -1.0f));   // 单个释放
            continue;
        }
        const csmFloat32 v = (csmFloat32)atof(eq + 1);

        add.push_back(std::make_pair(idx, v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v)));
    }
    for (size_t i = 0; i < add.size(); ++i)
    {
        for (size_t j = 0; j < _partHold.size(); ++j)
        {
            if (_partHold[j].first == add[i].first)
            {
                _partHold.erase(_partHold.begin() + (ptrdiff_t)j);
                break;
            }
        }
        if (add[i].second >= 0.0f)
        {
            _partHold.push_back(add[i]);
        }
    }
    NoteExternalActivity();
    return std::string();
}

/** 每帧推进（pre 层调用一次：动作 + 弹簧 + 动捕层，然后写"物理前"那批；post 由另一相写）*/
void L2DPlayer::ApplyBodyChannelsPre(csmFloat32 dt)
{
    _lastDt = dt;
    // 动作片段里的时间窗口（到点执行）：hand.r=锚点（IK）/ arm.r.set=手臂套
    for (size_t j = 0; j < _handEvents.size();)
    {
        if (_handEvents[j].at <= _perfClock)
        {
            const HandEvent ev = _handEvents[j];
            const size_t n = ev.ch.size();

            _handEvents.erase(_handEvents.begin() + (ptrdiff_t)j);
            if (n > 4 && ev.ch.compare(n - 4, 4, ".set") == 0)
            {
                SetArmSet(ev.ch.c_str(), ev.anchor == "off" ? "auto" : ev.anchor.c_str(), false);
            }
            else
            {
                SetHandTarget(ev.ch.c_str(), ev.anchor.c_str(), false);
            }
            continue;
        }
        ++j;
    }
    // 作者手臂套：推进换套过渡，然后压住"当前显示的套"——只设 pose 伪参数，
    // 由 SDK pose 做交叉淡化（直接压不透明度 = 硬跳）
    ArmTransTick();
    for (size_t a = 0; a < _authorArms.size(); ++a)
    {
        const AuthorArm& arm = _authorArms[a];

        if (arm.shown < 0 || (arm.cur < 0 && !arm.tr.active))
        {
            continue;   // auto：交给模型自己的 pose/动作
        }
        for (size_t k = 0; k < arm.allParts.size(); ++k)
        {
            HoldPoseRoot(arm.allParts[k], 0.0f);
        }
        const ArmSet& st = arm.sets[(size_t)arm.shown];

        for (size_t k = 0; k < st.show.size(); ++k)
        {
            HoldPoseRoot(st.show[k], 1.0f);
        }
        for (size_t k = 0; k < st.fixed.size(); ++k)
        {
            _model->SetParameterValue(st.fixed[k].first, st.fixed[k].second);   // 手型显示等
        }
    }
    // PART 指令：部件不透明度在 pose 之前压住（作者动作也是这么切手臂套的）
    for (size_t i = 0; i < _partHold.size(); ++i)
    {
        HoldPart(_partHold[i].first, _partHold[i].second);
    }
    // 1) 动作片段 → 各通道叠加层
    for (size_t i = 0; i < _channels.size(); ++i)
    {
        _channels[i].extra = 0.0f;
    }
    for (size_t j = 0; j < _liveActs.size();)
    {
        LiveActionTrack& lt = _liveActs[j];
        const csmFloat32 raw = (lt.dur > 0.0f) ? (_perfClock - lt.start) / lt.dur : 1.0f;

        if (raw < 0.0f)
        {
            ++j;
            continue;
        }
        csmFloat32 w = 1.0f;

        if (lt.fadeAt > 0.0f)
        {
            w = 1.0f - (_perfClock - lt.fadeAt) / 0.25f;
        }
        if (raw >= 1.0f || w <= 0.0f || lt.ch >= _channels.size())
        {
            _liveActs.erase(_liveActs.begin() + (ptrdiff_t)j);
            continue;
        }
        _channels[lt.ch].extra += EvalActionKeys(lt.keys, raw) * lt.scale * w;
        ++j;
    }
    if (_isPet)
    {
        EmotionTick(dt);   // 连续情绪叠加层（嘴角/眉/眼/头/脸红）
    }

    // 2) 弹簧（阻尼 0.72：有惯性、约 4% 过冲——"身体动过去"而不是"数值滑过去"）
    bool anyActive = !_liveActs.empty();

    for (size_t i = 0; i < _channels.size(); ++i)
    {
        BodyChannel& ch = _channels[i];

        if (!ch.set)
        {
            continue;
        }
        anyActive = true;
        const csmFloat32 w = ch.omega;
        const int nSub = 1 + (int)(dt * w / 0.25f);   // 大 dt（重模型 7fps）分步，保稳定
        const csmFloat32 h = dt / (csmFloat32)nSub;

        for (int s = 0; s < nSub; ++s)
        {
            const csmFloat32 acc = w * w * (ch.target - ch.value) - 2.0f * 0.72f * w * ch.vel;

            ch.vel += acc * h;
            ch.value += ch.vel * h;
        }
        if (ch.releasing && fabsf(ch.value) < 2e-3f && fabsf(ch.vel) < 2e-2f)
        {
            ch.set = false;
            ch.releasing = false;
            ch.value = ch.vel = 0.0f;
        }
    }

    // 3) 动捕层：有控制时（任一通道被设定/有动作）淡入——头身微动 + 身体跟头（滞后）
    {
        const csmFloat32 goal = (_mocapLevel > 0 && anyActive) ? 1.0f : 0.0f;
        const csmFloat32 k = dt * 1.5f;

        _mocapEnv += (goal - _mocapEnv) * (k > 1.0f ? 1.0f : k);
    }
    static const csmFloat32 kLevelGain[4] = { 0.0f, 1.0f, 1.8f, 2.6f };
    const csmFloat32 g = kLevelGain[_mocapLevel] * _mocapEnv;

    for (size_t i = 0; i < _channels.size(); ++i)
    {
        BodyChannel& ch = _channels[i];

        ch.out = (ch.set ? ch.value : 0.0f) + ch.extra;
        if (g > 0.0f)
        {
            ch.out += MocapNoiseAmp(ch.name) * g * MocapNoise(i, _perfClock);
        }
    }
    if (_mocapLevel > 0)
    {
        // 身体跟头（follow-through）：头转 → 身体滞后跟 30%（经物理再带动头发/衣服）
        static const char* const kPair[3][2] = {
            { "head.x", "body.x" }, { "head.y", "body.y" }, { "head.z", "body.z" } };
        const csmFloat32 k = dt * 3.0f;

        for (int p = 0; p < 3; ++p)
        {
            const int hi = FindBodyChannel(kPair[p][0]);
            const int bi = FindBodyChannel(kPair[p][1]);

            if (hi < 0 || bi < 0)
            {
                continue;
            }
            BodyChannel& b = _channels[(size_t)bi];
            const csmFloat32 src = _channels[(size_t)hi].out * 0.3f;

            b.follow += (src - b.follow) * (k > 1.0f ? 1.0f : k);
            b.out += b.follow;
        }
    }
    for (size_t i = 0; i < _channels.size(); ++i)
    {
        BodyChannel& ch = _channels[i];

        if (ch.pre && BodyChannelActive(ch))
        {
            WriteBodyChannel(ch);
        }
    }
}

/** 建一条手臂的骨架（静止姿态 = 当前 Core 顶点）。失败返回 false。*/
bool L2DPlayer::BuildOneArmRig(const std::string& chName, const std::vector<csmInt32>& partDraws, const jsonlite::Value& prof, ArmRig* out)
{
    ArmRig rig;

    rig.ch = chName;
    rig.side = chName.substr(chName.size() - 1);
    rig.sign = 1.0f;
    rig.bone0 = 1;
    rig.chS = rig.chE = rig.chW = -1;
    rig.nUpper = rig.nFore = rig.nHand = rig.nSplit = 0;
    rig.ik = false;
    rig.ikX = rig.ikY = rig.ikMiss = rig.ikS = rig.ikE = 0.0f;
    // 1) 只取**当前可见**那套（Haru 的 ArmRB 是另一套姿势美术，不参与）
    for (size_t i = 0; i < partDraws.size(); ++i)
    {
        const csmInt32 d = partDraws[i];

        if (d >= 0 && d < _model->GetDrawableCount() && _model->GetDrawableOpacity(d) > 0.01f
            && _model->GetDrawableVertexCount(d) > 0)
        {
            rig.draws.push_back(d);
        }
    }
    if (rig.draws.empty())
    {
        return false;
    }
    // 2) 臂轴：最高点（肩顶）→ 离它最远的点（指尖）
    csmFloat32 sx = 0.0f, sy = -1e9f;

    for (size_t i = 0; i < rig.draws.size(); ++i)
    {
        const csmInt32 d = rig.draws[i];
        const Live2D::Cubism::Core::csmVector2* pos = _model->GetDrawableVertexPositions(d);

        for (csmInt32 v = 0; v < _model->GetDrawableVertexCount(d); ++v)
        {
            if (pos[v].Y > sy) { sy = pos[v].Y; sx = pos[v].X; }
        }
    }
    csmFloat32 hx = sx, hy = sy, best = -1.0f;

    for (size_t i = 0; i < rig.draws.size(); ++i)
    {
        const csmInt32 d = rig.draws[i];
        const Live2D::Cubism::Core::csmVector2* pos = _model->GetDrawableVertexPositions(d);

        for (csmInt32 v = 0; v < _model->GetDrawableVertexCount(d); ++v)
        {
            const csmFloat32 dd = (pos[v].X - sx) * (pos[v].X - sx) + (pos[v].Y - sy) * (pos[v].Y - sy);

            if (dd > best) { best = dd; hx = pos[v].X; hy = pos[v].Y; }
        }
    }
    const csmFloat32 L = sqrtf(best);

    if (L < 1e-4f)
    {
        return false;
    }
    const csmFloat32 ax = (hx - sx) / L, ay = (hy - sy) / L;
    auto tOf = [&](csmFloat32 x, csmFloat32 y) { return ((x - sx) * ax + (y - sy) * ay) / L; };
    auto pOf = [&](csmFloat32 x, csmFloat32 y) { return ((x - sx) * -ay + (y - sy) * ax) / L; };
    // 3) 每张网格在臂轴上的范围
    const size_t nd = rig.draws.size();
    std::vector<csmFloat32> tmin(nd, 1e9f), tmax(nd, -1e9f), tmean(nd, 0.0f);
    std::vector<bool> isHand(nd, false);

    for (size_t i = 0; i < nd; ++i)
    {
        const csmInt32 d = rig.draws[i];
        const csmInt32 vc = _model->GetDrawableVertexCount(d);
        const Live2D::Cubism::Core::csmVector2* pos = _model->GetDrawableVertexPositions(d);
        double acc = 0.0;

        for (csmInt32 v = 0; v < vc; ++v)
        {
            const csmFloat32 t = tOf(pos[v].X, pos[v].Y);

            tmin[i] = t < tmin[i] ? t : tmin[i];
            tmax[i] = t > tmax[i] ? t : tmax[i];
            acc += t;
        }
        tmean[i] = (csmFloat32)(acc / (double)vc);
    }
    // 4) 关节位置：手 = 整张都在远端（tmin ≥ 0.55）的网格；腕 = 手部网格的起点；
    //    肘 = 网格分界（上臂网格的末端与前臂网格的起点之间），没有分界就按解剖比例
    csmFloat32 tW = 1e9f;

    for (size_t i = 0; i < nd; ++i)
    {
        if (tmin[i] >= 0.55f)
        {
            isHand[i] = true;
            tW = tmin[i] < tW ? tmin[i] : tW;
        }
    }
    if (tW > 1.0f)
    {
        tW = 0.80f;
    }
    csmFloat32 tB = 1e9f, tU = -1e9f;

    for (size_t i = 0; i < nd; ++i)
    {
        if (isHand[i])
        {
            continue;
        }
        if (tmin[i] < 0.10f)
        {
            const csmFloat32 e = tmax[i] < tW ? tmax[i] : tW;

            tU = e > tU ? e : tU;
        }
        else if (tmin[i] >= 0.25f && tmin[i] <= tW - 0.10f)
        {
            tB = tmin[i] < tB ? tmin[i] : tB;
        }
    }
    csmFloat32 tE = (tB < 1.0f && tU > 0.0f) ? 0.5f * (tB + tU) : tW * 0.54f;

    if (tE < 0.25f) tE = 0.25f;
    if (tE > tW - 0.15f) tE = tW - 0.15f;
    // profile 覆盖："skeleton":{"arm.r":{"elbowT":0.4,"wristT":0.7,"band":0.04,"shoulderBand":0.06,
    //                                     "limits":{"shoulder":[-40,95],"elbow":[-150,150],"wrist":[-45,45]}}}
    const jsonlite::Value& sk = prof.get("skeleton").get(chName.c_str());
    tE = (csmFloat32)sk.get("elbowT").numOr(tE);
    tW = (csmFloat32)sk.get("wristT").numOr(tW);
    const csmFloat32 band = (csmFloat32)sk.get("band").numOr(0.04);
    const csmFloat32 bandS = (csmFloat32)sk.get("shoulderBand").numOr(0.06);
    static const csmFloat32 kLim[3][2] = { { -40.0f, 95.0f }, { -150.0f, 150.0f }, { -45.0f, 45.0f } };
    static const char* const kJ[3] = { "shoulder", "elbow", "wrist" };

    for (int j = 0; j < 3; ++j)
    {
        const jsonlite::Value& lv = sk.get("limits").get(kJ[j]);

        rig.lim[j][0] = (lv.isArr() && lv.arr.size() >= 2) ? (csmFloat32)lv.arr[0].numOr(kLim[j][0]) : kLim[j][0];
        rig.lim[j][1] = (lv.isArr() && lv.arr.size() >= 2) ? (csmFloat32)lv.arr[1].numOr(kLim[j][1]) : kLim[j][1];
    }
    // 5) 肩关节球心：肩顶往下半个臂宽（截面在 t≈0.08 处量臂宽）
    csmFloat32 pLo = 1e9f, pHi = -1e9f;

    for (size_t i = 0; i < nd; ++i)
    {
        if (isHand[i]) continue;
        const csmInt32 d = rig.draws[i];
        const Live2D::Cubism::Core::csmVector2* pos = _model->GetDrawableVertexPositions(d);

        for (csmInt32 v = 0; v < _model->GetDrawableVertexCount(d); ++v)
        {
            if (fabsf(tOf(pos[v].X, pos[v].Y) - 0.08f) < 0.03f)
            {
                const csmFloat32 p = pOf(pos[v].X, pos[v].Y);

                pLo = p < pLo ? p : pLo;
                pHi = p > pHi ? p : pHi;
            }
        }
    }
    csmFloat32 tS = (pHi > pLo) ? 0.5f * (pHi - pLo) : 0.05f;

    if (tS < 0.03f) tS = 0.03f;
    if (tS > 0.15f) tS = 0.15f;
    rig.tS = tS;
    rig.tE = tE;
    rig.tW = tW;
    // 6) 关节锚点 = 截面上的那圈顶点（非手部网格）；腕找不到就用手部网格根部
    auto collect = [&](JointAnchor* ja, csmFloat32 tc, csmFloat32 win, bool handOnly)
    {
        for (size_t i = 0; i < nd; ++i)
        {
            if (isHand[i] != handOnly) continue;
            const csmInt32 d = rig.draws[i];
            const Live2D::Cubism::Core::csmVector2* pos = _model->GetDrawableVertexPositions(d);

            for (csmInt32 v = 0; v < _model->GetDrawableVertexCount(d); ++v)
            {
                if (fabsf(tOf(pos[v].X, pos[v].Y) - tc) < win)
                {
                    ja->verts.push_back(std::make_pair(d, v));
                }
            }
        }
    };
    collect(&rig.sh, tS, 0.03f, false);
    if (rig.sh.verts.empty())
    {
        rig.sh.ox = sx + ax * tS * L;
        rig.sh.oy = sy + ay * tS * L;
    }
    csmFloat32 shx, shy;

    AnchorPos(rig.sh, 0.0f, 0.0f, &shx, &shy);
    collect(&rig.el, tE, 0.03f, false);
    if (rig.el.verts.empty())
    {
        rig.el.ox = sx + ax * tE * L - shx;
        rig.el.oy = sy + ay * tE * L - shy;
    }
    collect(&rig.wr, tW, 0.03f, false);
    if (rig.wr.verts.empty())
    {
        collect(&rig.wr, tW + 0.03f, 0.04f, true);
    }
    if (rig.wr.verts.empty())
    {
        rig.wr.ox = sx + ax * tW * L - shx;
        rig.wr.oy = sy + ay * tW * L - shy;
    }
    for (size_t i = 0; i < nd; ++i)
    {
        if (!isHand[i]) continue;
        for (csmInt32 v = 0; v < _model->GetDrawableVertexCount(rig.draws[i]); ++v)
        {
            rig.hand.verts.push_back(std::make_pair(rig.draws[i], v));
        }
    }
    if (rig.hand.verts.empty())
    {
        rig.hand.ox = sx + ax * 0.92f * L - shx;
        rig.hand.oy = sy + ay * 0.92f * L - shy;
    }
    // 7) 蒙皮权重：整张网格不跨关节（越界 <0.15）→ 整张刚性跟一段（Haru 这种关节在网格之间
    //    的，零过渡 = 纯铰链）；跨关节的（shuflle 上臂+前臂一张）→ 按顶点切，只在关节 ±band 过渡
    rig.skin.resize(nd);
    for (size_t i = 0; i < nd; ++i)
    {
        const csmInt32 d = rig.draws[i];
        const csmInt32 vc = _model->GetDrawableVertexCount(d);
        const Live2D::Cubism::Core::csmVector2* pos = _model->GetDrawableVertexPositions(d);
        const unsigned char U = (unsigned char)rig.bone0, F = (unsigned char)(rig.bone0 + 1),
                            H = (unsigned char)(rig.bone0 + 2);
        int seg;   // 0=上臂 1=前臂 2=手 -1=按顶点切

        if (isHand[i])
        {
            seg = 2;
        }
        else
        {
            const int c = tmean[i] < tE ? 0 : (tmean[i] < tW ? 1 : 2);
            const csmFloat32 lo = c == 0 ? -1e9f : (c == 1 ? tE : tW);
            const csmFloat32 hi = c == 0 ? tE : (c == 1 ? tW : 1e9f);

            seg = (tmin[i] >= lo - 0.15f && tmax[i] <= hi + 0.15f) ? c : -1;
        }
        seg == 0 ? ++rig.nUpper : (seg == 1 ? ++rig.nFore : (seg == 2 ? ++rig.nHand : ++rig.nSplit));
        rig.skin[i].resize((size_t)vc);
        for (csmInt32 v = 0; v < vc; ++v)
        {
            const csmFloat32 t = tOf(pos[v].X, pos[v].Y);
            CubismRenderer_Software::SkinW w;

            if (seg >= 0)
            {
                w.b0 = w.b1 = (unsigned char)(rig.bone0 + seg);
                w.w = 0.0f;
            }
            else if (t < tE - band)
            {
                w.b0 = w.b1 = U; w.w = 0.0f;
            }
            else if (t < tE + band)
            {
                w.b0 = U; w.b1 = F; w.w = SmoothStep(tE - band, tE + band, t);
            }
            else if (t < tW - band)
            {
                w.b0 = w.b1 = F; w.w = 0.0f;
            }
            else if (t < tW + band)
            {
                w.b0 = F; w.b1 = H; w.w = SmoothStep(tW - band, tW + band, t);
            }
            else
            {
                w.b0 = w.b1 = H; w.w = 0.0f;
            }
            // 肩部过渡带：上臂顶端一小段部分留给躯干（切口贴住身体，不"从袖子里飞出来"）
            if (w.b0 == U && w.b1 == U && t < tS + bandS)
            {
                w.b0 = 0;
                w.b1 = U;
                w.w = SmoothStep(0.0f, tS + bandS, t);
            }
            rig.skin[i][(size_t)v] = w;
        }
    }
    *out = rig;
    return true;
}

/** 偏移参数（按序号）→ 动了的顶点的质心；空/全无效返回 false */
bool L2DPlayer::MovingCentroidIdx(const std::vector<csmInt32>& idxs, bool toMin, csmFloat32* cx, csmFloat32* cy)
{
    std::vector<std::vector<csmFloat32> > base, moved;
    std::vector<std::pair<csmInt32, csmFloat32> > saved;

    for (size_t k = 0; k < idxs.size(); ++k)
    {
        const csmInt32 idx = idxs[k];

        if (idx >= 0 && idx < (csmInt32)_model->GetParameterCount())
        {
            saved.push_back(std::make_pair(idx, _model->GetParameterValue(idx)));
        }
    }
    if (saved.empty())
    {
        return false;
    }
    _model->Update();
    SnapshotVerts(&base);
    for (size_t k = 0; k < saved.size(); ++k)
    {
        const csmInt32 idx = saved[k].first;

        _model->SetParameterValue(idx, toMin ? _model->GetParameterMinimumValue((csmUint32)idx)
                                             : _model->GetParameterMaximumValue((csmUint32)idx));
    }
    _model->Update();
    SnapshotVerts(&moved);
    for (size_t k = 0; k < saved.size(); ++k)
    {
        _model->SetParameterValue(saved[k].first, saved[k].second);
    }
    _model->Update();
    double ax = 0.0, ay = 0.0;
    long n = 0;

    for (size_t d = 0; d < base.size(); ++d)
    {
        for (size_t v = 0; v + 1 < base[d].size(); v += 2)
        {
            if (fabsf(moved[d][v] - base[d][v]) + fabsf(moved[d][v + 1] - base[d][v + 1]) > 1e-4f)
            {
                ax += base[d][v];
                ay += base[d][v + 1];
                ++n;
            }
        }
    }
    if (n == 0)
    {
        return false;
    }
    *cx = (csmFloat32)(ax / (double)n);
    *cy = (csmFloat32)(ay / (double)n);
    return true;
}

/** 无刚性骨架时的锚点：脸/下颌用参数质心，腰/胸口按包围盒比例，手用作者手参数质心 */
void L2DPlayer::BuildFallbackAnchors()
{
    static const char* const kEyes[] = { "ParamEyeLOpen", "ParamEyeROpen", "ParamEyeOpenL", "ParamEyeOpenR" };
    static const char* const kMouth[] = { "ParamMouthOpenY", "ParamJawOpen", "ParamJawOpenBL" };
    _hasFace = MovingCentroid(kEyes, 4, true, &_eyeX, &_eyeY);
    _hasFace = MovingCentroid(kMouth, 3, false, &_mouthX, &_mouthY) && _hasFace;
    csmFloat32 bb[4];
    ComputeArtBBox(bb);
    const csmFloat32 cx = (bb[0] + bb[2]) * 0.5f;
    const csmFloat32 H = bb[3] - bb[1] > 1.0f ? bb[3] - bb[1] : 1.0f;
    const csmFloat32 ex = _hasFace ? _eyeX : cx;
    const csmFloat32 ey = _hasFace ? _eyeY : bb[1] + H * 0.14f;
    const csmFloat32 mx = _hasFace ? _mouthX : ex;
    const csmFloat32 my = _hasFace ? _mouthY : bb[1] + H * 0.20f;
    const csmFloat32 chinY = my + H * 0.05f;
    _anchors["mouth"] = std::make_pair(mx, my);
    _anchors["eyes"] = std::make_pair(ex, ey);
    _anchors["face"] = std::make_pair(0.5f * (ex + mx), 0.5f * (ey + my));
    _anchors["chin"] = std::make_pair(mx, chinY);
    _anchors["head_top"] = std::make_pair(ex, ey - H * 0.13f);
    const csmFloat32 bodyBot = bb[3] > chinY + 1.0f ? bb[3] : chinY + H * 0.5f;
    _anchors["chest"] = std::make_pair(cx, chinY + (bodyBot - chinY) * 0.16f);
    _anchors["forward"] = std::make_pair(cx, chinY + (bodyBot - chinY) * 0.28f);
    _anchors["waist"] = std::make_pair(cx, chinY + (bodyBot - chinY) * 0.40f);
    // 手部锚点：作者手参数质心（有 hand.l/hand.r 通道才建；肘部无骨架 → 无肘分区）
    static const char* const kHandCh[2] = { "hand.l", "hand.r" };
    for (int k = 0; k < 2; ++k)
    {
        const int ci = FindBodyChannel(kHandCh[k]);
        if (ci < 0 || _channels[(size_t)ci].params.empty())
        {
            continue;
        }
        std::vector<csmInt32> idxs;
        for (size_t i = 0; i < _channels[(size_t)ci].params.size(); ++i)
        {
            idxs.push_back(_channels[(size_t)ci].params[i].first);
        }
        csmFloat32 hx = 0.0f, hy = 0.0f;
        if (MovingCentroidIdx(idxs, false, &hx, &hy))
        {
            _anchors[kHandCh[k]] = std::make_pair(hx, hy);
        }
    }
}

/** 偏移参数 → 动了的顶点的质心（定位嘴/眼用；参数不存在返回 false）*/
bool L2DPlayer::MovingCentroid(const char* const* ids, int nIds, bool toMin, csmFloat32* cx, csmFloat32* cy)
{
    std::vector<std::vector<csmFloat32> > base, moved;
    std::vector<std::pair<csmInt32, csmFloat32> > saved;

    for (int k = 0; k < nIds; ++k)
    {
        const csmInt32 idx = _model->GetParameterIndex(CubismFramework::GetIdManager()->GetId(ids[k]));

        if (idx >= 0)
        {
            saved.push_back(std::make_pair(idx, _model->GetParameterValue(idx)));
        }
    }
    if (saved.empty())
    {
        return false;
    }
    _model->Update();
    SnapshotVerts(&base);
    for (size_t k = 0; k < saved.size(); ++k)
    {
        const csmInt32 idx = saved[k].first;

        _model->SetParameterValue(idx, toMin ? _model->GetParameterMinimumValue((csmUint32)idx)
                                             : _model->GetParameterMaximumValue((csmUint32)idx));
    }
    _model->Update();
    SnapshotVerts(&moved);
    for (size_t k = 0; k < saved.size(); ++k)
    {
        _model->SetParameterValue(saved[k].first, saved[k].second);
    }
    _model->Update();
    double ax = 0.0, ay = 0.0;
    long n = 0;

    for (size_t d = 0; d < base.size(); ++d)
    {
        for (size_t v = 0; v + 1 < base[d].size(); v += 2)
        {
            if (fabsf(moved[d][v] - base[d][v]) + fabsf(moved[d][v + 1] - base[d][v + 1]) > 1e-4f)
            {
                ax += base[d][v];
                ay += base[d][v + 1];
                ++n;
            }
        }
    }
    if (n == 0)
    {
        return false;
    }
    *cx = (csmFloat32)(ax / (double)n);
    *cy = (csmFloat32)(ay / (double)n);
    return true;
}

/** 装载时建骨架（通道建好之后调用）：每条手臂一份 + 关节通道 + 命名锚点 */
void L2DPlayer::BuildArmRigs()
{
    _rigs.clear();
    _anchors.clear();
    _rigSkinSent = false;
    if (_parts.empty())
    {
        return;
    }
    jsonlite::Value prof;

    LoadProfileJson(&prof);
    _model->Update();   // 当前参数（装载后 = 默认姿态）落到顶点 = 静止姿态
    static const char* const kArms[2] = { "arm.r", "arm.l" };

    for (int k = 0; k < 2; ++k)
    {
        const int ci = FindBodyChannel(kArms[k]);
        std::map<std::string, std::vector<csmInt32> >::const_iterator it = _parts.find(kArms[k]);

        if (ci < 0 || !_channels[(size_t)ci].rigid || it == _parts.end())
        {
            continue;   // 作者绑好手臂的模型（profile rigid:false，如 yumi）走作者参数
        }
        ArmRig rig;

        if (BuildOneArmRig(kArms[k], it->second, prof, &rig))
        {
            RemapRigBones(&rig, 1 + 3 * (csmInt32)_rigs.size());
            _rigs.push_back(rig);
        }
    }
    if (_rigs.empty())
    {
        // 作者绑定模型（profile rigid:false，如 yumi）：没有刚性骨架可定位手/肘，
        // 锚点退化为"几何帧"（脸/下颌用参数质心、腰胸按包围盒比例、手用作者手参数质心）。
        // 手/肘分区：手可用、肘不可用——其余 14 区照常。
        BuildFallbackAnchors();
        printf("[zones] 无刚性臂骨架（作者绑定模型）：分区按几何帧推算（肘部无分区）\n");
        fflush(stdout);
        return;
    }
    // 面部锚点（嘴/眼）：偏移嘴/眼参数看哪些顶点动——装载时做一次（与姿势无关）
    static const char* const kEyes[] = { "ParamEyeLOpen", "ParamEyeROpen", "ParamEyeOpenL", "ParamEyeOpenR" };
    static const char* const kMouth[] = { "ParamMouthOpenY", "ParamJawOpen", "ParamJawOpenBL" };

    _hasFace = MovingCentroid(kEyes, 4, true, &_eyeX, &_eyeY);
    _hasFace = MovingCentroid(kMouth, 3, false, &_mouthX, &_mouthY) && _hasFace;
    ComputeRigFrame();

    // 关节通道：arm.r.shoulder / .elbow / .wrist（度；不写参数，由骨架消费）
    for (size_t r = 0; r < _rigs.size(); ++r)
    {
        ArmRig& rig = _rigs[r];
        static const char* const kJ[3] = { "shoulder", "elbow", "wrist" };
        static const csmFloat32 kSlew[3] = { 4.0f, 5.0f, 6.0f };

        _channels[(size_t)FindBodyChannel(rig.ch.c_str())].rig = (csmInt32)r;
        for (int j = 0; j < 3; ++j)
        {
            BodyChannel ch;

            ch.name = rig.ch + "." + kJ[j];
            ch.slew = kSlew[j];
            ch.omega = kSlew[j] * kBodyOmegaK;
            ch.rig = (csmInt32)r;
            ch.lo = rig.lim[j][0];
            ch.hi = rig.lim[j][1];
            _channels.push_back(ch);
            (j == 0 ? rig.chS : (j == 1 ? rig.chE : rig.chW)) = (csmInt32)_channels.size() - 1;
        }
        printf("[rig] %s：上臂 %d / 前臂 %d / 手 %d 张整网格刚性，%d 张按顶点切（关节 t：肩 %.2f 肘 %.2f 腕 %.2f）%s\n",
               rig.ch.c_str(), rig.nUpper, rig.nFore, rig.nHand, rig.nSplit, rig.tS, rig.tE, rig.tW,
               rig.sign > 0 ? "" : "（镜像）");
    }
    printf("[rig] 锚点：%s（嘴/眼%s）\n", "mouth,face,eyes,chin,head_top,chest,forward,waist + 每臂 hip/side/up/head_side/cheek",
           _hasFace ? "按参数定位" : "缺参数，按肩线估");
    fflush(stdout);
}

/** 中线、左右、身体锚点（依赖肩/肘位置；骨架几何重建后重算）*/
void L2DPlayer::ComputeRigFrame()
{
    csmFloat32 shX[2] = { 0, 0 }, shY[2] = { 0, 0 };

    for (size_t r = 0; r < _rigs.size() && r < 2; ++r)
    {
        AnchorPos(_rigs[r].sh, 0.0f, 0.0f, &shX[r], &shY[r]);
    }
    // 中线：眼睛中点（有面部参数时）> 两肩中点 > 0
    _rigMidX = _hasFace ? _eyeX : (_rigs.size() == 2 ? 0.5f * (shX[0] + shX[1]) : 0.0f);
    for (size_t r = 0; r < _rigs.size() && r < 2; ++r)
    {
        _rigs[r].sign = (shX[r] >= _rigMidX) ? 1.0f : -1.0f;
    }
    csmFloat32 l1 = 0.0f;
    {
        csmFloat32 elx, ely;

        AnchorPos(_rigs[0].el, shX[0], shY[0], &elx, &ely);
        l1 = sqrtf((elx - shX[0]) * (elx - shX[0]) + (ely - shY[0]) * (ely - shY[0]));
    }
    const csmFloat32 midShY = _rigs.size() == 2 ? 0.5f * (shY[0] + shY[1]) : shY[0];
    csmFloat32 ex = _eyeX, ey = _eyeY, mx = _mouthX, my = _mouthY;

    if (!_hasFace)
    {
        // 没有面部参数：按肩线粗估（脖子在两肩中点上方）
        mx = ex = _rigMidX;
        my = midShY + 0.55f * l1;
        ey = my + 0.30f * l1;
    }
    const csmFloat32 fx = ex - mx, fy = ey - my;   // 嘴→眼
    _anchors["mouth"] = std::make_pair(mx, my);
    _anchors["face"] = std::make_pair(0.5f * (ex + mx), 0.5f * (ey + my));
    _anchors["eyes"] = std::make_pair(ex, ey);
    _anchors["chin"] = std::make_pair(mx - fx * 0.8f, my - fy * 0.8f);
    _anchors["head_top"] = std::make_pair(ex + fx * 1.8f, ey + fy * 1.8f);
    _anchors["chest"] = std::make_pair(_rigMidX, midShY - 0.45f * l1);
    _anchors["forward"] = std::make_pair(_rigMidX, midShY - 0.75f * l1);
    _anchors["waist"] = std::make_pair(_rigMidX, midShY - 1.15f * l1);
    _rigUnit = sqrtf(fx * fx + fy * fy);
}

/** 命名锚点 → 坐标（每臂相关的按左右解析）*/
bool L2DPlayer::ResolveAnchor(const ArmRig& rig, const std::string& name, csmFloat32* x, csmFloat32* y) const
{
    std::map<std::string, std::pair<csmFloat32, csmFloat32> >::const_iterator it = _anchors.find(name);

    if (it != _anchors.end())
    {
        *x = it->second.first;
        *y = it->second.second;
        return true;
    }
    csmFloat32 sx, sy, ex, ey, wx, wy, hx, hy;

    AnchorPos(rig.sh, 0.0f, 0.0f, &sx, &sy);
    AnchorPos(rig.el, sx, sy, &ex, &ey);
    AnchorPos(rig.wr, sx, sy, &wx, &wy);
    AnchorPos(rig.hand, sx, sy, &hx, &hy);
    const csmFloat32 l1 = sqrtf((ex - sx) * (ex - sx) + (ey - sy) * (ey - sy));
    const csmFloat32 reach = l1 + sqrtf((hx - ex) * (hx - ex) + (hy - ey) * (hy - ey));
    const csmFloat32 o = rig.sign;
    std::map<std::string, std::pair<csmFloat32, csmFloat32> >::const_iterator eyes = _anchors.find("eyes");
    std::map<std::string, std::pair<csmFloat32, csmFloat32> >::const_iterator face = _anchors.find("face");

    if (name == "hip")        { *x = sx + o * 0.10f * l1; *y = sy - 1.15f * l1; return true; }
    if (name == "side")       { *x = sx + o * 0.95f * reach; *y = sy; return true; }
    if (name == "up")         { *x = sx + o * 0.25f * l1; *y = sy + 0.92f * reach; return true; }
    if (name == "head_side" && eyes != _anchors.end())
    {
        *x = eyes->second.first + o * 2.4f * _rigUnit;
        *y = eyes->second.second + 0.5f * _rigUnit;
        return true;
    }
    if (name == "cheek" && face != _anchors.end())
    {
        *x = face->second.first + o * 1.3f * _rigUnit;
        *y = face->second.second - 0.6f * _rigUnit;
        return true;
    }
    return false;
}

/** IK：手心放到 (tx,ty)。解出肩/肘（通道约定，度）；返回够不着的距离（占整臂长的比例）*/
csmFloat32 L2DPlayer::SolveArmIK(const ArmRig& rig, csmFloat32 tx, csmFloat32 ty, csmFloat32* shoulder, csmFloat32* elbow) const
{
    csmFloat32 sx, sy, ex, ey, hx, hy;

    AnchorPos(rig.sh, 0.0f, 0.0f, &sx, &sy);
    AnchorPos(rig.el, sx, sy, &ex, &ey);
    AnchorPos(rig.hand, sx, sy, &hx, &hy);
    const csmFloat32 l1 = sqrtf((ex - sx) * (ex - sx) + (ey - sy) * (ey - sy));
    const csmFloat32 l2 = sqrtf((hx - ex) * (hx - ex) + (hy - ey) * (hy - ey));
    csmFloat32 dx = tx - sx, dy = ty - sy;
    csmFloat32 d = sqrtf(dx * dx + dy * dy);
    const csmFloat32 dMin = fabsf(l1 - l2) + 1e-4f, dMax = l1 + l2 - 1e-4f;
    const csmFloat32 dc = d < dMin ? dMin : (d > dMax ? dMax : d);
    csmFloat32 cosA = (l1 * l1 + dc * dc - l2 * l2) / (2.0f * l1 * dc);

    cosA = cosA > 1.0f ? 1.0f : (cosA < -1.0f ? -1.0f : cosA);
    const csmFloat32 a = acosf(cosA);
    const csmFloat32 base = atan2f(dy, dx);
    const csmFloat32 kDeg = 57.2957795f;
    // 两个解都按关节上限钳过、正向算出手心误差，再选：
    //   ① 够得着（误差小）优先；② 肘不越过身体中线；③ 肘更低（真人托腮/摸头时肘自然下垂）
    const csmFloat32 u0 = atan2f(ey - sy, ex - sx), f0 = atan2f(hy - ey, hx - ex);
    const csmFloat32 tgx = sx + dx * (dc / (d > 1e-6f ? d : 1e-6f));
    const csmFloat32 tgy = sy + dy * (dc / (d > 1e-6f ? d : 1e-6f));
    csmFloat32 bestScore = -1e30f, bestMiss = 0.0f;

    for (int sg = -1; sg <= 1; sg += 2)
    {
        const csmFloat32 u = base + (csmFloat32)sg * a;
        const csmFloat32 e2x = sx + l1 * cosf(u), e2y = sy + l1 * sinf(u);
        const csmFloat32 thS = WrapDeg((u - u0) * kDeg);   // 数学角（逆时针）
        const csmFloat32 phi = WrapDeg((atan2f(tgy - e2y, tgx - e2x) - f0) * kDeg);
        const csmFloat32 thE = WrapDeg(phi - thS);
        // 通道约定：肩 正=外展（sign=+1 时就是逆时针）；肘 正=内折（sign=+1 时是顺时针）
        csmFloat32 sh = rig.sign * thS, el = -rig.sign * thE;

        sh = sh < rig.lim[0][0] ? rig.lim[0][0] : (sh > rig.lim[0][1] ? rig.lim[0][1] : sh);
        el = el < rig.lim[1][0] ? rig.lim[1][0] : (el > rig.lim[1][1] ? rig.lim[1][1] : el);
        // 钳过之后正向算手心
        const csmFloat32 us = u0 + rig.sign * sh / kDeg;
        const csmFloat32 fx = sx + l1 * cosf(us), fy = sy + l1 * sinf(us);
        const csmFloat32 fa = f0 + rig.sign * sh / kDeg - rig.sign * el / kDeg;
        const csmFloat32 px = fx + l2 * cosf(fa), py = fy + l2 * sinf(fa);
        const csmFloat32 miss = sqrtf((px - tx) * (px - tx) + (py - ty) * (py - ty));
        const csmFloat32 out = rig.sign * (fx - _rigMidX);
        const csmFloat32 score = -miss / (0.05f * l1)                  // 误差：每 5% 上臂长扣 1 分
                                 + (out > 0.25f * l1 ? 3.0f : 0.0f)    // 肘在外侧
                                 - fy / l1;                            // 肘低者优

        if (score > bestScore)
        {
            bestScore = score;
            bestMiss = miss;
            *shoulder = sh;
            *elbow = el;
        }
    }
    return bestMiss / (l1 + l2 > 1e-6f ? l1 + l2 : 1e-6f);   // 按臂长归一（0=正好够到）
}

/** hand.r / hand.l 指令："mouth" / "off" / "x,y"（模型坐标）。错误时返回说明（空=成功）*/
std::string L2DPlayer::SetHandTarget(const char* chName, const char* val, bool dryRun)
{
    const std::string side = (strlen(chName) > 5) ? std::string(chName + 5) : std::string();
    ArmRig* rig = NULL;

    for (size_t r = 0; r < _rigs.size(); ++r)
    {
        if (_rigs[r].side == side)
        {
            rig = &_rigs[r];
        }
    }
    if (rig == NULL)
    {
        return std::string("本模型没有 arm.") + side + " 骨架";
    }
    if (!strcmp(val, "off"))
    {
        if (!dryRun)
        {
            ReleaseHand(*rig);
        }
        return std::string();
    }
    csmFloat32 x, y;

    if (strchr(val, ',') != NULL)
    {
        x = (csmFloat32)atof(val);
        y = (csmFloat32)atof(strchr(val, ',') + 1);
        if (!dryRun)
        {
            rig->ikName.clear();
            rig->ikX = x;
            rig->ikY = y;
        }
    }
    else
    {
        if (!ResolveAnchor(*rig, val, &x, &y))
        {
            return std::string("锚点不认：") + val + "（LIST 的 ANCHORS=）";
        }
        if (!dryRun)
        {
            rig->ikName = val;
        }
    }
    if (!dryRun)
    {
        if (!_rigGeomFresh)
        {
            // 回复里的解要用"当前可见"的骨架（装载时那份可能混着隐藏的另一套手臂）
            RefreshRigGeometry();
            if (!rig->ikName.empty())
            {
                ResolveAnchor(*rig, rig->ikName, &x, &y);
            }
        }
        rig->ik = true;
        rig->ikMiss = SolveArmIK(*rig, x, y, &rig->ikS, &rig->ikE);
        NoteExternalActivity();
    }
    return std::string();
}

/** 每帧：IK → 关节目标；FK → 骨变换 → 渲染器蒙皮。返回本帧是否有手臂在动 */
csmBool L2DPlayer::ApplyArmRigs(CubismRenderer_Software* r)
{
    if (_rigs.empty() || r == NULL)
    {
        return false;
    }
    if (!_rigGeomFresh)
    {
        bool want = false;

        for (size_t k = 0; k < _rigs.size() && !want; ++k)
        {
            const ArmRig& rig = _rigs[k];

            want = rig.ik || BodyChannelActive(_channels[(size_t)FindBodyChannel(rig.ch.c_str())])
                   || BodyChannelActive(_channels[(size_t)rig.chS])
                   || BodyChannelActive(_channels[(size_t)rig.chE])
                   || BodyChannelActive(_channels[(size_t)rig.chW]);
        }
        if (!want)
        {
            return false;
        }
        RefreshRigGeometry();
    }
    const csmFloat32 kRad = 0.0174532925f;
    std::vector<csmFloat32> bones((size_t)(1 + 3 * _rigs.size()) * 6, 0.0f);
    bool any = false;

    bones[0] = 1.0f;
    bones[3] = 1.0f;   // 骨 0 = 恒等（躯干）
    for (size_t k = 0; k < _rigs.size(); ++k)
    {
        ArmRig& rig = _rigs[k];

        // IK：每帧按当前关节位置重解（跟得上呼吸/摆动），结果作为肩/肘通道的目标
        if (rig.ik)
        {
            csmFloat32 tx = rig.ikX, ty = rig.ikY;

            if (rig.ikName.empty() || ResolveAnchor(rig, rig.ikName, &tx, &ty))
            {
                rig.ikMiss = SolveArmIK(rig, tx, ty, &rig.ikS, &rig.ikE);
                BodyChannel& cs = _channels[(size_t)rig.chS];
                BodyChannel& ce = _channels[(size_t)rig.chE];

                cs.target = rig.ikS;
                ce.target = rig.ikE;
                cs.set = ce.set = true;
                cs.releasing = ce.releasing = false;
            }
        }
        const BodyChannel& legacy = _channels[(size_t)FindBodyChannel(rig.ch.c_str())];
        const BodyChannel& cs = _channels[(size_t)rig.chS];
        const BodyChannel& ce = _channels[(size_t)rig.chE];
        const BodyChannel& cw = _channels[(size_t)rig.chW];
        const bool active = BodyChannelActive(legacy) || BodyChannelActive(cs)
                            || BodyChannelActive(ce) || BodyChannelActive(cw);
        csmFloat32 lv = legacy.out > 1.1f ? 1.1f : (legacy.out < -1.1f ? -1.1f : legacy.out);
        csmFloat32 shoulder = lv * (lv >= 0.0f ? 90.0f : 60.0f) + cs.out;
        csmFloat32 elbow = ce.out, wrist = cw.out;

        // 关节上限（弹簧过冲/叠加后再钳一次，留 5° 余量给过冲）
        shoulder = shoulder < rig.lim[0][0] - 5 ? rig.lim[0][0] - 5 : (shoulder > rig.lim[0][1] + 5 ? rig.lim[0][1] + 5 : shoulder);
        elbow = elbow < rig.lim[1][0] - 5 ? rig.lim[1][0] - 5 : (elbow > rig.lim[1][1] + 5 ? rig.lim[1][1] + 5 : elbow);
        wrist = wrist < rig.lim[2][0] - 5 ? rig.lim[2][0] - 5 : (wrist > rig.lim[2][1] + 5 ? rig.lim[2][1] + 5 : wrist);
        any = any || active;
        // FK：肩绕 S 转 θs → 肘点跟着走 → 前臂绕新肘点转 θs+θe → 手绕新腕点转 θs+θe+θw
        const csmFloat32 thS = rig.sign * shoulder * kRad;
        const csmFloat32 thE = -rig.sign * elbow * kRad;
        const csmFloat32 thW = -rig.sign * wrist * kRad;
        csmFloat32 sx, sy, ex, ey, wx, wy;

        AnchorPos(rig.sh, 0.0f, 0.0f, &sx, &sy);
        AnchorPos(rig.el, sx, sy, &ex, &ey);
        AnchorPos(rig.wr, sx, sy, &wx, &wy);
        const csmFloat32 phi[3] = { thS, thS + thE, thS + thE + thW };
        const csmFloat32 c0 = cosf(phi[0]), s0 = sinf(phi[0]);
        const csmFloat32 c1 = cosf(phi[1]), s1 = sinf(phi[1]);
        const csmFloat32 e2x = sx + c0 * (ex - sx) - s0 * (ey - sy);
        const csmFloat32 e2y = sy + s0 * (ex - sx) + c0 * (ey - sy);
        const csmFloat32 w2x = e2x + c1 * (wx - ex) - s1 * (wy - ey);
        const csmFloat32 w2y = e2y + s1 * (wx - ex) + c1 * (wy - ey);
        const csmFloat32 piv[3][4] = { { sx, sy, sx, sy }, { ex, ey, e2x, e2y }, { wx, wy, w2x, w2y } };

        for (int b = 0; b < 3; ++b)
        {
            const csmFloat32 c = cosf(phi[b]), s = sinf(phi[b]);
            csmFloat32* A = &bones[(size_t)(rig.bone0 + b) * 6];

            A[0] = c;  A[1] = -s;
            A[2] = s;  A[3] = c;
            A[4] = piv[b][2] - (c * piv[b][0] - s * piv[b][1]);
            A[5] = piv[b][3] - (s * piv[b][0] + c * piv[b][1]);
        }
        // 前臂/手抬到身前时画在最上层（否则被头发/身体盖住）
        const csmBool onTop = active && (fabsf(shoulder) > 50.0f || fabsf(elbow) > 40.0f);

        for (size_t i = 0; i < rig.draws.size(); ++i)
        {
            r->SetDrawableLimbOnTop(rig.draws[i], onTop);
        }
    }
    if (!any)
    {
        if (_rigSkinSent)
        {
            r->ClearSkins();
            r->ClearDrawableLimbRotates();   // 连 onTop 一起清 → 与基线逐位一致
            _rigSkinSent = false;
        }
        return false;
    }
    if (!_rigSkinSent)
    {
        for (size_t k = 0; k < _rigs.size(); ++k)
        {
            for (size_t i = 0; i < _rigs[k].draws.size(); ++i)
            {
                r->SetDrawableSkin(_rigs[k].draws[i], &_rigs[k].skin[i][0], (csmInt32)_rigs[k].skin[i].size());
            }
        }
        _rigSkinSent = true;
    }
    r->SetSkinBones(&bones[0], (csmInt32)(bones.size() / 6));
    // 自检（L2D_RIG_CHECK=1）：刚性残差 = 同一根骨上的顶点两两距离变化的最大值（模型单位）。
    // "只在关节弯"⇔ 过渡带以外这个数 ≈ 0（bend 那种臂身弯曲会在这里暴露）
    static const bool rigCheck = (getenv("L2D_RIG_CHECK") != NULL);
    static int rigCheckFrame = 0;

    if (rigCheck && (++rigCheckFrame % 30) == 0)
    {
        for (size_t k = 0; k < _rigs.size(); ++k)
        {
            double worst = 0.0;
            long pure = 0, blended = 0;

            for (size_t i = 0; i < _rigs[k].draws.size(); ++i)
            {
                const csmInt32 d = _rigs[k].draws[i];
                const Live2D::Cubism::Core::csmVector2* pos = _model->GetDrawableVertexPositions(d);
                const std::vector<CubismRenderer_Software::SkinW>& sw = _rigs[k].skin[i];
                int ref[256];

                for (int b = 0; b < 256; ++b) ref[b] = -1;
                for (size_t v = 0; v < sw.size(); ++v)
                {
                    if (sw[v].b0 != sw[v].b1)
                    {
                        ++blended;
                        continue;
                    }
                    ++pure;
                    const int b = sw[v].b0;

                    if (ref[b] < 0)
                    {
                        ref[b] = (int)v;
                        continue;
                    }
                    const csmFloat32* A = &bones[(size_t)b * 6];
                    const csmFloat32 x0 = pos[ref[b]].X, y0 = pos[ref[b]].Y, x1 = pos[v].X, y1 = pos[v].Y;
                    const csmFloat32 X0 = A[0] * x0 + A[1] * y0 + A[4], Y0 = A[2] * x0 + A[3] * y0 + A[5];
                    const csmFloat32 X1 = A[0] * x1 + A[1] * y1 + A[4], Y1 = A[2] * x1 + A[3] * y1 + A[5];
                    const double before = sqrt((double)(x1 - x0) * (x1 - x0) + (double)(y1 - y0) * (y1 - y0));
                    const double after = sqrt((double)(X1 - X0) * (X1 - X0) + (double)(Y1 - Y0) * (Y1 - Y0));

                    worst = fabs(after - before) > worst ? fabs(after - before) : worst;
                }
            }
            printf("[rigcheck] %s 刚性残差 %.2e（纯刚性顶点 %ld，关节过渡带顶点 %ld = %.1f%%）\n",
                   _rigs[k].ch.c_str(), worst, pure, blended,
                   100.0 * (double)blended / (double)(pure + blended > 0 ? pure + blended : 1));
        }
        fflush(stdout);
    }
    return true;
}

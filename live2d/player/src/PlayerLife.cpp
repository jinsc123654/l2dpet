/**
 * PlayerLife.cpp — L2DPlayer 的内在状态 / 连续情绪实现（从 l2dtest.cpp 拆出，纯搬移，逻辑未改）。
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
#include "PetWatermark.h"
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
#include <cctype>
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

/** 每帧（ApplyBodyChannelsPre 里、动作叠加之后）：更新 VAD → 写面部/头身叠加层 */
void L2DPlayer::EmotionTick(csmFloat32 dt)
{
    if (dt <= 0.0f || dt > 1.0f)
    {
        return;
    }
    // 性格基线：随内在状态
    Vad base = { 0.08f + (_lifeAffection - 0.5f) * 0.35f, (_lifeEnergy - 0.6f) * 0.5f, 0.0f };

    if (_lifeState == LS_ASLEEP) { base.v = 0.15f; base.a = -0.85f; base.d = -0.1f; }
    else if (_lifeState == LS_DROWSY) { base.a = -0.6f; }
    else if (_lifeState == LS_SULK) { base = *EmotionPreset("sulky"); _emoHold = _emoHold > 2.0f ? _emoHold : 2.0f; }
    // 环境漂移：情绪"不完全静止"
    _emoDriftClock += dt;
    if (_emoDriftClock >= _emoDriftNext)
    {
        const csmFloat32 st = 0.04f;
        _emoDriftTgt.v = st * ((csmFloat32)(NextRand() % 2001u) / 1000.0f - 1.0f);
        _emoDriftTgt.a = st * 0.82f * ((csmFloat32)(NextRand() % 2001u) / 1000.0f - 1.0f);
        _emoDriftTgt.d = st * 0.68f * ((csmFloat32)(NextRand() % 2001u) / 1000.0f - 1.0f);
        _emoDriftNext = _emoDriftClock + 1.7f + (csmFloat32)(NextRand() % 420u) / 100.0f;
    }
    const csmFloat32 kd = 1.0f - expf(-dt * 0.62f);
    _emoDrift.v += (_emoDriftTgt.v - _emoDrift.v) * kd;
    _emoDrift.a += (_emoDriftTgt.a - _emoDrift.a) * kd;
    _emoDrift.d += (_emoDriftTgt.d - _emoDrift.d) * kd;
    // 当前逼近目标；目标保持后回落基线
    const Vad prev = _emoCur;
    const csmFloat32 ka = 1.0f - expf(-dt * 1.6f);
    const csmFloat32 kb = _emoHold > 0.0f ? 0.0f : 1.0f - expf(-dt * 0.08f);

    _emoHold = _emoHold > dt ? _emoHold - dt : 0.0f;
    _emoCur.v = ClampS(_emoCur.v + (_emoTgt.v + _emoDrift.v - _emoCur.v) * ka);
    _emoCur.a = ClampS(_emoCur.a + (_emoTgt.a + _emoDrift.a - _emoCur.a) * ka);
    _emoCur.d = ClampS(_emoCur.d + (_emoTgt.d + _emoDrift.d - _emoCur.d) * ka);
    _emoTgt.v += (base.v - _emoTgt.v) * kb;
    _emoTgt.a += (base.a - _emoTgt.a) * kb;
    _emoTgt.d += (base.d - _emoTgt.d) * kb;
    _emoMag = VadMag(_emoCur);
    _emoName = EmotionDominant(_emoCur, _emoMag);
    if (_perfClock - _emoSampleAt >= 10.0f)   // 情绪曲线：10 秒一个点，留 15 分钟
    {
        _emoSampleAt = _perfClock;
        if (_emoHist.size() >= 90) _emoHist.pop_front();
        _emoHist.push_back(EmoSample{ _emoCur.v, _emoCur.a });
    }
    if (_emoName != _emoFeedName && _emoName != "neutral" && _emoMag > 0.45f && _perfClock - _emoFeedAt > 20.0f)
    {
        _emoFeedAt = _perfClock;
        _emoFeedName = _emoName;
        FeedPush("emo", _emoName);   // 面板按键名翻成中文
    }

    // 微动脉冲：VAD 一帧内变化够大 → 头部一怔（唤醒升 = 往上一抬，降 = 往下一沉；愉悦带一点歪头）
    const csmFloat32 dv = _emoCur.v - prev.v, da = _emoCur.a - prev.a;
    const csmFloat32 rate = (fabsf(dv) + fabsf(da)) / dt;

    if (rate > 0.9f && _perfClock >= _emoPulseNext)
    {
        _emoPulseAt = _perfClock;
        _emoPulseY = (da >= 0.0f ? 1.0f : -1.0f) * (rate > 3.0f ? 3.0f : rate) * 1.6f;
        _emoPulseZ = (dv >= 0.0f ? 1.0f : -1.0f) * ((NextRand() & 1u) ? 1.0f : -1.0f) * 2.5f;
        _emoPulseNext = _perfClock + 1.2f;
    }

    // ---- VAD → 叠加层（模型无关的语义 → 标准通道；缺的通道自动跳过）
    const csmFloat32 g = _cfgExprGain;
    const csmFloat32 pos = _emoCur.v > 0 ? _emoCur.v : 0, neg = _emoCur.v < 0 ? -_emoCur.v : 0;
    const csmFloat32 aro = _emoCur.a > 0 ? _emoCur.a : 0, calm = _emoCur.a < 0 ? -_emoCur.a : 0;
    const csmFloat32 dom = _emoCur.d > 0 ? _emoCur.d : 0, sub = _emoCur.d < 0 ? -_emoCur.d : 0;
    const csmFloat32 smile = 0.95f * pos - 0.85f * neg;
    const csmFloat32 browY = 0.45f * aro * pos + 0.35f * neg * sub - 0.55f * neg * dom;
    const csmFloat32 browForm = 0.5f * pos - 0.9f * neg * (0.4f + dom);
    const csmFloat32 eyeOpen = 0.18f * aro - 0.3f * calm - 0.22f * pos * (1.0f - aro);
    const csmFloat32 headY = 7.0f * dom * (0.3f + aro) - 9.0f * sub - 5.0f * calm * neg + 3.0f * aro * pos;
    const csmFloat32 headZ = -7.0f * pos * sub;
    const csmFloat32 blush = 1.1f * pos * (sub + 0.4f * aro);
    csmFloat32 pulseY = 0.0f, pulseZ = 0.0f;
    const csmFloat32 pt = _perfClock - _emoPulseAt;

    if (pt >= 0.0f && pt < 0.7f)
    {
        const csmFloat32 env = sinf(pt / 0.7f * 3.14159f) * (1.0f - pt / 0.7f * 0.5f);
        pulseY = _emoPulseY * env;
        pulseZ = _emoPulseZ * env;
    }
    const struct { const char* ch; csmFloat32 v; } add[] = {
        { "mouth.form", smile * g }, { "brow.l", browY * g }, { "brow.r", browY * g },
        { "brow.l.form", browForm * g }, { "brow.r.form", browForm * g },
        { "eye.l", eyeOpen * g }, { "eye.r", eyeOpen * g },
        { "head.y", headY * g + pulseY }, { "head.z", headZ * g + pulseZ },
        { "cheek", blush * g }, { "body.y", 2.5f * aro * pos * g },
    };
    for (size_t i = 0; i < sizeof(add) / sizeof(add[0]); ++i)
    {
        const int c = FindBodyChannel(add[i].ch);

        if (c >= 0)
        {
            _channels[(size_t)c].extra += add[i].v;
        }
    }

    // ---- 峰值：强度够高时借用模型自带表情（只管自己设的那个）
    static const struct { const char* emo; const char* zh; } kPeak[] = {
        { "happy", "开心" }, { "excited", "开心" }, { "affectionate", "开心" }, { "shy", "害羞" },
        { "angry", "生气" }, { "sulky", "生气" }, { "surprised", "惊讶" }, { "sad", "难过" },
    };
    const char* want = NULL;

    for (size_t i = 0; i < sizeof(kPeak) / sizeof(kPeak[0]) && _emoMag > 0.3f && _cfgNativeExpr; ++i)
    {
        if (_emoName == kPeak[i].emo) want = kPeak[i].zh;
    }
    const bool mine = _emoExprIdx >= 0 && _expressionIndex == _emoExprIdx;

    if (want != NULL && (_emoExprFor != want || !mine) && (mine || _expressionIndex < 0))
    {
        if (StartEmotion(want))
        {
            _emoExprIdx = _expressionIndex;
            _emoExprFor = want;
        }
        else
        {
            _emoExprFor = want;   // 模型没有这类表情：只靠叠加层
        }
    }
    else if (want == NULL && mine && _emoMag < 0.22f)
    {
        ClearExpression();
        _emoExprIdx = -1;
        _emoExprFor.clear();
    }
}

/** panel_state.life：状态页的反馈数据 */
std::string L2DPlayer::LifePanelJson()
{
    std::string o = "{\"feed\":[";
    for (size_t i = 0; i < _feed.size(); ++i)
    {
        if (i) o += ",";
        o += "[" + std::to_string((long long)_feed[i].t) + ",";   // 整数秒（JNum 只有 6 位有效数字）
        JStr(o, _feed[i].kind);
        o += ",";
        JStr(o, _feed[i].text);
        o += "]";
    }
    o += "],\"emo_hist\":[";
    for (size_t i = 0; i < _emoHist.size(); ++i)
    {
        char b[48];
        snprintf(b, sizeof(b), "%s[%.2f,%.2f]", i ? "," : "", _emoHist[i].v, _emoHist[i].a);
        o += b;
    }
    o += "],\"zones\":[";
    bool first = true;
    for (int z = 0; z < ZONE_COUNT; ++z)
    {
        if (_zoneLife[z][0] + _zoneLife[z][1] + _zoneSess[z][0] + _zoneSess[z][1] == 0) continue;
        if (!first) o += ",";
        first = false;
        o += "[";
        JStr(o, ZoneLabel(z));
        o += "," + std::to_string(_zoneLife[z][0]) + "," + std::to_string(_zoneLife[z][1]) + ","
             + std::to_string(_zoneSess[z][0]) + "," + std::to_string(_zoneSess[z][1]) + "]";
    }
    char b[400];
    snprintf(b, sizeof(b),
             "],\"rps\":[%ld,%ld,%ld],\"first_seen\":%.0f,\"state_for\":%.0f,\"idle\":%.0f,\"since_touch\":%.0f,"
             "\"sleep_on\":%d,\"sleep_after\":%.0f,\"work\":%.0f,\"work_limit\":%.0f,\"seek_on\":%d}",
             _rps[0], _rps[1], _rps[2], _firstSeen, (double)(_perfClock - _lifeStateAt), (double)_userIdleT,
             (double)_sinceTouchT, _cfgSleep ? 1 : 0, (double)_cfgSleepAfter, (double)_workT,
             (double)(Cfg("work_remind") * 60.0f), _cfgSeek ? 1 : 0);
    return o + b;
}

void L2DPlayer::LifeLoad()
{
    _lifeLoaded = true;
    FILE* f = fopen(LifePath().c_str(), "rb");
    double last = 0.0;

    if (f != NULL)
    {
        char buf[4096] = { 0 };
        const size_t n = fread(buf, 1, sizeof(buf) - 1, f);

        fclose(f);
        buf[n] = '\0';
        jsonlite::Value v;
        jsonlite::parse(buf, n, v, NULL);

        _lifeAffection = (csmFloat32)v.get("affection").numOr(0.3);
        _lifeTouches = (long)v.get("touches").numOr(0.0);
        last = v.get("last_seen").numOr(0.0);
        _firstSeen = v.get("first_seen").numOr(0.0);
        const jsonlite::Value& zs = v.get("zones");   // [[点,搓], …] 按 ZoneKey 顺序
        for (int z = 0; zs.isArr() && z < ZONE_COUNT && z < (int)zs.arr.size(); ++z)
        {
            const jsonlite::Value& pr = zs.arr[(size_t)z];
            if (!pr.isArr() || pr.arr.size() < 2) continue;
            _zoneLife[z][0] = (long)pr.arr[0].numOr(0);
            _zoneLife[z][1] = (long)pr.arr[1].numOr(0);
        }
        const jsonlite::Value& rp = v.get("rps");
        for (int i = 0; rp.isArr() && i < 3 && i < (int)rp.arr.size(); ++i) _rps[i] = (long)rp.arr[(size_t)i].numOr(0);
    }
    // 开场招呼：看离上次多久、亲密度多少
    const double away = last > 0.0 ? (double)time(NULL) - last : -1.0;
    // 相识日：取"存档里的"与"agent 记忆里的 first_seen（habits.json，YYYY-MM-DD）"中更早的
    //（老存档没有这个字段；记忆是从第一次见面就开始记的，更准）
    {
        const std::string hp = plat::HomeDir() + "/.config/l2dpet/memory/chars/" + FingerprintText().substr(
            FingerprintText().find(':') == std::string::npos ? 0 : FingerprintText().find(':') + 1) + ".habits.json";
        const std::string ht = plat::ReadTextFile(hp);
        jsonlite::Value hv;
        int yy = 0, mm = 0, dd = 0;
        if (!ht.empty() && jsonlite::parse(ht.c_str(), ht.size(), hv, NULL)
            && sscanf(hv.get("first_seen").strOr(""), "%d-%d-%d", &yy, &mm, &dd) == 3)
        {
            struct tm t0;
            memset(&t0, 0, sizeof(t0));
            t0.tm_year = yy - 1900;
            t0.tm_mon = mm - 1;
            t0.tm_mday = dd;
            t0.tm_hour = 12;
            const double m = (double)mktime(&t0);
            if (m > 0.0 && (_firstSeen <= 0.0 || m < _firstSeen)) _firstSeen = m;
        }
    }
    if (_firstSeen <= 0.0) _firstSeen = last > 0.0 ? last : (double)time(NULL);
    // 好几天没见：亲密度慢慢回落（每天 -0.02，最低 0.3；关系不会因为一次长假清零）
    if (away > 86400.0 && _lifeAffection > 0.3f)
    {
        _lifeAffection = std::max(0.3f, _lifeAffection - 0.02f * (csmFloat32)(away / 86400.0));
    }

    if (away < 0.0)
    {
        LIFE_SAY("first", 3.5f, "你好呀，初次见面～", "嗯？这里是哪儿？……你好！");
    }
    else if (away > 12 * 3600.0)
    {
        if (_lifeAffection > 0.6f) LIFE_SAY("miss", 3.5f, "你终于来啦！好想你～", "好久不见！我一直在等你哦");
        else LIFE_SAY("back", 3.5f, "好久不见。", "哦，是你呀。");
    }
    else
    {
        LIFE_SAY("again", 3.0f, "又见面啦～", "嘿，回来啦");
    }
    _lifeSaveAt = _perfClock + 30.0f;
}

/** 触摸：按状态处理。返回 true = 已完全处理（不走默认区域反应） */
bool L2DPlayer::LifeTouch(int z, int kind, int n, csmFloat32 x, csmFloat32 y)
{
    if (!_isPet)
    {
        return false;
    }
    const bool gentleZone = (z == Z_HEAD || z == Z_FACE || z == Z_HAND_L || z == Z_HAND_R);
    const bool rudeZone = (z == Z_CHEST || z == Z_CROTCH);

    ++_lifeTouches;
    ++_sessionTouches;
    _sinceTouchT = 0.0f;
    _lifeLonely = Clamp01(_lifeLonely - 0.25f);
    if (z >= 0 && z < ZONE_COUNT)
    {
        ++_zoneLife[z][kind == 1 ? 1 : 0];
        ++_zoneSess[z][kind == 1 ? 1 : 0];
        FeedTouch(z, kind);
    }

    // ---- 睡着：摸头 = 睡梦里笑；戳 = 先迷糊，再戳才醒（被吵醒有点不高兴）
    if (_lifeState == LS_ASLEEP)
    {
        // 累趴了在补觉（精力 < 30%）：睡得很沉，戳不醒，只会嘟囔
        if (kind == 0 && _lifeEnergy < kTiredEnergy)
        {
            SetBodyChannel("head.z", 3.0f, 0.8f);
            if (_perfClock - _lastReactLineAt > 4.0f)
            {
                LIFE_SAY("deep_sleep", 2.0f, "……Zzz", "呼……呼……", "唔……（睡得好沉）");
            }
            return true;
        }
        if (kind == 1 && gentleZone)
        {
            AffectionUp(0.02f);
            SetBodyChannel("mouth.form", 0.8f, 0.6f);
            _sleepSmileUntil = _perfClock + 3.0f;
            if (_perfClock - _lastReactLineAt > 6.0f)
            {
                LIFE_SAY("sleep_pet", 2.5f, "唔……嘿嘿……", "呼……好舒服……", "……再摸……一下……");
            }
            return true;
        }
        if (_perfClock - _lifeStirAt > 6.0f)
        {
            _lifeStirs = 0;
        }
        _lifeStirAt = _perfClock;
        if (++_lifeStirs < 2)
        {
            SetBodyChannel("eye.l", -0.55f, 0.25f);   // 眼皮动了一下
            SetBodyChannel("eye.r", -0.55f, 0.25f);
            SetBodyChannel("head.z", 4.0f, 0.8f);
            _eyeFlutterUntil = _perfClock + 1.2f;
            LIFE_SAY("stir", 2.0f, "唔……", "嗯……别吵……", "……五分钟……");
            return true;
        }
        _lifeAnnoy = Clamp01(_lifeAnnoy + (_lifeEnergy < 0.4f ? 0.25f : 0.1f));
        LifeEnter(LS_AWAKE, "被戳醒");
        SetLookTargetPixels(x, y);
        _lookOverrideUntil = _perfClock + 2.0f;
        StartAction("surprise_big", 1.0f, 0.7f, 1);
        EmotionNudge(_lifeEnergy < 0.4f ? "sulky" : "surprised", 0.7f);
        if (_lifeEnergy < 0.4f) LIFE_SAY("woken_grumpy", 3.0f, "干嘛啦……人家还困着……", "呜……被吵醒了……");
        else LIFE_SAY("woken", 3.0f, "呀！……吓我一跳", "唔？我、我没睡着！");
        return true;
    }
    // ---- 犯困：一碰就醒（揉揉眼）
    if (_lifeState == LS_DROWSY)
    {
        // 累到犯困还被弄醒：不回精力，掉亲密度（以前碰一下 +0.1 精力 = 戳醒就能"充电"）
        if (_lifeEnergy < kTiredEnergy)
        {
            InteractCost(0.0f, "吵醒");
            LifeEnter(LS_AWAKE, "累着被吵醒");
            StartGesture("shake", 1.0f, 1, 0.5f);
            return true;
        }
        _lifeEnergy = Clamp01(_lifeEnergy + 0.1f);
        LifeEnter(LS_AWAKE, "被碰醒");
        StartGesture("shake", 1.0f, 1, 0.5f);
        LIFE_SAY("unsleepy", 2.5f, "嗯？我、我没打瞌睡！", "哈啊……你来啦", "差点睡着了……");
        return true;
    }
    // ---- 生闷气：戳 = 更不理；摸头 = 慢慢哄好
    if (_lifeState == LS_SULK)
    {
        if (kind == 1 && gentleZone)
        {
            _lifeAnnoy = Clamp01(_lifeAnnoy - 0.18f);
            if (_lifeAnnoy < 0.3f)
            {
                LifeEnter(LS_AWAKE, "被哄好");
                AffectionUp(0.03f);
                SetLookTargetPixels(x, y);
                _lookOverrideUntil = _perfClock + 2.0f;
                EmotionNudge("shy", 0.8f);
                StartAction("shy", 1.0f, 0.8f, 1);
                LIFE_SAY("forgive", 3.0f, "……好啦，原谅你了", "哼，下不为例哦", "这次就算了～");
            }
            else if (_perfClock - _lastReactLineAt > 2.5f)
            {
                LIFE_SAY("sulk_pet", 2.2f, "……别以为摸摸头就好了", "哼……", "……还在生气呢");
            }
            return true;
        }
        _lifeAnnoy = Clamp01(_lifeAnnoy + 0.1f);
        StartGesture("shake", 1.2f, 1, 0.6f);
        if (_perfClock - _lastReactLineAt > 2.0f)
        {
            LIFE_SAY("sulk", 2.0f, "哼。", "不理你了。", "……", "走开啦");
        }
        return true;
    }
    // ---- 醒着：每次互动都消耗精力（戳 0.015、摸 0.01）；累了还继续 → 掉亲密度、不做开心反应
    if (InteractCost(kind == 1 ? 0.01f : 0.015f, TouchVerb(ZoneKey(z), kind == 1)))
    {
        StartGesture("shake", 0.9f, 1, 0.4f);
        return true;
    }
    // ---- 醒着：先记账，再看要不要进别的状态
    if (kind == 1 && gentleZone)
    {
        AffectionUp(0.015f);
        _lifeAnnoy = Clamp01(_lifeAnnoy - 0.08f);
        // 持续抚摸：第一下是正常反应，之后进入"享受"——眯眼、嘴角上扬、往手那边蹭，不再每次重播动作
        if (_perfClock < _enjoyUntil + 1.5f)
        {
            _enjoyUntil = _perfClock + 2.5f;
            SetBodyChannel("eye.l", -0.6f, 0.5f);
            SetBodyChannel("eye.r", -0.6f, 0.5f);
            SetBodyChannel("mouth.form", 0.9f, 0.5f);
            SetBodyChannel("head.z", x < _targetW * 0.5f ? 7.0f : -7.0f, 0.8f);
            EmotionNudge("affectionate", 0.5f);
            if (_perfClock - _lastReactLineAt > 4.0f)
            {
                LIFE_SAY("enjoy", 2.2f, "呼噜呼噜～", "嘿嘿……", "再、再摸一会儿也行", "舒服～");
            }
            return true;
        }
        _enjoyUntil = _perfClock + 2.5f;
        return false;   // 第一下：默认区域反应
    }
    if (kind == 0)
    {
        csmFloat32 a = rudeZone ? 0.22f : 0.04f;

        if (n >= 3) a += 0.12f * (csmFloat32)(n - 2);
        if (_lifeAffection > 0.7f && !rudeZone) a *= 0.5f;   // 关系好，容忍度高
        _lifeAnnoy = Clamp01(_lifeAnnoy + a);
        if (rudeZone) _lifeAffection = Clamp01(_lifeAffection - 0.01f);
        if (z == Z_HAND_L || z == Z_HAND_R) AffectionUp(0.01f);
        if (_lifeAnnoy >= 0.7f)
        {
            LifeEnter(LS_SULK, "被戳烦了");
            StartAction("angry", 1.0f, 0.8f, 1);
            LIFE_SAY("sulk_start", 3.0f, "我生气了！不理你了！", "哼！讨厌！", "再戳我就真的生气了……已经生气了！");
            return true;
        }
    }
    return false;
}

/** 每帧：节律 + 状态机 + 自主行为。raw* = 全局指针（不论离人物远近），near = 指针在人物附近 */
void L2DPlayer::PetLifeTick(csmFloat32 dt, bool rawHas, csmFloat32 px, csmFloat32 py, bool near)
{
    if (!_isPet || dt <= 0.0f || dt > 1.0f)
    {
        return;
    }
    if (!_lifeLoaded)
    {
        _lifeLoaded = true;
        _lifeStartAt = _perfClock;
        _lifeLoadPending = true;
    }
    if (_lifeLoadPending && _perfClock - _lifeStartAt > 1.5f)   // 等第一帧画出来再打招呼
    {
        _lifeLoadPending = false;
        CfgLoad();
        LifeLoad();
        // 配置读好后推一次：agent 比这启动得早（它启动时读到的都是默认值），拿到这条才能对齐语言
        _petEvents.push_back("{\"event\":\"config\",\"config\":" + CfgJson() + "}");
    }
    // 用户在不在：全局指针动没动
    const bool moved = rawHas && (fabsf(px - _lastPx) + fabsf(py - _lastPy) > 3.0f * PxK());

    if (rawHas)
    {
        _lastPx = px;
        _lastPy = py;
    }
    const csmFloat32 wasIdle = _userIdleT;

    _userIdleT = moved ? 0.0f : _userIdleT + dt;
    _sinceTouchT += dt;
    if (near) _sinceTouchT = _sinceTouchT < 30.0f ? _sinceTouchT : 30.0f;   // 鼠标在身边 = 有人陪（寂寞不涨）

    // 连续工作时长（只有鼠标这一路信号：一直在动 ≈ 人在用电脑；离开 >5 分钟算中断）
    {
        if (_userIdleT < 5.0f) _workT += dt;
        else if (_userIdleT > 300.0f) _workT = 0.0f;
        const csmFloat32 wlimit = Cfg("work_remind") * 60.0f;
        if (wlimit >= 60.0f && _workT >= wlimit)
        {
            _workT = 0.0f;   // 下一次提醒要再攒满一轮
            char b[128];
            snprintf(b, sizeof(b), "{\"event\":\"work_long\",\"min\":%d}", (int)(wlimit / 60.0f));
            _petEvents.push_back(b);
            printf("[life] 连续工作 %d 分钟 → 提醒\n", (int)(wlimit / 60.0f));
            fflush(stdout);
        }
    }

    const time_t now = time(NULL);
    struct tm lt;
    plat::LocalTime(now, &lt);
    const bool night = lt.tm_hour >= 23 || lt.tm_hour < 6;
    const bool busy = !_petSeqs.empty() || !_scenes.empty();   // LLM 段在演：不抢戏

    // 节律
    // 睡觉回精力：满格约 25 分钟（原来 4 分钟——打个盹就满，状态页精力永远 100）
    if (_lifeState == LS_ASLEEP) _lifeEnergy = Clamp01(_lifeEnergy + dt / 1500.0f);
    else _lifeEnergy = Clamp01(_lifeEnergy - dt / (night ? 900.0f : 2400.0f));
    _lifeAnnoy = Clamp01(_lifeAnnoy - dt / (_lifeState == LS_SULK ? 90.0f : 35.0f));
    // 寂寞：被晾着约 20 分钟才满（原来 2.5 分钟就满——几乎永远 100）
    if (_sinceTouchT > 45.0f && _lifeState == LS_AWAKE) _lifeLonely = Clamp01(_lifeLonely + dt / 1200.0f);
    if (_cfgDirtyAt >= 0.0f && _perfClock - _cfgDirtyAt > 1.0f)
    {
        _cfgDirtyAt = -1.0f;
        CfgSave();
    }
    if (_perfClock >= _lifeSaveAt)
    {
        _lifeSaveAt = _perfClock + 30.0f;
        LifeSave();
    }
    if (_perfClock - _lifeMoodAt > 20.0f)   // 定期把心情推给 LLM
    {
        _lifeMoodAt = _perfClock;
        LifePushMood("tick");
    }
    const csmFloat32 inState = _perfClock - _lifeStateAt;

    // 精力耗尽：不管在干嘛，直接累趴睡着（补觉期间戳不醒，见 LifeTouch）
    if (_lifeState != LS_ASLEEP && _lifeEnergy <= 0.02f)
    {
        FeedPush("state", peti18n::Tr("累坏了，直接睡着了"));
        LifeEnter(LS_ASLEEP, "累坏了");
        StartAction("yawn", 1.0f, 1.0f, 1);
        LIFE_SAY("collapse", 3.0f, "撑……撑不住了……Zzz", "呼……不行了……", "好困……晚安……");
        return;
    }
    // 生闷气时累了、又没人再碰她 20 秒 → 气着气着睡着了
    if (_lifeState == LS_SULK && _lifeEnergy < 0.25f && _sinceTouchT > 20.0f)
    {
        LifeEnter(LS_ASLEEP, "气着气着睡着了");
        return;
    }
    switch (_lifeState)
    {
    case LS_AWAKE:
    {
        // 用户离开很久后回来 → 打招呼
        if (moved && wasIdle > 300.0f)
        {
            SetLookTargetPixels(px, py);
            _lookOverrideUntil = _perfClock + 2.0f;
            StartAction(_lifeAffection > 0.5f ? "hop" : "nod_yes", 1.0f, 0.7f, 1);
            EmotionNudge(_lifeAffection > 0.5f ? "excited" : "happy", 0.7f);
            {
                char why[48];
                snprintf(why, sizeof(why), "你回来了（离开 %d 分钟）", (int)(wasIdle / 60.0f));
                LifePushMood(why);   // agent 据此"叙旧"：结合记忆说一句
            }
            if (_lifeAffection > 0.5f) LIFE_SAY("welcome", 3.0f, "你回来啦！", "欢迎回来～等你好久了", "啊，回来了！");
            else LIFE_SAY("welcome_cool", 2.5f, "回来了啊。", "嗯？你回来了");
            _lifeLonely = Clamp01(_lifeLonely - 0.3f);
            break;
        }
        // 享受抚摸结束 → 放开眯眼/蹭头
        if (_enjoyUntil > 0.0f && _perfClock > _enjoyUntil)
        {
            _enjoyUntil = 0.0f;
            static const char* const kE[] = { "eye.l", "eye.r", "mouth.form", "head.z" };
            for (int i = 0; i < 4; ++i)
            {
                const int c = FindBodyChannel(kE[i]);
                if (c >= 0 && !_channels[(size_t)c].fromScene) ReleaseBodyChannel(_channels[(size_t)c]);
            }
        }
        if (busy)
        {
            break;
        }
        // 犯困：精力低，或用户很久没动、也很久没理它
        if (_cfgSleep && (_lifeEnergy < 0.25f || (_userIdleT > _cfgSleepAfter && _sinceTouchT > _cfgSleepAfter))
            && _sinceTouchT > 20.0f)
        {
            LifeEnter(LS_DROWSY, _lifeEnergy < 0.25f ? "累了" : "没人理");
            StartAction("yawn", 1.0f, 1.0f, 1);
            LIFE_SAY("drowsy", 2.5f, "哈啊……有点困了……", "呼啊～", "眼皮好重……");
            break;
        }
        // 寂寞：用户在（指针在动）但不理它 → 主动找你（看向鼠标、挥手、搭话）
        if (_cfgSeek && _lifeLonely > 0.6f && _userIdleT < 20.0f && rawHas && _perfClock >= _seekNextAt)
        {
            _seekNextAt = _perfClock + 120.0f + (csmFloat32)(NextRand() % 90u);
            _lifeLonely = Clamp01(_lifeLonely - 0.35f);
            SetLookTargetPixels(px, py);
            _lookOverrideUntil = _perfClock + 3.0f;
            if (!StartAction("wave_big", 1.0f, 0.8f, 1)) StartAction("hop", 1.0f, 0.8f, 1);
            EmotionNudge("sad", 0.35f);
            LIFE_SAY("lonely", 3.5f, "喂～陪我玩一会儿嘛", "你在忙什么呀？", "看看我嘛……", "好无聊哦～");
            LifePushMood("寂寞找你");
            break;
        }
        // 好奇：用户在忙（鼠标在别处动）→ 偶尔瞄一眼你在干嘛
        if (!near && moved && _perfClock >= _glanceNextAt && _perfClock >= _lookOverrideUntil)
        {
            _glanceNextAt = _perfClock + 15.0f + (csmFloat32)(NextRand() % 25u);
            SetLookTargetPixels(px, py);
            _lookOverrideUntil = _perfClock + 1.6f;
        }
        break;
    }
    case LS_DROWSY:
    {
        // 打瞌睡：半闭眼 + 头一点一点
        const csmFloat32 nodPh = fmodf(inState, 7.0f);
        SetBodyChannel("eye.l", nodPh > 5.0f ? -0.9f : -0.5f, 0.6f);
        SetBodyChannel("eye.r", nodPh > 5.0f ? -0.9f : -0.5f, 0.6f);
        SetBodyChannel("head.y", nodPh > 5.0f ? -14.0f : -5.0f, nodPh > 5.0f ? 1.2f : 0.35f);
        _lookOverrideUntil = _perfClock + 0.5f;
        if (moved && wasIdle > 60.0f)   // 用户回来了 → 醒醒神
        {
            _lifeEnergy = Clamp01(_lifeEnergy + 0.15f);
            LifeEnter(LS_AWAKE, "你回来了");
            LIFE_SAY("welcome_drowsy", 2.5f, "唔……你回来啦", "啊、在的在的！");
            break;
        }
        if (inState > 45.0f && _sinceTouchT > 45.0f && !busy)
        {
            LifeEnter(LS_ASLEEP, "睡着了");
            _zzzAt = _perfClock + 2.0f;
        }
        break;
    }
    case LS_ASLEEP:
    {
        // 睡着：闭眼、低头、慢呼吸；不跟鼠标；不时冒 Zzz / 梦话
        const csmFloat32 br = sinf(_perfClock * 1.25f);
        if (_perfClock > _eyeFlutterUntil)
        {
            SetBodyChannel("eye.l", -1.0f, 0.8f);
            SetBodyChannel("eye.r", -1.0f, 0.8f);
            SetBodyChannel("head.z", 6.0f, 2.0f);
        }
        SetBodyChannel("head.y", -15.0f + 2.0f * br, 1.5f);
        SetBodyChannel("body.x", 1.5f * br, 1.5f);
        if (_sleepSmileUntil > 0.0f && _perfClock > _sleepSmileUntil)
        {
            _sleepSmileUntil = 0.0f;
            SetBodyChannel("mouth.form", 0.0f, 1.0f);
        }
        SetLookTargetPixels(_targetW * 0.5f, _targetH * 0.7f);
        _lookOverrideUntil = _perfClock + 0.5f;
        if (_perfClock >= _zzzAt)
        {
            _zzzAt = _perfClock + 10.0f + (csmFloat32)(NextRand() % 8u);
            if (NextRand() % 5u == 0u)
            {
                static const char* const kTalk[] = { "唔……布丁……", "……不要走……", "嘿嘿……", "……再睡五分钟……" };
                LifeSay(LifeLine("sleep_talk", kTalk, 4), 2.5f, false);
            }
            else
            {
                LifeSay("Zzz…", 2.5f, false);
            }
        }
        // 睡饱自己醒（至少睡 1 分钟）
        if (_lifeEnergy > 0.95f && inState > 60.0f)
        {
            LifeEnter(LS_AWAKE, "睡饱了");
            StartAction("stretch_up", 1.0f, 1.0f, 1) || StartAction("yawn", 1.0f, 1.0f, 1);
            LIFE_SAY("wake", 3.0f, "睡得好饱～", "哈啊～早上好！……还是下午？");
        }
        break;
    }
    case LS_SULK:
    {
        // 生闷气：扭头背过去（朝鼠标的反方向）、皱眉撇嘴；不跟鼠标
        const bool curLeft = rawHas && px < _targetW * 0.5f;
        SetBodyChannel("head.x", curLeft ? 30.0f : -30.0f, 0.8f);
        SetBodyChannel("head.y", -6.0f, 0.8f);
        SetBodyChannel("body.x", curLeft ? 9.0f : -9.0f, 1.2f);
        SetBodyChannel("brow.l", -0.6f, 0.5f);
        SetBodyChannel("brow.r", -0.6f, 0.5f);
        SetBodyChannel("mouth.form", -0.7f, 0.5f);
        SetLookTargetPixels(curLeft ? _targetW * 0.95f : _targetW * 0.05f, _targetH * 0.35f);
        _lookOverrideUntil = _perfClock + 0.5f;
        // 偷瞄：气消一半后偶尔偷看你一眼
        if (_lifeAnnoy < 0.5f && _perfClock >= _glanceNextAt && rawHas)
        {
            _glanceNextAt = _perfClock + 8.0f + (csmFloat32)(NextRand() % 6u);
            SetLookTargetPixels(px, py);
            _lookOverrideUntil = _perfClock + 0.8f;
        }
        if (_lifeAnnoy < 0.15f)   // 自己消气
        {
            LifeEnter(LS_AWAKE, "气消了");
            LIFE_SAY("calm", 2.5f, "……算了，不跟你计较", "哼，气消了");
        }
        break;
    }
    }
}

/** 渲染出的人物轮廓包围盒（alpha > 24 的像素；渲染目标坐标）；没有帧缓冲返回 false */
bool L2DPlayer::SilhouetteBBox(csmFloat32* bb)
{
    CubismRenderer_Software* r = SoftwareRenderer();

    if (r == NULL || r->GetFramebuffer() == NULL)
    {
        return false;
    }
    if (_silValid && _perfClock - _silAt < 0.5f && _perfClock >= _silAt)
    {
        memcpy(bb, _silBB, sizeof(_silBB));
        return true;
    }
    const csmUint32* fb = (const csmUint32*)r->GetFramebuffer();
    const int W = (int)r->GetFramebufferWidth(), H = (int)r->GetFramebufferHeight();
    int x0 = W, y0 = H, x1 = -1, y1 = -1;

    for (int y = 0; y < H; y += 2)
    {
        const csmUint32* row = fb + (size_t)y * W;

        for (int x = 0; x < W; x += 2)
        {
            if ((row[x] >> 24) > 24)
            {
                x0 = x < x0 ? x : x0;
                x1 = x > x1 ? x : x1;
                y0 = y < y0 ? y : y0;
                y1 = y > y1 ? y : y1;
            }
        }
    }
    if (x1 < 0)
    {
        return false;
    }
    const csmFloat32 sx = _targetW / (csmFloat32)W, sy = _targetH / (csmFloat32)H;

    _silBB[0] = x0 * sx;
    _silBB[1] = y0 * sy;
    _silBB[2] = (x1 + 2) * sx;
    _silBB[3] = (y1 + 2) * sy;
    _silAt = _perfClock;
    _silValid = true;
    memcpy(bb, _silBB, sizeof(_silBB));
    return true;
}

csmInt32 L2DPlayer::HandleTapZ(csmInt32 x, csmInt32 y, const char** zoneOut)
{
    if (zoneOut != NULL)
    {
        *zoneOut = NULL;
    }
    if (!_modelBBoxValid)
    {
        return -1;
    }
    csmFloat32 bb[4];

    ComputeArtBBox(bb);
    if (bb[2] < bb[0] || (csmFloat32)x < bb[0] || (csmFloat32)x > bb[2]
        || (csmFloat32)y < bb[1] || (csmFloat32)y > bb[3])
    {
        /* 模型外（空白背景）：不是"戳到人"——留一条诊断（点击频率低，不刷屏）*/
        printf("[ui] 点击在人物外（%d,%d；人物 x[%.0f..%.0f] y[%.0f..%.0f]）\n",
               (int)x, (int)y, bb[0], bb[2], bb[1], bb[3]);
        fflush(stdout);
        return -1;
    }
    const csmInt32 zoneId = ClassifyZone((csmFloat32)x, (csmFloat32)y, bb);
    const csmInt32 zone = (zoneId == Z_HEAD || zoneId == Z_FACE || zoneId == Z_NECK) ? 0 : 1;

    // 区域分类不受冷却影响（连点计数要每次都拿到区域）；冷却只挡"反应"
    if (zoneOut != NULL && zoneId >= 0)
    {
        *zoneOut = ZoneKey(zoneId);
    }
    if (_tapCooldown > 0.0f)
    {
        // 冷却中：动作不重复起，但"看过去"照做（每一下都有回应）
        SetLookTargetPixels((csmFloat32)x, (csmFloat32)y);
        _lookOverrideUntil = _perfClock + 1.2f;
        return zone;
    }
    _tapCooldown = 0.8f;
    NoteExternalActivity();
    // 桌宠：按 16 区各自反应（连点计数来自 NotePetClick 的累计）
    if (_isPet && zoneId >= 0)
    {
        ZoneReact(zoneId, 0, (_clickZone == zoneId ? _clickCount + 1 : 1), (csmFloat32)x, (csmFloat32)y);
        return zone;
    }
    printf("[ui] 戳到%s\n", ZoneLabel(zoneId));
    fflush(stdout);

    // ① 模型自带的 Tap 系列动作（有素材的模型按它自己设计的方式反应）
    for (csmInt32 g = 0; g < MotionGroupCount(); ++g)
    {
        const char* gn = MotionGroupName(g);

        if (strncmp(gn, "Tap", 3) == 0 && MotionCount(g) > 0)
        {
            StartMotionByGroupIndex(g, (csmInt32)(NextRand() % (csmUint32)MotionCount(g)));
            return zone;
        }
    }

    // ② profile reactions → ③ 内置兜底
    const std::string* gName = NULL;
    const std::string* eName = NULL;

    if (_hasReaction[zone])
    {
        gName = &_reactions[zone].gesture;
        eName = &_reactions[zone].expr;
    }
    else if (_hasReaction[2])
    {
        gName = &_reactions[2].gesture;
        eName = &_reactions[2].expr;
    }
    else
    {
        static const std::string kHeadG = "tilt";
        static const std::string kBodyG = "surprise";
        gName = (zone == 0) ? &kHeadG : &kBodyG;
    }
    if (gName != NULL && !gName->empty())
    {
        StartGesture(gName->c_str());
    }
    if (eName != NULL && !eName->empty())
    {
        if (*eName == "next")
        {
            NextExpression();
        }
        else
        {
            StartExpressionByName(eName->c_str());
        }
    }
    return zone;
}

/** 写能力缓存（JSON；原子：tmp + rename）。探测跑完自动调 */
csmBool L2DPlayer::SaveCapabilityCache()
{
    if (_cap.empty() || _fingerprint == 0)
    {
        return false;
    }
    const std::string path = CachePath();
    const std::string tmp = path + ".tmp";
    FILE* f = fopen(tmp.c_str(), "wb");

    if (f == NULL)
    {
        return false;
    }
    const std::string fpText = FingerprintText();
    const std::string modelName = BaseName(_dir);

    fprintf(f, "{\"v\":1,\"model\":\"%s\",\"fp\":\"%s\",\"params\":%d,\"eff\":[",
            modelName.c_str(), fpText.c_str(), (int)_cap.size());
    bool first = true;
    csmInt32 probed = 0;

    for (size_t i = 0; i < _cap.size(); ++i)
    {
        if ((_cap[i].flags & kCapKnown) == 0)
        {
            continue;
        }
        ++probed;
        const char* pid = _model->GetParameterId((csmUint32)i)->GetString().GetRawString();
        const char* code = CapCode(_cap[i].flags);

        fprintf(f, "%s[\"%s\",\"%s\",%.4f]", first ? "" : ",", pid, code,
                (double)_cap[i].delta);
        first = false;
    }
    fprintf(f, "],\"probed\":%d", (int)probed);
    /* parts：肢体部件名单（"哪些 drawable 是手臂"，由探测学习）——运行时刚性抬手用 */
    if (!_parts.empty())
    {
        fprintf(f, ",\"parts\":{");
        bool firstPart = true;

        for (std::map<std::string, std::vector<csmInt32> >::const_iterator it = _parts.begin();
             it != _parts.end(); ++it)
        {
            fprintf(f, "%s\"%s\":[", firstPart ? "" : ",", it->first.c_str());
            for (size_t i = 0; i < it->second.size(); ++i)
            {
                fprintf(f, "%s%d", i ? "," : "", (int)it->second[i]);
            }
            fprintf(f, "]");
            firstPart = false;
        }
        fprintf(f, "}");
    }
    fprintf(f, "}\n");
    const bool closed = (fclose(f) == 0);

    if (!closed || !plat::ReplaceFile(tmp, path))
    {
        remove(tmp.c_str());
        return false;
    }
    return true;
}

/** 读能力缓存（指纹对不上 = 模型换过包，忽略并清零重测）。返回载入项数 */
csmInt32 L2DPlayer::LoadCapabilityCache()
{
    if (_cap.empty() || _fingerprint == 0)
    {
        return 0;
    }
    const std::string path = CachePath();
    FILE* f = fopen(path.c_str(), "rb");

    if (f == NULL)
    {
        return 0;
    }
    std::string text;
    char buf[4096];
    size_t got;

    while ((got = fread(buf, 1, sizeof(buf), f)) > 0)
    {
        text.append(buf, got);
        if (text.size() > (4u << 20))
        {
            break;   // 上限保护
        }
    }
    fclose(f);

    jsonlite::Value root;
    std::string perr;

    if (!jsonlite::parse(text.c_str(), text.size(), root, &perr))
    {
        printf("[cap] 缓存解析失败（忽略，稍后可重测）：%s\n", perr.c_str());
        return 0;
    }
    if (!root.isObj() || strcmp(root.get("fp").strOr(""), FingerprintText().c_str()) != 0)
    {
        printf("[cap] 缓存指纹不符（模型已变，忽略）\n");
        return 0;
    }
    const jsonlite::Value& arr = root.get("eff");
    csmInt32 loaded = 0;

    if (!arr.isArr())
    {
        return 0;
    }
    for (size_t k = 0; k < arr.arr.size(); ++k)
    {
        const jsonlite::Value& e = arr.arr[k];

        if (!e.isArr() || e.arr.size() < 2)
        {
            continue;
        }
        const char* pid = e.arr[0].strOr("");

        if (*pid == '\0')
        {
            continue;
        }
        const csmInt32 idx = _model->GetParameterIndex(
            CubismFramework::GetIdManager()->GetId(pid));

        if (idx < 0)
        {
            continue;
        }
        ParamCap& cap = _cap[(size_t)idx];

        cap.flags = CapParseCode(e.arr[1].strOr(""));
        if (e.arr.size() >= 3)
        {
            cap.delta = (csmFloat32)e.arr[2].numOr(0.0);
        }
        ++loaded;
    }
    /* parts：肢体部件名单（探测学到的"哪些 drawable 是手臂"） */
    const jsonlite::Value& parts = root.get("parts");

    if (parts.isObj())
    {
        for (size_t k = 0; k < parts.obj.size(); ++k)
        {
            const jsonlite::Value& arr2 = parts.obj[k].second;

            if (!arr2.isArr())
            {
                continue;
            }
            std::vector<csmInt32>& dst = _parts[parts.obj[k].first];

            dst.clear();
            for (size_t i = 0; i < arr2.arr.size(); ++i)
            {
                if (arr2.arr[i].isNum())
                {
                    dst.push_back((csmInt32)arr2.arr[i].num);
                }
            }
        }
        printf("[cap] 肢体部件：");
        for (std::map<std::string, std::vector<csmInt32> >::const_iterator it = _parts.begin();
             it != _parts.end(); ++it)
        {
            printf("%s=%d件 ", it->first.c_str(), (int)it->second.size());
        }
        printf("\n");
        fflush(stdout);
    }
    printf("[cap] 能力缓存载入 %d 项：%s\n", (int)loaded, path.c_str());
    return loaded;
}

/** 能力表（写入指令环 caps 区；宿主据此应答 LIST——它不解析 model3.json）*/
void L2DPlayer::BuildCapabilityText(char* out, size_t cap)
{
    size_t n = 0;
    n += (size_t)snprintf(out + n, cap > n ? cap - n : 0, "MODEL=%s\n",
                          BaseName(_dir).c_str());
    // FP：模型指纹（外部下发前记下它，指令行首 "@<fp> " 防串扰，见 L2dCmd.h）
    n += (size_t)snprintf(out + n, cap > n ? cap - n : 0, "FP=%s\n",
                          FingerprintText().c_str());
    n += (size_t)snprintf(out + n, cap > n ? cap - n : 0, "EXPR=%d:",
                          (int)_expressionNames.size());
    for (size_t i = 0; i < _expressionNames.size(); ++i)
    {
        n += (size_t)snprintf(out + n, cap > n ? cap - n : 0, "%s%s",
                              i ? "," : "", _expressionNames[i].c_str());
    }
    n += (size_t)snprintf(out + n, cap > n ? cap - n : 0, "\nMOTION=");
    for (csmInt32 g = 0; g < MotionGroupCount(); ++g)
    {
        n += (size_t)snprintf(out + n, cap > n ? cap - n : 0, "%s%s:%d",
                              g ? ";" : "", MotionGroupName(g), (int)MotionCount(g));
    }
    n += (size_t)snprintf(out + n, cap > n ? cap - n : 0, "\nGESTURE=");
    for (size_t i = 0; i < _gestures.size(); ++i)
    {
        n += (size_t)snprintf(out + n, cap > n ? cap - n : 0, "%s%s",
                              i ? "," : "", _gestures[i].name.c_str());
    }
    n += (size_t)snprintf(out + n, cap > n ? cap - n : 0, "\nIDLE=%d\n", (int)_idleLevel);

    // BODYCHANNELS：躯体通道清单（BODY 命令可设定；见 ApplyBodyChannels）。
    // 注意放在 PARAMGROUPS **之前**：后者（全部参数名）很长，放后面会被挤没。
    n += (size_t)snprintf(out + n, cap > n ? cap - n : 0, "BODYCHANNELS=");
    BodyChannelListText(out + n, cap > n ? cap - n : 0);
    n += strlen(out + n);   // 推进游标（否则下一句 snprintf 把清单原地覆盖掉）
    n += (size_t)snprintf(out + n, cap > n ? cap - n : 0, "\n");
    // BODYINFO：每通道"增量范围 + 方式"（joint/p/d），LLM 据此定幅度
    n += (size_t)snprintf(out + n, cap > n ? cap - n : 0, "BODYINFO=");
    BodyInfoText(out + n, cap > n ? cap - n : 0);
    n += strlen(out + n);
    // ACTIONS：动作片段库（ACTION <名> / 脚本 "action" 步）+ 动捕感强度
    n += (size_t)snprintf(out + n, cap > n ? cap - n : 0, "\nACTIONS=");
    ActionListText(out + n, cap > n ? cap - n : 0);
    n += strlen(out + n);
    n += (size_t)snprintf(out + n, cap > n ? cap - n : 0, "\nMOCAP=%d\n", (int)_mocapLevel);
    // SKELETON / ANCHORS：手臂关节骨架（关节角范围）+ IK 锚点（BODY hand.r=<锚点>）
    n += (size_t)snprintf(out + n, cap > n ? cap - n : 0, "SKELETON=");
    SkeletonText(out + n, cap > n ? cap - n : 0);
    n += strlen(out + n);
    n += (size_t)snprintf(out + n, cap > n ? cap - n : 0, "\nARMSETS=");
    ArmSetText(out + n, cap > n ? cap - n : 0, false);
    n += strlen(out + n);
    n += (size_t)snprintf(out + n, cap > n ? cap - n : 0, "\nANCHORS=%s\n", _rigs.empty() ? "" :
                          "mouth,face,eyes,chin,head_top,chest,forward,waist,hip,side,up,head_side,cheek");

    // PARAMGROUPS：cdi3 的参数分组（身体/手/头发/裙…）——让外部（LLM）知道能控制哪些部位
    n += (size_t)snprintf(out + n, cap > n ? cap - n : 0, "PARAMGROUPS=");
    for (size_t gi = 0; gi < _groupOrder.size(); ++gi)
    {
        const int g = _groupOrder[gi];
        n += (size_t)snprintf(out + n, cap > n ? cap - n : 0, "%s%s:", gi ? ";" : "",
                              _groupNames[(size_t)g].c_str());
        bool first = true;
        for (csmInt32 i = 0; i < _model->GetParameterCount(); ++i)
        {
            if (_groupOf[(size_t)i] != g)
            {
                continue;
            }
            n += (size_t)snprintf(out + n, cap > n ? cap - n : 0, "%s%s",
                                  first ? "" : ",", _model->GetParameterId(i)->GetString().GetRawString());
            first = false;
        }
    }
    n += (size_t)snprintf(out + n, cap > n ? cap - n : 0, "\n");
    (void)n;
}

/** 全量参数表（写指令环 params 区；PARAMS 查询用）：
 *  P <id> <默认> <最小> <最大> <组名> <中文名> eff=<码>[:<顶点位移>]（中文/组名来自 cdi3，缺则 "-"；
 *  eff 来自能力探测/缓存：d=顶点直连 p=经物理 a=换美术 -=无效果 ?=未探测）*/
void L2DPlayer::BuildParamTable(char* out, size_t cap)
{
    size_t n = 0;
    const csmInt32 pc = _model->GetParameterCount();

    n += (size_t)snprintf(out + n, cap > n ? cap - n : 0, "PARAMS=%d\n", (int)pc);
    n += (size_t)snprintf(out + n, cap > n ? cap - n : 0,
                          "# eff: d=顶点直连 p=仅经物理链 a=换美术(不透明度) -=无效果 ?=未探测\n");
    for (csmInt32 i = 0; i < pc; ++i)
    {
        if (cap < n + 190)
        {
            n += (size_t)snprintf(out + n, cap > n ? cap - n : 0, "...（截断）\n");
            break;
        }
        const char* group = "-";
        const char* pname = "-";

        if (_groupOf[(size_t)i] >= 0 && _groupOf[(size_t)i] < (csmInt32)_groupNames.size())
        {
            group = _groupNames[(size_t)_groupOf[(size_t)i]].c_str();
        }
        if (_paramName[(size_t)i].empty() == false)
        {
            pname = _paramName[(size_t)i].c_str();
        }
        n += (size_t)snprintf(out + n, cap > n ? cap - n : 0, "P %s %.3f %.3f %.3f %s %s",
                              _model->GetParameterId(i)->GetString().GetRawString(),
                              _model->GetParameterDefaultValue((csmUint32)i),
                              _model->GetParameterMinimumValue((csmUint32)i),
                              _model->GetParameterMaximumValue((csmUint32)i),
                              group, pname);
        const csmUint8 flags = (i < (csmInt32)_cap.size()) ? _cap[(size_t)i].flags : 0;

        n += (size_t)snprintf(out + n, cap > n ? cap - n : 0, " eff=%s", CapCode(flags));
        if ((flags & (kCapDirect | kCapPhys)) != 0)
        {
            n += (size_t)snprintf(out + n, cap > n ? cap - n : 0, ":%.3f",
                                  (double)_cap[(size_t)i].delta);
        }
        n += (size_t)snprintf(out + n, cap > n ? cap - n : 0, "\n");
    }
}

/** 右上角 FPS + 底部缩放条（在模型/球之后绘制，位于最上层） */
void L2DPlayer::DrawUi()
{
    CubismRenderer_Software* r = SoftwareRenderer();
    if (r == NULL || _targetW < 8.0f || g_uiOff)
    {
        return;
    }
    struct timespec tsA, tsB;
    clock_gettime(CLOCK_MONOTONIC, &tsA);
    const csmFloat32 W = _targetW;
    const csmFloat32 H = _targetH;

    const bool uiProf = UiProfOn();
    const double tA = uiProf ? UiNow() : 0.0;
    // --- 顶部状态栏：CPU / RAM / FPS（config 可关）---
    if (L2D_UI_STATUS_BAR)
    {
        char buf[64];
        snprintf(buf, sizeof(buf), "CPU %d%% RAM %d%% FPS %d",
                 _sysStats.Cpu(), _sysStats.Mem(), _fpsValue);
        const csmFloat32 textW = (csmFloat32)(strlen(buf) * 6) * 3.0f;
        r->FillRect(6.0f, 8.0f, textW + 24.0f, 38.0f, 0x00101018, 150);
        r->DrawText5x7(14.0f, 13.0f, 3, buf, L2D_UI_COLOR_ACCENT);
    }

    const double tB = uiProf ? UiNow() : 0.0;
    if (uiProf) { UiStat()[0] += tB - tA; }   // 状态栏
    // --- 跟随目标圆环（可视化"角色正在看的位置"；config 可关）---
    if (L2D_UI_TARGET_RING && _followEnabled)
    {
        const csmFloat32 tx = _followHasTarget ? _followTargetX : W * 0.5f;
        const csmFloat32 ty = _followHasTarget ? _followTargetY : H * 0.5f;
        const csmUint32 ringColor = _touchActive ? L2D_UI_COLOR_RING_ACTIVE : L2D_UI_COLOR_RING_IDLE;
        r->DrawCircleRing(tx, ty, L2D_UI_RING_RADIUS, L2D_UI_RING_THICKNESS, ringColor);
        r->DrawFlatCircle(tx, ty, 3.0f, ringColor);
    }

    const double tC = uiProf ? UiNow() : 0.0;
    if (uiProf) { UiStat()[1] += tC - tB; }   // 圆环
    // --- 底部控制条（三段）：[◀ 切模型] [缩放滚动] [▶ 切模型]（config 可关）---
    if (L2D_UI_ZOOM_SLIDER)
    {
        const csmFloat32 arrowW = W * 0.15f;
        const csmFloat32 barY = H - H * 0.052f;
        const csmFloat32 barH = H * 0.013f;
        const csmFloat32 x0 = arrowW + 8.0f;
        const csmFloat32 x1 = W - arrowW - 8.0f;
        const csmFloat32 hx = x0 + (x1 - x0) * _zoomSliderPos;

        // 左右箭头（半透明底 + 三角；指向内）
        const csmFloat32 asz = W * 0.045f;
        r->FillRect(6.0f, barY - asz * 1.5f, arrowW - 6.0f, barY + asz * 1.5f, 0x00101018, 130);
        r->DrawTriangleFilled(arrowW * 0.66f, barY - asz, arrowW * 0.66f, barY + asz,
                              arrowW * 0.34f, barY, L2D_UI_COLOR_HANDLE);
        r->FillRect(W - arrowW + 6.0f, barY - asz * 1.5f, W - 6.0f, barY + asz * 1.5f, 0x00101018, 130);
        r->DrawTriangleFilled(W - arrowW * 0.66f, barY - asz, W - arrowW * 0.66f, barY + asz,
                              W - arrowW * 0.34f, barY, L2D_UI_COLOR_HANDLE);

        // 中间滚动条：轨道 + 已选段 + 刻度 + 把手
        r->FillRect(x0 - 6.0f, barY - barH * 1.7f, x1 + 6.0f, barY + barH * 1.7f, 0x00101018, 130);
        r->FillRect(x0, barY - barH * 0.5f, x1, barY + barH * 0.5f, L2D_UI_COLOR_SLIDER_TRACK, 200);
        r->FillRect(x0, barY - barH * 0.5f, hx, barY + barH * 0.5f, L2D_UI_COLOR_SLIDER_FILL, 230);
        for (int i = 0; i <= 4; ++i) // 刻度点（25% 一格）
        {
            const csmFloat32 tx = x0 + (x1 - x0) * (csmFloat32)i / 4.0f;
            r->DrawFlatCircle(tx, barY, 2.0f, L2D_UI_COLOR_HANDLE);
        }
        r->DrawFilledSphere(hx, barY, barH * 2.2f, L2D_UI_COLOR_HANDLE);
        char buf[16];
        snprintf(buf, sizeof(buf), "%d%%", (int)(_zoomSliderPos * 100.0f + 0.5f));
        r->DrawText5x7(hx - 15.0f, barY - barH * 1.7f - 26.0f, 2, buf, L2D_UI_COLOR_HANDLE);
    }

    // --- 切换模型后的名称提示条（顶部居中，短暂显示）---
    if (_modelNameToastTimer > 0.0f && !_modelName.empty())
    {
        const csmFloat32 textW = (csmFloat32)(_modelName.size() * 6) * 3.0f;
        const csmFloat32 bx = (W - textW - 28.0f) * 0.5f;
        const csmFloat32 alpha = _modelNameToastTimer < 0.4f
                               ? (csmUint8)(_modelNameToastTimer / 0.4f * 200.0f) : (csmUint8)200;
        r->FillRect(bx, 52.0f, bx + textW + 28.0f, 88.0f, 0x00101018, alpha);
        r->DrawText5x7(bx + 14.0f, 59.0f, 3, _modelName.c_str(), L2D_UI_COLOR_ACCENT);
    }

    clock_gettime(CLOCK_MONOTONIC, &tsB);
    const double tD = uiProf ? UiNow() : 0.0;
    if (uiProf) { UiStat()[2] += tD - tC; ++UiCount(); }   // 缩放条
    r->NoteUiTime((double)(tsB.tv_sec - tsA.tv_sec) + (double)(tsB.tv_nsec - tsA.tv_nsec) * 1e-9);
}

/** 自动适配：用当前可见顶点在屏幕空间的包围盒计算"等比最大填充"的 zoom */
csmFloat32 L2DPlayer::ComputeFitZoom(const CubismMatrix44& baseProjection, csmUint32 width, csmUint32 height)
{
    CubismMatrix44 full = baseProjection;
    full.MultiplyByMatrix(GetModelMatrix());
    const csmFloat32* m = full.GetArray();

    const CubismModel* model = GetModel();
    const csmInt32 drawableCount = model->GetDrawableCount();
    csmFloat32 minX = 1e9f, maxX = -1e9f, minY = 1e9f, maxY = -1e9f;

    for (csmInt32 d = 0; d < drawableCount; ++d)
    {
        if (!model->GetDrawableDynamicFlagIsVisible(d))
        {
            continue;
        }
        const csmInt32 vertexCount = model->GetDrawableVertexCount(d);
        const Live2D::Cubism::Core::csmVector2* positions = model->GetDrawableVertexPositions(d);
        if (positions == NULL)
        {
            continue;
        }
        for (csmInt32 v = 0; v < vertexCount; ++v)
        {
            const csmFloat32 x = positions[v].X;
            const csmFloat32 y = positions[v].Y;
            const csmFloat32 px = x * m[0] + y * m[4] + m[12];
            const csmFloat32 py = x * m[1] + y * m[5] + m[13];
            const csmFloat32 pw = x * m[3] + y * m[7] + m[15];
            const csmFloat32 iw = (pw != 0.0f) ? 1.0f / pw : 1.0f;
            const csmFloat32 sx = (px * iw + 1.0f) * 0.5f * (csmFloat32)width;
            const csmFloat32 sy = (1.0f - py * iw) * 0.5f * (csmFloat32)height;
            if (sx < minX) minX = sx;
            if (sx > maxX) maxX = sx;
            if (sy < minY) minY = sy;
            if (sy > maxY) maxY = sy;
        }
    }

    const csmFloat32 bw = maxX - minX;
    const csmFloat32 bh = maxY - minY;
    if (bw < 1.0f || bh < 1.0f)
    {
        return 1.0f;
    }

    const csmFloat32 zoom = ((csmFloat32)width / bw < (csmFloat32)height / bh)
                          ? (csmFloat32)width / bw : (csmFloat32)height / bh;
    // 顶点会因呼吸/物理小幅摆动，乘以填充率留边距
    return zoom * _fitFill;
}

/** 一帧渲染（官方 Draw 语义：mvp = projection × modelMatrix，随后 DrawModel） */
void L2DPlayer::Render()
{
    CubismRenderer_Software* renderer = GetRenderer<CubismRenderer_Software>();
    if (renderer == NULL)
    {
        return;
    }

    const csmUint32 width = _targetW > 0.0f ? (csmUint32)_targetW : 480;
    const csmUint32 height = _targetH > 0.0f ? (csmUint32)_targetH : 800;

    CubismMatrix44 projection;
    BuildBaseProjection(&projection, width, height);

    if (_autoFit && _renderZoom <= 0.0f)
    {
        _renderZoom = ComputeFitZoom(projection, width, height);
        printf("[fit] 自动适配 zoom=%.3f（渲染目标 %ux%u，填充率 %.2f）\n",
               _renderZoom, width, height, _fitFill);
        fflush(stdout);
    }
    if (_renderZoom != 1.0f && _renderZoom > 0.0f)
    {
        // 围绕屏幕中心（NDC 原点）等比放大模型。
        // 注意：必须用 ScaleRelative（累乘）——CubismMatrix44::Scale 是赋值语义
        // （直接写 _tr[0]/_tr[5]），会覆盖 BuildBaseProjection 里的
        // 像素宽高比修正（Scale(1, w/h)），导致 zoom≠1 时画面纵向拉长！
        projection.ScaleRelative(_renderZoom, _renderZoom);
    }
    projection.MultiplyByMatrix(GetModelMatrix());

    // 模型包围盒（canvas 尺寸 × 最终 mvp；无旋转取四角 min/max）——
    // 供"点击人物"命中判断；每帧更新以跟随缩放变化
    {
        const csmFloat32 hw = GetModel()->GetCanvasWidth() * 0.5f;
        const csmFloat32 hh = GetModel()->GetCanvasHeight() * 0.5f;
        const csmFloat32* mm = projection.GetArray();
        csmFloat32 bbMinX = 1e9f, bbMaxX = -1e9f, bbMinY = 1e9f, bbMaxY = -1e9f;
        const csmFloat32 cornersX[4] = {-hw, hw, -hw, hw};
        const csmFloat32 cornersY[4] = {-hh, -hh, hh, hh};
        for (int i = 0; i < 4; ++i)
        {
            const csmFloat32 px = cornersX[i] * mm[0] + cornersY[i] * mm[4] + mm[12];
            const csmFloat32 py = cornersX[i] * mm[1] + cornersY[i] * mm[5] + mm[13];
            const csmFloat32 pw = cornersX[i] * mm[3] + cornersY[i] * mm[7] + mm[15];
            const csmFloat32 iw = (pw != 0.0f) ? 1.0f / pw : 1.0f;
            const csmFloat32 sx = (px * iw + 1.0f) * 0.5f * (csmFloat32)width;
            const csmFloat32 sy = (1.0f - py * iw) * 0.5f * (csmFloat32)height;
            if (sx < bbMinX) bbMinX = sx;
            if (sx > bbMaxX) bbMaxX = sx;
            if (sy < bbMinY) bbMinY = sy;
            if (sy > bbMaxY) bbMaxY = sy;
        }
        _modelBBox[0] = bbMinX;
        _modelBBox[1] = bbMinY;
        _modelBBox[2] = bbMaxX;
        _modelBBox[3] = bbMaxY;
        _modelBBoxValid = true;
    }

    renderer->SetMvpMatrix(&projection);
    renderer->DrawModel();

    // UI（状态栏 / 缩放条）在最上层
    DrawUi();
}

/** 加载 model3.json 及其引用的全部资产，并装配官方更新管线 */
bool L2DPlayer::Load(const char* model3Path)
{
    _dir = DirName(model3Path);

    csmSizeInt size = 0;
    csmByte* buffer = ReadModel3(model3Path, &size);
    if (buffer == NULL)
    {
        return false;
    }
    {
        // 先自己校验：坏 JSON / 没有 FileReferences.Moc 的 model3 交给 Framework 会在 GetModelFileName() 里空指针崩
        jsonlite::Value mv;
        if (!jsonlite::parse((const char*)buffer, (size_t)size, mv, NULL) || !mv.isObj()
            || !mv.get("FileReferences").get("Moc").isStr())
        {
            fprintf(stderr, "[l2dtest] model3.json 不合法或缺少 FileReferences.Moc：%s\n", model3Path);
            free(buffer);
            return false;
        }
    }
    _setting = new CubismModelSettingJson(buffer, size);
    free(buffer);

    if (strcmp(_setting->GetModelFileName(), "") == 0)
    {
        fprintf(stderr, "[l2dtest] model3.json 未引用 moc 文件\n");
        return false;
    }

    // --- MOC3 ---
    {
        const std::string path = _dir + "/" + _setting->GetModelFileName();
        buffer = ReadFile(path.c_str(), &size);
        if (buffer == NULL)
        {
            return false;
        }
        // 开 Core 的一致性校验：模型包是外来文件（导入的 zip），坏的/恶意的 .moc3 先被拒，不让它进 Core 解析
        LoadModel(buffer, size, true);
        free(buffer);
        if (GetModel() == NULL)
        {
            fprintf(stderr, "[l2dtest] moc3 校验失败或损坏：%s\n", path.c_str());
            return false;
        }
    }

    // --- 表情 ---
    for (csmInt32 i = 0; i < _setting->GetExpressionCount(); ++i)
    {
        const csmString name = _setting->GetExpressionName(i);
        const std::string path = _dir + "/" + _setting->GetExpressionFileName(i);

        buffer = ReadJsonFile(path.c_str(), &size);
        if (buffer == NULL)
        {
            continue; // 表情为可选资产：单文件缺失不阻止模型加载
        }
        ACubismMotion* motion = LoadExpression(buffer, size, name.GetRawString());
        if (motion != NULL)
        {
            if (_expressions[name] != NULL)
            {
                ACubismMotion::Delete(_expressions[name]);
            }
            _expressions[name] = motion;
            _expressionNames.push_back(name.GetRawString()); // 供点击循环切换
        }
        free(buffer);
    }

    // --- 水印控制（模型自带"水印"表情 → 默认关闭）---
    SetupWatermarkControl();

    // --- 演出引擎：内置通用手势 + 模型 profile（l2d.profile.json，可选）---
    LoadModelDisplayInfo();    // cdi3：参数中文名/分组（能力表与参数清单用）
    LoadGestureProfile();      // 先读 profile（同名手势优先）
    BuildBuiltinGestures();    // 再补内置（已存在的不覆盖）
    BuildBuiltinActions();     // 动作片段库（通道空间；profile 的 actions 同名优先）

    // --- 物理 / 姿势 ---
    if (strcmp(_setting->GetPhysicsFileName(), "") != 0)
    {
        const std::string path = _dir + "/" + _setting->GetPhysicsFileName();
        buffer = ReadJsonFile(path.c_str(), &size);
        if (buffer != NULL)
        {
            LoadPhysics(buffer, size);
            free(buffer);
        }
    }
    if (strcmp(_setting->GetPoseFileName(), "") != 0)
    {
        const std::string path = _dir + "/" + _setting->GetPoseFileName();
        buffer = ReadJsonFile(path.c_str(), &size);
        if (buffer != NULL)
        {
            LoadPose(buffer, size);
            free(buffer);
        }
    }

    // --- 更新管线装配（顺序经 SortUpdatableList 决定）---
    _updateScheduler.AddUpdatableList(CSM_NEW CubismExpressionUpdater(*_expressionManager));
    if (_pose != NULL)
    {
        _updateScheduler.AddUpdatableList(CSM_NEW CubismPoseUpdater(*_pose));
    }
    if (_physics != NULL)
    {
        _updateScheduler.AddUpdatableList(CSM_NEW CubismPhysicsUpdater(*_physics));
    }
    if (_setting->GetEyeBlinkParameterCount() > 0)
    {
        _eyeBlink = CubismEyeBlink::Create(_setting);
        _updateScheduler.AddUpdatableList(CSM_NEW CubismEyeBlinkUpdater(_motionUpdated, *_eyeBlink));
    }
    _breath = CubismBreath::Create();
    {
        CubismIdManager* ids = CubismFramework::GetIdManager();
        csmVector<CubismBreath::BreathParameterData> breathParameters;
        breathParameters.PushBack(CubismBreath::BreathParameterData(ids->GetId(DefaultParameterId::ParamAngleX), 0.0f, 15.0f, 6.5345f, 0.5f));
        breathParameters.PushBack(CubismBreath::BreathParameterData(ids->GetId(DefaultParameterId::ParamAngleY), 0.0f, 8.0f, 3.5345f, 0.5f));
        breathParameters.PushBack(CubismBreath::BreathParameterData(ids->GetId(DefaultParameterId::ParamAngleZ), 0.0f, 10.0f, 5.5345f, 0.5f));
        breathParameters.PushBack(CubismBreath::BreathParameterData(ids->GetId(DefaultParameterId::ParamBodyAngleX), 0.0f, 4.0f, 15.5345f, 0.5f));
        breathParameters.PushBack(CubismBreath::BreathParameterData(ids->GetId(DefaultParameterId::ParamBreath), 0.5f, 0.5f, 3.2345f, 0.5f));
        _breath->SetParameters(breathParameters);
    }
    _updateScheduler.AddUpdatableList(CSM_NEW CubismBreathUpdater(*_breath));

    // --- 视线跟随（BongoCat/官方标准设计）---
    // CubismLook 按标准参数表分配跟随量；CubismLookUpdater 驱动 TargetPoint
    // 的速度模型（0.15s 加速/运动学减速）并写入参数——缺哪个参数自动忽略，
    // 因此对任意标准 Live2D 模型兼容。
    {
        _look = CubismLook::Create();
        CubismIdManager* ids = CubismFramework::GetIdManager();

        csmVector<CubismLook::LookParameterData> lookParameters;
        lookParameters.PushBack(CubismLook::LookParameterData(
            ids->GetId(DefaultParameterId::ParamAngleX), L2D_LOOK_ANGLE_X));
        lookParameters.PushBack(CubismLook::LookParameterData(
            ids->GetId(DefaultParameterId::ParamAngleY), 0.0f, L2D_LOOK_ANGLE_Y));
        lookParameters.PushBack(CubismLook::LookParameterData(
            ids->GetId(DefaultParameterId::ParamAngleZ), 0.0f, 0.0f, -L2D_LOOK_ANGLE_Z));
        lookParameters.PushBack(CubismLook::LookParameterData(
            ids->GetId(DefaultParameterId::ParamBodyAngleX), L2D_LOOK_BODY_ANGLE_X));
        lookParameters.PushBack(CubismLook::LookParameterData(
            ids->GetId(DefaultParameterId::ParamEyeBallX), L2D_LOOK_EYEBALL_X));
        lookParameters.PushBack(CubismLook::LookParameterData(
            ids->GetId(DefaultParameterId::ParamEyeBallY), 0.0f, L2D_LOOK_EYEBALL_Y));
        _look->SetParameters(lookParameters);

        _updateScheduler.AddUpdatableList(CSM_NEW CubismLookUpdater(*_look, *_dragManager));
    }

    _updateScheduler.SortUpdatableList();

    // --- 动作组预加载 ---
    for (csmInt32 i = 0; i < _setting->GetMotionGroupCount(); ++i)
    {
        PreloadMotionGroup(_setting->GetMotionGroupName(i));
    }
    _motionManager->StopAllMotions();

    // --- 布局 → 模型矩阵；参数落一次基线 ---
    csmMap<csmString, csmFloat32> layout;
    _setting->GetLayoutMap(layout);
    GetModelMatrix()->SetupFromLayout(layout);
    _model->SaveParameters();

    // --- 指纹 + 能力表：参数已齐，算指纹；缓存指纹对得上就秒填 eff（否则等探测）---
    _fingerprint = ComputeFingerprint();
    _cap.assign((size_t)_model->GetParameterCount(), ParamCap());
    LoadCapabilityCache();

    // --- 躯体通道（BODY）：标准名映射 + profile 覆盖（见 ApplyBodyChannels）---
    BuildBodyChannels();

    return true;
}

/** 官方更新管线：motion → scheduler(表情/物理/姿势/眨眼/呼吸) → 球跟随覆盖 → Core 变形 */
void L2DPlayer::Update(csmFloat32 dt)
{
    _model->LoadParameters();

    SceneTick(dt); // 演出时钟：触发脚本步骤（表情/动作/视线/手势）、维护队列、自主待机
    PetTick();     // 桌宠协议的时间线（PROTOCOL-LLM.md）
    ZoneReactTick();

    _motionUpdated = false;
    if (!_noMotion)
    {
        if (_motionManager->IsFinished())
        {
            StartFirstMotion();
        }
        else
        {
            _motionUpdated = _motionManager->UpdateMotion(_model, dt);
        }
    }

    UpdateFollowTarget(dt); // 触摸目标 → CubismTargetPoint（必须在 scheduler 之前：
                            // LookUpdater 在 scheduler 内读取最新目标并做速度平滑）
    _model->SaveParameters();
    // 躯体通道（pre 层）：写在 SaveParameters 之后、scheduler 之前——物理链看得到，
    // 且不会被存进基线（否则每帧累加）。见 ApplyBodyChannelsPre 注释。
    ApplyBodyChannelsPre(dt);
    _updateScheduler.OnLateUpdate(_model, dt);
    if (_exprCooldown > 0.0f)
    {
        _exprCooldown -= dt;
    }
    if (_tapCooldown > 0.0f)
    {
        _tapCooldown -= dt;
    }
    if (_modelNameToastTimer > 0.0f)
    {
        _modelNameToastTimer -= dt;
    }
    if (_modelSwitchTest)
    {
        _modelSwitchTimer += dt;
        if (_modelSwitchTimer >= 3.0f)
        {
            _modelSwitchTimer = 0.0f;
            _modelSwitchRequest = 1; // 请求切下一个（main 执行）
        }
    }
    if (_exprTest)
    {
        _exprTimer += dt;
        if (_exprTimer >= 2.0f)
        {
            _exprTimer = 0.0f;
            NextExpression();
        }
    }
    ApplyWatermarkControl(); // 在 Core 变形之前落最终值（见函数注释）
    ApplySceneTracks();      // 演出轨道（手势/脚本）也在此刻写最终值
    ApplyBodyChannelsPost(SoftwareRenderer() != NULL); // 躯体通道（post 层：肢体旋转/直连参数）
    ApplyTalk();             // 说话口型：写在最后（最高优先级，叠在表情姿态之上）
    _model->Update();
}

/**
 * 触摸跟随（BongoCat/官方设计）：只负责把"目标方向"交给 CubismTargetPoint；
 * 速度平滑（0.15s 加速/运动学减速，类似人转头）由官方实现，
 * 参数写入由 scheduler 内的 CubismLookUpdater 按标准 Look 参数表完成
 * （ParamAngleX/Y/Z ±30、ParamBodyAngleX ±10、ParamEyeBallX/Y ±1——
 *  缺哪个参数自动忽略，因此对所有标准模型兼容）。
 * 必须在 _updateScheduler.OnLateUpdate 之前调用（LookUpdater 要读最新目标）。
 */
void L2DPlayer::UpdateFollowTarget(csmFloat32 dt)
{
    if (!_followEnabled)
    {
        return;
    }
    const csmFloat32 W = _targetW;
    const csmFloat32 H = _targetH;
    if (W < 8.0f || H < 8.0f)
    {
        return;
    }

    // --follow-test：目标每 2.5s 移到下一角（四角循环）
    if (_followTest)
    {
        _followTestTimer += dt;
        if (_followTestTimer >= 2.5f)
        {
            _followTestTimer = 0.0f;
            static const csmFloat32 kCornerX[4] = {0.15f, 0.85f, 0.85f, 0.15f};
            static const csmFloat32 kCornerY[4] = {0.15f, 0.15f, 0.85f, 0.85f};
            _followTargetX = W * kCornerX[_followTestCorner];
            _followTargetY = H * kCornerY[_followTestCorner];
            _followHasTarget = true;
            printf("[follow-test] 目标 → (%.0f, %.0f) 角%d\n",
                   _followTargetX, _followTargetY, _followTestCorner);
            fflush(stdout);
            _followTestCorner = (_followTestCorner + 1) % 4;
        }
    }

    // 目标：手指当前位置；从未触摸过时注视屏幕中心；手指抬起后保持最后位置
    const csmFloat32 tx = _followHasTarget ? _followTargetX : W * 0.5f;
    const csmFloat32 ty = _followHasTarget ? _followTargetY : H * 0.5f;

    // 归一化到 [-1,1]；x 用屏幕方向（右=正），y 取反（屏幕向下为正 → 目标方向
    // 向上为正，与 ParamAngleY"正=抬头"的标准约定一致；实测上下方向取反后正确）
    const csmFloat32 nx = (tx - W * 0.5f) / (W * 0.5f);
    const csmFloat32 ny = -(ty - H * 0.5f) / (H * 0.5f);
    SetDragging(nx, ny);
}

/**
 * 水印控制：找到模型自带的"水印"表情（作者提供的开关——VTS 里按空格触发的就是它），
 * 记下它改哪些参数、怎么改（Blend）。只有带这个表情的模型才动参数，避免和别的模型
 * 里恰好同名的参数误撞（实测库里有模型也有 id 叫 "Param" 的参数，但不是水印）。
 * 判据见 PetWatermark.h（表情名或文件名含水印关键词；子目录里的 exp3 由
 * FileUtil.cpp 的 VTS 补全负责登记，否则 EXP/ 发货格式的模型根本看不到表情）。
 */
void L2DPlayer::SetupWatermarkControl()
{
    _watermarkParams.clear();
    if (_setting == NULL)
    {
        return;
    }
    for (csmInt32 i = 0; i < _setting->GetExpressionCount(); ++i)
    {
        const csmString name = _setting->GetExpressionName(i);
        const csmString file = _setting->GetExpressionFileName(i);
        if (!petwatermark::NameMatch(name.GetRawString()) && !petwatermark::NameMatch(file.GetRawString()))
        {
            continue;
        }

        csmSizeInt size = 0;
        csmByte* buffer = ReadFile((_dir + "/" + file.GetRawString()).c_str(), &size);
        if (buffer == NULL)
        {
            break; // 表情为可选资产：读不到就不管
        }
        CubismExpressionMotion* expr = CubismExpressionMotion::Create(buffer, size);
        free(buffer);
        if (expr == NULL)
        {
            break;
        }
        const csmVector<CubismExpressionMotion::ExpressionParameter>& params =
            expr->GetExpressionParameters();
        for (csmUint32 k = 0; k < params.GetSize(); ++k)
        {
            WatermarkParam wp;
            wp.id = params[k].ParameterId;
            wp.blend = params[k].BlendType;
            wp.value = params[k].Value;
            _watermarkParams.push_back(wp);
        }
        ACubismMotion::Delete(expr);
        printf("[ui] 水印控制：%s（%d 个参数）→ %s\n", name.GetRawString(),
               (int)_watermarkParams.size(), _watermarkOff ? "关（--watermark on 可保留）" : "保留");
        fflush(stdout);
        break;
    }
}

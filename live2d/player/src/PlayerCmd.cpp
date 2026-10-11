/**
 * PlayerCmd.cpp — L2DPlayer 的远程指令 / 演出引擎实现（从 l2dtest.cpp 拆出，纯搬移，逻辑未改）。
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
#include "PetSpeech.h"   // SpeechLangOk：笑话池按台词语言过滤（英文笑话别在中文界面里冒出来）
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

/** 说话口的每帧驱动（Update 尾段、轨道之后调用）*/
void L2DPlayer::ApplyTalk()
{
    if (_talkEnd <= 0.0f)
    {
        return;
    }
    if (!IsTalking())
    {
        printf("[talk] 说完\n");
        fflush(stdout);
        _talkEnd = 0.0f;
        return;   // 停止写入 → 嘴部参数下一帧回到表情/默认
    }
    if (_perfClock >= _talkNextAt)
    {
        const csmUint32 r = NextRand();

        if ((r % 100u) < 12u)
        {
            _talkViseme = 0;                              // 闭口/换气停顿
        }
        else
        {
            _talkViseme = 1 + (csmInt32)((r >> 8) % 5u);  // 1..5 = A I U E O
        }
        _talkNextAt = _perfClock + 0.10f + (csmFloat32)((r >> 16) % 19u) * 0.01f;
    }
    /* 目标值（一个 viseme 的形状）→ 一阶平滑（让张合有过程感）*/
    const csmFloat32 kSlew = 0.30f;
    const csmFloat32 tOpen = (_talkViseme == 0) ? 0.02f : 0.42f;
    const csmFloat32 tForm = (_talkViseme == 0) ? 0.0f : 0.25f;

    _talkOpen += (tOpen - _talkOpen) * kSlew;
    for (int i = 0; i < 5; ++i)
    {
        const csmFloat32 t = (_talkViseme == i + 1) ? 0.85f : 0.0f;

        _talkVowel[i] += (t - _talkVowel[i]) * kSlew;
    }
    /* 驱动嘴部参数（存在才写；命名覆盖官方标准 + 本模型实测表）*/
    SetParamIfExists("ParamJawOpenBL", _talkOpen);
    SetParamIfExists("ParamJawOpen", _talkOpen);   // 有的模型用 ParamJawOpen（yumi）
    SetParamIfExists("ParamMouthOpenY", _talkOpen * 0.9f);
    SetParamIfExists("ParamMouthOpenY2", _talkOpen * 0.8f);
    SetParamIfExists("ParamMouthForm", tForm);
    SetParamIfExists("ParamMouthForm2", tForm * 0.8f);
    SetParamIfExists("ParamSilence", 0.0f);   // 有的模型用 Silence 压嘴型（说话时放开）
    static const char* const kV[5] = { "ParamA", "ParamI", "ParamU", "ParamE", "ParamO" };

    for (int i = 0; i < 5; ++i)
    {
        SetParamIfExists(kV[i], _talkVowel[i]);
    }
}

// ===================== 动作片段（ACTION："像动捕一样"的整身动作） =====================
// 写在**躯体通道空间**（arm.r / head.y / body.z …），不是参数空间——同一条
// `ACTION cheer` 在舒芙蕾上走肩关节旋转、在 yumi 上走作者 armup、在 Haru 上走两段臂，
// 模型缺的通道自动跳过（回复里列出来，LLM 知道这台模型做不全）。
// 关键帧按 Catmull-Rom 样条求值（过关键帧不停顿 = 连贯，不是"摆拍到摆拍"），
// 叠加在 BODY 设定的姿态之上（可以"举着手的同时笑"）；同一通道上新动作接管旧动作
// （旧的 0.25s 淡出，不跳变）。

/** 内置动作库（跨模型；profile 的 "actions" 段可同名覆盖/追加）*/
void L2DPlayer::BuildBuiltinActions()
{
    struct K { csmFloat32 t, v; };
    // 一条轨道：通道关键帧；或 IK 窗口（ch="hand.r"/"hand.l"，anchor=锚点，keys 的首/末时刻 = 起止）
    struct Tr { const char* ch; std::initializer_list<K> keys; const char* anchor; };
    auto add = [this](const char* name, const char* desc, csmFloat32 dur,
                      std::initializer_list<Tr> tracks)
    {
        for (size_t i = 0; i < _actions.size(); ++i)
        {
            if (_actions[i].name == name)
            {
                return;   // profile 已定义同名：profile 优先
            }
        }
        ActionDef a;

        a.name = name;
        a.desc = desc;
        a.dur = dur;
        for (const Tr& tr : tracks)
        {
            ActionTrack t;

            t.ch = tr.ch;
            t.anchor = tr.anchor != NULL ? tr.anchor : "";
            for (const K& k : tr.keys)
            {
                t.keys.push_back(std::make_pair(k.t, k.v));
            }
            a.tracks.push_back(t);
        }
        _actions.push_back(a);
    };
    // 手臂走**关节**（只在肩/肘/腕弯）：shoulder 正=外展抬起；elbow/wrist 正=往内侧折、
    // 负=往外侧折（抬臂时前臂竖起 = 挥手/举手）。"手放到哪"用 IK 窗口（hand.r + 锚点）。
    // 头 °；身 °；眼=开度增量（负=眯）。同一组数字左右臂通用（通道已按左右镜像归一）。
    add("wave_big", "举手挥舞", 2.6f, {
        { "arm.r.shoulder", {{0,0},{0.15f,72},{0.85f,72},{1,0}}, NULL },
        { "arm.r.elbow", {{0,0},{0.15f,-80},{0.25f,-55},{0.35f,-105},{0.45f,-55},{0.55f,-105},
                          {0.65f,-55},{0.75f,-100},{0.88f,-60},{1,0}}, NULL },
        { "arm.r.wrist", {{0,0},{0.25f,12},{0.35f,-12},{0.45f,12},{0.55f,-12},{0.65f,12},{0.75f,-8},{1,0}}, NULL },
        { "head.z", {{0,0},{0.2f,7},{0.75f,7},{1,0}}, NULL },
        { "head.x", {{0,0},{0.2f,5},{0.8f,5},{1,0}}, NULL },
        { "body.z", {{0,0},{0.2f,-2.5f},{0.8f,-2.5f},{1,0}}, NULL },
        { "mouth.form", {{0,0},{0.15f,0.8f},{0.85f,0.8f},{1,0}}, NULL },
    });
    add("wave_both", "双手挥舞", 2.6f, {
        { "arm.r.shoulder", {{0,0},{0.15f,70},{0.85f,70},{1,0}}, NULL },
        { "arm.l.shoulder", {{0,0},{0.15f,70},{0.85f,70},{1,0}}, NULL },
        { "arm.r.elbow", {{0,0},{0.15f,-80},{0.25f,-55},{0.35f,-105},{0.45f,-55},{0.55f,-105},{0.65f,-55},{0.75f,-100},{1,0}}, NULL },
        { "arm.l.elbow", {{0,0},{0.15f,-80},{0.25f,-105},{0.35f,-55},{0.45f,-105},{0.55f,-55},{0.65f,-105},{0.75f,-60},{1,0}}, NULL },
        { "head.y", {{0,0},{0.2f,6},{0.8f,6},{1,0}}, NULL },
        { "mouth.form", {{0,0},{0.15f,1},{0.85f,1},{1,0}}, NULL },
    });
    add("cheer", "欢呼", 2.2f, {
        { "arm.r.shoulder", {{0,0},{0.1f,-8},{0.28f,88},{0.85f,85},{1,0}}, NULL },
        { "arm.l.shoulder", {{0,0},{0.1f,-8},{0.28f,88},{0.85f,85},{1,0}}, NULL },
        { "arm.r.elbow", {{0,0},{0.1f,20},{0.28f,-70},{0.42f,-45},{0.56f,-75},{0.7f,-45},{0.84f,-70},{1,0}}, NULL },
        { "arm.l.elbow", {{0,0},{0.1f,20},{0.28f,-70},{0.42f,-45},{0.56f,-75},{0.7f,-45},{0.84f,-70},{1,0}}, NULL },
        { "head.y", {{0,0},{0.1f,-4},{0.28f,12},{0.85f,10},{1,0}}, NULL },
        { "body.y", {{0,0},{0.1f,-3},{0.28f,4},{0.42f,0},{0.56f,4},{0.7f,0},{0.84f,3},{1,0}}, NULL },
        { "mouth.open", {{0,0},{0.25f,0.7f},{0.85f,0.6f},{1,0}}, NULL },
        { "eye.l", {{0,0},{0.25f,-0.35f},{0.85f,-0.35f},{1,0}}, NULL },
        { "eye.r", {{0,0},{0.25f,-0.35f},{0.85f,-0.35f},{1,0}}, NULL },
        { "brow.l", {{0,0},{0.25f,0.6f},{0.85f,0.6f},{1,0}}, NULL },
        { "brow.r", {{0,0},{0.25f,0.6f},{0.85f,0.6f},{1,0}}, NULL },
    });
    add("fist_pump", "握拳打气", 1.8f, {
        { "arm.r.shoulder", {{0,0},{0.12f,-5},{0.3f,55},{0.8f,55},{1,0}}, NULL },
        { "arm.r.elbow", {{0,0},{0.12f,30},{0.3f,-120},{0.42f,-85},{0.56f,-125},{0.68f,-85},{0.8f,-115},{1,0}}, NULL },
        { "head.y", {{0,0},{0.12f,-5},{0.3f,8},{0.8f,7},{1,0}}, NULL },
        { "head.z", {{0,0},{0.3f,-5},{0.8f,-5},{1,0}}, NULL },
        { "mouth.open", {{0,0},{0.3f,0.5f},{0.8f,0.4f},{1,0}}, NULL },
        { "brow.l", {{0,0},{0.2f,-0.4f},{0.85f,-0.4f},{1,0}}, NULL },
        { "brow.r", {{0,0},{0.2f,-0.4f},{0.85f,-0.4f},{1,0}}, NULL },
    });
    add("shrug", "耸肩摊手", 1.8f, {
        { "arm.r.shoulder", {{0,0},{0.25f,22},{0.7f,22},{1,0}}, NULL },
        { "arm.l.shoulder", {{0,0},{0.25f,22},{0.7f,22},{1,0}}, NULL },
        { "arm.r.elbow", {{0,0},{0.25f,-75},{0.7f,-75},{1,0}}, NULL },
        { "arm.l.elbow", {{0,0},{0.25f,-75},{0.7f,-75},{1,0}}, NULL },
        { "arm.r.wrist", {{0,0},{0.25f,-25},{0.7f,-25},{1,0}}, NULL },
        { "arm.l.wrist", {{0,0},{0.25f,-25},{0.7f,-25},{1,0}}, NULL },
        { "head.z", {{0,0},{0.25f,12},{0.7f,12},{1,0}}, NULL },
        { "head.y", {{0,0},{0.25f,4},{0.7f,4},{1,0}}, NULL },
        { "body.y", {{0,0},{0.25f,3},{0.7f,3},{1,0}}, NULL },
        { "brow.l", {{0,0},{0.25f,0.7f},{0.7f,0.7f},{1,0}}, NULL },
        { "brow.r", {{0,0},{0.25f,0.7f},{0.7f,0.7f},{1,0}}, NULL },
        { "mouth.form", {{0,0},{0.25f,-0.4f},{0.7f,-0.4f},{1,0}}, NULL },
    });
    add("think", "思考（歪头看上方）", 3.2f, {
        { "hand.r", {{0.05f,1},{0.85f,0}}, "chin" },
        { "head.z", {{0,0},{0.2f,9},{0.55f,6},{0.85f,9},{1,0}}, NULL },
        { "head.y", {{0,0},{0.2f,-5},{0.85f,-5},{1,0}}, NULL },
        { "head.x", {{0,0},{0.2f,-6},{0.85f,-6},{1,0}}, NULL },
        { "gaze.x", {{0,0},{0.2f,0.5f},{0.85f,0.5f},{1,0}}, NULL },
        { "gaze.y", {{0,0},{0.2f,0.6f},{0.85f,0.6f},{1,0}}, NULL },
        { "brow.l", {{0,0},{0.2f,0.5f},{0.85f,0.5f},{1,0}}, NULL },
        { "brow.r", {{0,0},{0.2f,-0.2f},{0.85f,-0.2f},{1,0}}, NULL },
        { "mouth.form", {{0,0},{0.2f,-0.3f},{0.85f,-0.3f},{1,0}}, NULL },
    });
    add("point", "指向前方", 2.2f, {
        { "arm.r.shoulder", {{0,0},{0.18f,78},{0.85f,75},{1,0}}, NULL },
        { "arm.r.elbow", {{0,0},{0.18f,-15},{0.85f,-10},{1,0}}, NULL },
        { "head.x", {{0,0},{0.18f,10},{0.85f,10},{1,0}}, NULL },
        { "head.y", {{0,0},{0.18f,3},{0.85f,3},{1,0}}, NULL },
        { "gaze.x", {{0,0},{0.18f,0.6f},{0.85f,0.6f},{1,0}}, NULL },
        { "mouth.form", {{0,0},{0.18f,0.4f},{0.85f,0.4f},{1,0}}, NULL },
    });
    add("bow", "低头致意", 2.4f, {
        { "hand.r", {{0.05f,1},{0.75f,0}}, "forward" },
        { "hand.l", {{0.05f,1},{0.75f,0}}, "forward" },
        { "head.y", {{0,0},{0.3f,-24},{0.65f,-24},{1,0}}, NULL },
        { "body.y", {{0,0},{0.3f,-9},{0.65f,-9},{1,0}}, NULL },
        { "eye.l", {{0,0},{0.3f,-0.6f},{0.65f,-0.6f},{1,0}}, NULL },
        { "eye.r", {{0,0},{0.3f,-0.6f},{0.65f,-0.6f},{1,0}}, NULL },
    });
    add("laugh", "哈哈大笑", 2.4f, {
        { "hand.r", {{0.05f,1},{0.85f,0}}, "mouth" },
        { "head.y", {{0,0},{0.1f,8},{0.2f,2},{0.3f,9},{0.4f,2},{0.5f,8},{0.6f,2},{0.7f,7},{1,0}}, NULL },
        { "head.z", {{0,0},{0.1f,6},{0.9f,6},{1,0}}, NULL },
        { "body.y", {{0,0},{0.1f,2},{0.2f,-1},{0.3f,2},{0.4f,-1},{0.5f,2},{0.6f,-1},{0.7f,2},{1,0}}, NULL },
        { "eye.l", {{0,0},{0.1f,-0.75f},{0.9f,-0.75f},{1,0}}, NULL },
        { "eye.r", {{0,0},{0.1f,-0.75f},{0.9f,-0.75f},{1,0}}, NULL },
        { "mouth.open", {{0,0},{0.1f,0.85f},{0.2f,0.5f},{0.3f,0.85f},{0.4f,0.5f},{0.5f,0.85f},{0.6f,0.5f},{0.7f,0.8f},{1,0}}, NULL },
        { "mouth.form", {{0,0},{0.1f,1},{0.9f,1},{1,0}}, NULL },
        { "arm.l.shoulder", {{0,0},{0.1f,12},{0.9f,12},{1,0}}, NULL },
        { "arm.l.elbow", {{0,0},{0.1f,70},{0.3f,80},{0.5f,70},{0.7f,80},{1,0}}, NULL },
    });
    add("surprise_big", "大吃一惊", 1.8f, {
        { "arm.r.shoulder", {{0,0},{0.08f,32},{0.7f,30},{1,0}}, NULL },
        { "arm.l.shoulder", {{0,0},{0.08f,32},{0.7f,30},{1,0}}, NULL },
        { "arm.r.elbow", {{0,0},{0.08f,-115},{0.7f,-110},{1,0}}, NULL },
        { "arm.l.elbow", {{0,0},{0.08f,-115},{0.7f,-110},{1,0}}, NULL },
        { "head.y", {{0,0},{0.08f,10},{0.7f,8},{1,0}}, NULL },
        { "body.y", {{0,0},{0.08f,-3},{0.7f,-2},{1,0}}, NULL },
        { "eye.l", {{0,0},{0.08f,0.35f},{0.7f,0.3f},{1,0}}, NULL },
        { "eye.r", {{0,0},{0.08f,0.35f},{0.7f,0.3f},{1,0}}, NULL },
        { "brow.l", {{0,0},{0.08f,1},{0.7f,0.9f},{1,0}}, NULL },
        { "brow.r", {{0,0},{0.08f,1},{0.7f,0.9f},{1,0}}, NULL },
        { "mouth.open", {{0,0},{0.08f,0.75f},{0.7f,0.6f},{1,0}}, NULL },
    });
    add("dance", "摇摆跳舞", 4.0f, {
        { "body.z", {{0,0},{0.125f,8},{0.25f,0},{0.375f,-8},{0.5f,0},{0.625f,8},{0.75f,0},{0.875f,-8},{1,0}}, NULL },
        { "head.z", {{0,0},{0.15f,-5},{0.275f,0},{0.4f,5},{0.525f,0},{0.65f,-5},{0.775f,0},{0.9f,5},{1,0}}, NULL },
        { "head.x", {{0,0},{0.25f,6},{0.5f,-6},{0.75f,6},{1,0}}, NULL },
        { "body.y", {{0,0},{0.0625f,2},{0.125f,0},{0.1875f,2},{0.25f,0},{0.3125f,2},{0.375f,0},{0.4375f,2},
                     {0.5f,0},{0.5625f,2},{0.625f,0},{0.6875f,2},{0.75f,0},{0.8125f,2},{0.875f,0},{1,0}}, NULL },
        { "arm.r.shoulder", {{0,0},{0.125f,65},{0.25f,20},{0.375f,25},{0.5f,20},{0.625f,65},{0.75f,20},{0.875f,25},{1,0}}, NULL },
        { "arm.l.shoulder", {{0,0},{0.125f,25},{0.25f,20},{0.375f,65},{0.5f,20},{0.625f,25},{0.75f,20},{0.875f,65},{1,0}}, NULL },
        { "arm.r.elbow", {{0,0},{0.125f,-90},{0.25f,-40},{0.375f,-50},{0.5f,-40},{0.625f,-90},{0.75f,-40},{0.875f,-50},{1,0}}, NULL },
        { "arm.l.elbow", {{0,0},{0.125f,-50},{0.25f,-40},{0.375f,-90},{0.5f,-40},{0.625f,-50},{0.75f,-40},{0.875f,-90},{1,0}}, NULL },
        { "mouth.form", {{0,0},{0.1f,0.7f},{0.9f,0.7f},{1,0}}, NULL },
    });
    add("stretch_up", "伸懒腰", 3.2f, {
        { "arm.r.shoulder", {{0,0},{0.35f,90},{0.72f,90},{1,0}}, NULL },
        { "arm.l.shoulder", {{0,0},{0.35f,90},{0.72f,90},{1,0}}, NULL },
        { "arm.r.elbow", {{0,0},{0.35f,-80},{0.72f,-85},{1,0}}, NULL },
        { "arm.l.elbow", {{0,0},{0.35f,-80},{0.72f,-85},{1,0}}, NULL },
        { "head.y", {{0,0},{0.35f,12},{0.72f,12},{1,0}}, NULL },
        { "head.z", {{0,0},{0.35f,-4},{0.55f,4},{0.72f,0},{1,0}}, NULL },
        { "body.y", {{0,0},{0.35f,4},{0.72f,4},{1,0}}, NULL },
        { "eye.l", {{0,0},{0.35f,-0.8f},{0.72f,-0.8f},{1,0}}, NULL },
        { "eye.r", {{0,0},{0.35f,-0.8f},{0.72f,-0.8f},{1,0}}, NULL },
        { "mouth.open", {{0,0},{0.35f,0.6f},{0.65f,0.6f},{0.75f,0},{1,0}}, NULL },
    });
    add("yawn", "打哈欠", 2.8f, {
        { "head.y", {{0,0},{0.3f,10},{0.7f,10},{1,0}}, NULL },
        { "head.z", {{0,0},{0.3f,-5},{0.7f,-5},{1,0}}, NULL },
        { "eye.l", {{0,0},{0.25f,-0.9f},{0.75f,-0.9f},{1,0}}, NULL },
        { "eye.r", {{0,0},{0.25f,-0.9f},{0.75f,-0.9f},{1,0}}, NULL },
        { "mouth.open", {{0,0},{0.3f,0.9f},{0.65f,0.9f},{0.8f,0},{1,0}}, NULL },
        { "brow.l", {{0,0},{0.3f,0.4f},{0.7f,0.4f},{1,0}}, NULL },
        { "brow.r", {{0,0},{0.3f,0.4f},{0.7f,0.4f},{1,0}}, NULL },
    });
    add("sad", "沮丧低落", 3.2f, {
        { "head.y", {{0,0},{0.3f,-14},{0.85f,-14},{1,0}}, NULL },
        { "head.z", {{0,0},{0.3f,5},{0.85f,5},{1,0}}, NULL },
        { "body.y", {{0,0},{0.3f,-5},{0.85f,-5},{1,0}}, NULL },
        { "brow.l", {{0,0},{0.3f,-0.7f},{0.85f,-0.7f},{1,0}}, NULL },
        { "brow.r", {{0,0},{0.3f,-0.7f},{0.85f,-0.7f},{1,0}}, NULL },
        { "eye.l", {{0,0},{0.3f,-0.35f},{0.85f,-0.35f},{1,0}}, NULL },
        { "eye.r", {{0,0},{0.3f,-0.35f},{0.85f,-0.35f},{1,0}}, NULL },
        { "mouth.form", {{0,0},{0.3f,-0.9f},{0.85f,-0.9f},{1,0}}, NULL },
        { "arm.r.shoulder", {{0,0},{0.3f,-8},{0.85f,-8},{1,0}}, NULL },
        { "arm.l.shoulder", {{0,0},{0.3f,-8},{0.85f,-8},{1,0}}, NULL },
    });
    add("angry", "生气", 2.2f, {
        { "hand.r", {{0.05f,1},{0.85f,0}}, "hip" },
        { "hand.l", {{0.05f,1},{0.85f,0}}, "hip" },
        { "head.y", {{0,0},{0.15f,-7},{0.85f,-7},{1,0}}, NULL },
        { "head.x", {{0,0},{0.2f,4},{0.45f,-4},{0.7f,4},{1,0}}, NULL },
        { "body.y", {{0,0},{0.2f,-3},{0.3f,1},{0.45f,-3},{0.55f,1},{0.7f,-3},{0.8f,1},{1,0}}, NULL },
        { "brow.l", {{0,0},{0.15f,-1},{0.85f,-1},{1,0}}, NULL },
        { "brow.r", {{0,0},{0.15f,-1},{0.85f,-1},{1,0}}, NULL },
        { "mouth.form", {{0,0},{0.15f,-1},{0.85f,-1},{1,0}}, NULL },
    });
    add("shy", "害羞", 2.8f, {
        { "hand.r", {{0.05f,1},{0.85f,0}}, "cheek" },
        { "hand.l", {{0.05f,1},{0.85f,0}}, "cheek" },
        { "head.y", {{0,0},{0.25f,-9},{0.85f,-9},{1,0}}, NULL },
        { "head.z", {{0,0},{0.25f,11},{0.85f,11},{1,0}}, NULL },
        { "head.x", {{0,0},{0.25f,-10},{0.85f,-10},{1,0}}, NULL },
        { "body.z", {{0,0},{0.25f,3},{0.5f,-2},{0.75f,3},{1,0}}, NULL },
        { "gaze.x", {{0,0},{0.25f,-0.6f},{0.85f,-0.6f},{1,0}}, NULL },
        { "gaze.y", {{0,0},{0.25f,-0.5f},{0.85f,-0.5f},{1,0}}, NULL },
        { "eye.l", {{0,0},{0.25f,-0.3f},{0.85f,-0.3f},{1,0}}, NULL },
        { "eye.r", {{0,0},{0.25f,-0.3f},{0.85f,-0.3f},{1,0}}, NULL },
    });
    add("hug_self", "双手抱胸", 2.8f, {
        { "hand.r", {{0.05f,1},{0.85f,0}}, "chest" },
        { "hand.l", {{0.05f,1},{0.85f,0}}, "chest" },
        { "head.z", {{0,0},{0.25f,5},{0.85f,5},{1,0}}, NULL },
        { "head.y", {{0,0},{0.25f,-3},{0.85f,-3},{1,0}}, NULL },
    });
    add("mic", "举话筒", 3.0f, {
        { "hand.r", {{0.05f,1},{0.9f,0}}, "mouth" },
        { "head.z", {{0,0},{0.2f,-4},{0.85f,-4},{1,0}}, NULL },
    });
    add("nod_yes", "用力点头", 1.3f, {
        { "head.y", {{0,0},{0.15f,-13},{0.32f,4},{0.5f,-12},{0.68f,3},{0.85f,-4},{1,0}}, NULL },
        { "body.y", {{0,0},{0.15f,-2},{0.5f,-2},{1,0}}, NULL },
    });
    add("shake_no", "用力摇头", 1.5f, {
        { "head.x", {{0,0},{0.14f,15},{0.32f,-15},{0.5f,13},{0.68f,-11},{0.84f,5},{1,0}}, NULL },
        { "brow.l", {{0,0},{0.1f,-0.4f},{0.9f,-0.4f},{1,0}}, NULL },
        { "brow.r", {{0,0},{0.1f,-0.4f},{0.9f,-0.4f},{1,0}}, NULL },
        { "mouth.form", {{0,0},{0.1f,-0.5f},{0.9f,-0.5f},{1,0}}, NULL },
    });
    add("hop", "开心蹦跳", 1.4f, {
        { "body.y", {{0,0},{0.12f,-4},{0.25f,5},{0.4f,-2},{0.55f,5},{0.7f,-2},{0.85f,3},{1,0}}, NULL },
        { "head.y", {{0,0},{0.25f,6},{0.55f,6},{1,0}}, NULL },
        { "arm.r.shoulder", {{0,0},{0.25f,40},{0.4f,25},{0.55f,40},{1,0}}, NULL },
        { "arm.l.shoulder", {{0,0},{0.25f,40},{0.4f,25},{0.55f,40},{1,0}}, NULL },
        { "arm.r.elbow", {{0,0},{0.25f,-60},{0.55f,-60},{1,0}}, NULL },
        { "arm.l.elbow", {{0,0},{0.25f,-60},{0.55f,-60},{1,0}}, NULL },
        { "mouth.form", {{0,0},{0.2f,1},{0.9f,1},{1,0}}, NULL },
        { "eye.l", {{0,0},{0.2f,-0.4f},{0.9f,-0.4f},{1,0}}, NULL },
        { "eye.r", {{0,0},{0.2f,-0.4f},{0.9f,-0.4f},{1,0}}, NULL },
    });
    add("look_around", "东张西望", 3.6f, {
        { "head.x", {{0,0},{0.2f,-18},{0.4f,-18},{0.6f,16},{0.8f,16},{1,0}}, NULL },
        { "head.y", {{0,0},{0.2f,3},{0.6f,3},{1,0}}, NULL },
        { "gaze.x", {{0,0},{0.15f,-0.8f},{0.4f,-0.8f},{0.55f,0.8f},{0.8f,0.8f},{1,0}}, NULL },
    });
}

/** 起一个动作。missing（可空）= 本模型缺的通道（逗号分隔，回复给 LLM）。
 *  返回 false = 动作名不存在 */
csmBool L2DPlayer::StartAction(const char* name, csmFloat32 speed, csmFloat32 scale, csmInt32 repeat, std::string* missing, csmFloat32* outDur)
{
    const ActionDef* a = FindAction(name);

    if (a == NULL)
    {
        return false;
    }
    if (speed < 0.2f) speed = 0.2f;
    if (speed > 3.0f) speed = 3.0f;
    if (scale < 0.1f) scale = 0.1f;
    if (scale > 2.0f) scale = 2.0f;
    if (repeat < 1) repeat = 1;
    if (repeat > 8) repeat = 8;
    const csmFloat32 dur = (a->dur > 0.1f ? a->dur : 1.0f) / speed;

    for (size_t t = 0; t < a->tracks.size(); ++t)
    {
        // 内置动作不带手臂：手臂大动作只走作者骨骼（profile 动作 / pose 命令）——
        // 外加骨架转 2D 手臂在大角度下是扭曲的（DESIGN-ARM-JOINTS.md §7）
        if (!a->fromProfile && (a->tracks[t].ch.compare(0, 4, "arm.") == 0
                                || a->tracks[t].ch.compare(0, 5, "hand.") == 0))
        {
            continue;
        }
        if (!a->tracks[t].anchor.empty())
        {
            // IK 窗口：在 [首, 末] 时刻把手放到锚点，结束时放开（弹回）
            const ActionTrack& tr = a->tracks[t];

            const bool isSet = tr.ch.size() > 4 && tr.ch.compare(tr.ch.size() - 4, 4, ".set") == 0;
            const std::string chk = isSet ? SetArmSet(tr.ch.c_str(), tr.anchor.c_str(), true)
                                          : SetHandTarget(tr.ch.c_str(), tr.anchor.c_str(), true);

            if (!chk.empty() || tr.keys.empty())
            {
                if (missing != NULL && missing->find(tr.ch) == std::string::npos)
                {
                    *missing += (missing->empty() ? "" : ",") + tr.ch;
                }
                continue;
            }
            for (size_t j = 0; j < _handEvents.size();)
            {
                if (_handEvents[j].ch == tr.ch)
                {
                    _handEvents.erase(_handEvents.begin() + (ptrdiff_t)j);
                    continue;
                }
                ++j;
            }
            for (csmInt32 r = 0; r < repeat; ++r)
            {
                HandEvent on, off;

                on.ch = off.ch = tr.ch;
                on.anchor = tr.anchor;
                off.anchor = "off";
                on.at = _perfClock + ((csmFloat32)r + tr.keys.front().first) * dur;
                off.at = _perfClock + ((csmFloat32)r + tr.keys.back().first) * dur;
                _handEvents.push_back(on);
                if (r == repeat - 1)
                {
                    _handEvents.push_back(off);
                }
            }
            continue;
        }
        const int ci = FindBodyChannel(a->tracks[t].ch.c_str());

        if (ci < 0)
        {
            if (missing != NULL && missing->find(a->tracks[t].ch) == std::string::npos)
            {
                *missing += (missing->empty() ? "" : ",") + a->tracks[t].ch;
            }
            continue;
        }
        // 同一通道上的旧动作：已开始的淡出、还没开始的（旧的 repeat）丢掉
        for (size_t j = 0; j < _liveActs.size();)
        {
            LiveActionTrack& o = _liveActs[j];

            if (o.ch == (size_t)ci)
            {
                if (o.start > _perfClock)
                {
                    _liveActs.erase(_liveActs.begin() + (ptrdiff_t)j);
                    continue;
                }
                if (o.fadeAt <= 0.0f)
                {
                    o.fadeAt = _perfClock;
                }
            }
            ++j;
        }
        for (csmInt32 r = 0; r < repeat && _liveActs.size() < kMaxLiveActs; ++r)
        {
            LiveActionTrack lt;

            lt.ch = (size_t)ci;
            lt.start = _perfClock + (csmFloat32)r * dur;
            lt.dur = dur;
            lt.scale = scale;
            lt.fadeAt = 0.0f;
            lt.keys = a->tracks[t].keys;
            _liveActs.push_back(lt);
        }
    }
    if (!a->expr.empty() && !StartExpressionByName(a->expr.c_str()))
    {
        fprintf(stderr, "[action] 动作表情不存在：%s\n", a->expr.c_str());
    }
    if (outDur != NULL)
    {
        *outDur = dur * (csmFloat32)repeat;
    }
    NoteExternalActivity();
    printf("[action] %s ×%d（%.1fs，speed %.2f scale %.2f）%s%s\n", name, (int)repeat, dur,
           speed, scale, (missing != NULL && !missing->empty()) ? " 缺通道：" : "",
           (missing != NULL) ? missing->c_str() : "");
    fflush(stdout);
    return true;
}

/** 作者手臂（profile 或自动识别）收尾：外加骨架让位、建关节通道、补关节清单与显示名 */
void L2DPlayer::FinalizeAuthorArm(AuthorArm& arm)
{
    static const char* const kJ[4] = { "shoulder", "elbow", "wrist", "fingers" };

    if (arm.sets.empty())
    {
        return;
    }
    if (arm.name.empty())
    {
        arm.name = (arm.ch == "arm.r") ? "右腕" : ((arm.ch == "arm.l") ? "左腕" : arm.ch);
    }
    for (size_t k = 0; k < arm.sets.size(); ++k)
    {
        ArmSet& st = arm.sets[k];

        if (st.jlist.empty())   // profile 路径：由 joint[4] 补清单（显示名取 cdi3）
        {
            static const char* const kL[4] = { "shoulder", "elbow", "wrist", "finger1" };

            for (int j = 0; j < 4; ++j)
            {
                if (st.joint[j] >= 0)
                {
                    JointInfo ji;

                    ji.label = kL[j];
                    ji.param = st.joint[j];
                    ji.name = ((size_t)st.joint[j] < _paramName.size() && !_paramName[(size_t)st.joint[j]].empty())
                              ? _paramName[(size_t)st.joint[j]]
                              : _model->GetParameterId(st.joint[j])->GetString().GetRawString();
                    st.jlist.push_back(ji);
                }
            }
        }
    }
    // profile 补语义（结构靠自动识别，profile 只给"这套是什么姿势"和默认手型）：
    //   "poseNames": {"arm.r.B":"举手张掌", …}   "poseHands": {"arm.r.B":"05", …}
    {
        jsonlite::Value prof;

        if (LoadProfileJson(&prof))
        {
            for (size_t k = 0; k < arm.sets.size(); ++k)
            {
                ArmSet& st = arm.sets[k];
                const std::string key = arm.ch + "." + st.name;
                const char* nm = prof.get("poseNames").get(key.c_str()).strOr("");
                const char* hd = prof.get("poseHands").get(key.c_str()).strOr("");

                if (*nm != '\0')
                {
                    st.desc = nm;
                }
                if (*hd != '\0')
                {
                    SelectHand(st, hd);
                }
            }
        }
    }
    // 本侧整臂交给作者绑定：外加骨架不建（rigid=false），关节通道直连作者参数
    const int legacy = FindBodyChannel(arm.ch.c_str());

    if (legacy >= 0)
    {
        _channels[(size_t)legacy].rigid = false;
    }
    for (int j = 0; j < 4; ++j)
    {
        if (arm.sets[0].joint[j] < 0 && j == 3)
        {
            continue;
        }
        BodyChannel ch;

        ch.name = arm.ch + "." + kJ[j];
        ch.slew = j == 0 ? 4.0f : 5.0f;
        ch.omega = ch.slew * kBodyOmegaK;
        if (arm.sets[0].joint[j] >= 0)
        {
            ch.params.push_back(std::make_pair(arm.sets[0].joint[j], arm.sets[0].scale[j]));
        }
        const int old = FindBodyChannel(ch.name.c_str());

        if (old >= 0)
        {
            _channels[(size_t)old] = ch;
        }
        else
        {
            _channels.push_back(ch);
        }
    }
    _authorArms.push_back(arm);
    printf("[armset] %s（%s）：%d 套作者手臂（", arm.ch.c_str(), arm.name.c_str(), (int)arm.sets.size());
    for (size_t k = 0; k < arm.sets.size(); ++k)
    {
        printf("%s%s=%s[关节%d 手型%d]", k ? "，" : "", arm.sets[k].name.c_str(), arm.sets[k].desc.c_str(),
               (int)arm.sets[k].jlist.size(), (int)arm.sets[k].hands.size());
    }
    printf("）\n");
}

/** 物理链的输出参数（被动摆动，不算关节：舒芙蕾的 armR1~5 就是）*/
std::set<csmInt32> L2DPlayer::PhysicsOutputParams()
{
    std::set<csmInt32> out;

    if (_setting == NULL || _setting->GetPhysicsFileName() == NULL || _setting->GetPhysicsFileName()[0] == '\0')
    {
        return out;
    }
    csmSizeInt size = 0;
    csmByte* buffer = ReadFile((_dir + "/" + _setting->GetPhysicsFileName()).c_str(), &size);

    if (buffer == NULL)
    {
        return out;
    }
    jsonlite::Value root;

    if (jsonlite::parse((const char*)buffer, (size_t)size, root, NULL))
    {
        const jsonlite::Value& ss = root.get("PhysicsSettings");

        for (size_t i = 0; ss.isArr() && i < ss.arr.size(); ++i)
        {
            const jsonlite::Value& outs = ss.arr[i].get("Output");

            for (size_t k = 0; outs.isArr() && k < outs.arr.size(); ++k)
            {
                const char* id = outs.arr[k].get("Destination").get("Id").strOr("");
                const csmInt32 idx = *id ? _model->GetParameterIndex(CubismFramework::GetIdManager()->GetId(id)) : -1;

                if (idx >= 0 && idx < _model->GetParameterCount())
                {
                    out.insert(idx);
                }
            }
        }
    }
    free(buffer);
    return out;
}

/** 自动识别作者手臂（pose3 分组 = 几套手臂；参数名/显示名 = 关节）——见 PROTOCOL-LLM.md §3.2 */
void L2DPlayer::AutoDetectArmSets()
{
    // 1) pose3 里"手臂"互斥组：组内每个部件 = 一套
    struct SetSeed { char side; std::string set; std::string part; };
    std::vector<SetSeed> seeds;
    std::set<std::string> bilateralParts;

    if (_setting != NULL && _setting->GetPoseFileName() != NULL && _setting->GetPoseFileName()[0] != '\0')
    {
        csmSizeInt size = 0;
        csmByte* buffer = ReadFile((_dir + "/" + _setting->GetPoseFileName()).c_str(), &size);
        jsonlite::Value root;

        if (buffer != NULL && jsonlite::parse((const char*)buffer, (size_t)size, root, NULL))
        {
            const jsonlite::Value& groups = root.get("Groups");

            for (size_t g = 0; groups.isArr() && g < groups.arr.size(); ++g)
            {
                const jsonlite::Value& grp = groups.arr[g];

                for (size_t k = 0; grp.isArr() && k < grp.arr.size(); ++k)
                {
                    const std::string pid = grp.arr[k].get("Id").strOr("");
                    const std::string pname = _partName.count(pid) ? _partName[pid] : std::string();
                    std::string letters, rest;

                    if (!ParseArmTail(pid, &letters, &rest) && pname.find("腕") == std::string::npos)
                    {
                        continue;   // 不是手臂组（如 Natori 的怀表 A/B）
                    }
                    SetSeed sd;

                    SideAndSet(letters, &sd.side, &sd.set);
                    SideSetFromName(pname, &sd.side, &sd.set);
                    sd.part = pid;
                    if (sd.set.empty() && !letters.empty())
                    {
                        sd.set = letters;
                    }
                    if (sd.side != 0)
                    {
                        seeds.push_back(sd);
                    }
                    else if (!sd.set.empty())
                    {
                        // 不分左右的套（"腕 A"）：两侧都登记，切换时联动
                        sd.side = 'R';
                        seeds.push_back(sd);
                        sd.side = 'L';
                        seeds.push_back(sd);
                        bilateralParts.insert(pid);
                    }
                }
            }
        }
        free(buffer);
    }
    // 2) 关节参数
    const std::set<csmInt32> physOut = PhysicsOutputParams();
    struct JointSeed { char side; std::string set; std::string kind; JointInfo ji; bool hand; };
    std::vector<JointSeed> js;

    for (csmInt32 i = 0; i < _model->GetParameterCount(); ++i)
    {
        const std::string id = _model->GetParameterId(i)->GetString().GetRawString();
        const std::string nm = ((size_t)i < _paramName.size()) ? _paramName[(size_t)i] : std::string();
        std::string letters, rest;

        if (!ParseArmTail(id, &letters, &rest) || physOut.count(i))
        {
            continue;   // 不是手臂参数 / 是物理输出（被动摆动）
        }
        JointSeed j;

        SideAndSet(letters, &j.side, &j.set);
        SideSetFromName(nm, &j.side, &j.set);
        if (j.side == 0)
        {
            continue;
        }
        j.hand = false;
        const bool viaHand = id.find("Arm") == std::string::npos && id.find("arm") == std::string::npos;

        // Hand 开头的只认"纯手腕"：ParamHandLB / 名字带回転・角度（排除 Rice 的手持灯光开关 HandLightAOn）
        if (viaHand && !rest.empty() && nm.find("回転") == std::string::npos
            && nm.find("角度") == std::string::npos && id.find("Angle") == std::string::npos)
        {
            continue;
        }
        const bool roll = rest.find("Roll") != std::string::npos;
        const bool handSw = !roll && !viaHand && (nm.find("表示") != std::string::npos
                                      || (rest.compare(0, 4, "Hand") == 0 && rest.size() > 4));

        if (handSw)                                                     { j.hand = true; j.kind = rest.size() > 4 ? rest.substr(4) : rest; }
        else if (roll || nm.find("指") != std::string::npos)             { j.kind = "finger"; }
        else if (viaHand)                                                { j.kind = "wrist"; }
        else if (nm.find("肘") != std::string::npos || rest.find("Elbow") != std::string::npos) { j.kind = "elbow"; }
        else if (nm.find("手首") != std::string::npos || rest.find("Wrist") != std::string::npos) { j.kind = "wrist"; }
        else if (nm.find("前腕") != std::string::npos)                    { j.kind = "forearm"; }
        else if (nm.find("上腕") != std::string::npos)                    { j.kind = "upperarm"; }
        else if (nm.find("肩") != std::string::npos || rest.find("Shoulder") != std::string::npos) { j.kind = "shoulder"; }
        else if (rest.compare(0, 2, "01") == 0 || rest.empty())          { j.kind = "shoulder"; }
        else if (rest.compare(0, 2, "02") == 0)                          { j.kind = "elbow"; }
        else if (rest.compare(0, 2, "03") == 0)                          { j.kind = "wrist"; }
        else if (!rest.empty() && rest[0] >= '0' && rest[0] <= '9')       { j.kind = "extra"; }
        else
        {
            continue;   // 认不出（如 ParamArmChange 这类开关）
        }
        j.ji.param = i;
        j.ji.name = nm.empty() ? id : nm;
        js.push_back(j);
    }
    // 3) 按左右组装：有 pose 组的用组里的套；没有的建一套 "default"
    for (int s = 0; s < 2; ++s)
    {
        const char side = s == 0 ? 'R' : 'L';
        AuthorArm arm;

        arm.ch = s == 0 ? "arm.r" : "arm.l";
        arm.cur = -1;
        std::vector<std::string> setNames;

        for (size_t k = 0; k < seeds.size(); ++k)
        {
            if (seeds[k].side == side)
            {
                setNames.push_back(seeds[k].set.empty() ? seeds[k].part : seeds[k].set);
            }
        }
        if (setNames.empty())
        {
            bool any = false;

            for (size_t k = 0; k < js.size(); ++k)
            {
                any = any || js[k].side == side;
            }
            if (!any)
            {
                continue;
            }
            setNames.push_back("default");
        }
        for (size_t n = 0; n < setNames.size(); ++n)
        {
            ArmSet st;

            st.name = setNames[n];
            for (int j = 0; j < 4; ++j)
            {
                st.joint[j] = -1;
                st.scale[j] = 1.0f;
            }
            // 部件：组里那个部件 + 全部子部件（Natori A 套前臂是子部件）
            for (size_t k = 0; k < seeds.size(); ++k)
            {
                if (seeds[k].side != side || (seeds[k].set.empty() ? seeds[k].part : seeds[k].set) != st.name)
                {
                    continue;
                }
                const csmInt32 root = _model->GetPartIndex(CubismFramework::GetIdManager()->GetId(seeds[k].part.c_str()));

                if (root < 0 || root >= _model->GetPartCount())
                {
                    continue;
                }
                st.desc = _partName.count(seeds[k].part) ? _partName[seeds[k].part] : seeds[k].part;
                st.bilateral = st.bilateral || bilateralParts.count(seeds[k].part) > 0;
                // 只压 pose 组里的根部件：Core 会把父部件不透明度乘到子部件上（前臂/手跟着走）。
                // 早先连子部件一起压 0，回到自动后子部件停在 0 → 作者动作切到那套时"没手"（实测）
                st.show.push_back(root);
                if (std::find(arm.allParts.begin(), arm.allParts.end(), root) == arm.allParts.end())
                {
                    arm.allParts.push_back(root);
                }
            }
            // 关节：同侧 + 同套（default 套收同侧所有关节）
            int fingers = 0, extras = 0;

            for (size_t k = 0; k < js.size(); ++k)
            {
                const JointSeed& j = js[k];

                if (j.side != side || (st.name != "default" && !j.set.empty() && j.set != st.name))
                {
                    continue;   // 没标套名的关节（Hiyori 的 ParamHandL）归这一侧所有套
                }
                if (j.hand)
                {
                    JointInfo h = j.ji;

                    h.label = j.kind;   // 手型编号："01" / "05"
                    st.hands.push_back(h);
                    continue;
                }
                JointInfo ji = j.ji;

                ji.label = j.kind;
                if (j.kind == "finger")
                {
                    char b[16];
                    snprintf(b, sizeof(b), "finger%d", ++fingers);
                    ji.label = b;
                }
                else if (j.kind == "extra" || j.kind == "forearm" || j.kind == "upperarm")
                {
                    if (j.kind == "extra")
                    {
                        char b[16];
                        snprintf(b, sizeof(b), "extra%d", ++extras);
                        ji.label = b;
                    }
                }
                else
                {
                    // 同一关节名出现两次（Rice 的 ArmR01 与 ArmR01Y）：后者当附加关节
                    bool dup = false;

                    for (size_t q = 0; q < st.jlist.size(); ++q)
                    {
                        dup = dup || st.jlist[q].label == ji.label;
                    }
                    if (dup)
                    {
                        ji.label += "2";
                    }
                }
                const int slot = ji.label == "shoulder" ? 0 : (ji.label == "elbow" ? 1 : (ji.label == "wrist" ? 2 : (ji.label == "finger1" ? 3 : -1)));

                if (slot >= 0)
                {
                    st.joint[slot] = ji.param;
                }
                st.jlist.push_back(ji);
            }
            // 手型开关：默认第一种显示、其余关（不设的话这套手臂会半透明——Natori 实测）
            for (size_t h = 0; h < st.hands.size(); ++h)
            {
                st.fixed.push_back(std::make_pair(st.hands[h].param, h == 0 ? 1.0f : 0.0f));
            }
            if (st.desc.empty())
            {
                st.desc = st.name;
            }
            if (!st.show.empty() || !st.jlist.empty())
            {
                arm.sets.push_back(st);
            }
        }
        FinalizeAuthorArm(arm);
    }
    fflush(stdout);
}

/** profile "armSets"：
 *  { "arm.r": { "A": {"desc":"自然下垂","show":["PartArmAR","PartArmARFore"],
 *                     "shoulder":"ParamArmAR01","elbow":"ParamArmAR02","wrist":"ParamArmAR03",
 *                     "fingers":"…", "scale":{"elbow":-1}}, "B": {…} } }
 *  第一套 = 关节通道在 auto 时映射的那套。建好后本侧关节通道 arm.X.shoulder/elbow/wrist(/fingers)
 *  直接写作者参数（作者的关节链 = 骨骼），不走我们的外加骨架。 */
void L2DPlayer::LoadAuthorArmSets()
{
    _authorArms.clear();
    jsonlite::Value prof;

    // profile 没写 armSets → 自动识别（L2D_SKEL_AUTO=1 强制自动，用来和手写 profile 对拍）
    if (getenv("L2D_SKEL_AUTO") != NULL || !LoadProfileJson(&prof) || !prof.get("armSets").isObj())
    {
        AutoDetectArmSets();
        return;
    }
    const jsonlite::Value& root = prof.get("armSets");
    static const char* const kJ[4] = { "shoulder", "elbow", "wrist", "fingers" };

    for (size_t i = 0; i < root.obj.size(); ++i)
    {
        AuthorArm arm;

        arm.ch = root.obj[i].first;
        arm.cur = -1;
        const jsonlite::Value& sets = root.obj[i].second;

        if (!sets.isObj())
        {
            continue;
        }
        for (size_t k = 0; k < sets.obj.size(); ++k)
        {
            const jsonlite::Value& sv = sets.obj[k].second;
            ArmSet st;

            st.name = sets.obj[k].first;
            st.desc = sv.get("desc").strOr("");
            const jsonlite::Value& show = sv.get("show");

            for (size_t m = 0; show.isArr() && m < show.arr.size(); ++m)
            {
                const csmInt32 pi = _model->GetPartIndex(CubismFramework::GetIdManager()->GetId(show.arr[m].strOr("")));

                if (pi >= 0 && pi < _model->GetPartCount())
                {
                    st.show.push_back(pi);
                    if (std::find(arm.allParts.begin(), arm.allParts.end(), pi) == arm.allParts.end())
                    {
                        arm.allParts.push_back(pi);
                    }
                }
                else
                {
                    fprintf(stderr, "[armset] %s/%s 部件不存在：%s\n", arm.ch.c_str(), st.name.c_str(),
                            show.arr[m].strOr(""));
                }
            }
            for (int j = 0; j < 4; ++j)
            {
                const char* pid = sv.get(kJ[j]).strOr("");

                st.joint[j] = (*pid != '\0')
                              ? _model->GetParameterIndex(CubismFramework::GetIdManager()->GetId(pid)) : -1;
                if (st.joint[j] >= _model->GetParameterCount())
                {
                    st.joint[j] = -1;
                }
                st.scale[j] = (csmFloat32)sv.get("scale").get(kJ[j]).numOr(1.0);
            }
            const jsonlite::Value& fx = sv.get("params");

            for (size_t m = 0; fx.isObj() && m < fx.obj.size(); ++m)
            {
                const csmInt32 pi = _model->GetParameterIndex(
                    CubismFramework::GetIdManager()->GetId(fx.obj[m].first.c_str()));

                if (pi >= 0 && pi < _model->GetParameterCount() && fx.obj[m].second.isNum())
                {
                    st.fixed.push_back(std::make_pair(pi, (csmFloat32)fx.obj[m].second.num));
                }
            }
            if (!st.show.empty())
            {
                arm.sets.push_back(st);
            }
        }
        FinalizeAuthorArm(arm);
    }
    fflush(stdout);
}

/** 切手臂套：arm.r.set=B / auto。关节通道改接这套的关节参数。返回错误（空=成功）*/
std::string L2DPlayer::SetArmSet(const char* chSet, const char* name, bool dryRun)
{
    std::string ch(chSet);

    if (ch.size() > 4 && ch.compare(ch.size() - 4, 4, ".set") == 0)
    {
        ch.resize(ch.size() - 4);
    }
    for (size_t a = 0; a < _authorArms.size(); ++a)
    {
        AuthorArm& arm = _authorArms[a];

        if (arm.ch != ch)
        {
            continue;
        }
        int idx = -1;
        // "B/05" = 套 B + 手型 05（动作里切套时顺便选手型）
        std::string setName(name), hand;
        const size_t sl = setName.find('/');

        if (sl != std::string::npos)
        {
            hand = setName.substr(sl + 1);
            setName.resize(sl);
        }
        if (setName != "auto")
        {
            for (size_t k = 0; k < arm.sets.size(); ++k)
            {
                if (arm.sets[k].name == setName)
                {
                    idx = (int)k;
                }
            }
            if (idx < 0)
            {
                return std::string("手臂套不认：") + name + "（LIST 的 ARMSETS=）";
            }
            if (!hand.empty())
            {
                bool has = false;

                for (size_t q = 0; q < arm.sets[(size_t)idx].hands.size(); ++q)
                {
                    has = has || arm.sets[(size_t)idx].hands[q].label == hand;
                }
                if (!has)
                {
                    return std::string("手型不认：") + name;
                }
            }
        }
        if (dryRun)
        {
            return std::string();
        }
        if (idx >= 0 && !hand.empty())
        {
            SelectHand(arm.sets[(size_t)idx], hand);
        }
        // 换套 = 过渡（旧手臂转到交接姿势 → 淡化换手 → 新手臂转到位），不硬跳
        const std::vector<std::pair<csmInt32, csmFloat32> > none;

        BeginArmTransition(a, idx < 0 ? 0 : idx, none, idx < 0, 0.35f);   // 动作里的切套窗口：过渡短一些，别吃掉动作本身
        // 两臂共用一套部件（Hiyori）：另一侧跟着切同名套，否则两边互相隐藏
        const bool bil = idx >= 0 ? arm.sets[(size_t)idx].bilateral
                                  : (!arm.sets.empty() && arm.sets[0].bilateral);

        for (size_t o = 0; bil && o < _authorArms.size(); ++o)
        {
            if (o == a)
            {
                continue;
            }
            int k2 = 0;

            for (size_t k = 0; idx >= 0 && k < _authorArms[o].sets.size(); ++k)
            {
                if (_authorArms[o].sets[k].name == setName)
                {
                    k2 = (int)k;
                }
            }
            BeginArmTransition(o, k2, none, idx < 0, 0.35f);
        }
        NoteExternalActivity();
        return std::string();
    }
    return std::string("本模型没有作者手臂套（profile armSets）：") + ch;
}

/** 屏幕点 → 区域（bb = ComputeArtBBox 的结果；渲染目标坐标系）*/
int L2DPlayer::ClassifyZone(csmFloat32 x, csmFloat32 y, const csmFloat32* bb)
{
    if (bb == NULL || bb[2] < bb[0] || bb[3] < bb[1])
    {
        return Z_OUT;
    }
    if (x < bb[0] || x > bb[2] || y < bb[1] || y > bb[3])
    {
        return Z_OUT;
    }
    const csmFloat32 bboxH = bb[3] - bb[1];
    const csmFloat32 bboxW = bb[2] - bb[0];
    csmFloat32 ex = 0.0f, ey = 0.0f, chx = 0.0f, chy = 0.0f, wx = 0.0f, wy = 0.0f;
    const bool eyesOk = ProjectAnchor("eyes", &ex, &ey);
    const bool chinOk = ProjectAnchor("chin", &chx, &chy);
    const bool waistOk = ProjectAnchor("waist", &wx, &wy);
    const csmFloat32 centerX = chinOk ? chx : (bb[0] + bb[2]) * 0.5f;

    // 手/肘优先（手臂垂在身前时盖住躯干带）：臂骨架腕/肘关节，屏幕左右定左右
    if (bboxH > 8.0f && !_rigs.empty())
    {
        const csmFloat32 R = 0.065f * bboxH;

        for (size_t k = 0; k < _rigs.size(); ++k)
        {
            csmFloat32 sx = 0.0f, sy = 0.0f, jx = 0.0f, jy = 0.0f, jpx = 0.0f, jpy = 0.0f;

            AnchorPos(_rigs[k].sh, 0.0f, 0.0f, &sx, &sy);
            AnchorPos(_rigs[k].el, sx, sy, &jx, &jy);
            if (ProjectModelPoint(jx, jy, &jpx, &jpy) == false) continue;
            if ((jpx - x) * (jpx - x) + (jpy - y) * (jpy - y) < 0.64f * R * R)
            {
                return jpx < centerX ? Z_ELBOW_L : Z_ELBOW_R;
            }
            AnchorPos(_rigs[k].wr, sx, sy, &jx, &jy);
            if (ProjectModelPoint(jx, jy, &jpx, &jpy) == false) continue;
            if ((jpx - x) * (jpx - x) + (jpy - y) * (jpy - y) < R * R)
            {
                return jpx < centerX ? Z_HAND_L : Z_HAND_R;
            }
        }
    }
    if (!eyesOk || !chinOk)
    {
        return (y <= bb[1] + bboxH * 0.32f) ? Z_HEAD : Z_CHEST;   // 没锚点：老的上下两段兜底
    }
    // 无臂骨架模型（作者绑定）：手部用作者手参数质心判定（半径同骨架版；无肘分区）
    if (_rigs.empty() && bboxH > 8.0f)
    {
        const csmFloat32 R = 0.065f * bboxH;
        static const char* const kH[2] = { "hand.l", "hand.r" };
        for (int k = 0; k < 2; ++k)
        {
            csmFloat32 hx = 0.0f, hy = 0.0f;
            if (!ProjectAnchor(kH[k], &hx, &hy)) continue;
            if ((hx - x) * (hx - x) + (hy - y) * (hy - y) < 1.2f * R * R)
            {
                return k == 0 ? Z_HAND_L : Z_HAND_R;
            }
        }
    }
    const csmFloat32 waistY0 = waistOk ? wy : (chy + (bb[3] - chy) * 0.55f);
    // 腰锚点由臂长推算（midSh - 1.15×上臂长），短臂模型会偏上、把躯干带压扁：
    // 钳到"下颌→人物底"的 40%，保证腿不超过 6 成
    const csmFloat32 waistY = (waistY0 - chy < (bb[3] - chy) * 0.40f)
                              ? (chy + (bb[3] - chy) * 0.40f) : waistY0;

if (L2DPlayer::ZonesDebug() && _perfClock - _zoneDbgAt > 0.5f)
    {
        _zoneDbgAt = _perfClock;
        printf("[zone] bbox y[%.0f..%.0f] eyes(%.0f,%.0f) chin(%.0f,%.0f) waistY=%.0f(锚%.0f) → 输入(%.0f,%.0f)\n",
               bb[1], bb[3], ex, ey, chx, chy, waistY, waistY0, x, y);
        fflush(stdout);
    }
    if (y < chy)   // 下颌以上 = 头/脸
    {
        if (y >= ey - 0.06f * bboxH && fabsf(x - centerX) <= 0.36f * bboxW)
        {
            return Z_FACE;
        }
        return Z_HEAD;
    }
    const csmFloat32 S = waistY - chy > 1.0f ? (waistY - chy) : bboxH * 0.3f;
    // 身体宽度：两肩关节的屏幕间距（没有臂骨架就按身高估）。躯干/腿两侧超出身体的
    // 点（垂下来的长发）算"头部"（摸头发≈摸头），不误报成胸/肚子/腿
    csmFloat32 half = 0.16f * bboxH;
    {
        csmFloat32 m = 0.0f;

        for (size_t k = 0; k < _rigs.size(); ++k)
        {
            csmFloat32 sx = 0.0f, sy = 0.0f, px = 0.0f, py = 0.0f;

            AnchorPos(_rigs[k].sh, 0.0f, 0.0f, &sx, &sy);
            if (ProjectModelPoint(sx, sy, &px, &py))
            {
                m = std::max(m, fabsf(px - centerX));
            }
        }
        if (m > 4.0f)
        {
            half = m * 1.25f;
        }
    }
    const csmFloat32 legTop = chy + 0.78f * S + 0.10f * (bb[3] - waistY > 1.0f ? (bb[3] - waistY) : bboxH * 0.1f);

    if (y < waistY && fabsf(x - centerX) > half)
    {
        return Z_HEAD;   // 腰以上、身体两侧 = 垂下来的长发（腰以下两侧多是裙摆，照常分区）
    }
    if (y < chy + 0.20f * S) return Z_NECK;
    if (y < chy + 0.50f * S) return Z_CHEST;
    if (y < chy + 0.78f * S) return Z_BELLY;
    if (y < legTop) return Z_CROTCH;
    const csmFloat32 L = bb[3] - legTop > 1.0f ? (bb[3] - legTop) : bboxH * 0.1f;
    const bool left = x < centerX;

    if (y < legTop + 0.40f * L) return left ? Z_THIGH_L : Z_THIGH_R;
    if (y < legTop + 0.80f * L) return left ? Z_SHIN_L : Z_SHIN_R;
    return left ? Z_FOOT_L : Z_FOOT_R;
}

/** 桌宠指针每帧喂进来（渲染目标坐标；has=false 表示指针不在窗口/正拖动窗口）：
 *  hover（同区驻留 0.6s，之后每 7s 一次）+ rub（1.5s 内 ≥3 次左右反向）+ click 防抖计时 */
void L2DPlayer::PetPointerTick(csmFloat32 px, csmFloat32 py, bool has, csmFloat32 dt)
{
    if (_rubCooldown > 0.0f)
    {
        _rubCooldown -= dt;
    }
    if (_clickZone >= 0)
    {
        _clickClock += dt;
        if (_clickClock >= 0.5f)   // 连点合并窗口 0.5s（原 1s，上报太慢）
        {
            char b[96];

            snprintf(b, sizeof(b), "{\"event\":\"click\",\"zone\":\"%s\",\"n\":%d}",
                     ZoneKey(_clickZone), _clickCount);
            _petEvents.push_back(b);
            _clickZone = -1;
            _clickCount = 0;
        }
    }
    if (!has)
    {
        _hoverZone = -1;
        _hoverClock = 0.0f;
        _hoverEmitted = false;
        _rubBuf.clear();
        return;
    }
    int z = Z_OUT;

    if (_modelBBoxValid)
    {
        csmFloat32 bb[4];

        ComputeArtBBox(bb);
        z = ClassifyZone(px, py, bb);
    }
    if (z >= 0 && z == _hoverZone)
    {
        _hoverClock += dt;
        if ((!_hoverEmitted && _hoverClock >= 0.6f) || (_hoverEmitted && _hoverClock >= 7.0f))
        {
            char b[80];

            snprintf(b, sizeof(b), "{\"event\":\"hover\",\"zone\":\"%s\"}", ZoneKey(z));
            _petEvents.push_back(b);
            _hoverEmitted = true;
            _hoverClock = 0.0f;
        }
    }
    else
    {
        _hoverZone = z;
        _hoverClock = 0.0f;
        _hoverEmitted = false;
    }
    if (z >= 0)
    {
        _rubT += dt;
        _rubBuf.push_back(std::make_pair(_rubT, px));
        while (!_rubBuf.empty() && _rubT - _rubBuf.front().first > 1.5f)
        {
            _rubBuf.erase(_rubBuf.begin());
        }
        int flips = 0;
        int lastDir = 0;

        for (size_t i = 1; i < _rubBuf.size(); ++i)
        {
            const csmFloat32 dx = _rubBuf[i].second - _rubBuf[i - 1].second;

            if (fabsf(dx) < 4.0f * PxK()) continue;
            const int dir = dx > 0.0f ? 1 : -1;

            if (lastDir != 0 && dir != lastDir)
            {
                ++flips;
            }
            lastDir = dir;
        }
        if (flips >= 3 && _rubCooldown <= 0.0f)
        {
            char b[80];

            snprintf(b, sizeof(b), "{\"event\":\"rub\",\"zone\":\"%s\",\"count\":%d}", ZoneKey(z), flips);
            _petEvents.push_back(b);
            ZoneReact(z, 1, 0, px, py);   // 搓：本地立即反应
            _rubBuf.clear();
            _rubCooldown = 1.5f;
        }
    }
    else
    {
        _rubBuf.clear();
    }
}

/** 气泡选项被点中 → choice 事件（label 来自下发时的 options）*/
void L2DPlayer::PetChoiceMade(int index)
{
    MuteTouchResponse();   // 点了选项 = 一次交互：随后 3 秒点她/搓她别来抢这次交互的气泡
    // 猜拳进行中：选项 = 出的手，不走"回答问题"那条路（否则 agent 会当成答了个问题）
    // 标签校验：气泡过期后可能换成别的选项（LLM 提问），别把那次点选当出拳
    const bool isRps = _gameRps && _bubbleOptions.size() == 3 && _bubbleOptions[0] == "石头"
                       && _bubbleOptions[1] == "剪刀" && _bubbleOptions[2] == "布";
    _gameRps = 0;
    if (isRps)
    {
        if (_port != NULL)
        {
            ClearBubbleDisplay();
        }
        _bubbleOptions.clear();
        _bubbleOptionDo.clear();
        NoteExternalActivity();
        if (index < 0 || index > 2)
        {
            return;
        }
        static const char* const kMoves[3] = { "石头", "剪刀", "布" };
        const int user = index;
        const int pet = (int)(NextRand() % 3);
        // 0=石头 1=剪刀 2=布；X 赢 (X+1)%3
        const int res = (user == pet) ? 0 : (((user + 1) % 3 == pet) ? 2 : 1);   // 0平 1主人赢 2宠物赢
        static const char* const kWin[] = { "我赢啦！嘿嘿，是不是很厉害～", "耶！我赢了！要不要再来一局？",
                                            "看吧，我可没让着你哦！" };
        static const char* const kLose[] = { "唔……你运气好而已！再来！", "输了……不服气，再来一局嘛！",
                                             "哼，下次我一定会赢的！" };
        static const char* const kDraw[] = { "平局！心有灵犀诶～", "一样诶！那……再猜一次？",
                                             "平手！我们很有默契嘛。" };
        std::string line;
        if (res == 0) line = LifeLine("rps.draw", kDraw, 3);
        else if (res == 2) line = LifeLine("rps.win", kWin, 3);
        else line = LifeLine("rps.lose", kLose, 3);
        const std::string full = peti18n::TF("我出{}！{}", { peti18n::Tr(kMoves[pet]), line });
        // 反应：赢 → 得意跳；输 → 不服气歪头；平 → 点头
        if (res == 2)
        {
            StartAction("hop", 1.0f, 0.8f, 1);
            EmotionNudge("happy", 0.8f);
        }
        else if (res == 1)
        {
            StartAction("tilt_head", 1.0f, 0.7f, 1);
            EmotionNudge("sulky", 0.6f);
        }
        else
        {
            StartAction("nod_yes", 1.0f, 0.7f, 1);
            EmotionNudge("happy", 0.5f);
        }
        LifeSay(full, 5.5f);
        char ev[256];
        snprintf(ev, sizeof(ev), "{\"event\":\"game\",\"kind\":\"rps\",\"user\":\"%s\",\"pet\":\"%s\",\"result\":\"%s\"}",
                 kMoves[user], kMoves[pet], res == 0 ? "draw" : (res == 2 ? "pet" : "user"));
        _petEvents.push_back(ev);
        ++_rps[res];
        InteractCost(0.04f, "拉着玩猜拳");
        _lifeLonely = Clamp01(_lifeLonely - 0.2f);
        FeedPush("game", peti18n::TF("猜拳：你出{}，她出{} → {}",
                                     { peti18n::Tr(kMoves[user]), peti18n::Tr(kMoves[pet]),
                                       peti18n::Tr(res == 0 ? "平局" : (res == 2 ? "她赢了" : "你赢了")) }));
        printf("[game] 猜拳：主人%s / 我%s → %s\n", kMoves[user], kMoves[pet],
               res == 0 ? "平" : (res == 2 ? "我赢" : "主人赢"));
        fflush(stdout);
        return;
    }
    _lifeLonely = Clamp01(_lifeLonely - 0.3f);   // 回答了她的问题 = 理她了
    if (index >= 0 && index < (int)_bubbleOptions.size())
    {
        FeedPush("touch", peti18n::TF("你选了「{}」", { _bubbleOptions[(size_t)index] }));
    }
    // 这个选项自带动作（LLM 下发时附的 do）：点下去**立刻**演，不等下一次 LLM——
    // 交互的反馈是零延迟的，LLM 随后接着这个选择往下演（choice 事件里 did 告诉它已经演过什么）
    const bool hasDo = index >= 0 && index < (int)_bubbleOptionDo.size() && _bubbleOptionDo[(size_t)index].isObj()
                       && !_bubbleOptionDo[(size_t)index].obj.empty();
    std::string json = "{\"event\":\"choice\",\"index\":" + JNum(index) + ",\"label\":";

    JStr(json, (index >= 0 && index < (int)_bubbleOptions.size()) ? _bubbleOptions[(size_t)index]
                                                                  : std::string());
    if (hasDo)
    {
        // 只列**真能演**的键：do 里非法 fv（能力表里没有的表情/动作）执行时会被跳过，
        // 列进 did 就是骗 LLM"已经演过了"。逐键 dry-run 一次拿结论（点了才走这条冷路径）
        std::vector<std::string> keys;
        for (const auto& kv : _bubbleOptionDo[(size_t)index].obj)
        {
            jsonlite::Value one;
            one.setType(jsonlite::Value::OBJ);
            one.obj.push_back(kv);
            if (ExecPetStep(one, NULL, -1, true) > 0) keys.push_back(kv.first);
        }
        json += ",\"did\":[";
        for (size_t k = 0; k < keys.size(); ++k) { if (k) json += ","; JStr(json, keys[k]); }
        json += "]";
    }
    json += "}";
    _petEvents.push_back(json);
    if (_port != NULL)
    {
        ClearBubbleDisplay();
    }
    if (hasDo)
    {
        const jsonlite::Value d = _bubbleOptionDo[(size_t)index];   // 拷贝：执行时可能再次改写选项
        const std::string lab = index < (int)_bubbleOptions.size() ? _bubbleOptions[(size_t)index] : "?";   // do 里的 speak 会换掉气泡/选项
        const int n = ExecPetStep(d, NULL, -1, false);
        printf("[pet] 选项「%s」自带动作：执行 %d 项\n", lab.c_str(), n);
        fflush(stdout);
    }
    _bubbleOptions.clear();
    _bubbleOptionDo.clear();
    NoteExternalActivity();
}

const std::vector<L2DPlayer::MotionInfo>& L2DPlayer::Motions()
{
    if (_motionInfoReady)
    {
        return _motionInfo;
    }
    _motionInfoReady = true;
    for (csmInt32 g = 0; g < MotionGroupCount(); ++g)
    {
        const char* gn = MotionGroupName(g);

        for (csmInt32 i = 0; i < MotionCount(g); ++i)
        {
            MotionInfo mi;

            mi.group = gn;
            mi.index = i;
            mi.dur = 0.0f;
            mi.arm = false;
            const char* f = _setting->GetMotionFileName(gn, i);
            csmSizeInt size = 0;
            csmByte* buf = (f != NULL && *f) ? ReadFile((_dir + "/" + f).c_str(), &size) : NULL;
            jsonlite::Value root;

            if (buf != NULL && jsonlite::parse((const char*)buf, (size_t)size, root, NULL))
            {
                mi.dur = (csmFloat32)root.get("Meta").get("Duration").numOr(0.0);
                const jsonlite::Value& cs = root.get("Curves");

                for (size_t k = 0; cs.isArr() && k < cs.arr.size() && !mi.arm; ++k)
                {
                    const std::string id = cs.arr[k].get("Id").strOr("");

                    mi.arm = id.find("Arm") != std::string::npos || id.find("arm") != std::string::npos
                             || id.find("Hand") != std::string::npos;
                }
            }
            free(buf);
            _motionInfo.push_back(mi);
        }
    }
    return _motionInfo;
}

/** query：能力表（what 为空 = summary）*/
std::string L2DPlayer::CapabilitiesJson(const jsonlite::Value& what, csmFloat32 fps)
{
    std::set<std::string> want;

    if (what.isStr())
    {
        want.insert(what.str);
    }
    for (size_t i = 0; what.isArr() && i < what.arr.size(); ++i)
    {
        want.insert(what.arr[i].strOr(""));
    }
    if (want.empty())
    {
        want.insert("summary");
    }
    const bool all = want.count("all") > 0;
    std::string o = "\"type\":\"capabilities\",\"model\":{\"name\":";

    JStr(o, BaseName(_dir));
    o += ",\"fv\":";
    JStr(o, ModelFv());
    o += ",\"fps\":" + JNum(fps) + "}";

    // summary
    if (all || want.count("summary"))
    {
        int nAct = 0;

        for (size_t i = 0; i < _actions.size(); ++i)
        {
            nAct += ActionReportable(_actions[i]) ? 1 : 0;
        }
        const bool vowel = _model->GetParameterIndex(CubismFramework::GetIdManager()->GetId("ParamA")) >= 0
                           && _model->GetParameterIndex(CubismFramework::GetIdManager()->GetId("ParamA")) < _model->GetParameterCount();
        o += ",\"summary\":{\"skeleton\":\"";
        o += SkeletonLevel();
        o += "\",\"expressions\":" + JNum((double)_expressionNames.size());
        o += ",\"motions\":" + JNum((double)Motions().size());
        o += ",\"actions\":" + JNum(nAct);
        o += ",\"head\":\"";
        o += FindBodyChannel("head.x") >= 0 ? "full" : "none";
        o += "\",\"body\":\"";
        o += FindBodyChannel("body.x") >= 0 ? "full" : "none";
        o += "\",\"mouth\":\"";
        o += vowel ? "vowel" : (FindBodyChannel("mouth.open") >= 0 ? "open" : "none");
        o += "\"}";
    }
    // skeleton
    if (all || want.count("skeleton"))
    {
        o += ",\"skeleton\":{";
        for (size_t a = 0; a < _authorArms.size(); ++a)
        {
            const AuthorArm& arm = _authorArms[a];

            if (a) o += ",";
            JStr(o, arm.ch);
            o += ":{\"name\":";
            JStr(o, arm.name);
            o += ",\"poses\":[";
            for (size_t k = 0; k < arm.sets.size(); ++k)
            {
                const ArmSet& st = arm.sets[k];

                if (k) o += ",";
                o += "{\"name\":";
                JStr(o, st.desc.empty() ? st.name : st.desc);
                o += ",\"fv\":";
                JStr(o, "pose:" + arm.ch + "." + st.name);
                o += ",\"joints\":[";
                for (size_t j = 0; j < st.jlist.size(); ++j)
                {
                    const JointInfo& ji = st.jlist[j];

                    if (j) o += ",";
                    o += "{\"name\":";
                    JStr(o, ji.name);
                    o += ",\"fv\":";
                    JStr(o, "joint:" + arm.ch + "." + st.name + "." + ji.label);
                    o += ",\"range\":[" + JNum(_model->GetParameterMinimumValue((csmUint32)ji.param)) + ","
                         + JNum(_model->GetParameterMaximumValue((csmUint32)ji.param)) + "],\"default\":"
                         + JNum(_model->GetParameterDefaultValue((csmUint32)ji.param)) + "}";
                }
                o += "]";
                if (!st.hands.empty())
                {
                    o += ",\"hands\":[";
                    for (size_t h = 0; h < st.hands.size(); ++h)
                    {
                        if (h) o += ",";
                        o += "{\"name\":";
                        JStr(o, st.hands[h].name);
                        o += ",\"fv\":";
                        JStr(o, "hand:" + arm.ch + "." + st.name + "." + st.hands[h].label);
                        o += "}";
                    }
                    o += "]";
                }
                o += "}";
            }
            o += "]}";
        }
        o += "}";
    }
    // expressions
    if (all || want.count("expressions"))
    {
        o += ",\"expressions\":[";
        for (size_t i = 0; i < _expressionNames.size(); ++i)
        {
            if (i) o += ",";
            o += "{\"name\":";
            JStr(o, _expressionNames[i]);
            o += ",\"fv\":";
            JStr(o, "exp:" + _expressionNames[i]);
            o += "}";
        }
        o += "]";
    }
    // motions
    if (all || want.count("motions"))
    {
        const std::vector<MotionInfo>& ms = Motions();

        o += ",\"motions\":[";
        for (size_t i = 0; i < ms.size(); ++i)
        {
            char nm[96];

            snprintf(nm, sizeof(nm), "%s %d", ms[i].group.c_str(), (int)ms[i].index);
            if (i) o += ",";
            o += "{\"name\":";
            JStr(o, nm);
            o += ",\"fv\":";
            JStr(o, "mtn:" + ms[i].group + "/" + std::to_string(ms[i].index));
            o += ",\"dur\":" + JNum(ms[i].dur) + ",\"arm\":" + (ms[i].arm ? "true" : "false") + "}";
        }
        o += "]";
    }
    // actions
    if (all || want.count("actions"))
    {
        bool first = true;

        o += ",\"actions\":[";
        for (size_t i = 0; i < _actions.size(); ++i)
        {
            if (!ActionReportable(_actions[i]))
            {
                continue;
            }
            if (!first) o += ",";
            first = false;
            o += "{\"name\":";
            JStr(o, _actions[i].desc.empty() ? _actions[i].name : _actions[i].desc);
            o += ",\"fv\":";
            JStr(o, "act:" + _actions[i].name);
            o += ",\"dur\":" + JNum(_actions[i].dur) + ",\"needs\":";
            JStr(o, ActionNeeds(_actions[i]));
            o += "}";
        }
        o += "]";
    }
    // parts（通用部位：增量）
    if (all || want.count("parts"))
    {
        bool first = true;

        o += ",\"parts\":[";
        for (size_t i = 0; i < _channels.size(); ++i)
        {
            const char* label = PartLabel(_channels[i].name);

            if (label == NULL)
            {
                continue;
            }
            csmFloat32 lo, hi;

            ChannelRange(_channels[i], &lo, &hi);
            if (!first) o += ",";
            first = false;
            o += "{\"name\":";
            JStr(o, label);
            o += ",\"fv\":";
            JStr(o, "part:" + _channels[i].name);
            o += ",\"range\":[" + JNum(lo) + "," + JNum(hi) + "]}";
        }
        o += "]";
    }
    // zones（交互区域：click/hover/rub 事件用；左右 = 观看者屏幕口径）
    if (all || want.count("zones"))
    {
        o += ",\"zones\":[";
        for (int z = 0; z < ZONE_COUNT; ++z)
        {
            if (z) o += ",";
            o += "{\"name\":";
            JStr(o, ZoneLabel(z));
            o += ",\"fv\":";
            JStr(o, std::string("zone:") + ZoneKey(z));
            o += "}";
        }
        o += "]";
    }
    // params（专家：全量原始参数）
    if (want.count("params"))
    {
        o += ",\"params\":[";
        for (csmInt32 i = 0; i < _model->GetParameterCount(); ++i)
        {
            const std::string id = _model->GetParameterId(i)->GetString().GetRawString();

            if (i) o += ",";
            o += "{\"name\":";
            JStr(o, ((size_t)i < _paramName.size() && !_paramName[(size_t)i].empty()) ? _paramName[(size_t)i] : id);
            o += ",\"fv\":";
            JStr(o, "param:" + id);
            o += ",\"range\":[" + JNum(_model->GetParameterMinimumValue((csmUint32)i)) + ","
                 + JNum(_model->GetParameterMaximumValue((csmUint32)i)) + "],\"default\":"
                 + JNum(_model->GetParameterDefaultValue((csmUint32)i)) + "}";
        }
        o += "]";
    }
    return o;
}

/** 执行（dry=true 只校验）。返回生效的项数 */
int L2DPlayer::ExecPetStep(const jsonlite::Value& v, std::vector<std::string>* warn, int step, bool dry)
{
    int done = 0;

    // 连续情绪：{"emotion":"happy"} 或 {"emotion":{"name":"shy","intensity":0.8}}
    if (v.has("emotion"))
    {
        const jsonlite::Value& e = v.get("emotion");
        const std::string name = e.isObj() ? e.get("name").strOr("") : e.strOr("");
        const csmFloat32 it = e.isObj() ? (csmFloat32)e.get("intensity").numOr(0.7) : 0.7f;

        if (EmotionPreset(name) == NULL)
        {
            Warn(warn, step, name, "未知情绪（见 query emotions）");
        }
        else
        {
            if (!dry) EmotionNudge(name, it);
            ++done;
        }
    }

    if (v.has("expression"))
    {
        const jsonlite::Value& e = v.get("expression");

        if (e.isNull())
        {
            if (!dry) ClearExpression();
            ++done;
        }
        else
        {
            const std::string fv = e.strOr("");
            std::string name = fv.compare(0, 4, "exp:") == 0 ? fv.substr(4) : fv;

            // 宽容：LLM 常把 fv 写成裸名（"星星" 而非 "exp:星星"）——按名字直接找
            if (std::find(_expressionNames.begin(), _expressionNames.end(), name) == _expressionNames.end())
            {
                Warn(warn, step, fv, "表情不存在");
            }
            else
            {
                if (!dry) StartExpressionByName(name.c_str());
                ++done;
            }
        }
    }
    if (v.has("motion"))
    {
        const std::string fv = v.get("motion").strOr("");
        bool ok = false;

        if (fv.compare(0, 4, "mtn:") == 0)
        {
            const std::string rest = fv.substr(4);
            const size_t sl = rest.rfind('/');
            const std::string g = sl == std::string::npos ? rest : rest.substr(0, sl);
            const int idx = sl == std::string::npos ? 0 : atoi(rest.c_str() + sl + 1);

            for (csmInt32 k = 0; k < MotionGroupCount() && !ok; ++k)
            {
                if (g == MotionGroupName(k) && idx >= 0 && idx < MotionCount(k))
                {
                    ok = true;
                    if (!dry) StartMotionByGroupIndex(k, idx);
                }
            }
        }
        if (!ok) Warn(warn, step, fv, "动作不存在");
        else ++done;
    }
    if (v.has("action"))
    {
        const std::string fv = v.get("action").strOr("");
        const ActionDef* a = ResolveActionFv(fv);

        if (a == NULL)
        {
            Warn(warn, step, fv, "预置动作不存在或本模型做不了（query actions 查清单）");
        }
        else
        {
            if (!dry)
            {
                StartAction(a->name.c_str(), (csmFloat32)v.get("speed").numOr(1.0),
                            (csmFloat32)v.get("scale").numOr(1.0), (csmInt32)v.get("repeat").numOr(1.0));
            }
            ++done;
        }
    }
    if (v.get("pose").isObj())
    {
        const jsonlite::Value& p = v.get("pose");
        const csmFloat32 t = (csmFloat32)p.get("t").numOr(0.4);
        const jsonlite::Value& set = p.get("set");
        // 本步要换套的手臂 → 新套；同一步里给新套的关节值作为过渡终点（先过桥再转到位）
        std::map<size_t, size_t> newSet;
        std::map<size_t, std::vector<std::pair<csmInt32, csmFloat32> > > newTargets;

        if (set.isArr())
        {
            if (set.arr.empty())
            {
                if (!dry) ReleasePose();
                ++done;
            }
            for (size_t i = 0; i < set.arr.size(); ++i)
            {
                const std::string fv = set.arr[i].strOr("");
                size_t a, k;

                if (!ResolveSetFv(fv, &a, &k))
                {
                    Warn(warn, step, fv, _authorArms.empty() ? "本模型没有骨架（query skeleton 为空）" : "姿势套不存在");
                    continue;
                }
                newSet[a] = k;
                ++done;
            }
        }
        const jsonlite::Value& hands = p.get("hands");

        for (size_t i = 0; hands.isArr() && i < hands.arr.size(); ++i)
        {
            const std::string fv = hands.arr[i].strOr("");
            size_t a, k;
            const JointInfo* h = ResolveJointFv(fv, true, &a, &k);

            if (h == NULL)
            {
                Warn(warn, step, fv, "手型不存在");
                continue;
            }
            if (!dry)
            {
                ArmSet& st = _authorArms[a].sets[k];

                st.fixed.clear();
                for (size_t q = 0; q < st.hands.size(); ++q)
                {
                    st.fixed.push_back(std::make_pair(st.hands[q].param, st.hands[q].param == h->param ? 1.0f : 0.0f));
                }
            }
            ++done;
        }
        const jsonlite::Value& joints = p.get("joints");

        for (size_t i = 0; joints.isObj() && i < joints.obj.size(); ++i)
        {
            const std::string fv = joints.obj[i].first;
            size_t a, k;
            const JointInfo* ji = ResolveJointFv(fv, false, &a, &k);

            if (ji == NULL || !joints.obj[i].second.isNum())
            {
                Warn(warn, step, fv, _authorArms.empty() ? "本模型没有骨架（query skeleton 为空）" : "关节不存在或值不是数字");
                continue;
            }
            csmFloat32 val = (csmFloat32)joints.obj[i].second.num;
            const csmFloat32 lo = _model->GetParameterMinimumValue((csmUint32)ji->param);
            const csmFloat32 hi = _model->GetParameterMaximumValue((csmUint32)ji->param);

            if (val < lo || val > hi)
            {
                char m[96];

                snprintf(m, sizeof(m), "值 %g 超出范围 [%g,%g]，已钳制", val, lo, hi);
                Warn(warn, step, fv, m);
            }
            if (!dry)
            {
                const std::map<size_t, size_t>::const_iterator ns = newSet.find(a);

                if (ns != newSet.end() && ns->second == k)
                {
                    newTargets[a].push_back(std::make_pair(ji->param, val));   // 过渡第 3 段的终点
                }
                else
                {
                    HoldJoint(ji->param, val, t);
                }
            }
            ++done;
        }
        // 启动换套过渡（两臂共用一套的走 SetArmSet：两侧联动）
        for (std::map<size_t, size_t>::const_iterator it = newSet.begin(); !dry && it != newSet.end(); ++it)
        {
            AuthorArm& arm = _authorArms[it->first];
            const std::vector<std::pair<csmInt32, csmFloat32> >& tg = newTargets[it->first];

            if (arm.sets[it->second].bilateral)
            {
                SetArmSet((arm.ch + ".set").c_str(), arm.sets[it->second].name.c_str(), false);
                for (size_t q = 0; q < tg.size(); ++q)
                {
                    HoldJoint(tg[q].first, tg[q].second, t);
                }
            }
            else
            {
                BeginArmTransition(it->first, (int)it->second, tg, false, t + 0.05f);
            }
        }
        const jsonlite::Value& parts = p.get("parts");

        for (size_t i = 0; parts.isObj() && i < parts.obj.size(); ++i)
        {
            const std::string fv = parts.obj[i].first;
            const std::string ch = fv.compare(0, 5, "part:") == 0 ? fv.substr(5) : std::string();
            const int ci = FindBodyChannel(ch.c_str());

            if (ci < 0 || PartLabel(ch) == NULL || !parts.obj[i].second.isNum())
            {
                Warn(warn, step, fv, "部位不存在（query parts 查清单）");
                continue;
            }
            if (!dry)
            {
                SetBodyChannel(ch.c_str(), (csmFloat32)parts.obj[i].second.num, t);
            }
            ++done;
        }
        const csmFloat32 hold = (csmFloat32)p.get("hold").numOr(0.0);

        if (!dry && hold > 0.05f)
        {
            _poseReleaseAt = _perfClock + hold;
        }
    }
    if (v.has("look"))
    {
        const jsonlite::Value& l = v.get("look");
        csmFloat32 x = 0.0f, y = 0.0f;
        bool ok = true;
        const csmFloat32 W = (csmFloat32)_viewW, H = (csmFloat32)_viewH;

        if (l.isArr() && l.arr.size() >= 2)
        {
            x = (csmFloat32)l.arr[0].numOr(W * 0.5);
            y = (csmFloat32)l.arr[1].numOr(H * 0.3);
        }
        else
        {
            const std::string t = l.strOr("");

            if (t == "viewer")      { x = W * 0.5f; y = H * 0.30f; }
            else if (t == "left")   { x = 0.0f;     y = H * 0.35f; }
            else if (t == "right")  { x = W;        y = H * 0.35f; }
            else if (t == "up")     { x = W * 0.5f; y = 0.0f; }
            else if (t == "down")   { x = W * 0.5f; y = H; }
            else
            {
                ok = false;
                Warn(warn, step, t, "视线目标只能是 viewer/left/right/up/down 或 [x,y]");
            }
        }
        if (ok)
        {
            if (!dry)
            {
                SetLookTargetPixels(x, y);
                _lookOverrideUntil = _perfClock + (csmFloat32)v.get("look_hold").numOr(3.0);
            }
            ++done;
        }
    }
    if (v.has("speak"))
    {
        const std::string text = v.get("speak").strOr("");
        const csmFloat32 d = (csmFloat32)v.get("speak_dur").numOr(0.0);
        std::vector<std::string> opts;
        std::vector<jsonlite::Value> optDo;   // 与 opts 一一对应：点这个选项时立刻执行的动作（可空）

        {
            const jsonlite::Value ov = v.get("options");

            if (ov.isArr())
            {
                for (size_t oi = 0; oi < ov.arr.size() && opts.size() < 4; ++oi)
                {
                    // 选项两种写法："摸摸头" 或 {"label":"摸摸头","do":{"action":…,"expression":…,"emotion":…,"speak":…}}
                    // do 里引用的能力 fv 照常按能力表校验（不合法的报 warning、点了也不执行那一项）
                    const jsonlite::Value& o = ov.arr[oi];
                    const std::string s = o.isObj() ? std::string(o.get("label").strOr("")) : std::string(o.strOr(""));

                    if (!s.empty())
                    {
                        opts.push_back(s);
                        jsonlite::Value d;
                        if (o.isObj() && o.get("do").isObj())
                        {
                            d = o.get("do");
                            for (size_t k = 0; k < d.obj.size(); ++k)   // 不许嵌套选项（点一下又弹一串）
                            {
                                if (d.obj[k].first == "options") { d.obj.erase(d.obj.begin() + (long)k); break; }
                            }
                            if (dry)
                            {
                                ExecPetStep(d, warn, step, true);   // 收到时就校验，警告随回执回给 LLM
                            }
                        }
                        optDo.push_back(d);
                    }
                }
            }
        }
        if (!dry)
        {
            const csmFloat32 td = d > 0.05f ? d : EstimateTalkDur(text.c_str());

            // 正挂着一个没选的提问：普通台词只动嘴、不换气泡（否则用户还没来得及点，选项就被冲掉了）
            const bool keepQuestion = _port != NULL && opts.empty() && _port->BubbleAwaitingChoice();

            if (keepQuestion)
            {
                StartTalk(td, text.c_str());   // 只有"只动嘴"这一支要立刻开口；会换气泡的那支由
                                               // PresentBubble 到点上屏再开口（排队时不能提前动嘴）
                printf("[pet] 气泡里有待选的提问，这句只说不换气泡：%s\n", text.c_str());
                fflush(stdout);
            }
            if (_port != NULL && !text.empty() && !keepQuestion)
            {
                csmFloat32 bd = td + 2.5f + (csmFloat32)opts.size() * 1.5f;

                if (bd > 16.0f) bd = 16.0f;
                std::vector<const char*> arr;

                for (size_t oi = 0; oi < opts.size(); ++oi)
                {
                    arr.push_back(opts[oi].c_str());
                }
                ShowPetBubble(text, opts, opts, optDo, bd, true, td);
                FeedPush("llm", opts.empty() ? text : peti18n::TF("{}（等你回答）", { text }));
            }
        }
        ++done;
    }
    return done;
}

/** 处理一行协议请求，返回回复 JSON（无换行）*/
std::string L2DPlayer::PetCommand(const std::string& line, csmFloat32 fps, bool* subscribe)
{
    jsonlite::Value v;
    std::string perr;
    std::string seq = "null";

    *subscribe = false;
    if (!jsonlite::parse(line.c_str(), line.size(), v, &perr) || !v.isObj())
    {
        return "{\"ok\":false,\"code\":\"bad_json\",\"msg\":\"" + std::string("JSON 解析失败") + "\"}";
    }
    if (v.has("seq"))
    {
        const jsonlite::Value& s = v.get("seq");

        seq.clear();
        if (s.isStr()) JStr(seq, s.str);
        else seq = JNum(s.numOr(0.0));
    }
    std::string head = "{\"seq\":" + seq + ",";
    std::string cmd = v.get("cmd").strOr("");
    if ((cmd == "say" || cmd == "speak" || cmd == "sequence" || cmd == "action" || cmd == "expression") && !v.get("client_id").isNull())
    {
        const std::string id = v.get("client_id").strOr("");
        if (!_integration.Registered(id)) return head + "\"ok\":false,\"msg\":\"register client_id first\"}";
        if (!_integration.Enabled(id)) return head + "\"ok\":true,\"shown\":false,\"reason\":\"client_disabled\"}";
    }
    if (cmd == "say")
    {
        cmd = "speak";   // 别名：say ≡ speak（口型 + 气泡；text/options 字段同构）
    }



    if (cmd == "integration_client")
    {
        const std::string id = v.get("client_id").strOr("");
        const std::string action = v.get("action").strOr("");
        bool ok = false;
        if (action == "register") ok = _integration.Register(id, v.get("name").strOr(id.c_str()));
        else if (action == "set_enabled" && v.get("enabled").isBool())
            ok = _integration.SetEnabled(id, v.get("enabled").b);
        return head + "\"ok\":" + (ok ? "true" : "false") + ",\"type\":\"integration_client\",\"msg\":" + JStrRet(ok ? "" : "invalid or unregistered client") + "}";
    }

    if (cmd == "companion_event")
    {
        if (!_cfgLoaded) CfgLoad();
        const std::string kind = v.get("kind").strOr("");
        if (!PetCompanion::ValidKind(kind))
            return head + "\"ok\":false,\"code\":\"bad_args\",\"msg\":\"invalid companion event kind\"}";
        const std::string sourceHint = plat::Utf8Head(v.get("source").strOr("Codex"), 40);
        const bool explicitClient = !v.get("client_id").isNull();
        const std::string clientId = explicitClient ? v.get("client_id").strOr("") : PetIntegration::LegacyId(sourceHint);
        if (!PetIntegration::ValidId(clientId))
            return head + "\"ok\":false,\"msg\":\"invalid client_id\"}";
        if (!explicitClient && !_integration.Registered(clientId)) _integration.Register(clientId, sourceHint);
        if (!_integration.Registered(clientId))
            return head + "\"ok\":false,\"msg\":\"register client_id first\"}";
        if (!_integration.Enabled(clientId))
            return head + "\"ok\":true,\"shown\":false,\"reason\":\"client_disabled\"}";
        const std::string source = _integration.Name(clientId);
        const std::string message = plat::Utf8Head(v.get("message").strOr(""), 600);
        const std::string identity = plat::Utf8Head(v.get("event_id").strOr(""), 200);
        const auto feedback = _companion.Receive(kind, source, message, identity.empty() ? identity : clientId + ":" + identity, _perfClock,
            Cfg("companion_interval"), Cfg("companion") > 0.5f, (!_bubbleOptions.empty() || _bubbleQueue.HasQuestion()), peti18n::Code());
        if (feedback.shown)
        {
            std::string speech = feedback.speech;
            // A short original excerpt keeps the actual status visible; full text is in the settings log.
            if (!message.empty()) speech += "\n" + plat::Utf8Head(message, 60);
            std::string step = "{\"speak\":" + JStrRet(speech);
            if (Cfg("companion_motion") > 0.5f && _petSeqs.empty()
                && ResolveActionFv("act:" + feedback.action) != NULL)
                step += ",\"action\":" + JStrRet("act:" + feedback.action) + ",\"scale\":0.45";
            step += "}";
            jsonlite::Value sv;
            if (jsonlite::parse(step.c_str(), step.size(), sv, NULL)) ExecPetStep(sv, NULL, -1, false);
            NoteExternalActivity();
        }
        return head + "\"ok\":true,\"type\":\"companion_event\",\"shown\":"
            + (feedback.shown ? "true" : "false") + ",\"reason\":" + JStrRet(feedback.reason) + "}";
    }

    if (cmd == "query")
    {
        return head + "\"ok\":true," + CapabilitiesJson(v.get("what"), fps) + "}";
    }
    if (cmd == "subscribe")
    {
        *subscribe = true;
        std::string o = head + "\"ok\":true,\"type\":\"subscribed\"";
        if (g_builtinAgent != NULL && g_builtinAgent->Running()
            && line.find("\"role\":\"ui\"") == std::string::npos)
        {
            o += ",\"warn\":\"内置 agent 正在运行：外部 agent 会双驱动，建议关掉一个\"";
        }
        return o + "}";
    }
    // 配置：{"cmd":"config"} 读全部；{"cmd":"config","set":{"opacity":0.8,…}} 改（存盘 + 推 config 事件）
    if (cmd == "config")
    {
        const jsonlite::Value& st = v.get("set");
        const jsonlite::Value& ss = v.get("setstr");
        std::string bad;

        if (st.isObj() || ss.isObj())
        {
            // 界面语言变没变，要看**写入前后**（任何来源：面板切语言、外部工具、脚本）——
            // 变了就通知内置 agent：把存量的记忆展示/性格/台词库/题库/笑话换成新语言
            // （面板那边的"比较上次见到的值"不可靠：agent 启动时可能还没读到 str 段）
            const std::string langBefore = CfgStr("lang");
            for (size_t i = 0; st.isObj() && i < st.obj.size(); ++i)
            {
                const jsonlite::Value& x = st.obj[i].second;
                const csmFloat32 val = x.isBool() ? (x.b ? 1.0f : 0.0f) : (csmFloat32)x.numOr(0.0);

                if (!CfgSet(st.obj[i].first, val)) bad += (bad.empty() ? "" : ",") + st.obj[i].first;
            }
            // 字符串配置：{"cmd":"config","setstr":{"weather_loc":"北京"}}（空串 = 删）
            for (size_t i = 0; ss.isObj() && i < ss.obj.size(); ++i)
            {
                if (!ss.obj[i].second.isStr()) { bad += (bad.empty() ? "" : ",") + ss.obj[i].first; continue; }
                CfgStrSet(ss.obj[i].first, ss.obj[i].second.str);
            }
            CfgApply();
            // 气泡字号要实时生效：端口读的是环境变量（见 PortGlfwPet::BubbleBasePx），
            // 面板拖滑条改 config 后必须马上重推一次，否则要重启才变。
            {
                const csmFloat32 bp = Cfg("bubble_px");
                if (bp >= 8.0f && bp <= 64.0f)
                {
                    char bpx[32];
                    snprintf(bpx, sizeof(bpx), "%d", (int)(bp + 0.5f));
                    plat::SetEnv("L2DPET_BUBBLE_PX", bpx);
                }
            }
            CfgSave();
            _petEvents.push_back("{\"event\":\"config\",\"config\":" + CfgJson() + "}");
            if (CfgStr("lang") != langBefore && g_builtinAgent != NULL)
            {
                // 事件里带上**新值**：agent 那边的 _cfgLang 要等 config 事件才更新，
                // 而这条事件是同步推的、可能先到（agent 的日志曾因此显示旧语言）
                std::string lle;
                JStr(lle, CfgStr("lang"));
                g_builtinAgent->PushEvent("{\"event\":\"lang_changed\",\"lang\":" + lle + "}");
            }
        }
        std::string o = head + "\"ok\":" + (bad.empty() ? "true" : "false") + ",\"type\":\"config\",\"config\":" + CfgJson();
        if (!bad.empty()) { o += ",\"code\":\"unknown_key\",\"msg\":"; JStr(o, "未知配置项：" + bad); }
        return o + "}";
    }
    // 切模型：{"cmd":"model"} 列出模型库；{"cmd":"model","index":n} 或 {"cmd":"model","name":"x"} 切换
    // （下一帧在主循环里重建播放器；完成后推 model_changed，订阅方应重新 query）
    if (cmd == "model")
    {
        int idx = v.get("index").isNum() ? (int)v.get("index").num : -1;
        const std::string nm = v.get("name").strOr("");
        for (size_t i = 0; i < _modelNames.size() && !nm.empty(); ++i)
        {
            if (_modelNames[i] == nm) idx = (int)i;
        }
        if (v.has("index") || v.has("name"))
        {
            if (idx < 0 || idx >= (int)_modelNames.size())
            {
                return head + "\"ok\":false,\"code\":\"bad_model\",\"msg\":\"模型库里没有这个模型\"}";
            }
            _modelSwitchTo = idx;
        }
        std::string o = head + "\"ok\":true,\"type\":\"model\",\"current\":" + std::to_string(_modelIndex) + ",\"models\":[";
        for (size_t i = 0; i < _modelNames.size(); ++i)
        {
            if (i) o += ",";
            JStr(o, _modelNames[i]);
        }
        return o + "]" + (idx >= 0 ? ",\"switching_to\":" + std::to_string(idx) : std::string()) + "}";
    }
    // 能力探测（面板"模型"页）：{"cmd":"probe"} 直连档 / {"cmd":"probe","all":true} 全量
    //   {"cmd":"probe","phys":true} 物理稳态档（配合 all 用；批量生成能力的脚本走两遍：直连 + 物理）
    // 非阻塞后台推进（约 30s）；完成后写 l2d.capability.json + 自动重载模型（臂骨架/分区生效）
    if (cmd == "probe")
    {
        const bool phys = v.get("phys").isBool() && v.get("phys").b;
        SetProbePhys(phys);
        ProbeEnqueue(v.get("all").isBool() && v.get("all").b);
        _petEvents.push_back("{\"event\":\"probe\",\"started\":true}");
        return head + "\"ok\":true,\"type\":\"probe\",\"msg\":\"" + (phys ? "物理稳态档" : "直连档")
               + "能力探测已开始（后台推进，完成后自动重载模型）\"}";
    }
    // 导入模型（面板"模型"页）：{"cmd":"import_model","path":"/abs/x.zip 或 目录"}
    // → 解压/拷贝进用户模型库 → 请求主循环重扫并切过去（识别能力：加载时自动算指纹/补全 VTS 登记）
    if (cmd == "import_model")
    {
        const std::string src = v.get("path").strOr("");
        if (!IsAbsPath(src))
        {
            return head + "\"ok\":false,\"code\":\"bad_path\",\"msg\":\"给绝对路径（.zip 或含 model3.json 的目录）\"}";
        }
        std::string name, err;
        const std::string root = plat::HomeDir() + "/.local/share/l2dpet/models";
        if (!ImportModelInto(src, root, &name, &err))
        {
            _petEvents.push_back("{\"event\":\"model_import\",\"ok\":false,\"msg\":" + JStrRet(err) + "}");
            return head + "\"ok\":false,\"code\":\"import_failed\",\"msg\":" + JStrRet(err) + "}";
        }
        _modelRescanRequest = true;
        _modelRescanSwitch = name;
        _petEvents.push_back("{\"event\":\"model_import\",\"ok\":true,\"name\":" + JStrRet(name) + "}");
        return head + "\"ok\":true,\"type\":\"import_model\",\"name\":" + JStrRet(name) + "}";
    }
    // 删除模型（面板"模型"页）：{"cmd":"delete_model","name":"yumi"}
    // 只允许删用户模型库（~/.local/share/l2dpet/models）下的一级目录；正在使用的模型拒删（先切走）
    if (cmd == "delete_model")
    {
        const std::string nm = v.get("name").strOr("");
        if (nm.empty() || nm == "." || nm == ".." || nm[0] == '.'
            || nm.find('/') != std::string::npos || nm.find('\\') != std::string::npos)
        {
            return head + "\"ok\":false,\"code\":\"bad_name\",\"msg\":\"模型名不合法\"}";
        }
        const std::string root = plat::HomeDir() + "/.local/share/l2dpet/models";
        const std::string dir = root + "/" + nm;
        if (!plat::IsDir(dir))
        {
            _petEvents.push_back("{\"event\":\"model_delete\",\"ok\":false,\"name\":" + JStrRet(nm)
                                 + ",\"msg\":\"没找到这个模型目录（只删用户模型库里的）\"}");
            return head + "\"ok\":false,\"code\":\"not_found\",\"msg\":\"没找到这个模型目录（只删用户模型库里的）\"}";
        }
        if (plat::Canonicalize(dir) == plat::Canonicalize(_dir))
        {
            const std::string m = "正在使用的模型不能删除（先切换到别的模型）";
            _petEvents.push_back("{\"event\":\"model_delete\",\"ok\":false,\"name\":" + JStrRet(nm)
                                 + ",\"msg\":" + JStrRet(m) + "}");
            return head + "\"ok\":false,\"code\":\"in_use\",\"msg\":" + JStrRet(m) + "}";
        }
        RemoveDirTree(dir);
        if (plat::IsDir(dir))   // 没删干净（权限/占用）：报错，别假装成功
        {
            const std::string m = "删除失败（文件被占用或无权限）";
            _petEvents.push_back("{\"event\":\"model_delete\",\"ok\":false,\"name\":" + JStrRet(nm)
                                 + ",\"msg\":" + JStrRet(m) + "}");
            return head + "\"ok\":false,\"code\":\"delete_failed\",\"msg\":" + JStrRet(m) + "}";
        }
        _modelRescanRequest = true;   // 只刷新列表（不换模型、不重载）
        _modelRescanSwitch.clear();
        printf("[ui] 删除模型：%s\n", nm.c_str());
        fflush(stdout);
        _petEvents.push_back("{\"event\":\"model_delete\",\"ok\":true,\"name\":" + JStrRet(nm) + "}");
        return head + "\"ok\":true,\"type\":\"delete_model\",\"name\":" + JStrRet(nm) + "}";
    }
    // 讲个笑话（右键菜单/面板）：{"cmd":"joke"} —— 优先念 agent 定期补货的笑话池（本地秒回），
    // 其次台词库 life.joke 组（LLM 按性格写过），最后内置兜底；讲完推 joke_told 让 agent 记账
    if (cmd == "joke")
    {
        static const char* const kDef[] = {
            "给你讲个冷笑话：为什么程序员总分不清万圣节和圣诞节？因为 Oct 31 == Dec 25。",
            "从前有只小鸭子过马路，然后……它就过去了。",
            "我昨天梦见自己在吃棉花糖，醒来发现枕头不见了。",
        };
        std::string j;
        if (!_jokePool.empty())
        {
            // 语言对不上的跳过（切过语言后池子里会留旧语言的笑话）：本地讲出来会串语言
            for (size_t tries = 0; tries < _jokePool.size(); ++tries)
            {
                const std::string& cand = _jokePool[_jokeNext % _jokePool.size()];
                _jokeNext = (_jokeNext + 1) % _jokePool.size();
                if (SpeechLangOk(cand, peti18n::Code()))
                {
                    j = cand;
                    break;
                }
            }
        }
        if (j.empty())
        {
            j = (_isPet && _port != NULL)
                    ? LifeLine("joke", kDef, sizeof(kDef) / sizeof(kDef[0]))
                    : std::string();
        }
        if (j.empty())
        {
            return head + "\"ok\":false,\"code\":\"no_bubble\",\"msg\":\"没有气泡出口（非桌宠模式）\"}";
        }
        // 睡着了就别说笑话了（梦话体回一句，别吵醒她）
        if (_isPet && _lifeState == LS_ASLEEP)
        {
            static const char* const kDreamLines[] = { "Zzz……芙蕾在梦里给你讲……" };
            const std::string dream = LifeLine("sleep_talk", kDreamLines, 1);
            LifeSay(dream, 3.5f);   // LifeLine 已按语言翻好
            return head + "\"ok\":true,\"type\":\"joke\",\"sleeping\":true}";
        }
        const bool tiredJoke = _isPet && InteractCost(0.02f, "要求讲笑话");
        if (!tiredJoke) LifeSay(j, 6.5f);   // 累了：只抱怨，不讲
        if (!tiredJoke) _petEvents.push_back("{\"event\":\"joke_told\",\"text\":" + JStrRet(j) + "}");
        MuteTouchResponse();   // 笑话气泡在，3 秒内摸她别顶掉
        return head + "\"ok\":true,\"type\":\"joke\",\"text\":" + JStrRet(j)
               + ",\"pool\":" + std::to_string((int)_jokePool.size()) + "}";
    }
    // 笑话池（agent 定期补货后推下来）：{"cmd":"jokes","list":["…"]} —— 合并去重；新增的排后面优先讲
    if (cmd == "jokes")
    {
        const jsonlite::Value& list = v.get("list");
        const size_t oldSize = _jokePool.size();
        size_t added = 0, dropped = 0;
        if (list.isArr())
        {
            for (size_t i = 0; i < list.arr.size() && _jokePool.size() < 60; ++i)
            {
                const std::string t = list.arr[i].strOr("");
                if (t.empty() || t.size() > 300) continue;
                // 语言对不上的不收：中文界面里冒出整句英文的笑话（LLM 老毛病）比没有更糟
                if (!SpeechLangOk(t, peti18n::Code())) { ++dropped; continue; }
                bool dup = false;
                for (size_t k = 0; k < _jokePool.size() && !dup; ++k) dup = _jokePool[k] == t;
                if (dup) continue;
                _jokePool.push_back(t);
                ++added;
            }
        }
        if (added > 0)
        {
            _jokeNext = oldSize;   // 先讲刚补进来的新笑话
        }
        printf("[joke] 笑话池 %zu 条（新增 %zu，语言不符丢弃 %zu）\n", _jokePool.size(), added, dropped);
        fflush(stdout);
        return head + "\"ok\":true,\"type\":\"jokes\",\"pool\":" + std::to_string((int)_jokePool.size()) + "}";
    }
    // 猜拳：{"cmd":"game","kind":"rps"} —— 本地出题（气泡选项）→ 点选后判定 → 结果气泡 + game 事件（agent 接话/记战绩）
    if (cmd == "game")
    {
        if (!_isPet || _port == NULL)
        {
            return head + "\"ok\":false,\"code\":\"no_bubble\",\"msg\":\"没有气泡出口（非桌宠模式）\"}";
        }
        if (_port->BubbleAwaitingChoice())
        {
            return head + "\"ok\":false,\"code\":\"busy\",\"msg\":\"气泡里还有没选完的选项，等主人选完再玩\"}";
        }
        static const char* const kAsk[] = { "石头、剪刀、布！你出什么？", "来玩猜拳！出哪个？",
                                            "猜拳！手伸出来——石头，剪刀，还是布？" };
        const std::string ask = LifeLine("rps.ask", kAsk, sizeof(kAsk) / sizeof(kAsk[0]));
        std::vector<std::string> opts;
        opts.push_back("石头");
        opts.push_back("剪刀");
        opts.push_back("布");
        // 气泡里显示的手势名按语言翻译（内部判定/事件仍用中文标签，和 _bubbleOptions 对得上）
        // 两趟走：先填好 shown 再取指针——边 push_back 边 c_str() 会被扩容失效（第一格渲染成乱码/空白，踩过）
        std::vector<std::string> shown;
        for (size_t i = 0; i < opts.size(); ++i)
        {
            shown.push_back(peti18n::Tr(opts[i]));
        }
        std::vector<const char*> arr;
        for (size_t i = 0; i < shown.size(); ++i)
        {
            arr.push_back(shown[i].c_str());
        }
        ClearBubbleDisplay(); // Explicit user interaction replaces the previous conversation.
        ShowPetBubble(ask, shown, opts, {}, 20.0f);   // 口型由 PresentBubble 上屏时起（见 ShowPetBubble 注释）
        _gameRps = 1;
        MuteTouchResponse();   // 出拳期间点她/搓她别来抢这个气泡
        printf("[game] 猜拳开始：%s\n", ask.c_str());
        fflush(stdout);
        return head + "\"ok\":true,\"type\":\"game\",\"kind\":\"rps\"}";
    }
    // 聊天气：{"cmd":"weather"} —— 交给内置 agent 现编一段（带位置；见 agent 的 weather 事件）；
    // agent 不在就本地兜一句
    if (cmd == "weather")
    {
        MuteTouchResponse();   // 天气说出口之后 3 秒内别被"摸摸头"的反应顶掉
        if (g_builtinAgent != NULL && g_builtinAgent->Running())
        {
            g_builtinAgent->PushEvent("{\"event\":\"weather_ask\"}");
            return head + "\"ok\":true,\"type\":\"weather\",\"msg\":\"已让内置 agent 说说天气（几秒后出）\"}";
        }
        const std::string loc = CfgStr("weather_loc");
        LifeSay(loc.empty() ? peti18n::Tr("今天外面天气怎么样呀？")
                            : peti18n::TF("（{}）今天外面天气怎么样呀？", { loc }),
                3.5f);
        return head + "\"ok\":true,\"type\":\"weather\",\"local\":true}";
    }
    // 记忆压缩结果回报（agent → 桌宠 → 广播 mem_compress 事件；与上面的"面板请求压缩"分开，
    // 否则 agent 的回报会被当成新请求又转回 agent，来回打转）
    if (cmd == "mem_notify")
    {
        const bool ok = v.get("ok").isBool() && v.get("ok").b;
        // 事件名必须和 agent 监听的触发名（mem_compress）不同：所有 pet 事件都会喂给 agent，
        // 同名会让他把自己的回报当成新请求，来回打转
        _petEvents.push_back(std::string("{\"event\":\"mem_compress_done\",\"ok\":") + (ok ? "true" : "false")
                             + ",\"report\":" + JStrRet(v.get("report").strOr(""))
                             + ",\"msg\":" + JStrRet(v.get("msg").strOr("")) + "}");
        if (ok) _memoryCompressedHint = v.get("report").strOr("");   // 面板那行的即时反馈（下一次 state 刷新会换成真时间）
        return head + "\"ok\":true,\"type\":\"mem_notify\"}";
    }
    // 记忆压缩（面板"立即压缩"）：转给内置 agent（它才拿得到 LLM 与记忆对象）
    if (cmd == "mem_compress")
    {
        if (g_builtinAgent != NULL && g_builtinAgent->Running())
        {
            g_builtinAgent->PushEvent("{\"event\":\"mem_compress\"}");
            return head + "\"ok\":true,\"type\":\"mem_compress\",\"msg\":\"已让内置 agent 压缩记忆（几秒）\"}";
        }
        _petEvents.push_back("{\"event\":\"mem_compress\",\"ok\":false,\"msg\":\"内置 agent 没在跑\"}");
        return head + "\"ok\":false,\"code\":\"no_agent\",\"msg\":\"内置 agent 没在跑（外部形态请自行压缩）\"}";
    }
    // 备份导出/导入：记忆 + 性格 + 题库 + 笑话 + 配置 打成**一个 JSON 文件**（模型文件本身不含）
    // {"cmd":"export","path":"/abs/x.json"} / {"cmd":"import","path":"..."}
    if (cmd == "export" || cmd == "import")
    {
        const std::string path = v.get("path").strOr("");
        if (!IsAbsPath(path))
        {
            return head + "\"ok\":false,\"code\":\"bad_path\",\"msg\":\"给绝对路径（.json）\"}";
        }
        const std::string root = plat::HomeDir() + "/.config/l2dpet";
        if (cmd == "export")
        {
            std::string bundle, err;
            int files = 0;
            if (!BackupBuild(root, &bundle, &files, &err))
            {
                _petEvents.push_back("{\"event\":\"mem_export\",\"ok\":false,\"msg\":" + JStrRet(err) + "}");
                return head + "\"ok\":false,\"code\":\"export_failed\",\"msg\":" + JStrRet(err) + "}";
            }
            if (!plat::WritePrivateFile(path, bundle))
            {
                const std::string m = "写文件失败（目录不存在/无权限？）";
                _petEvents.push_back("{\"event\":\"mem_export\",\"ok\":false,\"msg\":" + JStrRet(m) + "}");
                return head + "\"ok\":false,\"code\":\"write_failed\",\"msg\":" + JStrRet(m) + "}";
            }
            printf("[backup] 导出 %d 个文件 → %s（%zu 字节）\n", files, path.c_str(), bundle.size());
            fflush(stdout);
            _petEvents.push_back("{\"event\":\"mem_export\",\"ok\":true,\"files\":" + std::to_string(files)
                                 + ",\"path\":" + JStrRet(path) + "}");
            return head + "\"ok\":true,\"type\":\"export\",\"files\":" + std::to_string(files)
                   + ",\"path\":" + JStrRet(path) + ",\"bytes\":" + std::to_string((long long)bundle.size()) + "}";
        }
        std::string err;
        int files = 0;
        if (!BackupRestore(path, root, &files, &err))
        {
            _petEvents.push_back("{\"event\":\"mem_import\",\"ok\":false,\"msg\":" + JStrRet(err) + "}");
            return head + "\"ok\":false,\"code\":\"import_failed\",\"msg\":" + JStrRet(err) + "}";
        }
        printf("[backup] 导入 %d 个文件 ← %s\n", files, path.c_str());
        fflush(stdout);
        // 让 agent 把盘上的记忆/性格/题库/笑话重读进内存（否则它还拿旧副本说话）
        if (g_builtinAgent != NULL && g_builtinAgent->Running())
        {
            g_builtinAgent->PushEvent("{\"event\":\"profile_reload\"}");
        }
        _petEvents.push_back("{\"event\":\"mem_import\",\"ok\":true,\"files\":" + std::to_string(files) + "}");
        return head + "\"ok\":true,\"type\":\"import\",\"files\":" + std::to_string(files) + "}";
    }
    // LLM 接入：{"cmd":"llm_config"} 读（不含密钥）；带 api_key/llm_model/base_url 则改**当前激活供应商**
    // （模型名用 llm_model：协议里的 "model" 是模型指纹，客户端会自动带）
    if (cmd == "llm_config")
    {
        bool changed = false;
        // 新机器还没有任何供应商：先建一个默认的再写——以前这里 cur==NULL，Key/地址被**静默丢弃**却回 ok:true
        if (_llmProviders.empty() && (v.get("api_key").isStr() || v.get("llm_model").isStr() || v.get("base_url").isStr()))
        {
            l2d::LlmProvider d;
            d.id = "default";
            d.name = "默认";
            d.baseUrl = "https://api.deepseek.com";
            d.model = "deepseek-chat";
            d.models.push_back(d.model);
            _llmProviders.push_back(d);
            _llmActive = d.id;
        }
        l2d::LlmProvider* cur = ActiveLlmProvider();
        if (v.get("api_key").isStr() && cur != NULL) { cur->apiKey = v.get("api_key").str; changed = true; }
        if (v.get("llm_model").isStr() && cur != NULL)
        {
            cur->model = v.get("llm_model").str;
            if (!cur->model.empty()
                && std::find(cur->models.begin(), cur->models.end(), cur->model) == cur->models.end())
            {
                cur->models.push_back(cur->model);   // 新模型名自动进列表
            }
            changed = true;
        }
        if (v.get("base_url").isStr() && cur != NULL) { cur->baseUrl = v.get("base_url").str; changed = true; }
        if ((cur != NULL && (cur->apiKey.size() > 300 || cur->model.size() > 120 || cur->baseUrl.size() > 300)))
        {
            LlmCfgLoad();
            return head + "\"ok\":false,\"code\":\"too_long\",\"msg\":\"字段过长\"}";
        }
        if (changed)
        {
            if (!LlmCfgSave())
            {
                return head + "\"ok\":false,\"code\":\"io\",\"msg\":\"写不了 " + LlmCfgPath() + "\"}";
            }
            _petEvents.push_back("{\"event\":\"llm_config\",\"llm\":" + LlmPublicJson() + "}");
        }
        return head + "\"ok\":true,\"type\":\"llm_config\",\"llm\":" + LlmPublicJson() + "}";
    }
    // 多供应商全量保存（面板"对话"页用）：
    // {"cmd":"llm_providers","active":"id","providers":[{id,name,base_url,model,models:[...],api_key?}]}
    // api_key 空/缺省 = 保留该 id 原密钥（面板不持有明文密钥）
    if (cmd == "llm_providers")
    {
        const jsonlite::Value& arr = v.get("providers");
        if (!arr.isArr())
        {
            return head + "\"ok\":false,\"code\":\"bad_args\",\"msg\":\"providers 必须是数组\"}";
        }
        if (arr.arr.size() > 32)
        {
            return head + "\"ok\":false,\"code\":\"too_many\",\"msg\":\"供应商最多 32 个\"}";
        }
        std::vector<l2d::LlmProvider> next;
        for (size_t i = 0; i < arr.arr.size(); ++i)
        {
            const jsonlite::Value& p = arr.arr[i];
            if (!p.isObj()) continue;
            l2d::LlmProvider one;
            one.id = p.get("id").strOr("");
            one.name = p.get("name").strOr("");
            one.baseUrl = p.get("base_url").strOr("");
            one.model = p.get("model").strOr("");
            one.apiKey = p.get("api_key").strOr("");
            const jsonlite::Value& ms = p.get("models");
            if (ms.isArr())
            {
                for (size_t k = 0; k < ms.arr.size() && one.models.size() < 32; ++k)
                {
                    const std::string m = ms.arr[k].strOr("");
                    if (!m.empty()) one.models.push_back(m);
                }
            }
            if (one.id.empty() || one.name.size() > 60 || one.baseUrl.size() > 300 || one.model.size() > 120)
            {
                return head + "\"ok\":false,\"code\":\"bad_provider\",\"msg\":\"供应商字段缺失/过长\"}";
            }
            if (one.apiKey.empty())
            {
                // 保留原密钥（按 id 对上）
                for (size_t k = 0; k < _llmProviders.size(); ++k)
                {
                    if (_llmProviders[k].id == one.id)
                    {
                        one.apiKey = _llmProviders[k].apiKey;
                        break;
                    }
                }
            }
            if (one.apiKey.size() > 300)
            {
                return head + "\"ok\":false,\"code\":\"too_long\",\"msg\":\"API Key 过长\"}";
            }
            if (one.model.empty() && !one.models.empty()) one.model = one.models[0];
            next.push_back(one);
        }
        std::string active = v.get("active").strOr("");
        const std::vector<l2d::LlmProvider> keep = _llmProviders;
        _llmProviders = next;
        _llmActive = active;
        if (!LlmCfgSave())
        {
            _llmProviders = keep;   // 写盘失败回滚内存态
            return head + "\"ok\":false,\"code\":\"io\",\"msg\":\"写不了 " + LlmCfgPath() + "\"}";
        }
        _petEvents.push_back("{\"event\":\"llm_config\",\"llm\":" + LlmPublicJson() + "}");
        return head + "\"ok\":true,\"type\":\"llm_providers\",\"llm\":" + LlmPublicJson() + "}";
    }
    // agent 自报接入来源（不含密钥）：{"cmd":"agent_info","key_source":"env|file|none","model":"…","base_url":"…"}
    if (cmd == "agent_info")
    {
        _agentInfo = "{\"key_source\":";
        JStr(_agentInfo, v.get("key_source").strOr("none"));
        _agentInfo += ",\"model\":";
        JStr(_agentInfo, std::string(v.get("model").strOr("")).substr(0, 120));
        _agentInfo += ",\"base_url\":";
        JStr(_agentInfo, std::string(v.get("base_url").strOr("")).substr(0, 300));
        _agentInfo += std::string(",\"paused\":") + (v.get("paused").isBool() && v.get("paused").b ? "true" : "false");
        _agentInfo += "}";
        return head + "\"ok\":true,\"type\":\"agent_info\"}";
    }
    // LLM 交互记录（agent 每次调用后上报）：{"cmd":"llm_log","entry":{kind,ms,tok_in,tok_out,ok,question,…}}
    if (cmd == "llm_log")
    {
        const jsonlite::Value& e = v.get("entry");
        if (!e.isObj())
        {
            return head + "\"ok\":false,\"code\":\"bad_json\",\"msg\":\"需要 entry 对象\"}";
        }
        // 原样存（截掉超长的）；统计
        const size_t a = line.find("\"entry\"");
        const size_t b0 = a == std::string::npos ? std::string::npos : line.find('{', a);
        const size_t b1 = line.rfind('}');
        std::string raw = (b0 != std::string::npos && b1 != std::string::npos && b1 > b0)
                              ? line.substr(b0, b1 - b0) : std::string("{}");
        jsonlite::Value chk;
        if (raw.size() > 4096 || !jsonlite::parse(raw.c_str(), raw.size(), chk, NULL) || !chk.isObj())
        {
            raw = "{\"kind\":\"?\",\"note\":\"（记录过长或格式不对，已丢弃）\"}";
        }
        _llmLog.push_back(raw);
        while (_llmLog.size() > 200) _llmLog.pop_front();
        ++_llmCalls;
        ++_llmKinds[e.get("kind").strOr("?")];
        if (e.get("ok").isBool() && !e.get("ok").b) ++_llmErrors;
        if (e.get("question").isBool() && e.get("question").b) ++_llmQuestions;
        _llmTokIn += (long)e.get("tok_in").numOr(0);
        _llmTokOut += (long)e.get("tok_out").numOr(0);
        _llmMsSum += e.get("ms").numOr(0);
        return head + "\"ok\":true,\"type\":\"llm_log\"}";
    }
    // 跨会话记忆（agent 归纳后上报）：{"cmd":"memory","text":"<markdown>","at":"10-09 17:33"}
    // 原样存着给面板"行为 → 记忆本"页显示（记忆文件本体在 agent 侧 ~/.config/l2dpet/memory.md）
    if (cmd == "memory")
    {
        const std::string t = v.get("text").strOr("");

        // 上限 12KB（中文 3 字节/字 ≈ 4000 字）；截断落在 UTF-8 字符边界上，别切出半个字
        size_t cut = t.size() > 12288 ? 12288 : t.size();
        while (cut > 0 && cut < t.size() && ((unsigned char)t[cut] & 0xC0) == 0x80) --cut;
        _memoryText = t.substr(0, cut);
        _memoryAt = v.get("at").strOr("");
        return head + "\"ok\":true,\"type\":\"memory\",\"chars\":" + JNum((double)_memoryText.size()) + "}";
    }
    // 模型性格（agent 生成后上报）：{"cmd":"persona","text":"<多行>","name":"…","by":"llm|heuristic"}
    // 原样存着给面板"模型"页显示（性格文件本体在 agent 侧 ~/.config/l2dpet/personas/<指纹>.json）；
    // 面板要重新生成：{"cmd":"persona","regen":true} → 转给内置 agent（没人接就回一声）
    // 名字与称呼（面板"模型"页）：{"cmd":"names","name":"…","nick":"…","call_user":"…"}（空串 = 清除）
    // 存在该模型的性格文件里（内置 agent 负责落盘与重写台词）；这里只做校验和转交
    if (cmd == "names")
    {
        if (g_builtinAgent == NULL || !g_builtinAgent->Running())
        {
            return head + "\"ok\":false,\"code\":\"no_agent\",\"msg\":\"内置 agent 没在跑\"}";
        }
        auto clean = [](std::string t) {
            for (char& c : t) if ((unsigned char)c < 0x20) c = ' ';   // 不许换行等控制字符（会混进提示词结构）
            return t.size() > 120 ? t.substr(0, 120) : t;
        };
        const std::string nm = clean(v.get("name").strOr("")), nk = clean(v.get("nick").strOr("")),
                          cl = clean(v.get("call_user").strOr(""));
        g_builtinAgent->PushEvent("{\"event\":\"names_set\",\"name\":" + JStrRet(nm) + ",\"nick\":" + JStrRet(nk)
                                  + ",\"call\":" + JStrRet(cl) + "}");
        return head + "\"ok\":true,\"type\":\"names\"}";
    }
    if (cmd == "persona")
    {
        if (v.get("regen").isBool() && v.get("regen").b)
        {
            if (g_builtinAgent != NULL && g_builtinAgent->Running())
            {
                if (!v.get("guidance").isNull() && !v.get("guidance").isStr())
                    return head + "\"ok\":false,\"code\":\"bad_guidance\",\"msg\":\"性格方向必须是文字\"}";
                const std::string guidance = v.get("guidance").strOr(_personaGuidance.c_str());
                if (guidance.size() > 4000 || std::count_if(guidance.begin(), guidance.end(),
                    [](unsigned char c) { return (c & 0xC0) != 0x80; }) > 1000)
                    return head + "\"ok\":false,\"code\":\"bad_guidance\",\"msg\":\"性格方向过长，请缩短到 1000 字以内\"}";
                g_builtinAgent->PushEvent("{\"event\":\"persona_regen\",\"guidance\":" + JStrRet(guidance) + "}");
                return head + "\"ok\":true,\"type\":\"persona\",\"msg\":\"已让内置 agent 重新生成性格（约 3~10 秒）\"}";
            }
            _petEvents.push_back("{\"event\":\"persona\",\"ok\":false,\"msg\":\"内置 agent 没在跑（外部 agent 形态请自行生成）\"}");
            return head + "\"ok\":false,\"code\":\"no_agent\",\"msg\":\"内置 agent 没在跑（外部 agent 形态请自行生成）\"}";
        }
        const std::string t = v.get("text").strOr("");
        if (!t.empty())
        {
            size_t cut = t.size() > 4096 ? 4096 : t.size();
            while (cut > 0 && cut < t.size() && ((unsigned char)t[cut] & 0xC0) == 0x80) --cut;
            _personaText = t.substr(0, cut);
            _personaName = v.get("name").strOr("");
            _personaBy = v.get("by").strOr("");
            _personaNick = v.get("nick").strOr("");
            _personaCall = v.get("call").strOr("");
            _personaUserName = v.get("user_name").strOr("");
            _personaGuidance = v.get("guidance").strOr("");
            PanelStateJson(0);
            _petEvents.push_back("{\"event\":\"persona\",\"ok\":true,\"name\":" + JStrRet(_personaName)
                                 + ",\"by\":" + JStrRet(_personaBy) + "}");
        }
        if (_personaText.empty())
        {
            return head + "\"ok\":false,\"code\":\"none\",\"msg\":\"还没有这个模型的性格（内置 agent 生成后显示；配好 API Key 会自动生成）\"}";
        }
        return head + "\"ok\":true,\"type\":\"persona\",\"name\":" + JStrRet(_personaName)
               + ",\"by\":" + JStrRet(_personaBy) + "}";
    }
    // UI 进程（l2dpet-ui）回来的操作：
    //   {"cmd":"choice","index":n}  气泡选项被点 → 照旧推 choice 事件给 LLM
    //   {"cmd":"window","visible":bool}  显示/隐藏角色窗口（托盘/菜单）
    //   {"cmd":"quit"}  退出桌宠
    if (cmd == "choice")
    {
        PetChoiceMade((int)v.get("index").numOr(-1.0));
        return head + "\"ok\":true,\"type\":\"choice\"}";
    }
    if (cmd == "window")
    {
        if (_port != NULL && v.get("visible").isBool()) _port->SetWindowVisible(v.get("visible").b);
        return head + "\"ok\":true,\"type\":\"window\",\"visible\":" + std::string(_port != NULL && _port->Hidden() ? "false" : "true") + "}";
    }
    if (cmd == "quit")
    {
        // 排错/测试用：退出前把"没命中 i18n 表的自带文案"打出来（L2DPET_I18N_DEBUG=1）
        if (getenv("L2DPET_I18N_DEBUG") != NULL && !peti18n::MissingReport().empty())
        {
            printf("[i18n] 未翻译的自带文案（%s）：\n%s\n", peti18n::Code(), peti18n::MissingReport().c_str());
            fflush(stdout);
        }
        if (_port != NULL) _port->RequestQuit();
        return head + "\"ok\":true,\"type\":\"quit\"}";
    }
    // UI 进程（l2dpet-ui，EUI-NEO）轮询用：{"cmd":"panel_state"}
    // → {"ok":true,"type":"panel_state","state":{…同 PanelStateJson…},"panel_req":{"page":n,"seq":k}}
    if (cmd == "panel_state")
    {
        return head + "\"ok\":true,\"type\":\"panel_state\",\"state\":" + PanelStateJson(_agentCount)
               + ",\"panel_req\":{\"page\":" + std::to_string(_panelReqPage) + ",\"seq\":" + std::to_string(_panelReqSeq) + "}}";
    }
    // 让已打开的面板切到某页（桌宠右键菜单/托盘点"状态与设置"时由 port 发）：{"cmd":"panel_request","page":n}
    if (cmd == "panel_request")
    {
        _panelReqPage = (int)v.get("page").numOr(0.0);
        ++_panelReqSeq;
        return head + "\"ok\":true,\"type\":\"panel_request\",\"seq\":" + std::to_string(_panelReqSeq) + "}";
    }
    // 打开面板：{"cmd":"panel","page":0..4} 设置窗口某页；{"cmd":"panel","menu":true} 右键菜单
    if (cmd == "panel")
    {
        if (_port != NULL)
        {
            _port->OpenPanel((int)v.get("page").numOr(0.0), v.get("menu").isBool() && v.get("menu").b);
        }
        return head + "\"ok\":true,\"type\":\"panel\"}";
    }
    // 面板/外部"摸一下"：{"cmd":"touch","zone":"head","kind":"rub"|"click"}（落在该区的代表点）
    if (cmd == "touch")
    {
        const int z = ZoneFromKey(v.get("zone").strOr("head"));
        const bool rub = strcmp(v.get("kind").strOr("rub"), "rub") == 0;
        csmFloat32 bb[4];

        if (z < 0 || !_modelBBoxValid)
        {
            return head + "\"ok\":false,\"code\":\"bad_zone\",\"msg\":\"未知区域\"}";
        }
        ComputeArtBBox(bb);
        const csmFloat32 x = (bb[0] + bb[2]) * 0.5f, y = bb[1] + (bb[3] - bb[1]) * (z == Z_HEAD ? 0.06f : 0.12f);
        char b[96];

        snprintf(b, sizeof(b), "{\"event\":\"%s\",\"zone\":\"%s\",\"n\":1}", rub ? "rub" : "click", ZoneKey(z));
        _petEvents.push_back(b);
        ZoneReact(z, rub ? 1 : 0, 1, x, y);
        MuteTouchResponse();   // 手动这一次已经反应过了：随后 3 秒的点按不再反应（只留特效）
        return head + "\"ok\":true,\"type\":\"touch\"}";
    }
    // 内在状态：读 / 调（调试与剧情用）：{"cmd":"mood"} / {"cmd":"mood","state":"asleep","energy":0.2,…}
    if (cmd == "mood")
    {
        if (v.get("energy").isNum()) _lifeEnergy = Clamp01((csmFloat32)v.get("energy").num);
        if (v.get("affection").isNum()) _lifeAffection = Clamp01((csmFloat32)v.get("affection").num);
        if (v.get("annoy").isNum()) _lifeAnnoy = Clamp01((csmFloat32)v.get("annoy").num);
        if (v.get("lonely").isNum()) _lifeLonely = Clamp01((csmFloat32)v.get("lonely").num);
        const std::string st = v.get("state").strOr("");

        for (int i = 0; i <= LS_SULK && !st.empty(); ++i)
        {
            if (st == LifeStateKey(i))
            {
                LifeEnter(i, "指令");
            }
        }
        MuteTouchResponse();   // 菜单里的哄睡（mood）也算一次交互：她说话/睡着的 3 秒里别被点她顶掉
        return head + "\"ok\":true,\"type\":\"mood\",\"mood\":" + LifeMoodJson() + "}";
    }
    // 本地台词库：{"cmd":"reactions","lines":{"head.click":["…"],"head.rub":[…],"angry":[…]}}
    // 桌宠被点/搓时立即从这里挑一句显示（不经 LLM，零延迟）；整体替换
    if (cmd == "reactions")
    {
        const jsonlite::Value& ls = v.get("lines");
        int n = 0;

        if (!ls.isObj())
        {
            return head + "\"ok\":false,\"code\":\"bad_json\",\"msg\":\"需要 lines 对象\"}";
        }
        _reactLines.clear();
        for (size_t i = 0; i < ls.obj.size(); ++i)
        {
            const jsonlite::Value& arr = ls.obj[i].second;
            std::vector<std::string> v2;

            for (size_t k = 0; arr.isArr() && k < arr.arr.size() && v2.size() < 8; ++k)
            {
                const std::string t = arr.arr[k].strOr("");

                if (!t.empty() && t.size() <= 60)
                {
                    v2.push_back(t);
                }
            }
            if (!v2.empty())
            {
                n += (int)v2.size();
                _reactLines[ls.obj[i].first] = v2;
            }
        }
        printf("[pet] 本地台词库：%d 组 %d 句\n", (int)_reactLines.size(), n);
        fflush(stdout);
        return head + "\"ok\":true,\"type\":\"reactions\",\"groups\":" + std::to_string(_reactLines.size())
               + ",\"lines\":" + std::to_string(n) + "}";
    }
    // 调试：分区图（整个渲染目标按 step 像素采样，返回每格的区域名）——验证 16 区都能命中
    if (cmd == "zonemap")
    {
        const int step = (int)v.get("step").numOr(8.0) < 2 ? 2 : (int)v.get("step").numOr(8.0);
        csmFloat32 bb[4];

        if (!_modelBBoxValid)
        {
            return head + "\"ok\":false,\"code\":\"loading\",\"msg\":\"模型还没画出来\"}";
        }
        ComputeArtBBox(bb);
        std::string o = head + "\"ok\":true,\"type\":\"zonemap\",\"step\":" + std::to_string(step)
                        + ",\"w\":" + std::to_string(_viewW) + ",\"h\":" + std::to_string(_viewH) + ",\"rows\":[";

        for (int y = step / 2, r = 0; y < _viewH; y += step, ++r)
        {
            o += r ? ",\"" : "\"";
            for (int x = step / 2; x < _viewW; x += step)
            {
                const int z = ClassifyZone((csmFloat32)x, (csmFloat32)y, bb);

                o += z < 0 ? '.' : (char)('a' + z);   // a=head b=face … p=elbow.r（顺序同 zones）
            }
            o += "\"";
        }
        return o + "]}";
    }
    // 调试：模拟一次点击（走与真实点击完全相同的路径：区域反应 + click 事件）
    if (cmd == "simulate")
    {
        const char* zk = NULL;
        const csmFloat32 sx = (csmFloat32)v.get("x").numOr(0.0), sy = (csmFloat32)v.get("y").numOr(0.0);

        if (strcmp(v.get("kind").strOr("click"), "rub") == 0)   // 模拟一次搓揉
        {
            csmFloat32 bb[4];
            int z = Z_OUT;

            if (_modelBBoxValid)
            {
                ComputeArtBBox(bb);
                z = ClassifyZone(sx, sy, bb);
            }
            if (z >= 0)
            {
                char b[80];

                zk = ZoneKey(z);
                snprintf(b, sizeof(b), "{\"event\":\"rub\",\"zone\":\"%s\",\"count\":3}", zk);
                _petEvents.push_back(b);
                ZoneReact(z, 1, 0, sx, sy);
            }
        }
        else
        {
            _tapCooldown = 0.0f;
            HandleTapZ((csmInt32)sx, (csmInt32)sy, &zk);
            NotePetClick(zk);
        }
        std::string o = head + "\"ok\":true,\"type\":\"simulate\",\"zone\":";

        if (zk != NULL) JStr(o, zk);
        else o += "null";
        return o + "}";
    }
    if (cmd == "status")
    {
        std::string o = head + "\"ok\":true,\"type\":\"status\",\"model\":";

        JStr(o, ModelFv());
        o += ",\"playing\":";
        o += _petSeqs.empty() ? std::string("null") : _petSeqs.front().seq;
        o += ",\"queue\":" + std::to_string(_petSeqs.size())
             + ",\"bubbles\":" + std::to_string(_bubbleQueue.PendingCount())
             + ",\"talking\":" + std::string(IsTalking() ? "true" : "false") + ",\"fps\":" + JNum(fps);
        if (_isPet)
        {
            o += ",\"mood\":" + LifeMoodJson() + ",\"config\":" + CfgJson();
        }
        return o + "}";
    }
    // 以下都是控制：校验模型特征值
    const std::string mfv = v.get("model").strOr("");

    if (!mfv.empty() && mfv != ModelFv())
    {
        std::string o = head + "\"ok\":false,\"code\":\"model_mismatch\",\"msg\":\"模型已变，请重新 query\",\"model\":";

        JStr(o, ModelFv());
        return o + "}";
    }
    std::vector<std::string> warn;

    if (mfv.empty())
    {
        Warn(&warn, -1, "model", "建议带上 model（模型特征值），换模型时可防误操作");
    }
    const bool strict = v.get("strict").isBool() && v.get("strict").b;
    int accepted = 0;
    csmFloat32 dur = 0.0f;

    if (cmd == "reset")
    {
        _petSeqs.clear();
        PerformanceReset();
        ReleasePose();
        if (_port != NULL)
        {
            ClearBubbleDisplay();
        }
        _bubbleOptions.clear();
        _bubbleOptionDo.clear();
        accepted = 1;
    }
    else if (cmd == "sequence")
    {
        const jsonlite::Value& steps = v.get("steps");

        if (!steps.isArr() || steps.arr.empty())
        {
            return head + "\"ok\":false,\"code\":\"bad_json\",\"msg\":\"sequence 需要非空 steps 数组\"}";
        }
        PetSeq sq;

        sq.seq = seq;   // 原样的 JSON 记号（没带 seq 时为 null）
        sq.started = false;
        sq.begin = 0.0f;
        csmFloat32 last = 0.0f;

        for (size_t i = 0; i < steps.arr.size(); ++i)
        {
            PetStep st;

            st.at = (csmFloat32)steps.arr[i].get("at").numOr(0.0);
            st.v = steps.arr[i];
            st.fired = false;
            accepted += ExecPetStep(st.v, &warn, (int)i, true);
            last = st.at > last ? st.at : last;
            sq.steps.push_back(st);
        }
        sq.dur = (csmFloat32)v.get("dur").numOr(last + 1.0);
        if (sq.dur > 60.0f)
        {
            return head + "\"ok\":false,\"code\":\"too_long\",\"msg\":\"时间线最长 60 秒\"}";
        }
        if (strict && !warn.empty())
        {
            accepted = 0;
        }
        else
        {
            const std::string mode = v.get("mode").strOr("queue");

            if (mode == "replace")
            {
                _petSeqs.clear();
                StopActions();
            }
            if (_petSeqs.size() >= 4)
            {
                return head + "\"ok\":false,\"code\":\"queue_full\",\"msg\":\"队列已满（最多 4 段）\"}";
            }
            _petSeqs.push_back(sq);
        }
        dur = sq.dur;
    }
    else if (cmd == "expression" || cmd == "motion" || cmd == "action" || cmd == "pose"
             || cmd == "look" || cmd == "speak" || cmd == "emotion")
    {
        // 单项命令：{"cmd":"action","fv":"act:x"} ≡ 时间线里的一步 {"action":"act:x"}
        jsonlite::Value stepv = v;

        if (cmd == "emotion")
        {
            // {"cmd":"emotion","name":"happy","intensity":0.7}（name 也可写 fv / emotion）
            if (!v.has("emotion"))
            {
                jsonlite::Value ev = v;
                if (!v.has("name")) ev.obj.push_back(std::make_pair(std::string("name"), v.get("fv")));
                stepv.obj.push_back(std::make_pair(std::string("emotion"), ev));
            }
        }
        else if (cmd != "pose" && cmd != "look" && cmd != "speak")
        {
            jsonlite::Value fvv = v.get("fv");

            stepv.obj.push_back(std::make_pair(cmd, fvv));
        }
        else if (cmd == "pose" && !v.get("pose").isObj())
        {
            stepv.obj.push_back(std::make_pair(std::string("pose"), v));   // 字段直接写在顶层
        }
        else if (cmd == "look" && !v.has("look"))
        {
            stepv.obj.push_back(std::make_pair(std::string("look"), v.get("target")));
        }
        else if (cmd == "speak" && !v.has("speak"))
        {
            stepv.obj.push_back(std::make_pair(std::string("speak"), v.get("text")));
        }
        accepted = ExecPetStep(stepv, &warn, -1, true);
        if (!(strict && !warn.empty()))
        {
            ExecPetStep(stepv, NULL, -1, false);
        }
        else
        {
            accepted = 0;
        }
    }
    else
    {
        std::string o = head + "\"ok\":false,\"code\":\"unknown_cmd\",\"msg\":";

        JStr(o, "未知命令：" + cmd);
        return o + "}";
    }
    NoteExternalActivity();
    std::string o = head + "\"ok\":" + ((strict && !warn.empty()) ? "false" : "true")
                    + ",\"accepted\":" + std::to_string(accepted);
    if (dur > 0.0f)
    {
        o += ",\"dur\":" + JNum(dur);
    }
    o += ",\"warnings\":[";
    for (size_t i = 0; i < warn.size(); ++i)
    {
        o += (i ? "," : "") + warn[i];
    }
    return o + "]}";
}

/** 区域反应：kind = 0 点击 / 1 搓；n = 连点次数；(x,y) = 渲染目标坐标 */
void L2DPlayer::ZoneReact(int z, int kind, int n, csmFloat32 x, csmFloat32 y)
{
    if (z < 0 || z >= ZONE_COUNT)
    {
        return;
    }
    // 手动交互后的触摸冷却（MuteTouchResponse）：只有端口画在窗口上的点击/拖动特效照旧，
    // 这里一概不反应（不播动作/情绪、不出台词、不上报 LLM）——不然刚弹出的交互气泡
    // （猜拳、讲笑话、提问…）会被"摸摸头"的反应气泡顶掉。
    if (TouchMuted())
    {
        printf("[pet] 交互冷却中（还剩 %.1fs）：这次触摸只给特效、不响应\n", _touchMuteUntil - _perfClock);
        fflush(stdout);
        return;
    }
    // 内在状态先接手：睡着/犯困/生闷气/正享受抚摸时，反应完全不同（不是每次都同一套）
    if (LifeTouch(z, kind, n, x, y))
    {
        return;
    }
    // 先"看过去"：注意到你点的地方（短暂压过鼠标跟随）
    SetLookTargetPixels(x, y);
    _lookOverrideUntil = _perfClock + 1.2f;

    struct R { const char* action; const char* gesture; const char* emo; };
    static const R kClick[ZONE_COUNT] = {
        { "shy", NULL, "害羞" },             // 头：摸头 → 害羞
        { "shy", NULL, "害羞" },             // 脸：戳脸 → 脸红
        { "laugh", NULL, "开心" },           // 颈：怕痒
        { "angry", NULL, "生气" },           // 胸：不许乱摸
        { "laugh", NULL, "开心" },           // 肚子：痒
        { "shake_no", NULL, "生气" },        // 胯：生气摇头
        { "surprise_big", NULL, "惊讶" },    // 左大腿
        { "surprise_big", NULL, "惊讶" },    // 右大腿
        { NULL, "tilt", "惊讶" },            // 左小腿：歪头看
        { NULL, "tilt", "惊讶" },            // 右小腿
        { "hop", NULL, "开心" },             // 左脚：踮脚
        { "hop", NULL, "开心" },             // 右脚
        { "nod_yes", NULL, "开心" },         // 左手：牵手 → 点头
        { "nod_yes", NULL, "开心" },         // 右手
        { NULL, "tilt", NULL },              // 左手肘
        { NULL, "tilt", NULL },              // 右手肘
    };
    static const R kRub[ZONE_COUNT] = {
        { "shy", NULL, "害羞" },             // 摸头
        { "shy", NULL, "害羞" },             // 摸脸
        { "laugh", NULL, "开心" },           // 挠脖子
        { "angry", NULL, "生气" },
        { "laugh", NULL, "开心" },           // 挠肚子
        { "angry", NULL, "生气" },
        { "shy", NULL, "害羞" }, { "shy", NULL, "害羞" },
        { "laugh", NULL, "开心" }, { "laugh", NULL, "开心" },
        { "laugh", NULL, "开心" }, { "laugh", NULL, "开心" },   // 挠脚心
        { "shy", NULL, "开心" }, { "shy", NULL, "开心" },
        { NULL, "tilt", NULL }, { NULL, "tilt", NULL },
    };
    R r = (kind == 1) ? kRub[z] : kClick[z];

    if (kind == 0 && n >= 3)
    {
        r.action = "angry";   // 同一处连点 ≥3 次：烦了
        r.gesture = NULL;
        r.emo = "生气";
    }
    // profile 覆盖
    std::string pa, pg, pe;
    const std::string key = std::string(kind == 1 ? "rub." : "") + ZoneKey(z);
    std::map<std::string, std::vector<std::string> >::const_iterator it = _zoneReactions.find(key);

    if (it != _zoneReactions.end())
    {
        pa = it->second[0];
        pg = it->second[1];
        pe = it->second[2];
        r.action = pa.empty() ? NULL : pa.c_str();
        r.gesture = pg.empty() ? NULL : pg.c_str();
        r.emo = pe.empty() ? NULL : pe.c_str();
    }
    bool did = false;

    if (r.action != NULL && FindAction(r.action) != NULL)
    {
        did = StartAction(r.action, 1.2f, 0.8f, 1);
    }
    if (!did && r.gesture != NULL)
    {
        did = StartGesture(r.gesture, 1.1f, 1, 0.8f);
    }
    if (r.emo != NULL)
    {
        // 情绪推一下（强度随连点/搓揉）；表情由情绪引擎连续生成，峰值时借模型表情，之后自然回落
        EmotionNudge(r.emo, kind == 1 ? 0.55f : (n >= 3 ? 0.95f : 0.7f));
    }
    // 即时台词（本地台词库，不经 LLM）：同区连点 ≥3 次用"生气"那组
    std::string line = ApplyCall(PickReactLine(z, kind, kind == 0 && n >= 3));
    // 亲密度很低（< 0.25，关系"陌生"）：多半是冷淡的回应——让"掉亲密度"真的看得出来
    if (_lifeAffection < 0.25f && (NextRand() % 100u) < 65u)
    {
        static const char* const kCold[] = { "……嗯。", "别碰我。", "哦。", "……你又想干嘛。", "我们不太熟吧。", "……" };
        line = kCold[NextRand() % 6u];
        EmotionNudge("calm", 0.5f);
    }

    // 内置兜底台词按语言翻译（LLM 下发的台词库不在表里 → 原样通过）
    line = peti18n::Tr(line);
    if (_cfgLines && !line.empty() && _port != NULL && !_port->BubbleAwaitingChoice()
        && _perfClock - _lastReactLineAt > 1.5f)
    {
        _lastReactLineAt = _perfClock;
        ShowPetBubble(line, {}, {}, {}, 2.6f);   // 口型在 PresentBubble 上屏时起（排队时不预动嘴）
        FeedPush("say", line);
    }
    printf("[pet] %s%s → %s%s%s「%s」\n", TouchVerb(ZoneKey(z), kind == 1), ZoneLabel(z),
           r.action ? r.action : (r.gesture ? r.gesture : "看一眼"), r.emo ? " + " : "", r.emo ? r.emo : "",
           line.c_str());
    fflush(stdout);
}

/** 本地台词库取一句（避免连着重复）：键 = "<区>.click" / "<区>.rub" / "angry"；没有就用内置兜底 */
std::string L2DPlayer::PickReactLine(int z, int kind, bool angry)
{
    std::vector<std::string> cands;
    const std::string zk = ZoneKey(z);
    const char* const kinds[2] = { ".click", ".rub" };
    std::map<std::string, std::vector<std::string> >::const_iterator it;

    if (angry && (it = _reactLines.find("angry")) != _reactLines.end()) cands = it->second;
    if (cands.empty() && (it = _reactLines.find(zk + kinds[kind])) != _reactLines.end()) cands = it->second;
    if (cands.empty() && kind == 1 && (it = _reactLines.find(zk + ".click")) != _reactLines.end()) cands = it->second;
    if (cands.empty())
    {
        // 内置兜底（台词库没下发时）：按部位
        static const char* const kAngry[] = { "够了够了！", "再戳我就不理你了！", "哼！" };
        static const char* const kLines[ZONE_COUNT][2][2] = {
            { { "嗯？戳我头干嘛～", "头、头不能乱戳啦" }, { "好舒服……", "呼噜呼噜～" } },
            { { "别捏脸啦！", "脸、脸要被捏扁了……" }, { "好痒……", "唔，脸被揉扁了" } },
            { { "呀！脖子怕痒！", "别碰脖子嘛" }, { "哈哈哈别挠了！", "痒、痒死了！" } },
            { { "喂！往哪儿戳呢！", "不许乱戳！" }, { "再这样我生气了！", "住手啦！" } },
            { { "戳肚子会痒的啦", "咕……肚子饿了吗" }, { "哈哈哈好痒！", "肚子不行！" } },
            { { "那里不行！", "变、变态！" }, { "住、住手！", "我要生气了！" } },
            { { "干嘛戳腿呀？", "腿也要管吗～" }, { "痒……", "别揉啦" } },
            { { "干嘛戳腿呀？", "腿也要管吗～" }, { "痒……", "别揉啦" } },
            { { "小腿也要戳吗？", "嗯？" }, { "好痒～", "别闹啦" } },
            { { "小腿也要戳吗？", "嗯？" }, { "好痒～", "别闹啦" } },
            { { "脚、脚不能碰！", "踩到你啦～" }, { "脚心好痒哈哈哈", "哈哈哈不行！" } },
            { { "脚、脚不能碰！", "踩到你啦～" }, { "脚心好痒哈哈哈", "哈哈哈不行！" } },
            { { "要牵手吗？", "手给你～" }, { "手被摸得暖暖的……", "嘿嘿，痒痒的" } },
            { { "要牵手吗？", "手给你～" }, { "手被摸得暖暖的……", "嘿嘿，痒痒的" } },
            { { "手肘麻了一下！", "嗯？" }, { "麻麻的……", "别闹～" } },
            { { "手肘麻了一下！", "嗯？" }, { "麻麻的……", "别闹～" } },
        };
        if (angry)
        {
            cands.assign(kAngry, kAngry + 3);
        }
        else
        {
            cands.push_back(kLines[z][kind][0]);
            cands.push_back(kLines[z][kind][1]);
        }
    }
    if (cands.empty())
    {
        return std::string();
    }
    size_t k = NextRand() % cands.size();

    if (cands.size() > 1 && cands[k] == _lastReactLine)
    {
        k = (k + 1) % cands.size();
    }
    _lastReactLine = cands[k];
    return cands[k];
}

/** 触发一个脚本步骤（sceneRemain=本段脚本剩余秒数，给"姿态保持到段尾"用）*/
void L2DPlayer::FireStep(const SceneStep& st, csmFloat32 sceneRemain)
{
    switch (st.kind)
    {
    case 0:
        StartGesture(st.s.c_str(), st.speed, st.repeat, st.scale);
        break;
    case 1:
        if (!StartExpressionByName(st.s.c_str()))
        {
            fprintf(stderr, "[scene] 表情不存在：%s\n", st.s.c_str());
        }
        break;
    case 2:
        SetLookTargetPixels(st.fa, st.fb);
        break;
    case 3:
        if (!StartMotionByGroupIndex(st.a, st.b))
        {
            fprintf(stderr, "[scene] 动作不存在：组%d 序号%d\n", st.a, st.b);
        }
        break;
    case 4:
        ClearExpression();
        break;
    case 5:
        if (!StartExpression(st.a))
        {
            fprintf(stderr, "[scene] 表情序号越界：%d\n", st.a);
        }
        break;
    case 6:
    {
        // 参数姿态：dur 内缓动到位，然后**保持到本段脚本结束**（轨道活到段尾）
        const csmFloat32 remain = sceneRemain > 0.1f ? sceneRemain : 0.1f;
        csmFloat32 ramp = st.dur > 0.05f ? st.dur : 1.0f;

        if (ramp > remain)
        {
            ramp = remain;
        }
        for (size_t i = 0; i < st.pose.size(); ++i)
        {
            if (_live.size() >= kMaxLiveTracks)
            {
                break;
            }
            SceneTrack tr;

            memset(&tr, 0, sizeof(tr));
            tr.index = st.pose[i].first;
            tr.additive = true;   // 增量语义（与视线/手势一致）
            tr.start = _perfClock;
            tr.dur = remain;
            tr.scale = 1.0f;
            tr.keyCount = 2;
            tr.keys[0].t = 0.0f;
            tr.keys[0].v = 0.0f;
            tr.keys[1].t = ramp / remain;
            tr.keys[1].v = st.pose[i].second;
            _live.push_back(tr);
        }
        printf("[scene] 参数姿态：%d 项（%.1fs 到位，保持到段尾）\n", (int)st.pose.size(), ramp);
        fflush(stdout);
        break;
    }
    case 7:
        // 说话：dur 秒（无 dur 时按文本估），口型叠在表情姿态上
        StartTalk(st.dur > 0.05f ? st.dur : EstimateTalkDur(st.s.c_str()), st.s.c_str());
        break;
    case 8:
        if (!StartAction(st.s.c_str(), st.speed, st.scale, st.repeat))
        {
            fprintf(stderr, "[scene] 动作不存在：%s\n", st.s.c_str());
        }
        break;
    case 9:
        // 躯体姿态：dur≈到位时间；保持到被改或本段演出结束（段尾自动弹回）
        for (size_t i = 0; i < st.body.size(); ++i)
        {
            SetBodyChannel(st.body[i].first.c_str(), st.body[i].second, st.dur, true);
        }
        break;
    case 10:
        ReleaseSceneBody();
        break;
    default:
        break;
    }
}

/** 解析一个"场景"对象（脚本 JSON 的一个元素）*/
bool L2DPlayer::ParseScene(const jsonlite::Value& v, char* errbuf, size_t errcap)
{
    if (!v.isObj())
    {
        snprintf(errbuf, errcap, "场景不是对象");
        return false;
    }
    // 防串扰：场景可声明 "fp"/"model"，对不上整段拒绝（给 A 编的脚本不落到 B 上）
    const char* wantFp = v.get("fp").strOr("");

    if (*wantFp != '\0' && strcmp(wantFp, FingerprintText().c_str()) != 0)
    {
        snprintf(errbuf, errcap, "指纹不匹配（脚本 %s ≠ 本机 %s）", wantFp,
                 FingerprintText().c_str());
        return false;
    }
    const char* wantModel = v.get("model").strOr("");

    if (*wantModel != '\0' && strcmp(wantModel, BaseName(_dir).c_str()) != 0)
    {
        snprintf(errbuf, errcap, "模型名不匹配（脚本 %s ≠ 本机 %s）", wantModel,
                 BaseName(_dir).c_str());
        return false;
    }

    Scene sc;
    sc.dur = (csmFloat32)v.get("dur").numOr(20.0);
    if (sc.dur < 1.0f) sc.dur = 1.0f;
    if (sc.dur > 600.0f) sc.dur = 600.0f;
    const jsonlite::Value& iv = v.get("interrupt");
    sc.interrupt = iv.isBool() ? iv.b : true;
    sc.idleLevel = _idleLevel;
    const jsonlite::Value& idv = v.get("idle");
    if (idv.isNum())
    {
        const csmInt32 lv = (csmInt32)idv.num;
        sc.idleLevel = lv < 0 ? 0 : (lv > 3 ? 3 : lv);
    }
    sc.beginAt = _perfClock;

    const jsonlite::Value& steps = v.get("steps");
    if (steps.isArr())
    {
        for (size_t i = 0; i < steps.arr.size(); ++i)
        {
            const jsonlite::Value& s = steps.arr[i];
            if (!s.isObj())
            {
                continue;
            }
            SceneStep st;
            st.at = (csmFloat32)s.get("at").numOr(0.0);
            if (st.at < 0.0f) st.at = 0.0f;
            st.speed = (csmFloat32)s.get("speed").numOr(1.0);
            st.dur = (csmFloat32)s.get("dur").numOr(1.0);
            st.repeat = (csmInt32)s.get("repeat").numOr(1.0);
            // 手势幅度缩放（"动作大一点"的旋钮；模型自己绑的参数幅度偏小时放大用，
            // 上限 2 倍——超过就是把作者绑的摆幅推到参数限幅，没必要更大）
            st.scale = (csmFloat32)s.get("scale").numOr(1.0);
            if (st.scale < 0.1f) st.scale = 0.1f;
            if (st.scale > 2.0f) st.scale = 2.0f;
            st.fired = false;
            st.a = st.b = 0;
            st.fa = st.fb = 0.0f;
            st.kind = -1;

            const jsonlite::Value& gv = s.get("gesture");
            const jsonlite::Value& mv = s.get("motion");
            const jsonlite::Value& lv = s.get("look");
            if (gv.isStr())
            {
                st.kind = 0;
                st.s = gv.str;
            }
            else if (mv.isStr())
            {
                st.kind = 3;
                st.s = mv.str;
                // "组名 序号"（序号可省）
                const size_t sp = st.s.find_last_of(' ');
                if (sp != std::string::npos && sp + 1 < st.s.size()
                    && st.s[sp + 1] >= '0' && st.s[sp + 1] <= '9')
                {
                    st.b = atoi(st.s.c_str() + sp + 1);
                    st.s.resize(sp);
                }
                st.a = -1;
                for (csmInt32 g = 0; g < MotionGroupCount(); ++g)
                {
                    if (st.s == MotionGroupName(g))
                    {
                        st.a = g;
                        break;
                    }
                }
            }
            else if (lv.isArr() && lv.arr.size() >= 2)
            {
                st.kind = 2;
                st.fa = (csmFloat32)lv.arr[0].numOr(240.0);
                st.fb = (csmFloat32)lv.arr[1].numOr(400.0);
            }
            else if (s.has("talk"))
            {
                // 说话：{"talk":8} 秒数 / {"talk":"你好呀"} 文本 / {"talk":{"dur":8,"text":"…"}}
                const jsonlite::Value& tv = s.get("talk");
                st.kind = 7;
                if (tv.isNum())
                {
                    st.dur = (csmFloat32)tv.num;
                }
                else if (tv.isStr())
                {
                    st.s = tv.str;
                    st.dur = (csmFloat32)s.get("dur").numOr(0.0);   // 0 → 按文本估
                }
                else if (tv.isObj())
                {
                    st.dur = (csmFloat32)tv.get("dur").numOr(0.0);
                    st.s = tv.get("text").strOr("");
                }
                else
                {
                    st.dur = 3.0f;
                }
            }
            else if (s.get("params").isObj())
            {
                // 参数姿态（增量）：{ "params": { "ParamarmR1": -0.7, ... }, "dur": 1.2 }
                const jsonlite::Value& pose = s.get("params");

                st.dur = (csmFloat32)s.get("dur").numOr(1.0);
                for (size_t pi = 0; pi < pose.obj.size(); ++pi)
                {
                    if (!pose.obj[pi].second.isNum())
                    {
                        continue;
                    }
                    const csmInt32 idx = _model->GetParameterIndex(
                        CubismFramework::GetIdManager()->GetId(pose.obj[pi].first.c_str()));

                    if (idx < 0)
                    {
                        fprintf(stderr, "[scene] 参数不存在：%s\n", pose.obj[pi].first.c_str());
                        continue;
                    }
                    st.pose.push_back(std::make_pair(
                        idx, (csmFloat32)pose.obj[pi].second.num));
                }
                if (!st.pose.empty())
                {
                    st.kind = 6;
                }
            }
            else if (s.get("action").isStr())
            {
                // 动作片段：{"action":"cheer","speed":1,"scale":1,"repeat":1}
                st.kind = 8;
                st.s = s.get("action").str;
                if (FindAction(st.s.c_str()) == NULL)
                {
                    fprintf(stderr, "[scene] 动作不存在：%s（LIST 查 ACTIONS=）\n", st.s.c_str());
                    st.kind = -1;
                }
            }
            else if (s.has("body"))
            {
                // 躯体姿态：{"body":{"arm.r":0.8,"head.z":10},"dur":0.4} ／ {"body":"reset"}
                const jsonlite::Value& bv = s.get("body");

                st.dur = (csmFloat32)s.get("dur").numOr(0.0);   // 0 = 通道默认速度
                if (bv.isObj())
                {
                    for (size_t bi = 0; bi < bv.obj.size(); ++bi)
                    {
                        if (!bv.obj[bi].second.isNum())
                        {
                            continue;
                        }
                        if (!HasBodyChannel(bv.obj[bi].first.c_str()))
                        {
                            fprintf(stderr, "[scene] 躯体通道不认（跳过）：%s\n",
                                    bv.obj[bi].first.c_str());
                            continue;
                        }
                        st.body.push_back(std::make_pair(bv.obj[bi].first,
                                                         (csmFloat32)bv.obj[bi].second.num));
                    }
                    if (!st.body.empty())
                    {
                        st.kind = 9;
                    }
                }
                else
                {
                    st.kind = 10;
                }
            }
            else if (s.has("expr"))
            {
                // expr：字符串=名字；数字=序号；显式 null=回默认
                const jsonlite::Value& ex = s.get("expr");
                if (ex.isStr())
                {
                    st.kind = 1;
                    st.s = ex.str;
                }
                else if (ex.isNum())
                {
                    st.kind = 5;
                    st.a = (csmInt32)ex.num;
                }
                else
                {
                    st.kind = 4; // null / 其它 → 回默认
                }
            }
            if (st.kind >= 0)
            {
                sc.steps.push_back(st);
            }
        }
    }
    if (_scenes.empty() && _live.empty())
    {
        sc.beginAt = _perfClock; // 立即开演
    }
    _scenes.push_back(sc);
    printf("[scene] 入队：dur=%.1fs steps=%d idle=%d\n", sc.dur, (int)sc.steps.size(), (int)sc.idleLevel);
    fflush(stdout);
    return true;
}

/** 读 cdi3.json（model3 的 DisplayInfo）：参数中文名 + 分组，供能力表/参数清单用 */
void L2DPlayer::LoadModelDisplayInfo()
{
    _paramName.assign((size_t)_model->GetParameterCount(), std::string());
    _groupOf.assign((size_t)_model->GetParameterCount(), -1);
    _groupNames.clear();
    _groupOrder.clear();
    if (_setting == NULL || _setting->GetDisplayInfoFileName() == NULL
        || _setting->GetDisplayInfoFileName()[0] == '\0')
    {
        return;
    }
    csmSizeInt size = 0;
    csmByte* buffer = ReadFile((_dir + "/" + _setting->GetDisplayInfoFileName()).c_str(), &size);

    if (buffer == NULL)
    {
        return;
    }
    jsonlite::Value root;
    std::string perr;

    if (!jsonlite::parse((const char*)buffer, (size_t)size, root, &perr))
    {
        fprintf(stderr, "[scene] cdi3 解析失败：%s\n", perr.c_str());
        free(buffer);
        return;
    }
    free(buffer);

    // 部件显示名（能力上报/骨架识别用："右腕B"、"左腕C"…）
    _partName.clear();
    {
        const jsonlite::Value& parts = root.get("Parts");

        for (size_t i = 0; parts.isArr() && i < parts.arr.size(); ++i)
        {
            const char* pid = parts.arr[i].get("Id").strOr("");

            if (*pid != '\0')
            {
                _partName[pid] = parts.arr[i].get("Name").strOr("");
            }
        }
    }

    std::vector<std::pair<std::string, std::string> > groups; // (Id, Name) 按文件顺序
    const jsonlite::Value& gs = root.get("ParameterGroups");

    if (gs.isArr())
    {
        for (size_t i = 0; i < gs.arr.size(); ++i)
        {
            if (gs.arr[i].isObj())
            {
                groups.push_back(std::make_pair(std::string(gs.arr[i].get("Id").strOr("")),
                                                std::string(gs.arr[i].get("Name").strOr(""))));
            }
        }
    }
    const jsonlite::Value& ps = root.get("Parameters");

    if (ps.isArr())
    {
        for (size_t i = 0; i < ps.arr.size(); ++i)
        {
            const jsonlite::Value& p = ps.arr[i];

            if (!p.isObj())
            {
                continue;
            }
            const char* id = p.get("Id").strOr("");

            if (id[0] == '\0')
            {
                continue;
            }
            const csmInt32 idx = _model->GetParameterIndex(
                CubismFramework::GetIdManager()->GetId(id));

            if (idx < 0)
            {
                continue; // cdi3 里有、模型没有：忽略
            }
            _paramName[(size_t)idx] = p.get("Name").strOr("");
            const std::string gid = p.get("GroupId").strOr("");

            for (size_t g = 0; g < groups.size(); ++g)
            {
                if (groups[g].first == gid)
                {
                    // 组名去重入表（保留文件顺序）
                    csmInt32 slot = -1;

                    for (size_t k = 0; k < _groupNames.size(); ++k)
                    {
                        if (_groupNames[k] == groups[g].second)
                        {
                            slot = (csmInt32)k;
                            break;
                        }
                    }
                    if (slot < 0)
                    {
                        slot = (csmInt32)_groupNames.size();
                        _groupNames.push_back(groups[g].second);
                    }
                    _groupOf[(size_t)idx] = slot;
                    break;
                }
            }
        }
    }
    // 组顺序：按"首次被参数引用"的顺序（基本等同 cdi3 文件顺序）
    for (size_t i = 0; i < _groupNames.size(); ++i)
    {
        _groupOrder.push_back((int)i);
    }
    printf("[scene] cdi3：%d 个参数带名字/分组，%d 个组\n",
           (int)ps.arr.size(), (int)_groupNames.size());
    fflush(stdout);
}

/** 内置通用手势（只用标准参数；模型缺哪个参数自动跳过）*/
void L2DPlayer::BuildBuiltinGestures()
{
    auto find = [this](const char* name) -> GestureDef* {
        for (size_t i = 0; i < _gestures.size(); ++i)
        {
            if (_gestures[i].name == name)
            {
                return &_gestures[i]; // profile 已定义 → 不覆盖
            }
        }
        return NULL;
    };

    if (find("nod") == NULL)
    {
        GestureDef g; g.name = "nod"; g.dur = 1.5f;
        const BuiltinKey k[] = { {0,0},{0.35f,12},{0.7f,3},{1,0} };
        AddBuiltinTrack(g, "ParamAngleX", true, k, 4);
        _gestures.push_back(g);
    }
    if (find("shake") == NULL)
    {
        GestureDef g; g.name = "shake"; g.dur = 1.8f;
        const BuiltinKey k[] = { {0,0},{0.25f,-9},{0.6f,9},{1,0} };
        AddBuiltinTrack(g, "ParamAngleZ", true, k, 4);
        _gestures.push_back(g);
    }
    if (find("tilt") == NULL)
    {
        GestureDef g; g.name = "tilt"; g.dur = 2.4f;
        const BuiltinKey k[] = { {0,0},{0.3f,-13},{0.8f,-13},{1,0} };
        AddBuiltinTrack(g, "ParamAngleZ", true, k, 4);
        _gestures.push_back(g);
    }
    if (find("surprise") == NULL)
    {
        GestureDef g; g.name = "surprise"; g.dur = 1.2f;
        const BuiltinKey k1[] = { {0,0},{0.15f,12},{0.6f,8},{1,0} };
        AddBuiltinTrack(g, "ParamAngleX", true, k1, 4);
        const BuiltinKey k2[] = { {0,0},{0.2f,3},{0.6f,2},{1,0} };
        AddBuiltinTrack(g, "ParamBodyAngleX", true, k2, 4);
        _gestures.push_back(g);
    }
    if (find("sigh") == NULL)
    {
        GestureDef g; g.name = "sigh"; g.dur = 2.6f;
        const BuiltinKey k[] = { {0,0},{0.3f,-9},{0.75f,-9},{1,0} };
        AddBuiltinTrack(g, "ParamAngleX", true, k, 4);
        _gestures.push_back(g);
    }
    if (find("think") == NULL)
    {
        GestureDef g; g.name = "think"; g.dur = 3.0f;
        const BuiltinKey k1[] = { {0,0},{0.3f,-10},{0.85f,-10},{1,0} };
        AddBuiltinTrack(g, "ParamAngleZ", true, k1, 4);
        const BuiltinKey k2[] = { {0,0},{0.3f,7},{0.85f,7},{1,0} };
        AddBuiltinTrack(g, "ParamAngleY", true, k2, 4);
        _gestures.push_back(g);
    }
}

/** 读模型目录下的 l2d.profile.json（手势定义 + 表情别名；可选）*/
void L2DPlayer::LoadGestureProfile()
{
    if (_dir.empty())
    {
        return;
    }
    /* 查找顺序：模型目录里的 l2d.profile.json（单模型手工覆盖）
     *           → L2D_PROFILE_DIR/<模型目录名>.profile.json（部署的稳定路径） */
    std::string path = _dir + "/l2d.profile.json";
    csmSizeInt size = 0;
    csmByte* buffer = NULL;

    if (plat::Exists(path))   // 可选文件：先判存在，避免无谓报错
    {
        buffer = ReadFile(path.c_str(), &size);
    }
    if (buffer == NULL && L2D_PROFILE_DIR[0] != '\0')
    {
        path = std::string(L2D_PROFILE_DIR) + "/" + BaseName(_dir) + ".profile.json";
        if (plat::Exists(path))
        {
            buffer = ReadFile(path.c_str(), &size);
        }
    }
    if (buffer == NULL)
    {
        return; // 都没有：不带 profile 跑（内置手势仍然可用）
    }
    jsonlite::Value root;
    std::string perr;

    if (!jsonlite::parse((const char*)buffer, (size_t)size, root, &perr))
    {
        fprintf(stderr, "[scene] profile 解析失败：%s（%s）\n", path.c_str(), perr.c_str());
        free(buffer);
        return;
    }
    free(buffer);

    const jsonlite::Value& aliases = root.get("exprAlias");
    if (aliases.isObj())
    {
        for (size_t i = 0; i < aliases.obj.size(); ++i)
        {
            if (aliases.obj[i].second.isStr())
            {
                _exprAliases.push_back(std::make_pair(aliases.obj[i].first,
                                                       aliases.obj[i].second.str));
            }
        }
    }
    const jsonlite::Value& rx = root.get("reactions");
    if (rx.isObj())
    {
        static const char* const kZones[3] = { "head", "body", "any" };

        for (int zi = 0; zi < 3; ++zi)
        {
            const jsonlite::Value& zv = rx.get(kZones[zi]);

            if (!zv.isObj())
            {
                continue;
            }
            _reactions[zi].gesture = zv.get("gesture").strOr("");
            _reactions[zi].expr = zv.get("expr").strOr("");
            _hasReaction[zi] = (!_reactions[zi].gesture.empty() || !_reactions[zi].expr.empty());
        }
    }
    // "zoneReactions"：{"head":{"action":"shy","gesture":"","expr":"害羞"}, "rub.belly":{…}}（覆盖内置区域反应）
    const jsonlite::Value& zr = root.get("zoneReactions");
    for (size_t zi = 0; zr.isObj() && zi < zr.obj.size(); ++zi)
    {
        const jsonlite::Value& zv = zr.obj[zi].second;
        std::vector<std::string> v3;

        v3.push_back(zv.get("action").strOr(""));
        v3.push_back(zv.get("gesture").strOr(""));
        v3.push_back(zv.get("expr").strOr(""));
        _zoneReactions[zr.obj[zi].first] = v3;
    }
    const jsonlite::Value& gs = root.get("gestures");
    if (gs.isObj())
    {
        for (size_t gi = 0; gi < gs.obj.size(); ++gi)
        {
            const std::string& gname = gs.obj[gi].first;
            const jsonlite::Value& gv = gs.obj[gi].second;

            if (!gv.isObj())
            {
                continue;
            }
            GestureDef g;
            g.name = gname;
            g.dur = (csmFloat32)gv.get("dur").numOr(2.0);
            if (g.dur < 0.2f) g.dur = 0.2f;
            g.expr = gv.get("expr").strOr("");   // 可选：手势自带表情（姿态类手势）
            const jsonlite::Value& tracks = gv.get("tracks");
            if (tracks.isArr())
            {
                for (size_t t = 0; t < tracks.arr.size(); ++t)
                {
                    const jsonlite::Value& tv = tracks.arr[t];
                    const jsonlite::Value& pv = tv.get("param");

                    if (!tv.isObj() || !pv.isStr())
                    {
                        continue;
                    }
                    SceneTrack tr;
                    memset(&tr, 0, sizeof(tr));
                    tr.index = _model->GetParameterIndex(
                        CubismFramework::GetIdManager()->GetId(pv.str.c_str()));
                    if (tr.index < 0)
                    {
                        fprintf(stderr, "[scene] profile 参数不存在：%s（手势 %s）\n",
                                pv.str.c_str(), g.name.c_str());
                        continue;
                    }
                    tr.additive = !(std::string(tv.get("mode").strOr("")) == "set");
                    tr.start = 0.0f;
                    tr.dur = g.dur;
                    tr.scale = 1.0f;
                    const jsonlite::Value& keys = tv.get("keys");
                    if (keys.isArr())
                    {
                        for (size_t k = 0; k < keys.arr.size() && tr.keyCount < 8; ++k)
                        {
                            if (keys.arr[k].isArr() && keys.arr[k].arr.size() >= 2)
                            {
                                tr.keys[tr.keyCount].t = (csmFloat32)keys.arr[k].arr[0].numOr(0.0);
                                tr.keys[tr.keyCount].v = (csmFloat32)keys.arr[k].arr[1].numOr(0.0);
                                ++tr.keyCount;
                            }
                        }
                    }
                    if (tr.keyCount > 0)
                    {
                        g.tracks.push_back(tr);
                    }
                }
            }
            if (!g.tracks.empty())
            {
                // 同名覆盖：profile 优先于内置
                for (size_t k = 0; k < _gestures.size(); ++k)
                {
                    if (_gestures[k].name == g.name)
                    {
                        _gestures.erase(_gestures.begin() + (ptrdiff_t)k);
                        break;
                    }
                }
                _gestures.push_back(g);
            }
        }
    }
    // "actions"：模型专属动作片段（通道空间关键帧；同名覆盖内置）
    //   { "名": { "dur":2, "desc":"…", "expr":"表情", "tracks":[{"ch":"arm.r","keys":[[0,0],[0.3,0.9],[1,0]]}] } }
    const jsonlite::Value& as = root.get("actions");
    if (as.isObj())
    {
        for (size_t ai = 0; ai < as.obj.size(); ++ai)
        {
            const jsonlite::Value& av = as.obj[ai].second;

            if (!av.isObj() || !av.get("tracks").isArr())
            {
                continue;
            }
            ActionDef a;

            a.name = as.obj[ai].first;
            a.desc = av.get("desc").strOr("");
            a.expr = av.get("expr").strOr("");
            a.dur = (csmFloat32)av.get("dur").numOr(2.0);
            if (a.dur < 0.2f) a.dur = 0.2f;
            const jsonlite::Value& tracks = av.get("tracks");

            for (size_t t = 0; t < tracks.arr.size(); ++t)
            {
                const jsonlite::Value& tv = tracks.arr[t];
                const jsonlite::Value& keys = tv.get("keys");
                ActionTrack tr;

                tr.ch = tv.get("ch").strOr("");
                tr.anchor = tv.get("anchor").strOr("");   // hand.r + anchor = IK 窗口
                if (tr.ch.empty() || !keys.isArr())
                {
                    continue;
                }
                for (size_t k = 0; k < keys.arr.size(); ++k)
                {
                    if (keys.arr[k].isArr() && keys.arr[k].arr.size() >= 2)
                    {
                        tr.keys.push_back(std::make_pair((csmFloat32)keys.arr[k].arr[0].numOr(0.0),
                                                         (csmFloat32)keys.arr[k].arr[1].numOr(0.0)));
                    }
                }
                if (!tr.keys.empty())
                {
                    a.tracks.push_back(tr);
                }
            }
            if (!a.tracks.empty())
            {
                a.fromProfile = true;
                _actions.push_back(a);
            }
        }
    }
    printf("[scene] profile：%d 条手势、%d 条动作、%d 条表情别名（%s）\n",
           (int)_gestures.size(), (int)_actions.size(), (int)_exprAliases.size(), path.c_str());
    fflush(stdout);
}

/** 上次记忆压缩时间（读当前模型 habits.json 的 compressT；10 秒缓存，别在状态推送上反复读盘） */
std::string L2DPlayer::MemoryCompressedText()
{
    static double cachedAt = -1e9;
    static std::string cached;
    const double now = plat::MonoNow();
    if (now - cachedAt < 10.0) return cached;
    cachedAt = now;
    cached.clear();
    std::string fp = FingerprintText();
    if (fp.compare(0, 6, "model:") == 0) fp = fp.substr(6);
    std::string id;
    for (size_t i = 0; i < fp.size() && id.size() < 64; ++i)
    {
        const unsigned char c = (unsigned char)fp[i];
        const bool ok = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')
                        || c == '_' || c == '-';
        id += ok ? fp[i] : '_';
    }
    if (id.empty()) return cached;
    const std::string path = plat::HomeDir() + "/.config/l2dpet/memory/chars/" + id + ".habits.json";
    const std::string text = plat::ReadTextFile(path);
    if (text.empty()) return cached;
    jsonlite::Value v;
    if (jsonlite::parse(text.c_str(), text.size(), v, NULL) && v.isObj())
    {
        const double t = v.get("compressT").numOr(0.0);
        if (t > 0.0)
        {
            char b[32];
            time_t tt = (time_t)t;
            struct tm lt;
#ifdef _WIN32
            localtime_s(&lt, &tt);
#else
            localtime_r(&tt, &lt);
#endif
            strftime(b, sizeof(b), "%m-%d %H:%M", &lt);
            cached = b;
        }
    }
    return cached;
}

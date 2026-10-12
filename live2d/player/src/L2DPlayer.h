/**
 * L2DPlayer.h — 播放器/桌宠主体（Cubism 宿主 + 协议 + 内在状态 + 演出引擎）。
 *
 * 从 l2dtest.cpp 拆出（纯搬移，逻辑未改）。本文件只有**类声明 + 数据成员 + 小内联访问器**；
 * 大函数（>= 40 行）的类外定义按职责分在这些文件里：
 *   PlayerCore.cpp   装载/重建/绘制/端口接线/指纹
 *   PlayerCmd.cpp    远程指令 / 演出引擎 / 说话 / 动作片段 / 协议 / 交互区域 / 手势
 *   PlayerBody.cpp   躯体通道 / 关节保持 / 换套过桥 / 手臂关节骨架
 *   PlayerCfg.cpp    配置读写 / LLM 接入
 *   PlayerLife.cpp   连续情绪 / 内在状态（生活逻辑）
 *   PlayerProbe.cpp  能力探测（PROBE）
 */
#pragma once

#include "SysStats.h"
#include "FileUtil.h"
#include "ModelImport.h"
#include "Backup.h"
#include "Platform.h"
#include "Port.hpp"
#include "JsonLite.h"
#include "CtlServer.hpp"
#include "PetI18n.h"
#include <cstdarg>
#include <ctime>
#include <time.h>
#include <csignal>
#include <cerrno>
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
#include "PlayerAllocator.h"  // Cubism 分配器（拆出）
#include "CubismRenderer_Software.hpp"
#include "SoftTexture.hpp"
#include "PetAgent.h"
#include "PetCompanion.h"
#include "PetBubbleQueue.h"
#include "PetIntegration.h"
#include "PetMemory.h"
#include "PetPersona.h"
#include "PetQuestions.h"
#include "PetJokes.h"
#include "PetVerbs.h"
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
#include <cmath>
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

// ---- 类之前就要有的小定义（原 l2dtest.cpp 的文件作用域部分）----

/** 弹簧固有频率 = 通道 slew × 本系数（slew 沿用旧"一阶收敛率"口径，换弹簧后手感对齐）*/
static const csmFloat32 kBodyOmegaK = 1.6f;

/** 躯体通道（BODY）：一个语义通道 → 本模型的一到多个参数（带系数）。
 *  语义是**增量**（相对呼吸/视线的自动行为），设定后每帧保持、一阶平滑过渡。
 *  写入分两层（SDK 语义决定，见 ApplyBodyChannelsPre/Post）：
 *    pre  = 物理输入类（头/身角度，靠物理链生效）→ 写在 SaveParameters 之后、
 *           scheduler 之前——物理看得到，且不会被存进"基线"（否则逐帧累加）。
 *    post = 直连类（手/眼/嘴…）→ 写在最后：躲开 EyeBlink 的**覆盖写**（眨眼每帧
 *           覆写 EyeLOpen，写在它之前保不住"闭眼/眨眼"）。 */
struct BodyChannel
{
    std::string name;                                    // "head.x" / "arm.r" …
    std::vector<std::pair<csmInt32, csmFloat32> > params; // 参数序号 → 系数
    csmFloat32 target;   // 目标值（指令设定）
    csmFloat32 value;    // 当前值（弹簧平滑后）
    csmFloat32 vel;      // 弹簧速度（动捕感：有惯性、轻微过冲，见 ApplyBodyChannelsPre）
    csmFloat32 slew;     // 默认响应速度（越大越快；弹簧固有频率 ω = slew×1.6）
    csmFloat32 omega;    // 当前弹簧固有频率（BODY t=<秒> 可临时改，下次设定恢复默认）
    csmFloat32 extra;    // 本帧叠加层：动作片段（ACTION）+ 微动 + 跟随（每帧重算）
    csmFloat32 out;      // 本帧最终写入值 = value + extra
    csmFloat32 follow;   // 跟随滞后状态（身体跟头：follow-through）
    csmBool set;         // 是否被外部设定（未设定 = 不写）
    csmBool releasing;   // 已释放、正在弹回 0（回到 0 后自动 set=false）
    csmBool fromScene;   // 由演出脚本的 body 步设定（段尾自动释放）
    csmBool pre;         // 写入层：true=物理前（见上）
    csmBool rigid;       // 有部件数据时是否走关节骨架（作者绑好手臂的模型置 false）
    csmInt32 rig;        // ≥0：由关节骨架消费（不写参数；见 ApplyArmRigs）；-1=普通通道
    csmFloat32 lo, hi;   // 关节通道的上下限（度）；普通通道不用
    BodyChannel() : target(0.0f), value(0.0f), vel(0.0f), slew(6.0f), omega(9.6f), extra(0.0f),
                    out(0.0f), follow(0.0f), set(false), releasing(false), fromScene(false),
                    pre(false), rigid(true), rig(-1), lo(0.0f), hi(0.0f) {}
};

/** 动作片段（ACTION）的一条通道轨道：通道空间的关键帧（跨模型：模型缺的通道自动跳过）*/
struct ActionTrack
{
    std::string ch;                                          // 通道名（arm.r / head.y …）；或 hand.r/hand.l（IK 窗口）
    std::vector<std::pair<csmFloat32, csmFloat32> > keys;    // (归一化时间 0..1, 值)；IK 窗口只用首/末时刻
    std::string anchor;                                      // IK 窗口：锚点名（chin/mouth/hip…）
};

/** 动作片段定义："像动捕一样"的整身动作（抬手挥舞/欢呼/耸肩…），写在躯体通道上 */
struct ActionDef
{
    std::string name;
    std::string desc;     // 一句话描述（给 LLM 的能力表用）
    std::string expr;     // 可选：动作自带表情
    csmFloat32 dur;
    std::vector<ActionTrack> tracks;
    bool fromProfile = false;   // profile 写的（角色化动作，按作者绑定调过）
};

/** 生效中的动作轨道（已解析到通道序号）*/
struct LiveActionTrack
{
    size_t ch;
    csmFloat32 start, dur, scale;
    csmFloat32 fadeAt;    // >0：从该时刻起 0.25s 淡出（被新动作接管同一通道时）
    std::vector<std::pair<csmFloat32, csmFloat32> > keys;   // 拷贝（切模型时定义表会重建）
};

// 指令环/证据输出那几支（定义在 l2dtest.cpp）：头文件里的内联成员会回调它们，
// 所以这里给出跨文件声明，定义处相应去掉 static。
class L2DPlayer;   // 下面三个声明要用到它（类定义在后面）
extern PetAgent* g_builtinAgent;   // 内置 agent（定义在 l2dtest.cpp 的主循环里）
extern bool g_uiOff;               // 关掉自带 UI（定义在 l2dtest.cpp；内联成员会读它）

#if defined(_WIN32) && !defined(strtok_r)
#define strtok_r strtok_s   // MSVC 的重入分词器（与 strtok_r 同签名）
#endif
void SetReply(const char* fmt, ...);
void WriteCaps(L2DPlayer& player);

class L2DPlayer : public CubismUserModel
{
public:
    /** 一个关节/手型开关（能力上报用：label 进 fv，name 是作者的显示名）*/
    struct JointInfo
    {
        std::string label;                   // shoulder / elbow / wrist / finger1 / forearm …（fv 里用）
        std::string name;                    // 作者显示名（"右腕B 肘の回転"）
        csmInt32 param;
    };
    struct ArmSet
    {
        std::string name, desc;
        std::vector<csmInt32> show;          // 选中时显示的部件（含子部件）
        csmInt32 joint[4];                   // 肩/肘/腕/手指 参数序号（-1=无）
        csmFloat32 scale[4];                 // 通道值 → 参数的系数（作者参数方向不一时用 -1）
        std::vector<std::pair<csmInt32, csmFloat32> > fixed;   // 这套生效时压住的参数（手型显示开关等）
        std::vector<JointInfo> jlist;        // 全部关节（含手指/前臂等附加关节）——能力上报
        std::vector<JointInfo> hands;        // 手型开关（同一套手臂的不同手型；选中者=1 其余=0）
        bool bilateral = false;              // 一套同时含左右两臂（Hiyori 的"腕 A/B"）：两侧联动切换
        // 过桥采样（肩×肘网格上这套手臂的轮廓：每样本 8 个数 = 肩, 肘, 中心x, 中心y, 包围盒 x0 y0 x1 y1）
        std::vector<csmFloat32> samp;
        bool sampled = false;
    };
    struct AuthorArm
    {
        std::string name;                    // 显示名（"右腕"/"左腕"）
        std::string ch;                      // "arm.r" / "arm.l"
        std::vector<ArmSet> sets;
        std::vector<csmInt32> allParts;      // 本侧所有套的部件（切换时全部先隐藏）
        csmInt32 cur;                        // 目标套（-1 = auto：模型自己的 pose/动作说了算）
        csmInt32 shown = -1;                 // 当前**显示**的套（过渡中先是旧套，换手后是新套）
        // 换套过渡（"真正移动过去"）：旧手臂转到交接姿势 → pose 交叉淡化 → 新手臂从交接姿势转到目标
        struct Trans
        {
            bool active = false;
            int from = -1, to = -1, phase = 0;
            csmFloat32 t0 = 0.0f, T = 0.45f;
            csmFloat32 bridgeFrom[2] = { 0.0f, 0.0f }, bridgeTo[2] = { 0.0f, 0.0f };
            bool toAuto = false;
            std::vector<std::pair<csmInt32, csmFloat32> > targets;   // 新套关节的最终目标
        } tr;
    };
    L2DPlayer();
    void SetNoMotion(csmBool noMotion) { _noMotion = noMotion; }
    /** 渲染缩放（围绕屏幕中心放大模型；1.0=官方 layout 原始大小；<=0 且启用 fit 时自动计算） */
    void SetRenderZoom(csmFloat32 zoom) { _renderZoom = zoom; }
    /** 自动等比适配：保持长宽比的最大填充（fillRatio 为填充率，1.0=贴满短边） */
    void SetAutoFit(csmBool enable, csmFloat32 fillRatio)
    {
        _autoFit = enable;
        _fitFill = fillRatio;
        if (enable)
        {
            _renderZoom = 0.0f; // 待计算
        }
    }
    /** 触摸跟随（拖动时眼睛/头部跟随手指位置） */
    void SetTouchFollow(csmBool enable) { _followEnabled = enable; }
    /**
     * 宿主（外部进程）给的注视目标（渲染目标像素坐标）。
     * 与 HandleTouch 的"跟随"部分同语义：只设目标位置，不参与点击/缩放条判定；
     * 速度平滑（0.15s 加速/运动学减速）仍由 CubismTargetPoint 在调度器里完成。
     * 宿主把触摸拖动位置经共享区命令字（--cmd-fd 指令环）送进来。
     */
    void SetLookTargetPixels(csmFloat32 x, csmFloat32 y)
    {
        _followTargetX = x;
        _followTargetY = y;
        _followHasTarget = true;
    }
    /** 水印开关：off（默认）＝应用模型自带的"水印"表情参数（作者在 VTS 里用空格触发的就是它） */
    void SetWatermarkOff(csmBool off) { _watermarkOff = off; }
    /** 跟随自检：目标每 2.5s 自动移到下一角（四角循环；调试/验收用） */
    void SetFollowTest(csmBool enable) { _followTest = enable; }
    /** main 在加载/切换模型后调用：设置模型名（顶部短暂提示条；自动转大写） */
    void SetModelName(const char* name)
    {
        _modelName = (name != NULL) ? name : "";
        for (size_t i = 0; i < _modelName.size(); ++i)
        {
            if (_modelName[i] >= 'a' && _modelName[i] <= 'z')
            {
                _modelName[i] = (char)(_modelName[i] - 32); // 点阵字库为大写
            }
        }
        _modelNameToastTimer = L2D_UI_MODEL_TOAST_S;
    }
    /** 模型库（桌宠托盘/菜单用：名字 = 目录名）*/
    void SetModelLibrary(const std::vector<std::string>& paths, int cur)
    {
        _modelNames.clear();
        for (size_t i = 0; i < paths.size(); ++i) _modelNames.push_back(BaseName(DirName(paths[i])));
        _modelIndex = cur;
    }
    /** 取走"切到第 n 个模型"的请求（-1 = 无）*/
    int TakeModelSwitchTo()
    {
        const int r = _modelSwitchTo;
        _modelSwitchTo = -1;
        return r;
    }
    void PushPetEvent(const std::string& ev) { _petEvents.push_back(ev); }
    void SetAgentCount(int n) { _agentCount = n; }
    /** 模型页导入后：主循环重扫模型库（并把 wantsName 指示的新模型切过去） */
    bool TakeModelRescan(std::string* switchName)
    {
        if (!_modelRescanRequest) return false;
        _modelRescanRequest = false;
        *switchName = _modelRescanSwitch;
        _modelRescanSwitch.clear();
        return true;
    }
    /** 主循环侧请求切换模型（重扫后按名字定位） */
    void RequestModelSwitchTo(int idx) { _modelSwitchTo = idx; }
    /** 主循环发现模型库有增减：只刷新列表（不换模型、不重载） */
    void RequestModelRescanRefresh()
    {
        _modelRescanRequest = true;
        _modelRescanSwitch.clear();
    }
    /** 能力探测完成后请求重载当前模型（部件→臂骨架要重load才建） */
    bool TakeModelReload()
    {
        const bool r = _modelReloadRequest;
        _modelReloadRequest = false;
        return r;
    }
    /** 取走模型切换请求（-1=上一个 +1=下一个 0=无；取后清零） */
    csmInt32 TakeModelSwitchRequest()
    {
        const csmInt32 r = _modelSwitchRequest;
        _modelSwitchRequest = 0;
        return r;
    }
    /** 当前渲染缩放（切换模型时保持用户调好的值） */
    csmFloat32 RenderZoom() const { return _renderZoom; }
    /** 配置里存的大小（config.zoom）；桌宠滚轮缩放以它为基准（见主循环 isPetPort 分支）*/
    csmFloat32 UiZoom() const { return Cfg("zoom"); }
    /** 切换自检：每 3s 自动请求切下一个模型（调试/验收用） */
    void SetModelSwitchTest(csmBool enable) { _modelSwitchTest = enable; }
    // （跟随幅度由 Load() 里 CubismLook 的参数表决定：AngleX/Y ±30、BodyAngleX ±10、EyeBall ±1）

    /** 每帧真实耗时（供右上角 FPS 显示；0.5s 滑动平均） */
    int MeasuredFps() const { return _fpsValue; }
    void NoteFrameTime(csmFloat32 realDt)
    {
        if (realDt <= 0.0f || realDt > 1.0f)
        {
            return;
        }
        _fpsAccum += realDt;
        ++_fpsFrames;
        if (_fpsAccum >= 0.25f)
        {
            _fpsValue = (int)((csmFloat32)_fpsFrames / _fpsAccum + 0.5f);
            _fpsAccum = 0.0f;
            _fpsFrames = 0;
        }
        _sysStats.Update(realDt);
    }
    /**
     * 触摸事件（渲染目标坐标系）：
     *  - 底部 10%：缩放条（5% 步进）
     *  - 其余区域：按下/拖动 → 眼睛/头部/身体注视跟随手指位置（抬起后保持）
     *  - 抬起时若未拖动（<30px）且按下点在模型包围盒内 → 切换下一个表情
     */
    void HandleTouch(int x, int y, bool down);
    /** 切换到下一个表情（循环） */
    void NextExpression()
    {
        if (_expressionNames.empty())
        {
            return;
        }
        _expressionIndex = (_expressionIndex + 1) % (int)_expressionNames.size();
        const csmString key(_expressionNames[_expressionIndex].c_str());
        ACubismMotion* motion = _expressions[key];
        if (motion != NULL)
        {
            _expressionManager->StartMotion(motion, false);
            printf("[ui] 表情 → %s (%d/%d)\n", _expressionNames[_expressionIndex].c_str(),
                   _expressionIndex + 1, (int)_expressionNames.size());
            fflush(stdout);
        }
    }
    /** 启动后每 N 秒自动切换表情（--expr-test 调试用） */
    void SetExprTest(csmBool enable) { _exprTest = enable; }
    // ======================= 远程指令 / 演出引擎 =======================
    // 由 main() 的指令环（L2dCmd.h）消费：表情/动作/手势/演出脚本/复位 + 能力表。
    // 说明：脚本 JSON 的解析只在这里做一份（复用 SDK 自带 CubismJson），宿主保持"哑"。

    csmInt32 ExpressionCount() const { return (csmInt32)_expressionNames.size(); }
    const char* ExpressionName(csmInt32 idx) const
    {
        if (idx < 0 || idx >= (csmInt32)_expressionNames.size())
        {
            return "";
        }
        return _expressionNames[(size_t)idx].c_str();
    }
    /** 按序号播表情（-1 = 下一个）*/
    csmBool StartExpression(csmInt32 idx)
    {
        if (idx == -1)
        {
            NextExpression();
            return true;
        }
        if (idx < 0 || idx >= (csmInt32)_expressionNames.size())
        {
            return false;
        }
        StartExpressionKey(_expressionNames[(size_t)idx].c_str());
        return true;
    }
    /** 按名字播表情（支持 profile 里的别名）*/
    csmBool StartExpressionByName(const char* name)
    {
        const char* real = name;

        for (size_t i = 0; i < _exprAliases.size(); ++i)
        {
            if (_exprAliases[i].first == name)
            {
                real = _exprAliases[i].second.c_str();
                break;
            }
        }
        for (size_t i = 0; i < _expressionNames.size(); ++i)
        {
            if (_expressionNames[i] == real)
            {
                StartExpressionKey(_expressionNames[i].c_str());
                return true;
            }
        }
        return false;
    }
    /** 清表情（回默认；演出脚本里 expr:null 用）*/
    void ClearExpression()
    {
        _expressionManager->StopAllMotions();
        _expressionIndex = -1;
        printf("[ui] 表情 → 默认\n");
        fflush(stdout);
    }
    csmInt32 MotionGroupCount() const
    {
        return _setting != NULL ? _setting->GetMotionGroupCount() : 0;
    }
    const char* MotionGroupName(csmInt32 g) const
    {
        if (_setting == NULL || g < 0 || g >= _setting->GetMotionGroupCount())
        {
            return "";
        }
        return _setting->GetMotionGroupName(g);
    }
    csmInt32 MotionCount(csmInt32 g) const
    {
        if (_setting == NULL || g < 0 || g >= _setting->GetMotionGroupCount())
        {
            return 0;
        }
        return _setting->GetMotionCount(MotionGroupName(g));
    }
    /** 播动作（"组序号_动作序号"；播完自动回 Idle，见 Update 的既有逻辑）*/
    csmBool StartMotionByGroupIndex(csmInt32 g, csmInt32 idx)
    {
        if (_setting == NULL || g < 0 || g >= _setting->GetMotionGroupCount() || idx < 0)
        {
            return false;
        }
        const csmString key = Utils::CubismString::GetFormatedString("%s_%d", MotionGroupName(g), idx);
        ACubismMotion* motion = _motions[key];
        if (motion == NULL)
        {
            return false;
        }
        _motionManager->StartMotionPriority(static_cast<CubismMotion*>(motion), false, 3 /* PriorityForce */);
        printf("[ui] 动作 → %s_%d\n", MotionGroupName(g), idx);
        fflush(stdout);
        return true;
    }
    /** 手势个数 / 名字（能力表用）*/
    csmInt32 GestureCount() const { return (csmInt32)_gestures.size(); }
    const char* GestureName(csmInt32 i) const
    {
        if (i < 0 || i >= (csmInt32)_gestures.size())
        {
            return "";
        }
        return _gestures[(size_t)i].name.c_str();
    }
    /** 触发手势（内置通用 + 模型 profile；参数轨道叠加在呼吸/视线之上）*/
    csmBool StartGesture(const char* name, csmFloat32 speed = 1.0f, csmInt32 repeat = 1, csmFloat32 scale = 1.0f)
    {
        for (size_t i = 0; i < _gestures.size(); ++i)
        {
            if (_gestures[i].name == name)
            {
                SpawnGestureTracks(_gestures[i], speed, repeat, scale);
                if (!_gestures[i].expr.empty())
                {
                    // 手势自带表情（姿态类手势：举话筒/猫爪…）——先切表情再动
                    if (!StartExpressionByName(_gestures[i].expr.c_str()))
                    {
                        fprintf(stderr, "[ui] 手势表情不存在：%s\n", _gestures[i].expr.c_str());
                    }
                }
                printf("[ui] 手势 → %s\n", name);
                fflush(stdout);
                return true;
            }
        }
        return false;
    }
    /** 复位：停表情/动作/手势/脚本，回默认待机 */
    void PerformanceReset()
    {
        _expressionManager->StopAllMotions();
        _motionManager->StopAllMotions();
        _scenes.clear();
        _live.clear();
        StopActions();
        ResetBodyChannels();   // 姿态弹回（不是瞬间跳回）
        _partHold.clear();
        _expressionIndex = -1;
        _idleNextAt = _perfClock + 1.0f;
        printf("[ui] 复位（回默认待机）\n");
        fflush(stdout);
    }
    /** 外部指令到达 → 自主待机静默一段时间（不与人的操作抢）*/
    void NoteExternalActivity() { _activityUntil = _perfClock + 8.0f; }
    // ===================== 说话（口型 / "像主播一样讲话"） =====================
    // 思路：文本或时长 → 时长估计 → 逐 viseme（元音 A/I/U/E/O + 闭口）驱动嘴部参数。
    // 写在轨道之后（ApplyTalk 在 Update 尾段）= 最高优先级，正好叠在"麦克风/猫猫手"
    // 这类表情姿态上：举着话筒说话。参数不存在自动忽略（跨模型安全）。
    // 不做音素对齐（没接 TTS）——目标是"看着像在说话"，嘴唇张合与元音形都有。

    void StartTalk(csmFloat32 dur, const char* text)
    {
        if (dur < 0.3f)
        {
            dur = 0.3f;
        }
        if (dur > 120.0f)
        {
            dur = 120.0f;
        }
        _talkEnd = _perfClock + dur;
        _talkNextAt = _perfClock;   // 立刻切第一个 viseme
        _talkViseme = 1;
        printf("[talk] 开始：%.1fs%s%s\n", dur,
               (text != NULL && *text != '\0') ? "：" : "",
               (text != NULL) ? text : "");
        fflush(stdout);
        NoteExternalActivity();
    }
    /** 说话时长估计：CJK 每字 ~0.22s、英文每字符 ~0.06s、空格 0.12s、ASCII 标点 0.25s
     *  （多字节 UTF-8 序列按"一个字"算——逐字节累加会让中文每字 0.66s，三倍高估）*/
    static csmFloat32 EstimateTalkDur(const char* text)
    {
        if (text == NULL || *text == '\0')
        {
            return 3.0f;
        }
        csmFloat32 d = 0.0f;

        for (const unsigned char* p = (const unsigned char*)text; *p != '\0';)
        {
            if (*p < 0x80u)
            {
                if (*p == ' ') { d += 0.12f; }
                else if (*p == ',' || *p == '.' || *p == '!' || *p == '?' || *p == ';') { d += 0.25f; }
                else { d += 0.06f; }
                ++p;
                continue;
            }
            // UTF-8 首字节 → 序列长度；续字节不合法（截断/坏数据）时按 1 字节处理，绝不多跳
            int len = (*p & 0xE0u) == 0xC0u ? 2 : ((*p & 0xF0u) == 0xE0u ? 3 : ((*p & 0xF8u) == 0xF0u ? 4 : 1));
            for (int k = 1; k < len; ++k)
            {
                if (p[k] == '\0' || (p[k] & 0xC0u) != 0x80u) { len = k; break; }
            }
            d += 0.22f;   // 一个字
            p += len;
        }
        return (d < 0.8f) ? 0.8f : d;
    }
    csmBool IsTalking() const { return _perfClock < _talkEnd; }
    /** 说话口的每帧驱动（Update 尾段、轨道之后调用）*/
    void ApplyTalk();
    /** 写参数（存在才写；不存在自动忽略——跨模型安全）；idx 缓存避免每帧查 id */
    void SetParamIfExists(const char* name, csmFloat32 v)
    {
        const csmInt32 idx = _model->GetParameterIndex(
            CubismFramework::GetIdManager()->GetId(name));

        if (idx >= 0 && idx < _model->GetParameterCount())
        {
            _model->SetParameterValue((csmUint32)idx, v);
        }
    }
    // ===================== 躯体通道（BODY：像控制身体一样控制模型） =====================
    // 通用通道名 → 本模型参数：标准名自动映射（各家同名参数直接可用），profile 的
    // "body" 段可覆盖/追加（如舒芙蕾的 5 段手臂链 → 一条 arm.r 通道）。
    // 语义：**增量**（写值叠加在呼吸/视线等自动行为之上，身体仍然"活着"）；
    // 设定后每帧保持 + 一阶平滑（看起来像身体在动过去），直到改值或 `BODY reset`。

    /** 建通道（参数全部不存在则整条通道不建——跨模型自动降级）。
     *  preOverride：-1=按能力数据自动判层（eff 含 p → 物理前），0=强制 post，1=强制 pre */
    void AddBodyChannel(const char* name, const std::vector<std::pair<const char*, csmFloat32> >& maps,
                        csmFloat32 slew, csmInt32 preOverride = -1, csmInt32 rigidOverride = -1);
    /** 装载时建标准通道表 + profile 覆盖 */
    void BuildBodyChannels();
    /** profile 里的 "body" 段：{ "arm.r": { "slew":4, "params":[{"param":"ParamarmR1","scale":-1}, …] } }
     *  同名通道 = 覆盖参数映射；新名字 = 追加通道 */
    void LoadBodyChannelProfile();
    /** 通道序号（找不到 -1）*/
    int FindBodyChannel(const char* name) const
    {
        for (size_t i = 0; i < _channels.size(); ++i)
        {
            if (_channels[i].name == name)
            {
                return (int)i;
            }
        }
        return -1;
    }
    /** 设定一条通道（找不到返回 false）。
     *  trans>0：这一次"大约多少秒到位"（BODY t=0.3 / 脚本 body 步的 dur）；0=通道默认速度 */
    csmBool SetBodyChannel(const char* name, csmFloat32 v, csmFloat32 trans = 0.0f,
                           csmBool fromScene = false)
    {
        const int i = FindBodyChannel(name);

        if (i < 0)
        {
            return false;
        }
        BodyChannel& ch = _channels[(size_t)i];

        // 显式设手臂关节角（或 arm.r 简写）= 接管这条手臂：取消它的 IK（否则下一帧被 IK 覆盖回去）
        if (ch.rig >= 0 && (size_t)ch.rig < _rigs.size())
        {
            _rigs[(size_t)ch.rig].ik = false;
        }
        ch.target = v;
        ch.set = true;
        ch.releasing = false;
        ch.fromScene = fromScene;
        if (trans > 0.05f)
        {
            // 阻尼 0.72 的弹簧约 4.5/ω 秒进入 2% 带
            csmFloat32 w = 4.5f / trans;

            ch.omega = w < 1.5f ? 1.5f : (w > 40.0f ? 40.0f : w);
        }
        else
        {
            ch.omega = ch.slew * kBodyOmegaK;
        }
        NoteExternalActivity();
        return true;
    }
    /** 通道是否存在（指令校验用）*/
    csmBool HasBodyChannel(const char* name) const
    {
        return FindBodyChannel(name) >= 0;
    }
    /** 全部释放（回到呼吸/视线自动行为）——**弹回**而不是瞬间跳回 0（动捕感：放下手是个动作）*/
    void ResetBodyChannels()
    {
        _handEvents.clear();
        for (size_t a = 0; a < _authorArms.size(); ++a)
        {
            ArmToAuto(_authorArms[a]);
        }
        for (size_t r = 0; r < _rigs.size(); ++r)
        {
            _rigs[r].ik = false;
        }
        for (size_t i = 0; i < _channels.size(); ++i)
        {
            ReleaseBodyChannel(_channels[i]);
        }
    }
    /** 只释放演出脚本设定的通道（一段演出结束 = 姿态收回；外部 BODY 设定的不动）*/
    void ReleaseSceneBody()
    {
        for (size_t i = 0; i < _channels.size(); ++i)
        {
            if (_channels[i].set && _channels[i].fromScene)
            {
                ReleaseBodyChannel(_channels[i]);
            }
        }
    }
    void ReleaseBodyChannel(BodyChannel& ch)
    {
        if (!ch.set)
        {
            return;
        }
        ch.target = 0.0f;
        ch.releasing = true;
        ch.fromScene = false;
        ch.omega = ch.slew * kBodyOmegaK;
    }
    /** 通道清单（caps 用："head.x,head.y,…"）*/
    void BodyChannelListText(char* out, size_t cap) const
    {
        size_t n = 0;

        out[0] = '\0';
        for (size_t i = 0; i < _channels.size(); ++i)
        {
            const int w = snprintf(out + n, cap > n ? cap - n : 0, "%s%s",
                                   i ? "," : "", _channels[i].name.c_str());

            if (w < 0 || (size_t)w >= cap - n)
            {
                break;
            }
            n += (size_t)w;
        }
    }
    /** 通道说明（caps 的 BODYINFO=，给 LLM 定幅度用）："名:下限..上限:方式"。
     *  方式：joint=渲染器肩关节旋转（0.45≈胸/肩 0.6≈斜上 1≈平举；负=外展）、
     *        p=经物理链（头/身，会带动头发衣服）、d=直连参数。范围=相对默认值的增量 */
    void BodyInfoText(char* out, size_t cap) const;
    /** 当前设定值（查询用："head.x=20.0 body.x=-5.0"；未设定为空）*/
    void BodyStateText(char* out, size_t cap) const
    {
        size_t n = 0;

        out[0] = '\0';
        for (size_t i = 0; i < _channels.size(); ++i)
        {
            if (!_channels[i].set || _channels[i].releasing)
            {
                continue;
            }
            const int w = snprintf(out + n, cap > n ? cap - n : 0, "%s%s=%.3g",
                                   n ? " " : "", _channels[i].name.c_str(),
                                   (double)_channels[i].target);

            if (w < 0 || (size_t)w >= cap - n)
            {
                break;
            }
            n += (size_t)w;
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
    void BuildBuiltinActions();
    /** 关键帧求值：Catmull-Rom（过点不停顿；端点切线按单侧差分）*/
    static csmFloat32 EvalActionKeys(const std::vector<std::pair<csmFloat32, csmFloat32> >& k, csmFloat32 u)
    {
        const size_t n = k.size();

        if (n == 0)
        {
            return 0.0f;
        }
        if (n == 1 || u <= k[0].first)
        {
            return k[0].second;
        }
        if (u >= k[n - 1].first)
        {
            return k[n - 1].second;
        }
        size_t i = 1;

        while (i < n - 1 && u > k[i].first)
        {
            ++i;
        }
        // 段 [i-1, i]
        const csmFloat32 t0 = k[i - 1].first, t1 = k[i].first;
        const csmFloat32 p0 = k[i - 1].second, p1 = k[i].second;
        const csmFloat32 h = (t1 > t0) ? (t1 - t0) : 1e-4f;
        const csmFloat32 s = (u - t0) / h;
        // 切线（按时间归一的 Catmull-Rom，非均匀间隔也稳）
        const csmFloat32 m0 = (i >= 2) ? (p1 - k[i - 2].second) / (t1 - k[i - 2].first) * h : (p1 - p0);
        const csmFloat32 m1 = (i + 1 < n) ? (k[i + 1].second - p0) / (k[i + 1].first - t0) * h : (p1 - p0);
        // 端点（0 值的起止帧）用 0 切线 = 起落柔和
        const csmFloat32 mm0 = (i == 1) ? 0.0f : m0;
        const csmFloat32 mm1 = (i + 1 == n) ? 0.0f : m1;
        const csmFloat32 s2 = s * s, s3 = s2 * s;

        return (2 * s3 - 3 * s2 + 1) * p0 + (s3 - 2 * s2 + s) * mm0
             + (-2 * s3 + 3 * s2) * p1 + (s3 - s2) * mm1;
    }
    const ActionDef* FindAction(const char* name) const
    {
        for (size_t i = 0; i < _actions.size(); ++i)
        {
            if (_actions[i].name == name)
            {
                return &_actions[i];
            }
        }
        return NULL;
    }
    /** 起一个动作。missing（可空）= 本模型缺的通道（逗号分隔，回复给 LLM）。
     *  返回 false = 动作名不存在 */
    csmBool StartAction(const char* name, csmFloat32 speed, csmFloat32 scale, csmInt32 repeat,
                        std::string* missing = NULL, csmFloat32* outDur = NULL);
    /** 动作清单（caps 的 ACTIONS=："wave_big(举手挥舞),cheer(欢呼举双手),…"）*/
    void ActionListText(char* out, size_t cap) const
    {
        size_t n = 0;

        out[0] = '\0';
        for (size_t i = 0; i < _actions.size(); ++i)
        {
            const int w = snprintf(out + n, cap > n ? cap - n : 0, "%s%s(%s)", i ? "," : "",
                                   _actions[i].name.c_str(), _actions[i].desc.c_str());

            if (w < 0 || (size_t)w >= cap - n)
            {
                break;
            }
            n += (size_t)w;
        }
    }
    void StopActions()
    {
        _liveActs.clear();
        _handEvents.clear();
        for (size_t r = 0; r < _rigs.size(); ++r)
        {
            ReleaseHand(_rigs[r]);
        }
    }
    /** 作者手臂（profile 或自动识别）收尾：外加骨架让位、建关节通道、补关节清单与显示名 */
    void FinalizeAuthorArm(AuthorArm& arm);
    /** 选手型：这套的手型开关里 label 匹配的 = 1，其余 = 0（找不到返回 false）*/
    static bool SelectHand(ArmSet& st, const std::string& label)
    {
        bool found = false;

        for (size_t q = 0; q < st.hands.size(); ++q)
        {
            found = found || st.hands[q].label == label;
        }
        if (!found)
        {
            return false;
        }
        st.fixed.clear();
        for (size_t q = 0; q < st.hands.size(); ++q)
        {
            st.fixed.push_back(std::make_pair(st.hands[q].param, st.hands[q].label == label ? 1.0f : 0.0f));
        }
        return true;
    }
    /** 物理链的输出参数（被动摆动，不算关节：舒芙蕾的 armR1~5 就是）*/
    std::set<csmInt32> PhysicsOutputParams();
    /** id 里 "Arm"/"arm" 之后的大写字母（≤2）与余下部分："ArmBR02"→("BR","02")，"ArmL01"→("L","01") */
    static bool ParseArmTail(const std::string& id, std::string* letters, std::string* rest)
    {
        size_t p = id.find("Arm");
        size_t len = 3;

        if (p == std::string::npos)
        {
            p = id.find("arm");
        }
        if (p == std::string::npos)
        {
            p = id.find("Hand");   // Hiyori：ParamHandLB（左手B 回転）= B 套左手腕
            len = 4;
        }
        if (p == std::string::npos)
        {
            return false;
        }
        size_t i = p + len;
        std::string L;

        while (i < id.size() && L.size() < 2 && id[i] >= 'A' && id[i] <= 'Z')
        {
            L += id[i++];
        }
        *letters = L;
        *rest = id.substr(i);
        return true;
    }
    /** 字母 → 左右 + 套名："AR"→(R,"A")（Natori）、"LA"→(L,"A")（Mao）、"L"→(L,"") */
    static void SideAndSet(const std::string& L, char* side, std::string* set)
    {
        const auto lr = [](char c) { return c == 'L' || c == 'R'; };

        *side = 0;
        set->clear();
        if (L.size() == 2)
        {
            if (lr(L[1]) && !lr(L[0])) { *side = L[1]; *set = L.substr(0, 1); }
            else if (lr(L[0]))         { *side = L[0]; *set = L.substr(1, 1); }
            else                       { *set = L; }
        }
        else if (L.size() == 1)
        {
            if (lr(L[0])) { *side = L[0]; }
            else          { *set = L; }
        }
    }
    /** 显示名里的左右/套名（作者偶有 id 笔误：Natori 左腕C 的参数叫 ParamArmCR01）*/
    static void SideSetFromName(const std::string& n, char* side, std::string* set)
    {
        if (n.find("右") != std::string::npos || n.find("Right") != std::string::npos) *side = 'R';
        if (n.find("左") != std::string::npos || n.find("Left") != std::string::npos) *side = 'L';
        const size_t p = n.find("腕");

        if (p != std::string::npos && p + 3 < n.size() && n[p + 3] >= 'A' && n[p + 3] <= 'Z')
        {
            *set = std::string(1, n[p + 3]);   // "左腕C" → C（"腕" 是 3 字节 UTF-8）
        }
    }
    /** 自动识别作者手臂（pose3 分组 = 几套手臂；参数名/显示名 = 关节）——见 PROTOCOL-LLM.md §3.2 */
    void AutoDetectArmSets();
    /** 骨架等级（PROTOCOL-LLM §3.2）：full / chain / swing / none */
    const char* SkeletonLevel() const
    {
        bool chain = false, full = false;

        for (size_t a = 0; a < _authorArms.size(); ++a)
        {
            int posed = 0;

            for (size_t k = 0; k < _authorArms[a].sets.size(); ++k)
            {
                const ArmSet& st = _authorArms[a].sets[k];

                posed += st.show.empty() ? 0 : 1;
                chain = chain || st.jlist.size() >= 2;
            }
            full = full || posed >= 2;
        }
        if (full) return "full";
        if (chain) return "chain";
        bool joints = false;

        for (size_t a = 0; a < _authorArms.size(); ++a)
        {
            for (size_t k = 0; k < _authorArms[a].sets.size(); ++k)
            {
                joints = joints || !_authorArms[a].sets[k].jlist.empty();
            }
        }
        const int r = FindBodyChannel("arm.r"), l = FindBodyChannel("arm.l");

        return (joints || r >= 0 || l >= 0) ? "swing" : "none";
    }
    /** profile "armSets"：
     *  { "arm.r": { "A": {"desc":"自然下垂","show":["PartArmAR","PartArmARFore"],
     *                     "shoulder":"ParamArmAR01","elbow":"ParamArmAR02","wrist":"ParamArmAR03",
     *                     "fingers":"…", "scale":{"elbow":-1}}, "B": {…} } }
     *  第一套 = 关节通道在 auto 时映射的那套。建好后本侧关节通道 arm.X.shoulder/elbow/wrist(/fingers)
     *  直接写作者参数（作者的关节链 = 骨骼），不走我们的外加骨架。 */
    void LoadAuthorArmSets();
    /** 手臂套交回模型自己的 pose/动作：根部件伪参数恢复成"第一套可见"（同 SDK pose 初始化），
     *  子部件不透明度恢复为 1（旧版本压过子部件的，这里一并修好），之后由作者动作去切 */
    void ArmToAuto(AuthorArm& arm)
    {
        arm.cur = -1;
        arm.shown = -1;
        arm.tr.active = false;
        for (size_t k = 0; k < arm.sets.size(); ++k)
        {
            for (size_t i = 0; i < arm.sets[k].show.size(); ++i)
            {
                const csmInt32 root = arm.sets[k].show[i];

                HoldPoseRoot(root, k == 0 ? 1.0f : 0.0f);
                for (csmInt32 p = 0; p < _model->GetPartCount(); ++p)
                {
                    for (csmInt32 q = _model->GetPartParentPartIndex((csmUint32)p); q >= 0;
                         q = _model->GetPartParentPartIndex((csmUint32)q))
                    {
                        if (q == root)
                        {
                            _model->SetPartOpacity(p, 1.0f);
                            break;
                        }
                    }
                }
            }
        }
    }
    /** 切手臂套：arm.r.set=B / auto。关节通道改接这套的关节参数。返回错误（空=成功）*/
    std::string SetArmSet(const char* chSet, const char* name, bool dryRun);
    /** 能力表 ARMSETS=arm.r:A(自然下垂)/B(举手)…;arm.l:… ；查询用的当前选择 */
    void ArmSetText(char* out, size_t cap, bool current) const
    {
        size_t n = 0;

        out[0] = '\0';
        for (size_t a = 0; a < _authorArms.size() && n < cap; ++a)
        {
            const AuthorArm& arm = _authorArms[a];

            if (current)
            {
                if (arm.cur >= 0)
                {
                    n += (size_t)snprintf(out + n, cap - n, "%s%s.set=%s", n ? " " : "", arm.ch.c_str(),
                                          arm.sets[(size_t)arm.cur].name.c_str());
                }
                continue;
            }
            n += (size_t)snprintf(out + n, cap - n, "%s%s:", a ? ";" : "", arm.ch.c_str());
            for (size_t k = 0; k < arm.sets.size() && n < cap; ++k)
            {
                n += (size_t)snprintf(out + n, cap - n, "%s%s(%s)", k ? "/" : "", arm.sets[k].name.c_str(),
                                      arm.sets[k].desc.c_str());
            }
        }
    }
    // ===================== 桌宠协议（PROTOCOL-LLM.md）：能力上报 + 控制 =====================
    // 每一项能力 = {name 显示名, fv 特征值}；控制只认 fv。内置动作只上报"纯头身表情"的——
    // 需要外加骨架（渲染器转 2D 手臂）的不上报（大角度手是扭曲的，见 DESIGN-ARM-JOINTS §7）。

    static void JStr(std::string& o, const std::string& s)
    {
        o += '"';
        for (size_t i = 0; i < s.size(); ++i)
        {
            const unsigned char c = (unsigned char)s[i];

            if (c == '"' || c == '\\') { o += '\\'; o += (char)c; }
            else if (c == '\n') { o += "\\n"; }
            else if (c == '\r') { o += "\\r"; }
            else if (c == '\t') { o += "\\t"; }
            else if (c < 0x20) { char b[8]; snprintf(b, sizeof(b), "\\u%04x", c); o += b; }
            else { o += (char)c; }
        }
        o += '"';
    }
    static std::string JNum(double v)
    {
        char b[32];
        snprintf(b, sizeof(b), "%.4g", v);
        return b;
    }
    /** 转义后带引号的 JSON 字符串（拼协议回复用） */
    static std::string JStrRet(const std::string& s)
    {
        std::string o;
        JStr(o, s);
        return o;
    }
    std::string ModelFv() const { return "model:" + FingerprintText(); }
    void SetViewSize(csmInt32 w, csmInt32 h)
    {
        _viewW = w;
        _viewH = h;
    }
    /** 协议 look 命令生效期间，端口的鼠标视线不覆盖它 */
    csmBool LookOverridden() const { return _perfClock < _lookOverrideUntil; }
    std::vector<std::string> TakePetEvents()
    {
        std::vector<std::string> out;

        out.swap(_petEvents);
        return out;
    }
    // ===================== 交互区域 + 指针手势（PROTOCOL-LLM.md §5.4）=====================
    // 16 区：头部/脸部/颈部/胸部/肚子/胯部 + 左右大腿/小腿/脚 + 左右手/肘（左右 = 观看者屏幕口径）。
    // 判定 = 模型屏幕包围盒 + 命名锚点投影（eyes/chin/waist）+ 臂骨架的腕/肘关节（Core 未旋转坐标，稳定）。
    enum
    {
        Z_OUT = -1,
        Z_HEAD = 0, Z_FACE, Z_NECK, Z_CHEST, Z_BELLY, Z_CROTCH,
        Z_THIGH_L, Z_THIGH_R, Z_SHIN_L, Z_SHIN_R, Z_FOOT_L, Z_FOOT_R,
        Z_HAND_L, Z_HAND_R, Z_ELBOW_L, Z_ELBOW_R,
        ZONE_COUNT
    };
    static const char* ZoneKey(int z)
    {
        static const char* const k[] = { "head", "face", "neck", "chest", "belly", "crotch",
                                         "thigh.l", "thigh.r", "shin.l", "shin.r", "foot.l", "foot.r",
                                         "hand.l", "hand.r", "elbow.l", "elbow.r" };
        return (z >= 0 && z < ZONE_COUNT) ? k[z] : NULL;
    }
    static const char* ZoneLabel(int z)
    {
        static const char* const k[] = { "头部", "脸部", "颈部", "胸部", "肚子", "胯部",
                                         "左大腿", "右大腿", "左小腿", "右小腿", "左脚", "右脚",
                                         "左手", "右手", "左手肘", "右手肘" };
        return (z >= 0 && z < ZONE_COUNT) ? k[z] : NULL;
    }
    static int ZoneFromKey(const char* key)
    {
        if (key == NULL) return Z_OUT;
        for (int z = 0; z < ZONE_COUNT; ++z)
        {
            if (strcmp(ZoneKey(z), key) == 0) return z;
        }
        return Z_OUT;
    }
    /** L2D_DBG_ZONES=1：点击时打印分区依据（锚点投影/包围盒），校准用 */
    static bool ZonesDebug()
    {
        static const bool on = (getenv("L2D_DBG_ZONES") != NULL);
        return on;
    }
    /** 模型坐标 → 渲染目标像素（与 ComputeArtBBox 同一投影；含 render zoom）*/
    bool ProjectModelPoint(csmFloat32 mx, csmFloat32 my, csmFloat32* sx, csmFloat32* sy)
    {
        if (_targetW < 8.0f || _targetH < 8.0f)
        {
            return false;
        }
        CubismMatrix44 projection;

        BuildBaseProjection(&projection, (csmUint32)_targetW, (csmUint32)_targetH);
        if (_renderZoom != 1.0f && _renderZoom > 0.0f)
        {
            projection.ScaleRelative(_renderZoom, _renderZoom);
        }
        projection.MultiplyByMatrix(GetModelMatrix());
        const csmFloat32* mm = projection.GetArray();
        const csmFloat32 px = mx * mm[0] + my * mm[4] + mm[12];
        const csmFloat32 py = mx * mm[1] + my * mm[5] + mm[13];
        const csmFloat32 pw = mx * mm[3] + my * mm[7] + mm[15];
        const csmFloat32 iw = (pw != 0.0f) ? 1.0f / pw : 1.0f;

        *sx = (px * iw + 1.0f) * 0.5f * _targetW;
        *sy = (1.0f - py * iw) * 0.5f * _targetH;
        return true;
    }
    bool ProjectAnchor(const char* name, csmFloat32* sx, csmFloat32* sy)
    {
        std::map<std::string, std::pair<csmFloat32, csmFloat32> >::const_iterator it = _anchors.find(name);

        if (it == _anchors.end())
        {
            return false;
        }
        return ProjectModelPoint(it->second.first, it->second.second, sx, sy);
    }
    /** 屏幕点 → 区域（bb = ComputeArtBBox 的结果；渲染目标坐标系）*/
    int ClassifyZone(csmFloat32 x, csmFloat32 y, const csmFloat32* bb);
    /** 桌宠指针每帧喂进来（渲染目标坐标；has=false 表示指针不在窗口/正拖动窗口）：
     *  hover（同区驻留 0.6s，之后每 7s 一次）+ rub（1.5s 内 ≥3 次左右反向）+ click 防抖计时 */
    void PetPointerTick(csmFloat32 px, csmFloat32 py, bool has, csmFloat32 dt);
    /** 桌宠点击：同区 1s 内连点合并计数（click 事件在安静 1s 后发出，见 PetPointerTick）*/
    void NotePetClick(const char* zone)
    {
        const int z = ZoneFromKey(zone);

        if (z < 0)
        {
            return;
        }
        if (_clickZone == z)
        {
            ++_clickCount;
        }
        else
        {
            _clickZone = z;
            _clickCount = 1;
        }
        _clickClock = 0.0f;
    }
    /** 气泡选项被点中 → choice 事件（label 来自下发时的 options）*/
    void PetChoiceMade(int index);
    /** 输出端口（桌宠气泡用；NULL = 无输出，speak 只动口型不出气泡）*/
    void SetPort(IPort* port)
    {
        _port = port;
        _isPet = (port != NULL && (strcmp(port->Name(), "pet") == 0 || strcmp(port->Name(), "glfwpet") == 0));
    }
    /** 通用部位显示名 */
    static const char* PartLabel(const std::string& ch)
    {
        static const struct { const char* ch; const char* name; } kNames[] = {
            { "head.x", "头左右转" }, { "head.y", "头上下" }, { "head.z", "头歪" },
            { "body.x", "身体左右转" }, { "body.y", "身体前后" }, { "body.z", "身体歪" },
            { "gaze.x", "视线左右" }, { "gaze.y", "视线上下" },
            { "eye.l", "左眼开合" }, { "eye.r", "右眼开合" },
            { "brow.l", "左眉上下" }, { "brow.r", "右眉上下" },
            { "brow.l.form", "左眉形" }, { "brow.r.form", "右眉形" },
            { "mouth.open", "嘴张开" }, { "mouth.form", "嘴形（笑/撇）" },
        };
        for (size_t i = 0; i < sizeof(kNames) / sizeof(kNames[0]); ++i)
        {
            if (ch == kNames[i].ch) return kNames[i].name;
        }
        return NULL;
    }
    /** 通道的增量范围（同 BODYINFO 的算法）*/
    void ChannelRange(const BodyChannel& ch, csmFloat32* lo, csmFloat32* hi) const
    {
        *lo = -1.0f;
        *hi = 1.0f;
        if (ch.params.empty())
        {
            return;
        }
        const csmUint32 idx = (csmUint32)ch.params[0].first;
        const csmFloat32 sc = ch.params[0].second != 0.0f ? ch.params[0].second : 1.0f;
        const csmFloat32 def = _model->GetParameterDefaultValue(idx);
        const csmFloat32 a = (_model->GetParameterMinimumValue(idx) - def) / sc;
        const csmFloat32 b = (_model->GetParameterMaximumValue(idx) - def) / sc;

        *lo = a < b ? a : b;
        *hi = a < b ? b : a;
    }
    /** 动作需求：face（纯头身表情）/ arm.pose（切作者手臂套）/ arm.joint（作者关节）/ arm.skeleton（外加骨架，不上报）*/
    /** 内置动作里"本质就是手臂"的：去掉手臂就不成立，不上报（手臂版由 profile 按作者骨骼写）*/
    static bool ArmEssential(const ActionDef& a)
    {
        static const char* const kArm[] = { "wave_big", "wave_both", "fist_pump", "point", "stretch_up",
                                             "mic", "hug_self", "shrug" };

        for (size_t i = 0; !a.fromProfile && i < sizeof(kArm) / sizeof(kArm[0]); ++i)
        {
            if (a.name == kArm[i]) return true;
        }
        return false;
    }
    std::string ActionNeeds(const ActionDef& a) const
    {
        if (!a.fromProfile)
        {
            return "face";   // 内置动作执行时不带手臂轨道（见 StartAction）
        }
        bool set = false, joint = false;

        for (size_t t = 0; t < a.tracks.size(); ++t)
        {
            const std::string& ch = a.tracks[t].ch;

            if (ch.compare(0, 5, "hand.") == 0)
            {
                return "arm.skeleton";   // IK 只在外加骨架上有
            }
            if (ch.compare(0, 4, "arm.") == 0)
            {
                if (ch.size() > 4 && ch.compare(ch.size() - 4, 4, ".set") == 0)
                {
                    set = true;
                }
                else
                {
                    joint = true;
                }
            }
        }
        if (set) return "arm.pose";
        if (joint) return _authorArms.empty() ? "arm.skeleton" : "arm.joint";
        return "face";
    }
    bool ActionReportable(const ActionDef& a) const
    {
        if (!a.fromProfile)
        {
            return !ArmEssential(a);
        }
        return ActionNeeds(a) != "arm.skeleton";
    }
    /** 动作素材信息（时长 / 是否动手臂）——首次查询时读 motion3 */
    struct MotionInfo { std::string group; csmInt32 index; csmFloat32 dur; bool arm; };
    const std::vector<MotionInfo>& Motions();
    /** query：能力表（what 为空 = summary）*/
    std::string CapabilitiesJson(const jsonlite::Value& what, csmFloat32 fps);
    // ---------- fv 解析 ----------
    bool ResolveSetFv(const std::string& fv, size_t* armIdx, size_t* setIdx) const
    {
        if (fv.compare(0, 5, "pose:") != 0)
        {
            return false;
        }
        const std::string rest = fv.substr(5);   // arm.r.B
        for (size_t a = 0; a < _authorArms.size(); ++a)
        {
            const std::string pre = _authorArms[a].ch + ".";

            if (rest.compare(0, pre.size(), pre) != 0)
            {
                continue;
            }
            const std::string name = rest.substr(pre.size());

            for (size_t k = 0; k < _authorArms[a].sets.size(); ++k)
            {
                if (_authorArms[a].sets[k].name == name)
                {
                    *armIdx = a;
                    *setIdx = k;
                    return true;
                }
            }
        }
        return false;
    }
    /** joint:arm.r.B.elbow / hand:arm.r.B.05 → (臂, 套, 项) */
    const JointInfo* ResolveJointFv(const std::string& fv, bool hand, size_t* armIdx, size_t* setIdx) const
    {
        const std::string prefix = hand ? "hand:" : "joint:";

        if (fv.compare(0, prefix.size(), prefix) != 0)
        {
            return NULL;
        }
        const std::string rest = fv.substr(prefix.size());
        const size_t dot = rest.rfind('.');

        if (dot == std::string::npos)
        {
            return NULL;
        }
        if (!ResolveSetFv("pose:" + rest.substr(0, dot), armIdx, setIdx))
        {
            return NULL;
        }
        const std::string label = rest.substr(dot + 1);
        const std::vector<JointInfo>& lst = hand ? _authorArms[*armIdx].sets[*setIdx].hands
                                                 : _authorArms[*armIdx].sets[*setIdx].jlist;

        for (size_t i = 0; i < lst.size(); ++i)
        {
            if (lst[i].label == label)
            {
                return &lst[i];
            }
        }
        return NULL;
    }
    const ActionDef* ResolveActionFv(const std::string& fv) const
    {
        // 宽容：LLM 可以把 act: 前缀写漏（"shy" 与 "act:shy" 等价）
        const std::string name = fv.compare(0, 4, "act:") == 0 ? fv.substr(4) : fv;
        const ActionDef* a = FindAction(name.c_str());

        return (a != NULL && ActionReportable(*a)) ? a : NULL;
    }
    // ---------- 关节保持（pose.joints：绝对值，弹簧过渡，叠在作者动作之上）----------
    struct JointHold
    {
        csmInt32 param;
        csmFloat32 target, value, vel, omega, weight;
        bool started, releasing;
    };
    void HoldJoint(csmInt32 param, csmFloat32 v, csmFloat32 trans)
    {
        const csmFloat32 lo = _model->GetParameterMinimumValue((csmUint32)param);
        const csmFloat32 hi = _model->GetParameterMaximumValue((csmUint32)param);
        const csmFloat32 w = (trans > 0.05f) ? 4.5f / trans : 8.0f;

        v = v < lo ? lo : (v > hi ? hi : v);
        for (size_t i = 0; i < _jointHolds.size(); ++i)
        {
            if (_jointHolds[i].param == param)
            {
                _jointHolds[i].target = v;
                _jointHolds[i].omega = w;
                _jointHolds[i].releasing = false;
                return;
            }
        }
        JointHold h;

        h.param = param;
        h.target = v;
        h.value = 0.0f;
        h.vel = 0.0f;
        h.omega = w;
        h.weight = 1.0f;
        h.started = false;
        h.releasing = false;
        _jointHolds.push_back(h);
    }
    void ReleaseJointHolds()
    {
        for (size_t i = 0; i < _jointHolds.size(); ++i)
        {
            _jointHolds[i].releasing = true;
        }
    }
    /** post 层（作者动作/scheduler 之后）调用：写关节最终值 */
    void ApplyJointHolds(csmFloat32 dt);
    /** 收回所有姿势：手臂套回 auto、关节放开、通用部位弹回 */
    void ReleasePose()
    {
        ReleaseJointHolds();
        // 手臂：移动回默认那套的自然姿势，再交回作者动作（过渡见 BeginArmTransition）
        const std::vector<std::pair<csmInt32, csmFloat32> > none;

        for (size_t a = 0; a < _authorArms.size(); ++a)
        {
            AuthorArm& arm = _authorArms[a];

            if (arm.cur >= 0 || arm.shown >= 0 || arm.tr.active)
            {
                if (!arm.sets.empty() && arm.sets[0].bilateral)
                {
                    ArmToAuto(arm);
                }
                else
                {
                    BeginArmTransition(a, 0, none, true, 0.45f);
                }
            }
        }
        for (size_t i = 0; i < _channels.size(); ++i)
        {
            if (PartLabel(_channels[i].name) != NULL)
            {
                ReleaseBodyChannel(_channels[i]);
            }
        }
    }
    // ---------- 执行一步（单项命令和 sequence 的每一步共用）----------
    static void Warn(std::vector<std::string>* w, int step, const std::string& fv, const std::string& msg)
    {
        if (w == NULL)
        {
            return;
        }
        std::string o = "{";

        if (step >= 0)
        {
            o += "\"step\":" + std::to_string(step) + ",";
        }
        o += "\"fv\":";
        JStr(o, fv);
        o += ",\"msg\":";
        JStr(o, msg);
        o += "}";
        w->push_back(o);
    }
    /** 执行（dry=true 只校验）。返回生效的项数 */
    int ExecPetStep(const jsonlite::Value& v, std::vector<std::string>* warn, int step, bool dry);
    // ---------- 时间线 ----------
    struct PetStep { csmFloat32 at; jsonlite::Value v; bool fired; };
    struct PetSeq { std::string seq; /* JSON 记号：带引号的字符串 / 数字 / null */ csmFloat32 dur; csmFloat32 begin; bool started; std::vector<PetStep> steps; };
    /** 每帧推进（Update 开头调用）*/
    void PetTick()
    {
        PetBubbleQueue::Bubble bubble;
        if (_port && _bubbleQueue.Take(_perfClock, Cfg("bubble_min_secs"), _port->BubbleAwaitingChoice(), &bubble))
            PresentBubble(bubble);
        // 提问气泡过期/被收起后清掉陈旧选项与猜拳状态：否则它们一直挂着，companion 会按
        //"主人在选"把外部消息静默挡掉（_bubbleOptions 只在点选/换气泡/reset 时清过）。
        // 必须晚于帧首的点选消费——点下去的瞬间端口就不再 awaiting，这一帧点选已被取走才安全。
        if (_port != NULL && !_bubbleOptions.empty() && !_port->BubbleAwaitingChoice())
        {
            _bubbleOptions.clear();
            _bubbleOptionDo.clear();
            _gameRps = 0;
        }
        if (_petSeqs.empty())
        {
            return;
        }
        PetSeq& sq = _petSeqs.front();

        if (!sq.started)
        {
            sq.started = true;
            sq.begin = _perfClock;
        }
        const csmFloat32 rel = _perfClock - sq.begin;

        for (size_t i = 0; i < sq.steps.size(); ++i)
        {
            if (!sq.steps[i].fired && rel >= sq.steps[i].at)
            {
                sq.steps[i].fired = true;
                ExecPetStep(sq.steps[i].v, NULL, (int)i, false);
            }
        }
        if (rel >= sq.dur)
        {
            const std::string ev = "{\"event\":\"done\",\"seq\":" + sq.seq + "}";

            _petEvents.push_back(ev);
            _petSeqs.pop_front();
        }
        NoteExternalActivity();   // 演出期间自主待机不插话
    }
    /** 处理一行协议请求，返回回复 JSON（无换行）*/
    std::string PetCommand(const std::string& line, csmFloat32 fps, bool* subscribe);
    // ===================== 换套过渡：关节过桥（"真正移动过去"，不跳变）=====================
    // 两套手臂是作者画的两张画，无法互相变形。做法：在两套各自的"肩×肘"空间里找轮廓最接近的
    // 一对姿势（交接姿势）——旧手臂用关节转过去、在两者重合处交给 pose 交叉淡化、新手臂再从
    // 交接姿势转到目标。看起来就是一条手臂连续移动过去。

    /** pose 组根部件：只设同名伪参数，让 SDK pose 按它的 FadeInTime 做交叉淡化（不直接压不透明度）*/
    void HoldPoseRoot(csmInt32 partIndex, csmFloat32 v)
    {
        _model->SetParameterValue(_model->GetParameterIndex(_model->GetPartId((csmUint32)partIndex)), v);
    }
    /** 采样一套手臂：肩×肘 5×5 网格，每个姿势下这套手臂（根部件下所有 drawable）的轮廓 */
    void SampleArmSet(ArmSet& st);
    /** 当前实际显示的套（auto 时由作者动作/pose 决定：看根部件不透明度最大的）*/
    int VisibleSet(const AuthorArm& arm) const
    {
        int best = -1;
        csmFloat32 bv = 0.01f;

        for (size_t k = 0; k < arm.sets.size(); ++k)
        {
            if (!arm.sets[k].show.empty())
            {
                const csmFloat32 o = _model->GetPartOpacity(arm.sets[k].show[0]);

                if (o > bv)
                {
                    bv = o;
                    best = (int)k;
                }
            }
        }
        return best;
    }
    void HoldJointSnap(csmInt32 param, csmFloat32 v)
    {
        HoldJoint(param, v, 0.0f);
        for (size_t i = 0; i < _jointHolds.size(); ++i)
        {
            if (_jointHolds[i].param == param)
            {
                _jointHolds[i].value = _jointHolds[i].target;
                _jointHolds[i].vel = 0.0f;
                _jointHolds[i].weight = 1.0f;
                _jointHolds[i].started = true;
            }
        }
    }
    void ReleaseJoint(csmInt32 param)
    {
        for (size_t i = 0; i < _jointHolds.size(); ++i)
        {
            if (_jointHolds[i].param == param)
            {
                _jointHolds[i].releasing = true;
            }
        }
    }
    /** 关节通道（arm.r.shoulder…）改接某一套的关节参数 */
    void RemapArmChannels(const AuthorArm& arm, int idx)
    {
        static const char* const kJ[4] = { "shoulder", "elbow", "wrist", "fingers" };
        const ArmSet& st = arm.sets[(size_t)(idx < 0 ? 0 : idx)];

        for (int j = 0; j < 4; ++j)
        {
            const int ci = FindBodyChannel((arm.ch + "." + kJ[j]).c_str());

            if (ci < 0)
            {
                continue;
            }
            _channels[(size_t)ci].params.clear();
            if (st.joint[j] >= 0)
            {
                _channels[(size_t)ci].params.push_back(std::make_pair(st.joint[j], st.scale[j]));
            }
        }
    }
    /** 开始换套过渡（to = 目标套；toAuto = 过渡完交回自动）。T = 每段移动时长 */
    void BeginArmTransition(size_t a, int to, const std::vector<std::pair<csmInt32, csmFloat32> >& targets,
                            bool toAuto, csmFloat32 T);
    /** 每帧推进过渡（pre 层、压部件之前调用）*/
    void ArmTransTick();
    /** 压住一个部件的可见性：部件不透明度 + **同名伪参数**。
     *  SDK 的 pose 判"哪套可见"看的是与部件同名的伪参数（作者动作的 PartOpacity 曲线写的就是它），
     *  不是部件不透明度——只压不透明度的话 pose 仍以为旧套可见，每帧往旧套渐变一步
     *  （实测卡在 0.83/0.17 交叉渐变，手臂半透明 + 旧手臂残影）。 */
    void HoldPart(csmInt32 partIndex, csmFloat32 v)
    {
        _model->SetPartOpacity(partIndex, v);
        _model->SetParameterValue(_model->GetParameterIndex(_model->GetPartId((csmUint32)partIndex)), v);
    }
    /** PART 部件=不透明度 …（每帧在 pose 之前压住；切作者画的另一套手臂用）／ PART off。
     *  pose 按"组内第一个不透明度 > 0 的部件"选可见套，所以切套时要把同组其他套压成 0。
     *  返回错误说明（空=成功）*/
    std::string SetPartHold(const char* args);
    /** 动捕感强度：0=关（纯指令值，逐位可复现）1=默认 2=活泼 3=夸张 */
    void SetMocapLevel(csmInt32 lv) { _mocapLevel = lv < 0 ? 0 : (lv > 3 ? 3 : lv); }
    csmInt32 MocapLevel() const { return _mocapLevel; }
    /** 微动幅度（动捕的"活着"：真人不会一动不动；单位同通道）*/
    static csmFloat32 MocapNoiseAmp(const std::string& n)
    {
        if (n == "head.x") return 2.0f;
        if (n == "head.y") return 1.4f;
        if (n == "head.z") return 1.6f;
        if (n == "body.x" || n == "body.z") return 0.8f;
        if (n == "body.y") return 0.5f;
        if (n == "arm.r" || n == "arm.l") return 0.025f;
        if (n.size() > 9 && n.compare(n.size() - 9, 9, ".shoulder") == 0) return 1.2f;
        if (n.size() > 6 && n.compare(n.size() - 6, 6, ".elbow") == 0) return 1.8f;
        if (n.size() > 6 && n.compare(n.size() - 6, 6, ".wrist") == 0) return 2.0f;
        return 0.0f;
    }
    /** 平滑噪声：三个不可公度的正弦（0.13/0.31/0.59 Hz，人体晃动频段），值域约 ±1 */
    static csmFloat32 MocapNoise(size_t seed, csmFloat32 t)
    {
        const csmFloat32 s = (csmFloat32)(seed * 7 + 3);

        return 0.5f * sinf(t * 0.83f + s * 1.7f) + 0.3f * sinf(t * 1.97f + s * 2.9f)
             + 0.2f * sinf(t * 3.71f + s * 0.61f);
    }
    /** 每帧推进（pre 层调用一次：动作 + 弹簧 + 动捕层，然后写"物理前"那批；post 由另一相写）*/
    void ApplyBodyChannelsPre(csmFloat32 dt);
    /** 本帧是否需要写（被设定 / 有动作或动捕叠加）*/
    static bool BodyChannelActive(const BodyChannel& ch)
    {
        return ch.set || fabsf(ch.out) > 1e-5f;
    }
    void ApplyBodyChannelsPost(csmBool rendererAvailable)
    {
        csmBool anyLimb = false;

        for (size_t i = 0; i < _channels.size(); ++i)
        {
            BodyChannel& ch = _channels[i];

            if (ch.pre || !BodyChannelActive(ch))
            {
                continue;
            }
            /* 骨架消费的通道（arm.r 与 arm.r.shoulder/elbow/wrist）不写参数：见 ApplyArmRigs */
            if (ch.rig >= 0 && rendererAvailable)
            {
                continue;
            }
            WriteBodyChannel(ch);
        }
        _limbActive = ApplyArmRigs(rendererAvailable ? SoftwareRenderer() : NULL);
        ApplyJointHolds(_lastDt);   // 桌宠协议 pose.joints：作者关节的最终值
    }
    // ===================== 手臂关节骨架（只在关节弯折；live2d/DESIGN-ARM-JOINTS.md） =====================
    // 肩/肘/腕三个铰链，上臂/前臂/手三段刚性；网格只在关节两侧窄带（默认 ±4% 臂长）里两骨混合。
    // 关节点锚在**顶点**上（每帧取那圈顶点的均值）→ 跟着呼吸/物理摆动走，转心不漂。
    // 通道约定（度）：shoulder 正=外展抬起、负=内收；elbow/wrist 正=往身体内侧折（托腮/抱胸）、
    // 负=往外侧折（抬臂挥手时前臂竖起）。老通道 arm.r = 肩的简写（1.0=90°，负值 1.0=60°）。
    // IK：hand.r=<锚点|x,y> → 两骨 IK 解出肩/肘目标（肘朝外），交给弹簧过去。

    struct JointAnchor
    {
        std::vector<std::pair<csmInt32, csmInt32> > verts;   // (drawable, 顶点)：本帧均值 = 关节点
        csmFloat32 ox, oy;   // verts 为空时：相对肩关节的固定偏移（肩自己为空时 = 绝对坐标）
        JointAnchor() : ox(0.0f), oy(0.0f) {}
    };
    struct ArmRig
    {
        std::string ch;          // "arm.r" / "arm.l"
        std::string side;        // "r" / "l"
        csmFloat32 sign;         // +1：在中线 +x 侧（逆时针=外展抬起）；-1：镜像
        std::vector<csmInt32> draws;                                        // 参与的 drawable（可见那套）
        std::vector<std::vector<CubismRenderer_Software::SkinW> > skin;    // 与 draws 对齐
        JointAnchor sh, el, wr, hand;   // 肩（球心）/ 肘 / 腕 / 手心（IK 末端）
        csmInt32 bone0;          // 骨号：bone0=上臂 +1=前臂 +2=手（0 = 躯干/恒等）
        csmFloat32 lim[3][2];    // 肩/肘/腕 上下限（度）
        csmInt32 chS, chE, chW;  // 关节通道序号
        csmFloat32 tS, tE, tW;   // 诊断：关节在臂轴上的位置（0=肩顶 1=指尖）
        int nUpper, nFore, nHand, nSplit;
        csmBool ik;              // IK 生效
        std::string ikName;      // 锚点名（空 = 坐标）
        csmFloat32 ikX, ikY;     // 坐标目标
        csmFloat32 ikMiss;       // 最近一次解：够不着的距离（占整臂长比例；0=够得着）
        csmFloat32 ikS, ikE;     // 最近一次解出的肩/肘（度，通道约定）
    };
    static csmFloat32 SmoothStep(csmFloat32 e0, csmFloat32 e1, csmFloat32 x)
    {
        if (e1 <= e0)
        {
            return x < e0 ? 0.0f : 1.0f;
        }
        csmFloat32 t = (x - e0) / (e1 - e0);

        t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
        return t * t * (3.0f - 2.0f * t);
    }
    static csmFloat32 WrapDeg(csmFloat32 a)
    {
        while (a > 180.0f) a -= 360.0f;
        while (a < -180.0f) a += 360.0f;
        return a;
    }
    void AnchorPos(const JointAnchor& a, csmFloat32 sx, csmFloat32 sy, csmFloat32* x, csmFloat32* y) const
    {
        if (a.verts.empty())
        {
            *x = sx + a.ox;
            *y = sy + a.oy;
            return;
        }
        double ax = 0.0, ay = 0.0;

        for (size_t i = 0; i < a.verts.size(); ++i)
        {
            const Live2D::Cubism::Core::csmVector2* pos = _model->GetDrawableVertexPositions(a.verts[i].first);

            ax += pos[a.verts[i].second].X;
            ay += pos[a.verts[i].second].Y;
        }
        *x = (csmFloat32)(ax / (double)a.verts.size());
        *y = (csmFloat32)(ay / (double)a.verts.size());
    }
    /** 读模型目录的 l2d.profile.json（可选；失败返回 false）*/
    bool LoadProfileJson(jsonlite::Value* root) const
    {
        if (_dir.empty())
        {
            return false;
        }
        FILE* f = fopen((_dir + "/l2d.profile.json").c_str(), "rb");

        if (f == NULL)
        {
            return false;
        }
        std::string text;
        char buf[4096];
        size_t got;

        while ((got = fread(buf, 1, sizeof(buf), f)) > 0 && text.size() < (1u << 20))
        {
            text.append(buf, got);
        }
        fclose(f);
        return jsonlite::parse(text.c_str(), text.size(), *root, NULL);
    }
    /** 建一条手臂的骨架（静止姿态 = 当前 Core 顶点）。失败返回 false。*/
    bool BuildOneArmRig(const std::string& chName, const std::vector<csmInt32>& partDraws,
                        const jsonlite::Value& prof, ArmRig* out);
    /** 偏移参数（按序号）→ 动了的顶点的质心；空/全无效返回 false */
    bool MovingCentroidIdx(const std::vector<csmInt32>& idxs, bool toMin, csmFloat32* cx, csmFloat32* cy);
    /** 无刚性骨架时的锚点：脸/下颌用参数质心，腰/胸口按包围盒比例，手用作者手参数质心 */
    void BuildFallbackAnchors();
    /** 偏移参数 → 动了的顶点的质心（定位嘴/眼用；参数不存在返回 false）*/
    bool MovingCentroid(const char* const* ids, int nIds, bool toMin, csmFloat32* cx, csmFloat32* cy);
    /** 装载时建骨架（通道建好之后调用）：每条手臂一份 + 关节通道 + 命名锚点 */
    void BuildArmRigs();
    /** 权重里的骨号按本臂实际起点重编号（BuildOneArmRig 假定 bone0=1）*/
    static void RemapRigBones(ArmRig* rig, csmInt32 bone0)
    {
        rig->bone0 = bone0;
        for (size_t i = 0; i < rig->skin.size(); ++i)
        {
            for (size_t v = 0; v < rig->skin[i].size(); ++v)
            {
                CubismRenderer_Software::SkinW& w = rig->skin[i][v];

                if (w.b0 > 0) w.b0 = (unsigned char)(w.b0 - 1 + bone0);
                if (w.b1 > 0) w.b1 = (unsigned char)(w.b1 - 1 + bone0);
            }
        }
    }
    /** 中线、左右、身体锚点（依赖肩/肘位置；骨架几何重建后重算）*/
    void ComputeRigFrame();
    /** 骨架几何按"当前"可见网格重建（懒：第一次驱动手臂时做）——装载时 pose 还没生效，
     *  Haru 的 ArmRB（另一套姿势美术）那时也"可见"，会被错收进骨架 */
    void RefreshRigGeometry()
    {
        jsonlite::Value prof;

        LoadProfileJson(&prof);
        for (size_t k = 0; k < _rigs.size(); ++k)
        {
            ArmRig& rig = _rigs[k];
            std::map<std::string, std::vector<csmInt32> >::const_iterator it = _parts.find(rig.ch);
            ArmRig nr;

            if (it == _parts.end() || !BuildOneArmRig(rig.ch, it->second, prof, &nr))
            {
                continue;
            }
            RemapRigBones(&nr, rig.bone0);
            rig.draws = nr.draws;
            rig.skin = nr.skin;
            rig.sh = nr.sh;
            rig.el = nr.el;
            rig.wr = nr.wr;
            rig.hand = nr.hand;
            rig.tS = nr.tS;
            rig.tE = nr.tE;
            rig.tW = nr.tW;
            rig.nUpper = nr.nUpper;
            rig.nFore = nr.nFore;
            rig.nHand = nr.nHand;
            rig.nSplit = nr.nSplit;
            printf("[rig] %s（按当前可见网格重建）：上臂 %d / 前臂 %d / 手 %d 张整网格刚性，%d 张按顶点切"
                   "（关节 t：肩 %.2f 肘 %.2f 腕 %.2f）\n", rig.ch.c_str(), rig.nUpper, rig.nFore, rig.nHand,
                   rig.nSplit, rig.tS, rig.tE, rig.tW);
        }
        ComputeRigFrame();
        _rigGeomFresh = true;
        fflush(stdout);
    }
    /** 命名锚点 → 坐标（每臂相关的按左右解析）*/
    bool ResolveAnchor(const ArmRig& rig, const std::string& name, csmFloat32* x, csmFloat32* y) const;
    /** IK：手心放到 (tx,ty)。解出肩/肘（通道约定，度）；返回够不着的距离（占整臂长的比例）*/
    csmFloat32 SolveArmIK(const ArmRig& rig, csmFloat32 tx, csmFloat32 ty, csmFloat32* shoulder, csmFloat32* elbow) const;
    /** 放开 IK（肩/肘弹回自然）*/
    void ReleaseHand(ArmRig& rig)
    {
        if (!rig.ik)
        {
            return;
        }
        rig.ik = false;
        ReleaseBodyChannel(_channels[(size_t)rig.chS]);
        ReleaseBodyChannel(_channels[(size_t)rig.chE]);
    }
    /** hand.r / hand.l 指令："mouth" / "off" / "x,y"（模型坐标）。错误时返回说明（空=成功）*/
    std::string SetHandTarget(const char* chName, const char* val, bool dryRun);
    /** 每帧：IK → 关节目标；FK → 骨变换 → 渲染器蒙皮。返回本帧是否有手臂在动 */
    csmBool ApplyArmRigs(CubismRenderer_Software* r);
    /** 能力表：SKELETON=arm.r:fk+ik,…；ANCHORS=…（给 LLM）*/
    void SkeletonText(char* out, size_t cap) const
    {
        size_t n = 0;

        out[0] = '\0';
        for (size_t r = 0; r < _rigs.size() && n < cap; ++r)
        {
            n += (size_t)snprintf(out + n, cap - n, "%s%s:fk+ik(肩%g..%g 肘%g..%g 腕%g..%g)", r ? "," : "",
                                  _rigs[r].ch.c_str(), (double)_rigs[r].lim[0][0], (double)_rigs[r].lim[0][1],
                                  (double)_rigs[r].lim[1][0], (double)_rigs[r].lim[1][1],
                                  (double)_rigs[r].lim[2][0], (double)_rigs[r].lim[2][1]);
        }
    }
    /** IK 状态（BODY 查询/回复用）："hand.r=mouth(肩20 肘135 差0.01)" */
    void HandStateText(char* out, size_t cap) const
    {
        size_t n = 0;

        out[0] = '\0';
        for (size_t r = 0; r < _rigs.size() && n < cap; ++r)
        {
            if (!_rigs[r].ik)
            {
                continue;
            }
            n += (size_t)snprintf(out + n, cap - n, "%shand.%s=%s(肩%.0f 肘%.0f%s)", n ? " " : "",
                                  _rigs[r].side.c_str(),
                                  _rigs[r].ikName.empty() ? "坐标" : _rigs[r].ikName.c_str(),
                                  (double)_rigs[r].ikS, (double)_rigs[r].ikE,
                                  _rigs[r].ikMiss > 0.08f ? " 够不着" : "");
        }
    }
    /** 增量写一条通道的所有参数（夹取到参数值域）*/
    void WriteBodyChannel(const BodyChannel& ch)
    {
        for (size_t j = 0; j < ch.params.size(); ++j)
        {
            const csmInt32 idx = ch.params[j].first;
            const csmFloat32 scale = ch.params[j].second;
            csmFloat32 v = _model->GetParameterValue((csmUint32)idx) + ch.out * scale;
            const csmFloat32 lo = _model->GetParameterMinimumValue((csmUint32)idx);
            const csmFloat32 hi = _model->GetParameterMaximumValue((csmUint32)idx);

            if (v < lo) { v = lo; }
            if (v > hi) { v = hi; }
            _model->SetParameterValue((csmUint32)idx, v);
        }
    }
    // ===================== 能力探测（PROBE；调试用） =====================
    // 把参数推到两侧极值，测"顶点级效果"（动没动/动多远/动的范围）——用于跨模型
    // 统计"手/头/口/眼 哪些能力通用、哪些命名各异"。纯解算不渲染，跑得飞快。
    // 输出每行：P <param> exists=0|1 [range=.. def=.. side=.. maxDelta=.. moved=.. bbox=..]

    /** 把全部参数恢复默认值并解算一次 */
    void ResetToDefaults()
    {
        const csmInt32 pc = _model->GetParameterCount();

        for (csmInt32 i = 0; i < pc; ++i)
        {
            _model->SetParameterValue(i, _model->GetParameterDefaultValue((csmUint32)i));
        }
        _model->Update();
    }
    /** 默认姿态顶点快照（按 drawable 存 xy） */
    void SnapshotVerts(std::vector<std::vector<csmFloat32> >* out)
    {
        const csmInt32 dc = _model->GetDrawableCount();

        out->assign((size_t)dc, std::vector<csmFloat32>());
        for (csmInt32 d = 0; d < dc; ++d)
        {
            const csmInt32 vc = _model->GetDrawableVertexCount(d);
            const Live2D::Cubism::Core::csmVector2* pos = _model->GetDrawableVertexPositions(d);
            std::vector<csmFloat32>& row = (*out)[(size_t)d];

            row.assign((size_t)(vc > 0 ? vc : 0) * 2, 0.0f);
            if (pos == NULL)
            {
                continue;
            }
            for (csmInt32 v = 0; v < vc; ++v)
            {
                row[(size_t)v * 2] = pos[v].X;
                row[(size_t)v * 2 + 1] = pos[v].Y;
            }
        }
    }
    /** 参数值快照（物理收敛判据用；只比"物理输出"参数，它们动完才算稳） */
    void SnapshotParams(std::vector<csmFloat32>* out) const
    {
        const csmInt32 pc = _model->GetParameterCount();

        out->assign((size_t)pc, 0.0f);
        for (csmInt32 i = 0; i < pc; ++i)
        {
            (*out)[(size_t)i] = _model->GetParameterValue((csmUint32)i);
        }
    }
    /** 物理跑到"稳定"：20 步一组推进，组间**全部参数**变化都小于阈值就停（上限 maxSteps）。
     *  物理是积分器，固定 40 步对不同模型不保证收敛——不收敛时测到的"效果"其实是
     *  残余摆动（老矩阵就有这种假阳性）。返回实际步数（诊断）。*/
    csmInt32 SettlePhysicsStable(csmInt32 maxSteps);
    /** 物理跑到稳态（自适应；L2D_PROBE_SETTLE 指定固定步数时用固定值，诊断收敛用）——
     *  有的模型美术不直接绑参数，而绑在"物理链输出"（如 ●X 镜像）上：
     *  不跑物理就测不出效果（实测 shuflle/sage）*/
    void SettlePhysics()
    {
        static const char* envN = getenv("L2D_PROBE_SETTLE");

        if (envN != NULL)
        {
            const int steps = atoi(envN);

            if (_physics != NULL)
            {
                for (int t = 0; t < steps; ++t)
                {
                    _physics->Evaluate(_model, 1.0f / 30.0f);
                }
            }
            _model->Update();
            return;
        }
        SettlePhysicsStable(600);   // 上限 600 步（20s 物理时间）；一般几十~百余步就收敛
    }
    /** 单参数效果档（效果探测结果 / 缓存载入；位定义见 kCap* ）*/
    struct ParamCap
    {
        csmUint8 flags;   // kCap* 按位或
        csmFloat32 delta; // 顶点最大位移（探测时观测值；纯换美术=0）
        ParamCap() : flags(0), delta(0.0f) {}
    };
    static const csmUint8 kCapKnown  = 1u;  // 已探测（未探测输出 eff=?）
    static const csmUint8 kCapDirect = 2u;  // 顶点直连（不跑物理就动）
    static const csmUint8 kCapPhys   = 4u;  // 经物理链（跑物理稳态才动）
    static const csmUint8 kCapArt    = 8u;  // 换美术（drawable 不透明度；顶点不动）
    /** 效果码（"dp"/"a"/"-"/"?"；PARAMS 表与缓存文件共用）*/
    static const char* CapCode(csmUint8 flags)
    {
        if ((flags & kCapKnown) == 0)
        {
            return "?";
        }
        if ((flags & (kCapDirect | kCapPhys | kCapArt)) == 0)
        {
            return "-";
        }
        static char buf[8];

        int n = 0;

        if (flags & kCapDirect) { buf[n++] = 'd'; }
        if (flags & kCapPhys)   { buf[n++] = 'p'; }
        if (flags & kCapArt)    { buf[n++] = 'a'; }
        buf[n] = '\0';
        return buf;
    }
    /** 效果码 → 位（缓存载入用；'?'/未知输入返回 0）*/
    static csmUint8 CapParseCode(const char* code)
    {
        csmUint8 f = kCapKnown;

        for (const char* p = code; *p != '\0'; ++p)
        {
            if (*p == 'd') { f |= kCapDirect; }
            else if (*p == 'p') { f |= kCapPhys; }
            else if (*p == 'a') { f |= kCapArt; }
        }
        return f;
    }
    /** 探测效果（按值返回，不用出参指针） */
    struct ProbeEffect
    {
        csmFloat32 delta;
        csmInt32 moved;
        csmFloat32 x0, x1, y0, y1;
        ProbeEffect() : delta(0.0f), moved(0), x0(0.0f), x1(0.0f), y0(0.0f), y1(0.0f) {}
    };
    /** 测"某个取值"下的顶点位移；physMode=false 直连（不跑物理）、true 走物理稳态 */
    ProbeEffect ProbeSide(csmInt32 idx, csmFloat32 val, csmBool physMode);
    /** 探测单个参数（按名字入口；不存在则打印 exists=0）
     *  注意：Cubism 的 GetParameterIndex 对**不存在**的 id 返回合成序号（≥参数数，指向
     *  "不存在参数表"），不是 -1——必须用 count 判存在，否则会越界探测（老 probe 的坑）*/
    void ProbeParam(const char* paramCode)
    {
        const csmInt32 idx = _model->GetParameterIndex(
            CubismFramework::GetIdManager()->GetId(paramCode));

        if (idx < 0 || idx >= _model->GetParameterCount())
        {
            printf("P %s exists=0\n", paramCode);
            fflush(stdout);
            return;
        }
        ProbeParamByIdx(idx);
    }
    /** 探测单个参数（按序号）；结果并入能力表 _cap，打印 O/P 两行（P 行尾带 eff=）*/
    void ProbeParamByIdx(csmInt32 idx);
    /** 批量探测**入队**（非阻塞：每帧跑一点，见 ProbeTick）。
     *  all=false → 标准表 + 自动族（名字含 arm/hand/leg/foot/knee 等）；all=true → 全量 */
    void SetProbePhys(csmBool on) { _probePhys = on; }
    void ProbeEnqueue(csmBool all);
    /** 每帧推进一步（主循环在正式解算前调用：探测的中间姿态会被本帧解算覆盖，
     *  屏幕不受影响；物理档会给物理链留过渡状态，探测期间模型轻微晃动属正常）*/
    void ProbeTick();
    /** 探测收尾：写能力缓存 + 重建参数表 + 回复（缓存里带模型指纹）*/
    void ProbeFinish()
    {
        _probeActive = false;
        const std::string cachePath = CachePath();
        const csmBool saved = SaveCapabilityCache();

        WriteCaps(*this);   // 参数表带上 eff 重新发布（宿主/工具立刻查得到）
        _petEvents.push_back(saved ? "{\"event\":\"probe\",\"done\":true,\"saved\":true}"
                                   : "{\"event\":\"probe\",\"done\":true,\"saved\":false}");
        _modelReloadRequest = true;   // 学到的部件要重载模型才变成臂骨架（分区随之完整）
        if (saved)
        {
            printf("[probe] 完成 %d/%d 项，能力缓存已写 %s\n",
                   (int)_probeDone, (int)_probeTotal, cachePath.c_str());
            SetReply("OK 探测完成 %d 项，能力已缓存", (int)_probeDone);
        }
        else
        {
            printf("[probe] 完成 %d/%d 项（缓存写入失败，仅本次运行有效）\n",
                   (int)_probeDone, (int)_probeTotal);
            SetReply("OK 探测完成 %d 项（缓存写盘失败）", (int)_probeDone);
        }
        fflush(stdout);
    }
    /**
     * "画出部分"的屏幕包围盒（顶点级；比 canvas 包围盒紧——点空白背景不算戳到人）。
     * 每次点击算一次（几万顶点变换 ≈ 1~2ms，点击频率下无所谓）。
     */
    void ComputeArtBBox(csmFloat32* bb);
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
    bool StartEmotion(const char* emo);
    /** 区域反应：kind = 0 点击 / 1 搓；n = 连点次数；(x,y) = 渲染目标坐标 */
    void ZoneReact(int z, int kind, int n, csmFloat32 x, csmFloat32 y);
    /** 本地台词库取一句（避免连着重复）：键 = "<区>.click" / "<区>.rub" / "angry"；没有就用内置兜底 */
    std::string PickReactLine(int z, int kind, bool angry);
    /** 每帧：本地反应带出的表情到点回默认 */
    void ZoneReactTick()
    {
        if (_reactExprUntil > 0.0f && _perfClock >= _reactExprUntil)
        {
            if (_expressionIndex == _reactExprIdx)
            {
                ClearExpression();
            }
            _reactExprUntil = 0.0f;
        }
    }
    // ================================================================ 配置（偏好窗口 / {"cmd":"config"}）
    // 扁平键值，存 ~/.config/l2dpet/config.json；窗口类键（opacity/top/passthrough）由 pet port 自己应用，
    // 这里只负责存取和随状态下发。llm.* 给 agent 读（config 事件 / status）。
    PetCompanion _companion;
    PetBubbleQueue _bubbleQueue;
    void PresentBubble(const PetBubbleQueue::Bubble& b)
    {
        if (!_port) return;
        std::vector<const char*> arr;
        for (const auto& option : b.shown) arr.push_back(option.c_str());
        _port->ShowBubble(b.text.c_str(), arr.empty() ? NULL : &arr[0], (int)arr.size(),
            (float)PetBubbleQueue::Hold(b, Cfg("bubble_min_secs")));
        _bubbleOptions = b.options;
        _bubbleOptionDo = b.actions;
        // 口型跟着"上屏"走（排队的气泡不能提前动嘴）；speak_dur 之类的显式时长在这里生效
        if (b.talk) StartTalk(b.talkDur > 0.05f ? b.talkDur : EstimateTalkDur(b.text.c_str()), b.text.c_str());
    }
    void ShowPetBubble(const std::string& text, const std::vector<std::string>& shown,
                       const std::vector<std::string>& options, const std::vector<jsonlite::Value>& actions,
                       float duration, bool talk = true, float talkDur = 0.0f)
    {
        if (!_port || text.empty()) return;
        if (_port->BubbleAwaitingChoice()) return;
        PetBubbleQueue::Bubble b;
        b.text = text; b.shown = shown; b.options = options; b.actions = actions;
        b.duration = duration; b.talk = talk; b.talkDur = talkDur;
        if (_bubbleQueue.Offer(b, _perfClock, Cfg("bubble_min_secs"))) PresentBubble(b);
    }
    void ClearBubbleDisplay()
    {
        _bubbleQueue.Clear();
        if (_port) _port->HideBubble();
    }
    PetIntegration _integration;
    struct CfgDef { const char* key; csmFloat32 def, lo, hi; const char* label; };
    static const CfgDef* CfgDefs(size_t* n)
    {
        static const CfgDef k[] = {
            { "lines", 1, 0, 1, "即时台词气泡" },          { "sleep", 1, 0, 1, "会犯困、睡觉" },
            { "seek", 1, 0, 1, "寂寞时主动找你" },         { "native_expr", 1, 0, 1, "借用模型自带表情" },
            { "reactivity", 1, 0.3f, 2, "情绪敏感度" },     { "expr_gain", 1, 0, 2, "表情幅度" },
            { "sleep_after", 150, 60, 900, "无人理会多久犯困（秒）" }, { "idle", 2, 0, 3, "自主小动作" },
            { "zoom", 1, 0.5f, 2, "大小" },                 { "opacity", 1, 0.2f, 1, "不透明度" },
            { "top", 1, 0, 1, "总在最前" },                 { "passthrough", 0, 0, 1, "鼠标穿透" },
            { "lock", 0, 0, 1, "锁定大小和位置" },
            // 窗口位置（拖动/缩放后由桌宠端口自动保存；-1 = 没存过 → 默认贴工作区左下角）
            { "pos_l", -1, -1, 20000, "窗口左边 x（像素）" }, { "pos_b", -1, -1, 20000, "窗口底边距屏幕底（像素，可为负）" },
            { "pos_set", 0, 0, 1, "位置已记录（内部：区分「还没存过」和「负坐标」）" },
            { "llm_idle", 1, 0, 1, "LLM 闲时表演" },        { "llm_period", 60, 20, 600, "闲时表演间隔（秒）" },
            { "llm_token_tier", 1, 0, 3, "Token 节省档位" },
            { "llm_questions", 1, 0, 1, "LLM 可以提问" },
            // 连续工作多久提醒一次（分钟；0 = 关）；天气闲聊（0/1，位置在字符串配置 weather_loc）
            { "work_remind", 60, 0, 240, "连续工作提醒（分钟，0=关）" },
            { "weather", 0, 0, 1, "天气闲聊" },
            // 气泡文字大小（像素）：Windows 上默认字号常显得偏小/发虚，给一个可调项
            { "bubble_min_secs", 3, 1, 10, "气泡最短显示时间" },
            { "bubble_px", 14, 10, 40, "气泡字号（像素）" },
            // 气泡与角色轮廓的最小间距（像素，随缩放）——气泡锚点不再按"身高百分比"算（那会让
            // 小圆点埋进头发），而是轮廓顶往上留这个间距；见两个端口的 AnchorBubble
            { "bubble_gap", 0, 0, 60, "气泡与角色间距（像素）" },
            { "ctl_port", 23333, 1024, 65535, "本地接入端口（重启生效）" },
            { "companion", 1, 0, 1, "外部消息陪伴" },
            { "companion_motion", 1, 0, 1, "陪伴小动作" },
            { "companion_interval", 30, 10, 120, "进度提醒间隔（秒）" },
        };
        *n = sizeof(k) / sizeof(k[0]);
        return k;
    }
    csmFloat32 Cfg(const char* key) const
    {
        std::map<std::string, csmFloat32>::const_iterator it = _cfg.find(key);
        return it != _cfg.end() ? it->second : 0.0f;
    }
    std::string CfgPath() const
    {
        return plat::HomeDir() + "/.config/l2dpet/config.json";
    }
    /** 配置目录（config.json / memory / questions … 所在；面板"打开配置目录"用）*/
    static std::string ConfigDirPath()
    {
        return PathNative(plat::HomeDir() + "/.config/l2dpet");
    }
    /** 用户模型库目录（托盘/右键菜单里可选的模型；MainLoop 扫描与面板"打开模型目录"同一套约定）*/
    static std::string ModelsLibDir()
    {
        return PathNative(plat::HomeDir() + "/.local/share/l2dpet/models");
    }
    /** Windows 上把分隔符统一成 '\'：这两个目录会显示在面板上，混着 '/' 难看（两种都能用）*/
    static std::string PathNative(std::string p)
    {
#if defined(_WIN32)
        for (size_t i = 0; i < p.size(); ++i) { if (p[i] == '/') p[i] = '\\'; }
#endif
        return p;
    }
    void CfgLoad();
    void CfgSave()
    {
        // 兜底：没把配置读上来就存盘 = 把主人的设置写成空表。
        // （踩过：切换模型原地重建播放器后紧接着 CfgSave 记模型名，config.json 里只剩 str.model。）
        if (!_cfgLoaded) return;
        const std::string path = CfgPath();
        const std::string dir = path.substr(0, path.rfind('/'));

        plat::CreateDir(dir, true);
        FILE* f = fopen((path + ".tmp").c_str(), "wb");

        if (f == NULL) return;
        fprintf(f, "%s\n", CfgJson(true).c_str());   // 存盘一定带 str 段（setstr 可能在 CfgLoad 之前到）
        fclose(f);
        plat::ReplaceFile(path + ".tmp", path);
    }
    /** 配置 JSON。alwaysStr=true（存盘用）时一定带 "str" 段：`setstr` 可能在 CfgLoad 之前到，
     *  那时若省略 str，存盘会把主人设的 lang/theme 丢掉。回执里则用 _cfgLoaded 当"配置读好了"的信号：
     *  agent 启动时靠它判断能不能拿到语言（桌宠第一帧后 1.5s 才读配置，之前读到的都是默认值）。 */
    std::string CfgJson(bool alwaysStr = false) const
    {
        std::string o = "{";

        for (std::map<std::string, csmFloat32>::const_iterator it = _cfg.begin(); it != _cfg.end(); ++it)
        {
            if (o.size() > 1) o += ",";
            JStr(o, it->first);
            o += ":" + (it->first == "ctl_port" ? std::to_string((int)it->second) : JNum(it->second));
        }
        if (!_cfgStr.empty() || alwaysStr || _cfgLoaded)
        {
            // 面板/agent 从 state.config.str 读位置等字符串（与数值键分开放，避免解析歧义）
            // 注意逗号要**有条件**加：数值段为空（全新安装、只设过 lang/theme 这类字符串键）时
            // 会生成 {,"str":{…}} 这种坏 JSON——agent 解析不到 config，启动就一直按默认语言生成（踩过）
            if (o.size() > 1) o += ",";
            o += "\"str\":{";
            bool first = true;
            for (std::map<std::string, std::string>::const_iterator it = _cfgStr.begin(); it != _cfgStr.end(); ++it)
            {
                if (!first) o += ",";
                first = false;
                JStr(o, it->first);
                o += ":";
                JStr(o, it->second);
            }
            o += "}";
        }
        return o + "}";
    }
    /** 字符串配置读（没有返回 def） */
    std::string CfgStr(const char* key, const char* def = "") const
    {
        std::map<std::string, std::string>::const_iterator it = _cfgStr.find(key);
        return it != _cfgStr.end() && !it->second.empty() ? it->second : std::string(def);
    }
    /** 字符串配置写（键 ≤32 字节、值 ≤200 字节；空值 = 删掉该键） */
    void CfgStrSet(const std::string& key, const std::string& value)
    {
        if (key.empty() || key.size() > 32) return;
        size_t cut = value.size() > 200 ? 200 : value.size();
        while (cut > 0 && cut < value.size() && ((unsigned char)value[cut] & 0xC0) == 0x80) --cut;
        if (cut == 0) _cfgStr.erase(key);
        else _cfgStr[key] = value.substr(0, cut);
    }
    /** 设一项（钳到范围）；返回 false = 未知键 */
    bool CfgSet(const std::string& key, csmFloat32 v)
    {
        if (key == "ctl_port" && (!std::isfinite(v) || v < 1024 || v > 65535 || v != std::floor(v))) return false;
        size_t n = 0;
        const CfgDef* d = CfgDefs(&n);

        for (size_t i = 0; i < n; ++i)
        {
            if (key == d[i].key)
            {
                _cfg[key] = v < d[i].lo ? d[i].lo : (v > d[i].hi ? d[i].hi : v);
                return true;
            }
        }
        return false;
    }
    void CfgApply()
    {
        peti18n::SetLangCode(CfgStr("lang", "zh"));   // 自带台词/动态文案的语言（面板切 lang 时跟着换）
        _cfgLines = Cfg("lines") > 0.5f;
        _cfgSleep = Cfg("sleep") > 0.5f;
        _cfgSeek = Cfg("seek") > 0.5f;
        _cfgNativeExpr = Cfg("native_expr") > 0.5f;
        _cfgReactivity = Cfg("reactivity");
        _cfgExprGain = Cfg("expr_gain");
        _cfgSleepAfter = Cfg("sleep_after");
        SetIdleLevel((csmInt32)(Cfg("idle") + 0.5f));
        {
            // 缩放：桌宠 = 整窗缩放（窗口跟着变、模型永不裁剪）；其它端口 = 渲染器内部缩放
            const csmFloat32 uiZoom = Cfg("zoom");

            if (_port != NULL && _port->Name() != NULL
                && (strcmp(_port->Name(), "pet") == 0 || strcmp(_port->Name(), "glfwpet") == 0))
            {
                // 首次显示前记住 / 配置来得晚时补上（见 PortX11Pet::SetSavedPlacement）。
                // 兼容旧配置：以前没有 pos_set，那时能存下的坐标一定非负 → 一对非负值就当作"存过"
                const bool posSet = Cfg("pos_set") > 0.5f
                    || (Cfg("pos_l") >= 0.0f && Cfg("pos_b") >= 0.0f);
                _port->SetSavedPlacement((int)Cfg("pos_l"), (int)Cfg("pos_b"), posSet);
                _port->SetZoom(uiZoom);
                SetRenderZoom(1.0f);   // 渲染始终满幅（端口负责整体缩放）
            }
            else
            {
                SetRenderZoom(uiZoom);
            }
        }
        if (!_cfgNativeExpr && _emoExprIdx >= 0 && _expressionIndex == _emoExprIdx)
        {
            ClearExpression();
            _emoExprIdx = -1;
        }
    }
    void NoteZoom(csmFloat32 z)
    {
        if (!_cfg.empty() && CfgSet("zoom", z))
        {
            _cfgDirtyAt = _perfClock;   // 滚轮连续滚：停 1s 再存
        }
    }
    /** 面板状态（约 2Hz 推给 pet port） */
    /** LLM 连接状态（面板"LLM 已连接"只认这个）：
     *  no_agent 没有 agent / no_key 没配 Key / pending 有 Key 但还没调用过 / error 最近一次调用失败 / ok 最近一次调用成功。
     *  光有 agent 进程（内置 agent 总在跑）不算连接——新电脑没配 LLM 时以前就误显示"已连接"。 */
    std::string LlmStateJson(int agents) const
    {
        std::string state = "no_agent", err;
        if (g_builtinAgent != NULL && g_builtinAgent->Running())
        {
            // 内置 agent：同进程，直接看 LlmClient 记录的最近一次真实请求
            const l2d::LlmStatus st = l2d::LlmLastStatus();
            if (l2d::LlmLoadSettings().key.empty()) state = "no_key";
            else if (st.state == 1) state = "ok";
            else if (st.state == 2) { state = "error"; err = st.err; }
            else state = "pending";
        }
        else if (agents > 0)
        {
            // 外部 agent（pet_agent.py 等）：看它上报的 agent_info（有没有 Key）和最近一条调用日志的成败
            jsonlite::Value ai;
            const bool haveInfo = jsonlite::parse(_agentInfo.c_str(), _agentInfo.size(), ai, NULL) && ai.isObj();
            const std::string ks = haveInfo ? std::string(ai.get("key_source").strOr("none")) : std::string("none");
            state = "pending";
            if (haveInfo && (ks.empty() || ks == "none")) state = "no_key";
            for (auto it = _llmLog.rbegin(); it != _llmLog.rend() && state != "no_key"; ++it)
            {
                jsonlite::Value e;
                if (!jsonlite::parse(it->c_str(), it->size(), e, NULL) || !e.isObj() || !e.get("ok").isBool()) continue;
                state = e.get("ok").b ? "ok" : "error";
                if (!e.get("ok").b) err = e.get("err").strOr("");
                break;
            }
        }
        std::string o = ",\"llm_state\":";
        JStr(o, state);
        o += ",\"llm_err\":";
        JStr(o, plat::Utf8Head(err, 100));
        return o;
    }
    std::string PanelStateJson(int agents);
    // ================================================================ LLM 接入与交互记录（面板"对话"页）
    // 密钥/模型/接口地址存 ~/.config/l2dpet/llm.json（权限 0600），由 agent 读取（环境变量优先）。
    // 任何回执、事件、状态里都只给"是否已设 + 末 4 位"，不回显密钥。
    std::string LlmCfgPath() const
    {
        return plat::HomeDir() + "/.config/l2dpet/llm.json";
    }
    void LlmCfgLoad()
    {
        _llmActive.clear();
        _llmProviders.clear();
        l2d::LlmLoadProviders(&_llmActive, &_llmProviders);   // v2；v1 文件自动迁移成单供应商
    }
    /** 当前激活的供应商（没有供应商/active 对不上时兜底第一个） */
    l2d::LlmProvider* ActiveLlmProvider()
    {
        if (_llmProviders.empty()) return NULL;
        for (size_t i = 0; i < _llmProviders.size(); ++i)
        {
            if (_llmProviders[i].id == _llmActive) return &_llmProviders[i];
        }
        return &_llmProviders.front();
    }
    bool LlmCfgSave()
    {
        const std::string path = LlmCfgPath();
        const std::string dir = path.substr(0, path.rfind('/'));
        plat::CreateDir(dir, true);
        std::string o = "{\"v\":2,\"active\":";
        JStr(o, _llmActive);
        o += ",\"providers\":[";
        for (size_t i = 0; i < _llmProviders.size(); ++i)
        {
            const l2d::LlmProvider& p = _llmProviders[i];
            o += i ? ",{" : "{";
            o += "\"id\":";
            JStr(o, p.id);
            o += ",\"name\":";
            JStr(o, p.name);
            o += ",\"base_url\":";
            JStr(o, p.baseUrl);
            o += ",\"api_key\":";
            JStr(o, p.apiKey);
            o += ",\"model\":";
            JStr(o, p.model);
            o += ",\"models\":[";
            for (size_t k = 0; k < p.models.size(); ++k)
            {
                if (k) o += ",";
                JStr(o, p.models[k]);
            }
            o += "]}";
        }
        o += "]}\n";
        // 私有原子写（0600 + tmp→rename）：与存档/记忆同一套（plat::WritePrivateFile）
        return plat::WritePrivateFile(path, o);
    }
    /** 公开信息（不含密钥）：v2 全量（供应商列表带 key_set/key_tail）+ 顶部兼容字段（= 激活供应商） */
    std::string LlmPublicJson() const;
    std::string LlmStatsJson() const
    {
        char b[256];
        snprintf(b, sizeof(b), "{\"calls\":%ld,\"errors\":%ld,\"questions\":%ld,\"tok_in\":%ld,\"tok_out\":%ld,\"avg_ms\":%.0f",
                 _llmCalls, _llmErrors, _llmQuestions, _llmTokIn, _llmTokOut,
                 _llmCalls > 0 ? _llmMsSum / (double)_llmCalls : 0.0);
        std::string o = b;
        const l2d::LlmTokenStats tokens = l2d::LlmGetTokenStats();
        o += ",\"last_tok_in\":" + std::to_string(tokens.last.tokIn)
            + ",\"last_tok_out\":" + std::to_string(tokens.last.tokOut)
            + ",\"session_tok_in\":" + std::to_string(tokens.total.tokIn)
            + ",\"session_tok_out\":" + std::to_string(tokens.total.tokOut);
        o += ",\"kinds\":{";
        bool first = true;
        for (std::map<std::string, long>::const_iterator it = _llmKinds.begin(); it != _llmKinds.end(); ++it)
        {
            if (!first) o += ",";
            first = false;
            JStr(o, it->first);
            o += ":" + std::to_string(it->second);
        }
        o += "},\"lines_at\":";
        JStr(o, _linesAt);              // 台词库上次成功更新（时间 + 组/句数），失败另给
        o += ",\"lines_note\":";
        JStr(o, _linesNote);
        o += ",\"lines_fail_at\":";
        JStr(o, _linesFailAt);
        o += ",\"lines_fail_err\":";
        JStr(o, _linesFailErr);
        return o + "}";
    }
    // ================================================================ 连续情绪（参考 soullink-emotion-sdk 的 VAD 思路）
    // 情绪不是"切一个表情"，而是一个连续状态：V 愉悦 / A 唤醒 / D 支配，各 -1..1。
    //  - 事件只"推"目标（nudge：按强度往某个情绪原型靠），当前值指数逼近目标；
    //  - 目标保持一段时间（hold）后向**性格基线**回落；基线随内在状态变（亲密高→偏愉悦，困→低唤醒）；
    //  - 每帧把 VAD 连续映射成面部/头身的**叠加层**（嘴角、眉、眼、头姿、脸红），在动作/眨眼之上累加；
    //  - 强度高时复用模型自带的表情文件做"峰值"（只清自己设的那个，LLM 指定的表情不动）；
    //  - VAD 变化瞬间给一个小的头部微动脉冲（"一怔""一缩"）。
    struct Vad { csmFloat32 v, a, d; };
    static const Vad* EmotionPreset(const std::string& name)
    {
        static const struct { const char* n; Vad p; } k[] = {
            { "neutral", { 0.0f, 0.0f, 0.0f } },        { "calm", { 0.25f, -0.45f, 0.2f } },
            { "happy", { 0.75f, 0.45f, 0.35f } },       { "excited", { 0.85f, 0.85f, 0.45f } },
            { "shy", { 0.35f, 0.6f, -0.45f } },         { "affectionate", { 0.65f, 0.1f, 0.1f } },
            { "curious", { 0.35f, 0.55f, 0.2f } },      { "confused", { -0.1f, 0.35f, -0.3f } },
            { "tired", { -0.25f, -0.7f, -0.3f } },      { "sad", { -0.65f, -0.45f, -0.5f } },
            { "anxious", { -0.6f, 0.7f, -0.55f } },     { "angry", { -0.7f, 0.75f, 0.55f } },
            { "surprised", { 0.18f, 0.78f, -0.08f } },  { "sulky", { -0.45f, 0.2f, 0.35f } },
        };
        static const struct { const char* zh; const char* en; } alias[] = {
            { "开心", "happy" }, { "高兴", "happy" }, { "兴奋", "excited" }, { "害羞", "shy" },
            { "亲昵", "affectionate" }, { "好奇", "curious" }, { "困惑", "confused" }, { "困", "tired" },
            { "难过", "sad" }, { "伤心", "sad" }, { "紧张", "anxious" }, { "生气", "angry" },
            { "惊讶", "surprised" }, { "平静", "calm" }, { "赌气", "sulky" }, { "anger", "angry" },
            { "anxiety", "anxious" }, { "surprise", "surprised" },
        };
        std::string n = name;

        for (size_t i = 0; i < sizeof(alias) / sizeof(alias[0]); ++i)
        {
            if (n == alias[i].zh) { n = alias[i].en; break; }
        }
        for (size_t i = 0; i < sizeof(k) / sizeof(k[0]); ++i)
        {
            if (n == k[i].n) return &k[i].p;
        }
        return NULL;
    }
    static csmFloat32 ClampS(csmFloat32 x) { return x < -1.0f ? -1.0f : (x > 1.0f ? 1.0f : x); }
    /** 推一下情绪：name = 原型名（中英皆可），intensity 0..1 */
    bool EmotionNudge(const std::string& name, csmFloat32 intensity)
    {
        const Vad* p = EmotionPreset(name);

        if (p == NULL)
        {
            return false;
        }
        intensity = Clamp01(intensity);
        csmFloat32 amt = (0.28f + intensity * 0.58f) * _cfgReactivity;
        amt = amt > 0.96f ? 0.96f : amt;
        _emoTgt.v = ClampS(_emoTgt.v + (p->v - _emoTgt.v) * amt);
        _emoTgt.a = ClampS(_emoTgt.a + (p->a - _emoTgt.a) * amt);
        _emoTgt.d = ClampS(_emoTgt.d + (p->d - _emoTgt.d) * amt);
        const csmFloat32 hold = 4.0f + intensity * 12.0f;
        _emoHold = _emoHold > hold ? _emoHold : hold;
        _emoLastNudge = name;
        return true;
    }
    std::string EmotionDominant(const Vad& x, csmFloat32 mag) const
    {
        if (mag < 0.06f) return "neutral";
        if (mag < 0.2f)   // 轻微：只按愉悦/唤醒的大方向给"淡"情绪名（soullink 的 soft-* 思路）
        {
            if (x.v > 0.04f) return x.a > 0.04f ? "content" : "calm";
            if (x.v < -0.04f) return x.a > 0.04f ? "uneasy" : "low";
            return x.a > 0.04f ? "curious" : (x.a < -0.04f ? "calm" : "neutral");
        }
        if (x.v > 0.12f && x.d < -0.22f) return "shy";
        if (x.v < -0.34f && x.a > 0.38f && x.d < -0.12f) return "anxious";
        if (x.v < -0.42f && x.a > 0.42f && x.d > 0.18f) return "angry";
        if (x.v > 0.58f && x.a > 0.62f) return "excited";
        if (x.v > 0.2f && x.a < -0.24f) return "calm";
        static const char* const kN[] = { "calm", "happy", "excited", "shy", "affectionate", "curious", "confused",
                                          "tired", "sad", "anxious", "angry", "surprised", "sulky" };
        std::string best = "neutral";
        csmFloat32 bd = 0.92f;

        for (size_t i = 0; i < sizeof(kN) / sizeof(kN[0]); ++i)
        {
            const Vad* p = EmotionPreset(kN[i]);
            const csmFloat32 dv = x.v - p->v, da = x.a - p->a, dd = x.d - p->d;
            const csmFloat32 dist = dv * dv * 1.08f + da * da * 0.88f + dd * dd * 1.28f;

            if (dist < bd) { bd = dist; best = kN[i]; }
        }
        return best;
    }
    static csmFloat32 VadMag(const Vad& x)
    {
        const csmFloat32 m = (fabsf(x.v) + fabsf(x.a) * 0.82f + fabsf(x.d) * 0.64f) / 2.46f;
        return m > 1.0f ? 1.0f : m;
    }
    /** 每帧（ApplyBodyChannelsPre 里、动作叠加之后）：更新 VAD → 写面部/头身叠加层 */
    void EmotionTick(csmFloat32 dt);
    std::string EmotionJson() const
    {
        char b[200];

        snprintf(b, sizeof(b), "{\"name\":\"%s\",\"intensity\":%.2f,\"v\":%.2f,\"a\":%.2f,\"d\":%.2f}",
                 _emoName.c_str(), _emoMag, _emoCur.v, _emoCur.a, _emoCur.d);
        return b;
    }
    // ================================================================ 内在状态（桌宠"活着"）
    // 不靠 LLM 也有自己的节律：精力（会困、会睡、睡饱自己醒）、亲密度（摸头/牵手涨，乱戳降；存盘，
    // 下次启动还记得）、恼火（乱戳涨，慢慢消；高了生闷气、扭头不理）、寂寞（没人理就涨，高了主动找你）。
    // 触摸的反应由当时的状态决定；状态变化推 {"event":"mood"} 给 LLM，status 也带 mood。
    enum LifeState { LS_AWAKE = 0, LS_DROWSY, LS_ASLEEP, LS_SULK };
    static const char* LifeStateKey(int st)
    {
        static const char* const k[] = { "awake", "drowsy", "asleep", "sulk" };
        return (st >= 0 && st <= 3) ? k[st] : "awake";
    }
    /** 台词：台词库键 "life.<key>" 优先，否则内置 */
    std::string LifeLine(const char* key, const char* const* defs, size_t n)
    {
        std::vector<std::string> c;
        std::map<std::string, std::vector<std::string> >::const_iterator it = _reactLines.find(std::string("life.") + key);

        if (it != _reactLines.end())
        {
            c = it->second;
        }
        else
        {
            c.assign(defs, defs + n);
        }
        if (c.empty())
        {
            return std::string();
        }
        size_t k = NextRand() % c.size();

        if (c.size() > 1 && c[k] == _lastReactLine)
        {
            k = (k + 1) % c.size();
        }
        _lastReactLine = c[k];
        return peti18n::Tr(c[k]);   // 内置兜底按语言翻译（台词库下发的句子不在表里 → 原样通过）
    }
    /** 最近动态：kind = state/say/llm/touch/game/emo（面板按 kind 配图标）；
     *  收**最终文案**（自带描述调用方用 peti18n::Tr/TF 翻好，LLM 文案原样） */
    void FeedPush(const char* kind, const std::string& text)
    {
        if (text.empty()) return;
        // 同一句 60 秒内重复（睡着时的 Zzz、反复的抱怨）：只刷新时间，不刷屏
        for (size_t k = _feed.size(), seen = 0; k > 0 && seen < 3; --k, ++seen)
        {
            FeedItem& it = _feed[k - 1];
            if (it.kind == kind && it.text == text && (double)time(NULL) - it.t < 60.0)
            {
                it.t = (double)time(NULL);
                return;
            }
        }
        if (_feed.size() >= 40) _feed.pop_front();
        _feed.push_back(FeedItem{ (double)time(NULL), kind, text });
    }
    /** 触摸动态的整句文案（动词 + 部位拼的，语序差异放进 "{}了{}" 模板里） */
    static std::string TouchFeedText(int z, int kind)
    {
        const char* zl = ZoneLabel(z);
        return peti18n::TF("{}了{}", { peti18n::Tr(TouchVerb(ZoneKey(z), kind == 1)),
                                       peti18n::Tr(zl == NULL ? std::string() : std::string(zl)) });
    }
    /** 触摸进动态：同一部位同一手法 8 秒内合并成"摸了头部 ×3"（动词按部位：戳/牵/捏、揉/摸，见 PetVerbs.h） */
    void FeedTouch(int z, int kind)
    {
        const char* zl = ZoneLabel(z);
        if (zl == NULL) return;
        // 往回找最近的触摸条（中间可能夹着她的即时台词）
        FeedItem* last = NULL;
        for (size_t k = _feed.size(), seen = 0; k > 0 && seen < 4; --k, ++seen)
        {
            if (_feed[k - 1].kind == "touch") { last = &_feed[k - 1]; break; }
            if (_feed[k - 1].kind != "say") break;
        }
        if (z == _feedTouchZone && kind == _feedTouchKind && _perfClock - _feedTouchAt < 8.0f && last != NULL)
        {
            ++_feedTouchN;
            last->text = TouchFeedText(z, kind) + " ×" + std::to_string(_feedTouchN);
            last->t = (double)time(NULL);
        }
        else
        {
            _feedTouchN = 1;
            FeedPush("touch", TouchFeedText(z, kind));
        }
        _feedTouchZone = z;
        _feedTouchKind = kind;
        _feedTouchAt = _perfClock;
    }
    /** 亲密度上涨：越接近满越难涨（边际递减），否则摸几十下就顶满、状态页永远 99 */
    void AffectionUp(csmFloat32 g)
    {
        // 高处递减（≥0.375 起逐渐变小），低处不放大；关系冷淡（< 0.25）时信任要慢慢重建：再减半
        const csmFloat32 k = std::min(1.0f, (1.0f - _lifeAffection) * 1.6f) * (_lifeAffection < 0.25f ? 0.5f : 1.0f);
        _lifeAffection = Clamp01(_lifeAffection + g * k);
    }
    static const char* LifeStateZh(int st)
    {
        return st == LS_ASLEEP ? "睡着了" : (st == LS_DROWSY ? "犯困" : (st == LS_SULK ? "生闷气" : "醒着"));
    }
    /** panel_state.life：状态页的反馈数据 */
    std::string LifePanelJson();
    /** 主人设了"怎么叫我"：本地台词（内置兜底/各语言）里的"主人 / ご主人さま / Master"换成它 */
    std::string ApplyCall(std::string line) const
    {
        if (_personaCall.empty()) return line;
        static const char* const kWords[] = { "ご主人さま", "ご主人様", "主人", "Master", "master" };
        for (const char* w : kWords)
        {
            const std::string k = w;
            for (size_t p = line.find(k); p != std::string::npos; p = line.find(k, p + _personaCall.size()))
            {
                line.replace(p, k.size(), _personaCall);
            }
        }
        return line;
    }
    /** 说一句（气泡 + 动嘴）；talk=false 只出气泡（梦话 Zzz） */
    void LifeSay(const std::string& line0, csmFloat32 hold, bool talk = true)
    {
        const std::string line = ApplyCall(line0);
        if (line.empty() || _port == NULL || _port->BubbleAwaitingChoice() || !_cfgLines)
        {
            return;
        }
        // 这里收的是**最终文案**：内置台词在 LifeLine / PickReactLine / peti18n::TF 里翻好，
        // LLM 与台词库下发的句子本来就是当前语言 → 不再二次翻译（二次翻译会把成品当"未命中"记进清单）
        _lastReactLineAt = _perfClock;
        // 口型不在这里起：气泡可能排队，PresentBubble 到上屏那一刻才开口（talk=false 的梦话不出声）
        ShowPetBubble(line, {}, {}, {}, hold, talk);
        if (talk && _lifeState != LS_ASLEEP) FeedPush("say", line);   // 梦话/Zzz 不进动态
        printf("[life] %s「%s」\n", LifeStateKey(_lifeState), line.c_str());
        fflush(stdout);
    }
#define LIFE_SAY(key, hold, ...) do { static const char* const _d[] = { __VA_ARGS__ }; \
        LifeSay(LifeLine(key, _d, sizeof(_d) / sizeof(_d[0])), hold); } while (0)

    /** 精力低于这个 = 累了：再互动就掉亲密度 */
    static constexpr csmFloat32 kTiredEnergy = 0.3f;
    /** 醒着时的精力平衡点：低于它慢慢回升，高于它慢慢落回（见 PetLifeTick）。
     *  满格只能睡出来——醒着再怎么待机也只回到这里，所以"陪她玩"能真正把她玩累 */
    static constexpr csmFloat32 kAwakeEnergyCeil = 0.65f;
    /** 一次互动的代价：先扣精力；如果她**已经累了**（扣之前就 < kTiredEnergy），这次互动还要
     *  掉亲密度（不走边际递减，线性 -0.02）、涨一点恼火，并抱怨一句（8 秒内最多一句）。
     *  返回 true = 她累了（调用方据此不再做正常的开心反应） */
    bool InteractCost(csmFloat32 cost, const char* what)
    {
        if (cost > 0.0f)
        {
            // 短时间连续玩耍叠加：4 秒内接着玩算连击，每一多下 +25%，最多翻倍（别叠加太高）；
            // 停手超过 4 秒就清零，慢慢陪她玩不受影响
            _playCombo = (_perfClock - _playComboAt <= 4.0f) ? _playCombo + 1 : 1;
            _playComboAt = _perfClock;
            cost *= std::min(1.0f + 0.25f * (csmFloat32)(_playCombo - 1), 2.0f);
        }
        const bool tired = _lifeEnergy < kTiredEnergy;
        _lifeEnergy = Clamp01(_lifeEnergy - cost);
        if (!tired) return false;
        _lifeAffection = Clamp01(_lifeAffection - 0.02f);
        _lifeAnnoy = Clamp01(_lifeAnnoy + 0.06f);
        EmotionNudge("tired", 0.7f);
        // 累了还被闹到恼火 70% → 生气不理人（之后亲密度不再往下掉——她已经背过身去了）
        if (_lifeState == LS_AWAKE && _lifeAnnoy >= 0.7f)
        {
            FeedPush("state", peti18n::Tr("累了还被闹，生气了（亲密度 -2）"));
            LifeEnter(LS_SULK, "累了还被闹");
            StartAction("angry", 1.0f, 0.8f, 1);
            LIFE_SAY("sulk_tired", 3.0f, "都说了好累！不理你了！", "哼！人家都没力气了还闹！", "讨厌……让我休息啦！");
            _tiredSayAt = _perfClock;
            return true;
        }
        if (_perfClock - _tiredSayAt > 8.0f)
        {
            _tiredSayAt = _perfClock;
            FeedPush("state", peti18n::TF("累了还被{}（亲密度 -2）", { peti18n::Tr(what) }));
            LIFE_SAY("tired", 2.6f, "好累……让我歇会儿嘛", "呜……没力气了……", "人家真的好困……别闹了",
                     "再闹我就不理你了……");
        }
        return true;
    }
    void LifePushMood(const char* why)
    {
        char b[256];

        snprintf(b, sizeof(b), "{\"event\":\"mood\",\"state\":\"%s\",\"why\":\"%s\",\"energy\":%.2f,"
                 "\"affection\":%.2f,\"annoy\":%.2f,\"lonely\":%.2f,\"emotion\":", LifeStateKey(_lifeState), why,
                 _lifeEnergy, _lifeAffection, _lifeAnnoy, _lifeLonely);
        _petEvents.push_back(std::string(b) + EmotionJson() + "}");
    }
    std::string LifeMoodJson() const
    {
        char b[200];

        snprintf(b, sizeof(b), "{\"state\":\"%s\",\"energy\":%.2f,\"affection\":%.2f,\"annoy\":%.2f,"
                 "\"lonely\":%.2f,\"user_idle\":%.0f,\"emotion\":", LifeStateKey(_lifeState), _lifeEnergy,
                 _lifeAffection, _lifeAnnoy, _lifeLonely, _userIdleT);
        return std::string(b) + EmotionJson() + "}";
    }
    /** 状态切换：先放掉上一个状态的姿态，再摆新状态的 */
    void LifeEnter(int st, const char* why)
    {
        if (st == _lifeState)
        {
            return;
        }
        static const char* const kHeld[] = { "eye.l", "eye.r", "head.x", "head.y", "head.z", "body.x",
                                             "brow.l", "brow.r", "mouth.form" };
        for (size_t i = 0; i < sizeof(kHeld) / sizeof(kHeld[0]); ++i)
        {
            const int c = FindBodyChannel(kHeld[i]);

            if (c >= 0 && !_channels[(size_t)c].fromScene)
            {
                ReleaseBodyChannel(_channels[(size_t)c]);
            }
        }
        if (st == LS_ASLEEP && _emoExprIdx >= 0 && _expressionIndex == _emoExprIdx)
        {
            ClearExpression();
            _emoExprIdx = -1;
        }
        printf("[life] %s → %s（%s）精力 %.2f 亲密 %.2f 恼火 %.2f 寂寞 %.2f\n", LifeStateKey(_lifeState),
               LifeStateKey(st), why, _lifeEnergy, _lifeAffection, _lifeAnnoy, _lifeLonely);
        fflush(stdout);
        FeedPush("state", peti18n::TF("{}（{}）", { peti18n::Tr(LifeStateZh(st)), peti18n::Tr(why) }));
        _lifeState = st;
        _lifeStateAt = _perfClock;
        _lifeStirs = 0;
        if (st == LS_SULK)
        {
            EmotionNudge("angry", 0.9f);
        }
        LifePushMood(why);
    }
    std::string LifePath() const
    {
        return plat::HomeDir() + "/.local/state/l2dpet/" + FingerprintText() + ".json";
    }
    void LifeLoad();
    void LifeSave()
    {
        const std::string path = LifePath();
        const std::string dir = path.substr(0, path.rfind('/'));

        plat::CreateDir(dir, true);
        FILE* f = fopen((path + ".tmp").c_str(), "wb");

        if (f == NULL)
        {
            return;
        }
        fprintf(f, "{\"affection\":%.3f,\"touches\":%ld,\"last_seen\":%.0f,\"first_seen\":%.0f,\"rps\":[%ld,%ld,%ld],\"zones\":[",
                _lifeAffection, _lifeTouches, (double)time(NULL), _firstSeen, _rps[0], _rps[1], _rps[2]);
        for (int z = 0; z < ZONE_COUNT; ++z) fprintf(f, "%s[%ld,%ld]", z ? "," : "", _zoneLife[z][0], _zoneLife[z][1]);
        fprintf(f, "]}\n");
        fclose(f);
        plat::ReplaceFile(path + ".tmp", path);
    }
    static csmFloat32 Clamp01(csmFloat32 v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }
    /** 触摸：按状态处理。返回 true = 已完全处理（不走默认区域反应） */
    bool LifeTouch(int z, int kind, int n, csmFloat32 x, csmFloat32 y);
    /** 每帧：节律 + 状态机 + 自主行为。raw* = 全局指针（不论离人物远近），near = 指针在人物附近 */
    void PetLifeTick(csmFloat32 dt, bool rawHas, csmFloat32 px, csmFloat32 py, bool near);
    /** 渲染出的人物轮廓包围盒（alpha > 24 的像素；渲染目标坐标）；没有帧缓冲返回 false */
    bool SilhouetteBBox(csmFloat32* bb);
    /** 渲染像素换算系数：交互阈值按 360 宽为 1×（高分屏/大窗口渲染时等比放大）*/
    csmFloat32 PxK() const { return _targetW > 0.0f ? _targetW / 360.0f : 1.0f; }
    /** 点在人物附近（艺术包围盒外扩 margin 像素内；渲染目标坐标）——视线只在这时跟鼠标 */
    bool PointNearModel(csmFloat32 x, csmFloat32 y, csmFloat32 margin)
    {
        if (!_modelBBoxValid)
        {
            return false;
        }
        csmFloat32 bb[4];

        ComputeArtBBox(bb);
        return bb[2] >= bb[0] && x >= bb[0] - margin && x <= bb[2] + margin && y >= bb[1] - margin
               && y <= bb[3] + margin;
    }
    /** 放开视线：回到"看前方"，之后由自主待机东张西望 */
    void ReleaseLook()
    {
        _followHasTarget = false;
    }
    csmInt32 HandleTapZ(csmInt32 x, csmInt32 y, const char** zoneOut);
    /** 兼容入口：不带区域输出 */
    csmInt32 HandleTap(csmInt32 x, csmInt32 y)
    {
        return HandleTapZ(x, y, NULL);
    }
    csmInt32 IdleLevel() const { return _idleLevel; }
    void SetIdleLevel(csmInt32 lv)
    {
        _idleLevel = lv < 0 ? 0 : (lv > 3 ? 3 : lv);
    }
    /** 演出状态（STATUS 用）*/
    csmInt32 PendingSceneCount() const { return (csmInt32)_scenes.size(); }
    csmFloat32 CurrentSceneRemain() const
    {
        if (_scenes.empty())
        {
            return 0.0f;
        }
        const csmFloat32 rel = _perfClock - _scenes.front().beginAt;
        const csmFloat32 remain = _scenes.front().dur - rel;
        return remain > 0.0f ? remain : 0.0f;
    }
    /** 演出脚本（JSON；对象=单个，数组=队列）→ 入队执行 */
    csmBool PlayScriptJson(const char* text, csmInt32 len, char* errbuf, size_t errcap)
    {
        jsonlite::Value root;
        std::string perr;

        if (!jsonlite::parse(text, (size_t)len, root, &perr))
        {
            snprintf(errbuf, errcap, "JSON 解析失败：%s", perr.c_str());
            return false;
        }
        bool ok = false;

        if (root.isArr())
        {
            for (size_t i = 0; i < root.arr.size(); ++i)
            {
                ok |= ParseScene(root.arr[i], errbuf, errcap);
            }
        }
        else if (root.isObj())
        {
            ok = ParseScene(root, errbuf, errcap);
        }
        else
        {
            snprintf(errbuf, errcap, "根节点不是对象/数组");
        }
        if (ok)
        {
            NoteExternalActivity();
        }
        return ok;
    }
    /** 能力缓存路径（模型目录下；随模型走，换卡换库都不串）*/
    std::string CachePath() const { return _dir + "/l2d.capability.json"; }
    /** FNV-1a 64 混入（模型指纹用）*/
    static void MixBytes(csmUint64* h, const void* data, size_t n)
    {
        const unsigned char* p = (const unsigned char*)data;

        for (size_t i = 0; i < n; ++i)
        {
            *h ^= (csmUint64)p[i];
            *h *= 1099511628211ull;
        }
    }
    static void MixInt(csmUint64* h, csmUint64 v)
    {
        for (int i = 0; i < 8; ++i)
        {
            *h ^= (v >> (i * 8)) & 0xffu;
            *h *= 1099511628211ull;
        }
    }
    /** 模型指纹：参数 id 序列 + 参数/drawable 规模（换模型/换模型包必变）。
     *  外部把 caps 的 FP= 记下，指令行首带 "@<fp> " 即可防"发给 A 的指令落到 B"。*/
    csmUint64 ComputeFingerprint() const
    {
        csmUint64 h = 1469598103934665603ull;

        MixInt(&h, (csmUint64)_model->GetParameterCount());
        MixInt(&h, (csmUint64)_model->GetDrawableCount());
        for (csmInt32 i = 0; i < _model->GetParameterCount(); ++i)
        {
            const char* pid = _model->GetParameterId((csmUint32)i)->GetString().GetRawString();

            MixBytes(&h, pid, strlen(pid));
            MixInt(&h, 0x0au);   // 分隔符：防 "ab"+"c" 与 "a"+"bc" 撞
        }
        return h;
    }
    csmUint64 Fingerprint() const { return _fingerprint; }
    /** 指纹文本（"0x…16 位"；caps 的 FP= 与 @ 前缀都用它）*/
    std::string FingerprintText() const
    {
        char buf[24];

        snprintf(buf, sizeof(buf), "0x%016llx", (unsigned long long)_fingerprint);
        return std::string(buf);
    }
    /** 写能力缓存（JSON；原子：tmp + rename）。探测跑完自动调 */
    csmBool SaveCapabilityCache();
    /** 读能力缓存（指纹对不上 = 模型换过包，忽略并清零重测）。返回载入项数 */
    csmInt32 LoadCapabilityCache();
    /** 能力表（写入指令环 caps 区；宿主据此应答 LIST——它不解析 model3.json）*/
    void BuildCapabilityText(char* out, size_t cap);
    /** 全量参数表（写指令环 params 区；PARAMS 查询用）：
     *  P <id> <默认> <最小> <最大> <组名> <中文名> eff=<码>[:<顶点位移>]（中文/组名来自 cdi3，缺则 "-"；
     *  eff 来自能力探测/缓存：d=顶点直连 p=经物理 a=换美术 -=无效果 ?=未探测）*/
    void BuildParamTable(char* out, size_t cap);
    /** UI 分项耗时（L2D_DBG_PROF=1 时统计；生产路径零开销） */
    static bool UiProfOn() { static const bool on = (getenv("L2D_DBG_PROF") != NULL); return on; }
    static double* UiStat() { static double s[4] = {0, 0, 0, 0}; return s; }
    static long& UiCount() { static long c = 0; return c; }
    static double UiNow() { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec*1e-9; }
    /** 右上角 FPS + 底部缩放条（在模型/球之后绘制，位于最上层） */
    void DrawUi();
    ~L2DPlayer()
    {
        for (csmMap<csmString, ACubismMotion*>::const_iterator it = _motions.Begin(); it != _motions.End(); ++it)
        {
            if (it->Second != NULL)
            {
                ACubismMotion::Delete(it->Second);
            }
        }
        _motions.Clear();

        for (csmMap<csmString, ACubismMotion*>::const_iterator it = _expressions.Begin(); it != _expressions.End(); ++it)
        {
            if (it->Second != NULL)
            {
                ACubismMotion::Delete(it->Second);
            }
        }
        _expressions.Clear();

        for (size_t i = 0; i < _textures.size(); ++i)
        {
            delete _textures[i];
        }
        _textures.clear();

        if (_setting != NULL)
        {
            delete _setting;
            _setting = NULL;
        }
    }
    /** 装配软件渲染器：创建渲染目标 + 加载并绑定模型纹理（对应官方 SetupTextures） */
    bool SetupRender(csmUint32 width, csmUint32 height)
    {
        _targetW = (csmFloat32)width;
        _targetH = (csmFloat32)height;
        DeleteRenderer();
        CreateRenderer(width, height);

        CubismRenderer_Software* renderer = GetRenderer<CubismRenderer_Software>();
        if (renderer == NULL)
        {
            fprintf(stderr, "[l2dtest] 软渲染器创建失败\n");
            return false;
        }

        const csmInt32 textureCount = _setting->GetTextureCount();
        for (csmInt32 i = 0; i < textureCount; ++i)
        {
            const char* fileName = _setting->GetTextureFileName(i);
            if (fileName == NULL || strcmp(fileName, "") == 0)
            {
                continue;
            }
            const std::string path = _dir + "/" + fileName;
            SoftTexture* texture = SoftTexture::LoadFromFile(path.c_str());
            if (texture == NULL)
            {
                return false;
            }
            renderer->BindTexture(i, texture);
            _textures.push_back(texture);
        }
        return true;
    }
    /** 官方 mainMinimum 的基础投影（不含缩放） */
    void BuildBaseProjection(CubismMatrix44* projection, csmUint32 width, csmUint32 height)
    {
        projection->LoadIdentity();
        if (GetModel()->GetCanvasWidth() > 1.0f && width < height)
        {
            // 横长模型 → 竖屏：按宽度归一到可见范围（官方 mainMinimum 逻辑）
            GetModelMatrix()->SetWidth(2.0f);
            projection->Scale(1.0f, (csmFloat32)width / (csmFloat32)height);
        }
        else
        {
            projection->Scale((csmFloat32)height / (csmFloat32)width, 1.0f);
        }
    }
    /** 自动适配：用当前可见顶点在屏幕空间的包围盒计算"等比最大填充"的 zoom */
    csmFloat32 ComputeFitZoom(const CubismMatrix44& baseProjection,
                              csmUint32 width, csmUint32 height);
    /** 一帧渲染（官方 Draw 语义：mvp = projection × modelMatrix，随后 DrawModel） */
    void Render();
    /** UI 分项耗时汇总（诊断用） */
    static void PrintUiStats()
    {
        const double* st = UiStat();
        const long n = UiCount();
        if (n <= 0 || getenv("L2D_DBG_PROF") == NULL) { return; }
        printf("[ui-prof] 帧数=%ld 状态栏=%.2fms/帧 圆环=%.2fms/帧 缩放条=%.2fms/帧\n",
               n, st[0]*1000/n, st[1]*1000/n, st[2]*1000/n);
    }
    CubismRenderer_Software* SoftwareRenderer() const
    {
        return const_cast<L2DPlayer*>(this)->GetRenderer<CubismRenderer_Software>();
    }
    /** 渲染目标尺寸（SetupRender 时记录；渲染器 framebuffer 首帧才建立，不能依赖其查询） */
    csmFloat32 RenderTargetWidth() const { return _targetW; }
    csmFloat32 RenderTargetHeight() const { return _targetH; }
    /** 加载 model3.json 及其引用的全部资产，并装配官方更新管线 */
    bool Load(const char* model3Path);
    /** 官方更新管线：motion → scheduler(表情/物理/姿势/眨眼/呼吸) → 球跟随覆盖 → Core 变形 */
    void Update(csmFloat32 dt);
    /**
     * 触摸跟随（BongoCat/官方设计）：只负责把"目标方向"交给 CubismTargetPoint；
     * 速度平滑（0.15s 加速/运动学减速，类似人转头）由官方实现，
     * 参数写入由 scheduler 内的 CubismLookUpdater 按标准 Look 参数表完成
     * （ParamAngleX/Y/Z ±30、ParamBodyAngleX ±10、ParamEyeBallX/Y ±1——
     *  缺哪个参数自动忽略，因此对所有标准模型兼容）。
     * 必须在 _updateScheduler.OnLateUpdate 之前调用（LookUpdater 要读最新目标）。
     */
    void UpdateFollowTarget(csmFloat32 dt);
    // ---- 资产信息（供输出）----
    ICubismModelSetting* Setting() const { return _setting; }
    const csmMap<csmString, ACubismMotion*>& Motions() const { return _motions; }
    const csmChar* FirstMotionGroupName() const
    {
        return _setting->GetMotionGroupCount() > 0 ? _setting->GetMotionGroupName(0) : "";
    }
    bool IsPlayingMotion() const { return _motionManager != NULL && !_motionManager->IsFinished(); }
private:
    void PreloadMotionGroup(const csmChar* group)
    {
        const csmInt32 count = _setting->GetMotionCount(group);
        for (csmInt32 i = 0; i < count; ++i)
        {
            const csmString name = Utils::CubismString::GetFormatedString("%s_%d", group, i);
            const std::string path = _dir + "/" + _setting->GetMotionFileName(group, i);

            csmSizeInt size = 0;
            csmByte* buffer = ReadJsonFile(path.c_str(), &size);
            if (buffer == NULL)
            {
                continue;
            }
            CubismMotion* motion = static_cast<CubismMotion*>(
                LoadMotion(buffer, size, name.GetRawString(), NULL, NULL, _setting, group, i));
            if (motion != NULL)
            {
                if (_motions[name] != NULL)
                {
                    ACubismMotion::Delete(_motions[name]);
                }
                _motions[name] = motion;
            }
            free(buffer);
        }
    }
    void StartFirstMotion()
    {
        const csmChar* group = FirstMotionGroupName();
        if (strcmp(group, "") == 0)
        {
            return;
        }
        const csmString name = Utils::CubismString::GetFormatedString("%s_%d", group, 0);
        ACubismMotion* motion = _motions[name];
        if (motion != NULL)
        {
            _motionManager->StartMotionPriority(static_cast<CubismMotion*>(motion), false, 3 /* PriorityForce */);
        }
    }
    /**
     * 水印控制：找到模型自带的"水印"表情（作者提供的开关——VTS 里按空格触发的就是它），
     * 记下它改哪些参数、怎么改（Blend）。只有带这个表情的模型才动参数，避免和别的模型
     * 里恰好同名的参数误撞（实测库里有模型也有 id 叫 "Param" 的参数，但不是水印）。
     * 判据：表情名含"水印"或文件名为 shuiyin*.exp3.json。
     */
    void SetupWatermarkControl();
    /** 把"水印关"每帧强制生效：放在 scheduler（表情/物理）之后、Core 变形之前，
     *  这样它不随动作/表情回弹，也不会被下一次 LoadParameters 抹掉。 */
    void ApplyWatermarkControl()
    {
        if (!_watermarkOff)
        {
            return;
        }
        for (size_t i = 0; i < _watermarkParams.size(); ++i)
        {
            const WatermarkParam& wp = _watermarkParams[i];
            switch (wp.blend)
            {
            case CubismExpressionMotion::Additive:
                _model->AddParameterValue(wp.id, wp.value, 1.0f);
                break;
            case CubismExpressionMotion::Multiply:
                _model->MultiplyParameterValue(wp.id, wp.value, 1.0f);
                break;
            default: // Overwrite
                _model->SetParameterValue(wp.id, wp.value, 1.0f);
                break;
            }
        }
    }
    // ---------------- 演出引擎私有实现（远程指令 / 手势 / 脚本 / 自主待机）----------------

    /** 参数轨道的一个关键帧（t 归一化 0..1） */
    struct SceneKey
    {
        csmFloat32 t, v;
    };
    /** 一条参数轨道：把某个参数在 [start, start+dur] 内按关键帧推过去 */
    struct SceneTrack
    {
        csmInt32 index;         // 参数序号（-1=无效）
        csmBool additive;       // true=AddParameterValue（叠加，默认）；false=SetParameterValue
        csmFloat32 start;       // 绝对起止时刻（演出时钟，秒）
        csmFloat32 dur;
        csmInt32 keyCount;
        SceneKey keys[8];
        csmFloat32 scale;       // 幅度缩放（自主待机的小动作 <1）
    };
    /** 脚本里的一个步骤 */
    struct SceneStep
    {
        csmFloat32 at;          // 相对脚本开始的秒数
        csmInt32 kind;          // 0=gesture 1=expr(名) 2=look 3=motion 4=expr清空 5=expr(序号) 6=params 姿态 7=说话(talk)
                                // 8=action（动作片段）9=body（躯体通道姿态）10=body 释放
        csmInt32 a, b;          // motion: 组/序号；expr 序号
        csmFloat32 fa, fb;      // look x/y
        csmFloat32 speed;       // gesture 语速
        csmFloat32 scale;       // gesture 幅度缩放（0.1~2.0；模型自绑幅度偏小时放大）
        csmFloat32 dur;         // 缓动时长（params 步用；默认 1s）；talk 步=说话时长
        csmInt32 repeat;
        csmBool fired;
        std::string s;          // gesture/expr/motion 名字；talk 步=文本（可空）
        std::vector<std::pair<csmInt32, csmFloat32> > pose;  // params 步：参数序号→增量
        std::vector<std::pair<std::string, csmFloat32> > body; // body 步：通道名→目标
    };
    struct Scene
    {
        csmFloat32 dur;
        csmBool interrupt;
        csmInt32 idleLevel;
        csmFloat32 beginAt;     // 绝对开始时刻（入队时定）
        std::vector<SceneStep> steps;
    };
    /** 手势定义：一组轨道（start 相对 0）*/
    struct GestureDef
    {
        std::string name;
        std::string expr;   // 可选：手势自带表情（姿态类手势：举话筒/猫爪…）
        csmFloat32 dur;
        std::vector<SceneTrack> tracks;
    };
    void StartExpressionKey(const char* name)
    {
        const csmString key(name);
        ACubismMotion* motion = _expressions[key];

        if (motion == NULL)
        {
            return;
        }
        _expressionManager->StartMotion(motion, false);
        for (size_t i = 0; i < _expressionNames.size(); ++i)
        {
            if (_expressionNames[i] == name)
            {
                _expressionIndex = (csmInt32)i;
                break;
            }
        }
        printf("[ui] 表情 → %s\n", name);
        fflush(stdout);
    }
    csmUint32 NextRand()
    {
        _rngState = _rngState * 1664525u + 1013904223u;
        return _rngState >> 8;
    }
    /** 把一条手势展开成若干"正在生效"的轨道（支持 speed/repeat/幅度缩放）*/
    void SpawnGestureTracks(const GestureDef& g, csmFloat32 speed, csmInt32 repeat, csmFloat32 scale)
    {
        if (speed < 0.2f) speed = 0.2f;
        if (speed > 3.0f) speed = 3.0f;
        if (repeat < 1) repeat = 1;
        if (repeat > 8) repeat = 8;
        const csmFloat32 dur = (g.dur > 0.05f ? g.dur : 1.0f) / speed;

        for (csmInt32 r = 0; r < repeat; ++r)
        {
            for (size_t t = 0; t < g.tracks.size(); ++t)
            {
                if (_live.size() >= kMaxLiveTracks)
                {
                    return; // 轨道池满：丢弃（正常不会到）
                }
                SceneTrack tr = g.tracks[t];
                tr.start = _perfClock + (csmFloat32)r * dur;
                tr.dur = dur;
                tr.scale = scale;
                _live.push_back(tr);
            }
        }
    }
    /** 轨道按关键帧求值（段内 smoothstep 缓动）*/
    static csmFloat32 EvalTrack(const SceneTrack& tr, csmFloat32 u)
    {
        if (tr.keyCount <= 0)
        {
            return 0.0f;
        }
        if (tr.keyCount == 1 || u <= tr.keys[0].t)
        {
            return tr.keys[0].v * tr.scale;
        }
        for (csmInt32 k = 1; k < tr.keyCount; ++k)
        {
            if (u <= tr.keys[k].t)
            {
                const csmFloat32 t0 = tr.keys[k - 1].t;
                const csmFloat32 t1 = tr.keys[k].t;
                const csmFloat32 local = (t1 > t0) ? (u - t0) / (t1 - t0) : 1.0f;
                const csmFloat32 e = local * local * (3.0f - 2.0f * local); // smoothstep
                return (tr.keys[k - 1].v + (tr.keys[k].v - tr.keys[k - 1].v) * e) * tr.scale;
            }
        }
        return tr.keys[tr.keyCount - 1].v * tr.scale;
    }
    /** 每帧把生效中的轨道写进参数（在 scheduler 之后、Core 变形之前调用）*/
    void ApplySceneTracks()
    {
        for (size_t i = 0; i < _live.size();)
        {
            SceneTrack& tr = _live[i];
            const csmFloat32 raw = (tr.dur > 0.0f) ? (_perfClock - tr.start) / tr.dur : 1.0f;

            if (raw < 0.0f)
            {
                ++i; // 还没到点
                continue;
            }
            const csmFloat32 u = raw > 1.0f ? 1.0f : raw;
            const csmFloat32 v = EvalTrack(tr, u);

            if (tr.index >= 0)
            {
                if (tr.additive)
                {
                    _model->AddParameterValue(tr.index, v);
                }
                else
                {
                    _model->SetParameterValue(tr.index, v);
                }
            }
            if (raw >= 1.0f)
            {
                _live.erase(_live.begin() + (ptrdiff_t)i);
            }
            else
            {
                ++i;
            }
        }
    }
    /** 触发一个脚本步骤（sceneRemain=本段脚本剩余秒数，给"姿态保持到段尾"用）*/
    void FireStep(const SceneStep& st, csmFloat32 sceneRemain);
    /** 演出时钟推进：触发到点的步骤、维护队列、驱动自主待机 */
    void SceneTick(csmFloat32 dt)
    {
        _perfClock += dt;

        if (!_scenes.empty())
        {
            Scene& sc = _scenes.front();
            const csmFloat32 rel = _perfClock - sc.beginAt;

            for (size_t i = 0; i < sc.steps.size(); ++i)
            {
                if (!sc.steps[i].fired && rel >= sc.steps[i].at)
                {
                    sc.steps[i].fired = true;
                    FireStep(sc.steps[i], sc.dur - sc.steps[i].at);
                }
            }
            if (rel >= sc.dur)
            {
                printf("[scene] 一段演出结束（队列剩 %d）\n", (int)_scenes.size() - 1);
                fflush(stdout);
                SetIdleLevel(sc.idleLevel);
                ReleaseSceneBody();   // 本段的 body 姿态收回（下一段从自然姿态起）
                _scenes.pop_front();
                if (!_scenes.empty())
                {
                    _scenes.front().beginAt = _perfClock;
                }
            }
        }

        AutoIdleTick();
    }
    /** 自主待机：没有演出/轨道时，隔几秒做个小动作（保证"不呆"）*/
    void AutoIdleTick()
    {
        if (_idleLevel <= 0 || !_scenes.empty() || !_live.empty())
        {
            return;
        }
        if (_perfClock < _idleNextAt || _perfClock < _activityUntil)
        {
            return;
        }
        const csmUint32 r = NextRand();
        const int pick = (int)(r % 100);

        if (_idleLevel == 1 || pick < 55)
        {
            // 张望：看屏幕中部区域的随机点
            // 按渲染尺寸等比（原常量按 360×640 写的；高分屏渲染 2× 时不能还用绝对像素）
            const csmFloat32 kx = _targetW / 360.0f, ky = _targetH / 640.0f;
            const csmFloat32 x = (130.0f + (csmFloat32)(NextRand() % 220u)) * kx;
            const csmFloat32 y = (140.0f + (csmFloat32)(NextRand() % 320u)) * ky;
            SetLookTargetPixels(x, y);
        }
        else
        {
            // 轻动作：歪头/点头/叹气（半幅度）
            static const char* const kLight[] = { "tilt", "nod", "sigh", "think" };
            StartGesture(kLight[NextRand() % 4u], 0.8f, 1, 0.55f);
        }
        _idleNextAt = _perfClock + (csmFloat32)(2 + (NextRand() % (_idleLevel >= 3 ? 3u : 6u)));
    }
    /** 解析一个"场景"对象（脚本 JSON 的一个元素）*/
    bool ParseScene(const jsonlite::Value& v, char* errbuf, size_t errcap);
    /** 读 cdi3.json（model3 的 DisplayInfo）：参数中文名 + 分组，供能力表/参数清单用 */
    void LoadModelDisplayInfo();
    struct BuiltinKey { csmFloat32 t, v; };
    /** 加一条内置轨道（keyCount ≤ 8）*/
    void AddBuiltinTrack(GestureDef& g, const char* param, bool additive,
                         const BuiltinKey* keys, csmInt32 n)
    {
        SceneTrack tr;
        memset(&tr, 0, sizeof(tr));
        tr.index = _model->GetParameterIndex(CubismFramework::GetIdManager()->GetId(param));
        if (tr.index < 0)
        {
            return; // 模型没有这个参数 → 该轨道跳过（对模型自动降级）
        }
        tr.additive = additive;
        tr.start = 0.0f;
        tr.dur = g.dur;
        tr.scale = 1.0f;
        tr.keyCount = n > 8 ? 8 : n;
        for (csmInt32 i = 0; i < tr.keyCount; ++i)
        {
            tr.keys[i].t = keys[i].t;
            tr.keys[i].v = keys[i].v;
        }
        g.tracks.push_back(tr);
    }
    /** 内置通用手势（只用标准参数；模型缺哪个参数自动跳过）*/
    void BuildBuiltinGestures();
    /** 读模型目录下的 l2d.profile.json（手势定义 + 表情别名；可选）*/
    void LoadGestureProfile();
    // ---------------- 演出引擎成员 ----------------
    static const size_t kMaxLiveTracks = 48;
    struct Reaction
    {
        std::string gesture;   // 手势名（可空）
        std::string expr;      // 表情名（可空；"next"=下一个）
    };
    Reaction _reactions[3];    // 0=头 1=身 2=any
    csmBool _hasReaction[3];
    csmFloat32 _tapCooldown;   // 点击冷却（秒）
    csmBool _probePhys;        // 能力探测：是否走"物理稳态"档（PROBEPHYS；默认直连档）
    // 能力探测（非阻塞队列：每帧按时间预算跑一点，见 ProbeTick）与结果表
    std::vector<csmInt32> _probeQueue;   // 待探测参数序号
    csmBool _probeActive;                // 本轮探测进行中（完成后写缓存 + 回复）
    csmInt32 _probeDone;                 // 本轮已完成
    csmInt32 _probeTotal;                // 本轮总数
    csmUint64 _fingerprint;              // 模型指纹（装载时算；防指令串扰，见 L2dCmd.h）
    std::vector<ParamCap> _cap;          // 每参数效果档（探测/缓存载入填充；PARAMS 用）
    // 肢体部件（"哪些 drawable 是手臂"——探测时从"被手臂参数带动的 drawable"学出来，
    // 存进能力缓存；运行时供关节骨架用，见 BuildArmRigs）
    std::map<std::string, std::vector<csmInt32> > _parts;
    csmBool _limbActive;   // 本帧有手臂骨架在动
    std::vector<ArmRig> _rigs;   // 手臂关节骨架（见 BuildArmRigs）
    std::map<std::string, std::pair<csmFloat32, csmFloat32> > _anchors;   // IK 命名锚点（模型坐标）
    csmBool _rigSkinSent;        // 蒙皮权重已交给渲染器
    csmFloat32 _rigMidX;         // 身体中线 x
    csmFloat32 _rigUnit;         // 尺度单位（嘴↔眼距离）
    csmBool _rigGeomFresh;       // 骨架几何已按"当前可见"重建（见 RefreshRigGeometry）
    csmBool _hasFace;            // 嘴/眼位置由参数定位成功
    csmFloat32 _eyeX, _eyeY, _mouthX, _mouthY;
    // 说话（口型）状态：viseme 时间表驱动嘴部参数，叠在表情/轨道之上
    csmFloat32 _talkEnd;                 // 说话结束的演出时钟（0=未在说）
    csmFloat32 _talkNextAt;              // 下一个 viseme 切换时刻
    csmInt32 _talkViseme;                // 当前 viseme（0=闭/停顿；1..5= A I U E O）
    csmFloat32 _talkOpen;                // 平滑后的下颌开度
    csmFloat32 _talkVowel[5];            // 平滑后的五个元音权重
    // 躯体通道（BODY：像控制身体一样控制模型；见 ApplyBodyChannels）
    std::vector<BodyChannel> _channels;  // 通道表（装载时按"标准名映射 + profile 覆盖"建）
    // 动作片段（ACTION；见 BuildBuiltinActions）与动捕层
    std::vector<ActionDef> _actions;         // 内置 + profile（profile 同名优先）
    std::vector<LiveActionTrack> _liveActs;  // 生效中的动作轨道
    struct HandEvent { csmFloat32 at; std::string ch, anchor; };
    std::vector<HandEvent> _handEvents;      // 动作里的 IK 窗口（到点 SetHandTarget / off）
    std::vector<std::pair<csmInt32, csmFloat32> > _partHold;   // PART 指令：部件不透明度每帧压住
    // 作者手臂套（profile "armSets"；Natori/Mao 这类作者画了多套手臂 + 关节链的模型）
    std::vector<AuthorArm> _authorArms;
    static const size_t kMaxLiveActs = 96;
    csmInt32 _mocapLevel;                    // 动捕感强度 0..3（MOCAP 指令；默认 1）
    csmFloat32 _mocapEnv;                    // 动捕层淡入淡出包络 0..1
    // 桌宠协议（PROTOCOL-LLM.md）
    std::vector<std::string> _petEvents;     // 待推送事件（main 取走广播）
    std::vector<JointHold> _jointHolds;      // pose.joints 的关节保持
    std::deque<PetSeq> _petSeqs;             // 时间线队列
    std::vector<MotionInfo> _motionInfo;     // 动作素材信息（首次查询时读）
    bool _motionInfoReady = false;
    csmFloat32 _poseReleaseAt = 0.0f;        // pose.hold 到期时刻
    csmFloat32 _lookOverrideUntil = 0.0f;    // look 命令生效到
    IPort* _port;                            // 输出端口（气泡；NULL=无输出只动口型）
    std::vector<std::string> _bubbleOptions; // 当前气泡的选项（choice 事件回 label 用）
    std::vector<jsonlite::Value> _bubbleOptionDo;   // 每个选项点下去立刻执行的动作（LLM 随选项下发；空 = 没有）
    bool _isPet = false;                     // 桌宠模式（区域反应等）
    std::map<std::string, std::vector<std::string> > _reactLines;   // 本地台词库（{"cmd":"reactions"} 下发）
    std::string _lastReactLine;
    csmFloat32 _lastReactLineAt = -10.0f;
    /** 手动交互（猜拳/讲笑话/菜单摸摸头/哄睡/聊天气/点了气泡选项）之后的"触摸冷却"截止时刻：
     *  这段时间里点她/搓她只有端口画的特效，不做反应（不播动作情绪、不出台词、不上报 LLM）——
     *  否则刚弹出的交互气泡会被"摸摸头"的反应顶掉。*/
    csmFloat32 _touchMuteUntil = -1e9f;
    void MuteTouchResponse(csmFloat32 sec = 3.0f) { _touchMuteUntil = _perfClock + sec; }
    bool TouchMuted() const { return _perfClock < _touchMuteUntil; }
    // 连续情绪（见 EmotionTick）
    Vad _emoCur = { 0, 0, 0 }, _emoTgt = { 0, 0, 0 }, _emoDrift = { 0, 0, 0 }, _emoDriftTgt = { 0, 0, 0 };
    csmFloat32 _emoHold = 0.0f, _emoMag = 0.0f, _emoDriftClock = 0.0f, _emoDriftNext = 1.0f;
    csmFloat32 _emoPulseAt = -10.0f, _emoPulseNext = 0.0f, _emoPulseY = 0.0f, _emoPulseZ = 0.0f;
    std::string _emoName = "neutral", _emoLastNudge, _emoExprFor;
    int _emoExprIdx = -1;
    // 配置（偏好窗口可改；见 CfgApply）
    std::map<std::string, csmFloat32> _cfg;
    std::map<std::string, std::string> _cfgStr;   // 字符串配置（weather_loc 等；进 config.json 的 "str" 段）
    std::vector<std::string> _modelNames;     // 模型库（目录名）
    int _modelIndex = 0, _modelSwitchTo = -1;
    bool _modelRescanRequest = false;      // 模型库导入后置位：请主循环重扫
    std::string _modelRescanSwitch;        // 重扫后要切过去的目标目录名
    bool _modelReloadRequest = false;      // 探测完成→重载当前模型（部件学到位）
    // LLM 接入（密钥只落 0600 文件、绝不回显；见 LlmCfgPath）
    // LLM 接入（v2：多供应商 + 多模型；同一时间只用 active 那一个）
    std::string _llmActive;
    std::vector<l2d::LlmProvider> _llmProviders;
    std::deque<std::string> _llmLog;          // agent 上报的交互记录（JSON 对象原文，最新在后）
    std::string _memoryText;                  // agent 归纳的跨会话记忆（markdown；面板"记忆本"页显示）
    std::string _personaText;                 // 当前模型的性格（agent 生成上报；面板"模型"页显示）
    std::string _personaName, _personaBy, _personaGuidance;
    std::string _personaNick, _personaCall, _personaUserName;   // 主人手设的小名 / 怎么叫主人 / 名字（空 = 没设）
    std::string _memoryCompressedHint;        // 压缩刚完成时的即时提示（面板显示；真时间从 habits.json 读）
    /** 上次记忆压缩时间（读当前模型 habits.json 的 compressT；10 秒缓存，别在状态推送上反复读盘） */
    std::string MemoryCompressedText();
    std::string _memoryAt;                    // 记忆更新时间（agent 给的字符串）
    long _llmCalls = 0, _llmErrors = 0, _llmQuestions = 0, _llmTokIn = 0, _llmTokOut = 0;
    std::map<std::string, long> _llmKinds;    // 回应/闲时/台词库… → 次数
    double _llmMsSum = 0.0;
    // 台词库最近一次成功/失败（面板要能回答"到底更新了没有"）：取自 agent 的 llm_log 条目
    std::string _linesAt, _linesNote, _linesFailAt, _linesFailErr;
    std::string _agentInfo;                   // agent 自报的接入来源（不含密钥）
    int _agentCount = 0;                      // 订阅事件的连接数（≈ LLM agent；面板不订阅，不算在内）
    int _panelReqPage = 0;
    long _panelReqSeq = 0;
    long _sessionTouches = 0;
    csmFloat32 _cfgDirtyAt = -1.0f;
    csmFloat32 _cfgReactivity = 1.0f, _cfgExprGain = 1.0f;
    bool _cfgNativeExpr = true, _cfgLines = true, _cfgSleep = true, _cfgSeek = true;
    csmFloat32 _cfgSleepAfter = 150.0f;
    // 内在状态（见 PetLifeTick）
    int _lifeState = 0;
    csmFloat32 _lifeEnergy = 0.8f, _lifeAffection = 0.3f, _lifeAnnoy = 0.0f, _lifeLonely = 0.2f;
    csmFloat32 _lifeStateAt = 0.0f, _lifeStartAt = 0.0f, _lifeSaveAt = 30.0f, _lifeMoodAt = 0.0f;
    csmFloat32 _userIdleT = 0.0f, _sinceTouchT = 0.0f, _lastPx = -1.0f, _lastPy = -1.0f;
    csmFloat32 _workT = 0.0f;
    csmFloat32 _tiredSayAt = -100.0f;   // 上次"好累"抱怨（防刷屏）
    int _playCombo = 0;                 // 连续玩耍的连击数（4 秒内接着玩才叠加，见 InteractCost）
    csmFloat32 _playComboAt = -100.0f;  // 上一次花钱的互动时刻
    // ---- 状态页反馈数据（panel_state.life）：动态流 / 情绪曲线 / 部位分布 / 猜拳战绩 / 相识日
    struct FeedItem { double t; std::string kind, text; };
    std::deque<FeedItem> _feed;                    // 最近动态（≤40 条）
    int _feedTouchZone = -1, _feedTouchKind = -1, _feedTouchN = 0;
    csmFloat32 _feedTouchAt = -100.0f;
    struct EmoSample { csmFloat32 v, a; };
    std::deque<EmoSample> _emoHist;                // 每 10s 一个采样（≤90 = 15 分钟）
    csmFloat32 _emoSampleAt = -100.0f, _emoFeedAt = -100.0f;
    std::string _emoFeedName = "neutral";
    long _zoneLife[ZONE_COUNT][2] = {};            // 各部位 [点, 搓]：累计（存盘）
    long _zoneSess[ZONE_COUNT][2] = {};            //                  本次
    long _rps[3] = { 0, 0, 0 };                    // 猜拳 [平, 主人赢, 她赢]（存盘）
    double _firstSeen = 0.0;                       // 第一次见面（unix 秒，存盘）   // 连续"在动"累计（工作时长提醒用；离开 >5 分钟清零）
    int _gameRps = 0;           // 猜拳进行中（1 = 等主人选）；见 PetChoiceMade
    std::vector<std::string> _jokePool;   // 笑话池（agent 定期补货推下来；讲笑话本地秒回）
    size_t _jokeNext = 0;                 // 下一条要讲的（轮转）
    csmFloat32 _enjoyUntil = 0.0f, _sleepSmileUntil = 0.0f, _eyeFlutterUntil = 0.0f, _zzzAt = 0.0f;
    csmFloat32 _lifeStirAt = -10.0f, _seekNextAt = 60.0f, _glanceNextAt = 10.0f;
    int _lifeStirs = 0;
    long _lifeTouches = 0;
    bool _lifeLoaded = false, _lifeLoadPending = false;
    bool _cfgLoaded = false;   // 配置读上来了（agent 启动靠 config.str 的存在与否判断"能不能拿语言了"）
    csmFloat32 _silBB[4] = { 0, 0, 0, 0 };   // 渲染轮廓包围盒缓存（见 SilhouetteBBox）
    csmFloat32 _silAt = -1.0f;
    bool _silValid = false;
    csmFloat32 _reactExprUntil = 0.0f;       // 本地反应表情到期
    csmInt32 _reactExprIdx = -1;
    std::map<std::string, std::vector<std::string> > _zoneReactions;   // profile "zoneReactions"（action, gesture, expr）
    int _clickZone;                          // 连击计数中的区域（-1=无）
    int _clickCount;                         // 同区连击次数
    csmFloat32 _clickClock;                  // 距上次点击的安静时长
    int _hoverZone;                          // 指针当前区域（-1=模型外）
    csmFloat32 _hoverClock;                  // 区域内驻留时长
    bool _hoverEmitted;                      // 本轮驻留已发过 hover
    csmFloat32 _rubT;                        // 摩擦检测累计时钟
    csmFloat32 _rubCooldown;                 // 摩擦事件冷却
    csmFloat32 _zoneDbgAt;                   // 分区调试打印节流（_perfClock 口径）
    std::vector<std::pair<csmFloat32, csmFloat32> > _rubBuf; // (t,x) 采样（左右反向计数）
    csmFloat32 _lastDt = 1.0f / 30.0f;
    csmInt32 _viewW = 480, _viewH = 800;
    // cdi3 参数元数据（按参数序号对齐）
    std::vector<std::string> _paramName;   // 中文名
    std::map<std::string, std::string> _partName;   // 部件 id → 显示名（cdi3 Parts）
    std::vector<csmInt32> _groupOf;        // 所属组序号（-1=无）
    std::vector<std::string> _groupNames;  // 组名表
    std::vector<int> _groupOrder;          // 组的展示顺序
    std::vector<GestureDef> _gestures;                            // 内置通用 + profile
    std::vector<std::pair<std::string, std::string>> _exprAliases;
    std::deque<Scene> _scenes;                                    // 演出队列（front=在播）
    std::vector<SceneTrack> _live;                                // 生效中的参数轨道
    csmFloat32 _perfClock;                                        // 演出时钟（秒）
    csmInt32 _idleLevel;                                          // 自主待机强度 0..3
    csmFloat32 _idleNextAt;
    csmFloat32 _activityUntil;                                    // 外部指令后的静默期
    csmUint32 _rngState;
    ICubismModelSetting* _setting;
    std::string _dir;
    csmMap<csmString, ACubismMotion*> _motions;
    csmMap<csmString, ACubismMotion*> _expressions;
    csmBool _motionUpdated;
    csmBool _noMotion;
    csmFloat32 _renderZoom;
    csmBool _autoFit;
    csmFloat32 _fitFill;
    csmFloat32 _targetW;
    csmFloat32 _targetH;
    std::vector<SoftTexture*> _textures;
    // UI 状态：缩放条（0..1，默认中间=原始尺寸）、FPS 与系统统计
    csmFloat32 _zoomSliderPos;
    csmInt32 _fpsValue;
    csmFloat32 _fpsAccum;
    csmInt32 _fpsFrames;
    SysStats _sysStats;
    // 表情切换（点击人物）与命中包围盒
    std::vector<std::string> _expressionNames;
    csmInt32 _expressionIndex;
    csmFloat32 _exprCooldown;
    csmBool _exprTest;
    csmFloat32 _exprTimer;
    csmFloat32 _modelBBox[4];
    csmBool _modelBBoxValid;
    // 触摸跟随状态（渲染目标像素坐标系）
    csmBool _followEnabled;
    csmBool _touchActive;
    csmBool _touchMoved;
    csmBool _touchInModel;
    csmInt32 _touchDownX;
    csmInt32 _touchDownY;
    csmFloat32 _followTargetX;
    csmFloat32 _followTargetY;
    csmBool _followHasTarget;
    csmBool _followTest;      // --follow-test：目标自动循环四角（调试/验证）
    csmFloat32 _followTestTimer;
    csmInt32 _followTestCorner;
    // 模型切换（底部箭头 UI 请求 → main 执行重建加载）
    csmInt32 _modelSwitchRequest;    // -1/0/+1
    csmFloat32 _modelNameToastTimer; // 切换后名称提示剩余时长
    std::string _modelName;
    csmBool _modelSwitchTest;        // --model-switch-test：每 3s 自动切换
    csmFloat32 _modelSwitchTimer;
    // 水印控制（模型自带"水印"表情=作者提供的开关；默认关，详见 SetupWatermarkControl）
    struct WatermarkParam
    {
        CubismIdHandle id;                          // 目标参数（id 句柄）
        CubismExpressionMotion::ExpressionBlendType blend;    // 与表情同语义的混合方式
        csmFloat32 value;                           // 表情里定义的目标值
    };
    csmBool _watermarkOff;                      // --watermark off（默认）/ on
    std::vector<WatermarkParam> _watermarkParams;  // 空 = 本模型没有水印开关（不做任何事）
};

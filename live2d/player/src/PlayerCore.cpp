/**
 * PlayerCore.cpp — L2DPlayer 的核心实现（从 l2dtest.cpp 拆出，纯搬移，逻辑未改）。
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

L2DPlayer::L2DPlayer()
    : _perfClock(0.0f)
    , _idleLevel(2)
    , _idleNextAt(3.0f)
    , _activityUntil(0.0f)
    , _rngState(0x9e3779b9u)
    , _tapCooldown(0.0f)
    , _probePhys(false)
    , _probeActive(false)
    , _probeDone(0)
    , _probeTotal(0)
    , _fingerprint(0)
    , _talkEnd(0.0f)
    , _talkNextAt(0.0f)
    , _talkViseme(0)
    , _talkOpen(0.0f)
    , _limbActive(false)
    , _rigSkinSent(false)
    , _rigMidX(0.0f)
    , _rigUnit(0.0f)
    , _rigGeomFresh(false)
    , _hasFace(false)
    , _eyeX(0.0f), _eyeY(0.0f), _mouthX(0.0f), _mouthY(0.0f)
    , _mocapLevel(1)
    , _mocapEnv(0.0f)
    , _setting(NULL)
    , _motionUpdated(false)
    , _noMotion(false)
    , _renderZoom(1.0f)
    , _autoFit(false)
    , _fitFill(0.95f)
    , _targetW(0.0f)
    , _targetH(0.0f)
    , _zoomSliderPos(0.5f)
    , _fpsValue(0)
    , _fpsAccum(0.0f)
    , _fpsFrames(0)
    , _expressionIndex(-1)
    , _exprCooldown(0.0f)
    , _exprTest(false)
    , _exprTimer(0.0f)
    , _modelBBoxValid(false)
    , _followEnabled(false)
    , _touchActive(false)
    , _touchMoved(false)
    , _touchInModel(false)
    , _touchDownX(0)
    , _touchDownY(0)
    , _followTargetX(0.0f)
    , _followTargetY(0.0f)
    , _followHasTarget(false)
    , _followTest(false)
    , _followTestTimer(0.0f)
    , _followTestCorner(0)
    , _modelSwitchRequest(0)
    , _modelNameToastTimer(0.0f)
    , _modelSwitchTest(false)
    , _modelSwitchTimer(0.0f)
    , _watermarkOff(true)
    , _port(NULL)
    , _clickZone(-1)
    , _clickCount(0)
    , _clickClock(0.0f)
    , _hoverZone(-1)
    , _hoverClock(0.0f)
    , _hoverEmitted(false)
    , _rubT(0.0f)
    , _rubCooldown(0.0f)
    , _zoneDbgAt(-10.0f)
{
    _modelBBox[0] = _modelBBox[1] = _modelBBox[2] = _modelBBox[3] = 0.0f;
    for (int i = 0; i < 3; ++i)
    {
        _hasReaction[i] = false;
    }
}

/**
 * 触摸事件（渲染目标坐标系）：
 *  - 底部 10%：缩放条（5% 步进）
 *  - 其余区域：按下/拖动 → 眼睛/头部/身体注视跟随手指位置（抬起后保持）
 *  - 抬起时若未拖动（<30px）且按下点在模型包围盒内 → 切换下一个表情
 */
void L2DPlayer::HandleTouch(int x, int y, bool down)
{
    const csmFloat32 W = _targetW;
    const csmFloat32 H = _targetH;
    if (W < 8.0f || H < 8.0f)
    {
        return;
    }

    // --- 底部控制条（三段）：[◀ 箭头] [缩放滚动区] [▶ 箭头] ---
    // 箭头 = 切换模型（按下触发一次，抬手才能再触发）；中间 = 拖动调缩放
    const csmFloat32 areaTop = H - H * 0.10f;
    if (L2D_UI_ZOOM_SLIDER && down && (csmFloat32)y >= areaTop)
    {
        const csmFloat32 arrowW = W * 0.15f;
        if ((csmFloat32)x < arrowW || (csmFloat32)x > W - arrowW)
        {
            // 箭头：按下即请求切换（一次触摸只触发一次）
            if (!_touchActive)
            {
                _modelSwitchRequest = ((csmFloat32)x < arrowW) ? -1 : 1;
                printf("[ui] 切换模型（%s）\n", _modelSwitchRequest < 0 ? "上一个" : "下一个");
                fflush(stdout);
            }
            _touchActive = true;   // 占用触摸状态（防重复触发）
            _touchMoved = true;    // 抬起不作为点击（不触发表情）
            _touchInModel = false;
            _touchDownX = x;
            _touchDownY = y;
            return;
        }

        // 中间：缩放滚动区（拖动=绝对位置，5% 步进）
        const csmFloat32 margin = arrowW + 8.0f;
        csmFloat32 p = ((csmFloat32)x - margin) / (W - 2.0f * margin);
        if (p < 0.0f) p = 0.0f;
        if (p > 1.0f) p = 1.0f;
        const csmFloat32 stepCount = 100.0f / (csmFloat32)L2D_UI_SLIDER_STEP_PCT;
        _zoomSliderPos = roundf(p * stepCount) / stepCount; // 配置步进（默认 5%）
        // 0%→L2D_UI_SLIDER_MIN  50%→1.0x  100%→L2D_UI_SLIDER_MAX
        _renderZoom = L2D_UI_SLIDER_MIN
                    + _zoomSliderPos * (L2D_UI_SLIDER_MAX - L2D_UI_SLIDER_MIN);
        _autoFit = false;                            // 手动调整后不再自动适配
        _touchActive = false;                        // 中断进行中的注视触摸
        printf("[ui] 缩放 %d%% (zoom=%.2f)\n", (int)(_zoomSliderPos * 100.0f + 0.5f), _renderZoom);
        fflush(stdout);
        return;
    }

    if (down)
    {
        if (!_touchActive)
        {
            _touchActive = true;
            _touchDownX = x;
            _touchDownY = y;
            _touchMoved = false;
            _touchInModel = _modelBBoxValid &&
                (csmFloat32)x >= _modelBBox[0] && (csmFloat32)x <= _modelBBox[2] &&
                (csmFloat32)y >= _modelBBox[1] && (csmFloat32)y <= _modelBBox[3];
        }
        else
        {
            const int dx = x - _touchDownX;
            const int dy = y - _touchDownY;
            if (dx * dx + dy * dy > L2D_CLICK_MOVE_THRESHOLD * L2D_CLICK_MOVE_THRESHOLD)
            {
                _touchMoved = true; // 超过阈值 → 判为拖动（抬起不再算点击）
            }
        }
        // 注视目标 = 手指当前位置（仅非缩放条区域）
        _followTargetX = (csmFloat32)x;
        _followTargetY = (csmFloat32)y;
        _followHasTarget = true;
    }
    else if (_touchActive)
    {
        // 抬起：点击（未拖动 + 按下时命中模型）→ 切换表情（可配置开关）
        if (L2D_DEFAULT_TAP_EXPR && !_touchMoved && _touchInModel && _exprCooldown <= 0.0f)
        {
            NextExpression();
            _exprCooldown = L2D_TAP_EXPR_COOLDOWN;
        }
        _touchActive = false;
    }
}

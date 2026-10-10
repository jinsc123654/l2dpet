/**
 * MainLoop.h — 桌宠的运行期：装载模型库 → 起端口 → 主循环（原 main 的后半段）。
 *
 * 从 l2dtest.cpp 拆出（纯搬移，逻辑未改）。
 */
#pragma once

#include "PetBootstrap.h"

int RunMainLoop(PetOptions& opt);   // 非 const：运行期会修正 renderWidth/Height、rasterThreads 等

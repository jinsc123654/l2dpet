/**
 * PpmPlayer.h — PPM 帧序列上屏（对照/验收用：不加载模型，只读帧 → 上屏）。
 *
 * 从 l2dtest.cpp 拆出（纯搬移，逻辑未改）。
 */
#pragma once

#include <csignal>

/** 主循环的优雅退出标志（定义在 l2dtest.cpp：信号处理里置位） */
extern volatile sig_atomic_t g_quit;

int RunPpmPlayer(const char* dir, int fps, const char* portRequest, int width, int height);


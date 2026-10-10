#!/usr/bin/env bash
# build-panel.sh — 构建桌宠的 UI 进程 l2dpet-ui（EUI-NEO：托盘/右键菜单/气泡/状态与设置）
# 产物：$WORK/build-panel/l2dpet-ui（旁边带 assets/ 和 l2dpet.png）；桌宠按这个路径启动它（或 L2DPET_UI 指定）
set -euo pipefail
SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
WORK=${L2D_WORK:-$HOME/l2d-build}
cmake -S "$SCRIPT_DIR/panel" -B "$WORK/build-panel" -DCMAKE_BUILD_TYPE=Release -DEUI_DEPS_MODE=auto
cmake --build "$WORK/build-panel" --target l2dpet-ui --parallel "${L2D_JOBS:-$(nproc 2>/dev/null || echo 4)}"
echo "[i] UI 进程 → $WORK/build-panel/l2dpet-ui"

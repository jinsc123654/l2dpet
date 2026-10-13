#!/usr/bin/env bash
# build-panel-win.sh — 构建桌宠的 UI 进程 l2dpet-ui（Windows / MSVC，EUI-NEO）
#
# 与 Linux 的 build-panel.sh 同构：同一个 CMake 工程、同一套 EUI-NEO 补丁，
# 只是平台侧三处差异：
#
#   [Win] 1. Visual Studio 生成器 + -A x64（它会自己调 vcvars）
#   [Win] 2. 托盘走 EUI-NEO 的 Win32 原生后端（代码就绪，首次实机验证）
#   [Win] 3. 路径经 cygpath 转成 Windows 形式（cmake.exe 认不得 /c/...）
#
# 产物：$WORK/build-panel-win/Release/l2dpet-ui.exe（旁边带 assets/ 与 l2dpet.png）
#
# 用法：
#   live2d/build-panel-win.sh               编（x64 / Release）
#   live2d/build-panel-win.sh -h            看用法
#
# 依赖：Visual Studio（MSVC）+ CMake ≥ 3.14。
#       EUI-NEO 用**仓库自带的本地副本**（live2d/3rd/EUI-NEO，已含 eui-patches/ 的改动），
#       不需要 git/网络；想用外部检出：EUI_NEO_DIR=<目录>（需落在钉死提交上）。
set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
WORK=${L2D_WORK:-$HOME/l2d-build}
BUILD_DIR="$WORK/build-panel-win"

say() { printf '%s\n' "$*"; }
die() { printf '[x] %s\n' "$*" >&2; exit 1; }
step() { printf '\n\033[1m== %s\033[0m\n' "$*"; }

case ${1:-} in
    -h|--help) sed -n '2,18p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    "") ;;
    *) die "不认识的参数：$1（-h 看用法）" ;;
esac

# EUI-NEO：默认用仓库自带的本地副本（面板工程里已经这么默认了）；EUI_NEO_DIR 可覆盖
EUI_NEO="${EUI_NEO_DIR:-$SCRIPT_DIR/../live2d/3rd/EUI-NEO}"
say "[i] EUI-NEO：$EUI_NEO"

# [Win] 3. 路径转 Windows 形式
step "构建 l2dpet-ui（Windows / MSVC）"
cmake -S "$SCRIPT_DIR/panel" -B "$BUILD_DIR" -A x64 \
      -DCMAKE_BUILD_TYPE=Release \
      -DEUI_DEPS_MODE=auto \
      -DEUI_NEO_DIR="$(cygpath -m "$EUI_NEO")"
# MSBuild 开关用 -p: 而不是 /p:：Git Bash（MSYS）会把 / 开头的参数当路径转换，
# 到 msbuild 手里就变成 "MSB1008: Only one project can be specified"（CI 上踩过）。
cmake --build "$BUILD_DIR" --config Release --target l2dpet-ui --parallel "$(nproc 2>/dev/null || echo 4)" -- -p:MultiProcessorCompilation=true

echo
echo "产物:"
ls -la "$BUILD_DIR/Release/l2dpet-ui.exe" 2>/dev/null || die "没有 $BUILD_DIR/Release/l2dpet-ui.exe"
say "[i] UI 进程 → $BUILD_DIR/Release/l2dpet-ui.exe"
exit 0

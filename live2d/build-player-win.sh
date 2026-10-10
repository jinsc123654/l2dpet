#!/usr/bin/env bash
# build-player-win.sh — 组装构建树并构建 Live2D 播放器/桌宠主程序（Windows / MSVC）
#
# 与 Linux 的 build-player.sh **同构**：SDK 物料 → 本地构建树 → 注入软渲染后端 →
# CMake 构建，连变量名（CSM_SDK / L2D_WORK / SDK_TREE）都一样，只是平台侧的三处差异：
#
#   [Win] 1. 组装构建树用 cp -r（Git Bash 不自带 rsync）
#   [Win] 2. SDK 5-r.5 起 Windows Core 库按 MSVC 工具集分目录（x86_64/143/…），
#            这里补成 player/CMakeLists.txt 期望的扁平路径 x86_64/Live2DCubismCore.lib
#   [Win] 3. 交给 Visual Studio 生成器（它自己会调 vcvars，不必先进 x64 开发者命令行）
#
# 产物：$WORK/build-win/Release/l2dtest.exe
#
# 用法：
#   live2d/build-player-win.sh              编（x64 / Release）
#   live2d/build-player-win.sh -h           看用法
#
# 依赖：
#   - Visual Studio（MSVC + Windows SDK）+ CMake ≥ 3.14
#   - Cubism SDK 解包目录（CSM_SDK；默认在 $WORK/live2d-sdk 下自动找 CubismSdkForNative-*）
#   - GLFW/glad/freetype 三方源码：**仓库自带**（live2d/3rd/EUI-NEO/3rd），不用另外准备
#   - Git Bash（用 cygpath 把 MSYS 路径换成 CMake 认得的 Windows 形式）
#
# 环境变量：
#   CSM_SDK         Cubism SDK 解包目录
#   L2D_WORK        构建根（默认 ~/l2d-build；sdk-tree 与 build-win 都在它下面）
#   L2D_3RD_DIR     三方源码目录（默认用仓库自带的 live2d/3rd/EUI-NEO/3rd）
set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
PLAYER_DIR="$SCRIPT_DIR/player"
REPO_DIR=$(cd "$SCRIPT_DIR/.." && pwd)
WORK=${L2D_WORK:-$HOME/l2d-build}
SDK=${CSM_SDK:-}
SDK_TREE="$WORK/sdk-tree"

say() { printf '%s\n' "$*"; }
die() { printf '[x] %s\n' "$*" >&2; exit 1; }
step() { printf '\n\033[1m== %s\033[0m\n' "$*"; }

case ${1:-} in
    -h|--help) sed -n '2,27p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    "") ;;
    *) die "不认识的参数：$1（-h 看用法）" ;;
esac

# --- 0. SDK 物料：优先用 $CSM_SDK；没有但构建树已就绪（sdk-tree）就跳过同步 ---
if [ -z "$SDK" ]; then
    for d in "$WORK"/live2d-sdk/CubismSdkForNative-*; do
        [ -d "$d/Core" ] && SDK="$d"
    done
fi
if [ -n "$SDK" ] && [ -d "$SDK/Core" ]; then
    NEED_SYNC=1
elif [ -d "$SDK_TREE/Framework" ]; then
    say "[i] 没找到 SDK 物料，但构建树已存在 —— 跳过同步，直接用 $SDK_TREE"
    NEED_SYNC=0
else
    die "找不到 Cubism SDK（CSM_SDK=<解包目录> 指定，或放到 $WORK/live2d-sdk/ 下）"
fi

# --- 1. 组装构建树（Core + Framework，注入软渲染后端 + 第三方头）---
step "组装构建树 → $SDK_TREE"
mkdir -p "$SDK_TREE"
if [ "$NEED_SYNC" = 1 ]; then
    say "[i] 同步 SDK 物料（Windows 侧用 cp -r）"
    rm -rf "$SDK_TREE/Core" "$SDK_TREE/Framework"
    cp -r "$SDK/Core" "$SDK_TREE/Core"
    cp -r "$SDK/Framework" "$SDK_TREE/Framework"

    # [Win] 2. Core 静态库：工具集子目录 → 扁平路径
    # 优先 143，再退 142/141；取 _MT，与主程序的静态 CRT 一致。
    CORE_SRC=""
    for ts in 143 142 141; do
        if [ -f "$SDK/Core/lib/windows/x86_64/$ts/Live2DCubismCore_MT.lib" ]; then
            CORE_SRC="$SDK/Core/lib/windows/x86_64/$ts/Live2DCubismCore_MT.lib"
            break
        fi
    done
    [ -n "$CORE_SRC" ] || die "找不到 Windows x64 的 Live2DCubismCore_*.lib（看 $SDK/Core/lib/windows/x86_64）"
    cp "$CORE_SRC" "$SDK_TREE/Core/lib/windows/x86_64/Live2DCubismCore.lib"
    say "[i] Core 静态库：${CORE_SRC#*/windows/} → x86_64/Live2DCubismCore.lib"
fi

rm -rf "$SDK_TREE/Framework/src/Rendering/Software"
cp -r "$PLAYER_DIR/soft-renderer" "$SDK_TREE/Framework/src/Rendering/Software"

# stb_image（软渲染纹理解码；SDK 包内自带同一份，按需解出）
STB="$SDK_TREE/third_party/stb_image.h"
if [ ! -f "$STB" ]; then
    [ -n "$SDK" ] || die "缺 $STB 且没有 SDK 物料可提取（先给 CSM_SDK）"
    src=$(find "$SDK/Samples" -name stb_image.h 2>/dev/null | head -1)
    [ -n "$src" ] || die "SDK 的 Samples 里找不到 stb_image.h"
    say "[i] 注入 stb_image.h → $SDK_TREE/third_party/"
    mkdir -p "$SDK_TREE/third_party"
    cp "$src" "$STB"
fi

# --- 2. 构建 ---
# [Win] 3. Visual Studio 生成器 + -A x64；构建类型由 --config Release 给（不是 CMAKE_BUILD_TYPE）
# 路径转成 Windows 形式：cmake.exe 是原生程序，认不得 MSYS 的 /c/...
step "构建 l2dtest（Windows / MSVC）"
BUILD_DIR="$WORK/build-win"
cmake -S "$PLAYER_DIR" -B "$BUILD_DIR" -A x64 \
      -DCSM_BUILD_ROOT="$(cygpath -m "$SDK_TREE")" \
      -DL2D_3RD_DIR="$(cygpath -m "${L2D_3RD_DIR:-$REPO_DIR/live2d/3rd/EUI-NEO/3rd}")" \
      -DCMAKE_BUILD_TYPE=Release
cmake --build "$BUILD_DIR" --config Release --parallel "$(nproc 2>/dev/null || echo 4)" -- /p:MultiProcessorCompilation=true

echo
echo "产物:"
ls -la "$BUILD_DIR/Release/l2dtest.exe" 2>/dev/null || die "没有 $BUILD_DIR/Release/l2dtest.exe"
exit 0

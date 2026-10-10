#!/usr/bin/env bash
# build-player.sh — 组装构建树并构建 Live2D 播放器/桌宠主程序（三端桌面：Linux / Windows / macOS）
#
# 设计：SDK 物料（Cubism Core + Framework）保持原样；把 Framework 源码 rsync 到本地构建树，
# 并把 live2d/player/soft-renderer 注入成 Framework/src/Rendering/Software（软渲染后端，
# 无 GPU 平台与测试路径用）。
# 产物：$WORK/build-host/l2dtest

set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
PLAYER_DIR="$SCRIPT_DIR/player"

WORK=${L2D_WORK:-$HOME/l2d-build}
SDK=${CSM_SDK:-$WORK/live2d-sdk}
SDK_TREE="$WORK/sdk-tree"

# 支持直接指向 SDK，或指向装有官方解包子目录的父目录。
if [ ! -d "$SDK/Framework" ]; then
    for d in "$SDK"/CubismSdkForNative-*; do
        if [ -d "$d/Framework" ]; then SDK=$d; break; fi
    done
fi

# --- 0. SDK 物料：优先用 $CSM_SDK；没有但构建树已就绪（sdk-tree）就跳过同步 ---
if [ ! -d "$SDK/Framework" ] && [ -d "$SDK_TREE/Framework" ]; then
    echo "[i] 没找到 SDK 物料（$SDK），但构建树已存在 —— 跳过同步，直接用 $SDK_TREE"
    NEED_SYNC=0
else
    NEED_SYNC=1
fi

if [ "$NEED_SYNC" = 1 ] && [ ! -d "$SDK/Framework" ]; then
    if [ -f "$SDK/cubism.zip" ]; then
        echo "[i] SDK 缺 Framework，从 cubism.zip 解压…"
        tmp=$(mktemp -d)
        unzip -q "$SDK/cubism.zip" 'CubismSdkForNative-*/Framework/*' 'CubismSdkForNative-*/Core/*' -d "$tmp"
        mv "$tmp"/CubismSdkForNative-*/Framework "$SDK/Framework"
        if [ ! -d "$SDK/Core" ]; then mv "$tmp"/CubismSdkForNative-*/Core "$SDK/Core"; fi
        rm -rf "$tmp"
    else
        echo "[x] 找不到 $SDK/Framework 或 $SDK/cubism.zip（可用 CSM_SDK=<SDK 目录> 指定）" >&2
        exit 1
    fi
fi

# --- 1. 组装构建树（Framework + Core，注入软渲染后端 + 第三方头）---
mkdir -p "$SDK_TREE"
if [ "$NEED_SYNC" = 1 ]; then
    echo "[i] 同步构建树 -> $SDK_TREE"
    if [ -d "$SDK/Core" ]; then
        rsync -a --delete "$SDK/Core/" "$SDK_TREE/Core/"
    elif [ ! -d "$SCRIPT_DIR/3rd/CubismCore/include" ]; then
        echo "[x] SDK 与仓库均缺 Cubism Core" >&2
        exit 1
    fi
    rsync -a --delete "$SDK/Framework/" "$SDK_TREE/Framework/"
fi
mkdir -p "$SDK_TREE/Framework/src/Rendering/Software"
rsync -a --delete --exclude=.mimosa --exclude=.git \
    "$PLAYER_DIR/soft-renderer/" "$SDK_TREE/Framework/src/Rendering/Software/"

# stb_image（软渲染纹理解码；SDK zip 内自带同一份，按需解出）
STB="$SDK_TREE/third_party/stb_image.h"
if [ ! -s "$STB" ]; then
    echo "[i] 注入 stb_image.h -> $SDK_TREE/third_party/"
    mkdir -p "$SDK_TREE/third_party"
    src=""
    if [ -d "$SDK/Samples" ]; then
        src=$(find "$SDK/Samples" -name stb_image.h -type f -print -quit)
    fi
    if [ -n "$src" ]; then
        cp "$src" "$STB"
    elif [ -f "$SDK/cubism.zip" ]; then
        unzip -p "$SDK/cubism.zip" 'CubismSdkForNative-*/Samples/OpenGL/thirdParty/stb/stb_image.h' > "$STB"
    elif [ -s "$SCRIPT_DIR/3rd/EUI-NEO/3rd/stb_image.h" ]; then
        cp "$SCRIPT_DIR/3rd/EUI-NEO/3rd/stb_image.h" "$STB"
    else
        echo "[x] 找不到 stb_image.h：请提供完整 SDK Samples 或 cubism.zip" >&2
        exit 1
    fi
    [ -s "$STB" ] || { echo "[x] stb_image.h 为空，SDK 提取失败" >&2; exit 1; }
fi

# --- 2. 构建 ---
build() {
    local name=$1
    local toolchain=${2:-}
    local bdir="$WORK/build-$name"
    local args=(-S "$PLAYER_DIR" -B "$bdir" -DCSM_BUILD_ROOT="$SDK_TREE" -DCMAKE_BUILD_TYPE=Release)
    if [ -n "$toolchain" ]; then
        args+=(-DCMAKE_TOOLCHAIN_FILE="$toolchain")
    fi
    cmake "${args[@]}"
    cmake --build "$bdir" --parallel "${L2D_JOBS:-$(nproc)}"
    echo "[ok] $bdir/l2dtest"
}

# 用法：build-player.sh [host|<自定义构建名>]
#   host（默认） 主机（Linux 桌面）构建 → $WORK/build-host/l2dtest
#   其它名字     用同名工具链文件（player/cmake/<name>-toolchain.cmake）交叉构建，
#                例如 Win 交叉编译：build-player.sh mingw
MODE=${1:-host}

case $MODE in
    *[!A-Za-z0-9_-]* | "") echo "用法: $0 [host|<构建名>]" >&2; exit 2 ;;
esac

if [ "$MODE" = "host" ]; then
    build host
else
    TC="$PLAYER_DIR/cmake/$MODE-toolchain.cmake"
    [ -f "$TC" ] || { echo "[x] 没有工具链文件 $TC" >&2; exit 1; }
    build "$MODE" "$TC"
fi

echo
echo "产物:"
ls -la "$WORK/build-$MODE/l2dtest" 2>/dev/null || true
exit 0

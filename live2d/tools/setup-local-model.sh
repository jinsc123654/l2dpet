#!/usr/bin/env bash
# setup-local-model.sh — 把仓库里的 profile + 能力缓存装进本机模型目录
#
# 为什么需要：桌宠的 16 区点击判定依赖"臂骨架 + 关节锚点"，而它们来自两个仓库文件：
#   live2d/player/profiles/<模型>/l2d.profile.json        （手臂通道→作者参数链）
#   live2d/player/capabilities/<模型>.capability.json      （部件名单/参数效果档/指纹）
# 手工把模型拷到 ~/.local/share/l2dpet/models/<模型>/ 时若漏了这两个文件，
# 分区会**静默退化成"头/身两带"**（症状：点哪儿都像点胸/头，"点击歪"）——
# 启动日志会打 "[zones] 未建臂骨架：分区退化为头/身两带"。
#
# 用法：
#   live2d/tools/setup-local-model.sh 舒芙蕾          # 装 profile + 能力缓存
#   live2d/tools/setup-local-model.sh --all           # 给模型库里所有模型能匹配上的都装
#   live2d/tools/setup-local-model.sh 舒芙蕾 /path/to/models   # 指定模型库根

set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO_DIR=$(cd "$SCRIPT_DIR/../.." && pwd)
PROF_DIR="$REPO_DIR/live2d/player/profiles"
CAP_DIR="$REPO_DIR/live2d/player/capabilities"
LIB_DIR="${3:-$HOME/.local/share/l2dpet/models}"

install_one() {
    local name="$1" dir="$LIB_DIR/$1"
    [ -d "$dir" ] || { echo "[skip] $dir 不存在"; return; }
    local did=0
    # profile：中文目录名 → 仓库里可能是拼音（舒芙蕾→shuflle）；先按别名表，再按原名/小写
    local alias=""
    case "$name" in
        舒芙蕾) alias="shuflle" ;;
        林翩翩) alias="linpianpian" ;;
    esac
    for cand in "$PROF_DIR/$alias" "$PROF_DIR/$name" "$PROF_DIR/$(echo "$name" | tr 'A-Z' 'a-z')"; do
        if [ -n "$cand" ] && [ -f "$cand/l2d.profile.json" ]; then
            cp -f "$cand/l2d.profile.json" "$dir/l2d.profile.json" && did=1
            echo "[ok] profile  → $dir/l2d.profile.json"
            break
        fi
    done
    # 能力缓存：同样按别名，再按原名
    for cand in "$CAP_DIR/$alias.capability.json" "$CAP_DIR/$name.capability.json"; do
        if [ -n "$cand" ] && [ -f "$cand" ]; then
            # 指纹由桌宠启动时校验（不符自动忽略），这里只做拷贝
            cp -f "$cand" "$dir/l2d.capability.json" && did=1
            echo "[ok] 能力缓存 → $dir/l2d.capability.json"
            break
        fi
    done
    [ "$did" = 1 ] || echo "[warn] $name：仓库里没找到对应的 profile/能力缓存（可加别名映射）"
}

if [ "${1:-}" = "--all" ]; then
    for d in "$LIB_DIR"/*/; do
        install_one "$(basename "$d")"
    done
else
    [ $# -ge 1 ] || { sed -n '2,17p' "$0"; exit 1; }
    install_one "$1"
fi

echo "完成。重启桌宠后启动日志应显示 16 区（不再有 '[zones] 未建臂骨架' 警告）。"

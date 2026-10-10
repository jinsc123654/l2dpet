#!/bin/sh
# uninstall.sh — 卸载 Live2D 桌宠
#
# 默认只删程序本体与菜单项，**保留**你的数据：
#   ~/.config/l2dpet     配置（LLM 供应商/密钥、性格、试题库、记忆）
#   ~/.local/state/l2dpet 亲密度存档、日志
#   ~/.local/share/l2dpet/models 模型库（模型体积大，绝不自动删）
#
# 用法：
#   ./uninstall.sh            卸载程序（保留数据）
#   ./uninstall.sh --purge    连配置/存档一起删（模型库仍然保留，除非再加 --purge-models）
#   ./uninstall.sh --purge --purge-models --yes   全清（含模型，慎用）
set -u

HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd) || exit 1
PURGE=0
PURGE_MODELS=0
YES=0
for a in "$@"; do
    case $a in
        --purge) PURGE=1 ;;
        --purge-models) PURGE_MODELS=1 ;;
        --yes|-y) YES=1 ;;
        -h|--help) sed -n '2,14p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) printf '不认识的参数：%s（-h 看用法）\n' "$a" >&2; exit 2 ;;
    esac
done

say() { printf '%s\n' "$*"; }

# ---------------------------------------------------------------- 找"装在哪儿"
# 三种情况都要对：① 从安装目录里跑（~/.local/opt/l2dpet/uninstall.sh）
#                     ② 从解压出来的包里跑（那时本脚本所在目录不是安装目录 → 顺着启动器软链找）
#                     ③ 都没有（已经卸过了）
looks_like_install() { [ -e "$1/l2dtest" ] && [ -e "$1/l2dpet" ]; }

resolve_link() {   # 解析符号链接（readlink -f 不是 POSIX，自己走一层）
    p=$1
    n=0
    while [ -L "$p" ] && [ "$n" -lt 40 ]; do
        t=$(readlink "$p") || break
        case $t in
            /*) p=$t ;;
            *) p=$(dirname "$p")/$t ;;
        esac
        n=$((n + 1))
    done
    printf '%s\n' "$p"
}

normalize_dir() {  # 消掉 .. / 多余斜杠，并解析到物理路径（/usr/bin/../../opt/x → /opt/x）
    d=$(readlink -f -- "$1" 2>/dev/null || true)
    if [ -n "$d" ] && [ -d "$d" ]; then
        printf '%s\n' "$d"
    elif [ -d "$1" ]; then
        ( CDPATH= cd -- "$1" && pwd -P ) 2>/dev/null || printf '%s\n' "$1"
    else
        printf '%s\n' "$1"
    fi
}

PREFIX=""
if [ -f "$HERE/.installed" ]; then          # ① 从安装目录里跑（install.sh 会写这个标记）
    PREFIX=$HERE
else                                        # ② 从解压出来的包里跑：顺启动器软链找真正装在哪
    for l in "$HOME/.local/bin/l2dpet" /usr/local/bin/l2dpet /usr/bin/l2dpet; do
        [ -L "$l" ] || [ -e "$l" ] || continue
        target=$(resolve_link "$l")
        d=$(normalize_dir "$(dirname -- "$target")")
        if [ -f "$d/.installed" ] || looks_like_install "$d"; then
            PREFIX=$d
            break
        fi
    done
    if [ -z "$PREFIX" ]; then
        for d in "$HOME/.local/opt/l2dpet" /opt/l2dpet; do
            if [ -f "$d/.installed" ] || looks_like_install "$d"; then
                PREFIX=$d
                break
            fi
        done
    fi
fi
PREFIX=$(normalize_dir "$PREFIX")   # /usr/bin/../../opt/l2dpet → /opt/l2dpet（否则系统安装会被当用户安装）

if [ -z "$PREFIX" ]; then
    say "[uninstall] 没找到已安装的 l2dpet（查过：本目录、~/.local/bin/l2dpet、/usr/bin/l2dpet、"
    say "           ~/.local/opt/l2dpet、/opt/l2dpet）——可能已经卸载过了。"
    say "           要连数据一起删：手动 rm -rf ~/.config/l2dpet ~/.local/state/l2dpet"
    exit 0
fi

# 程序可能在 ~/.local/opt/l2dpet 或 /opt/l2dpet
case $PREFIX in
    /opt/*) SYSTEM=1 ;;
    *) SYSTEM=0 ;;
esac
if [ "$SYSTEM" = "1" ]; then
    BINDIR=/usr/bin
    APPDIR=/usr/share/applications
    ICONROOT=/usr/share/icons/hicolor
    [ "$(id -u)" = "0" ] || { printf '[uninstall] 需要 root（sudo ./uninstall.sh）\n' >&2; exit 1; }
else
    BINDIR=$HOME/.local/bin
    APPDIR=$HOME/.local/share/applications
    ICONROOT=$HOME/.local/share/icons/hicolor
fi

# 先停掉
if [ -x "$PREFIX/l2dpet" ]; then
    "$PREFIX/l2dpet" stop >/dev/null 2>&1 || true
fi

# 删目录前先自证：不能是 / ~ 之类的危险路径，且得**看起来像我们的安装目录**
case $PREFIX in
    /|/home|/usr|/usr/local|/opt|"$HOME"|"")
        printf '[uninstall] 拒绝：安装目录看起来不对（%s），什么都没删\n' "$PREFIX" >&2
        exit 1
        ;;
esac
if ! looks_like_install "$PREFIX"; then
    printf '[uninstall] 拒绝：%s 里既没有 l2dtest 也没有 l2dpet——不像安装目录，什么都没删\n' "$PREFIX" >&2
    exit 1
fi

say "[uninstall] 删除程序目录 $PREFIX"
rm -rf -- "$PREFIX"
say "[uninstall] 删除启动器/菜单项/图标"
[ -L "$BINDIR/l2dpet" ] && rm -f "$BINDIR/l2dpet"
rm -f "$APPDIR/l2dpet.desktop" "$ICONROOT/128x128/apps/l2dpet.png" 2>/dev/null || true
rm -f "$HOME/.config/autostart/l2dpet.desktop" 2>/dev/null || true
# 桌面图标（--desktop-icon 装的；用户可能改过名，只删我们自己那个名字）
DESK=$(xdg-user-dir DESKTOP 2>/dev/null || true)
[ -n "$DESK" ] && [ "$DESK" != "$HOME" ] || DESK=$HOME/Desktop
[ -f "$DESK/l2dpet.desktop" ] && rm -f "$DESK/l2dpet.desktop" && say "[uninstall] 删除桌面图标 $DESK/l2dpet.desktop"
# Dock 收藏夹里的固定项（--dock 装的）——别留个点不动的图标在 Dock 上
if command -v gsettings >/dev/null 2>&1 && gsettings get org.gnome.shell favorite-apps >/dev/null 2>&1; then
    cur=$(gsettings get org.gnome.shell favorite-apps)
    case $cur in
        *l2dpet.desktop*)
            new=$(printf '%s' "$cur" | sed "s/, 'l2dpet.desktop'//; s/'l2dpet.desktop', //; s/'l2dpet.desktop'//")
            gsettings set org.gnome.shell favorite-apps "$new" 2>/dev/null && say "[uninstall] 已从 Dock 收藏夹移除"
            ;;
    esac
fi
command -v update-desktop-database >/dev/null 2>&1 && update-desktop-database "$APPDIR" >/dev/null 2>&1 || true

if [ "$PURGE" = "1" ]; then
    if [ "$YES" = "0" ]; then
        say ""
        say "要删除这些数据（配置/记忆/亲密度存档/日志）："
        say "  ~/.config/l2dpet  ~/.local/state/l2dpet"
        printf '确认？[y/N] '
        read -r ans
        case $ans in y|Y|yes|YES) ;; *) say "跳过数据删除"; PURGE=0 ;; esac
    fi
    if [ "$PURGE" = "1" ]; then
        # $HOME 来自环境：空/根 都不动手（否则可能删到意料之外的路径）
        case $HOME in
            ""|/)
                say "[uninstall] 拒绝：HOME 不可信（${HOME:-空}），跳过数据删除"
                PURGE=0
                ;;
        esac
    fi
    if [ "$PURGE" = "1" ]; then
        [ -d "$HOME/.config/l2dpet" ] && rm -rf -- "$HOME/.config/l2dpet"
        [ -d "$HOME/.local/state/l2dpet" ] && rm -rf -- "$HOME/.local/state/l2dpet"
        say "[uninstall] 已删除配置与存档"
        if [ "$PURGE_MODELS" = "1" ]; then
            [ -d "$HOME/.local/share/l2dpet/models" ] && rm -rf -- "$HOME/.local/share/l2dpet/models"
            say "[uninstall] 已删除模型库"
        else
            say "[uninstall] 模型库保留：~/.local/share/l2dpet/models"
        fi
    fi
else
    say "[uninstall] 数据和模型都保留（配置 ~/.config/l2dpet，模型 ~/.local/share/l2dpet/models）"
fi
say "[uninstall] 完成"

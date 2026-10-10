#!/bin/sh
# install.sh — 安装 Live2D 桌宠（Linux 桌面）
#
# 默认装到**用户目录**（不需要 root）：
#   $PREFIX          ~/.local/opt/l2dpet      程序本体
#   ~/.local/bin/l2dpet                       启动器（在 PATH 里就能直接敲 l2dpet）
#   ~/.local/share/applications/l2dpet.desktop 应用菜单项
#   ~/.local/share/icons/hicolor/128x128/apps/l2dpet.png
#   ~/.local/share/l2dpet/models/             模型库（首次启动若为空，启动器放入随包示例模型 Hiyori）
# 用户数据（配置/记忆/亲密度）在 ~/.config/l2dpet 与 ~/.local/state/l2dpet，卸载默认不动。
#
# 用法：
#   ./install.sh              用户安装（默认）
#   sudo ./install.sh --system   装到 /opt/l2dpet + /usr/{bin,share}（全机器可用）
#   ./install.sh --autostart  顺带加登录自启（~/.config/autostart/l2dpet.desktop）
#   ./install.sh --desktop-icon  桌面上放一个启动图标（~/Desktop/l2dpet.desktop，双击即起）
#   ./install.sh --dock       把图标固定到 Dock/任务栏（GNOME 收藏夹；其它桌面按提示手动）
#   ./install.sh --no-menu    不装菜单项/图标
#   ./install.sh -h
set -eu
trap 'rc=$?; if [ "$rc" -ne 0 ]; then printf "[install] 安装失败，未完成；请检查上面的错误后重试。\n" >&2; fi' 0

HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd) || exit 1
SYSTEM=0
AUTOSTART=0
MENU=1
DESKICON=0
DOCKPIN=0
PREFIX=${PREFIX:-}

while [ $# -gt 0 ]; do
    case $1 in
        --system) SYSTEM=1; AUTOSTART=1 ;;
        --autostart) AUTOSTART=1 ;;
        --no-menu) MENU=0 ;;
        --desktop-icon) DESKICON=1 ;;
        --dock) DOCKPIN=1 ;;
        -h|--help) sed -n '2,20p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) printf '不认识的参数：%s（-h 看用法）\n' "$1" >&2; exit 2 ;;
    esac
    shift
done

say() { printf '%s\n' "$*"; }
die() { printf '[install] %s\n' "$*" >&2; exit 1; }

[ -x "$HERE/l2dtest" ] || die "这个目录里没有 l2dtest——请在解压出来的安装包里跑 install.sh"
for f in l2dpet l2dpet-ui l2dpet.png l2dpet-logo.png desktop-entry.sh uninstall.sh VERSION README.md BUILD-INFO; do
    [ -f "$HERE/$f" ] || die "安装包缺少 $f"
done
[ -f "$HERE/assets/Font Awesome 7 Free-Solid-900.otf" ] || die "安装包缺少图标字体"
[ -f "$HERE/share/applications/l2dpet.desktop" ] || die "安装包缺少菜单模板"
. "$HERE/desktop-entry.sh"
desktop_template() { printf '%s\n' "$HERE/share/applications/l2dpet.desktop"; }

if [ "$SYSTEM" = "1" ]; then
    [ "$(id -u)" = "0" ] || die "--system 需要 root（sudo ./install.sh --system）"
    PREFIX=${PREFIX:-/opt/l2dpet}
    BINDIR=/usr/bin
    APPDIR=/usr/share/applications
    ICONROOT=/usr/share/icons/hicolor
else
    PREFIX=${PREFIX:-$HOME/.local/opt/l2dpet}
    BINDIR=$HOME/.local/bin
    APPDIR=$HOME/.local/share/applications
    ICONROOT=$HOME/.local/share/icons/hicolor
fi

say "[install] 程序 → $PREFIX"
mkdir -p "$PREFIX" || die "建目录失败：$PREFIX"
PREFIX=$(CDPATH= cd -- "$PREFIX" && pwd -P)
[ "$PREFIX" != "$HERE" ] || die "安装目录不能与解压目录相同"
for f in l2dpet l2dtest l2dpet-ui l2dpet.png l2dpet-logo.png desktop-entry.sh VERSION README.md BUILD-INFO uninstall.sh; do
    cp -f "$HERE/$f" "$PREFIX/$f"
done
for d in tools assets models share; do
    if [ -d "$HERE/$d" ]; then
        mkdir -p "$PREFIX/$d"
        cp -Rf "$HERE/$d/." "$PREFIX/$d/"
    fi
done
chmod 755 "$PREFIX/l2dpet" "$PREFIX/l2dtest" "$PREFIX/l2dpet-ui" "$PREFIX/uninstall.sh"
# 安装标记：uninstall.sh 靠它区分"安装目录"和"解压出来的安装包目录"（两边文件长得一样）
printf '%s\n' "$PREFIX" >"$PREFIX/.installed"

# 启动器进 PATH
mkdir -p "$BINDIR"
ln -sf "$PREFIX/l2dpet" "$BINDIR/l2dpet"
say "[install] 启动器 → $BINDIR/l2dpet"
case ":$PATH:" in
    *":$BINDIR:"*) ;;
    *) say "[install] 注意：$BINDIR 不在 PATH 里——加一句 export PATH=\"\$PATH:$BINDIR\"，或直接用 $PREFIX/l2dpet" ;;
esac

# 菜单项 + 图标
if [ "$MENU" = "1" ]; then
    mkdir -p "$APPDIR" "$ICONROOT/128x128/apps"
    if [ -f "$HERE/share/applications/l2dpet.desktop" ]; then
        # 用户安装：Exec/Icon 写绝对路径（不赌 PATH 与图标主题缓存）
        if [ "$SYSTEM" = "1" ]; then
            cp -f "$HERE/share/applications/l2dpet.desktop" "$APPDIR/l2dpet.desktop"
        else
            write_desktop_file "$APPDIR/l2dpet.desktop" "$PREFIX/l2dpet" "$PREFIX/l2dpet.png"
        fi
        cp -f "$HERE/l2dpet.png" "$ICONROOT/128x128/apps/l2dpet.png"
        command -v update-desktop-database >/dev/null 2>&1 && update-desktop-database "$APPDIR" >/dev/null 2>&1 || true
        command -v gtk-update-icon-cache >/dev/null 2>&1 && gtk-update-icon-cache -q -t -f "$ICONROOT" >/dev/null 2>&1 || true
        say "[install] 应用菜单项已装（搜 \"Live2D\" 或 \"桌宠\"）"
    fi
fi

# 桌面集成（桌面图标 / Dock 固定 / 登录自启）：**统一由启动器实现**（AppImage 那边调的是同一份代码），
# 这里只负责装完顺手调一下。L2DPET_PREFIX/L2DPET_SYSTEM 告诉它"装在哪、哪个作用域"，
# 否则它会按用户级默认路径去写菜单项与图标路径。
for want in \
    "$AUTOSTART:autostart on" \
    "$DESKICON:desktop-icon" \
    "$DOCKPIN:dock"; do
    flag=${want%%:*}
    sub=${want#*:}
    if [ "$flag" = "1" ]; then
        # sub 里有空格（"autostart on"）→ 这里按词拆是故意的
        L2DPET_PREFIX="$PREFIX" L2DPET_SYSTEM="$SYSTEM" "$PREFIX/l2dpet" $sub \
            || say "[install] 提示：$sub 未完成，可在安装后重试"
    fi
done

# 模型库（包里只带一个 Live2D 官方示例模型，第一次 l2dpet 时若库为空会自动放进来；其它模型各有许可，不随包分发）
MODELS=${L2DPET_MODEL_DIR:-$HOME/.local/share/l2dpet/models}
mkdir -p "$MODELS"
if [ ! -f "$MODELS/README.txt" ]; then
    {
        echo "把 Live2D 模型放在这里：<模型名>/<x.model3.json>（整个文件夹拷进来即可）"
        echo "也可以在面板「模型」页 → 选 zip…/选目录… → 导入并识别（会自动解算能力、切过去）"
        echo "注意：模型有各自的授权（个人使用/禁二次分发等），别把别人给的模型再传出去。"
    } >"$MODELS/README.txt"
fi

n=$(find "$MODELS" -maxdepth 2 -name '*.model3.json' 2>/dev/null | wc -l)
say ""
say "[install] 完成。下一步："
if [ "$n" = "0" ]; then
    say "  l2dpet             ← 启动（首次会导入随包示例模型，也可从面板导入自己的模型）"
else
    say "  l2dpet             ← 启动（模型库里有 $n 个模型，恢复上次选择）"
fi
say "  面板：托盘图标 → 状态与设置…（或右键角色）；状态：l2dpet status"
say "  卸载：l2dpet uninstall（= $PREFIX/uninstall.sh；默认保留配置/存档/模型，加 --purge 连数据一起删）"
say ""
say "  想再要图标入口：l2dpet desktop-icon（桌面图标）/ l2dpet dock（固定到 Dock）/ l2dpet autostart on（登录自启）"

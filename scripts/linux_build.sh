#!/usr/bin/env bash
# linux_build.sh — 打 Linux 桌面安装包（桌宠 + 面板 + 启动器/安装器）
#
# 产物（默认落在仓库 dist/ 下）：
#   dist/linux-<arch>/l2dpet-<版本>-linux-<arch>.tar.gz   便携包：解压 → ./install.sh
#   dist/linux-<arch>/l2dpet-<版本>-linux-<arch>.deb      Debian/Ubuntu 包（装了 dpkg-deb 才有）
#   dist/linux-<arch>/stage/                              打包树（临时打包树，成功后删除）
#
# 做什么：
#   1) 编主机端两个二进制：l2dtest（桌宠，含内置 agent）+ l2dpet-ui（面板；EUI-NEO）
#      —— 复用 live2d/build-player.sh host 与 live2d/build-panel.sh
#   2) 组装打包树：二进制 + EUI 图标字体 + 启动器/安装器/卸载器 + .desktop/图标 + 调试工具
#      + 一个随包示例模型（live2d/bundled-models：Live2D 官方 Hiyori，Free Material License；
#        模型库为空时启动器拷进用户目录，保证开箱能跑）。其它模型各有授权，不打包；也不含你的 Key/记忆
#   3) tar.gz；装了 dpkg-deb 再顺手出 .deb（Depends 由 dpkg-shlibdeps 按符号生成最低版本）
#
# 用法：
#   scripts/linux_build.sh                   编 + 打包
#   scripts/linux_build.sh --skip-build      用现有二进制直接打包（改打包内容时快）
#   scripts/linux_build.sh --no-deb          只出 tar.gz
#   scripts/linux_build.sh --version 1.2.3   指定版本号（默认取 version.txt / git describe）
#   scripts/linux_build.sh --appimage        额外出一个 AppImage（自带依赖闭包，双击即用；
#                                            首次会下载 appimagetool 约 10MB 到 ~/l2d-build/appimage-tool/）
#   scripts/linux_build.sh --out /tmp/pkg    换输出目录
#   scripts/linux_build.sh --clean           先清掉旧的 stage
set -euo pipefail

ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
WORK=${L2D_WORK:-$HOME/l2d-build}
OUT=$ROOT/dist
VERSION=""
SKIP_BUILD=0
MAKE_DEB=auto
MAKE_APPIMAGE=0
CLEAN=0

while [ $# -gt 0 ]; do
    case $1 in
        --skip-build) SKIP_BUILD=1 ;;
        --no-deb) MAKE_DEB=no ;;
        --deb) MAKE_DEB=yes ;;
        --appimage) MAKE_APPIMAGE=1 ;;
        --version) [ $# -ge 2 ] && [ -n "$2" ] || { echo "--version 缺少值" >&2; exit 2; }; VERSION=$2; shift ;;
        --out) [ $# -ge 2 ] && [ -n "$2" ] || { echo "--out 缺少值" >&2; exit 2; }; OUT=$2; shift ;;
        --clean) CLEAN=1 ;;
        -h|--help) sed -n '2,26p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) printf '不认识的参数：%s（-h 看用法）\n' "$1" >&2; exit 2 ;;
    esac
    shift
done

say() { printf '%s\n' "$*"; }
die() { printf '[linux_build] %s\n' "$*" >&2; exit 1; }
step() { printf '\n\033[1m== %s\033[0m\n' "$*"; }

mkdir -p "$OUT"
OUT=$(cd -- "$OUT" && pwd)
ARCH=$(uname -m)
PET_BIN="$WORK/build-host/l2dtest"
UI_DIR="$WORK/build-panel"
UI_BIN="$UI_DIR/l2dpet-ui"
UI_ICON="$UI_DIR/l2dpet.png"
UI_LOGO="$UI_DIR/l2dpet-logo.png"    # 关于页的头像（面板按"程序旁边的相对路径"找；漏打=关于页空白，Windows 侧打包有这条，Linux 侧漏过）

# ---------------------------------------------------------------- 版本号
if [ -z "$VERSION" ]; then
    if [ -f "$ROOT/version.txt" ]; then
        VERSION=$(tr -d '{}\n \t' <"$ROOT/version.txt")
    fi
fi
if [ -z "$VERSION" ] && command -v git >/dev/null 2>&1 && [ -d "$ROOT/.git" ]; then
    VERSION=$(git -C "$ROOT" describe --tags --always --dirty 2>/dev/null || true)
fi
[ -n "$VERSION" ] || VERSION=0.0.0
case $VERSION in *[!A-Za-z0-9.+:~-]*) die "版本号包含非法字符：$VERSION" ;; esac
# .deb 的版本号必须以数字开头：a-1.0.0 → 0.a-1.0.0
case $VERSION in [0-9]*) DEBVER=$VERSION ;; *) DEBVER="0.$VERSION" ;; esac

NAME="l2dpet-$VERSION-linux-$ARCH"
STAGE_ROOT="$OUT/linux-$ARCH"
STAGE="$STAGE_ROOT/stage/$NAME"

step "参数"
say "仓库     : $ROOT"
say "构建目录 : $WORK"
say "版本     : $VERSION（deb: $DEBVER）"
say "架构     : $ARCH"
say "输出     : $OUT"

# ---------------------------------------------------------------- 1. 构建
if [ "$SKIP_BUILD" = "1" ]; then
    step "跳过构建（--skip-build）"
else
    step "构建桌宠（主机端；含内置 agent）"
    "$ROOT/live2d/build-player.sh" host
    step "构建面板（EUI-NEO 常驻进程）"
    "$ROOT/live2d/build-panel.sh"
fi

[ -x "$PET_BIN" ] || die "没有 $PET_BIN —— 先跑 live2d/build-player.sh host"
[ -x "$UI_BIN" ] || die "没有 $UI_BIN —— 先跑 live2d/build-panel.sh"
[ -f "$UI_ICON" ] || die "没有 $UI_ICON（面板自带的托盘图标）"
[ -f "$UI_LOGO" ] || die "没有 $UI_LOGO（关于页头像；面板源码里 live2d/panel/l2dpet-logo.png，正常不该缺）"

# 二进制架构要和包名一致（防止把别的架构的二进制打进来）
for b in "$PET_BIN" "$UI_BIN"; do
    desc=$(file -b "$b" 2>/dev/null || echo "")
    case "$ARCH:$desc" in
        x86_64:*x86-64*) ;;
        aarch64:*aarch64*|aarch64:*ARM\ aarch64*) ;;
        *) die "$b 的架构与 $ARCH 不一致（$desc）" ;;
    esac
done

# ---------------------------------------------------------------- 2. 组装打包树
step "组装打包树 → $STAGE"
if [ "$CLEAN" = "1" ]; then rm -rf -- "$STAGE_ROOT/stage"; fi
# 每次从空目录组装，避免失败后重打时混入已删除的旧资源。
rm -rf -- "$STAGE"
mkdir -p "$STAGE/assets" "$STAGE/tools" "$STAGE/share/applications" "$STAGE/share/icons/hicolor/128x128/apps"

cp -f "$PET_BIN" "$STAGE/l2dtest"
cp -f "$UI_BIN" "$STAGE/l2dpet-ui"
cp -f "$UI_ICON" "$STAGE/l2dpet.png"
cp -f "$UI_LOGO" "$STAGE/l2dpet-logo.png"
cp -f "$ROOT/scripts/linux-pack/l2dpet" "$STAGE/l2dpet"
cp -f "$ROOT/scripts/linux-pack/desktop-entry.sh" "$STAGE/desktop-entry.sh"
cp -f "$ROOT/scripts/linux-pack/install.sh" "$STAGE/install.sh"
cp -f "$ROOT/scripts/linux-pack/uninstall.sh" "$STAGE/uninstall.sh"
cp -f "$ROOT/scripts/linux-pack/README.md" "$STAGE/README.md"
# 随包示例模型（Live2D 官方 Hiyori，Free Material License）：模型库为空时启动器拷进用户目录，保证开箱能跑
if [ -d "$ROOT/live2d/bundled-models" ]; then
    mkdir -p "$STAGE/models"
    cp -R "$ROOT/live2d/bundled-models/." "$STAGE/models/"
else
    say "[!] 缺 live2d/bundled-models（包里不带示例模型：没有模型的机器起不来）"
fi
cp -f "$ROOT/scripts/linux-pack/l2dpet.desktop" "$STAGE/share/applications/l2dpet.desktop"
cp -f "$UI_ICON" "$STAGE/share/icons/hicolor/128x128/apps/l2dpet.png"
chmod 755 "$STAGE/l2dtest" "$STAGE/l2dpet-ui" "$STAGE/l2dpet" "$STAGE/install.sh" "$STAGE/uninstall.sh"

# EUI 的图标字体（面板的图标字形从 exe 同目录 assets/ 里加载；其余 assets 是 EUI 示例物料，不带）
ICON_FONT="$UI_DIR/assets/Font Awesome 7 Free-Solid-900.otf"
UI_FONT="$UI_DIR/assets/JingNanJunJunTi-JinNanJunJunTi-Bold-2.ttf"
[ -f "$ICON_FONT" ] && cp -f "$ICON_FONT" "$STAGE/assets/" || say "[!] 缺 $ICON_FONT（面板图标会退化成系统符号字体）"
[ -f "$UI_FONT" ] && cp -f "$UI_FONT" "$STAGE/assets/" || true   # 系统没有 CJK 字体时兜底（面板默认用系统字体）

# 调试工具（协议客户端等；都是标准库 Python）
for t in pet_client.py pet_agent.py pet_memory.py pet_verbs.py; do
    [ -f "$ROOT/live2d/tools/$t" ] && cp -f "$ROOT/live2d/tools/$t" "$STAGE/tools/$t"
done
cp -f "$ROOT/PROTOCOL-LLM.md" "$STAGE/tools/PROTOCOL-LLM.md" 2>/dev/null || true

printf '%s\n' "$VERSION" >"$STAGE/VERSION"

# 构建信息（出问题时一眼看出是哪次构建）
GITREV=$(git -C "$ROOT" rev-parse --short HEAD 2>/dev/null || echo "unknown")
GITDIRTY=""
git -C "$ROOT" diff --quiet 2>/dev/null || GITDIRTY="（工作区有未提交改动）"
DEPS=$( { ldd "$STAGE/l2dtest" 2>/dev/null; ldd "$STAGE/l2dpet-ui" 2>/dev/null; } \
        | awk '{print $3}' | grep '^/' | sort -u | sed 's|.*/||' | tr '\n' ' ')
{
    echo "l2dpet $VERSION"
    echo "构建时间 : $(date '+%Y-%m-%d %H:%M:%S %z')"
    echo "构建机   : $(uname -srm)"
    echo "架构     : $ARCH"
    echo "源码版本 : $GITREV $GITDIRTY"
    echo "动态库   : $DEPS"
} >"$STAGE/BUILD-INFO"

# ---------------------------------------------------------------- 3. tar.gz
step "打 tar.gz"
mkdir -p "$STAGE_ROOT"
TARBALL="$STAGE_ROOT/$NAME.tar.gz"
# 可复现参数：固定属主/排序/时间戳（mtime 用源码提交时间，没 git 就用当前时间）
MTIME=${SOURCE_DATE_EPOCH:-$(git -C "$ROOT" log -1 --format=%ct 2>/dev/null || date +%s)}
( cd "$STAGE_ROOT/stage" && tar --sort=name --owner=0 --group=0 --numeric-owner \
      --mtime="@$MTIME" -czf "$TARBALL" "$NAME" )
say "  $TARBALL"
# 自检：可执行位必须在**包子内部**（仓库里是 644 也照样要能装能跑——靠这一步兜住）
tar -tvzf "$TARBALL" | awk '{print $1, $NF}' | grep -E "/(l2dpet|install\.sh|uninstall\.sh)$" | while read -r mode path; do
    case $mode in
        -rwx*) say "  [自检] $path 可执行 ✓" ;;
        *) say "  [自检] ✗ $path 缺执行位（$mode）——装完 ./install.sh 会跑不起来"; exit 1 ;;
    esac
done || die "tar.gz 自检没过"
[ -f "$STAGE/assets/Font Awesome 7 Free-Solid-900.otf" ] || die "包里缺 EUI 图标字体（面板图标会退化）"
[ -f "$STAGE/l2dpet-logo.png" ] || die "包里缺关于页头像 l2dpet-logo.png（关于页会空白）"

# 真正安装到隔离 HOME，覆盖安装器漏文件、菜单路径含空格、安装后启动器不可用。
INSTALL_SMOKE=$(mktemp -d)
if ! HOME="$INSTALL_SMOKE/home with spaces" sh "$STAGE/install.sh" >"$INSTALL_SMOKE/install.log" 2>&1; then
    cat "$INSTALL_SMOKE/install.log" >&2
    rm -rf -- "$INSTALL_SMOKE"
    die "tar.gz 安装自检失败"
fi
IPREFIX="$INSTALL_SMOKE/home with spaces/.local/opt/l2dpet"
for f in l2dpet l2dtest l2dpet-ui l2dpet.png l2dpet-logo.png desktop-entry.sh; do
    cmp -s "$STAGE/$f" "$IPREFIX/$f" || { rm -rf -- "$INSTALL_SMOKE"; die "安装后缺失或损坏 $f"; }
done
HOME="$INSTALL_SMOKE/home with spaces" "$IPREFIX/l2dpet" version >/dev/null \
    || { rm -rf -- "$INSTALL_SMOKE"; die "安装后启动器不可用"; }
if command -v desktop-file-validate >/dev/null 2>&1; then
    desktop-file-validate "$INSTALL_SMOKE/home with spaces/.local/share/applications/l2dpet.desktop" \
        || { rm -rf -- "$INSTALL_SMOKE"; die "安装后的菜单项无效"; }
fi
rm -rf -- "$INSTALL_SMOKE"
say "  [自检] 隔离用户目录安装、资源与含空格的菜单路径 ✓"

# ---------------------------------------------------------------- 4. .deb（可选）
if [ "$MAKE_DEB" != "no" ] && command -v dpkg-deb >/dev/null 2>&1; then
    step "打 .deb"
    DEBROOT="$STAGE_ROOT/debroot"
    rm -rf -- "$DEBROOT"
    mkdir -p "$DEBROOT/DEBIAN" "$DEBROOT/opt/l2dpet" "$DEBROOT/usr/bin" \
             "$DEBROOT/usr/share/applications" "$DEBROOT/usr/share/icons/hicolor/128x128/apps"
    cp -a "$STAGE/." "$DEBROOT/opt/l2dpet/"
    rm -f "$DEBROOT/opt/l2dpet/install.sh" "$DEBROOT/opt/l2dpet/uninstall.sh"   # deb 用 dpkg 卸载
    rm -rf "$DEBROOT/opt/l2dpet/share"                                          # 菜单/图标走 /usr/share
    cp -f "$STAGE/share/applications/l2dpet.desktop" "$DEBROOT/usr/share/applications/l2dpet.desktop"
    cp -f "$STAGE/l2dpet.png" "$DEBROOT/usr/share/icons/hicolor/128x128/apps/l2dpet.png"
    # 相对软链要**数对层级**：/usr/bin/l2dpet → ../../opt/l2dpet/l2dpet（一层是 /usr/opt ✗ 断链）。
    # 断链的后果不只是"命令找不到"：GLib 判定 Exec 不可解析 → 整个 .desktop 被剔出应用列表，
    # 菜单/搜索里根本看不到这个应用（踩过：dpkg 装完"没图标"，根因就是这个）。
    ln -sf ../../opt/l2dpet/l2dpet "$DEBROOT/usr/bin/l2dpet"

    # Debian 的 symbols/shlibs 元数据包含最低符号版本，不能只按 soname 写包名。
    command -v dpkg-shlibdeps >/dev/null 2>&1 || die "打 .deb 需要 dpkg-shlibdeps（安装 dpkg-dev，或用 --no-deb）"
    SHLIBWORK="$STAGE_ROOT/shlibdeps"
    mkdir -p "$SHLIBWORK/debian"
    cat >"$SHLIBWORK/debian/control" <<'CONTROL'
Source: l2dpet
Section: utils
Priority: optional
Maintainer: vanxoak <jinsc@vanxoak.local>

Package: l2dpet
Architecture: any
Description: Live2D desktop pet
CONTROL
    RAW_DEPS=$(cd "$SHLIBWORK" && dpkg-shlibdeps -O \
        -e"$DEBROOT/opt/l2dpet/l2dtest" -e"$DEBROOT/opt/l2dpet/l2dpet-ui") \
        || die "无法生成最低依赖版本，拒绝生成可能不可运行的 .deb"
    RAW_DEPS=${RAW_DEPS#shlibs:Depends=}
    [ -n "$RAW_DEPS" ] || die "dpkg-shlibdeps 没有生成 Depends"
    # 同一 SONAME 在 Debian/Ubuntu 的 t64 改名：两边均保留符号分析得出的版本约束。
    DEPPKGS=$(printf '%s\n' "$RAW_DEPS" | awk -F ', ' '{
        for (i = 1; i <= NF; i++) {
            p = $i
            if (p ~ /^libcurl4(t64)?( |$)/) {
                sub(/^libcurl4(t64)?/, "", p); p = "libcurl4" p " | libcurl4t64" p
            } else if (p ~ /^libglib2[.]0-0(t64)?( |$)/) {
                sub(/^libglib2[.]0-0(t64)?/, "", p); p = "libglib2.0-0" p " | libglib2.0-0t64" p
            } else if (p ~ /^libpng16-16(t64)?( |$)/) {
                sub(/^libpng16-16(t64)?/, "", p); p = "libpng16-16" p " | libpng16-16t64" p
            }
            printf "%s%s", i == 1 ? "" : ", ", p
        }
        print ""
    }')
    if [ -n "${DEB_DEPS:-}" ]; then
        # 手工覆盖其它库时仍保留 glibc 与编译器运行库的最低版本。
        ABI_DEPS=$(printf '%s\n' "$RAW_DEPS" | awk -F ', ' '{
            sep = ""; for (i = 1; i <= NF; i++) if ($i ~ /^(libc6|libstdc[+][+]6|libgcc-s1)( |$)/) {
                printf "%s%s", sep, $i; sep = ", "
            }
            print ""
        }')
        DEPPKGS="$DEB_DEPS${ABI_DEPS:+, $ABI_DEPS}"
    fi
    rm -rf -- "$SHLIBWORK"
    say "  Depends: $DEPPKGS"
    ARCH_DEB=$(case $ARCH in x86_64) echo amd64 ;; aarch64) echo arm64 ;; *) echo "$ARCH" ;; esac)

    cat >"$DEBROOT/DEBIAN/control" <<EOF
Package: l2dpet
Version: $DEBVER
Section: utils
Priority: optional
Architecture: $ARCH_DEB
Depends: $DEPPKGS
Maintainer: vanxoak <jinsc@vanxoak.local>
Homepage: https://github.com/jinsc123654/vanxoak_lvgl
Description: Live2D desktop pet (character window + tray + settings panel)
 A Live2D character that lives on your desktop: transparent always-on-top window,
 drag/scroll interactions, moods and sleep, tray icon, right-click menu and a
 settings panel (themes, language, models, memory, LLM providers).
 Talks over your own OpenAI-compatible LLM API key (built-in agent, no extra
 process); without a key she only uses local lines (fully offline).
 .
 Includes the official Hiyori sample model. Import other licensed models into
 ~/.local/share/l2dpet/models/ or use the panel's Model page.
EOF
    cat >"$DEBROOT/DEBIAN/postinst" <<'EOF'
#!/bin/sh
set -e
[ -n "$XDG_DATA_DIRS" ] || true
command -v update-desktop-database >/dev/null 2>&1 && update-desktop-database /usr/share/applications >/dev/null 2>&1 || true
command -v gtk-update-icon-cache >/dev/null 2>&1 && gtk-update-icon-cache -q -t -f /usr/share/icons/hicolor >/dev/null 2>&1 || true
exit 0
EOF
    cat >"$DEBROOT/DEBIAN/postrm" <<'EOF'
#!/bin/sh
set -e
command -v update-desktop-database >/dev/null 2>&1 && update-desktop-database /usr/share/applications >/dev/null 2>&1 || true
command -v gtk-update-icon-cache >/dev/null 2>&1 && gtk-update-icon-cache -q -t -f /usr/share/icons/hicolor >/dev/null 2>&1 || true
exit 0
EOF
    chmod 755 "$DEBROOT/DEBIAN/postinst" "$DEBROOT/DEBIAN/postrm"
    DEB="$STAGE_ROOT/$NAME.deb"
    dpkg-deb --build --root-owner-group "$DEBROOT" "$DEB" >/dev/null
    say "  $DEB"

    # 冒烟自检：把 deb 解到临时目录，按"装完之后的样子"检查关键几项。
    # 教训：前面只 `dpkg-deb -c` 看文件清单，看不出软链是断的（清单里就是一行 l2dpet 软链）。
    SMOKE=$(mktemp -d)
    dpkg-deb -x "$DEB" "$SMOKE"
    bad=0
    # ① /usr/bin/l2dpet 软链必须解得开、且可执行（相对层级数错 = 断链）
    if [ -x "$SMOKE/usr/bin/l2dpet" ] && [ -f "$SMOKE/opt/l2dpet/l2dtest" ]; then
        say "  [自检] /usr/bin/l2dpet → $(readlink "$SMOKE/usr/bin/l2dpet")（解得开、可执行）✓"
    else
        say "  [自检] ✗ /usr/bin/l2dpet 解不开（$(readlink "$SMOKE/usr/bin/l2dpet" 2>/dev/null)）——菜单里会看不到这个应用"
        bad=1
    fi
    # ② .desktop 的 Exec 在装完之后必须能被解析（GLib 会因此剔除整个条目）
    #    只在**解出来的树里**判，不看宿主 PATH——否则本机恰好有个能用的同名命令就会假通过
    #    （正是这种假通过让"deb 里软链是断的"溜过去过一次）。
    _exec=$(sed -n 's/^Exec=//p' "$SMOKE/usr/share/applications/l2dpet.desktop" | head -n1)
    _execbin=${_exec%% *}
    case $_execbin in
        /*) _found="$SMOKE$_execbin" ;;
        *) _found="$SMOKE/usr/bin/$_execbin" ;;
    esac
    if [ -x "$_found" ]; then
        say "  [自检] .desktop 的 Exec=$_execbin 在包里可解析 ✓"
    else
        say "  [自检] ✗ .desktop 的 Exec=$_execbin 在包里解析不了（$_found 不存在/不可执行）——菜单里会被剔除"
        bad=1
    fi
    # ③ 图标在位
    [ -f "$SMOKE/usr/share/icons/hicolor/128x128/apps/l2dpet.png" ] \
        && say "  [自检] 图标在位 ✓" || { say "  [自检] ✗ 图标缺失"; bad=1; }
    # ③b 关于页头像在位（面板按 exe 同目录的相对路径找 l2dpet-logo.png；漏打的表现=关于页头像空白）
    [ -f "$SMOKE/opt/l2dpet/l2dpet-logo.png" ] \
        && say "  [自检] 关于页头像在位 ✓" || { say "  [自检] ✗ 关于页头像缺失（关于页会空白）"; bad=1; }
    # ④ 启动器**裸跑**必须有输出。这条专门盯一类"静默死亡"：脚本里哪怕一处
    #    `shift` 越界，dash 下也会让非交互 shell 直接退出（rc=2、后面一行不执行），
    #    表现就是敲 `l2dpet` 毫无反应——纯语法检查（sh -n）看不出来，必须真跑一次。
    #    用不存在的模型目录把它引到"找不到模型"分支（不会真起桌宠），并用临时 HOME
    #    避开真实 pidfile（机器上有实例在跑时会先打印"已经在跑了"、走不到这一步）。
    _h=$(mktemp -d)
    _lo=$(HOME=$_h L2DPET_MODEL_DIR=/nonexistent-l2dpet-selfcheck L2DPET_PORT=1 sh "$SMOKE/opt/l2dpet/l2dpet" 2>&1 || true)
    rm -rf "$_h"
    case $_lo in
        "") say "  [自检] ✗ 启动器裸跑**一点输出都没有**（脚本提前死了？shift 越界会让 dash 直接退出）"; bad=1 ;;
        *"找不到模型"*) say "  [自检] 启动器裸跑有输出（找不到模型分支）✓" ;;
        *) say "  [自检] 启动器裸跑有输出 ✓" ;;
    esac
    rm -rf "$SMOKE"
    [ "$bad" = "0" ] || die "deb 自检没过（上面带 ✗ 的项）"
else
    if [ "$MAKE_DEB" = "yes" ]; then
        die "要求打 .deb 但没装 dpkg-deb"
    fi
    say "(没装 dpkg-deb → 跳过 .deb；Debian/Ubuntu 上 apt install dpkg-dev 即可)"
fi

# ---------------------------------------------------------------- 4b. AppImage（--appimage）
if [ "$MAKE_APPIMAGE" = "1" ]; then
    step "打 AppImage（自带依赖闭包：解压即用，不装、不找依赖）"
    APPDIR="$STAGE_ROOT/AppDir"
    rm -rf -- "$APPDIR"
    mkdir -p "$APPDIR/usr/bin" "$APPDIR/usr/lib" "$APPDIR/usr/share/applications" \
             "$APPDIR/usr/share/icons/hicolor/128x128/apps"
    cp -a "$STAGE/." "$APPDIR/usr/bin/"
    rm -f "$APPDIR/usr/bin/install.sh" "$APPDIR/usr/bin/uninstall.sh"   # AppImage 不需要装/卸
    # 保留 share/ 菜单模板，启动器的桌面集成共用它。
    cp -f "$STAGE/share/applications/l2dpet.desktop" "$APPDIR/usr/share/applications/l2dpet.desktop"
    cp -f "$STAGE/share/applications/l2dpet.desktop" "$APPDIR/l2dpet.desktop"   # appimagetool 要求根目录有
    cp -f "$STAGE/l2dpet.png" "$APPDIR/l2dpet.png"
    cp -f "$STAGE/l2dpet.png" "$APPDIR/.DirIcon"
    cp -f "$STAGE/l2dpet.png" "$APPDIR/usr/share/icons/hicolor/128x128/apps/l2dpet.png"

    # 依赖闭包 → usr/lib。减掉三类**必须用宿主**的：
    #  ① glibc 家族（ld-linux/libc/libm/…）与 libstdc++/libgcc（AppImage 官方 exclude 列表：
    #     它们要和宿主的 glibc 配套，混装会炸）
    #  ② 图形驱动（GL/EGL/GLX/vulkan/nvidia——必须和宿主驱动/内核配套）
    #  ③ GLFW 用 dlopen 加载的 wayland 栈（不走 NEEDED，ldd 看不到）→ 单独补上
    _libs="$STAGE_ROOT/.libs.txt"
    for b in "$APPDIR/usr/bin/l2dtest" "$APPDIR/usr/bin/l2dpet-ui"; do
        ldd "$b" 2>/dev/null | awk '{print $3}' | grep '^/'
    done | sort -u >"$_libs"
    if command -v ldconfig >/dev/null 2>&1; then
        # awk 里**不能** `exit` 提前收工：ldconfig 会吃到 SIGPIPE，set -o pipefail 记成 141，
        # set -e 当场把脚本干掉（本脚本在这儿踩过一次）。让 awk 读完整流、只打第一行。
        _ldcfg=$(ldconfig -p 2>/dev/null || true)
        for w in libwayland-client.so.0 libwayland-cursor.so.0 libwayland-egl.so.1; do
            # 优先 /usr 下的系统库（ldconfig 顺序里可能先出 /opt 的厂家覆盖，别带那个）
            p=$(printf '%s\n' "$_ldcfg" | awk -v n="$w" '$1 == n && $NF ~ /^\/usr\// && !d { print $NF; d = 1 }')
            if [ -z "$p" ]; then
                p=$(printf '%s\n' "$_ldcfg" | awk -v n="$w" '$1 == n && !d { print $NF; d = 1 }')
            fi
            # 同理：不能写 `[ -n "$p" ] && printf ...`（循环体最后返回 1 也会被 set -e 干掉）
            if [ -n "$p" ]; then printf '%s\n' "$p" >>"$_libs"; fi
        done
        sort -u -o "$_libs" "$_libs"
    fi
    nlib=0
    while read -r lib; do
        base=$(basename "$lib")
        case $base in
            ld-linux*|libc.so*|libm.so*|libpthread*|libdl*|librt*|libresolv*|libnss_*|libutil*|libanl*|libcrypt*) continue ;;
            libgcc_s.so*|libstdc++.so*) continue ;;
            libGL.so*|libEGL*|libGLX*|libGLdispatch*|libOpenGL*|libGLES*|libvulkan*|libnvidia*) continue ;;
        esac
        cp -Lf "$lib" "$APPDIR/usr/lib/$base" && nlib=$((nlib + 1))
    done <"$_libs"
    rm -f "$_libs"
    say "  带 $nlib 个依赖（$(du -sh "$APPDIR/usr/lib" | cut -f1)）；glibc/编译器/显卡驱动仍用宿主的"

    cat >"$APPDIR/AppRun" <<'APPRUN'
#!/bin/sh
# AppRun — 把包内依赖挂上，再交给启动器（它自己找模型、起桌宠 + 面板；模型库为空时先放入随包示例模型）
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
export APPDIR=${APPDIR:-$HERE}
export LD_LIBRARY_PATH="$APPDIR/usr/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export L2DPET_UI="$APPDIR/usr/bin/l2dpet-ui"
exec "$APPDIR/usr/bin/l2dpet" "$@"
APPRUN
    chmod 755 "$APPDIR/AppRun"

    # appimagetool（缓存，不进仓库；它自己也是 AppImage → 用 --appimage-extract-and-run，免 FUSE）
    TOOL_DIR="$WORK/appimage-tool"
    case $ARCH in x86_64|aarch64) ;; *) die "AppImage 工具不支持当前架构 $ARCH" ;; esac
    TOOL="$TOOL_DIR/appimagetool-$ARCH.AppImage"
    if [ ! -x "$TOOL" ]; then
        mkdir -p "$TOOL_DIR"
        say "  下载 appimagetool（约 10MB，缓存到 $TOOL，只下一次）"
        curl -fL --retry 2 --connect-timeout 20 -o "$TOOL.part" \
            "https://github.com/AppImage/appimagetool/releases/download/continuous/appimagetool-$ARCH.AppImage" \
            || die "下载 appimagetool 失败（离线就手动塞到 $TOOL 再跑）"
        mv -f "$TOOL.part" "$TOOL"
        chmod 755 "$TOOL"
    fi
    command -v mksquashfs >/dev/null 2>&1 || die "缺 mksquashfs（apt install squashfs-tools）"
    AI_OUT="$STAGE_ROOT/$NAME.AppImage"
    rm -f -- "$AI_OUT"
    # appimagetool 自身缓存后仍会下载 type2 runtime；也缓存这份运行时，支持离线重打。
    RUNTIME=${APPIMAGE_RUNTIME_FILE:-$TOOL_DIR/runtime-$ARCH}
    if [ ! -s "$RUNTIME" ]; then
        [ -z "${APPIMAGE_RUNTIME_FILE:-}" ] || die "APPIMAGE_RUNTIME_FILE 不存在或为空：$RUNTIME"
        curl -fL --retry 2 --connect-timeout 20 -o "$RUNTIME.part" \
            "https://github.com/AppImage/type2-runtime/releases/download/continuous/runtime-$ARCH" \
            || die "下载 AppImage runtime 失败；可用 APPIMAGE_RUNTIME_FILE=<本地运行时> 离线打包"
        mv -f "$RUNTIME.part" "$RUNTIME"
    fi
    ARCH="$ARCH" "$TOOL" --appimage-extract-and-run --runtime-file "$RUNTIME" \
        --no-appstream "$APPDIR" "$AI_OUT" >/dev/null
    chmod 755 "$AI_OUT"
    say "  $AI_OUT"

    # 自检：解开跑一遍（--appimage-extract-and-run 免 FUSE，不看宿主图形会话）。
    # **用临时 HOME**：否则会读到真实用户的 pidfile，机器上正好有在跑的实例时
    # 就会先打印"已经在跑了"、根本走不到端口选择那一步（踩过）。
    bad=0
    AIHOME=$(mktemp -d)
    _st=$(HOME=$AIHOME "$AI_OUT" --appimage-extract-and-run status 2>&1 || true)
    case $_st in
        *模型库*) say "  [自检] AppImage 能跑（status 正常）✓" ;;
        *) say "  [自检] ✗ AppImage 跑不起来：$_st"; bad=1 ;;
    esac
    _bd=$(HOME=$AIHOME L2DPET_MODEL_DIR=/nonexistent-ai-check "$AI_OUT" --appimage-extract-and-run 2>&1 || true)
    case $_bd in
        "") say "  [自检] ✗ AppImage 裸跑零输出"; bad=1 ;;
        *"找不到模型"*) say "  [自检] AppImage 裸跑有输出（找不到模型分支）✓" ;;
        *) say "  [自检] AppImage 裸跑有输出 ✓" ;;
    esac
    # 会话 → 端口的选择（dry-run + 任意存在的文件冒充模型路径，只为走到那一步；不会真加载）
    _wm=$STAGE/l2dpet.png
    _wl=$(HOME=$AIHOME DISPLAY= WAYLAND_DISPLAY=wayland-0 L2DPET_DRY_RUN=1 L2DPET_MODEL="$_wm" \
          "$AI_OUT" --appimage-extract-and-run 2>&1 || true)
    case $_wl in
        *"--port glfwpet"*) say "  [自检] 纯 Wayland 会话会退回 GLFW 端口 ✓" ;;
        *) say "  [自检] ✗ Wayland 分支没走 GLFW 端口：$_wl"; bad=1 ;;
    esac
    _x11=$(HOME=$AIHOME DISPLAY=:0 WAYLAND_DISPLAY= L2DPET_DRY_RUN=1 L2DPET_MODEL="$_wm" \
           "$AI_OUT" --appimage-extract-and-run 2>&1 || true)
    case $_x11 in
        *"--port glfwpet"*) say "  [自检] ✗ X11 会话误用 GLFW 端口"; bad=1 ;;
        *--pet*) say "  [自检] X11 会话走 X11 端口 ✓" ;;
        *) say "  [自检] ✗ X11 分支异常：$_x11"; bad=1 ;;
    esac
    # 首次运行自动做的桌面集成（上面几次 dry-run 已经触发过）：菜单项要指向这个 .AppImage 文件本身
    if [ -f "$AIHOME/.local/share/applications/l2dpet.desktop" ] \
       && grep -qxF "Exec=$(sh -c ' . "$1"; desktop_entry_exec "$2"' sh "$ROOT/scripts/linux-pack/desktop-entry.sh" "$AI_OUT")" "$AIHOME/.local/share/applications/l2dpet.desktop" \
       && [ -f "$AIHOME/.local/share/l2dpet/l2dpet.png" ] && [ -x "$AIHOME/.local/bin/l2dpet" ]; then
        say "  [自检] 首次运行自动集成（菜单项→本 AppImage / 图标 / 启动器）✓"
    else
        say "  [自检] ✗ 首次运行没有做桌面集成（菜单里会搜不到它）"
        bad=1
    fi
    rm -rf "$AIHOME"
    rm -rf -- "$APPDIR"
    [ "$bad" = "0" ] || die "AppImage 自检没过（上面带 ✗ 的项）"
fi

# 清掉中间树，只留 tar.gz / deb / AppImage（打包树里全是可重建的副本）
rm -rf -- "$STAGE_ROOT/stage" "$STAGE_ROOT/debroot"

# ---------------------------------------------------------------- 5. 汇总
step "产物"
for f in "$STAGE_ROOT"/*.tar.gz "$STAGE_ROOT"/*.deb "$STAGE_ROOT"/*.AppImage; do
    [ -f "$f" ] || continue
    printf '  %-46s %s\n' "$(basename "$f")" "$(du -h "$f" | cut -f1)"
done
say ""
say "校验和："
( cd "$STAGE_ROOT" && sha256sum ./*.tar.gz ./*.deb ./*.AppImage 2>/dev/null | sed 's/^/  /' ) || true

step "怎么验"
cat <<EOF
  # 解压 + 装（用户目录，不需要 root）
  tar xzf $STAGE_ROOT/$NAME.tar.gz -C /tmp
  ( cd /tmp/$NAME && ./install.sh )
  l2dpet status            # 进程/端口/模型库；模型库为空时 l2dpet 会先放入随包示例模型 Hiyori
  # .deb
  sudo apt install $STAGE_ROOT/$NAME.deb   # 或 dpkg -i（依赖缺了会提示）
EOF

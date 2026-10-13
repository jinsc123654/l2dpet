#!/usr/bin/env bash
# Tests the real X11 port in its own GNOME/Xvfb session, never the user's desktop.
set -euo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
if [ "${1:-}" != "--inside-xvfb" ]; then
    for tool in g++ pkg-config xvfb-run dbus-run-session gnome-shell setsid; do
        command -v "$tool" >/dev/null || { echo "Missing test dependency: $tool" >&2; exit 1; }
    done
    TEST_DIR=$(mktemp -d /tmp/l2dpet-linux-windows.XXXXXX)
    trap 'rm -rf "$TEST_DIR"' EXIT
    g++ -std=c++14 -DL2D_WITH_X11_PET -I "$ROOT/live2d/player" $(pkg-config --cflags xft) \
        "$ROOT/live2d/player/tests/linux_window_test.cpp" "$ROOT/live2d/player/src/Platform.cpp" \
        -o "$TEST_DIR/test" -lX11 -lXext $(pkg-config --libs xft)
    xvfb-run -a -s '-screen 0 1280x720x24 -nolisten tcp' bash "$0" --inside-xvfb "$TEST_DIR/test"
    exit
fi
FIXTURE=$(mktemp -d /tmp/l2dpet-gnome-session.XXXXXX)
export HOME="$FIXTURE/home" XDG_CONFIG_HOME="$FIXTURE/config" XDG_CACHE_HOME="$FIXTURE/cache"
export XDG_DATA_HOME="$FIXTURE/data" XDG_RUNTIME_DIR="$FIXTURE/runtime" LIBGL_ALWAYS_SOFTWARE=1
export L2D_UI_SCALE=1
mkdir -p "$HOME" "$XDG_CONFIG_HOME" "$XDG_CACHE_HOME" "$XDG_DATA_HOME" "$XDG_RUNTIME_DIR"
chmod 700 "$XDG_RUNTIME_DIR"
setsid dbus-run-session -- gnome-shell --x11 --sm-disable >"$FIXTURE/gnome.log" 2>&1 &
WM_PID=$!
cleanup() {
    kill -- -"$WM_PID" 2>/dev/null || true
    wait "$WM_PID" 2>/dev/null || true
    # Portals can leave mounts under this test-only runtime; don't traverse a live mount.
    for portal in "$XDG_RUNTIME_DIR/gvfs" "$XDG_RUNTIME_DIR/doc"; do
        if mountpoint -q "$portal" && command -v fusermount3 >/dev/null; then
            fusermount3 -u -z "$portal" 2>/dev/null || true
        fi
    done
    if ! mountpoint -q "$XDG_RUNTIME_DIR/gvfs" && ! mountpoint -q "$XDG_RUNTIME_DIR/doc"; then
        rm -rf "$FIXTURE" 2>/dev/null || true
    fi
}
trap cleanup EXIT
if ! "$2"; then
    tail -40 "$FIXTURE/gnome.log" >&2
    exit 1
fi

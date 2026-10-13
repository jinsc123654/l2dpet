#!/usr/bin/env bash
# macOS CI packaging; graphical behavior has not been verified on real hardware.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
WORK=${L2D_WORK:-$HOME/l2d-build}
[[ $(uname -s) == Darwin ]] || { echo 'macOS required'; exit 1; }
bash "$ROOT/live2d/build-player.sh" host
bash "$ROOT/live2d/build-panel.sh"
python3 "$ROOT/scripts/package_macos.py" "$ROOT" "$WORK"

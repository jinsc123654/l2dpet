#!/usr/bin/env bash
# Check actual ELF loading and one software-rendered frame on Ubuntu 24.04.
set -euo pipefail
if [ "${1:-}" != "--inside-container" ]; then
    ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
    PACKAGE=${1:-$ROOT/dist/linux-x86_64/l2dpet-1.0.3-linux-x86_64.AppImage}
    PACKAGE_DIR=$(cd "$(dirname "$PACKAGE")" && pwd)
    docker run --rm \
        --mount "type=bind,source=$PACKAGE_DIR,target=/packages,readonly" \
        --mount "type=bind,source=$ROOT/scripts/test_appimage_ubuntu24.sh,target=/check.sh,readonly" \
        --env "PACKAGE_NAME=$(basename "$PACKAGE")" \
        ubuntu:24.04 bash /check.sh --inside-container
    exit
fi
# A minimal Ubuntu container lacks the compiler runtime and graphics libraries already
# present on Ubuntu Desktop. Install those host libraries, keeping the bundled SSL pair.
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -qq -y --no-install-recommends libstdc++6 libgl1 > /tmp/apt.log 2>&1
mkdir /tmp/appimage-check
cd /tmp/appimage-check
"/packages/$PACKAGE_NAME" --appimage-extract > /tmp/extract.log
APPDIR=$PWD/squashfs-root
export LD_LIBRARY_PATH="$APPDIR/usr/lib"
for binary in l2dtest l2dpet-ui; do
    LC_ALL=C ldd -r "$APPDIR/usr/bin/$binary" > "/tmp/$binary.ldd" 2>&1
    if grep -Eq 'not found|undefined symbol' "/tmp/$binary.ldd"; then
        cat "/tmp/$binary.ldd" >&2
        exit 1
    fi
    grep -F "libcrypto.so.3 => $APPDIR/usr/lib/libcrypto.so.3" "/tmp/$binary.ldd" >/dev/null
    grep -F "libssl.so.3 => $APPDIR/usr/lib/libssl.so.3" "/tmp/$binary.ldd" >/dev/null
    echo "PASS Ubuntu 24.04: $binary dependencies and symbol versions; SSL/crypto both from AppImage"
done
mkdir -p /tmp/frames
if ! timeout 30 "$APPDIR/usr/bin/l2dtest" \
    "$APPDIR/usr/bin/models/Hiyori/Hiyori.model3.json" 1 0.0333 \
    --port headless --render /tmp/frames --render-every 1 > /tmp/render.log 2>&1; then
    cat /tmp/render.log >&2
    exit 1
fi
if [ -z "$(find /tmp/frames -name '*.ppm' -type f -print -quit)" ]; then
    cat /tmp/render.log >&2
    echo "Player exited without producing a rendered frame" >&2
    exit 1
fi
echo "PASS Ubuntu 24.04: actual player starts, loads Hiyori and renders a frame"

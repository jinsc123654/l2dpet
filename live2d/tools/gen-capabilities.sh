#!/usr/bin/env bash
# gen-capabilities.sh — 批量生成模型能力缓存（live2d/player/capabilities/<模型>.capability.json）
#
# 每个模型跑两遍探测（协议命令，见 PROTOCOL-LLM.md §7「模型页」）：
#   ① 直连档：不跑物理，直接写参数能否动美术
#   ② 物理稳态档（{"cmd":"probe","all":true,"phys":true}）：自适应收敛后，按"轨道写值在物理之后"
#      的运行时语义再写一次
# 两档结果由播放器自动合并进 <模型目录>/l2d.capability.json（带模型指纹），本脚本收拢到输出目录。
#
# 用法：gen-capabilities.sh <模型库根> [输出目录]
#   环境：L2D_PLAYER=播放器路径（默认 ~/l2d-build/build-host/l2dtest）
#         L2D_PROBE_PORT=控制端口（默认 24444）
#         L2D_PROBE_TIMEOUT=单档超时秒（默认 240）
set -euo pipefail

ROOT=${1:?"用法: gen-capabilities.sh <模型库根> [输出目录]"}
OUT=${2:-"$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)/player/capabilities"}
PLAYER=${L2D_PLAYER:-"$HOME/l2d-build/build-host/l2dtest"}
PORT=${L2D_PROBE_PORT:-24444}
TIMEOUT=${L2D_PROBE_TIMEOUT:-240}

if [ ! -x "$PLAYER" ]; then
    echo "[x] 找不到播放器：$PLAYER（先跑 live2d/build-player.sh）" >&2
    exit 1
fi

mkdir -p "$OUT/logs"
TMP="$OUT/logs"
echo "[i] 探测日志留在 $TMP/<模型>.<档>.log"

# 一档探测：起播放器（headless + 控制端口）→ 发 probe → 等"完成"事件 → 退出
probe_pass() {  # probe_pass <模型3.json> <phys:0|1> <日志>
    local model3=$1 phys=$2 log=$3

    L2D_PROBE_PLAYER="$PLAYER" L2D_PROBE_MODEL="$model3" L2D_PROBE_PHYS="$phys" \
        L2D_PROBE_CTL="$PORT" L2D_PROBE_WAIT="$TIMEOUT" python3 - >"$log" 2>&1 <<'PY'
import json, os, socket, subprocess, sys, tempfile, time

player = os.environ["L2D_PROBE_PLAYER"]
model = os.environ["L2D_PROBE_MODEL"]
phys = os.environ["L2D_PROBE_PHYS"] == "1"
port = int(os.environ["L2D_PROBE_CTL"])
wait_s = float(os.environ["L2D_PROBE_WAIT"])

env = dict(os.environ, L2D_NO_AGENT="1", L2DPET_PORT=str(port))
log = tempfile.TemporaryFile("w+b")
p = subprocess.Popen(["setsid", player, model, "0", "0.0333", "--port", "headless",
                      "--ctl", str(port), "--size", "480x800", "--order", "static"],
                     env=env, stdout=log, stderr=subprocess.STDOUT)


def connect():
    """Authenticate before anything else: the control port refuses untokened requests."""
    from pathlib import Path
    token = (Path.home() / ".config/l2dpet" / ("control-token-%d" % port)).read_text(encoding="ascii").strip()
    sock = socket.create_connection(("127.0.0.1", port), timeout=3)
    stream = sock.makefile("rwb")
    stream.write((json.dumps({"cmd": "auth", "token": token}) + "\n").encode())
    stream.flush()
    if json.loads(stream.readline()).get("ok") is not True:
        raise SystemExit("[x] 控制端口认证失败：凭据文件对不上（%s）" % port)
    return sock, stream


def rpc(obj, sock, stream):
    stream.write((json.dumps(obj) + "\n").encode())
    stream.flush()
    return stream.readline().decode("utf-8", "replace")


deadline = time.time() + 60
while time.time() < deadline:          # 等播放器把控制端口起起来
    try:
        s, sstream = connect()
        break
    except (OSError, ValueError):
        time.sleep(0.5)
else:
    print("[x] 播放器没起来")
    p.kill()
    sys.exit(1)

sub, substream = connect()
substream.write(b'{"cmd":"subscribe","role":"ui"}\n')
substream.flush()
substream.readline()                   # 订阅回执
cmd = {"cmd": "probe", "all": True}
if phys:
    cmd["phys"] = True
line = rpc(cmd, s, sstream)
print("[i] probe 回执：", (line or "").strip()[:120])
if '"ok":true' not in (line or ""):
    print("[x] probe 被拒")
    p.kill()
    sys.exit(1)

sub.settimeout(wait_s)
buf = b""
done = False
try:
    while not done:
        chunk = sub.recv(65536)
        if not chunk:
            break
        buf += chunk
        while b"\n" in buf:
            one, buf = buf.split(b"\n", 1)
            try:
                e = json.loads(one.decode("utf-8", "replace"))
            except Exception:
                continue
            if e.get("event") == "probe" and e.get("done"):
                print("[i] probe done：saved=%s" % e.get("saved"))
                done = True
                break
except socket.timeout:
    print("[x] 等 probe 完成超时（%gs）" % wait_s)

try:
    rpc({"cmd": "quit"}, s, sstream)
    p.wait(timeout=10)
except Exception:
    p.kill()
sub.close()
s.close()
sys.exit(0 if done else 1)
PY
    return $?
}

fail=0
for d in "$ROOT"/*/; do
    name=$(basename "$d")
    model3=$(ls "$d"/*.model3.json 2>/dev/null | head -1 || true)

    if [ -z "$model3" ]; then
        continue   # 不是模型目录（如脚本文件/杂目录）
    fi
    cache="$d/l2d.capability.json"
    rm -f "$cache"   # 从零开始两档（物理档的 p 标记要求先有直连档结果）
    echo "== [$name] 直连档 =="
    if probe_pass "$model3" 0 "$TMP/$name.direct.log"; then
        echo "== [$name] 物理稳态档 =="
        if probe_pass "$model3" 1 "$TMP/$name.phys.log"; then
            cp "$cache" "$OUT/$name.capability.json"
            printf '   → %s（%s 项）\n' "$OUT/$name.capability.json" \
                "$(grep -o '"probed":[0-9]*' "$cache" | cut -d: -f2)"
        else
            echo "[!] 物理稳态档没完成（$name）：见 $TMP/$name.phys.log" >&2
            fail=1
        fi
    else
        echo "[!] 直连档没完成（$name）：见 $TMP/$name.direct.log" >&2
        fail=1
    fi
done

[ "$fail" -eq 0 ] && echo "[ok] 全部完成 → $OUT"
exit "$fail"

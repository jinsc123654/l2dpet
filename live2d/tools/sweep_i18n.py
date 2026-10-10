#!/usr/bin/env python3
"""sweep_i18n.py — 把所有内置台词逼出来，报告"还没翻译"的自带文案。

做法：以目标语言起桌宠（隔离 HOME、无 LLM Key → 所有台词都来自内置表），
把每个部位 × 点/揉、猜拳、笑话、天气、各种状态切换都打一遍，然后退出，
让桌宠打印未命中清单（L2DPET_I18N_DEBUG=1）。

两段跑：第一段只把语言写进隔离 HOME 的配置就退出；第二段带着该语言开机，
招呼语等启动台词也是目标语言。日志走匿名临时文件（不落盘）。

用法：python3 live2d/tools/sweep_i18n.py [lang] [port]
"""
import glob
import json
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import time

LANG = sys.argv[1] if len(sys.argv) > 1 else "ja"
PORT = int(sys.argv[2]) if len(sys.argv) > 2 else 23980
SWEEP_HOME = "/tmp/l2dpet-sweep-home"        # 字面量：隔离 HOME，她自己的配置/记忆都落这儿
L2D = "/home/jinsc/l2d-build/build-host/l2dtest"
MODEL_GLOB = "~/.local/share/l2dpet/models/*/*.model3.json"


def rpc(obj):
    s = socket.create_connection(("127.0.0.1", PORT), timeout=8)
    s.sendall((json.dumps(obj) + "\n").encode())
    line = s.makefile().readline()
    s.close()
    return (line or "").strip()


def models():
    return sorted(glob.glob(os.path.expanduser(MODEL_GLOB)))


def spawn(model, log):
    env = dict(os.environ)
    env.update(HOME=SWEEP_HOME, L2DPET_PORT=str(PORT), L2D_NO_AGENT="1", L2DPET_I18N_DEBUG="1")
    for k in ("LLM_API_KEY", "LLM_BASE_URL", "LLM_MODEL"):
        env.pop(k, None)
    return subprocess.Popen(
        ["setsid", L2D, model, "0", "0.0333", "--pet", "--ctl", str(PORT),
         "--size", "360x640", "--order", "static"],
        env=env, stdout=log, stderr=subprocess.STDOUT)


def stop(p, log):
    try:
        rpc({"cmd": "quit"})          # 退出时打印未命中清单
    except Exception:
        p.terminate()
    try:
        p.wait(timeout=12)
    except Exception:
        p.kill()
    log.seek(0)
    return log.read().decode("utf-8", "replace")


def warmup(model):
    """第一段：把语言/天气位置写进隔离 HOME 的配置（她自己在 RPC 里存盘）"""
    log = tempfile.TemporaryFile("w+b")
    p = spawn(model, log)
    time.sleep(6)
    rpc({"cmd": "config", "set": {"lines": 1, "idle": 1, "weather": 1, "work_remind": 0,
                                  "llm_idle": 0, "llm_questions": 0, "sleep_after": 1800}})
    rpc({"cmd": "config", "setstr": {"lang": LANG, "weather_loc": "東京"}})
    time.sleep(1)
    stop(p, log)


def drive():
    zones = ["head", "face", "neck", "chest", "belly", "crotch", "thigh.l", "thigh.r",
             "shin.l", "shin.r", "foot.l", "foot.r", "hand.l", "hand.r", "elbow.l", "elbow.r"]
    for z in zones:                       # 每个部位：点 + 揉（各两下，触发连戳/生气那套）
        for kind in ("click", "rub"):
            for _ in range(2):
                rpc({"cmd": "touch", "zone": z, "kind": kind})
                time.sleep(0.3)
    for mv in range(3):                   # 猜拳：三个出法覆盖平/胜/负
        rpc({"cmd": "game", "kind": "rps"})
        time.sleep(0.4)
        rpc({"cmd": "choice", "index": mv})
        time.sleep(0.9)
    rpc({"cmd": "joke"})
    time.sleep(0.5)
    rpc({"cmd": "weather"})               # 没 agent → 走本地兜底句（正好要它）
    time.sleep(0.5)
    for st in ("drowsy", "asleep"):       # 困 → 睡 → 睡梦中被摸/被戳醒
        rpc({"cmd": "mood", "state": st})
        time.sleep(1.2)
    rpc({"cmd": "touch", "zone": "head", "kind": "rub"})
    time.sleep(0.4)
    rpc({"cmd": "touch", "zone": "head", "kind": "click"})
    time.sleep(0.6)
    rpc({"cmd": "mood", "state": "sulk"})          # 生闷气 → 戳 → 摸头哄好
    time.sleep(1.0)
    rpc({"cmd": "touch", "zone": "head", "kind": "click"})
    time.sleep(0.4)
    for _ in range(2):
        rpc({"cmd": "touch", "zone": "head", "kind": "rub"})
        time.sleep(0.4)
    rpc({"cmd": "mood", "state": "awake", "energy": 0.02})   # 精力见底 → 累了还闹她
    time.sleep(1.0)
    for _ in range(2):
        rpc({"cmd": "touch", "zone": "belly", "kind": "click"})
        time.sleep(0.5)
    rpc({"cmd": "mood", "state": "awake", "energy": 0.5})
    time.sleep(20)                        # 闲一会儿，等她主动说话（闲时/想主人那几套）


def main():
    ms = models()
    if not ms:
        print("SKIP：模型库里没有模型")
        return 0
    if not os.path.isfile(L2D):
        print("SKIP：没找到 " + L2D)
        return 0
    shutil.rmtree(SWEEP_HOME, ignore_errors=True)
    os.makedirs(SWEEP_HOME, exist_ok=True)

    warmup(ms[0])
    log = tempfile.TemporaryFile("w+b")
    p = spawn(ms[0], log)
    time.sleep(7)
    state = {}
    try:
        drive()
        try:
            state = json.loads(rpc({"cmd": "panel_state"}))   # 退出前抓一次：最近动态/被摸的地方
        except Exception:
            state = {}
    finally:
        text = stop(p, log)

    said = []
    for ln in text.splitlines():
        if "「" in ln and "」" in ln and ("[life]" in ln or "[pet]" in ln):
            said.append(ln.split("「", 1)[1].rsplit("」", 1)[0])
    print("扫了一遍：她说了 %d 次。样本：" % len(said))
    for s in said[:10]:
        print("   ", s)

    life = (state.get("state") or {}).get("life") or {}
    feed = [(it[1], it[2]) for it in (life.get("feed") or []) if isinstance(it, list) and len(it) >= 3]
    zones = [z[0] for z in (life.get("zones") or []) if isinstance(z, list) and z]
    print("\n最近动态 %d 条（后 12 条）：" % len(feed))
    for kind, txt in feed[-12:]:
        print("    [%s] %s" % (kind, txt))
    print("被摸的地方：%s" % "、".join(zones))
    # 日语模式下不该再出现"纯中文"（有汉字、没假名）的句子——用户上次截图就是这种
    def has_kana(s):
        return any("\u3040" <= c <= "\u30ff" for c in s)

    def has_han(s):
        return any("\u4e00" <= c <= "\u9fff" for c in s)

    weird = [s for s in said if not has_kana(s) and has_han(s)]
    weird += ["[%s] %s" % (k, t) for k, t in feed if not has_kana(t) and has_han(t)]
    if LANG == "ja" and weird:
        print("\n== 日语模式下疑似还是中文（%d 条）==" % len(weird))
        for s in weird[:15]:
            print("   ", s)

    miss = ""
    if "[i18n] 未翻译的自带文案" in text:
        tail = text.split("[i18n] 未翻译的自带文案", 1)[1]
        miss = tail.split("\n", 1)[1].strip() if "\n" in tail else tail.strip()
    if miss:
        lines = [l.strip() for l in miss.splitlines() if l.strip()]
        print("\n== 还没翻译（%d 条）==" % len(lines))
        for l in lines:
            print("   ", l)
        return 1
    print("\n== 全命中：所有说出口的自带文案都翻译了 ==")
    return 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""pet_client.py — 桌宠协议调试客户端（协议见仓库根 PROTOCOL-LLM.md）

用法：
  pet_client.py query [summary skeleton expressions motions actions parts params all]
  pet_client.py status
  pet_client.py send '{"cmd":"action","fv":"act:wave_big"}'   # 自动补 model 特征值
  pet_client.py play <时间线.json>                              # 文件内容 = sequence 对象
  pet_client.py events                                         # 订阅事件（Ctrl-C 退出）
环境：PET_PORT（默认 23333）
"""
import json
import os
import pathlib
import socket
import sys
from pet_bridge import authenticate

HOST = "127.0.0.1"
PORT = int(os.environ.get("PET_PORT", "23333"))


class Pet:
    def __init__(self):
        self.sock = socket.create_connection((HOST, PORT), timeout=5)
        authenticate(self.sock, PORT)
        self.buf = b""
        self.seq = 0
        self.model = None

    def _readline(self, timeout=5):
        self.sock.settimeout(timeout)
        while b"\n" not in self.buf:
            chunk = self.sock.recv(65536)
            if not chunk:
                raise ConnectionError("连接已关闭")
            self.buf += chunk
        line, self.buf = self.buf.split(b"\n", 1)
        return json.loads(line.decode("utf-8"))

    def call(self, obj):
        self.seq += 1
        obj.setdefault("seq", self.seq)
        if obj.get("cmd") not in ("query", "status", "subscribe") and "model" not in obj:
            if self.model is None:
                self.model = self.call({"cmd": "status"})["model"]
            obj["model"] = self.model
        self.sock.sendall((json.dumps(obj, ensure_ascii=False) + "\n").encode("utf-8"))
        return self._readline()


def _read_timeline(arg):
    """play 的文件参数：规范化后必须落在当前目录子树内的 .json（调试工具，不做任意路径读）"""
    path = os.path.realpath(arg)
    root = os.path.realpath(os.getcwd())
    if not path.startswith(root + os.sep):
        raise SystemExit(f"时间线文件须在当前目录内：{arg}（请 cd 到所在目录再运行）")
    if not path.endswith(".json") or not os.path.isfile(path):
        raise SystemExit("play 只接受 .json 时间线文件")
    return json.loads(pathlib.Path(path).read_text(encoding="utf-8"))


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 1
    pet = Pet()
    op = argv[1]
    if op == "query":
        reply = pet.call({"cmd": "query", "what": argv[2:] or ["summary"]})
    elif op == "status":
        reply = pet.call({"cmd": "status"})
    elif op == "send":
        reply = pet.call(json.loads(argv[2]))
    elif op == "play":
        obj = _read_timeline(argv[2])
        obj.setdefault("cmd", "sequence")
        reply = pet.call(obj)
    elif op == "events":
        print(json.dumps(pet.call({"cmd": "subscribe"}), ensure_ascii=False))
        try:
            while True:
                print(json.dumps(pet._readline(timeout=3600), ensure_ascii=False), flush=True)
        except KeyboardInterrupt:
            return 0
    else:
        print(__doc__)
        return 1
    print(json.dumps(reply, ensure_ascii=False, indent=1))
    return 0 if reply.get("ok") else 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))

#!/usr/bin/env python3
"""ui_click_test.py — 桌宠 UI 进程（l2dpet-ui）真实鼠标回归测试

用 XTEST 扩展（X server 级，等同真实输入设备）注入点击，验证拆分后的 UI 链路：
  1. 右键角色 → ui_menu 事件 → 菜单窗口弹出（含翻转/钳位）
  2. 真实左键点菜单第 1 项"状态与设置…" → 设置主窗口映射、菜单关闭
  3. speak 带选项 → 气泡窗口弹出 → 真实点选项 → choice 事件、气泡收起
  4. panel page:-1 → ui_close → 设置窗口取消映射

依赖：python-xlib（XTEST）、DISPLAY 指向运行中的桌宠（--ctl 端口默认 23333）。
会移动鼠标指针（结束后还原）。2026-10-09 首跑全过；修复 page:-1 后复跑全过。

用法：python3 live2d/tools/ui_click_test.py [--port 23333]
"""
import argparse
import json
import socket
import sys
import time

from Xlib import X, display
from Xlib.ext import xtest

d = display.Display()
root = d.screen().root


def ptr():
    p = root.query_pointer()
    return p.root_x, p.root_y


def warp(x, y):
    xtest.fake_input(d, X.MotionNotify, x=x, y=y)
    d.sync()


def click(button, x, y):
    warp(x, y)
    time.sleep(0.03)
    xtest.fake_input(d, X.ButtonPress, button)
    d.sync()
    time.sleep(0.06)
    xtest.fake_input(d, X.ButtonRelease, button)
    d.sync()


def wm_name(wid):
    try:
        p = wid.get_full_property(d.intern_atom('_NET_WM_NAME'), d.intern_atom('UTF8_STRING'))
        if p and p.value:
            return bytes(p.value).decode('utf-8', 'replace')
    except Exception:
        pass
    n = wid.get_wm_name()
    if n:
        try:
            return n.encode('latin-1').decode('utf-8')   # python-xlib 把 UTF-8 当 latin-1 的还原
        except Exception:
            return n
    return None


def find(name):
    """按窗口名找窗口 → [(id, name, root_x, root_y, w, h, mapped)]"""
    res = []
    for wid in root.query_tree().children:
        try:
            n = wm_name(wid)
            if n and name in n:
                g, wa = wid.get_geometry(), wid.get_attributes()
                tr = wid.translate_coords(root, 0, 0)
                res.append((wid.id, n, -tr.x, -tr.y, g.width, g.height, wa.map_state == 2))
        except Exception:
            pass
    return res


def cmd(port, payload):
    c = socket.create_connection(("127.0.0.1", port), timeout=5)
    from pet_bridge import authenticate
    authenticate(c, port)
    c.sendall((json.dumps(payload) + "\n").encode())
    r = c.recv(4096).decode().strip()
    c.close()
    return r


def wait_gone(name, timeout=2.5):
    """轮询等待窗口消失（淡出/销毁有延迟，固定 sleep 会偶发假失败）"""
    t0 = time.time()
    while time.time() - t0 < timeout:
        if not [b for b in find(name) if b[6]]:
            return True
        time.sleep(0.15)
    return False


def wait_mapped(name, timeout=2.5):
    """轮询等待窗口出现并映射"""
    t0 = time.time()
    while time.time() - t0 < timeout:
        ws = [b for b in find(name) if b[6]]
        if ws:
            return ws[0]
        time.sleep(0.15)
    return None


class Mon:
    """subscribe role=ui，收事件流（结束后断开；不计入 agent 数）"""

    def __init__(self, port):
        self.sock = socket.create_connection(("127.0.0.1", port), timeout=5)
        from pet_bridge import authenticate
        authenticate(self.sock, port)
        self.sock.sendall(b'{"cmd":"subscribe","role":"ui"}\n')
        self.sock.settimeout(0.2)
        self.buf = b""

    def wait_event(self, pred, timeout=5.0):
        t0 = time.time()
        while time.time() - t0 < timeout:
            try:
                d_ = self.sock.recv(65536)
                if not d_:
                    return None
                self.buf += d_
            except socket.timeout:
                pass
            while b"\n" in self.buf:
                line, self.buf = self.buf.split(b"\n", 1)
                if not line.strip():
                    continue
                try:
                    ev = json.loads(line)
                except Exception:
                    continue
                if pred(ev):
                    return ev
        return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=23333)
    args = ap.parse_args()

    pets = find("Live2D")
    if not pets:
        print("x 找不到桌宠角色窗口（先启动 l2dtest --pet）")
        return 1
    _, _, px, py, pw, ph, _ = pets[0]
    print(f"[i] 角色窗口 @({px},{py}) {pw}x{ph}")

    ok = True
    saved = ptr()
    mon = Mon(args.port)
    try:
        # 1) 右键 → 菜单
        click(3, px + pw // 2, py + int(ph * 0.55))
        ev = mon.wait_event(lambda e: e.get("event") == "ui_menu")
        menus = find("桌宠菜单")
        print(("ok " if ev and menus else "FAIL ") + f"右键→ui_menu {ev} 菜单窗 {menus[0][2:6] if menus else '无'}")
        ok &= bool(ev and menus)
        if not menus:
            return 1
        _, _, mx, my, mw, mh, _ = menus[0]
        scale = mw / 240.0
        click(1, mx + mw // 2, my + int(59 * scale))     # 第 1 项"状态与设置…"（8+34+17 逻辑px）
        opened = wait_mapped("状态与设置") is not None
        gone = wait_gone("桌宠菜单")
        print(("ok " if opened and gone else "FAIL ") + f"点菜单项→设置窗映射={opened} 菜单关闭={gone}")
        ok &= opened and gone

        # 2) 气泡选项（气泡已回桌宠进程直绘：X11 窗 "l2dpet-bubble"，无 ui_bubble 事件）
        cmd(args.port, {"cmd": "speak", "text": "在忙什么呀？", "options": ["在写代码", "在摸鱼"], "speak_dur": 8})
        time.sleep(1.2)
        bubs = [b for b in find("l2dpet-bubble") if b[6]]
        print(("ok " if bubs else "FAIL ") + f"气泡窗 {bubs[0][2:6] if bubs else '无'}")
        if not bubs:
            return 1
        # 选项胶囊位置从气泡窗口像素里找（深青描边簇，下半区；旧版 SDF 气泡配色 (27,78,96)）
        import subprocess as _sp
        import os as _os
        from PIL import Image as _Img
        _, _, bx, by, bw, bh, _ = bubs[0]
        _bubpng = f"/tmp/l2d-click-bub-{_os.getpid()}.png"
        _sp.run(["import", "-window", hex(bubs[0][0]), _bubpng])
        im = _Img.open(_bubpng).convert("RGBA")
        p = im.load()
        # 找选项胶囊：扫描"长青行"（≥80px 连续深青 = 胶囊上下边；文字抗锯齿的短段 ≤50px、
        # 椭圆描边是细线，都不会命中），取第一对（上边 + 下边）的中心作为点击点
        wide_rows = []
        for y in range(im.height):
            cur = None
            for x in range(im.width):
                # 胶囊描边是实心深青 (27,78,96)：收紧颜色 + 要求近乎不透明
                if (p[x, y][3] > 200 and p[x, y][0] < 60 and 60 < p[x, y][1] < 105 and 80 < p[x, y][2] < 130):
                    if cur is None:
                        cur = [x, x]
                    else:
                        cur[1] = x
                else:
                    if cur is not None and cur[1] - cur[0] >= 80:
                        wide_rows.append((y, cur[0], cur[1]))
                    cur = None
            if cur is not None and cur[1] - cur[0] >= 80:
                wide_rows.append((y, cur[0], cur[1]))
        pill = None
        for i in range(len(wide_rows)):
            y0, x0, x1 = wide_rows[i]
            for j in range(i + 1, len(wide_rows)):
                y1 = wide_rows[j][0]
                if 25 <= y1 - y0 <= 80:          # 上下边间距 = 胶囊高
                    pill = ((x0 + x1) // 2, (y0 + y1) // 2)
                    break
            if pill:
                break
        if pill is None:
            print("FAIL 气泡里没找到选项胶囊（长青行配对失败）")
            return 1
        cx, mid_y = pill
        click(1, bx + cx, by + mid_y)
        ev = mon.wait_event(lambda e: e.get("event") == "choice")
        hidden = wait_gone("l2dpet-bubble")   # 选中后 0.22s 淡出 → 轮询等消失
        print(("ok " if ev and hidden else "FAIL ") + f"点选项→choice {ev} 气泡收起={hidden}")
        ok &= bool(ev) and hidden

        # 3) panel page:-1 → ui_close → 设置窗收起
        cmd(args.port, {"cmd": "panel", "page": 0})
        opened = wait_mapped("状态与设置") is not None
        cmd(args.port, {"cmd": "panel", "page": -1})
        ev = mon.wait_event(lambda e: e.get("event") == "ui_close")
        closed = wait_gone("状态与设置")
        print(("ok " if opened and ev and closed else "FAIL ")
              + f"设置窗开={opened} ui_close={bool(ev)} page:-1 后未映射={closed}")
        ok &= opened and bool(ev) and closed
    finally:
        warp(*saved)
        mon.sock.close()
    print("[=] 全过" if ok else "[x] 有失败项")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())

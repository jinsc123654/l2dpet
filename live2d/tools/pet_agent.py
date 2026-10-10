#!/usr/bin/env python3
"""pet_agent.py — 让桌宠"自己动起来"的示例 Agent（协议见仓库根 PROTOCOL-LLM.md）

流程（设计见 live2d/DESIGN-PET-LIFE.md）：query 能力表 → 订阅事件 → （--llm）让 LLM 按人设写好即时台词库，
      {"cmd":"reactions"} 下发给桌宠本地用 → 三层交互：
        即时：桌宠本地（每次点/搓立即看向+动作+情绪+台词，不经本程序）
        回应：用户停手 2s 后给一段 ≤6s 的【回应】（replace 打断闲时段；两次 ≥10s）
        闲时：没人互动时，平均 llm_period 秒一段【闲时】小日常
      桌宠的 mood（醒着/犯困/睡着/生闷气 + 情绪 + 亲密度…）写进每段提示；犯困/睡着时不发闲时段；
      config 事件（桌宠"状态与设置"窗口）实时生效：llm_idle / llm_period / llm_questions。
      回执里的 warnings 带进下一轮。每次 LLM 调用记一条日志：上报桌宠（"对话 → 交互日志"页）+
      ~/.local/state/l2dpet/llm-log.jsonl。桌宠换模型（model_changed）时重新 query、重写台词库。
      注意：必须在桌宠端口就绪后启动；桌宠重启后要重启本程序（台词库在桌宠内存里）。

记忆（跨会话；pet_memory.py，设计见 live2d/DESIGN-PET-LIFE.md §10）：
      ~/.config/l2dpet/memory/owner.md（关于主人，所有角色共享）+ chars/<模型指纹>.md（我和主人之间）
      + habits/pending/episodes；全部 0600、人可读可手改。戳/摸只计数；主人的选项回答、生闷气/被哄好/被吵醒
      是重要要点；待整理权重攒够 --memory-every 或每 --memory-idle 秒归纳一次（LLM 合并去重、压缩旧事）。
      用法：注入每段上下文 + 按情境从归档召回往事；久别重逢开场叙旧；对主人了解不足时用选项问一个小问题；
      记忆更新后（≥--lines-refresh 秒）带记忆重生成即时台词库。--no-memory 全关；--memory-tidy 按当前规则重整。

两种生成方式：
  --demo        不需要 LLM：按能力表随机组合（动作 / 姿势套 / 表情 / 视线 / 台词），本机直接看效果
  --llm         OpenAI 兼容接口（默认 DeepSeek）。接入参数优先级：环境变量 > 桌宠「状态与设置 → 对话」里填的
                （存 ~/.config/l2dpet/llm.json，0600）> 默认值；没有 key 时不退出，等设置里填好自动开始：
                  LLM_API_KEY  LLM_BASE_URL（默认 https://api.deepseek.com）  LLM_MODEL（默认 deepseek-chat）
                  LLM_ALLOW_PRIVATE=1（端点在本机/内网时显式放开，如 Ollama/LM Studio）
其它：--period 秒（闲时段间隔的默认值，默认 60，另加 0~50% 随机；桌宠设置里的 llm_period 优先）
      --port（默认 23333 / 环境变量 PET_PORT）  --once（只跑一段）  --show（打印步骤）
      --memory-every 权重（默认 6）  --memory-idle 秒（默认 1200）  --lines-refresh 秒（默认 900）
      --no-memory  --memory-tidy

节制（代码里强制，不靠 LLM 自觉）：
  - 每段最多 2 句台词、最多换 2 次表情（多的只删台词/表情、保留动作）；【回应】段截到 6s；
  - 带选项的提问：≥ 5 分钟一次、每段最多 1 个、必须挂在 speak 步上；问出去后等用户选或 25s 超时，
    期间不下发闲时段；设置 llm_questions=0 时一律剥掉；
  - 用户点了选项（choice）总是立即回应。
"""
import argparse
import functools
import http.client
import ipaddress
import json
import os
import random
import re
import socket
import threading
import time
import urllib.parse

HOST = "127.0.0.1"
_print = functools.partial(print, flush=True)   # 输出重定向到文件时也即时可见


def print(*a, **k):   # 每行带时分秒：看得出事件到 LLM 回应之间隔了多久
    _print(time.strftime("%H:%M:%S"), *a, **k)

SYSTEM_PROMPT = """你在控制一个 Live2D 桌宠。下面是它的能力表（JSON），每项有 name（含义）和 fv（引用键）。
{caps}
规则：
- 只输出一个 JSON：{{"cmd":"sequence","model":"{model}","mode":"queue","dur":<秒>,"steps":[…]}}，不要其它文字。
- step 可用的键：at（秒）、speak（中文台词）、emotion（情绪，见下）、action（act:…）、expression（exp:… 或 null）、motion（mtn:…）、
  look（viewer/left/right/up/down）、pose（{{"set":[pose:…],"hands":[hand:…],"joints":{{joint:…:值}},"parts":{{part:…:值}},"t":秒}}）。
- 只能引用能力表里出现的 fv；关节值必须在 range 内；parts 的值是相对自然姿态的增量。
- skeleton 为空时不要写 pose.set / joints，用表情、头部、视线、说话和预置动作来表达。
- 你的台词（speak 的文字）会显示成角色头顶的**气泡**。**少说话**：每段最多 1~2 句，大多数步骤只做
  动作/表情/视线，不要每步都说话。偶尔（很少）可以向用户提问：在那一步加
  "options":["选项一","选项二"]（最多 3 个、每个 ≤6 字），用户点选后你会收到
  {{"event":"choice","label":"…"}} 事件，据此继续剧情（别再重复问过的问题）。提问之后的同一段里不要再说别的话。
- 你会实时收到用户和角色的身体交互事件：click（点击某区域，n=连点次数；手=牵手、脸=捏脸、其余=戳）、
  rub（在某区域来回拖动；头、手=摸，其余=揉）、hover（鼠标停在区域上）。说话时用这些动词（牵/捏/戳、摸/揉）。区域见能力表 zones（head 头部/face 脸部/belly 肚子/
  hand.l 左手…，左右按屏幕方位）。桌宠本地已经对每次点/搓**立即**做了看向+动作+一句短台词，
  所以你不用逐次复述，而是对"这一阵互动"做整体回应。
- 角色有自己的内在状态（用户消息里会给出）：awake 醒着 / drowsy 犯困 / sulk 生闷气（扭头不理人）/
  asleep 睡着；affection 亲密度 0~1（越高越黏人、越放得开），annoy 恼火，lonely 寂寞。
  你写的内容必须和当前状态一致：生闷气时就别突然热情（可以傲娇、嘴硬、慢慢软化），亲密度低时客气些、
  高时撒娇。桌宠本地会自己犯困、睡觉、生气、主动找人，你不用安排这些，只负责"说什么、怎么演"。
- 段落分两种（用户消息里会说明是哪种）：
  · 【回应】用户刚和你互动完：dur 3~5 秒、2~3 步，第一步（at=0）就说一句针对部位和轻重的话，
    配 1 个贴合的动作或表情；不要加 look_around/跳/欢呼之类无关填充动作。
  · 【闲时】没人理你：8~15 秒的小日常，动作和表情为主，可以没有台词。
- 情绪优先用 emotion 键：{{"emotion":"happy"}} 或 {{"emotion":{{"name":"shy","intensity":0.8}}}}，可选
  happy/excited/shy/affectionate/curious/confused/tired/sad/anxious/angry/surprised/calm/sulky。
  它会让嘴角、眉眼、头姿、脸红连续地变过去并自然回落（不会硬切），强烈时还会自动套用模型自带的对应表情；
  所以大多数时候不用写 expression。expression 只在需要特定造型时用，
  每段最多换 2 次；
  眼罩、手柄、麦克风、猫猫手、星星等道具/装扮类表情除非剧情明确需要否则不要用。
- 最后一步用 {{"pose":{{"set":[]}}}} 收回姿势，并把 expression 设为 null 复原。
- 台词里**不要出现任何日期、时间或时间戳**（如 10-10、02:09、10月10日）；记忆里的时间只是记录用的，不是名字也不是称呼。
  称呼主人只用记忆里明确写出的名字/称呼，没有就叫"你"。"""

LINES_PROMPT = """为一个 Live2D 桌宠写"被触碰时的即时台词库"。角色：{name}。
用户可以点击（click：手=牵、脸=捏、其余=戳）或来回拖动（rub：头和手=摸、其余=揉）她身体的这些部位：{zones}。
输出一个 JSON：{{"lines":{{"<部位键>.click":[3 句],"<部位键>.rub":[3 句],…,"angry":[4 句]}}}}
- 部位键用上面括号里的英文键；每个部位都要有 .click 和 .rub。angry 是被同一处连戳好几次时说的。
- 另外还要这些生活场景键（各 3 句）：life.lonely（没人理、主动找用户搭话）、life.welcome（用户离开很久回来）、
  life.drowsy（犯困打哈欠）、life.wake（睡饱醒来）、life.woken（睡着被戳醒）、life.sulk_start（被戳烦了开始生气）、
  life.sulk（生闷气时又被戳）、life.forgive（被摸头哄好）、life.enjoy（被持续摸头很享受）、life.sleep_talk（梦话）。
- 每句 ≤12 个字，口语、俏皮、有反应感，符合部位与动作（摸头舒服、捏脸害羞、牵手开心、揉肚子怕痒、戳胸部/胯部会生气、戳脚心怕痒…）；
  同一部位的几句语气要有变化，不要用表情符号；台词里不要出现日期、时间或时间戳（如 10-10、02:09），记忆里的时间只是记录，不是称呼。只输出 JSON。{memory_block}"""


class Conn:
    def __init__(self, port):
        self.sock = socket.create_connection((HOST, port), timeout=10)
        self.buf = b""
        self.lock = threading.Lock()

    def send(self, obj):
        with self.lock:
            self.sock.sendall((json.dumps(obj, ensure_ascii=False) + "\n").encode("utf-8"))
            return self.readline(10)

    def readline(self, timeout):
        self.sock.settimeout(timeout)
        while b"\n" not in self.buf:
            chunk = self.sock.recv(65536)
            if not chunk:
                raise ConnectionError("桌宠连接已关闭")
            self.buf += chunk
        line, self.buf = self.buf.split(b"\n", 1)
        return json.loads(line.decode("utf-8"))


# ---------------------------------------------------------------- 生成：随机组合（--demo）

LINES = ["你好呀，今天过得怎么样？", "要不要休息一下？", "我在这儿陪着你。", "嗯……让我想想。",
         "这个主意不错！", "加油，马上就好了。", "有什么想聊的吗？", "哈哈，真有意思。"]


def demo_sequence(caps, model, react=None):
    rnd = random.SystemRandom()
    acts = [a["fv"] for a in caps.get("actions", [])]
    exps = [e["fv"] for e in caps.get("expressions", [])]
    poses = [p for arm in caps.get("skeleton", {}).values() for p in arm["poses"]]
    steps, t = [], 0.0
    if react:
        # 用户交互的即时短反应：(kind, 区域中文名, 连点次数)
        kind, zone, cnt = react
        if kind == "rub":
            say = f"唔……{zone}被揉得好痒啦～"
        elif kind == "choice":
            say = "好哦，那就这么定！"
        else:
            say = f"呀！{zone}被戳了{'好几下' if cnt and cnt > 1 else '一下'}～"
        step = {"at": 0.0, "speak": say,
                "action": "act:shy" if "act:shy" in acts else (rnd.choice(acts) if acts else None)}
        if exps and rnd.random() < 0.6:
            step["expression"] = rnd.choice(exps)
        steps.append({k: v for k, v in step.items() if v is not None})
        return {"cmd": "sequence", "model": model, "mode": "replace", "dur": 4, "steps": steps}
    for _ in range(rnd.randint(2, 4)):
        step = {"at": round(t, 1), "speak": rnd.choice(LINES)}
        r = rnd.random()
        if poses and r < 0.45:
            p = rnd.choice(poses)
            pose = {"set": [p["fv"]], "t": 0.5}
            if p.get("joints"):
                j = rnd.choice(p["joints"])
                lo, hi = j["range"]
                pose["joints"] = {j["fv"]: round(lo + (hi - lo) * rnd.uniform(0.3, 0.7), 1)}
            if p.get("hands"):
                pose["hands"] = [rnd.choice(p["hands"])["fv"]]
            step["pose"] = pose
        elif acts:
            step["action"] = rnd.choice(acts)
        if exps and rnd.random() < 0.5:
            step["expression"] = rnd.choice(exps)
        if rnd.random() < 0.4:
            step["look"] = rnd.choice(["viewer", "left", "right", "up"])
        steps.append(step)
        t += rnd.uniform(3.0, 4.5)
    steps.append({"at": round(t, 1), "pose": {"set": []}, "expression": None})
    return {"cmd": "sequence", "model": model, "mode": "queue", "dur": round(t + 1.5, 1), "steps": steps}


# ---------------------------------------------------------------- 生成：LLM（--llm）

LLM_CFG_PATH = os.path.expanduser("~/.config/l2dpet/llm.json")     # 桌宠"对话"页写的（权限 0600）
LLM_LOG_PATH = os.path.expanduser("~/.local/state/l2dpet/llm-log.jsonl")
_last_call = {}   # 最近一次 LLM 调用的 {ms, tok_in, tok_out}


def llm_settings():
    """接入参数：环境变量优先，其次桌宠设置窗口写的 ~/.config/l2dpet/llm.json，最后默认值。"""
    f = {}
    try:
        with open(LLM_CFG_PATH, encoding="utf-8") as fp:
            f = json.load(fp)
    except (OSError, ValueError):
        pass
    return {"key": os.environ.get("LLM_API_KEY") or f.get("api_key") or "",
            "model": os.environ.get("LLM_MODEL") or f.get("model") or "deepseek-chat",
            "base_url": os.environ.get("LLM_BASE_URL") or f.get("base_url") or "https://api.deepseek.com"}


def _chat_endpoint(base_url=None):
    """校验接口地址，返回 (scheme, host, port, api_path)。
    只允许干净的 http/https；主机解析出的 IP 必须是公网（内网/回环/链路本地一律拒绝，
    指向本机 Ollama/LM Studio 这类本地端点时需显式设 LLM_ALLOW_PRIVATE=1）。"""
    base = (base_url or llm_settings()["base_url"]).rstrip("/")
    p = urllib.parse.urlparse(base)
    if (p.scheme not in ("http", "https") or not p.netloc
            or p.query or p.fragment or p.username or p.password
            or any(c in base for c in "\r\n\t")):
        raise ValueError("LLM_BASE_URL 只允许干净的 http/https 地址")
    if not p.hostname:
        raise ValueError("LLM_BASE_URL 缺少主机名")
    port = p.port or (443 if p.scheme == "https" else 80)
    api_path = ((p.path.rstrip("/") + "/") if p.path else "/") + "chat/completions"
    if os.environ.get("LLM_ALLOW_PRIVATE") != "1":
        infos = socket.getaddrinfo(p.hostname, port, proto=socket.IPPROTO_TCP)
        for ip in sorted({ai[4][0] for ai in infos}):
            a = ipaddress.ip_address(ip)
            # 198.18.0.0/15 = Clash/mihomo fake-ip：DNS 在本机代理环境下会给公网域名
            # 返回这个段，实际经 TUN 代理出去——它就是这台机器的出口，不算内网目标
            via_fake_ip = a in ipaddress.ip_network("198.18.0.0/15")
            if not via_fake_ip and (a.is_private or a.is_loopback or a.is_link_local
                                    or a.is_reserved or a.is_multicast or a.is_unspecified):
                raise ValueError(f"LLM_BASE_URL 解析到内网或本机地址 {ip}；"
                                 "本地端点请显式设 LLM_ALLOW_PRIVATE=1")
    return p.scheme, p.hostname, port, api_path


def _llm_chat(scheme, host, port, api_path, body, key):
    """POST chat/completions（http.client 直连：不跟随重定向；响应最多读 2MB）；用时/用量记进 _last_call"""
    t0 = time.time()
    cls = http.client.HTTPSConnection if scheme == "https" else http.client.HTTPConnection
    conn = cls(host, port, timeout=60)
    try:
        conn.request("POST", api_path, body=body,
                     headers={"Content-Type": "application/json", "Authorization": "Bearer " + key})
        resp = conn.getresponse()
        data = resp.read(2 * 1024 * 1024)
    finally:
        conn.close()
    if resp.status != 200:
        raise RuntimeError(f"LLM 端点返回 HTTP {resp.status}：{data[:200].decode('utf-8', 'replace')}")
    obj = json.loads(data)
    usage = obj.get("usage") or {}
    _last_call["ms"] = _last_call.get("ms", 0) + int((time.time() - t0) * 1000)
    _last_call["tok_in"] = _last_call.get("tok_in", 0) + int(usage.get("prompt_tokens", 0))
    _last_call["tok_out"] = _last_call.get("tok_out", 0) + int(usage.get("completion_tokens", 0))
    return obj["choices"][0]["message"]["content"]


def llm_sequence(caps, model, history, note, memory=""):
    cfg = llm_settings()
    scheme, host, port, api_path = _chat_endpoint(cfg["base_url"])
    key = cfg["key"]
    if not key:
        raise RuntimeError("没有 API Key（在桌宠「状态与设置 → 对话」里填，或设环境变量 LLM_API_KEY）")
    msgs = [{"role": "system", "content": SYSTEM_PROMPT.format(
        caps=json.dumps(caps, ensure_ascii=False), model=model)}]
    if memory:
        msgs.append({"role": "system", "content":
                     "【你记得的事（跨会话记忆，自动整理；可能不全，别生硬复述）】\n" + memory
                     + "\n台词可以偶尔自然呼应记忆里的往事或主人的偏好，但大多数时候自然聊天就好。"})
    msgs += history[-6:]
    msgs.append({"role": "user", "content": note})
    for attempt in range(3):
        body = json.dumps({"model": cfg["model"], "messages": msgs,
                           "temperature": 0.9, "response_format": {"type": "json_object"}}).encode("utf-8")
        text = _llm_chat(scheme, host, port, api_path, body, key)
        text = re.sub(r"^```(json)?|```$", "", text.strip(), flags=re.M).strip()
        try:
            seq = json.loads(text)
            break
        except json.JSONDecodeError as e:
            # LLM 偶尔写出不合法的 JSON：把出错位置附近的原文打出来，并让它按错误重写
            print(f"[agent] LLM 输出不是合法 JSON（{e.msg}，第 {e.pos} 字符附近：…{text[max(0, e.pos - 60):e.pos + 20]}…），重试 {attempt + 1}/2")
            msgs.append({"role": "assistant", "content": text})
            msgs.append({"role": "user", "content": f"上面的输出不是合法 JSON：{e.msg}（第 {e.pos} 字符）。请只输出修正后的完整 JSON。"})
    else:
        raise RuntimeError("LLM 连续 3 次输出不合法 JSON")
    seq["cmd"] = "sequence"
    seq["model"] = model
    for st in seq.get("steps") or []:   # 台词去日期/时间戳（兜底：提示词里已要求不写）
        if isinstance(st, dict) and isinstance(st.get("speak"), str):
            st["speak"] = strip_stamps(st["speak"])
            if not st["speak"]:
                st.pop("speak")
        if isinstance(st, dict) and isinstance(st.get("options"), list):
            st["options"] = [strip_stamps(o) if isinstance(o, str) else o for o in st["options"]]
    history.append({"role": "user", "content": note})
    history.append({"role": "assistant", "content": json.dumps(seq, ensure_ascii=False)})
    return seq


def llm_react_lines(caps, memory=""):
    """让 LLM 按人设写好"即时台词库"，下发给桌宠（之后点/搓不再等 LLM）。
    memory 非空时：台词可以带着共同记忆（每组最多 1 句呼应，其余保持通用）。"""
    cfg = llm_settings()
    if not cfg["key"]:
        raise RuntimeError("没有 API Key")
    scheme, host, port, api_path = _chat_endpoint(cfg["base_url"])
    zones = "、".join(f"{z['name']}({z['fv'].split(':', 1)[1]})" for z in caps.get("zones", []))
    mb = ""
    if memory:
        mb = ("\n- 你记得的事（跨会话记忆）——每组台词里最多 1 句可以自然呼应它（主人的称呼、"
              "一起经历过的小事、偏好），其余句子保持通用：这些台词会被长期反复播放，"
              "太具体会让主人听腻、也会露馅。\n" + memory + "\n")
    prompt = LINES_PROMPT.format(name=caps["model"]["name"], zones=zones, memory_block=mb)
    body = json.dumps({"model": cfg["model"],
                       "messages": [{"role": "user", "content": prompt}], "temperature": 0.9,
                       "response_format": {"type": "json_object"}}).encode("utf-8")
    text = _llm_chat(scheme, host, port, api_path, body, cfg["key"])
    lines = json.loads(re.sub(r"^```(json)?|```$", "", text.strip(), flags=re.M).strip()).get("lines", {})
    return {k: [strip_stamps(t) for t in v if isinstance(t, str) and 0 < len(strip_stamps(t)) <= 20][:6]
            for k, v in lines.items() if isinstance(v, list)}


# ---------------------------------------------------------------- 记忆（pet_memory.py：分层、按角色、0600）

# 同目录模块。python3 -I（含 -P）不会把脚本目录放进 sys.path，所以按本文件位置显式加——只加这一个固定目录
import sys  # noqa: E402
sys.path.insert(0, os.path.dirname(os.path.realpath(__file__)))
from pet_memory import PetMemory, touch_verb, strip_stamps  # noqa: E402


def fmt_gap(sec):
    if sec >= 86400:
        return f"{int(sec // 86400)} 天"
    if sec >= 3600:
        return f"{int(sec // 3600)} 小时"
    return f"{int(sec // 60)} 分钟"


def llm_json(prompt, temperature=0.3):
    """一次性 JSON 调用（记忆归纳等）：返回 dict；失败抛异常"""
    cfg = llm_settings()
    if not cfg["key"]:
        raise RuntimeError("没有 API Key")
    scheme, host, port, api_path = _chat_endpoint(cfg["base_url"])
    body = json.dumps({"model": cfg["model"], "messages": [{"role": "user", "content": prompt}],
                       "temperature": temperature, "response_format": {"type": "json_object"}}).encode("utf-8")
    text = _llm_chat(scheme, host, port, api_path, body, cfg["key"])
    return json.loads(re.sub(r"^```(json)?|```$", "", text.strip(), flags=re.M).strip())


# ---------------------------------------------------------------- 主循环

# ---------------------------------------------------------------- 节制（不让 LLM 介入过于频繁）

MAX_SPEAK_PER_SEG = 2        # 每段最多几句台词
QUESTION_COOLDOWN = 300.0    # 带选项的提问最短间隔（秒）
QUESTION_WAIT = 25.0         # 提问后最多等多久（等用户选；超时视为没选）
QUIET_AFTER_TOUCH = 2.0      # 用户停手多久后给出【回应】段（还在点/搓时只有本地即时反应）
RESPOND_COOLDOWN = 10.0      # 两次【回应】段的最短间隔（秒）
RESPOND_MAX_DUR = 6.0        # 【回应】段最长（秒）：超出的步骤截掉
MAX_EXPR_PER_SEG = 2         # 每段最多换几次表情（null 复原不算）


def throttle(seq, now, last_question_at, allow_questions=True):
    """就地削减：台词 ≤ MAX_SPEAK_PER_SEG；提问 ≤1 个且受冷却。返回 (是否含提问, 删掉了什么)"""
    steps = seq.get("steps", [])
    speaks, asked, cut = 0, False, []
    for st in steps:
        if "options" in st:
            if not allow_questions or asked or now - last_question_at < QUESTION_COOLDOWN or "speak" not in st:
                st.pop("options")
                cut.append("提问")
            else:
                asked = True
        if "speak" in st:
            speaks += 1
            if speaks > MAX_SPEAK_PER_SEG and "options" not in st:
                st.pop("speak")
                cut.append("台词")
    exprs = 0
    for st in steps:
        if st.get("expression"):
            exprs += 1
            if exprs > MAX_EXPR_PER_SEG:
                st.pop("expression")
                cut.append("表情")
    seq["steps"] = [st for st in steps if len(set(st) - {"at"}) > 0]
    return asked, cut


def clamp_respond(seq):
    """【回应】段：截到 RESPOND_MAX_DUR 秒内，并保证最后能收回姿势/表情"""
    steps = [st for st in seq.get("steps", []) if float(st.get("at", 0)) <= RESPOND_MAX_DUR - 0.5]
    if len(steps) < len(seq.get("steps", [])):
        steps.append({"at": RESPOND_MAX_DUR - 0.5, "pose": {"set": []}, "expression": None})
    seq["steps"] = steps
    seq["dur"] = min(float(seq.get("dur", RESPOND_MAX_DUR)), RESPOND_MAX_DUR)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    mode = ap.add_mutually_exclusive_group(required=True)
    mode.add_argument("--demo", action="store_true")
    mode.add_argument("--llm", action="store_true")
    ap.add_argument("--period", type=float, default=60.0, help="【闲时】段的平均间隔（秒）")
    ap.add_argument("--port", type=int, default=int(os.environ.get("PET_PORT", "23333")))
    ap.add_argument("--once", action="store_true")
    ap.add_argument("--show", action="store_true", help="打印每段时间线的步骤")
    ap.add_argument("--memory-every", type=int, default=6,
                    help="待整理要点的权重攒到多少就归纳一次记忆（一般事 1、主人的选择/关系变化 2；默认 6）")
    ap.add_argument("--memory-idle", type=float, default=1200.0,
                    help="有待整理的事、但攒不够时，最多隔多久也归纳一次（秒，默认 1200）")
    ap.add_argument("--lines-refresh", type=float, default=900.0,
                    help="记忆更新后带新记忆重生成台词库的最小间隔（秒，默认 900）")
    ap.add_argument("--no-memory", action="store_true", help="关闭记忆（不读不写、不刷台词库）")
    ap.add_argument("--memory-tidy", action="store_true",
                    help="启动时按当前规则把现有记忆重新整理一遍（改了归纳规则 / 手改乱了之后用）")
    args = ap.parse_args()
    memory_on = args.llm and not args.no_memory

    ctl = Conn(args.port)
    zone_zh = {}

    def load_caps():
        c = ctl.send({"cmd": "query", "what": ["summary", "skeleton", "expressions", "motions",
                                               "actions", "parts", "zones"]})
        zone_zh.clear()
        zone_zh.update({z["fv"].split(":", 1)[1]: z["name"] for z in c.get("zones", [])})
        print(f"[agent] 模型 {c['model']['name']}（{c['model']['fv']}）骨架={c['summary']['skeleton']} "
              f"动作={len(c.get('actions', []))} 表情={len(c.get('expressions', []))} "
              f"区域={len(c.get('zones', []))}")
        return c, c["model"]["fv"]

    caps, model = load_caps()

    def zone_cn(key):
        return zone_zh.get(key or "", key or "?")

    def summarize(seq):
        """一段时间线 → 日志里的"她说了什么、做了什么" """
        say, emos, acts = [], [], []
        for st in seq.get("steps", []):
            if st.get("speak"): say.append(str(st["speak"]))
            e = st.get("emotion")
            if e: emos.append(e.get("name") if isinstance(e, dict) else str(e))
            if st.get("action"): acts.append(str(st["action"]).replace("act:", ""))
            if st.get("expression"): acts.append("表情:" + str(st["expression"]).replace("exp:", ""))
        return say, emos, acts

    def report(kind, note="", seq=None, ok=True, err="", asked=False, warn=0):
        """一次 LLM 交互 → 桌宠（"对话 → 交互日志"页）+ 本地 JSONL 日志（不含密钥）"""
        say, emos, acts = summarize(seq) if seq else ([], [], [])
        entry = {"t": time.strftime("%H:%M:%S"), "kind": kind, "ok": ok, "question": asked, "warn": warn,
                 "ms": _last_call.get("ms", 0), "tok_in": _last_call.get("tok_in", 0),
                 "tok_out": _last_call.get("tok_out", 0), "note": note[:300], "say": say[:4],
                 "emotions": emos[:6], "actions": acts[:6], "err": err[:200]}
        _last_call.clear()
        try:
            ctl.send({"cmd": "llm_log", "entry": entry})
        except Exception:
            pass
        try:
            os.makedirs(os.path.dirname(LLM_LOG_PATH), exist_ok=True)
            with open(LLM_LOG_PATH, "a", encoding="utf-8") as fp:
                fp.write(json.dumps(entry, ensure_ascii=False) + "\n")
        except OSError:
            pass

    lines_ok = [False]
    lines_meta = {"last": 0.0, "dirty": False}   # 台词库：上次下发时刻 / 记忆更新后待刷新
    # 记忆（pet_memory.py）：owner.md 共享 + chars/<角色>.* 按角色；box = [当前角色的 PetMemory 或 None]
    mem_box = [None]
    mem_meta = {"last": time.time()}             # 上次归纳时刻

    def open_memory():
        if not memory_on:
            mem_box[0] = None
            return
        fp = model.split(":", 1)[-1]
        m = PetMemory(fp, caps["model"]["name"])   # 文件按模型指纹分：同一模型换目录/改名仍是同一个"她"
        mem_box[0] = m
        gap = "第一次见面" if m.gap < 0 else f"距上次 {int(m.gap // 60)} 分钟"
        print(f"[agent] 记忆：{m.char_path.name}（{gap}；关于主人 {m.owner_known()} 条；"
              f"待整理权重 {m.pending_weight()}）")

    def mem_ctx(query=""):
        return strip_stamps(mem_box[0].context(query, zone_cn)) if mem_box[0] else ""

    # 睡眠省 token：睡着即暂停一切 LLM 调用；只有主人点她（或被戳醒）才恢复——自己睡饱醒来只用本地台词库
    paused = [False]

    def set_paused(v, why):
        if paused[0] != v:
            paused[0] = v
            print(f"[agent] {why}：" + ("暂停一切 LLM 调用，点她一下才恢复" if v else "恢复 LLM 调用"))

    def consolidate(reason, tidy=False):
        """归纳待整理要点进记忆；成功则推面板 + 标记台词库待刷新"""
        m = mem_box[0]
        if m is None or not llm_settings()["key"] or paused[0]:
            return
        try:
            changed = m.consolidate(lambda pr: llm_json(pr), zone_cn, tidy=tidy)
            report("记忆", f"归纳记忆（{reason}）", {"steps": []}, ok=True)
            mem_meta["last"] = time.time()
            if changed:
                lines_meta["dirty"] = True
                push_memory()
                print(f"[agent] 记忆已更新（{reason}）：关于主人 {m.owner_known()} 条 → {m.char_path.parent}")
        except Exception as ex:
            report("记忆", f"归纳记忆（{reason}）", None, ok=False, err=str(ex))
            mem_meta["last"] = time.time()   # 失败也别每圈重试；要点留在 pending 里
            print(f"[agent] 记忆归纳失败（{ex}），要点留着下次再试")

    open_memory()

    def announce():
        """把"用的哪来的 key / 哪个模型"告诉桌宠（不含 key 本身），面板据此显示"""
        st = llm_settings()
        src = "env" if os.environ.get("LLM_API_KEY") else ("file" if st["key"] else "none")
        try:
            ctl.send({"cmd": "agent_info", "key_source": src, "model": st["model"], "base_url": st["base_url"]})
        except Exception:
            pass

    announce()

    def push_lines(reason="启动"):
        """生成并下发即时台词库（没有 key 时跳过，等设置里填好再来）；带记忆生成 → 离线台词会"记得你" """
        if paused[0]:
            return
        if not args.llm or not llm_settings()["key"]:
            if args.llm:
                print("[agent] 还没有 API Key：在桌宠「状态与设置 → 对话」里填好后自动开始")
            return
        try:
            lines = llm_react_lines(caps, mem_ctx())
            r = ctl.send({"cmd": "reactions", "lines": lines})
            lines_ok[0] = True
            lines_meta["last"] = time.time()
            lines_meta["dirty"] = False
            report("台词库", f"为 {caps['model']['name']} 生成即时台词库（{reason}）",
                   {"steps": []}, ok=True)
            print(f"[agent] 即时台词库（{reason}）：{r.get('groups')} 组 {r.get('lines')} 句 → 桌宠本地使用")
            sample = []
            for k in ("head.click", "head.rub", "life.welcome"):
                v = lines.get(k) or []
                if v:
                    sample.append(f"{k}: {v[0]}")
            if sample:
                print("[agent] 台词样例：" + " ｜ ".join(sample))
        except Exception as ex:   # 失败就用桌宠内置兜底台词
            report("台词库", "生成即时台词库", None, ok=False, err=str(ex))
            print(f"[agent] 台词库生成失败（{ex}），桌宠用内置台词")

    events, ev_lock = [], threading.Lock()

    def listen():
        ev = Conn(args.port)
        ev.send({"cmd": "subscribe"})
        while True:
            try:
                e = ev.readline(3600)
            except Exception:
                return
            with ev_lock:
                events.append(e)

    threading.Thread(target=listen, daemon=True).start()

    def push_memory():
        """把记忆文本推给桌宠（面板"行为 → 记忆本"页显示）；旧版桌宠不认这个命令，忽略即可"""
        if mem_box[0] is None:
            return
        try:
            ctl.send({"cmd": "memory", "text": mem_box[0].panel_text(zone_cn), "at": time.strftime("%m-%d %H:%M")})
        except Exception:
            pass

    if mem_box[0] is not None and (mem_box[0].pending_weight() > 0 or args.memory_tidy):
        consolidate("重新整理" if args.memory_tidy else "上次没整理完", tidy=args.memory_tidy)
    push_lines("启动")
    push_memory()

    history, last_warn, n = [], [], 0
    next_at = 0.0
    last_llm_at = 0.0             # 上次调用 LLM 的时刻
    last_question_at = -1e9       # 上次（真正发出的）提问时刻
    awaiting_until = 0.0          # 提问等待截止（>now 期间不下发新段，除非用户选了）
    backlog = {}                  # 还没回应的交互：部位 → [点击次数, 搓揉次数]
    last_touch_at = 0.0           # 最近一次点/搓的时刻（停手 QUIET_AFTER_TOUCH 秒后回应）
    last_hover = ""
    st0 = ctl.send({"cmd": "status"})
    mood = st0.get("mood") or {"state": "awake"}
    cfg = st0.get("config") or {}   # 桌宠"状态与设置"窗口里的 LLM 参与度（config 事件实时更新）
    mood_cn = {"awake": "醒着", "drowsy": "犯困", "asleep": "睡着了", "sulk": "在生闷气"}

    def mood_text():
        emo = (mood.get("emotion") or {})
        return (f"【她现在{mood_cn.get(mood.get('state'), '醒着')}，情绪 {emo.get('name', 'neutral')}"
                f"（强度 {emo.get('intensity', 0):.2f}），亲密度 {mood.get('affection', 0.3):.2f}，"
                f"恼火 {mood.get('annoy', 0):.2f}，寂寞 {mood.get('lonely', 0):.2f}】")

    def backlog_text():
        items = sorted(backlog.items(), key=lambda kv: -(kv[1][0] + kv[1][1]))
        parts = []
        for zone, (c, r) in items[:6]:
            seg = []
            if r: seg.append(f"被{touch_verb(zone, True)}了 {r} 次")
            if c: seg.append(f"被{touch_verb(zone, False)}了 {c} 次")
            parts.append(f"{zone_cn(zone)}" + "、".join(seg))
        return "；".join(parts)
    last_q = {"text": "", "opts": []}   # 最近一次提问（选项）——choice 回来时记进记忆
    # 久别重逢：开场第一段就"叙旧"（离开 >30 分钟 / 第一次见面）
    reunion = ""
    if mem_box[0] is not None and args.llm:
        g = mem_box[0].gap
        if g < 0:
            reunion = "【重逢】这是你第一次见到主人：简短打个招呼、介绍一下自己（一两句）。"
        elif g > 1800:
            reunion = (f"【重逢】主人离开了约 {fmt_gap(g)} 后回来了：用一句话打招呼；如果记忆里有合适的事"
                       "（上次的约定、让你开心或生气的事、主人的习惯），自然地提一句，不要列举、不要像念记录。")
    while True:
        if mood.get("state") == "asleep" and not paused[0]:
            set_paused(True, "她睡着了")
        if mem_box[0] is not None:
            mem_box[0].heartbeat()
            # 有待整理的事、但攒不够 --memory-every：最多隔 --memory-idle 秒也归纳一次（不让新鲜事一直悬着）
            m = mem_box[0]
            if time.time() - mem_meta["last"] >= args.memory_idle and (m.pending_weight() > 0 or m.session_touch):
                consolidate("定时")
        # 记忆更新过 → 按 --lines-refresh 的节奏把离线台词库也刷成"记得你"的版本（闲时也检查）
        if memory_on and lines_meta["dirty"] and time.time() - lines_meta["last"] >= args.lines_refresh:
            push_lines("记忆更新")
        interrupt = reunion   # 需要立刻回应的用户交互（【回应】/【重逢】段）
        reunion = ""
        ambient = []     # 弱信号：只进下一段的上下文
        react = None     # demo 模式的即时反应素材 (kind, 区域名, 次数)
        with ev_lock:
            pending, events[:] = list(events), []
        for e in pending:
            ev = e.get("event")
            if ev is None or ev == "done" or str(ev).startswith("ui_"):
                continue   # ui_* 是桌宠给 UI 进程的（菜单/气泡/打开设置），agent 不管
            if ev == "llm_config":
                info = e.get("llm") or {}
                print(f"[agent] 接入设置已更新：key={'已设' if info.get('key_set') else '未设'} "
                      f"model={info.get('model') or '默认'}")
                announce()
                if not lines_ok[0]:
                    push_lines("设置更新")
                continue
            if ev == "config":
                cfg = e.get("config") or cfg
                print(f"[agent] 设置更新：闲时小剧场={'开' if cfg.get('llm_idle', 1) else '关'} "
                      f"间隔={cfg.get('llm_period', args.period)}s 提问={'允许' if cfg.get('llm_questions', 1) else '不允许'}")
                continue
            if ev == "mood":
                mood = e
                if paused[0] and e.get("state") == "awake":
                    if e.get("why") == "被戳醒":
                        set_paused(False, "主人把她戳醒了")
                    elif e.get("why") == "睡饱了":
                        print("[agent] 她自己睡醒了：只用本地台词，点她一下才恢复 LLM")
                if e.get("why") != "tick":
                    print(f"[agent] 心情：{mood_cn.get(e.get('state'), e.get('state'))}（{e.get('why')}）"
                          f" 亲密 {e.get('affection')} 恼火 {e.get('annoy')} 寂寞 {e.get('lonely')}")
                    why = str(e.get("why", ""))
                    if mem_box[0] is not None:
                        # 只记"关系里的事"：生闷气/被哄好/被吵醒（权重 2）、自己消气/寂寞找主人（1）；
                        # 犯困、睡着、睡饱、被碰醒这些日常节律不记（会把记忆刷成流水账）
                        w = 2 if any(k in why for k in ("烦", "哄好", "戳醒")) else \
                            (1 if any(k in why for k in ("气消", "寂寞")) else 0)
                        if w:
                            mem_box[0].note(f"我{mood_cn.get(e.get('state'), e.get('state'))}了（{why}）", w)
                    if why.startswith("你回来了") and not interrupt and args.llm:
                        mins = re.search(r"(\d+) 分钟", why)
                        interrupt = (f"【重逢】主人离开了约 {mins.group(1) if mins else '好一会儿'} 分钟后回来了，"
                                     "你刚已经说了句欢迎；现在再自然地接一句（可以结合记忆提一件小事，别列举）。")
                continue
            if ev != "hover":
                print("[agent] 事件", json.dumps(e, ensure_ascii=False))
            if ev == "model_changed":
                # 换模型：重新 query 能力表、清对话上下文、重新生成台词库（台词按新角色来）
                if mem_box[0] is not None:
                    mem_box[0].save_habits(force=True)   # 旧角色的待整理要点留在磁盘，下次见到它再归纳
                caps, model = load_caps()
                open_memory()
                push_memory()
                history.clear()
                backlog = {}
                last_warn = []
                lines_ok[0] = False
                push_lines("换模型")
                next_at = time.time() + 5.0
                continue
            if ev == "click" and paused[0] and mood.get("state") != "asleep":
                set_paused(False, "主人点了她")
            if ev in ("click", "rub"):
                z = e.get("zone")
                backlog.setdefault(z, [0, 0])[0 if ev == "click" else 1] += 1
                last_touch_at = time.time()
                if mem_box[0] is not None:
                    mem_box[0].touch(z, ev)   # 只计数（习惯统计），不进 LLM 归纳
                react = (ev, zone_cn(z), e.get("n", 1))
            elif ev == "choice":
                label = e.get("label", "")
                interrupt = f"【回应】用户在气泡选项里选了「{label}」，接着这个选择继续（这段可以到 8 秒）。"
                if mem_box[0] is not None and last_q["text"] and label:
                    mem_box[0].choice(last_q["text"], label)   # 主人的回答：重要记忆（只认我刚问过的题）
                last_q["text"] = ""
                react = ("choice", label, 0)
                awaiting_until = 0.0
            elif ev == "hover":
                last_hover = zone_cn(e.get("zone"))
        # 用户停手了 → 对这一阵互动整体回应（本地已逐次即时反应，这里补"有思考的"那一句）
        if paused[0]:
            backlog.clear()   # 暂停中：不产生任何 LLM 段（本地已经即时反应过了）
            time.sleep(0.1)
            continue
        now_b = time.time()
        sleepy = mood.get("state") in ("asleep", "drowsy")
        if sleepy and backlog and now_b - last_touch_at >= QUIET_AFTER_TOUCH:
            backlog = {}   # 睡着时被碰：桌宠本地处理（迷糊/被吵醒），醒了会有 mood 事件
        if not interrupt and backlog and now_b - last_touch_at >= QUIET_AFTER_TOUCH \
                and now_b - last_llm_at >= RESPOND_COOLDOWN:
            awaiting_until = 0.0
            interrupt = "【回应】用户刚才和你身体互动：" + backlog_text() \
                + "。以次数最多的部位为主，按轻重（摸/揉=舒服/亲昵，牵手=开心，捏脸=害羞，连戳=恼）回应一句。"
            backlog = {}
        if not interrupt and (sleepy or not cfg.get("llm_idle", 1)):
            next_at = max(next_at, now_b + 10.0)   # 犯困/睡着 / 设置里关了小剧场：不安排闲时表演
        if not interrupt and (now_b < next_at or now_b < awaiting_until or backlog or sleepy
                              or not cfg.get("llm_idle", 1)):
            time.sleep(0.1)
            continue
        if not interrupt:
            interrupt = "【闲时】"
            m = mem_box[0]
            if (m is not None and m.owner_known() < 3 and cfg.get("llm_questions", 1)
                    and now_b - last_question_at >= QUESTION_COOLDOWN):
                asked_before = "；".join(m.habits.get("choices", [])[-6:]) or "无"
                interrupt += ("（你还不太了解主人：这段可以在一句台词上用 options 问主人一个了解他的小问题——"
                              f"称呼/作息/喜好之类，2~3 个短选项；问过的别再问：{asked_before}）")
        if last_hover:
            ambient.append(f"用户的鼠标刚在你的{last_hover}附近")
            last_hover = ""
        n += 1
        note = f"第 {n} 段。" + mood_text() + interrupt \
            + (("；" + "；".join(ambient) + "。") if ambient else "") \
            + (f"上一段的警告：{json.dumps(last_warn, ensure_ascii=False)}，请避免。" if last_warn else "")
        t0 = time.time()
        kind = "续戏" if "选项里选了" in interrupt else ("回应" if interrupt.startswith("【回应】")
                                                       else ("重逢" if interrupt.startswith("【重逢】") else "闲时"))
        if args.llm and not llm_settings()["key"]:
            next_at = time.time() + 10.0   # 没 key：等设置
            continue
        try:
            seq = demo_sequence(caps, model, react) if args.demo \
                else llm_sequence(caps, model, history, note, mem_ctx(interrupt))
        except Exception as ex:
            # 网络/接口/格式错误：记一条失败日志，30s 后再试（不退出）
            print(f"[agent] LLM 调用失败：{ex}")
            if args.llm:
                report(kind, note, None, ok=False, err=str(ex))
            last_llm_at = time.time()
            next_at = time.time() + 30.0
            continue
        last_llm_at = time.time()
        responding = interrupt.startswith("【回应】") or interrupt.startswith("【重逢】")
        if responding:
            seq["mode"] = "replace"   # 打断正在播的闲时段，立刻回应
            if "选项" not in interrupt:
                clamp_respond(seq)
        print(f"[agent] {'回应' if responding else '闲时'}段，LLM 用时 {last_llm_at - t0:.1f}s")
        asked, cut = throttle(seq, time.time(), last_question_at, bool(cfg.get("llm_questions", 1)))
        if cut:
            print(f"[agent] 节流：删掉 {'、'.join(cut)}")
        seq["seq"] = f"s{n}"   # 段落编号：done 事件据此对上
        if args.show:
            for st in seq.get("steps", []):
                print("    ", json.dumps(st, ensure_ascii=False))
        reply = ctl.send(seq)
        if not reply.get("ok") and reply.get("code") == "queue_full":
            # 自己的段还没播完（实际时长 > 声明的 dur）：抢占，别把整条链路堵死
            seq["mode"] = "replace"
            reply = ctl.send(seq)
            print("[agent] 队列满 → replace 重发")
        last_warn = reply.get("warnings", [])
        fail = "" if reply.get("ok") else f" code={reply.get('code')} msg={reply.get('msg')}"
        print(f"[agent] 第 {n} 段 dur={seq.get('dur')} → ok={reply.get('ok')} accepted={reply.get('accepted')} "
              f"warnings={len(last_warn)}{fail}")
        if args.llm:
            report(kind, note, seq, ok=bool(reply.get("ok")), asked=asked, warn=len(last_warn),
                   err="" if reply.get("ok") else str(reply.get("msg", "")))
        for st in seq.get("steps", []):
            if st.get("options") and reply.get("ok"):
                last_q["text"], last_q["opts"] = str(st.get("speak", "")), \
                    [o.get("label", "") if isinstance(o, dict) else str(o) for o in st["options"]]   # 选项可带 do
        if mem_box[0] is not None and kind in ("续戏", "重逢"):
            # 记忆的原料只要"关系里的事"：主人的选择后她怎么接、久别重逢说了什么（戳/摸只计数）
            say, _emos, _acts = summarize(seq)
            if say:
                mem_box[0].note(f"（{kind}）我说：" + "/".join(say[:2]), 1)
        if args.once:
            return
        if asked and reply.get("ok"):
            last_question_at = time.time()
            awaiting_until = time.time() + QUESTION_WAIT
        # 待整理要点攒够权重 → 归纳进记忆（失败要点留在 pending；成功后按节奏刷台词库）
        if mem_box[0] is not None and mem_box[0].pending_weight() >= max(1, args.memory_every):
            consolidate("攒够了")
        gap = float(cfg.get("llm_period", args.period)) * (1.0 + 0.5 * random.SystemRandom().random())
        next_at = time.time() + max(gap, float(seq.get("dur", 8)) + 5.0)


if __name__ == "__main__":
    main()

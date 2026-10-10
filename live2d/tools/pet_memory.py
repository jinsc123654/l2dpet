"""pet_memory.py — 桌宠的长期记忆（pet_agent.py 用；设计见 live2d/DESIGN-PET-LIFE.md §记忆）

分三层，全部是人可读、可手改的本地文件（权限 0600，目录 0700）：

  ~/.config/l2dpet/memory/
    owner.md                     关于主人（**所有角色共享**：称呼、作息、喜好、忌讳、约定……）
    （<角色> = 模型指纹，如 0xab07a53d5d0b0092：同一模型换目录/改名仍是同一个"她"，与桌宠亲密度存档同口径）
    chars/<角色>.md              我和主人之间（**每个角色一份**：最近发生 / 更早）
    chars/<角色>.habits.json     习惯统计（程序算，不经 LLM）：各部位摸/戳次数、几点常来、见面次数…
    chars/<角色>.pending.jsonl   还没归纳的要点（随时落盘——agent 被杀也不丢，下次启动先归纳）
    chars/<角色>.episodes.jsonl  归档：每次归纳用过的原始要点（永不删；按相关度召回往事）

为什么这样分：
  - 主人是同一个人，换角色不该"失忆"；但角色之间的私事不该串（Natori 不该记得你戳过 shuflle）。
  - 戳/摸这类高频事件不进 LLM 归纳（会把记忆刷成流水账），只计数；归纳时给一份"本轮统计"即可。
  - 工作记忆（md）有字数上限、会压缩；细节进归档，用到时按关键词召回，不会永久丢失。
"""
import json
import os
import pathlib
import re
import time

ROOT = pathlib.Path(os.path.expanduser("~/.config/l2dpet/memory"))
LEGACY = pathlib.Path(os.path.expanduser("~/.config/l2dpet/memory.md"))   # 旧版单文件（自动迁移）
OWNER_MAX = 700        # owner.md 上限（字符）
CHAR_MAX = 1600        # chars/<角色>.md 上限（字符）
EMPTY_OWNER = "## 关于主人\n（还不了解）\n"
EMPTY_CHAR = "## 最近发生\n（还没有）\n\n## 更早\n（空）\n"


def _safe_name(s):
    """角色文件名：只留字母数字和少量符号（角色 id 来自模型指纹/目录名，防路径穿越）"""
    s = re.sub(r"[^0-9A-Za-z_\-一-鿿]", "_", s or "pet")
    return s[:64] or "pet"


def _write_private(path, text):
    """0600 原子写（tmp + rename）"""
    path.parent.mkdir(parents=True, exist_ok=True)
    try:
        os.chmod(path.parent, 0o700)
    except OSError:
        pass
    tmp = path.with_name(path.name + ".tmp")
    fd = os.open(str(tmp), os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    with os.fdopen(fd, "w", encoding="utf-8") as fp:
        fp.write(text)
    os.replace(tmp, path)


def _append_private(path, line):
    path.parent.mkdir(parents=True, exist_ok=True)
    fd = os.open(str(path), os.O_WRONLY | os.O_CREAT | os.O_APPEND, 0o600)
    with os.fdopen(fd, "a", encoding="utf-8") as fp:
        fp.write(line + "\n")


def _read(path, default=""):
    try:
        return path.read_text(encoding="utf-8")
    except OSError:
        return default


def _bigrams(s):
    s = re.sub(r"\s+", "", s or "")
    return {s[i:i + 2] for i in range(len(s) - 1)}


_STAMP_RES = [
    re.compile(r"\d{4}\s*[-/.年]\s*\d{1,2}\s*[-/.月]\s*\d{1,2}\s*[日号]?"),
    re.compile(r"\d{1,2}\s*月\s*\d{1,2}\s*[日号]"),
    re.compile(r"\b\d{1,2}[-/]\d{1,2}\b(\s*\d{1,2}:\d{2}(\s*[–\-~～]\s*\d{1,2}:\d{2})?)?"),
    re.compile(r"\b\d{1,2}:\d{2}(\s*[–\-~～]\s*\d{1,2}:\d{2})?\b"),
]


def strip_stamps(text):
    """去日期/时间戳（与 C++ PetSpeech.h 一致）：记忆进提示词前、台词输出后都过一遍——
    否则 LLM 会把"10-10"照抄进台词，甚至当成主人的名字"""
    s = str(text)
    for r in _STAMP_RES:
        s = r.sub("", s)
    s = re.sub(r"[，、,]\s*([，、,。！？])", r"\1", s)
    s = re.sub(r"：\s*[，、,\s]\s*", "：", s)
    s = re.sub(r"(^|\n)\s*(-\s+)?[，、,\s]+", r"\1\2", s)
    return re.sub(r"[ \t]{2,}", " ", s).strip(" \t")


def touch_verb(zone, rub):
    """触摸动词（按部位；与 C++ PetVerbs.h 一致）：点击 手=牵 脸=捏 其余=戳；拖动 头/手=摸 其余=揉"""
    hand, head, face = str(zone).startswith("hand"), zone == "head", zone == "face"
    if rub:
        return "摸" if (head or hand) else "揉"
    return "牵" if hand else ("捏" if face else "戳")


class PetMemory:
    """一个角色的记忆视图（owner.md 是共享的）。所有方法失败都不抛到调用方——记忆是增强，不是依赖。"""

    def __init__(self, char_id, char_name):
        self.char_id = _safe_name(char_id)
        self.name = char_name or char_id
        self.dir = ROOT / "chars"
        self.owner_path = ROOT / "owner.md"
        self.char_path = self.dir / f"{self.char_id}.md"
        self.habits_path = self.dir / f"{self.char_id}.habits.json"
        self.pending_path = self.dir / f"{self.char_id}.pending.jsonl"
        self.episodes_path = self.dir / f"{self.char_id}.episodes.jsonl"
        self._legacy_mtime = 0.0
        self._migrate_legacy()
        self.habits = self._load_habits()
        self._habits_dirty = False
        self._habits_saved_at = 0.0
        # 本次会话：距上次见面多久（秒；首次见面 = -1）
        last = self.habits.get("last_seen", 0)
        if not last:
            # 没有统计（老版本迁移过来 / 统计文件丢了）：有记忆就按记忆文件的时间算，别把老朋友当初次见面
            if self._legacy_mtime:
                last = self._legacy_mtime
            elif self.char_path.exists() and self.us() != EMPTY_CHAR.strip():
                last = self.char_path.stat().st_mtime
        self.gap = (time.time() - last) if last else -1.0
        if self.gap < 0 or self.gap > 1800:      # 离开 >30 分钟才算"新的一次见面"
            self.habits["sessions"] = self.habits.get("sessions", 0) + 1
        self.habits.setdefault("first_seen", time.strftime("%Y-%m-%d"))
        self.habits["last_seen"] = time.time()
        self.session_touch = {}                  # 本轮（上次归纳以来）的部位计数：zone → [点, 搓]
        self.save_habits(force=True)

    # ------------------------------------------------------------ 读
    def owner(self):
        return _read(self.owner_path, EMPTY_OWNER).strip()[:OWNER_MAX]

    def us(self):
        return _read(self.char_path, EMPTY_CHAR).strip()[:CHAR_MAX]

    def owner_known(self):
        """关于主人还几乎不了解（≤1 条）→ agent 可以找机会用选项问一个小问题"""
        body = [ln for ln in self.owner().splitlines()
                if ln.strip().startswith("-")
                and not re.search(r"还不了解|未知|不清楚|（无）|（看起来）|^-\s*称呼[:：]\s*主人\s*$", ln.strip())]
        # 只数"主人亲自告诉过的"：统计推断（看起来）和默认称呼不算了解
        return len(body)

    def habits_text(self, zone_cn=lambda z: z):
        """习惯统计 → 一两句人话（程序生成，不经 LLM）"""
        h = self.habits
        parts = []
        zt = h.get("zones", {})
        tot = sum(c + r for c, r in zt.values()) or 0
        if tot >= 5:
            top = sorted(zt.items(), key=lambda kv: -(kv[1][0] + kv[1][1]))[:3]
            desc = []
            for z, (c, r) in top:
                verb = touch_verb(z, r >= c)
                desc.append(f"{verb}{zone_cn(z)} {round(100 * (c + r) / tot)}%")
            parts.append("主人最常" + "、".join(desc) + f"（共 {tot} 次）")
        hours = h.get("hours", [0] * 24)
        if sum(hours) >= 8:
            peak = max(range(24), key=lambda i: hours[i] + hours[(i + 1) % 24])
            parts.append(f"常在 {peak}–{(peak + 2) % 24} 点来找我")
        if h.get("sessions"):
            parts.append(f"从 {h.get('first_seen', '?')} 起见过 {h['sessions']} 次面")
        ch = h.get("choices", [])
        if ch:
            parts.append("主人选过：" + "；".join(ch[-4:]))
        return "；".join(parts)

    def context(self, query="", zone_cn=lambda z: z):
        """注入生成上下文的记忆块：主人 + 我们 + 习惯 + 按 query 召回的往事（最多 3 条）"""
        blk = [self.owner(), "## 我和主人之间\n" + self.us().replace("## ", "### ")]
        ht = self.habits_text(zone_cn)
        if ht:
            blk.append("## 习惯（统计）\n" + ht)
        old = self.recall(query)
        if old:
            blk.append("## 可能相关的往事（归档）\n" + "\n".join("- " + e for e in old))
        return "\n\n".join(b for b in blk if b.strip())

    def panel_text(self, zone_cn=lambda z: z):
        """桌宠面板"记忆本"页显示的全文"""
        ht = self.habits_text(zone_cn) or "（还在攒）"
        pend = len(self._pending())
        return (self.owner() + "\n\n## 我和主人之间（" + self.name + "）\n"
                + self.us().replace("## ", "## · ") + "\n\n## 习惯（统计，程序生成）\n" + ht
                + (f"\n\n## 待整理\n还有 {pend} 条新鲜事等着记进去" if pend else ""))

    def recall(self, query, k=3):
        """归档召回：按字二元组重合度挑最相关的旧事（不在工作记忆里的才算）"""
        q = _bigrams(query)
        if len(q) < 2:
            return []
        cur = _bigrams(self.us())
        scored = []
        for ln in _read(self.episodes_path).splitlines()[-2000:]:
            try:
                ep = json.loads(ln)
            except ValueError:
                continue
            t = ep.get("text", "")
            b = _bigrams(t)
            if not b:
                continue
            s = len(q & b) / (len(q) ** 0.5 * len(b) ** 0.5)
            if s > 0.12 and len(b & cur) / len(b) < 0.6:   # 和工作记忆大半重合 = 已经记着了
                scored.append((s, f"{ep.get('d', '')} {t}"))
        scored.sort(key=lambda x: -x[0])
        out, seen = [], set()
        for _, t in scored:
            if t not in seen:
                out.append(t[:80])
                seen.add(t)
            if len(out) >= k:
                break
        return out

    # ------------------------------------------------------------ 写
    def note(self, text, weight=1):
        """攒一条要点（立即落盘）。weight：1 一般 / 2 重要（主人的选择、约定、关系变化）"""
        try:
            _append_private(self.pending_path, json.dumps(
                {"d": time.strftime("%m-%d %H:%M"), "w": int(weight), "text": str(text)[:200]}, ensure_ascii=False))
        except OSError:
            pass

    def touch(self, zone, kind):
        """点/搓只计数（不进 LLM 归纳）"""
        h = self.habits
        zt = h.setdefault("zones", {})
        rec = zt.setdefault(zone, [0, 0])
        rec[0 if kind == "click" else 1] += 1
        hours = h.setdefault("hours", [0] * 24)
        hours[time.localtime().tm_hour] += 1
        s = self.session_touch.setdefault(zone, [0, 0])
        s[0 if kind == "click" else 1] += 1
        self._habits_dirty = True
        self.save_habits()

    def choice(self, question, label):
        """主人在选项里的回答：既是习惯（最近几次），也是重要要点"""
        ch = self.habits.setdefault("choices", [])
        ch.append(f"「{question[:20]}」→{label}" if question else label)
        del ch[:-12]
        self._habits_dirty = True
        self.save_habits(force=True)
        self.note(f"主人回答了我的问题：{question[:40]}→选了「{label}」", weight=2)

    def save_habits(self, force=False):
        if not self._habits_dirty and not force:
            return
        if not force and time.time() - self._habits_saved_at < 20:
            return
        self.habits["last_seen"] = time.time()
        try:
            _write_private(self.habits_path, json.dumps(self.habits, ensure_ascii=False))
            self._habits_dirty = False
            self._habits_saved_at = time.time()
        except OSError:
            pass

    def heartbeat(self):
        """agent 主循环里调：每分钟刷一次"上次见面"时间并存盘（没人碰时也要知道她什么时候还开着）"""
        if time.time() - self._habits_saved_at > 60:
            self.save_habits(force=True)

    def pending_weight(self):
        return sum(p.get("w", 1) for p in self._pending())

    # ------------------------------------------------------------ 归纳（由 agent 提供 LLM 调用）
    def consolidate(self, llm_json, zone_cn=lambda z: z, tidy=False):
        """把待整理要点 + 本轮触摸统计归纳进 owner.md / chars/<角色>.md；要点归档。
        llm_json(prompt) -> dict。返回 True = 有更新。失败抛异常（要点留在 pending 里下次再试）。"""
        pend = self._pending()
        touch = self._session_touch_text(zone_cn)
        if not pend and not touch and not tidy:
            return False
        lines = [f"{p.get('d', '')} {'【重要】' if p.get('w', 1) >= 2 else ''}{p.get('text', '')}" for p in pend[-60:]]
        if touch:
            lines.append(f"{time.strftime('%m-%d')} 本轮身体互动统计：{touch}")
        prompt = CONSOLIDATE_PROMPT.format(
            name=self.name, owner=self.owner(), us=self.us(),   # 每次都从磁盘读：保留主人的手改
            notes="\n".join(lines) or "（没有新事：按下面的规则把现有两份记忆重新整理一遍）",
            habits=self.habits_text(zone_cn) or "（无）")
        obj = llm_json(prompt)
        owner = str(obj.get("owner", "")).strip()
        us = str(obj.get("us", "")).strip()
        if not owner.startswith("## 关于主人"):
            owner = "## 关于主人\n" + owner if owner else self.owner()
        if "## 最近发生" not in us:
            raise ValueError("归纳结果缺少『## 最近发生』段，丢弃这次结果")
        changed = owner != self.owner() or us != self.us()
        _write_private(self.owner_path, owner[:OWNER_MAX] + "\n")
        _write_private(self.char_path, us[:CHAR_MAX] + "\n")
        # 归档 + 清空待整理
        for p in pend:
            _append_private(self.episodes_path, json.dumps(p, ensure_ascii=False))
        if touch:
            _append_private(self.episodes_path, json.dumps(
                {"d": time.strftime("%m-%d %H:%M"), "w": 1, "text": "身体互动：" + touch}, ensure_ascii=False))
        _write_private(self.pending_path, "")
        self.session_touch = {}
        return changed

    def _session_touch_text(self, zone_cn):
        if not self.session_touch:
            return ""
        items = sorted(self.session_touch.items(), key=lambda kv: -(kv[1][0] + kv[1][1]))[:6]
        out = []
        for z, (c, r) in items:
            seg = []
            if r: seg.append(f"摸{r}")
            if c: seg.append(f"戳{c}")
            out.append(zone_cn(z) + "".join(seg))
        return "、".join(out)

    def _pending(self):
        out = []
        for ln in _read(self.pending_path).splitlines():
            try:
                out.append(json.loads(ln))
            except ValueError:
                pass
        return out

    def _load_habits(self):
        try:
            h = json.loads(_read(self.habits_path, "{}") or "{}")
            return h if isinstance(h, dict) else {}
        except ValueError:
            return {}

    def _migrate_legacy(self):
        """旧版单文件 memory.md → owner.md + 当前角色的 chars/<角色>.md（只做一次；原文件改名留底）"""
        if not LEGACY.exists() or self.char_path.exists():
            return
        try:
            text = LEGACY.read_text(encoding="utf-8")
            self._legacy_mtime = LEGACY.stat().st_mtime
            m = re.search(r"## 关于主人\n(.*?)(?=\n## |\Z)", text, re.S)
            owner = "## 关于主人\n" + (m.group(1).strip() if m else "（还不了解）") + "\n"
            rest = re.sub(r"## 关于主人\n.*?(?=\n## |\Z)", "", text, flags=re.S).strip() or EMPTY_CHAR
            if not self.owner_path.exists():
                _write_private(self.owner_path, owner)
            _write_private(self.char_path, rest + "\n")
            os.replace(LEGACY, LEGACY.with_name("memory.md.migrated"))
            os.chmod(LEGACY.with_name("memory.md.migrated"), 0o600)
        except OSError:
            pass


CONSOLIDATE_PROMPT = """你是桌宠"{name}"的"记忆整理器"。主人和她相处时发生的事要整理进两份记忆。

现有《关于主人》（所有角色共享，只放主人本人的稳定信息）：
<owner>
{owner}
</owner>

现有《我和主人之间》（只属于 {name}）：
<us>
{us}
</us>

程序统计的习惯（可信，可据此写"主人好像…"这类推断，但要写成推断语气）：
{habits}

最近的新事（按时间；【重要】= 主人亲自选择/表态、关系变化，务必保留）：
<new>
{notes}
</new>

输出一个 JSON：{{"owner":"<更新后的《关于主人》>","us":"<更新后的《我和主人之间》>"}}
《关于主人》（owner）的规则——宁缺毋滥：
- 以 `## 关于主人` 开头，下面每条一行以 `- ` 开头，只收三类：
  1) 主人亲自表达过的（【重要】里主人选的答案、主人的称呼、主人说过的喜好/讨厌）；
  2) 主人和她的约定；
  3) 习惯统计里稳定的规律，写成"（看起来）……"，最多 2 条（如"（看起来）爱摸我的头""（看起来）晚上常来"）。
- **不要**写一次性的事件（那属于 us）；**不要**揣测主人的内心或动机（如"他好像没注意到""他故意的"）；
  **不要**写"作息：（还不了解）"这类占位行——一条信息都没有时整段只写一行"（还不了解）"。≤400 字。
《我和主人之间》（us）的规则：
- 固定两段 `## 最近发生`（每条一行、以 月-日 开头、最多 10 条）和 `## 更早`（压缩成短句）。
- 记"关系里的事"：生气/被哄好/被吵醒、主人的选择和她怎么接、约定、久别重逢、第一次……
  身体互动**按天合并成一条**（如"10-09 主人摸了很多次头，也爱戳脸，戳胸让我生了回闷气"），不要逐次记录。
- 同一天的同类事合并；超过 10 条把最旧的压进"更早"。≤1400 字。
通用：只写已发生的事实，不编造；拿不准的不写。中文，用她的第一人称（"我"）称呼自己、"主人"称呼对方。
"""

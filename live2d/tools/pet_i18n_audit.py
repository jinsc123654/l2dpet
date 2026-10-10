#!/usr/bin/env python3
"""pet_i18n_audit.py — 静态对账：桌宠**说出口**的中文文案，是否都在 PetI18n 表里。

范围（宁窄勿滥）：只查会进气泡/最近动态/记忆页的句子来源——
  · LIFE_SAY(...) / LifeLine(...) / LifeSay(...) 的参数
  · FeedPush(...) / FeedTouch / InteractCost(..., 理由) / LifeState(..., 原因)
  · 内置兜底数组：kLines / kAngry / kCold / kDef / kDreamLines / kAsk / kWin / kLose / kDraw / kTalk
（printf/snprintf/fprintf/Log 里的日志、动作名表、协议错误串不算；LLM 下发的内容不在表里是正常的）

做法：从 PetI18n.cpp 的 kEn 表取出全部 zh 键；在上面这些来源里取含汉字的字符串字面量比对。

用法：python3 live2d/tools/pet_i18n_audit.py
"""
import re
import sys

SRC = ["live2d/player/src/l2dtest.cpp"]
TABLE = "live2d/player/src/PetI18n.cpp"
RE_HAN = re.compile(r"[\u4e00-\u9fff]")
RE_STR = re.compile(r'"((?:[^"\\]|\\.)*)"')
MARKERS = ("LIFE_SAY(", "LifeLine(", "LifeSay(", "FeedPush(", "FeedTouch(",
           "InteractCost(", "LifeState(")
ARRAYS = ("kLines", "kAngry", "kCold", "kDef", "kDreamLines", "kAsk", "kWin",
          "kLose", "kDraw", "kTalk", "kLinesPrompt")
LOGS = ("printf(", "snprintf(", "fprintf(", "Log(")


def strip_comments(text):
    out = []
    i = 0
    n = len(text)
    while i < n:
        c = text[i]
        if c == '"':
            j = i + 1
            while j < n:
                if text[j] == "\\":
                    j += 2
                    continue
                if text[j] == '"':
                    break
                j += 1
            out.append(text[i:j + 1])
            i = j + 1
            continue
        if c == "/" and i + 1 < n and text[i + 1] == "/":
            j = text.find("\n", i)
            i = n if j < 0 else j
            continue
        if c == "/" and i + 1 < n and text[i + 1] == "*":
            j = text.find("*/", i + 2)
            i = n if j < 0 else j + 2
            continue
        out.append(c)
        i += 1
    return "".join(out)


def table_pairs(name):
    """取某张表（kEn/kJa）的全部 { "zh", "译文" } 对（容忍一条目跨两行、键里带 {}、行内注释）"""
    text = open(TABLE, encoding="utf-8").read()
    start = text.index(name + "[] = {")
    end = text.index("};", start)
    block = text[start:end]
    # 把字符串挖出来换成占位符（键里可能有 {}，不能直接按花括号切）
    spans = [(m.start(), m.end(), m.group(1)) for m in RE_STR.finditer(block)]
    out = []
    last = 0
    for i, (s, e, _v) in enumerate(spans):
        out.append(block[last:s])
        out.append('"@%d"' % i)
        last = e
    out.append(block[last:])
    masked = re.sub(r"//[^\n]*", "", "".join(out))
    pairs = {}
    for group in re.findall(r"\{([^{}]*)\}", masked):
        idxs = [int(x) for x in re.findall(r"@(\d+)", group)]
        if len(idxs) >= 2:
            pairs[spans[idxs[0]][2]] = spans[idxs[1]][2]
    return pairs


def table_keys():
    return set(table_pairs("kEn").keys())


def main():
    en = table_pairs("kEn")
    ja = table_pairs("kJa")
    keys = set(en.keys())
    print("表里共 %d 条 zh 键（en %d / ja %d）" % (len(keys), len(en), len(ja)))
    only_en = sorted(set(en) - set(ja))
    only_ja = sorted(set(ja) - set(en))
    if only_en:
        print("!! 只有 en 有：%s" % "、".join(only_en))
    if only_ja:
        print("!! 只有 ja 有：%s" % "、".join(only_ja))
    holes = [k for k in keys if en[k].count("{}") != ja[k].count("{}")]
    if holes:
        print("!! 占位符数量对不上：")
        for k in holes:
            print("   %s  en=%d 个 ja=%d 个" % (k, en[k].count("{}"), ja[k].count("{}")))
    if not (only_en or only_ja or holes):
        print("两表键完全对齐、占位符数量一致")
    bad = 0
    for path in SRC:
        raw = open(path, encoding="utf-8").read()
        lines = raw.splitlines()
        clean = strip_comments(raw).splitlines()
        cur_array = ""
        open_braces = 0
        prev_marker = False
        for idx, cline in enumerate(clean):
            rline = lines[idx] if idx < len(lines) else ""
            # 记录当前正在定义的数组（粗略：出现 kXXX[...] = { 后，直到花括号配平）
            m = re.search(r"\b(k[A-Za-z]+)\b[^;=]*=\s*\{", cline)
            if m and m.group(1) in ARRAYS:
                cur_array = m.group(1)
                open_braces = cline.count("{") - cline.count("}")
            elif cur_array:
                open_braces += cline.count("{") - cline.count("}")
                if open_braces <= 0:
                    cur_array = ""
            in_scope = cur_array != "" or prev_marker or any(k in cline for k in MARKERS)
            prev_marker = any(k in cline for k in MARKERS) and ";" not in cline
            if not in_scope:
                continue
            for sm in RE_STR.finditer(cline):
                s = sm.group(1)
                if not RE_HAN.search(s) or s in keys:
                    continue
                head = cline[: sm.start()]
                if any(k in head for k in LOGS) or "i18n-skip" in rline:
                    continue
                print("  %5d  %s" % (idx + 1, s))
                bad += 1
        print("\n-- %s：%d 条待补 --" % (path, bad))
    if bad == 0:
        print("== 全命中 ==")
        return 0
    return 1


if __name__ == "__main__":
    sys.exit(main())

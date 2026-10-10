#!/usr/bin/env python3
"""i18n_check.py — 面板中英对照表的对账脚本（只读）

用法：python3 live2d/tools/i18n_check.py [--missing-only]

做什么：
  1. 把 app.cpp 里的字符串字面量全部抽出来（先按 C++ 词法去掉注释），只留含中文的；
  2. 和 i18n.cpp 里 kEn 表的**键**对账：
     - 漏译：app.cpp 里有、表里没有（= 英文界面下会显示中文）
     - 多余：表里有、app.cpp 里再也找不到（改文案后忘了删）
     - 值不一致：同一个中文出现在 i18n.cpp 里两次
  3. 校验 {} 占位符：键里有几个 {}，译文里就该有几个

IGNORE：只给"不是给人看的界面文本"用（stderr 调试输出、当作数据用的字面量如 "】"）。
"""
import re
import sys
import os

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))   # live2d/
APP = os.path.join(ROOT, "panel", "app.cpp")
I18N = os.path.join(ROOT, "panel", "i18n.cpp")

# 不是界面文案：调试输出 / 协议字段 / 当数据用的字面量
IGNORE = {
    "】", "「", "」%s", "（", "）", "，",   # 数据处理的字面量（find/substr 用）
    "(空)",                                # [picker] 调试输出里的占位
    # 文件对话框的过滤器（NUL 分隔的数据串，不是给人看的文案，翻译成别的语言反而要改结构）
    "模型包 (*.zip)\\0*.zip\\0所有文件 (*.*)\\0*.*\\0",
}
IGNORE_PREFIX = ("[picker]", "[ui]", "[fed]", "[cfg]")

CJK = re.compile(r"[\u2e80-\u9fff\u3000-\u303f\uff00-\uffef]")


def strip_comments(src: str) -> str:
    """按 C++ 词法去注释（字符串/字符字面量里的 // 不算注释）"""
    out = []
    i, n = 0, len(src)
    while i < n:
        c = src[i]
        if c == '"' or c == "'":
            q = c
            out.append(c)
            i += 1
            while i < n:
                if src[i] == "\\":
                    out.append(src[i:i + 2])
                    i += 2
                    continue
                out.append(src[i])
                if src[i] == q:
                    i += 1
                    break
                i += 1
            continue
        if src.startswith("//", i):
            j = src.find("\n", i)
            i = n if j < 0 else j
            continue
        if src.startswith("/*", i):
            j = src.find("*/", i + 2)
            i = n if j < 0 else j + 2
            continue
        out.append(c)
        i += 1
    return "".join(out)


def literals(src: str):
    """抽字符串字面量（把相邻的 "a" "b" 拼接合并成一条，和 C++ 一致）"""
    src = strip_comments(src)
    res, i, n = [], 0, len(src)
    while i < n:
        if src[i] != '"':
            i += 1
            continue
        i += 1
        buf = []
        while i < n and src[i] != '"':
            if src[i] == "\\":
                buf.append(src[i:i + 2])
                i += 2
                continue
            buf.append(src[i])
            i += 1
        i += 1
        # 跳过空白继续吃相邻字面量（C++ 的字面量拼接）
        j = i
        while j < n and src[j] in " \t\r\n":
            j += 1
        while j < n and src[j] == '"':
            j += 1
            while j < n and src[j] != '"':
                if src[j] == "\\":
                    buf.append(src[j:j + 2])
                    j += 2
                    continue
                buf.append(src[j])
                j += 1
            j += 1
            i = j
            while j < n and src[j] in " \t\r\n":
                j += 1
        res.append("".join(buf))
    return res


def table(src: str, array: str = "kEn"):
    """解析 i18n.cpp 的某张表（kEn[] / kJa[]）：键/值都允许相邻字面量拼接（和 C++ 一致）"""
    marker = "const std::pair<const char*, const char*> %s[]" % array
    i = src.index(marker)
    body = strip_comments(src[i:src.index("};", i)])

    def read_str(k):
        while k < len(body) and body[k] != '"':
            k += 1
        buf = []
        while True:
            k += 1                                     # 跳过开引号
            while k < len(body) and body[k] != '"':
                if body[k] == "\\":
                    buf.append(body[k:k + 2])
                    k += 2
                    continue
                buf.append(body[k])
                k += 1
            k += 1                                     # 跳过闭引号
            m = k
            while m < len(body) and body[m] in " \t\r\n":
                m += 1
            if m < len(body) and body[m] == '"':
                k = m
                continue
            return "".join(buf), k

    out, k = [], 0
    while k < len(body):
        if body[k] == "{":
            key, k = read_str(k + 1)
            val, k = read_str(k)
            out.append((key, val))
            continue
        k += 1
    return out


def unesc(s: str) -> str:
    return s.replace('\\"', '"').replace("\\n", "\n").replace("\\\\", "\\")


def main():
    args = set(sys.argv[1:])
    app_src = open(APP, encoding="utf-8").read()
    i18n_src = open(I18N, encoding="utf-8").read()

    used, order = set(), []
    for lit in literals(app_src):
        if not CJK.search(lit):
            continue
        t = unesc(lit)
        if t in IGNORE or t.startswith(IGNORE_PREFIX):
            continue
        # 只当数据、不需要译文的（表里留空值也行）——单独看
        if t not in used:
            order.append(t)
        used.add(t)

    tbl = table(i18n_src, "kEn")
    tbl_ja = table(i18n_src, "kJa")
    kmap, dups, data_keys = {}, [], set()
    for k, v in tbl:
        k = unesc(k)
        if k in kmap:
            dups.append(k)
        kmap[k] = unesc(v)
    # 行尾带 // data 的键 = 桌宠上报的数据类中文，app.cpp 里没有源码字面量
    for line in i18n_src.split("\n"):
        if "// data" in line:
            m = re.search(r'\{\s*"((?:[^"\\]|\\.)*)"', line)
            if m:
                data_keys.add(unesc(m.group(1)))

    missing = [t for t in order if t not in kmap]
    extra = [k for k in kmap if k not in used and k not in data_keys]

    bad_ph = []
    for k, v in kmap.items():
        if k.count("{}") != v.count("{}"):
            bad_ph.append((k, v))

    ja = {unesc(k): unesc(v) for k, v in tbl_ja}
    ja_missing = [k for k in kmap if k not in ja]
    ja_extra = [k for k in ja if k not in kmap]
    ja_bad_ph = [(k, v) for k, v in ja.items() if k.count("{}") != v.count("{}")]
    print(f"app.cpp 中文字面量：{len(order)} 条；i18n 表：{len(kmap)} 条（en）+ {len(ja)} 条（ja）")
    if ja_missing:
        print(f"\n== 日语表缺键（{len(ja_missing)}）==")
        for t in ja_missing:
            print(f"  {t!r}")
    if ja_extra:
        print(f"\n== 日语表多出来的键（{len(ja_extra)}）==")
        for t in ja_extra:
            print(f"  {t!r}")
    if ja_bad_ph:
        print(f"\n== 日语表占位符数量对不上（{len(ja_bad_ph)}）==")
        for k, v in ja_bad_ph:
            print(f"  {k!r} vs {v!r}")
    if missing:
        print(f"\n== 漏译（{len(missing)}）==")
        for t in missing:
            print(f"  {t!r}")
    if extra:
        print(f"\n== 表里多余（{len(extra)}）==")
        for t in extra:
            print(f"  {t!r}")
    if dups:
        print(f"\n== 表里重复的键（{len(dups)}）==")
        for t in sorted(set(dups)):
            print(f"  {t!r}")
    if bad_ph:
        print(f"\n== 占位符数量对不上（{len(bad_ph)}）==")
        for k, v in bad_ph:
            print(f"  {k!r} ({k.count('{}')}) vs {v!r} ({v.count('{}')})")
    if not (missing or extra or dups or bad_ph or ja_missing or ja_extra or ja_bad_ph):
        print("全命中 ✓")
        return 0
    return 1


if __name__ == "__main__":
    sys.exit(main())

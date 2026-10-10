#!/usr/bin/env python3
# capability_matrix.py — 由探测日志生成"能力矩阵"表（live2d/img/capability-matrix.txt 的数据段）
#
# 输入：显式列出的探测日志（gen-capabilities.sh 产出的 <模型>.direct.log / <模型>.phys.log，
#       内为播放器 PROBE/PROBEPHYS 的 P 行）。用法（文件由 shell 展开，脚本自己不扫描目录）：
#   python3 capability_matrix.py <日志目录>/*.direct.log <...>/*.phys.log
#
# 码表（eff=，与各模型 l2d.capability.json 同源）：
#   d=顶点直连（轨道可驱动）  p=仅经物理链  a=换美术（不透明度）  -=无效果  ?=未探测
# 表内记号：直 / 物 / 美术 / 直+美术 / 物+美术 / 无 / ×（无此参数）
import re
import sys

PARAMS = [
    "ParamAngleX", "ParamAngleY", "ParamAngleZ",
    "ParamBodyAngleX", "ParamBodyAngleY", "ParamBodyAngleZ",
    "ParamEyeBallX", "ParamEyeBallY",
    "ParamEyeLOpen", "ParamEyeROpen",
    "ParamBrowLY", "ParamBrowRY", "ParamBrowLForm", "ParamBrowRForm",
    "ParamMouthOpenY", "ParamMouthForm", "ParamMouthX",
    "ParamBreath",
]

FAMILY = ["arm", "Arm", "hand", "Hand", "wrist", "Wrist",
          "Leg", "leg", "Foot", "foot", "Knee", "knee"]

P_RE = re.compile(r"^P (\S+) exists=(\d+)(?: .*? eff=(\S+))?$")


def check_input(path):
    """只接受普通文件（来自命令行显式给出；不跟随符号链接目录遍历）"""
    if ".." in path.split("/"):
        sys.exit(f"路径不合法：{path}")
    with open(path, "rb") as probe:
        if probe.read(0) is None:      # 触发一次打开错误即报错
            pass


def parse_log(path):
    """→ {param: {'exists': bool, 'code': str}}"""
    check_input(path)
    out = {}
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        for line in f:
            m = P_RE.match(line.strip())
            if not m:
                continue
            name, exists, code = m.group(1), m.group(2), m.group(3) or "?"
            out[name] = {"exists": exists == "1", "code": code}
    return out


def mark(param, direct, phys):
    """两档合并 → 表内记号（注意：日志须来自 ALL 全量档——两档都没有该参数 = 模型没这个参数）"""
    d = direct.get(param)
    p = phys.get(param)
    if d is None and p is None:
        return "×"
    if not (d and d["exists"]) and not (p and p["exists"]):
        return "×"
    code = (p or d)["code"]          # phys 日志是合并后的最终码
    if code == "-":
        return "无"
    if code == "?":
        return "?"
    letters = set(code)
    base = None
    if "d" in letters and "p" in letters:
        base = "双"                   # 旧口径（现在的 p 只在无直连时才出现，基本见不到）
    elif "d" in letters:
        base = "直"
    elif "p" in letters:
        base = "物"
    if "a" in letters:
        return (base + "+美术") if base else "美术"
    return base or "?"


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    out_file = None
    for a in sys.argv[1:]:
        if a.startswith("--out="):
            out_file = a.split("=", 1)[1]
    if not args:
        print(__doc__)
        return 2

    pairs = {}
    for raw in args:
        for mode in ("direct", "phys"):
            suffix = f".{mode}.log"
            if raw.endswith(suffix):
                name = raw[:-len(suffix)].split("/")[-1]
                pairs.setdefault(name, {})[mode] = raw
    models = []
    for name in sorted(pairs):
        m = pairs[name]
        if "direct" in m and "phys" in m:
            models.append((name, parse_log(m["direct"]), parse_log(m["phys"])))
    if not models:
        print("没凑齐 <模型>.direct.log + <模型>.phys.log 配对")
        return 2

    lines = ["=== live2d 能力矩阵（2026-10-08 探测重生成；eff 码：d 直连 / p 仅经物理 / a 换美术 / - 无 / ? 未测）==="]
    lines.append("参数".ljust(16) + "".join(m[0].ljust(11) for m in models).rstrip())
    for param in PARAMS:
        row = param.ljust(16)
        for _, direct, phys in models:
            row += mark(param, direct, phys).ljust(11)
        lines.append(row.rstrip())

    lines.append("")
    lines.append("=== 家族通用性（分母=该模型实际存在的参数数；旧版分母被'幻影参数'虚增过，本版已修）===")
    for label, keys in (("头(角度)", ["ParamAngleX", "ParamAngleY", "ParamAngleZ"]),
                        ("身(角度)", ["ParamBodyAngleX", "ParamBodyAngleY", "ParamBodyAngleZ"]),
                        ("眼(注视/开闭)", ["ParamEyeBallX", "ParamEyeBallY", "ParamEyeLOpen", "ParamEyeROpen"]),
                        ("眉", ["ParamBrowLY", "ParamBrowRY", "ParamBrowLForm", "ParamBrowRForm"]),
                        ("口", ["ParamMouthOpenY", "ParamMouthForm", "ParamMouthX"]),
                        ("呼吸", ["ParamBreath"])):
        present = good = 0
        detail = []
        for name, direct, phys in models:
            n = d = 0
            for k in keys:
                mk = mark(k, direct, phys)
                if mk in ("×", "?"):
                    continue
                n += 1
                if mk != "无":
                    d += 1
            present += n
            good += d
            detail.append(f"{name}:{d}/{n}")
        lines.append(f"{label.ljust(10)} 有效 {good}/{present}  | " + " ".join(detail))

    fam_present = fam_good = 0
    fam_detail = []
    for name, direct, phys in models:
        names = set(direct) | set(phys)
        sel = [p for p in names if any(k in p for k in FAMILY)]
        n = d = 0
        for p in sel:
            mk = mark(p, direct, phys)
            if mk in ("×", "?"):
                continue
            n += 1
            if mk != "无":
                d += 1
        fam_present += n
        fam_good += d
        fam_detail.append(f"{name}:{d}/{n}")
    lines.append(f"{'手/臂/腿族'.ljust(10)} 有效 {fam_good}/{fam_present}  | " + " ".join(fam_detail))

    text = "\n".join(lines) + "\n"
    sys.stdout.write(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())

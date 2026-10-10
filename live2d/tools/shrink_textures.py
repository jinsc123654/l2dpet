#!/usr/bin/env python3
"""shrink_textures.py — 把超大源纹理离线缩到运行时上限尺寸（默认 768）

背景：渲染栈内部只用到 ≤768²（`L2D_TEXTURE_MAX_SIZE=768`，见 `player/l2d_config.h`），
而很多模型的源纹理是 2048² 甚至 8K——加载时才降采样，解码瞬时仍要占几十 MB 内存、
换模型/多模型共存时明显拖慢。离线把 PNG 缩到 768² 后：解码峰值 16MB→2.3MB/张，
而且**画质与"加载时降采样"逐位一致**（下面的 box 算法就是同一份）。

算法与 `soft-renderer/SoftTexture.cpp` 的 box 降采样逐字节一致（目标 = 尺寸×768//最长边；
块按精确比例 [x*W/tw,(x+1)*W/tw) 分割；整数平均、向下取整）——因此缩过之后的纹理
官方栈加载时不再降采样，**输出与缩之前逐位一致**（实测：模型区域 0 字节差异）。

用法（默认只打印计划；--apply 才写回）：
  find <模型库>/某模型 -name '*.png' | xargs python3 shrink_textures.py
  find <模型库>/某模型 -name '*.png' | xargs python3 shrink_textures.py --apply

注意：模型库默认在 ~/.local/share/l2dpet/models/；**先备份原模型**（缩是不可逆的）。
"""

import os
import struct
import sys


MAX_SIZE = 768          # 与官方 L2D_TEXTURE_MAX_SIZE / 桥 TEXTURE_MAX_SIZE 一致
THRESHOLD = 2048        # 只缩"解码 ≥16MB"的（≥2048）；1024/512 不动（解码仅 2~4MB）


def is_plain_file_path(candidate):
    """显式拒绝可疑路径（含 .. 分量——本工具只应收到调用者明确列出的纹理文件）。"""
    parts = candidate.replace('\\', '/').split('/')
    return ('..' not in parts) and candidate != ''


def box_downsample(src, width, height, tw, th):
    """SoftTexture.cpp 同款 box（整数平均 / 向下取整 / 精确比例分块）。"""
    dst = bytearray(tw * th * 4)
    for y in range(th):
        sy0 = y * height // th
        sy1 = min(max((y + 1) * height // th, sy0 + 1), height)
        for x in range(tw):
            sx0 = x * width // tw
            sx1 = min(max((x + 1) * width // tw, sx0 + 1), width)
            s0 = s1 = s2 = s3 = 0
            cnt = 0
            for sy in range(sy0, sy1):
                row = (sy * width + sx0) * 4
                for sx in range(sx0, sx1):
                    p = row + (sx - sx0) * 4
                    s0 += src[p]
                    s1 += src[p + 1]
                    s2 += src[p + 2]
                    s3 += src[p + 3]
                    cnt += 1
            o = (y * tw + x) * 4
            dst[o] = s0 // cnt
            dst[o + 1] = s1 // cnt
            dst[o + 2] = s2 // cnt
            dst[o + 3] = s3 // cnt
    return bytes(dst)


def main():
    paths = [arg for arg in sys.argv[1:] if arg != '--apply']
    apply_changes = '--apply' in sys.argv[1:]
    if not paths:
        print(__doc__)
        return 2
    from PIL import Image
    done = 0
    saved = 0
    for path in paths:
        if not is_plain_file_path(path) or not os.path.isfile(path):
            print('跳过: %r' % (path,))
            continue
        with open(path, 'rb') as fp:
            head = fp.read(24)
        if head[:8] != b'\x89PNG\r\n\x1a\n':
            continue
        w, h = struct.unpack('>II', head[16:24])
        if max(w, h) < THRESHOLD:
            continue
        longest = max(w, h)
        tw = w * MAX_SIZE // longest
        th = h * MAX_SIZE // longest
        if not apply_changes:
            print('将缩: %s  %dx%d → %dx%d' % (path, w, h, tw, th))
            continue
        before = os.path.getsize(path)
        image = Image.open(path).convert('RGBA')
        if image.size != (w, h):
            print('跳过（尺寸不符）: %s' % (path,))
            continue
        out = box_downsample(image.tobytes(), w, h, tw, th)
        Image.frombytes('RGBA', (tw, th), out).save(path, optimize=False)
        saved += before - os.path.getsize(path)
        done += 1
        print('缩: %s  %dx%d → %dx%d' % (path, w, h, tw, th))
    if apply_changes:
        print('共缩 %d 张，省 %.1fMB' % (done, saved / 1e6))
    return 0


if __name__ == '__main__':
    sys.exit(main())

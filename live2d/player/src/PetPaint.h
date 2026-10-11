/**
 * PetPaint.h — 桌宠特效/气泡的共享像素绘制（预乘 ARGB；无平台依赖、header-only）
 *
 * 从 PortX11Pet 抽出，供 PortX11Pet / PortGlfwPet 共用（X11 端口暂保留自副本，后续迁移）。
 * 缓冲格式：0xAARRGGBB，预乘 alpha（与渲染器/合成器约定一致）。
 */
#pragma once

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

namespace petpaint
{

typedef std::function<int(const std::string&)> TextWidthFn;   // UTF-8 → 像素宽

// ---------------------------------------------------------------- 特效

struct Fx
{
    int kind;                    // 0=扩散圆环 1=飞散星星 2=滑动闪光
    float x, y, vx, vy, age, life, r0, r1;
};

inline unsigned char Clamp8(int v) { return (unsigned char)(v < 0 ? 0 : (v > 255 ? 255 : v)); }

/** 预乘 over：dst = src*a + dst*(1-a) */
inline void FxOver(uint32_t* p, int r, int g, int b, float a)
{
    if (a <= 0.0f) return;
    if (a > 1.0f) a = 1.0f;
    const uint32_t d = *p;
    const float da = (float)((d >> 24) & 0xFF) / 255.0f;
    const float ia = 1.0f - a;
    const int R = (int)((float)r * a + (float)((d >> 16) & 0xFF) * ia);
    const int G = (int)((float)g * a + (float)((d >> 8) & 0xFF) * ia);
    const int B = (int)((float)b * a + (float)(d & 0xFF) * ia);
    const int A = (int)(255.0f * (a + da * ia));
    *p = ((uint32_t)Clamp8(A) << 24) | ((uint32_t)Clamp8(R) << 16) | ((uint32_t)Clamp8(G) << 8) | (uint32_t)Clamp8(B);
}

inline void FxDisc(uint32_t* buf, int W, int H, float cx, float cy, float r, int cr, int cg, int cb, float a)
{
    const int x0 = (int)(cx - r - 1), x1 = (int)(cx + r + 1);
    const int y0 = (int)(cy - r - 1), y1 = (int)(cy + r + 1);
    for (int y = std::max(0, y0); y <= std::min(H - 1, y1); ++y)
    {
        for (int x = std::max(0, x0); x <= std::min(W - 1, x1); ++x)
        {
            const float dx = (float)x + 0.5f - cx, dy = (float)y + 0.5f - cy;
            const float d = sqrtf(dx * dx + dy * dy);
            if (d < r + 1.0f)
            {
                FxOver(&buf[(size_t)y * W + x], cr, cg, cb, a * (d < r - 1.0f ? 1.0f : (r + 1.0f - d) * 0.5f));
            }
        }
    }
}

inline void FxRingPix(uint32_t* buf, int W, int H, float cx, float cy, float r, float w, int cr, int cg, int cb, float a)
{
    const int x0 = (int)(cx - r - w - 1), x1 = (int)(cx + r + w + 1);
    const int y0 = (int)(cy - r - w - 1), y1 = (int)(cy + r + w + 1);
    for (int y = std::max(0, y0); y <= std::min(H - 1, y1); ++y)
    {
        for (int x = std::max(0, x0); x <= std::min(W - 1, x1); ++x)
        {
            const float dx = (float)x + 0.5f - cx, dy = (float)y + 0.5f - cy;
            const float d = sqrtf(dx * dx + dy * dy) - r;
            if (fabsf(d) < w) FxOver(&buf[(size_t)y * W + x], cr, cg, cb, a * (1.0f - fabsf(d) / w));
        }
    }
}

/** 一颗四芒星：亮心 + 四条短光线（光线用渐小的圆点串成） */
inline void FxStar(uint32_t* buf, int W, int H, float x, float y, float s, float a)
{
    FxDisc(buf, W, H, x, y, s * 0.9f, 255, 252, 240, a);
    FxDisc(buf, W, H, x, y, s * 0.45f, 255, 255, 255, a);
    for (int k = 0; k < 4; ++k)
    {
        const float ang = 1.5707964f * (float)k + 0.35f;
        for (int j = 1; j <= 3; ++j)
        {
            const float rr = s * (0.9f + 1.15f * (float)j);
            FxDisc(buf, W, H, x + cosf(ang) * rr, y + sinf(ang) * rr, s * (0.42f - 0.1f * (float)j), 255, 236, 170,
                   a * (1.0f - 0.22f * (float)j));
        }
    }
}

/** 点击爆发：扩散圆环×2 + 7 颗四芒星飞散 */
inline void Burst(std::vector<Fx>* fxv, float x, float y, float s)
{
    fxv->push_back(Fx{ 0, x, y, 0, 0, 0.0f, 0.40f, 8 * s, 34 * s });
    fxv->push_back(Fx{ 0, x, y, 0, 0, -0.09f, 0.32f, 5 * s, 20 * s });
    for (int i = 0; i < 7; ++i)
    {
        const float ang = (float)rand() / (float)RAND_MAX * 6.28318f;
        const float sp = (35.0f + (float)rand() / (float)RAND_MAX * 75.0f) * s;
        fxv->push_back(Fx{ 1, x, y, cosf(ang) * sp, sinf(ang) * sp - 26.0f * s, 0.0f,
                           0.45f + (float)rand() / (float)RAND_MAX * 0.2f,
                           1.4f * s + 1.6f * s * ((float)rand() / (float)RAND_MAX), 0 });
    }
    if (fxv->size() > 96) fxv->erase(fxv->begin(), fxv->begin() + (int)(fxv->size() - 96));
}

/** 滑动闪光 */
inline void Trail(std::vector<Fx>* fxv, float x, float y, float s)
{
    fxv->push_back(Fx{ 2, x + ((float)(rand() % 100) / 100.0f - 0.5f) * 6 * s,
                       y + ((float)(rand() % 100) / 100.0f - 0.5f) * 6 * s,
                       (float)(rand() % 40 - 20) * 0.6f * s, -14.0f * s, 0.0f,
                       0.28f + (float)(rand() % 100) / 100.0f * 0.14f, 2.4f * s, 0.8f * s });
    if (fxv->size() > 96) fxv->erase(fxv->begin(), fxv->begin() + (int)(fxv->size() - 96));
}

/** 推进并叠画到 dst（调用方负责 dst 是 src 的拷贝）；W/H = 当前窗口尺寸 */
inline void Advance(std::vector<Fx>* fxv, uint32_t* dst, int W, int H, float dt, float uiScale)
{
    size_t w = 0;
    for (size_t i = 0; i < fxv->size(); ++i)
    {
        Fx& f = (*fxv)[i];
        f.age += dt;
        if (f.age < 0.0f)
        {
            (*fxv)[w++] = f;   // 还没出生（延迟环）
            continue;
        }
        const float t = f.age / f.life;
        if (t >= 1.0f) continue;   // 死了
        f.x += f.vx * dt;
        f.y += f.vy * dt;
        f.vx *= (1.0f - 2.4f * dt);
        f.vy *= (1.0f - 2.4f * dt);
        if (f.kind == 0)
        {
            const float r = f.r0 + (f.r1 - f.r0) * t;
            FxRingPix(dst, W, H, f.x, f.y, r, 2.6f * uiScale, 255, 205, 130, 0.85f * (1.0f - t) * (1.0f - t));
        }
        else if (f.kind == 1)
        {
            FxStar(dst, W, H, f.x, f.y, f.r0, (1.0f - t));
        }
        else
        {
            FxDisc(dst, W, H, f.x, f.y, f.r0 * (1.0f - t * 0.5f), 255, 240, 190, 0.75f * (1.0f - t));
            FxDisc(dst, W, H, f.x, f.y, f.r0 * 0.4f, 255, 255, 255, 0.8f * (1.0f - t));
        }
        (*fxv)[w++] = f;
    }
    fxv->resize(w);
}

// ---------------------------------------------------------------- 气泡（SDF 鼓泡）

inline float SdRoundBox(float px, float py, float x, float y, float w, float h, float r)
{
    const float cx = x + w * 0.5f, cy = y + h * 0.5f;
    const float qx = fabsf(px - cx) - (w * 0.5f - r), qy = fabsf(py - cy) - (h * 0.5f - r);
    const float ox = qx > 0 ? qx : 0, oy = qy > 0 ? qy : 0;
    return sqrtf(ox * ox + oy * oy) + std::min(std::max(qx, qy), 0.0f) - r;
}

/** 椭圆（近似 SDF）；r 随角度轻微起伏 = 手绘感 */
inline float SdBlob(float px, float py, float cx, float cy, float a, float b)
{
    const float dx = px - cx, dy = py - cy;
    const float th = atan2f(dy * a, dx * b);
    const float w = 1.0f + 0.018f * sinf(2.0f * th + 0.6f) + 0.008f * sinf(3.0f * th + 1.3f);
    const float A = a * w, B = b * w;
    const float k0 = sqrtf((dx * dx) / (A * A) + (dy * dy) / (B * B));
    const float k1 = sqrtf((dx * dx) / (A * A * A * A) + (dy * dy) / (B * B * B * B));
    return k1 > 1e-6f ? k0 * (k0 - 1.0f) / k1 : -std::min(A, B);
}

inline float SdCapsule(float px, float py, float ax, float ay, float bx, float by, float r)
{
    const float pax = px - ax, pay = py - ay, bax = bx - ax, bay = by - ay;
    float h = (pax * bax + pay * bay) / (bax * bax + bay * bay);
    h = h < 0 ? 0 : (h > 1 ? 1 : h);
    const float dx = pax - bax * h, dy = pay - bay * h;
    return sqrtf(dx * dx + dy * dy) - r;
}

inline void Over(float* d, float r, float g, float b, float a)
{
    d[0] = r * a + d[0] * (1 - a);
    d[1] = g * a + d[1] * (1 - a);
    d[2] = b * a + d[2] * (1 - a);
    d[3] = a + d[3] * (1 - a);
}

inline float Cov(float d) { return std::min(std::max(0.5f - d, 0.0f), 1.0f); }

/** 气泡几何（布局单位；× bubScale 得窗口像素） */
struct BubbleGeom
{
    std::vector<std::string> lines;
    std::vector<std::string> opts;
    std::vector<std::pair<int, int> > rectXW;   // 选项胶囊 x/宽（内容块坐标）
    std::vector<int> rectY;                     // 选项胶囊的行顶 y（多行换行时每行不同；X11 端就是 XRectangle.y）
    float cw = 0, ch = 0;                       // 内容块尺寸
    float ea = 0, eb = 0;                       // 椭圆半径
    int W = 0, H = 0;                           // 窗口尺寸
    float ecx = 0, ecy = 0;                     // 椭圆中心
    float d1x = 0, d1y = 0;                     // 大圆点圆心
    float d2x = 0, d2y = 0;                     // 小圆点圆心
    int tipX = 0, tipY = 0;                     // 小圆点末端（= 头顶锚点）
    int contentX = 0, contentY = 0;             // 内容块左上
    int pillH = 0, lineH = 0, gap = 0;
    int fontH = 0;                              // 布局时的字体实测行高（气泡按它长个儿）
};

/** 折行 + 椭圆/圆点几何（textW: UTF-8 → 宽；s = 气泡缩放；fontH = 字体实测行高，0 = 按旧常量估） */
inline void LayoutBubble(BubbleGeom* g, const std::string& text, const char* const* options, int n,
                         const TextWidthFn& textW, float s, int fontH)
{
    g->lines.clear();
    g->opts.clear();
    g->rectXW.clear();
    g->rectY.clear();
    const int maxTextW = (int)(210 * s);
    std::string line;
    int maxW = 0;
    size_t i = 0;
    static const char* const kNoStart[] = { "，", "。", "！", "？", "、", "；", "：", "）", "」", "』", "】",
                                            "》", "…", "—", "～", "”", "’", ",", ".", "!", "?", ";", ":", ")", "]", "~" };
    while (i < text.size())
    {
        if (text[i] == '\n')
        {
            maxW = std::max(maxW, textW(line));
            g->lines.push_back(line);
            line.clear();
            ++i;
            continue;
        }
        const unsigned char c = (unsigned char)text[i];
        int cl = c < 0x80 ? 1 : ((c & 0xE0) == 0xC0 ? 2 : ((c & 0xF0) == 0xE0 ? 3 : 4));
        if (i + (size_t)cl > text.size()) cl = 1;
        const std::string ch = text.substr(i, (size_t)cl);
        if (textW(line + ch) > maxTextW && !line.empty())
        {
            bool noStart = false;
            for (size_t k = 0; k < sizeof(kNoStart) / sizeof(kNoStart[0]); ++k) noStart = noStart || ch == kNoStart[k];
            if (!noStart)
            {
                maxW = std::max(maxW, textW(line));
                g->lines.push_back(line);
                line.clear();
                continue;
            }
        }
        line += ch;
        i += (size_t)cl;
    }
    if (!line.empty())
    {
        maxW = std::max(maxW, textW(line));
        g->lines.push_back(line);
    }
    // 行高/胶囊高按**字体实测高度**推挤（= X11 端 _font->height + 4/12 的同一套算法）。
    // 写死 22/30 那种常量只对"14px 字 + 缩放 1"成立：气泡字号（bubble_px）和缩放比是两回事，
    // 字号调大而缩放小的时候，字比行高高一倍，几行字就叠在一起了。
    g->fontH = fontH > 0 ? fontH : (int)(18 * s);   // 老常量 18 ≈ 14px CJK 字体的行高
    const float lh = (float)g->fontH + 4.0f * s;    // 行高 = 字高 + 4（旧版同：18 + 4 = 22）
    g->lineH = (int)(lh + 0.5f);
    g->pillH = g->fontH + (int)(12 * s);            // 胶囊高 = 字高 + 12（旧版同：18 + 12 = 30）
    g->gap = (int)(8 * s);
    int rowW = 0;
    for (int k = 0; k < n; ++k)
    {
        g->opts.push_back(options[k]);
        const int w = textW(g->opts[(size_t)k]) + (int)(28 * s);
        g->rectXW.push_back(std::make_pair(w, 0));
        g->rectY.push_back(0);
        rowW += w + (k ? g->gap : 0);
    }
    // 选项胶囊：一行放不下按行排（与旧版一致：行内居中）
    g->cw = std::max((float)maxW, (float)std::min(rowW, (int)(260 * s)));
    float y = (float)g->lines.size() * lh - 4.0f * s;
    if (n > 0)
    {
        y += 12.0f * s;
        int x = 0, rowStart = 0;
        for (int k = 0; k <= n; ++k)
        {
            const bool end = (k == n);
            if (!end && ((float)(x + g->rectXW[(size_t)k].first) <= g->cw || k == rowStart))
            {
                g->rectXW[(size_t)k].second = x;
                g->rectY[(size_t)k] = (int)y;   // 本行行顶（换行后每行不同，绘制/命中共用）
                x += g->rectXW[(size_t)k].first + g->gap;
                continue;
            }
            const int shift = ((int)g->cw - (x - g->gap)) / 2;
            for (int j = rowStart; j < k; ++j) g->rectXW[(size_t)j].second += shift;
            if (end)
            {
                y += g->pillH;
                break;
            }
            y += g->pillH + g->gap;
            x = 0;
            rowStart = k;
            --k;
        }
    }
    g->ch = y;
    g->ea = (g->cw * 0.5f + 16.0f * s) * 1.32f;
    g->eb = std::max(std::max((g->ch * 0.5f + 12.0f * s) * 1.32f, g->ea * 0.55f), 44.0f * s);
    const int M = (int)(8 * s), dotsW = (int)(60 * s), dotsH = (int)(56 * s);
    g->W = (int)(2 * g->ea) + 2 * M + dotsW;
    g->H = (int)(2 * g->eb) + 2 * M + dotsH;
    const bool sideLeft = true;   // 由调用方按 _side 填；此处 geometry 用 side=-1（圆点在右下）
    (void)sideLeft;
    g->ecx = M + g->ea;
    g->ecy = M + g->eb;
    const float dir = 1.0f;
    const float th = 0.70f;
    const float bx = g->ecx + dir * g->ea * cosf(th), by = g->ecy + g->eb * sinf(th);
    float nx = dir * cosf(th) / g->ea, ny = sinf(th) / g->eb;
    const float nl = sqrtf(nx * nx + ny * ny);
    nx /= nl;
    ny /= nl;
    const float lw = 3.4f * s, d1r = 11.0f * s, d2r = 6.0f * s;
    g->d1x = bx + nx * (lw + d1r + 7.0f * s);
    g->d1y = by + ny * (lw + d1r + 7.0f * s);
    g->d2x = g->d1x + nx * (d1r + d2r + 9.0f * s);
    g->d2y = g->d1y + ny * (d1r + d2r + 9.0f * s);
    // 小圆点末端（= 调用方的锚点）：不填就只能拿 W/H 去凑，横竖两个方向都会偏
    g->tipX = (int)(g->d2x + dir * d2r);
    g->tipY = (int)(g->d2y + d2r);
    g->contentX = (int)(g->ecx - g->cw * 0.5f);
    g->contentY = (int)(g->ecy - g->ch * 0.5f);
}

/** side=+1 时镜像几何（圆点朝左下）；在 LayoutBubble 之后、渲染之前调用 */
inline void MirrorBubble(BubbleGeom* g, float s)
{
    const float lw = 3.4f * s, d1r = 11.0f * s, d2r = 6.0f * s;
    const int M = (int)(8 * s);
    g->ecx = g->W - M - g->ea;
    const float dir = -1.0f;
    const float th = 0.70f;
    const float bx = g->ecx + dir * g->ea * cosf(th), by = g->ecy + g->eb * sinf(th);
    float nx = dir * cosf(th) / g->ea, ny = sinf(th) / g->eb;
    const float nl = sqrtf(nx * nx + ny * ny);
    nx /= nl;
    ny /= nl;
    g->d1x = bx + nx * (lw + d1r + 7.0f * s);
    g->d1y = by + ny * (lw + d1r + 7.0f * s);
    g->d2x = g->d1x + nx * (d1r + d2r + 9.0f * s);
    g->d2y = g->d1y + ny * (d1r + d2r + 9.0f * s);
    // 镜像后锚点也得跟着翻到另一侧（否则 tipX/tipY 还指着镜像前的小圆点）
    g->tipX = (int)(g->d2x + dir * d2r);
    g->tipY = (int)(g->d2y + d2r);
    g->contentX = (int)(g->ecx - g->cw * 0.5f);
    g->contentY = (int)(g->ecy - g->ch * 0.5f);
}

/** 选项 r 的胶囊行顶 y（内容块坐标）。选项换行后每行的 y 不同——丢了它所有行会叠在一起
 *  （框出来的老 bug：长选项全挤成一个胶囊）。rectY 没填时退回单行公式（= 第一行的 y）。*/
inline int PillTopY(const BubbleGeom& g, size_t r, float s)
{
    if (r < g.rectY.size()) return g.rectY[r];
    const int lh = g.lineH > 0 ? g.lineH : (int)(22.0f * s);
    return (int)g.lines.size() * lh - (int)(4 * s) + (int)(12 * s);
}

/** 画气泡背景（椭圆描边/白底/下弯浅蓝/右上高光/双圆点/选项胶囊）；a = 总不透明度 */
inline void RenderBubbleBg(const BubbleGeom& g, float s, int hover, float a, std::vector<uint32_t>* buf)
{
    const float olR = 27 / 255.f, olG = 78 / 255.f, olB = 96 / 255.f;
    const float lw = 3.4f * s;
    buf->assign((size_t)g.W * g.H, 0u);
    for (int y = 0; y < g.H; ++y)
    {
        for (int x = 0; x < g.W; ++x)
        {
            const float px = (float)x + 0.5f, py = (float)y + 0.5f;
            float c[4] = { 0, 0, 0, 0 };
            const float d = SdBlob(px, py, g.ecx, g.ecy, g.ea, g.eb);
            const float outer = Cov(d - lw * 0.5f);
            if (outer > 0)
            {
                const float inner = Cov(d + lw * 0.5f);
                const float up = SdBlob(px, py, g.ecx, g.ecy - g.eb * 0.30f, g.ea * 0.98f, g.eb * 0.98f);
                const float shade = Cov(-up);
                float fr = 1.0f, fg = 1.0f, fb = 1.0f;
                fr -= shade * (1.0f - 222 / 255.f);
                fg -= shade * (1.0f - 238 / 255.f);
                fb -= shade * (1.0f - 250 / 255.f);
                Over(c, olR, olG, olB, outer);
                Over(c, fr, fg, fb, inner * 0.94f);
            }
            {
                const float hx = g.ecx + g.ea * 0.55f, hy = g.ecy - g.eb * 0.64f;
                const float h1 = SdCapsule(px, py, hx - g.ea * 0.13f, hy - g.eb * 0.05f, hx + g.ea * 0.05f, hy + g.eb * 0.07f, 1.6f * s);
                const float h2 = SdCapsule(px, py, hx + g.ea * 0.12f, hy + g.eb * 0.13f, hx + g.ea * 0.17f, hy + g.eb * 0.22f, 1.6f * s);
                const float hc = std::max(Cov(h1), Cov(h2));
                const float mg = 6.0f * s;
                const bool inContent = px > g.contentX - mg && px < g.contentX + g.cw + mg
                                       && py > g.contentY - mg && py < g.contentY + g.ch + mg;
                if (hc > 0 && d < -lw - 2.0f * s && !inContent) Over(c, olR, olG, olB, hc * 0.9f);
            }
            const float dd[2][3] = { { g.d1x, g.d1y, 11.0f * s }, { g.d2x, g.d2y, 6.0f * s } };
            for (int k = 0; k < 2; ++k)
            {
                const float dq = sqrtf((px - dd[k][0]) * (px - dd[k][0]) + (py - dd[k][1]) * (py - dd[k][1])) - dd[k][2];
                const float o = Cov(dq - 1.6f * s), in = Cov(dq + 1.6f * s);
                if (o > 0)
                {
                    Over(c, olR, olG, olB, o);
                    Over(c, 1.0f, 1.0f, 1.0f, in * 0.94f);
                }
            }
            for (size_t r = 0; r < g.rectXW.size() && r < g.opts.size(); ++r)
            {
                const float qx = (float)(g.contentX + g.rectXW[r].second), qy = (float)(g.contentY + PillTopY(g, r, s));
                const float qw = (float)g.rectXW[r].first, qh = (float)g.pillH;
                if (px < qx - 3 || px > qx + qw + 3 || py < qy - 3 || py > qy + qh + 3) continue;
                const float dq = SdRoundBox(px, py, qx, qy, qw, qh, qh * 0.5f);
                const bool hov = (int)r == hover;
                Over(c, olR, olG, olB, Cov(dq - std::max(1.0f, s)));
                Over(c, hov ? 0.86f : 1.0f, hov ? 0.93f : 1.0f, 1.0f, Cov(dq + std::max(1.0f, s)));
            }
            const unsigned A = (unsigned)(c[3] * a * 255.f + 0.5f), R = (unsigned)(c[0] * a * 255.f + 0.5f),
                           G = (unsigned)(c[1] * a * 255.f + 0.5f), B = (unsigned)(c[2] * a * 255.f + 0.5f);
            (*buf)[(size_t)y * g.W + x] = (A << 24) | (R << 16) | (G << 8) | B;
        }
    }
}

/** 选项胶囊命中（气泡窗口像素坐标）→ 下标 / -1 */
inline int OptionAt(const BubbleGeom& g, double x, double y, float s)
{
    for (size_t r = 0; r < g.rectXW.size() && r < g.opts.size(); ++r)
    {
        const int qx = g.contentX + g.rectXW[r].second;
        const int qy = g.contentY + PillTopY(g, r, s);
        if (x >= qx && x < qx + g.rectXW[r].first && y >= qy && y < qy + g.pillH) return (int)r;
    }
    return -1;
}

}   // namespace petpaint

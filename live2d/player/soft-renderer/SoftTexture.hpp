/**
 * SoftTexture.hpp — 软渲染用的纹理对象（stb_image 解码，RGBA8888）
 *
 * 行序约定：_pixels 第 0 行 = 图像顶行（stb 原生行序）。
 * 采样时按官方 GLES2 顶点着色器的 v_texCoord.y = 1 - y 约定取行：
 *   row = (1 - uv.y) * height
 *
 * 采样函数为内联热点（软光栅化每像素调用），实现放在本头文件。
 */

#pragma once

#include <CubismFramework.hpp>   // csmUint8 / csmFloat32 等基础类型
#include "l2d_config.h"          // L2D_ALPHA_SUMMARY / L2D_ALPHA_BLOCK_SHIFT
#include "SoftCompat.hpp"        // L2D_INLINE_ATTR（GCC 属性 / MSVC 空展开）

namespace Live2D { namespace Cubism { namespace Framework { namespace Rendering {

class SoftTexture
{
public:
    /** 支持 stb_image 能解码的格式（PNG 为主） */
    static SoftTexture* LoadFromFile(const char* path);

    /**
     * 从已解码的 RGBA8888 像素构造（行序：首行 = 图像顶部）。
     * 供"数据由调用方提供"的路径使用（调用方已完成解码与降采样，
     * 这里只做拷贝 + 摘要表，不重复解码）。宽高非法/内存不足返回 NULL。
     */
    static SoftTexture* FromRgba(const csmUint8* rgba, int width, int height);

    ~SoftTexture();

    int Width() const { return _width; }
    int Height() const { return _height; }

    /** RGBA8888，行优先，首行=图像顶部；行间步长 = 4*Width */
    const csmUint8* Pixels() const { return _pixels; }

    /** 4×4 max-alpha 摘要表（整数采样路径直接访问；无表时为 NULL） */
    const csmUint8* AlphaSummary() const { return _alphaSummary; }
    int SummaryWidth() const { return _summaryW; }
    int SummaryHeight() const { return _summaryH; }

    /**
     * 快速透明预判：采样点邻域是否确定全透明（基于 4×4 块的 max-alpha 摘要）。
     * 摘要表仅 ~w*h/16 字节，可常驻 cache；命中即可跳过 4 次主纹理访问。
     * 保守策略：查 3×3 块邻域全 0 才判定透明（双线性采样半径 < 1 块）。
     */
    inline bool IsTransparentRegion(csmFloat32 u, csmFloat32 v) const L2D_INLINE_ATTR
    {
        if (_alphaSummary == NULL)
        {
            return false;
        }
        const int tx = (int)(u * (csmFloat32)_width);
        const int ty = (int)((1.0f - v) * (csmFloat32)_height);
        const int bx = tx >> kBlockShift;
        const int by = ty >> kBlockShift;
        if (bx < 1 || by < 1 || bx >= _summaryW - 1 || by >= _summaryH - 1)
        {
            return false; // 边界附近保守处理
        }
        for (int dy = -1; dy <= 1; ++dy)
        {
            const csmUint8* row = _alphaSummary + (size_t)(by + dy) * _summaryW + (bx - 1);
            if (row[0] | row[1] | row[2])
            {
                return false;
            }
        }
        return true;
    }

    /**
     * 整数版透明预判（与 IsTransparentRegion 同一摘要，但吃整数纹理坐标：
     * 定点路径下 x0/tx 已是整数，省掉每像素 2 次浮点→整数转换）。
     * tx/ty 为纹理像素下标（floor 语义）。
     */
    inline bool IsTransparentRegionI(csmInt32 tx, csmInt32 ty) const L2D_INLINE_ATTR
    {
        if (_alphaSummary == NULL)
        {
            return false;
        }
        const int bx = tx >> kBlockShift;
        const int by = ty >> kBlockShift;
        if (bx < 1 || by < 1 || bx >= _summaryW - 1 || by >= _summaryH - 1)
        {
            return false; // 边界附近保守处理
        }
        const csmUint8* row = _alphaSummary + (size_t)(by - 1) * _summaryW + (bx - 1);
        if (row[0] | row[1] | row[2]) { return false; }
        row += _summaryW;
        if (row[0] | row[1] | row[2]) { return false; }
        row += _summaryW;
        if (row[0] | row[1] | row[2]) { return false; }
        return true;
    }

    /** 最近邻采样（最快路径；越界按 GL_CLAMP_TO_EDGE） */
    inline void SampleNearest(csmFloat32 u, csmFloat32 v, csmUint8* out) const L2D_INLINE_ATTR
    {
        int x = (int)(u * (csmFloat32)_width);
        int y = (int)((1.0f - v) * (csmFloat32)_height);
        if (x < 0) x = 0; else if (x >= _width) x = _width - 1;
        if (y < 0) y = 0; else if (y >= _height) y = _height - 1;
        const csmUint8* p = _pixels + ((size_t)y * (size_t)_width + (size_t)x) * 4;
        out[0] = p[0]; out[1] = p[1]; out[2] = p[2]; out[3] = p[3];
    }

    /** 双线性采样（画质优先；u,v 任意实数，v 按 Cubism/GL 约定：0=底、1=顶） */
    inline void SampleBilinear(csmFloat32 u, csmFloat32 v, csmUint8* out) const L2D_INLINE_ATTR
    {
        const csmFloat32 fx = u * (csmFloat32)_width - 0.5f;
        const csmFloat32 fy = (1.0f - v) * (csmFloat32)_height - 0.5f;

        int x0 = (int)fx;
        int y0 = (int)fy;
        if (fx < 0.0f) x0 -= 1;
        if (fy < 0.0f) y0 -= 1;

        const csmFloat32 tx = fx - (csmFloat32)x0;
        const csmFloat32 ty = fy - (csmFloat32)y0;

        const int x1 = x0 + 1;
        const int y1 = y0 + 1;

        const int cx0 = x0 < 0 ? 0 : (x0 >= _width ? _width - 1 : x0);
        const int cx1 = x1 < 0 ? 0 : (x1 >= _width ? _width - 1 : x1);
        const int cy0 = y0 < 0 ? 0 : (y0 >= _height ? _height - 1 : y0);
        const int cy1 = y1 < 0 ? 0 : (y1 >= _height ? _height - 1 : y1);

        const csmUint8* p00 = _pixels + ((size_t)cy0 * (size_t)_width + (size_t)cx0) * 4;
        const csmUint8* p10 = _pixels + ((size_t)cy0 * (size_t)_width + (size_t)cx1) * 4;
        const csmUint8* p01 = _pixels + ((size_t)cy1 * (size_t)_width + (size_t)cx0) * 4;
        const csmUint8* p11 = _pixels + ((size_t)cy1 * (size_t)_width + (size_t)cx1) * 4;

        const csmFloat32 w00 = (1.0f - tx) * (1.0f - ty);
        const csmFloat32 w10 = tx * (1.0f - ty);
        const csmFloat32 w01 = (1.0f - tx) * ty;
        const csmFloat32 w11 = tx * ty;

        for (int c = 0; c < 4; ++c)
        {
            const csmFloat32 value = p00[c] * w00 + p10[c] * w10 + p01[c] * w01 + p11[c] * w11;
            const csmInt32 iv = (csmInt32)(value + 0.5f);
            out[c] = (csmUint8)(iv < 0 ? 0 : (iv > 255 ? 255 : iv));
        }
    }

private:
    SoftTexture();

    static const int kBlockShift = L2D_ALPHA_BLOCK_SHIFT; // 摘要块尺寸（默认 4×4）

    csmUint8* _pixels;
    int _width;
    int _height;

    csmUint8* _alphaSummary;            // 每 4×4 块的最大 alpha（1 字节/块）
    int _summaryW;
    int _summaryH;
};

}}}}

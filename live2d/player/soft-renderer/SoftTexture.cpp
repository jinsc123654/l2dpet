/**
 * SoftTexture.cpp — stb_image 解码（采样实现见 .hpp 内联版本）
 */

#include "SoftTexture.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
// 贴图来自导入的模型包（不可信）：限制边长，防"解压炸弹"式的超大尺寸 PNG 一次申请几个 GB
// （默认上限 1<<24）。Live2D 贴图最大 8192。
#define STBI_MAX_DIMENSIONS 8192
#include "stb_image.h"

#if defined(_WIN32)
// WIN32_LEAN_AND_MEAN 必须带上：不然 rpcndr.h 会把 small 定义成宏，
// 下面 `csmUint8* small = ...` 就被改写成 `csmUint8* char` 直接编不过。
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <string>
#endif

#if defined(_WIN32)
/** 按 UTF-8 路径读整个文件（内部走 W 版打开）。
 *  为什么要自己读：stbi_load 内部用 fopen —— 窄字符路径按**本地代码页**解释，
 *  模型目录名带中文时（ACP=936 这类）贴图一律打不开，只能读成字节再交给内存解码。*/
static bool SoftReadWholeFileUtf8(const char* path, std::string* out)
{
    const int n = MultiByteToWideChar(CP_UTF8, 0, path, -1, NULL, 0);
    if (n <= 0) return false;
    std::wstring w((size_t)n, 0);
    MultiByteToWideChar(CP_UTF8, 0, path, -1, &w[0], n);
    FILE* f = _wfopen(w.c_str(), L"rb");
    if (f == NULL) return false;
    char buf[65536];
    size_t r = 0;
    while ((r = fread(buf, 1, sizeof(buf), f)) > 0) out->append(buf, r);
    fclose(f);
    return true;
}
#endif

namespace Live2D { namespace Cubism { namespace Framework { namespace Rendering {

SoftTexture::SoftTexture()
    : _pixels(NULL)
    , _width(0)
    , _height(0)
    , _alphaSummary(NULL)
    , _summaryW(0)
    , _summaryH(0)
{
}

SoftTexture::~SoftTexture()
{
    stbi_image_free(_pixels);
    free(_alphaSummary);
}

SoftTexture* SoftTexture::LoadFromFile(const char* path)
{
    int width = 0;
    int height = 0;
    int channels = 0;
#if defined(_WIN32)
    // Windows：自己按 UTF-8 路径读字节，再让 stb 从内存解码（见上面 SoftReadWholeFileUtf8）
    std::string bytes;
    stbi_uc* pixels = SoftReadWholeFileUtf8(path, &bytes)
                        ? stbi_load_from_memory((const stbi_uc*)bytes.data(), (int)bytes.size(),
                                                &width, &height, &channels, STBI_rgb_alpha)
                        : NULL;
#else
    stbi_uc* pixels = stbi_load(path, &width, &height, &channels, STBI_rgb_alpha);
#endif
    if (pixels == NULL)
    {
        fprintf(stderr, "[SoftTexture] 解码失败: %s (%s)\n", path, stbi_failure_reason());
        return NULL;
    }

    // 超限降采样（内存受限平台关键优化：2048² RGBA 一张 16MB；UV 为归一化
    // 坐标，整图缩放不影响映射；屏上显示尺寸远小于 1024 时画质无感）
    if (L2D_TEXTURE_MAX_SIZE > 0 && (width > L2D_TEXTURE_MAX_SIZE || height > L2D_TEXTURE_MAX_SIZE))
    {
        const int longest = width > height ? width : height;
        const int tw = width * L2D_TEXTURE_MAX_SIZE / longest;
        const int th = height * L2D_TEXTURE_MAX_SIZE / longest;
        if (tw > 0 && th > 0)
        {
            csmUint8* small = (csmUint8*)malloc((size_t)tw * (size_t)th * 4);
            if (small != NULL)
            {
                for (int y = 0; y < th; ++y)
                {
                    const int sy0 = (int)((int64_t)y * height / th);
                    int sy1 = (int)((int64_t)(y + 1) * height / th);
                    if (sy1 <= sy0) sy1 = sy0 + 1;
                    for (int x = 0; x < tw; ++x)
                    {
                        const int sx0 = (int)((int64_t)x * width / tw);
                        int sx1 = (int)((int64_t)(x + 1) * width / tw);
                        if (sx1 <= sx0) sx1 = sx0 + 1;
                        int sum0 = 0, sum1 = 0, sum2 = 0, sum3 = 0, cnt = 0;
                        for (int sy = sy0; sy < sy1 && sy < height; ++sy)
                        {
                            const csmUint8* row = pixels + ((size_t)sy * (size_t)width + (size_t)sx0) * 4;
                            for (int sx = sx0; sx < sx1 && sx < width; ++sx, row += 4)
                            {
                                sum0 += row[0];
                                sum1 += row[1];
                                sum2 += row[2];
                                sum3 += row[3];
                                ++cnt;
                            }
                        }
                        csmUint8* dst = small + ((size_t)y * (size_t)tw + (size_t)x) * 4;
                        dst[0] = (csmUint8)(sum0 / cnt);
                        dst[1] = (csmUint8)(sum1 / cnt);
                        dst[2] = (csmUint8)(sum2 / cnt);
                        dst[3] = (csmUint8)(sum3 / cnt);
                    }
                }
                stbi_image_free(pixels);
                pixels = small;
                width = tw;
                height = th;
            }
        }
    }

    SoftTexture* texture = new SoftTexture();
    texture->_pixels = (csmUint8*)pixels; // 析构时 free()（stbi_image_free == free）
    texture->_width = width;
    texture->_height = height;

#if !L2D_ALPHA_SUMMARY
    return texture; // 透明预判摘要表已由 config 关闭（IsTransparentRegion 恒 false）
#endif

    // 生成 4×4 块 max-alpha 摘要（透明预判用）
    const int blockSize = 1 << kBlockShift;
    texture->_summaryW = (width + blockSize - 1) >> kBlockShift;
    texture->_summaryH = (height + blockSize - 1) >> kBlockShift;
    const size_t summaryCount = (size_t)texture->_summaryW * (size_t)texture->_summaryH;
    texture->_alphaSummary = (csmUint8*)malloc(summaryCount);
    if (texture->_alphaSummary != NULL)
    {
        for (int by = 0; by < texture->_summaryH; ++by)
        {
            for (int bx = 0; bx < texture->_summaryW; ++bx)
            {
                csmUint8 maxAlpha = 0;
                const int yEnd = (by << kBlockShift) + blockSize;
                const int xEnd = (bx << kBlockShift) + blockSize;
                for (int y = (by << kBlockShift); y < yEnd && y < height; ++y)
                {
                    const csmUint8* row = pixels + ((size_t)y * (size_t)width) * 4;
                    for (int x = (bx << kBlockShift); x < xEnd && x < width; ++x)
                    {
                        const csmUint8 a = row[(size_t)x * 4 + 3];
                        if (a > maxAlpha)
                        {
                            maxAlpha = a;
                        }
                    }
                }
                texture->_alphaSummary[(size_t)by * texture->_summaryW + bx] = maxAlpha;
            }
        }
    }

    return texture;
}

SoftTexture* SoftTexture::FromRgba(const csmUint8* rgba, int width, int height)
{
    if (rgba == NULL || width <= 0 || height <= 0 || width > 8192 || height > 8192)
    {
        return NULL;
    }
    const size_t bytes = (size_t)width * (size_t)height * 4;
    csmUint8* copy = (csmUint8*)malloc(bytes);
    if (copy == NULL)
    {
        return NULL;
    }
    memcpy(copy, rgba, bytes);

    SoftTexture* texture = new SoftTexture();
    texture->_pixels = copy;
    texture->_width = width;
    texture->_height = height;

#if !L2D_ALPHA_SUMMARY
    return texture; // 透明预判摘要表已由 config 关闭
#endif

    const int blockSize = 1 << kBlockShift;
    texture->_summaryW = (width + blockSize - 1) >> kBlockShift;
    texture->_summaryH = (height + blockSize - 1) >> kBlockShift;
    const size_t summaryCount = (size_t)texture->_summaryW * (size_t)texture->_summaryH;
    texture->_alphaSummary = (csmUint8*)malloc(summaryCount);
    if (texture->_alphaSummary != NULL)
    {
        for (int by = 0; by < texture->_summaryH; ++by)
        {
            for (int bx = 0; bx < texture->_summaryW; ++bx)
            {
                csmUint8 maxAlpha = 0;
                const int yEnd = (by << kBlockShift) + blockSize;
                const int xEnd = (bx << kBlockShift) + blockSize;
                for (int y = (by << kBlockShift); y < yEnd && y < height; ++y)
                {
                    const csmUint8* row = copy + ((size_t)y * (size_t)width) * 4;
                    for (int x = (bx << kBlockShift); x < xEnd && x < width; ++x)
                    {
                        const csmUint8 a = row[(size_t)x * 4 + 3];
                        if (a > maxAlpha)
                        {
                            maxAlpha = a;
                        }
                    }
                }
                texture->_alphaSummary[(size_t)by * texture->_summaryW + bx] = maxAlpha;
            }
        }
    }
    return texture;
}

}}}}

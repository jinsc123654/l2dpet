/**
 * PetText.cpp — 见 PetText.h
 *
 * 唯一的消费者是 GLFW 桌宠端口（PortGlfwPet）——freetype 也是那边引入的依赖。
 * 不带该端口的构建（板子的 armhf 交叉构建）整份跳过：sysroot 里没有 ft2build.h，
 * 编它会直接断在 include 上。
 */
#ifdef L2D_WITH_GLFW_PET

#include "PetText.h"
#include "Platform.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace pet
{

namespace
{

/** 系统 CJK 字体候选（按平台；取第一个存在的） */
const char* kFontCandidates[] =
{
#if defined(_WIN32)
    "C:/Windows/Fonts/msyh.ttc",          // 微软雅黑
    "C:/Windows/Fonts/msyh.ttf",
    "C:/Windows/Fonts/simhei.ttf",        // 黑体
    "C:/Windows/Fonts/simsun.ttc",        // 宋体
#elif defined(__APPLE__)
    "/System/Library/Fonts/PingFang.ttc", // 苹方
    "/System/Library/Fonts/STHeiti Medium.ttc",
    "/Library/Fonts/Arial Unicode.ttf",
#else
    "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
    "/usr/share/fonts/truetype/wqy/wqy-microhei.ttc",
    "/usr/share/fonts/truetype/wqy/wqy-zenhei.ttc",
    "/usr/share/fonts/opentype/noto/NotoSansCJK-Light.ttc",
    "/usr/share/fonts/truetype/misans/MiSans-Medium.ttf",
    "/usr/share/fonts/truetype/droid/DroidSansFallbackFull.ttf",
    "/usr/share/fonts/truetype/arphic/uming.ttc",
#endif
};

}   // namespace

PetFont::~PetFont()
{
    if (_face != NULL) FT_Done_Face(_face);
    if (_lib != NULL) FT_Done_FreeType(_lib);
}

uint32_t PetFont::NextCodepoint(const std::string& s, size_t* i)
{
    const unsigned char c = (unsigned char)s[*i];
    uint32_t cp = c;
    size_t n = 1;
    if (c >= 0xF0) { cp = (uint32_t)(c & 0x07); n = 4; }
    else if (c >= 0xE0) { cp = (uint32_t)(c & 0x0F); n = 3; }
    else if (c >= 0xC0) { cp = (uint32_t)(c & 0x1F); n = 2; }
    for (size_t k = 1; k < n && *i + k < s.size(); ++k)
    {
        cp = (cp << 6) | (uint32_t)((unsigned char)s[*i + k] & 0x3F);
    }
    *i += n;
    return cp;
}

bool PetFont::Load(float pixelSize)
{
    if (_lib == NULL && FT_Init_FreeType(&_lib) != 0)
    {
        fprintf(stderr, "[pettext] freetype 初始化失败\n");
        return false;
    }
    if (_face == NULL)
    {
        std::string path;
        if (const char* e = getenv("L2DPET_FONT")) path = e;
        if (path.empty())
        {
            for (size_t i = 0; i < sizeof(kFontCandidates) / sizeof(kFontCandidates[0]); ++i)
            {
                if (plat::IsFile(kFontCandidates[i]))
                {
                    path = kFontCandidates[i];
                    break;
                }
            }
        }
        if (path.empty())
        {
            fprintf(stderr, "[pettext] 找不到可用 CJK 字体（L2DPET_FONT 可指定）\n");
            return false;
        }
        if (FT_New_Face(_lib, path.c_str(), 0, &_face) != 0)
        {
            fprintf(stderr, "[pettext] 字体打不开：%s\n", path.c_str());
            _face = NULL;
            return false;
        }
        SetPixelSize(pixelSize);
        fprintf(stderr, "[pettext] 字体：%s（%.1fpx，行高 %d）\n", path.c_str(), pixelSize, _lineH);
    }
    return SetPixelSize(pixelSize);
}

bool PetFont::SetPixelSize(float pixelSize)
{
    if (_face == NULL) return false;
    const int px = (int)(pixelSize + 0.5f);
    if (px <= 0) return false;
    if (fabsf(pixelSize - _pixelSize) < 0.5f && _lineH > 0) return true;
    if (FT_Set_Pixel_Sizes(_face, 0, (FT_UInt)px) != 0) return false;
    _pixelSize = pixelSize;
    _lineH = (int)(_face->size->metrics.height / 64.0f + 0.5f);
    if (_lineH <= 0) _lineH = px + 4;
    _cache.clear();   // 字号变了，缓存失效
    return true;
}

const PetFont::Glyph* PetFont::GetGlyph(uint32_t cp)
{
    std::map<uint32_t, Glyph>::iterator it = _cache.find(cp);
    if (it != _cache.end()) return &it->second;
    if (_face == NULL) return NULL;
    if (FT_Load_Char(_face, cp, FT_LOAD_RENDER) != 0) return NULL;
    const FT_GlyphSlot g = _face->glyph;
    Glyph gl;
    gl.w = (int)g->bitmap.width;
    gl.h = (int)g->bitmap.rows;
    gl.left = g->bitmap_left;
    gl.top = g->bitmap_top;
    gl.advance = (int)(g->advance.x / 64);
    if (gl.w > 0 && gl.h > 0)
    {
        gl.alpha.resize((size_t)gl.w * gl.h);
        for (int y = 0; y < gl.h; ++y)
        {
            memcpy(&gl.alpha[(size_t)y * gl.w], g->bitmap.buffer + (size_t)y * g->bitmap.pitch, (size_t)gl.w);
        }
    }
    return &(_cache[cp] = gl);
}

int PetFont::TextWidth(const std::string& utf8)
{
    if (_face == NULL) return 0;
    int x = 0;
    for (size_t i = 0; i < utf8.size();)
    {
        const uint32_t cp = NextCodepoint(utf8, &i);
        const Glyph* g = GetGlyph(cp);
        if (g != NULL) x += g->advance;
    }
    return x;
}

void PetFont::Draw(uint32_t* buf, int bufW, int bufH, int x0, int y0, const std::string& utf8,
                   unsigned char r, unsigned char g, unsigned char b, float a)
{
    if (_face == NULL || a <= 0.0f) return;
    if (a > 1.0f) a = 1.0f;
    const int baseline = y0 + (int)(_face->size->metrics.ascender / 64.0f + 0.5f);
    int pen = x0;
    for (size_t i = 0; i < utf8.size();)
    {
        const uint32_t cp = NextCodepoint(utf8, &i);
        const Glyph* gl = GetGlyph(cp);
        if (gl == NULL) continue;
        const int gx = pen + gl->left;
        const int gy = baseline - gl->top;
        for (int y = 0; y < gl->h; ++y)
        {
            const int py = gy + y;
            if (py < 0 || py >= bufH) continue;
            const unsigned char* row = (const unsigned char*)gl->alpha.data() + (size_t)y * gl->w;
            for (int x = 0; x < gl->w; ++x)
            {
                const int px = gx + x;
                if (px < 0 || px >= bufW) continue;
                const float ga = (float)row[x] / 255.0f * a;
                if (ga <= 0.0f) continue;
                uint32_t* d = buf + (size_t)py * bufW + px;
                const uint32_t dv = *d;
                const float da = (float)((dv >> 24) & 0xFF) / 255.0f;
                const float ia = 1.0f - ga;
                const int R = (int)((float)r * ga + (float)((dv >> 16) & 0xFF) * ia);
                const int G = (int)((float)g * ga + (float)((dv >> 8) & 0xFF) * ia);
                const int B = (int)((float)b * ga + (float)(dv & 0xFF) * ia);
                const int A = (int)(255.0f * (ga + da * ia));
                *d = ((uint32_t)(A > 255 ? 255 : A) << 24) | ((uint32_t)(R > 255 ? 255 : R) << 16)
                     | ((uint32_t)(G > 255 ? 255 : G) << 8) | (uint32_t)(B > 255 ? 255 : B);
            }
        }
        pen += gl->advance;
    }
}

}   // namespace pet

#endif // L2D_WITH_GLFW_PET

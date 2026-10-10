/**
 * PetText.h — 跨平台字体渲染（freetype）：UTF-8 文字画进预乘 ARGB 缓冲
 *
 * 用途：气泡文字（三种桌面平台同一份像素代码，不依赖 Xft）。
 * 字体自动查找：L2DPET_FONT 环境变量 > 各平台常见 CJK 字体路径。
 */
#pragma once

#include <cstdint>
#include <map>
#include <string>

#include <ft2build.h>
#include FT_FREETYPE_H

namespace pet
{

class PetFont
{
public:
    PetFont() {}
    ~PetFont();

    /** 按像素字号加载（找不到字体返回 false；L2DPET_FONT 可指定字体文件）。 */
    bool Load(float pixelSize);

    /** 换字号（气泡跟随缩放）。 */
    bool SetPixelSize(float pixelSize);

    bool Ready() const { return _face != NULL; }
    int LineHeight() const { return _lineH; }

    /** 文本像素宽（UTF-8；缺字形按 .notdef 宽度算）。 */
    int TextWidth(const std::string& utf8);

    /**
     * 画一段 UTF-8 文字到预乘 ARGB 缓冲（左上角 x,y；抗锯齿用字形灰度）。
     * a = 总透明度（淡入淡出）；颜色 (r,g,b) 直接写进缓冲（预乘）。
     */
    void Draw(uint32_t* buf, int bufW, int bufH, int x, int y, const std::string& utf8,
              unsigned char r, unsigned char g, unsigned char b, float a);

private:
    struct Glyph
    {
        int w = 0, h = 0;         // 位图尺寸
        int left = 0, top = 0;    // 相对笔位
        int advance = 0;
        std::string alpha;        // w*h 灰度
    };

    const Glyph* GetGlyph(uint32_t cp);
    static uint32_t NextCodepoint(const std::string& s, size_t* i);

    FT_Library _lib = NULL;
    FT_Face _face = NULL;
    float _pixelSize = 0.0f;
    int _lineH = 0;
    std::map<uint32_t, Glyph> _cache;
};

}   // namespace pet

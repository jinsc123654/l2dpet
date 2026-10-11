#pragma once
#include <cctype>
#include <cstring>
#include <string>

/**
 * 水印判定：作者提供的水印开关表情叫什么名字没有统一规范——实测见过
 * "水印.exp3.json"、"13 水印消除"、"shuiyin_off"、"watermark off"、"ウォーターマーク"。
 * 这里按关键词匹配表情名**或**文件名（ASCII 关键词不区分大小写），供
 * SetupWatermarkControl 找"该模型的水印开关"（见 PlayerLife.cpp）。
 *
 * 只认水印本身的词：不要把"版权/logo/B站"这类算进来——那种表情多半是**显示**
 * 版权字样，套上去等于把水印打开。
 */
namespace petwatermark
{
inline bool NameMatch(const char* s)
{
    if (s == NULL || *s == '\0')
    {
        return false;
    }
    static const char* const kWord[] = { "水印", "ウォーターマーク", "透かし" };
    for (size_t i = 0; i < sizeof(kWord) / sizeof(kWord[0]); ++i)
    {
        if (strstr(s, kWord[i]) != NULL) return true;
    }
    std::string low(s);
    for (size_t i = 0; i < low.size(); ++i) low[i] = (char)tolower((unsigned char)low[i]);
    static const char* const kAscii[] = { "shuiyin", "watermark" };
    for (size_t i = 0; i < sizeof(kAscii) / sizeof(kAscii[0]); ++i)
    {
        if (low.find(kAscii[i]) != std::string::npos) return true;
    }
    return false;
}
}   // namespace petwatermark

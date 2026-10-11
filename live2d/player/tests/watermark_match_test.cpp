// Standalone regression test: the watermark-expression keyword rule.
// Windows:  cl /nologo /std:c++14 /EHsc /utf-8 live2d/player/tests/watermark_match_test.cpp /Fe:watermark_match_test.exe
// Linux:    g++ -std=c++11 live2d/player/tests/watermark_match_test.cpp -o /tmp/watermark_match_test
#include "../src/PetWatermark.h"
#include <cassert>
#include <iostream>

int main()
{
    // 各家的叫法都要认出来（大肥鱼 EXP/13 水印消除、舒芙蕾 水印.exp3.json 是实际遇到的）
    assert(petwatermark::NameMatch("13 水印消除"));
    assert(petwatermark::NameMatch("水印"));
    assert(petwatermark::NameMatch("EXP/13 水印消除.exp3.json"));
    assert(petwatermark::NameMatch("shuiyin_off.exp3.json"));
    assert(petwatermark::NameMatch("ShuiYin.exp3.json"));
    assert(petwatermark::NameMatch("watermark off.exp3.json"));
    assert(petwatermark::NameMatch("WaterMark.exp3.json"));
    assert(petwatermark::NameMatch("ウォーターマーク.exp3.json"));
    assert(petwatermark::NameMatch("透かし.exp3.json"));

    // 普通表情 / 空值不能误伤：水印控制会每帧强压匹配到的参数值
    assert(!petwatermark::NameMatch("哭哭"));
    assert(!petwatermark::NameMatch("13 尾巴消失"));
    assert(!petwatermark::NameMatch("exp/01 口型切换.exp3.json"));
    assert(!petwatermark::NameMatch("water.exp3.json"));
    assert(!petwatermark::NameMatch("copyright.exp3.json"));
    assert(!petwatermark::NameMatch(""));
    assert(!petwatermark::NameMatch(NULL));

    std::cout << "PASS watermark keyword matching (cn/ja/pinyin/english, case-insensitive; no false hits)\n";
}

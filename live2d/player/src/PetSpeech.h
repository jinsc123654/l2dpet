/**
 * PetSpeech.h — 给 LLM 的记忆文本 / LLM 写出来的台词：去掉日期与时间戳。
 *
 * 为什么：记忆里每条都带"10-10 02:09–02:11"这样的时间戳（存盘要用），原样喂给 LLM 后它会照抄进台词，
 * 甚至把"称呼：10-10 他让我叫他的名字"里的 10-10 当成主人的名字（"再摸一下下嘛，10-10。"）。
 * 记忆文件本身保留时间戳；进提示词前、以及台词/台词库/笑话/问题输出后，统一过一遍 StripStamps。
 */
#pragma once

#include <regex>
#include <string>

inline std::string StripStamps(const std::string& in)
{
    // 只匹配 ASCII 数字与分隔符（UTF-8 安全：中文字节都 ≥0x80，不会被拆开）
    static const std::regex kPats[] = {
        // 注意：中文只能用 (a|b) 交替，不能放进 [...]——字符类按字节匹配，会把汉字拆成半个
        std::regex(R"(\d{4}\s*(-|/|\.|年)\s*\d{1,2}\s*(-|/|\.|月)\s*\d{1,2}\s*(日|号)?)"),         // 2026-10-10 / 2026年10月10日
        std::regex(R"(\d{1,2}\s*月\s*\d{1,2}\s*(日|号))"),                                            // 10月10日
        std::regex(R"(\b\d{1,2}[-/]\d{1,2}\b(\s*\d{1,2}:\d{2}(\s*(–|-|~|～)\s*\d{1,2}:\d{2})?)?)"),  // 10-10 / 10-10 02:09–02:11
        std::regex(R"(\b\d{1,2}:\d{2}(\s*(–|-|~|～)\s*\d{1,2}:\d{2})?\b)"),                         // 02:09 / 02:09–02:11
    };
    std::string s = in;
    for (const std::regex& re : kPats) s = std::regex_replace(s, re, "");
    // 收拾残留标点/空白："，，"→"，"、"：，"→"："、行首/句首的"，、 "
    static const std::regex kFix[] = { std::regex("(，|、|,)\\s*(，|、|,|。|！|？)"), std::regex("：\\s*(，|、|,|\\s)\\s*"),
                                       std::regex("(^|\\n)\\s*(-\\s+)?(，|、|,|\\s)+") };
    s = std::regex_replace(s, kFix[0], "$2");
    s = std::regex_replace(s, kFix[1], "：");
    s = std::regex_replace(s, kFix[2], "$1$2");
    s = std::regex_replace(s, std::regex("[ \\t]{2,}"), " ");
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t')) --b;
    return s.substr(a, b - a);
}

// ---------------------------------------------------------------- 台词语言校验
// 切到日语/英语后，LLM 写台词库时常把中文夹进来（"やめてよ、脸会扁""ふわ、脖子痒"）或整句中文。
// 生成结果逐句过这一关，不合格的丢掉（该组没剩下就整组不下发 → 桌宠用内置的本地化兜底台词）。

/** 当前台词语言（"zh"/"ja"/"en"）：agent 线程在语言变化时设置；生成笑话/题库时据此过滤 */
inline std::string& SpeechLang()
{
    static std::string lang = "zh";
    return lang;
}

/** 这句话是不是目标语言写的（zh/ja/en 三选一；别的一律通过） */
inline bool SpeechLangOk(const std::string& s, const std::string& lang)
{
    if (lang != "ja" && lang != "en" && lang != "zh") return true;
    // 中文简体专用字（日语用不同字形或不用）：日文台词里出现它们 = 夹了中文
    static const char* const kZhOnly[] = { "的", "了", "说", "话", "这", "们", "没", "给", "让", "别", "对", "过", "还", "边",
                                           "进", "远", "开", "关", "车", "门", "问", "见", "觉", "长", "头", "发", "脸", "脖",
                                           "样", "个", "为", "乐", "爱", "欢", "气", "亲", "吗", "呢", "啦", "吧", "么", "哦",
                                           "嘛", "呀", "哈", "时", "间", "后", "里", "东", "坏", "许" };
    int han = 0, kana = 0, other = 0;
    for (size_t i = 0; i < s.size();)
    {
        const unsigned char c = (unsigned char)s[i];
        const size_t n = c < 0x80 ? 1 : ((c & 0xE0) == 0xC0 ? 2 : ((c & 0xF0) == 0xE0 ? 3 : 4));
        if (i + n > s.size()) break;
        unsigned cp = n == 1 ? c : (n == 2 ? (c & 0x1F) : (n == 3 ? (c & 0x0F) : (c & 0x07)));
        for (size_t k = 1; k < n; ++k) cp = (cp << 6) | ((unsigned char)s[i + k] & 0x3F);
        if (cp >= 0x4E00 && cp <= 0x9FFF) ++han;
        else if ((cp >= 0x3040 && cp <= 0x30FF) || (cp >= 0x31F0 && cp <= 0x31FF)) ++kana;   // 平/片假名
        else if (cp >= 0x3000 && cp <= 0x303F) {}   // 中日共用标点
        else if (cp >= 0xFF00 && cp <= 0xFFEF) {}   // 全角符号
        else if (cp >= 0x80) ++other;
        i += n;
    }
    if (lang == "en") return han == 0 && kana == 0;   // 英文台词不许有任何汉字/假名
    if (lang == "zh")
    {
        // 中文台词里**一个汉字都没有、却有成串拉丁字母** = 整句英文（"Why did the chicken…"）。
        // 中文里夹英文词很正常（"今天被 Python 毒打了"），所以只看"没有一个汉字"这一种情况；
        // 零星 ASCII（Oct 31 / 2333 / :)）放过。
        if (han > 0) return true;
        int latin = 0;
        for (size_t i = 0; i < s.size(); ++i)
        {
            const char c = s[i];
            if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) ++latin;
        }
        return latin < 3;
    }
    for (const char* z : kZhOnly)
    {
        if (s.find(z) != std::string::npos) return false;
    }
    return !(kana == 0 && han >= 4);   // 一长串汉字却没有一个假名：多半是中文句子（"大丈夫！"这种短的放过）
}

inline bool SpeechLangOk(const std::string& s)
{
    return SpeechLangOk(s, SpeechLang());
}

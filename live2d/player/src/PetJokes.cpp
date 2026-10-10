/**
 * PetJokes.cpp — 见 PetJokes.h（笑话池：LLM 增量补货 + 落盘 + 讲过标记）
 */

#include "PetJokes.h"
#include "PetSpeech.h"

#include <cstdio>
#include <cstdlib>
#include <ctime>

#include "Platform.h"

namespace
{

const size_t kPoolMax = 40;   // 池子上限（超了丢最久没讲的已讲条目）

std::string FileId(const std::string& in)
{
    std::string s = in;
    if (s.compare(0, 6, "model:") == 0) s = s.substr(6);
    std::string out;
    for (size_t i = 0; i < s.size() && out.size() < 64; ++i)
    {
        const unsigned char c = (unsigned char)s[i];
        const bool ok = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')
                        || c == '_' || c == '-';
        out += ok ? s[i] : '_';
    }
    return out.empty() ? std::string("pet") : out;
}

std::string NowStamp()
{
    time_t t = time(NULL);
    struct tm lt;
#ifdef _WIN32
    localtime_s(&lt, &t);
#else
    localtime_r(&t, &lt);
#endif
    char buf[32];
    strftime(buf, sizeof(buf), "%m-%d %H:%M", &lt);
    return buf;
}

std::string CutUtf8(const std::string& s, size_t maxBytes)
{
    if (s.size() <= maxBytes) return s;
    size_t cut = maxBytes;
    while (cut > 0 && ((unsigned char)s[cut] & 0xC0) == 0x80) --cut;
    return s.substr(0, cut);
}

std::string JsonEsc(const std::string& s)
{
    std::string o;
    for (size_t i = 0; i < s.size(); ++i)
    {
        const unsigned char c = (unsigned char)s[i];
        switch (c)
        {
        case '"': o += "\\\""; break;
        case '\\': o += "\\\\"; break;
        case '\n': o += "\\n"; break;
        case '\r': o += "\\r"; break;
        case '\t': o += "\\t"; break;
        default:
            if (c < 0x20)
            {
                char b[8];
                snprintf(b, sizeof(b), "\\u%04x", c);
                o += b;
            }
            else
            {
                o += (char)c;
            }
            break;
        }
    }
    return o;
}

const jsonlite::Value* Field(const jsonlite::Value& o, const char* key)
{
    return o.isObj() ? &o.get(key) : NULL;
}

std::string Str(const jsonlite::Value& o, const char* key)
{
    const jsonlite::Value* v = Field(o, key);
    return v != NULL ? v->strOr("") : std::string();
}

/** 规范化（判重用）：去掉空白与中英文标点，按 UTF-8 码点处理（不能逐字节比 '，'——那是多字节字符） */
std::string NormPunct(const std::string& s)
{
    static const char* const kDrop[] = { "，", "。", "！", "？", "、", "；", "：", "…", "～", "（", "）", "「", "」",
                                         "“", "”", "‘", "’", "　", "—" };
    std::string o;
    for (size_t i = 0; i < s.size();)
    {
        const unsigned char c = (unsigned char)s[i];
        const size_t n = c < 0x80 ? 1 : ((c & 0xE0) == 0xC0 ? 2 : ((c & 0xF0) == 0xE0 ? 3 : 4));
        if (i + n > s.size()) break;
        bool drop = false;
        if (n == 1)
        {
            drop = c < 0x20 || c == ' ' || c == '\t' || c == '!' || c == '?' || c == ',' || c == '.' || c == '~';
        }
        else
        {
            for (const char* k : kDrop) drop = drop || s.compare(i, n, k) == 0;
        }
        if (!drop) o.append(s, i, n);
        i += n;
    }
    return o;
}

/** 去空白/标点后判重（LLM 有时只改标点） */
std::string Norm(const std::string& s)
{
    return NormPunct(s);
}

/** 目标语言的**自称**（写进提示词：LLM 看到哪种语言的名字就更容易用哪种语言写正文）。
 *  词库是按台词语言（SpeechLang()）生成的——不点明的话模型常按自己的习惯整段写英文。*/
const char* LangWord()
{
    const std::string& l = SpeechLang();
    if (l == "ja") return "日本語";
    if (l == "en") return "English";
    return "简体中文";
}

const char* kGenSystem =
    "你在给一个 Live2D 桌宠写\"讲给主人听的短笑话\"。要求：\n"
    "- 只输出一个 JSON：{\"jokes\":[\"…\",\"…\"]}，不要解释。\n"
    "- 每条 ≤45 个字，口语、念出来就能听懂（不要表情符号、不要网络烂梗、不要\"哈哈哈\"收尾、不要问\"你懂了吗\"）。\n"
    "- 可以自嘲、可以关于小动物/吃的/天气/程序员/日常小事；冷幽默或可爱的都行。\n"
    "- 要像**她**讲的：符合给的角色描述（口吻可以有她的口癖，但别每句都加，也别把笑话写成自我介绍）。\n"
    "- 用户会给一份\"已经有的\"清单，**不要重复**它们（换汤不换药也不行）。";

}   // namespace

bool PetJokes::Load(const std::string& fp)
{
    Reset();
    _fp = FileId(fp);
    _path = plat::HomeDir() + "/.config/l2dpet/jokes/" + _fp + ".json";
    const std::string text = plat::ReadTextFile(_path);
    if (text.empty()) return false;
    jsonlite::Value v;
    if (!jsonlite::parse(text.c_str(), text.size(), v, NULL) || !v.isObj()) return false;
    _model = Str(v, "model");
    _at = Str(v, "at");
    if (const jsonlite::Value* t = Field(v, "t")) _t = t->numOr(0.0);
    const jsonlite::Value* items = Field(v, "items");
    if (items == NULL || !items->isArr()) return false;
    for (size_t i = 0; i < items->arr.size(); ++i)
    {
        const jsonlite::Value& it = items->arr[i];
        Item one;
        one.j = Str(it, "j");
        one.u = Str(it, "u");
        if (const jsonlite::Value* u = Field(it, "used")) one.used = (int)u->numOr(0.0);
        if (one.j.empty()) continue;
        // 语言对不上的（切语言前那批）**读进来就丢**：留着只会占"没讲过"的名额，
        // 让 NeedMore 误以为池子够用，还把本地讲的笑话锁死在旧语言上（下次 Save 顺手清文件）
        if (!SpeechLangOk(one.j)) continue;
        _items.push_back(one);
    }
    return !_items.empty();
}

bool PetJokes::Save(std::string* err) const
{
    plat::CreateDir(plat::HomeDir() + "/.config/l2dpet/jokes", true);
    std::string o = "{\"v\":1,\"fp\":\"" + JsonEsc(_fp) + "\",\"model\":\"" + JsonEsc(_model) + "\"";
    o += ",\"at\":\"" + JsonEsc(_at) + "\"";
    {
        char b[32];
        snprintf(b, sizeof(b), "%.0f", _t);
        o += std::string(",\"t\":") + b;
    }
    o += ",\"items\":[";
    for (size_t i = 0; i < _items.size(); ++i)
    {
        if (i) o += ",";
        o += "{\"j\":\"" + JsonEsc(_items[i].j) + "\",\"used\":" + std::to_string(_items[i].used);
        if (!_items[i].u.empty()) o += ",\"u\":\"" + JsonEsc(_items[i].u) + "\"";
        o += "}";
    }
    o += "]}";
    if (!plat::WritePrivateFile(_path, o))
    {
        if (err != NULL) *err = "笑话池写入失败（权限/磁盘）";
        return false;
    }
    return true;
}

bool PetJokes::Generate(const std::string& fp, const std::string& modelName, const std::string& personaBlurb, int n,
                        const LlmFn& callFn, std::string* err)
{
    if (!callFn)
    {
        if (err != NULL) *err = "没有 LLM 调用器";
        return false;
    }
    if (n < 1) n = 6;
    if (n > 10) n = 10;
    std::string user = "角色：" + (personaBlurb.empty() ? modelName : personaBlurb) + "\n";
    user += "已经有的（不要重复）：\n";
    if (_items.empty())
    {
        user += "  （还没有，这是第一批）\n";
    }
    else
    {
        for (size_t i = 0; i < _items.size() && i < 24; ++i) user += "  - " + _items[i].j + "\n";
        if (_items.size() > 24) user += "  …（还有 " + std::to_string(_items.size() - 24) + " 条，同样别重复）\n";
    }
    user += "\n请用" + std::string(LangWord()) + "写 " + std::to_string(n) + " 条新的短笑话 JSON（正文就用"
            + LangWord() + "，整句别用别的语言）。";
    std::string out;
    // 系统提示里也点一句：只在 user 里说的话，模型有时还是按自己的习惯整段写英文
    const std::string sys = std::string(kGenSystem) + "\n- 笑话正文必须用" + LangWord() + "写。";
    if (!callFn(sys, user, &out, err)) return false;
    const size_t b = out.find('{');
    const size_t e = out.rfind('}');
    if (b == std::string::npos || e == std::string::npos || e <= b)
    {
        if (err != NULL) *err = "笑话输出不是 JSON";
        return false;
    }
    jsonlite::Value v;
    if (!jsonlite::parse(out.c_str() + b, e - b + 1, v, NULL) || !v.isObj())
    {
        if (err != NULL) *err = "笑话 JSON 解析失败（开头：" + CutUtf8(out, 90) + "…）";
        return false;
    }
    const jsonlite::Value* arr = Field(v, "jokes");
    if (arr == NULL || !arr->isArr() || arr->arr.empty())
    {
        if (err != NULL) *err = "笑话缺少 jokes 数组";
        return false;
    }
    std::vector<std::string> have;
    for (size_t i = 0; i < _items.size(); ++i) have.push_back(Norm(_items[i].j));
    int added = 0;
    for (size_t i = 0; i < arr->arr.size() && added < n; ++i)
    {
        std::string j = arr->arr[i].strOr("");
        if (j.empty() || j.size() > 200) continue;
        if (!SpeechLangOk(j)) continue;   // 切到日语/英语后夹了中文的笑话不要
        const std::string key = Norm(j);
        if (key.size() < 4) continue;
        bool dup = false;
        for (size_t k = 0; k < have.size() && !dup; ++k)
        {
            dup = have[k] == key || have[k].find(key) != std::string::npos || key.find(have[k]) != std::string::npos;
        }
        if (dup) continue;
        Item one;
        one.j = CutUtf8(StripStamps(j), 150);
        _items.push_back(one);
        have.push_back(key);
        ++added;
    }
    if (added == 0)
    {
        if (err != NULL) *err = "生成的笑话都是已有/不可用的";
        return false;
    }
    // 超上限：丢最久没讲的已讲条目（没讲过的都留着）
    while (_items.size() > kPoolMax)
    {
        size_t victim = 0;
        bool found = false;
        for (size_t i = 0; i < _items.size(); ++i)
        {
            if (_items[i].used <= 0) continue;
            if (!found || _items[i].u < _items[victim].u)
            {
                victim = i;
                found = true;
            }
        }
        if (!found) break;
        _items.erase(_items.begin() + (long)victim);
    }
    _fp = FileId(fp);
    _model = modelName;
    _at = NowStamp();
    _t = (double)time(NULL);
    return true;
}

int PetJokes::UnusedCount() const
{
    int n = 0;
    for (size_t i = 0; i < _items.size(); ++i) n += _items[i].used <= 0 ? 1 : 0;
    return n;
}

bool PetJokes::NeedMore(double minGapSec) const
{
    if (_items.empty()) return true;
    if (UnusedCount() >= 2) return false;
    return (double)time(NULL) - _t >= minGapSec;
}

void PetJokes::MarkUsed(const std::string& j)
{
    const std::string key = Norm(j);
    if (key.empty()) return;
    for (size_t i = 0; i < _items.size(); ++i)
    {
        const std::string have = Norm(_items[i].j);
        if (have == key || have.find(key) != std::string::npos || key.find(have) != std::string::npos)
        {
            _items[i].used++;
            _items[i].u = NowStamp();
            return;
        }
    }
}

std::vector<std::string> PetJokes::Texts() const
{
    std::vector<std::string> out;
    for (size_t i = 0; i < _items.size(); ++i) out.push_back(_items[i].j);
    return out;
}

std::string PetJokes::Summary() const
{
    if (_items.empty()) return "笑话池：空";
    return "笑话池：" + std::to_string(_items.size()) + " 条（没讲过 " + std::to_string(UnusedCount()) + " 条）";
}

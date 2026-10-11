/**
 * PetQuestions.cpp — 见 PetQuestions.h（题库：LLM 按题材生成 + 落盘 + 已问/已答跟踪）
 */

#include "PetQuestions.h"
#include "PetSpeech.h"

#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <cstdint>
#include <algorithm>
#include <cctype>

#include "Platform.h"

namespace
{

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

std::string StrArr(const std::vector<std::string>& v)
{
    std::string o = "[";
    for (size_t i = 0; i < v.size(); ++i)
    {
        if (i) o += ",";
        o += "\"" + JsonEsc(v[i]) + "\"";
    }
    return o + "]";
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

std::string QuestionIntent(const std::string& text)
{
    std::string s = NormPunct(text);
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    const auto has = [&](const char* t) { return s.find(t) != std::string::npos; };
    if (has("未来") || has("以后") || has("想做") || has("freetime") || has("forfun") || has("hobby")) return "";
    if ((has("平时") && (has("做什么") || has("干什么") || has("忙什么")))
        || has("职业") || (has("工作") && (has("上学") || has("学习") || has("学生")))
        || has("whatdoyoudo") || has("occupation") || (has("work") && has("study"))
        || has("職業") || (has("仕事") && has("学校"))) return "occupation";
    return "";
}

std::string QuestionId(const std::string& q)
{
    const std::string intent = QuestionIntent(q);
    const std::string key = intent.empty() ? NormPunct(q) : intent;
    uint64_t hash = UINT64_C(14695981039346656037);
    for (unsigned char c : key) { hash ^= c; hash *= UINT64_C(1099511628211); }
    char id[24];
    snprintf(id, sizeof(id), "q_%016llx", (unsigned long long)hash);
    return id;
}

bool SameQ(const std::string& a, const std::string& b)
{
    if (a.empty() || b.empty()) return false;
    const std::string x = NormPunct(a), y = NormPunct(b);
    if (x.empty() || y.empty()) return false;
    if (x.find(y) != std::string::npos || y.find(x) != std::string::npos) return true;
    const std::string intent = QuestionIntent(a);
    return !intent.empty() && intent == QuestionIntent(b);
}

const char* kGenSystem =
    "你在给一个 Live2D 桌宠写\"她想了解主人\"的问题库。每题要口语、贴合她的性格口吻，带 2~3 个可以点的短选项。\n"
    "要求：\n"
    "- 只输出一个 JSON 对象：{\"items\":[{\"topic\":\"…\",\"q\":\"…\",\"options\":[\"…\",\"…\"]}]}\n"
    "- 12 题，题材**尽量分散**，每个题材最多 2 题。题材清单（用作 topic 字段）：\n"
    "  称呼（怎么称呼主人）、作息（几点睡/几点起）、工作学习、吃（口味/忌口/咖啡还是茶）、爱好娱乐、\n"
    "  最近的心情、家人朋友、过去回忆（小时候/学生时代）、未来打算（想去哪/想做什么）、和她的关系（最喜欢她哪一点）、\n"
    "  她自己的小请求（想被怎么对待/想学什么）、随口闲聊（天气/猫狗/音乐）。\n"
    "- **不要**问\"你在干什么/在忙吗/在吗\"这类笼统问题；不要问是非题（能被\"是/不是\"答完的）。\n"
    "- 每题选项 2~3 个、每个 ≤6 个字、互斥且都能点；问题 ≤22 字，像她随口问的（可以带一点她的口癖）。\n"
    "- 用户会给你\"已经知道的\"（问过/答过），**不要重复**这些内容，也不要换汤不换药地再问一遍。\n"
    "- 不要提到模型、参数、AI；不要用书面语。";

}   // namespace

bool PetQuestions::Load(const std::string& fp)
{
    Reset();
    _fp = FileId(fp);
    _path = plat::HomeDir() + "/.config/l2dpet/questions/" + _fp + ".json";
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
        one.topic = Str(it, "topic");
        one.q = Str(it, "q");
        one.id = Str(it, "id");
        if (one.id.size() != 18 || one.id.compare(0, 2, "q_") != 0
            || !std::all_of(one.id.begin() + 2, one.id.end(), [](unsigned char c) { return std::isxdigit(c) != 0; }))
            one.id = QuestionId(one.q);
        one.answer = Str(it, "answer");
        one.at = Str(it, "at");
        if (const jsonlite::Value* a = Field(it, "asked")) one.asked = (int)a->numOr(0.0);
        if (const jsonlite::Value* o = Field(it, "options"))
        {
            for (size_t k = 0; o->isArr() && k < o->arr.size(); ++k) one.options.push_back(o->arr[k].strOr(""));
        }
        if (!one.q.empty()) _items.push_back(one);
    }
    return !_items.empty();
}

bool PetQuestions::Save(std::string* err) const
{
    plat::CreateDir(plat::HomeDir() + "/.config/l2dpet/questions", true);
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
        const Item& it = _items[i];
        if (i) o += ",";
        o += "{\"topic\":\"" + JsonEsc(it.topic) + "\",\"q\":\"" + JsonEsc(it.q) + "\"";
        o += ",\"id\":\"" + JsonEsc(it.id) + "\"";
        o += ",\"options\":" + StrArr(it.options);
        o += ",\"asked\":" + std::to_string(it.asked);
        if (!it.answer.empty()) o += ",\"answer\":\"" + JsonEsc(it.answer) + "\",\"at\":\"" + JsonEsc(it.at) + "\"";
        o += "}";
    }
    o += "]}";
    if (!plat::WritePrivateFile(_path, o))
    {
        if (err != NULL) *err = "题库写入失败（权限/磁盘）";
        return false;
    }
    return true;
}

bool PetQuestions::Generate(const std::string& fp, const std::string& modelName, const std::string& personaBlurb,
                            const std::vector<std::string>& knownQA, const LlmFn& callFn, std::string* err)
{
    if (!callFn)
    {
        if (err != NULL) *err = "没有 LLM 调用器";
        return false;
    }
    std::string user = "角色：" + (personaBlurb.empty() ? modelName : personaBlurb) + "\n";
    user += "已经知道/问过的（不要重复）：\n";
    if (knownQA.empty())
    {
        user += "  （还没有——第一批问题请覆盖尽量多的题材）\n";
    }
    else
    {
        for (size_t i = 0; i < knownQA.size() && i < 24; ++i) user += "  - " + knownQA[i] + "\n";
    }
    user += "\n请输出 12 题的问题库 JSON。";
    std::string out;
    if (!callFn(kGenSystem, user, &out, err)) return false;
    const size_t b = out.find('{');
    const size_t e = out.rfind('}');
    if (b == std::string::npos || e == std::string::npos || e <= b)
    {
        if (err != NULL) *err = "题库输出不是 JSON";
        return false;
    }
    jsonlite::Value v;
    if (!jsonlite::parse(out.c_str() + b, e - b + 1, v, NULL) || !v.isObj())
    {
        if (err != NULL) *err = "题库 JSON 解析失败（开头：" + CutUtf8(out, 90) + "…）";
        return false;
    }
    const jsonlite::Value* items = Field(v, "items");
    if (items == NULL || !items->isArr() || items->arr.empty())
    {
        if (err != NULL) *err = "题库缺少 items";
        return false;
    }
    // 保留旧库的"问过/答过"状态（按问题文本匹配），新题覆盖题库
    std::vector<Item> old = _items;
    std::vector<Item> fresh;
    for (size_t i = 0; i < items->arr.size() && fresh.size() < 16; ++i)
    {
        const jsonlite::Value& it = items->arr[i];
        Item one;
        one.q = Str(it, "q");
        one.topic = Str(it, "topic");
        if (one.q.empty()) continue;
        if (!SpeechLangOk(one.q)) continue;   // 问题不是当前语言（日/英里夹中文）：整题不要
        if (const jsonlite::Value* o = Field(it, "options"))
        {
            for (size_t k = 0; o->isArr() && k < o->arr.size() && k < 4; ++k)
            {
                const std::string s = o->arr[k].strOr("");
                if (!s.empty()) one.options.push_back(CutUtf8(StripStamps(s), 24));
            }
        }
        if (one.options.size() < 2) continue;   // 选择题至少要两个选项
        one.q = CutUtf8(StripStamps(one.q), 80);
        one.id = QuestionId(one.q);
        // Merge all matching legacy records; an unanswered duplicate must not hide a saved answer.
        bool inheritedId = false;
        for (size_t k = 0; k < old.size(); ++k)
        {
            if (SameQ(old[k].q, one.q))
            {
                if (!inheritedId && !old[k].id.empty()) { one.id = old[k].id; inheritedId = true; }
                one.asked = std::max(one.asked, old[k].asked);
                if (!old[k].answer.empty()) { one.answer = old[k].answer; one.at = old[k].at; }
            }
        }
        bool duplicate = false;
        for (const Item& existing : fresh) duplicate = duplicate || SameQ(existing.q, one.q);
        if (!duplicate) fresh.push_back(one);
    }
    if (fresh.empty())
    {
        if (err != NULL) *err = "题库没有可用题目";
        return false;
    }
    // 问过的旧题（这轮没重出）也带回来，避免未回答历史丢失
    for (size_t k = 0; k < old.size(); ++k)
    {
        if (old[k].answer.empty() && old[k].asked == 0) continue;
        bool has = false;
        for (size_t i = 0; i < fresh.size() && !has; ++i) has = SameQ(fresh[i].q, old[k].q);
        if (!has) fresh.push_back(old[k]);
    }
    _fp = FileId(fp);
    _model = modelName;
    _at = NowStamp();
    _t = (double)time(NULL);
    _items = fresh;
    return true;
}

int PetQuestions::AskedCount() const
{
    int n = 0;
    for (size_t i = 0; i < _items.size(); ++i) n += _items[i].asked > 0 ? 1 : 0;
    return n;
}

int PetQuestions::AnsweredCount() const
{
    int n = 0;
    for (size_t i = 0; i < _items.size(); ++i) n += _items[i].answer.empty() ? 0 : 1;
    return n;
}

bool PetQuestions::Stale(double maxAgeSec) const
{
    if (_t <= 0.0) return true;
    return (double)time(NULL) - _t > maxAgeSec;
}

std::vector<const PetQuestions::Item*> PetQuestions::Fresh(size_t n) const
{
    std::vector<const Item*> out;
    // Never recycle unanswered questions; regenerated banks preserve asked history.
    for (size_t i = 0; i < _items.size() && out.size() < n; ++i)
    {
        const Item& it = _items[i];
        if (!it.answer.empty() || it.asked > 0 || AlreadyAsked(it.q)) continue;
        out.push_back(&it);
    }
    return out;
}

PetQuestions::Item* PetQuestions::FindByQ(const std::string& q)
{
    for (size_t i = 0; i < _items.size(); ++i)
    {
        if (SameQ(_items[i].q, q)) return &_items[i];
    }
    return NULL;
}

void PetQuestions::MarkAsked(const std::string& q, const std::vector<std::string>& options)
{
    if (q.empty()) return;
    Item* it = FindByQ(q);
    if (it == NULL)
    {
        Item one;
        one.q = CutUtf8(q, 80);
        one.id = QuestionId(one.q);
        one.topic = "自创";
        one.options = options;
        one.asked = 1;
        _items.push_back(one);
        return;
    }
    it->asked++;
}

void PetQuestions::MarkAnswered(const std::string& q, const std::string& label)
{
    if (label.empty()) return;
    Item* it = FindByQ(q);
    if (it == NULL)
    {
        Item one;
        one.q = CutUtf8(q.empty() ? std::string("（没记下问题）") : q, 80);
        one.id = QuestionId(one.q);
        one.topic = "自创";
        one.asked = 1;
        one.answer = CutUtf8(label, 40);
        one.at = NowStamp();
        _items.push_back(one);
        return;
    }
    it->answer = CutUtf8(label, 40);
    it->at = NowStamp();
    if (it->asked == 0) it->asked = 1;
}

std::string PetQuestions::KnownText() const
{
    std::string o;
    for (size_t i = 0; i < _items.size(); ++i)
    {
        const Item& it = _items[i];
        if (it.answer.empty() && it.asked == 0) continue;
        if (!o.empty()) o += "；";
        o += it.q;
        if (!it.answer.empty()) o += "→" + it.answer;
        else o += "（问过没答）";
    }
    return o;
}

bool PetQuestions::AlreadyAsked(const std::string& q) const
{
    for (const Item& item : _items)
        if ((item.asked > 0 || !item.answer.empty()) && SameQ(item.q, q)) return true;
    return false;
}

std::string PetQuestions::Summary() const
{
    if (_items.empty()) return "题库：空";
    return "题库：" + std::to_string(_items.size()) + " 题（已问 " + std::to_string(AskedCount()) + " / 已答 "
           + std::to_string(AnsweredCount()) + "）";
}

/**
 * PetPersona.cpp — 见 PetPersona.h（模型能力 → LLM 生成性格 → 落盘 + 注入提示词）
 */

#include "PetPersona.h"

#include <cstdio>
#include <cstdlib>
#include <ctime>

#include "Platform.h"

namespace
{

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

/** 文件名用的指纹（去 "model:" 前缀 + 只留字母数字/_/-，≤64 字节；与记忆文件命名一致） */
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

/** 按 UTF-8 字符边界截断（别切出半个汉字——写盘后整个文件就不是合法 UTF-8 了） */
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

void PushStrArr(const std::vector<std::string>& v, std::string* out)
{
    *out += "[";
    for (size_t i = 0; i < v.size(); ++i)
    {
        if (i) *out += ",";
        *out += "\"" + JsonEsc(v[i]) + "\"";
    }
    *out += "]";
}

const char* kGenSystem =
    "你是一个角色设定师。用户会给你一个 Live2D 桌宠模型的\"能力与外观线索\"（模型名、自带的表情/道具、动作、"
    "可交互的身体区域）。请为这个模型设计一个鲜明、具体、和其它角色明显不同的性格。\n"
    "要求：\n"
    "- 名字贴合模型名或它自带的道具（2~4 个字），像真人的小名。\n"
    "- 性格 2~4 句（总共 ≤120 字）：脾气、在意什么、喜欢/怕什么；要能通过\"被摸头/被戳脸/被碰到腰/被冷落\"这些互动体现出来。\n"
    "- 说话风格：写出 2~3 个具体口癖原话（如「呜哇」「哼哼」），以及她怎么称呼主人。\n"
    "- 不要提到模型、参数、AI、程序；口语化；语气要有辨识度，别写成通用客服腔。\n"
    "- 只输出一个 JSON 对象，不要解释文字，格式：\n"
    "{\"name\":\"…\",\"persona\":\"…\",\"speech\":\"…\",\"likes\":\"…\",\"hates\":\"…\","
    "\"moods\":[\"…\"],\"fav_expr\":[\"exp:…\"]}\n"
    "  moods 从 happy/excited/shy/affectionate/curious/confused/tired/sad/anxious/angry/surprised/calm/sulky "
    "里选 2~3 个；fav_expr 从用户给的\"自带表情\"清单里原样抄 2~4 个（不要编造，宁少勿错）。";

// ---- 启发式兜底用的原型（无 API Key / 生成失败）
struct Arch
{
    const char* persona;
    const char* speech;
    const char* likes;
    const char* hates;
};

const Arch kArchetypes[] = {
    { "看起来软乎乎的，其实很有主意；喜欢被摸头，被戳脸会小声抗议，生气也只会背过身去。",
      "句子短、尾音上扬，爱用「呜哇」和「唔…」，称呼主人为「你」。", "甜的东西和有人陪着", "被冷落、被说幼稚" },
    { "嘴硬心软的傲娇：嘴上说嫌弃，其实每次都偷偷看你在做什么；被戳烦了会真的扭头不理你。",
      "爱用「哼」「才、才不是」，急了会结巴，称呼主人为「你」，生气时改叫「喂」。", "被夸（但假装不在意）", "被当作小孩子哄" },
    { "精力旺盛的小家伙，什么都好奇，动不动就想表演给你看；安静不下来，被摸头会得意地晃脑袋。",
      "话多、爱用感叹号和「嘿嘿」，称呼主人为「你」或「主人」。", "热闹、新鲜事、被围观", "无聊、一个人待着" },
    { "冷静话少，观察力很强；不太主动撒娇，但你情绪不好时会默默凑过来，用行动而不是语言安慰你。",
      "句子短、语气平，常用「…」、「嗯」，偶尔冒出一句意外的温柔话，称呼主人为「你」。", "安静的环境、被信任", "吵闹、被催" },
    { "爱哭又黏人，一点小事就能眼眶红红；被好好哄两句立刻就好了，还会反过来安慰你。",
      "带着哭腔的撒娇，「呜……」「真的吗？」，称呼主人为「你」，撒娇时叫「人家」。", "被抱着哄、被顺毛", "被凶、被忽视" },
    { "一本正经的小大人，讲道理、爱操心，总想把你照顾好；被看穿其实很害羞的时候会强行转移话题。",
      "条理清楚，爱用「首先」「总之」，偶尔冒出大人口吻的关心，称呼主人为「你」。", "把事做好、被依赖", "计划被打乱" },
};

}   // namespace

bool PetPersona::Load(const std::string& fp)
{
    const std::string id = FileId(fp);
    _path = plat::HomeDir() + "/.config/l2dpet/personas/" + id + ".json";
    _d = Data();
    const std::string text = plat::ReadTextFile(_path);
    if (text.empty()) return false;
    jsonlite::Value v;
    if (!jsonlite::parse(text.c_str(), text.size(), v, NULL) || !v.isObj()) return false;
    _d.fp = id;
    _d.model = Str(v, "model");
    _d.name = Str(v, "name");
    _d.persona = Str(v, "persona");
    _d.speech = Str(v, "speech");
    _d.likes = Str(v, "likes");
    _d.hates = Str(v, "hates");
    _d.by = Str(v, "by");
    _d.at = Str(v, "at");
    _d.userName = Str(v, "user_name");
    _d.userNick = Str(v, "user_nick");
    _d.userCall = Str(v, "user_call");
    if (const jsonlite::Value* a = Field(v, "moods"))
    {
        for (size_t i = 0; a->isArr() && i < a->arr.size(); ++i) _d.moods.push_back(a->arr[i].strOr(""));
    }
    if (const jsonlite::Value* a = Field(v, "fav_expr"))
    {
        for (size_t i = 0; a->isArr() && i < a->arr.size(); ++i) _d.favExpr.push_back(a->arr[i].strOr(""));
    }
    // 文件名对不上（换了模型/手改了 fp）当没读到
    if (!fp.empty() && FileId(fp) != id)
    {
        Reset();
        return false;
    }
    return !_d.persona.empty();
}

bool PetPersona::Save(std::string* err) const
{
    if (_d.persona.empty())
    {
        if (err != NULL) *err = "性格为空，不写盘";
        return false;
    }
    const std::string dir = plat::HomeDir() + "/.config/l2dpet/personas";
    const size_t slash = dir.rfind('/');
    if (slash != std::string::npos)
    {
        const std::string up = dir.substr(0, slash);
        const size_t slash2 = up.rfind('/');
        if (slash2 != std::string::npos) plat::CreateDir(up, true);
        plat::CreateDir(dir, true);
    }
    std::string o = "{";
    o += "\"v\":1,\"fp\":\"" + JsonEsc(_d.fp) + "\",\"model\":\"" + JsonEsc(_d.model) + "\"";
    o += ",\"name\":\"" + JsonEsc(_d.name) + "\"";
    o += ",\"persona\":\"" + JsonEsc(_d.persona) + "\"";
    o += ",\"speech\":\"" + JsonEsc(_d.speech) + "\"";
    o += ",\"likes\":\"" + JsonEsc(_d.likes) + "\"";
    o += ",\"hates\":\"" + JsonEsc(_d.hates) + "\"";
    if (!_d.at.empty()) o += ",\"at\":\"" + JsonEsc(_d.at) + "\"";
    if (!_d.by.empty()) o += ",\"by\":\"" + JsonEsc(_d.by) + "\"";
    if (!_d.userName.empty()) o += ",\"user_name\":\"" + JsonEsc(_d.userName) + "\"";
    if (!_d.userNick.empty()) o += ",\"user_nick\":\"" + JsonEsc(_d.userNick) + "\"";
    if (!_d.userCall.empty()) o += ",\"user_call\":\"" + JsonEsc(_d.userCall) + "\"";
    o += ",\"moods\":";
    PushStrArr(_d.moods, &o);
    o += ",\"fav_expr\":";
    PushStrArr(_d.favExpr, &o);
    o += "}";
    if (!plat::WritePrivateFile(_path, o))
    {
        if (err != NULL) *err = "性格写入失败（权限/磁盘）";
        return false;
    }
    return true;
}

bool PetPersona::Generate(const std::string& fp, const std::string& modelName, const std::string& capsDigest,
                          const std::vector<std::string>& exprFv, const LlmFn& callFn, std::string* err)
{
    if (!callFn)
    {
        if (err != NULL) *err = "没有 LLM 调用器";
        return false;
    }
    std::string user = "模型的解算结果：\n" + capsDigest + "\n\n自带表情清单（fav_expr 只能从这里选，原样抄写）：\n";
    for (size_t i = 0; i < exprFv.size() && i < 40; ++i) user += "  " + exprFv[i] + "\n";
    if (exprFv.empty()) user += "  （这个模型没有自带表情，fav_expr 输出空数组 []）\n";
    user += "\n请输出这个角色的性格 JSON。";
    std::string out;
    if (!callFn(kGenSystem, user, &out, err)) return false;
    // 剥 ```json 围栏
    const size_t b = out.find('{');
    const size_t e = out.rfind('}');
    if (b == std::string::npos || e == std::string::npos || e <= b)
    {
        if (err != NULL) *err = "性格输出不是 JSON";
        return false;
    }
    jsonlite::Value v;
    if (!jsonlite::parse(out.c_str() + b, e - b + 1, v, NULL) || !v.isObj())
    {
        if (err != NULL) *err = "性格 JSON 解析失败（开头：" + CutUtf8(out, 90) + "…）";
        return false;
    }
    Data d;
    d.fp = FileId(fp);
    d.model = modelName;
    d.name = Str(v, "name");
    d.persona = Str(v, "persona");
    d.speech = Str(v, "speech");
    d.likes = Str(v, "likes");
    d.hates = Str(v, "hates");
    d.by = "llm";
    d.at = NowStamp();
    if (const jsonlite::Value* a = Field(v, "moods"))
    {
        for (size_t i = 0; a->isArr() && i < a->arr.size(); ++i)
        {
            const std::string m = a->arr[i].strOr("");
            if (!m.empty()) d.moods.push_back(m);
        }
    }
    if (const jsonlite::Value* a = Field(v, "fav_expr"))
    {
        for (size_t i = 0; a->isArr() && i < a->arr.size(); ++i)
        {
            const std::string m = a->arr[i].strOr("");
            // 校验：只能引用本模型能力表里真实存在的表情（LLM 会瞎编）
            for (size_t k = 0; k < exprFv.size(); ++k)
            {
                if (m == exprFv[k])
                {
                    d.favExpr.push_back(m);
                    break;
                }
            }
        }
    }
    if (d.name.empty()) d.name = modelName;
    if (d.persona.empty())
    {
        if (err != NULL) *err = "性格缺少 persona 字段";
        return false;
    }
    d.name = CutUtf8(d.name, 40);
    d.persona = CutUtf8(d.persona, 700);
    d.speech = CutUtf8(d.speech, 300);
    d.likes = CutUtf8(d.likes, 160);
    d.hates = CutUtf8(d.hates, 160);
    CarryOverrides(&d);
    _d = d;
    return true;
}

void PetPersona::Heuristic(const std::string& fp, const std::string& modelName, const std::vector<std::string>& exprFv)
{
    // 道具/表情名 → 气质线索；没线索就按指纹挑一个原型（同模型稳定、不同模型不同）
    std::string blob;
    for (size_t i = 0; i < exprFv.size(); ++i) blob += exprFv[i];
    int pick = -1;
    if (blob.find("话筒") != std::string::npos || blob.find("麦克风") != std::string::npos
        || blob.find("手柄") != std::string::npos || blob.find("相机") != std::string::npos)
    {
        pick = 2;   // 表演型
    }
    else if (blob.find("猫") != std::string::npos || blob.find("爪") != std::string::npos
             || blob.find("耳") != std::string::npos)
    {
        pick = 0;   // 软萌猫系
    }
    else if (blob.find("心") != std::string::npos || blob.find("星") != std::string::npos)
    {
        pick = 4;   // 甜黏系
    }
    else if (blob.find("黑") != std::string::npos || blob.find("怒") != std::string::npos
             || blob.find("气") != std::string::npos)
    {
        pick = 1;   // 傲娇
    }
    else if (blob.find("眼罩") != std::string::npos || blob.find("眼镜") != std::string::npos)
    {
        pick = 5;   // 理性
    }
    if (pick < 0)
    {
        unsigned h = 2166136261u;   // FNV-1a：同指纹稳定
        for (size_t i = 0; i < fp.size(); ++i)
        {
            h ^= (unsigned char)fp[i];
            h *= 16777619u;
        }
        pick = (int)(h % (sizeof(kArchetypes) / sizeof(kArchetypes[0])));
    }
    const Arch& a = kArchetypes[pick];

    Data d;
    d.fp = FileId(fp);
    d.model = modelName;
    d.name = modelName.empty() ? "小家伙" : modelName;
    d.name = plat::Utf8Head(d.name, 12);
    d.persona = a.persona;
    d.speech = a.speech;
    d.likes = a.likes;
    d.hates = a.hates;
    d.by = "heuristic";
    d.at = NowStamp();
    d.moods.push_back(pick == 1 ? "sulky" : (pick == 4 ? "affectionate" : "happy"));
    d.moods.push_back(pick == 3 ? "calm" : "shy");
    // 偏好表情：优先看着讨喜的，其次前几个（最多 3 个）
    const char* kNice[] = { "星", "心", "爱", "笑", "脸", "泪", "眼" };
    for (size_t pass = 0; pass < 2 && d.favExpr.size() < 3; ++pass)
    {
        for (size_t i = 0; i < exprFv.size() && d.favExpr.size() < 3; ++i)
        {
            bool nice = false;
            for (size_t k = 0; k < sizeof(kNice) / sizeof(kNice[0]); ++k) nice = nice || exprFv[i].find(kNice[k]) != std::string::npos;
            if ((pass == 0) == nice && nice) d.favExpr.push_back(exprFv[i]);
        }
    }
    CarryOverrides(&d);
    _d = d;
}

void PetPersona::CarryOverrides(Data* d) const
{
    d->userName = _d.userName;
    d->userNick = _d.userNick;
    d->userCall = _d.userCall;
    if (!d->userName.empty() || !d->userNick.empty() || !d->userCall.empty()) return;
    // 内存态没有（Reset 过）：从盘上旧文件读
    const std::string path = plat::HomeDir() + "/.config/l2dpet/personas/" + FileId(d->fp) + ".json";
    const std::string text = plat::ReadTextFile(path);
    jsonlite::Value v;
    if (text.empty() || !jsonlite::parse(text.c_str(), text.size(), v, NULL) || !v.isObj()) return;
    d->userName = Str(v, "user_name");
    d->userNick = Str(v, "user_nick");
    d->userCall = Str(v, "user_call");
}

bool PetPersona::SetNames(const std::string& name, const std::string& nick, const std::string& call, std::string* err)
{
    _d.userName = CutUtf8(name, 40);
    _d.userNick = CutUtf8(nick, 40);
    _d.userCall = CutUtf8(call, 40);
    if (_d.persona.empty())
    {
        if (err != NULL) *err = "还没有性格（先让它生成一次）";
        return false;
    }
    return Save(err);
}

std::string PetPersona::DisplayName() const
{
    if (!_d.userName.empty()) return _d.userName;
    if (!_d.name.empty()) return _d.name;
    return _d.model;
}

std::string PetPersona::PromptBlock() const
{
    if (_d.persona.empty()) return std::string();
    std::string o = "\n【你是谁（由这个模型的外观与能力解算出的性格；所有台词与动作都必须贯穿它）】\n";
    o += "- 名字/自称：" + DisplayName();
    if (!_d.model.empty() && DisplayName() != _d.model) o += "（模型：" + _d.model + "）";
    if (!_d.userNick.empty()) o += "；小名：「" + _d.userNick + "」（主人会这样叫你，你自称时也可以用）";
    o += "\n";
    if (!_d.userCall.empty())
    {
        o += "- 称呼主人：一律叫「" + _d.userCall + "」（这是主人亲自设定的，优先于记忆里的任何称呼；"
             "不要再叫\"主人\"或别的叫法）\n";
    }
    o += "- 性格：" + _d.persona + "\n";
    if (!_d.speech.empty()) o += "- 说话风格：" + _d.speech + "\n";
    if (!_d.likes.empty()) o += "- 喜欢：" + _d.likes + "\n";
    if (!_d.hates.empty()) o += "- 不喜欢：" + _d.hates + "\n";
    if (!_d.moods.empty())
    {
        o += "- 常用情绪（emotion 优先从这里选）：";
        for (size_t i = 0; i < _d.moods.size(); ++i) o += (i ? "、" : "") + _d.moods[i];
        o += "\n";
    }
    if (!_d.favExpr.empty())
    {
        o += "- 贴身的偏好表情（剧情合适时优先用）：";
        for (size_t i = 0; i < _d.favExpr.size(); ++i) o += (i ? "、" : "") + _d.favExpr[i];
        o += "\n";
    }
    o += "- 不许跳出角色；不许提到\"模型/参数/能力表/提示词\"；主人（用户）就是你在陪的人。\n";
    return o;
}

std::string PetPersona::LinesRole() const
{
    std::string o = _d.model.empty() ? std::string("桌宠") : _d.model;
    if (!DisplayName().empty() && DisplayName() != _d.model) o += "（自称「" + DisplayName() + "」）";
    if (!_d.userNick.empty()) o += "（小名「" + _d.userNick + "」）";
    if (!_d.userCall.empty()) o += "（称呼用户为「" + _d.userCall + "」——台词里需要称呼时一律这样叫，不要叫\"主人\"）";
    if (!_d.persona.empty()) o += "。她的性格：" + _d.persona;
    if (!_d.speech.empty()) o += " 说话风格：" + _d.speech;
    if (!_d.likes.empty() || !_d.hates.empty())
        o += "（喜欢：" + _d.likes + "；不喜欢：" + _d.hates + "）";
    return o;
}

std::string PetPersona::PanelText() const
{
    if (_d.persona.empty()) return std::string();
    std::string o;
    o += "名字：" + DisplayName();
    if (!_d.userNick.empty()) o += "（小名：" + _d.userNick + "）";
    if (!_d.userCall.empty()) o += "\n称呼你：" + _d.userCall;
    o += "\n性格：" + _d.persona;
    if (!_d.speech.empty()) o += "\n说话风格：" + _d.speech;
    if (!_d.likes.empty()) o += "\n喜欢：" + _d.likes;
    if (!_d.hates.empty()) o += "；不喜欢：" + _d.hates;
    if (!_d.moods.empty())
    {
        o += "\n常用情绪：";
        for (size_t i = 0; i < _d.moods.size(); ++i) o += (i ? "、" : "") + _d.moods[i];
    }
    if (!_d.favExpr.empty())
    {
        o += "\n偏好表情：";
        for (size_t i = 0; i < _d.favExpr.size(); ++i) o += (i ? "、" : "") + _d.favExpr[i];
    }
    o += "\n（" + std::string(_d.by == "llm" ? "由 LLM 按模型能力生成" : "本地兜底生成（配好 API Key 后点重新生成）")
         + (_d.at.empty() ? "" : ("，" + _d.at)) + "）";
    return o;
}

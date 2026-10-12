/**
 * PetAgent.cpp — 见 PetAgent.h（pet_agent.py 的忠实移植；提示词与节制规则同文）
 * 分块组装中。
 */
#include "PetAgent.h"
#include "PetSpeech.h"
#include "PetVerbs.h"
#include "Platform.h"
#include "TokenBudget.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <ctime>
#include <sys/stat.h>

#include <algorithm>
#include <chrono>
#include <functional>
#include <set>
#include <utility>

namespace
{

/** 界面语言归一化：config.str.lang（zh/en/ja/auto）→ "zh" | "en" | "ja"
 *  （auto 与面板 i18n::Detect 同口径：按系统 locale 猜） */
static std::string LangResolve(const std::string& code)
{
    if (code == "zh" || code == "en" || code == "ja") return code;
    if (code != "auto") return "zh";
    const char* keys[] = { "LC_ALL", "LC_MESSAGES", "LANG" };
    for (int i = 0; i < 3; ++i)
    {
        const char* v = getenv(keys[i]);
        if (v == NULL || *v == 0 || std::string(v) == "C" || std::string(v) == "POSIX") continue;
        const std::string s = v;
        if (s.find("zh") != std::string::npos) return "zh";
        if (s.find("ja") != std::string::npos) return "ja";
        return "en";
    }
    return "zh";
}

/** 语言的中文名（提示词与日志里用） */
static std::string LangCnName(const std::string& l)
{
    return l == "ja" ? std::string("日语") : (l == "en" ? std::string("英语") : std::string("中文"));
}

static void ReplaceAll(std::string& s, const std::string& from, const std::string& to)
{
    if (from.empty()) return;
    for (size_t p = s.find(from); p != std::string::npos; p = s.find(from, p + to.size())) s.replace(p, from.size(), to);
}

/** 提示词里写死的中文措辞按目标语言改掉。
 *  为什么必须改：`speak（中文台词）` 这种指令会和"用日语写"的语言提示**打架**——
 *  实测 LLM 会听前面的（日语界面下冒出中文台词，用户就是这么发现的）。 */
static std::string LangFixup(const std::string& text, const std::string& lang)
{
    if (lang == "zh") return text;
    std::string o = text;
    const std::string cn = LangCnName(lang);
    ReplaceAll(o, "speak（中文台词）", "speak（" + cn + "台词）");
    ReplaceAll(o, "（中文台词）", "（" + cn + "台词）");
    ReplaceAll(o, "每句 ≤12 个字", lang == "ja" ? "每句 ≤18 字（日语）" : "每句 ≤12 个词左右（英语）");
    ReplaceAll(o, "中文，她的第一人称", cn + "，她的第一人称");
    return o;
}

// 单调墙钟秒数。注意不能用 clock()：那是进程 CPU 时间——渲染 8 线程时跑得比墙钟快几倍，窗口隐藏时几乎不走
double NowSec()
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

std::string WallTime()
{
    time_t t = time(NULL);
    struct tm lt;
#ifdef _WIN32
    localtime_s(&lt, &t);
#else
    localtime_r(&t, &lt);
#endif
    char buf[32];
    strftime(buf, sizeof(buf), "%H:%M:%S", &lt);
    return buf;
}

void Log(const std::string& msg)
{
    printf("%s [agent] %s\n", WallTime().c_str(), msg.c_str());
    fflush(stdout);
}

void NapMs(long ms)
{
    plat::SleepMs((int)ms);   // Linux nanosleep / Windows Sleep
}

bool StartsWith(const std::string& s, const char* p)
{
    const size_t n = strlen(p);
    return s.size() >= n && s.compare(0, n, p) == 0;
}

bool Contains(const std::string& s, const char* n)
{
    return s.find(n) != std::string::npos;
}

size_t Utf8Len(const std::string& s)
{
    size_t n = 0;
    for (size_t i = 0; i < s.size();)
    {
        const unsigned char c = (unsigned char)s[i];
        i += c < 0x80 ? 1 : ((c & 0xE0) == 0xC0 ? 2 : ((c & 0xF0) == 0xE0 ? 3 : 4));
        ++n;
    }
    return n;
}

std::string JsonEscape(const std::string& s)
{
    // 先过一遍 UTF-8 合法性：截断的多字节序列/孤立续字节丢弃（md 按字节截断会产生）
    std::string clean;
    clean.reserve(s.size());
    for (size_t i = 0; i < s.size();)
    {
        const unsigned char c = (unsigned char)s[i];
        size_t need = c < 0x80 ? 1 : ((c & 0xE0) == 0xC0 ? 2 : ((c & 0xF0) == 0xE0 ? 3 : ((c & 0xF8) == 0xF0 ? 4 : 0)));
        if (need == 0 || i + need > s.size())
        {
            ++i;   // 孤立续字节 / 截断的头字节：丢弃
            continue;
        }
        bool valid = true;
        for (size_t j = 1; j < need; ++j)
        {
            if (((unsigned char)s[i + j] & 0xC0) != 0x80) { valid = false; break; }
        }
        if (!valid)
        {
            ++i;
            continue;
        }
        clean.append(s, i, need);
        i += need;
    }
    std::string out;
    for (size_t i = 0; i < clean.size(); ++i)
    {
        const unsigned char c = (unsigned char)clean[i];
        if (c == '"') out += "\\\"";
        else if (c == '\\') out += "\\\\";
        else if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else if (c == '\t') out += "\\t";
        else if (c < 0x20) { char b[8]; snprintf(b, sizeof(b), "\\u%04x", c); out += b; }
        else out += (char)c;
    }
    return out;
}

/** jsonlite::Value → 紧凑 JSON 文本（解析器的序列化补位） */
std::string ValueToJson(const jsonlite::Value& v)
{
    if (v.isStr()) return "\"" + JsonEscape(v.str) + "\"";
    if (v.isNum())
    {
        char b[40];
        const double d = v.num;
        if (d == floor(d) && fabs(d) < 1e15) snprintf(b, sizeof(b), "%lld", (long long)d);
        else snprintf(b, sizeof(b), "%.6g", d);
        return b;
    }
    if (v.isBool()) return v.num != 0 ? "true" : "false";
    if (v.isArr())
    {
        std::string out = "[";
        for (size_t i = 0; i < v.arr.size(); ++i)
        {
            if (i) out += ",";
            out += ValueToJson(v.arr[i]);
        }
        return out + "]";
    }
    if (v.isObj())
    {
        std::string out = "{";
        for (size_t i = 0; i < v.obj.size(); ++i)
        {
            if (i) out += ",";
            out += "\"" + JsonEscape(v.obj[i].first) + "\":" + ValueToJson(v.obj[i].second);
        }
        return out + "}";
    }
    return "null";
}

const jsonlite::Value* ObjGet(const jsonlite::Value* obj, const std::string& key)
{
    if (obj == NULL || !obj->isObj()) return NULL;
    for (size_t i = 0; i < obj->obj.size(); ++i)
    {
        if (obj->obj[i].first == key) return &obj->obj[i].second;
    }
    return NULL;
}

jsonlite::Value* ObjGetMut(jsonlite::Value* obj, const std::string& key)
{
    if (obj == NULL || !obj->isObj()) return NULL;
    for (size_t i = 0; i < obj->obj.size(); ++i)
    {
        if (obj->obj[i].first == key) return &obj->obj[i].second;
    }
    return NULL;
}

void ObjSet(jsonlite::Value* obj, const std::string& key, const jsonlite::Value& v)
{
    jsonlite::Value* cur = ObjGetMut(obj, key);
    if (cur != NULL) *cur = v;
    else obj->obj.push_back(std::make_pair(key, v));
}

void ObjDel(jsonlite::Value* obj, const char* key)
{
    if (obj == NULL || !obj->isObj()) return;
    for (size_t i = 0; i < obj->obj.size(); ++i)
    {
        if (obj->obj[i].first == key)
        {
            obj->obj.erase(obj->obj.begin() + i);
            return;
        }
    }
}

std::string GetStr(const jsonlite::Value* obj, const char* key, const std::string& def = "")
{
    const jsonlite::Value* v = ObjGet(obj, key);
    return v != NULL && v->isStr() ? v->str : def;
}

double GetNum(const jsonlite::Value* obj, const char* key, double def = 0)
{
    const jsonlite::Value* v = ObjGet(obj, key);
    if (v == NULL) return def;
    if (v->isBool()) return v->b ? 1 : 0;   // jsonlite 的 bool 存 b 字段，numOr 拿不到
    return v->numOr(def);
}

std::string TrimCopy(const std::string& s)
{
    size_t a = 0, b = s.size();
    while (a < b && (unsigned char)s[a] <= ' ') ++a;
    while (b > a && (unsigned char)s[b - 1] <= ' ') --b;
    return s.substr(a, b - a);
}

std::vector<std::string> SplitLines(const std::string& text)
{
    std::vector<std::string> out;
    std::string cur;
    for (size_t i = 0; i < text.size(); ++i)
    {
        if (text[i] == '\n') { out.push_back(cur); cur.clear(); }
        else cur += text[i];
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

/** 剥掉 LLM 输出外面包的 ```json / ``` 围栏行 */
std::string StripFences(const std::string& raw)
{
    std::vector<std::string> lines = SplitLines(raw);
    std::string out;
    for (size_t i = 0; i < lines.size(); ++i)
    {
        const std::string t = TrimCopy(lines[i]);
        if (t == "```" || t == "```json") continue;
        if (!out.empty()) out += "\n";
        out += lines[i];
    }
    return TrimCopy(out);
}

std::string FmtGap(double sec)
{
    char b[32];
    if (sec >= 86400) snprintf(b, sizeof(b), "%.0f 天", floor(sec / 86400));
    else if (sec >= 3600) snprintf(b, sizeof(b), "%.0f 小时", floor(sec / 3600));
    else snprintf(b, sizeof(b), "%.0f 分钟", floor(sec / 60));
    return b;
}

std::string HomePath(const char* rel)
{
    const char* home = getenv("HOME");
#ifdef _WIN32
    if (home == NULL) home = getenv("USERPROFILE");
#endif
    return std::string(home != NULL ? home : "/tmp") + rel;
}

std::string MoodStateZh(const std::string& s)
{
    if (s == "drowsy") return "犯困";
    if (s == "asleep") return "睡着了";
    if (s == "sulk") return "在生闷气";
    return "醒着";
}

// 候选题用编号关联选项；兼容旧模型只输出原题（忽略标点与口癖前缀）。
std::string QuestionTextKey(const std::string& text)
{
    std::string out;
    const char* punctuation[] = {"，", "。", "？", "！", "…", "、", "“", "”", "：", "　"};
    for (size_t i = 0; i < text.size();)
    {
        const unsigned char c = (unsigned char)text[i];
        const size_t n = c < 0x80 ? 1 : ((c & 0xe0) == 0xc0 ? 2 : ((c & 0xf0) == 0xe0 ? 3 : 4));
        if (i + n > text.size()) break;
        bool drop = n == 1 && (c <= ' ' || std::strchr(",.!?:~", c) != NULL);
        for (const char* mark : punctuation) drop = drop || text.compare(i, n, mark) == 0;
        if (!drop) out.append(text, i, n);
        i += n;
    }
    return out;
}

void RestoreQuestionOptions(jsonlite::Value* seq, const std::vector<const PetQuestions::Item*>& candidates,
                            const PetQuestions* questions = NULL)
{
    jsonlite::Value* steps = ObjGetMut(seq, "steps");
    if (steps == NULL || !steps->isArr()) return;
    for (jsonlite::Value& step : steps->arr)
    {
        const jsonlite::Value id = step.get("question_id");
        ObjDel(&step, "question_id"); // 仅供 agent 使用，不发给播放器
        const std::string speech = GetStr(&step, "speak");
        if (speech.empty()) continue;
        const PetQuestions::Item* selected = NULL;
        if (id.isStr())
        {
            for (const PetQuestions::Item* item : candidates)
                if (!item->id.empty() && item->id == id.str) selected = item;
        }
        else if (id.isNum() && id.num >= 1 && id.num <= candidates.size() && std::floor(id.num) == id.num)
            selected = candidates[(size_t)id.num - 1];
        else
        {
            const std::string key = QuestionTextKey(speech);
            for (const PetQuestions::Item* item : candidates)
            {
                const std::string q = QuestionTextKey(item->q);
                if (!q.empty() && key.find(q) != std::string::npos)
                {
                    if (selected != NULL) { selected = NULL; break; } // 多题混在一句里不能猜选项
                    selected = item;
                }
            }
        }
        const jsonlite::Value& opts = step.get("options");
        if (questions != NULL)
        {
            const std::string speechKey = QuestionTextKey(speech);
            const auto ends = [&](const char* tail) {
                const std::string value = tail;
                return speechKey.size() >= value.size()
                    && speechKey.compare(speechKey.size() - value.size(), value.size(), value) == 0;
            };
            const bool bareQuestion = ends("什么") || ends("什么呀") || ends("什么呢")
                || ends("干嘛") || ends("干嘛呀") || ends("吗") || ends("么");
            const bool questionLike = opts.isArr() || id.isStr() || id.isNum()
                || bareQuestion || speech.find("?") != std::string::npos || speech.find("？") != std::string::npos;
            if ((questionLike && questions->AlreadyAsked(speech))
                || (questionLike && (selected == NULL || selected->asked > 0 || !selected->answer.empty())))
            {
                ObjDel(&step, "speak");
                ObjDel(&step, "options");
                continue; // Keep unrelated animations, never show an untracked or repeated question.
            }
            if (selected != NULL)
            {
                jsonlite::Value canonical;
                canonical.setType(jsonlite::Value::STR);
                canonical.str = selected->q;
                ObjSet(&step, "speak", canonical); // Record and answer the same canonical question, despite paraphrasing.
            }
        }
        size_t valid = 0;
        for (size_t i = 0; opts.isArr() && i < opts.arr.size(); ++i)
        {
            const jsonlite::Value& o = opts.arr[i];
            if (!(o.isObj() ? std::string(o.get("label").strOr("")) : std::string(o.strOr(""))).empty()) ++valid;
        }
        if (selected == NULL || valid >= 2) continue; // 保留模型写好的选项与 do 动作
        jsonlite::Value restored;
        restored.setType(jsonlite::Value::ARR);
        for (const std::string& label : selected->options)
        {
            jsonlite::Value value;
            value.setType(jsonlite::Value::STR);
            value.str = label;
            restored.arr.push_back(value);
        }
        ObjSet(&step, "options", restored);
    }
}

// 节制（定义在后文第二个匿名命名空间；Run 在前面用，先声明）
bool ThrottleSeq(jsonlite::Value* seq, double now, double lastQuestionAt, bool allowQuestions, std::string* cutWhat);
void ClampRespond(jsonlite::Value* seq);

// ---- 节制常量（与 pet_agent.py 同值）
const int kMaxSpeakPerSeg = 2;
const double kQuestionCooldown = 300.0;
const double kQuestionWait = 25.0;
const double kQuietAfterTouch = 2.0;
const double kRespondCooldown = 10.0;
const double kRespondMaxDur = 6.0;
const int kMaxExprPerSeg = 2;
const int kMemoryEvery = 6;
const double kMemoryIdle = 1200.0;
/** 后台 LLM 活（台词库重写 / 题库补货 / 笑话补货 / 记忆压缩）要等主人这边安静这么久才开工：
 *  它们单次 10 秒上下，排在"刚点完她的回应"前面就会把回应堵慢（比 kRespondCooldown 略宽，
 *  把一整轮回应窗口让过去——2026-10-12 那条 11.7 秒的台词库重写就是这么堵的） */
const double kBackgroundQuiet = 12.0;
const double kLinesRefresh = 900.0;
/** 台词库生成失败后的退避：一次不成就等这么久再试（太短会把 agent 堵在重试里，太长又赶不上"主人回答"） */
const double kLinesRetry = 120.0;
/** 长生成的单次请求上限（性格 / 题库 / 笑话 / 台词库 / 记忆整理）：这些要写很长的 JSON，
 *  思考型模型（mimo、Claude 扩展思考）先想很久才吐答案，60 秒会假超时——日志里的
 *  "网络错误：Timeout was reached" 就是这么来的 */
const int kLongGenTimeoutSec = 180;

/** 长生成用的接入参数：把单次超时放宽到 kLongGenTimeoutSec（性格/题库/笑话/台词库/记忆整理） */
l2d::LlmSettings LongGenCfg(const l2d::LlmSettings& cfg)
{
    l2d::LlmSettings out = cfg;
    out.timeoutSec = kLongGenTimeoutSec;
    return out;
}
const double kIdlePeriodDefault = 60.0;

// ---- 提示词（与 pet_agent.py 同文）
const char* kSystemPromptHead =
    "你在控制一个 Live2D 桌宠。下面是它的能力表（JSON），每项有 name（含义）和 fv（引用键）。\n";
const char* kSystemPromptTail =
    "\n规则：\n"
    "- 只输出一个 JSON：{\"cmd\":\"sequence\",\"model\":\"{model}\",\"mode\":\"queue\",\"dur\":<秒>,\"steps\":[…]}，不要其它文字。\n"
    "- step 可用的键：at（秒）、speak（中文台词）、options（可点击选项数组）、question_id（候选题号，数字）、emotion（情绪，见下）、action（act:…）、expression（exp:… 或 null）、motion（mtn:…）、\n"
    "  look（viewer/left/right/up/down）、pose（{\"set\":[pose:…],\"hands\":[hand:…],\"joints\":{joint:…:值},\"parts\":{part:…:值},\"t\":秒}）。\n"
    "- 只能引用能力表里出现的 fv；关节值必须在 range 内；parts 的值是相对自然姿态的增量。\n"
    "- skeleton 为空时不要写 pose.set / joints，用表情、头部、视线、说话和预置动作来表达。\n"
    "- 你的台词（speak 的文字）会显示成角色头顶的**气泡**。**少说话**：每段最多 1~2 句，大多数步骤只做\n"
    "  动作/表情/视线，不要每步都说话。偶尔（很少）可以向用户提问：在那一步加\n"
    "  \"options\":[\"选项一\",\"选项二\"]（最多 3 个、每个 ≤6 字），用户点选后你会收到\n"
    "  {\"event\":\"choice\",\"label\":\"…\"} 事件，据此继续剧情（别再重复问过的问题）。提问之后的同一段里不要再说别的话。\n"
    "  **选项可以带上她的能力**：写成 {\"label\":\"一起打游戏\",\"do\":{\"expression\":\"exp:…\",\"action\":\"act:…\",\"emotion\":\"excited\",\"speak\":\"好耶！\"}}，\n"
    "  主人一点，do 里的动作/表情/情绪/一句短台词会**立刻**在她身上演出来（不等你下一次回复），然后你收到的 choice 事件里\n"
    "  会有 did 字段告诉你已经演过什么——接着往下演，别重复同一个动作。do 里只能用能力表里的 fv（和步骤同样的键：\n"
    "  action/expression/motion/emotion/look/speak；不要 options）。**让选项和她能做的事挂钩**：模型有手柄表情就可以有\n"
    "  \"一起打游戏\"、有猫猫手就可以有\"喵一个\"、有爱心就可以有\"比个心\"……不带能力的普通回答仍然写成字符串。\n"
    "  提问要有**变化**：称呼、作息、吃、工作学习、爱好、心情、家人、回忆、打算、和你的关系、你自己的小请求、\n"
    "  随口闲聊…… 别总问\"在干什么/在忙吗\"这类笼统的；闲时段的用户消息里会给你**候选题**（来自你的问题库），\n"
    "  优先从中挑一个最贴合气氛的；问过的（含换种说法）不要再问。\n"
    "- 别的互动（右侧菜单里主人可以点）：主人点\"讲个笑话\"你会收到 joke 事件、点\"猜拳\"会收到 game 事件、\n"
    "  点\"聊天气\"时根据提供的联网查询结果回应。笑话和猜拳的**具体台词与判定由桌宠本地完成**（life.joke / life.rps.*），\n"
    "  你收到事件后只需要**接着玩**：顺着刚才的事说一句（别重复它已经说过的设定），也可以顺势调侃两句；\n"
    "  你自己在闲时也可以偶尔讲个冷笑话（用 speak 直接说，1 句就够）或邀请主人玩一局猜拳。\n"
    "- 你会实时收到用户和角色的身体交互事件：click（点击某区域，n=连点次数；手=牵手、脸=捏脸、其余=戳）、\n"
    "  rub（在某区域来回拖动；头、手=摸，其余=揉）、hover（鼠标停在区域上）。说话时用这些动词（牵/捏/戳、摸/揉）。区域见能力表 zones（head 头部/face 脸部/belly 肚子/\n"
    "  hand.l 左手…，左右按屏幕方位）。桌宠本地已经对每次点/搓**立即**做了看向+动作+一句短台词，\n"
    "  所以你不用逐次复述，而是对\"这一阵互动\"做整体回应。\n"
    "- 角色有自己的内在状态（用户消息里会给出）：awake 醒着 / drowsy 犯困 / sulk 生闷气（扭头不理人）/\n"
    "  asleep 睡着；affection 亲密度 0~1（越高越黏人、越放得开），annoy 恼火，lonely 寂寞。\n"
    "  你写的内容必须和当前状态一致：生闷气时就别突然热情（可以傲娇、嘴硬、慢慢软化），亲密度低时客气些、\n"
    "  高时撒娇。桌宠本地会自己犯困、睡觉、生气、主动找人，你不用安排这些，只负责\"说什么、怎么演\"。\n"
    "- 段落分两种（用户消息里会说明是哪种）：\n"
    "  · 【回应】用户刚和你互动完：dur 3~5 秒、2~3 步，第一步（at=0）就说一句针对部位和轻重的话，\n"
    "    配 1 个贴合的动作或表情；不要加 look_around/跳/欢呼之类无关填充动作。\n"
    "  · 【闲时】没人理你：8~15 秒的小日常，动作和表情为主，可以没有台词。\n"
    "- 情绪优先用 emotion 键：{\"emotion\":\"happy\"} 或 {\"emotion\":{\"name\":\"shy\",\"intensity\":0.8}}，可选\n"
    "  happy/excited/shy/affectionate/curious/confused/tired/sad/anxious/angry/surprised/calm/sulky。\n"
    "  它会让嘴角、眉眼、头姿、脸红连续地变过去并自然回落（不会硬切），强烈时还会自动套用模型自带的对应表情；\n"
    "  所以大多数时候不用写 expression。expression 只在需要特定造型时用，每段最多换 2 次；\n"
    "  眼罩、手柄、麦克风、猫猫手、星星等道具/装扮类表情除非剧情明确需要否则不要用。\n"
    "- 最后一步用 {\"pose\":{\"set\":[]}} 收回姿势，并把 expression 设为 null 复原。\n"
    "- 台词里**不要出现任何日期、时间或时间戳**（如 10-10、02:09、10月10日、今天几号）；记忆里的时间只是记录用的，\n"
    "  不是名字也不是称呼。称呼主人只用记忆里明确写出的名字/称呼，没有就叫\"你\"。";

// 台词库提示词（三段拼接：角色名 / 区域清单 / 记忆块）
const char* kLinesPromptA =
    "为一个 Live2D 桌宠写\"被触碰时的即时台词库\"。角色：";
const char* kLinesPromptB =
    "。\n用户可以点击（click：手=牵、脸=捏、其余=戳）或来回拖动（rub：头和手=摸、其余=揉）她身体的这些部位：";
const char* kLinesPromptC =
    "。\n输出一个 JSON：{\"lines\":{\"<部位键>.click\":[3 句],\"<部位键>.rub\":[3 句],…,\"angry\":[4 句]}}\n"
    "- 部位键用上面括号里的英文键；每个部位都要有 .click 和 .rub。angry 是被同一处连戳好几次时说的。\n"
    "- 另外还要这些生活场景键（各 3 句）：life.lonely（没人理、主动找用户搭话）、life.welcome（用户离开很久回来）、\n"
    "  life.drowsy（犯困打哈欠）、life.wake（睡饱醒来）、life.woken（睡着被戳醒）、life.sulk_start（被戳烦了开始生气）、\n"
    "  life.sulk（生闷气时又被戳）、life.forgive（被摸头哄好）、life.enjoy（被持续摸头很享受）、life.sleep_talk（梦话）。\n"
    "- 再加这些「玩」的（1~2 句短句，符合她的性格与口癖）：\n"
    "  life.joke（讲笑话前的铺垫不用写，直接是笑话/段子的**正文**，共 6 句，每句 ≤45 字，要真的好笑或可爱，\n"
    "    可以自嘲、可以关于小动物/天气/吃的，别用网络烂梗、别用表情符号）；\n"
    "  life.rps.ask（邀请猜拳，3 句）、life.rps.win（她赢了，得意但不刻薄，3 句）、\n"
    "  life.rps.lose（她输了，不服气/撒娇，3 句）、life.rps.draw（平局，3 句）。\n"
    "- 每句 ≤12 个字，口语、俏皮、有反应感，符合部位与动作（摸头舒服、捏脸害羞、牵手开心、揉肚子怕痒、戳胸部/胯部会生气、戳脚心怕痒…）；\n"
    "  所有台词都要符合她的性格与口癖（见开头角色描述），换个部位语气要有明显变化；\n"
    "  同一部位的几句语气也要有变化，不要用表情符号；台词里不要出现日期、时间或时间戳（如 10-10、02:09），\n"
    "  记忆里的时间只是记录，不是称呼。只输出 JSON。";

}   // namespace

PetAgent::~PetAgent()
{
    Stop();
}

void PetAgent::Start()
{
    if (_run.exchange(true)) return;
    _th = std::thread(&PetAgent::Run, this);
}

void PetAgent::Stop()
{
    if (!_run.exchange(false)) return;
    l2d::LlmCancelAll();   // 打断进行中的 LLM 请求（否则退出要等它最多 75s，窗口一直挂着）
    {
        std::lock_guard<std::mutex> lk(_evMu);
        _events.clear();
    }
    if (_th.joinable()) _th.join();
    {
        std::lock_guard<std::mutex> lk(_rpcMu);
        for (size_t i = 0; i < _pending.size(); ++i)
        {
            std::lock_guard<std::mutex> g(_pending[i]->mu);
            _pending[i]->abandoned = true;
            _pending[i]->cv.notify_all();
        }
        _pending.clear();
    }
}

void PetAgent::PushEvent(const std::string& jsonLine)
{
    std::lock_guard<std::mutex> lk(_evMu);
    if (_events.size() > 512) _events.erase(_events.begin());
    _events.push_back(jsonLine);
}

bool PetAgent::PopPending(RpcPtr* out)
{
    std::lock_guard<std::mutex> lk(_rpcMu);
    if (_pending.empty()) return false;
    *out = _pending.front();
    _pending.pop_front();
    return true;
}

void PetAgent::FinishPending(const RpcPtr& r)
{
    std::lock_guard<std::mutex> lk(r->mu);
    r->done = true;
    r->cv.notify_all();
}

std::string PetAgent::Rpc(const std::string& line)
{
    RpcPtr r = std::make_shared<RpcCall>();
    r->line = line;
    {
        std::lock_guard<std::mutex> lk(_rpcMu);
        _pending.push_back(r);
    }
    std::unique_lock<std::mutex> lk(r->mu);
    while (!r->done && !r->abandoned && _run.load())
    {
        r->cv.wait_for(lk, std::chrono::milliseconds(100));
    }
    return r->reply;
}

// 题库该不该重来（定义在下面 OpenQuestions 附近；Run 里也要用，故先声明）
static bool QuestionsNeedRefresh(const PetQuestions& q, int tokenTier = 1);
// 笑话补货最小间隔（定义在下面 OpenJokes 附近；Run 里也要用）
static double JokeGapSec();

void PetAgent::Run()
{
    _weatherCadence.Reset(NowSec());
    srand((unsigned)time(NULL));
    if (!LoadCaps())
    {
        Log("query 能力表失败，内置 agent 退出");
        _run.store(false);
        return;
    }
    // 先读状态与配置（语言、心情）再做任何生成：以前放在台词库生成之后，导致界面是日语/英语时，
    // 每次启动生成的即时台词库仍是中文（_cfgLang 还是默认 zh），语言过滤也按 zh 放行
    // 桌宠要等第一帧画出来（+1.5s）才读配置，所以这里的 status 一开始还没有 str 段——桌宠读好后
    // 回执里一定会带上 "str"（哪怕空对象），这就是"配置读好了"的信号。重的模型加载慢，等宽点（8s）
    for (int tries = 0; tries < 40 && _run.load(); ++tries)
    {
        jsonlite::Value st;
        bool haveStr = false;
        if (JsonRpcOk(Rpc("{\"cmd\":\"status\"}"), &st))
        {
            const jsonlite::Value* mood = ObjGet(&st, "mood");
            const jsonlite::Value* cfg = ObjGet(&st, "config");
            if (mood != NULL) ApplyMood(mood);
            if (cfg != NULL) ApplyConfig(cfg);
            haveStr = cfg != NULL && cfg->get("str").isObj();
        }
        if (haveStr) break;
        if (tries == 39) Log("等了 8 秒还没读到配置（桌宠还在加载模型？）——先用默认语言，收到 config 事件会自动对齐");
        NapMs(200);
    }
    _weather.Load(plat::HomeDir() + "/.config/l2dpet/weather-search.json");
    SpeechLang() = LangResolve(_cfgLang);
    Log("台词语言：" + LangCnName(LangResolve(_cfgLang)));
    OpenMemory();
    OpenPersona(false);   // 每个模型一份性格（缓存命中直接用；首次让 LLM 按能力解算）
    OpenQuestions(false); // 每个模型一份"想了解主人"的问题库（多题材，防每次只问在干什么）
    OpenJokes(false);     // 每个模型一份笑话池（只有"没讲过的不够"才补货）
    // 盘上的缓存可能是**别的语言**生成的：语言在桌宠没跑的时候被改过（手改配置）、导入别人的备份、
    // 或这个模型上次是在旧语言下用的（切语言只重写了当时那个模型的文件）。校验内容语言，不符就重生成；
    // 重生成也拿不到（没 Key/网络）就把旧语言的清掉——宁可这几轮不提问，也别拿中文题库说日语
    if (QuestionsStaleLang())
    {
        Log("题库缓存不是" + LangCnName(LangResolve(_cfgLang)) + "：重新生成");
        OpenQuestions(true);
        if (QuestionsStaleLang())
        {
            _questions.Reset();
            Log("题库重生成没成功：先把旧语言的清掉（免得说错语言），配好 Key 后自动生成");
        }
    }
    if (PersonaStaleLang())
    {
        Log("性格缓存不是" + LangCnName(LangResolve(_cfgLang)) + "：重新生成");
        OpenPersona(true);   // 失败会退回本地兜底版（by=heuristic，语言中性），不再触发本检查
    }
    if (_mem != NULL && _mem->PendingWeight() > 0)
    {
        Consolidate("上次没整理完", false);
    }
    Announce();
    PushLines("启动");
    PushMemory();
    // 久别重逢：开场第一段"叙旧"
    std::string reunion;
    if (_mem != NULL)
    {
        const double g = _mem->Gap();
        if (g < 0)
        {
            reunion = "【重逢】这是你第一次见到主人：简短打个招呼、介绍一下自己（一两句）。";
        }
        else if (g > 1800)
        {
            reunion = "【重逢】主人离开了约 " + FmtGap(g)
                      + " 后回来了：用一句话打招呼；如果记忆里有合适的事（上次的约定、让你开心或生气的事、主人的习惯），"
                        "自然地提一句，不要列举、不要像念记录。";
        }
    }
    Log(std::string("内置 agent 已启动（模型 ") + _caps.modelName + "）");
    // 启动后 1 分钟内不提问（每次重启都被问一句会烦；之后按题库正常节奏走）
    if (_lastQuestionAt < 0.0)
    {
        _lastQuestionAt = NowSec() + 60.0 - kQuestionCooldown;
    }
    while (_run.load())
    {
        SpeechLang() = LangResolve(_cfgLang);   // 生成台词/笑话/题库时按当前语言过滤（见 PetSpeech.h）
        // 睡着就暂停一切 LLM 调用（记忆归纳/压缩、台词库、题库、笑话补货、对话段全停，只留本地心跳）。
        // 恢复条件只有一个：主人点了她（click 事件，或被戳醒）。自己睡饱醒来仍暂停——只用本地台词库
        if (_moodState == "asleep" && !_llmPaused) SetLlmPaused(true, "她睡着了");
        const bool asleep = _llmPaused;
        if (_mem != NULL)
        {
            _mem->Heartbeat();
            if (!asleep && NowSec() - _memLast >= kMemoryIdle && (_mem->PendingWeight() > 0 || !_mem->SessionTouchEmpty()))
            {
                Consolidate("定时", false);
            }
        }
        // 后台的 LLM 活（台词库重写、题库/笑话补货、记忆压缩）让路给主人这边的事：有攒着的身体互动、
        // 还在等主人选选项、或距上次被碰不足一轮回应冷却，就先不做
        const double backNow = NowSec();
        const bool backgroundOk = _backlog[0].empty() && _backlog[1].empty() && backNow >= _awaitingUntil
                                  && backNow - _lastTouchAt >= kBackgroundQuiet;
        if (!asleep && backgroundOk && _linesDirty && backNow >= _linesRetryAt
            && (_linesUrgent || backNow - _linesLast >= kLinesRefresh) && backNow - _lastLlmAt >= 3.0)
        {
            _linesUrgent = false;
            PushLines(_linesDirtyReason.empty() ? "记忆更新" : _linesDirtyReason);
            _linesDirtyReason.clear();
        }
        // 问题库定期体检：答够几题 / 没新鲜题 / 太旧 → 带着"已知"重新生成一批（新题材、不重复）
        if (!asleep && backgroundOk)
        {
            static double qCheckAt = NowSec();   // 从启动时算起（启动时已经各做过一次）
            if (NowSec() - qCheckAt >= 300.0 && !_questions.Empty())
            {
                qCheckAt = NowSec();
                if (QuestionsNeedRefresh(_questions, _cfgTokenTier) && _questions.Stale(300.0)) OpenQuestions(false);
            }
            // 笑话池补货：没讲过的少于 2 条、且距上次生成够久（L2D_JOKE_GAP_SEC 可覆盖，测试用）
            static double jCheckAt = NowSec();   // 从启动时算起（启动时已经各做过一次）
            if (NowSec() - jCheckAt >= 300.0)
            {
                jCheckAt = NowSec();
                if (_jokes.NeedMore(JokeGapSec()) && NowSec() - _jokesLastGenAt >= 120.0)
                {
                    _jokesLastGenAt = NowSec();
                    OpenJokes(false);
                }
            }
            // 记忆压缩：每天一次（醒着才跑；压缩后立即把记忆本报给面板）
            static double cCheckAt = NowSec();   // 从启动时算起（启动时已经各做过一次）
            if (NowSec() - cCheckAt >= 600.0 && _mem != NULL)
            {
                cCheckAt = NowSec();
                if (_mem->NeedsCompress(24.0 * 3600.0) && !l2d::LlmLoadSettings().key.empty())
                {
                    CompressMemory("每天自动");
                }
            }
        }
        if (!asleep && _cfgWeather) RefreshWeather();
        std::string interrupt = reunion;
        reunion.clear();
        std::vector<std::string> ambient;
        std::vector<std::string> pending;
        {
            std::lock_guard<std::mutex> lk(_evMu);
            pending.swap(_events);
            _events.clear();
        }
        for (size_t i = 0; i < pending.size(); ++i)
        {
            jsonlite::Value e;
            if (!jsonlite::parse(pending[i].c_str(), pending[i].size(), e, NULL) || !e.isObj()) continue;
            const std::string ev = GetStr(&e, "event");
            if (ev.empty() || ev == "done" || StartsWith(ev.c_str(), "ui_")) continue;
            if (ev == "llm_config")
            {
                const jsonlite::Value* info = ObjGet(&e, "llm");
                const jsonlite::Value* keySet = ObjGet(info, "key_set");
                const bool hasKey = keySet != NULL
                    && ((keySet->isBool() && keySet->b) || keySet->numOr(0) != 0
                        || (keySet->isStr() && !keySet->str.empty()));
                const std::string mdl = info != NULL ? std::string(info->get("model").strOr("默认")) : std::string("默认");
                Log(std::string("接入设置已更新：key=") + (hasKey ? "已设" : "未设") + " model=" + mdl);
                Announce();
                if (!_linesOk) PushLines("设置更新");
                continue;
            }
            if (ev == "config")
            {
                const jsonlite::Value* cfg = ObjGet(&e, "config");
                if (cfg != NULL)
                {
                    const std::string before = LangResolve(_cfgLang);
                    ApplyConfig(cfg);
                    // 语言跟当前的**不一样**（启动那次没读到配置、后来才对齐；或外部工具改的配置）：
                    // 按"切语言"处理，把存量内容一起换成新语言，别只换新生成的那部分
                    if (LangResolve(_cfgLang) != before)
                    {
                        _deferLangRewrite = true;
                        _pendingLang = LangResolve(_cfgLang);
                        ResetConversation("配置里的语言变了");
                        Log("界面语言 → " + LangCnName(_pendingLang) + "（配置事件）：排队重写记忆/性格/台词库/题库/笑话");
                    }
                }
                continue;
            }
            if (ev == "lang_changed")   // 桌宠在写配置处发现 lang 变了（面板切语言 / 外部工具）
            {
                // 用事件里带的新语言（config 事件可能还没到，_cfgLang 可能还是旧值）
                const std::string nl = GetStr(&e, "lang");
                if (!nl.empty()) _cfgLang = nl;
                _deferLangRewrite = true;
                _pendingLang = LangResolve(_cfgLang);
                ResetConversation("切换语言");   // 旧语言的多轮对话不能带进新语言（LLM 会照着上文继续用旧语言回）
                Log("界面语言 → " + LangCnName(_pendingLang) + "：排队用新语言重写记忆/性格/台词库/题库/笑话"
                    + (_llmPaused ? "（她睡着，醒来后补做）" : ""));
                continue;
            }
            if (ev == "mood")
            {
                ApplyMood(&e);
                if (_llmPaused && _moodState == "awake")
                {
                    if (_moodWhy == "被戳醒") SetLlmPaused(false, "主人把她戳醒了");
                    else if (_moodWhy == "睡饱了") Log("她自己睡醒了：只用本地台词，点她一下才恢复 LLM");
                }
                if (_moodWhy != "tick")
                {
                    Log("心情：" + MoodStateZh(_moodState) + "（" + _moodWhy + "）");
                    if (_mem != NULL)
                    {
                        // 只记"关系里的事"：生闷气/被哄好/被吵醒（2）、自己消气/寂寞找主人（1）
                        int w = 0;
                        if (Contains(_moodWhy, "烦") || Contains(_moodWhy, "哄好") || Contains(_moodWhy, "戳醒")) w = 2;
                        else if (Contains(_moodWhy, "气消") || Contains(_moodWhy, "寂寞")) w = 1;
                        if (w > 0) _mem->Note("我" + MoodStateZh(_moodState) + "了（" + _moodWhy + "）", w);
                    }
                    if (StartsWith(_moodWhy.c_str(), "你回来了") && interrupt.empty())
                    {
                        std::string mins = "好一会儿";
                        const size_t p = _moodWhy.find("分钟");
                        if (p != std::string::npos && p >= 1)
                        {
                            size_t a = p;
                            while (a > 0 && _moodWhy[a - 1] >= '0' && _moodWhy[a - 1] <= '9') --a;
                            if (a < p) mins = _moodWhy.substr(a, p - a);
                        }
                        interrupt = "【重逢】主人离开了约 " + mins
                                    + " 分钟后回来了，你刚已经说了句欢迎；现在再自然地接一句（可以结合记忆提一件小事，别列举）。";
                    }
                }
                continue;
            }
            if (ev == "model_changed")
            {
                if (_mem != NULL) _mem->SaveSessionTouchNow();
                if (!LoadCaps()) continue;
                ClearRoleContext("换模型");
                interrupt.clear();
                OpenMemory();
                OpenPersona(false);   // 换了模型 = 换了角色：读/生成它的性格
                OpenQuestions(false); // 也换它的问题库（每个模型关心的事不一样）
                OpenJokes(false);     // 笑话也按模型走（不同角色的段子不同）
                PushMemory();
                _history.clear();
                _backlog[0].clear();
                _backlog[1].clear();
                _lastWarn.clear();
                _linesOk = false;
                PushLines("换模型");
                _nextAt = NowSec() + 5.0;
                continue;
            }
            if (ev == "names_set")       // 面板"名字与称呼"：名字/小名/怎么叫主人（主人手设，优先于生成的）
            {
                std::string werr;
                if (_persona.SetNames(GetStr(&e, "name"), GetStr(&e, "nick"), GetStr(&e, "call"), &werr))
                {
                    Log("名字与称呼已更新：" + _persona.DisplayName()
                        + (_persona.Get().userNick.empty() ? "" : "（小名 " + _persona.Get().userNick + "）")
                        + (_persona.Get().userCall.empty() ? "" : "，称呼主人「" + _persona.Get().userCall + "」"));
                    PushPersona();
                    ResetConversation("改了名字/称呼");   // 上文里都是旧叫法：新开一轮，不然 LLM 照着上文继续用旧的
                    _linesOk = false;
                    _linesDirty = true;
                    _linesUrgent = true;                   // 台词库按新名字/称呼重写（睡着时等点醒）
                    _linesDirtyReason = "改了名字/称呼";
                }
                else
                {
                    Log("名字与称呼保存失败：" + werr);
                }
                continue;
            }
            if (ev == "persona_regen" && _llmPaused)
            {
                _deferPersona = true;
                _deferPersonaGuidance = GetStr(&e, "guidance");
                Log("她在睡觉：重新生成性格等她被点醒后再做");
                continue;
            }
            if (ev == "persona_regen")   // 面板"重新生成性格"按钮
            {
                const std::string guidance = GetStr(&e, "guidance");
                OpenPersona(true, &guidance);
                ResetConversation("性格更新");
                interrupt.clear();
                _linesOk = false;        // 台词库也要按新性格重写
                PushLines("性格更新");
                continue;
            }
            if (ev == "work_long")       // 主人在电脑前连续动了 N 分钟 → 关心提醒
            {
                const int mins = (int)GetNum(&e, "min", 60);
                interrupt = "【关心】主人已经连续工作/在电脑前动来动去约 " + std::to_string(mins)
                            + " 分钟了：说一句体贴的话提醒他起来动一动、喝口水、看看远处，"
                              "配一个伸展/招手/点头的动作，1~2 句就够，别唠叨；"
                              "如果记忆里知道他的作息或工作习惯，可以顺带一句。";
                if (_mem != NULL) _mem->Note("主人连续忙了 " + std::to_string(mins) + " 分钟，我提醒他休息", 1);
                continue;
            }
            if (ev == "game")            // 刚和主人猜完拳：接一句话（不复盘式，纯玩）
            {
                const std::string user = GetStr(&e, "user");
                const std::string pet = GetStr(&e, "pet");
                const std::string res = GetStr(&e, "result");
                if (!user.empty() && !pet.empty())
                {
                    interrupt = "【游戏】刚和主人猜拳：主人出" + user + "，你出" + pet + "，结果"
                                + (res == "pet" ? "你赢了" : (res == "user" ? "你输了" : "平局"))
                                + "。自然接一句（赢了别太得意、输了可以不服气、平局就夸有默契），配上动作；"
                                  "这只是玩游戏，不要写成严肃的反思或记流水账。";
                    if (_mem != NULL)
                    {
                        _mem->Note("和主人玩猜拳（" + user + " vs " + pet + "，"
                                       + (res == "pet" ? "我赢" : (res == "user" ? "主人赢" : "平")) + "）", 1);
                    }
                }
                continue;
            }
            if (ev == "weather_ask")     // 主人点了"聊天气"
            {
                RefreshWeather();
                interrupt = "【天气】主人想聊天气。根据提供的模型联网天气查询结果自然关心他，1–2 句即可；"
                            "如果暂无有效查询结果就说明暂时查不到，不要编造具体温度或降雨。";
                continue;
            }
            if (ev == "joke_told")       // 桌宠本地讲了一条笑话：记进池子（下次补货时作为"已有的"避开）
            {
                const std::string j = GetStr(&e, "text");
                if (!j.empty())
                {
                    _jokes.MarkUsed(j);
                    std::string werr;
                    if (!_jokes.Save(&werr)) Log("笑话池落盘失败：" + werr);
                    Log("讲了笑话（" + _jokes.Summary() + "）：" + (Utf8Len(j) > 30 ? plat::Utf8Head(j, 30) + "…" : j));
                }
                continue;
            }
            if (ev == "mem_compress" && _llmPaused)
            {
                _deferCompress = true;
                Log("她在睡觉：记忆压缩等她被点醒后再做");
                continue;
            }
            if (ev == "mem_compress")    // 面板"立即压缩记忆"
            {
                CompressMemory("面板指令");
                continue;
            }
            if (ev == "profile_reload")  // 导入备份后：把盘上的记忆/性格/题库/笑话重新读进内存
            {
                // 恢复已经写入磁盘；不要让旧 habits 内存副本反向盖掉备份。
                ClearRoleContext("导入备份");
                interrupt.clear();
                if (!LoadCaps()) continue;
                OpenMemory();
                OpenPersona(false);
                OpenQuestions(false);
                OpenJokes(false);
                PushMemory();
                PushPersona();
                _history.clear();
                _linesOk = false;
                PushLines("导入备份");
                Log("导入备份后已重载：记忆 / 性格 / 题库 / 笑话");
                continue;
            }
            if (ev == "click" && _llmPaused && _moodState != "asleep")
            {
                SetLlmPaused(false, "主人点了她");
            }
            if (ev == "click" || ev == "rub")
            {
                const std::string z = GetStr(&e, "zone");
                _backlog[ev == "click" ? 0 : 1][z] += 1;
                _lastTouchAt = NowSec();
                if (_mem != NULL) _mem->Touch(z, ev == "rub");
            }
            else if (ev == "choice")
            {
                const std::string label = GetStr(&e, "label");
                interrupt = "【回应】用户在气泡选项里选了「" + label + "」，接着这个选择继续（这段可以到 8 秒）；"
                            "第一步就说一句回应这个选择的话，别只做动作。";
                const jsonlite::Value* did = ObjGet(&e, "did");
                _choiceDid = did != NULL && did->isArr() && !did->arr.empty();
                if (_choiceDid)
                {
                    std::string keys;
                    for (size_t k = 0; k < did->arr.size(); ++k) keys += (k ? "、" : "") + std::string(did->arr[k].strOr(""));
                    interrupt += "（点下去的瞬间她已经演了这个选项自带的 " + keys + "——接着往下，别重复同样的动作/台词）";
                }
                // 只有"我刚问过的题"的回答才算主人回答：别的气泡选项（调试 say、剧情分支、问题已答完后又点的）
                // 记成答案会往记忆/题库里塞"（没记下问题）→好"这种空题——以前确实发生过
                const bool isAnswer = !_lastQText.empty() && !label.empty();
                if (_mem != NULL && isAnswer) _mem->ChoiceMade(_lastQText, label);
                // 答案也记进问题库（以后不再问；重生成题库时作为"已知"喂回去），
                // 并让即时台词库尽快按"新知道的事"重写——不只对话，点/摸的离线台词也跟着变
                if (isAnswer)
                {
                    _questions.MarkAnswered(_lastQText, label);
                    std::string qerr;
                    if (!_questions.Save(&qerr)) Log("问题库落盘失败：" + qerr);
                    Log("主人回答：" + plat::Utf8Head(_lastQText, 40) + " → " + label
                        + "（" + _questions.Summary() + "）");
                    _linesDirty = true;
                    _linesUrgent = true;             // 不等 15 分钟的常规节流，空闲时立刻重写台词库
                    _linesDirtyReason = "主人回答";
                }
                _lastQText.clear();   // 这题答完了：之后别的气泡选项不能再记到它头上
                _lastQOpts.clear();
                _awaitingUntil = 0.0;
            }
            else if (ev == "hover")
            {
                _lastHover = ZoneCn(GetStr(&e, "zone"));
            }
        }
        if (_llmPaused)
        {
            // 暂停中：不产生任何 LLM 段（重逢/关心/游戏接话/回应全部丢弃——本地已经即时反应过了）
            _backlog[0].clear();
            _backlog[1].clear();
            _awaitingUntil = 0.0;
            NapMs(100);
            continue;
        }
        if (_deferPersona || _deferCompress || _deferLangRewrite)   // 刚被点醒：补做暂停期间要求的事
        {
            if (_deferPersona)
            {
                _deferPersona = false;
                OpenPersona(true, &_deferPersonaGuidance);
                _deferPersonaGuidance.clear();
                ResetConversation("性格更新");
                interrupt.clear();
                _linesOk = false;
                PushLines("性格更新");
            }
            if (_deferCompress)
            {
                _deferCompress = false;
                CompressMemory("面板指令（醒来后补做）");
            }
            if (_deferLangRewrite)
            {
                _deferLangRewrite = false;
                // 用排队时记下的目标语言（别用 _cfgLang：它可能被后来的 config 事件改成别的）
                LangRewriteAll(_pendingLang.empty() ? _cfgLang : _pendingLang);
            }
        }
        const double nowB = NowSec();
        const bool sleepy = _moodState == "asleep" || _moodState == "drowsy";
        if (sleepy && (!_backlog[0].empty() || !_backlog[1].empty()) && nowB - _lastTouchAt >= kQuietAfterTouch)
        {
            _backlog[0].clear();
            _backlog[1].clear();
        }
        if (interrupt.empty() && (!_backlog[0].empty() || !_backlog[1].empty())
            && nowB - _lastTouchAt >= kQuietAfterTouch && nowB - _lastLlmAt >= kRespondCooldown)
        {
            _awaitingUntil = 0.0;
            interrupt = "【回应】用户刚才和你身体互动：" + BacklogText()
                        + "。以次数最多的部位为主，按轻重（摸/揉=舒服/亲昵，牵手=开心，捏脸=害羞，连戳=恼）回应一句。";
            _backlog[0].clear();
            _backlog[1].clear();
        }
        bool scheduledWeather = false;
        if (interrupt.empty() && !sleepy && nowB >= _awaitingUntil
            && _backlog[0].empty() && _backlog[1].empty() && _cfgWeather
            && _weatherCadence.Due(nowB) && _weather.Fresh(_cfgWeatherLoc, (double)time(NULL)))
        {
            scheduledWeather = true;
            interrupt = "【天气闲聊】结合刚查询的天气，必须自然地说一句贴近主人生活的天气关心；"
                        "不要只做动作，不要提问，不要重复播报，不要虚构天气。";
        }
        if (interrupt.empty() && (sleepy || !_cfgLlmIdle)) _nextAt = std::max(_nextAt, nowB + 10.0);
        if (interrupt.empty() && (nowB < _nextAt || nowB < _awaitingUntil || !_backlog[0].empty() || !_backlog[1].empty()
                                  || sleepy || !_cfgLlmIdle))
        {
            NapMs(100);
            continue;
        }
        std::vector<const PetQuestions::Item*> questionCandidates;
        if (interrupt.empty())
        {
            interrupt = "【闲时】";
            // 提问：题库候选每次都給（不管了不了解主人）——以前只有"还不了解主人"时给一句含糊引导，
            // 于是 LLM 每次都落回"你在干什么呀？"。现在从多题材题库里挑，问过的不再重复。
            const bool canAsk = _cfgLlmQuestions && nowB - _lastQuestionAt >= kQuestionCooldown
                                && _moodState != "asleep" && _moodState != "drowsy";
            if (canAsk)
            {
                questionCandidates = _questions.Fresh(3);
                const std::string note = AskCandidatesNote(questionCandidates);
                if (!note.empty())
                {
                    std::string eager;
                    if (_mem != NULL && _mem->OwnerKnown() < 3)
                    {
                        eager = "（你还不太了解主人，这段**尽量**问一个；";
                    }
                    else
                    {
                        eager = "（想更了解主人的话可以问一个；";
                    }
                    interrupt += eager + note + "；问过的（含其它说法）别再问）";
                }
            }
        }
        if (!_lastHover.empty())
        {
            ambient.push_back("用户的鼠标刚在你的" + _lastHover + "附近");
            _lastHover.clear();
        }
        ++_segN;
        std::string note = "第 " + std::to_string(_segN) + " 段。" + MoodText() + interrupt;
        if (!ambient.empty())
        {
            note += "；";
            for (size_t i = 0; i < ambient.size(); ++i)
            {
                if (i) note += "；";
                note += ambient[i];
            }
            note += "。";
        }
        if (!_lastWarn.empty())
        {
            std::string w = "[";
            for (size_t i = 0; i < _lastWarn.size(); ++i)
            {
                if (i) w += ",";
                w += ValueToJson(_lastWarn[i]);
            }
            w += "]";
            note += "上一段的警告：" + w + "，请避免。";
        }
        const std::string kind = Contains(interrupt.c_str(), "选项里选了") ? "续戏"
                               : (StartsWith(interrupt.c_str(), "【天气】") || scheduledWeather ? "天气"
                                  : (StartsWith(interrupt.c_str(), "【回应】") ? "回应"
                                     : (StartsWith(interrupt.c_str(), "【重逢】") ? "重逢" : "闲时")));
        const l2d::LlmSettings cfgNow = l2d::LlmLoadSettings();
        if (cfgNow.key.empty())
        {
            if (scheduledWeather) _weatherCadence.RetryLater(nowB);
            _nextAt = NowSec() + 10.0;
            continue;
        }
        const double t0 = NowSec();
        jsonlite::Value seq;
        std::string llmErr;
        // 记忆进提示词前去掉时间戳（否则 LLM 会把"10-10"照抄进台词、甚至当成主人的名字）
        const std::string memCtx = _mem != NULL ? StripStamps(_mem->Context(interrupt, [this](const std::string& z) { return ZoneCn(z); })) : "";
        if (!LlmSequence(note, memCtx, &seq, &llmErr))
        {
            if (scheduledWeather) _weatherCadence.RetryLater(NowSec());
            Log("LLM 调用失败：" + llmErr);
            Report(kind, note, NULL, false, llmErr, false, 0);
            _lastLlmAt = NowSec();
            _nextAt = NowSec() + 30.0;
            continue;
        }
        _lastLlmAt = NowSec();
        const bool responding = StartsWith(interrupt.c_str(), "【回应】") || StartsWith(interrupt.c_str(), "【重逢】");
        if (responding && !_choiceDid)   // 选项自带动作正在演：续戏排在它后面（queue），别把它打断
        {
            jsonlite::Value mode;
            mode.setType(jsonlite::Value::STR);
            mode.str = "replace";
            ObjSet(&seq, "mode", mode);
            if (!Contains(interrupt.c_str(), "选项")) ClampRespond(&seq);
        }
        char msg[96];
        _choiceDid = false;   // 只管紧接着的这一段
        snprintf(msg, sizeof(msg), "%s段，LLM 用时 %.1fs", responding ? "回应" : "闲时", _lastLlmAt - t0);
        Log(msg);
        RestoreQuestionOptions(&seq, questionCandidates, &_questions);
        // 台词去日期/时间戳（兜底：提示词里已要求不写）
        if (jsonlite::Value* steps = ObjGetMut(&seq, "steps"))
        {
            for (size_t si = 0; steps->isArr() && si < steps->arr.size(); ++si)
            {
                jsonlite::Value* sp = ObjGetMut(&steps->arr[si], "speak");
                if (sp != NULL && sp->isStr())
                {
                    sp->str = StripStamps(sp->str);
                    if (!sp->str.empty() && !SpeechLangOk(sp->str, LangResolve(_cfgLang)))
                    {
                        Log("台词不是" + LangCnName(LangResolve(_cfgLang)) + "，不说：" + plat::Utf8Head(sp->str, 30));
                        sp->str.clear();
                        ObjDel(&steps->arr[si], "options");   // 问题本身不能用，选项也一起不要
                    }
                    if (sp->str.empty()) ObjDel(&steps->arr[si], "speak");
                }
                jsonlite::Value* op = ObjGetMut(&steps->arr[si], "options");
                for (size_t oi = 0; op != NULL && op->isArr() && oi < op->arr.size(); ++oi)
                {
                    // 选项可以是字符串，也可以是 {"label":…,"do":{…}}（点了立刻演的动作）——文字部分都要清洗
                    jsonlite::Value* lab = op->arr[oi].isObj() ? ObjGetMut(&op->arr[oi], "label") : &op->arr[oi];
                    if (lab != NULL && lab->isStr()) lab->str = StripStamps(lab->str);
                    jsonlite::Value* d = op->arr[oi].isObj() ? ObjGetMut(&op->arr[oi], "do") : NULL;
                    jsonlite::Value* dsp = d != NULL ? ObjGetMut(d, "speak") : NULL;
                    if (dsp != NULL && dsp->isStr())
                    {
                        dsp->str = StripStamps(dsp->str);
                        if (!SpeechLangOk(dsp->str, LangResolve(_cfgLang))) ObjDel(d, "speak");
                    }
                }
            }
        }
        std::string cutWhat;
        const bool asked = ThrottleSeq(&seq, NowSec(), _lastQuestionAt, _cfgLlmQuestions, &cutWhat);
        if (!cutWhat.empty()) Log("节流：删掉 " + cutWhat);
        {
            jsonlite::Value seqId;
            seqId.setType(jsonlite::Value::STR);
            seqId.str = "s" + std::to_string(_segN);
            ObjSet(&seq, "seq", seqId);
        }
        jsonlite::Value reply;
        if (!JsonRpcOk(Rpc(ValueToJson(seq)), &reply))
        {
            if (GetStr(&reply, "code") == "queue_full")
            {
                jsonlite::Value mode;
                mode.setType(jsonlite::Value::STR);
                mode.str = "replace";
                ObjSet(&seq, "mode", mode);
                if (JsonRpcOk(Rpc(ValueToJson(seq)), &reply)) Log("队列满 → replace 重发");
            }
        }
        const bool ok = GetNum(&reply, "ok", 0) != 0;
        if (scheduledWeather)
        {
            if (ok && !SummarizeSay(seq).empty()) _weatherCadence.Delivered(NowSec());
            else _weatherCadence.RetryLater(NowSec());
        }
        _lastWarn.clear();
        const jsonlite::Value* warns = ObjGet(&reply, "warnings");
        if (warns != NULL && warns->isArr())
        {
            for (size_t i = 0; i < warns->arr.size(); ++i) _lastWarn.push_back(warns->arr[i]);
        }
        char res[200];
        // 应了一段却一句台词都没有 = 主人刚答完/刚摸完却什么也没听见（提示词要求有台词，模型有时仍只做动作）
        const bool silentReply = responding && SummarizeSay(seq).empty();
        snprintf(res, sizeof(res), "第 %ld 段 dur=%.1f → ok=%d accepted=%.0f warnings=%zu%s",
                 _segN, GetNum(&seq, "dur", 0), (int)ok, GetNum(&reply, "accepted", 0), _lastWarn.size(),
                 silentReply ? "（这句回应一句台词都没有）" : "");
        Log(res);
        Report(kind, note, &seq, ok, ok ? "" : GetStr(&reply, "msg"), asked, (int)_lastWarn.size());
        // 记住提问内容（choice 回来时记进记忆）
        const jsonlite::Value* stepsOut = ObjGet(&seq, "steps");
        if (stepsOut != NULL && stepsOut->isArr())
        {
            for (size_t i = 0; i < stepsOut->arr.size(); ++i)
            {
                const jsonlite::Value* opts = ObjGet(&stepsOut->arr[i], "options");
                if (opts != NULL && opts->isArr() && ok)
                {
                    _lastQText = GetStr(&stepsOut->arr[i], "speak");
                    _lastQOpts.clear();
                    for (size_t j = 0; j < opts->arr.size(); ++j)
                    {
                        const jsonlite::Value& o = opts->arr[j];
                        _lastQOpts.push_back(o.isObj() ? std::string(o.get("label").strOr("")) : std::string(o.strOr("")));
                    }
                    // 题库记一笔"问过了"（LLM 自创的题也会收进库里，避免以后重复）
                    _questions.MarkAsked(_lastQText, _lastQOpts);
                    std::string qerr;
                    if (!_questions.Save(&qerr)) Log("问题库落盘失败：" + qerr);
                }
            }
        }
        if (_mem != NULL && (kind == "续戏" || kind == "重逢"))
        {
            std::vector<std::string> say = SummarizeSay(seq);
            if (!say.empty())
            {
                std::string joined;
                for (size_t i = 0; i < say.size() && i < 2; ++i)
                {
                    if (i) joined += "/";
                    joined += say[i];
                }
                _mem->Note("（" + kind + "）我说：" + joined, 1);
            }
        }
        if (asked && ok)
        {
            _lastQuestionAt = NowSec();
            _awaitingUntil = NowSec() + kQuestionWait;
        }
        if (_mem != NULL && _mem->PendingWeight() >= kMemoryEvery)
        {
            Consolidate("攒够了", false);
        }
        const double gap = _cfgIdlePeriod * (1.0 + 0.5 * ((double)rand() / (double)RAND_MAX));
        _nextAt = NowSec() + std::max(gap, GetNum(&seq, "dur", 8) + 5.0);
    }
    if (_mem != NULL) _mem->SaveSessionTouchNow();
    Log("内置 agent 线程退出");
}

// ---------------------------------------------------------------- 辅助

bool PetAgent::LoadCaps()
{
    const std::string reply =
        Rpc("{\"cmd\":\"query\",\"what\":[\"summary\",\"skeleton\",\"expressions\",\"motions\",\"actions\",\"parts\",\"zones\"]}");
    jsonlite::Value v;
    if (!jsonlite::parse(reply.c_str(), reply.size(), v, NULL) || !v.isObj() || GetNum(&v, "ok", 0) == 0)
    {
        Log("query 回复（前 200 字符）：" + (reply.empty() ? std::string("<空>") : plat::Utf8Head(reply, 200)));
        return false;
    }
    _caps.raw = "{";
    const char* keys[] = { "model", "summary", "skeleton", "expressions", "motions", "actions", "parts", "zones" };
    bool first = true;
    for (size_t k = 0; k < sizeof(keys) / sizeof(keys[0]); ++k)
    {
        const jsonlite::Value* item = ObjGet(&v, keys[k]);
        if (item == NULL) continue;
        if (!first) _caps.raw += ",";
        first = false;
        _caps.raw += std::string("\"") + keys[k] + "\":" + ValueToJson(*item);
    }
    _caps.raw += "}";
    const jsonlite::Value* model = ObjGet(&v, "model");
    _caps.modelName = model != NULL ? model->get("name").strOr("?") : "?";
    _caps.modelFv = model != NULL ? model->get("fv").strOr("") : "";
    _caps.zoneZh.clear();
    const jsonlite::Value* zones = ObjGet(&v, "zones");
    if (zones != NULL && zones->isArr())
    {
        for (size_t i = 0; i < zones->arr.size(); ++i)
        {
            const std::string fv = zones->arr[i].get("fv").strOr("");
            const size_t c = fv.find(':');
            if (c != std::string::npos) _caps.zoneZh[fv.substr(c + 1)] = std::string(zones->arr[i].get("name").strOr(fv.c_str()));
        }
    }
    // 表情清单（性格生成的 fav_expr 白名单 + 能力摘要）；水印开关类表情排除
    // （桌宠每帧强制关水印，让 LLM 偏好它就等于每次露面都把水印打开）
    _caps.exprFv.clear();
    _caps.exprZh.clear();
    const jsonlite::Value* exps = ObjGet(&v, "expressions");
    if (exps != NULL && exps->isArr())
    {
        for (size_t i = 0; i < exps->arr.size(); ++i)
        {
            const std::string fv = exps->arr[i].get("fv").strOr("");
            if (fv.empty()) continue;
            const std::string nm = exps->arr[i].get("name").strOr("");
            if (nm.find("水印") != std::string::npos || nm.find("watermark") != std::string::npos) continue;
            _caps.exprFv.push_back(fv);
            _caps.exprZh.push_back(nm);
        }
    }
    char msg[192];
    const jsonlite::Value* acts = ObjGet(&v, "actions");
    snprintf(msg, sizeof(msg), "模型 %s（%s）区域=%zu 动作=%zu 表情=%zu", _caps.modelName.c_str(), _caps.modelFv.c_str(),
             _caps.zoneZh.size(), acts != NULL && acts->isArr() ? acts->arr.size() : (size_t)0,
             _caps.exprFv.size());
    Log(msg);
    return true;
}

std::string PetAgent::ZoneCn(const std::string& key) const
{
    std::map<std::string, std::string>::const_iterator it = _caps.zoneZh.find(key);
    return it != _caps.zoneZh.end() ? it->second : (key.empty() ? "?" : key);
}

bool PetAgent::JsonRpcOk(const std::string& reply, jsonlite::Value* out)
{
    return jsonlite::parse(reply.c_str(), reply.size(), *out, NULL) && out->isObj() && GetNum(out, "ok", 0) != 0;
}

void PetAgent::ApplyMood(const jsonlite::Value* mood)
{
    _moodState = GetStr(mood, "state", "awake");
    _moodWhy = GetStr(mood, "why");
    const jsonlite::Value* emo = ObjGet(mood, "emotion");
    _moodEmoName = emo != NULL ? emo->get("name").strOr("neutral") : "neutral";
    _moodEmoIntensity = emo != NULL ? emo->get("intensity").numOr(0) : 0;
    _moodAffection = GetNum(mood, "affection", 0.3);
    _moodAnnoy = GetNum(mood, "annoy", 0);
    _moodLonely = GetNum(mood, "lonely", 0);
}

void PetAgent::ApplyConfig(const jsonlite::Value* cfg)
{
    const int oldTier = _cfgTokenTier;
    _cfgTokenTier = l2d::TokenTier(GetNum(cfg, "llm_token_tier", 1));
    const l2d::TokenBudget budget = l2d::BudgetForTier(_cfgTokenTier);
    _cfgLlmIdle = GetNum(cfg, "llm_idle", 1) > 0.5 && budget.idleEnabled;
    _cfgIdlePeriod = GetNum(cfg, "llm_period", kIdlePeriodDefault);
    if (_cfgIdlePeriod <= 0) _cfgIdlePeriod = kIdlePeriodDefault;
    _cfgIdlePeriod *= budget.idleMultiplier;
    if (_cfgTokenTier != oldTier) _nextAt = NowSec() + _cfgIdlePeriod;
    _cfgLlmQuestions = GetNum(cfg, "llm_questions", 1) > 0.5;
    _cfgWeather = GetNum(cfg, "weather", 0) > 0.5;
    _cfgWeatherLoc.clear();
    if (cfg != NULL && cfg->isObj())
    {
        const jsonlite::Value& sv = cfg->get("str");
        if (sv.isObj())
        {
            const jsonlite::Value& loc = sv.get("weather_loc");
            if (loc.isStr()) _cfgWeatherLoc = loc.str;
            const jsonlite::Value& lg = sv.get("lang");
            if (lg.isStr() && !lg.str.empty()) _cfgLang = lg.str;
            // 注意：**不在这里**靠"比较上次见到的 lang"来发现语言切换——agent 启动时那次 status
            // 回复可能还没有 str 段（基线就错了），会把第一次真正的切换当成基线吃掉（踩过）。
            // 改由桌宠在写配置的地方判定前后是否变化，再推 lang_changed 事件（任何来源都覆盖）。
        }
    }
}

/** 字符串配置（config 的 str 段；agent 只读） */
std::string PetAgent::CfgGetStr(const char* key) const
{
    if (std::string(key) == "weather_loc") return _cfgWeatherLoc;
    if (std::string(key) == "lang") return _cfgLang;
    return std::string();
}

std::string PetAgent::MoodText() const
{
    char b[192];
    snprintf(b, sizeof(b), "【她现在%s，情绪 %s（强度 %.2f），亲密度 %.2f，恼火 %.2f，寂寞 %.2f】",
             MoodStateZh(_moodState).c_str(), _moodEmoName.c_str(), _moodEmoIntensity, _moodAffection, _moodAnnoy,
             _moodLonely);
    return b;
}

std::string PetAgent::BacklogText() const
{
    std::map<std::string, std::pair<long, long> > all;   // zone → [click, rub]
    for (int t = 0; t < 2; ++t)
    {
        for (std::map<std::string, long>::const_iterator it = _backlog[t].begin(); it != _backlog[t].end(); ++it)
        {
            if (t == 0) all[it->first].first += it->second;
            else all[it->first].second += it->second;
        }
    }
    std::vector<std::pair<long, std::string> > items;
    for (std::map<std::string, std::pair<long, long> >::const_iterator it = all.begin(); it != all.end(); ++it)
    {
        items.push_back(std::make_pair(it->second.first + it->second.second, it->first));
    }
    std::sort(items.begin(), items.end(), std::greater<std::pair<long, std::string> >());
    std::string out;
    for (size_t i = 0; i < items.size() && i < 6; ++i)
    {
        const std::pair<long, long>& cnt = all[items[i].second];
        std::string seg;
        const char* zk = items[i].second.c_str();
        if (cnt.second > 0) { char b[48]; snprintf(b, sizeof(b), "被%s了 %ld 次", TouchVerb(zk, true), cnt.second); seg = b; }
        if (cnt.first > 0)
        {
            char b[48];
            snprintf(b, sizeof(b), "被%s了 %ld 次", TouchVerb(zk, false), cnt.first);
            if (!seg.empty()) seg += "、";
            seg += b;
        }
        if (i) out += "；";
        out += ZoneCn(items[i].second) + seg;
    }
    return out;
}

std::vector<std::string> PetAgent::SummarizeSay(const jsonlite::Value& seq)
{
    std::vector<std::string> say;
    const jsonlite::Value* steps = ObjGet(&seq, "steps");
    if (steps != NULL && steps->isArr())
    {
        for (size_t i = 0; i < steps->arr.size(); ++i)
        {
            const std::string s = GetStr(&steps->arr[i], "speak");
            if (!s.empty()) say.push_back(s);
        }
    }
    return say;
}

void PetAgent::Announce()
{
    const l2d::LlmSettings st = l2d::LlmLoadSettings();
    Rpc("{\"cmd\":\"agent_info\",\"key_source\":\"" + st.keySource + "\",\"model\":\"" + JsonEscape(st.model)
        + "\",\"base_url\":\"" + JsonEscape(st.baseUrl) + "\",\"paused\":" + (_llmPaused ? "true" : "false") + "}");
}

void PetAgent::SetLlmPaused(bool paused, const std::string& why)
{
    if (_llmPaused == paused) return;
    _llmPaused = paused;
    l2d::LlmSetPaused(paused);
    Log(paused ? why + "：暂停一切 LLM 调用（记忆/台词库/题库/笑话/对话），点她一下才恢复"
               : why + "：恢复 LLM 调用");
    if (!paused) _nextAt = std::max(_nextAt, NowSec() + 2.0);   // 先让本地反应演完
    Announce();   // 面板据此显示"LLM 休眠中"
}

void PetAgent::OpenMemory()
{
    _mem.reset();
    std::string fp = _caps.modelFv;
    const size_t c = fp.rfind(':');
    if (c != std::string::npos) fp = fp.substr(c + 1);
    _mem.reset(new PetMemory(fp, _caps.modelName));
    const std::string gapTxt = _mem->Gap() < 0 ? "第一次见面" : "距上次 " + FmtGap(_mem->Gap());
    char msg[192];
    snprintf(msg, sizeof(msg), "记忆：%s.md（%s；关于主人 %d 条；待整理权重 %d）", fp.c_str(), gapTxt.c_str(),
             _mem->OwnerKnown(), _mem->PendingWeight());
    Log(msg);
    _memLast = NowSec();
}

/** 记忆压缩（每天自动 / 面板点"立即压缩"）：合并去重 + 旧事折"更早" + 归档截断 */
void PetAgent::CompressMemory(const std::string& reason)
{
    if (_mem == NULL) return;
    const l2d::LlmSettings cfg = l2d::LlmLoadSettings();
    if (cfg.key.empty())
    {
        Log("没有 API Key：记忆压缩跳过");
        return;
    }
    std::string err, report;
    Log("记忆压缩（" + reason + "）…");
    const bool ok = _mem->Compress(
        [this, cfg](const std::string& prompt, std::string* owner, std::string* us) -> bool {
            std::vector<l2d::LlmMsg> msgs;
            l2d::LlmMsg u;
            u.role = "user";
            u.content = prompt;
            msgs.push_back(u);
            std::string text, e;
            if (!l2d::LlmChat(LongGenCfg(cfg), msgs, 0.3f, true, &text, &e, &_lastCall)) return false;
            text = StripFences(text);
            jsonlite::Value v;
            if (!jsonlite::parse(text.c_str(), text.size(), v, NULL) || !v.isObj()) return false;
            const jsonlite::Value* o = ObjGet(&v, "owner");
            const jsonlite::Value* s = ObjGet(&v, "us");
            if (o == NULL || s == NULL || !o->isStr() || !s->isStr()) return false;
            *owner = o->str;
            *us = s->str;
            return true;
        },
        [this](const std::string& z) { return ZoneCn(z); }, &err, &report);
    if (!ok)
    {
        Log("记忆压缩未成（" + err + "）——下次再试");
        Report("压缩", "记忆压缩", NULL, false, err, false, 0);
        Rpc("{\"cmd\":\"mem_notify\",\"ok\":false,\"msg\":\"" + JsonEscape(err) + "\"}");
        return;
    }
    Log("记忆压缩完成：" + report);
    Report("压缩", "记忆压缩（" + reason + "）：" + report, NULL, true, "", false, 0);
    PushMemory();
    Rpc("{\"cmd\":\"mem_notify\",\"ok\":true,\"report\":\"" + JsonEscape(report) + "\"}");
}

void PetAgent::PushMemory(){
    if (_mem == NULL) return;
    time_t t = time(NULL);
    struct tm lt;
#ifdef _WIN32
    localtime_s(&lt, &t);
#else
    localtime_r(&t, &lt);
#endif
    char at[32];
    strftime(at, sizeof(at), "%m-%d %H:%M", &lt);
    const std::string panel = _mem->PanelText([this](const std::string& z) { return ZoneCn(z); });
    // 记忆本末尾附一行题库摘要（面板"记忆"页能看到她还想知道什么）
    const std::string withQ = panel + "\n\n## " + _questions.Summary()
                              + "\n（每模型一份：~/.config/l2dpet/questions/<指纹>.json；答过的不会再问）";
    Rpc("{\"cmd\":\"memory\",\"text\":\"" + JsonEscape(withQ) + "\",\"at\":\"" + at + "\"}");
}

/** 能力摘要（喂给性格生成；比整张能力表短得多，只留"她是个什么样的角色"相关的） */
std::string PetAgent::CapsDigest() const
{
    std::string o = "- 模型名：" + _caps.modelName + "\n";
    if (_caps.exprZh.empty())
    {
        o += "- 自带表情/道具：无\n";
    }
    else
    {
        o += "- 自带表情/道具（她可以用来表达自己的东西）：";
        for (size_t i = 0; i < _caps.exprZh.size() && i < 24; ++i)
        {
            o += (i ? "、" : "") + _caps.exprZh[i];
        }
        if (_caps.exprZh.size() > 24) o += "…";
        o += "\n";
    }
    {
        std::string skel;
        const size_t s = _caps.raw.find("\"skeleton\":");
        if (s != std::string::npos)
        {
            const size_t e = _caps.raw.find_first_of(",}", s + 11);
            skel = _caps.raw.substr(s + 11, e == std::string::npos ? 20 : e - s - 11);
        }
        if (!skel.empty() && skel != "\"\"") o += "- 骨架等级：" + skel + "\n";
    }
    {
        o += "- 可交互的身体区域：" + std::to_string(_caps.zoneZh.size()) + " 个（";
        bool first = true;
        for (std::map<std::string, std::string>::const_iterator it = _caps.zoneZh.begin(); it != _caps.zoneZh.end(); ++it)
        {
            o += (first ? "" : "、") + it->second;
            first = false;
        }
        o += "）\n";
    }
    return o;
}

void PetAgent::OpenPersona(bool regen, const std::string* guidance)
{
    const std::string fp = _caps.modelFv;
    const l2d::LlmSettings cfg = l2d::LlmLoadSettings();
    _persona.Reset();
    const bool loaded = _persona.Load(fp);
    const std::string userGuidance = guidance != NULL ? *guidance : _persona.Get().userGuidance;
    if (!regen && loaded)
    {
        // 兜底版（当时没 Key）现在有 Key 了 → 再试一次 LLM 升级；LLM 版直接用缓存
        if (_persona.Get().by != "heuristic" || cfg.key.empty())
        {
            Log("性格（已缓存）：" + _persona.Get().name + "——" + plat::Utf8Head(_persona.Get().persona, 60));
            PushPersona();
            return;
        }
        Log("性格是本地兜底版、现在有 API Key：换 LLM 重新解算…");
    }
    std::string err;
    bool ok = false;
    if (!cfg.key.empty())
    {
        Log(regen ? "重新生成性格（LLM）…" : "首次见到这个模型：让 LLM 按能力解算性格…");
        const l2d::LlmSettings cfg2 = cfg;
        const std::string langHint = SpeechLangHint();
        const std::string langNow = LangResolve(_cfgLang);   // lambda 里不能碰 this：先算好
        PetPersona::LlmFn call = [this, cfg2, langHint, langNow](const std::string& system, const std::string& user, std::string* out,
                                        std::string* e) -> bool {
            std::vector<l2d::LlmMsg> msgs;
            l2d::LlmMsg s;
            s.role = "system";
            s.content = LangFixup(system, langNow) + langHint;
            msgs.push_back(s);
            l2d::LlmMsg u;
            u.role = "user";
            u.content = user;
            msgs.push_back(u);
            std::string text;
            if (!l2d::LlmChat(LongGenCfg(cfg2), msgs, 0.8f, true, &text, e, &_lastCall)) return false;
            *out = text;
            return true;
        };
        for (int attempt = 0; attempt < 2 && !ok; ++attempt)   // LLM 偶尔吐坏 JSON：重试一次
        {
            ok = _persona.Generate(fp, _caps.modelName, CapsDigest(), _caps.exprFv, call, &err, userGuidance);
            if (!ok && attempt == 0) Log("性格生成失败（" + err + "），重试一次…");
        }
    }
    else
    {
        err = "没有 API Key";
    }
    if (!ok)
    {
        Log("性格 LLM 生成未成（" + err + "）→ 用本地兜底");
        _persona.Heuristic(fp, _caps.modelName, _caps.exprFv);
    }
    std::string werr;
    _persona.SetGuidance(userGuidance);
    if (!_persona.Save(&werr)) Log("性格落盘失败：" + werr);
    Log("性格：" + _persona.Get().name + "——" + plat::Utf8Head(_persona.Get().persona, 60)
        + "（" + _persona.Get().by + "）");
    Report("性格", "为 " + _caps.modelName + " " + (regen ? "重新生成" : "解算") + "性格（"
                       + (_persona.Get().by == "llm" ? "LLM" : "本地兜底") + "）：" + _persona.Get().name,
           NULL, ok, ok ? "" : err, false, 0);
    PushPersona();
}

void PetAgent::PushPersona()
{
    if (_persona.Empty()) return;
    Rpc("{\"cmd\":\"persona\",\"text\":\"" + JsonEscape(_persona.PanelText())
        + "\",\"name\":\"" + JsonEscape(_persona.DisplayName())
        + "\",\"nick\":\"" + JsonEscape(_persona.Get().userNick)
        + "\",\"call\":\"" + JsonEscape(_persona.Get().userCall)
        + "\",\"guidance\":\"" + JsonEscape(_persona.Get().userGuidance)
        + "\",\"user_name\":\"" + JsonEscape(_persona.Get().userName)
        + "\",\"by\":\"" + JsonEscape(_persona.Get().by) + "\"}");
}

// ---------------------------------------------------------------- 问题库（想了解主人的问题）

/** Savings tiers refill only on exhaustion; Full retains proactive refill/24h renewal. */
static bool QuestionsNeedRefresh(const PetQuestions& q, int tokenTier){
    if (q.Empty()) return true;
    if (q.Fresh(1).empty()) return true;
    return tokenTier == 0 && (q.Fresh(3).size() < 3 || q.Stale(24.0 * 3600.0));
}

void PetAgent::OpenQuestions(bool regen)
{
    const std::string fp = _caps.modelFv;
    _questions.Reset();
    const bool loaded = _questions.Load(fp);
    if (loaded && !regen && !QuestionsNeedRefresh(_questions, _cfgTokenTier))
    {
        Log("问题库（已缓存）：" + _questions.Summary());
        return;
    }
    const l2d::LlmSettings cfg = l2d::LlmLoadSettings();
    if (cfg.key.empty())
    {
        Log("还没有 API Key：问题库先不生成（配好后自动生成）");
        return;
    }
    // 已知 = 记忆里主人的情况 + 题库里已问/已答的题（防重复）
    std::vector<std::string> knownQA;
    if (loaded)
    {
        const std::string kt = _questions.KnownText();
        if (!kt.empty()) knownQA.push_back("【问题库历史】" + kt);
    }
    if (_mem != NULL)
    {
        const std::string own = StripStamps(_mem->Owner());
        if (!own.empty()) knownQA.push_back("【记忆里已知的主人】" + own);
    }
    Log(regen ? "重新生成问题库（LLM）…" : "生成问题库（LLM，12 题多题材）…");
    const l2d::LlmSettings cfg2 = cfg;
    const std::string langHint = SpeechLangHint();
    const std::string langNow = LangResolve(_cfgLang);   // lambda 里不能碰 this：先算好
    PetQuestions::LlmFn call = [this, cfg2, langHint, langNow](const std::string& system, const std::string& user, std::string* out,
                                      std::string* e) -> bool {
        std::vector<l2d::LlmMsg> msgs;
        l2d::LlmMsg s;
        s.role = "system";
        s.content = LangFixup(system, langNow) + langHint;
        msgs.push_back(s);
        l2d::LlmMsg u;
        u.role = "user";
        u.content = user;
        msgs.push_back(u);
        std::string text;
        if (!l2d::LlmChat(LongGenCfg(cfg2), msgs, 0.9f, true, &text, e, &_lastCall)) return false;
        *out = text;
        return true;
    };
    std::string err;
    bool ok = false;
    for (int attempt = 0; attempt < 2 && !ok; ++attempt)
    {
        ok = _questions.Generate(fp, _caps.modelName, _persona.LinesRole(), knownQA, call, &err);
        if (!ok && attempt == 0) Log("问题库生成失败（" + err + "），重试一次…");
    }
    if (!ok)
    {
        Log("问题库生成未成（" + err + "）——这次先不提问，下次再试");
        return;
    }
    std::string werr;
    if (!_questions.Save(&werr)) Log("问题库落盘失败：" + werr);
    Log("问题库：" + _questions.Summary() + " → " + std::to_string(_questions.Fresh(1000).size()) + " 题可用");
    Report("问题库", "为 " + _caps.modelName + " 生成问题库：" + _questions.Summary(), NULL, true, "", false, 0);
}

// ---------------------------------------------------------------- 笑话池（本地秒回的内容，LLM 定期补货）

/** 补货最小间隔（秒）：池子里"没讲过的"少于 2 条、且距上次生成够久，才生成新的 */
static double JokeGapSec()
{
    if (const char* e = getenv("L2D_JOKE_GAP_SEC"))
    {
        const double v = atof(e);
        if (v > 0.0) return v;
    }
    return 1800.0;   // 30 分钟
}

void PetAgent::OpenJokes(bool force)
{
    const std::string fp = _caps.modelFv;
    if (force)
    {
        _jokes.Reset();
    }
    else if (!_jokes.Load(fp))
    {
        _jokes.Reset();
    }
    if (!_jokes.Empty() && !force)
    {
        Log("笑话池（已缓存）：" + _jokes.Summary());
        PushJokes();
    }
    if (!force && !_jokes.Empty() && !_jokes.NeedMore(JokeGapSec()))
    {
        return;
    }
    const l2d::LlmSettings cfg = l2d::LlmLoadSettings();
    if (cfg.key.empty())
    {
        if (_jokes.Empty()) Log("还没有 API Key：笑话池先空着（桌宠用台词库/内置笑话兜底）");
        return;
    }
    const l2d::LlmSettings cfg2 = cfg;
    const std::string langHint = SpeechLangHint();
    const std::string langNow = LangResolve(_cfgLang);   // lambda 里不能碰 this：先算好
    PetJokes::LlmFn call = [this, cfg2, langHint, langNow](const std::string& system, const std::string& user, std::string* out,
                                  std::string* e) -> bool {
        std::vector<l2d::LlmMsg> msgs;
        l2d::LlmMsg s;
        s.role = "system";
        s.content = LangFixup(system, langNow) + langHint;
        msgs.push_back(s);
        l2d::LlmMsg u;
        u.role = "user";
        u.content = user;
        msgs.push_back(u);
        std::string text;
        if (!l2d::LlmChat(LongGenCfg(cfg2), msgs, 0.9f, true, &text, e, &_lastCall)) return false;
        *out = text;
        return true;
    };
    std::string err;
    Log(force ? "补笑话（LLM，指定）…" : "补充笑话（LLM，池子里没讲过的不够）…");
    bool ok = false;
    for (int attempt = 0; attempt < 2 && !ok; ++attempt)
    {
        ok = _jokes.Generate(fp, _caps.modelName, _persona.LinesRole(), 6, call, &err);
        if (!ok && attempt == 0) Log("笑话生成失败（" + err + "），重试一次…");
    }
    if (!ok)
    {
        Log("笑话生成未成（" + err + "）——这次先不补，下次再试");
        return;
    }
    std::string werr;
    if (!_jokes.Save(&werr)) Log("笑话池落盘失败：" + werr);
    Log("笑话池：" + _jokes.Summary());
    Report("笑话", "为 " + _caps.modelName + " 补了笑话：" + _jokes.Summary(), NULL, true, "", false, 0);
    PushJokes();
}

void PetAgent::RefreshWeather()
{
    const double now = (double)time(NULL);
    if (_llmPaused || !_weather.NeedsRefresh(_cfgWeatherLoc, now)) return;
    const l2d::LlmSettings cfg = l2d::LlmLoadSettings();
    if (cfg.key.empty()) return;
    std::string err;
    Log("联网查询天气：" + _cfgWeatherLoc);
    // 取数顺序：先直连 open-meteo（免 key、不花 token，国内外链路都实测可达），
    // 不行再退回模型的联网搜索（需要官方 DeepSeek 供应商）。两个都失败就保持"不聊天气、问你"。
    const bool ok = _weather.Refresh(_cfgWeatherLoc, now,
        [cfg](const std::string& location, std::string* facts, std::string* error) {
            std::string directErr;
            if (l2d::OpenMeteoWeather(location, facts, &directErr))
            {
                Log("天气取数：open-meteo 直连成功");
                return true;
            }
            std::string response, modelErr;
            if (l2d::LlmWeatherSearch(cfg, location, &response, &modelErr)
                && PetWeather::ParseSearchResult(response, facts, &modelErr))
            {
                Log("天气取数：open-meteo 直连失败（" + directErr + "），模型联网搜索成功");
                return true;
            }
            *error = "open-meteo 直连：" + directErr + "；模型联网搜索：" + modelErr;
            return false;
        }, &err);
    if (ok)
    {
        const std::string dir = plat::HomeDir() + "/.config/l2dpet";
        plat::CreateDir(dir, true);
        if (!_weather.Save(dir + "/weather-search.json")) Log("天气查询缓存写入失败（本次仍可使用内存缓存）");
        Log("天气查询完成，5 小时内复用");
    }
    else Log("天气查询未成功：" + err);
    Report("天气查询", _cfgWeatherLoc + "（直连失败才用模型搜索）", NULL, ok, err, false, 0);
}

void PetAgent::PushJokes()
{
    if (_jokes.Empty()) return;
    const std::vector<std::string> all = _jokes.Texts();
    std::string o = "{\"cmd\":\"jokes\",\"list\":[";
    bool first = true;
    for (size_t i = 0; i < all.size(); ++i)
    {
        if (!SpeechLangOk(all[i], LangResolve(_cfgLang))) continue;   // 池子里旧语言的笑话不推（本地讲时会冒中文）
        if (!first) o += ",";
        first = false;
        o += "\"" + JsonEscape(all[i]) + "\"";
    }
    o += "]}";
    Rpc(o);
}

/** 闲时段要提问时：从题库挑 2~3 个候选写进提示词 */
std::string PetAgent::AskCandidatesNote(const std::vector<const PetQuestions::Item*>& cands) const
{
    if (cands.empty()) return std::string();
    std::string o = "（这段可以用一句台词 + options 问主人一个问题，用来更了解他；**从下面挑一个最贴合当前气氛的**，"
                    "可以按你的口吻微调措辞、选项照抄或略改；不要问成是非题；问完这段就别再说别的。"
                    "只能选择候选题，不能自创问题；在同一个 speak 步骤里必须写 question_id（下面的稳定 ID，字符串）和 options 数组，"
                    "例如 {\"at\":0,\"speak\":\"问题台词\",\"question_id\":\"q_…\",\"options\":[\"选项一\",\"选项二\"]}；"
                    "只写问句不会产生可点击的按钮。候选题：\n";
    for (size_t i = 0; i < cands.size(); ++i)
    {
        o += "  " + cands[i]->id + ") " + cands[i]->q + " 选项：";
        for (size_t k = 0; k < cands[i]->options.size(); ++k)
        {
            o += (k ? "/" : "") + cands[i]->options[k];
        }
        o += "\n";
    }
    const std::string known = _questions.KnownText();
    if (!known.empty())
    {
        o += "  已经知道/问过的别再问：" + known + "\n";
    }
    o += "  如果当下气氛不适合提问，就不问。）";
    return o;
}

void PetAgent::PushLines(const std::string& reason)
{
    const l2d::LlmSettings cfg = l2d::LlmLoadSettings();
    if (cfg.key.empty())
    {
        Log("还没有 API Key：在桌宠「状态与设置 → 对话」里填好后自动开始");
        return;
    }
    jsonlite::Value lines;
    std::string err;
    const std::string memCtx = _mem != NULL ? StripStamps(_mem->Context("", [this](const std::string& z) { return ZoneCn(z); })) : "";
    if (!LlmReactLines(memCtx, &lines, &err))
    {
        Report("台词库", "生成即时台词库", NULL, false, err, false, 0);
        Log("台词库生成失败（" + err + "），桌宠用内置台词");
        _linesRetryAt = NowSec() + kLinesRetry;   // 失败退避（_linesLast 只记成功，所以这里必须自己记）
        return;
    }
    const std::string reply = Rpc("{\"cmd\":\"reactions\",\"lines\":" + ValueToJson(lines) + "}");
    jsonlite::Value r;
    _linesOk = true;
    _linesLast = NowSec();
    _linesDirty = false;
    if (JsonRpcOk(reply, &r))
    {
        const std::string msg = "即时台词库（" + reason + "）：" + std::to_string((long long)GetNum(&r, "groups", 0))
                                + " 组 " + std::to_string((long long)GetNum(&r, "lines", 0)) + " 句 → 桌宠本地使用";
        Log(msg);
    }
    Report("台词库", "为 " + _caps.modelName + " 生成即时台词库（" + reason + "）", NULL, true, "", false, 0);
}

void PetAgent::Report(const std::string& kind, const std::string& note, const jsonlite::Value* seq,
                      bool ok, const std::string& err, bool asked, int warn)
{
    std::vector<std::string> say = seq != NULL ? SummarizeSay(*seq) : std::vector<std::string>();
    std::string sayArr = "[";
    for (size_t i = 0; i < say.size() && i < 4; ++i)
    {
        if (i) sayArr += ",";
        sayArr += "\"" + JsonEscape(say[i]) + "\"";
    }
    sayArr += "]";
    std::string noteCut = plat::Utf8Head(note, 300);
    std::string errCut = plat::Utf8Head(err, 200);
    // 纯拼接构造日志条目（不经格式化函数）
    const std::string entry = "{\"t\":\"" + WallTime() + "\",\"kind\":\"" + JsonEscape(kind)
                              + "\",\"ok\":" + (ok ? "true" : "false")
                              + ",\"question\":" + (asked ? "true" : "false")
                              + ",\"warn\":" + std::to_string(warn)
                              + ",\"ms\":" + std::to_string(_lastCall.ms)
                              + ",\"tok_in\":" + std::to_string(_lastCall.tokIn)
                              + ",\"tok_out\":" + std::to_string(_lastCall.tokOut)
                              + ",\"note\":\"" + JsonEscape(noteCut)
                              + "\",\"say\":" + sayArr
                              + ",\"err\":\"" + JsonEscape(errCut) + "\"}";
    _lastCall = l2d::LlmUsage();
    Rpc("{\"cmd\":\"llm_log\",\"entry\":" + entry + "}");
}

void PetAgent::Consolidate(const std::string& reason, bool tidy)
{
    if (_mem == NULL) return;
    const l2d::LlmSettings cfg = l2d::LlmLoadSettings();
    if (cfg.key.empty()) return;
    std::string err;
    const bool changed = _mem->Consolidate(
        [this, cfg](const std::string& prompt, std::string* owner, std::string* us) -> bool
        {
            std::vector<l2d::LlmMsg> msgs;
            l2d::LlmMsg u;
            u.role = "user";
            u.content = prompt;
            msgs.push_back(u);
            std::string text, e;
            if (!l2d::LlmChat(LongGenCfg(cfg), msgs, 0.3f, true, &text, &e, &_lastCall)) return false;
            text = StripFences(text);
            jsonlite::Value v;
            if (!jsonlite::parse(text.c_str(), text.size(), v, NULL) || !v.isObj()) return false;
            *owner = v.get("owner").strOr("");
            *us = v.get("us").strOr("");
            return true;
        },
        [this](const std::string& z) { return ZoneCn(z); }, tidy, &err);
    Report("记忆", "归纳记忆（" + reason + "）", NULL, true, "", false, 0);
    _memLast = NowSec();
    if (changed)
    {
        _linesDirty = true;
        PushMemory();
        char msg[160];
        snprintf(msg, sizeof(msg), "记忆已更新（%s）：关于主人 %d 条", reason.c_str(), _mem->OwnerKnown());
        Log(msg);
    }
}

// ---------------------------------------------------------------- 节制

namespace
{

bool ThrottleSeq(jsonlite::Value* seq, double now, double lastQuestionAt, bool allowQuestions, std::string* cutWhat)
{
    jsonlite::Value* steps = ObjGetMut(seq, "steps");
    if (steps == NULL || !steps->isArr()) return false;
    bool asked = false;
    int speaks = 0;
    for (size_t i = 0; i < steps->arr.size(); ++i)
    {
        jsonlite::Value* st = &steps->arr[i];
        if (!st->isObj()) continue;
        if (ObjGet(st, "options") != NULL)
        {
            if (!allowQuestions || asked || now - lastQuestionAt < kQuestionCooldown || ObjGet(st, "speak") == NULL)
            {
                ObjDel(st, "options");
                ObjDel(st, "speak"); // 不允许提问时连问句一起去掉，避免留下无法回答的问题
                *cutWhat += cutWhat->empty() ? "提问" : "、提问";
            }
            else
            {
                asked = true;
            }
        }
        if (ObjGet(st, "speak") != NULL)
        {
            ++speaks;
            if ((speaks > kMaxSpeakPerSeg || asked) && ObjGet(st, "options") == NULL)
            {
                ObjDel(st, "speak");
                *cutWhat += cutWhat->empty() ? "台词" : "、台词";
            }
        }
    }
    int exprs = 0;
    for (size_t i = 0; i < steps->arr.size(); ++i)
    {
        jsonlite::Value* st = &steps->arr[i];
        if (!st->isObj()) continue;
        const jsonlite::Value* ex = ObjGet(st, "expression");
        if (ex != NULL && ex->isStr() && !ex->str.empty())
        {
            ++exprs;
            if (exprs > kMaxExprPerSeg)
            {
                ObjDel(st, "expression");
                *cutWhat += cutWhat->empty() ? "表情" : "、表情";
            }
        }
    }
    std::vector<jsonlite::Value> kept;
    for (size_t i = 0; i < steps->arr.size(); ++i)
    {
        if (!steps->arr[i].isObj()) continue;
        if (steps->arr[i].obj.size() == 1 && steps->arr[i].obj[0].first == "at") continue;   // 只剩 at 的空步骤
        kept.push_back(steps->arr[i]);
    }
    steps->arr = kept;
    return asked;
}

void ClampRespond(jsonlite::Value* seq)
{
    jsonlite::Value* steps = ObjGetMut(seq, "steps");
    jsonlite::Value* dur = ObjGetMut(seq, "dur");
    if (dur != NULL) dur->num = std::min(dur->numOr(kRespondMaxDur), kRespondMaxDur);
    if (steps == NULL || !steps->isArr()) return;
    std::vector<jsonlite::Value> kept;
    for (size_t i = 0; i < steps->arr.size(); ++i)
    {
        if (steps->arr[i].isObj() && GetNum(&steps->arr[i], "at", 0) <= kRespondMaxDur - 0.5)
        {
            kept.push_back(steps->arr[i]);
        }
    }
    if (kept.size() != steps->arr.size())
    {
        jsonlite::Value tail;
        tail.setType(jsonlite::Value::OBJ);
        jsonlite::Value at, pose, ex;
        at.setType(jsonlite::Value::NUM);
        at.num = kRespondMaxDur - 0.5;
        pose.setType(jsonlite::Value::OBJ);
        ex.setType(jsonlite::Value::STR);
        ex.str = "";
        tail.obj.push_back(std::make_pair(std::string("at"), at));
        tail.obj.push_back(std::make_pair(std::string("pose"), pose));
        tail.obj.push_back(std::make_pair(std::string("expression"), ex));
        kept.push_back(tail);
    }
    steps->arr = kept;
}

}   // namespace

// ---------------------------------------------------------------- LLM 生成

/** 她说的话跟着界面语言走（config.str.lang）：**只加在会产出"说出口的话"的提示词里**
 *  （对话段 speak / 台词库 / 笑话 / 提问，以及性格——性格会进所有提示词）。
 *  中文/缺省返回空串：提示词与以前逐字节一致。 */
std::string PetAgent::SpeechLangHint() const
{
    const std::string l = LangResolve(_cfgLang);
    if (l == "zh")
    {
        // 提示词本身就是中文，但模型仍会偶尔整段写英文（笑话、台词库都踩过：
        // "Boss, I told my snack bowl a secret…"）。点明一句，成本极低。
        return "\n【语言】说出口的内容（speak 台词、台词库里的句子、笑话、提问与气泡选项）一律用简体中文写，"
               "不要写成英文句子。\n";
    }
    if (l == "ja")
    {
        return "\n【语言】主人把界面语言设成了日语：**你说出口的内容一律用自然的日语写**"
               "（提示词里凡出现「中文」「中文台词」等字样，一律按日语理解）"
               "（speak 台词、台词库里的句子、笑话、提问与气泡选项）；不要写中文。"
               "能力表/记忆里的中文只当资料看，不要照抄。**每一句从头到尾都必须是日语，绝不能中日混写**"
               "（例：「やめてよ、脸会扁」是错的，应写「やめてよ、顔がつぶれちゃう」）；混了中文的句子会被直接丢弃。\n";
    }
    return "\n【语言】主人把界面语言设成了英语：**你说出口的内容一律用自然英语写**"
           "（提示词里凡出现「中文」「中文台词」等字样，一律按英语理解）"
           "（speak 台词、台词库里的句子、笑话、提问与气泡选项）；不要写中文。"
           "能力表/记忆里的中文只当资料看，不要照抄。**每一句都必须全是英语，不能夹任何中文字**；夹了中文的句子会被直接丢弃。\n";
}

/** 界面语言变了：把**存起来的内容**全换成新语言（面板切语言时由 ApplyConfig 排队触发）。
 *  · 记忆展示（面板"记忆"页那份）：LLM 原样改写（结构/条数不变，只换语言）
 *  · 性格：LLM 版才重写（本地兜底那份没有语言差异）
 *  · 台词库 / 题库 / 笑话池：**清掉重生成**（旧内容是旧语言，留着会混着说）
 *  睡着/暂停时不调用（排队等到醒来；见主循环的 _defer* 段）。 */
void PetAgent::LangRewriteAll(const std::string& lang)
{
    const std::string l = LangResolve(lang);
    const std::string cn = LangCnName(l);
    Log("界面语言 → " + cn + "：开始用新语言重写记忆与离线台词…");
    bool did = false;

    // ① 记忆展示：改写（不是压缩——条数与时间戳都要原样）
    const l2d::LlmSettings cfg = l2d::LlmLoadSettings();
    if (_mem != NULL && !cfg.key.empty())
    {
        const l2d::LlmSettings cfg2 = cfg;
        std::string err, report;
        const bool ok = _mem->Rewrite(
            l,
            [this, cfg2](const std::string& prompt, std::string* owner, std::string* us) -> bool {
                std::vector<l2d::LlmMsg> msgs;
                l2d::LlmMsg s;
                s.role = "system";
                s.content = "你是记忆改写器：只换语言、不改内容结构。只输出 JSON。";
                msgs.push_back(s);
                l2d::LlmMsg u;
                u.role = "user";
                u.content = prompt;
                msgs.push_back(u);
                std::string text, e;
                if (!l2d::LlmChat(LongGenCfg(cfg2), msgs, 0.2f, true, &text, &e, &_lastCall)) return false;
                text = StripFences(text);
                jsonlite::Value v;
                if (!jsonlite::parse(text.c_str(), text.size(), v, NULL) || !v.isObj()) return false;
                *owner = v.get("owner").strOr("");
                *us = v.get("us").strOr("");
                return !owner->empty() || !us->empty();
            },
            &err, &report);
        if (ok)
        {
            PushMemory();   // 面板"记忆"页立刻看到新语言的那份
            Log("记忆已用" + cn + "重写（" + report + "）");
            did = true;
        }
        else
        {
            Log("记忆重写失败：" + err + "（保留原文）");
        }
    }

    // ② 性格：只有 LLM 版需要重写（本地兜底是代码里写死的，没有语言之分）
    if (!_persona.Empty() && _persona.Get().by == "llm")
    {
        OpenPersona(true);
        did = true;
    }

    // ③ 台词库（即时台词跟着语言）
    _linesOk = false;
    PushLines("语言切换");
    did = true;

    // ④ 题库 / ⑤ 笑话池：清掉再生成（不然新语言里混着旧语言的句子）
    _questions.Reset();
    OpenQuestions(true);
    _jokes.Reset();
    _jokesLastGenAt = 0.0;
    OpenJokes(true);

    char b[192];
    snprintf(b, sizeof(b), "语言切换完成（%s）：%s", cn.c_str(), did ? "已重写记忆/性格/台词库/题库/笑话" : "没有可重写的内容");
    Log(b);
    PushLines("语言切换后");   // 再推一次：性格/题库/笑话都可能带出新的台词素材
    ResetConversation("语言切换完成");   // 重写期间若有段落，也是混着旧记忆写的：再清一次，用新语言从头聊
}

void PetAgent::ClearRoleContext(const std::string& why)
{
    _deferPersona = false;
    _deferPersonaGuidance.clear();
    _deferCompress = false;
    _backlog[0].clear();
    _backlog[1].clear();
    _lastHover.clear();
    _weatherCadence.Reset(NowSec());
    ResetConversation(why);
}

/** 新开一轮对话：丢掉多轮上下文和与之相关的状态（上段警告、待答的问题），下一段只看系统提示 + 记忆 */
void PetAgent::ResetConversation(const std::string& why)
{
    _history.clear();
    _lastWarn.clear();
    _lastQText.clear();
    _lastQOpts.clear();
    _awaitingUntil = 0.0;
    _historyLang = LangResolve(_cfgLang);
    Rpc("{\"cmd\":\"reset\",\"model\":\"" + JsonEscape(_caps.modelFv) + "\"}");
    Log("新开一轮 LLM 对话（" + why + "）");
}

/** 题库缓存的语言和当前不符？只看"会拿来说的"部分：没问过的候选 + 已问过的历史 */
bool PetAgent::QuestionsStaleLang() const
{
    const std::string want = LangResolve(_cfgLang);
    if (want == "zh" || _questions.Empty()) return false;
    const std::vector<const PetQuestions::Item*> f = _questions.Fresh(4);
    for (size_t i = 0; i < f.size(); ++i)
    {
        if (!SpeechLangOk(f[i]->q, want)) return true;
    }
    const std::string kt = _questions.KnownText();
    return !kt.empty() && !SpeechLangOk(kt, want);
}

/** 性格缓存的语言和当前不符？本地兜底版（by != llm）是按能力解算的中性模板，不算 */
bool PetAgent::PersonaStaleLang() const
{
    const std::string want = LangResolve(_cfgLang);
    if (want == "zh" || _persona.Empty() || _persona.Get().by != "llm") return false;
    const PetPersona::Data& d = _persona.Get();
    return !SpeechLangOk(d.persona + " " + d.speech, want);
}

bool PetAgent::LlmSequence(const std::string& note, const std::string& memory, jsonlite::Value* seqOut, std::string* err)
{
    l2d::LlmSettings cfg = l2d::LlmLoadSettings();
    const l2d::TokenBudget budget = l2d::BudgetForTier(_cfgTokenTier);
    cfg.maxReplyTokens = budget.replyTokens;
    std::vector<l2d::LlmMsg> msgs;
    {
        l2d::LlmMsg sys;
        sys.role = "system";
        sys.content = LangFixup(std::string(kSystemPromptHead) + _persona.PromptBlock() + _caps.raw + kSystemPromptTail,
                                LangResolve(_cfgLang))
                      + SpeechLangHint()
                      + "\n天气只是偶尔的闲聊素材。除非主人主动询问或本段标有【天气闲聊】，不要主动聊天气、温度、降雨、穿衣或带伞；换一个贴近当前互动的话题。";
        const size_t m = sys.content.find("{model}");
        if (m != std::string::npos) sys.content.replace(m, 7, _caps.modelFv);
        msgs.push_back(sys);
    }
    if (_cfgTokenTier > 0)
    {
        l2d::LlmMsg concise;
        concise.role = "system";
        concise.content = "保持要求的 JSON 格式，最多 " + std::to_string(budget.steps)
            + " 个 steps；台词简短自然，不重复背景说明。保留当前问题的必要回答。";
        msgs.push_back(concise);
    }
    if (!memory.empty())
    {
        l2d::LlmMsg mem;
        mem.role = "system";
        mem.content = "【你记得的事（跨会话记忆，自动整理；可能不全，别生硬复述）】\n" + memory
                      + "\n用法（**不只是聊天**）：记忆里的称呼/作息/口味/爱好/心情要体现在**所有交互**里——\n"
                        "· 台词可以偶尔自然呼应往事或主人的偏好（大多数时候自然聊天就好）；\n"
                        "· 称呼用主人认可的那个；他讨厌的事别做、喜欢的东西可以顺势提；\n"
                        "· 选动作/表情/视线时也可照顾他的偏好与当前作息（比如他在忙就安静些、他爱猫就用猫系姿态）；\n"
                        "· 你觉得重要但记忆里没有的，用提问慢慢弄清（问题见闲时段的候选题）。\n";
        msgs.push_back(mem);
    }
    if (note.find("【天气】") != std::string::npos || note.find("【天气闲聊】") != std::string::npos)
    {
        l2d::LlmMsg weather;
        weather.role = "system";
        weather.content = _weather.Context(_cfgWeatherLoc, (double)time(NULL));
        msgs.push_back(weather);
    }
    // 兜底：多轮上下文是别的语言写的（无论语言是从哪儿改的）→ 丢掉，按当前语言新开一轮
    if (!_history.empty() && _historyLang != LangResolve(_cfgLang))
    {
        ResetConversation("上下文语言与当前语言不一致");
    }
    if (_history.empty()) _historyLang = LangResolve(_cfgLang);
    const size_t keep = (size_t)budget.historyMessages;
    for (size_t i = _history.size() > keep ? _history.size() - keep : 0; i < _history.size(); ++i)
    {
        msgs.push_back(_history[i]);
    }
    {
        l2d::LlmMsg u;
        u.role = "user";
        u.content = note;
        msgs.push_back(u);
    }
    for (int attempt = 0; attempt < 3; ++attempt)
    {
        std::string text, e;
        // 对话段保持默认 60 秒超时：卡住时别把回应堵太久（长生成才用 LongGenCfg）
        if (!l2d::LlmChat(cfg, msgs, 0.9f, true, &text, &e, &_lastCall))
        {
            *err = e;
            return false;
        }
        text = StripFences(text);
        jsonlite::Value seq;
        if (jsonlite::parse(text.c_str(), text.size(), seq, NULL) && seq.isObj())
        {
            jsonlite::Value cmd, model;
            cmd.setType(jsonlite::Value::STR);
            cmd.str = "sequence";
            model.setType(jsonlite::Value::STR);
            model.str = _caps.modelFv;
            ObjSet(&seq, "cmd", cmd);
            ObjSet(&seq, "model", model);
            *seqOut = seq;
            l2d::LlmMsg hu, ha;
            hu.role = "user";
            hu.content = note;
            ha.role = "assistant";
            ha.content = ValueToJson(seq);
            _history.push_back(hu);
            _history.push_back(ha);
            while (_history.size() > 12) _history.erase(_history.begin());
            return true;
        }
        char em[128];
        snprintf(em, sizeof(em), "LLM 输出不是合法 JSON，重试 %d/2", attempt + 1);
        Log(em);
        l2d::LlmMsg bad, fix;
        bad.role = "assistant";
        bad.content = text;
        fix.role = "user";
        fix.content = "上面的输出不是合法 JSON。请只输出修正后的完整 JSON。";
        msgs.push_back(bad);
        msgs.push_back(fix);
    }
    *err = "LLM 连续 3 次输出不合法 JSON";
    return false;
}

bool PetAgent::LlmReactLines(const std::string& memory, jsonlite::Value* linesOut, std::string* err)
{
    const l2d::LlmSettings cfg = l2d::LlmLoadSettings();
    if (cfg.key.empty())
    {
        *err = "没有 API Key";
        return false;
    }
    std::string zones;
    for (std::map<std::string, std::string>::const_iterator it = _caps.zoneZh.begin(); it != _caps.zoneZh.end(); ++it)
    {
        if (!zones.empty()) zones += "、";
        zones += it->second + "(" + it->first + ")";
    }
    std::string mb;
    if (!memory.empty())
    {
        mb = "\n- 你记得的事（跨会话记忆）——每组台词里最多 1 句可以自然呼应它（主人的称呼、"
             "一起经历过的小事、偏好），其余句子保持通用：这些台词会被长期反复播放，"
             "太具体会让主人听腻、也会露馅。\n" + memory + "\n";
    }
    // 角色描述 = 模型名 + 这个模型的性格（PetPersona）：台词库要按她的口吻写
    const std::string prompt = LangFixup(std::string(kLinesPromptA) + _persona.LinesRole() + kLinesPromptB + zones
                                             + kLinesPromptC + mb,
                                         LangResolve(_cfgLang))
                               + SpeechLangHint();
    std::vector<l2d::LlmMsg> msgs;
    l2d::LlmMsg u;
    u.role = "user";
    u.content = prompt;
    msgs.push_back(u);
    std::string text, e;
    jsonlite::Value v;
    bool parsed = false;
    for (int attempt = 0; attempt < 2 && !parsed; ++attempt)   // 偶发输出坏 JSON（日/英长输出更常见）：重试一次
    {
        if (!l2d::LlmChat(LongGenCfg(cfg), msgs, 0.9f, true, &text, &e, &_lastCall))
        {
            *err = e;
            return false;
        }
        text = StripFences(text);
        const size_t b0 = text.find('{'), b1 = text.rfind('}');   // 只取最外层 {...}（前后有说明文字也能用）
        if (b0 != std::string::npos && b1 != std::string::npos && b1 > b0) text = text.substr(b0, b1 - b0 + 1);
        parsed = jsonlite::parse(text.c_str(), text.size(), v, NULL) && v.isObj();
        if (!parsed)
        {
            Log("台词库输出不是合法 JSON（开头：" + plat::Utf8Head(text, 60) + "…）" + (attempt == 0 ? "，重试一次" : ""));
        }
    }
    if (!parsed)
    {
        *err = "台词库输出不是合法 JSON";
        return false;
    }
    const jsonlite::Value* lines = ObjGet(&v, "lines");
    if (lines == NULL || !lines->isObj())
    {
        *err = "台词库输出缺少 lines";
        return false;
    }
    jsonlite::Value out;
    out.setType(jsonlite::Value::OBJ);
    int langDropped = 0;
    for (size_t i = 0; i < lines->obj.size(); ++i)
    {
        const jsonlite::Value& arr = lines->obj[i].second;
        if (!arr.isArr()) continue;
        jsonlite::Value keep;
        keep.setType(jsonlite::Value::ARR);
        for (size_t j = 0; j < arr.arr.size() && keep.arr.size() < 6; ++j)
        {
            const std::string t = StripStamps(arr.arr[j].strOr(""));
            if (t.empty() || Utf8Len(t) > 20) continue;
            if (!SpeechLangOk(t, LangResolve(_cfgLang)))   // 日/英模式下夹了中文（"やめてよ、脸会扁"）：丢掉
            {
                ++langDropped;
                continue;
            }
            jsonlite::Value one;
            one.setType(jsonlite::Value::STR);
            one.str = t;
            keep.arr.push_back(one);
        }
        if (!keep.arr.empty()) out.obj.push_back(std::make_pair(lines->obj[i].first, keep));
    }
    if (langDropped > 0)
    {
        Log("台词库：丢掉 " + std::to_string(langDropped) + " 句不是" + LangCnName(LangResolve(_cfgLang))
            + "的（缺的组用桌宠内置的本地化台词兜底）");
    }
    *linesOut = out;
    return true;
}

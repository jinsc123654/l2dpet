/**
 * PetMemory.cpp — 见 PetMemory.h（pet_memory.py 的忠实移植）
 */
#include "PetI18n.h"
#include "PetMemory.h"
#include "PetVerbs.h"
#include "Platform.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <ctime>
#include <sys/stat.h>

#include <algorithm>
#include <set>
#include <sstream>

#ifndef _WIN32
#include <unistd.h>
#endif

namespace
{

const size_t kOwnerMax = 1600;
const size_t kCharMax = 5600;
const char* kEmptyOwner = "## 关于主人\n（还不了解）";
const char* kEmptyChar = "## 最近发生\n（还没有）\n\n## 更早\n（空）";

std::string HomePath(const char* rel)
{
    const char* home = getenv("HOME");
#ifdef _WIN32
    if (home == NULL) home = getenv("USERPROFILE");
#endif
    return std::string(home != NULL ? home : "/tmp") + rel;
}

double NowSec()
{
    return (double)time(NULL);
}

std::string FmtDate(const char* fmt)
{
    time_t t = time(NULL);
    struct tm lt;
#ifdef _WIN32
    localtime_s(&lt, &t);
#else
    localtime_r(&t, &lt);
#endif
    char buf[64];
    strftime(buf, sizeof(buf), fmt, &lt);
    return buf;
}

std::string ReadText(const std::string& path)
{
    FILE* f = fopen(path.c_str(), "rb");
    if (f == NULL) return "";
    std::string text;
    char buf[4096];
    size_t r;
    while ((r = fread(buf, 1, sizeof(buf), f)) > 0) text.append(buf, r);
    fclose(f);
    return text;
}

void EnsureDir(const std::string& dir)
{
    plat::CreateDir(dir);   // Windows _mkdir / POSIX mkdir(0700)
#ifndef _WIN32
    chmod(dir.c_str(), 0700);   // 已存在但权限松的也收回来
#endif
}

/** 0600 原子写（tmp + rename）。Linux/macOS 生效；Windows 上写文件但权限语义不同。 */
void WritePrivate(const std::string& path, const std::string& text)
{
    const size_t slash = path.rfind('/');
    if (slash != std::string::npos) EnsureDir(path.substr(0, slash));
    plat::WritePrivateFile(path, text);   // 0600 创建 + 原子替换（Windows 也能覆盖已有文件）
}

void AppendPrivate(const std::string& path, const std::string& line)
{
    const size_t slash = path.rfind('/');
    if (slash != std::string::npos) EnsureDir(path.substr(0, slash));
    FILE* f = fopen(path.c_str(), "ab");
    if (f == NULL) return;
    fwrite(line.data(), 1, line.size(), f);
    fwrite("\n", 1, 1, f);
    fclose(f);
}

std::string TrimCopy(const std::string& s)
{
    size_t a = 0, b = s.size();
    while (a < b && (unsigned char)s[a] <= ' ') ++a;
    while (b > a && (unsigned char)s[b - 1] <= ' ') --b;
    return s.substr(a, b - a);
}

bool StartsWith(const std::string& s, const char* prefix)
{
    const size_t n = strlen(prefix);
    return s.size() >= n && s.compare(0, n, prefix) == 0;
}

bool Contains(const std::string& s, const char* needle)
{
    return s.find(needle) != std::string::npos;
}

std::vector<std::string> SplitLines(const std::string& text)
{
    std::vector<std::string> out;
    std::string cur;
    for (size_t i = 0; i < text.size(); ++i)
    {
        if (text[i] == '\n')
        {
            out.push_back(cur);
            cur.clear();
        }
        else
        {
            cur += text[i];
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

/** 字节二元组集合（近似 Python 的字符二元组：对 CJK 相似度同样有效）*/
std::set<std::string> Bigrams(const std::string& raw)
{
    std::string s;
    for (size_t i = 0; i < raw.size(); ++i)
    {
        const unsigned char c = (unsigned char)raw[i];
        if (c <= ' ') continue;   // 去空白
        s += raw[i];
    }
    std::set<std::string> out;
    for (size_t i = 0; i + 1 < s.size(); ++i) out.insert(s.substr(i, 2));
    return out;
}

std::string JsonEscape(const std::string& s)
{
    std::string out;
    for (size_t i = 0; i < s.size(); ++i)
    {
        const unsigned char c = (unsigned char)s[i];
        if (c == '"') out += "\\\"";
        else if (c == '\\') out += "\\\\";
        else if (c == '\n') out += "\\n";
        else if (c < 0x20) { char b[8]; snprintf(b, sizeof(b), "\\u%04x", c); out += b; }
        else out += (char)c;
    }
    return out;
}

/** 角色文件名：只留字母数字/下划线/连字符/CJK（防路径穿越），≤64 字节 */
std::string SafeName(const std::string& in)
{
    std::string out;
    for (size_t i = 0; i < in.size() && out.size() < 64; ++i)
    {
        const unsigned char c = (unsigned char)in[i];
        const bool ok = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')
                        || c == '_' || c == '-' || c >= 0x80;   // ≥0x80：放行 UTF-8（CJK 等）
        out += ok ? in[i] : '_';
    }
    return out.empty() ? "pet" : out;
}

jsonlite::Value* ObjFind(jsonlite::Value* obj, const std::string& key)
{
    if (obj == NULL || !obj->isObj()) return NULL;
    for (size_t i = 0; i < obj->obj.size(); ++i)
    {
        if (obj->obj[i].first == key) return &obj->obj[i].second;
    }
    return NULL;
}

const jsonlite::Value* ObjFindC(const jsonlite::Value* obj, const std::string& key)
{
    return ObjFind(const_cast<jsonlite::Value*>(obj), key);
}

}   // namespace

PetMemory::PetMemory(const std::string& charId, const std::string& charName)
    : _charId(SafeName(charId.empty() ? "pet" : charId)), _name(charName.empty() ? charId : charName)
{
    _root = HomePath("/.config/l2dpet/memory");
    _charsDir = _root + "/chars";
    // 递归建好 memory/ 与 memory/chars：下面的 EnsureDir 只有一层 mkdir（CreateDir 默认非递归），
    // 父目录不存在时 mkdir("…/chars") 直接 ENOENT —— 全新配置目录上写记忆/习惯会**静默失败**
    // （memory_isolation_test 在干净 HOME 上也因此挂过）。
    plat::CreateDir(_charsDir, true);
    _ownerPath = _root + "/owner.md";
    _charPath = _charsDir + "/" + _charId + ".md";
    _habitsPath = _charsDir + "/" + _charId + ".habits.json";
    _pendingPath = _charsDir + "/" + _charId + ".pending.jsonl";
    _episodesPath = _charsDir + "/" + _charId + ".episodes.jsonl";
    MigrateLegacy();
    // habits
    _habits.setType(jsonlite::Value::OBJ);
    const std::string ht = ReadText(_habitsPath);
    if (!ht.empty())
    {
        jsonlite::Value v;
        if (jsonlite::parse(ht.c_str(), ht.size(), v, NULL) && v.isObj()) _habits = v;
    }
    // 本次会话间隔（没有统计时按记忆文件的 mtime 算，别把老朋友当初次见面）
    double last = ObjFindC(&_habits, "last_seen") != NULL ? ObjFindC(&_habits, "last_seen")->numOr(0) : 0;
    struct stat st;
    if (last <= 0)
    {
        if (_legacyMtime > 0) last = _legacyMtime;
        else if (stat(_charPath.c_str(), &st) == 0 && Us() != TrimCopy(kEmptyChar)) last = (double)st.st_mtime;
    }
    _gap = last > 0 ? NowSec() - last : -1.0;
    if (_gap < 0 || _gap > 1800)   // 离开 >30 分钟才算"新的一次见面"
    {
        double sessions = ObjFindC(&_habits, "sessions") != NULL ? ObjFindC(&_habits, "sessions")->numOr(0) : 0;
        jsonlite::Value sv; sv.setType(jsonlite::Value::NUM); sv.num = sessions + 1;
        _habits.obj.push_back(std::make_pair(std::string("sessions"), sv));
    }
    if (ObjFindC(&_habits, "first_seen") == NULL)
    {
        jsonlite::Value fv; fv.setType(jsonlite::Value::STR); fv.str = FmtDate("%Y-%m-%d");
        _habits.obj.push_back(std::make_pair(std::string("first_seen"), fv));
    }
    SaveHabits(true);
}

/** 按字节上限截断但退到 UTF-8 字符边界（防切断多字节字符 → 下游 JSON 编码炸）*/
std::string TruncAtBoundary(const std::string& s, size_t maxBytes)
{
    if (s.size() <= maxBytes) return s;
    size_t cut = maxBytes;
    while (cut > 0 && (unsigned char)s[cut] >= 0x80 && (unsigned char)s[cut] < 0xC0) --cut;   // 退过续字节
    // substr excludes s[cut]; backing up past the leading byte would split
    // the preceding character instead.
    return s.substr(0, cut);
}

std::string PetMemory::Owner() const
{
    std::string s = ReadText(_ownerPath);
    if (s.empty()) s = kEmptyOwner;
    s = TrimCopy(s);
    return TruncAtBoundary(s, kOwnerMax);
}

std::string PetMemory::Us() const
{
    std::string s = ReadText(_charPath);
    if (s.empty()) s = kEmptyChar;
    s = TrimCopy(s);
    return TruncAtBoundary(s, kCharMax);
}

int PetMemory::OwnerKnown() const
{
    // 只数"主人亲自告诉过的"：统计推断（看起来）和默认称呼不算了解
    int n = 0;
    const std::vector<std::string> ownerLines = SplitLines(Owner());   // 临时 vector 必须先存局部变量
    for (std::vector<std::string>::const_iterator it = ownerLines.begin(); it != ownerLines.end(); ++it)
    {
        const std::string ln = TrimCopy(*it);
        if (!StartsWith(ln.c_str(), "-")) continue;
        if (Contains(ln, "还不了解") || Contains(ln, "未知") || Contains(ln, "不清楚")
            || Contains(ln, "（无）") || Contains(ln, "（看起来）")
            || ln == "- 称呼:主人" || ln == "- 称呼：主人")
        {
            continue;
        }
        ++n;
    }
    return n;
}

std::string PetMemory::HabitsText(const ZoneNameFn& zoneCn) const
{
    std::vector<std::string> parts;
    const jsonlite::Value* zones = ObjFindC(&_habits, "zones");
    if (zones != NULL && zones->isObj())
    {
        long tot = 0;
        for (size_t i = 0; i < zones->obj.size(); ++i)
        {
            const jsonlite::Value& rec = zones->obj[i].second;
            if (!rec.isArr() || rec.arr.empty()) continue;
            tot += (long)(rec.arr[0].numOr(0) + (rec.arr.size() > 1 ? rec.arr[1].numOr(0) : 0));
        }
        if (tot >= 5)
        {
            std::vector<std::pair<long, size_t> > top;
            for (size_t i = 0; i < zones->obj.size(); ++i)
            {
                const jsonlite::Value& rec = zones->obj[i].second;
                if (!rec.isArr() || rec.arr.empty()) continue;
                const long c = (long)rec.arr[0].numOr(0);
                const long r = (long)(rec.arr.size() > 1 ? rec.arr[1].numOr(0) : 0);
                top.push_back(std::make_pair(c + r, i));
            }
            std::sort(top.begin(), top.end(), std::greater<std::pair<long, size_t> >());
            std::string desc;
            for (size_t i = 0; i < top.size() && i < 3; ++i)
            {
                const jsonlite::Value& rec = zones->obj[top[i].second].second;
                const long c = (long)rec.arr[0].numOr(0);
                const long r = (long)(rec.arr.size() > 1 ? rec.arr[1].numOr(0) : 0);
                // 动词与部位各自按语言翻（en 的动词值自带尾随空格，拼起来就是 "stroked head"）
                const std::string verb = peti18n::Tr(TouchVerb(zones->obj[top[i].second].first.c_str(), r >= c));
                const std::string zone = peti18n::Tr(zoneCn ? zoneCn(zones->obj[top[i].second].first)
                                                            : zones->obj[top[i].second].first);
                if (!desc.empty()) desc += peti18n::Get() == peti18n::Lang::En ? ", " : "、";
                desc += verb + zone + " " + std::to_string((long)(100 * (c + r) / tot)) + "%";
            }
            parts.push_back(peti18n::TF("主人最常{}（共 {} 次）", { desc, std::to_string(tot) }));
        }
    }
    const jsonlite::Value* hours = ObjFindC(&_habits, "hours");
    if (hours != NULL && hours->isArr() && hours->arr.size() >= 24)
    {
        long sum = 0;
        for (int i = 0; i < 24; ++i) sum += (long)hours->arr[i].numOr(0);
        if (sum >= 8)
        {
            int peak = 0;
            long best = -1;
            for (int i = 0; i < 24; ++i)
            {
                const long v = (long)hours->arr[i].numOr(0) + (long)hours->arr[(i + 1) % 24].numOr(0);
                if (v > best)
                {
                    best = v;
                    peak = i;
                }
            }
            parts.push_back(peti18n::TF("常在 {}–{} 点来找我",
                                        { std::to_string(peak), std::to_string((peak + 2) % 24) }));
        }
    }
    const jsonlite::Value* sessions = ObjFindC(&_habits, "sessions");
    if (sessions != NULL && sessions->numOr(0) > 0)
    {
        char line[96];
        const std::string first = ObjFindC(&_habits, "first_seen") != NULL ? ObjFindC(&_habits, "first_seen")->strOr("?")
                                                                        : "?";
        char nbuf[32];
        snprintf(nbuf, sizeof(nbuf), "%.0f", sessions->numOr(0));
        parts.push_back(peti18n::TF("从 {} 起见过 {} 次面", { first, nbuf }));
    }
    const jsonlite::Value* choices = ObjFindC(&_habits, "choices");
    if (choices != NULL && choices->isArr() && !choices->arr.empty())
    {
        std::string joined;
        for (size_t i = choices->arr.size() >= 4 ? choices->arr.size() - 4 : 0; i < choices->arr.size(); ++i)
        {
            if (!joined.empty()) joined += "；";
            joined += choices->arr[i].strOr("");
        }
        parts.push_back("主人选过：" + joined);
    }
    std::string out;
    for (size_t i = 0; i < parts.size(); ++i)
    {
        if (i) out += "；";
        out += parts[i];
    }
    return out;
}

std::string PetMemory::Context(const std::string& query, const ZoneNameFn& zoneCn) const
{
    std::vector<std::string> blk;
    blk.push_back(Owner());
    // "## " → "### "
    std::string us = Us();
    for (size_t p = 0; (p = us.find("## ", p)) != std::string::npos; p += 4) us.replace(p, 3, "### ");
    blk.push_back("## 我和主人之间\n" + us);
    const std::string ht = HabitsText(zoneCn);
    if (!ht.empty()) blk.push_back("## 习惯（统计）\n" + ht);
    const std::vector<std::string> old = Recall(query);
    if (!old.empty())
    {
        std::string joined;
        for (size_t i = 0; i < old.size(); ++i) joined += (i ? "\n- " : "- ") + old[i];
        blk.push_back("## 可能相关的往事（归档）\n" + joined);
    }
    std::string out;
    for (size_t i = 0; i < blk.size(); ++i)
    {
        if (blk[i].empty()) continue;
        if (i) out += "\n\n";
        out += blk[i];
    }
    return out;
}

std::string PetMemory::PanelText(const ZoneNameFn& zoneCn) const
{
    // 段标题与占位都是"自带文案" → 按界面语言翻（正文是她自己那份记忆，已经跟着语言走过一遍）
    const std::string ht = HabitsText(zoneCn).empty() ? peti18n::Tr("（还在攒）") : HabitsText(zoneCn);
    const size_t pend = PendingAll().size();
    std::string us = Us();
    for (size_t p = 0; (p = us.find("## ", p)) != std::string::npos; p += 5) us.replace(p, 3, "## · ");
    std::string out = Owner() + "\n\n## " + peti18n::Tr("我和主人之间") + "（" + _name + "）\n" + us
                      + "\n\n## " + peti18n::Tr("习惯") + peti18n::Tr("（统计，程序生成）") + "\n" + ht;
    if (pend > 0)
    {
        out += "\n\n## " + peti18n::Tr("待整理") + "\n"
               + peti18n::TF("还有 {} 条新鲜事等着记进去", { std::to_string(pend) });
    }
    return out;
}

std::vector<std::string> PetMemory::Recall(const std::string& query, int k) const
{
    std::vector<std::string> out;
    const std::set<std::string> q = Bigrams(query);
    if (q.size() < 2) return out;
    const std::set<std::string> cur = Bigrams(Us());
    std::vector<std::pair<double, std::string> > scored;
    const std::string eps = ReadText(_episodesPath);
    std::vector<std::string> lines = SplitLines(eps);
    if (lines.size() > 2000) lines.erase(lines.begin(), lines.end() - 2000);
    for (size_t i = 0; i < lines.size(); ++i)
    {
        jsonlite::Value ep;
        if (!jsonlite::parse(lines[i].c_str(), lines[i].size(), ep, NULL) || !ep.isObj()) continue;
        const std::string t = ep.get("text").strOr("");
        const std::set<std::string> b = Bigrams(t);
        if (b.empty()) continue;
        size_t inter = 0;
        for (std::set<std::string>::const_iterator it = q.begin(); it != q.end(); ++it) inter += b.count(*it);
        const double s = (double)inter / (sqrt((double)q.size()) * sqrt((double)b.size()));
        if (s > 0.12)
        {
            size_t inCur = 0;
            for (std::set<std::string>::const_iterator it = b.begin(); it != b.end(); ++it) inCur += cur.count(*it);
            if ((double)inCur / (double)b.size() < 0.6)   // 和工作记忆大半重合 = 已经记着了
            {
                scored.push_back(std::make_pair(s, std::string(ep.get("d").strOr("")) + " " + t));
            }
        }
    }
    std::sort(scored.begin(), scored.end(),
              [](const std::pair<double, std::string>& a, const std::pair<double, std::string>& b)
              { return a.first > b.first; });
    std::set<std::string> seen;
    for (size_t i = 0; i < scored.size() && (int)out.size() < k; ++i)
    {
        std::string t = scored[i].second;
        t = plat::Utf8Head(t, 80);
        if (seen.insert(t).second) out.push_back(t);
    }
    return out;
}

void PetMemory::Note(const std::string& text, int weight)
{
    std::string t = text;
    t = plat::Utf8Head(t, 200);
    std::string line = "{\"d\":\"" + JsonEscape(FmtDate("%m-%d %H:%M")) + "\",\"w\":" + std::to_string(weight)
                       + ",\"text\":\"" + JsonEscape(t) + "\"}";
    AppendPrivate(_pendingPath, line);
}

void PetMemory::Touch(const std::string& zone, bool rub)
{
    jsonlite::Value* zones = ObjFind(&_habits, "zones");
    if (zones == NULL)
    {
        jsonlite::Value zv;
        zv.setType(jsonlite::Value::OBJ);
        _habits.obj.push_back(std::make_pair(std::string("zones"), zv));
        zones = ObjFind(&_habits, "zones");
    }
    if (zones == NULL || !zones->isObj()) return;
    jsonlite::Value* rec = ObjFind(zones, zone);
    if (rec == NULL || !rec->isArr() || rec->arr.size() < 2)
    {
        jsonlite::Value rv;
        rv.setType(jsonlite::Value::ARR);
        jsonlite::Value c, r;
        c.setType(jsonlite::Value::NUM);
        r.setType(jsonlite::Value::NUM);
        c.num = 0;
        r.num = 0;
        rv.arr.push_back(c);
        rv.arr.push_back(r);
        zones->obj.push_back(std::make_pair(zone, rv));
        rec = ObjFind(zones, zone);
    }
    if (rec == NULL) return;
    rec->arr[rub ? 1 : 0].num = rec->arr[rub ? 1 : 0].numOr(0) + 1;
    jsonlite::Value* hours = ObjFind(&_habits, "hours");
    if (hours == NULL || !hours->isArr() || hours->arr.size() != 24)
    {
        jsonlite::Value hv;
        hv.setType(jsonlite::Value::ARR);
        for (int i = 0; i < 24; ++i)
        {
            jsonlite::Value z;
            z.setType(jsonlite::Value::NUM);
            z.num = 0;
            hv.arr.push_back(z);
        }
        _habits.obj.push_back(std::make_pair(std::string("hours"), hv));
        hours = ObjFind(&_habits, "hours");
    }
    if (hours != NULL)
    {
        const int hour = atoi(FmtDate("%H").c_str());
        if (hour >= 0 && hour < 24) hours->arr[hour].num = hours->arr[hour].numOr(0) + 1;
    }
    std::pair<int, int>& s = _sessionTouch[zone];   // operator[] 默认构造 (0,0)
    if (rub) s.second += 1;
    else s.first += 1;
    _habitsDirty = true;
    SaveHabits();
}

void PetMemory::ChoiceMade(const std::string& question, const std::string& label)
{
    jsonlite::Value* choices = ObjFind(&_habits, "choices");
    if (choices == NULL)
    {
        jsonlite::Value cv;
        cv.setType(jsonlite::Value::ARR);
        _habits.obj.push_back(std::make_pair(std::string("choices"), cv));
        choices = ObjFind(&_habits, "choices");
    }
    if (choices != NULL && choices->isArr())
    {
        std::string q = question;
        q = plat::Utf8Head(q, 20);
        jsonlite::Value one;
        one.setType(jsonlite::Value::STR);
        one.str = q.empty() ? label : "「" + q + "」→" + label;
        choices->arr.push_back(one);
        while (choices->arr.size() > 12) choices->arr.erase(choices->arr.begin());
    }
    _habitsDirty = true;
    SaveHabits(true);
    std::string q = question;
    q = plat::Utf8Head(q, 40);
    Note("主人回答了我的问题：" + q + "→选了「" + label + "」", 2);
}

void PetMemory::SaveHabits(bool force)
{
    if (!_habitsDirty && !force) return;
    if (!force && NowSec() - _habitsSavedAt < 20) return;
    // last_seen
    jsonlite::Value* last = ObjFind(&_habits, "last_seen");
    if (last == NULL)
    {
        jsonlite::Value lv;
        lv.setType(jsonlite::Value::NUM);
        _habits.obj.push_back(std::make_pair(std::string("last_seen"), lv));
        last = ObjFind(&_habits, "last_seen");
    }
    if (last != NULL) last->num = NowSec();
    // 手写序列化（Value 只解析不构建的补位：obj/arr/str/num 平铺够用）
    std::string json = "{";
    for (size_t i = 0; i < _habits.obj.size(); ++i)
    {
        if (i) json += ",";
        const jsonlite::Value& v = _habits.obj[i].second;
        json += "\"" + JsonEscape(_habits.obj[i].first) + "\":";
        if (v.isStr()) json += "\"" + JsonEscape(v.str) + "\"";
        else if (v.isNum())
        {
            char b[32];
            snprintf(b, sizeof(b), "%lld", (long long)v.num);
            json += b;
        }
        else if (v.isArr())
        {
            json += "[";
            for (size_t j = 0; j < v.arr.size(); ++j)
            {
                if (j) json += ",";
                const jsonlite::Value& a = v.arr[j];
                if (a.isNum())
                {
                    char b[32];
                    snprintf(b, sizeof(b), "%lld", (long long)a.num);
                    json += b;
                }
                else if (a.isStr()) json += "\"" + JsonEscape(a.str) + "\"";
                else json += "0";
            }
            json += "]";
        }
        else if (v.isObj())
        {
            json += "{";
            for (size_t j = 0; j < v.obj.size(); ++j)
            {
                if (j) json += ",";
                const jsonlite::Value& a = v.obj[j].second;
                json += "\"" + JsonEscape(v.obj[j].first) + "\":[";
                for (size_t k2 = 0; k2 < a.arr.size() && k2 < 2; ++k2)
                {
                    if (k2) json += ",";
                    char b[32];
                    snprintf(b, sizeof(b), "%lld", (long long)a.arr[k2].numOr(0));
                    json += b;
                }
                json += "]";
            }
            json += "}";
        }
        else json += "null";
    }
    json += "}";
    WritePrivate(_habitsPath, json);
    _habitsDirty = false;
    _habitsSavedAt = NowSec();
}

void PetMemory::Heartbeat()
{
    if (NowSec() - _habitsSavedAt > 60) SaveHabits(true);
}

int PetMemory::PendingWeight() const
{
    int w = 0;
    std::vector<jsonlite::Value> p = PendingAll();
    for (size_t i = 0; i < p.size(); ++i) w += (int)p[i].get("w").numOr(1);
    return w;
}

std::vector<std::string> PetMemory::RecentChoices() const
{
    std::vector<std::string> out;
    const jsonlite::Value* choices = ObjFindC(&_habits, "choices");
    if (choices != NULL && choices->isArr())
    {
        for (size_t i = 0; i < choices->arr.size(); ++i) out.push_back(choices->arr[i].strOr(""));
    }
    return out;
}

std::vector<jsonlite::Value> PetMemory::PendingAll() const
{
    std::vector<jsonlite::Value> out;
    const std::vector<std::string> lines = SplitLines(ReadText(_pendingPath));
    for (size_t i = 0; i < lines.size(); ++i)
    {
        jsonlite::Value v;
        if (!lines[i].empty() && jsonlite::parse(lines[i].c_str(), lines[i].size(), v, NULL) && v.isObj())
        {
            out.push_back(v);
        }
    }
    return out;
}

std::string PetMemory::SessionTouchText(const ZoneNameFn& zoneCn) const
{
    if (_sessionTouch.empty()) return "";
    std::vector<std::pair<long, std::string> > items;
    for (std::map<std::string, std::pair<int, int> >::const_iterator it = _sessionTouch.begin(); it != _sessionTouch.end(); ++it)
    {
        items.push_back(std::make_pair((long)it->second.first + (long)it->second.second, it->first));
    }
    std::sort(items.begin(), items.end(), std::greater<std::pair<long, std::string> >());
    std::string out;
    for (size_t i = 0; i < items.size() && i < 6; ++i)
    {
    std::map<std::string, std::pair<int, int> >::const_iterator found = _sessionTouch.find(items[i].second);
    if (found == _sessionTouch.end()) continue;
    const std::pair<int, int>& cnt = found->second;
        std::string one = zoneCn ? zoneCn(items[i].second) : items[i].second;
        char b[64];
        if (cnt.second > 0)
        {
            snprintf(b, sizeof(b), "摸%d", cnt.second);
            one += b;
        }
        if (cnt.first > 0)
        {
            snprintf(b, sizeof(b), "戳%d", cnt.first);
            one += b;
        }
        if (i) out += "、";
        out += one;
    }
    return out;
}

bool PetMemory::Consolidate(const std::function<bool(const std::string&, std::string*, std::string*)>& llmJson,
                            const ZoneNameFn& zoneCn, bool tidy, std::string* err)
{
    const std::string ownerRaw = ReadText(_ownerPath), charRaw = ReadText(_charPath);
    const std::string pendingRaw = ReadText(_pendingPath);
    std::vector<jsonlite::Value> pend = PendingAll();
    const std::string touch = SessionTouchText(zoneCn);
    if (pend.empty() && touch.empty() && !tidy) return false;
    std::string notes;
    for (size_t i = pend.size() > 60 ? pend.size() - 60 : 0; i < pend.size(); ++i)
    {
        if (!notes.empty()) notes += "\n";
        notes += std::string(pend[i].get("d").strOr("")) + (pend[i].get("w").numOr(1) >= 2 ? " 【重要】" : "") + " "
                 + pend[i].get("text").strOr("");
    }
    if (!touch.empty()) notes += (notes.empty() ? "" : "\n") + FmtDate("%m-%d") + " 本轮身体互动统计：" + touch;
    if (notes.empty()) notes = "（没有新事：按下面的规则把现有两份记忆重新整理一遍）";
    // CONSOLIDATE_PROMPT（pet_memory.py 同文）
    const std::string prompt =
        "你是桌宠\"" + _name + "\"的\"记忆整理器\"。主人和她相处时发生的事要整理进两份记忆。\n\n"
        "现有《关于主人》（所有角色共享，只放主人本人的稳定信息）：\n<owner>\n" + Owner()
        + "\n</owner>\n\n现有《我和主人之间》（只属于 " + _name + "）：\n<us>\n" + Us()
        + "\n</us>\n\n程序统计的习惯（可信，可据此写\"主人好像…\"这类推断，但要写成推断语气）：\n"
        + (HabitsText(zoneCn).empty() ? "（无）" : HabitsText(zoneCn))
        + "\n\n最近的新事（按时间；【重要】= 主人亲自选择/表态、关系变化，务必保留）：\n<new>\n" + notes
        + "\n</new>\n\n输出一个 JSON：{\"owner\":\"<更新后的《关于主人》>\",\"us\":\"<更新后的《我和主人之间》>\"}\n"
        "《关于主人》（owner）的规则——宁缺毋滥：\n"
        "- 以 `## 关于主人` 开头，下面每条一行以 `- ` 开头，只收三类：\n"
        "  1) 主人亲自表达过的（【重要】里主人选的答案、主人的称呼、主人说过的喜好/讨厌）；\n"
        "  2) 主人和她的约定；\n"
        "  3) 习惯统计里稳定的规律，写成\"（看起来）……\"，最多 2 条（如\"（看起来）爱摸我的头\"\"（看起来）晚上常来\"）。\n"
        "- **不要**写一次性的事件（那属于 us）；**不要**揣测主人的内心或动机（如\"他好像没注意到\"\"他故意的\"）；\n"
        "  **不要**写\"作息：（还不了解）\"这类占位行——一条信息都没有时整段只写一行\"（还不了解）\"。≤400 字。\n"
        "《我和主人之间》（us）的规则：\n"
        "- 固定两段 `## 最近发生`（每条一行、以 月-日 开头、最多 10 条）和 `## 更早`（压缩成短句）。\n"
        "- 记\"关系里的事\"：生气/被哄好/被吵醒、主人的选择和她怎么接、约定、久别重逢、第一次……\n"
        "  身体互动**按天合并成一条**（如\"10-09 主人摸了很多次头，也爱戳脸，戳胸让我生了回闷气\"），不要逐次记录。\n"
        "- 同一天的同类事合并；超过 10 条把最旧的压进\"更早\"。≤1400 字。\n"
        "通用：只写已发生的事实，不编造；拿不准的不写。中文，用她的第一人称（\"我\"）称呼自己、\"主人\"称呼对方。";
    std::string owner, us;
    if (!llmJson(prompt, &owner, &us))
    {
        if (err != NULL) *err = "LLM 归纳调用失败";
        return false;
    }
    owner = TrimCopy(owner);
    us = TrimCopy(us);
    if (!StartsWith(owner.c_str(), "## 关于主人")) owner = owner.empty() ? Owner() : "## 关于主人\n" + owner;
    if (!Contains(us.c_str(), "## 最近发生"))
    {
        if (err != NULL) *err = "归纳结果缺少『## 最近发生』段，丢弃这次结果";
        return false;
    }
    if (ReadText(_ownerPath) != ownerRaw || ReadText(_charPath) != charRaw || ReadText(_pendingPath) != pendingRaw)
    {
        if (err != NULL) *err = "归纳期间记忆已变更，丢弃旧结果，保留待整理数据";
        return false;
    }
    if (owner.size() > kOwnerMax || us.size() > kCharMax || !Contains(us, "## 更早"))
    {
        if (err != NULL) *err = "归纳结果超长或结构不完整，保留原记忆与待整理数据";
        return false;
    }
    const bool changed = owner != Owner() || us != Us();
    if (ReadText(_ownerPath) != ownerRaw || ReadText(_charPath) != charRaw)
    {
        if (err != NULL) *err = "整理期间记忆已变更，丢弃旧结果";
        return false;
    }
    if (owner.size() > kOwnerMax || us.size() > kCharMax)
    {
        if (err != NULL) *err = "整理结果超长，保留原记忆，不进行硬截断";
        return false;
    }
    if (!plat::WritePrivateFile(_ownerPath, owner + "\n"))
    {
        if (err != NULL) *err = "主人记忆写入失败，保留待整理数据";
        return false;
    }
    if (!plat::WritePrivateFile(_charPath, us + "\n"))
    {
        const bool restored = plat::WritePrivateFile(_ownerPath, ownerRaw);
        if (err != NULL) *err = restored ? "角色记忆写入失败，已恢复主人记忆" : "角色记忆写入失败，主人记忆恢复失败";
        return false;
    }
    for (size_t i = 0; i < pend.size(); ++i)
    {
        AppendPrivate(_episodesPath, "{\"d\":\"" + JsonEscape(pend[i].get("d").strOr("")) + "\",\"w\":"
                          + std::to_string((long long)pend[i].get("w").numOr(1)) + ",\"text\":\""
                          + JsonEscape(pend[i].get("text").strOr("")) + "\"}");
    }
    if (!touch.empty())
    {
        AppendPrivate(_episodesPath, "{\"d\":\"" + JsonEscape(FmtDate("%m-%d %H:%M"))
                          + "\",\"w\":1,\"text\":\"身体互动：" + JsonEscape(touch) + "\"}");
    }
    WritePrivate(_pendingPath, "");
    _sessionTouch.clear();
    return changed;
}


// ---------------------------------------------------------------- 记忆压缩（每天一次；见 PetMemory.h）

bool PetMemory::NeedsCompress(double maxAgeSec) const
{
    const double last = LastCompressAt();
    if (last <= 0.0) return true;
    return NowSec() - last >= maxAgeSec;
}

double PetMemory::LastCompressAt() const
{
    const jsonlite::Value* v = ObjFindC(&_habits, "compressT");
    return v != NULL ? v->numOr(0.0) : 0.0;
}

/** 记忆里 `- ` 条目的条数（结构校验用：改写前后条数应当一致） */
static size_t BulletCount(const std::string& s)
{
    size_t n = 0;
    size_t a = 0;
    while (a <= s.size())
    {
        size_t b = s.find('\n', a);
        if (b == std::string::npos) b = s.size();
        if (s.compare(a, 2, "- ") == 0) ++n;
        a = b + 1;
    }
    return n;
}

bool PetMemory::Rewrite(const std::string& lang,
                        const std::function<bool(const std::string&, std::string*, std::string*)>& llmJson,
                        std::string* err, std::string* report)
{
    const std::string ownerRaw = ReadText(_ownerPath), charRaw = ReadText(_charPath);
    const std::string owner0 = Owner();
    const std::string us0 = Us();
    if (owner0.empty() && us0.empty())
    {
        if (err != NULL) *err = "还没有记忆可改写成新语言";
        return false;
    }
    const std::string l = (lang == "ja") ? std::string("ja") : (lang == "en" ? std::string("en") : std::string("zh"));
    const std::string langCn = (l == "ja") ? "日语" : (l == "en" ? "英语" : "中文");
    const std::string first = (l == "ja") ? "私" : (l == "en" ? "I" : "我");
    const std::string master = (l == "ja") ? "ご主人" : (l == "en" ? "master" : "主人");

    const std::string prompt =
        std::string("你是桌宠\"") + _name + "\"的\"记忆改写器\"。主人把界面语言换成了" + langCn + "，"
        "请把下面两份记忆**原样改写成" + langCn + "**（不是总结、不是压缩）：\n"
        "规则：\n"
        "- `## 小标题` 照旧（标题也可译为" + langCn + "），每条 `- ` 一条对一条：**不许增删、不许合并、不许拆分**；\n"
        "- 只换语言：人名/地名/专业词/数字原样保留；时间戳（如 10-10 02:09）**一个字符都不要改**；\n"
        "- 不要新增任何事实，也不要删掉任何细节；\n"
        "- 她的第一人称用 \"" + first + "\"，对方称 \"" + master + "\"；\n"
        "- 只输出 JSON：{\"owner\":\"<改写后的《关于主人》>\",\"us\":\"<改写后的《我和主人之间》>\"}\n\n"
        "现在《关于主人》（所有角色共享）：\n<owner>\n" + owner0 + "\n</owner>\n\n"
        "现在《我和主人之间》（只属于 " + _name + "）：\n<us>\n" + us0 + "\n</us>\n";

    std::string owner, us;
    if (!llmJson(prompt, &owner, &us))
    {
        if (err != NULL) *err = "LLM 改写调用失败";
        return false;
    }
    owner = TrimCopy(owner);
    us = TrimCopy(us);
    if (!StartsWith(owner, "## ")) owner = owner0;          // 段名都丢了 → 结果不可信，保留原文
    if (!Contains(us, "## ")) us = us0;
    // 结构校验（换语言不看字节数，看**条数**）：条数不一致说明它偷偷合并/增减了 → 保留原文那一侧
    if (BulletCount(owner0) != BulletCount(owner)) owner = owner0;
    if (BulletCount(us0) != BulletCount(us)) us = us0;
    if (owner == owner0 && us == us0)
    {
        if (err != NULL) *err = "改写结果结构对不上，保留原文";
        return false;
    }
    if (ReadText(_ownerPath) != ownerRaw || ReadText(_charPath) != charRaw)
    {
        if (err != NULL) *err = "整理期间记忆已变更，丢弃旧结果";
        return false;
    }
    if (owner.size() > kOwnerMax || us.size() > kCharMax)
    {
        if (err != NULL) *err = "整理结果超长，保留原记忆，不进行硬截断";
        return false;
    }
    if (!plat::WritePrivateFile(_ownerPath, owner + "\n"))
    {
        if (err != NULL) *err = "主人记忆写入失败，保留待整理数据";
        return false;
    }
    if (!plat::WritePrivateFile(_charPath, us + "\n"))
    {
        const bool restored = plat::WritePrivateFile(_ownerPath, ownerRaw);
        if (err != NULL) *err = restored ? "角色记忆写入失败，已恢复主人记忆" : "角色记忆写入失败，主人记忆恢复失败";
        return false;
    }
    if (report != NULL)
    {
        *report = "关于主人 " + std::to_string(BulletCount(owner0)) + " 条、" + _name + " " + std::to_string(BulletCount(us0))
                  + " 条已改写成" + langCn;
    }
    return true;
}

bool PetMemory::Compress(const std::function<bool(const std::string&, std::string*, std::string*)>& llmJson,
                         const ZoneNameFn& zoneCn, std::string* err, std::string* report)
{
    const std::string ownerRaw = ReadText(_ownerPath), charRaw = ReadText(_charPath);
    const std::string owner0 = Owner();
    const std::string us0 = Us();
    if (owner0.empty() && us0.empty())
    {
        if (err != NULL) *err = "还没有记忆可压缩";
        return false;
    }
    // 归档（episodes）也一并喂进去：压缩后"更早"能吸收它们，归档文件随后截断
    std::vector<std::string> eps = SplitLines(ReadText(_episodesPath));
    std::string epochs;
    for (size_t i = eps.size() > 200 ? eps.size() - 200 : 0; i < eps.size(); ++i)
    {
        jsonlite::Value v;
        if (!jsonlite::parse(eps[i].c_str(), eps[i].size(), v, NULL) || !v.isObj()) continue;
        epochs += std::string("- ") + v.get("d").strOr("") + " " + v.get("text").strOr("") + "\n";
    }
    const std::string prompt =
        "你是桌宠\"" + _name + "\"的\"记忆压缩器\"。请把下面两份记忆**压缩**：合并重复、把久远的事折进《更早》、"
        "删掉低价值细节（一次性的小互动、已经被后来的事覆盖的、含糊没信息的），**不新增任何事实**。\n\n"
        "现在《关于主人》（所有角色共享）：\n<owner>\n" + owner0 + "\n</owner>\n\n"
        "现在《我和主人之间》（只属于 " + _name + "）：\n<us>\n" + us0 + "\n</us>\n\n"
        "更早的归档（可能已被吸收过，别重复）：\n<archive>\n" + (epochs.empty() ? "（无）" : epochs) + "</archive>\n\n"
        "程序统计的习惯（可信，可据此写\"（看起来）……\"这类推断）：\n"
        + (HabitsText(zoneCn).empty() ? "（无）" : HabitsText(zoneCn))
        + "\n\n输出一个 JSON：{\"owner\":\"<压缩后的《关于主人》>\",\"us\":\"<压缩后的《我和主人之间》>\"}\n"
        "规则：\n"
        "- owner 以 `## 关于主人` 开头，只留主人本人的**稳定**信息（称呼、偏好、讨厌、和她的约定、稳定的规律），"
        "每条一行 `- ` 开头，**≤300 字**。\n"
        "- us 固定两段：`## 最近发生`（保留最近 5~8 条，每条一行、以 月-日 开头）和 `## 更早`（把剩下的**压缩成 2~5 条**），"
        "**≤900 字**。\n"
        "- 已经够短就不要硬删；但**重复/同一件事反复记的必须合并成一条**（哪怕总量不长也要合并）；"
        "宁可少一点也不要丢主人亲自说过/表态过的事。\n"
        "- 中文，她的第一人称（\"我\"）、对方叫\"主人\"。只输出 JSON。";
    std::string owner, us;
    if (!llmJson(prompt, &owner, &us))
    {
        if (err != NULL) *err = "LLM 压缩调用失败";
        return false;
    }
    owner = TrimCopy(owner);
    us = TrimCopy(us);
    if (!StartsWith(owner.c_str(), "## 关于主人")) owner = owner.empty() ? owner0 : "## 关于主人\n" + owner;
    if (!Contains(us.c_str(), "## 最近发生"))
    {
        if (err != NULL) *err = "压缩结果缺少『## 最近发生』段，丢弃这次结果";
        return false;
    }
    // 硬约束：压缩不该让它变长（LLM 偶尔会把内容写得更啰嗦，再被截断成半句）——超过原量 5% 就保留原文
    if (owner0.size() > 0 && owner.size() > owner0.size() + owner0.size() / 20 + 16)
    {
        owner = owner0;
    }
    if (us0.size() > 0 && us.size() > us0.size() + us0.size() / 20 + 16)
    {
        us = us0;
    }
    if (ReadText(_ownerPath) != ownerRaw || ReadText(_charPath) != charRaw)
    {
        if (err != NULL) *err = "整理期间记忆已变更，丢弃旧结果";
        return false;
    }
    if (owner.size() > kOwnerMax || us.size() > kCharMax)
    {
        if (err != NULL) *err = "整理结果超长，保留原记忆，不进行硬截断";
        return false;
    }
    if (!plat::WritePrivateFile(_ownerPath, owner + "\n"))
    {
        if (err != NULL) *err = "主人记忆写入失败，保留待整理数据";
        return false;
    }
    if (!plat::WritePrivateFile(_charPath, us + "\n"))
    {
        const bool restored = plat::WritePrivateFile(_ownerPath, ownerRaw);
        if (err != NULL) *err = restored ? "角色记忆写入失败，已恢复主人记忆" : "角色记忆写入失败，主人记忆恢复失败";
        return false;
    }
    // 归档截断：老的已经在上面喂给 LLM 并折进"更早"，只留最近 60 条
    if (eps.size() > 60)
    {
        std::string keep;
        for (size_t i = eps.size() - 60; i < eps.size(); ++i) keep += eps[i] + "\n";
        WritePrivate(_episodesPath, keep);
    }
    // 记压缩时间（habits.json 里加一个字段；HabitsText 不读它，不影响提示词）
    jsonlite::Value* t = ObjFind(&_habits, "compressT");
    if (t == NULL)
    {
        jsonlite::Value nv;
        nv.setType(jsonlite::Value::NUM);
        nv.num = NowSec();
        _habits.obj.push_back(std::make_pair(std::string("compressT"), nv));
    }
    else
    {
        t->num = NowSec();
    }
    _habitsDirty = true;
    SaveHabits(true);
    if (report != NULL)
    {
        *report = "关于主人 " + std::to_string(owner0.size()) + "→" + std::to_string(owner.size())
                  + " 字节，我和主人 " + std::to_string(us0.size()) + "→" + std::to_string(us.size())
                  + " 字节，归档 " + std::to_string(eps.size()) + "→" + std::to_string(eps.size() > 60 ? 60 : eps.size())
                  + " 条";
    }
    return true;
}

void PetMemory::MigrateLegacy()
{
    const std::string legacy = HomePath("/.config/l2dpet/memory.md");
    struct stat st;
    if (stat(legacy.c_str(), &st) != 0 || stat(_charPath.c_str(), &st) == 0) return;
    const std::string text = ReadText(legacy);
    if (text.empty()) return;
    _legacyMtime = (double)st.st_mtime;
    // 切出"## 关于主人"段（到下一个 "\n## " 或文末）
    std::string owner = kEmptyOwner;
    size_t h = text.find("## 关于主人\n");
    if (h != std::string::npos)
    {
        size_t end = text.find("\n## ", h + 6);
        if (end == std::string::npos) end = text.size();
        owner = "## 关于主人\n" + TrimCopy(text.substr(h + strlen("## 关于主人\n"), end - (h + strlen("## 关于主人\n")))) + "\n";
    }
    std::string rest = text;
    if (h != std::string::npos)
    {
        size_t end = text.find("\n## ", h + 6);
        if (end == std::string::npos) end = text.size();
        rest = text.substr(0, h) + (end < text.size() ? text.substr(end + 1) : "");
    }
    rest = TrimCopy(rest).empty() ? kEmptyChar : TrimCopy(rest);
    struct stat ost;
    if (stat(_ownerPath.c_str(), &ost) != 0) WritePrivate(_ownerPath, owner);
    WritePrivate(_charPath, rest + "\n");
    const std::string migrated = legacy + ".migrated";
    rename(legacy.c_str(), migrated.c_str());
}

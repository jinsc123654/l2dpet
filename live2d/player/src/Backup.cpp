/** Backup.cpp — 见 Backup.h（从 l2dtest.cpp 拆出，纯搬移） */
#include "Backup.h"

#include "FileUtil.h"
#include "Platform.h"
#include "JsonLite.h"
#include <CubismFramework.hpp>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

/** 只保留合法 UTF-8 序列（历史文件里可能有坏字节——早期版本截断留下过半个汉字；
 *  带坏字节的包在严格解析器里直接读不了，导出时先洗一遍） */
static std::string BkCleanUtf8(const std::string& s)
{
    std::string o;
    size_t i = 0;
    while (i < s.size())
    {
        const unsigned char c = (unsigned char)s[i];
        if (c < 0x80)
        {
            o += (char)c;
            ++i;
            continue;
        }
        int n = 0;
        if (c >= 0xC2 && c <= 0xDF) n = 1;
        else if (c >= 0xE0 && c <= 0xEF) n = 2;
        else if (c >= 0xF0 && c <= 0xF4) n = 3;
        if (n == 0 || i + (size_t)n >= s.size())
        {
            ++i;   // 非法首字节 / 被截断：丢掉
            continue;
        }
        bool ok = true;
        for (int k = 1; k <= n && ok; ++k) ok = ((unsigned char)s[i + (size_t)k] & 0xC0) == 0x80;
        // 过头/过脚的组合（E0 A0..BF 之类）也按严格校验挡住
        if (ok && c == 0xE0) ok = (unsigned char)s[i + 1] >= 0xA0;
        if (ok && c == 0xED) ok = (unsigned char)s[i + 1] < 0xA0;
        if (ok && c == 0xF0) ok = (unsigned char)s[i + 1] >= 0x90;
        if (ok && c == 0xF4) ok = (unsigned char)s[i + 1] < 0x90;
        if (!ok)
        {
            ++i;
            continue;
        }
        o.append(s, i, (size_t)n + 1);
        i += (size_t)n + 1;
    }
    return o;
}

/** JSON 转义（备份包内联文本用；与播放器里的 JStr 分开，这里在文件更早处） */
static std::string BkEsc(const std::string& raw)
{
    const std::string s = BkCleanUtf8(raw);
    std::string o;
    for (size_t i = 0; i < s.size(); ++i)
    {
        const unsigned char c = (unsigned char)s[i];

        if (c == '"' || c == '\\') { o += '\\'; o += (char)c; }
        else if (c == '\n') { o += "\\n"; }
        else if (c == '\r') { o += "\\r"; }
        else if (c == '\t') { o += "\\t"; }
        else if (c < 0x20) { char b[8]; snprintf(b, sizeof(b), "\\u%04x", c); o += b; }
        else { o += (char)c; }
    }
    return o;
}

/** 当前时间戳（备份包里的 at 字段） */
static std::string BkStamp()
{
    time_t t = time(NULL);
    struct tm lt;
#ifdef _WIN32
    localtime_s(&lt, &t);
#else
    localtime_r(&t, &lt);
#endif
    char buf[32];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", &lt);
    return buf;
}

// 全部是 ~/.config/l2dpet 下的小文本文件，打进**一个 JSON**（模型本体/密钥不入包——密钥在 llm.json，
// 有意不导出：备份常被发给别人/传网盘，把 Key 带出去是最常见的泄漏方式）

/** 收集要打包的相对路径清单（memory/xxx、personas/xxx.json…） */
static void BackupCollect(const std::string& root, const std::string& sub, const std::string& suffix,
                          std::vector<std::string>* out)
{
    std::vector<std::string> files;
    const std::string dir = root + "/" + sub;
    if (!plat::ListDir(dir, &files)) return;
    for (size_t i = 0; i < files.size(); ++i)
    {
        if (files[i].empty() || files[i][0] == '.') continue;
        if (!suffix.empty() && files[i].size() >= suffix.size()
            && files[i].compare(files[i].size() - suffix.size(), suffix.size(), suffix) != 0)
        {
            continue;
        }
        const std::string rel = sub + "/" + files[i];
        if (plat::IsFile(root + "/" + rel)) out->push_back(rel);
    }
}

bool BackupBuild(const std::string& root, std::string* out, int* files, std::string* err)
{
    std::vector<std::string> rels;
    BackupCollect(root, "memory", "", &rels);            // owner.md（+ 旧的 memory.md）
    BackupCollect(root, "memory/chars", "", &rels);      // <指纹>.md / .habits.json / .pending.jsonl / .episodes.jsonl
    BackupCollect(root, "personas", ".json", &rels);
    BackupCollect(root, "questions", ".json", &rels);
    BackupCollect(root, "jokes", ".json", &rels);
    rels.push_back("config.json");
    std::string o = "{\"v\":1,\"app\":\"l2dpet\",\"at\":\"" + BkStamp() + "\",\"files\":{";
    int n = 0;
    for (size_t i = 0; i < rels.size(); ++i)
    {
        const std::string text = plat::ReadTextFile(root + "/" + rels[i]);
        if (text.empty()) continue;
        if (text.size() > 2u * 1024u * 1024u) continue;   // 单项 >2MB 不收（异常文件）
        if (n) o += ",";
        o += "\"" + BkEsc(rels[i]) + "\":\"" + BkEsc(text) + "\"";
        ++n;
    }
    o += "}}";
    if (n == 0)
    {
        if (err != NULL) *err = "没有可导出的内容（记忆/性格/题库/笑话都还是空的？）";
        return false;
    }
    *files = n;
    *out = o;
    return true;
}

bool BackupRestore(const std::string& path, const std::string& root, int* files, std::string* err)
{
    const std::string text = plat::ReadTextFile(path);
    if (text.empty())
    {
        if (err != NULL) *err = "读不到备份文件（路径不对？）";
        return false;
    }
    if (text.size() > 8u * 1024u * 1024u)
    {
        if (err != NULL) *err = "备份文件太大（>8MB），不像桌宠备份";
        return false;
    }
    jsonlite::Value v;
    if (!jsonlite::parse(text.c_str(), text.size(), v, NULL) || !v.isObj())
    {
        if (err != NULL) *err = "备份不是合法 JSON";
        return false;
    }
    if (std::string(v.get("app").strOr("")) != "l2dpet")   // strOr 返回 const char*：必须转 std::string，否则比的是指针
    {
        if (err != NULL) *err = "不是桌宠的备份文件（缺 app:l2dpet）";
        return false;
    }
    const jsonlite::Value& fs = v.get("files");
    if (!fs.isObj() || fs.obj.empty())
    {
        if (err != NULL) *err = "备份里没有 files";
        return false;
    }
    int n = 0;
    for (size_t i = 0; i < fs.obj.size(); ++i)
    {
        const std::string rel = fs.obj[i].first;
        if (!fs.obj[i].second.isStr()) continue;
        // 只允许写回白名单目录（防备份文件里带 ../ 之类的路径穿越）
        bool allowed = rel == "config.json" || rel == "memory/owner.md" || rel == "memory/memory.md"
                       || rel.compare(0, 13, "memory/chars/") == 0 || rel.compare(0, 9, "personas/") == 0
                       || rel.compare(0, 10, "questions/") == 0 || rel.compare(0, 6, "jokes/") == 0;
        if (!allowed || rel.find("..") != std::string::npos || rel.size() > 200) continue;
        const std::string target = root + "/" + rel;
        const size_t slash = target.rfind('/');
        if (slash != std::string::npos) plat::CreateDir(target.substr(0, slash), true);
        if (plat::WritePrivateFile(target, fs.obj[i].second.str)) ++n;
    }
    if (n == 0)
    {
        if (err != NULL) *err = "备份里没有可写回的条目";
        return false;
    }
    *files = n;
    return true;
}

/** FileUtil.cpp — 见 FileUtil.h（从 l2dtest.cpp 拆出，纯搬移） */
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

using namespace Live2D::Cubism::Framework;

// 本块位于这些辅助函数定义之前 → 前向声明（定义见文件后段）

/**
 * 读 model3.json；**VTube Studio 发货格式**补全：VTS 的模型常常不在 model3.json 里登记表情/动作
 * （表情靠 .vtube.json 的热键引用，exp3/motion3 文件就放在同目录）。这时 SDK 看到 0 个表情——
 * 作者的"水印"开关（水印.exp3.json）也就无从关闭，角色身上满是版权字样。
 * 缺 "Expressions" 时把目录里的 *.exp3.json 全部登记（名字 = 文件名去后缀）；
 * 缺 "Motions" 时把 *.motion3.json 登记进 Idle（文件名含 idle）/ Extra 组。原文件不改，只改内存里的副本。
 */
csmByte* ReadModel3(const char* path, csmSizeInt* outSize)
{
    csmByte* raw = ReadFile(path, outSize);
    if (raw == NULL)
    {
        return NULL;
    }
    std::string text((const char*)raw, (size_t)*outSize);
    jsonlite::Value root;
    if (!jsonlite::parse(text.c_str(), text.size(), root, NULL) || !root.get("FileReferences").isObj())
    {
        return raw;
    }
    const jsonlite::Value& fr = root.get("FileReferences");
    const bool needExp = !fr.has("Expressions"), needMot = !fr.has("Motions");
    if (!needExp && !needMot)
    {
        return raw;
    }
    std::vector<std::string> exps, idles, extras;
    const std::string dir = DirName(path);
    std::vector<std::string> dirNames;
    if (plat::ListDir(dir, &dirNames))
    {
        for (size_t di = 0; di < dirNames.size(); ++di)
        {
            const std::string& n = dirNames[di];
            if (needExp && n.size() > 10 && n.compare(n.size() - 10, 10, ".exp3.json") == 0) exps.push_back(n);
            if (needMot && n.size() > 13 && n.compare(n.size() - 13, 13, ".motion3.json") == 0)
            {
                std::string low = n;
                for (size_t i = 0; i < low.size(); ++i) low[i] = (char)tolower((unsigned char)low[i]);
                (low.find("idle") != std::string::npos ? idles : extras).push_back(n);
            }
        }
    }
    std::sort(exps.begin(), exps.end());
    std::sort(idles.begin(), idles.end());
    std::sort(extras.begin(), extras.end());
    if (exps.empty() && idles.empty() && extras.empty())
    {
        return raw;
    }
    auto jstr = [](const std::string& v) {
        std::string o = "\"";
        for (size_t i = 0; i < v.size(); ++i)
        {
            if (v[i] == '"' || v[i] == '\\') o += '\\';
            o += v[i];
        }
        return o + "\"";
    };
    std::string ins;
    if (!exps.empty())
    {
        ins += "\"Expressions\":[";
        for (size_t i = 0; i < exps.size(); ++i)
        {
            ins += (i ? ",{\"Name\":" : "{\"Name\":") + jstr(exps[i].substr(0, exps[i].size() - 10)) + ",\"File\":" + jstr(exps[i]) + "}";
        }
        ins += "],";
    }
    if (!idles.empty() || !extras.empty())
    {
        ins += "\"Motions\":{";
        auto group = [&](const char* g, const std::vector<std::string>& fs) {
            if (fs.empty()) return;
            if (ins.back() != '{') ins += ",";
            ins += std::string("\"") + g + "\":[";
            for (size_t i = 0; i < fs.size(); ++i) ins += (i ? ",{\"File\":" : "{\"File\":") + jstr(fs[i]) + "}";
            ins += "]";
        };
        group("Idle", idles);
        group("Extra", extras);
        ins += "},";
    }
    // 插到 "FileReferences": { 的左花括号后面
    const size_t k = text.find("\"FileReferences\"");
    const size_t brace = k == std::string::npos ? std::string::npos : text.find('{', k);
    if (brace == std::string::npos)
    {
        return raw;
    }
    text.insert(brace + 1, ins);
    jsonlite::Value chk;
    if (!jsonlite::parse(text.c_str(), text.size(), chk, NULL))
    {
        return raw;   // 补全出了问题：退回原文件（宁可少表情，不让模型加载失败）
    }
    free(raw);
    csmByte* out = (csmByte*)malloc(text.size());
    if (out == NULL)
    {
        return NULL;
    }
    memcpy(out, text.data(), text.size());
    *outSize = (csmSizeInt)text.size();
    printf("[model] VTS 格式补全：从目录登记 %d 个表情、%d 个动作（%s）\n", (int)exps.size(),
           (int)(idles.size() + extras.size()), BaseName(path).c_str());
    fflush(stdout);
    return out;
}

/** 读模型里的 JSON 资产（exp3/motion3/physics3/pose3）：先用 jsonlite（有嵌套深度上限）校验是合法 JSON 对象，
 *  不合法返回 NULL——Cubism Framework 的 JSON 解析遇到坏文件会解引用空根节点直接崩（导入的模型包是外来文件）。 */
csmByte* ReadJsonFile(const char* path, csmSizeInt* outSize)
{
    csmByte* buf = ReadFile(path, outSize);
    if (buf == NULL) return NULL;
    jsonlite::Value v;
    if (!jsonlite::parse((const char*)buf, (size_t)*outSize, v, NULL) || !v.isObj())
    {
        fprintf(stderr, "[l2dtest] 不是合法 JSON，跳过：%s\n", path);
        free(buf);
        return NULL;
    }
    return buf;
}

csmByte* ReadFile(const char* path, csmSizeInt* outSize)
{
    // 走 plat::ReadBinaryFile（Windows 上是 W 版打开）：fopen 在 ACP=936 下
    // 按本地代码页解释路径，模型目录名带中文时 .moc3/.png/motion 全读不出来。
    std::string blob;
    if (!plat::ReadBinaryFile(path, &blob))
    {
        fprintf(stderr, "[l2dtest] 无法打开文件: %s\n", path);
        return NULL;
    }
    const long size = (long)blob.size();
    csmByte* buffer = (csmByte*)malloc(blob.empty() ? 1 : blob.size());
    if (buffer == NULL)
    {
        return NULL;
    }
    if (!blob.empty()) memcpy(buffer, blob.data(), blob.size());
    *outSize = (csmSizeInt)size;
    return buffer;
}

// 路径切分要同时认 '/' 和 '\\'：Windows 上 plat::Canonicalize（_fullpath）产出的是反斜杠路径，
// 只按 '/' 切会把目录名判成 "."（模型于是找不到同目录的 .moc3/.png）。
// 与 plat::CreateDir 的 find_last_of("/\\") 同口径。
std::string DirName(const std::string& path)
{
    const size_t pos = path.find_last_of("/\\");
    if (pos == std::string::npos)
    {
        return std::string(".");
    }
    return path.substr(0, pos);
}

std::string BaseName(const std::string& path)
{
    const size_t pos = path.find_last_of("/\\");
    return (pos == std::string::npos) ? path : path.substr(pos + 1);
}

/** Absolute-path test: '/...' (POSIX), 'X:\' or 'X:/' (drive), '\\server\share' (UNC).
 *  Only '/'-leading used to count, so on Windows a typed drive path - and a UNC network path
 *  in particular - was rejected as "not absolute".
 *  (char)92 = backslash, written this way to keep the source free of stray escapes. */
bool IsAbsPath(const std::string& p)
{
    if (p.empty()) return false;
    if (p[0] == '/') return true;
#if defined(_WIN32)
    const char B = (char)92;
    if (p.size() >= 3 && p[1] == ':' && (p[2] == B || p[2] == '/') &&
        ((p[0] >= 'A' && p[0] <= 'Z') || (p[0] >= 'a' && p[0] <= 'z')))
    {
        return true;
    }
    if (p.size() >= 2 && p[0] == B && p[1] == B) return true;
#endif
    return false;
}


/** 路径归一化（失败原样返回） */
std::string RealPath(const char* path)
{
    return plat::Canonicalize(std::string(path));
}

/**
 * 扫描模型库目录：每个直接子目录里取第一个 *.model3.json；
 * 扫描结果按路径排序（切换顺序可预期）。返回模型数量。
 */
int ScanModelDir(const char* dir, std::vector<std::string>* out)
{
    out->clear();
    std::vector<std::string> names;
    if (!plat::ListDir(dir, &names))
    {
        return 0;
    }
    std::vector<std::string> found;
    for (size_t di = 0; di < names.size(); ++di)
    {
        if (names[di].empty() || names[di][0] == '.')
        {
            continue;
        }
        const std::string sub = std::string(dir) + "/" + names[di];
        if (!plat::IsDir(sub))
        {
            continue;
        }
        std::vector<std::string> inner;
        if (!plat::ListDir(sub, &inner))
        {
            continue;
        }
        std::string modelFile;
        for (size_t si = 0; si < inner.size(); ++si)
        {
            const size_t len = inner[si].size();
            if (len > 12 && inner[si].compare(len - 12, 12, ".model3.json") == 0)
            {
                modelFile = sub + "/" + inner[si];
                break;
            }
        }
        if (!modelFile.empty())
        {
            found.push_back(modelFile);
        }
    }
    std::sort(found.begin(), found.end());
    *out = found;
    return (int)found.size();
}

// PPM(P6) 输出：帧缓冲为预乘 ARGB8888，黑底合成后即取其 RGB 分量
bool WritePpm(const char* path, const csmUint32* framebuffer, int width, int height)
{
    if (framebuffer == NULL || width <= 0 || height <= 0)
    {
        return false;
    }
    FILE* fp = fopen(path, "wb");
    if (fp == NULL)
    {
        fprintf(stderr, "[l2dtest] 无法写 %s\n", path);
        return false;
    }
    fprintf(fp, "P6\n%d %d\n255\n", width, height);
    for (int i = 0; i < width * height; ++i)
    {
        const csmUint32 p = framebuffer[i];
        const unsigned char rgb[3] = {
            (unsigned char)((p >> 16) & 0xff),
            (unsigned char)((p >> 8) & 0xff),
            (unsigned char)(p & 0xff)};
        fwrite(rgb, 1, 3, fp);
    }
    fclose(fp);
    return true;
}

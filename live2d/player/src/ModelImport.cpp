/** ModelImport.cpp — 见 ModelImport.h（从 l2dtest.cpp 拆出，纯搬移） */
#include "ModelImport.h"

#include "FileUtil.h"
#include "Platform.h"
#include <CubismFramework.hpp>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

/** 找 dir 下（含子目录，深度 ≤ maxDepth）第一个 *.model3.json；返回文件路径或空 */
std::string FindModel3Under(const std::string& dir, int maxDepth)
{
    std::vector<std::string> names;
    if (!plat::ListDir(dir, &names))
    {
        return std::string();
    }
    std::vector<std::string> subdirs;
    for (size_t i = 0; i < names.size(); ++i)
    {
        if (names[i].empty() || names[i][0] == '.') continue;
        const std::string full = dir + "/" + names[i];
        if (plat::IsDir(full))
        {
            subdirs.push_back(full);
        }
        else
        {
            const size_t len = names[i].size();
            if (len > 12 && names[i].compare(len - 12, 12, ".model3.json") == 0)
            {
                return full;
            }
        }
    }
    if (maxDepth <= 0)
    {
        return std::string();
    }
    for (size_t i = 0; i < subdirs.size(); ++i)
    {
        const std::string r = FindModel3Under(subdirs[i], maxDepth - 1);
        if (!r.empty()) return r;
    }
    return std::string();
}

/** 递归复制目录（跳过隐藏项；模型包都是小文件，够用） */
bool CopyDirTree(const std::string& src, const std::string& dst)
{
    if (!plat::CreateDir(dst))
    {
        return false;
    }
    std::vector<std::string> names;
    if (!plat::ListDir(src, &names))
    {
        return false;
    }
    for (size_t i = 0; i < names.size(); ++i)
    {
        if (names[i].empty() || names[i][0] == '.') continue;
        const std::string s = src + "/" + names[i];
        const std::string d = dst + "/" + names[i];
        if (plat::IsSymlink(s)) continue;   // 不跟链接：模型包里的链接可能指向包外（/etc、家目录…）
        if (plat::IsDir(s))
        {
            if (!CopyDirTree(s, d)) return false;
        }
        else
        {
            if (!plat::FileCopy(s, d)) return false;   // W 版复制：包里的中文文件名也搬得动
        }
    }
    return true;
}


// ------------------------------------------------------ 模型导入（从 l2dtest.cpp 拆出 // 见 ModelImport.h）
/** 删除目录树（尽力而为；导入临时目录清理 / 模型删除用）
 *  - 符号链接只删链接本身、绝不跟进（否则模型包里一个指向家目录的链接 = 删模型时把家目录清空）；
 *  - 隐藏文件也删（.DS_Store / __MACOSX 残留；跳过它们会让目录永远删不掉）。ListDir 已排除 . 和 ..。 */
void RemoveDirTree(const std::string& dir)
{
    if (plat::IsSymlink(dir))
    {
        plat::RemoveFile(dir);
        return;
    }
    std::vector<std::string> entries;
    if (!plat::ListDir(dir, &entries)) return;
    for (size_t i = 0; i < entries.size(); ++i)
    {
        if (entries[i].empty()) continue;
        const std::string p = dir + "/" + entries[i];
        if (!plat::IsSymlink(p) && plat::IsDir(p))
        {
            RemoveDirTree(p);
        }
        else
        {
            plat::RemoveFile(p);   // 普通文件 / 链接本身
        }
    }
    plat::RemoveEmptyDir(dir);
}

/** 删掉目录树里所有符号链接（解压出来的模型包不该带链接；带了就可能指向包外） */
static int StripSymlinks(const std::string& dir, int depth)
{
    int n = 0;
    std::vector<std::string> entries;
    if (depth > 16 || !plat::ListDir(dir, &entries)) return 0;
    for (size_t i = 0; i < entries.size(); ++i)
    {
        const std::string p = dir + "/" + entries[i];
        if (plat::IsSymlink(p))
        {
            if (plat::RemoveFile(p)) ++n;
        }
        else if (plat::IsDir(p))
        {
            n += StripSymlinks(p, depth + 1);
        }
    }
    return n;
}

/** 导入模型：src = .zip 或含 *.model3.json 的目录 → 放到 root/<名字>/；outName = 目录名 */
bool ImportModelInto(const std::string& srcPath, const std::string& root, std::string* outName, std::string* err)
{
    if (!plat::Exists(srcPath))
    {
        *err = "路径不存在：" + srcPath;
        return false;
    }
    // 名字：源文件/目录名（.zip 后缀去掉），去重
    std::string base = BaseName(srcPath);
    if (base.size() > 4 && base.compare(base.size() - 4, 4, ".zip") == 0)
    {
        base = base.substr(0, base.size() - 4);
    }
    if (base.empty() || base[0] == '.')
    {
        *err = "名字不合法";
        return false;
    }
    std::string target = root + "/" + base;
    for (int k = 2; plat::Exists(target) && k < 100; ++k)
    {
        target = root + "/" + base + "-" + std::to_string(k);
    }
    plat::CreateDir(root);

    std::string modelDir;
    std::string m3Name;   // 模型自己声称的名字（<名>.model3.json 的 <名>，通常等于 <名>.moc3）
    std::string tmpDir;
    const bool isZip = srcPath.size() > 4 && srcPath.compare(srcPath.size() - 4, 4, ".zip") == 0;
    if (isZip)
    {
        tmpDir = root + "/.import-tmp-" + std::to_string(plat::CurrentPid());
        RemoveDirTree(tmpDir);
        if (!plat::CreateDir(tmpDir))
        {
            *err = "临时目录建不了";
            return false;
        }
        // 解压走独立可执行（参数数组直传，不经 shell）
#if defined(_WIN32)
        // Windows 用系统自带的 bsdtar（Win10 1803+ 的 tar.exe 能解 zip）。
        // 注意 plat::SpawnDetached 底层是 _spawnv —— 不搜 PATH，所以得给完整路径。
        std::string tarExe = "tar";
        if (const char* sr = getenv("SystemRoot"))
        {
            tarExe = std::string(sr) + "\\System32\\tar.exe";
        }
        const long pid = plat::SpawnDetached(tarExe, { "-xf", srcPath, "-C", tmpDir }, std::string());
#else
        const long pid = plat::SpawnDetached("/usr/bin/unzip", { "-o", "-q", srcPath, "-d", tmpDir }, std::string());
#endif
        if (pid <= 0 || !plat::WaitChildExit(pid, 120000))
        {
            RemoveDirTree(tmpDir);
            *err = "解压失败（解压工具不可用或包损坏）";
            return false;
        }
        const int links = StripSymlinks(tmpDir, 0);
        if (links > 0)
        {
            printf("[import] 包里有 %d 个符号链接，已丢弃\n", links);
            fflush(stdout);
        }
        const std::string m3 = FindModel3Under(tmpDir, 4);
        if (m3.empty())
        {
            RemoveDirTree(tmpDir);
            *err = "包里没找到 *.model3.json（不是 Live2D 模型包？）";
            return false;
        }
        modelDir = m3.substr(0, m3.rfind('/'));
        m3Name = BaseName(m3);
    }
    else if (plat::IsDir(srcPath))
    {
        const std::string m3 = FindModel3Under(srcPath, 3);
        if (m3.empty())
        {
            *err = "目录里没找到 *.model3.json（深度 ≤3）";
            return false;
        }
        modelDir = m3.substr(0, m3.rfind('/'));
        m3Name = BaseName(m3);
    }
    else
    {
        *err = "只支持 .zip 或目录";
        return false;
    }

    // 名字优先按模型自己的 *.model3.json（<名>.model3.json 就是 <名>.moc3 的那个名字）：
    // 从 zip 导入时用 zip 名当模型名，往往不是用户认得的那个（"舒芙蕾发货文件" vs "舒芙蕾"）。
    // 拿不到（或就是当前名字）才退回源文件/目录名。
    if (!m3Name.empty())
    {
        const std::string suffix = ".model3.json";
        if (m3Name.size() > suffix.size() &&
            m3Name.compare(m3Name.size() - suffix.size(), suffix.size(), suffix) == 0)
        {
            const std::string fromModel = m3Name.substr(0, m3Name.size() - suffix.size());
            if (!fromModel.empty() && fromModel[0] != '.' && fromModel != base)
            {
                printf("[import] 模型名按 %s.model3.json 定为「%s」（原来按源名是「%s」）\n",
                       fromModel.c_str(), fromModel.c_str(), base.c_str());
                base = fromModel;
                target = root + "/" + base;
                for (int k = 2; plat::Exists(target) && k < 100; ++k)
                {
                    target = root + "/" + base + "-" + std::to_string(k);
                }
            }
        }
    }

    bool ok = false;
    if (!tmpDir.empty() && modelDir == tmpDir)
    {
        ok = plat::ReplaceFile(tmpDir, target);   // 解出来就是模型根（W 版移动：中文名不被代码页搞坏）
    }
    else
    {
        // 解压临时目录里的子目录可以直接搬；用户给的源目录必须**拷贝**（rename 会把主人原来的文件夹挪走）
        // （按真实路径判断：包里的链接可能把 modelDir 指到包外，那种也只拷不搬）
        const std::string realTmp = tmpDir.empty() ? std::string() : plat::Canonicalize(tmpDir);
        const bool fromTmp = !realTmp.empty() && plat::Canonicalize(modelDir).compare(0, realTmp.size() + 1, realTmp + "/") == 0;
        ok = (fromTmp && plat::ReplaceFile(modelDir, target)) || CopyDirTree(modelDir, target);
        if (!ok) RemoveDirTree(target);   // 拷一半失败：别留半个模型在库里
    }
    if (!tmpDir.empty())
    {
        RemoveDirTree(tmpDir);
    }
    if (!ok)
    {
        *err = "搬进模型库失败（名字：" + BaseName(target) + "）";
        return false;
    }
    *outName = BaseName(target);
    return true;
}
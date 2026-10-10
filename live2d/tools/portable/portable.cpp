/**
 * portable.cpp — l2dpet 便携单文件启动器（自解包）
 *
 * 为什么需要它：l2dpet 本体是两个进程（桌宠 l2dtest.exe + 面板 l2dpet-ui.exe）加
 * 资源目录（assets/ 图标字体、models/ 自带示例模型），直接分发要给人一个文件夹。
 * 这个启动器把整个文件夹当 RCDATA 资源塞进**一个 exe**：
 *
 *   双击 → 解包到 %LOCALAPPDATA%\l2dpet\portable-<key>\ → 拉起 l2dtest.exe（cwd = 解包目录）
 *
 * key 里带版本号 / 路径与内容的 SHA-256：payload 没变就不再解包；
 * 变了会在旁边另解一份（旧的那份可以手动删）。解包是"先解到 .tmp<pid> 再改名"，
 * 双击两次也不会解出半个目录。
 *
 * 单文件 exe 与桌宠本体一样是静态 CRT（/MT）：空白 Windows 10/11 上双击即可，
 * 不用装 VC++ 运行库、不写注册表（配置仍在 %USERPROFILE%\.config\l2dpet\）。
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <stdarg.h>
#include <stdio.h>
#include <cwchar>
#include <string>

#include "payload_files.h"   // CMake 生成：文件清单（id / 相对路径 / 大小）+ key

namespace
{

/** 诊断日志：%TEMP%\l2dpet-portable.log（每次运行重写，只有十几行）。
 *  "双击没反应"这类问题全指望它——启动器是 WIN32 子系统，没有控制台可看。*/
HANDLE g_log = INVALID_HANDLE_VALUE;

void LogOpen()
{
    wchar_t tmp[MAX_PATH] = L"";
    if (GetTempPathW(MAX_PATH, tmp) == 0) return;
    std::wstring path = std::wstring(tmp) + L"l2dpet-portable.log";
    g_log = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
}

void Log(const wchar_t* fmt, ...)
{
    if (g_log == INVALID_HANDLE_VALUE) return;
    wchar_t line[1024] = {};
    va_list ap;
    va_start(ap, fmt);
    vswprintf(line, sizeof(line) / sizeof(line[0]), fmt, ap);
    line[sizeof(line) / sizeof(line[0]) - 1] = L'\0';
    va_end(ap);
    char u8[3072];   // 按 UTF-8 写：记事本 / grep / cat 都能直接看
    const int n = WideCharToMultiByte(CP_UTF8, 0, line, -1, u8, sizeof(u8), NULL, NULL);
    if (n <= 1) return;
    DWORD wrote = 0;
    WriteFile(g_log, u8, (DWORD)(n - 1), &wrote, NULL);
    WriteFile(g_log, "\r\n", 2, &wrote, NULL);
}

/** 窄→宽（只用得上 ASCII 的 key 与文件名，按 UTF-8 处理更稳妥） */
std::wstring Widen(const char* s)
{
    if (s == NULL || s[0] == '\0') return std::wstring();
    const int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    if (n <= 1) return std::wstring();
    std::wstring out((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s, -1, &out[0], n);
    out.resize((size_t)n - 1);
    return out;
}

/** %LOCALAPPDATA%（拿不到就退回空，由调用方报错） */
std::wstring LocalAppData()
{
    PWSTR p = NULL;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, NULL, &p)) || p == NULL)
    {
        return std::wstring();
    }
    std::wstring out(p);
    CoTaskMemFree(p);
    return out;
}

/** 建出 rel 里所有中间目录（rel 用 '/' 分隔） */
bool MakeParents(const std::wstring& full)
{
    for (size_t i = 0; i < full.size(); ++i)
    {
        if (full[i] != L'/' && full[i] != L'\\') continue;
        if (i == 0) continue;
        const std::wstring dir = full.substr(0, i);
        if (dir.size() == 2 && dir[1] == L':') continue;   // "C:"
        if (!CreateDirectoryW(dir.c_str(), NULL) && GetLastError() != ERROR_ALREADY_EXISTS)
        {
            return false;
        }
    }
    return true;
}

/** 把一条 RCDATA 资源写盘（资源在编译期由 payload.rc 生成） */
bool ExtractOne(HINSTANCE hInst, const PayloadFile& f, const std::wstring& dir, DWORD* err)
{
    const HRSRC res = FindResourceW(hInst, MAKEINTRESOURCEW(f.id), RT_RCDATA);
    if (res == NULL)
    {
        *err = GetLastError();
        return false;
    }
    const DWORD size = SizeofResource(hInst, res);
    const HGLOBAL h = LoadResource(hInst, res);
    const void* data = (h != NULL) ? LockResource(h) : NULL;
    if (data == NULL || size != (DWORD)f.size)
    {
        *err = ERROR_INVALID_DATA;
        return false;
    }
    const std::wstring out = dir + L"/" + f.rel;
    if (!MakeParents(out))
    {
        *err = GetLastError();
        return false;
    }
    const HANDLE file = CreateFileW(out.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE)
    {
        *err = GetLastError();
        return false;
    }
    const char* p = (const char*)data;
    DWORD left = size;
    while (left > 0)
    {
        DWORD wrote = 0;
        if (!WriteFile(file, p, left, &wrote, NULL) || wrote == 0)
        {
            *err = GetLastError();
            CloseHandle(file);
            return false;
        }
        p += wrote;
        left -= wrote;
    }
    CloseHandle(file);   // 保持/时间戳不进校验：解包是"内容变了才重做"，不用比时间
    return true;
}

/** 写一个空标记文件（表示这份解包是完整的） */
bool WriteMarker(const std::wstring& dir, DWORD* err)
{
    const std::wstring m = dir + L"/.ready";
    const HANDLE f = CreateFileW(m.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE)
    {
        *err = GetLastError();
        return false;
    }
    CloseHandle(f);
    return true;
}

/** 仅在互斥锁内向临时目录解包；完整写入后才发布给其它实例。 */
bool ExtractAll(HINSTANCE hInst, const std::wstring& dir, DWORD* err)
{
    for (int i = 0; i < kPayloadCount; ++i)
    {
        if (!ExtractOne(hInst, kPayloadFiles[i], dir, err))
        {
            Log(L"[portable] 解包失败：第 %d/%d 个（id=%d err=%lu）%ls",
                i + 1, kPayloadCount, kPayloadFiles[i].id, (unsigned long)*err, kPayloadFiles[i].rel);
            return false;
        }
    }
    return WriteMarker(dir, err);
}

// 锁持有到 CreateProcess 完成，避免另一个实例在检查缓存时重写正在启动的 exe。
class ExtractionLock
{
public:
    explicit ExtractionLock(const std::wstring& name)
        : handle(CreateMutexW(NULL, FALSE, name.c_str())), held(false) {}
    ~ExtractionLock()
    {
        if (held) ReleaseMutex(handle);
        if (handle != NULL) CloseHandle(handle);
    }
    bool Acquire()
    {
        if (handle == NULL) return false;
        DWORD result = WaitForSingleObject(handle, 30000);
        held = result == WAIT_OBJECT_0 || result == WAIT_ABANDONED;
        return held;
    }
private:
    HANDLE handle;
    bool held;
};

bool CacheReady(const std::wstring& dir)
{
    if (GetFileAttributesW((dir + L"/.ready").c_str()) == INVALID_FILE_ATTRIBUTES) return false;
    for (int i = 0; i < kPayloadCount; ++i)
    {
        WIN32_FILE_ATTRIBUTE_DATA info;
        if (!GetFileAttributesExW((dir + L"/" + kPayloadFiles[i].rel).c_str(), GetFileExInfoStandard, &info)
            || (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) return false;
        unsigned long long size = ((unsigned long long)info.nFileSizeHigh << 32) | info.nFileSizeLow;
        if (size != kPayloadFiles[i].size) return false;
    }
    return true;
}

bool RemoveDirectoryTree(const std::wstring& dir)
{
    if (GetFileAttributesW(dir.c_str()) == INVALID_FILE_ATTRIBUTES) return true;
    // SHFileOperation 要求双 NUL 结尾，且调用方只传本程序拥有的缓存目录。
    std::wstring paths = dir;
    paths.push_back(L'\0');
    SHFILEOPSTRUCTW op = {};
    op.wFunc = FO_DELETE;
    op.pFrom = paths.c_str();
    op.fFlags = FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_SILENT;
    return SHFileOperationW(&op) == 0 && !op.fAnyOperationsAborted;
}

bool PrepareCache(HINSTANCE hInst, const std::wstring& dir, DWORD* err)
{
    if (CacheReady(dir)) return true;
    const std::wstring temp = dir + L".tmp" + std::to_wstring(GetCurrentProcessId());
    if (!RemoveDirectoryTree(temp)) { *err = ERROR_ACCESS_DENIED; return false; }
    if (!ExtractAll(hInst, temp, err))
    {
        RemoveDirectoryTree(temp);
        return false;
    }
    // 删除上次未完成或缺文件的缓存；同一内容只有持锁者可进入这里。
    if (!RemoveDirectoryTree(dir))
    {
        *err = ERROR_ACCESS_DENIED;
        RemoveDirectoryTree(temp);
        return false;
    }
    if (!MoveFileExW(temp.c_str(), dir.c_str(), MOVEFILE_WRITE_THROUGH))
    {
        *err = GetLastError();
        RemoveDirectoryTree(temp);
        return false;
    }
    return true;
}

/** 命令行里 argv[0] 之后的部分（转发给桌宠，比如 --pet 之外的调试参数） */
const wchar_t* CmdTail()
{
    static wchar_t buf[4000];
    static bool inited = false;
    if (!inited)
    {
        inited = true;
        const wchar_t* p = GetCommandLineW();
        if (p != NULL)
        {
            while (*p == L' ' || *p == L'\t') ++p;
            if (*p == L'"') { ++p; while (*p != L'\0' && *p != L'"') ++p; if (*p == L'"') ++p; }
            else { while (*p != L'\0' && *p != L' ' && *p != L'\t') ++p; }
            while (*p == L' ' || *p == L'\t') ++p;
            if (*p != L'\0') lstrcpynW(buf, p, 4000);
        }
    }
    return (buf[0] != L'\0') ? buf : NULL;
}

}   // namespace

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR, int)
{
    const std::wstring base = LocalAppData();
    if (base.empty())
    {
        Log(L"[portable] 拿不到 %%LOCALAPPDATA%%");
        MessageBoxW(NULL, L"拿不到 %LOCALAPPDATA%，没法自解包。", L"l2dpet", MB_ICONERROR);
        return 1;
    }
    const std::wstring dir = base + L"\\l2dpet\\portable-" + Widen(kPayloadKey);
    ExtractionLock lock(L"Local\\l2dpet-portable-" + Widen(kPayloadKey));
    if (!lock.Acquire())
    {
        MessageBoxW(NULL, L"另一实例正在准备便携包，或无法获取解包锁。请稍后重试。", L"l2dpet", MB_ICONERROR);
        return 1;
    }
    LogOpen();
    Log(L"[portable] key=%ls dir=%ls", Widen(kPayloadKey).c_str(), dir.c_str());

    if (!CacheReady(dir))
    {
        Log(L"[portable] 首次运行（或上次没解完），解包到 %ls", dir.c_str());
        DWORD err = 0;
        if (!PrepareCache(hInst, dir, &err))
        {
            wchar_t msg[512];
            swprintf(msg, sizeof(msg) / sizeof(msg[0]), L"解包失败（错误 %lu）。\n目标目录：%ls\n细节见 %%TEMP%%\\l2dpet-portable.log", (unsigned long)err, dir.c_str());
            MessageBoxW(NULL, msg, L"l2dpet", MB_ICONERROR);
            return 1;
        }
        Log(L"[portable] 解包完成");
    }
    else
    {
        Log(L"[portable] 已解包过，直接用");
    }

    const std::wstring exe = dir + L"\\l2dtest.exe";
    std::wstring cmd = L"\"" + exe + L"\"";
    if (const wchar_t* tail = CmdTail())
    {
        cmd += L" ";
        cmd += tail;
    }

    STARTUPINFOW si;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    // 桌宠是**控制台子系统**程序：不指定的话 Windows 会给它新开一个控制台窗口，
    // 双击会看到黑窗口弹一下（日志窗）。这里在创建进程时就指定 SW_HIDE ——
    // 窗口从出生就是隐藏的，连闪都不会闪；日志照旧写在 %TEMP%\l2dpet.log。
    //
    // 调试用（想看那个控制台）：设 L2DPET_DEBUG_CONSOLE=1 再运行本程序，
    // 或者干脆从命令行直接跑解包出来的 l2dtest.exe（终端里日志就在眼前）。
    const bool debugConsole = GetEnvironmentVariableW(L"L2DPET_DEBUG_CONSOLE", NULL, 0) > 0;
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = debugConsole ? SW_SHOWNORMAL : SW_HIDE;
    Log(L"[portable] 启动桌宠（控制台 %ls）", debugConsole ? L"可见：L2DPET_DEBUG_CONSOLE=1" : L"隐藏");
    PROCESS_INFORMATION pi;
    ZeroMemory(&pi, sizeof(pi));
    if (!CreateProcessW(exe.c_str(), &cmd[0], NULL, NULL, FALSE, 0, NULL, dir.c_str(), &si, &pi))
    {
        const DWORD err = GetLastError();
        Log(L"[portable] CreateProcess 失败（err=%lu）：%ls", (unsigned long)err, exe.c_str());
        wchar_t msg[512];
        swprintf(msg, sizeof(msg) / sizeof(msg[0]), L"启动桌宠失败（错误 %lu）：\n%ls", (unsigned long)err, exe.c_str());
        MessageBoxW(NULL, msg, L"l2dpet", MB_ICONERROR);
        return 2;
    }
    Log(L"[portable] 桌宠已启动（pid=%lu）", (unsigned long)pi.dwProcessId);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return 0;   // 启动器自己退出，桌宠继续跑
}

/**
 * Platform.cpp — 见 Platform.h
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE   // posix_spawn_file_actions_addchdir_np（glibc）
#endif
#include "Platform.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cerrno>

#if defined(_WIN32)
#include <windows.h>
#include <direct.h>
#include <io.h>
#include <process.h>   // _spawnv / _P_NOWAIT（SpawnDetached）
// windows.h 把 ReplaceFile 定义成 ReplaceFileA/W 宏：它在本文件里会把 plat::ReplaceFile 的
// **定义**也展开改名（符号变成 plat::ReplaceFileW），而调用方（不含 windows.h 的 TU）
// 按 plat::ReplaceFile 去找 → 链接不上。这里先把宏摘掉；本文件的实现用的是 MoveFileExA。
#ifdef ReplaceFile
#undef ReplaceFile
#endif
#else
#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#include <mach/mach_time.h>
#include <crt_externs.h>   // _NSGetEnviron：Apple 的 <unistd.h> 不声明 environ
#endif
#endif

#if defined(_WIN32) && !defined(__MINGW32__)
// Platform.h 声明的时钟垫片：时间线统一用 QueryPerformanceCounter（自洽即可——
// 调用方只在垫片内部比较"绝对时刻"）。不能跟 plat::MonoNow 的时间轴混用，两者基准不同。
extern "C" int clock_gettime(int id, struct timespec* tp)
{
    (void)id;
    LARGE_INTEGER freq;
    LARGE_INTEGER now;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&now);
    const long long ns = (long long)((double)now.QuadPart * 1e9 / (double)freq.QuadPart);
    tp->tv_sec = (time_t)(ns / 1000000000LL);
    tp->tv_nsec = (long)(ns % 1000000000LL);
    return 0;
}

extern "C" int clock_nanosleep(int id, int flags, const struct timespec* req, struct timespec* rem)
{
    (void)id;
    (void)rem;
    long long ns;
    if ((flags & TIMER_ABSTIME) != 0)
    {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        ns = ((long long)req->tv_sec - (long long)now.tv_sec) * 1000000000LL
           + ((long long)req->tv_nsec - (long long)now.tv_nsec);
    }
    else
    {
        ns = (long long)req->tv_sec * 1000000000LL + (long long)req->tv_nsec;
    }
    if (ns > 0)
    {
        Sleep((DWORD)((ns + 999999LL) / 1000000LL));   // 毫秒向上取整（宁可晚醒不早醒）
    }
    return 0;
}
#endif

#if defined(_WIN32)
/** UTF-8 <-> UTF-16.
 *  Windows 的窄字符 API（无论是 CRT 的还是 ...A 版的 Win32）都是按**本地代码页**解释路径的，
 *  而工程内部字符串一律 UTF-8 —— 于是带中文的路径（模型名、用户目录、网络共享）全都找不到。
 *  所以平台层的路径操作一律走 W 版 API，在这里做一次转换；应用里剩下的 narrow 调用
 *  （fopen 等）由启动时的 _setmbcp(_MB_CP_UTF8) 兜住。
 *  (char)92 = 反斜杠，这样写是为了让源码里不出现多余的转义。 */
static std::wstring WideFromUtf8(const std::string& s)
{
    if (s.empty()) return std::wstring();
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), NULL, 0);
    if (n <= 0) return std::wstring();
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}

static std::string Utf8FromWide(const wchar_t* w)
{
    if (w == NULL || w[0] == 0) return std::string();
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    if (n <= 0) return std::string();
    std::string s((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, &s[0], n, NULL, NULL);
    s.pop_back();
    return s;
}
#endif

namespace plat
{

void SleepUntil(const struct timespec& deadline)
{
#if defined(__APPLE__)
    // Darwin 没有 clock_nanosleep；每次中断后重新计算距截止时间的间隔。
    for (;;)
    {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        long long ns = (static_cast<long long>(deadline.tv_sec) - now.tv_sec) * 1000000000LL
                     + deadline.tv_nsec - now.tv_nsec;
        if (ns <= 0) return;
        struct timespec remaining = {static_cast<time_t>(ns / 1000000000LL),
                                     static_cast<long>(ns % 1000000000LL)};
        if (nanosleep(&remaining, NULL) == 0 || errno != EINTR) return;
    }
#else
    int result;
    do { result = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &deadline, NULL); }
    while (result == EINTR);
#endif
}


double MonoNow()
{
#if defined(_WIN32)
    static LARGE_INTEGER freq = { { 0 } };
    static LARGE_INTEGER base = { { 0 } };
    if (freq.QuadPart == 0)
    {
        QueryPerformanceFrequency(&freq);
        QueryPerformanceCounter(&base);
    }
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return (double)(now.QuadPart - base.QuadPart) / (double)freq.QuadPart;
#elif defined(__APPLE__)
    static mach_timebase_info_data_t tb = { 0, 0 };
    if (tb.denom == 0) mach_timebase_info(&tb);
    return (double)mach_absolute_time() * (double)tb.numer / (double)tb.denom * 1e-9;
#else
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec * 1e-9;
#endif
}

void SleepMs(int ms)
{
#if defined(_WIN32)
    Sleep((DWORD)ms);
#else
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
#endif
}

std::string HomeDir()
{
#if defined(_WIN32)
    const char* h = getenv("USERPROFILE");
    if (h == NULL) h = getenv("HOME");
#else
    const char* h = getenv("HOME");
#endif
    return std::string(h != NULL ? h : ".");
}

bool SetEnv(const char* name, const char* value)
{
#if defined(_WIN32)
    return _putenv_s(name, value) == 0;
#else
    return setenv(name, value, 1) == 0;
#endif
}

int CpuCount()
{
#if defined(_WIN32)
    const DWORD n = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    return n > 0 ? (int)n : 1;
#else
    const long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? (int)n : 1;
#endif
}

bool Exists(const std::string& path)
{
#if defined(_WIN32)
    return GetFileAttributesW(WideFromUtf8(path).c_str()) != INVALID_FILE_ATTRIBUTES;
#else
    return access(path.c_str(), R_OK) == 0;
#endif
}

bool IsDir(const std::string& path)
{
#if defined(_WIN32)
    const DWORD a = GetFileAttributesW(WideFromUtf8(path).c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
#else
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
#endif
}

bool IsFile(const std::string& path)
{
#if defined(_WIN32)
    const DWORD a = GetFileAttributesW(WideFromUtf8(path).c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) == 0;
#else
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
#endif
}

bool IsSymlink(const std::string& path)
{
#if defined(_WIN32)
    const DWORD a = GetFileAttributesW(WideFromUtf8(path).c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
#else
    struct stat st;
    return lstat(path.c_str(), &st) == 0 && S_ISLNK(st.st_mode);
#endif
}

bool RemoveFile(const std::string& path)
{
#if defined(_WIN32)
    return DeleteFileW(WideFromUtf8(path).c_str()) != 0;
#else
    return remove(path.c_str()) == 0;
#endif
}

bool RemoveEmptyDir(const std::string& path)
{
#if defined(_WIN32)
    return RemoveDirectoryW(WideFromUtf8(path).c_str()) != 0;
#else
    return rmdir(path.c_str()) == 0;
#endif
}

bool CreateDir(const std::string& path, bool recursive)
{
    if (path.empty()) return false;
    if (IsDir(path)) return true;
    if (!recursive)
    {
#if defined(_WIN32)
        return CreateDirectoryW(WideFromUtf8(path).c_str(), NULL) != 0;
#else
        return mkdir(path.c_str(), 0700) == 0;
#endif
    }
    const size_t slash = path.find_last_of("/\\");
    if (slash != std::string::npos && slash > 0 && !CreateDir(path.substr(0, slash), true)) return false;
#if defined(_WIN32)
    return CreateDirectoryW(WideFromUtf8(path).c_str(), NULL) != 0 || IsDir(path);
#else
    return mkdir(path.c_str(), 0700) == 0 || IsDir(path);
#endif
}

bool ReadBinaryFile(const std::string& path, std::string* out)
{
    out->clear();
#if defined(_WIN32)
    FILE* f = _wfopen(WideFromUtf8(path).c_str(), L"rb");
#else
    FILE* f = fopen(path.c_str(), "rb");
#endif
    if (f == NULL) return false;
    char buf[65536];
    size_t r = 0;
    while ((r = fread(buf, 1, sizeof(buf), f)) > 0) out->append(buf, r);
    const bool bad = ferror(f) != 0;
    fclose(f);
    if (bad) { out->clear(); return false; }
    return true;
}

std::string ReadTextFile(const std::string& path)
{
    std::string text;
    ReadBinaryFile(path, &text);   // 走同一套：中文路径也能读
    return text;
}

bool FileCopy(const std::string& from, const std::string& to)
{
#if defined(_WIN32)
    return CopyFileW(WideFromUtf8(from).c_str(), WideFromUtf8(to).c_str(), FALSE) != 0;
#else
    FILE* in = fopen(from.c_str(), "rb");
    if (in == NULL) return false;
    FILE* out = fopen(to.c_str(), "wb");
    if (out == NULL) { fclose(in); return false; }
    char buf[65536];
    size_t r = 0;
    bool ok = true;
    while ((r = fread(buf, 1, sizeof(buf), in)) > 0)
    {
        if (fwrite(buf, 1, r, out) != r) { ok = false; break; }
    }
    if (ferror(in)) ok = false;
    fclose(in);
    if (fclose(out) != 0) ok = false;
    return ok;
#endif
}

bool ReplaceFile(const std::string& from, const std::string& to)
{
#if defined(_WIN32)
    return MoveFileExW(WideFromUtf8(from).c_str(), WideFromUtf8(to).c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
    return rename(from.c_str(), to.c_str()) == 0;
#endif
}

bool WritePrivateFile(const std::string& path, const std::string& text)
{
    const std::string tmp = path + ".tmp";
#if defined(_WIN32)
    FILE* f = _wfopen(WideFromUtf8(tmp).c_str(), L"wb");
    if (f == NULL) return false;
    const size_t n = text.empty() ? 0 : fwrite(text.data(), 1, text.size(), f);
    const bool closed = fclose(f) == 0;
    const bool ok = closed && n == text.size();
#else
    // 一创建就是 0600（fopen 按 umask 建成 0644，chmod 之前那一刻记忆/备份是别人可读的）
    RemoveFile(tmp);   // 残留的 tmp（或别人放的链接）先删掉，O_EXCL 保证这次是我们新建的
    const int fd = open(tmp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_TRUNC, 0600);
    if (fd < 0) return false;
    size_t off = 0;
    while (off < text.size())
    {
        const ssize_t w = write(fd, text.data() + off, text.size() - off);
        if (w <= 0) break;
        off += (size_t)w;
    }
    const bool ok = (close(fd) == 0) && off == text.size();
#endif
    if (!ok || !ReplaceFile(tmp, path))
    {
        RemoveFile(tmp);
        return false;
    }
    return true;
}

std::string Canonicalize(const std::string& path)
{
#if defined(_WIN32)
    wchar_t bufW[512];
    const std::wstring w = WideFromUtf8(path);
    const DWORD n = GetFullPathNameW(w.c_str(), (DWORD)(sizeof(bufW) / sizeof(bufW[0])), bufW, NULL);
    if (n > 0 && n < (DWORD)(sizeof(bufW) / sizeof(bufW[0]))) return Utf8FromWide(bufW);
    return path;
#else
    char buf[4096];
    if (realpath(path.c_str(), buf) != NULL) return std::string(buf);
    return path;
#endif
}

std::string SelfExePath()
{
#if defined(_WIN32)
    wchar_t bufW[4096 / sizeof(wchar_t)];
    const DWORD n = GetModuleFileNameW(NULL, bufW, (DWORD)(sizeof(bufW) / sizeof(bufW[0])));
    return n > 0 ? Utf8FromWide(bufW) : std::string();
#elif defined(__APPLE__)
    char buf[4096];
    uint32_t size = (uint32_t)sizeof(buf);
    if (_NSGetExecutablePath(buf, &size) == 0) return std::string(buf);
    return std::string();
#else
    char buf[4096];
    const ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n > 0)
    {
        buf[n] = '\0';
        return std::string(buf);
    }
    return std::string();
#endif
}

std::string Join(const std::string& dir, const std::string& name)
{
    if (dir.empty()) return name;
    const char last = dir[dir.size() - 1];
    if (last == '/' || last == '\\') return dir + name;
    return dir + "/" + name;
}

bool ListDir(const std::string& dir, std::vector<std::string>* names)
{
    names->clear();
#if defined(_WIN32)
    std::string pattern = Join(dir, "*");
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(WideFromUtf8(pattern).c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return false;
    do
    {
        const std::string n = Utf8FromWide(fd.cFileName);
        if (n != "." && n != "..") names->push_back(n);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return true;
#else
    DIR* d = opendir(dir.c_str());
    if (d == NULL) return false;
    struct dirent* e;
    while ((e = readdir(d)) != NULL)
    {
        const std::string n = e->d_name;
        if (n != "." && n != "..") names->push_back(n);
    }
    closedir(d);
    return true;
#endif
}

long CurrentPid()
{
#if defined(_WIN32)
    return (long)GetCurrentProcessId();
#else
    return (long)getpid();
#endif
}

void TerminateChild(long pid)
{
#if defined(_WIN32)
    HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, (DWORD)pid);
    if (h != NULL)
    {
        TerminateProcess(h, 0);
        CloseHandle(h);
    }
#else
    kill((pid_t)pid, SIGTERM);
#endif
}

bool ReapChild(long pid, bool* exited, int* status)
{
    *exited = false;
    *status = 0;
#if defined(_WIN32)
    HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, (DWORD)pid);
    if (h == NULL) return true;   // 打不开 = 已退出
    const DWORD w = WaitForSingleObject(h, 0);
    CloseHandle(h);
    if (w == WAIT_OBJECT_0) *exited = true;
    return true;
#else
    int st = 0;
    if (waitpid((pid_t)pid, &st, WNOHANG) == (pid_t)pid)
    {
        *exited = true;
        *status = st;
    }
    return true;
#endif
}

bool WaitChildExit(long pid, int timeoutMs, bool stopOnTimeout, int* exitCode)
{
    if (exitCode != NULL) *exitCode = -1;
    // pid <= 0 是无效句柄：POSIX 侧 waitpid(-1) 会变成"等任意子进程"，超时分支的 kill(-1)
    // 更是会波及整个会话（spawn 失败没查返回值时就是这样）。
    if (pid <= 0) return false;
#if defined(_WIN32)
    HANDLE h = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION | (stopOnTimeout ? PROCESS_TERMINATE : 0), FALSE, (DWORD)pid);
    if (h == NULL) return !stopOnTimeout;
    const DWORD w = WaitForSingleObject(h, timeoutMs < 0 ? INFINITE : (DWORD)timeoutMs);
    if (w == WAIT_TIMEOUT && stopOnTimeout) { TerminateProcess(h, 124); WaitForSingleObject(h, INFINITE); }
    DWORD code = 0;
    if (w == WAIT_OBJECT_0 && GetExitCodeProcess(h, &code) && exitCode != NULL) *exitCode = (int)code;
    CloseHandle(h);
    return w == WAIT_OBJECT_0;
#else
    for (int waited = 0;; ++waited)
    {
        int st = 0;
        const pid_t result = waitpid((pid_t)pid, &st, WNOHANG);
        if (result == (pid_t)pid)
        {
            if (exitCode != NULL) *exitCode = WIFEXITED(st) ? WEXITSTATUS(st) : -1;
            return true;
        }
        if (result < 0 && errno != EINTR) return false;
        if (timeoutMs >= 0 && waited * 20 >= timeoutMs)
        {
            if (stopOnTimeout)
            {
                kill((pid_t)pid, SIGTERM);
                for (int grace = 0; grace < 25; ++grace)
                {
                    if (waitpid((pid_t)pid, &st, WNOHANG) == (pid_t)pid) return false;
                    SleepMs(20);
                }
                kill((pid_t)pid, SIGKILL);
                while (waitpid((pid_t)pid, &st, 0) < 0 && errno == EINTR) {}
            }
            return false;
        }
        SleepMs(20);
    }
#endif
}

long SpawnDetached(const std::string& exe, const std::vector<std::string>& args, const std::string& workdir)
{
#if defined(_WIN32)
    // 用 _wspawnv（宽字符版）：argv 直接以 UTF-16 交给被启动进程，不经任何代码页转换。
    // 原来是 _spawnv —— 它把窄 argv 按当前 mbcp 转宽，路径里带中文时一旦那一步不是 UTF-8，
    // 被启动的程序拿到就是乱码路径（tar 解中文名 zip 就是这么失败的）。
    std::vector<std::wstring> wargs;
    wargs.push_back(WideFromUtf8(exe));
    for (size_t i = 0; i < args.size(); ++i) wargs.push_back(WideFromUtf8(args[i]));
    std::vector<const wchar_t*> wargv;
    for (size_t i = 0; i < wargs.size(); ++i) wargv.push_back(wargs[i].c_str());
    wargv.push_back(NULL);
    const intptr_t r = _wspawnv(_P_NOWAIT, wargs[0].c_str(), const_cast<wchar_t* const*>(wargv.data()));
    if (r == -1) return -1;
    const DWORD pid = GetProcessId((HANDLE)r);
    CloseHandle((HANDLE)r);
    return pid != 0 ? (long)pid : (long)r;
#else
    // posix_spawn（fork+exec 的现代替代）；工作目录经 file_actions 设置
    std::vector<char*> argv;
    argv.push_back(const_cast<char*>(exe.c_str()));
    for (size_t i = 0; i < args.size(); ++i) argv.push_back(const_cast<char*>(args[i].c_str()));
    argv.push_back(NULL);
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    if (!workdir.empty()) posix_spawn_file_actions_addchdir_np(&fa, workdir.c_str());
#if defined(__APPLE__)
    char** const envp = *_NSGetEnviron();   // 见文件顶部的 crt_externs.h
#else
    char** const envp = environ;
#endif
    pid_t pid = -1;
    const int rc = posix_spawn(&pid, exe.c_str(), &fa, NULL, argv.data(), envp);
    posix_spawn_file_actions_destroy(&fa);
    return rc == 0 ? (long)pid : -1;
#endif
}

}   // namespace plat

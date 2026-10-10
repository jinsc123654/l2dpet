/**
 * Platform.h — 跨平台小工具层（Linux / Windows / macOS）
 *
 * 三端化的最小公共层：时间、路径、目录枚举、环境变量、子进程。
 * 平台专属功能留在各自 Port 里，不进这层。
 */
#pragma once

#include <string>
#include <vector>
#include <time.h>   // time_t / struct tm（plat::LocalTime）；MSVC 的 struct timespec 也在这

#if defined(_WIN32) && !defined(__MINGW32__)
// MSVC 无 clock_gettime / clock_nanosleep：提供 CLOCK_MONOTONIC + TIMER_ABSTIME 垫片
// （实现见 Platform.cpp；mingw-w64 原生自带，无需垫片）
#ifndef CLOCK_MONOTONIC
#define CLOCK_MONOTONIC 1
#endif
#ifndef TIMER_ABSTIME
#define TIMER_ABSTIME 1
#endif
extern "C" int clock_gettime(int id, struct timespec* tp);
extern "C" int clock_nanosleep(int id, int flags, const struct timespec* req, struct timespec* rem);
#endif

namespace plat
{

/** 单调时钟（秒，双精度；Linux clock_gettime / Win QueryPerformanceCounter / Mac mach_absolute_time）。 */
double MonoNow();

/** 睡眠（毫秒）。 */
void SleepMs(int ms);

/** 本地时间（Linux/macOS localtime_r / Windows localtime_s——两者参数顺序相反，故包一层）。 */
inline void LocalTime(time_t t, struct tm* out)
{
#if defined(_WIN32)
    localtime_s(out, &t);
#else
    localtime_r(&t, out);
#endif
}

/** 用户主目录（HOME / USERPROFILE / NSHomeDirectory；取不到返回 "."）。 */
std::string HomeDir();

/** 设置环境变量（setenv / _putenv_s）。 */
bool SetEnv(const char* name, const char* value);

/** 逻辑 CPU 数（sysconf / GetActiveProcessorCount / hardware_concurrency；至少 1）。 */
int CpuCount();

/** 路径存在（且可读）。 */
bool Exists(const std::string& path);

/** 路径是目录 / 常规文件。 */
bool IsDir(const std::string& path);
bool IsFile(const std::string& path);

/** 路径本身是符号链接（不跟随；Windows 上是重解析点 = 符号链接/联接）。删除/拷贝目录树时据此不跟进链接。 */
bool IsSymlink(const std::string& path);

/** 删文件 / 删空目录（失败 false）。 */
bool RemoveFile(const std::string& path);
bool RemoveEmptyDir(const std::string& path);

/** 建目录（recursive=true 逐级创建）；已存在视为成功。 */
bool CreateDir(const std::string& path, bool recursive = false);

/** 读整个文本文件（失败返回空串）。 */
std::string ReadTextFile(const std::string& path);

/** 私有原子写（tmp + rename；Linux 下 0600）。返回 false = 写失败。 */
bool WritePrivateFile(const std::string& path, const std::string& text);

/** 读整个二进制文件（失败返回 false）。Windows 走 W 版打开 —— 窄字符 fopen/ofstream
 *  按**本地代码页**解释路径，带中文的模型名/目录一律打不开（系统 ACP=936 时必踩）。*/
bool ReadBinaryFile(const std::string& path, std::string* out);

/** 复制单个文件（覆盖已存在目标）。Windows 走 CopyFileW —— 路径 UTF-8 → UTF-16 转换，
 *  不能用 std::ofstream/fopen 那种窄字符路径（会按本地代码页解释，中文名必失败）。*/
bool FileCopy(const std::string& from, const std::string& to);

/** 原子替换：from → to（to 已存在也覆盖）。POSIX rename 天然覆盖；Windows 的 rename 遇到已存在目标会失败，
 *  所以走 MoveFileEx(REPLACE_EXISTING)。所有"写 tmp 再改名"的保存都应走这里。 */
bool ReplaceFile(const std::string& from, const std::string& to);

/** 前 n 个 UTF-8 **字符**（不是字节；等价于 Python 的 s[:n]，不会切出半个汉字） */
inline std::string Utf8Head(const std::string& s, size_t n)
{
    size_t i = 0;
    for (size_t k = 0; k < n && i < s.size(); ++k)
    {
        const unsigned char c = (unsigned char)s[i];
        i += c < 0x80 ? 1 : ((c & 0xE0) == 0xC0 ? 2 : ((c & 0xF0) == 0xE0 ? 3 : 4));
    }
    return i >= s.size() ? s : s.substr(0, i);
}

/** 绝对路径归一（realpath / _fullpath；失败原样返回）。 */
std::string Canonicalize(const std::string& path);

/** 当前可执行文件完整路径（/proc/self/exe / GetModuleFileName / _NSGetExecutablePath）。 */
std::string SelfExePath();

/** 拼接路径（自动处理分隔符）。 */
std::string Join(const std::string& dir, const std::string& name);

/** 枚举目录条目名（不含路径、不递归、跳过 . 与 ..）。返回 false = 目录打不开。 */
bool ListDir(const std::string& dir, std::vector<std::string>* names);

/** 当前线程 PID（调试打印用）。 */
long CurrentPid();

// ------------------------------------------------------------ 子进程（UI 进程 l2dpet-ui 的拉起/监管）
// 返回子进程 id（>0）；失败 <= 0。workdir 为空 = 继承当前目录。
long SpawnDetached(const std::string& exe, const std::vector<std::string>& args, const std::string& workdir);

/** 结束子进程（SIGTERM / TerminateProcess）。 */
void TerminateChild(long pid);

/** 非阻塞回收；exited=true 时 *status 已填（Windows 只给 0）。 */
bool ReapChild(long pid, bool* exited, int* status);

/** 阻塞等待子进程退出（最多 timeoutMs；timeoutMs<0 = 无限等）。返回 true = 已退出。 */
bool WaitChildExit(long pid, int timeoutMs);

}   // namespace plat

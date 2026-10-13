#include "../src/Platform.h"
#include <cassert>
#include <initializer_list>
#include <iostream>
#include <string>
#ifndef _WIN32
#include <signal.h>
#include <cerrno>
#include <unistd.h>
#endif

#ifndef _WIN32
// sleep / false / true 的位置各平台不同（macOS 的 false/true 在 /usr/bin，Linux 在 /bin）：
// 按候选列表取第一个真正存在可执行的，别把测试写死在某个平台的布局上。
static std::string PickBinary(std::initializer_list<const char*> candidates)
{
    for (const char* p : candidates)
    {
        if (access(p, X_OK) == 0) return p;
    }
    return *candidates.begin();
}
#endif

int main()
{
#ifndef _WIN32
    const std::string kSleep = PickBinary({"/bin/sleep", "/usr/bin/sleep"});
    const std::string kFalse = PickBinary({"/bin/false", "/usr/bin/false"});
    const std::string kTrue  = PickBinary({"/bin/true", "/usr/bin/true"});
    int code = -1;
    long child = plat::SpawnDetached(kSleep, {"20"}, "");
    assert(child > 0);
    assert(!plat::WaitChildExit(child, 20, true, &code));
    assert(kill((pid_t)child, 0) == -1 && errno == ESRCH);
    child = plat::SpawnDetached(kFalse, {}, "");
    assert(child > 0);   // spawn 失败返回 -1；放过去 waitpid(-1) 就变成"等任意子进程"了
    assert(plat::WaitChildExit(child, 1000, true, &code) && code != 0);
    child = plat::SpawnDetached(kTrue, {}, "");
    assert(child > 0);
    assert(plat::WaitChildExit(child, 1000, true, &code) && code == 0);
#endif
    std::cout << "PASS child termination, reaping and extraction exit status\n";
}

#include "../src/Platform.h"
#include <cassert>
#include <iostream>
#ifndef _WIN32
#include <signal.h>
#include <cerrno>
#endif
int main()
{
#ifndef _WIN32
    int code = -1;
    long child = plat::SpawnDetached("/bin/sleep", {"20"}, "");
    assert(child > 0);
    assert(!plat::WaitChildExit(child, 20, true, &code));
    assert(kill((pid_t)child, 0) == -1 && errno == ESRCH);
    child = plat::SpawnDetached("/bin/false", {}, "");
    assert(plat::WaitChildExit(child, 1000, true, &code) && code != 0);
    child = plat::SpawnDetached("/bin/true", {}, "");
    assert(plat::WaitChildExit(child, 1000, true, &code) && code == 0);
#endif
    std::cout << "PASS child termination, reaping and extraction exit status\n";
}

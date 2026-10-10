/**
 * Port.cpp — port 工厂（按名创建；自动探测逻辑由调用方按候选列表尝试 Initialize）
 *
 * 各 port 的编译条件由 CMake 决定；未编译的 port 在工厂里返回 NULL
 * （CreatePortXxx 的桩实现见对应文件尾部）。
 */

#include "Port.hpp"

#include <cstring>

IPort* CreatePortX11();      // Linux + X11
IPort* CreatePortX11Pet();   // Linux + X11 + Xext + Xft
IPort* CreatePortGlfwPet();  // GLFW 3.4 跨平台桌宠端口（三端）
IPort* CreatePortSdl();      // SDL2/SDL3（三端）
IPort* CreatePortHeadless(); // 纯软兜底（全平台）

IPort* CreatePort(const char* name)
{
    if (name == NULL)
    {
        return NULL; // 需要具体名字；自动探测流程见 main（依次 Initialize）
    }
    if (strcmp(name, "pet") == 0)
    {
        return CreatePortX11Pet();
    }
    if (strcmp(name, "glfwpet") == 0)
    {
        return CreatePortGlfwPet();
    }
    if (strcmp(name, "x11") == 0)
    {
        return CreatePortX11();
    }
    if (strcmp(name, "sdl") == 0)
    {
        return CreatePortSdl();
    }
    if (strcmp(name, "headless") == 0)
    {
        return CreatePortHeadless();
    }
    return NULL;
}

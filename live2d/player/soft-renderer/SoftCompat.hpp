/**
 * SoftCompat.hpp — 软渲染后端的编译器兼容垫片
 *
 * 本后端原先只在 GCC/Clang（Linux 主机 + 板卡）下编译，内联控制写的是
 * `__attribute__((...))`；MSVC（Windows 三端构建）没有这套语法，此处统一抽象。
 *
 * 线程 / 屏障 / 单调时钟 / 核数这四项也各有一份 POSIX 写法，但只有
 * CubismRenderer_Software.cpp 用得到，就地按平台分流（见该文件顶部的平台垫片），
 * 不塞进本头文件，免得 windows.h 污染软渲染的其它翻译单元。
 */
#pragma once

#if defined(_MSC_VER)

/* MSVC：类内定义的成员函数本身就是 inline，内联决策交给 /O2 */
#define L2D_INLINE_ATTR
#define L2D_NOINLINE __declspec(noinline)

#else

#define L2D_INLINE_ATTR __attribute__((always_inline))
#define L2D_NOINLINE __attribute__((noinline))

#endif

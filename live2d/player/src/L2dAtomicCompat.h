/*
 * L2dAtomicCompat.h — GCC `__atomic_*` 内建在 MSVC 下的最小等价实现
 *
 * L2dCmd.h 的 SPSC 指令环契约（宿主 → 播放器）用 GCC 内建表达原子读写，
 * Linux 侧保持原样；MSVC 没有这套内建，这里用同语义的 Interlocked 原语顶上。
 *
 * 只覆盖本工程实际用到的三个入口，且只作用于 32 位无符号通道
 * （环游标 head/tail + 诊断计数 lines/dropped/replySeq/capsReady，见 L2dCmd.h）。
 * 用宏改写而不是包函数，是为了**不动调用点**：GCC/Clang 继续走原内建，
 * MSVC 走这里；两边语义一致，源码里看不出分叉。
 *
 * 内存序：Interlocked 家族在 x86/x64 上自带全屏障（强于 acquire/release）。
 * 拿更强的序当 acquire/release 用是安全的（只会更保守），
 * 而 SPSC 环本就不靠弱序换性能，因此不额外区分 order 参数。
 */
#ifndef L2D_ATOMIC_COMPAT_H
#define L2D_ATOMIC_COMPAT_H

#if defined(_MSC_VER) && !defined(__clang__)

#include <intrin.h>

#ifndef __ATOMIC_RELAXED
#define __ATOMIC_RELAXED 0
#endif
#ifndef __ATOMIC_CONSUME
#define __ATOMIC_CONSUME 1
#endif
#ifndef __ATOMIC_ACQUIRE
#define __ATOMIC_ACQUIRE 2
#endif
#ifndef __ATOMIC_RELEASE
#define __ATOMIC_RELEASE 3
#endif
#ifndef __ATOMIC_ACQ_REL
#define __ATOMIC_ACQ_REL 4
#endif
#ifndef __ATOMIC_SEQ_CST
#define __ATOMIC_SEQ_CST 5
#endif

/* 读：lock cmpxchg(ptr,0,0) = 原子读 + 全屏障（顺带挡住编译器重排） */
#define __atomic_load_n(ptr, order) \
    ((unsigned int)_InterlockedCompareExchange((volatile long*)(ptr), 0, 0))

/* 写：xchg 自带屏障，写完对其它线程立即可见 */
#define __atomic_store_n(ptr, val, order) \
    ((void)_InterlockedExchange((volatile long*)(ptr), (long)(val)))

/* 加并返回**新值**（与 __atomic_add_fetch 的返回语义一致） */
#define __atomic_add_fetch(ptr, val, order) \
    ((unsigned int)(_InterlockedExchangeAdd((volatile long*)(ptr), (long)(val)) + (long)(val)))

#endif /* _MSC_VER && !__clang__ */

#endif /* L2D_ATOMIC_COMPAT_H */

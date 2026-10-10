/**
 * PlayerAllocator.h — Cubism 框架用的分配器（含对齐分配）。
 *
 * 从 l2dtest.cpp 拆出（纯搬移，逻辑未改）：类内定义。
 * 注意 Windows 上 _aligned_malloc 必须配 _aligned_free（见实现里的注释）。
 */
#pragma once

#include <CubismFramework.hpp>
#include <cstdlib>

#if defined(_WIN32)
#include <malloc.h>   // _aligned_malloc / _aligned_free
#endif

using namespace Live2D::Cubism::Framework;

class L2DAllocator : public ICubismAllocator
{
public:
    virtual void* Allocate(const csmSizeType size) override
    {
        return malloc(size);
    }

    virtual void Deallocate(void* address) override
    {
        free(address);
    }

    virtual void* AllocateAligned(const csmSizeType size, const csmUint32 alignment) override
    {
        const size_t align = alignment < sizeof(void*) ? sizeof(void*) : alignment;
#if defined(_WIN32)
        return _aligned_malloc(size, align);   // 必须配 _aligned_free（见 DeallocateAligned）
#else
        void* ptr = NULL;
        if (posix_memalign(&ptr, align, size) != 0)
        {
            return NULL;
        }
        return ptr;
#endif
    }

    virtual void DeallocateAligned(void* address) override
    {
#if defined(_WIN32)
        _aligned_free(address);   // 不能与 free 混用（_aligned_malloc 的分配头在指针前面）
#else
        free(address);
#endif
    }
};

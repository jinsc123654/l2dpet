/**
 * SysStats.h — 系统状态采样（CPU / 内存；Linux 读 /proc，Windows 走 Win32 计数器）。
 *
 * 从 l2dtest.cpp 拆出（纯搬移，逻辑未改）：类内定义，本就是头文件件。
 */
#pragma once

#include <CubismFramework.hpp>   // csmFloat32 / csmBool 等基础类型
#include <cstring>               // strncmp（MSVC 由其它头间接带入，gcc/clang 不会）
#include <string>

using namespace Live2D::Cubism::Framework;

/** /proc 采样（0.5s 间隔；CPU 为两采样差分的总体占用率） */
class SysStats
{
public:
    SysStats()
        : _accum(0.0f)
        , _cpu(-1)
        , _mem(-1)
    {
        _prevBusy = 0;
        _prevTotal = 0;
    }

    void Update(csmFloat32 dt)
    {
        _accum += dt;
        if (_accum < 0.5f)
        {
            return;
        }
        _accum = 0.0f;

        // --- /proc/stat 第一行（cpu 总计）---
        FILE* fp = fopen("/proc/stat", "r");
        if (fp != NULL)
        {
            char line[256];
            if (fgets(line, sizeof(line), fp) != NULL &&
                strncmp(line, "cpu ", 4) == 0)
            {
                unsigned long long user = 0, nice = 0, sys = 0, idle = 0;
                unsigned long long iowait = 0, irq = 0, softirq = 0, steal = 0;
                if (sscanf(line + 4, "%llu %llu %llu %llu %llu %llu %llu %llu",
                           &user, &nice, &sys, &idle, &iowait, &irq, &softirq, &steal) >= 4)
                {
                    const unsigned long long busy = user + nice + sys + irq + softirq + steal;
                    const unsigned long long total = busy + idle + iowait;
                    if (_prevTotal > 0 && total > _prevTotal)
                    {
                        const double dBusy = (double)(busy - _prevBusy);
                        const double dTotal = (double)(total - _prevTotal);
                        _cpu = (int)(dBusy * 100.0 / dTotal + 0.5);
                        if (_cpu > 100) _cpu = 100;
                    }
                    _prevBusy = busy;
                    _prevTotal = total;
                }
            }
            fclose(fp);
        }

        // --- /proc/meminfo ---
        fp = fopen("/proc/meminfo", "r");
        if (fp != NULL)
        {
            char line[256];
            unsigned long long total = 0, avail = 0;
            while (fgets(line, sizeof(line), fp) != NULL)
            {
                if (strncmp(line, "MemTotal:", 9) == 0)
                {
                    sscanf(line + 9, "%llu", &total);
                }
                else if (strncmp(line, "MemAvailable:", 13) == 0)
                {
                    sscanf(line + 13, "%llu", &avail);
                }
            }
            fclose(fp);
            if (total > 0)
            {
                _mem = (int)((total - avail) * 100ULL / total);
            }
        }
    }

    int Cpu() const { return _cpu; }
    int Mem() const { return _mem; }

private:
    csmFloat32 _accum;
    unsigned long long _prevBusy;
    unsigned long long _prevTotal;
    int _cpu;
    int _mem;
};

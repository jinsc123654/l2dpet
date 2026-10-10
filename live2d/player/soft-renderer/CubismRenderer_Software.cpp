/**
 * CubismRenderer_Software.cpp — 软件光栅化实现（规格来源：官方 GLES2 后端 + StandardES shader）
 *
 * 混合表（官方 CubismShader_OpenGLES2::SetupShaderProgramForDrawable 的 5.2- 路径，
 * 着色器输出为预乘 alpha：gl_FragColor = vec4(color.rgb*color.a, color.a)）：
 *   Normal  (csmColorBlendType_Normal=0)             : SRC=ONE,       DST=ONE_MINUS_SRC_ALPHA
 *   Add     (csmColorBlendType_AddCompatible=1)      : SRC=ONE,       DST=ONE
 *   Multiply(csmColorBlendType_MultiplyCompatible=2) : SRC=DST_COLOR, DST=ONE_MINUS_SRC_ALPHA
 *   （alpha 通道：Normal: SRC=ONE,DST=ONE_MINUS_SRC_ALPHA；Add/Mult: SRC=ZERO,DST=ONE → alpha 保持）
 *
 * 顶点变换：CubismMatrix44 为行主序存储；官方以 glUniformMatrix4fv(transpose=GL_FALSE)
 * 上传（GL 按列主序解释 ⇒ 等价于行向量右乘）：
 *   x' = x*m[0] + y*m[4] + m[12]   y' = x*m[1] + y*m[5] + m[13]   w' = x*m[3] + y*m[7] + m[15]
 * 屏幕映射：sx = (x'/w'+1)*0.5*W，sy = (1-y'/w')*0.5*H（GL NDC 的 y 向上 → 屏幕 y 向下）
 */

#include "CubismRenderer_Software.hpp"
#include "SoftTexture.hpp"

#include <Model/CubismModel.hpp>
#include <Math/CubismMatrix44.hpp>
#include <Utils/CubismDebug.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#if defined(_WIN32)
// Win32 线程池/屏障/单调时钟：LEAN_AND_MEAN 省掉 rpc.h 等（后者会把 small/hyper
// 定义成宏）；NOMINMAX 防止 min/max 宏污染标准库。
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <intrin.h>   // _InterlockedExchangeAdd
#else
#include <pthread.h>
#include <unistd.h>
#endif
#include <time.h>

#include "l2d_config.h" // 统一配置（L2D_MAX_THREADS / L2D_ENABLE_NEON 等）
#include "SoftCompat.hpp" // 内联属性垫片（L2D_INLINE_ATTR / L2D_NOINLINE）

// NEON 内核开关：意愿开关（L2D_ENABLE_NEON）× 平台能力（__ARM_NEON）。
// x86/主机构建 __ARM_NEON 未定义 → 自动走标量，功能与之前完全一致。
#if L2D_ENABLE_NEON && defined(__ARM_NEON)
#define L2D_NEON_KERNEL 1
#include <arm_neon.h>
#else
#define L2D_NEON_KERNEL 0
#endif

// ---------------------------------------------------------------------------
// 平台垫片：线程池原语 + 单调时钟 + 核数
//   POSIX（Linux 主机 / 板卡）：pthread + pthread_barrier + sched_yield
//   Win32（Windows 三端）      ：CreateThread + SYNCHRONIZATION_BARRIER + SwitchToThread
// 语义对齐：屏障计数都含调用者自己（pthread_barrier 与 Win32 屏障同义）。
// ---------------------------------------------------------------------------
#if defined(_WIN32)

typedef HANDLE                  L2dThread;
typedef SYNCHRONIZATION_BARRIER L2dBarrier;

static int L2dThreadCreate(L2dThread* t, LPTHREAD_START_ROUTINE fn, void* arg)
{
    *t = CreateThread(NULL, 0, fn, arg, 0, NULL);
    return (*t != NULL) ? 0 : -1;
}

static void L2dThreadJoin(L2dThread t)
{
    WaitForSingleObject(t, INFINITE);
    CloseHandle(t);
}

static void L2dBarrierInit(L2dBarrier* b, int count)
{
    InitializeSynchronizationBarrier(b, (LONG)count, -1);
}
static void L2dBarrierWait(L2dBarrier* b) { EnterSynchronizationBarrier(b, 0); }
static void L2dBarrierDestroy(L2dBarrier* b) { DeleteSynchronizationBarrier(b); }

static void L2dYield() { SwitchToThread(); }

static int L2dCpuCount()
{
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return (si.dwNumberOfProcessors > 0) ? (int)si.dwNumberOfProcessors : 1;
}

static double L2dMonotonicNow()
{
    LARGE_INTEGER freq;
    LARGE_INTEGER now;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&now);
    return (double)now.QuadPart / (double)freq.QuadPart;
}

// 工作线程入口返回类型（pthread 是 void*，Win32 回调是 DWORD WINAPI）
#define L2D_THREAD_ENTRY DWORD WINAPI

#else

typedef pthread_t         L2dThread;
typedef pthread_barrier_t L2dBarrier;

static int L2dThreadCreate(L2dThread* t, void* (*fn)(void*), void* arg)
{
    return pthread_create(t, NULL, fn, arg);
}

static void L2dThreadJoin(L2dThread t) { pthread_join(t, NULL); }

static void L2dBarrierInit(L2dBarrier* b, int count) { pthread_barrier_init(b, NULL, (unsigned)count); }
static void L2dBarrierWait(L2dBarrier* b) { pthread_barrier_wait(b); }
static void L2dBarrierDestroy(L2dBarrier* b) { pthread_barrier_destroy(b); }

static void L2dYield() { sched_yield(); }

static int L2dCpuCount()
{
    const long online = sysconf(_SC_NPROCESSORS_ONLN);
    return (online > 0) ? (int)online : 1;
}

static double L2dMonotonicNow()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

#define L2D_THREAD_ENTRY void*

#endif

// 原子加（两种实现都返回"加之前的原值"）：GCC 内建 ↔ MSVC 内联函数。
// 用于行带/掩码组的动态分配游标（多线程竞争同一条递增序列）。
// 写成模板是因为 csmInt32 定义在 Live2D::Cubism::Framework 里，
// 本垫片在全局作用域——模板实参推导不必写出那个限定名。
template <typename T>
static T L2dAtomicAdd(volatile T* p, T v)
{
    static_assert(sizeof(T) == 4, "L2dAtomicAdd 只用于 32 位原子量（Interlocked 家族）");
#if defined(_WIN32)
    return (T)_InterlockedExchangeAdd(reinterpret_cast<volatile long*>(p), (long)v);
#else
    return __sync_fetch_and_add(p, v);
#endif
}

namespace Live2D { namespace Cubism { namespace Framework { namespace Rendering {

// ---------------------------------------------------------------------------
// 性能剖析（L2D_DBG_PROF=1 时启用；关闭时代价 = 每 drawable/每像素各一个可预测分支）
// 目的：给出"阶段级"耗时分解（memset / 顶点变换 / 光栅化 / 其余 / UI / 线程管理）
// 与像素级计数（扫描像素 / 通过摘要检查 / 实际混合），配合 L2D_DBG_NEAREST /
// L2D_DBG_RAW 两个开关即可把逐像素成本拆成 采样-数学-混合 三块。
// ---------------------------------------------------------------------------

struct CubismRenderer_Software::SoftProf
{
    double tMemset;      // 帧缓冲清零
    double tVert;        // 顶点 MVP 变换
    double tRaster;      // RasterTriangleScanline（含扫描线插值 + 逐像素循环）
    double tOther;       // drawable 循环其余部分（可见性/混合模式/基色/索引剔除/调用）
    double tThread;      // 线程创建 + join（pthread / Win32 同一套垫片）
    double tUi;          // UI（状态栏/缩放条/圆环）
    double tMask;        // 剪裁掩码阶段（条带并行前，含并行墙钟）

    csmUint64 spanPx;      // 扫描线跨度像素（三角形覆盖率上界）
    csmUint64 sampledPx;   // 通过透明摘要检查、进入采样的像素
    csmUint64 blendedPx;   // texel alpha != 0、真正参与混合的像素
    csmUint64 triCount;    // 实际光栅化的三角形数
    csmUint64 drawCalls;   // drawable 循环迭代数（含被跳过的）
    csmUint64 frames;      // 帧数
    double workerBusy;     // 工作线程光栅时间累积（判负载均衡）
    csmUint64 workerSamples;

    SoftProf()
    {
        memset(this, 0, sizeof(*this));
    }

    void Add(const SoftProf& o)
    {
        tMemset += o.tMemset; tVert += o.tVert; tRaster += o.tRaster;
        tOther += o.tOther; tThread += o.tThread; tUi += o.tUi; tMask += o.tMask;
        spanPx += o.spanPx; sampledPx += o.sampledPx; blendedPx += o.blendedPx;
        triCount += o.triCount; drawCalls += o.drawCalls; frames += o.frames;
        workerBusy += o.workerBusy; workerSamples += o.workerSamples;
    }
};

// 全局累加器（主线程在 join 后汇总；渲染线程只写自己的 job 局部实例）
static CubismRenderer_Software::SoftProf g_prof;

namespace {

inline double ProfNow()
{
    return L2dMonotonicNow();
}

// 调试开关（namespace 级静态：无每帧 getenv 开销）
const bool kRawDebug = (getenv("L2D_DBG_RAW") != NULL);
const bool kNearestDebug = (getenv("L2D_DBG_NEAREST") != NULL);
const bool kProf = (getenv("L2D_DBG_PROF") != NULL);

// 诊断：跳过逐像素处理（只保留三角形/扫描线骨架），用于分解 setup 与像素成本
const bool kNoPixelDebug = (getenv("L2D_DBG_NOPIXEL") != NULL);

// 诊断：关闭剪裁掩码（同二进制 A/B 对照；也可作为异常时的降级开关）
const bool kNoMaskDebug = (getenv("L2D_DBG_NOMASK") != NULL);

// 诊断：关闭背面剔除（定位"近退化三角形随形变过零 → 整片闪烁"类问题）
const bool kNoCullDebug = (getenv("L2D_DBG_NOCULL") != NULL);

// 诊断：只绘制指定的 drawable（逗号分隔序号）——用于定位"某个方块/异常图层"是谁画的
// 诊断：L2D_DBG_SKIP=<逗号分隔序号> 跳过指定 drawable（与 ONLY 相反：看"少了它是什么样"）
// 诊断：L2D_DBG_BBOX=1 逐 drawable 转储屏幕包围盒/可见性/纹理/混合（定位用）
const char* kOnlyDrawablesEnv = getenv("L2D_DBG_ONLY");
const char* kSkipDrawablesEnv = getenv("L2D_DBG_SKIP");
const bool kBboxDebug = (getenv("L2D_DBG_BBOX") != NULL);

/** 解析逗号分隔序号表，命中返回 true（供 ONLY/SKIP 两个开关共用） */
static bool DrawableListContains(const char* list, csmInt32 index)
{
    if (list == NULL)
    {
        return false;
    }
    const char* p = list;
    while (*p != '\0')
    {
        char* end = NULL;
        const long v = strtol(p, &end, 10);
        if (end == p)
        {
            break;
        }
        if (v == (long)index)
        {
            return true;
        }
        p = end;
        while (*p == ',' || *p == ' ')
        {
            ++p;
        }
    }
    return false;
}

bool IsOnlyDrawable(csmInt32 index)
{
    return kOnlyDrawablesEnv == NULL || DrawableListContains(kOnlyDrawablesEnv, index);
}

bool IsDrawableSkipped(csmInt32 index)
{
    return DrawableListContains(kSkipDrawablesEnv, index);
}

// 诊断：打印掩码组结构/掩码 drawable 属性，并把掩码缓冲转储为 PGM
const char* kMaskDumpDir = getenv("L2D_DBG_MASKDUMP");
const bool kMaskLogDebug = (getenv("L2D_DBG_MASKS") != NULL);




enum SoftBlend
{
    SoftBlend_Normal = 0,
    SoftBlend_Add = 1,
    SoftBlend_Multiply = 2
};

struct ScreenVertex
{
    csmFloat32 x;
    csmFloat32 y;
    csmFloat32 u;
    csmFloat32 v;
};

inline void TransformVtx(const csmFloat32* m, csmFloat32 x, csmFloat32 y,
                         csmFloat32 width, csmFloat32 height,
                         csmFloat32* outX, csmFloat32* outY)
{
    const csmFloat32 px = x * m[0] + y * m[4] + m[12];
    const csmFloat32 py = x * m[1] + y * m[5] + m[13];
    const csmFloat32 pw = x * m[3] + y * m[7] + m[15];
    const csmFloat32 invW = (pw != 0.0f) ? (1.0f / pw) : 1.0f;
    *outX = (px * invW + 1.0f) * 0.5f * width;
    *outY = (1.0f - py * invW) * 0.5f * height;
}

inline csmUint32 ClampByte(csmFloat32 v)
{
    const csmInt32 iv = (csmInt32)(v + 0.5f);
    return (csmUint32)(iv < 0 ? 0 : (iv > 255 ? 255 : iv));
}

// 屏幕空间线性插值器（绕过 y 的增量）
struct ScanInterp
{
    csmFloat32 x0, y0, u0, v0;
    csmFloat32 dxdy, dudy, dvdy;

    void Init(const ScreenVertex& p0, const ScreenVertex& p1)
    {
        const csmFloat32 dy = p1.y - p0.y;
        if (dy > 1e-9f || dy < -1e-9f)
        {
            const csmFloat32 invDy = 1.0f / dy;
            dxdy = (p1.x - p0.x) * invDy;
            dudy = (p1.u - p0.u) * invDy;
            dvdy = (p1.v - p0.v) * invDy;
        }
        else
        {
            dxdy = dudy = dvdy = 0.0f;
        }
        x0 = p0.x; y0 = p0.y; u0 = p0.u; v0 = p0.v;
    }

    inline void At(csmFloat32 y, csmFloat32* x, csmFloat32* u, csmFloat32* v) const
    {
        const csmFloat32 t = y - y0;
        *x = x0 + dxdy * t;
        *u = u0 + dudy * t;
        *v = v0 + dvdy * t;
    }
};

/**
 * 单个纹素的片元着色 + 混合（官方片元语义；0..255 量纲）：
 *   texel × multiply → screen 合成 → × base(模型色×opacity) → 预乘 alpha → 混合
 * 从扫描线内循环抽出来，便于预取路径/普通路径共用与后续向量化。
 */
inline void ShadeAndBlend(const csmUint8 texel[4], const csmFloat32 baseRGBA[4],
                          const csmFloat32 mulRGB[3], const csmFloat32 scrRGB[3],
                          int blendMode, csmUint32* dstPixel,
                          csmInt32 maskByte = 255)
{
    const csmFloat32 inv255 = 1.0f / 255.0f;

    // —— 官方片元着色器语义（mul/scr/base 为 0..1 系数）——
    csmFloat32 r0 = texel[0] * mulRGB[0];
    csmFloat32 g0 = texel[1] * mulRGB[1];
    csmFloat32 b0 = texel[2] * mulRGB[2];
    // screen: c' = c + s - c*s（0..255 量纲 ⇒ c + s*255 - c*s）
    r0 = r0 + scrRGB[0] * 255.0f - r0 * scrRGB[0];
    g0 = g0 + scrRGB[1] * 255.0f - g0 * scrRGB[1];
    b0 = b0 + scrRGB[2] * 255.0f - b0 * scrRGB[2];

    const csmFloat32 maskMul = (maskByte != 255) ? (csmFloat32)maskByte * inv255 : 1.0f;
    const csmFloat32 sa = texel[3] * baseRGBA[3] * maskMul; // 0..255（乘掩码）
    const csmFloat32 sr = r0 * baseRGBA[0] * (sa * inv255); // 预乘，0..255
    const csmFloat32 sg = g0 * baseRGBA[1] * (sa * inv255);
    const csmFloat32 sb = b0 * baseRGBA[2] * (sa * inv255);

    if (kRawDebug)
    {
        *dstPixel = 0xff000000u | (ClampByte(r0 * baseRGBA[0]) << 16)
                                | (ClampByte(g0 * baseRGBA[1]) << 8)
                                | ClampByte(b0 * baseRGBA[2]);
        return;
    }

    const csmUint32 dst = *dstPixel;
    const csmFloat32 da = (csmFloat32)((dst >> 24) & 0xff);
    const csmFloat32 dr = (csmFloat32)((dst >> 16) & 0xff);
    const csmFloat32 dg = (csmFloat32)((dst >> 8) & 0xff);
    const csmFloat32 db = (csmFloat32)(dst & 0xff);

    csmFloat32 orr, og, ob, oa;
    switch (blendMode)
    {
    case SoftBlend_Add:
        // SRC=ONE, DST=ONE（alpha 通道不变）
        orr = sr + dr;
        og = sg + dg;
        ob = sb + db;
        oa = da;
        break;
    case SoftBlend_Multiply:
        // SRC=DST_COLOR, DST=ONE_MINUS_SRC_ALPHA（alpha 通道不变）
        orr = sr * dr * inv255 + dr * (255.0f - sa) * inv255;
        og = sg * dg * inv255 + dg * (255.0f - sa) * inv255;
        ob = sb * db * inv255 + db * (255.0f - sa) * inv255;
        oa = da;
        break;
    case SoftBlend_Normal:
    default:
        // SRC=ONE, DST=ONE_MINUS_SRC_ALPHA
        orr = sr + dr * (255.0f - sa) * inv255;
        og = sg + dg * (255.0f - sa) * inv255;
        ob = sb + db * (255.0f - sa) * inv255;
        oa = sa + da * (255.0f - sa) * inv255;
        break;
    }

    *dstPixel = (ClampByte(oa) << 24) | (ClampByte(orr) << 16) | (ClampByte(og) << 8) | ClampByte(ob);
}

// ---------------------------------------------------------------------------
// 整数定点逐像素管线（默认路径；L2D_DBG_FLOAT=1 可切回浮点用于 A/B）
//
// 动机（板上实测）：A7 上标量浮点吞吐 ~2.25ns/op、f→i 转换 ~19 周期，而逐像素
// 管线每像素有 ~120 条浮点指令 + 8 次转换 → 单线程实测 536ns/像素（≈800 周期）。
// 整数化后的每像素路径：
//   · 坐标走查：纹理空间 (整数部分, 16.16 小数) 双累加器，纯整数加减移位
//   · 双线性：SWAR 打包（R/B 一个 32 位字、G/A 一个 32 位字），8 位权重和恰为
//     256 → 4 个 texel 的加权和只 >>8 一次（每 16 位道独立、无进位）
//   · 着色：预乘系数折叠成每 drawable 常数 kb=(base*baseA)>>8，
//     sr=(texR*texA*kbR)>>16、sa=(texA*baseA8)>>8
//   · 混合：官方三式整数化，/255 用 (x+128)*257>>16（0..65535 内精确四舍五入）
// 语义与浮点路径一致（仅四舍五入位置不同，实测 ≤1 LSB）。
// ---------------------------------------------------------------------------

const bool kFloatDebug = (getenv("L2D_DBG_FLOAT") != NULL);

inline csmInt32 ClampByteI(csmInt32 v)
{
    return v < 0 ? 0 : (v > 255 ? 255 : v);
}

/** x/255 的精确四舍五入（0 ≤ x ≤ 65535） */
inline csmInt32 Div255(csmInt32 x)
{
    return ((x + 128) * 257) >> 16;
}

/** 每 drawable 的整数着色常数（PrepareFrame 预计算；classic=false 时走浮点路径） */
struct IntShadeParams
{
    const csmUint8* pixels;
    const csmUint8* summary;
    csmInt32 texW, texH;
    csmInt32 summaryW, summaryH;
    csmInt32 baseA8;         // base.A × 256
    csmInt32 kbR, kbG, kbB;  // (base.C×256 × base.A×256) >> 8
};

/**
 * 每像素整数内核（标量 SWAR 版 = **默认路径**；见下方 NEON 内核的负结果说明）
 * "纯标量路径"与"NEON 路径的收尾像素"共用，保证两条路径的逐位语义只有一份实现。
 * 返回：0 = 摘要预判全透明（跳过）；1 = 采样后全透明（跳过）；2 = 已混合
 */
inline csmInt32 RasterPixelScalar(csmUint32* dst, csmInt32 x0, csmInt32 y0,
                                 csmInt32 fxF, csmInt32 fyF,
                                 const IntShadeParams& sh, csmInt32 blendMode,
                                 csmInt32 maskByte)
{
    const csmInt32 texW = sh.texW;
    const csmInt32 texH = sh.texH;
    const csmInt32 baseA8 = sh.baseA8;
    const csmInt32 kbR = sh.kbR, kbG = sh.kbG, kbB = sh.kbB;

    // 透明摘要预判（整数下标，直接命中；同浮点版为保守 3×3 块判全零）
    if (sh.summary != NULL)
    {
        const csmInt32 bx = x0 >> 2;
        const csmInt32 by = y0 >> 2;
        if (bx >= 1 && by >= 1 && bx < sh.summaryW - 1 && by < sh.summaryH - 1)
        {
            const csmUint8* srow = sh.summary + (size_t)(by - 1) * sh.summaryW + (bx - 1);
            if (!(srow[0] | srow[1] | srow[2]) &&
                !(srow[sh.summaryW] | srow[sh.summaryW + 1] | srow[sh.summaryW + 2]) &&
                !(srow[2 * sh.summaryW] | srow[2 * sh.summaryW + 1] | srow[2 * sh.summaryW + 2]))
            {
                return 0;
            }
        }
    }

    // 钳位（GL_CLAMP_TO_EDGE；x0 可为 -1，x1 = x0+1）
    const csmInt32 x1 = x0 + 1;
    const csmInt32 y1 = y0 + 1;
    const csmInt32 cx0 = x0 < 0 ? 0 : (x0 >= texW ? texW - 1 : x0);
    const csmInt32 cx1 = x1 < 0 ? 0 : (x1 >= texW ? texW - 1 : x1);
    const csmInt32 cy0 = y0 < 0 ? 0 : (y0 >= texH ? texH - 1 : y0);
    const csmInt32 cy1 = y1 < 0 ? 0 : (y1 >= texH ? texH - 1 : y1);

    const csmUint32* texRow0 = (const csmUint32*)(sh.pixels + (size_t)cy0 * texW * 4);
    const csmUint32* texRow1 = (const csmUint32*)(sh.pixels + (size_t)cy1 * texW * 4);
    const csmUint32 t00 = texRow0[cx0];
    const csmUint32 t10 = texRow0[cx1];
    const csmUint32 t01 = texRow1[cx0];
    const csmUint32 t11 = texRow1[cx1];

    // 8 位双线性权重（和恰为 256 → 输出为 0..255 且 4 项和 ≤ 65280 不串道）
    const csmInt32 tx8 = (fxF >> 8) & 0xff;
    const csmInt32 ty8 = (fyF >> 8) & 0xff;
    const csmInt32 itx = 256 - tx8;
    const csmInt32 ity = 256 - ty8;
    const csmInt32 w00 = (itx * ity) >> 8;
    const csmInt32 w10 = (tx8 * ity) >> 8;
    const csmInt32 w01 = (itx * ty8) >> 8;
    const csmInt32 w11 = 256 - w00 - w10 - w01;

    // SWAR 双线性：R/B 一个 32 位字（16 位道）、G/A 一个 32 位字
    const csmUint32 rb = (((t00 & 0x00FF00FFu) * (csmUint32)w00
                         + (t10 & 0x00FF00FFu) * (csmUint32)w10
                         + (t01 & 0x00FF00FFu) * (csmUint32)w01
                         + (t11 & 0x00FF00FFu) * (csmUint32)w11) >> 8);
    const csmUint32 ga = ((((t00 >> 8) & 0x00FF00FFu) * (csmUint32)w00
                         + ((t10 >> 8) & 0x00FF00FFu) * (csmUint32)w10
                         + ((t01 >> 8) & 0x00FF00FFu) * (csmUint32)w01
                         + ((t11 >> 8) & 0x00FF00FFu) * (csmUint32)w11) >> 8);

    // >>8 后：R/G 结果在 bit0-7，B/A 结果在 bit16-23（各自 16 位道的低字节）
    const csmInt32 texR = (csmInt32)(rb & 0xffu);
    const csmInt32 texB = (csmInt32)((rb >> 16) & 0xffu);
    const csmInt32 texG = (csmInt32)(ga & 0xffu);
    const csmInt32 texA = (csmInt32)((ga >> 16) & 0xffu);
    if (texA == 0)
    {
        return 1; // 全透明：任何混合模式下都无贡献
    }

    // 着色（预乘；base 与 baseA 折叠为每 drawable 常数）
    csmInt32 sa = (texA * baseA8 + 128) >> 8;                     // 0..255
    csmInt32 sr = (texR * texA * kbR + 32768) >> 16;              // 预乘 0..255
    csmInt32 sg = (texG * texA * kbG + 32768) >> 16;
    csmInt32 sb = (texB * texA * kbB + 32768) >> 16;

    if (maskByte != 255)
    {
        // 剪裁掩码：预乘色与 alpha 整体 × m（官方 Mask shader col_formask * maskVal；
        // maskByte=255 为恒等（Div255 精确），做单分支跳过）
        sa = Div255(sa * maskByte);
        sr = Div255(sr * maskByte);
        sg = Div255(sg * maskByte);
        sb = Div255(sb * maskByte);
    }

    const csmUint32 d = *dst;
    const csmInt32 inv = 255 - sa;
    csmInt32 oa, orr, og, ob;
    switch (blendMode)
    {
    case SoftBlend_Add:
        orr = sr + (csmInt32)((d >> 16) & 0xffu);
        og = sg + (csmInt32)((d >> 8) & 0xffu);
        ob = sb + (csmInt32)(d & 0xffu);
        oa = (csmInt32)(d >> 24);
        if (orr > 255) orr = 255;
        if (og > 255) og = 255;
        if (ob > 255) ob = 255;
        break;
    case SoftBlend_Multiply:
    {
        const csmInt32 dr = (csmInt32)((d >> 16) & 0xffu);
        const csmInt32 dg = (csmInt32)((d >> 8) & 0xffu);
        const csmInt32 db = (csmInt32)(d & 0xffu);
        orr = Div255(sr * dr) + Div255(dr * inv);
        og = Div255(sg * dg) + Div255(dg * inv);
        ob = Div255(sb * db) + Div255(db * inv);
        oa = (csmInt32)(d >> 24);
        break;
    }
    case SoftBlend_Normal:
    default:
        orr = sr + Div255((csmInt32)((d >> 16) & 0xffu) * inv);
        og = sg + Div255((csmInt32)((d >> 8) & 0xffu) * inv);
        ob = sb + Div255((csmInt32)(d & 0xffu) * inv);
        oa = sa + Div255((csmInt32)(d >> 24) * inv);
        break;
    }

    *dst = ((csmUint32)ClampByteI(oa) << 24) | ((csmUint32)ClampByteI(orr) << 16)
         | ((csmUint32)ClampByteI(og) << 8) | (csmUint32)ClampByteI(ob);
    return 2;
}

// ---------------------------------------------------------------------------
// NEON 内核（L2D_ENABLE_NEON=1 且 __ARM_NEON 时启用；x86 等平台自动走标量）
//
// ★ 实测结论（2026-10-08，RK3506/3×Cortex-A7/1.5GHz，板上 A/B）：
//   本内核**逐位正确**但与标量 SWAR 相比**慢 5~6%**（sage 14.77→15.65、
//   Hiyori 21.62→22.59 render ms/帧；A/B 各 3 轮交替、数值稳定）。
//   故 config 默认 L2D_ENABLE_NEON=0（标量），本内核保留为可选项（置 1 启用）。
//   原因（分阶段实测 + 反汇编）：
//     · 逐像素成本构成：纹理采样 ~32%、混合 ~10%、着色 ~3%、几何/其余 ~55%
//       —— 访存延迟主导，不是算力受限，向量化能吃的算术面本就有限；
//     · A7 的 NEON 是 64 位通路，本内核把标量的短整数串换成
//       NMUL→VMULL.U8→VSHR→VMUL/VMLAL→VRSHRN→混合 约 12~15 条串行 NEON
//       （每条 2~4 周期），顺序核填不满；
//     · 另外要付 ARM↔NEON 搬运（8 条/2 像素）+ 16 个 d 寄存器不够导致的
//       溢出（内循环实测仍有 ~10 条 d 寄存器 vldr/vstr）。
//   —— "省下的指令 < 拆装/延迟代价"，与 §13.6 上轮的定性判断一致，本轮坐实。
//
// 设计（2 像素/迭代，8 个 16 位道 = 2 像素 × RGBA）：
//   · 双线性：VMULL.U8（u8 纹素 × u8 权重 → u16 乘积，无串道、无溢出）
//     权重和恒为 256 → 4 项和 ≤ 65280，一次 VSHR #8 与标量 SWAR 逐位等价
//   · 着色：通道 16 位道 VMUL/VMSLL；A 道用 vbsl 换成 baseA8，
//     使 (·×256 + 32768)>>16 恰好等于标量的 (A×baseA8 + 128)>>8
//   · 混合：16 位道整数运算；Div255 用 16 位精确形式
//     ((x+128) + ((x+128)>>8))>>8（已穷举验证 0..65025 与 ((x+128)*257)>>16 全等）
//   · 纹理 gather / 坐标走查 / 摘要预判仍是标量（访存延迟主导，向量化无益）
//
// 字节序（关键）：纹理是 RGBA 内存序（stb 解码），而帧缓冲是 XRGB8888
// （内存字节 B,G,R,A；标量管线把 R 写在 bit16-23 同理）。故纹素向量的通道道
// 顺序为 [R,G,B,A]，而目标/输出字节序为 [B,G,R,A] —— 着色得到的 S 需按
// 每像素 R/B 换位后再进混合，否则会出现整体 R/B 互换（首版即踩此坑，
// 板上逐像素比对 + 中间量转储定位）。
// ---------------------------------------------------------------------------

#if L2D_NEON_KERNEL

namespace {

/** 每像素 8 位双线性权重（与标量同式）+ 两个退化情形的等价替换。
 *  权重和恒为 256，但 u8 域只能表示 0..255，故 256 权重（等价于"最近邻"）
 *  改用 (255,…,1) 并把权重 1 乘的 texel 换成权重 256 的那个 texel：
 *    · w00 == 256：仅当 tx8 == 0 && ty8 == 0（w10 = w01 = w11 = 0）
 *      → (255,0,0,1) 且 T11 ← T00：255*T + 1*T = 256*T → >>8 == T ✓
 *    · w11 == 256：仅当 tx8 == 255 && ty8 == 255（w00 = w10 = w01 = 0）
 *      → (1,0,0,255) 且 T00 ← T11：1*T + 255*T = 256*T → >>8 == T ✓
 *  两种情形各占 ~1/65536 像素（首版只处理了前者，Hiyori 每帧因此偶发 1~5 个
 *  像素差异——板上逐帧比对发现）。
 *  注意：权重以独立标量返回（而非数组），否则 GCC 会把它们放栈上逐字节拼向量
 *  （实测生成 192 条 strb，性能反降）。 */
inline void Weight8(csmInt32 fxF, csmInt32 fyF, csmUint32* w00, csmUint32* w10,
                    csmUint32* w01, csmUint32* w11, csmUint32* t00, csmUint32* t11)
{
    const csmInt32 tx8 = (fxF >> 8) & 0xff;
    const csmInt32 ty8 = (fyF >> 8) & 0xff;
    const csmInt32 itx = 256 - tx8;
    const csmInt32 ity = 256 - ty8;
    const csmInt32 w0 = (itx * ity) >> 8;
    if (w0 > 255)
    {
        *w00 = 255; *w10 = 0; *w01 = 0; *w11 = 1;
        *t11 = *t00;
        return;
    }
    const csmInt32 w1 = (tx8 * ity) >> 8;
    const csmInt32 w2 = (itx * ty8) >> 8;
    const csmInt32 w3 = 256 - w0 - w1 - w2;
    if (w3 > 255)
    {
        *w00 = 1; *w10 = 0; *w01 = 0; *w11 = 255;
        *t00 = *t11;
        return;
    }
    *w00 = (csmUint32)w0;
    *w10 = (csmUint32)w1;
    *w01 = (csmUint32)w2;
    *w11 = (csmUint32)w3;
}

/** 两个像素的同一个权重 → u8x8 向量 [w0×4 | w1×4]（每像素 4 通道同权重） */
inline uint8x8_t WeightPair(csmUint32 wa, csmUint32 wb)
{
    // ARM 侧字节复制（1 mul）+ 两寄存器拼 d 寄存器（1 vmov）
    const csmUint32 a = wa * 0x01010101u;
    const csmUint32 b = wb * 0x01010101u;
    return vreinterpret_u8_u64(vcreate_u64((csmUint64)a | ((csmUint64)b << 32)));
}

/** 2 个像素同一位置的 texel（RGBA 内存序）→ u8x8 向量 [RGBA | RGBA] */
inline uint8x8_t TexelPair(csmUint32 ta, csmUint32 tb)
{
    return vreinterpret_u8_u64(vcreate_u64((csmUint64)ta | ((csmUint64)tb << 32)));
}

/** 16 位道 Div255（x ≤ 65025）：与 ((x+128)*257)>>16 全等 */
inline uint16x8_t Div255Vec(uint16x8_t x)
{
    const uint16x8_t t = vaddq_u16(x, vdupq_n_u16(128));
    return vshrq_n_u16(vaddq_u16(t, vshrq_n_u16(t, 8)), 8);
}

/** 着色：B=[R,G,B,A]×2 → S=[sr,sg,sb,sa]×2
 *  A 道：M 道取 baseA8 后 (A*baseA8*256 + 32768)>>16 == (A*baseA8 + 128)>>8 ✓ */
inline uint16x8_t ShadeVec(uint16x8_t B, uint16x8_t alphaLane,
                           uint16x8_t baseAVec, uint16x4_t kb)
{
    const uint16x8_t abr = vcombine_u16(vdup_lane_u16(vget_low_u16(B), 3),
                                        vdup_lane_u16(vget_high_u16(B), 3));
    const uint16x8_t M = vbslq_u16(alphaLane, baseAVec, abr);
    const uint16x8_t P = vmulq_u16(B, M);   // [R*A, G*A, B*A, A*baseA8]（≤65280）
    return vcombine_u16(vrshrn_n_u32(vmull_u16(vget_low_u16(P), kb), 16),
                        vrshrn_n_u32(vmull_u16(vget_high_u16(P), kb), 16));
}

} // namespace

/**
 * NEON 扫描线：2 像素一组，尾随奇数像素交回标量内核（逐位同源）。
 * 摘要判定全透明的像素以"4 个 texel = 0"参与本组运算：
 * 着色结果为 0、混合退化为 dst 原值（Div255(d*255) ≡ d，已穷举验证），
 * 因此写回后与"跳过"完全等价（三种混合模式均成立）。
 */
template <int kBlend>
L2D_NOINLINE
void RasterScanlineIntNeonImpl(csmUint32* row, csmInt32 pxStart, csmInt32 pxEnd,
                               csmInt32 x0Base, csmInt32 fxFrac, csmInt32 dfx16,
                               csmInt32 y0Base, csmInt32 fyFrac, csmInt32 dfy16,
                               const IntShadeParams& sh,
                               csmUint64* sampledPx, csmUint64* blendedPx)
{
    const csmInt32 texW = sh.texW;
    const csmInt32 texH = sh.texH;
    const csmUint8* pixels = sh.pixels;
    const csmUint8* summary = sh.summary;
    const csmInt32 summaryW = sh.summaryW;
    const csmInt32 summaryH = sh.summaryH;

    // 循环外常量（每扫描线一次）
    static const csmUint16 kAlphaLaneData[8] = {0, 0, 0, 0xffff, 0, 0, 0, 0xffff};
    const uint16x8_t alphaLane = vld1q_u16(kAlphaLaneData);
    // 每像素 [R,G,B,A] → [B,G,R,A]（16 位道里即字节 0↔4、1↔5、6/7 保持）
    static const csmUint8 kRBSwapIdxData[8] = {4, 5, 2, 3, 0, 1, 6, 7};
    const uint8x8_t rbSwapIdx = vld1_u8(kRBSwapIdxData);
    const uint16x8_t baseAVec = vdupq_n_u16((csmUint16)sh.baseA8);
    const csmInt32 baseA8Sel = sh.baseA8;
    const uint16x4_t kb = vcreate_u16(
        (csmUint64)(csmUint16)sh.kbR | ((csmUint64)(csmUint16)sh.kbG << 16)
        | ((csmUint64)(csmUint16)sh.kbB << 32) | ((csmUint64)256 << 48));
    const uint16x8_t c128 = vdupq_n_u16(128);

    csmInt32 fxF = fxFrac;
    csmInt32 fyF = fyFrac;
    csmUint32* dst = row + pxStart;
    csmInt32 remaining = pxEnd - pxStart;

    for (; remaining >= 2; remaining -= 2, dst += 2,
                          fxF += 2 * dfx16, fyF += 2 * dfy16)
    {
        const csmInt32 fxF1 = fxF + dfx16;
        const csmInt32 fyF1 = fyF + dfy16;
        const csmInt32 x0a = x0Base + (fxF >> 16);
        const csmInt32 y0a = y0Base + (fyF >> 16);
        const csmInt32 x0b = x0Base + (fxF1 >> 16);
        const csmInt32 y0b = y0Base + (fyF1 >> 16);

        // --- 透明摘要预判（同标量：3×3 块全零判定 + 短路，命中首字节即刻判定） ---
        csmBool activeA = true;
        csmBool activeB = true;
        if (summary != NULL)
        {
            const csmInt32 bxA = x0a >> 2, byA = y0a >> 2;
            if (bxA >= 1 && byA >= 1 && bxA < summaryW - 1 && byA < summaryH - 1)
            {
                const csmUint8* s = summary + (size_t)(byA - 1) * summaryW + (bxA - 1);
                activeA = ((s[0] | s[1] | s[2])
                           || (s[summaryW] | s[summaryW + 1] | s[summaryW + 2])
                           || (s[2 * summaryW] | s[2 * summaryW + 1] | s[2 * summaryW + 2]))
                          != 0;
            }
            const csmInt32 bxB = x0b >> 2, byB = y0b >> 2;
            if (bxB >= 1 && byB >= 1 && bxB < summaryW - 1 && byB < summaryH - 1)
            {
                const csmUint8* s = summary + (size_t)(byB - 1) * summaryW + (bxB - 1);
                activeB = ((s[0] | s[1] | s[2])
                           || (s[summaryW] | s[summaryW + 1] | s[summaryW + 2])
                           || (s[2 * summaryW] | s[2 * summaryW + 1] | s[2 * summaryW + 2]))
                          != 0;
            }
        }
        if (!activeA && !activeB)
        {
            continue; // 整组全透明：不采样、不读写目标
        }

        // --- 采样（活跃像素；非活跃 → texel 全 0，写回恒等） ---
        csmUint32 t00a = 0, t10a = 0, t01a = 0, t11a = 0;
        csmUint32 t00b = 0, t10b = 0, t01b = 0, t11b = 0;
        {
            const csmInt32 x1 = x0a + 1;
            const csmInt32 y1 = y0a + 1;
            const csmInt32 cx0 = x0a < 0 ? 0 : (x0a >= texW ? texW - 1 : x0a);
            const csmInt32 cx1 = x1 < 0 ? 0 : (x1 >= texW ? texW - 1 : x1);
            const csmInt32 cy0 = y0a < 0 ? 0 : (y0a >= texH ? texH - 1 : y0a);
            const csmInt32 cy1 = y1 < 0 ? 0 : (y1 >= texH ? texH - 1 : y1);
            if (activeA)
            {
                const csmUint32* r0 = (const csmUint32*)(pixels + (size_t)cy0 * texW * 4);
                const csmUint32* r1 = (const csmUint32*)(pixels + (size_t)cy1 * texW * 4);
                t00a = r0[cx0]; t10a = r0[cx1]; t01a = r1[cx0]; t11a = r1[cx1];
            }
            if (activeB)
            {
                const csmInt32 x1b = x0b + 1;
                const csmInt32 y1b = y0b + 1;
                const csmInt32 cx0b = x0b < 0 ? 0 : (x0b >= texW ? texW - 1 : x0b);
                const csmInt32 cx1b = x1b < 0 ? 0 : (x1b >= texW ? texW - 1 : x1b);
                const csmInt32 cy0b = y0b < 0 ? 0 : (y0b >= texH ? texH - 1 : y0b);
                const csmInt32 cy1b = y1b < 0 ? 0 : (y1b >= texH ? texH - 1 : y1b);
                const csmUint32* r0 = (const csmUint32*)(pixels + (size_t)cy0b * texW * 4);
                const csmUint32* r1 = (const csmUint32*)(pixels + (size_t)cy1b * texW * 4);
                t00b = r0[cx0b]; t10b = r0[cx1b]; t01b = r1[cx0b]; t11b = r1[cx1b];
            }
        }

        // --- 权重（8 位域，与标量同式）；非活跃像素 texel 为 0，权重任意（置 0 省算） ---
        csmUint32 w00a = 0, w10a = 0, w01a = 0, w11a = 0;
        csmUint32 w00b = 0, w10b = 0, w01b = 0, w11b = 0;
        if (activeA)
        {
            Weight8(fxF, fyF, &w00a, &w10a, &w01a, &w11a, &t00a, &t11a);
        }
        if (activeB)
        {
            Weight8(fxF1, fyF1, &w00b, &w10b, &w01b, &w11b, &t00b, &t11b);
        }

        // --- NEON：双线性 + 着色 ---
        const uint8x8_t T00 = TexelPair(t00a, t00b);
        const uint8x8_t T10 = TexelPair(t10a, t10b);
        const uint8x8_t T01 = TexelPair(t01a, t01b);
        const uint8x8_t T11 = TexelPair(t11a, t11b);
        const uint8x8_t W00 = WeightPair(w00a, w00b);
        const uint8x8_t W10 = WeightPair(w10a, w10b);
        const uint8x8_t W01 = WeightPair(w01a, w01b);
        const uint8x8_t W11 = WeightPair(w11a, w11b);
        const uint16x8_t sum = vaddq_u16(vaddq_u16(vmull_u8(T00, W00), vmull_u8(T10, W10)),
                                         vaddq_u16(vmull_u8(T01, W01), vmull_u8(T11, W11)));
        const uint16x8_t B = vshrq_n_u16(sum, 8);   // 双线性结果（≤255）
        const uint16x8_t S = ShadeVec(B, alphaLane, baseAVec, kb);
        // S 的道序是纹素序 [sr,sg,sb,sa]；混合/写出按帧缓冲字节序 [B,G,R,A]
        const uint16x8_t Sfb = vcombine_u16(
            vreinterpret_u16_u8(vtbl1_u8(vreinterpret_u8_u16(vget_low_u16(S)), rbSwapIdx)),
            vreinterpret_u16_u8(vtbl1_u8(vreinterpret_u8_u16(vget_high_u16(S)), rbSwapIdx)));

        // --- NEON：混合（dst 读改写，2 像素一次 LD/ST 8 字节） ---
        csmUint8* dstBytes = (csmUint8*)dst;
        const uint16x8_t d = vmovl_u8(vld1_u8(dstBytes));
        // inv = 255 - sa（逐像素广播；sa 在 A 道，R/B 换位不影响它）
        const uint16x8_t inv = vsubq_u16(vdupq_n_u16(255),
            vcombine_u16(vdup_lane_u16(vget_low_u16(Sfb), 3),
                         vdup_lane_u16(vget_high_u16(Sfb), 3)));
        uint16x8_t out;
        if (kBlend == SoftBlend_Add)
        {
            // SRC=ONE, DST=ONE（alpha 通道不变）
            out = vaddq_u16(Sfb, d);
            out = vbslq_u16(alphaLane, d, out);
        }
        else if (kBlend == SoftBlend_Multiply)
        {
            // SRC=DST_COLOR, DST=ONE_MINUS_SRC_ALPHA（alpha 通道不变）
            const uint16x8_t t1 = vmulq_u16(Sfb, d);
            const uint16x8_t t2 = vmulq_u16(d, inv);
            out = vaddq_u16(Div255Vec(t1), Div255Vec(t2));
            out = vbslq_u16(alphaLane, d, out);
        }
        else
        {
            // SRC=ONE, DST=ONE_MINUS_SRC_ALPHA
            const uint16x8_t u = vaddq_u16(vmulq_u16(d, inv), c128);
            out = vaddq_u16(Sfb, vshrq_n_u16(vaddq_u16(u, vshrq_n_u16(u, 8)), 8));
        }
        // VQMOVN 饱和收窄 == ClampByteI（0..255 截断），省 vmin 与 255 常量寄存器
        vst1_u8(dstBytes, vqmovn_u16(out));

        if (sampledPx != NULL)
        {
            *sampledPx += (csmUint64)((activeA ? 1u : 0u) + (activeB ? 1u : 0u));
        }
        if (blendedPx != NULL)
        {
            *blendedPx += (csmUint64)((activeA ? 1u : 0u) + (activeB ? 1u : 0u));
        }
    }

    // --- 尾随单个像素：交回标量内核（同一实现，逐位一致） ---
    if (remaining == 1)
    {
        const csmInt32 x0 = x0Base + (fxF >> 16);
        const csmInt32 y0 = y0Base + (fyF >> 16);
        if (RasterPixelScalar(dst, x0, y0, fxF, fyF, sh, kBlend, 255) == 2)
        {
            if (sampledPx != NULL) { ++(*sampledPx); }
            if (blendedPx != NULL) { ++(*blendedPx); }
        }
    }
}

L2D_NOINLINE
void RasterScanlineIntNeon(csmUint32* row, csmInt32 pxStart, csmInt32 pxEnd,
                           csmInt32 x0Base, csmInt32 fxFrac, csmInt32 dfx16,
                           csmInt32 y0Base, csmInt32 fyFrac, csmInt32 dfy16,
                           const IntShadeParams& sh, csmInt32 blendMode,
                           csmUint64* sampledPx, csmUint64* blendedPx)
{
    switch (blendMode)
    {
    case SoftBlend_Add:
        RasterScanlineIntNeonImpl<SoftBlend_Add>(row, pxStart, pxEnd, x0Base, fxFrac,
                                                 dfx16, y0Base, fyFrac, dfy16, sh,
                                                 sampledPx, blendedPx);
        break;
    case SoftBlend_Multiply:
        RasterScanlineIntNeonImpl<SoftBlend_Multiply>(row, pxStart, pxEnd, x0Base,
                                                      fxFrac, dfx16, y0Base, fyFrac,
                                                      dfy16, sh, sampledPx, blendedPx);
        break;
    default:
        RasterScanlineIntNeonImpl<SoftBlend_Normal>(row, pxStart, pxEnd, x0Base, fxFrac,
                                                    dfx16, y0Base, fyFrac, dfy16, sh,
                                                    sampledPx, blendedPx);
        break;
    }
}

#endif /* L2D_NEON_KERNEL */

/**
 * 整数扫描线：坐标以 (整型基数 + 16.16 小数累加器) 走查，全程无浮点/转换。
 * x0Base/y0Base + frac 表示 fx = u*texW - 0.5 的 floor 分解。
 * 内核选择：NEON（L2D_ENABLE_NEON=1 且 ARM NEON 可用）否则标量 SWAR。
 * maskRow != NULL 时逐像素乘掩码（NEON 内核未做掩码通道 → 回退标量，
 * 逐位语义仍由同一标量内核保证）。
 */
void RasterScanlineInt(csmUint32* row, csmInt32 pxStart, csmInt32 pxEnd,
                       csmInt32 x0Base, csmInt32 fxFrac, csmInt32 dfx16,
                       csmInt32 y0Base, csmInt32 fyFrac, csmInt32 dfy16,
                       const IntShadeParams& sh, csmInt32 blendMode,
                       csmUint64* sampledPx, csmUint64* blendedPx,
                       const csmUint8* maskRow, csmInt32 maskXor)
{
#if L2D_NEON_KERNEL
    if (maskRow == NULL)
    {
        RasterScanlineIntNeon(row, pxStart, pxEnd, x0Base, fxFrac, dfx16,
                              y0Base, fyFrac, dfy16, sh, blendMode,
                              sampledPx, blendedPx);
        return;
    }
#else
    (void)maskXor;
#endif
    csmInt32 fxF = fxFrac;
    csmInt32 fyF = fyFrac;
    csmUint32* dst = row + pxStart;
    const csmUint8* m = maskRow; // 与 dst 同步推进（NULL 表示无掩码）

    for (csmInt32 i = pxEnd - pxStart; i > 0; --i, ++dst, ++m, fxF += dfx16, fyF += dfy16)
    {
        const csmInt32 x0 = x0Base + (fxF >> 16);
        const csmInt32 y0 = y0Base + (fyF >> 16);
        // maskXor：0=普通掩码；255=倒转掩码（factor 取 255−m，异或实现免分支）
        const csmInt32 mb = (maskRow != NULL) ? ((csmInt32)(*m) ^ maskXor) : 255;
        const csmInt32 r = RasterPixelScalar(dst, x0, y0, fxF, fyF, sh, blendMode, mb);
        if (r == 2)
        {
            if (sampledPx != NULL) { ++(*sampledPx); }
            if (blendedPx != NULL) { ++(*blendedPx); }
        }
    }
}

// 扫描线光栅化：每扫描线求左右边界 → 行内步进插值 UV（无 bbox 空转）
// strip[Y0,Y1) 限定本线程负责的扫描行范围（条带并行）
void RasterTriangleScanline(const ScreenVertex& a, const ScreenVertex& b, const ScreenVertex& c,
                            const SoftTexture* texture,
                            const csmFloat32 baseRGBA[4],
                            const csmFloat32 mulRGB[3], const csmFloat32 scrRGB[3],
                            int blendMode,
                            csmUint32* fb, int fbW, int fbH,
                            int stripY0, int stripY1,
                            csmUint32* rasterCount, csmUint64* scanPixels,
                            CubismRenderer_Software::SoftProf* prof,
                            const IntShadeParams* intShade,
                            const csmUint8* maskBuf, csmInt32 maskXor)
{
    const csmFloat32 area = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
    if (area > -1e-9f && area < 1e-9f)
    {
        return; // 退化
    }

    // 顶点按 y 排序（p0 顶、p2 底）
    const ScreenVertex* p0 = &a;
    const ScreenVertex* p1 = &b;
    const ScreenVertex* p2 = &c;
    if (p0->y > p1->y) { const ScreenVertex* t = p0; p0 = p1; p1 = t; }
    if (p1->y > p2->y) { const ScreenVertex* t = p1; p1 = p2; p2 = t; }
    if (p0->y > p1->y) { const ScreenVertex* t = p0; p0 = p1; p1 = t; }

    int yStart = (int)ceilf(p0->y - 0.5f);
    int yEnd = (int)ceilf(p2->y - 0.5f);
    if (yStart < stripY0) yStart = stripY0;
    if (yEnd > stripY1) yEnd = stripY1;
    if (yEnd > fbH) yEnd = fbH;
    if (yStart < 0) yStart = 0;
    if (yStart >= yEnd)
    {
        return;
    }

    ++(*rasterCount);

    ScanInterp longEdge;   // p0 → p2
    ScanInterp shortTop;   // p0 → p1
    ScanInterp shortBot;   // p1 → p2
    longEdge.Init(*p0, *p2);
    shortTop.Init(*p0, *p1);
    shortBot.Init(*p1, *p2);

    const csmFloat32 inv255 = 1.0f / 255.0f;

    for (int py = yStart; py < yEnd; ++py)
    {
        const csmFloat32 cy = (csmFloat32)py + 0.5f;

        csmFloat32 xA, uA, vA;
        longEdge.At(cy, &xA, &uA, &vA);

        csmFloat32 xS, uS, vS;
        if (cy < p1->y)
        {
            shortTop.At(cy, &xS, &uS, &vS);
        }
        else
        {
            shortBot.At(cy, &xS, &uS, &vS);
        }

        csmFloat32 xL, uL, vL, xR, uR, vR;
        if (xA <= xS)
        {
            xL = xA; uL = uA; vL = vA;
            xR = xS; uR = uS; vR = vS;
        }
        else
        {
            xL = xS; uL = uS; vL = vS;
            xR = xA; uR = uA; vR = vA;
        }

        int pxStart = (int)ceilf(xL - 0.5f);
        int pxEnd = (int)ceilf(xR - 0.5f);
        if (pxStart < 0) pxStart = 0;
        if (pxEnd > fbW) pxEnd = fbW;
        if (pxStart >= pxEnd)
        {
            continue;
        }

        const csmFloat32 invSpan = (xR > xL) ? (1.0f / (xR - xL)) : 0.0f;
        const csmFloat32 dudx = (uR - uL) * invSpan;
        const csmFloat32 dvdx = (vR - vL) * invSpan;
        const csmFloat32 xFirst = (csmFloat32)pxStart + 0.5f;
        csmFloat32 u = uL + (xFirst - xL) * dudx;
        csmFloat32 v = vL + (xFirst - xL) * dvdx;

        (*scanPixels) += (csmUint64)(pxEnd - pxStart);
        if (kNoPixelDebug)
        {
            continue; // 诊断用：只跑几何/扫描线骨架，跳过像素处理（分解 setup 与逐像素成本）
        }

        csmUint32* row = fb + (size_t)py * fbW;

        // ---- 整数定点路径（默认；classic 着色且无调试开关时）----
        if (intShade != NULL)
        {
            if (kNoPixelDebug)
            {
                continue; // 诊断：只跑几何骨架
            }
            // 每扫描线只做 6 次浮点→整数分解：坐标 = 整型基数 + 16.16 小数累加器
            const csmFloat32 fxS = u * (csmFloat32)intShade->texW - 0.5f;
            csmInt32 x0Base = (csmInt32)fxS;
            if (fxS < 0.0f)
            {
                x0Base -= 1;
            }
            const csmFloat32 fxFracF = (fxS - (csmFloat32)x0Base) * 65536.0f;
            const csmInt32 fxFrac = (csmInt32)(fxFracF + 0.5f);
            const csmFloat32 dfxScaled = dudx * (csmFloat32)intShade->texW * 65536.0f;
            const csmInt32 dfx16 = (csmInt32)(dfxScaled + (dfxScaled >= 0.0f ? 0.5f : -0.5f));

            const csmFloat32 fyS = (1.0f - v) * (csmFloat32)intShade->texH - 0.5f;
            csmInt32 y0Base = (csmInt32)fyS;
            if (fyS < 0.0f)
            {
                y0Base -= 1;
            }
            const csmFloat32 fyFracF = (fyS - (csmFloat32)y0Base) * 65536.0f;
            const csmInt32 fyFrac = (csmInt32)(fyFracF + 0.5f);
            const csmFloat32 dfyScaled = -dvdx * (csmFloat32)intShade->texH * 65536.0f;
            const csmInt32 dfy16 = (csmInt32)(dfyScaled + (dfyScaled >= 0.0f ? 0.5f : -0.5f));

            RasterScanlineInt(row, pxStart, pxEnd,
                              x0Base, fxFrac, dfx16, y0Base, fyFrac, dfy16,
                              *intShade, blendMode,
                              (kProf && prof != NULL) ? &prof->sampledPx : NULL,
                              (kProf && prof != NULL) ? &prof->blendedPx : NULL,
                              (maskBuf != NULL) ? maskBuf + (size_t)py * fbW + pxStart : NULL,
                              maskXor);
            continue;
        }
        if (kNoPixelDebug)
        {
            continue; // 诊断用：只跑几何/扫描线骨架（浮点路径下的分解）
        }

        for (int px = pxStart; px < pxEnd; ++px, u += dudx, v += dvdx)
        {
            // 摘要预判：确定全透明则跳过（省 4 次主纹理访问）
            if (texture->IsTransparentRegion(u, v))
            {
                continue;
            }

            csmUint8 texel[4];
            if (kNearestDebug)
            {
                texture->SampleNearest(u, v, texel);
            }
            else
            {
                texture->SampleBilinear(u, v, texel);
            }

            // 全透明像素对任何混合模式都无贡献（sr/sg/sb 均含 sa 因子）
            if (texel[3] == 0)
            {
                continue;
            }
            if (kProf && prof != NULL)
            {
                ++prof->sampledPx;
            }

            csmUint32* dstPixel = row + px;
            const csmInt32 mb = (maskBuf != NULL)
                ? ((csmInt32)maskBuf[(size_t)py * fbW + px] ^ maskXor) : 255;
            ShadeAndBlend(texel, baseRGBA, mulRGB, scrRGB, blendMode, dstPixel, mb);
            if (kProf && prof != NULL)
            {
                ++prof->blendedPx;
            }
        }
    }
}

/**
 * 剪裁掩码光栅化：单字节/像素（行距 = fbW；0 = 完全裁剪，255 = 完全显示）。
 * 扫描线约定与 RasterTriangleScanline 完全一致（像素中心采样、同 y 起止规则），
 * 保证掩码边缘与使用它的 drawable 边缘像素对齐。
 *
 * 合成语义（官方 SetupMask shader + GLES2 掩码混合 dst *= 1−srcAlpha 的反解）：
 *   m' = a + m×(255−a)/255    （a = 掩码 drawable 的纹理 alpha；不含 opacity/基色）
 * 多掩码 drawable 在同一组内依次累积 = 「覆盖并集」；主绘制乘 1−∏(1−a)。
 * 不使用透明摘要预判（掩码网格小，且需要 alpha 的连续梯度做软边）。
 */
void RasterMaskTriangle(const ScreenVertex& a, const ScreenVertex& b, const ScreenVertex& c,
                        const SoftTexture* texture,
                        csmUint8* mask, int fbW, int fbH)
{
    const csmFloat32 area = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
    if (area > -1e-9f && area < 1e-9f)
    {
        return; // 退化
    }

    // 顶点按 y 排序（p0 顶、p2 底）
    const ScreenVertex* p0 = &a;
    const ScreenVertex* p1 = &b;
    const ScreenVertex* p2 = &c;
    if (p0->y > p1->y) { const ScreenVertex* t = p0; p0 = p1; p1 = t; }
    if (p1->y > p2->y) { const ScreenVertex* t = p1; p1 = p2; p2 = t; }
    if (p0->y > p1->y) { const ScreenVertex* t = p0; p0 = p1; p1 = t; }

    int yStart = (int)ceilf(p0->y - 0.5f);
    int yEnd = (int)ceilf(p2->y - 0.5f);
    if (yStart < 0) yStart = 0;
    if (yEnd > fbH) yEnd = fbH;
    if (yStart >= yEnd)
    {
        return;
    }

    ScanInterp longEdge;   // p0 → p2
    ScanInterp shortTop;   // p0 → p1
    ScanInterp shortBot;   // p1 → p2
    longEdge.Init(*p0, *p2);
    shortTop.Init(*p0, *p1);
    shortBot.Init(*p1, *p2);

    for (int py = yStart; py < yEnd; ++py)
    {
        const csmFloat32 cy = (csmFloat32)py + 0.5f;

        csmFloat32 xA, uA, vA;
        longEdge.At(cy, &xA, &uA, &vA);

        csmFloat32 xS, uS, vS;
        if (cy < p1->y)
        {
            shortTop.At(cy, &xS, &uS, &vS);
        }
        else
        {
            shortBot.At(cy, &xS, &uS, &vS);
        }

        csmFloat32 xL, uL, vL, xR, uR, vR;
        if (xA <= xS)
        {
            xL = xA; uL = uA; vL = vA;
            xR = xS; uR = uS; vR = vS;
        }
        else
        {
            xL = xS; uL = uS; vL = vS;
            xR = xA; uR = uA; vR = vA;
        }

        int pxStart = (int)ceilf(xL - 0.5f);
        int pxEnd = (int)ceilf(xR - 0.5f);
        if (pxStart < 0) pxStart = 0;
        if (pxEnd > fbW) pxEnd = fbW;
        if (pxStart >= pxEnd)
        {
            continue;
        }

        const csmFloat32 invSpan = (xR > xL) ? (1.0f / (xR - xL)) : 0.0f;
        const csmFloat32 dudx = (uR - uL) * invSpan;
        const csmFloat32 dvdx = (vR - vL) * invSpan;
        const csmFloat32 xFirst = (csmFloat32)pxStart + 0.5f;
        csmFloat32 u = uL + (xFirst - xL) * dudx;
        csmFloat32 v = vL + (xFirst - xL) * dvdx;

        csmUint8* row = mask + (size_t)py * fbW + pxStart;
        for (int px = pxStart; px < pxEnd; ++px, ++row, u += dudx, v += dvdx)
        {
            // 双线性采样：官方掩码 RT/纹理均 GL_LINEAR → 与官方一致的软边。
            // 成本实测（板上 sage，掩码覆盖 ~1.9 万像素）：双线性 1.5ms/帧 vs 最近邻 0.8ms/帧；
            // 30fps 预算（33ms）下 17.6~18.3ms 均富余 → 取保真（如需提速可换 SampleNearest）
            csmUint8 texel[4];
            texture->SampleBilinear(u, v, texel);
            const csmInt32 av = texel[3];
            if (av == 0)
            {
                continue;
            }
            const csmInt32 cur = *row;
            *row = (csmUint8)(av + Div255(cur * (255 - av)));
        }
    }
}

} // namespace

// ---------------------------------------------------------------------------

CubismRenderer_Software::CubismRenderer_Software(csmUint32 width, csmUint32 height)
    : CubismRenderer(width, height)
    , _framebuffer(NULL)
    , _framebufferSize(0)
    , _framebufferWidth(0)
    , _framebufferHeight(0)
    , _textures(NULL)
    , _textureCount(0)
    , _sortedDrawables(NULL)
    , _staticOrder(false)
    , _orderSorted(false)
    , _lastVisibleCount(0)
    , _lastRasterTriangles(0)
    , _lastScanPixels(0)
    , _maskWarnCount(0)
    , _rasterThreads(0)
    , _scratch(NULL)
    , _pool(NULL)
    , _poolThreads(0)
    , _nextBand(0)
    , _masks(NULL)
    , _maskBufferStride(0)
    , _maskGroupCount(0)
    , _nextMaskGroup(0)
{
}

CubismRenderer_Software::~CubismRenderer_Software()
{
    ReleasePool();
    DestroyScratch();
    DestroyMaskScratch();
    delete[] _framebuffer;
    delete[] _textures;
    delete[] _sortedDrawables;
}

CubismRenderer_Software* CubismRenderer_Software::Create(csmUint32 width, csmUint32 height)
{
    return CSM_NEW CubismRenderer_Software(width, height);
}

void CubismRenderer_Software::BindTexture(csmInt32 textureIndex, const SoftTexture* texture)
{
    if (textureIndex < 0)
    {
        return;
    }
    if (textureIndex >= _textureCount)
    {
        const csmInt32 newCount = textureIndex + 1;
        const SoftTexture** newTable = new const SoftTexture*[newCount];
        for (csmInt32 i = 0; i < newCount; ++i)
        {
            newTable[i] = (i < _textureCount) ? _textures[i] : NULL;
        }
        delete[] _textures;
        _textures = newTable;
        _textureCount = newCount;
    }
    _textures[textureIndex] = texture;
}

const csmUint32* CubismRenderer_Software::GetFramebuffer() const { return _framebuffer; }
csmUint32 CubismRenderer_Software::GetFramebufferWidth() const { return _framebufferWidth; }
csmUint32 CubismRenderer_Software::GetFramebufferHeight() const { return _framebufferHeight; }
csmUint32 CubismRenderer_Software::GetLastVisibleDrawableCount() const { return _lastVisibleCount; }
csmUint32 CubismRenderer_Software::GetLastRasterTriangleCount() const { return _lastRasterTriangles; }
csmUint64 CubismRenderer_Software::GetLastScanPixelCount() const { return _lastScanPixels; }
csmUint32 CubismRenderer_Software::GetLastMaskWarnCount() const { return _maskWarnCount; }
void CubismRenderer_Software::ResetMaskWarn() { _maskWarnCount = 0; }

void CubismRenderer_Software::EnsureFramebuffer()
{
    const csmUint32 width = _modelRenderTargetWidth > 0 ? _modelRenderTargetWidth : 1;
    const csmUint32 height = _modelRenderTargetHeight > 0 ? _modelRenderTargetHeight : 1;
    const csmUint32 size = width * height;

    if (_framebuffer == NULL || _framebufferWidth != width || _framebufferHeight != height)
    {
        delete[] _framebuffer;
        _framebuffer = new csmUint32[size];
        _framebufferSize = size;
        _framebufferWidth = width;
        _framebufferHeight = height;
    }
    memset(_framebuffer, 0, sizeof(csmUint32) * size);
}

void CubismRenderer_Software::BuildSortedDrawableOrder()
{
    // 静态序：首帧（模型初始姿态）排序一次后冻结。
    // 动因：5.x 模型可用参数驱动绘制顺序——shuflle 在看向右上角/呼吸抬头相位时，
    // Core 会把胸口皮肤层（#174/#175/#176）的 drawOrder/renderOrder 重排到裙子之上
    //（官方语义如此，VTS 同样会弹）；本工程按产品要求保持稳定层序（默认静态）。
    if (_staticOrder && _orderSorted)
    {
        return;
    }
    const csmInt32 drawableCount = GetModel()->GetDrawableCount();
    if (_sortedDrawables == NULL)
    {
        _sortedDrawables = new csmInt32[drawableCount];
    }
    // 按 renderOrder **值排序**（稳定：相等时保持原下标序）。
    //
    // 历史坑①：曾用"order 值当槽下标"填表 → r.5 含 offscreen 的模型其 renderOrder 值域
    //   为 [0, drawableCount + offscreenCount)（offscreen 也占 order），越界槽未初始化
    //   → 被当作 drawable 索引 → **越界 segfault**（Ren，§17）。当时的补丁是"越界者填 -1 跳过"。
    // 历史坑②：上述补丁把"跳过"当成解法，于是 **order ≥ drawableCount 的 drawable 被整块丢弃**
    //   ——Ren 的 renderOrder 最大值 221 > drawableCount 198，丢掉最多 24 个 drawable
    //   （袖子靠这些高光/阴影层雕出轮廓，丢了就变成"方块袖"，实测用户肉眼发现）。
    //   正确做法是排序而非索引：一个 drawable 都不丢（与 Mocari 的 draw_order_indices 同语义）。
    const csmInt32* renderOrder = GetModel()->GetRenderOrders();
    // drawOrder 走 Core 直调（Framework 未包装）：offscreen 感知的绘制顺序主键
    const csmInt32* drawOrder = Core::csmGetDrawableDrawOrders(GetModel()->GetModel());
    for (csmInt32 i = 0; i < drawableCount; ++i)
    {
        _sortedDrawables[i] = i;
    }
    std::stable_sort(_sortedDrawables, _sortedDrawables + drawableCount,
                     [drawOrder, renderOrder](csmInt32 a, csmInt32 b)
                     {
                         if (drawOrder[a] != drawOrder[b])
                         {
                             return drawOrder[a] < drawOrder[b];
                         }
                         return renderOrder[a] < renderOrder[b];
                     });
    // 为什么主键是 drawOrder：r.5 含 offscreen 的模型里，renderOrder 的槽位把 offscreen 与
    // drawable 混排（Ren：值域 0..221 而 drawable 仅 198），单独按它排序会把 offscreen
    // 成员 drawable 提前十几到二十层（被后画的图层盖住）；drawOrder 才是 offscreen 感知的
    // 绘制顺序（成员排在所属 offscreen 的位置），与 Mocari/官方 5.x 一致。
    // 被提前约 20 层后被覆盖，呈现"扁平方块 + 硬边"（用户肉眼发现，RS 栈正确）。

    // 诊断：L2D_DBG_ORDER=0 逐帧打印胸部件（174..177）；=N 只在第 N 帧全量转储所有绘制序
    static const char* kOrderDebugEnv = getenv("L2D_DBG_ORDER");
    if (kOrderDebugEnv != NULL)
    {
        static int dbgFrame = 0;
        const int only = atoi(kOrderDebugEnv);
        if (only == 0 && drawableCount > 177)
        {
            printf("[order] f%d drawOrder d#174=%d d#175=%d d#176=%d d#177=%d | renderOrder d#175=%d\n",
                   dbgFrame, drawOrder[174], drawOrder[175], drawOrder[176], drawOrder[177],
                   renderOrder[175]);
        }
        else if (only == dbgFrame)
        {
            for (csmInt32 i = 0; i < drawableCount; ++i)
            {
                printf("[order-all] %d %d %d\n", i, drawOrder[i], renderOrder[i]);
            }
        }
        ++dbgFrame;
    }

    _orderSorted = true;   // 静态序：本帧结果即冻结结果（动态序下该位仅作记录）
}

void CubismRenderer_Software::SetRasterThreads(int threads)
{
    _rasterThreads = threads;
}

void CubismRenderer_Software::SetStaticDrawOrder(csmBool enable)
{
    _staticOrder = enable;
}

// ---------------------------------------------------------------- 肢体刚性覆盖
// 把一组 drawable 的顶点绕肩点旋转（模型坐标、y 向上）——"纸片人"式抬手/摸头：
// 不依赖作者的参数绑定；BODY 通道 arm.r/arm.l 由播放器换算成这里的旋转。
// 默认全关（恒等变换）→ 与逐位基线一致。

void CubismRenderer_Software::SetDrawableLimbOnTop(csmInt32 drawableIndex, csmBool onTop)
{
    if (drawableIndex < 0)
    {
        return;
    }
    if ((size_t)drawableIndex >= _limbRot.size())
    {
        _limbRot.resize((size_t)drawableIndex + 1);
    }
    _limbRot[(size_t)drawableIndex].onTop = onTop;
}

void CubismRenderer_Software::SetDrawableLimbRotate(csmInt32 drawableIndex, csmFloat32 px,
                                                    csmFloat32 py, csmFloat32 deg)
{
    if (drawableIndex < 0)
    {
        return;
    }
    if ((size_t)drawableIndex >= _limbRot.size())
    {
        _limbRot.resize((size_t)drawableIndex + 1);
    }
    LimbRot& r = _limbRot[(size_t)drawableIndex];
    const csmFloat32 rad = deg * 3.14159265358979f / 180.0f;

    r.px = px;
    r.py = py;
    r.cosA = cosf(rad);
    r.sinA = sinf(rad);
}

void CubismRenderer_Software::ClearDrawableLimbRotates()
{
    _limbRot.clear();
}

void CubismRenderer_Software::SetSkinBones(const csmFloat32* affine6, csmInt32 boneCount)
{
    if (affine6 == NULL || boneCount <= 0)
    {
        _skinBones.clear();
        return;
    }
    _skinBones.assign(affine6, affine6 + (size_t)boneCount * 6);
}

void CubismRenderer_Software::SetDrawableSkin(csmInt32 drawableIndex, const SkinW* weights,
                                              csmInt32 vertexCount)
{
    if (drawableIndex < 0)
    {
        return;
    }
    if ((size_t)drawableIndex >= _skins.size())
    {
        _skins.resize((size_t)drawableIndex + 1);
    }
    if (weights == NULL || vertexCount <= 0)
    {
        _skins[(size_t)drawableIndex].clear();
        return;
    }
    _skins[(size_t)drawableIndex].assign(weights, weights + vertexCount);
}

void CubismRenderer_Software::ClearSkins()
{
    _skins.clear();
    _skinBones.clear();
}



// ---------------------------------------------------------------------------
// 每帧共享渲染数据（PrepareFrame 串行准备一次；各渲染线程只读）
//   · drawable 状态（纹理/混合/基色/mul·scr/顶点区间）每帧只解析一次
//   · 顶点 MVP 变换每帧只做一次（原实现每条带重复做，3 线程 × 若干子条带 = 多倍）
//   · 三角形做背面剔除 + y 升序 + 行带分桶，条带渲染只遍历与本带相交的三角形
// ---------------------------------------------------------------------------

struct CubismRenderer_Software::FrameScratch
{
    struct DrawState
    {
        const SoftTexture* texture;
        csmInt32 blendMode;
        csmBool useMultiplyScreen;
        csmFloat32 baseRGBA[4];
        csmFloat32 mulRGB[3];
        csmFloat32 scrRGB[3];
        csmInt32 vertOffset;      // 屏幕顶点在 verts 中的起始
        csmInt32 vertCount;
        csmInt32 yMin, yMax;      // 屏幕行范围（整 drawable 剔除；已含 1 行余量）
        csmBool intShadeOk;       // 整数管线可用（无 multiply/screen 的新式混合）
        IntShadeParams intShade;  // 整数管线常数（纹理指针/尺寸/预乘系数）
        csmInt32 maskGroup;       // 剪裁掩码组（-1 = 不使用；≥0 时主绘制采样该组掩码图）
        csmInt32 maskXor;         // 0=普通掩码（乘 m）；255=倒转掩码（乘 255−m）
    };

    struct Tri
    {
        csmUint16 i0, i1, i2;     // 已按 y 升序（顶→底，与光栅化内部约定一致）
        csmUint16 drawable;       // DrawState 下标
        csmInt16 yS, yE;          // 未裁剪的扫描行范围 [yS, yE)
    };

    DrawState* draws;
    csmInt32 drawsCap;
    csmInt32 drawCount;

    ScreenVertex* verts;
    csmInt32 vertsCap;

    Tri* tris;
    csmInt32 trisCap;
    csmInt32 triCount;

    csmInt32* bandTris;           // 各行带的三角形条目下标（按绘制顺序）
    csmInt32 bandTrisCap;
    csmInt32* bandOffset;         // 行带前缀和（bandCount+1）
    csmInt32* bandCursor;         // 填充游标（复用）
    csmInt32 bandCount;
    csmInt32 bandHeight;

    FrameScratch()
        : draws(NULL), drawsCap(0), drawCount(0)
        , verts(NULL), vertsCap(0)
        , tris(NULL), trisCap(0), triCount(0)
        , bandTris(NULL), bandTrisCap(0)
        , bandOffset(NULL), bandCursor(NULL)
        , bandCount(0), bandHeight(32)
    {
    }
};

// ---------------------------------------------------------------------------
// 剪裁掩码（clipping mask）——官方语义（SetupMask shader + GLES2 SetupClippingContext）：
//   · 每掩码组一份全屏单字节图；0 = 完全裁剪，255 = 完全显示
//     （官方缓冲存「外部=1」并做 dst *= 1−srcAlpha；这里存 m = 1−dst 的反解，
//      累积式 m ← a + m×(255−a)/255，主绘制直接乘 m，省掉逐像素减法）
//   · a = 掩码 drawable 的纹理 alpha（官方 SetupMask shader 不乘 opacity/基色）
//   · 组 = 掩码 drawable 索引列表；相同列表去重（官方 FindSameClip：数量同 + 集合同）
//   · 本帧只有「有可见使用者实际绘制」的组才渲染（对应官方 _isUsing）
//   · 倒转掩码（GetDrawableInvertedMask）：factor 取 255−m（官方 MaskInverted shader）
// ---------------------------------------------------------------------------

struct CubismRenderer_Software::MaskScratch
{
    std::vector<csmInt32> groupOfDrawable;  // [drawableCount] 掩码组 id，-1 = 不使用
    std::vector<csmInt32> groupFirst;       // [groupCount+1] 组 → groupDrawables 区间
    std::vector<csmInt32> groupDrawables;   // 平铺的掩码 drawable 索引
    std::vector<csmUint8> needed;           // [groupCount] 本帧使用标记
    std::vector<csmInt32> lastRow0;         // [groupCount] 上帧写入行范围（清零最小化用）
    std::vector<csmInt32> lastRow1;
    std::vector<csmUint8*> buffers;         // [groupCount] 按需分配（NULL=未分配；W×H 字节/组）
    csmUint32 stride;                       // 单组字节数（= W×H）
    const CubismModel* model;               // 组表对应模型（变化则重建）
    csmInt32 groupCount;
    csmInt32 drawableCount;

    MaskScratch()
        : stride(0), model(NULL), groupCount(0), drawableCount(0)
    {
    }

    void FreeBuffers()
    {
        for (size_t i = 0; i < buffers.size(); ++i)
        {
            delete[] buffers[i];
        }
        buffers.clear();
    }
};

// ---------------------------------------------------------------------------
// 持久光栅化线程池：工作线程常驻，每帧每阶段两次屏障同步（替代每帧 create/join）
// 行带用原子计数器动态分配 → 负载自动均衡（原固定交错子条带在内容偏斜时
// 会造成主线程空等十几毫秒）
// 阶段：taskKind=0 → 剪裁掩码（按组并行）；taskKind=1 → 行带绘制
// ---------------------------------------------------------------------------

struct CubismRenderer_Software::ThreadPool
{
    CubismRenderer_Software* self;
    L2dThread tids[8];
    int workerCount;              // 工作线程数（= 总线程数 - 1）
    L2dBarrier startBar;
    L2dBarrier doneBar;
    volatile bool ready;          // 池初始化完成（工作线程先自旋等待，避免屏障计数竞态）
    volatile bool exit;
    volatile int taskKind;        // 0 = 掩码组；1 = 行带
    SoftProf prof[8];
};

namespace {
struct PoolWorker
{
    CubismRenderer_Software::ThreadPool* pool;
    int index;
};

L2D_THREAD_ENTRY PoolWorkerThunk(void* arg)
{
    PoolWorker* w = static_cast<PoolWorker*>(arg);
    CubismRenderer_Software::ThreadPool* pool = w->pool;

    while (!pool->ready)
    {
        L2dYield();
    }
    if (pool->exit)
    {
        return NULL;
    }

    for (;;)
    {
        L2dBarrierWait(&pool->startBar);
        if (pool->exit)
        {
            L2dBarrierWait(&pool->doneBar);
            break;
        }
        pool->prof[w->index] = CubismRenderer_Software::SoftProf();
        if (pool->taskKind == 0)
        {
            pool->self->DrainMaskGroups(&pool->prof[w->index]);
        }
        else
        {
            pool->self->DrainBands(&pool->prof[w->index]);
        }
        L2dBarrierWait(&pool->doneBar);
    }
    return NULL;
}

} // namespace

void CubismRenderer_Software::DestroyScratch()
{
    if (_scratch == NULL)
    {
        return;
    }
    delete[] _scratch->draws;
    delete[] _scratch->verts;
    delete[] _scratch->tris;
    delete[] _scratch->bandTris;
    delete[] _scratch->bandOffset;
    delete[] _scratch->bandCursor;
    delete _scratch;
    _scratch = NULL;
}

void CubismRenderer_Software::EnsureScratch(csmInt32 drawableCount, csmInt32 indexCount,
                                            csmInt32 vertexCount)
{
    if (_scratch == NULL)
    {
        _scratch = new FrameScratch();
    }
    FrameScratch* sc = _scratch;

    if (sc->drawsCap < drawableCount)
    {
        delete[] sc->draws;
        sc->draws = new FrameScratch::DrawState[drawableCount];
        sc->drawsCap = drawableCount;
    }
    if (sc->vertsCap < vertexCount)
    {
        delete[] sc->verts;
        sc->verts = new ScreenVertex[vertexCount];
        sc->vertsCap = vertexCount;
    }
    const csmInt32 triCap = (indexCount / 3) + 1;
    if (sc->trisCap < triCap)
    {
        delete[] sc->tris;
        sc->tris = new FrameScratch::Tri[triCap];
        sc->trisCap = triCap;
    }

    // 行带高度：默认 32 行（内容集中时越细越均衡，但每带要遍历自己的三角形列表）
    static const int kBandHeightEnv = (getenv("L2D_BAND") != NULL) ? atoi(getenv("L2D_BAND")) : 32;
    csmInt32 bandHeight = kBandHeightEnv > 0 ? kBandHeightEnv : 32;
    if (bandHeight < 4) bandHeight = 4;
    if (bandHeight > 200) bandHeight = 200;
    sc->bandHeight = bandHeight;

    const csmInt32 fbH = (csmInt32)_framebufferHeight;
    const csmInt32 bandCount = (fbH + bandHeight - 1) / bandHeight;
    if (sc->bandCount != bandCount)
    {
        delete[] sc->bandOffset;
        delete[] sc->bandCursor;
        sc->bandOffset = new csmInt32[bandCount + 1];
        sc->bandCursor = new csmInt32[bandCount + 1];
        sc->bandCount = bandCount;
    }
    if (sc->bandTrisCap == 0)
    {
        sc->bandTrisCap = 4096;
        sc->bandTris = new csmInt32[sc->bandTrisCap];
    }
}

// ---------------------------------------------------------------------------
// 剪裁掩码：组表构建 / 每帧渲染（条带并行前）
// ---------------------------------------------------------------------------

void CubismRenderer_Software::DestroyMaskScratch()
{
    // 定义在 MaskScratch 完整类型之后（析构里只知道前向声明）
    if (_masks != NULL)
    {
        _masks->FreeBuffers();
        delete _masks;
        _masks = NULL;
    }
    _maskGroupCount = 0;
    _maskBufferStride = 0;
}

void CubismRenderer_Software::EnsureMaskScratch(CubismModel* model)
{
    if (_masks == NULL)
    {
        _masks = new MaskScratch();
    }
    MaskScratch* ms = _masks;
    const csmInt32 drawableCount = model->GetDrawableCount();
    const csmUint32 wh = _framebufferWidth * _framebufferHeight;

    if (ms->model != model || ms->drawableCount != drawableCount)
    {
        ms->FreeBuffers(); // 模型变化：旧缓冲作废（组语义已变）
        ms->model = model;
        ms->drawableCount = drawableCount;
        ms->groupOfDrawable.assign((size_t)drawableCount, -1);
        ms->groupFirst.assign(1, 0);
        ms->groupDrawables.clear();

        const csmInt32* maskCounts = model->GetDrawableMaskCounts();
        const csmInt32** masks = model->GetDrawableMasks();
        for (csmInt32 d = 0; d < drawableCount; ++d)
        {
            const csmInt32 mc = maskCounts[d];
            if (mc <= 0 || masks[d] == NULL)
            {
                continue;
            }
            // 去重：与官方 FindSameClip 一致（数量相同 且 集合一致）
            csmInt32 found = -1;
            for (csmInt32 g = 0; g < ms->groupCount; ++g)
            {
                const csmInt32 first = ms->groupFirst[g];
                const csmInt32 cnt = ms->groupFirst[g + 1] - first;
                if (cnt != mc)
                {
                    continue;
                }
                csmInt32 same = 0;
                for (csmInt32 j = 0; j < mc; ++j)
                {
                    for (csmInt32 k = 0; k < cnt; ++k)
                    {
                        if (masks[d][j] == ms->groupDrawables[first + k])
                        {
                            ++same;
                            break;
                        }
                    }
                }
                if (same == mc)
                {
                    found = g;
                    break;
                }
            }
            if (found < 0)
            {
                found = ms->groupCount++;
                for (csmInt32 j = 0; j < mc; ++j)
                {
                    ms->groupDrawables.push_back(masks[d][j]);
                }
                ms->groupFirst.push_back((csmInt32)ms->groupDrawables.size());
            }
            ms->groupOfDrawable[d] = found;
        }
        ms->needed.assign((size_t)ms->groupCount, 0);
        ms->lastRow0.assign((size_t)ms->groupCount, 0);
        ms->lastRow1.assign((size_t)ms->groupCount, -1);  // -1 行 = 无历史写入

        if (ms->groupCount > 0)
        {
            CubismLogInfo("[SoftRenderer] 剪裁掩码：%d 个掩码组，%d 个 drawable 使用掩码（缓冲按需分配）",
                          ms->groupCount, (csmInt32)ms->groupDrawables.size());
        }
        if (kMaskLogDebug)
        {
            printf("[maskdbg] drawable=%d 组数=%d\n", drawableCount, ms->groupCount);
            for (csmInt32 g = 0; g < ms->groupCount; ++g)
            {
                printf("[maskdbg] 组%d 掩码drawable:", g);
                for (csmInt32 k = ms->groupFirst[g]; k < ms->groupFirst[g + 1]; ++k)
                {
                    const csmInt32 md = ms->groupDrawables[k];
                    printf(" #%d(vis=%d op=%.3f inv=%d tex=%d vtx=%d)", md,
                           (int)model->GetDrawableDynamicFlagIsVisible(md),
                           model->GetDrawableOpacity(md),
                           (int)model->GetDrawableInvertedMask(md),
                           model->GetDrawableTextureIndex(md),
                           model->GetDrawableVertexCount(md));
                }
                printf("\n[maskdbg] 组%d 使用者:", g);
                for (csmInt32 d = 0; d < drawableCount; ++d)
                {
                    if (ms->groupOfDrawable[d] == g)
                    {
                        printf(" #%d(vis=%d op=%.3f inv=%d tex=%d vtx=%d)", d,
                               (int)model->GetDrawableDynamicFlagIsVisible(d),
                               model->GetDrawableOpacity(d),
                               (int)model->GetDrawableInvertedMask(d),
                               model->GetDrawableTextureIndex(d),
                               model->GetDrawableVertexCount(d));
                    }
                }
                printf("\n");
            }
        }
    }

    // 缓冲：组数或帧尺寸变化时释放（实际分配在 RenderMasks 里按帧按需做，省内存——
    // 模型可有多达 16 个掩码组，全量分配在 98MB 的板子上不可接受；单组 = W×H 字节）
    if (_maskGroupCount != ms->groupCount || ms->stride != wh)
    {
        ms->FreeBuffers();
        _maskGroupCount = ms->groupCount;
        _maskBufferStride = wh;
        ms->stride = wh;
        ms->buffers.assign((size_t)ms->groupCount, (csmUint8*)NULL);
        ms->lastRow0.assign((size_t)ms->groupCount, 0);
        ms->lastRow1.assign((size_t)ms->groupCount, -1);
    }
    // 无需每帧整体清零：本帧使用的组必被 RenderMaskGroup 先行清零重画；
    // 未使用的组不会被任何绘制采样（使用标记在 PrepareFrame 里由绘制者置位）
}

void CubismRenderer_Software::RenderMaskGroup(csmInt32 group)
{
    CubismModel* model = GetModel();
    if (model == NULL || _masks == NULL || group < 0 || group >= (csmInt32)_masks->buffers.size())
    {
        return;
    }
    csmUint8* buf = _masks->buffers[group];
    if (buf == NULL)
    {
        return; // 未分配（RenderMasks 已保证使用中的组必已分配）
    }

    // 注意：GetMvpMatrix() 按值返回——必须持有副本，GetArray() 才对副本取址
    //（直接 GetMvpMatrix().GetArray() 是指向已销毁临时对象的悬垂指针；栈被后续调用
    //  覆盖后矩阵内容变垃圾，表现为顶点全部塌到屏幕中心的退化三角形）
    CubismMatrix44 mvpMatrix = GetMvpMatrix();
    const csmFloat32* m = mvpMatrix.GetArray();
    const int fbW = (int)_framebufferWidth;
    const int fbH = (int)_framebufferHeight;
    const csmInt32 first = _masks->groupFirst[group];
    const csmInt32 last = _masks->groupFirst[group + 1];
    const csmInt32 drawableCount = model->GetDrawableCount();

    // --- 清零：仅本组几何的行范围 ∪ 上帧行范围（整屏 384KB 清零是掩码阶段的主要开销）---
    // 不变式：帧末非零数据 ⊆ 本帧行范围 → 每帧清「本帧 ∪ 上帧」即可无残影
    int curRow0 = fbH;
    int curRow1 = 0;
    for (csmInt32 k = first; k < last; ++k)
    {
        const csmInt32 d = _masks->groupDrawables[k];
        if (d < 0 || d >= drawableCount)
        {
            continue;
        }
        const csmInt32 vertexCount = model->GetDrawableVertexCount(d);
        const Core::csmVector2* positions = model->GetDrawableVertexPositions(d);
        if (vertexCount <= 0 || positions == NULL)
        {
            continue;
        }
        for (csmInt32 v = 0; v < vertexCount; ++v)
        {
            csmFloat32 sx, sy;
            TransformVtx(m, positions[v].X, positions[v].Y,
                         (csmFloat32)fbW, (csmFloat32)fbH, &sx, &sy);
            const int yy = (int)sy;
            if (yy < curRow0) curRow0 = yy;
            if (yy > curRow1) curRow1 = yy;
        }
    }
    if (curRow0 > curRow1)
    {
        curRow0 = 0; // 无几何：本帧不写
        curRow1 = -1;
    }
    int clearRow0 = (curRow0 < _masks->lastRow0[group]) ? curRow0 : _masks->lastRow0[group];
    int clearRow1 = (curRow1 > _masks->lastRow1[group]) ? curRow1 : _masks->lastRow1[group];
    if (clearRow0 < 0) { clearRow0 = 0; }
    if (clearRow1 >= fbH) { clearRow1 = fbH - 1; }
    if (clearRow1 >= clearRow0)
    {
        memset(buf + (size_t)clearRow0 * fbW, 0, (size_t)(clearRow1 + 1 - clearRow0) * fbW);
    }
    _masks->lastRow0[group] = curRow0;
    _masks->lastRow1[group] = curRow1;

    for (csmInt32 k = first; k < last; ++k)
    {
        const csmInt32 d = _masks->groupDrawables[k];
        if (d < 0 || d >= drawableCount)
        {
            continue;
        }
        const csmInt32 textureIndex = model->GetDrawableTextureIndex(d);
        if (textureIndex < 0 || textureIndex >= _textureCount || _textures[textureIndex] == NULL)
        {
            continue;
        }
        const SoftTexture* texture = _textures[textureIndex];
        const csmInt32 vertexCount = model->GetDrawableVertexCount(d);
        const csmInt32 indexCount = model->GetDrawableVertexIndexCount(d);
        const Core::csmVector2* positions = model->GetDrawableVertexPositions(d);
        const Core::csmVector2* uvs = model->GetDrawableVertexUvs(d);
        const csmUint16* indices = model->GetDrawableVertexIndices(d);
        if (vertexCount <= 0 || indexCount < 3 || positions == NULL || uvs == NULL || indices == NULL)
        {
            continue;
        }
        const bool culling = (model->GetDrawableCulling(d) != 0);

        // 诊断：掩码 drawable 的屏幕/UV 包围盒（每帧打印，可看重睑/开合时的变化）
        if (kMaskLogDebug)
        {
            csmFloat32 x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
            csmFloat32 u0 = 1e30f, v0 = 1e30f, u1 = -1e30f, v1 = -1e30f;
            for (csmInt32 v = 0; v < vertexCount; ++v)
            {
                csmFloat32 sx, sy;
                TransformVtx(m, positions[v].X, positions[v].Y,
                             (csmFloat32)fbW, (csmFloat32)fbH, &sx, &sy);
                if (sx < x0) x0 = sx;
                if (sx > x1) x1 = sx;
                if (sy < y0) y0 = sy;
                if (sy > y1) y1 = sy;
                if (uvs[v].X < u0) u0 = uvs[v].X;
                if (uvs[v].X > u1) u1 = uvs[v].X;
                if (uvs[v].Y < v0) v0 = uvs[v].Y;
                if (uvs[v].Y > v1) v1 = uvs[v].Y;
            }
            printf("[maskdbg] 组%d 掩码drawable #%d 屏幕bbox x[%.0f..%.0f] y[%.0f..%.0f] uv[%.3f..%.3f]x[%.3f..%.3f] vis=%d op=%.3f tex=%dx%d\n",
                   group, d, x0, x1, y0, y1, u0, u1, v0, v1,
                   (int)model->GetDrawableDynamicFlagIsVisible(d), model->GetDrawableOpacity(d),
                   texture->Width(), texture->Height());
        }

        // 逐三角形现场变换 3 顶点（掩码网格小；免去每线程顶点暂存，组间并行安全）
        csmInt32 nTri = 0, nCulled = 0, nZeroArea = 0, nRaster = 0;
        csmFloat32 areaSum = 0.0f;
        for (csmInt32 i = 0; i + 2 < indexCount; i += 3)
        {
            ++nTri;
            const csmUint16 vi[3] = { indices[i], indices[i + 1], indices[i + 2] };
            if (vi[0] >= (csmUint16)vertexCount || vi[1] >= (csmUint16)vertexCount
                || vi[2] >= (csmUint16)vertexCount)
            {
                continue;
            }
            ScreenVertex q[3];
            for (int t = 0; t < 3; ++t)
            {
                TransformVtx(m, positions[vi[t]].X, positions[vi[t]].Y,
                             (csmFloat32)fbW, (csmFloat32)fbH, &q[t].x, &q[t].y);
                q[t].u = uvs[vi[t]].X;
                q[t].v = uvs[vi[t]].Y;
            }
            const csmFloat32 area = (q[1].x - q[0].x) * (q[2].y - q[0].y)
                                  - (q[1].y - q[0].y) * (q[2].x - q[0].x);
            areaSum += area;
            if (area > -1e-6f && area < 1e-6f)
            {
                ++nZeroArea;
            }
            if (culling)
            {
                // 与主路径同约定：剔除 CW（面积 < 0）
                if (area <= 0.0f)
                {
                    ++nCulled;
                    continue;
                }
            }
            ++nRaster;
            RasterMaskTriangle(q[0], q[1], q[2], texture, buf, fbW, fbH);
        }
        if (kMaskLogDebug)
        {
            // 三角形统计（诊断"退化三角形/全被剔除/UV 异常"类问题时的第一手数据）
            printf("[maskdbg] 组%d drawable #%d: 三角形=%d 剔除=%d 零面积=%d 光栅=%d 面积和=%.1f cullFlag=%d\n",
                   group, d, nTri, nCulled, nZeroArea, nRaster, areaSum, (int)culling);
        }
    }

    // 诊断：掩码图转储（每组每个进程一次）
    if (kMaskDumpDir != NULL)
    {
        static volatile csmInt32 dumpedFlags[64] = {0};
        if (group >= 0 && group < 64 && L2dAtomicAdd(&dumpedFlags[group], 1) == 0)
        {
            char path[512];
            snprintf(path, sizeof(path), "%s/mask_g%d.pgm", kMaskDumpDir, group);
            FILE* fp = fopen(path, "wb");
            if (fp != NULL)
            {
                fprintf(fp, "P5\n%d %d\n255\n", fbW, fbH);
                fwrite(buf, 1, (size_t)fbW * fbH, fp);
                fclose(fp);
                printf("[maskdbg] 转储 %s\n", path);
            }
        }
    }
}

void CubismRenderer_Software::DrainMaskGroups(SoftProf* prof)
{
    for (;;)
    {
        const csmInt32 g = L2dAtomicAdd(&_nextMaskGroup, 1);
        if (g >= _maskGroupCount)
        {
            break;
        }
        if (_masks == NULL || g >= (csmInt32)_masks->needed.size() || !_masks->needed[g])
        {
            continue; // 本帧无人使用
        }
        RenderMaskGroup(g);
    }
}

void CubismRenderer_Software::RenderMasks(CubismModel* model, csmInt32 threads)
{
    (void)model;
    if (_masks == NULL || _maskGroupCount <= 0 || _maskBufferStride == 0)
    {
        return;
    }
    // 按需分配（主线程、屏障之前；工作线程随后只读写已存在的缓冲）
    int neededCount = 0;
    for (csmInt32 g = 0; g < _maskGroupCount; ++g)
    {
        if (!_masks->needed[g])
        {
            continue;
        }
        ++neededCount;
        if (_masks->buffers[g] == NULL)
        {
            _masks->buffers[g] = new csmUint8[_maskBufferStride](); // 零初始化 = 全裁状态
            _masks->lastRow0[g] = 0;
            _masks->lastRow1[g] = -1;
        }
    }
    if (neededCount == 0)
    {
        return;
    }

    _nextMaskGroup = 0;
    if (threads <= 1 || _pool == NULL || neededCount == 1)
    {
        DrainMaskGroups(NULL); // 单线程或仅一组：主线程直接跑（省两次屏障）
        return;
    }
    ThreadPool* pool = _pool;
    pool->taskKind = 0;
    L2dBarrierWait(&pool->startBar);
    DrainMaskGroups(NULL);
    L2dBarrierWait(&pool->doneBar);
}

void CubismRenderer_Software::PrepareFrame(CubismModel* model, SoftProf* prof)
{
    const double tPrepareStart = kProf ? ProfNow() : 0.0;
    double tVertAcc = 0.0;

    CubismMatrix44 mvpMatrix = GetMvpMatrix(); // 按值返回：持副本防悬垂（见 RenderMaskGroup 注释）
    const csmFloat32* m = mvpMatrix.GetArray();
    const int fbW = (int)_framebufferWidth;
    const int fbH = (int)_framebufferHeight;
    const csmInt32 drawableCount = model->GetDrawableCount();
    const CubismModelMultiplyAndScreenColor& ov = model->GetOverrideMultiplyAndScreenColor();

    if (_masks != NULL && !_masks->needed.empty())
    {
        memset(_masks->needed.data(), 0, _masks->needed.size()); // 本帧使用标记复位
    }

    FrameScratch* sc = _scratch;
    sc->drawCount = 0;
    sc->triCount = 0;
    const csmInt32 bandCount = sc->bandCount;
    const csmInt32 bandHeight = sc->bandHeight;
    memset(sc->bandOffset, 0, sizeof(csmInt32) * (bandCount + 1));

    csmInt32 vertUsed = 0;
    csmInt32 triUsed = 0;

    /* 两遍：第 1 遍普通层序，第 2 遍"抬起的肢体"（onTop）——抬臂时手臂要画在头发/身体之上，
     * 否则举起来的部分被长发盖住（实测 165° 时整条手臂藏进头发后面）。onTop 恒空时
     * 两遍等价于原先的一遍（第 2 遍无匹配项）。 */
    for (int limbPass = 0; limbPass < 2; ++limbPass)
    for (csmInt32 k = 0; k < drawableCount; ++k)
    {
        const csmInt32 d = _sortedDrawables[k];
        if (!IsOnlyDrawable(d) || IsDrawableSkipped(d))
        {
            continue; // 诊断：只画指定 drawable
        }
        if (d < 0)
        {
            continue; // 空洞槽（order 被 offscreen 占用；见 BuildSortedDrawableOrder）
        }
        if (LimbIsOnTop(d) != (limbPass == 1))
        {
            continue; // 本遍不画它（另一遍画）
        }

        if (!model->GetDrawableDynamicFlagIsVisible(d))
        {
            continue;
        }
        const csmInt32 textureIndex = model->GetDrawableTextureIndex(d);
        if (textureIndex < 0 || textureIndex >= _textureCount || _textures[textureIndex] == NULL)
        {
            continue;
        }
        const SoftTexture* texture = _textures[textureIndex];

        // 混合模式：经典三式（官方 5.2- shader，不含 multiply/screen）；
        // 新式（5.3+ shader）才应用 multiply/screen 颜色——老模型上这两个值
        // 可能未定义（全 0），若误用会画出全黑。
        const csmBlendMode blendModeType = model->GetDrawableBlendModeType(d);
        csmInt32 blendMode = SoftBlend_Normal;
        csmBool useMultiplyScreen = false;
        switch (blendModeType.GetColorBlendType())
        {
        case Core::csmColorBlendType_Normal:             blendMode = SoftBlend_Normal; break;
        case Core::csmColorBlendType_AddCompatible:      blendMode = SoftBlend_Add; break;
        case Core::csmColorBlendType_MultiplyCompatible: blendMode = SoftBlend_Multiply; break;
        default:
            if (_maskWarnCount == 0)
            {
                CubismLogWarning("[SoftRenderer] 新式混合模式(color=%d alpha=%d)未实现，按 Normal 近似",
                                 blendModeType.GetColorBlendType(), blendModeType.GetAlphaBlendType());
                ++_maskWarnCount;
            }
            useMultiplyScreen = true;
            break;
        }

        // 剪裁掩码：查组表（EnsureMaskScratch 按模型构建），标记本帧使用
        csmInt32 maskGroup = -1;
        if (!kNoMaskDebug && _masks != NULL && model->GetDrawableMaskCounts()[d] > 0)
        {
            if (d < (csmInt32)_masks->groupOfDrawable.size())
            {
                maskGroup = _masks->groupOfDrawable[d];
            }
            if (maskGroup >= 0 && maskGroup < (csmInt32)_masks->needed.size())
            {
                _masks->needed[maskGroup] = 1;
            }
        }

        const csmInt32 vertexCount = model->GetDrawableVertexCount(d);
        const Core::csmVector2* positions = model->GetDrawableVertexPositions(d);
        const Core::csmVector2* uvs = model->GetDrawableVertexUvs(d);
        const csmUint16* indices = model->GetDrawableVertexIndices(d);
        const csmInt32 indexCount = model->GetDrawableVertexIndexCount(d);
        if (vertexCount <= 0 || positions == NULL || uvs == NULL
            || indices == NULL || indexCount < 3)
        {
            continue;
        }
        if (vertUsed + vertexCount > sc->vertsCap || triUsed + indexCount / 3 > sc->trisCap)
        {
            continue; // 容量兜底（EnsureScratch 已按模型规模分配，正常不会触发）
        }

        // 基色 / multiply / screen（每帧一次；原实现每条带重算）
        const CubismTextureColor base = GetModelColorWithOpacity(model->GetDrawableOpacity(d));
        const CubismTextureColor mul = ov.GetDrawableMultiplyColor(d);
        const CubismTextureColor scr = ov.GetDrawableScreenColor(d);

        FrameScratch::DrawState& st = sc->draws[sc->drawCount];
        st.texture = texture;
        st.blendMode = blendMode;
        st.useMultiplyScreen = useMultiplyScreen;
        st.baseRGBA[0] = base.R; st.baseRGBA[1] = base.G;
        st.baseRGBA[2] = base.B; st.baseRGBA[3] = base.A;
        st.mulRGB[0] = useMultiplyScreen ? mul.R : 1.0f;
        st.mulRGB[1] = useMultiplyScreen ? mul.G : 1.0f;
        st.mulRGB[2] = useMultiplyScreen ? mul.B : 1.0f;
        st.scrRGB[0] = useMultiplyScreen ? scr.R : 0.0f;
        st.scrRGB[1] = useMultiplyScreen ? scr.G : 0.0f;
        st.scrRGB[2] = useMultiplyScreen ? scr.B : 0.0f;
        st.vertOffset = vertUsed;
        st.vertCount = vertexCount;
        st.maskGroup = maskGroup;
        st.maskXor = (maskGroup >= 0 && model->GetDrawableInvertedMask(d) != 0) ? 255 : 0;
        // 整数管线常数：classic（无 multiply/screen）即可用；
        // kb = (base.C×256 × base.A×256) >> 8，把着色化简为 2 乘 1 移
        st.intShadeOk = !useMultiplyScreen;
        {
            const csmInt32 baseA8 = (csmInt32)(base.A * 256.0f + 0.5f);
            const csmInt32 baseR8 = (csmInt32)(base.R * 256.0f + 0.5f);
            const csmInt32 baseG8 = (csmInt32)(base.G * 256.0f + 0.5f);
            const csmInt32 baseB8 = (csmInt32)(base.B * 256.0f + 0.5f);
            st.intShade.pixels = texture->Pixels();
            st.intShade.summary = texture->AlphaSummary();
            st.intShade.texW = texture->Width();
            st.intShade.texH = texture->Height();
            st.intShade.summaryW = texture->SummaryWidth();
            st.intShade.summaryH = texture->SummaryHeight();
            st.intShade.baseA8 = baseA8;
            st.intShade.kbR = (baseR8 * baseA8) >> 8;
            st.intShade.kbG = (baseG8 * baseA8) >> 8;
            st.intShade.kbB = (baseB8 * baseA8) >> 8;
        }
        const csmInt32 drawableSlot = sc->drawCount;
        ++sc->drawCount;

        // --- 顶点变换（每帧一次，全线程共享） ---
        const double tVertStart = kProf ? ProfNow() : 0.0;
        ScreenVertex* verts = sc->verts + vertUsed;
        csmFloat32 yMin = 1e30f, yMax = -1e30f;
        for (csmInt32 i = 0; i < vertexCount; ++i)
        {
            csmFloat32 lx, ly;

            LimbXform(d, i, positions[i].X, positions[i].Y, &lx, &ly);
            TransformVtx(m, lx, ly,
                         (csmFloat32)fbW, (csmFloat32)fbH, &verts[i].x, &verts[i].y);
            verts[i].u = uvs[i].X;
            verts[i].v = uvs[i].Y;
            if (verts[i].y < yMin) yMin = verts[i].y;
            if (verts[i].y > yMax) yMax = verts[i].y;
        }
        if (kBboxDebug)
        {
            csmFloat32 xMin = 1e30f, xMax = -1e30f;
            for (csmInt32 i = 0; i < vertexCount; ++i)
            {
                if (verts[i].x < xMin) xMin = verts[i].x;
                if (verts[i].x > xMax) xMax = verts[i].x;
            }
            printf("[bbox] d#%d tex=%d vtx=%d 屏 x[%.0f..%.0f] y[%.0f..%.0f] op=%.2f blend=%d masks=%d\n",
                   d, textureIndex, vertexCount, xMin, xMax, yMin, yMax,
                   model->GetDrawableOpacity(d), blendMode, model->GetDrawableMaskCounts()[d]);
        }
        vertUsed += vertexCount;
        st.yMin = (csmInt32)floorf(yMin);
        st.yMax = (csmInt32)ceilf(yMax) + 1;
        if (kProf)
        {
            tVertAcc += ProfNow() - tVertStart;
        }

        // --- 三角形：背面剔除 + y 升序 + 行带分桶 ---
        const bool culling = (model->GetDrawableCulling(d) != 0) && !kNoCullDebug;
        for (csmInt32 i = 0; i + 2 < indexCount; i += 3)
        {
            csmUint16 i0 = indices[i];
            csmUint16 i1 = indices[i + 1];
            csmUint16 i2 = indices[i + 2];
            if (i0 >= (csmUint16)vertexCount || i1 >= (csmUint16)vertexCount
                || i2 >= (csmUint16)vertexCount)
            {
                continue;
            }
            const ScreenVertex& q0 = verts[i0];
            const ScreenVertex& q1 = verts[i1];
            const ScreenVertex& q2 = verts[i2];
            if (culling)
            {
                // 背面剔除。注意**屏幕坐标 y 向下**（GL 窗口坐标 y 向上）——镜像使绕序反号：
                // GL 的正面（CCW，y 向上）在我们的屏幕坐标里是**负**有向面积，
                // 故剔除「面积 > 0」= 背面。（曾按 GL 文档写成 `area <= 0` 剔除：
                // 实际剔的是正面 → 开了 culling 的模型整块消失，实测 Wanko 全空白、
                // 官方栈只光栅化 26/447 个三角形。）
                if ((q1.x - q0.x) * (q2.y - q0.y) - (q1.y - q0.y) * (q2.x - q0.x) > 0.0f)
                {
                    continue;
                }
            }
            // y 升序（顶→底）——与 RasterTriangleScanline 内部约定一致
            if (verts[i0].y > verts[i1].y) { const csmUint16 t = i0; i0 = i1; i1 = t; }
            if (verts[i1].y > verts[i2].y) { const csmUint16 t = i1; i1 = i2; i2 = t; }
            if (verts[i0].y > verts[i1].y) { const csmUint16 t = i0; i0 = i1; i1 = t; }

            const ScreenVertex& a = verts[i0];
            const ScreenVertex& b = verts[i1];
            const ScreenVertex& c = verts[i2];
            const csmFloat32 area = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
            if (area > -1e-9f && area < 1e-9f)
            {
                continue; // 退化三角形
            }
            csmInt32 yS = (csmInt32)ceilf(a.y - 0.5f);
            csmInt32 yE = (csmInt32)ceilf(c.y - 0.5f);
            if (yS < 0) yS = 0;
            if (yE > fbH) yE = fbH;
            if (yS >= yE)
            {
                continue;
            }

            FrameScratch::Tri& tri = sc->tris[triUsed];
            // 索引转成"共享顶点缓冲"的全局下标（RenderBand 按全局下标取顶点）
            tri.i0 = (csmUint16)(st.vertOffset + i0);
            tri.i1 = (csmUint16)(st.vertOffset + i1);
            tri.i2 = (csmUint16)(st.vertOffset + i2);
            tri.drawable = (csmUint16)drawableSlot;
            tri.yS = (csmInt16)yS;
            tri.yE = (csmInt16)yE;
            ++triUsed;

            // 行带计数（用 bandOffset[b+1] 暂存，随后前缀和变为起始偏移）
            const csmInt32 b0 = yS / bandHeight;
            const csmInt32 b1 = (yE - 1) / bandHeight;
            for (csmInt32 b = b0; b <= b1 && b < bandCount; ++b)
            {
                ++sc->bandOffset[b + 1];
            }
        }
    }

    sc->triCount = triUsed;

    // --- 行带三角形列表（计数排序：前缀和 → 填充） ---
    for (csmInt32 b = 0; b < bandCount; ++b)
    {
        sc->bandOffset[b + 1] += sc->bandOffset[b];
    }
    const csmInt32 total = sc->bandOffset[bandCount];
    if (total > sc->bandTrisCap)
    {
        delete[] sc->bandTris;
        sc->bandTrisCap = total + 1024;
        sc->bandTris = new csmInt32[sc->bandTrisCap];
    }
    memcpy(sc->bandCursor, sc->bandOffset, sizeof(csmInt32) * bandCount);
    for (csmInt32 t = 0; t < triUsed; ++t)
    {
        const FrameScratch::Tri& tri = sc->tris[t];
        const csmInt32 b0 = tri.yS / bandHeight;
        const csmInt32 b1 = (tri.yE - 1) / bandHeight;
        for (csmInt32 b = b0; b <= b1 && b < bandCount; ++b)
        {
            sc->bandTris[sc->bandCursor[b]++] = t;
        }
    }

    if (kProf)
    {
        prof->tVert += tVertAcc;
        prof->tOther += (ProfNow() - tPrepareStart) - tVertAcc;
    }
}

void CubismRenderer_Software::RenderBand(int bandIndex, int y0, int y1, SoftProf* prof)
{
    FrameScratch* sc = _scratch;
    const int fbW = (int)_framebufferWidth;
    const int fbH = (int)_framebufferHeight;
    const FrameScratch::Tri* tris = sc->tris;
    const ScreenVertex* verts = sc->verts;

    csmUint32 triangles = 0;
    csmUint64 scanPixels = 0;

    const csmInt32 begin = sc->bandOffset[bandIndex];
    const csmInt32 end = sc->bandOffset[bandIndex + 1];
    const csmInt32* list = sc->bandTris;

    csmInt32 curSlot = -1;
    const FrameScratch::DrawState* st = NULL;
    for (csmInt32 k = begin; k < end; ++k)
    {
        const FrameScratch::Tri& tri = tris[list[k]];
        if (tri.drawable != curSlot)
        {
            curSlot = tri.drawable;
            st = &sc->draws[curSlot];
        }
        // 本带范围内的扫描行（分桶已保证相交；仍需夹到带内）
        csmInt32 yS = tri.yS > y0 ? tri.yS : y0;
        csmInt32 yE = tri.yE < y1 ? tri.yE : y1;
        if (yS >= yE)
        {
            continue;
        }
        const IntShadeParams* intShade =
            (st->intShadeOk && !kFloatDebug && !kNearestDebug && !kRawDebug) ? &st->intShade : NULL;
        const csmUint8* maskBuf = NULL;
        if (st->maskGroup >= 0 && _masks != NULL
            && st->maskGroup < (csmInt32)_masks->buffers.size())
        {
            maskBuf = _masks->buffers[st->maskGroup];
        }
        RasterTriangleScanline(verts[tri.i0], verts[tri.i1], verts[tri.i2],
                               st->texture, st->baseRGBA, st->mulRGB, st->scrRGB,
                               st->blendMode, _framebuffer, fbW, fbH, yS, yE,
                               &triangles, &scanPixels, prof, intShade, maskBuf, st->maskXor);
    }

    prof->triCount += triangles;
    prof->spanPx += scanPixels;
}

void CubismRenderer_Software::DrainBands(SoftProf* prof)
{
    FrameScratch* sc = _scratch;
    const int fbH = (int)_framebufferHeight;
    if (kProf)
    {
        const double t0 = ProfNow();
        for (;;)
        {
            const csmInt32 b = L2dAtomicAdd(&_nextBand, 1);
            if (b >= sc->bandCount)
            {
                break;
            }
            const int y0 = b * sc->bandHeight;
            int y1 = y0 + sc->bandHeight;
            if (y1 > fbH)
            {
                y1 = fbH;
            }
            RenderBand(b, y0, y1, prof);
        }
        prof->tRaster += ProfNow() - t0;
        return;
    }
    for (;;)
    {
        const csmInt32 b = L2dAtomicAdd(&_nextBand, 1);
        if (b >= sc->bandCount)
        {
            break;
        }
        const int y0 = b * sc->bandHeight;
        int y1 = y0 + sc->bandHeight;
        if (y1 > fbH)
        {
            y1 = fbH;
        }
        RenderBand(b, y0, y1, prof);
    }
}

void CubismRenderer_Software::EnsurePool(csmInt32 threads)
{
    if (_pool != NULL && _poolThreads == threads)
    {
        return;
    }
    ReleasePool();

    ThreadPool* pool = new ThreadPool();
    pool->self = this;
    pool->ready = false;
    pool->exit = false;
    pool->workerCount = 0;
    pool->taskKind = 1;

    // 先建线程（各自自旋等 ready），再按实际创建数初始化屏障计数——
    // 避免"屏障计数与参与者不一致"导致的死锁
    static PoolWorker args[8]; // 常驻：线程生命周期内地址稳定
    for (int i = 1; i < threads; ++i)
    {
        args[i].pool = pool;
        args[i].index = i;
        if (L2dThreadCreate(&pool->tids[i], PoolWorkerThunk, &args[i]) != 0)
        {
            break;
        }
        ++pool->workerCount;
    }

    const int total = pool->workerCount + 1;
    L2dBarrierInit(&pool->startBar, total);
    L2dBarrierInit(&pool->doneBar, total);
    pool->ready = true;

    _pool = pool;
    _poolThreads = total;
}

void CubismRenderer_Software::ReleasePool()
{
    if (_pool == NULL)
    {
        return;
    }
    ThreadPool* pool = _pool;
    if (pool->workerCount > 0)
    {
        pool->exit = true;
        L2dBarrierWait(&pool->startBar);
        L2dBarrierWait(&pool->doneBar);
        for (int i = 1; i <= pool->workerCount; ++i)
        {
            L2dThreadJoin(pool->tids[i]);
        }
    }
    L2dBarrierDestroy(&pool->startBar);
    L2dBarrierDestroy(&pool->doneBar);
    delete pool;
    _pool = NULL;
    _poolThreads = 0;
}

void CubismRenderer_Software::DoDrawModel()
{
    CubismModel* model = GetModel();
    if (model == NULL)
    {
        return;
    }

    if (kProf)
    {
        ++g_prof.frames;
    }

    SoftProf frameProf; // 本帧局部累加（剖析关闭时全部为空操作）
    const double tClearStart = kProf ? ProfNow() : 0.0;

    EnsureFramebuffer();
    BuildSortedDrawableOrder();

    const csmInt32 drawableCount = model->GetDrawableCount();
    {
        csmInt32 totalVerts = 0;
        csmInt32 totalIndex = 0;
        for (csmInt32 d = 0; d < drawableCount; ++d)
        {
            totalVerts += model->GetDrawableVertexCount(d);
            totalIndex += model->GetDrawableVertexIndexCount(d);
        }
        EnsureScratch(drawableCount, totalIndex, totalVerts);
    }
    EnsureMaskScratch(model);

    if (kProf)
    {
        frameProf.tMemset = ProfNow() - tClearStart;
    }

    _lastVisibleCount = 0;
    _lastRasterTriangles = 0;
    _lastScanPixels = 0;

    if (model->GetOffscreenCount() > 0 && _maskWarnCount == 0)
    {
        CubismLogWarning("[SoftRenderer] 模型含 offscreen(%d)——当前未实现，已跳过",
                         model->GetOffscreenCount());
        ++_maskWarnCount;
    }

    // --- 阶段 1（串行）：drawable 状态 + 顶点变换 + 三角形分桶 ---
    PrepareFrame(model, &frameProf);
    _lastVisibleCount = (csmUint32)_scratch->drawCount;

    // 线程数决策（掩码阶段与条带阶段共用）
    int threads = _rasterThreads;
    if (threads <= 0)
    {
        threads = L2dCpuCount();
        if (threads > L2D_MAX_THREADS)
        {
            threads = L2D_MAX_THREADS;
        }
    }
    if (threads > _scratch->bandCount)
    {
        threads = _scratch->bandCount;
    }
    if (threads < 1)
    {
        threads = 1;
    }
    if (threads > 1)
    {
        EnsurePool(threads);
    }

    // --- 阶段 1.5：剪裁掩码（按组并行；条带绘制要采样掩码输出，必须先完成）---
    const double tMaskStart = kProf ? ProfNow() : 0.0;
    RenderMasks(model, threads);
    if (kProf)
    {
        frameProf.tMask = ProfNow() - tMaskStart;
    }

    // --- 阶段 2（并行）：行带动态分配 ---
    _nextBand = 0;
    if (threads == 1)
    {
        DrainBands(&frameProf);
        _lastRasterTriangles = frameProf.triCount;
        _lastScanPixels = frameProf.spanPx;
        g_prof.Add(frameProf);
        return;
    }

    ThreadPool* pool = _pool;
    const int activeThreads = pool->workerCount + 1;
    pool->taskKind = 1;

    const double tThreadStart = kProf ? ProfNow() : 0.0;
    if (kProf)
    {
        frameProf.tRaster -= 0.0; // 占位：DrainBands 会累加主线程光栅时间
    }
    L2dBarrierWait(&pool->startBar);
    DrainBands(&frameProf);
    L2dBarrierWait(&pool->doneBar);
    if (kProf)
    {
        const double wall = ProfNow() - tThreadStart;
        frameProf.tThread = wall - frameProf.tRaster; // 屏障/创建/等待开销
        if (frameProf.tThread < 0.0)
        {
            frameProf.tThread = 0.0;
        }
    }

    for (int i = 1; i < activeThreads; ++i)
    {
        frameProf.Add(pool->prof[i]);
    }
    _lastRasterTriangles = frameProf.triCount;
    _lastScanPixels = frameProf.spanPx;
    if (kProf)
    {
        // 工作线程负载（累积到全局，供末尾一次性输出——不要在帧内 printf）
        for (int i = 1; i < activeThreads; ++i)
        {
            g_prof.workerBusy += pool->prof[i].tRaster;
            ++g_prof.workerSamples;
        }
    }

    g_prof.Add(frameProf);
}

// ---------------------------------------------------------------------------
// 外部核心路径（--core=mocari 等）：数据来自调用方，像素管线与官方路径完全共用。
// 串行实现（不参与行带并行/线程池）：先清屏 → 逐组掩码 → 按绘制顺序光栅化。
// 已知差异（相对官方路径）：不做背面剔除（外部数据未携带 culling 标志）。
// ---------------------------------------------------------------------------
void CubismRenderer_Software::DrawExternalModel(const ExternalMesh* meshes, csmInt32 meshCount,
                                                const csmInt32* drawOrder, csmInt32 orderCount,
                                                const CubismMatrix44& mvpMatrix)
{
    if (meshes == NULL || meshCount <= 0)
    {
        return;
    }
    EnsureFramebuffer();
    if (_framebuffer == NULL)
    {
        return;
    }
    const int fbW = (int)_framebufferWidth;
    const int fbH = (int)_framebufferHeight;
    memset(_framebuffer, 0, sizeof(csmUint32) * (size_t)_framebufferSize);

    CubismMatrix44 mvp = mvpMatrix; // GetArray 非 const → 持副本（勿直接取址，见 RenderMaskGroup 注释）
    const csmFloat32* m = mvp.GetArray();
    const size_t wh = (size_t)fbW * (size_t)fbH;

    // --- 掩码组（按 masks 列表去重；与官方 FindSameClip 同思路）---
    std::vector<const csmInt32*> groupMasks;
    std::vector<csmInt32> groupCounts;
    std::vector<csmInt32> groupOf((size_t)meshCount, -1);
    const csmInt32 orderN = (drawOrder != NULL) ? orderCount : meshCount;
    for (csmInt32 k = 0; k < orderN; ++k)
    {
        const csmInt32 i = (drawOrder != NULL) ? drawOrder[k] : k;
        if (i < 0 || i >= meshCount || !meshes[i].visible || meshes[i].maskCount <= 0)
        {
            continue;
        }
        csmInt32 gid = -1;
        for (size_t g = 0; g < groupMasks.size(); ++g)
        {
            if (groupCounts[g] != meshes[i].maskCount)
            {
                continue;
            }
            csmInt32 same = 0;
            for (csmInt32 a = 0; a < groupCounts[g]; ++a)
            {
                for (csmInt32 b = 0; b < meshes[i].maskCount; ++b)
                {
                    if (groupMasks[g][a] == meshes[i].masks[b])
                    {
                        ++same;
                        break;
                    }
                }
            }
            if (same == groupCounts[g])
            {
                gid = (csmInt32)g;
                break;
            }
        }
        if (gid < 0)
        {
            gid = (csmInt32)groupMasks.size();
            groupMasks.push_back(meshes[i].masks);
            groupCounts.push_back(meshes[i].maskCount);
        }
        groupOf[(size_t)i] = gid;
    }

    // --- 掩码图（每组一张全屏：0=全裁，255=全显）---
    std::vector<std::vector<csmUint8> > maskBufs(groupMasks.size());
    for (size_t g = 0; g < groupMasks.size(); ++g)
    {
        std::vector<csmUint8>& buf = maskBufs[g];
        buf.assign(wh, 0);
        for (csmInt32 a = 0; a < groupCounts[g]; ++a)
        {
            const csmInt32 md = groupMasks[g][a];
            if (md < 0 || md >= meshCount)
            {
                continue;
            }
            const ExternalMesh& mm = meshes[md];
            if (mm.texture == NULL || mm.vertexCount <= 0 || mm.indexCount < 3)
            {
                continue;
            }
            const csmFloat32* vp = mm.xy_uv;
            for (csmInt32 t = 0; t + 2 < mm.indexCount; t += 3)
            {
                const csmUint16 i0 = mm.indices[t];
                const csmUint16 i1 = mm.indices[t + 1];
                const csmUint16 i2 = mm.indices[t + 2];
                if (i0 >= mm.vertexCount || i1 >= mm.vertexCount || i2 >= mm.vertexCount)
                {
                    continue;
                }
                ScreenVertex q[3];
                const csmUint16 vi[3] = { i0, i1, i2 };
                for (int s = 0; s < 3; ++s)
                {
                    TransformVtx(m, vp[vi[s] * 4], vp[vi[s] * 4 + 1], (csmFloat32)fbW, (csmFloat32)fbH,
                                 &q[s].x, &q[s].y);
                    q[s].u = vp[vi[s] * 4 + 2];
                    q[s].v = vp[vi[s] * 4 + 3];
                }
                RasterMaskTriangle(q[0], q[1], q[2], mm.texture, buf.data(), fbW, fbH);
            }
        }
    }

    // --- 主绘制（按绘制顺序，串行）---
    SoftProf prof;
    csmUint32 visibleDrawn = 0;
    csmUint32 totalTris = 0;
    csmUint64 totalScanPx = 0;
    for (csmInt32 k = 0; k < orderN; ++k)
    {
        const csmInt32 i = (drawOrder != NULL) ? drawOrder[k] : k;
        if (i < 0 || i >= meshCount)
        {
            continue;
        }
        if (!IsOnlyDrawable(i) || IsDrawableSkipped(i))   // 诊断开关（与官方路径同语义）
        {
            continue;
        }
        const ExternalMesh& dm = meshes[i];
        if (!dm.visible || dm.texture == NULL || dm.vertexCount <= 0 || dm.indexCount < 3)
        {
            continue;
        }

        // 基色 = (1,1,1,opacity) → 整数常数与 PrepareFrame 的 classic 情形一致
        const csmFloat32 baseRGBA[4] = { 1.0f, 1.0f, 1.0f, dm.opacity };
        const csmFloat32 mulRGB[3] = { 1.0f, 1.0f, 1.0f };
        const csmFloat32 scrRGB[3] = { 0.0f, 0.0f, 0.0f };
        IntShadeParams intShade;
        {
            const csmInt32 baseA8 = (csmInt32)(dm.opacity * 256.0f + 0.5f);
            intShade.pixels = dm.texture->Pixels();
            intShade.summary = dm.texture->AlphaSummary();
            intShade.texW = dm.texture->Width();
            intShade.texH = dm.texture->Height();
            intShade.summaryW = dm.texture->SummaryWidth();
            intShade.summaryH = dm.texture->SummaryHeight();
            intShade.baseA8 = baseA8;
            intShade.kbR = baseA8; // (256 × baseA8) >> 8
            intShade.kbG = baseA8;
            intShade.kbB = baseA8;
        }
        const csmInt32 gid = groupOf[(size_t)i];
        const csmUint8* maskBuf = (gid >= 0) ? maskBufs[(size_t)gid].data() : NULL;
        const csmInt32 maskXor = (gid >= 0 && dm.invertedMask) ? 255 : 0;
        const csmInt32 blend = (dm.blendMode >= 0 && dm.blendMode <= 2) ? dm.blendMode : SoftBlend_Normal;

        const csmFloat32* vp = dm.xy_uv;
        csmUint32 triCount = 0;
        csmUint64 scanPx = 0;
        ++visibleDrawn;
        for (csmInt32 t = 0; t + 2 < dm.indexCount; t += 3)
        {
            const csmUint16 i0 = dm.indices[t];
            const csmUint16 i1 = dm.indices[t + 1];
            const csmUint16 i2 = dm.indices[t + 2];
            if (i0 >= dm.vertexCount || i1 >= dm.vertexCount || i2 >= dm.vertexCount)
            {
                continue;
            }
            ScreenVertex q[3];
            const csmUint16 vi[3] = { i0, i1, i2 };
            for (int s = 0; s < 3; ++s)
            {
                TransformVtx(m, vp[vi[s] * 4], vp[vi[s] * 4 + 1], (csmFloat32)fbW, (csmFloat32)fbH,
                             &q[s].x, &q[s].y);
                q[s].u = vp[vi[s] * 4 + 2];
                q[s].v = vp[vi[s] * 4 + 3];
            }
            RasterTriangleScanline(q[0], q[1], q[2], dm.texture, baseRGBA, mulRGB, scrRGB,
                                   blend, _framebuffer, fbW, fbH, 0, fbH,
                                   &triCount, &scanPx, &prof, &intShade, maskBuf, maskXor);
        }
        totalTris += triCount;
        totalScanPx += scanPx;
    }

    // 诊断计数（与官方路径同语义；此前外部路径不更新 → 板上无统计可看）
    _lastVisibleCount = visibleDrawn;
    _lastRasterTriangles = totalTris;
    _lastScanPixels = totalScanPx;
}

void CubismRenderer_Software::RenderStrip(int stripY0, int stripY1,
                                          csmUint32* outTriangles, csmUint64* outScanPixels,
                                          SoftProf* prof)
{
    // 兼容旧接口：按行带遍历给定范围（单线程语义）
    FrameScratch* sc = _scratch;
    if (sc == NULL || stripY0 >= stripY1)
    {
        return;
    }
    SoftProf local;
    SoftProf* p = (prof != NULL) ? prof : &local;
    const csmInt32 b0 = stripY0 / sc->bandHeight;
    const csmInt32 b1 = (stripY1 - 1) / sc->bandHeight;
    const int fbH = (int)_framebufferHeight;
    for (csmInt32 b = b0; b <= b1 && b < sc->bandCount; ++b)
    {
        const int y0 = b * sc->bandHeight;
        int y1 = y0 + sc->bandHeight;
        if (y1 > fbH)
        {
            y1 = fbH;
        }
        RenderBand(b, y0, y1, p);
    }
    if (outTriangles != NULL)
    {
        *outTriangles = (csmUint32)p->triCount;
    }
    if (outScanPixels != NULL)
    {
        *outScanPixels = p->spanPx;
    }
}

void CubismRenderer_Software::DrawFilledSphere(csmFloat32 cx, csmFloat32 cy, csmFloat32 radius,
                                               csmUint32 rgbColor)
{
    if (_framebuffer == NULL || radius < 1.0f)
    {
        return;
    }
    const int fbW = (int)_framebufferWidth;
    const int fbH = (int)_framebufferHeight;

    int x0 = (int)(cx - radius) - 1;
    int x1 = (int)(cx + radius) + 2;
    int y0 = (int)(cy - radius) - 1;
    int y1 = (int)(cy + radius) + 2;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > fbW) x1 = fbW;
    if (y1 > fbH) y1 = fbH;

    const csmFloat32 baseR = (csmFloat32)((rgbColor >> 16) & 0xff);
    const csmFloat32 baseG = (csmFloat32)((rgbColor >> 8) & 0xff);
    const csmFloat32 baseB = (csmFloat32)(rgbColor & 0xff);

    // 光源方向（右上偏前）
    const csmFloat32 lx = -0.45f;
    const csmFloat32 ly = -0.55f;
    const csmFloat32 lz = 0.70f;

    const csmFloat32 r2 = radius * radius;

    for (int y = y0; y < y1; ++y)
    {
        const csmFloat32 dy = (csmFloat32)y + 0.5f - cy;
        csmUint32* row = _framebuffer + (size_t)y * fbW;
        for (int x = x0; x < x1; ++x)
        {
            const csmFloat32 dx = (csmFloat32)x + 0.5f - cx;
            const csmFloat32 d2 = dx * dx + dy * dy;
            if (d2 > r2)
            {
                continue;
            }
            // 球面法线
            const csmFloat32 nz = sqrtf(1.0f - d2 / r2);
            const csmFloat32 nx = dx / radius;
            const csmFloat32 ny = dy / radius;
            csmFloat32 diff = nx * lx + ny * ly + nz * lz;
            if (diff < 0.0f) diff = 0.0f;
            csmFloat32 shade = 0.42f + 0.58f * diff;

            // 边缘 1.5px 抗锯齿（与背景预乘混合）
            csmFloat32 alpha = 1.0f;
            const csmFloat32 edge = radius - sqrtf(d2);
            if (edge < 1.5f)
            {
                alpha = edge / 1.5f;
                shade = shade * 0.85f + 0.15f;
            }

            csmFloat32 rr = baseR * shade;
            csmFloat32 gg = baseG * shade;
            csmFloat32 bb = baseB * shade;
            // 镜面高光：diff^22 用反复平方（powf 每像素 ~100+ 周期，这里是 UI 热点）
            const csmFloat32 d2s = diff * diff;
            const csmFloat32 d4s = d2s * d2s;
            const csmFloat32 d8s = d4s * d4s;
            const csmFloat32 d16s = d8s * d8s;
            const csmFloat32 spec = d16s * d4s * d2s;
            rr += spec * 220.0f;
            gg += spec * 220.0f;
            bb += spec * 220.0f;

            const csmUint32 a255 = ClampByte(alpha * 255.0f);
            const csmUint32 color = (ClampByte(rr) << 16) | (ClampByte(gg) << 8) | ClampByte(bb);

            if (a255 >= 250)
            {
                row[x] = 0xff000000u | color;
            }
            else
            {
                // 边缘抗锯齿：与背景做预乘混合
                const csmUint32 dst = row[x];
                const csmUint32 da = (dst >> 24) & 0xff;
                const csmUint32 dr = (dst >> 16) & 0xff;
                const csmUint32 dg = (dst >> 8) & 0xff;
                const csmUint32 db = dst & 0xff;
                const csmFloat32 invA = 1.0f - alpha;
                const csmUint32 oa = (csmUint32)((csmFloat32)a255 + (csmFloat32)da * invA);
                const csmUint32 orr = (csmUint32)(((color >> 16) & 0xff) * alpha + (csmFloat32)dr * invA);
                const csmUint32 og = (csmUint32)(((color >> 8) & 0xff) * alpha + (csmFloat32)dg * invA);
                const csmUint32 ob = (csmUint32)((color & 0xff) * alpha + (csmFloat32)db * invA);
                row[x] = (ClampByte((csmFloat32)oa) << 24) | (ClampByte((csmFloat32)orr) << 16)
                       | (ClampByte((csmFloat32)og) << 8) | ClampByte((csmFloat32)ob);
            }
        }
    }
}

void CubismRenderer_Software::DrawFlatCircle(csmFloat32 cx, csmFloat32 cy, csmFloat32 radius,
                                             csmUint32 rgbColor)
{
    if (_framebuffer == NULL || radius < 1.0f)
    {
        return;
    }
    const int fbW = (int)_framebufferWidth;
    const int fbH = (int)_framebufferHeight;
    int x0 = (int)(cx - radius) - 1;
    int x1 = (int)(cx + radius) + 2;
    int y0 = (int)(cy - radius) - 1;
    int y1 = (int)(cy + radius) + 2;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > fbW) x1 = fbW;
    if (y1 > fbH) y1 = fbH;

    const csmUint32 fr = (rgbColor >> 16) & 0xff;
    const csmUint32 fg = (rgbColor >> 8) & 0xff;
    const csmUint32 fb = rgbColor & 0xff;
    const csmFloat32 r2 = radius * radius;
    const csmFloat32 rInner2 = (radius - 1.0f) * (radius - 1.0f);

    for (int y = y0; y < y1; ++y)
    {
        const csmFloat32 dy = (csmFloat32)y + 0.5f - cy;
        csmUint32* row = _framebuffer + (size_t)y * fbW;
        for (int x = x0; x < x1; ++x)
        {
            const csmFloat32 dx = (csmFloat32)x + 0.5f - cx;
            const csmFloat32 d2 = dx * dx + dy * dy;
            if (d2 > r2)
            {
                continue;
            }
            csmFloat32 alpha = 1.0f;
            // 仅在 1px 边缘带才需要 sqrt（内部直接不透明填充）
            if (d2 > rInner2)
            {
                const csmFloat32 edge = radius - sqrtf(d2);
                if (edge < 1.0f)
                {
                    alpha = edge;
                }
            }
            const csmUint32 a255 = ClampByte(alpha * 255.0f);
            if (a255 >= 250)
            {
                row[x] = 0xff000000u | (fr << 16) | (fg << 8) | fb;
            }
            else
            {
                const csmUint32 dst = row[x];
                const csmUint32 da = (dst >> 24) & 0xff;
                const csmUint32 dr = (dst >> 16) & 0xff;
                const csmUint32 dg = (dst >> 8) & 0xff;
                const csmUint32 db = dst & 0xff;
                const csmFloat32 invA = 1.0f - alpha;
                const csmUint32 oa = (csmUint32)((csmFloat32)a255 + (csmFloat32)da * invA);
                const csmUint32 orr = (csmUint32)((csmFloat32)fr * alpha + (csmFloat32)dr * invA);
                const csmUint32 og = (csmUint32)((csmFloat32)fg * alpha + (csmFloat32)dg * invA);
                const csmUint32 ob = (csmUint32)((csmFloat32)fb * alpha + (csmFloat32)db * invA);
                row[x] = (ClampByte((csmFloat32)oa) << 24) | (ClampByte((csmFloat32)orr) << 16)
                       | (ClampByte((csmFloat32)og) << 8) | ClampByte((csmFloat32)ob);
            }
        }
    }
}

void CubismRenderer_Software::DrawTriangleFilled(csmFloat32 x0, csmFloat32 y0,
                                                 csmFloat32 x1, csmFloat32 y1,
                                                 csmFloat32 x2, csmFloat32 y2,
                                                 csmUint32 rgbColor)
{
    if (_framebuffer == NULL)
    {
        return;
    }
    const int fbW = (int)_framebufferWidth;
    const int fbH = (int)_framebufferHeight;

    csmFloat32 minXf = x0 < x1 ? x0 : x1;
    if (x2 < minXf) minXf = x2;
    csmFloat32 maxXf = x0 > x1 ? x0 : x1;
    if (x2 > maxXf) maxXf = x2;
    csmFloat32 minYf = y0 < y1 ? y0 : y1;
    if (y2 < minYf) minYf = y2;
    csmFloat32 maxYf = y0 > y1 ? y0 : y1;
    if (y2 > maxYf) maxYf = y2;

    int minX = (int)minXf - 2;
    int maxX = (int)maxXf + 3;
    int minY = (int)minYf - 2;
    int maxY = (int)maxYf + 3;
    if (minX < 0) minX = 0;
    if (minY < 0) minY = 0;
    if (maxX > fbW) maxX = fbW;
    if (maxY > fbH) maxY = fbH;
    if (minX >= maxX || minY >= maxY)
    {
        return;
    }

    const csmFloat32 area = (x1 - x0) * (y2 - y0) - (y1 - y0) * (x2 - x0);
    if (area > -1e-6f && area < 1e-6f)
    {
        return;
    }
    const csmFloat32 invArea = 1.0f / area;

    const csmUint32 fr = (rgbColor >> 16) & 0xff;
    const csmUint32 fg = (rgbColor >> 8) & 0xff;
    const csmUint32 fb = rgbColor & 0xff;
    const csmFloat32 aa = 1.0f; // 边缘 1px 柔化（近似抗锯齿）

    for (int y = minY; y < maxY; ++y)
    {
        const csmFloat32 cy = (csmFloat32)y + 0.5f;
        csmUint32* row = _framebuffer + (size_t)y * fbW;
        for (int x = minX; x < maxX; ++x)
        {
            const csmFloat32 cx = (csmFloat32)x + 0.5f;
            // 归一化重心（与场景一致的方向约定）
            const csmFloat32 w0 = ((x2 - x1) * (cy - y1) - (y2 - y1) * (cx - x1)) * invArea;
            const csmFloat32 w1 = ((x0 - x2) * (cy - y2) - (y0 - y2) * (cx - x2)) * invArea;
            const csmFloat32 w2 = 1.0f - w0 - w1;
            csmFloat32 m = w0 < w1 ? w0 : w1;
            if (w2 < m) m = w2; // 最小重心权重（负=在外）
            csmFloat32 alpha;
            if (m >= aa)
            {
                alpha = 1.0f;
            }
            else if (m <= -aa)
            {
                continue;
            }
            else
            {
                alpha = (m + aa) / (2.0f * aa) * 1.0f;
            }

            const csmUint32 a255 = ClampByte(alpha * 255.0f);
            if (a255 >= 250)
            {
                row[x] = 0xff000000u | (fr << 16) | (fg << 8) | fb;
            }
            else
            {
                const csmUint32 dst = row[x];
                const csmUint32 da = (dst >> 24) & 0xff;
                const csmUint32 dr = (dst >> 16) & 0xff;
                const csmUint32 dg = (dst >> 8) & 0xff;
                const csmUint32 db = dst & 0xff;
                const csmFloat32 invA = 1.0f - alpha;
                const csmUint32 oa = (csmUint32)((csmFloat32)a255 + (csmFloat32)da * invA);
                const csmUint32 orr = (csmUint32)((csmFloat32)fr * alpha + (csmFloat32)dr * invA);
                const csmUint32 og = (csmUint32)((csmFloat32)fg * alpha + (csmFloat32)dg * invA);
                const csmUint32 ob = (csmUint32)((csmFloat32)fb * alpha + (csmFloat32)db * invA);
                row[x] = (ClampByte((csmFloat32)oa) << 24) | (ClampByte((csmFloat32)orr) << 16)
                       | (ClampByte((csmFloat32)og) << 8) | ClampByte((csmFloat32)ob);
            }
        }
    }
}

void CubismRenderer_Software::DrawCircleRing(csmFloat32 cx, csmFloat32 cy, csmFloat32 radius,
                                             csmFloat32 thickness, csmUint32 rgbColor)
{
    if (_framebuffer == NULL || radius < 2.0f)
    {
        return;
    }
    const int fbW = (int)_framebufferWidth;
    const int fbH = (int)_framebufferHeight;
    int x0 = (int)(cx - radius - thickness) - 1;
    int x1 = (int)(cx + radius + thickness) + 2;
    int y0 = (int)(cy - radius - thickness) - 1;
    int y1 = (int)(cy + radius + thickness) + 2;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > fbW) x1 = fbW;
    if (y1 > fbH) y1 = fbH;

    const csmUint32 fr = (rgbColor >> 16) & 0xff;
    const csmUint32 fg = (rgbColor >> 8) & 0xff;
    const csmUint32 fb = rgbColor & 0xff;
    const csmFloat32 outer = radius + thickness * 0.5f;
    const csmFloat32 inner = radius - thickness * 0.5f;
    const csmFloat32 aa = 1.2f; // 边缘柔化带宽

    const csmFloat32 outer2 = (outer + aa) * (outer + aa);
    const csmFloat32 inner2 = (inner - aa) * (inner - aa);

    for (int y = y0; y < y1; ++y)
    {
        const csmFloat32 dy = (csmFloat32)y + 0.5f - cy;
        csmUint32* row = _framebuffer + (size_t)y * fbW;
        for (int x = x0; x < x1; ++x)
        {
            const csmFloat32 dx = (csmFloat32)x + 0.5f - cx;
            const csmFloat32 d2 = dx * dx + dy * dy;
            // 环带外/内（含柔化带宽）直接跳过：只有边缘带才需要 sqrt
            if (d2 <= inner2 || d2 >= outer2)
            {
                continue;
            }
            const csmFloat32 d = sqrtf(d2);
            // 环带权重（内外边缘柔化）
            csmFloat32 a = 1.0f;
            const csmFloat32 dOuter = outer - d;
            const csmFloat32 dInner = d - inner;
            if (dOuter < aa) a = dOuter / aa;
            if (dInner < aa && dInner < a) a = dInner / aa;
            if (a <= 0.0f)
            {
                continue;
            }
            const csmUint32 a255 = ClampByte(a * 255.0f);
            if (a255 >= 250)
            {
                row[x] = 0xff000000u | (fr << 16) | (fg << 8) | fb;
            }
            else
            {
                const csmUint32 dst = row[x];
                const csmUint32 da = (dst >> 24) & 0xff;
                const csmUint32 dr = (dst >> 16) & 0xff;
                const csmUint32 dg = (dst >> 8) & 0xff;
                const csmUint32 db = dst & 0xff;
                const csmFloat32 invA = 1.0f - a;
                const csmUint32 oa = (csmUint32)((csmFloat32)a255 + (csmFloat32)da * invA);
                const csmUint32 orr = (csmUint32)((csmFloat32)fr * a + (csmFloat32)dr * invA);
                const csmUint32 og = (csmUint32)((csmFloat32)fg * a + (csmFloat32)dg * invA);
                const csmUint32 ob = (csmUint32)((csmFloat32)fb * a + (csmFloat32)db * invA);
                row[x] = (ClampByte((csmFloat32)oa) << 24) | (ClampByte((csmFloat32)orr) << 16)
                       | (ClampByte((csmFloat32)og) << 8) | ClampByte((csmFloat32)ob);
            }
        }
    }
}

void CubismRenderer_Software::FillRect(csmFloat32 x0, csmFloat32 y0, csmFloat32 x1, csmFloat32 y1,
                                       csmUint32 rgbColor, csmUint8 alpha)
{
    if (_framebuffer == NULL || alpha == 0)
    {
        return;
    }
    const int fbW = (int)_framebufferWidth;
    const int fbH = (int)_framebufferHeight;
    int ix0 = (int)x0;
    int iy0 = (int)y0;
    int ix1 = (int)x1 + 1;
    int iy1 = (int)y1 + 1;
    if (ix0 < 0) ix0 = 0;
    if (iy0 < 0) iy0 = 0;
    if (ix1 > fbW) ix1 = fbW;
    if (iy1 > fbH) iy1 = fbH;
    if (ix0 >= ix1 || iy0 >= iy1)
    {
        return;
    }

    // 整数打包混合（UI 面板是大面积单色填充——纯标量浮点曾是 2.4ms/帧的主要来源）：
    //   out = src_premul + dst*(255-a)/255（Normal，预乘语义）
    //   R/B 一个 32 位字（16 位道）、G/A 一个 32 位字；每道值与权重乘积 ≤ 255×255
    //   不串道；/255 用 (x+128)*257>>16 精确四舍五入；alpha 单独算以便饱和。
    const csmInt32 a = (csmInt32)alpha;
    const csmInt32 inv = 255 - a;
    const csmUint32 addRound = 0x00800080u;
    const csmUint32 srcRB = (Div255(((rgbColor >> 16) & 0xffu) * (csmUint32)a) << 16)
                          | Div255((rgbColor & 0xffu) * (csmUint32)a);
    // G 与 ga 字的 G 道对齐（bits0-7）；A 单独标量算（便于饱和到 255）
    const csmUint32 srcG = Div255(((rgbColor >> 8) & 0xffu) * (csmUint32)a);

    for (int y = iy0; y < iy1; ++y)
    {
        csmUint32* row = _framebuffer + (size_t)y * fbW;
        for (int x = ix0; x < ix1; ++x)
        {
            const csmUint32 d = row[x];
            const csmUint32 rb = ((((d & 0x00FF00FFu) * (csmUint32)inv) + addRound) >> 8) & 0x00FF00FFu;
            const csmUint32 ga = (((((d >> 8) & 0x00FF00FFu) * (csmUint32)inv) + addRound) >> 8) & 0x00FF00FFu;
            const csmUint32 oa = (csmUint32)(a + Div255((csmInt32)(d >> 24) * inv));
            const csmUint32 sat = (oa > 255u) ? 255u : oa;
            // RGB 相加需防字段溢出：sr/Div256 两项各 ≤255，和 ≤510 → 用 16 位道内加法后饱和
            const csmUint32 sumRB = rb + srcRB;
            const csmUint32 sumG = ga + srcG; // ga 的 A 道（bits16-23）不参与，取出时已屏蔽
            const csmUint32 bOut = (sumRB & 0x100u) ? 0xFFu : (sumRB & 0xFFu);
            const csmUint32 rOut = (sumRB & 0x01000000u) ? 0xFFu : ((sumRB >> 16) & 0xFFu);
            const csmUint32 gOut = (sumG & 0x100u) ? 0xFFu : (sumG & 0xFFu);
            row[x] = (sat << 24) | (rOut << 16) | (gOut << 8) | bOut;
        }
    }
}

// 5×7 点阵字库（列式：每字符 5 列，每列 bit0=顶行 .. bit6=底行）
namespace
{
struct Glyph5x7
{
    char ch;
    csmUint8 col[5];
};

const Glyph5x7 kGlyphs5x7[] = {
    {' ', {0x00, 0x00, 0x00, 0x00, 0x00}},
    {'%', {0x23, 0x13, 0x08, 0x64, 0x62}},
    {'0', {0x3E, 0x51, 0x49, 0x45, 0x3E}},
    {'1', {0x00, 0x42, 0x7F, 0x40, 0x00}},
    {'2', {0x42, 0x61, 0x51, 0x49, 0x46}},
    {'3', {0x21, 0x41, 0x45, 0x4B, 0x31}},
    {'4', {0x18, 0x14, 0x12, 0x7F, 0x10}},
    {'5', {0x27, 0x45, 0x45, 0x45, 0x39}},
    {'6', {0x3C, 0x4A, 0x49, 0x49, 0x30}},
    {'7', {0x01, 0x71, 0x09, 0x05, 0x03}},
    {'8', {0x36, 0x49, 0x49, 0x49, 0x36}},
    {'9', {0x06, 0x49, 0x49, 0x29, 0x1E}},
    {'F', {0x7F, 0x09, 0x09, 0x09, 0x01}},
    {'P', {0x7F, 0x09, 0x09, 0x09, 0x06}},
    {'S', {0x46, 0x49, 0x49, 0x49, 0x31}},
    {'x', {0x00, 0x14, 0x08, 0x14, 0x00}},
    {'A', {0x7E, 0x11, 0x11, 0x11, 0x7E}},
    {'C', {0x3E, 0x41, 0x41, 0x41, 0x22}},
    {'M', {0x7F, 0x02, 0x0C, 0x02, 0x7F}},
    {'R', {0x7F, 0x09, 0x19, 0x29, 0x46}},
    {'U', {0x3F, 0x40, 0x40, 0x40, 0x3F}},
    // 补全大写字母（模型名显示用）+ 连字符
    {'B', {0x7F, 0x49, 0x49, 0x49, 0x36}},
    {'D', {0x7F, 0x41, 0x41, 0x22, 0x1C}},
    {'E', {0x7F, 0x49, 0x49, 0x49, 0x41}},
    {'G', {0x3E, 0x41, 0x49, 0x49, 0x7A}},
    {'H', {0x7F, 0x08, 0x08, 0x08, 0x7F}},
    {'I', {0x00, 0x41, 0x7F, 0x41, 0x00}},
    {'J', {0x20, 0x40, 0x41, 0x3F, 0x01}},
    {'K', {0x7F, 0x08, 0x14, 0x22, 0x41}},
    {'L', {0x7F, 0x40, 0x40, 0x40, 0x40}},
    {'N', {0x7F, 0x04, 0x08, 0x10, 0x7F}},
    {'O', {0x3E, 0x41, 0x41, 0x41, 0x3E}},
    {'Q', {0x3E, 0x41, 0x51, 0x21, 0x5E}},
    {'T', {0x01, 0x01, 0x7F, 0x01, 0x01}},
    {'V', {0x1F, 0x20, 0x40, 0x20, 0x1F}},
    {'W', {0x7F, 0x20, 0x18, 0x20, 0x7F}},
    {'X', {0x63, 0x14, 0x08, 0x14, 0x63}},
    {'Y', {0x03, 0x04, 0x78, 0x04, 0x03}},
    {'Z', {0x61, 0x51, 0x49, 0x45, 0x43}},
    {'-', {0x08, 0x08, 0x08, 0x08, 0x08}},
    {'.', {0x00, 0x60, 0x60, 0x00, 0x00}},
};
} // namespace

void CubismRenderer_Software::DrawText5x7(csmFloat32 x, csmFloat32 y, csmInt32 scale,
                                          const char* text, csmUint32 rgbColor)
{
    if (_framebuffer == NULL || text == NULL || scale < 1)
    {
        return;
    }
    const int fbW = (int)_framebufferWidth;
    const int fbH = (int)_framebufferHeight;
    const csmUint32 color = 0xff000000u | (rgbColor & 0xffffffu);

    csmFloat32 cx = x;
    for (const char* p = text; *p != '\0'; ++p)
    {
        const Glyph5x7* glyph = NULL;
        for (size_t i = 0; i < sizeof(kGlyphs5x7) / sizeof(kGlyphs5x7[0]); ++i)
        {
            if (kGlyphs5x7[i].ch == *p)
            {
                glyph = &kGlyphs5x7[i];
                break;
            }
        }
        if (glyph != NULL)
        {
            for (int col = 0; col < 5; ++col)
            {
                const csmUint8 bits = glyph->col[col];
                for (int row = 0; row < 7; ++row)
                {
                    if ((bits >> row) & 1)
                    {
                        const int px0 = (int)(cx + (csmFloat32)(col * scale));
                        const int py0 = (int)(y + (csmFloat32)(row * scale));
                        for (int dy = 0; dy < scale; ++dy)
                        {
                            const int py = py0 + dy;
                            if (py < 0 || py >= fbH)
                            {
                                continue;
                            }
                            csmUint32* frow = _framebuffer + (size_t)py * fbW;
                            for (int dx = 0; dx < scale; ++dx)
                            {
                                const int px = px0 + dx;
                                if (px >= 0 && px < fbW)
                                {
                                    frow[px] = color;
                                }
                            }
                        }
                    }
                }
            }
        }
        cx += (csmFloat32)(6 * scale); // 5 列 + 1 列字距
    }
}

void CubismRenderer_Software::PrintProfile()
{
    if (!kProf)
    {
        return;
    }
    static int printed = 0;
    if (printed++ > 0)
    {
        return;
    }
    const double ms = 1000.0;
    const double f = g_prof.frames > 0 ? (double)g_prof.frames : 1.0;
    const double tot = (g_prof.tMemset + g_prof.tVert + g_prof.tRaster + g_prof.tOther
                        + g_prof.tThread + g_prof.tUi + g_prof.tMask) * ms / f;
    printf("[prof] 帧数=%.0f 渲染内部=%.2fms/帧 | 清屏=%.2f 顶点变换=%.2f 光栅化=%.2f 其余=%.2f 线程=%.2f 掩码=%.2f UI=%.2f\n",
           f, tot, g_prof.tMemset * ms / f, g_prof.tVert * ms / f, g_prof.tRaster * ms / f,
           g_prof.tOther * ms / f, g_prof.tThread * ms / f, g_prof.tMask * ms / f,
           g_prof.tUi * ms / f);
    printf("[prof] 像素/帧: 扫描=%.0f 过摘要=%.0f 实际混合=%.0f | 三角形/帧=%.0f drawable迭代/帧=%.0f\n",
           (double)g_prof.spanPx / f, (double)g_prof.sampledPx / f,
           (double)g_prof.blendedPx / f, (double)g_prof.triCount / f,
           (double)g_prof.drawCalls / f);
    if (g_prof.workerSamples > 0)
    {
        const double avg = g_prof.workerBusy / (double)g_prof.workerSamples;
        const double mainAvg = (g_prof.tRaster - g_prof.workerBusy) / f;
        printf("[prof] 线程负载(光栅 ms/帧): 工作线程均值=%.2f 主线程=%.2f\n",
               avg * 1000.0, mainAvg * 1000.0);
    }
    if (g_prof.tRaster > 0.0 && g_prof.blendedPx > 0)
    {
        printf("[prof] 每混合像素光栅化成本=%.1f ns\n",
               g_prof.tRaster * 1e9 / (double)g_prof.blendedPx);
    }
}

void CubismRenderer_Software::NoteUiTime(double seconds)
{
    if (kProf)
    {
        g_prof.tUi += seconds;
    }
}

void CubismRenderer_Software::SaveProfile()
{
    // 无图形 API 状态可保存（软渲染直接写内存缓冲）
}

void CubismRenderer_Software::RestoreProfile()
{
}

void CubismRenderer_Software::BeforeDrawModelRenderTarget()
{
}

void CubismRenderer_Software::AfterDrawModelRenderTarget()
{
}

}}}}

// ---- Framework 要求的后端工厂（与官方各后端一样只保留一份实现）----

namespace Live2D { namespace Cubism { namespace Framework { namespace Rendering {

CubismRenderer* CubismRenderer::Create(csmUint32 width, csmUint32 height)
{
    return CubismRenderer_Software::Create(width, height);
}

void CubismRenderer::StaticRelease()
{
}

}}}}

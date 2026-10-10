/**
 * CubismRenderer_Software.hpp — 软件光栅化后端（Live2D Cubism Framework r.5）
 * （无 GPU 平台与测试/离屏渲染路径；桌面桌宠正常走官方 GLES2 后端）
 *
 * 与官方 GLES2 后端同构：实现 5 个纯虚 + 工厂 CubismRenderer::Create。
 * 绘制管线（对应官方 vertex/fragment shader 语义）：
 *   1. 顶点：屏幕坐标 = MVP（行向量右乘约定）→ NDC → 像素；UV 直传
 *   2. 片元：texel → ×multiplyColor → screen 混合 → ×baseColor(模型色×drawable opacity)
 *      → **预乘 alpha**（与官方 FragShaderSrc.frag 的 color.rgb*color.a 一致）
 *   3. 混合：Normal/AddCompatible/MultiplyCompatible 三式（官方 5.2- 路径的
 *      glBlendFuncSeparate 组合，见 .cpp 注释表）
 *   4. 顺序：按 renderOrder 排序后逐个 drawable 绘制
 *   5. 剪裁掩码：每掩码组一份全屏单字节图（见 .cpp「剪裁掩码」段），
 *      条带绘制前先按组渲染（并行），主绘制采样后乘入预乘色与 alpha
 *
 * 未实现（后续迭代）：offscreen / 5.3+ 新式混合模式（检测到会告警）。
 */

#pragma once

#include <vector>
#include <cmath>

#include <Rendering/CubismRenderer.hpp>

namespace Live2D { namespace Cubism { namespace Framework { namespace Rendering {

class SoftTexture;

class CubismRenderer_Software : public CubismRenderer
{
public:
    static CubismRenderer_Software* Create(csmUint32 width, csmUint32 height);

    /** 绑定模型纹理（对应官方 BindTexture(GLuint)；index 来自 drawable 的 textureIndex） */
    void BindTexture(csmInt32 textureIndex, const SoftTexture* texture);

    /** 渲染目标（ARGB8888，预乘 alpha；alpha=0 表示透明） */
    const csmUint32* GetFramebuffer() const;
    csmUint32 GetFramebufferWidth() const;
    csmUint32 GetFramebufferHeight() const;

    /** 最近一次 DoDrawModel 的统计（诊断用） */
    csmUint32 GetLastVisibleDrawableCount() const;
    csmUint32 GetLastRasterTriangleCount() const;
    csmUint64 GetLastScanPixelCount() const;
    csmUint32 GetLastMaskWarnCount() const;
    void ResetMaskWarn();

    /** 光栅化线程数（0=自动：CPU 数，上限 4）；单线程=1 */
    void SetRasterThreads(int threads);

    /**
     * 绘制顺序模式：
     *   true （静态）= 首帧按 Core 的 (drawOrder, renderOrder) 排序一次后**冻结**；
     *   false（动态）= 每帧重排（官方语义：5.x 模型可用参数驱动绘制顺序，
     *                  实测 shuflle 看向右上角/呼吸抬头相位时把胸口皮肤层弹到裙子上面）。
     * 默认 false（保持官方语义）；播放器 `--order static|dynamic` 控制（默认 static）。
     */
    void SetStaticDrawOrder(csmBool enable);

    /**
     * 肢体刚性覆盖（"纸片人"式）：把指定 drawable 的顶点在投影前绕 (px,py) 旋转 deg 度
     * （模型坐标、y 向上）。用途：模型作者没绑"抬手/摸头"参数时，仍能让 LLM 把手臂
     * 美术整条抬起（BODY 通道 arm.r/arm.l → 播放器换算成这里的旋转）。
     * 默认**全部关闭**（变换恒等）→ 输出与基线逐位一致。清除 = ClearDrawableLimbRotates()。
     */
    void SetDrawableLimbRotate(csmInt32 drawableIndex, csmFloat32 px, csmFloat32 py, csmFloat32 deg);
    /** 抬起的肢体画在最上层（默认 false；ClearDrawableLimbRotates 一并清） */
    void SetDrawableLimbOnTop(csmInt32 drawableIndex, csmBool onTop);
    void ClearDrawableLimbRotates();

    /**
     * 关节骨架蒙皮（手臂"只在关节弯折"；见 live2d/DESIGN-ARM-JOINTS.md）：
     * 骨 k 是一个 2D 仿射（模型坐标，y 向上）：x' = a·x + b·y + tx，y' = c·x + d·y + ty，
     * 按 [a,b,c,d,tx,ty] 6 个数一组传入；骨 0 约定为恒等（躯干）。
     * 每个顶点 = 两根骨按权重混合：pos = (1-w)·B0(v) + w·B1(v)。远离关节的顶点 b0==b1（纯刚性），
     * 只有关节两侧的窄带里 0<w<1。默认没有蒙皮（恒等）→ 输出与基线逐位一致。
     */
    struct SkinW
    {
        unsigned char b0, b1;
        csmFloat32 w;
    };
    void SetSkinBones(const csmFloat32* affine6, csmInt32 boneCount);
    void SetDrawableSkin(csmInt32 drawableIndex, const SkinW* weights, csmInt32 vertexCount);
    void ClearSkins();

    struct SoftProf; // 性能剖析累加器（L2D_DBG_PROF=1 时有效）

    /** 每帧共享渲染数据：drawable 状态 + 屏幕顶点 + 三角形按行带分桶（定义在 .cpp） */
    struct FrameScratch;

    /** 持久光栅化线程池（定义在 .cpp） */
    struct ThreadPool;

    /** 剪裁掩码：按模型的掩码组表 + 本帧使用标记（定义在 .cpp） */
    struct MaskScratch;

    /**
     * 外部网格（数据由调用方提供）：
     * 与官方路径共用同一套像素管线（整数着色/双线性/掩码/三种混合），只是数据来源不同。
     * 顶点为模型空间 [x,y,u,v]×n，v 按官方 Cubism 约定（0=底部）——调用方负责翻译。
     */
    struct ExternalMesh
    {
        const csmFloat32* xy_uv;
        const csmUint16* indices;
        csmInt32 vertexCount;
        csmInt32 indexCount;
        csmFloat32 opacity;          // 0..1
        csmInt32 blendMode;          // 0=Normal 1=Add 2=Multiply（与 SoftBlend 一致）
        csmInt32 visible;
        const csmInt32* masks;
        csmInt32 maskCount;
        csmInt32 invertedMask;
        const SoftTexture* texture;  // 由调用方按 textureIndex 解析；NULL=跳过该 drawable
    };

    /** 外部核心画一帧：清屏 → 各组掩码 → 按 order 顺序串行光栅化 */
    void DrawExternalModel(const ExternalMesh* meshes, csmInt32 meshCount,
                           const csmInt32* drawOrder, csmInt32 orderCount,
                           const CubismMatrix44& mvp);

    /** 渲染一条水平条带（条带间互不重叠；单线程调试用） */
    void RenderStrip(int yStart, int yEnd, csmUint32* outTriangles, csmUint64* outScanPixels,
                     SoftProf* prof = NULL);

    /** 渲染分配给本线程的下一批行带（原子取任务；持久线程池的工作函数） */
    void DrainBands(SoftProf* prof);

    /** 渲染分配给本线程的下一个掩码组（原子取组；掩码阶段工作函数） */
    void DrainMaskGroups(SoftProf* prof);

    /** 渲染一个行带 [y0,y1)（读 PrepareFrame 建立的共享数据） */
    void RenderBand(int bandIndex, int y0, int y1, SoftProf* prof);

    /** 打印一次性能剖析（L2D_DBG_PROF=1 才有数据；供播放器汇总输出） */
    void PrintProfile();

    /** UI（状态栏/缩放条/圆环）耗时计入剖析 */
    void NoteUiTime(double seconds);

    /** 在帧缓冲上画一个实心球（带简单球面光照；用于 UI 把手）。在 DrawModel 之后调用。 */
    void DrawFilledSphere(csmFloat32 cx, csmFloat32 cy, csmFloat32 radius,
                          csmUint32 rgbColor);

    /** 扁平实心圆（纯色 + 1px 边缘抗锯齿） */
    void DrawFlatCircle(csmFloat32 cx, csmFloat32 cy, csmFloat32 radius,
                        csmUint32 rgbColor);

    /** 圆环（空心圆；用于显示跟随目标位置） */
    void DrawCircleRing(csmFloat32 cx, csmFloat32 cy, csmFloat32 radius,
                        csmFloat32 thickness, csmUint32 rgbColor);

    /** 实心三角形（UI 箭头等；屏幕空间坐标，edge-function 填充 + 边缘抗锯齿） */
    void DrawTriangleFilled(csmFloat32 x0, csmFloat32 y0,
                            csmFloat32 x1, csmFloat32 y1,
                            csmFloat32 x2, csmFloat32 y2, csmUint32 rgbColor);

    /** 矩形填充（alpha 0..255 混合；用于 UI 面板/控制条） */
    void FillRect(csmFloat32 x0, csmFloat32 y0, csmFloat32 x1, csmFloat32 y1,
                  csmUint32 rgbColor, csmUint8 alpha);

    /** 5×7 点阵文字（支持 0-9 F P S % 空格等；scale 为像素放大倍数） */
    void DrawText5x7(csmFloat32 x, csmFloat32 y, csmInt32 scale,
                     const char* text, csmUint32 rgbColor);

protected:
    CubismRenderer_Software(csmUint32 width, csmUint32 height);
    virtual ~CubismRenderer_Software();

    virtual void DoDrawModel();
    virtual void SaveProfile();
    virtual void RestoreProfile();
    virtual void BeforeDrawModelRenderTarget();
    virtual void AfterDrawModelRenderTarget();

private:
    void EnsureFramebuffer();
    void DrawDrawable(csmInt32 drawableIndex);
    void BuildSortedDrawableOrder();

    void EnsureScratch(csmInt32 drawableCount, csmInt32 indexCount, csmInt32 vertexCount);
    void DestroyScratch();
    void PrepareFrame(CubismModel* model, SoftProf* prof);
    void EnsurePool(csmInt32 threads);
    void ReleasePool();

    void EnsureMaskScratch(CubismModel* model);   // 建/更新掩码组表 + 缓冲
    void DestroyMaskScratch();
    void RenderMasks(CubismModel* model, csmInt32 threads); // 掩码阶段（条带并行前）
    void RenderMaskGroup(csmInt32 group);         // 渲染一个掩码组（含清零）

    csmUint32* _framebuffer;
    csmUint32 _framebufferSize;         // 元素个数
    csmUint32 _framebufferWidth;
    csmUint32 _framebufferHeight;

    const SoftTexture** _textures;      // 纹理表（与 model3.json Textures 顺序一致）
    csmInt32 _textureCount;

    csmInt32* _sortedDrawables;         // 按 renderOrder 排序的 drawable 索引表
    csmBool _staticOrder;               // true=静态序（首帧排序后冻结，见 SetStaticDrawOrder）

    /** 肢体刚性覆盖：按 drawable 索引的旋转表（空 = 全关；见 SetDrawableLimbRotate）。
     *  默认构造 = 恒等（cosA=1/sinA=0），resize 出来的槽位不会意外旋转。 */
    struct LimbRot
    {
        csmFloat32 px, py, cosA, sinA;
        csmBool onTop;   // 抬起时画在最上层（否则被长发等盖住）
        // 注：曾有"弯曲变形（bend：角度沿臂轴渐变 + 拉伸）"模式——**已删除（bug）**：
        // 手臂只能在关节处弯折；臂身自己弯成弧线 = 软管，违背骨架常识。
        LimbRot()
            : px(0.0f), py(0.0f), cosA(1.0f), sinA(0.0f), onTop(false) {}
    };
    std::vector<LimbRot> _limbRot;
    std::vector<csmFloat32> _skinBones;               // 6×骨数（见 SetSkinBones）
    std::vector<std::vector<SkinW> > _skins;          // 按 drawable；空 = 无蒙皮

    /** "抬起的肢体画在最上层"（按 drawable；见 SetDrawableLimbRotate 的 onTop）*/
    inline csmBool LimbIsOnTop(csmInt32 d) const
    {
        return (!_limbRot.empty() && d >= 0 && (size_t)d < _limbRot.size())
               ? _limbRot[(size_t)d].onTop : false;
    }

    /** 顶点读取钩子（模型坐标）：有蒙皮走骨架；否则无覆盖原样返回，有则绕 (px,py) 旋转 */
    inline void LimbXform(csmInt32 d, csmInt32 vi, csmFloat32 x, csmFloat32 y,
                          csmFloat32* ox, csmFloat32* oy) const
    {
        if (!_skins.empty() && d >= 0 && (size_t)d < _skins.size() && !_skins[(size_t)d].empty()
            && vi >= 0 && (size_t)vi < _skins[(size_t)d].size())
        {
            const SkinW& sw = _skins[(size_t)d][(size_t)vi];
            const size_t nb = _skinBones.size() / 6;
            const csmFloat32* A = (sw.b0 < nb) ? &_skinBones[(size_t)sw.b0 * 6] : NULL;
            const csmFloat32* B = (sw.b1 < nb) ? &_skinBones[(size_t)sw.b1 * 6] : NULL;
            const csmFloat32 ax = A ? A[0] * x + A[1] * y + A[4] : x;
            const csmFloat32 ay = A ? A[2] * x + A[3] * y + A[5] : y;

            if (sw.b0 == sw.b1 || sw.w <= 0.0f)
            {
                *ox = ax;
                *oy = ay;
                return;
            }
            const csmFloat32 bx = B ? B[0] * x + B[1] * y + B[4] : x;
            const csmFloat32 by = B ? B[2] * x + B[3] * y + B[5] : y;

            *ox = ax + (bx - ax) * sw.w;
            *oy = ay + (by - ay) * sw.w;
            return;
        }
        if (_limbRot.empty() || d < 0 || (size_t)d >= _limbRot.size())
        {
            *ox = x;
            *oy = y;
            return;
        }
        const LimbRot& r = _limbRot[(size_t)d];

        if (r.cosA == 1.0f && r.sinA == 0.0f)
        {
            *ox = x;
            *oy = y;
            return;
        }
        const csmFloat32 dx = x - r.px, dy = y - r.py;

        *ox = r.px + dx * r.cosA - dy * r.sinA;
        *oy = r.py + dx * r.sinA + dy * r.cosA;
    }
    csmBool _orderSorted;               // 已排序过一次（静态序复用）

    csmUint32 _lastVisibleCount;
    csmUint32 _lastRasterTriangles;
    csmUint64 _lastScanPixels;
    csmUint32 _maskWarnCount;
    int _rasterThreads;

    FrameScratch* _scratch;             // 每帧共享渲染数据（复用，不每帧分配）
    ThreadPool* _pool;                  // 持久线程池
    csmInt32 _poolThreads;              // 池总线程数（含主线程）
    volatile csmInt32 _nextBand;        // 行带动态分配游标（原子递增）

    MaskScratch* _masks;                // 掩码组表 + 各组缓冲（按模型/按帧懒分配）
    csmUint32 _maskBufferStride;        // 单组字节数（= W×H）
    csmInt32 _maskGroupCount;
    volatile csmInt32 _nextMaskGroup;   // 掩码组动态分配游标
};

}}}}

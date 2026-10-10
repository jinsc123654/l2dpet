/**
 * l2d_config.h — Live2D 桌宠统一配置（对齐 LVGL lv_conf.h 的心智）
 *
 * 两层结构：
 *   【Port】 平台兼容层——默认输出平台、各平台设备、硬件加速开关；
 *   【软件】 性能/渲染/交互/UI 的全部默认值与开关。
 *
 * 用法与优先级（CLI > 自定义配置 > 本文件默认）：
 *   1. 直接改本文件（仓库默认配置）；或
 *   2. 全量拷贝本文件到自己的目录改动，构建时指定路径：
 *        cmake -DL2D_CONFIG_FILE=/my/conf/l2d_config.h
 *   3. 差异覆盖（**只写要改的项**，其余继承默认）——自定义目录里的
 *      l2d_config.h 这样写（用 #include_next 拉默认值，GCC/Clang 支持）：
 *        #ifndef L2D_MY_CONFIG_H          // 注意 guard 不同名（避免挡住默认文件）
 *        #define L2D_MY_CONFIG_H
 *        #define L2D_DEFAULT_FPS      20
 *        #define L2D_UI_STATUS_BAR    0
 *        #include_next "l2d_config.h"     // 从下一搜索路径取默认（只补未定义项）
 *        #endif
 *   4. 运行时可被命令行参数覆盖（如 --fps=20 --port=x11）。
 *
 * 注意：L2D_ENABLE_RGA 只是"意愿"开关——平台是否真有 librga 由 CMake 检测
 * （未检出时该功能自动不可用，代码走纯软路径）。全部项均有 #ifndef 守卫。
 */

#ifndef L2D_CONFIG_H
#define L2D_CONFIG_H

/* ==================== Port：平台兼容层 ==================== */

/** 默认输出平台：auto | pet | glfwpet | x11 | sdl | headless
 *  auto = 依次探测 x11 → sdl → headless（首个可用者） */
#ifndef L2D_PORT_DEFAULT
#define L2D_PORT_DEFAULT            "auto"
#endif

/** 无设备平台的默认表面尺寸（headless / 窗口尺寸） */
#ifndef L2D_SCREEN_WIDTH
#define L2D_SCREEN_WIDTH            480
#endif
#ifndef L2D_SCREEN_HEIGHT
#define L2D_SCREEN_HEIGHT           800
#endif

/* ==================== 硬件加速开关 ==================== */

/** NEON SIMD：ARM NEON 单指令多数据，加速软渲染逐像素内核
 *  （双线性采样/着色/混合的向量化路径；整数语义与标量版逐位一致）。
 *  仅 ARM 平台生效（x86 等自动回退标量）。默认为 0（标量内核，实测更快：
 *  该负载是访存延迟受限，NEON 的拆装/长依赖链代价大于省下的指令数）。
 *  1 = 启用 NEON 内核（需 __ARM_NEON；x86 仍自动回退标量）。 */
#ifndef L2D_ENABLE_NEON
#define L2D_ENABLE_NEON             0
#endif

/* ==================== 性能与渲染 ==================== */

/** 最大帧率（port 层精确节流；0 = 不节流） */
/* 模型 profile 的"稳定目录"（可选）：模型目录里没有 l2d.profile.json 时，来这里找
 * <模型目录名>.profile.json；桌宠默认不用它（模型自带的 l2d.profile.json 优先，
 * 模型库在 ~/.local/share/l2dpet/models 下），留空 = 不查这个目录 */
#ifndef L2D_PROFILE_DIR
#define L2D_PROFILE_DIR             ""
#endif

#ifndef L2D_DEFAULT_FPS
#define L2D_DEFAULT_FPS             30
#endif

/** 光栅化线程数（0 = 自动 = CPU 核数；上限见 L2D_MAX_THREADS） */
#ifndef L2D_DEFAULT_THREADS
#define L2D_DEFAULT_THREADS         0
#endif
#ifndef L2D_MAX_THREADS
#define L2D_MAX_THREADS             4
#endif

/** 渲染分辨率比例（<1 降采样，上屏由 port 放大；无加速平台建议 1.0） */
#ifndef L2D_DEFAULT_SCALE
#define L2D_DEFAULT_SCALE           1.0f
#endif

/** 模型缩放（1.0 = 官方 layout 原始大小） */
#ifndef L2D_DEFAULT_ZOOM
#define L2D_DEFAULT_ZOOM            1.0f
#endif

/** 等比最大填充（按首帧顶点包围盒自动缩放；填充率留边距） */
#ifndef L2D_DEFAULT_AUTOFIT
#define L2D_DEFAULT_AUTOFIT         0
#endif
#ifndef L2D_AUTOFIT_FILL
#define L2D_AUTOFIT_FILL            0.95f
#endif

/** 纹理尺寸上限（像素，长边；超过则加载时 box 降采样。桌宠默认 768：
 *  2048² RGBA 一张 16MB，官方模型常有多张；768² 仅 2.25MB，桌面上画质几乎无感
 *  但换模型/多模型共存时的显存与加载时间省一大截（工具：tools/shrink_textures.py
 *  可以把纹理直接缩好入库）。
 *  0 = 不限制（按原始尺寸加载）） */
#ifndef L2D_TEXTURE_MAX_SIZE
#define L2D_TEXTURE_MAX_SIZE        768
#endif

/** 纹理透明预判摘要表（4×4 块 max-alpha；跳过全透明像素的采样） */
#ifndef L2D_ALPHA_SUMMARY
#define L2D_ALPHA_SUMMARY           1
#endif
#ifndef L2D_ALPHA_BLOCK_SHIFT
#define L2D_ALPHA_BLOCK_SHIFT       2   /* 块尺寸 = 2^shift（2 = 4×4） */
#endif

/* ==================== 交互 ==================== */

/** 触摸拖动跟随（眼睛/头/身体注视手指；抬起保持） */
#ifndef L2D_DEFAULT_FOLLOW
#define L2D_DEFAULT_FOLLOW          1
#endif

/** 点击（未拖动）切换表情 */
#ifndef L2D_DEFAULT_TAP_EXPR
#define L2D_DEFAULT_TAP_EXPR        1
#endif

/** 视线跟随参数表（标准 Live2D 参数名；模型缺哪个自动忽略——BongoCat/官方设计） */
#ifndef L2D_LOOK_ANGLE_X
#define L2D_LOOK_ANGLE_X            30.0f
#endif
#ifndef L2D_LOOK_ANGLE_Y
#define L2D_LOOK_ANGLE_Y            30.0f
#endif
#ifndef L2D_LOOK_ANGLE_Z
#define L2D_LOOK_ANGLE_Z            30.0f
#endif
#ifndef L2D_LOOK_BODY_ANGLE_X
#define L2D_LOOK_BODY_ANGLE_X       10.0f
#endif
#ifndef L2D_LOOK_EYEBALL_X
#define L2D_LOOK_EYEBALL_X          1.0f
#endif
#ifndef L2D_LOOK_EYEBALL_Y
#define L2D_LOOK_EYEBALL_Y          1.0f
#endif

/** 点击判定：按下到抬起的移动阈值（像素；超过即视为拖动不触发点击） */
#ifndef L2D_CLICK_MOVE_THRESHOLD
#define L2D_CLICK_MOVE_THRESHOLD    30
#endif

/** 表情切换冷却（秒；防连点） */
#ifndef L2D_TAP_EXPR_COOLDOWN
#define L2D_TAP_EXPR_COOLDOWN       0.35f
#endif

/* ==================== 屏幕 UI ==================== */

/** 顶部状态栏（CPU/RAM/FPS） */
#ifndef L2D_UI_STATUS_BAR
#define L2D_UI_STATUS_BAR           1
#endif
/** 底部缩放控制条（触摸拖动；StepPct=步进百分比、Min/Max=缩放范围） */
#ifndef L2D_UI_ZOOM_SLIDER
#define L2D_UI_ZOOM_SLIDER          1
#endif
#ifndef L2D_UI_SLIDER_MIN
#define L2D_UI_SLIDER_MIN           0.5f
#endif
#ifndef L2D_UI_SLIDER_MAX
#define L2D_UI_SLIDER_MAX           1.5f
#endif
#ifndef L2D_UI_SLIDER_STEP_PCT
#define L2D_UI_SLIDER_STEP_PCT      5
#endif
/** 跟随目标圆环（拖动中亮色/静止灰色） */
#ifndef L2D_UI_TARGET_RING
#define L2D_UI_TARGET_RING          1
#endif
#ifndef L2D_UI_RING_RADIUS
#define L2D_UI_RING_RADIUS          26.0f
#endif
#ifndef L2D_UI_RING_THICKNESS
#define L2D_UI_RING_THICKNESS       3.5f
#endif

/** UI 配色（0x00RRGGBB） */
#ifndef L2D_UI_COLOR_ACCENT
#define L2D_UI_COLOR_ACCENT         0x00ffe080  /* 状态栏文字 */
#endif
#ifndef L2D_UI_COLOR_RING_ACTIVE
#define L2D_UI_COLOR_RING_ACTIVE    0x00ffd060  /* 圆环-触摸中 */
#endif
#ifndef L2D_UI_COLOR_RING_IDLE
#define L2D_UI_COLOR_RING_IDLE      0x00708090  /* 圆环-静止 */
#endif
#ifndef L2D_UI_COLOR_SLIDER_TRACK
#define L2D_UI_COLOR_SLIDER_TRACK   0x00405060  /* 缩放条轨道 */
#endif
#ifndef L2D_UI_COLOR_SLIDER_FILL
#define L2D_UI_COLOR_SLIDER_FILL    0x004080ff  /* 缩放条已选段 */
#endif
#ifndef L2D_UI_COLOR_HANDLE
#define L2D_UI_COLOR_HANDLE         0x00ffffff  /* 缩放条把手 */
#endif

/* ==================== 模型库 ==================== */

/** 模型库目录（兜底）：其下每个子目录含 *.model3.json 即被扫描进切换列表
 *  （底部 ←/→ 箭头循环切换）。优先级：--model-dir > 位置参数所在目录 > 本项。
 *  桌宠正常用不到它——模型库是 ~/.local/share/l2dpet/models，由启动器/面板
 *  按当前模型显式传给播放器；留空 = 没有兜底目录。 */
#ifndef L2D_MODEL_DIR
#define L2D_MODEL_DIR               ""
#endif

/** 切换模型后名称提示条显示时长（秒；0 = 不显示） */
#ifndef L2D_UI_MODEL_TOAST_S
#define L2D_UI_MODEL_TOAST_S        1.6f
#endif

/* ==================== 运行默认 ==================== */

/** 默认播放帧数（0 = 一直播）与证据打印间隔 */
#ifndef L2D_DEFAULT_FRAMES
#define L2D_DEFAULT_FRAMES          180
#endif
#ifndef L2D_REPORT_EVERY
#define L2D_REPORT_EVERY            30
#endif

#endif /* L2D_CONFIG_H */

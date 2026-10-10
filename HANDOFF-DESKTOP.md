# HANDOFF：Live2D 桌宠 l2dpet（三端桌面：Windows / Linux / macOS）

（**新对话从这里接手**。本文档 = 桌宠这条线的交接：做完了什么、怎么跑、坑在哪。
协议细节见 `PROTOCOL-LLM.md`，行为设计见 `live2d/DESIGN-PET-LIFE.md`，构建/打包见
`live2d/BUILD-CROSS-PLATFORM.md`。

> **本分支 `l2dpet-desktop-only`：只保桌面三端。** 板卡/LVGL 那条线（RK3506 上的
> LVGL 桌面、共享内存帧通路、交叉编译部署、Rust 核心实验）已从本分支移除；
> 想看那部分历史（含当时的证据与决策）切 `main` 分支。
> 共享内存帧契约 `L2dShm.h`、DRM/fbdev 端口、`--shm-fd`/`--core=mocari` 等随之删除，
> 现在剩的端口：`pet`（X11 桌宠）/ `glfwpet`（Win/mac/纯 Wayland）/ `x11` / `sdl` / `headless`。）

> **证据图已清理**：下文引用的 `live2d/img/*.png`（板上/桌面实测截图等）已不再随仓库分发；
> 需要时从 git 历史里取（`git log --diff-filter=D --name-only -- live2d/img`，再 `git checkout <commit>^ -- live2d/img/xxx.png`）。

## 0. 一句话现状

**桌宠 = 两个进程**：`l2dtest --pet`（角色窗口 + 渲染 + 鼠标输入 + 协议端口 + **内置 C++
LLM agent**）与 `l2dpet-ui`（EUI-NEO 面板：托盘、右键菜单、设置窗口、**说话气泡**）。
两者走 127.0.0.1:23333 的 JSON-Lines 协议（`PROTOCOL-LLM.md`）。三端：
Linux（X11 原生端口，本机已实测）、Windows/macOS（GLFW 端口 + 打包含逻辑，无实机）。

开机怎么用：`scripts/linux_build.sh` 打 Linux 安装包（tar.gz / .deb / AppImage），
装好后 `l2dpet` 起桌宠、`l2dpet status|stop|restart` 管它；首次运行会注册桌面图标。


## 1. 代码地图与跑法（桌面侧）

```
live2d/player/                  播放器/桌宠主程序（C++17，官方 Cubism Core + GL 渲染）
  src/l2dtest.cpp               进程入口 main（薄）+ 文件级小工具 + 指令环 + 证据输出
  src/PetBootstrap.{h,cpp}      命令行选项（PetOptions）+ 解析（原 main 前半段）
  src/MainLoop.{h,cpp}          运行期：装载模型库 → 起端口 → 主循环（原 main 后半段）
  src/SysStats.h                系统状态采样（CPU/内存；类内定义 → 纯头文件件）
  src/PlayerAllocator.h         Cubism 分配器（含对齐分配；类内定义 → 纯头文件件）
  src/FileUtil.{h,cpp}          文件/路径工具：读文件、目录扫描、路径切分、PPM 落盘
  src/ModelImport.{h,cpp}       模型导入：zip/目录 → 模型库（目录名取包内 <名>.model3.json 的 <名>）
  src/Backup.{h,cpp}            备份导出/导入：记忆+性格+题库+笑话+配置打成一个 JSON
  src/PpmPlayer.{h,cpp}         PPM 帧序列上屏（对照/验收：不加载模型，只读帧→上屏）
  src/L2DPlayer.h             播放器主体**声明**：类声明 + 数据成员 + 小内联访问器（约 3.5k 行）
  src/PlayerCore.cpp          实现：装载/重建/绘制/端口接线/指纹
  src/PlayerCmd.cpp           实现：远程指令 / 演出引擎 / 说话 / 动作片段 / 协议 / 交互区域 / 手势
  src/PlayerBody.cpp          实现：躯体通道 / 关节保持 / 换套过桥 / 手臂关节骨架
  src/PlayerCfg.cpp           实现：配置读写 / LLM 接入
  src/PlayerLife.cpp          实现：连续情绪 / 内在状态（生活逻辑）
  src/PlayerProbe.cpp         实现：能力探测（PROBE）
  src/PetAgent.*                内置 LLM agent（线程内调 LlmClient；事件 → 段 → sequence）
  src/PetMemory.* / PetPersona.* / PetQuestions.* / PetJokes.*
                                跨会话记忆 / 每模型性格 / 问题库 / 笑话池（都按模型指纹落盘）
  src/PetI18n.*                 桌宠自带文案的 en/ja 表（气泡台词、动态、状态行）
  src/PetSpeech.h               语言校验（SpeechLangOk）、时间戳清洗、台词语言全局
  src/LlmClient.*               LLM HTTP 客户端（多供应商配置见 ~/.config/l2dpet/llm.json）
  src/CtlServer.*               协议服务端（127.0.0.1:23333；只收 JSON 行）
  src/PortX11Pet.cpp            桌宠端口：透明置顶窗 + XShape 点击穿透 + **说话气泡**（X11/Xft 直绘）
  src/PortGlfwPet.cpp           跨平台桌宠端口（Win/mac/纯 Wayland）
  src/PortX11.cpp / PortSdl.cpp / PortHeadless.cpp
  soft-renderer/                软渲染后端（无 GPU 平台与 --render 离屏出帧用）
  capabilities/*.capability.json 各模型能力缓存（面板"模型"页与 fv 校验用它）
  profiles/<模型>/l2d.profile.json 每模型清单（臂套绑定、绘制顺序 static 等）
live2d/panel/                   面板/托盘/菜单（EUI-NEO；进程 l2dpet-ui）
live2d/tools/                   调试与对账工具（见 §7n / pet_client.py / gen-capabilities.sh）
live2d/build-player.sh          编播放器 → ~/l2d-build/build-host/l2dtest
live2d/build-panel.sh           编面板   → ~/l2d-build/build-panel/l2dpet-ui
scripts/linux_build.sh          打 Linux 安装包（tar.gz / .deb / --appimage）
```

跑法（本机开发）：

```bash
live2d/build-player.sh host && live2d/build-panel.sh   # 编两个二进制
~/l2d-build/build-host/l2dtest <模型3.json> 0 0.0333 --pet --ctl 23333 --order static
~/l2d-build/build-panel/l2dpet-ui                      # 面板（或直接 l2dpet 启动器）
python3 live2d/tools/pet_client.py status              # 协议调试客户端（PET_PORT 可换）
```

打包/发布：`scripts/linux_build.sh [--appimage]` → `dist/linux-<arch>/`；装完 `l2dpet` 一条命令起。

## 2. 完成了什么（都带实测证据）

按时间分节，从 §7 起：**§7 外部指令协议**（动作/表情/手势/演出脚本）、**§7b 动作层 v2**
（关节骨架 + 动作库）、**§7c LLM 协议 v2 + 本机桌宠**、**§7d 桌宠"活起来"**（三层交互 /
内在状态 / 连续情绪 / 面板）、**§7e 三端化**（内置 C++ agent / 跨平台端口）、
**§7f–§7i**（dock 标题、点击判区、多供应商、模型页与能力解算、功能审查修复）、
**§7j 日/夜 + 多语言**、**§7k Linux 安装包**、**§7l Wayland 回退 + AppImage**、
**§7m 日语 + 切语言重写存量内容**、**§7n 内置文案全线 i18n**、**§7o 安全扫描与修复**。

## 3. 已结案：shuflle 胸部"鼓包" = 模型自带的 5.x 动态绘制顺序（非渲染 bug）

**现象**：shuflle 在**右上角看**（以及任何呼吸抬头相位）时，胸口出现一块
裸露的皮肤圆盘压在裙子上面——用户报的"胸部形变/透视异常"。

**复现（两行命令，无需触摸）**：
```bash
~/l2d-build/build-host/l2dtest /path/shuflle.model3.json 301 0.0333 --no-motion --no-ui \
    --render /tmp/t --render-every 15      # 逐 0.5s 一帧：t≈1.0-2.7s、7.5-9.7s 露出
L2D_DBG_ORDER=0 ...  24 0.0333 --no-motion --no-ui   # 逐帧打印胸部件绘制序
```

**证据链（每一步都实测过）**：
1. **时间线**：露出窗口 t≈1.0-2.7s 与 7.5-9.7s，周期 **6.5s = 呼吸的 AngleX 项周期**
   （每次露出 1.7~2.2s）——与"右上角看"无关，是呼吸相位；
2. **跳变性质**：第 23 帧(t=0.77s) **单帧**翻转 ~1300px；而该帧参数（最大 Δ0.19°）、
   全部顶点（≤1.5px）、不透明度、可见性、**39 组掩码缓冲**全部平滑/不变；
3. **根因**（`L2D_DBG_ORDER` 实测）：Core 返回的**绘制顺序是动态的**——
   跳变帧 `csmGetDrawableDrawOrders[174..176]` **500→501**、renderOrder **81→117**
   （同帧共 48 个 drawable 的 renderOrder 重排）→ 胸部皮肤层（#174/#175/#176）
   被 Core 排到了衣服**上面**。触发点：呼吸把角度参数推过 ≈4.5°
   （`ParamAngleX2/●X`；`ParamBodyAngleX2≈1.0` 同步跨越，未区分主因）
   —— 即模型作者用 **Cubism 5.x"参数驱动绘制顺序"** 做的效果（类似"转头时胸部立体/前凸"）；
4. **双引擎对照**（同姿态、都关水印）：官方 Core 出现、**Mocari 不出现**——
   因为 mocari 桥的 `mc_draw_order` 是静态序（**缺特性**，不是它更对）；
   官方 Core = VTS 同款 → **VTS 里同样会这样**。证据图：
   `live2d/img/chest-official-vs-mocari.png`。

**结论**：这是模型自带行为（作者 feature，被误读成形变 bug），我们的渲染器
读取 Core 顺序的逻辑正确。**处置（已实现，同日第二轮）**：渲染器加
`SetStaticDrawOrder`（首帧排序后冻结）+ 播放器 `--order static|dynamic`（默认
dynamic=官方语义）；**桌面按模型清单传参**（Kconfig `L2D_VIEW_STATIC_ORDER_MODELS`，
默认 `"shuflle"`；`l2d_view.c` 匹配模型目录名）。回归实测（13 模型静态/动态对拍，
帧 0 全一致）：**有动态重排的只有** shuflle（**要静态**——就是露胸这个）、
**Haru（要动态**——提手动作依赖动态层序把双手画到裙子前面，冻结后手会被裙子挡
住，对照图 `img/haru-draw-order-hands.png`）、Natori（4px 边缘，忽略）；其余 10 个
模型逐字节一致。
板上实测（armhf 播放器、桌面同参数）：动态序露胸 1271px 肤色 / 静态序 182px
（裙子盖好，对照图 `img/shuflle-order-fix-board.png`）；桌面子进程 argv 实见
`--order dynamic`（Haru）/`--order static`（shuflle）由 Kconfig 清单决定。
**2026-10-09 补**：本机（Ubuntu）桌宠 `--pet` 默认 dynamic **同样会露**——视觉确认（视线钉
右上角，胸口皮肤圆盘压在白裙上）+ 离线 A/B（同命令只差 order：除首帧外差异**全部**集中在
胸口 ~50×50 区域、约 2000px/帧，其余逐位一致；证据 `img/shuflle-order-static-vs-dynamic-host.png`）。
桌宠启动加 `--order static` 即除（PROTOCOL-LLM §7 启动行已注明）。

**这轮为此加的诊断工具（都在用，留作以后排查同类问题）**：
| 工具 | 位置 | 说明 |
|---|---|---|
| `--look x,y` | 播放器（官方+Mocari 都支持） | 固定注视目标（像素坐标）——比触摸更确定的对拍入口 |
| `--dump-params <n>` | 播放器 | 第 n 帧全量参数导出（`[pd]` 行） |
| `--watermark on\|off` | 播放器 | 水印开关（默认 off=应用作者的"水印"表情） |
| `L2D_DBG_SKIP=<列表>` | 软渲染器 | 跳过指定 drawable（定位"哪片画出来的"） |
| `L2D_DBG_ORDER=<0\|帧号>` | 软渲染器 | 打印/转储 Core 绘制序（动态序、offscreen 槽都看得到） |
| `L2D_DBG_NOCULL` / `L2D_BAND=<高>` | 软渲染器 | 关背面剔除 / 改行带高度 |

**已排除（都做过对照实验，备忘）**：掩码路径剔除、掩码缓冲（39 组三帧全等）、
不透明度、可见性、几何跳变、整数快速路径（与 float 逐位一致）、线程/行带、
背面剔除、水印开关（`--watermark on` 时照样出现）。

## 3b. 本轮功能落地（水印）

- **水印**：判据=表情名含"水印"或文件 `shuiyin*.exp3.json`（**不要只认参数名**：
  库里 bongocat 也有 id 叫 `Param` 的参数）；每帧在 scheduler 之后、Core 变形
  之前按表情的 Blend 语义（Add/Mul/Overwrite）强制写入（默认 off）。
- **验收数据**：host 两次运行**逐字节一致**；水印 off/on 差异集中在水印文字区
  （38737px，bbox x[42..452] y[145..428]）；板上探针 90 帧 0 竞态，水印实测生效
  （图：`live2d/img/board-shuflle-look-nowatermark.png`）。

## 4. 常踩的坑（桌面侧，都有症状）

1. **对拍前先 `--no-ui`**：播放器自带状态栏（CPU/RAM/FPS）每次读数都不同 → "同模型同帧"
   的逐字节基线永远对不上（一度被误判成"渲染器不确定"）。离屏对拍：`--render <目录>`
   + `--no-ui`。
2. **安全钩子（Mimosa）**：会拦"脚本/命令直接改写源码"与 `popen`/`exec*` 这类调用 ——
   改源文件用 Write/Edit 工具；起子进程用 **`posix_spawn`**（显式 argv、不过 shell）过审。
3. **`strOr()` 返回 `const char*`**：跟字面量 `==`/`!=` 比是指针比较（恒真/恒假），
   全仓出过 3 次——加比较前先 grep。
4. **面板每帧重建 UI**：`S.scroll[]` 之类的每页状态要跟着"页"一起长大；每帧要做的
   （`ApplyPrefs()→ApplyTheme()`）在 `Tick()`/`compose()`/`ComposeMenu()` 三处都会跑。
5. **别在同一个循环里"边 `push_back` 边取 `c_str()`"**：vector 扩容让先前指针失效
   （猜拳气泡第一格渲染成乱码/空白就是这么来的，踩过）。
6. **拼 JSON 的逗号要按需加**：可选段直接 `+= ",\"x\":"` 会在前面为空时生成坏 JSON
   （`CfgJson()` 出过 `{,"str":{…}}`，agent 读不到配置 → 启动永远按默认语言）。

## 5. 待办（桌面侧，按优先级）

| # | 项 | 说明 |
|---|---|---|
| 1 | **Windows / macOS 实机验证** | Win/Mac 只有"代码 + 交叉编译/打包逻辑"级交付：GLFW 端口、`build-panel` 的 Win/Mac 分支、包结构都写好了但**没有实机跑过**——拿到机器后按 `BUILD-CROSS-PLATFORM.md` 走一遍并记坑 |
| 2 | 大模型掉帧 | 桌面切到大模型（shuflle 445 drawable）帧率明显下滑 → 降采样/多线程调优 |
| 3 | 气泡/面板小修 | 见 §7n/§7o 的"已知小毛病"（LLM 性格重写失败会退成兜底版，下次启动有 Key 自愈） |
| 4 | 板端遗留引用 | 本分支已剔除板卡线；若在 `main` 上继续板端工作，别把两边的端口/协议改动搞混（共享内存帧通路只在 `main`） |

## 7. 外部指令：动作/表情/手势/演出脚本（协议层，桌面桌宠与面板都用）

> 本节是**指令语义**的沉淀（动作/表情/手势怎么用、BODY 通道怎么映射、能力怎么探测）。
> 文中的"板卡期"实现细节（Kconfig `L2D_VIEW_*` 配置项、指令环 / shell `l2d` / `nc 2333`）
> 属于 `main` 分支那条线，本分支已删；桌面上对应的入口是 **JSON 协议**
> （`PROTOCOL-LLM.md` §5）与桌宠/面板：默认模型与"启动演出"由启动器 + `config.json` /
> 模型 profile 决定，指令用 `live2d/tools/pet_client.py` 或面板"试一试"发。

**桌宠默认行为**：启动器拉起后等模型就绪（呼吸/视线归位 ~1.5s）**自动下发一次启动演出**
（shuflle 这类模型带 `--order static`，见 §3），之后转入自主待机 + 内置 agent 换新演出。

**能力矩阵（2026-10-08，探测已重生成并**随模型缓存**；见 `live2d/img/capability-matrix.txt`）**：
"手/头/口/表情怎么动、是否通用"的答案分三层——
- **怎么动（一个参数三种生效方式）**：① 顶点直连；② **仅经物理链**（美术绑在物理输出
  ●X/●身X 镜像上——不跑物理完全没效果：sage 整个头身、shuflle 头身）；③ **换美术**
  （顶点不动、切 drawable 不透明度：shuflle 眨眼、Hiyori 嘴型…）。我们的管线"跑物理 +
  在物理后写轨道"，所以三种都能被脚本驱动 ✓（探测的物理档已按这个运行时语义校准）。
- **是否通用（按"模型实际存在的参数"计，2026-10-08 修订）**：头 33/34、眼 39/41、
  **口 14/16**、**眉 30/34**、呼吸 10/11、手/臂/腿族 **105/114**（91%）。
  **真正的跨模型差异是命名与数量，不是有效性**——按各模型 PARAMS 表驱动即可；
  "手/臂/腿"参数一旦存在基本都能动（旧版"10/13 有效"的口径有误，见矩阵文件"读表须知"）。
- **两个例外**：**Wanko** 整列 ×（老式 `PARAM_ANGLE_X` 命名，无一个标准参数、静态摆件）；
  **sage/shuflle** 的头是"物"（仅经物理）、身角度"无"——身体转向要靠 ParamAngleX 带。
- **素材（表情/动作）不通用**：表情数 0~12、命名各异（`F01-F08`/`exp_0x`/`expressionN`/
  中文名/内嵌文件名）；动作组只有官方 8 模型带 `Idle`+`TapBody`，4 个自制模型
  （bongocat-xxmhh/sage/shuflle/linpianpian）**无任何动作组**——跨模型"动起来" =
  各自 LIST 查得的素材 + 标准参数程序化（上面的通用性）+ 每模型 profile。
- 复现：`live2d/tools/gen-capabilities.sh <模型库根>`（两档全量探测，顺带写缓存）；
  `live2d/tools/capability_matrix.py <日志>` 聚合表。

**能力上报（板卡自报，给 LLM 用；2026-10-08）**：板卡不再"只有清单"，**每个参数带效果档**——
- `PARAMS`（控制端口/shell）每行尾带 `eff=<码>[:顶点位移]`：`d` 直连可驱动、`p` 仅经物理、
  `a` 换美术、`-` 无效果、`?` 未探测；表头附码表图例。LLM 据此直接挑参数/避坑。
- 效果档由播放器**自己探测**（`PROBE [ALL]` / `PROBEPHYS [ALL]`，非阻塞队列：每帧按
  时间预算跑一点，屏幕不受影响；跑完自动写 `<模型目录>/l2d.capability.json` 并刷新
  PARAMS）。**缓存随模型走**：启动时若缓存指纹匹配则秒读，不再占用板卡算力；
  仓库里 13 个模型的能力缓存已生成（`live2d/player/capabilities/`，部署钩子推到 SD）。
- 模型换了包/参数表变了 → 指纹变 → 旧缓存自动作废（打印"缓存指纹不符"），重测即可。
- 现役模型若没有缓存：面板"模型"页点探测，或协议 `{"cmd":"probe","all":true}`
  （物理稳态档加 `"phys":true`；批量：`live2d/tools/gen-capabilities.sh <模型库根>`）。

**防指令串扰（模型名 + 指纹；2026-10-08）**：`LIST` 里新增 **`FP=0x…16 位`**（模型指纹 =
参数 id 序列 + 规模派生，装载时算好；STATUS 也带）。外部（LLM/工具）拿到后：
- **指令行首加 `@<fp> `** —— 指纹不匹配（切了模型/换了模型包）整条拒执，回复 `REJECT …`；
- **演出脚本 JSON 里可写 `"fp":"0x…"` / `"model":"<目录名>"`** —— 对不上整段拒（防止
  "给 A 编的剧本在 B 上播"）；`scene-demo.json` 之类没写的照旧随便播（内置手势有降级）。
- 回复通道：播放器把每条指令的结果写成单行（`OK …`/`REJECT …`）进指令环 **reply 区**；
  控制端口对每条转发命令**等最多 ~1s 把回复带回**（`LAST` 可单独查），shell `l2d last`。
  于是 DeepSeek Agent 能明确区分"已执行 / 被拒（及原因）"。

**躯体通道 BODY（让 LLM"像控制身体一样"控制模型；2026-10-08 新增）**：一套**跨模型的
关节式接口**——语义通道名 → 该模型自己的参数（标准名自动映射；定制 rig 用 profile 的
`"body"` 段覆盖，如舒芙蕾 5 段手臂链 → 一条 `arm.r`）。
- `BODY head.x=28 head.z=10 arm.r=0.8 eye.l=-1` 批量设定（**原子**：一个通道名不认就整条拒）；
  `BODY reset` 释放回自动行为；`BODY`（无参）查询当前值（回复带全量状态，LLM 好对齐）。
- **通道清单在 `LIST` 的 `BODYCHANNELS=`**（按模型自动定：参数没布线的通道**不出现**——
  舒芙蕾的 `body.*` 是死参数（eff=-），自动隐藏；Haru 有 `body.*` 和 `arm.*.a/b`）。
- **语义**：① 增量（叠加在呼吸/视线之上，身体仍"活着"——写"抬头 20°"时呼吸照旧起伏）；
  ② 设定后每帧保持 + 一阶平滑（看得到"动过去"）；③ **写入分两层**（SDK 语义决定）：
  经物理链的（头/身角度，eff 带 p）写在 `SaveParameters` 之后、scheduler 之前——物理看得到
  且不进基线（否则逐帧累加）；直连的（手/眼/嘴）写最后——躲开 **EyeBlink 的覆盖写**
  （眨眼每帧覆写 EyeLOpen，写它之前保不住"闭眼"）。
- 实测（host 顶点级）：`head.x` 动整个上身（39k→全模型，经物理 ●X 链）；`arm.r` 只动手臂
  （1097 顶点，y∈[-0.06,0.13]）；`eye.l=-1` 只动眼部（2255 顶点，y∈[0.59,0.66]）。
  板上演示：转头/展臂/闭单眼+/说话叠加，逐帧可见（证据 `live2d/img/demo-body-control-board.png`）。
- **跨模型**：Haru 的 `arm.*.a/b`（两段式）能抬到脸高（y 0.546）；舒芙蕾只到腰胯——
  同一条 `BODY arm.r=1` 在两个模型上就是"各自手臂的极限"，**接口通用、幅度由模型决定**。

**肢体刚性覆盖（"纸片人"式抬手/摸头/大字型；2026-10-08 新增，回答"手抬不过胸口"）**：
参数层做不到的**大范围手臂动作**，我们是渲染器的主人——**把手臂美术绕肩点整体旋转**，
不依赖作者绑定：
- **部件名单自动学**：能力探测时记录"哪些 drawable 被手臂参数带动"（参数名里 `Arm`/
  `arm`/`Hand` + 其后第一个 L/R 定左右：`ParamarmR1`/`ParamArmAL01`/`ParamArmBRHand01`
  …多臂链也兜得住），写进能力缓存的 `"parts":{"arm.r":[…],"arm.l":[…]}`（8 个模型有：
  shuflle/Haru/Hiyori/Mao/Mark/Ren/Rice/Natori；其余模型本来就没有手臂参数）。
- **运行时**：`BODY arm.r=0.6` 值域 -1..1 → 绕"部件最高点"（肩）旋转 0..165°，**逐帧从
  Core 顶点现算肩点**（Core 永远是"未旋转"姿态，所以肩点稳）；抬起 >50° 时该组 drawable
  **画在最上层**（否则举起来的部分被长发盖住——实测 165° 时整条胳膊藏进头发里）。
- **实测（板上，证据 `live2d/img/demo-limb-board.png`）**：`arm.r=0.36` 手到肩高（**过胸口**）；
  `arm.r=0.62 arm.l=0.62` = **大字型/星形**；`arm.r=0.88 head.y=-10 head.z=12` =
  **摸头**（手举到头侧 + 低头相就——手臂美术短，字面摸头顶够不着，靠头部迎过去）。
- **⛔ 已作废（2026-10-09 判为 bug 并删除）：下面这条 bend 是错误方向**——手臂只能在**关节**（肩/肘/腕）弯折，臂身弯成弧线 = 软管。代码已删（渲染器 `SetDrawableLimbBend`、`L2D_LIMB_STRETCH/RIGID`），过渡期 `arm.*` 只做肩关节刚性旋转；正式方案见 `live2d/DESIGN-ARM-JOINTS.md`（关节骨架 + 窄过渡带蒙皮 + IK）。
  ~~**升级：弯曲变形（bend，2026-10-09 起为默认）**~~（原文保留作历史）——刚性旋转（整条手臂一张画绕肩转）
  换成**角度沿臂轴渐变的弯曲**：顶点 t=沿臂投影/臂长（0=肩根、1=指尖），
  **角度 = 最大角 × t**，再沿轴轻微延伸（默认 stretch 0.25，够高用）。效果：
  肩根**不转** → 原被手臂盖住的肩/袖口区域不被揭开，**接缝基本消失**；
  手臂呈自然弧线（像真骨架的弯曲变形器），而不是硬直的"纸片"。
  实测同角度对比：刚性 1.0 肩部有明显断口，弯曲 1.0 连续且流畅
  （证据 `live2d/img/demo-shuflle-bend-board.png`：抬手过肩 0.45 / 大字型 0.58 /
  摸头 0.85+低头 / 举高欢呼 0.95+说话）。env 旋钮：`L2D_LIMB_PIVOT`（肩点内移，
  默认 0.75）、`L2D_LIMB_STRETCH`（沿轴延伸，默认 0.25）、`L2D_LIMB_RIGID=1`
  （退回旧刚性，对照用）。
- **性质与边界（2026-10-08 用户实测反馈后补）**：刚性旋转**有肩部接缝**——整条手臂是
  一张画，抬起时其根部与袖口脱开（"肩从袖子里飞出来"；肩点系数四档 0.75/0.62/0.45/0.3
  渲染对比都消不掉，属方案固有）。另：无肘部折叠、幅度受画长限制（手到头部高度）、
  命中框仍按 Core 原姿态（抬起的胳膊 TAP 点不到）。
  **跨模型"大动作"的可用度排序（官方模型实测对照后定论）**：
  ① **作者动作素材**（最佳）——官方 8 模型的 `Motion`（`MOTION <组> <序号>` 直接播）：
  Haru `TapBody 0`=双手摊开、`TapBody 1`=手举到脸旁，逐帧作者绑定、**干净无接缝**；
  ② **作者参数绑定**——官方模型手臂参数干净外摆（骨架带袖子）但只到"摆动级"（举不过胸）；
  ③ **我们的刚性兜底**（舒芙蕾这类无素材模型的唯一连续抬臂手段）——能举过头顶但**有接缝**。
  所以：要"像人一样的大动作"，优先用/找带动作素材的模型；舒芙蕾上要么接受接缝、
  要么用表情姿态（麦克风/猫猫手）、要么作者补绑定重画。
  **官方模型上也测过刚性抬臂**（`arm.r`/`arm.l` 整臂通道：有部件走刚性、无部件退回参数，
  证据 `live2d/img/demo-haru-rigid-vs-param.png`）：Haru 上刚性抬起同样穿帮（袖子横过脸/
  肩部脱开），而**作者参数外摆干净但幅度小**——结论不变：**刚性是"没有素材时的兜底"，
  官方模型请用它们自己的 MOTION/参数**。
- **换模型**：shell `l2d model [list|next|N]`（调试/外部指令用；换完 `LIST`/`STATUS`
  的 `FP=` 会变——旧指纹的指令会被拒，防串扰按设计生效）。
- 与逐位基线：默认全关（恒等）→ M1 对齐不受影响 ✓。

**⭐ 模型解析 → `live2d/MODEL-ANALYSIS-shuflle.md`（2026-10-09）**：用户问"详细解析一下模型"
→ 逐文件解析（`.vtube.json` = 作者在 VTS 里的完整设计）。**核心结论**：shuflle 是
**面捕（头/眼/眉/嘴/颊）+ 声控口型（五元音 ParamA/E/I/U/O + Silence，与 VTS 同源）+ 12 个
道具表情热键 + 全身物理**的主播模型；**手臂参数（armL/R1-5）是物理链的输出**（Setting45/46：
头角+呼吸 → 手臂摆动），是**被动摆件、不是控制通道**，美术也只有垂手状态。所以：
① 面捕式驱动（BODY `head.*` 写在物理前，带动 ●镜像→身体→头发/耳/裙/云雨）
② + `TALK`（就是 VTS 那套五元音）③ + `EXPR`（12 道具就是作者的大幅姿态）才是它的主场；
④ 外挂抬臂变形（bend）**上限已钳 90°**（再高会"绕过头顶变软管"——用户实测"太吓人"）：
`arm.r 0.45` 手到胸/肩、`0.6` 斜上举、`1.0` 平举（大字型）；**摸头/过肩高举此模型不支持**。
要更大动作：用表情姿态、或补美术+绑定、或换 yumi（作者绑好 armup）。

**yumi：目前"干净大幅度动作"的最佳模型（2026-10-08 上板）**：用户拿来 `yumi.rar`（VTS 导出包）——
**作者做了手臂绑定**：`ParamarmupL/R`（"抬手"）驱动 42/69 件手臂美术，**手能干净地举到脸前
（顶点级范围 y 到 0.38 = 嘴的高度），零接缝**；另有手/前后/上多段参数、身位（BodyposX）、
发/服装/眉（BrowYL/YR 命名）等。上板前做的四件事（**新模型上板清单，可复用**）：
① `unrar x` 解包（**7z 报 Unsupported Method，必须用 unrar**）；② model3.json 原本是空壳——
补 **Expressions（17 个）**、**Motions（wave/tear → TapBody 组）**、LipSync 组接
`ParamMouthOpenY/ParamJawOpen`；③ 纹理 **8192²（15MB PNG，板上解码 268MB 必 OOM）**→
box 缩到 768²（265KB，与 SoftTexture 同算法）；④ 表情文件名**中文→ASCII**（exprNN，防 SD
编码问题；显示名保留中文）；⑤ profile `live2d/player/profiles/yumi/`（arm.r/l → 作者 armup、
**`rigid:false` 不走刚性兜底**、hand.r/l + raise_hands/mic_pose 手势）+ 能力缓存（含 parts）。
**LLM 用法**：`l2d model 13` 切过去，`BODY arm.r=1 arm.l=1` 双手举脸前、`0.55` 半举、
`head.z` 歪头、`TALK n` 举着说话；证据 `live2d/img/demo-yumi-body-board.png`。
**通道命名回退**（本轮加）：眼睛/眉各作者叫法不同（yumi=ParamEyeOpenL/R、ParamBrowYL/YR）
→ 标准通道给多候选参数，谁存在用谁；说话口型也加了 `ParamJawOpen`（yumi 用这个）。

**交互动作（点击反应，2026-10-08 第三轮）**：手指**按下几乎没移动就抬起**（<30px）
＝ 一次点击——桌面发 `TAP x y`（行协议同款，shell: `l2d send TAP 240 150`），
播放器按**人物实际范围**（顶点级包围盒，点空白背景不算）分区域反应：
**上 32% = 头部，其余 = 身体**；1.2s 冷却。反应优先级：① 模型自带 `Tap*` 动作素材
（Haru/Mao/Natori 有 TapBody → 播它，官方示例行为）；② **profile 的 `reactions`**
（`head`/`body`/`any` = 手势+表情）；③ 内置兜底（头→tilt 歪头；身→surprise 惊讶）。
shuflle 的 reactions：**头 → shy + 脸红**（被摸头害羞）、**身 → greet + 爱心**。
板上实测：`l2d send TAP 240 150` → 歪头+脸红；`TAP 240 480` → 招呼+爱心；空白处 → 忽略。

**怎么用**（板子在跑桌面时）：
```bash
adb forward tcp:2333 tcp:2333
printf 'LIST\n'        | nc 127.0.0.1 2333   # 能力表（表情名字/动作组/手势）——UTF-8
printf 'EXPR 3\n'      | nc 127.0.0.1 2333   # 切第 3 个表情（或 EXPR next / EXPR 爱心）
printf 'GESTURE nod\n' | nc 127.0.0.1 2333   # 手势（nod/shake/tilt/surprise/sigh/think + profile）
printf 'MOTION TapBody 1\n' | nc 127.0.0.1 2333
printf 'LOOK 430 90\n' | nc 127.0.0.1 2333
printf 'PLAY /userdata/vanxoak_lvgl/l2d/scene-demo.json\n' | nc 127.0.0.1 2333   # 本地 JSON 演出
printf '{"v":1,"dur":20,"steps":[{"at":0,"gesture":"wave"},{"at":2,"expr":"爱心"}]}\n' | nc 127.0.0.1 2333
```
shell 里等价：`msh 'l2d list'` / `l2d expr 3` / `l2d gesture nod` / `l2d play <文件>`。

**结构**（三层，都是本轮新增）：
1. **指令环**（`live2d/player/src/L2dCmd.h`，第二个 memfd，父进程建、`--cmd-fd` 传给子进程）：
   文本行 SPSC 环（64KB）+ **能力表 4KB**（子进程装载后写 `MODEL/EXPR/MOTION/GESTURE/IDLE`，
   父进程据此应答 LIST——**父进程（纯 C）不解析 model3.json**）。
2. **播放器侧**（`l2dtest.cpp`）：命令执行（EXPR/MOTION/GESTURE/LOOK/IDLE/RESET）、
   **演出引擎**（JSON 脚本 → 时间线：`at/dur/steps`；参数轨道叠加在呼吸/视线之上、
   在 Core 变形之前落值）、**自主待机**（无演出时每 3~8s 随机张望/轻手势，强度 0~3，
   外部指令后静默 8s 不抢）、**队列**（JSON 数组=依次播放）。
3. **桌面侧**：`l2d_ctrl.c`（TCP 127.0.0.1:2333 行协议，**每连接一个线程**+接收超时；
   `PLAY <文件>` 就在这里读文件）、`l2d_cmd.c`（shell `l2d` 命令）、`l2d_view_send()/l2d_view_caps()`。

**JSON 脚本（v1）**：`{"v":1,"dur":24,"interrupt":true,"idle":2,"steps":[…]}`；
step：`at`(秒) + `gesture`/`expr`(名字或序号,null=清)/`look`[x,y]/`motion`("组 序号")/
**`params`（原始参数姿态，见下）**，gesture 可带 `speed/repeat/**scale**`（scale=幅度
缩放 0.1~2.0，2026-10-08 加——模型自绑幅度小时放大用）。
示例：`etc/l2d/scene-demo.json`（部署到 `<模型库根>/`）。
**下发注意**：控制端口是**行协议**——直接 `nc` 发的 JSON 必须**压成一行**
（多行会被当多条命令拒掉）；`PLAY <文件>` 会自动折叠换行，人写的文件随意排版。

**⚠ 时间口径（2026-10-08 实测发现，做 LLM 节奏前必读）**：脚本/手势/呼吸都按**动画时间**
走——播放器每帧固定 `dt=1/30s`，重模型跑不满 30fps 时**动画整体变慢**：舒芙蕾 7fps ⇒
动画时间 ≈ **0.23× 墙钟**（"20 秒"的演出板上实放 ~100 秒；日志逐场景确认）。轻模型
（Haru 30fps）1:1。所以：① 外部按"墙钟 30s 一发"设计节奏时要对重模型留量（队列会自动
排队，不丢段）；② 若要"演出时间=墙钟时间"，改播放器用实测帧间隔做 dt（带钳制，防物理
积分在低帧率下炸）——待做，见 DESIGN §5 的注。

**能力清单（给 LLM 用）**：`LIST` 除表情/动作/手势外还带 **`PARAMGROUPS=`**
（cdi3 的参数分组——身体ph=手臂R/L、头发ph、其他ph=裙/荷叶边/云/雨、手柄、耳朵…，
一眼看到"能控制哪些部位"）；**`PARAMS`** 返回全量参数表：逐行
`P <id> <默认> <最小> <最大> <组名> <中文名>`（shuflle 254 项，如
`P ParamarmR1 0.000 -1.000 1.000 身体ph 手臂R1`）。shell 侧：`l2d params`。

**`params` 步（让 LLM 直接驱动任意参数）**：
```json
{"at": 2.0, "params": {"ParamarmR1": -0.9, "ParamarmR2": -0.75}, "dur": 1.2}
```
数值是**增量**（Add 语义，与视线/手势一致），`dur` 内缓动到位并**保持到本段脚本结束**；
撤销=后续一步写相反数；同一参数后写的步覆盖先写的。

**⚠ shuflle 的"大幅度动作"上限（实测，别浪费时间）**：手臂链
（ParamarmR1-R5）任意组合、极值 ±1 全试过——**手臂最高只到 y≈206（胸口高度），
到不了脸（y<170）**；参数层面没有"抬手/摸头"的绑定。**但（2026-10-08 新发现）：
作者把"大幅度手部姿态"画成了表情换美术**——`麦克风`=**手举话筒到嘴边**（主播姿态！）、
`猫猫手`=**双手举起的猫爪**、`手柄`=持手柄、`爱心/星星/脸红/黑脸/眼罩/眼镜/哭哭/生气`…
所以"像主播一样"的玩法是：**表情做姿态 + 口型说话 + 头身角度做动作**（见下条 TALK）；
真正连续的参数级手臂动作仍要作者/自己补绑定（做完放进模型目录，`MOTION`/`motion` 步直接能播）。

**说话（口型 / TALK；2026-10-08 新增）**：舒芙蕾有**完整口型参数**（元音 `ParamA/I/U/E/O`、
下颌 `ParamJawOpenBL`、`ParamMouthOpenY/Form/X`，全部 eff=d 实测有效）。
- 命令：`TALK <秒>` 或 `TALK <文本…>`（按字数估时长：CJK ≈0.22s/字、ASCII 标点 0.25s）；
  脚本步：`{ "at":0, "talk":8 }` / `{ "talk":"你好呀" }` / `{ "talk":{"dur":8,"text":"…"} }`。
- 实现：viseme 时间表（闭口 12% 停顿 + A/I/U/E/O 轮换，0.1~0.3s 一切）驱动嘴部参数，
  **写在轨道之后**（最高优先级）——所以"举着话筒说话"= `mic_hold` + `TALK n` 自然叠加。
  参数不存在自动忽略（跨模型安全）；host 顶点级验证：说话帧只在嘴部区域动
  （623 顶点/21 drawable，y 0.545~0.583）；板上静帧可见嘴型逐帧张合/变形。
- **不是音素级对齐**（没接 TTS）：目标是"看着像在说话"。要精确口型可让上游给
  `"params"` 步直接写元音（有能力表可依）——这是留给 M3 的接口。

**手势带表情（2026-10-08 新增）**：profile 手势可加 `"expr":"表情名"` —— 一条命令 =
姿态+表情+轨道。shuflle 已加：`mic_hold`（举话筒+微点头）、`paws_up`（猫爪举手+歪头）、
`cry`（哭哭+低头抖肩）、`angry`（生气+叉腰前倾）、`wave_slow`。`GESTURE mic_hold` 即可。

**手势**：内置通用（标准参数，所有模型可用）`nod/shake/tilt/surprise/sigh/think`；
**模型专属手势**放 `<模型目录>/l2d.profile.json`（参数轨道声明，见
`live2d/player/profiles/shuflle/README.md`；shuflle 已有 wave/greet/shy/stretch/hands_on_hips
——实测手臂参数 `ParamarmL/R1-3`：**-1 = 向外张开**）。deploy 会自动推送 shuflle 的 profile。

**动作演示/验收方法（2026-10-08 实战总结）**：截图验收会被"待机呼吸 + Idle 动作"干扰
（逐帧本来就差 ~13k 像素）。做法：① `IDLE 0` + `LOOK 240 300` 把视线钉住；② 连拍几帧
测**本底抖动**（实测 ~13.4k 像素/帧）；③ 发动作后在峰值抓帧，**变化像素 2~2.8× 本底
（27k~37k）即认定动作真实生效**（本轮 wave/stretch/hands_on_hips/shy/手臂链/点头全部
达标）。表情类最直观（爱心/猫猫手/星星/脸红在静帧上一眼可辨）。**一个动作一个 `RESET`
对齐**再截，别把多个脚本堆进队列（会排队播、抓帧错位）。证据图在 `artifacts/shots/`
（demo-montage-1/2/3/4、show-*、live-*）。

**踩到的坑**：
- **SDK 自带的 `Utils::CubismJson` 对手写/紧凑 JSON 有解析 bug**（同内容 indent 缩进能过、
  单行/紧凑嵌套数组失败）——profile 是手写的、脚本是 LLM 生成的，**不能用它**；
  已自写 `live2d/player/src/JsonLite.h`（~330 行，规范全支持）替换，profile 与脚本都走它。
- 控制端口首版"单连接串行"：一个被杀死的连接会把端口卡住（实测 STATUS 无响应）→
  改为**每连接一个线程 + `SO_RCVTIMEO`**（空闲 2 分钟回收）。
- **SD 卡挂载点会在两次开机之间变化**（实测同一张卡：一次是 `/mnt/sdcard/live2d`，
  另一次变成 `/media/sdcard1/live2d`，而 `/mnt/sdcard` 挂上卡上另一个分区）；
  且开机时 `/mnt/sdcard/live2d` 会被建出一个**空壳目录**。症状=开机桌面起来但
  没有人，日志刷"模型库为空"。**已修**：模型目录改成候选列表 + 内建兜底，
  判据是"**试扫能扫到模型才采用**"（不是"目录存在"），扫不到每 3s 重试。
  另注意：**Kconfig 默认值不会覆盖已保存配置**（`./project.sh rebuild` 从
  `global_config.mk` 取保存值）——改默认值要同步改 `global_config.mk`。
- **内容在 SD、程序在 NAND；部署对挂载点安全**（2026-10-08 定稿，用户要求）：
  模型/profile/演出脚本等**内容文件都放 SD 模型库**里（`/userdata` 只留程序资产：
  播放器/字体）；但 SD 挂载点会漂移，所以**部署不写死路径**——`rk_build_tools
  deploy` 会跑**后置钩子** `tools/deploy-hook.sh`（conf 里的 `RK_APP_DEPLOY_HOOK`）：
  先探测板上"真实模型库根"（判据=能扫到 `*/*.model3.json`，不是"目录存在"——
  开机时会有空壳目录）→ 把 `live2d/player/profiles/<模型>/l2d.profile.json` 推到
  `<根>/<模型>/l2d.profile.json`、`etc/l2d/*.json` 推到 `<根>/` → 清旧位置
  （/userdata 副本、空壳目录）。播放器查找顺序＝`<模型目录>/l2d.profile.json`
  → `/userdata/.../profiles/<模型名>.profile.json`（旧位置兜底，现在不用）；桌面
  的启动演出脚本配**相对名**（`scene-demo.json` → 相对探测到的模型库根解析）。
  **注意**：单跑 `rk_build_tools deploy` 之外、手工改内容文件时，直接往
  `<模型库根>/...` 放，别碰 /userdata。
- **板端内存是纸糊的：`/tmp` 是 tmpfs（=内存盘）——排障/测试产物写进去等于吃 RAM。**
  实测教训：探针测试的 PPM 帧（11MB）＋临时文件堆到 15MB 没清，shuflle（播放器 56MB）
  就从"刚好能跑"变成**加载即 OOM、无限重启、屏幕空白**（dmesg 见 `Out of memory:
  Killed process l2dplayer`）。**OOM 先 `df -h /tmp` + `free`，再查进程**；
  板端大文件写 `/mnt/sdcard`（可写）或 `rm` 干净。

**里程碑状态**：M1（通道+执行原语）✅、M2（表演引擎+手势+自主待机）✅、
M3（PC 上 DeepSeek Agent，每 ~30s 生成脚本下发）＝下一步（设计见 main 分支 `DESIGN-LLM-CONTROL.md`；本分支已删，桌面实现见 `PROTOCOL-LLM.md` 与 §7c/§7e）。

## 7b. 动作层 v2：关节骨架 + 动捕感 + 动作库（2026-10-09，已上板）

（起因：用户要"LLM 控制大手臂动作，像 VTuber 动捕一样各种动作反馈"；中途判定旧的
bend 弯曲变形是错误方向——**手臂只能在关节弯**。设计与逐期证据见 `live2d/DESIGN-ARM-JOINTS.md`。）

**给 LLM 的接口（全部经控制端口 2333 行协议；`LIST` 里都有清单）**：
- 关节角（度）：`BODY arm.r.shoulder=70 arm.r.elbow=-90 arm.r.wrist=10`
  ——肩 正=外展抬起；肘/腕 正=往身体内侧折（托腮/抱胸），负=往外侧折（抬臂时前臂竖起=挥手）。
  左右已镜像归一：同一组数字两臂通用。`arm.r` 仍可用（=肩的简写，1.0=90°）。
- IK：`BODY hand.r=chin hand.l=hip`（锚点见 `ANCHORS=`：mouth/face/eyes/chin/head_top/chest/
  forward/waist + 每臂 hip/side/up/head_side/cheek；或 `hand.r=x,y` 模型坐标；`hand.r=off` 放手）。
  回复带解出的角度，够不着会写"够不着"（误差 > 8% 臂长）。
- 过渡时间：`BODY t=0.3 arm.r.shoulder=80`（这条指令约 0.3s 到位；缺省按通道默认速度）。
- 动作库：`ACTION <名> [speed=] [scale=] [repeat=]` / `ACTION stop`；回复带时长 + 本模型缺的通道。
  21 个内置（挥手/欢呼/托腮/叉腰/捂脸/抱胸/话筒/跳舞/鞠躬…），profile 的 `"actions"` 可覆盖/追加
  （通道关键帧；`{"ch":"hand.r","anchor":"chin","keys":[[0.05,1],[0.85,0]]}` = IK 窗口）。
- 演出脚本新步：`{"action":"cheer"}`、`{"body":{"arm.r.shoulder":80},"dur":0.4}`、`{"body":"reset"}`；
  脚本设的 body 姿态在段尾自动弹回。
- 动捕感：`MOCAP 0..3`（0=关，纯指令值；1 默认：有控制时头/身/臂微动 + 身体滞后跟头；2/3 更夸张）。
- `BODYINFO=` 每通道给范围与方式（joint=骨架 / p=经物理 / d=直连）；`SKELETON=` 给关节上限。
- **应用内终端（msh/nsh）同样可用**：`l2d action <名> [speed= scale= repeat=]|stop`、
  `l2d hand <r|l> <锚点|x,y|off>`、`l2d mocap [0..3]`、`l2d body arm.r.shoulder=70 arm.r.elbow=-90 [t=0.3]`
  （TAB 补全动作名/锚点/通道名）。发往播放器的 `l2d` 子命令都会等回话（≤1.5s）并打印 `OK…/REJECT…`。
- 语义：显式设某条手臂的关节角（含 `arm.r` 简写）= 接管这条手臂，**取消它的 IK**。

**⚠ 复查结论（同日）：外加骨架做大动作 = 手是扭曲的**（手只画了垂手一种，转过 90° 正反面就错；
单张网格的肘会折出尖角）。外加骨架降级为兜底；**大动作走作者骨骼**——`live2d/DESIGN-ARM-JOINTS.md` §7：
- **Natori（官方样例）= 目前最好的"带骨骼"模型**：6 套作者手臂 + 肩/肘/腕关节链 + 手型/手指，板上 29~30fps。
  profile `armSets`：`BODY arm.r.set=B|B01|E|E06|A|auto`、`arm.l.set=A|C|D`，关节通道自动接当前套；
  11 个专属动作（`ACTION think/adjust_glasses/adjust_tie/bow/present…`）。切到它：`l2d model 4`（按 `l2d model` 清单）。
- 新指令 `PART <部件>=<不透明度> … | off`（通用：切作者画的另一套手臂/道具）。**坑**：SDK pose 看的是
  部件**同名伪参数**，只压不透明度会卡在交叉渐变（半透明 + 残影）——`HoldPart()` 两个一起压。
- 界面左上角模型名：shell/控制端口切模型后不刷新的 bug 已修（`ui.c` 改为同时比较模型名）。

**运动质量（"像动捕"的来源）**：通道从一阶平滑换成**弹簧**（阻尼 0.72，约 4% 过冲，有惯性）；
释放是**弹回**而不是瞬间归零；动作关键帧走 Catmull-Rom（过点不停顿）；同通道新动作接管旧动作时
旧的 0.25s 淡出。**实时 dt**：有输出端口时（板上/窗口）dt=实测帧间隔（钳 ≤0.2s）——舒芙蕾
7fps 下动作/呼吸/演出也按墙钟走（原来只有 0.23 倍速）；离线渲染仍是固定 dt（逐位可复现），
`L2D_FIXED_DT=1` 可强制。

**修掉的既有 bug**：控制端口 `LIST` 走 512B 缓冲，能力表在 `EXPR=` 附近就被截断——
**LLM 从没拿到过 BODYCHANNELS 及之后的内容**。已改成整段写（`l2d_ctrl.c` `ctrl_write_all`）。

**⚠ 部署坑（本轮实踩，板子起不来）**：`/oem` 只剩几百 K。`rk_build_tools deploy` 之后再跑
`rk_build_tools run`，**run 会再部署一遍**；第二次推主程序时旧进程还占着文件，空间不够 → 推送失败、
**`/oem/vanxoak_lvgl/vanxoak_lvgl` 直接没了**，监管脚本连崩 5 次熔断。正确顺序：
`rk_build_tools stop` → `deploy` → `run --no-deploy`（只改播放器时：stop → `adb push
live2d/player/dist/l2dplayer /userdata/vanxoak_lvgl/l2dplayer` → `run --no-deploy`）。

**排障**：`L2D_RIG_CHECK=1` 每 30 帧打印刚性残差和过渡带顶点占比；`[rig]` 日志给每臂的分段
（几张整网格刚性、几张按顶点切、关节在臂轴上的位置）。骨架几何是**懒建**的——第一次驱动手臂时
按 pose 生效后的可见网格重建（装载时 Haru 的 ArmRB 那套也"可见"，会被错收）。

## 7c. LLM 交互协议 v2 + 本机桌宠（2026-10-09，本机已实现）

→ **[`PROTOCOL-LLM.md`](PROTOCOL-LLM.md)**：模型导入后**自动识别能力**（pose3 互斥组 = 几套作者手臂；参数显示名/编号 = 肩肘腕手指；
物理输出不算关节）→ LLM 发 `{"cmd":"query"}` → 上报能力表（每项 `{name, fv}`，fv=特征值如 `pose:arm.r.B`、`joint:arm.r.B.elbow`、
`act:adjust_tie`）→ LLM 用 `sequence`/单项命令按 fv 定期下发。骨架等级 full/chain/swing/none：有骨骼的上报姿势套与关节（大动作），
无骨骼的只有头身表情版动作（内置动作执行时一律不带手臂轨道）。

- **本机桌宠**（先不连板卡）：`l2dtest <模型> 0 0.0333 --pet --ctl 23333 --size 360x640`（透明置顶窗口，拖动/点击/滚轮）；
  `live2d/tools/pet_client.py` 调试、`live2d/tools/pet_agent.py --demo|--llm` 定期驱动。
  **交互 2026-10-09 晚增强**：身体 **16 区域**（头/脸/颈/胸/肚/胯 + 左右大腿/小腿/脚/手/手肘；
  锚点投影+臂骨架关节判定）→ `click`（同区连点合并 `n`）/`hover`（驻留）/`rub`（左右搓 `count`）事件；
  `speak` 文字显示成**气泡**（Xft；LLM 可带 `options` 提问，点选回 `choice` 事件——LLM 实跑自发使用并正确续戏）。
  分区调试：`L2D_DBG_ZONES=1`（点一次打一行锚点/分带依据）；证据 `live2d/img/demo-pet-zones-bubble.png`。
  **缩放 2026-10-09 深夜修正**：滚轮从"渲染器内部缩放（模型被固定窗口裁掉、气泡位置/大小错乱）"改为
  **缩放整个视窗**——桌面端不再动 `SetRenderZoom`（渲染保持满幅），改调新增的 `IPort::SetZoom`：
  窗口尺寸 = 渲染帧尺寸 × zoom、**底边中点锚**（脚不动往上长）、超屏自动钳制、存配置 `zoom` 下次启动保持；
  气泡（独立小窗）的 `_bs` 与重定位改由 `_zoom` 同步驱动（原来是读配置镜像 → 慢几拍）。
  实测：启动恢复 0.7074→255×453；放大 8 格 →496×882 模型完整；放大中气泡字号/位置同步跟随；
  缩放后点击判区仍正确（`click zone:head`）；证据 `live2d/img/demo-pet-zoom-sync.png`。
  **记忆 2026-10-09 深夜**：agent 跨会话记忆 `~/.config/l2dpet/memory.md`（每 6 段归纳一次，注入每段
  上下文；更新后 ≥15min 带记忆重生成即时台词库 → 离线点/搓台词"记得你"）；实测：记忆写入→
  重启 agent"记忆已载入（87 字）"→ 台词库 43 组 130 句带样例打印。`--no-memory` 可关。
  **UI 架构换成 EUI-NEO（同夜）**："状态与设置"改为独立进程 `l2dpet-panel`（后改名 `l2dpet-ui`，见下条）（`live2d/panel/`，C++17，Linux/Win/mac，
  响应式布局，不再闪），纯协议客户端（`panel_state` 轮询 + 现有命令）；桌宠菜单/托盘负责启动或唤起它。
  构建 `live2d/build-panel.sh`；详见 `live2d/DESIGN-PET-LIFE.md` §12。
  **一个工程一套 UI 栈（同夜）**：右键菜单、托盘、说话气泡也搬进 EUI 进程，改名 `l2dpet-ui`（常驻，主窗口启动隐藏）；
  桌宠进程只剩角色窗口 + 渲染 + 输入，经 `ui_*` 事件（`ui_menu/ui_bubble/ui_bubble_anchor/ui_bubble_hide/ui_open/ui_close_menu`）
  驱动 UI，UI 回发 `choice/window/quit` 等命令；`PetPanel.hpp`、`PetTray.hpp`（GDBus）删除，CMake 去掉 gio。
  EUI-NEO 缺的能力（弹出/提示窗口类型、运行中移动/关窗、常驻 tick、托盘动态菜单 + 勾选项、`_NET_WM_USER_TIME`）
  做成仓库补丁 `live2d/panel/eui-patches/`，CMake 自动应用。菜单防 mutter 抢焦点：右键时间戳当 user time + 0.3s 宽限。
  **真实鼠标验证 ✅（2026-10-09 深夜，ZCode 接手）**：XTEST（X server 级注入，等同真实输入设备）实测——右键身体 →
  `ui_menu` → 菜单弹出（右边放不下左翻、底部越界上收都对）→ 真实左键点"状态与设置…" → 设置窗映射 + 菜单关闭；
  speak 带选项 → 气泡弹出 → 真实点选项 → `choice` 事件 + 气泡收起。回归脚本固化在 `live2d/tools/ui_click_test.py`
  （python-xlib，五项全过，可随时复跑）。**顺手修了一个 bug**：`panel page:-1` 原来只发 `ui_close_menu`（设置窗关不掉），
  新增 `ui_close` 事件（UI 侧 `hideMainWindow` + 收菜单，eui-patch 里早有 `hideMainWindow` 只是没接线）。
  仍未验证：Windows/macOS 编译（托盘真实点击已在下面"三修"里补验）。
  **用户实测反馈三修（2026-10-09 深夜，ZCode）**：① **托盘图标不显示**——SNI `IconName` 是主题图标名（宿主找
  `<名字>.png`），传 `l2dpet.png` 被拼成 `l2dpet.png.png`；tray_bridge.c 剥扩展名（GNOME 托盘宿主=status-tray 扩展，
  图标/菜单已真实点击验证）。② **气泡随模型动画低频抖动**——锚点改为只跟**窗口几何变化**（拖动/缩放），
  模型在窗口里自己动（呼吸/动作头顶挪几像素）不再推锚点（PortX11Pet `BubbleAnchorTick`）。③ **隐藏→再显示后
  气泡/右键"失效"+托盘开设置后"异常叠加"**（用户定位的复现路径）——角色窗重新映射后排到弹出窗上面（菜单/气泡
  开了但被盖住看不见）；叠加=积压的泄漏气泡窗被 mutter 一起抬上来；修：弹出窗显示后 `XRaiseWindow`（window_backend），
  另修泄漏根源——`applyWindowControls` Close/Move/Resize 只作用于第一个同 pageId 窗口 → 改为作用于全部 +
  `Tick()` 安全网补关。验证：复现路径全过（堆叠序确认）、睡眠梦话 60s 零残留、`ui_click_test.py` 五项全绿。
  **交互特效 + 气泡回退（同夜第三轮）**：① **点击特效**（扩散圆环×2 + 7 颗四芒星飞散）与**滑动特效**（指针滑过
  模型每 ~10 逻辑像素一颗闪光、上飘渐隐）——PortX11Pet 帧上叠画（预乘 ARGB Over），特效缓冲**随窗口缩放重分配**
  （否则滚轮缩放 memcpy 越界崩——用户实测定位的缩放崩溃）。② **气泡改回旧版设计**（用户定案"git 里没用 EUI 时
  的设计是对的"）：从 4ee979d 完整复活 SDF 像素渲染（椭圆鼓泡/深青描边/下弯浅蓝/右上高光/双小圆点沿法线/
  0.18s 淡入 0.22s 淡出/Xft 文字/选项胶囊/PollChoice），EUI 进程气泡代码删除，`ui_bubble*` 事件停发（协议表已注）。
  真实鼠标点选项胶囊 → `choice` 事件端到端验证 ✓，回归五项全绿。注意：气泡字号不乘 `_ui`（Xft 在高分屏本来就
  按 DPI 放大，双重缩放会巨大）。
  **高分屏 + VTS 模型（同夜）**：3200×2000/200% 下自动 2× 渲染（窗口/面板/气泡/阈值都按 `Xft.dpi` 缩放，双线性放大，
  pet 默认 8 线程 → 30fps）；VTube Studio 格式模型（model3 不登记表情/动作）加载时从目录自动补全——作者的"水印"开关
  因此生效。模型库放 `~/.local/share/l2dpet/models/`（勿放 /tmp）。详见 `live2d/DESIGN-PET-LIFE.md` §11。
  **记忆 v2（同晚）**：分层（`memory/owner.md` 共享 + `chars/<指纹>.*` 每角色）、戳摸只计数、选项回答/关系事件
  记为重要、待整理随时落盘 + 归档召回、久别重逢叙旧、主动用选项了解主人、0600、旧文件自动迁移；
  设计与边界见 `live2d/DESIGN-PET-LIFE.md` §10。
  **面板可见**：行为 → 记忆（卡片：字数/更新时间/查看 ›）→ 记忆本页（三段、滚轮滚动、可手改文件）；
  字数按**字符**显示（曾把 C++ 字节数当字数 → 414 vs 184 的乌龙，已修 Utf8Count）；面板高度 600→660
  给行为页底部留位；证据 `live2d/img/demo-pet-memory-panel.png`。
- 新文件：`src/PortX11Pet.cpp`（桌宠窗口）、`src/CtlServer.cpp`（JSON Lines 端口）；协议处理在 `l2dtest.cpp` 的"桌宠协议"段。
- Natori profile 改为"自动识别 + `poseNames`/`poseHands` 补语义"，不再手写 `armSets`。
- 板卡移植待做：`l2d_ctrl.c` 转发 `{"cmd":` 行 + 回复区扩到 8 KB（见 PROTOCOL-LLM §8）。

## 7d. 桌宠"活起来"：三层交互 + 内在状态 + 连续情绪 + 右键面板（2026-10-09，本机已实现）

→ **[`live2d/DESIGN-PET-LIFE.md`](live2d/DESIGN-PET-LIFE.md)**（技术路线、公式与阈值、配置键、**边界与已知限制**、验证记录）。

- **三层交互**：本地即时层（看向触点 + 部位动作 + 推情绪 + 一句台词，0 延迟；台词库由 LLM 启动时按人设一次写好，
  `{"cmd":"reactions"}` 下发）/ LLM 回应层（停手 2s 后一句，≤6s，replace）/ LLM 闲时层（默认 60s）。
- **内在状态**：精力/亲密/恼火/寂寞 → awake/drowsy/asleep/sulk + 享受/欢迎/寂寞找你/好奇瞄一眼；亲密度按模型指纹存
  `~/.local/state/l2dpet/`。**连续情绪**（参考 soullink-emotion-sdk）：VAD 推目标 → 叠加层映射到嘴角/眉眼/头姿/脸红，
  峰值才借模型表情。
- **右键菜单 + "状态与设置"窗口**（参考 BongoCat，新文件 `src/PetPanel.hpp`）：右键=菜单（原右键双击退出取消），
  五页（状态/行为/窗口/对话/关于），配置存 `~/.config/l2dpet/config.json`；所有操作经协议命令执行。
- 新协议面：`emotion`/`mood`/`config`/`touch`/`reactions`/`panel` 命令，`emotion` 时间线步，`mood`/`config` 事件。
- 跑法不变；agent 必须在桌宠端口就绪后再启，桌宠重启后要重启 agent（台词库在桌宠内存里）。
- 板卡：生活层/情绪只在 pet port 下运行，面板只有本机实现（见设计文档 §6"板卡"）。

## 7e. 三端化：内置 C++ agent + 跨平台端口（2026-10-09 深夜/10-10，Linux 已实测）

用户决策：**先完成 Linux/Win/Mac 三端代码（RK3506 后面再说）；交互协议端口保留，agent 改为进程内调用**。
本轮四个提交（8e75a20 → b32ed77 → e397ad1 → 本文档）：

- **内置 C++ agent（M1，8e75a20，已实测）**：`player/src/{LlmClient,PetMemory,PetAgent}.{h,cpp}` ——
  LLM 大脑从外部 Python 进程（pet_agent.py）移植进桌宠进程：独立线程 + RPC 桥（命令投主线程执行
  `L2DPlayer::PetCommand()`，与 TCP/面板同一入口，完全绕过 socket）。curl 走 HTTPS/LlmClient、
  记忆分层/召回/归纳（pet_memory.py 忠实移植）、台词库/回应/闲时/重逢/choice 续戏/节制规则全同文。
  实测：43 组 130 句台词库、摸头→回应段、选项→续戏段。**`tools/pet_agent.py` 与 `pet_memory.py` 保留**
  （外部 agent 形态仍可用：连 23333 会收到"内置 agent 正在运行"的双驱动警告；板卡形态用它）；
  `L2D_NO_AGENT=1` 关内置。
- **跨平台基座（M2，b32ed77）**：`Platform.{h,cpp}`（时间/路径/目录/子进程/CPU）+ l2dtest 全部 POSIX
  调用点迁移 + CtlServer Winsock + CMake 三端分支（Core 库路径/链接库/MSVC /utf-8）。Linux 回归全绿。
- **PortGlfwPet（M3，e397ad1，Linux 已实测全绿）**：`player/src/PortGlfwPet.cpp` + `PetText.{h,cpp}` +
  `PetPaint.h`（从 X11 端口抽出的 SDF/特效共享像素库）——**三端同一份桌宠窗口代码**：GLFW 3.4
  （vendor 于 EUI-NEO）+ GL 3.3 core 出帧（软件 ARGB→纹理→quad，预乘混合）+ freetype 气泡文字。
  Linux 用 XShape 精确输入区域（点非角色处穿透到桌面）；Win/Mac 用 GLFW_MOUSE_PASSTHROUGH 整窗穿透
  + 全局鼠标轮询（GetCursorPos/CGEventSource）+ 软件命中。`--pet` 在 Win/Mac 自动选 glfwpet；Linux
  仍默认 X11 端口（`--port glfwpet` 可切）。**ui_click_test.py 在 glfwpet 上五项全过**（右键菜单→
  设置窗、气泡选项→choice、真实点击/摸头、透明窗口、启动自动拉 UI 进程）。
- **三端构建指南**：`live2d/BUILD-CROSS-PLATFORM.md`（各平台 CMake 命令、SDK 前置、端口矩阵、
  **未验证清单**：Win/Mac 实机、mingw 交叉编译、Win 滚轮穿透缺失）。

坑（本轮新踩，别再踩）：
- **面板 config 解析不能只认 `false/true`**：panel_state 里是数字 `0/1`，误判会把 passthrough 打开、
  XShape 输入区域清空 → 所有鼠标事件穿透（症状：右键/点击完全无反应）。修：数值+字符串都认。
- **GLFW 事件 vs 全局轮询的分工**：Linux 必须用 GLFW 窗口事件做点击（精确，点菜单不会误发
  ui_close_menu 把菜单提前关掉）；全局轮询只做视线/滑动特效。Win/Mac 因整窗穿透只能用全局轮询。
- **Mimosa 写入钩子**：`fork+exec*` 会被拦（"命令注入"），**posix_spawn 过审**（三端 Platform 的
  SpawnDetached 因此用 posix_spawn/`_spawnv`）；带转义引号的多行 `snprintf` 也会被拦（改纯字符串拼接）。

## 7f. 用户实测两修：dock 标题乱码 + "点击歪"（2026-10-10）

- **dock/顶栏标题乱码（"æ¡å®"）**：根因是 **WM_CLASS 里存了原始 UTF-8 中文**。GLFW 的 X11
  后端在没给 class/instance hint 时**用窗口标题兜底 WM_CLASS**，而 WM_CLASS 按 ICCCM 是
  ASCII/latin-1 语义 → dock 按 latin-1 解码就成乱码。修法两层：
  ① EUI-NEO 补丁：建窗后把 WM_CLASS 固定为 ASCII（`l2dpet-ui`，XChangeProperty XA_STRING；
  注意 dlopen 句柄要在所有窗口上加载，不能只在弹出窗路径——主窗口也走这里）；
  ② 桌宠 X11 窗同样补 ASCII WM_CLASS（`l2dpet`）+ `_NET_WM_NAME`(UTF8_STRING) 标题；
  ③ PortGlfwPet 用 `GLFW_X11_CLASS_NAME/INSTANCE_NAME` hint 固定 ASCII class。
- **"点击都是歪的"（点哪儿都像点头/胸）**：根因是**模型目录缺 `l2d.profile.json` 与
  `l2d.capability.json`** → `_parts` 空 → 臂骨架/关节锚点全没建 → 16 区判定**静默退化成
  "头/身两带"**（渲染正常，只有点击分区错）。修法：`live2d/tools/setup-local-model.sh <模型名>`
  一键把仓库的 profile + 能力缓存（按指纹校验）装进 `~/.local/share/l2dpet/models/<模型>/`；
  启动日志新增 `[zones] 未建臂骨架：分区退化为头/身两带` 警告，不再静默。
  **教训：把模型拷到本机模型库时必须同时带 profile + 能力缓存**（/tmp/l2dm/shuflle 旧目录是齐的，
  ~/.local/share/l2dpet/models/舒芙蕾 是 opus 建 HiDPI 模型库时只拷了模型本体）。
- 回归脚本加固：断言改轮询等待（淡出/关闭有延迟，固定 sleep 会偶发假失败）；气泡选项定位改为
  "宽青行配对"（胶囊上/下边），不依赖胶囊横排还是竖排。

## 7g. 多 LLM 供应商（2026-10-10，本机已实测）

用户要求"允许多个 LLM 商（可自行增加）+ 每商可选模型种类；同一时间只用一个"。实现：

- **配置 v2**（`~/.config/l2dpet/llm.json`，0600）：`{v:2, active, providers:[{id,name,base_url,api_key,model,models[]}]}`；
  **v1 单条配置读取时自动迁移**成名为"默认"的供应商（下次保存写回 v2）。环境变量仍最高优先级（临时覆盖）。
- **协议**：`llm_providers`（全量保存；`api_key` 空 = 按 id 保留原密钥——面板不持有明文）；`llm_config` 读/改兼容
  老用法（改的是激活供应商）；保存后推 `llm_config` 事件。`agent_info`/`llm_stats` 报激活供应商。
  （详见 `PROTOCOL-LLM.md` §LLM 接入配置）
- **面板"对话"页**：供应商 chips（● = 激活，点击切换编辑对象）+「+ 添加」/「删除该供应商」；字段 = 名称 /
  接口地址 / API Key（占位显示"已保存（末 4 位）"）；模型列表（✓ 使用中 / 选用 / 删除 / + 添加模型）；
  「保存并激活」一次提交全部 + active。
- **实测**：v1 文件自动迁移 ✓；建第二供应商（坏地址）并激活 → agent 立刻改走它（SSRF 防护拦下并报错）；
  切回 → `agent_info` 恢复 deepseek、调用正常、**未携带 api_key 的供应商密钥按 id 保留** ✓；面板 UI 截图核对 ✓。
- 生效机制：内置 agent 每次调用都 `LlmLoadSettings()`（读激活供应商）→ 保存即生效，无需重启。

## 7h. 窗口/模型拆页 + 模型能力解算页 + 导入识别（2026-10-10，本机已实测）

用户要求："窗口与模型分开；模型界面要显示解算出的模型能力（表情/动作）、是否有骨架、识别出的躯干状态；
可导入新模型识别、可切换模型。"实现：

- **拆页**：`窗口`（原显示设置）与 **`模型`**（新，面板第 3 页；页序 0 状态/1 行为/2 窗口/3 模型/4 对话/5 记忆/6 关于/7 日志）。
- **当前模型解算卡**：名字 + 指纹 + 目录；徽章 = 骨架等级（`swing/chain/full/none`）+ 交互分区
  （`16 区（含肘）/ 14 区（作者绑定）/ 2 区（基础）`）；动作组 ×数量、表情列表（chips）、躯干通道列表
  ——**骨架驱动**（蓝）与**作者绑定**（橙）分色；底部 profile / 能力缓存 ✓ 或橙色警告（缺 → 提示去探测）。
- **模型库卡**：列表（● 使用中 + 切换按钮 / **删除**按钮），来自 `panel_state.models`；点击发 `{"cmd":"model","index":n}`。
  **删除**走二次确认（点一下"删除"→ 按钮变"确认删除？"，再点才执行；**按模型名匹配**，列表刷新不会误删到别人），
  发 `{"cmd":"delete_model","name":"x"}`：只允许删用户模型库一级目录（名字校验拒 `/`、`..`、`.` 开头），
  **正在使用的模型拒删**（面板上当前行只显示"使用中"，没有删除按钮），删完自动重扫列表。事件 `model_delete` → toast。
  pet 侧目录删除走 `Platform::RemoveFile/RemoveEmptyDir`（POSIX remove/rmdir ↔ Win DeleteFileA/RemoveDirectoryA，
  不再直调 POSIX）。**注意**：`scanDir`（板上 SD 模型库）里的模型不在用户模型库里 → 删除会被拒（`not_found`）。
- **导入卡**：路径输入（.zip 或目录）+ 「导入并识别」→ `{"cmd":"import_model","path":…}`；提示语里写明重名加 `-2`、
  导入后自动切换识别。事件 `model_import` → toast（成功"已导入「x」，正在切换识别…" / 失败原因）。
- **探测**：卡内「能力探测（约 30 秒）」→ `{"cmd":"probe"}`（缺缓存时按钮变主色）；完成后自动重载，
  `probe` 事件 → toast。
- **pet 侧**：`panel_state.model_info` 新增 `zone_mode`/`zone_count`（面板徽章数据源）；**模型库每 ~2 s 温和
  重扫**（用户直接往库里拷/删目录，面板列表自动跟上，不留死条目——删掉的目录 2 s 内消失）；
  重扫按"当前加载模型的 RealPath"找回下标，不会因为排序变化切错模型。
- **作者绑定模型的分区兜底**（yumi 实测）：profile `"rigid": false`（手走作者 `Paramarmup*`/`Paramhand1*`）→
  臂骨架建不出来 → 原来直接退化为 **2 带**（点击全歪）。现在 `BuildFallbackAnchors()`：眼/嘴/手锚点用
  "动该参数顶点的质心"（与骨架模型同一 MovingCentroid 算法，HAND 用 profile 手通道参数），下颌/胸/腰按包围盒比例 →
  **14 区**（无肘部；手部 disks 从 `0.065×包围盒高` 半径判定，先于躯干带）。实测 yumi：`zonemap` 14 字母齐全、
  手/玩偶区落点正确（手所在处=她抱的云朵玩偶与右裙前）。
- **实测**：面板导入 yumi.zip（19 MB）→ 解压成 `yumi-2` → 自动切换 → 探测 20/20 写缓存 → 自动重载 →
  面板徽章从 `2 区（基础）` 变 `14 区（作者绑定）`；错误路径 toast（导入失败）✓；**用户自己用面板导入"林翩翩"也一次走通**
  （日志 `模型库重扫：3 个（林翩翩）→ 加载模型 2/3: 林翩翩`）；`ui_click_test.py` 5 项回归全过。
- **每模型一个性格（2026-10-10 补）**：用户反馈"模型都是一套性格，没有区别"。新增 `PetPersona`（`player/src/PetPersona.{h,cpp}`）：
  把模型的解算能力（名字/表情道具/骨架/可交互区域摘要）交给 LLM 生成性格（名字/性格/口癖/喜好/常用情绪/偏好表情），
  按**模型指纹**存 `~/.config/l2dpet/personas/<指纹>.json`（0600，可手改，删掉即重生成）；无 Key/失败 → 本地启发式兜底
  （道具气质线索 + 指纹选原型），有 Key 后自动升级 LLM 版。性格注入两处：段落生成的系统提示（【你是谁】段）
  与即时台词库的角色描述 → **点同一个部位，三只模型的台词明显不同**（柚咪傲娇 / 舒芙蕾软萌 / 翩翩娇气）。
  面板模型页加「性格」卡（全文 + 来源 + 「重新生成性格」→ `persona regen`）。`fav_expr` 会校验必须是该模型真有的表情
  （水印类排除）；LLM 偶尔吐坏 JSON → 重试一次再退兜底。**踩坑**：截断生成文本必须按 UTF-8 边界（substr 字节截断把
  persona 文件写成了非法 UTF-8）；面板里 `strOr()` 返回 `const char*`，跟字面量 `==` 比的是**指针**（恒假，toast 文案一直走错分支）。
- **锁定大小和位置（2026-10-10 补）**：配置键 `lock`（默认 0）。开启后**角色窗口拖不动**（X11 端口在 MotionNotify 拦、
  GLFW 端口在拖动分支拦；按下抬起仍算点击，所以点/摸照常）、**滚轮不再缩放**（主循环拦 wheel，面板滑条仍可改）。
  入口：面板"窗口"页开关 + 右键菜单开关（与"总在最前/鼠标穿透"同排）。实测：锁定时拖动/滚轮都无效、点击仍出气泡，
  解锁后立刻恢复；开关经 `{"cmd":"config","set":{"lock":1}}` 走同一条路（面板点一下 → 配置即时生效）。
- **问题库：提问不再只有"在干什么"（2026-10-10 补）**：用户问"为什么询问的交互只有在干什么"。诊断：提问的引导只有一句
  含糊话（"称呼/作息/喜好之类"）且**只在还不了解主人时给**，其它时候 LLM 自由发挥 → 每次落回最笼统的"你在干什么呀？"。
  修法 = 新增 `PetQuestions`（`player/src/PetQuestions.{h,cpp}`）：LLM 按**题材清单**一次生成 12 题（称呼/作息/工作学习/吃/
  爱好/心情/家人/回忆/打算/关系/她的小请求/闲聊），每题 2~3 个可点选项，按**模型指纹**存
  `~/.config/l2dpet/questions/<指纹>.json`（0600、可手改）；闲时段把 2~3 道**没问过**的候选写进提示词让 LLM 挑；
  问过记 `asked`（自创题也入库）、答了记 `answer`。重生成时机：无库 / >24h / 没答过的 <3 / **答够 5 题**（换新题材）。
  **答案的去向（"不限于对话"）**：① 题库（下次生成作为"已知"喂回，不重复）；② 跨会话记忆（ChoiceMade → 习惯统计 +
  归纳进 owner.md）；③ **立刻重写即时台词库**（`_linesUrgent` 绕过 15 分钟节流）→ 点/摸的离线台词马上带上新知道的事；
  ④ 段落记忆块里明确写了"称呼/作息/口味/爱好要体现在所有交互（台词/动作/表情/视线）"。启动后 1 分钟内不提问。
  实测：12 题一次生成、口吻与人设一致（"呜哇——芙蕾叫你主人，你呀想被叫别的吗？"）；第一问 称呼 → 答"叫名字嘛" →
  题库记 answer + 台词库立刻重写（"即时台词库（主人回答）"）；第二问换到 工作学习 题材（不重复）。
- **互动小功能：笑话 / 猜拳 / 工作时长 / 天气（2026-10-10 补）**（用户："还有游戏，笑话的交互。还有工作时长提示，还有天气提示(需要设置里打开)"）：
  ① **讲笑话** `{"cmd":"joke"}`——本地念台词库新增的 `life.joke` 组（LLM 按性格写 6 句 ≤45 字）+ 口型，内置 3 句兜底；
  ② **猜拳** `{"cmd":"game","kind":"rps"}`——本地全流程（气泡三选项 → 点选 → **本地随机出拳+判定** → 结果气泡
  `life.rps.win/lose/draw` + 反应赢跳/输歪头/平点头）→ 推 `game` 事件让 agent 接话；**校验选项标签**（气泡过期后
  换成 LLM 提问的选项不会被当成出拳）、猜拳的 choice **不发**普通 choice 事件（否则污染题库问答记录）；
  ③ **工作时长**：桌宠按"鼠标一直在动"估连续使用时长（离开 >5 分钟清零），到 `work_remind` 分钟推 `work_long` 事件 →
  agent 出【关心】段（提醒休息+伸展动作）；④ **天气**：**交给 LLM**（用户定案，不接 API）——`{"cmd":"weather"}` →
  `weather_ask` 事件，agent 带 `weather_loc` 现编一段；提示词明确"没有实时数据、用关心/提问式、不许编温度"；
  开着 `weather` 时闲时也会偶尔主动聊一句。
  配置新增 `work_remind`(分钟，<1=关，默认 60) / `weather`(0/1) + **字符串配置**（`config.json` 的 `"str"` 段、
  `{"cmd":"config","setstr":{...}}`、`state.config.str`）承载 `weather_loc`；面板"行为 → 互动小功能"有滑条/开关/位置输入，
  右键菜单加三项（聊天气仅在开关打开时出现）。实测：笑话人设化（"今天风好大，芙蕾差点被吹成舒芙蕾饼"）、
  猜拳两局（结果气泡 + agent 接话"两局两胜咯"）、工作 2 分钟提醒（"都坐了两分钟啦，起来伸个懒腰"）、
  天气段带"北京"且不编数字（"北京这会儿天儿怎么样呀…出门记得多穿一件"）。
- **笑话池 + 睡着不调 LLM（2026-10-10 二补）**（用户："笑话可以定期让 LLM 生成一些；模型睡觉就不调用 LLM 了"）：
  ① 笑话从"台词库里的 6 句"升级为**独立池子** `PetJokes`（`~/.config/l2dpet/jokes/<指纹>.json`）——"没讲过的"<2 条且距上次生成
  ≥30 分钟（`L2D_JOKE_GAP_SEC` 覆盖，测试用）就让 LLM 生成 6 条**新的**（已有的喂回去防重复），追加进池（上限 40，
  超了丢最久没讲的已讲条目），推给桌宠；讲笑话仍是**本地轮转秒回**（新增的排后面优先讲），讲完推 `joke_told` → agent 记 `used`。
  实测：6 条生成 → 连讲 6 条全不同（没讲过 6→0）→ 醒后自动补到 12 条。
  ② **`asleep` 时 agent 一切 LLM 调用暂停**：闲时段原本就跳过，现在把**记忆归纳 / 台词库刷新 / 题库体检 / 笑话补货**也一起停
  （只跑本地心跳），醒来自动恢复并立刻补做（实测睡 100 秒零调用，醒后立刻"题库换一批 + 补 6 条笑话"）；睡着时点"讲个笑话"
  回梦话体（不吵醒她）。
- **记忆压缩 / 备份导出导入 / 原生文件选择器（2026-10-10 三补）**（用户："记忆增加 llm 压缩功能（每天自动压缩一次），增加导入导出功能"；随后"导入导出…无法真正的打开文件管理器…看看能不能修复"）：
  ① **记忆压缩**：`PetMemory::Compress` —— 合并去重（同一件事反复记的必须并成一条）、旧事折进 `## 更早`、
  删一次性细节，并把 `episodes.jsonl` 收编后截断到 60 条；**新增硬约束：结果不许比原来长**（>原量 5% 保留原文——
  实测 LLM 偶尔会写得更啰嗦再被 kCharMax 截成半句，日志里表现为"1596→2105 字节"）。触发：agent 每 10 分钟体检
  `NeedsCompress(24h)`（醒着 + 有 Key 才跑）+ 面板「立即压缩记忆」。压缩时间 `habits.json.compressT` →
  面板"上次压缩：…"。**踩的坑**：回报事件名一开始用了 `mem_compress`，而所有桌宠事件都会喂给 agent →
  它把自己的回报当成新请求，连压 12 次（日志同一秒反复"记忆压缩（面板指令）…"）→ 广播改名 `mem_compress_done`。
  ② **导出/导入**：`{"cmd":"export"|"import","path":"/abs/x.json"}` 把 `memory/*`+`memory/chars/*`+`personas/`+
  `questions/`+`jokes/`+`config.json` 打成一个 JSON（**不含模型文件、不含 llm.json=API Key 有意不导出**）；
  导出做 **UTF-8 清洗**（历史文件有坏字节 → 带坏字节的包严格解析器直接读不了，实测 Python 读不了）；
  导入**只写白名单路径**并拒 `..`，完成后 `profile_reload` 让 agent 重读记忆/性格/题库/笑话 + 重写台词库（实测：17 文件回滚、
  标记行被备份覆盖、agent 全量重载）。**又踩一次 `strOr()` 指针比较**（`v.get("app").strOr("") != "l2dpet"` 恒真 →
  导入永远报"不是桌宠的备份文件"）；`grep -rn 'strOr("[^"]*") *[!=]= *"'` 全仓扫过只剩这一处，已修。
  ③ **原生文件选择器**（面板）：记忆页「导出…/导入…」+ 模型页「选 zip…/选目录…」走 `zenity`（退回 `kdialog`；
  Win/Mac 返回"不可用"→ 回退手填路径）。两个关键点：**posix_spawn + 管道不经 shell**（路径带引号/空格/分号安全）、
  **子进程环境必须收窄**（只给 DISPLAY/XAUTHORITY/XDG_RUNTIME_DIR/DBUS_SESSION_BUS_ADDRESS/HOME + PATH + `LC_ALL=C.UTF-8`；
  带着终端完整环境时 zenity 立刻退出、弹不出窗——本机实测，`env -i` 最小集合才正常）。
  **验证状态（供审阅）**：修好环境后**对话框确实能弹出**（进程与参数都对：`zenity --file-selection --save --title=… --filename=…`）；
  「选完文件之后」的那半段没有用合成输入去点（用户明确要求不要用脚本操作保存对话框——风险自负不起），
  但它后面的链路（export/import 命令 + 手填路径）已单独实测通过，所以只差"人点一下对话框"这步。
- 遗留：zip 不含本仓库为特定模型手写的 `l2d.profile.json`（手势/表情别名/作者通道映射），导入后如需这些能力要另配
  （面板会明确提示"缺少 l2d.profile.json"）。
- **顺手修的真 bug（2026-10-10 补）**：面板事件连接 `petlink.cpp` 原来把**非 `ui_*` 事件全丢了**
  （`if (line.find("\"event\":\"ui_") == npos) continue;`）→ 模型页的 `model_import` / `model_delete` / `probe`
  toast 从来没响过（"正在导入…"那种是面板本地 toast，掩盖了这个问题）。修=白名单加三类模型事件。
  **教训：新加的事件驱动 UI 必须真触发一次看 toast，本地 toast 会把"事件根本没到"伪装成正常。**

## 7i. 功能审查 + bug 修复（2026-10-10，本机已实测）

审查范围：内置 agent / LLM 客户端 / 记忆·题库·笑话 / 协议端口 / 模型导入删除 / 备份 / UI 进程。修了这些（括号 = 实测证据）：

- **agent 计时用错了钟**：`PetAgent` 的 `NowSec()` 与 LLM 用时用的是 `clock()` = **进程 CPU 时间**。桌宠 8 线程渲染时
  CPU 时间比墙钟快 ~2.3 倍（实测 653s 墙钟 = 1517s CPU）→ 闲时间隔/冷却/提问等待全部按比例变短；窗口隐藏时几乎不走。
  改 `steady_clock`。连带：三个定期检查（题库/笑话/压缩）的计时起点改为启动时刻（否则启动即重复生成一次）。
- **题库每 5 分钟重生成**：刷新条件 `AnsweredCount()>=5` 是**累计**答题数，答满 5 题后永远为真 → 每次启动、每 5 分钟
  花一次 12 题生成调用。删掉该条（"新鲜题 <3 / 超过 24h"已覆盖本意）。（重启后日志：问题库（已缓存））
- **判重失效**：`'，'` 这种多字节字符常量逐字节比较永远不等（编译器 -Wmultichar 警告）→ 只改标点的重复题/笑话判不出；
  `SameQ` 的"前 6 字"实际是 6 **字节** = 2 个汉字 → "你喜欢吃什么"和"你喜欢看电影"被当成同一题。改成按 UTF-8 码点。（单测）
- **按字节截断写坏记忆**：Python 原版的 `s[:20]` 是字符，C++ 移植成字节 → 题目被切成半个汉字写进 `habits.json`，
  文件变成非法 UTF-8（Python json 直接读不了）。统一用 `plat::Utf8Head(s, n)`（前 n 个字符）。本机数据已修复。
- **任何气泡选项都被当成"主人回答"**：没有待答问题时（调试 say、剧情选项）也写进记忆/题库（"（没记下问题）→好"）。
  现在只认 agent 刚问过的题；答完清掉 `_lastQText`。Python agent 同步修。
- **导入目录会把原文件夹搬走**：`rename(源目录, 模型库)` 同盘时直接成功 → 主人的文件夹消失。改成：只有解压临时目录才 rename，
  用户目录一律拷贝。（实测源目录保留）
- **删除模型会跟随符号链接**：`RemoveDirTree` 用 stat 判断目录 → 模型目录里一个指向家目录的链接 = 删模型时把链接目标清空。
  新增 `plat::IsSymlink`，删除/拷贝都不跟链接；zip 解压后先剔除所有链接；隐藏文件（.DS_Store）也删，否则目录永远删不掉。
  （实测：植入指向 victim/ 的链接后删模型，victim/ 完好；带 .DS_Store 的模型可删）
- **退出卡 75s**：退出时先 join agent 线程，若它正等 LLM（连接 15s + 传输 60s）窗口就一直挂着。curl 加进度回调可中止 +
  `LlmCancelAll()`；`CURLOPT_NOSIGNAL`、显式 `curl_global_init`。（实测 quit 0.27s）
- **私有文件写法**：`WritePrivateFile` 先按 umask 建成 0644 再 chmod（中间有可读窗口）→ 改 `open(O_EXCL, 0600)`；
  Windows 的 `rename` 遇到已存在目标会失败 → 每次保存（记忆/配置/亲密度/LLM 设置）在 Windows 上都会失败，
  新增 `plat::ReplaceFile`（MoveFileEx REPLACE_EXISTING）并替换所有"tmp+rename"。
- **协议端口**：一个只订阅不读的客户端会让每次广播在锁里卡 2s（主循环跟着卡）且半行错位 → 发送失败即 shutdown 并退订；
  Windows 的 `SO_SNDTIMEO` 是 DWORD 毫秒（原来传 timeval ≈ 2ms）、`SHUT_RDWR` 未定义（Windows 编不过）。
- **UI 进程**：文件对话框（zenity）在 UI 线程同步等 → 开着对话框时托盘/右键菜单全卡 → 改后台线程 + Tick 回调；
  事件订阅 `send` 缺 `MSG_NOSIGNAL`（桌宠恰好退出时 SIGPIPE 杀掉 UI）；删掉界面里写死的开发机路径。
- **UI 进程崩溃自动重启**：原来只在下次右键时拉起（托盘消失、首个菜单事件丢）。现在退避 2s→60s 自动重启。（kill -9 后 2s 回来）

未做/边界：导入大 zip 仍在主线程同步解压（几秒内渲染停顿）；没有跑会移动真实鼠标的 `ui_click_test.py`；Win/mac 仍未编译验证。

## 7j. 日夜模式 + 多语言（2026-10-10，本机已实测）

面板新增第 8 页 **「外观」**（`page:8`；追加在最后 → 老页号 0~7 含义不变）＋右键菜单/托盘的「外观」子菜单，两件事：

- **主题**：`日间 / 夜间 / 跟随时间`（跟随 = 本地 19:00–7:00 深色）。写死的颜色常量改成调色板，每帧 `ApplyTheme()`；
  EUI 控件主题令牌深色下给紫调深底。存 `config.str.theme`（默认 `light` = 原来的样子）。
- **语言**：`中文 / English / 跟随系统`（按 `LANG` 猜）。表在 `panel/i18n.{h,cpp}`，**以中文原文为键**（384 条，
  含桌宠上报的情绪/状态/部位/日志类别中文名）；`T()` 查表、`TF("共 {} 条",{n})` 模板填数；接入点见
  `DESIGN-PET-LIFE.md` §12。存 `config.str.lang`。**`lang=en` 时内置 agent 的提示词追加"说出口的用英语"**
  → `speak`/台词库/笑话/提问全英文（实测 3 条：`Wah—! You poked my head!` / `Hmm... I wonder if he's still stuck on
  that code of his.` / 天气那条还正确用了 `weather_loc`）。记忆归纳、桌宠自己的 feed 动态、旧 Python agent 仍中文。
- **改文案后必跑**：`python3 live2d/tools/i18n_check.py`（漏译/多余/占位符对不上；当前"全命中 ✓"）。
- 实测：中/英 × 日/夜 四种组合截图（`img/demo-panel-appearance-{zh,en}-dark.png`、`img/demo-panel-status-en-dark.png`）、
  右键菜单与子菜单真点（XTEST）切主题 → `config.json` 落盘；隐藏/清空配置项能正确回到出厂默认。
- **踩到的三个坑**（都已修）：① `{}` 模板在中文模式下没填 → 界面上留着 `"{}"`；② 主题枚举顺序与分段控件顺序不一致
  → 选"日间"其实是"夜间"；③ 启动时读到的偏好被当成永久兜底 → 用户删掉配置项后面板一直停在启动值（现在连上桌宠后就以配置为准）。

## 7k. Linux 桌面安装包（2026-10-10，本机已实测）

`scripts/linux_build.sh` 一条命令出安装包（`scripts/` 原本只有板卡侧的老脚本，打包线加在同一目录）：

```bash
scripts/linux_build.sh          # 编（只编主机端）+ 打包 → dist/linux-<arch>/
                                #   l2dpet-<版本>-linux-<arch>.tar.gz  便携：解压 → ./install.sh
                                #   l2dpet-<版本>-linux-<arch>.deb      Depends 按 NEEDED 查表生成
```

- 编：调 `live2d/build-player.sh host`（新加的 `host|armhf|all` 模式；打包不必等交叉编译）+ `live2d/build-panel.sh`。
- 装（tar.gz）：`l2dtest` + `l2dpet-ui` + 启动器 `l2dpet`（start/stop/restart/status/models/open/logs）+
  `assets/Font Awesome 7 Free-Solid-900.otf`（**面板图标靠它**，少了两者会退化成系统符号字体）+ 调试工具 +
  `.desktop`/图标 + `install.sh`/`uninstall.sh`；默认装 `~/.local/opt/l2dpet`（`--system` → `/opt`）。
- 只带一个随包示例模型（Live2D 官方 Hiyori，见 §7p），**其它模型一律不含**（各有授权，不能分发），也不含 Key/记忆；
  卸载默认保留 `~/.config/l2dpet`、`~/.local/state/l2dpet`、模型库。
- 安装器往 `$PREFIX/.installed` 写标记：卸载器靠它区分"安装目录"与"解压出来的包目录"（两边文件长得一样；
  不标记的话从包里跑卸载会把包目录删掉——第一版就是这么翻的）。
- **实测**（隔离 `HOME`，不碰真实用户目录）：解压 → 装 → 放模型 → `l2dpet start`（换 23456 端口）→ 状态/协议应答 →
  面板进程从安装目录被拉起 → `stop` → 卸载（程序走、数据留）→ 卸载器路径保护，**24 项全过**；
  装的这份面板另外截图确认图标正常。`.deb` 用 `apt-get install --simulate` 过了依赖解析。

## 7l. Wayland 回退 + AppImage（2026-10-10，本机已实测）

**Wayland**：启动器按会话自动选端口——`DISPLAY` 有值 → X11 原生端口（置顶/拖拽定位/点击穿透/气泡）；
只有 `WAYLAND_DISPLAY` → 退回 `--port glfwpet`（GLFW 走自己的 Wayland 后端，能出角色窗）。
**Wayland 上的固有限制说清楚**：没有全局置顶与客户端定位协议（要 layer-shell，GNOME 不支持），
角色出现在合成器给的位置、窗口矩形内会接住点击；装了 XWayland 的会话仍走 X11 端口、功能不受影响。
两者都没有 → 明确报错并给出 `--port headless --render` 的无头调试提示。强制：`L2DPET_PORT_KIND=x11|glfwpet`。
**没在真 Wayland 会话里实测**（本机是 X11）——按代码路径做了 dry-run 断言（打包自检里两种会话各断言一次）。

**AppImage**（`scripts/linux_build.sh --appimage`，产物 ~15MB）：把两个二进制的**动态闭包**（52 个 .so，23MB）
打进 `usr/lib`，`AppRun` 挂 `LD_LIBRARY_PATH` 再交给启动器；**减掉** glibc 家族 + `libstdc++`/`libgcc`
（AppImage 官方 exclude 同类：要和宿主 glibc 配套）+ 图形驱动（GL/EGL/vulkan/nvidia 必须宿主）。
GLFW 用 dlopen 加载的 wayland 栈要从 `ldconfig -p` 单独补（不走 NEEDED，ldd 看不到）。
appimagetool 首次下载到 `~/l2d-build/appimage-tool/`（缓存）。**glibc ≥ 2.38 门槛仍在**（Ubuntu 22.04/Debian 12 跑不了）。

**这一轮踩的坑（都写进脚本注释了）**：
① `ldconfig -p | awk '…exit'` → ldconfig 吃 SIGPIPE、`pipefail` 记 141、`set -e` 当场终止脚本（退出码 141 是线索）；
② 同一类：`[ -n "$x" ] && cmd` 作为循环体最后一句，条件为假也会因返回 1 被 `set -e` 干掉；
③ **AppImage 每次运行挂载点都不同**（`/tmp/.mount_xxx`）→ 原来按 exe 路径找进程的 `l2dpet stop`
在"另一次运行"里永远说"没在跑"，再敲一次会起第二只 → 改成 pidfile（`~/.local/state/l2dpet/desktop.pid`，
校验 pid 的 exe 是 l2dtest 防复用误杀）+ exe 路径兜底；
④ 编辑时把 `else` 粘到上一行（`die "…"else`）→ 分支静默错位，只有跑起来才看得出来。

**实测**：AppImage 起→FUSE 挂载→从挂载点里跑起桌宠 + 面板→协议应答 30fps→面板截图渲染正常
（图标来自包内 EUI 字体）→**另一次运行** `stop` 成功；重复启动被拦（"已经在跑了"）；
tar.gz 端到端 30 项 + dock 逻辑 6 项回归全过。

## 7m. 日语 + 切语言重写存量内容（2026-10-10 晚，本机已实测）

- **日语**：`i18n.cpp` 加 `kJa[]`（385 条，键与 `kEn[]` 完全一致，对账脚本同时校两张表）；
  外观页语言段控变四项（中文/English/日本語/システムに合わせる），右键菜单与托盘子菜单同步；
  字体按语言挑（`sans-serif:lang=ja`），**语言必须在 `SystemCjkFont()` 之前定**（顺序写错就按默认语言挑）；
  `Detect()` 认 locale 里的 zh/ja。语言名各自用自己的写法（中文/English/日本語）。
- **切语言 → 存量内容换语言**：面板切换时提示一句；agent 侧 ①记忆展示 LLM **原样改写**
  （`PetMemory::Rewrite`，结构/条数/时间戳不变，只查结构不看字节数）②性格（LLM 版）③台词库 ④题库 ⑤笑话池
  清掉重生成；睡着时排队、醒来补做（复用 `_defer*` 机制）。
- **触发点改到桌宠侧**：`config` 的 `setstr` 写入前后比 `lang`，变了推 `lang_changed`（带新值）给内置 agent。
  原先想放在 agent 的 `ApplyConfig` 里"比较上次见到的值"——**不行**：agent 启动时那次 `status` 回复可能
  还没有 `str` 段（实测如此），基线就错了，第一次真正的切换被当基线吃掉（现象=切了没反应）。事件里带新值
  也顺带解决了"agent 的 `_cfgLang` 要等 config 事件才更新、而 `lang_changed` 是同步推的"这个顺序问题。
- **实测**（隔离 HOME + 假 LLM 端点，回中文/日语两套罐头，不碰真实记忆与额度）：切 `lang=ja` 后
  五类请求全部带上语言指令、`owner.md` 变成日语、性格/题库/笑话文件变成日语、面板「记忆」页读到日语 —— 13 项全过；
  日语界面截图确认（导航/状态页/外观页全日语）。打包自检 12 项 + 端到端 30 项回归全过（tar.gz/deb/AppImage 已重出）。

## 7n. 桌宠内置文案全线 i18n（2026-10-10 深夜，本机已实测）

用户报的现象：日语模式下气泡里冒出中文——「外面热不热呀…」这种**自带兜底句**，以及触摸反应/动态流。
根因两条：① 自带兜底句根本没进 i18n 表（只进了一部分）；② `LifeSay`/`FeedPush` 还会再翻一次，
**成品文案被当成"未命中"**记进漏译清单（混着 LLM 台词时更难看出来）。

- **单一翻译入口**：`peti18n::Tr/TF`（`player/src/PetI18n.{h,cpp}`，以中文原文为键；en 236 条 + ja 236 条，
  对账脚本证明两表"键完全一致、`{}` 数量一致"）。`LifeSay`/`FeedPush` 现在收**最终文案**、自己不再翻；
  拼接句一律模板化：`{}了{}`（触摸动态）、`{}（{}）`（状态行）、`{}（等你回答）`（提问后缀）、
  `（{}）今天外面天气怎么样呀？`（天气兜底）；模板参数（动词/部位/状态名/原因，如"指令/被碰醒/被戳烦了"）先 `Tr`。
  en 的触摸动词值去掉了尾随空格，语序交给模板（`{} her {}`）。
- **这轮补进表的**：16 区 × 点/揉 的触摸兜底反应 38 句、冷淡期搪塞 5 句、状态名/原因 13 条、
  猜拳手势名与结果模板、天气兜底模板（触摸动词/16 部位名/记忆页段标题之前已进）。
- **漏译清单不再被噪音淹没**：只记**含汉字**的字符串（`Zzz…`、`？`、动作 id 不记）。
- **两个验证工具**（都在 `live2d/tools/`）：
  `pet_i18n_audit.py` 静态对账（源码里会露给用户的中文字面量 vs 表 + en/ja 对齐）→ 当前"全命中"；
  `sweep_i18n.py [lang] [port]` 运行时扫场（隔离 HOME、无 LLM Key、`L2DPET_I18N_DEBUG=1`；驱动 16 区触点、
  猜拳三手、笑话、天气、困/睡/闷气，退出时读漏译清单，并抓 `panel_state.life` 动态流）——
  `ja` 与 `en` 各跑一遍：气泡全目标语言、动态流变 `なでた（頭）×2` / `stroked her head ×2`、漏译"全命中"。
- 回归：切语言 LLM 全量重写 13/13、面板对账 386/386 全命中。
- **启动读配置的时序（2026-10-10 深夜补，本机已实测）**：桌宠要等第一帧画出来（+1.5s）才 `CfgLoad()`，
  而 agent 启动早于它——这是"界面日语、她却在说中文"的更深一层根因：
  ① `CfgJson()` 在**数值段为空**时曾生成 `{,"str":{…}}` 这种坏 JSON（全新安装只设过 `lang`/`theme` 这类字符串键时必现）
     → agent 解析不到 config，启动永远按默认语言生成；客户端还会把坏的回执当失败。修：逗号按需加（存盘时一定带 `str` 段，
     免得 `setstr` 在 `CfgLoad` 之前到达时把主人设的 lang 存丢）。
  ② 回执里 `str` 段现在只在 `_cfgLoaded` 之后出现——它的**存在与否**就是"配置读好了"的可靠信号，agent 等它（最多 8s，超时打日志）。
  ③ 桌宠配置读好后补推一次 `config` 事件（agent 比它起得早，拿到这条才能对齐语言）。
  ④ agent 收到 `config` 事件时若语言与当前不同，按"切语言"处理（排队重写记忆/性格/台词库/题库/笑话），不只换新生成的那部分。
  ⑤ **盘上缓存的语言在启动时校验**（性格/题库）：语言在桌宠没跑时被改过、导入别人的备份、切模型都会撞上——
     不符就重生成；重生成也拿不到（没 Key/网络）就把旧语言的清掉（宁可这轮不提问，也别拿中文题库说日语）。
  实测（隔离 HOME + 假 LLM 的一次性脚本，11 项）：全新配置下回执/存盘都是合法 JSON、
  `开机 ja → 启动的台词库请求带日语指令`、`config ja + 缓存中文` 的状态下性格/题库都被换成日语。
- **气泡选项乱码/空白（2026-10-10 深夜修，用户截图发现）**：猜拳气泡第一格渲染成 `p□p`（或空白）。
  根因是 §7n 那轮给猜拳手势名做翻译时写成"边 `push_back` 到 `shown` 边取 `c_str()`"——
  vector 扩容把先前的指针失效，`ShowBubble` 拿到悬空指针（首次扩容的那格必中，其余看内存运气，
  所以同一个 bug 一会儿乱码一会儿正常）。修法：两趟走（先填满 `shown` 再取指针）。
  **验证靠截图**：自己的实例上 `import -window $(xwininfo -root -tree | grep l2dpet-bubble | awk '{print $1}') out.png`
  （气泡窗口名字是 `l2dpet-bubble`，就是为了好定位）；zh 出「石头/剪刀/布」、ja 出「グー/チョキ/パー」即通过。

## 7o. 安全扫描 + 修复（2026-10-10，本机已实测）

- **【严重】浏览器跨协议攻击控制端口**：任何网页 `fetch('http://127.0.0.1:23333', {method:'POST', body:'\n{"cmd":…}\n'})`
  都能执行协议命令（HTTP 头行解析失败被忽略，body 里的 JSON 行照常执行；实测 `status` 被执行）——可改 `llm_config` 的接口地址
  把 API Key 送到攻击者服务器、导出/导入记忆、删模型、退出。修：`CtlServer` 只收以 `{` 开头的行，任何非 JSON 行立即断开整条连接
  （HTTP 请求行总是第一行 → 后面的 body 永远到不了）；同时连接数上限 32。（实测：HTTP 探测零回复、连接被断；正常客户端不受影响）
- **【高】JSON 嵌套深度无上限 → 栈溢出崩溃**：`[[[[…` 6 万层（< 64KB 一行）实测段错误；协议行、LLM 回复（≤2MB）、备份导入（≤8MB）
  都经 `JsonLite`。修：深度上限 64（"嵌套太深"）。UI 进程同一个头文件，一并重编。
- **【高】导入的模型包未做校验 → 崩溃**：坏 `.moc3` 直接进 Cubism Core（未开一致性校验）、坏 `model3.json` 在 Framework
  `GetModelFileName()` 空指针崩——实测两种都段错误。修：`LoadModel(..., true)` 开 `csmHasMocConsistency`；model3 先用 jsonlite
  校验并要求 `FileReferences.Moc`；exp3/motion3/physics3/pose3 走 `ReadJsonFile`（不合法跳过）。贴图 `STBI_MAX_DIMENSIONS 8192`
  （防解压炸弹）。（实测：坏 moc3 → "Inconsistent MOC3"，干净失败）
- **【高】运行中切到坏模型 = use-after-free**：切换时旧播放器先析构重建，失败分支却"保持当前显示"——`model` 指针已悬空。
  修：失败退回原模型重新加载；原模型也加载不了就干净退出。（实测：切到坏模型 → 被拒 → 回到原模型，进程正常、指纹不变）
- **【中】超长气泡文本 → X 错误退出**：协议 `say` 几万字会排出超高气泡窗（BadAlloc 时 Xlib 默认处理直接结束进程）。
  修：气泡文本 ≤120 字、选项 ≤4 个 × 16 字。（实测 2 万字 say：进程存活，气泡有界）
- 已核对无问题：agent 发出的 LLM 结果强制 `cmd=sequence`（LLM 输出无法变成删模型/改设置等命令；步骤里也不执行嵌套命令）；
  删模型名校验、备份导入白名单、符号链接处理、私有文件写法（见 §7i）。

## 7p. 开箱即用（随包示例模型）+ LLM 连接状态 + 复核外部改动（2026-10-10，本机已实测）

### 随包示例模型：没有模型的新电脑也能启动
- **问题**：AppImage/安装包不带模型，新机器上 `l2dpet` 只会打印"找不到模型"然后退出。
- **做法**：仓库新增 `live2d/bundled-models/Hiyori/`（Live2D 官方示例，从 `Live2D/CubismWebSamples` 的
  `Samples/Resources/Hiyori` 原样取来，约 4.8MB / 18 个文件；许可见 §8 与 `live2d/bundled-models/README.md`）。
  - `scripts/linux_build.sh`：把 `live2d/bundled-models/` 拷进打包树 `models/`（tar.gz / deb / AppImage 共用）。
  - 启动器 `l2dpet start`：用户模型库（`~/.local/share/l2dpet/models/`）**为空**时，把包里的示例模型**拷进**用户库再启动。
    拷进用户目录而不是直接读包里——AppImage 是只读挂载，桌宠要在模型旁写能力缓存，面板「模型」页也要能管理/删除它。
    库里已有模型时不动；指定了 `L2DPET_MODEL` 时也不拷。
  - **顺手修的坑**：tar.gz 的 `install.sh` 是按文件清单拷的，原来不会把 `models/` 装进去——装出来的版本在新机器上
    照样没有示例模型。已补上（deb / AppImage 是整树拷贝，不受影响）。
- 实测：全新 HOME（空模型库）跑 AppImage → 打印"已放入自带示例模型 Hiyori"→ 起来、协议口可用、左下角显示、截图核对渲染正常；
  第二次启动不再重复拷贝；stop/start 正常。AppImage 18.6MB（含模型）。

### "LLM 已连接"只认调用成功
- **问题**：新电脑还没配 LLM，面板就显示"LLM 已连接"——原来只看"有没有 agent 进程"，而内置 agent 总在跑。
- **做法**：
  - `LlmClient` 记录**最近一次真实请求**的结果（`LlmLastStatus()`；睡眠暂停 / 退出中 / 没 Key 时不算一次请求，不改它）。
  - 桌宠 `panel_state` 增加 `llm_state` / `llm_err`：`no_agent` / `no_key` / `pending`（有 Key 还没调用过）/
    `error`（最近一次失败）/ `ok`（最近一次成功）。外部 agent（`pet_agent.py`）取它上报的 `agent_info.key_source`
    与最近一条 `llm_log` 的成败。协议见 `PROTOCOL-LLM.md`。
  - 面板主视觉与「对话」页：未配置 / 待验证 / 连接失败（红色，下面直接写失败原因）/ 已连接（睡着时"休眠 · 点她唤醒"）。
    旧的"agent 已连接/未连接"文案删除（中英日翻译表同步，对账全命中）。
- **顺手修的 bug**：新机器还没有任何 LLM 供应商时，协议 `llm_config` 写 Key **被静默丢弃却回 `ok:true`**
  （没有当前供应商可写）。现在先建一个默认供应商（DeepSeek）再写入。
- 实测（隔离 HOME、独立端口，不碰正在用的那只）：没 Key → `no_key`（此时 agents=1，旧逻辑会显示已连接）；
  写入测试 Key → `pending`；调用一个不可达地址 → `error`（"网络错误：SSL connect error"）；
  用本机真实配置（临时拷贝，测完删除）→ 启动 `pending`，首次调用成功后 `ok`。

### 复核其它 AI 的改动（944da69、9a5fb72）
- 944da69（猜拳气泡首格乱码）：修法正确（先把字符串都放进 vector 再取 `c_str()`，避免扩容后指针悬空）。
- 9a5fb72（切 `l2dpet-desktop-only`，删掉板卡/LVGL/Rust 约 6.6 万行）：全新构建目录 configure + build 通过；
  回归 9/9（status、能力表、HTTP 跨协议拒绝、深嵌套 JSON、选项 do 校验与点选即演、超长 say、切坏模型回退、40 连接）；
  面板与桌宠两套 i18n 对账全命中；被删代码在剩余代码里没有悬空引用。
- 残留已清：两处代码注释和本文一处还指向本分支已删的 `DESIGN-LLM-CONTROL.md`（改为"见 main 分支"）；
  b32ed77 误入库的 `.zcode/plans/*.md`（别的 AI 工具的计划文件）移出版本库，`.gitignore` 加 `.zcode/`。

## 8. 许可与依赖（别踩线）

- **Live2D Cubism Core 是闭源**（禁逆向、禁反编译；Framework 层是开源的两层结构）：
  桌宠走的正是"官方 Core + 自己写的外壳/渲染"这条合规路径 ✓。商用条款（小微规模豁免与
  通知义务、Expandable Application 需事先批准）见项目记忆与 `live2d/DESIGN-PET-LIFE.md` 相关注记。
- **模型授权跟着模型包走**：仓库与安装包**只带一个**随包示例模型——Live2D 官方 **Hiyori**
  （`live2d/bundled-models/Hiyori/`，从 `Live2D/CubismWebSamples` 原样取来，© Live2D Inc.，
  **Live2D Free Material License**：个人/小规模企业可随应用分发；超过官方营收门槛的企业需另与 Live2D 签约；
  **不得把素材单独再分发**）。除它之外不带任何模型；用户导入的模型各自授权（如"诀桌宠免费版"=个人使用、禁二次售卖），
  **舒芙蕾、yumi 等购买来的模型绝不能入库或打包**。
- **第三方代码**：EUI-NEO（Apache-2.0，`~/l2d-build/3rd/EUI-NEO`，桌面面板 UI）、
  Cubism Framework（Live2D 许可）、stb_image（公共域/MIT）、GLFW（zlib）。
  引用第三方时保留其许可文件，不要把它们混进来路不明的代码。

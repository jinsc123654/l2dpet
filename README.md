# l2dpet — Live2D 桌宠（三端桌面：Windows / Linux / macOS）

一只住在桌面上的 Live2D 角色：透明置顶窗口、可拖动/缩放、点击穿透、摸它会有反应，
会犯困睡觉、有内在状态与情绪；接上 LLM 就有记忆、性格、台词库与互动小功能；
托盘图标 + 右键菜单 + 设置面板（EUI-NEO）。

> **本仓库专注 Windows / Linux / macOS 桌宠代码。**
> 早先的板卡/LVGL 线（RK3506 上的 LVGL 桌面、共享内存帧通路、交叉编译部署、
> Rust 核心实验）不包含在本仓库中；历史代码见原仓库
> [vanxoak_lvgl](https://github.com/jinsc123654/vanxoak_lvgl) 的 `main` 分支。

## 从哪里读起

| 想要 | 看这里 |
|---|---|
| 交接/现状/怎么跑/坑 | **`HANDOFF-DESKTOP.md`**（接手先读这个） |
| 协议（桌宠 ↔ 面板/外部工具，JSON-Lines 23333） | `PROTOCOL-LLM.md` |
| 行为设计（内在状态、互动、记忆、语言、LLM 提示词） | `live2d/DESIGN-PET-LIFE.md` |
| 动作层（关节骨架、手臂绑定） | `live2d/DESIGN-ARM-JOINTS.md` |
| 构建与打包（Linux 安装包 / Win、Mac 交叉编译） | `live2d/BUILD-CROSS-PLATFORM.md`、`scripts/README.md` |
| 目录与依赖 | `live2d/README.md` |

## 快速开始（Linux）

```bash
live2d/build-player.sh host && live2d/build-panel.sh    # 编桌宠 + 面板
scripts/linux_build.sh --appimage                       # 打安装包（tar.gz / .deb / AppImage）
# 或者开发时直接起：
~/l2d-build/build-host/l2dtest <模型.model3.json> 0 0.0333 --pet --ctl 23333 --order static
~/l2d-build/build-panel/l2dpet-ui
```

模型放到 `~/.local/share/l2dpet/models/<名字>/<x>.model3.json`（或用面板"模型"页导入 zip）。
**开箱即用**：安装包自带一个示例模型（Live2D 官方 **Hiyori**），第一次启动时模型库若是空的会自动放进去，
没有任何模型的新电脑也能直接跑起来。

LLM 可选：在面板「对话」页填 API Key（存 `~/.config/l2dpet/llm.json`，0600），或设环境变量 `LLM_API_KEY`；
不配也能跑（内置台词 + 本地互动）。面板上的 **"LLM 已连接" 只在最近一次调用真正成功后才显示**——
没配 Key 显示"未配置"、配了还没调用显示"待验证"、调用失败显示"连接失败"并给出原因。

## 许可

本项目**自己的代码**（桌宠 player、面板 panel、脚本与工具）用 **MIT**，见仓库根 `LICENSE`（作者：第九个下弦月）。
第三方组件各自许可、**不在 MIT 授权范围内**：EUI-NEO（Apache-2.0）、GLFW（zlib）、FreeType（FTL/GPLv2）、
glad/stb/zlib/libpng/md4c/yyjson/tray、libcurl（curl License）等，许可原文随源码分发。

**不建议也不允许改成 GPL/AGPL**：GPL 系要求整个组合作品都能按 GPL 分发，而下面的 Live2D Cubism Core 是
专有闭源、与 GPL 不兼容；静态链接进同一个可执行文件后，分发二进制会同时违反 GPL 与 Live2D 的条款。

Live2D Cubism Core 闭源（走官方 Core 合规路径）；随包示例模型 Hiyori 按 Live2D Free Material License 分发
（`live2d/bundled-models/README.md`），其它模型各有授权、不入库；详见 `HANDOFF-DESKTOP.md` §8
与 `live2d/README.md`。

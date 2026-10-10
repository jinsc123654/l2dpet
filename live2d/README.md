# Live2D 桌宠 l2dpet（三端桌面：Windows / Linux / macOS）

桌面上住一只会动的 Live2D 角色：透明置顶窗口、可拖可缩放、能摸能戳、会犯困睡觉，
说得出话（LLM 大脑 + 跨会话记忆）、有托盘图标与设置面板。**本目录 = 桌宠这条线的全部代码。**

> **本分支 `l2dpet-desktop-only`：只保桌面三端。** 板卡/LVGL 那条线（RK3506 上的 LVGL 桌面、
> 共享内存帧通路、交叉编译部署、Rust 核心实验）已从本分支移除；那部分历史切 `main` 分支。

## 目录

```
live2d/player/    播放器/桌宠主程序（C++17；官方 Cubism Core + GL 渲染）
live2d/panel/     面板/托盘/右键菜单（EUI-NEO，进程 l2dpet-ui）
live2d/tools/     调试/对账工具（pet_client.py、i18n 检查、能力生成、扫场脚本…）
build-player.sh   编播放器 → ~/l2d-build/build-host/l2dtest
build-panel.sh    编面板   → ~/l2d-build/build-panel/l2dpet-ui
BUILD-CROSS-PLATFORM.md  三端构建/打包说明（Linux 安装包、Win/Mac 交叉编译）
DESIGN-PET-LIFE.md       行为设计（内在状态、互动、记忆、语言…）
DESIGN-ARM-JOINTS.md     动作层：关节骨架与手臂绑定
```

仓库根还有：`PROTOCOL-LLM.md`（桌宠 ↔ 面板/外部工具的 JSON 协议）、
`HANDOFF-DESKTOP.md`（交接文档：做了什么、怎么跑、坑在哪）、
`scripts/linux_build.sh`（打 Linux 安装包）。

## 30 秒跑起来（Linux）

```bash
live2d/build-player.sh host && live2d/build-panel.sh     # 编两个二进制
# 把模型放到 ~/.local/share/l2dpet/models/<名字>/<x>.model3.json
scripts/linux_build.sh --appimage                        # 或者直接打包安装
```

开发时不用打包，直接起（见 `HANDOFF-DESKTOP.md` §1 的"跑法"）：

```bash
~/l2d-build/build-host/l2dtest <模型.model3.json> 0 0.0333 --pet --ctl 23333 --order static
~/l2d-build/build-panel/l2dpet-ui                        # 面板（托盘/菜单/设置）
```

## 依赖

- CMake + C++17 编译器；Linux 需 X11/Xft/fontconfig 开发包（桌宠气泡用）、可选 SDL2。
- **Live2D Cubism SDK for Native**（Core 闭源 + Framework 开源）：把 SDK 放到
  `~/l2d-build/live2d-sdk`（或用 `CSM_SDK=<目录>` 指定），`build-player.sh` 会自动同步到构建树。
- 面板的 UI 栈 **EUI-NEO** 已作为**本地源码随仓库分发**（`live2d/3rd/EUI-NEO`，本工程的改动已落在
  那些文件里），不需要另外准备、不依赖 git/网络；GLFW/glad/freetype 也取自它的 `3rd/`。
  想用外部副本：`-DEUI_NEO_DIR=<目录>`（外部检出需落在 `panel/eui-patches/README.md` 记的提交上）、
  `-DL2D_3RD_DIR=<目录>`。
- LLM（可选）：`~/.config/l2dpet/llm.json` 配供应商/模型/Key，或环境变量 `LLM_API_KEY`；
  不配也能跑（只用内置台词）。

## 许可（重要）

- **Live2D Cubism Core 闭源**：禁逆向/反编译；商用条款见 SDK 许可（小微豁免与通知义务等）。
  本项目走"官方 Core + 自研外壳"的合规路径。
- **模型**：仓库与安装包只带一个随包示例模型——Live2D 官方 **Hiyori**（`live2d/bundled-models/`，© Live2D Inc.，
  Live2D Free Material License，可随应用分发、不得单独再分发），保证没有模型的新机器也能启动；
  其它模型各自的授权跟着模型包走，一律不入库、不打包。
  详见 `HANDOFF-DESKTOP.md` §8。

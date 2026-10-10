# 三端构建指南（Linux / Windows / macOS）

桌宠 = **两个可执行文件** + 一个 Cubism SDK 构建树：

| 产物 | 源码 | 作用 |
|---|---|---|
| `l2dtest`（桌宠进程） | `live2d/player/` | 角色窗口 + 渲染 + 输入 + 说话气泡 + 特效 + 内置 LLM agent + 协议端口 23333 |
| `l2dpet-ui`（UI 进程） | `live2d/panel/`（EUI-NEO） | 托盘 / 右键菜单 / 状态与设置窗口（纯协议客户端，随桌宠自动拉起） |

两者只通过 `127.0.0.1:23333` 的 JSON Lines 协议通信（`PROTOCOL-LLM.md`），可各自单独构建/运行。

---

## 0. 前置：Cubism SDK 构建树（三端共用一份源码）

> **需要外部准备 Cubism SDK 的 Framework 源码**。Cubism Core 头文件与预编译库已有仓库副本，构建时优先使用。
> UI 栈 EUI-NEO 与 GLFW/glad/freetype 已经作为本地源码随仓库分发（`live2d/3rd/EUI-NEO`，
> 含本工程的改动），因此三端都能离线直接编。

需要官方 **CubismSdkForNative**（本仓库不附带）。官网下载页（要同意 SDK 许可）：
<https://www.live2d.com/en/sdk/download/native/> —— 直链形如
`https://cubism.live2d.com/sdk-native/bin/CubismSdkForNative-5-r.5.zip`。

解包后放在**任意目录**，构建脚本会把它同步进本地构建树（`$WORK/sdk-tree`）。本工程按
**5-r.5 的目录布局**取物料：

```
<csm-root>/                          # = CubismSdkForNative-5-r.5/
  Core/include/Live2DCubismCore.h    # 头文件
  Core/lib/linux/x86_64/libLive2DCubismCore.a
  Core/lib/macos/{arm64,x86_64}/libLive2DCubismCore.a
  Core/lib/windows/x86_64/<工具集>/Live2DCubismCore_{MD,MDd,MT,MTd}.lib
                                     #   <工具集> = 141 (VS2017) / 142 (VS2019) / 143 (VS2022+)
  Framework/                         # 开源 C++ 框架（构建时注入软渲染后端）
  Samples/OpenGL/thirdParty/stb/stb_image.h   # 软渲染的纹理解码（脚本自动提取）
```

> ⚠️ **Windows Core 不是扁平路径**：5-r.5 起按 MSVC 工具集分子目录，而
> 构建优先使用仓库内的 `Live2DCubismCore_MT.lib`；外部 SDK 作为后备。
> 构建脚本会挑 `_MT` 版补成扁平路径，与程序的 `/MT` 静态运行库一致。
>
> 📁 默认位置：SDK 解包目录放 `~/l2d-build/live2d-sdk/` 下（脚本自动找
> `CubismSdkForNative-*`），或用 `CSM_SDK=<解包目录>` 指定。

Linux 上 `live2d/build-player.sh` 会自动把 SDK（`CSM_SDK`，默认 `~/l2d-build/live2d-sdk`）同步到
`~/l2d-build/sdk-tree`（`CSM_BUILD_ROOT`）并注入 `player/soft-renderer`（软渲染后端）；
sdk-tree 已就绪时可以直接编（不要求 SDK 原目录在场）。Windows 的一键脚本会自动做同样的准备；macOS 手工准备（rsync 或复制），
然后把 `-DCSM_BUILD_ROOT=<csm-root>` 传给 CMake 即可——**Framework 与软渲染后端三端同源**。

---

## 1. Linux

```bash
live2d/build-player.sh          # 产出 ~/l2d-build/build-host/l2dtest（x86_64）
live2d/build-panel.sh           # 产出 ~/l2d-build/build-panel/l2dpet-ui（EUI-NEO）
```

运行（桌面桌宠，X11 原生端口）：

```bash
~/l2d-build/build-host/l2dtest <模型.model3.json> 0 0.0333 --pet --ctl 23333 --size 360x640 --order static
```

可选端口：

```bash
... --port glfwpet ...   # GLFW 跨平台端口（与 X11 端口等价；Windows/macOS 上用这个）
... --port headless ...  # 无窗口（纯解算/落帧，调参用）
```

内置 LLM agent 随桌宠自动启动（读 `~/.config/l2dpet/llm.json` 或环境变量 `LLM_API_KEY`）；`L2D_NO_AGENT=1` 关闭。

### 1.1 打包成 Linux 安装包（tar.gz / .deb）

```bash
scripts/linux_build.sh                 # 编（只看主机端）+ 打包 → dist/linux-<arch>/
scripts/linux_build.sh --appimage      # 额外出一个 AppImage（自带依赖闭包，双击即用）
scripts/linux_build.sh --skip-build    # 用现有二进制重新打包（改打包内容时快）
scripts/linux_build.sh --no-deb        # 不要 .deb
L2D_JOBS=4 scripts/linux_build.sh       # 限制编译并行数
```

| 产物 | 用途 |
|---|---|
| `l2dpet-<版本>-linux-<arch>.tar.gz` | 便携包：解压 → `./install.sh`（默认装 `~/.local/opt/l2dpet`，不需要 root；`--system` 装 `/opt`） |
| `l2dpet-<版本>-linux-<arch>.deb` | Debian/Ubuntu：装 `/opt/l2dpet` + `/usr/bin/l2dpet` + 菜单项；`Depends` 由 `dpkg-shlibdeps` 根据符号生成**最低版本**，如 `libc6 (>= 2.38)`；t64 改名库保留带版本的备选（`libcurl4 \| libcurl4t64`），不兼容的旧系统会在安装时被拒绝 |
| `l2dpet-<版本>-x86_64.AppImage`（`--appimage`） | 单文件、**不装不找依赖**：把两个二进制的动态闭包（52 个 .so，约 23MB）打进 `usr/lib`，`AppRun` 挂 `LD_LIBRARY_PATH` 再交给启动器。约 15MB。目标机直接 `chmod +x` 双击/运行即可（需要 `libfuse2`；没有就 `--appimage-extract-and-run`） |

包内容见下；AppImage 里**没有** install/uninstall 脚本（不需要装），其余一致。

打 `.deb` 需要 `dpkg-dev`（`dpkg-deb` + `dpkg-shlibdeps`）；无法解析依赖时会停止，
不会生成缺少版本约束的包。AppImage 工具与 type2 runtime 均缓存到 `$L2D_WORK/appimage-tool/`；
离线时可预先放入对应架构的工具与 `runtime-<arch>`，或用
`APPIMAGE_RUNTIME_FILE=<本地运行时文件>` 指定运行时。

**AppImage 的桌面入口怎么来**（构建时做不到、只能运行时请求创建）：AppImage 是单文件，构建时无法把
菜单项放进用户的 `~/.local/share/applications`。所以 `--appimage` 只把 `l2dpet.desktop` / `l2dpet.png` /
`.DirIcon` 打进包内（给 AppImageLauncher/appimaged 这类集成工具用），真正的入口由**启动器在第一次
`start` 时幂等创建**：`~/.local/share/applications/l2dpet.desktop`（`Exec=` 指向该 .AppImage 文件本身）+
`~/.local/share/l2dpet/l2dpet.png`（图标必须拷出挂载点）+ `~/.local/bin/l2dpet`（转发脚本）。
移动/改名 AppImage 后下次运行会自动更新（含启动器内容——只看 `[ -x ]` 看不出"指向死路径"）；
`L2DPET_NO_INTEGRATE=1` 跳过。打包自检里断言了这套文件真的被创建（隔离 HOME 下 dry-run 触发）。

**AppImage 里带什么、不带什么**：带——两个二进制、`assets/`（EUI 图标字体 + 面板兜底中文字体）、
启动器、调试工具，以及**除下列之外**的全部动态依赖（X11/Xext/Xft/fontconfig/freetype/xcb、glib/gio/gobject、
libcurl 及其依赖、libdrm、wayland 栈…）。
不带（必须用宿主，AppImage 官方 exclude 同类）：**glibc 家族**（ld-linux/libc/libm/…）与 `libstdc++`/`libgcc`
（要和宿主 glibc 配套）、**图形驱动**（GL/EGL/GLX/vulkan/nvidia）——所以二进制仍要求在 **glibc ≥ 2.38**
的系统上跑（Ubuntu 23.10+/Debian 13+/Fedora 39+；Ubuntu 22.04、Debian 12 不行）。想要更老也跑，得换成
在目标系统/老容器里编。

**会话（X11 / Wayland）**：启动器按会话自动选端口——`DISPLAY` 有值 → X11 原生端口（置顶/拖拽定位/
点击穿透/气泡，功能最全）；只有 `WAYLAND_DISPLAY` → 退回 GLFW 端口（GLFW 走它的 Wayland 后端，
能出角色窗）。**Wayland 上的固有限制**：没有全局置顶与客户端定位协议（要 layer-shell，GNOME 不支持），
所以角色出现在合成器给的位置、窗口矩形内会接住鼠标点击；装了 XWayland 的会话（多数发行版默认）因为
`DISPLAY` 存在，仍走 X11 端口、功能不受影响。强制指定：`L2DPET_PORT_KIND=x11|glfwpet`。

包里：`l2dtest`（桌宠，含内置 agent）+ `l2dpet-ui`（面板）+ 启动器 `l2dpet`
（`start/stop/restart/status/models/open/logs/version`）+ `assets/`（EUI 的**图标字体**——面板图标靠它，
别漏，少了两者会退化成系统符号字体）+ 调试工具 + `.desktop`/图标 + `install.sh`/`uninstall.sh`。

包里带官方示例模型 Hiyori；其它模型（各有授权）、LLM Key/记忆不随包分发（用户数据在 `~/.config/l2dpet`）。
卸载默认只删程序，配置/存档/模型库都留着（`--purge` 才动数据；`--purge-models` 另外加）。

用户侧流程：`l2dpet`（模型库为空时自动放入随包示例模型 Hiyori；自己的模型用面板「模型」页导入）→ 托盘图标进设置面板。

**图标与 Dock 行为**（常被问到，一次说清）：

| 位置 | 装完有没有 | 机制 |
|---|---|---|
| 应用菜单 / 搜索（Activities） | ✅ 有 | `~/.local/share/applications/l2dpet.desktop` + `hicolor/128x128/apps/l2dpet.png`，并跑 `update-desktop-database`/`gtk-update-icon-cache` |
| Dock/任务栏里**运行中**的窗口 | ✅ 认成本应用（显示 `l2dpet.desktop` 的名字与图标） | 面板窗口的 `WM_CLASS=l2dpet-ui`（EUI 补丁里写死）→ 靠 .desktop 的 `StartupWMClass=l2dpet-ui` 匹配；**角色窗口不出现在 dock**：`_NET_WM_STATE` 带 `SKIP_TASKBAR`+`SKIP_PAGER`、`_NET_WM_WINDOW_TYPE=UTILITY`（桌宠该有的行为，否则任务栏会多一条） |
| Dock 上**常驻固定**（收藏夹） | ❌ 默认没有，`./install.sh --dock` 可加 | GNOME 写 `org.gnome.shell favorite-apps`（幂等；卸载会摘掉）；其它桌面环境按提示手动右键固定 |
| 桌面图标（`~/Desktop` 双击启动） | ❌ 默认没有，`./install.sh --desktop-icon` 可加 | 拷 `.desktop` 到桌面 + `chmod +x` + `gio set metadata::trusted true`（GNOME 的桌面图标扩展只显示这个目录里的文件） |
| 登录自启 | ❌ 默认没有，`./install.sh --autostart`（`--system` 自带） | `~/.config/autostart/l2dpet.desktop` |

`.deb` 只装菜单项（Debian 政策：包不替用户建桌面图标/改收藏夹），要桌面图标/Dock 固定请用 tar.gz 的安装器或手动。

## 2. Windows

**一键**（cmd.exe；不需要 x64 开发者命令行，脚本自己用 VS 生成器）——编两个二进制 + 组装
`dist\windows-x64\` 便携目录（两个 exe + assets + 图标 + 随包示例模型 + README），
并生成 `dist\l2dpet-portable-x64.exe` 单文件自解包版：

```bat
scripts\windows_build.bat                  :: 编 + 组装
scripts\windows_build.bat --skip-build     :: 用现有 exe 直接组装
scripts\windows_build.bat --clean          :: 先删构建目录与 dist 再编
scripts\windows_build.bat --help
```

产物拷走即用：文件夹版双击 `l2dtest.exe`；单文件版双击 `l2dpet-portable-x64.exe`。不写注册表。
单文件缓存目录由版本号和文件路径/内容的 SHA-256 决定，同版本、同大小的修改也会重新解包。
首次解包通过互斥锁串行执行，完整写入临时目录后再改名发布；缺文件或大小不符时会重新解包。
`--skip-build` 不需要 SDK，但仍需要 CMake 与 Visual Studio 来生成单文件启动器。

**分步编**（Git Bash；与 Linux 的 `build-player.sh` / `build-panel.sh` 同构）：

```bash
live2d/build-player-win.sh     # → $WORK/build-win/Release/l2dtest.exe
live2d/build-panel-win.sh      # → $WORK/build-panel-win/Release/l2dpet-ui.exe
```

脚本会自己完成 SDK 构建树组装、软渲染后端注入、`cygpath` 路径转换与 `-A x64` 生成器选择。

依赖：CMake ≥ 3.14、Visual Studio（MSVC + Windows SDK）、**Cubism SDK 解包目录**。
SDK 按 §0 放好：`CSM_SDK` 可指向解包目录，默认在 `%USERPROFILE%/l2d-build/live2d-sdk/`
下寻找 `CubismSdkForNative-*`。EUI-NEO 与 GLFW/glad/freetype 在仓库的 `live2d/3rd/EUI-NEO`，
已包含本工程补丁，不需要单独 git 检出或再次打补丁。首次构建 Windows 主程序会下载 libcurl 源码；
后续使用构建缓存。
> SDK 5-r.5 起，Windows Core 静态库放在 MSVC 工具集子目录下
> （`Core/lib/windows/x86_64/{141,142,143}/Live2DCubismCore_{MD,MT,M*}.lib`），
> 而工程按扁平路径找库；构建脚本会自动挑 **143**（VS2022/2026 的 ABI）
> 的 `_MT` 版补成 `x86_64/Live2DCubismCore.lib`——**不用手工动 SDK**。

手工编（不想用脚本时）：

```bat
:: 1) 构建桌宠（在 x64 Native Tools 命令提示符里）
cmake -S live2d\player -B build-win -A x64 ^
      -DCSM_BUILD_ROOT=<csm-root> ^
      -DL2D_3RD_DIR=<EUI-NEO>\3rd ^
      -DCMAKE_BUILD_TYPE=Release
cmake --build build-win --config Release
:: 产物 build-win\Release\l2dtest.exe

:: 2) 构建 UI 进程（EUI-NEO 自动处理 GLFW/freetype 等；托盘走 Win32 原生 API）
cmake -S live2d\panel -B build-panel-win -A x64 -DCMAKE_BUILD_TYPE=Release
cmake --build build-panel-win --config Release
:: 产物 build-panel-win\Release\l2dpet-ui.exe（assets/ 与 l2dpet.png/.ico 会在旁边）
```

运行（PowerShell）：

```powershell
.\build-win\Release\l2dtest.exe <模型.model3.json> 0 0.0333 --pet --ctl 23333 --size 360x640 --order static
```

`--pet` 在 Windows 上自动选择 **glfwpet** 端口。UI 进程会自动被拉起
（`l2dpet-ui.exe` 放同目录或 `L2DPET_UI` 指定）；托盘的 LLM 配置页写入的
`llm.json` 在 `%USERPROFILE%\.config\l2dpet\`（与 Linux 同路径语义）。

**用户数据落在哪**（三端同路径语义，Windows 上家目录 = `%USERPROFILE%`）：

| 内容 | 路径 |
|---|---|
| 设置 + **上次选中的模型**（`str.model`，键为模型目录名）+ LLM 供应商 | `%USERPROFILE%\.config\l2dpet\config.json`、`llm.json` |
| 性格 / 记忆 / 题库 / 笑话（按模型指纹落盘） | `%USERPROFILE%\.config\l2dpet\personas\`、`memory\` |
| 模型库（每个模型一个子目录） | `%USERPROFILE%\.local\share\l2dpet\models\` |

> "切了模型、重启又变回 Hiyori"这类问题看 `config.json` 的 `str.model`：它是**模型目录名**，
> 启动时按它匹配模型库（匹配不到就回到扫描结果里的第一个）。导入 zip 时目录名优先取包内
> `<名>.model3.json` 的 `<名>`（拿不到才用源文件名）。

**输入模型（Windows 与 Linux 的对应关系）**：Linux 用 XShape 把窗口输入区域收窄到角色轮廓；
Windows 走 `WM_NCHITTEST` 做同一件事——角色轮廓内返回 `HTCLIENT`（这次点击归宠物，不再穿到
下面的窗口），轮廓外返回 `HTTRANSPARENT`（交给下面的窗口）。命中判定复用软渲染上一帧的
alpha 轮廓（`PointOnCharacter`），所以两者语义一致；窗口带 `WS_EX_NOACTIVATE`，点角色不会把
当前应用的前台焦点抢走。

**libcurl（LLM）**：Windows 上没有 pkg-config / 系统 libcurl，官方给的又是 mingw 版。
`player/CMakeLists.txt` 因此在 Windows 上用 FetchContent 拉官方源码包**就地编一份静态 +
Schannel 的 libcurl**（只编 HTTP(S)，TLS 走 Windows 原生）——`l2dtest.exe` 不依赖任何
`libcurl.dll`，绿色版拷走就能用。首次配置会下一遍源码包（约 4MB，缓存在构建树里）。
Linux/macOS 仍走 pkg-config 找系统 libcurl。

**图标（Windows 上这是两条独立的路，只治一条会留下"一半是旧图标"）**：

| 显示位置 | 来源 | 配在哪 |
|---|---|---|
| 资源管理器里的文件、文件属性页 | exe **内嵌的 ICON 资源**（编译期） | EUI 的 `eui_neo_configure_app()` 会按 `EUI_APP_ICON_ICO` 生成 `.rc` 挂到目标上；面板在 `panel/CMakeLists.txt` 里把它设成 `l2dpet.ico`。**不要自己再加一个 `.rc`**——两个 ICON 资源会撞成 `CVT1100`/`LNK1123` |
| 标题栏 / 任务栏按钮 / Alt-Tab | **运行期**从 EUI 的 `DslAppConfig.iconPath` 指定的图加载（默认值 `assets/icon.png`），再 `glfwSetWindowIcon` | 面板显式设 `.iconPath("l2dpet.png")`。**不设就会用 EUI 自带的那张默认图**（历史上表现为"窗口左上角是 EUI 的图标"） |
| 托盘 | `l2dpet.ico` 文件路径（面板传给 EUI 的 `.trayIcon(...)`） | Win32 的 `ExtractIconEx` 只认 `.ico/.exe/.dll`，**给 PNG 会拿不到图标**（退化成默认图标） |

> 排查提示：exe 的资源图标被覆盖后，**资源管理器仍可能显示旧缩略图**（同一路径反复覆盖时尤其明显）。
> 先别改代码——用 `ie4uinit.exe -show` 刷缓存，或重启资源管理器/重启系统再确认。
> 想确认二进制里到底是什么，可以问 Windows 自己：`ExtractAssociatedIcon` 取文件图标、
> `SHGetFileInfo` 取 shell 显示用的图标、对窗口发 `WM_GETICON` 取标题栏/任务栏用的图标。

**EUI-NEO 补丁**（`live2d/panel/eui-patches/`）：仓库副本已包含这些修改；
只有通过 `EUI_NEO_DIR` 指定外部检出时，配置阶段才应用补丁。

| 补丁 | 作用 |
|---|---|
| `0001-…` | 弹出窗口/窗口控制/常驻 tick/托盘自定义菜单（上游缺的能力） |
| `0002-raise-main-window-to-front-on-windows.patch` | 设置窗口弹出时抬到 Z 序最前（Windows 上 `glfwShowWindow` 走 SW_SHOWNA 不动 Z 序，`glfwFocusWindow` 又常被前台锁挡掉，不补这一步窗口会"显示"在别的窗口后面） |
| `0003-tray-menu-utf8-wide-apis.patch` | 托盘菜单改走 W 版 API（原实现用 ANSI 的 `InsertMenuItem`，UTF-8 标签被按本地代码页解释 → 中文乱码） |

### 已知边界（Windows，v1）

- **弹出菜单的"点空白处收起"**：Linux 靠弹窗失焦判定，Windows 上前台锁让弹窗拿不到焦点，
  改由面板轮询全局左键实现（点菜单矩形外即收起）。
- 透明窗口依赖桌面合成（Win10+ 默认开 DWM）。
- **模型导入（zip）**：走的是 `/usr/bin/unzip`（Linux 路径），Windows 上该分支不可用——
  用「面板模型页导入 zip」会失败；直接把模型目录放到模型库里即可（见 §6）。
- **Esc 退出**：角色窗口是 `WS_EX_NOACTIVATE`（不抢焦点），所以 Esc 到不了它；
  用面板的「隐藏/退出」或托盘的菜单项退出。

## 3. macOS

依赖：CMake、Xcode 命令行工具（clang）、libcurl（brew install curl 或系统自带）。

```bash
cmake -S live2d/player -B build-mac \
      -DCSM_BUILD_ROOT=<csm-root> \
      -DL2D_3RD_DIR=<EUI-NEO>/3rd \
      -DCMAKE_BUILD_TYPE=Release
cmake --build build-mac -j

cmake -S live2d/panel -B build-panel-mac -DCMAKE_BUILD_TYPE=Release
cmake --build build-panel-mac -j
```

运行：

```bash
./build-mac/l2dtest <模型.model3.json> 0 0.0333 --pet --ctl 23333 --size 360x640 --order static
```

`--pet` 在 macOS 上同样自动选 **glfwpet**。文字用系统苹方（`/System/Library/Fonts/PingFang.ttc`），
托盘走 EUI-NEO 的 Cocoa 后端。

### 已知边界（macOS，v1）

- **鼠标穿透**：GLFW 3.4 的 `GLFW_MOUSE_PASSTHROUGH` 在 macOS 用 `NSWindow.ignoresMouseEvents` 实现，
  语义是"整窗穿透"——宠物点击靠全局轮询（`CGEventSource`）+ 软件命中（与 Windows 同路径）。
- **全局鼠标权限**：读取全局鼠标位置通常无需辅助功能权限，但如被系统拦截，宠物将收不到窗外交互。
- 未在实机验证（本机无 Mac）。

---

## 4. 端口矩阵

| 端口 | Linux | Windows | macOS | 说明 |
|---|---|---|---|---|
| `pet`（X11 原生） | ✅ 现役 | — | — | 最成熟：XShape 逐像素输入区域、Xft 气泡、EWMH 全套 |
| `glfwpet`（跨平台） | ✅ 已实测 | ✅ 代码就绪 | ✅ 代码就绪 | GLFW 透明窗 + 全局输入 + freetype 气泡；三端同一份代码 |
| `sdl` | ✅ | ✅ | ✅ | 纯窗口（无宠物特性），对照/兜底用 |
| `headless` | ✅ | ✅ | ✅ | 无窗口解算（--render 落帧，测试/对拍用） |

桌面三端的**完整功能**（透明/穿透/置顶/气泡/菜单/托盘/agent）由 `glfwpet + l2dpet-ui` 提供；
Linux 上另有更成熟的 `pet` 端口，两者可随时切换（`--port`）。

## 5. 内置 agent 与协议端口

- **agent 在桌宠进程内**（`player/src/PetAgent.cpp`，线程 + 内部协议调用）：跨会话记忆、台词库、
  互动回应/闲时小剧场，全部自动运行，**不再需要外部 Python 进程**。
- **协议端口 23333 保留**：UI 进程（托盘/菜单/设置窗口）用它；调试工具（`tools/pet_client.py`）、
  以及任何想遥控桌宠的外部程序都走它。外部 agent 仍可连接（会收到"内置 agent 正在运行"警告）。
- LLM 接入设置：桌宠「状态与设置 → 对话」页（或环境变量 `LLM_API_KEY` / `LLM_BASE_URL` / `LLM_MODEL`；
  本地端点需 `LLM_ALLOW_PRIVATE=1`）。

## 6. 未验证清单（诚实标注）

- **macOS 实机运行**（本机无 Mac）：窗口透明/穿透/托盘/菜单/气泡渲染的实机表现待测。
- **mingw 交叉编译**：本机无 mingw 工具链，未做编译级验证（装 `mingw-w64` 后可用
  `-DCMAKE_TOOLCHAIN_FILE` 复跑）。
- macOS 的全局鼠标权限提示。
- **Windows 首次实机验证已完成**（2026-10-10，Windows 10/11 + VS2026/MSVC 14.50 + SDK 5-r.5）：
  透明窗 / 逐像素命中（`WM_NCHITTEST`）/ 置顶 / 拖拽 / 点击与右侧菜单 / 面板托盘与设置窗口 /
  气泡直绘（freetype + 系统 msyh）/ 内置 agent 与协议端口 23333 / 软渲染线程池出帧，均已实测可用。
  该轮实测同时修掉的问题（都写进代码注释了）：软渲染后端的 pthread/GCC 内建移植、
  `ReplaceFile` 与 `windows.h` 宏冲突、`DirName` 只认 `/`、`_pressed` 卡住导致窗口被甩飞、
  角色窗/气泡窗的任务栏入口、设置窗口不置前、托盘中文乱码。仍未覆盖：滚轮缩放、
  模型 zip 导入（见 §2 已知边界）、多显示器下的 DPI 混用。


## 7. 打包回归验证

```bash
python3 scripts/test_packaging.py
scripts/linux_build.sh --appimage
```

回归覆盖：默认启动不覆盖保存的模型、显式模型优先级、安装后资源完整、复制失败退出、
含空格/引号/百分号的桌面入口与 AppImage 转发、不覆盖已有安装、完整解包 SDK 的头文件注入，以及
Windows 单文件包同版本同大小内容更新的缓存标识。Linux 打包还自动在隔离 HOME 中真实安装，
检查安装目录和菜单项；`.deb`/AppImage 各自检查启动器与关键资源。

Windows 实机待测（退出桌宠后再重新打包）：

1. `scripts\windows_build.bat --clean`：两个 exe 与单文件产物都应生成。
2. 暂时移走外部 SDK，运行 `--skip-build`：仍可重新组装和生成单文件包。
3. 把单文件包放入含空格、中文、`!` 的路径：首次启动、关闭后再次启动均正常。
4. 同版本重新打包，修改一个资源但保持字节数不变：新包应创建新缓存并显示新资源。
5. 首次启动连续双击两次：不应出现解包共享冲突或半成品缓存。
6. 退出后删除解包缓存中的一个资源，保留 `.ready`：下次启动应修复缓存。
7. 新用户/未装 VC++ 运行库的 Windows 10/11：角色、面板图标与关于页头像均正常。

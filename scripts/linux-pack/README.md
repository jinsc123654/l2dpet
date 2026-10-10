# Live2D 桌宠（Linux 桌面安装包）

住在桌面上的 Live2D 角色：透明窗口、可拖动/缩放、会犯困睡觉、能摸能戳、有托盘图标与设置面板；
配上你自己的 LLM API Key 之后她会聊天、会主动搭话、记得你（内置 agent，无需额外进程）。

## 三种装法

```bash
# ① AppImage（最省事：不装、不找依赖，单文件）
chmod +x l2dpet-*-x86_64.AppImage
./l2dpet-*-x86_64.AppImage                 # 直接跑：首次运行会**自动做一次桌面集成**（见下）
./l2dpet-*-x86_64.AppImage install         # 或正式装到本机：~/.local/opt/l2dpet + 菜单项 + 启动器
#   install 可选开关：--system（装 /opt，需 root）/ --desktop-icon / --dock / --autostart
#   装完就能直接敲 l2dpet；卸载 l2dpet uninstall（默认保留配置/存档/模型）
#   需要 libfuse2（Ubuntu: sudo apt install libfuse2）；没有就用 --appimage-extract-and-run

# ② 便携包（装到用户目录，不需要 root）
tar xzf l2dpet-*-linux-x86_64.tar.gz && cd l2dpet-*-linux-x86_64 && ./install.sh

# ③ .deb（系统级，apt 会自动补依赖）
sudo apt install ./l2dpet-*-linux-x86_64.deb
```

### AppImage 直接运行时的"自动桌面集成"

AppImage 是**一个文件**，构建时没法把菜单项塞进你的 `~/.local/share/applications`（那是用户目录里的东西），
所以做成"**第一次运行时请求创建一次**"，写这三个（都指向**这个 .AppImage 文件**本身）：

| 写什么 | 作用 |
|---|---|
| `~/.local/share/applications/l2dpet.desktop` | 应用菜单/搜索里能搜到（带 `StartupWMClass`，Dock 里跑起来的面板会显示成本应用） |
| `~/.local/share/l2dpet/l2dpet.png` | 图标（必须拷出挂载点——AppImage 一退出挂载就没了） |
| `~/.local/bin/l2dpet` | 转发到该 AppImage 的小脚本，于是 `l2dpet status/stop/...` 在 PATH 里可用 |

- **幂等**：已经指向同一个文件就不重写；AppImage 被移动/改名后，下次运行会自己更新（含启动器内容）。
- **不想要**：`L2DPET_NO_INTEGRATE=1` 运行一次即可跳过；或删掉上面那两个文件。
- **想更正式**（拷到固定位置、可选系统级/桌面图标/Dock/自启）：`./l2dpet-*.AppImage install`。
- **Dock 常驻 pin** 仍是用户动作（GNOME：应用菜单里右键 → 添加到收藏夹），或 `l2dpet dock`。

想要的图标入口（三种装法都行，都是同一条命令）：

```bash
l2dpet desktop-icon  # 桌面上放一个启动图标（双击即起）
l2dpet dock          # 固定到 Dock/任务栏（GNOME 收藏夹；其它桌面按提示右键固定）
l2dpet autostart on  # 登录自启（off 关掉）
```

`install.sh` 的 `--desktop-icon / --dock / --autostart` 就是转调这几条命令。

装完直接用 `l2dpet` 启动：

```bash
l2dpet                # 恢复上次选中的模型；模型库为空时放入示例 Hiyori
l2dpet status         # 进程 / 端口 / 模型库 / 启动日志尾部
l2dpet stop           # 优雅退出
l2dpet open 8         # 打开面板第 8 页（外观：主题 / 语言）
l2dpet logs           # 跟日志
L2DPET_DRY_RUN=1 l2dpet   # 只看"会用哪个端口/怎么起"，不真起
```

面板 = 托盘图标 →「状态与设置…」，或直接在角色上右键。关窗只是收起，退出走托盘/右键菜单。

**会话（X11 / Wayland）**：有 X11 就用 X11 端口（置顶 / 拖拽定位 / 点击穿透 / 气泡，功能最全）；
只有 Wayland（没有 XWayland，`DISPLAY` 为空）时自动退回 GLFW 端口——能出角色窗，但 Wayland 没有全局
置顶与客户端定位协议，角色出现在合成器给的位置、窗口矩形内会接住点击。想强制：`L2DPET_PORT_KIND=x11|glfwpet`。

**Dock 里会看到什么**：应用菜单里能搜到「Live2D 桌宠」；打开设置面板时，Dock/任务栏里那条窗口会显示
本应用的名字和图标（靠 `.desktop` 的 `StartupWMClass`）。**角色本身不出现在 Dock/任务栏**——这是有意的
（否则任务栏永远挂着一条"桌宠"）。Dock 上的常驻图标要 `--dock` 或你在应用菜单里右键"添加到收藏夹"。

## 模型

**自带一个示例模型**：Live2D 官方示例 **Hiyori**（© Live2D Inc.，按 *Live2D Free Material License Agreement* 随本应用分发，
<https://www.live2d.com/eula/live2d-free-material-license-agreement_en.html>；不得把它单独再分发）。
第一次启动时如果模型库是空的，启动器会把它拷到 `~/.local/share/l2dpet/models/Hiyori/`，所以没有任何模型也能直接起来；
模型库里已经有模型时不会再拷，不想要它就在面板「模型」页删掉。

其它模型有各自的授权（个人使用 / 禁二次分发等），所以不随包分发。两种放法：

1. **面板导入（推荐）**：起界面 →「模型」页 →「选 zip…」或「选目录…」→「导入并识别」
   （会自动解算骨架/表情/动作/可交互分区并切过去）
2. **手动**：把含 `model3.json` 的文件夹整个拷到 `~/.local/share/l2dpet/models/<名字>/`

## 依赖

`.deb` 会把依赖的最低版本写成 `Depends`，由 apt 补齐并拒绝不兼容的旧系统；`.tar.gz` 手动一条；**AppImage 自带依赖，什么都不用装**（除 `libfuse2`）。

需要这些运行库（主流发行版都自带，Debian/Ubuntu 上对应 `libx11-6 libxext6 libxft2 libfontconfig1
libcurl4 libglib2.0-0 libdrm2`）：

- **X11 系**：`libX11` `libXext` `libXft` `libfontconfig`（角色窗口、点击穿透、气泡文字）
- **网络**：`libcurl`（她的 LLM 调用）
- **面板**：`glib/gio`（托盘 SNI / dbusmenu）
- 运行时还需要：CJK 字体（`fc-match sans-serif:lang=zh-cn` 能找到即可；找不到用包内兜底字体）、
  一个 X11 或 XWayland 会话（纯 Wayland 会自动走 GLFW 端口，见上）、OpenGL 驱动（面板用）、
  托盘宿主（GNOME 需 AppIndicator 扩展；KDE/XFCE 自带）

**系统门槛**：二进制要求 **glibc ≥ 2.38**（Ubuntu 23.10+ / Debian 13+ / Fedora 39+）。
Ubuntu 22.04（2.35）、Debian 12（2.36）跑不了——AppImage 也一样，因为 glibc 不能随包带。

## 数据在哪（卸载默认都保留）

| 路径 | 内容 |
|---|---|
| `~/.config/l2dpet/` | 配置（LLM 供应商与密钥 0600）、每个模型的性格、问题库、笑话池、记忆 |
| `~/.local/state/l2dpet/` | 亲密度存档、交互日志 `llm-log.jsonl`、桌面日志 `desktop.log` |
| `~/.local/share/l2dpet/models/` | 模型库 |

面板「记忆」页能把这三样里的记忆/性格/题库/笑话/配置**导出成一个 JSON 备份**（不含模型与 Key），
换机器时导回去即可。

## 隐私

- 协议端口只监听 `127.0.0.1`，不对外。
- 只有你自己配的 LLM API Key 会用来发请求（`~/.config/l2dpet/llm.json`，0600，或环境变量）；
  不配 Key 时她只用本地台词库，完全离线。
- 记忆/台词是她自己整理的文本，存在上面那几个目录里，人可读可改。

## 目录结构（装完之后）

```
~/.local/opt/l2dpet/
  l2dpet         启动器（l2dpet start/stop/status/...）
  l2dtest        桌宠进程（渲染 + 内在状态 + 内置 LLM agent）
  l2dpet-ui      面板进程（托盘/右键菜单/设置页；桌宠自动拉起）
  l2dpet.png     托盘与应用图标
  assets/        EUI 图标字体（面板图标靠它）
  tools/         调试工具（pet_client.py 协议客户端等）
  VERSION BUILD-INFO README.md install.sh uninstall.sh
```

排错：

```bash
l2dpet status                 # 先看进程/端口/日志（会显示"起法：端口档/协议口/模型"）
l2dpet logs                   # 角色窗口/面板 pid/协议
L2DPET_DRY_RUN=1 l2dpet       # 只看会用哪个端口、怎么起（不真起）
L2DPET_UI_DEBUG=1 l2dpet      # 面板事件级调试
L2DPET_FOREGROUND=1 l2dpet    # 前台跑，看启动报错
L2DPET_PORT_KIND=glfwpet l2dpet   # 强制换端口（x11|glfwpet）
```

AppImage 特有：`--appimage-extract-and-run`（没有 libfuse2 时用）、`--appimage-extract`（手动解包看内容）、
`--appimage-help`。AppImage 每次运行的挂载点都不同，所以"找在跑的桌宠"靠
`~/.local/state/l2dpet/desktop.pid`（启动器自己写/清）。

# EUI-NEO 补丁（基于提交 782c56993dc1890e0589e2100cfa74322bb0e0bf）

> **本目录现在是"改动记录"，构建时不再打补丁。**
> EUI-NEO 已作为**本地源码随仓库分发**（`live2d/3rd/EUI-NEO`），下面这些改动**已经落在那些文件里**——
> 不再依赖 git/网络，Linux / Windows / macOS 用同一份本地副本直接编。
> 这里的 .patch 保留下来是当**相对上游的 diff 对照**（升级 EUI-NEO 时按它重做一遍即可）。
>
> 只有当你用 `-DEUI_NEO_DIR=<外部检出>` 指向别处的 EUI-NEO 时，`../CMakeLists.txt` 才会
> 按序把这些补丁打上去（那份检出需落在上面的提交）。仓库自带的那份不会被重复打补丁。

目的：让托盘、右键菜单、说话气泡、设置窗口全部用 EUI-NEO 一套 UI 栈（桌宠进程里不再有 UI 控件）。

`0001-l2dpet-popup-windows-window-control-tick-tray-menu.patch`：

| 能力 | 改动 |
|---|---|
| 弹出窗口 / 气泡 | `DslWindowConfig` 增 `position` / `decorated` / `alwaysOnTop` / `transparent` / `focusOnShow` / `resizable` / `windowType`（1 弹出菜单 2 提示气泡 3 工具窗；X11 设 `_NET_WM_WINDOW_TYPE` + 不进任务栏）/ `userTime`（X11 `_NET_WM_USER_TIME`；菜单再发 `_NET_ACTIVE_WINDOW`，避免被 WM 防抢焦点挡掉）/ `onFocusChange`；X11 函数经 dlopen/dlsym 取，不增加链接依赖；GLFW 先隐藏创建、定好位置再显示 |
| 运行中控制窗口 | `app::closeWindow` / `moveWindow` / `resizeWindow` / `showMainWindow` / `hideMainWindow` / `isWindowOpen`（主循环执行的队列）|
| 常驻进程 | `DslAppConfig::onTick(fn, interval)`（每圈主循环调用，主窗口隐藏时也调；主循环最多等 interval 秒）、`startHidden()`、`hideOnClose()`；主窗口隐藏时子窗口照常创建/更新/渲染；非模态子窗口不再阻止隐藏主窗口 |
| 托盘菜单 | `eui::platform::setTrayMenu(items)` / `consumeTrayMenuClick(&id)`（`checkable` 勾选项/单选/禁用/分隔线/一级子菜单；SNI 下 checkable 项即使未勾选也带 `toggle-type`）；SNI（Linux，GDBus + dbusmenu，变更时发 `LayoutUpdated`）、Win32 与 AppIndicator（`tray.h` 嵌套菜单）、AppKit（NSMenu）四个后端；托盘标题取 `trayTitle`（原来写死 "EUI NEO"）|

`0002-raise-main-window-to-front-on-windows.patch`（Windows 实测补的）：

| 问题 | 改动 |
|---|---|
| 设置窗口"显示"了却压在其他窗口后面 | Windows 上 `glfwShowWindow` 走 `SW_SHOWNA`（不激活、不动 Z 序），`glfwFocusWindow` 的 `SetForegroundWindow` 又常被前台锁挡掉 → 补 `raiseWindowToFront()`（`SetWindowPos(HWND_TOP)`，不受前台锁限制），主窗口显示与托盘还原两条路都调 |

`0003-tray-menu-utf8-wide-apis.patch`（Windows 实测补的）：

| 问题 | 改动 |
|---|---|
| 托盘右键菜单中文乱码 | `3rd/tray/tray.h` 用 ANSI 的 `InsertMenuItem`，UTF-8 标签被按本地代码页（936）解释 → 改 `MultiByteToWideChar(CP_UTF8)` 转 UTF-16 + `InsertMenuItemW`/`GetMenuItemInfoW`/`InsertMenuW`；`MIIM_TYPE` 拆成 `MIIM_FTYPE\|MIIM_STRING`（`InsertMenuItem` 不接受 `MIIM_TYPE`）|

另有一处不在补丁里、直接写在本地副本中的改动（见 `../CMakeLists.txt` 与 `live2d/3rd/EUI-NEO/CMakeLists.txt`）：
exe 图标由 EUI 生成的资源提供，本地副本里给它加了 `EUI_APP_ICON_ICO` 覆盖点，面板指向自己的 `l2dpet.ico`
（自己另加 .rc 会撞成"资源重复 ICON"链接失败）。

`0004-hide-popup-windows-from-taskbar.patch`（Windows 实测补的）：

| 问题 | 改动 |
|---|---|
| 右键模型弹出的菜单在任务栏/Alt-Tab 里露头（还会带一张缩略图预览） | `core/window/window_backend.cpp`：Win32 下 `glfwCreateWindow` 之后，**只对弹出窗**（`windowType != 0`：菜单 1 / 气泡 2 / 工具窗 3）设 `WS_EX_TOOLWINDOW` 并清掉 GLFW 默认带的 `WS_EX_APPWINDOW`（留着工具窗样式不生效）。弹出窗本来就是隐藏创建、定好位置再显示，样式在显示前生效。**主窗口（状态与设置）保持原样**——用户要在任务栏里看到它、能切回来。X11 侧早有 `_NET_WM_WINDOW_TYPE` + "不进任务栏"，这是 Windows 的对应实现 |

只改了 GLFW 窗口后端；SDL2 后端下这些新接口存在但不生效。
升级 EUI-NEO 时：在新版本上重新应用/调整这些补丁，把结果覆盖进 `live2d/3rd/EUI-NEO`，并更新 `EUI_NEO_COMMIT`。
可以考虑提交给上游。


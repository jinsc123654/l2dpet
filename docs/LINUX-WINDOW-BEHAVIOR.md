# Linux 桌宠窗口与自启动

适用 X11 / XWayland。纯 Wayland 的任意窗口定位仍由合成器协议决定。

## 自启动入口

- 直接运行 AppImage 时，自启动使用实际的 `$APPIMAGE` 文件，不假设已复制到默认安装目录。
- `install --autostart` 则指向新安装的副本。
- 面板优先使用启动器；构建目录没有启动器时，裸播放器带 `--pet` 参数，并指定工作目录。
- 写入前验证目标可执行，防止界面显示开启但目标不存在。
- 启动新版 AppImage 时，自动迁移旧版本生成的、指向不存在的默认安装目录的启动项；不重启或改写已禁用的启动项。

## 保存半屏外位置

GNOME Mutter 48 对普通窗口的程序移窗请求会强制应用屏幕边界限制。`_NET_MOVERESIZE_WINDOW` 也会进入相同的限制逻辑；source 字段不能绕开它。此前的原生鼠标拖动允许移出屏幕，但启动后的程序恢复仍会被夹回。

现在 Linux 使用桌面挂件语义：置顶开启时模型是无 strut 的 DOCK 窗口，不保留工作区；关闭时是 DESKTOP 窗口，位于普通应用后面。两种类型都允许恢复屏幕外位置，仍保留至少 8 像素可见。模型通过客户端鼠标拖动直接移窗，避免依赖 WM 对 DOCK/DESKTOP 的原生拖动支持。

保存的位置在首次映射后重新应用，覆盖 WM 初始摆放；配置晚到时也能恢复。Windows 的窗口类型、位置恢复和自启动快捷方式逻辑保留远端实现。

## 气泡与 Dock

模型和气泡都显式设置 `WM_HINTS.input=false`、`_NET_WM_USER_TIME=0`，气泡不请求激活或 `XMapRaised`。置顶开启时气泡是被 WM 管理的 TOOLTIP，关闭时回到 DESKTOP 层。它保留点击选项的输入事件，但不会成为新的普通应用窗口抢走设置页焦点。

## 回归测试

```sh
python3 scripts/test_desktop_startup.py
python3 scripts/test_packaging.py
bash scripts/test_linux_windows.sh
```

前两项用临时文件和可执行替身验证真实 `.desktop` 启动、特殊路径、工作目录、播放器参数、安装副本及禁用状态。窗口测试在隔离的 GNOME 48/Xvfb 会话运行真实 `PortX11Pet`，覆盖关闭/重启、晚到配置、隐藏/显示、屏幕外移窗、负底边距保存、重复气泡、焦点和关闭置顶后的层叠。不读取真实用户配置或操作当前桌面。

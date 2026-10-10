# scripts/ — 构建与打包脚本

Live2D 桌宠（三端桌面：Windows / Linux / macOS）的构建与打包入口。
编单个二进制的脚本在 `live2d/`（`build-player.sh` / `build-panel.sh`，Windows 侧是
`build-player-win.sh` / `build-panel-win.sh`），本目录放"打包/发布/辅助"这一类。

## 桌宠：打 Linux 安装包

| 脚本 | 作用 |
|---|---|
| `linux_build.sh` | **编 + 打 Linux 桌面安装包**：调 `live2d/build-player.sh host`（桌宠）与 `live2d/build-panel.sh`（面板）→ 组装 → `dist/linux-<arch>/` 出 `l2dpet-<版本>-linux-<arch>.tar.gz` 与 `.deb`（`--appimage` 再加 AppImage） |
| `linux-pack/` | 安装包的"物料"（构建时被原样打进包里）：启动器 `l2dpet`、`install.sh`、`uninstall.sh`、`l2dpet.desktop`、包内 `README.md` |

```bash
scripts/linux_build.sh                 # 编 + 打包
scripts/linux_build.sh --skip-build    # 只重新打包
scripts/linux_build.sh --appimage      # 再加 AppImage
scripts/linux_build.sh -h              # 全部选项（--version/--out/--no-deb/--clean）
```

产物落在 `dist/`（已 gitignore）。用户拿到 tar.gz 后：`tar xzf … && cd … && ./install.sh` → `l2dpet`。
详见 `live2d/BUILD-CROSS-PLATFORM.md` §1.1 与包内 `README.md`。

## 桌宠：打 Windows 便携包

| 脚本 | 作用 |
|---|---|
| `windows_build.bat` | **编 + 组装 Windows 便携包**（cmd.exe，不需要 x64 开发者命令行）：自己组装 Cubism SDK 构建树 → 编 `l2dtest.exe`（含静态 libcurl，LLM 可用）与 `l2dpet-ui.exe`（EUI-NEO）→ 组装 `dist\windows-x64\`（两个 exe + assets + 图标 + 随包示例模型 + README）→ 再打成**单文件** `dist\l2dpet-portable-x64.exe` → 自检产物 |

```bat
scripts\windows_build.bat                  :: 编 + 组装
scripts\windows_build.bat --skip-build     :: 用现有 exe 直接组装
scripts\windows_build.bat --clean          :: 先删构建目录与 dist 再编
scripts\windows_build.bat --help
```

两个产物，按需要拿：

- `dist\windows-x64\`：便携**文件夹**（两个 exe + assets + 图标 + 随包示例模型），
  拷走即用，双击 `l2dtest.exe` 启动；不写注册表。
- `dist\l2dpet-portable-x64.exe`：**单文件**版（整个文件夹作为资源塞进一个自解包 exe，约 16MB）。
  拷到任何 Windows 10/11 双击即可：首次运行解包到 `%LOCALAPPDATA%\l2dpet\portable-<key>\`
  再拉起桌宠，之后复用已验证的缓存；版本号或文件路径/内容的 SHA-256 变化都会重解包，
  同大小的内容更新也会生效。首次解包使用互斥锁与临时目录提交，缺文件时自动修复。
  两个 exe 都是静态 CRT（/MT）+ 静态 Core，只依赖系统 DLL——不用装 VC++ 运行库。
  排障看 `%TEMP%\l2dpet.log`（桌宠自己，每次运行重写）和 `%TEMP%\l2dpet-portable.log`（启动器）；都是 UTF-8。
  双击启动时桌宠会把自动弹出的控制台立刻藏掉、日志转存到上面那个文件；从终端启动时日志照旧进终端。

依赖与细节见 `live2d/BUILD-CROSS-PLATFORM.md` §2；Git Bash 环境也可以直接用
`live2d/build-player-win.sh` 与 `live2d/build-panel-win.sh` 分步编。

## 约定

- Linux 构建脚本需要 `bash`；Windows 一键入口使用 `cmd.exe`，无需 Git Bash。`linux_build.sh` 自己会用 `set -euo pipefail` 停在任何一步失败上。
- `windows_build.bat` 用**纯 ASCII**：cmd.exe 按系统 OEM 代码页逐字节读批处理，
  而"在 .bat 里执行 chcp 换代码页"会让它按错误的字节偏移继续读文件（注释片段被当命令执行）——
  中文注释的批处理在中文 Windows 上踩过这个坑，所以这里干脆全用 ASCII，中文说明留在本文档与 .md 里。
- 依赖：CMake + 编译器（编）、`dpkg-dev`（可选，出 .deb 时需要 `dpkg-deb` 与 `dpkg-shlibdeps`）、`tar`/`sha256sum`（打包）。
- 不动用户数据：打包脚本只写 `dist/`（和 `~/l2d-build` 构建树），安装/卸载脚本的行为见包内 README。


## 验证与离线构建

`python3 scripts/test_packaging.py` 运行临时目录中的打包回归，不修改用户安装或数据。
Linux 打包自动验证真实安装后的文件和菜单路径；`.deb` 依赖包含最低符号版本，旧系统会被拒绝安装。
`L2D_JOBS=4` 可限制 Linux 编译并行数。AppImage 工具与 type2 runtime 均缓存；
离线时可用 `APPIMAGE_RUNTIME_FILE=<本地运行时>` 指定运行时。
Windows `--skip-build` 无需 SDK，仍需要 CMake/Visual Studio 编译单文件启动器。
Windows 实机验证清单见 `live2d/BUILD-CROSS-PLATFORM.md` §7。

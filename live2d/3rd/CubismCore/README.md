# Cubism Core（随仓库自带的预编译库）

Live2D Cubism SDK for Native **5-r.5** 的 Core —— SDK 里闭源、预编译的那部分。
放这里是为了 **clone 下来就能编**：不用先去 live2d.com 下 SDK，也不用设 `CSM_SDK`。

| 文件 | 用在哪 |
|---|---|
| `include/Live2DCubismCore.h` | 三端共用的头文件 |
| `lib/windows/x86_64/Live2DCubismCore_MT.lib` | Windows x64 **Release**（本工程编 /MT 静态 CRT，见 `player/CMakeLists.txt` 开头） |
| `lib/windows/x86_64/Live2DCubismCore_MTd.lib` | Windows x64 Debug（/MTd） |
| `lib/windows/x86_64/Live2DCubismCore_MD.lib`、`_MDd.lib` | 动态 CRT（/MD）构建用；本工程不取，留给别的宿主工程（如 LVGL 那条线） |
| `lib/linux/x86_64/libLive2DCubismCore.a` | Linux x86_64 |
| `lib/macOS/x86_64/libLive2DCubismCore.a`、`lib/macOS/arm64/…` | macOS，按架构取 |
| `dll/windows/x86_64/Live2DCubismCore.dll` + `Live2DCubismCore.lib` | Windows **动态**版 Core 与它的导入库；本工程走静态库，这份只是备着 |

SDK 自己的分目录规则是 工具集（141/142/143）× CRT（MD/MT）× 配置（d），
这里为了目录浅一点，只留了 x64 的 143（VS2015+ 同一套 ABI），CRT/配置靠文件名区分。

`LICENSE.md`、`NOTICE.md`、`NOTICE.ja.md` 是 SDK 原样带过来的许可与声明 —— **别删**。

怎么用：`player/CMakeLists.txt` 优先从这里取 Core（取不到才退回 `CSM_BUILD_ROOT` 里的
SDK 副本），三端（Windows / Linux / macOS）走同一套选择逻辑。Windows 打包脚本
`scripts/windows_build.bat` 仍从 SDK 同步 Framework 源码（那部分没入库），Core 用仓库这份。

怎么升级：下新 SDK，按上表同名文件从它的 `Core/` 与根目录拷回来，再改本文件里的版本号。

<div align="center">

<img src="live2d/panel/l2dpet-logo.png" alt="l2dpet" width="180" />

# l2dpet

[简体中文](README.md) · [English](README.en.md) · [日本語](README.ja.md)

**🌸 Live2D × C++ × LLM — A desktop companion that responds to you.**

Transparent windows · Mouse interactions · Chat and memory · Windows / Linux / macOS

[Download](https://github.com/jinsc123654/l2dpet/releases) · [Build from source](#build-from-source) · [Report an issue](https://github.com/jinsc123654/l2dpet/issues)

![License](https://img.shields.io/badge/license-MIT-blue)
![Language](https://img.shields.io/badge/language-C%2B%2B17-orange)
![Platforms](https://img.shields.io/badge/platforms-Windows%20%7C%20Linux%20%7C%20macOS-green)

</div>

A Live2D character living on your desktop: keeping you company while you work or study, reacting to touch, and resting when sleepy. Use local interactions and built-in dialogue, or connect an LLM for conversations, memories, and a personality of its own.

## ✨ Features

- **Desktop companionship**: a transparent, always-on-top window with dragging, scaling, and character silhouette hit testing.
- **Character interactions**: motions, expressions, touch reactions, speech bubbles, and day/night behavior.
- **Chat and memory**: optional LLM integration with memory across sessions and per-model personalities.
- **Easy management**: tray menu, settings panel, model management, and a multilingual interface.
- **Portable packages**: Windows folder and single-file packages; Linux tar.gz, deb, and AppImage packaging.

## 🖼️ Screenshots

### Your desktop companion

The transparent character window runs alongside the settings panel, with bubbles showing its resting state.

![Desktop companion with a sleep bubble and status panel](docs/images/desktop-companion.png)

<details>
<summary>More screenshots: status and interactions, emotions and activity, LLM settings</summary>

### Status and interactions

Check energy, closeness, and mood. Try head pats, holding hands, jokes, and rock-paper-scissors.

![Status panel and interaction controls](docs/images/status-interactions.png)

### Emotions and recent activity

Follow changes through the emotion chart, recent interactions, and touch statistics.

![Emotion chart, recent activity, and touch statistics](docs/images/emotion-activity.png)

### LLM settings

Configure providers and models, and adjust how the character participates during idle time.

![LLM provider and participation settings](docs/images/llm-settings.png)

</details>

> These are actual Windows screenshots. The pictured character is a user-supplied model used to demonstrate the application; it is not included in this repository or its packages. The bundled sample model is Hiyori.

## 📥 Download and run

Visit [GitHub Releases](https://github.com/jinsc123654/l2dpet/releases) for published packages. If no suitable package is available, build from source below.

**Windows**: double-click `l2dpet-portable-x64.exe`, or extract the folder package and run `l2dtest.exe`. The settings panel starts automatically. Portable packages do not require a separate VC++ runtime installation.

**Linux**: extract the tar.gz package, run `./install.sh`, then `l2dpet`. Make an AppImage executable before running it. Install deb packages with your system package manager.

Packages include the official **Hiyori (桃瀬ひより)** sample model. Available motions and expressions depend on the model.

<a id="build-from-source"></a>

## 🛠️ Build from source

### 📋 Prerequisites

```bash
git clone https://github.com/jinsc123654/l2dpet.git
cd l2dpet
```

**Cubism SDK for Native** is required; the verified version is **5-r.5**. Download and extract it from the [official Live2D download page](https://www.live2d.com/en/sdk/download/native/). Set `CSM_SDK` to the SDK root containing `Core/`, `Framework/`, and `Samples/`.

The repository includes EUI-NEO and its local dependencies, plus Cubism Core headers and prebuilt libraries. You must obtain the full SDK separately. The first Windows build downloads and statically compiles libcurl.

| Platform | Build environment | Status |
| --- | --- | --- |
| Windows x64 | Visual Studio with Desktop development with C++ and Windows SDK; CMake ≥ 3.14 | Compilation, packaging, and some runtime checks completed |
| Linux | GCC/Clang, CMake, pkg-config, rsync; OpenGL, X11, FreeType, Fontconfig, and libcurl development packages | Build, packaging, and runtime verification available |
| macOS | Xcode Command Line Tools, CMake, pkg-config, libcurl, rsync | Build support provided; not yet verified on a physical Mac |

### 🪟 Windows

Open **cmd.exe** in the repository root:

```bat
set "CSM_SDK=C:\SDK\CubismSdkForNative-5-r.5"
scripts\windows_build.bat
```

The script prepares the SDK build tree, compiles the character and panel, and produces:

```text
dist/
├── windows-x64/               # Portable folder
│   ├── l2dtest.exe             # Desktop pet
│   ├── l2dpet-ui.exe           # Settings panel
│   ├── assets/
│   └── models/
└── l2dpet-portable-x64.exe     # Single-file portable package
```

The default build directory is `%USERPROFILE%\l2d-build`; override it with `L2D_WORK`. Use `scripts\windows_build.bat --skip-build` to repackage existing character and panel binaries. Building the single-file launcher still requires CMake and Visual Studio.

### 🐧 Linux

Example dependencies for Debian/Ubuntu:

```bash
sudo apt-get update
sudo apt-get install -y build-essential cmake pkg-config rsync unzip \
  libgl1-mesa-dev libx11-dev libxext-dev libxrender-dev libxft-dev \
  libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev \
  libfreetype6-dev libfontconfig1-dev libcurl4-openssl-dev \
  libglib2.0-dev libwayland-dev libwayland-bin wayland-protocols libxkbcommon-dev
```

After preparing the SDK, build and package:

```bash
export CSM_SDK="$HOME/SDK/CubismSdkForNative-5-r.5"
bash scripts/linux_build.sh --no-deb
```

Outputs go to `dist/`. Creating deb packages also requires `dpkg-dev`. For AppImage, use `bash scripts/linux_build.sh --appimage`; packaging tools are downloaded on the first run.

To compile only the two programs:

```bash
bash live2d/build-player.sh host
bash live2d/build-panel.sh
```

Default outputs are `~/l2d-build/build-host/l2dtest` and `~/l2d-build/build-panel/l2dpet-ui`.

### 🍎 macOS

After preparing the SDK and dependencies:

```bash
export CSM_SDK="$HOME/SDK/CubismSdkForNative-5-r.5"
export L2D_JOBS=4
bash live2d/build-player.sh host
bash live2d/build-panel.sh
```

macOS has not yet been verified on a physical Mac. Build and runtime reports are welcome.

## 🎭 Change models

Use a separate folder for each model, preserving relative paths for `.model3.json`, `.moc3`, textures, motions, and related files.

| Platform | Model library |
| --- | --- |
| Windows | `%USERPROFILE%\.local\share\l2dpet\models\` |
| Linux / macOS | `~/.local/share/l2dpet/models/` |

For example: `models/MyPet/MyPet.model3.json`. Select models in the settings panel, or import a ZIP or directory. Windows ZIP import uses the built-in `tar.exe` (Windows 10 1803 or later). The model library directory is created automatically on the first import.

Use models you are authorized to use, and follow their authors' usage and distribution requirements.

## 💬 Configure an LLM

Open the Chat page in the settings panel and enter your service URL, model, and API key. Configuration is stored in `~/.config/l2dpet/llm.json`, or `%USERPROFILE%\.config\l2dpet\llm.json` on Windows.

LLM integration is optional. Local interactions and built-in dialogue work without it. The panel shows “Connected” only after a successful request, and displays the reason when a request fails.

When enabled, conversations and related context are sent to your configured provider. Do not commit API keys or personal configuration to the repository.

## ❓ FAQ

### Can I build immediately after cloning?

Install your platform's build tools and prepare the official Cubism Native SDK first. The initial Windows build also needs network access to download libcurl.

### Can I use it without an API key?

Yes. Basic desktop pet interactions do not depend on an online service.

### Where are the logs if nothing happens on Windows?

Check `%TEMP%\l2dpet.log` and, for the single-file launcher, `%TEMP%\l2dpet-portable.log`. Include your OS version, build method, and relevant logs in reports, with personal information removed.

### Does it work on Wayland?

A GLFW desktop pet backend is provided. Transparency, positioning, always-on-top behavior, and mouse interactions depend on the compositor; verify them in your desktop environment.

## 🧩 Architecture

`l2dtest` handles the Live2D character, behavior, and built-in LLM agent. `l2dpet-ui` handles the tray and settings panel. They communicate through a local JSON Lines interface at `127.0.0.1:23333`.

```text
live2d/player/          Character and behavior (C++17)
live2d/panel/           Tray and settings panel (EUI-NEO)
live2d/3rd/             Third-party dependencies
live2d/bundled-models/  Bundled sample model
live2d/tools/           Debugging tools and portable launcher
scripts/               Build and packaging scripts
```

## 📜 License

The project's own code is licensed under the [MIT License](LICENSE). Third-party components retain their respective licenses and copyright notices.

**Live2D Cubism Core is proprietary and is not covered by MIT.** Follow its included license and official Live2D terms when using or distributing it.

The bundled Hiyori model is copyrighted by Live2D Inc. and subject to the Live2D Free Material License Agreement. See the [model license notes](live2d/bundled-models/README.md). Other models follow their authors' licenses.

## 🙏 Acknowledgments

Thanks to Live2D, EUI-NEO, and the authors and contributors of GLFW, FreeType, libcurl, and other open-source components.

Report problems through [Issues](https://github.com/jinsc123654/l2dpet/issues), or submit a pull request to help improve this desktop companion.

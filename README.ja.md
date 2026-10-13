<div align="center">

<img src="live2d/panel/l2dpet-logo.png" alt="l2dpet" width="180" />

# l2dpet

[简体中文](README.md) · [English](README.en.md) · [日本語](README.ja.md)

**🌸 Live2D × C++ × LLM — デスクトップに、あなたに応える小さな仲間を。**

透明ウィンドウ · マウスでのふれあい · 会話と記憶 · Windows / Linux / macOS

[ダウンロード](https://github.com/jinsc123654/l2dpet/releases) · [ソースからビルド](#build-from-source) · [不具合報告](https://github.com/jinsc123654/l2dpet/issues)

![License](https://img.shields.io/badge/license-MIT-blue)
![Language](https://img.shields.io/badge/language-C%2B%2B17-orange)
![Platforms](https://img.shields.io/badge/platforms-Windows%20%7C%20Linux%20%7C%20macOS-green)

</div>

デスクトップに暮らす Live2D キャラクター。仕事や勉強のそばにいて、触れると反応し、眠くなると休みます。ローカルのふれあいや内蔵セリフを楽しむほか、LLM を接続して会話・記憶・キャラクターごとの性格を利用できます。

## ✨ 機能

- **いつもそばに**：透明・最前面ウィンドウ、ドラッグ、拡大縮小、キャラクターの輪郭に沿ったクリック判定。
- **キャラクターとのふれあい**：モーション、表情、タッチへの反応、吹き出し、昼夜の行動。
- **会話と記憶**：任意の LLM 接続、セッションをまたぐ記憶、モデルごとの性格。
- **使いやすい管理画面**：トレイメニュー、設定パネル、モデル管理、多言語 UI。
- **ポータブルパッケージ**：Windows のフォルダー版と単一ファイル版、Linux の tar.gz・deb・AppImage。

## 🖼️ 動作画面

### デスクトップの小さな仲間

透明なキャラクターウィンドウと設定パネルが一緒に動作し、休息中の状態も吹き出しで表示します。

![キャラクター、睡眠中の吹き出し、状態パネル](docs/images/desktop-companion.png)

<details>
<summary>もっと見る：状態とふれあい、感情と最近の出来事、LLM 設定</summary>

### 状態とふれあい

元気度、親密度、現在の気分を確認できます。頭をなでる、手をつなぐ、ジョーク、じゃんけんも試せます。

![状態パネルとふれあいの操作](docs/images/status-interactions.png)

### 感情と最近の出来事

感情のグラフ、最近のふれあい、タッチの統計から、キャラクターの変化を確認できます。

![感情のグラフ、最近の出来事、タッチの統計](docs/images/emotion-activity.png)

### LLM の会話設定

プロバイダーとモデルを設定し、操作していないときのキャラクターの参加度を調整できます。

![LLM のプロバイダーと参加度の設定](docs/images/llm-settings.png)

</details>

> Windows で実際に動作している画面です。画像のキャラクターはユーザーが用意したモデルで、動作例として掲載しています。本リポジトリや配布パッケージには含まれません。同梱のサンプルモデルは Hiyori です。

## 📥 ダウンロードと起動

公開済みのパッケージは [GitHub Releases](https://github.com/jinsc123654/l2dpet/releases) をご覧ください。対象のパッケージがない場合は、以下の手順でビルドできます。

**Windows**：`l2dpet-portable-x64.exe` をダブルクリックするか、フォルダー版を展開して `l2dtest.exe` を起動します。設定パネルは自動で起動します。ポータブル版では VC++ ランタイムの別途インストールは不要です。

**Linux**：tar.gz を展開して `./install.sh` を実行し、その後 `l2dpet` を起動します。AppImage は実行権限を付けて起動してください。deb はシステムのパッケージ管理ツールでインストールできます。

パッケージには公式サンプル **Hiyori（桃瀬ひより）** が含まれます。利用できる動作や表情はモデルによって異なります。

<a id="build-from-source"></a>

## 🛠️ ソースからビルド

### 📋 事前準備

```bash
git clone https://github.com/jinsc123654/l2dpet.git
cd l2dpet
```

**Cubism SDK for Native** が必要です。検証済みのバージョンは **5-r.5** です。[Live2D 公式ダウンロードページ](https://www.live2d.com/en/sdk/download/native/) から取得・展開し、`Core/`、`Framework/`、`Samples/` を含む SDK のルートを `CSM_SDK` に指定してください。

EUI-NEO とその依存ソース、Cubism Core のヘッダーとビルド済みライブラリはリポジトリに含まれます。完全な SDK は別途用意してください。Windows の初回ビルドでは libcurl をダウンロードして静的にコンパイルします。

| プラットフォーム | ビルド環境 | 検証状況 |
| --- | --- | --- |
| Windows x64 | Visual Studio の「C++ によるデスクトップ開発」と Windows SDK、CMake ≥ 3.14 | ビルド・パッケージ作成・一部の動作を検証済み |
| Linux | GCC/Clang、CMake、pkg-config、rsync、OpenGL・X11・FreeType・Fontconfig・libcurl の開発パッケージ | ビルド・パッケージ作成・動作の検証あり |
| macOS | Xcode Command Line Tools、CMake、pkg-config、libcurl、rsync | ビルド対応あり、実機では未検証 |

### 🪟 Windows

リポジトリのルートで **cmd.exe** を開いて実行します。

```bat
set "CSM_SDK=C:\SDK\CubismSdkForNative-5-r.5"
scripts\windows_build.bat
```

スクリプトが SDK のビルドツリーを準備し、本体とパネルをビルドして次のファイルを生成します。

```text
dist/
├── windows-x64/               # ポータブルフォルダー版
│   ├── l2dtest.exe             # デスクトップキャラクター
│   ├── l2dpet-ui.exe           # 設定パネル
│   ├── assets/
│   └── models/
└── l2dpet-portable-x64.exe     # 単一ファイル版
```

既定のビルド先は `%USERPROFILE%\l2d-build` です。`L2D_WORK` で変更できます。既存の本体とパネルを再パッケージするには `scripts\windows_build.bat --skip-build` を使います。単一ファイルのランチャー生成には引き続き CMake と Visual Studio が必要です。

### 🐧 Linux

Debian/Ubuntu での依存パッケージの例：

```bash
sudo apt-get update
sudo apt-get install -y build-essential cmake pkg-config rsync unzip \
  libgl1-mesa-dev libx11-dev libxext-dev libxrender-dev libxft-dev \
  libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev \
  libfreetype6-dev libfontconfig1-dev libcurl4-openssl-dev \
  libglib2.0-dev libwayland-dev libwayland-bin wayland-protocols libxkbcommon-dev
```

SDK を準備した後、ビルドとパッケージ作成を実行します。

```bash
export CSM_SDK="$HOME/SDK/CubismSdkForNative-5-r.5"
bash scripts/linux_build.sh --no-deb
```

出力先は `dist/` です。deb の作成には `dpkg-dev` も必要です。AppImage は `bash scripts/linux_build.sh --appimage` で作成できます。初回にはパッケージ作成ツールのダウンロードが必要です。

本体とパネルのビルドのみを行う場合：

```bash
bash live2d/build-player.sh host
bash live2d/build-panel.sh
```

既定の出力は `~/l2d-build/build-host/l2dtest` と `~/l2d-build/build-panel/l2dpet-ui` です。

### 🍎 macOS

SDK と依存ツールを準備した後：

```bash
export CSM_SDK="$HOME/SDK/CubismSdkForNative-5-r.5"
export L2D_JOBS=4
bash live2d/build-player.sh host
bash live2d/build-panel.sh
```

macOS の実機検証はまだ完了していません。ビルドや動作の報告を歓迎します。

## 🎭 モデルの変更

モデルごとに専用のフォルダーを用意し、`.model3.json`、`.moc3`、テクスチャ、モーションなどの相対パスを維持してください。

| プラットフォーム | モデルライブラリ |
| --- | --- |
| Windows | `%USERPROFILE%\.local\share\l2dpet\models\` |
| Linux / macOS | `~/.local/share/l2dpet/models/` |

例：`models/MyPet/MyPet.model3.json`。設定パネルでモデルを選択したり、ZIP またはフォルダーをインポートしたりできます。Windows の ZIP インポートは標準の `tar.exe`（Windows 10 1803 以降）を使用します。初回インポート時にモデルライブラリのディレクトリを自動作成します。

使用許諾を得たモデルを使い、作者が定める使用・配布条件を守ってください。

## 💬 LLM の設定

設定パネルの「対話」ページでサービスの URL、モデル、API Key を入力します。設定は `~/.config/l2dpet/llm.json`、Windows では `%USERPROFILE%\.config\l2dpet\llm.json` に保存されます。

LLM は任意の機能です。未設定でもローカルのふれあいや内蔵セリフを利用できます。実際のリクエストが成功して初めて「接続済み」と表示され、失敗時には理由が表示されます。

LLM を有効にすると会話と関連するコンテキストが設定したプロバイダーに送信されます。API Key や個人設定をリポジトリにコミットしないでください。

## ❓ よくある質問

### クローンするだけでビルドできますか？

対応するビルドツールと公式 Cubism Native SDK の準備が必要です。Windows の初回ビルドでは libcurl のダウンロードにネット接続も必要です。

### API Key がなくても使えますか？

はい。基本的なふれあい機能はオンラインサービスに依存しません。

### Windows で起動しない場合、ログはどこにありますか？

`%TEMP%\l2dpet.log` を確認してください。単一ファイル版のランチャーログは `%TEMP%\l2dpet-portable.log` です。報告には OS バージョン、ビルド方法、関連ログを添え、個人情報は除いてください。

### Wayland で使えますか？

GLFW のデスクトップキャラクター用バックエンドがあります。透明表示、位置指定、最前面表示、マウス操作はコンポジターに依存するため、利用する環境で検証してください。

## 🧩 技術構成

`l2dtest` が Live2D キャラクター、行動、内蔵 LLM agent を担当し、`l2dpet-ui` がトレイと設定パネルを担当します。両者はローカルの `127.0.0.1:23333` で JSON Lines を使って通信します。

```text
live2d/player/          キャラクターと行動（C++17）
live2d/panel/           トレイと設定パネル（EUI-NEO）
live2d/3rd/             サードパーティ依存
live2d/bundled-models/  同梱サンプルモデル
live2d/tools/           デバッグツールとポータブルランチャー
scripts/               ビルド・パッケージ作成スクリプト
```

## 📜 ライセンス

本プロジェクト独自のコードは [MIT License](LICENSE) で公開しています。サードパーティのコンポーネントにはそれぞれのライセンスが適用され、許諾文と著作権表示を保持しています。

**Live2D Cubism Core はプロプライエタリのコンポーネントで、MIT の対象ではありません。** 使用・配布の際は、同梱ライセンスと Live2D 公式の条件に従ってください。

同梱の Hiyori モデルの著作権は Live2D Inc. に帰属し、Live2D Free Material License Agreement が適用されます。[モデルのライセンス説明](live2d/bundled-models/README.md) を参照してください。他のモデルには各作者の許諾条件が適用されます。

## 🙏 謝辞

Live2D、EUI-NEO、GLFW、FreeType、libcurl などの作者とコントリビューターの皆様に感謝します。

[Issues](https://github.com/jinsc123654/l2dpet/issues) での報告や Pull Request を歓迎します。一緒にこのデスクトップの仲間を育てていきましょう。

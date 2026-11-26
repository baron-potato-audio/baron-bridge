# Baron Bridge

オーディオアプリのステレオ出力を、WASAPI共有モードでWindowsの再生デバイスに送るWindows x64用ドライバーです。画面録画のシステム音声に演奏を含めたい場合に使えます。

1. Vesper Clav単体版の「オーディオ設定」で、ドライバーの一覧から「Baron Bridge」を選びます。
2. ドライバーの「Control Panel」でWindowsの再生先を選べます。初期設定ではWindowsの既定の再生先を使います。
3. 画面録画のシステム音声を有効にし、同じWindows再生先を収録します。

44.1、48、88.2、96、176.4、192 kHzのステレオ出力を受け取ります。再生先とサンプルレートが異なる場合はWindowsの変換を使います。

## ビルドと検証

Windows x64、CMake 3.22以降、MSVCのC++デスクトップ開発ツールとWindows SDK、Inno Setup 6を使います。この配布版はVisual Studio 2026とInno Setup 6.7.3でビルドしています。以下のコマンドは、この `README.md` がある `Windows\BaronBridge` の2つ上のフォルダーで実行します。

```powershell
.\Windows\Build-Baron-Bridge.ps1 -Generator 'Visual Studio 18 2026'
$dll = (Resolve-Path .\Windows\build-baron-bridge\Release\BaronBridge.dll).Path
& .\Windows\build-baron-bridge\Release\baron_bridge_check.exe $dll
```

CMakeでこのVisual Studioジェネレーターを使うにはCMake 4.2以降が必要です。Visual Studio 2022を使う場合はジェネレーターを `Visual Studio 17 2022` にします。Inno Setupを別の場所にインストールした場合は `-ISCC` でコンパイラーのパスを指定します。

ビルドに必要なSteinbergのヘッダーは `third_party/steinberg-sdk` に同梱しています。インストーラーの出力先は `Windows\dist\Baron-Bridge-x64-Setup.exe` です。自分でビルドしたインストーラーで、改変したDLLのインストールもできます。

専用テストは指定されたDLLを直接読み込み、左右別のテスト音が同じWASAPI再生先のループバックから取れることを確かめます。再生デバイスを有効にして実行してください。引数を省くとインストール済みのドライバーをテストします。

## ライセンス

Copyright (C) 2026 Baron Potato

Baron Bridgeは [GNU General Public License version 3](COPYING.txt) で配布します。第三者のコードとツールの出典およびライセンスは [THIRD-PARTY-NOTICES.txt](THIRD-PARTY-NOTICES.txt) を参照してください。

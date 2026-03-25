# TVTestHTTPPlugin ビルド・セットアップ手順

## 必要環境

| ソフトウェア | バージョン |
|---|---|
| Windows 10/11 x64 | - |
| Visual Studio | 2019 または 2022 |
| CMake | 3.20 以上 |
| インターネット接続 | ヘッダーダウンロードのため |

### Visual Studio に必要なコンポーネント

Visual Studio Installer で以下を確認・インストール:
- **C++ によるデスクトップ開発** (ワークロード)
- **MSVC v143 ビルドツール** (または v142)
- **Windows 10/11 SDK**
- **CMake ツール** (VS 組み込みか別途インストール)

---

## ビルド手順

### 1. CMake で構成

```powershell
# PowerShell で TVTestHTTPPlugin フォルダに移動
cd path\to\TVTestMCP\TVTestHTTPPlugin

# ビルドディレクトリを作成して構成 (x64 Release)
cmake -B build -A x64 -DCMAKE_BUILD_TYPE=Release
```

初回実行時に `TVTestPlugin.h` と `httplib.h` が自動でダウンロードされます。

### 2. ビルド

```powershell
cmake --build build --config Release
```

成功すると `build\Release\TVTestHTTPPlugin.tvtp` が生成されます。

---

## TVTest へのインストール

1. `TVTestHTTPPlugin.tvtp` を TVTest の `Plugins` フォルダにコピー

   ```
   TVTest_64bit\
   └── Plugins\
       └── TVTestHTTPPlugin.tvtp   ← ここに配置
   ```

2. TVTest を起動

3. メニュー → **プラグイン** → **TVTest HTTP API** を **有効** にする

4. HTTP サーバーが起動し、ポート **40152** で待ち受けを開始

---

## 動作確認

TVTest が起動している状態でブラウザやcurlでアクセス:

```bash
# 現在の状態を取得
curl http://WindowsのIPアドレス:40152/api/status

# チャンネル一覧を取得
curl http://WindowsのIPアドレス:40152/api/channels

# リモコン 3 ch に変更
curl -X POST http://WindowsのIPアドレス:40152/api/channel \
     -H "Content-Type: application/json" \
     -d '{"remoteControlKey":3}'

# 音量を 50 に設定
curl -X POST http://WindowsのIPアドレス:40152/api/volume \
     -H "Content-Type: application/json" \
     -d '{"volume":50}'
```

---

## ポート番号の変更

`TVTestHTTPPlugin.cpp` の先頭にある定数を変更してリビルド:

```cpp
static const int HTTP_PORT = 40152;  // ← 変更
```

---

## トラブルシューティング

| 症状 | 対処 |
|---|---|
| ビルドエラー: `TVTestPlugin.h not found` | ネット接続を確認し CMake を再実行 |
| ポート競合エラー | `HTTP_PORT` を変更してリビルド |
| チャンネル変更が効かない | チャンネルスキャン済みか確認。BonDriver が正常動作しているか確認 |
| 録画開始失敗 | 録画フォルダが設定されているか確認 |
| Windows ファイアウォール | ポート 40152 の受信規則を追加 |

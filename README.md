# TVTestHTTPPlugin

TVTest 向けの HTTP REST API プラグインです。外部アプリケーションから TVTest をリモート操作できます。

## 概要

TVTest に組み込みの HTTP サーバーを追加し、チャンネル切り替え・音量調節・録画操作・番組情報取得などを REST API 経由で制御できるようにします。
デフォルトのポートは **40152** です。

https://github.com/Ebycow/TVTest-MCPServer や https://github.com/Ebycow/TVTest-Discord-Bot 用のAPIとして使用できます。

## 機能

- チャンネル切り替え（リモコンキー番号 / `space` + `channel` 指定）
- 音量・ミュート制御
- 録画の開始・停止
- 現在放送中の番組情報取得
- 指定チャンネルまたは指定イベントの番組情報取得
- 複数チャンネルの番組情報一括取得
- BonDriver 切り替え
- TTRec 連携（予約一覧取得 / デフォルト設定での番組予約）
- IP アドレスによるアクセス制限（許可リスト / 拒否リスト）
- CORS 対応（ブラウザからの直接アクセス可）

## ビルド・インストール

詳細は [SETUP.md](SETUP.md) を参照してください。

### 必要環境

| ソフトウェア | バージョン |
|---|---|
| Windows 10/11 x64 | - |
| Visual Studio | 2019 または 2022 |
| CMake | 3.20 以上 |

### クイックビルド

```powershell
cmake -B build -A x64 -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

初回ビルド時に `TVTestPlugin.h` と `httplib.h` が自動でダウンロードされます。
成功すると `build\Release\TVTestHTTPPlugin.tvtp` が生成されます。

### インストール

`TVTestHTTPPlugin.tvtp` を TVTest の `Plugins` フォルダに配置し、TVTest のメニューからプラグインを有効化します。

## 設定

TVTest のプラグイン設定ダイアログから以下を変更できます（`TVTestHTTPPlugin.ini` に保存されます）。

| 設定項目 | デフォルト | 説明 |
|---|---|---|
| ポート番号 | 40152 | HTTP サーバーのリスンポート（1024〜65535） |
| 許可リスト | （空 = 全許可） | カンマ区切りの IP アドレス / CIDR ブロック |
| 拒否リスト | （空 = 拒否なし） | カンマ区切りの IP アドレス / CIDR ブロック |

IP フィルタリングは「拒否リストを先に評価 → 許可リストで確認」の順に処理されます。

## API リファレンス

すべてのエンドポイントは `http://<ホスト>:<ポート>` をベース URL とします。

### 状態・情報取得

| メソッド | エンドポイント | 説明 |
|---|---|---|
| GET | `/api/status` | 現在の状態（チャンネル・音量・ミュート・録画・番組情報） |
| GET | `/api/channels` | 利用可能なチャンネル一覧 |
| GET | `/api/volume` | 音量・ミュート状態 |
| GET | `/api/program` | 現在視聴中チャンネルの番組情報 |
| GET | `/api/driver` | 現在の BonDriver と利用可能なドライバ一覧 |
| GET | `/api/record/status` | 録画状態 |

### 操作

| メソッド | エンドポイント | 説明 |
|---|---|---|
| POST | `/api/channel` | チャンネル変更 |
| POST | `/api/volume` | 音量・ミュート設定 |
| POST | `/api/record/start` | 録画開始 |
| POST | `/api/record/stop` | 録画停止 |
| POST | `/api/driver` | BonDriver 切り替え |

### 番組情報取得

| メソッド | エンドポイント | 説明 |
|---|---|---|
| GET | `/api/program/channel` | 指定チャンネルの現在番組、または `eventId` 指定時はそのイベント情報を取得 |
| POST | `/api/program/channels` | 複数チャンネルの現在番組を一括取得 |

### TTRec 連携

https://github.com/xtne6f/TTRec

| メソッド | エンドポイント | 説明 |
|---|---|---|
| GET | `/api/ttrec/reserves` | TTRec の予約一覧を取得 |
| POST | `/api/ttrec/reserve/default` | TTRec のデフォルト設定で番組を予約 |

### 入力形式メモ

- `POST /api/channel` は `{"remoteControlKey":3}` または `{"space":0,"channel":5}` を受け付けます。
- `GET /api/program/channel` は `space` + `channel` または `networkId` + `serviceId` を受け付けます。`transportStreamId` は省略可能です。
- `GET /api/program/channel` は `eventId` を付けると現在番組ではなく、そのイベント情報を取得します。
- `POST /api/program/channels` は JSON 配列で、各要素に `space` + `channel` または `networkId` + `serviceId` を指定します。
- `POST /api/driver` は `{"driver":"BonDriver_Proxy_S.dll"}` に加えて、切り替え後のチャンネルとして `remoteControlKey` または `space` + `channel` を任意で指定できます。
- `POST /api/ttrec/reserve/default` は `onid` / `tsid` / `sid` / `eid` を優先して受け付け、後方互換として `networkId` / `transportStreamId` / `serviceId` / `eventId` も利用できます。
- `startTime` は `YYYY-MM-DDTHH:MM:SS` 形式、`duration` は秒単位です。

### リクエスト / レスポンス例

```bash
# 現在の状態を取得
curl http://localhost:40152/api/status

# チャンネル一覧を取得
curl http://localhost:40152/api/channels

# リモコン 3 ch に変更
curl -X POST http://localhost:40152/api/channel \
     -H "Content-Type: application/json" \
     -d '{"remoteControlKey":3}'

# スペース+チャンネルインデックスで変更
curl -X POST http://localhost:40152/api/channel \
     -H "Content-Type: application/json" \
     -d '{"space":0,"channel":5}'

# 音量を 50 に設定
curl -X POST http://localhost:40152/api/volume \
     -H "Content-Type: application/json" \
     -d '{"volume":50}'

# ミュートを有効化
curl -X POST http://localhost:40152/api/volume \
     -H "Content-Type: application/json" \
     -d '{"mute":true}'

# 録画開始 / 停止
curl -X POST http://localhost:40152/api/record/start
curl -X POST http://localhost:40152/api/record/stop

# 特定チャンネルの番組情報取得（networkId + serviceId 指定）
curl "http://localhost:40152/api/program/channel?networkId=32736&serviceId=1024"

# eventId を指定して特定イベントの情報を取得
curl "http://localhost:40152/api/program/channel?networkId=32736&serviceId=1024&eventId=12345"

# TTRec の予約一覧を取得
curl http://localhost:40152/api/ttrec/reserves

# TTRec に予約追加（EDCB 互換キー指定）
curl -X POST http://localhost:40152/api/ttrec/reserve/default \
     -H "Content-Type: application/json" \
     -d '{"onid":32736,"tsid":32736,"sid":1024,"eid":12345,"startTime":"2026-03-26T02:00:00","duration":1800}'
```

## トラブルシューティング

| 症状 | 対処 |
|---|---|
| ビルドエラー: `TVTestPlugin.h not found` | インターネット接続を確認し CMake を再実行 |
| ポート競合エラー | 設定ダイアログでポート番号を変更 |
| チャンネル変更が効かない | チャンネルスキャン済みか確認。BonDriver が正常動作しているか確認 |
| 録画開始失敗 | 録画フォルダが設定されているか確認 |
| 接続できない | Windows ファイアウォールでポート 40152 の受信規則を追加 |

## 依存ライブラリ

- [TVTestPlugin.h](https://github.com/DBCTRADO/TVTest) — TVTest プラグイン SDK
- [cpp-httplib](https://github.com/yhirose/cpp-httplib) — ヘッダーオンリー HTTP サーバーライブラリ

両ライブラリは CMake の初回ビルド時に自動でダウンロードされます。

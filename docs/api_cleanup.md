# 未使用パブリックAPI整理 設計書

## 背景

libmucom88は以下の2プロジェクトからのみ参照されている:
- **MUCOM88V** (`takamori-tech/mucom88v`) — VST/AUプラグイン
- **CLAUDIUS** (`takamori-tech/rpi5-native-game`) — レトロSTGゲーム

両プロジェクトの全ソースコードをスキャンし、libmucom88のパブリックAPIの使用状況を調査した。
その結果、後方互換性のために残されていた旧APIや、内部実装が誤ってpublicとして公開されている
メソッドが複数見つかった。外部利用者がいないため安全に整理できる。

## 調査結果サマリ

### MmlEngine パブリックメソッド使用状況

| メソッド | mucom88v | CLAUDIUS | 判定 |
|---------|----------|----------|------|
| `loadMml(mml, ch)` | ✗ | ✗ | **削除** |
| `loadPcmFile(path)` | ✗ | ✗ | **削除** |
| `globalLoopRestart()` | ✗ | ✗ | **private化** |
| `perChannelRestart(ch)` | ✗ | ✗ | **private化** |
| `setGlobalAttenuation(att)` | ✗ | ✗ | **private化** |
| `globalAttenuation()` | ✗ | ✗ | **private化** |

### フリー関数

| 関数 | mucom88v | CLAUDIUS | 判定 |
|------|----------|----------|------|
| `parseSingleChannelMml()` | ✗ | ✗ | **削除** |
| `makeDefaultPatch()` | ✗ | ✗ | **クラス内private化** |

### 保持するAPI（未使用だがAPI設計上有効）

以下はどちらのプロジェクトからも直接呼ばれていないが、設計上有効なAPIとして保持する:

- SE制御: `playSe()`, `stopSe()`, `stopAllSe()`, `setSeFrequency()`, `isSeActive()`, `activeSeCount()`, `seMode()`
- Classic SEモード: `hijackChannel()`, `releaseChannel()`, `isChannelHijacked()`
- フェード: `fadeIn()`, `isFading()`
- PCMロード: `loadPcmBinary(data, size)` — `loadPcmBinaryFile()`が内部で呼出
- ボイス: `loadVoiceTable(path)` — ファイルパス版パススルー
- チャンネル情報: `isFM()`, `isSSG()` 等のstatic判定メソッド
- パーサー: `hasVoiceDat()`, `trackCharToChannel()`
- 定数: `MAX_MML_CHANNELS`, `PPQ`, `WHOLE_TICK` 等

---

## 変更一覧

### 1. `MmlEngine::loadMml()` の削除

**理由:** `setEvents()` / `loadFromParseResult()` に完全に置き換え済み。内部で `parseSingleChannelMml()` を呼ぶ旧インターフェース。

```cpp
// 削除対象（mml_engine.hpp）
void loadMml(const std::string& mml, int ch = 0)
```

### 2. `parseSingleChannelMml()` フリー関数の削除

**理由:** `loadMml()` の削除に伴い、呼び出し元がゼロになる。mml_parser.hpp末尾のフリー関数。

```cpp
// 削除対象（mml_parser.hpp）
inline std::vector<MmlEvent> parseSingleChannelMml(const std::string& mml, int channel = 0)
```

### 3. `MmlEngine::loadPcmFile()` の削除

**理由:** PCMテーブルのみロードし、ADPCMデータは別途ロードが必要な紛らわしいAPI。
`loadPcmBinaryFile()` がテーブル+データの統合ロードを提供しており、完全な上位互換。

```cpp
// 削除対象（mml_engine.hpp）
[[nodiscard]] bool loadPcmFile(const std::string& path)
```

### 4. `globalLoopRestart()` の private 化

**理由:** `advance()` 内部からのみ呼ばれる。コメントに「外部からの明示的呼び出し用に残す」
とあるが、両プロジェクトとも外部呼び出しなし。public APIとして不要。

### 5. `perChannelRestart()` の private 化

**理由:** `advance()` 内部からのみ呼ばれる。外部呼び出しなし。

### 6. `setGlobalAttenuation()` / `globalAttenuation()` の private 化

**理由:** ダッキング機構の内部API。`setDucking()` が上位APIとして公開済み。
コメントに「後方互換」とあるが、両プロジェクトとも `setDucking()` のみ使用。

### 7. `makeDefaultPatch()` のクラス内 private static 化

**理由:** `MmlEngine::init()` からのみ呼ばれるファイルスコープ関数。
ODRリスクは `inline` で解消済みだが、API表面の明確化のためクラス内に移動。

---

## 影響範囲

- `mml_engine.hpp`: 6メソッドのアクセス修飾変更 + 2メソッド削除 + 1関数移動
- `mml_parser.hpp`: 1フリー関数削除
- `fm_common.hpp`: 変更なし
- `fm_engine_interface.hpp`: 変更なし

## 後方互換性

- 両プロジェクトのビルドに影響なし（未使用APIのみ対象）
- 外部利用者なし（git submodule経由の2プロジェクトのみ）

## 検証

```bash
cd ~/git-projects/mucom88v && cmake --build build -- -j8
build/muc_regtest -sec 30
```

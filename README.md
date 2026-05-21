# libmucom88

MUCOM88互換 MMLパーサー＋シーケンサー＋ADPCM-Bボイス再生ライブラリ（YM2608 / OPNA）。

ヘッダーオンリーC++17。外部依存なし。

## 概要

[MUCOM88](https://www.ancient.co.jp/~mucom88/)（古代祐三氏がNEC PC-8801向けに開発した音楽ドライバー）と互換のMMLパーサー＋シーケンサーを提供する。MMLテキストからYM2608のレジスタ書き込みを生成し、任意のYM2608エミュレータ（fmgen等）をバックエンドとして使用できる。

BGM再生に加え、ADPCM-Bを使ったゲームボイス再生にも対応。BGM再生中にボイスを差し込む際のKトラック優先制御（BGMのADPCM-Bを自動抑制）を内蔵。

## アーキテクチャ

```
MUCテキスト (.muc)
    │
    ▼
MmlParser ── パース、マクロ展開、イベント列生成
    │
    ▼
MmlEngine ── シーケンス再生、Timer-B駆動、レジスタ書き込み
    │         ボイス再生時のKトラック優先制御
    ▼
IFmEngine ── 抽象インターフェース（writeReg, generateInterleaved, ...）
    │
    ▼
[YM2608エミュレータ]  （fmgen 等）
```

## クイックスタート

```cpp
#include <mucom88/mml_parser.hpp>
#include <mucom88/mml_engine.hpp>
#include <mucom88/fm_engine_interface.hpp>

// 1. IFmEngine を YM2608 エミュレータで実装
class MyFmEngine : public IFmEngine { /* ... */ };

// 2. MMLパース
MmlParser parser;
parser.loadVoiceDat("voice.dat");
auto result = parser.parse(mmlText);

// 3. エンジンセットアップ
MyFmEngine fmEngine;
fmEngine.init(44100);

MmlEngine engine;
engine.init(&fmEngine, 44100);
for (auto& [no, patch] : result.patches)
    engine.setPatch(no, patch);
engine.setWholeTick(result.wholeTick);
for (int ch = 0; ch < 11; ch++)
    engine.setEvents(ch, result.channelEvents[ch]);

// 4. 再生
engine.setLoop(true);
engine.play();
while (engine.isPlaying()) {
    engine.advance(256);
    int16_t buf[512];
    fmEngine.generateInterleaved(buf, 256);
    // ... buf をオーディオデバイスへ出力
}

// 5. ボイス再生（BGM中に差し込み可能）
fmEngine.loadVoiceTable("voice_table.bin");
engine.playVoice(0);  // BGMのKトラックは自動抑制
```

## ドキュメント

- **[ゲームプログラム組み込みガイド](docs/integration_guide.md)** — IFmEngine実装、BGM再生、ボイス再生、ダッキング
- **[APIリファレンス](docs/api_reference.md)** — 全クラス・メソッドの詳細

## ファイル構成

| ファイル | 内容 |
|---------|------|
| `fm_common.hpp` | FM音色定義（FmPatch）、周波数変換、voice.datパーサー |
| `fm_engine_interface.hpp` | IFmEngine 抽象インターフェース |
| `mml_parser.hpp` | MMLパーサー（MUCOM88形式、132曲検証済み） |
| `mml_engine.hpp` | MMLシーケンサー（Timer-B駆動、11ch、リバーブ、LFO、ポルタメント） |

## 対応MML機能

- **11チャンネル**: FM(6ch) + SSG(3ch) + リズム(1ch) + ADPCM-B(1ch)
- **音符**: `cdefgab`、オクターブ `<>o`、シャープ `+#`、フラット `-`、休符 `r`
- **音長**: `l`デフォルト、数値、付点`.`（複数）、タイ `&`/`^`
- **音量**: `v`（FM: FMVDATテーブル、SSG: 0-15、ADPCM-B: 0-255）、`()`相対
- **テンポ**: `t`(BPM)、`T`(Timer-B直接)、`C`(クロック)
- **音色**: `@N`(voice.dat/インライン)、`@"name"`(名前検索)
- **ループ**: `[...]N`、`/`(ブレーク)、`L`(曲全体ループ)
- **マクロ**: `*N{...}`定義、`*N`展開
- **エフェクト**: `q`スタッカート、`D`デチューン、`M`ビブラート、`H`ハードウェアLFO
- **リバーブ**: `R`(擬似リバーブ)、`RF`(ON/OFF)、`Rm`(モード)
- **ポルタメント**: `{note1 note2}`
- **エコー**: `\=N,M` / `\`
- **SSG**: `@N`プリセット(SOFENVソフトウェアエンベロープ)、`E`カスタムADSR
- **リズム**: `@`楽器ビットマスク、`v`楽器別レベル
- **ADPCM-B**: Kトラック、delta-Nピッチ、mucompcm.binマルチサンプル

## 組み込み方法

```bash
git submodule add https://github.com/takamori-tech/libmucom88.git vendor/libmucom88
```

```cmake
target_include_directories(your_target PRIVATE vendor/libmucom88/include)
```

## 利用プロジェクト

- [CLAUDIUS](https://github.com/takamori-tech/rpi5-native-game) — レトロSTGゲーム（Raspberry Pi）
- [MUCOM88V](https://github.com/takamori-tech/mucom88v) — YM2608 VST/AUプラグイン

## ライセンス

MIT License

## クレジット

- MML形式: [MUCOM88](https://www.ancient.co.jp/~mucom88/) by 古代祐三
- パーサー/シーケンサー: takamori-tech + Claude (Anthropic)

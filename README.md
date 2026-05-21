# libmucom88

MUCOM88互換 MMLパーサー＋シーケンサー＋ADPCM-Bボイス再生ライブラリ（YM2608 / OPNA）。  
A MUCOM88-compatible MML parser, sequencer, and ADPCM-B voice playback library for YM2608 (OPNA).

ヘッダーオンリーC++17。外部依存なし。  
Header-only C++17. No external dependencies.

## 概要 / Overview

[MUCOM88](https://www.ancient.co.jp/~mucom88/)（古代祐三氏がNEC PC-8801向けに開発した音楽ドライバー）と互換のMMLパーサー＋シーケンサーを提供する。MMLテキストからYM2608のレジスタ書き込みを生成し、任意のYM2608エミュレータ（fmgen等）をバックエンドとして使用できる。  
Provides a MML parser and sequencer compatible with [MUCOM88](https://www.ancient.co.jp/~mucom88/) (a music driver for the NEC PC-8801 by Yuzo Koshiro). Generates YM2608 register writes from MML text, using any YM2608 emulator (e.g. fmgen) as the backend.

BGM再生に加え、ADPCM-Bを使ったゲームボイス再生にも対応。BGM再生中にボイスを差し込む際のKトラック優先制御（BGMのADPCM-Bを自動抑制）と自動ダッキング（FM/SSG減衰）を内蔵。  
Supports ADPCM-B game voice playback alongside BGM. Built-in K-track priority control (auto-suppresses BGM ADPCM-B) and automatic ducking (FM/SSG attenuation) during voice playback.

## アーキテクチャ / Architecture

```
MUCテキスト (.muc)
    │
    ▼
MmlParser ── パース、マクロ展開、イベント列生成
    │
    ▼
MmlEngine ── シーケンス再生、Timer-B駆動、レジスタ書き込み
    │         ボイス再生時のKトラック優先制御 + 自動ダッキング
    ▼
IFmEngine ── 抽象インターフェース（writeReg, generateInterleaved, ...）
    │
    ▼
[YM2608エミュレータ]  （fmgen 等）
```

## クイックスタート / Quick Start

```cpp
#include <mucom88/mml_parser.hpp>
#include <mucom88/mml_engine.hpp>
#include <mucom88/fm_engine_interface.hpp>

// 1. IFmEngine を YM2608 エミュレータで実装
//    Implement IFmEngine with your YM2608 emulator
class MyFmEngine : public IFmEngine { /* ... */ };

// 2. MMLパース / Parse MML
MmlParser parser;
parser.loadVoiceDat("voice.dat");
auto result = parser.parse(mmlText);

// 3. エンジンセットアップ / Engine setup
MyFmEngine fmEngine;
fmEngine.init(44100);

MmlEngine engine;
engine.init(&fmEngine, 44100);
for (auto& [no, patch] : result.patches)
    engine.setPatch(no, patch);
engine.setWholeTick(result.wholeTick);
for (int ch = 0; ch < 11; ch++)
    engine.setEvents(ch, result.channelEvents[ch]);

// 4. 再生 / Playback
engine.setLoop(true);
engine.play();
while (engine.isPlaying()) {
    engine.advance(256);
    int16_t buf[512];
    fmEngine.generateInterleaved(buf, 256);
    // ... buf をオーディオデバイスへ出力 / output buf to audio device
}

// 5. ボイス再生（BGM中に差し込み可能）
//    Voice playback (can be triggered during BGM)
fmEngine.loadVoiceTable("voice_table.bin");
engine.playVoice(0);  // BGMのKトラックは自動抑制、FM/SSGは自動ダッキング
```

## ファイル構成 / File Structure

| ファイル / File | 内容 / Description |
|---------|------|
| `fm_common.hpp` | FM音色定義（FmPatch）、周波数変換、voice.datパーサー / FM patch definitions, frequency conversion, voice.dat parser |
| `fm_engine_interface.hpp` | IFmEngine 抽象インターフェース / IFmEngine abstract interface |
| `mml_parser.hpp` | MMLパーサー（MUCOM88形式、132曲検証済み）/ MML parser (MUCOM88 format, verified with 132 songs) |
| `mml_engine.hpp` | MMLシーケンサー（Timer-B駆動、11ch、リバーブ、LFO、ポルタメント）/ MML sequencer (Timer-B driven, 11ch, reverb, LFO, portamento) |

## ドキュメント / Documentation

- **[ゲームプログラム組み込みガイド / Integration Guide](docs/integration_guide.md)** — IFmEngine実装、BGM再生、ボイス再生、ダッキング
- **[APIリファレンス / API Reference](docs/api_reference.md)** — 全クラス・メソッドの詳細

## 対応MML機能 / Supported MML Features

- **11チャンネル / 11 channels**: FM(6ch) + SSG(3ch) + リズム/Rhythm(1ch) + ADPCM-B(1ch)
- **音符 / Notes**: `cdefgab`、オクターブ `<>o`、シャープ `+#`、フラット `-`、休符 `r`
- **音長 / Duration**: `l`デフォルト、数値、付点`.`（複数）、タイ `&`/`^`
- **音量 / Volume**: `v`（FM: FMVDATテーブル、SSG: 0-15、ADPCM-B: 0-255）、`()`相対
- **テンポ / Tempo**: `t`(BPM)、`T`(Timer-B直接)、`C`(クロック)
- **音色 / Patch**: `@N`(voice.dat/インライン)、`@"name"`(名前検索)
- **ループ / Loop**: `[...]N`、`/`(ブレーク)、`L`(曲全体ループ)
- **マクロ / Macro**: `*N{...}`定義、`*N`展開
- **エフェクト / Effects**: `q`スタッカート、`D`デチューン、`M`ビブラート、`H`ハードウェアLFO
- **リバーブ / Reverb**: `R`(擬似リバーブ)、`RF`(ON/OFF)、`Rm`(モード)
- **ポルタメント / Portamento**: `{note1 note2}`
- **エコー / Echo**: `\=N,M` / `\`
- **SSG**: `@N`プリセット(SOFENVソフトウェアエンベロープ)、`E`カスタムADSR
- **リズム / Rhythm**: `@`楽器ビットマスク、`v`楽器別レベル
- **ADPCM-B**: Kトラック、delta-Nピッチ、mucompcm.binマルチサンプル

## 組み込み方法 / Installation

```bash
git submodule add https://github.com/takamori-tech/libmucom88.git vendor/libmucom88
```

```cmake
target_include_directories(your_target PRIVATE vendor/libmucom88/include)
```

## 利用プロジェクト / Projects Using This Library

- [MUCOM88V](https://github.com/takamori-tech/mucom88v) — YM2608 VST3/AUプラグイン / YM2608 FM synthesizer plugin
- [CLAUDIUS](https://github.com/takamori-tech/rpi5-native-game) — レトロSTGゲーム（Raspberry Pi）/ Retro STG game

## ライセンス / License

MIT License

## クレジット / Credits

- MML形式 / MML format: [MUCOM88](https://www.ancient.co.jp/~mucom88/) by 古代祐三 / Yuzo Koshiro
- パーサー/シーケンサー / Parser & Sequencer: takamori-tech + Claude (Anthropic)

# libmucom88

MUCOM88互換 MMLパーサー＋シーケンサー＋ADPCM-Bボイス再生ライブラリ（YM2608 / OPNA）。  
A MUCOM88-compatible MML parser, sequencer, and ADPCM-B voice playback library for YM2608 (OPNA).

ヘッダーオンリーC++17。コアは外部依存なし。ymfm互換アダプタを使う場合のみ、利用側でymfmのヘッダとリンクを追加する。  
Header-only C++17. The core has no external dependencies. The optional ymfm adapter requires the consumer to add ymfm headers and linkage.

CMake の `INTERFACE` ターゲットとして組み込み可能で、単体ビルドでは付属ツールとヘッダースモークテストもビルドできる。  
Can be consumed as a CMake `INTERFACE` target, and the standalone build also builds the bundled tool and header smoke test.

`mucom88/regression_metrics.hpp` と `mucom88/test_muc_data.hpp` は、mucom88v / CLAUDIUS などの consumer が同じ MUC コーパスと A/B 回帰判定を再利用するための共有テスト基盤。
`mucom88/regression_metrics.hpp` and `mucom88/test_muc_data.hpp` provide shared MUC corpus and A/B regression helpers for consumers such as mucom88v and CLAUDIUS.

## 概要 / Overview

[MUCOM88](https://www.ancient.co.jp/~mucom88/)（古代祐三氏がNEC PC-8801向けに開発した音楽ドライバー）と互換のMMLパーサー＋シーケンサーを提供する。MMLテキストからYM2608のレジスタ書き込みを生成し、任意のYM2608エミュレータ（fmgen等）をバックエンドとして使用できる。  
Provides a MML parser and sequencer compatible with [MUCOM88](https://www.ancient.co.jp/~mucom88/) (a music driver for the NEC PC-8801 by Yuzo Koshiro). Generates YM2608 register writes from MML text, using any YM2608 emulator (e.g. fmgen) as the backend.

BGM再生に加え、ADPCM-Bを使ったゲームボイス再生にも対応。BGM再生中にボイスを差し込む際のKトラック優先制御（BGMのADPCM-Bを自動抑制）と自動ダッキング（FM/SSG減衰）を内蔵。  
Supports ADPCM-B game voice playback alongside BGM. Built-in K-track priority control (auto-suppresses BGM ADPCM-B) and automatic ducking (FM/SSG attenuation) during voice playback.

YM2608 ADPCM-A（リズム音源）の整数デコード/エンコードヘッダーも含む。付属の `drumkit_gen` は、ユーザー自身が用意した WAV ファイルから 8192 バイトの ADPCM-A リズム ROM を生成する。サンプルデータは同梱しないため、ライセンスクリーンに利用できる。  
It also includes integer YM2608 ADPCM-A (rhythm) decode/encode headers. The bundled `drumkit_gen` tool builds an 8192-byte ADPCM-A rhythm ROM from WAV files supplied by the user. No sample data is shipped, keeping the library license-clean.

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
| `CMakeLists.txt` | ヘッダーオンリー `mucom88::mucom88` ターゲット、付属ツール/テストの単体ビルド / Header-only `mucom88::mucom88` target plus standalone tool/test build |
| `include/mucom88/adpcm_a_decode.hpp` | YM2608 ADPCM-A リズム ROM デコード / YM2608 ADPCM-A rhythm ROM decoder |
| `include/mucom88/adpcm_a_encode.hpp` | YM2608 ADPCM-A リズム ROM エンコード / YM2608 ADPCM-A rhythm ROM encoder |
| `chip_output_tuning.hpp` | engine別のNative/Tuned出力プリセット / Engine-specific Native/Tuned output presets |
| `logical_stem_mixer.hpp` | opt-in 64-bit logical stem summing helper / opt-in 64-bit logical stem summing helper |
| `fm_common.hpp` | FM音色定義（FmPatch）、周波数変換、voice.datパーサー / FM patch definitions, frequency conversion, voice.dat parser |
| `fm_engine_interface.hpp` | IFmEngine 抽象インターフェース / IFmEngine abstract interface |
| `ymfm_engine.hpp` | optional ymfm OPNA `IFmEngine` 互換アダプタ / optional ymfm OPNA `IFmEngine` adapter |
| `mml_parser.hpp` | MMLパーサー（MUCOM88形式、132曲検証済み）/ MML parser (MUCOM88 format, verified with 132 songs) |
| `mml_engine.hpp` | MMLシーケンサー（Timer-B駆動、11ch、リバーブ、LFO、ポルタメント）/ MML sequencer (Timer-B driven, 11ch, reverb, LFO, portamento) |
| `tools/drumkit_gen.cpp` | WAV から YM2608 ADPCM-A リズム ROM を生成する CLI / CLI that builds a YM2608 ADPCM-A rhythm ROM from WAV files |

## ドキュメント / Documentation

- **[ゲームプログラム組み込みガイド / Integration Guide](docs/integration_guide.md)** — IFmEngine実装、BGM再生、ボイス再生、ダッキング
- **[APIリファレンス / API Reference](docs/api_reference.md)** — 全クラス・メソッドの詳細
- **[Logical Stem Mixing](docs/logical_stem_mixing.md)** — opt-in 64-bit stem summing、headroom、backend ordering

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

### CMake

親プロジェクトから `add_subdirectory` すると、`mucom88::mucom88` をリンクするだけで `include/` が設定される。  
When used through `add_subdirectory`, link `mucom88::mucom88` and the `include/` path is configured automatically.

```cmake
add_subdirectory(vendor/libmucom88)
target_link_libraries(your_target PRIVATE mucom88::mucom88)
```

### Include Path Only

CMake を使わない場合は `include` をインクルードパスへ追加する。  
Without CMake, add `include` to your compiler include path.

```cmake
target_include_directories(your_target PRIVATE vendor/libmucom88/include)
```

```bash
g++ -std=c++17 -I vendor/libmucom88/include your_app.cpp -o your_app
```

## 単体ビルド / Standalone Build

単体ビルドではヘッダーオンリーライブラリに加えて、`drumkit_gen` とヘッダースモークテストをビルドする。  
The standalone build creates the header-only target, the `drumkit_gen` tool, and a header smoke test.

```bash
cmake -B build .
cmake --build build
ctest --test-dir build --output-on-failure
```

CMake オプションでツール/テストを個別に無効化できる。  
Tools and tests can be disabled independently with CMake options.

```bash
cmake -B build . -DLIBMUCOM88_BUILD_TOOLS=OFF -DLIBMUCOM88_BUILD_TESTS=OFF
```

## drumkit_gen

`drumkit_gen` は、6つの WAV ファイル（BD/SD/CY/HH/TM/RS）から YM2608 ADPCM-A リズム ROM（8192 バイト）を生成する CLI ツール。省略したスロットは無音になり、`-base` を指定すると既存 ROM の未指定スロットを保持して差し替えできる。  
`drumkit_gen` is a CLI tool that builds a YM2608 ADPCM-A rhythm ROM (8192 bytes) from up to six WAV files (BD/SD/CY/HH/TM/RS). Omitted slots become silence, or with `-base`, omitted slots keep the existing ROM data.

このリポジトリは著作権のあるドラムサンプルを同梱しない。自分で録音・作成した WAV、または利用許諾のある WAV を指定して、自分専用のリズム ROM を生成する。  
This repository does not ship copyrighted drum samples. Use WAV files you recorded, created, or are licensed to use, and generate your own rhythm ROM.

```bash
./build/drumkit_gen -o my_drums.bin \
  -bd BD.wav -sd SD.wav -cy CY.wav \
  -hh HH.wav -tm TM.wav -rs RS.wav
```

```text
Usage: drumkit_gen -o <output.bin> [-bd BD.wav] [-sd SD.wav] [-cy CY.wav]
                                  [-hh HH.wav] [-tm TM.wav] [-rs RS.wav]
       drumkit_gen -o <output.bin> -base <rom.bin> [-bd BD.wav] ...
```

CMake なしでも単体コンパイルできる。  
It can also be compiled directly without CMake.

```bash
g++ -std=c++17 -O2 -I include tools/drumkit_gen.cpp -o drumkit_gen
```

## 利用プロジェクト / Projects Using This Library

- [MUCOM88V](https://github.com/takamori-tech/mucom88v) — YM2608 VST3/AUプラグイン / YM2608 FM synthesizer plugin
- [CLAUDIUS](https://github.com/takamori-tech/rpi5-native-game) — レトロSTGゲーム（Raspberry Pi）/ Retro STG game

## ライセンス / License

MIT License

## クレジット / Credits

- MML形式 / MML format: [MUCOM88](https://www.ancient.co.jp/~mucom88/) by 古代祐三 / Yuzo Koshiro
- パーサー/シーケンサー / Parser & Sequencer: takamori-tech + Claude (Anthropic)

# libmucom88

MUCOM88互換 MMLパーサー＋シーケンサー＋ADPCM-Bボイス再生ライブラリ（YM2608 / OPNA）。
A MUCOM88-compatible MML parser, sequencer, and ADPCM-B voice playback library for YM2608 (OPNA).

ヘッダーオンリーC++17。コアは外部依存なし。ymfm互換アダプタを使う場合のみ、利用側でymfmのヘッダとリンクを追加する。
Header-only C++17. The core has no external dependencies. The optional ymfm adapter requires the consumer to add ymfm headers and linkage.

CMake の `INTERFACE` ターゲットとして組み込み可能で、単体ビルドでは付属ツールと単体テストをビルドできる。
Can be consumed as a CMake `INTERFACE` target, and the standalone build also builds the bundled tool and unit tests.

`mucom88/regression_metrics.hpp` と `mucom88/test_muc_data.hpp` は、利用側が同じMUCコーパスとA/B回帰判定を再利用するための共有テスト基盤。
`mucom88/regression_metrics.hpp` and `mucom88/test_muc_data.hpp` provide shared MUC corpus and A/B regression helpers for applications embedding the library.

## 概要 / Overview

[MUCOM88](https://www.ancient.co.jp/~mucom88/)（古代祐三氏がNEC PC-8801向けに開発した音楽ドライバー）と互換のMMLパーサー＋シーケンサーを提供する。MMLテキストからYM2608のレジスタ書き込みを生成し、任意のYM2608エミュレータ（fmgen等）をバックエンドとして使用できる。
Provides a MML parser and sequencer compatible with [MUCOM88](https://www.ancient.co.jp/~mucom88/) (a music driver for the NEC PC-8801 by Yuzo Koshiro). Generates YM2608 register writes from MML text, using any YM2608 emulator (e.g. fmgen) as the backend.

BGM再生に加え、ADPCM-Bを使ったゲームボイス再生にも対応。BGM再生中にボイスを差し込む際のKトラック優先制御（BGMのADPCM-Bを自動抑制）と自動ダッキング（FM/SSG減衰）を内蔵。
Supports ADPCM-B game voice playback alongside BGM. Built-in K-track priority control (auto-suppresses BGM ADPCM-B) and automatic ducking (FM/SSG attenuation) during voice playback.

YM2608 ADPCM-A（リズム音源）の整数デコード/エンコードヘッダーも含む。付属の `drumkit_gen` は、ユーザー自身が用意した WAV ファイルから 8192 バイトの ADPCM-A リズム ROM を生成する。このツール用のWAV・リズムROMは同梱しない。
It also includes integer YM2608 ADPCM-A (rhythm) decode/encode headers. The bundled `drumkit_gen` tool builds an 8192-byte ADPCM-A rhythm ROM from WAV files supplied by the user. WAV samples and rhythm ROM images for this tool are not bundled.

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

利用側で `IFmEngine` を実装するか、optional `FmEngineYmfm` アダプタを使用する。
以下は既存バックエンドを受け取る組み込み例。初期化・パース・イベント設定は音声コールバック外で行う。
Implement `IFmEngine` or use the optional `FmEngineYmfm` adapter. Initialize, parse, and load events outside the audio callback.

```cpp
#include <cstdint>
#include <string>
#include <mucom88/mml_parser.hpp>
#include <mucom88/mml_engine.hpp>
#include <mucom88/fm_engine_interface.hpp>

// 音声スレッドを停止した状態で初期化する。chipはengineより長く生存させる。
void prepareSong(MmlEngine& engine, IFmEngine& chip,
                 const std::string& mmlText, uint32_t sampleRate)
{
    MmlParser parser;
    // voice.datが必要なら、parse前にloadVoiceDat(path)の成功を確認する。
    const auto song = parser.parse(mmlText);
    chip.init(sampleRate);
    engine.init(&chip, sampleRate);
    engine.loadFromParseResult(song);
    engine.setLoop(false);
    engine.play();
}

// outは利用側が用意したframeCount * 2要素のint16_tバッファ（L/R交互）。
void renderAudio(MmlEngine& engine, int16_t* out, uint32_t frameCount) noexcept
{
    engine.renderMixed(out, frameCount);
}
```

`renderMixed()` がシーケンサーの進行とPCM生成を小区間で処理するため、同じブロックで
`advance()` やバックエンドの `generateInterleaved()` を別途呼ばない。
`renderMixed()` advances sequencing and generates PCM in small chunks; do not separately advance or generate the same block.

パーサーの `parse()` は解析結果を返すが、全構文の受理を保証する診断APIではない。
未知の指示や不正入力の扱いは実装を確認し、外部入力のサイズ・数値・展開量は利用側でも制限する。
API・音声スレッドの所有権、音色/PCM・ボイステーブルの設定は
[組み込みガイド](docs/integration_guide.md)を参照。
Parsing does not certify every input directive. Apply input/resource limits in the host and consult the integration guide for assets and ownership.

## ファイル構成 / File Structure

| ファイル / File | 内容 / Description |
|---------|------|
| `CMakeLists.txt` | ヘッダーオンリー `mucom88::mucom88` ターゲット、付属ツール/テストの単体ビルド / Header-only `mucom88::mucom88` target plus standalone tool/test build |
| `include/mucom88/adpcm_a_decode.hpp` | YM2608 ADPCM-A リズム ROM デコード / YM2608 ADPCM-A rhythm ROM decoder |
| `include/mucom88/adpcm_a_encode.hpp` | YM2608 ADPCM-A リズム ROM エンコード / YM2608 ADPCM-A rhythm ROM encoder |
| `chip_output_tuning.hpp` | engine別のNative/Tuned出力プリセット / Engine-specific Native/Tuned output presets |
| `post_chip_processor.hpp` | チップ出力後のoptional DC除去・EQ・出力処理 / Optional post-chip DC filtering, EQ and output processing |
| `regression_metrics.hpp` / `test_muc_data.hpp` | 回帰評価ヘルパーと埋め込みMUC/音色/PCMテストデータ（通常のコア利用には不要）/ Optional regression metrics and embedded MUC/voice/PCM test data |
| `logical_stem_mixer.hpp` | opt-in 64-bit logical stem summing helper / opt-in 64-bit logical stem summing helper |
| `fm_common.hpp` | FM音色定義（FmPatch）、周波数変換、voice.datパーサー / FM patch definitions, frequency conversion, voice.dat parser |
| `fm_engine_interface.hpp` | IFmEngine 抽象インターフェース / IFmEngine abstract interface |
| `ymfm_engine.hpp` | optional ymfm OPNA `IFmEngine` 互換アダプタ / optional ymfm OPNA `IFmEngine` adapter |
| `mml_parser.hpp` | MMLパーサー（MUCOM88形式）/ MML parser for the MUCOM88 format |
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
- **テンポ / Tempo**: `T`(BPM)、`t`(Timer-B直接)、`C`(全音符のクロック数、既定128)
- **音色 / Patch**: `@N`(voice.dat/インライン)、`@"name"`(名前検索)
- **ループ / Loop**: `[...]N`、`/`(ブレーク)、`L`(曲全体ループ)
- **マクロ / Macro**: `#*N{...}`定義、`*N`展開
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

単体ビルドではヘッダーオンリーターゲットに加えて、`drumkit_gen` と単体テストをビルドする。
The standalone build creates the header-only target, the `drumkit_gen` tool, and unit tests.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DLIBMUCOM88_BUILD_TOOLS=ON -DLIBMUCOM88_BUILD_TESTS=ON
cmake --build build --parallel 8
ctest --test-dir build --output-on-failure --no-tests=error
```

2026-09-06の上記構成では8/8テストが成功。ADPCM-A、チップ較正、レンダリング、
ポルタメント、stem mix、post-chip処理、回帰指標、WAV入力処理を検証する。
これは任意のMUC全曲互換性や実音・DAW動作の保証ではない。optional ymfmのcompile/linkと
利用側の音声回帰は別途確認する。
The configuration above passed 8/8 tests on 2026-09-06. Optional ymfm integration and host audio compatibility require separate validation.

CMake オプションでツール/テストを個別に無効化できる。
Tools and tests can be disabled independently with CMake options.

```bash
cmake -B build . -DLIBMUCOM88_BUILD_TOOLS=OFF -DLIBMUCOM88_BUILD_TESTS=OFF
```

## drumkit_gen

`drumkit_gen` は、6つの WAV ファイル（BD/SD/CY/HH/TM/RS）から YM2608 ADPCM-A リズム ROM（8192 バイト）を生成する CLI ツール。省略したスロットは無音になり、`-base` を指定すると既存 ROM の未指定スロットを保持して差し替えできる。
`drumkit_gen` is a CLI tool that builds a YM2608 ADPCM-A rhythm ROM (8192 bytes) from up to six WAV files (BD/SD/CY/HH/TM/RS). Omitted slots become silence, or with `-base`, omitted slots keep the existing ROM data.

入力には自分で録音・作成したWAV、または利用許諾のあるWAVを用意する。
Provide WAV files you recorded, created, or are licensed to use.

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

## ライセンス / License

MIT License

## クレジット / Credits

- MML形式 / MML format: [MUCOM88](https://www.ancient.co.jp/~mucom88/) by 古代祐三 / Yuzo Koshiro
- パーサー/シーケンサー / Parser & Sequencer: takamori-tech + Claude (Anthropic)

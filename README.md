# libmucom88

**MUCOM88形式のMMLを、C++アプリケーションで再生するためのライブラリ。**

MMLの解析、演奏イベントの進行、YM2608（OPNA）の制御を提供します。
ゲームや音楽ツールに、FM・SSG・リズム・ADPCM-Bによる楽曲再生を組み込めます。
コアは**ヘッダーオンリーのC++17**で、音源エミュレータとオーディオ出力は利用側で選択します。

*Header-only C++17 library for parsing and sequencing MUCOM88 MML, with YM2608 register control and ADPCM-B voice playback. The core has no external dependencies; audio rendering requires a chip backend.*

[導入](#導入) · [最初の実行](#最初の実行mmlを解析する) · [音声への組み込み](#音声への組み込み) · [API](docs/api_reference.md) · [LICENSE](LICENSE)

## できること

- **MMLの解析と再生** — 音符、休符、テンポ、音色、ループ、マクロ、LFO、ポルタメント、SSGエンベロープなど。
- **OPNAの11トラック制御** — FM 6、SSG 3、リズム1、ADPCM-B 1。
- **BGMとボイス・効果音の併用** — ADPCM-Bボイスの優先再生とダッキング、効果音の再生制御。
- **音色・PCMの利用** — MML内のFM音色定義、`voice.dat`、バックエンドを通じたADPCMデータの読み込み。
- **リズムROMの生成** — 付属CLI `drumkit_gen` で、用意したWAVをADPCM-A形式へ変換。

libmucom88が担当するのは、MMLから音源制御までです。音声デバイスの選択、GUI、
プラグイン形式、ファイル選択などはアプリケーション側で実装します。

```text
MMLテキスト
    │ MmlParser: 解析 → 音色・トラック別イベント
    ▼
MmlEngine: 演奏の進行・レジスタ制御
    │ IFmEngine
    ▼
音源バックエンド → ステレオPCM → アプリケーションの音声出力
```

## 導入

必要なのはC++17対応コンパイラです。付属ツールとテストをCMakeでビルドする場合は、
CMake 3.21以上も使用します。**MMLの解析だけなら音源エミュレータは不要**です。

### CMakeプロジェクトに追加する

利用側のGitリポジトリで実行します。

```bash
git submodule add https://github.com/takamori-tech/libmucom88.git vendor/libmucom88
```

利用側の `CMakeLists.txt` で、既存のアプリケーションターゲットにリンクします。

```cmake
add_subdirectory(vendor/libmucom88)
target_link_libraries(your_app PRIVATE mucom88::mucom88)
```

`mucom88::mucom88` はインクルードパスとC++17要件を伝える `INTERFACE` ターゲットです。
ライブラリ本体のリンク用バイナリは生成しません。CMakeを使わない場合は、
`include/` をコンパイラのインクルードパスへ追加してください。

### 音を生成する場合

`IFmEngine` を実装したバックエンドが必要です。
既存エミュレータをラップするか、付属の `mucom88/ymfm_engine.hpp` にある
`FmEngineYmfm` アダプタを利用します。

ymfmアダプタを使う場合は、**ymfm本体のヘッダと実装も利用側でビルド・リンク**します。
コアのCMakeターゲットはymfmを自動取得・リンクしません。
設定方法は[組み込みガイド](docs/integration_guide.md#ifmengine-の実装)を参照してください。

## 最初の実行：MMLを解析する

リポジトリを取得します。

```bash
git clone https://github.com/takamori-tech/libmucom88.git
cd libmucom88
```

次を `parse_example.cpp` として保存します。ROM、音色ファイル、エミュレータを用意せずに実行できます。

```cpp
#include <iostream>
#include <mucom88/mml_parser.hpp>

int main()
{
    MmlParser parser;
    const auto song = parser.parse(
        "#title First melody\n"
        "D T120 o4 l4 v12 cdefgab>c\n");

    int notes = 0;
    for (const auto& event : song.channelEvents[3]) { // Dトラック = 最初のSSG
        if (event.type == MmlEventType::NOTE_ON)
            ++notes;
    }
    std::cout << song.title << "\nD: " << notes << " notes\n";
}
```

リポジトリのルートでコンパイル・実行します。

```bash
mkdir -p build
c++ -std=c++17 -I include parse_example.cpp -o build/parse_example
./build/parse_example
```

出力:

```text
First melody
D: 8 notes
```

この例ではイベントを確認します。音声を生成するには、解析結果を次のように `MmlEngine` へ渡します。

## 音声への組み込み

次の例の `chip` には、利用側で実装・選択した `IFmEngine` バックエンドを渡します。
`engine` と `chip` は再生中も保持し、`chip` は `engine` より長く生存させてください。

```cpp
#include <cstdint>
#include <mucom88/mml_engine.hpp>

// 音声コールバックの開始前に呼ぶ。
void preparePlayback(MmlEngine& engine, IFmEngine& chip)
{
    MmlParser parser;
    const auto song = parser.parse("D T120 o4 l4 v12 cdefgab>c\n");

    chip.init(44100);
    engine.init(&chip, 44100);
    engine.loadFromParseResult(song);
    engine.setLoop(false);
    engine.play();
}

// outは利用側が確保した、frames * 2要素のint16_tバッファ。
void renderPlayback(MmlEngine& engine, int16_t* out, uint32_t frames) noexcept
{
    engine.renderMixed(out, frames);
}
```

出力は `L, R, L, R, …` の順に並ぶ符号付き16bit PCMです。アプリケーションの音声出力へ渡し、
出力先がfloat形式などを要求する場合は利用側で変換します。実際のデバイスとバックエンド、
シーケンサーのサンプルレートを揃えてください。

`renderMixed()` は演奏の進行とPCM生成をまとめて処理します。
同じ音声ブロックに `advance()` やバックエンドの `generateInterleaved()` を重ねて呼ばないでください。

### 音色・リズム・ボイスを追加する

| 使いたい音 | 準備するもの |
| --- | --- |
| FM | MML内の音色定義、または解析前に `MmlParser::loadVoiceDat()` で音色ファイルを読み込む |
| SSG | 上の例は外部音色なしで設定可能。必要に応じてMMLでエンベロープを指定する |
| ADPCM-Aリズム | 利用可能なリズムROMをバックエンドへ読み込む。下記の生成ツールも利用できる |
| ADPCM-B楽曲パート | 曲が使用するPCMデータと、対応するバックエンドの読み込み処理 |
| ADPCM-Bボイス | ボイステーブルとボイス機能を実装したバックエンド。`IFmEngine` のボイス機能は任意実装 |

読み込みAPIの戻り値を確認し、ファイルI/O・初期化・解析・イベント設定は音声コールバック外で行います。
ボイス優先制御、ダッキング、効果音、複数チップの設定は[組み込みガイド](docs/integration_guide.md)を参照してください。

## MMLの基本

行頭でトラックを指定し、その後に演奏内容を書きます。

| トラック | 音源 |
| --- | --- |
| A–C | FM 1–3 |
| D–F | SSG 1–3 |
| G | ADPCM-Aリズム |
| H–J | FM 4–6 |
| K | ADPCM-B |

例の `D T120 o4 l4 v12 cdefgab>c` は、Dトラックへテンポ・オクターブ・音長・音量を設定し、音階を並べています。

| 記法 | 意味 |
| --- | --- |
| `cdefgab` / `r` | 音符 / 休符 |
| `o4` / `>` / `<` | オクターブ / 上げる / 下げる |
| `l8` / `c4` / `c.` | 既定の音長 / 音符ごとの音長 / 付点 |
| `T120` / `t200` | BPM指定 / Timer-Bの直接値。大文字と小文字で意味が異なる |
| `C128` | 全音符のクロック数。既定値は128 |
| `@0` / `v12` | 音色番号 / 音量。音源によって音量の解釈が異なる |
| `[cdef]2` / `L` | 範囲の繰り返し / 曲全体のループ位置 |
| `#*1{cdef}` / `*1` | マクロ定義 / 呼び出し |

これは記法の入口です。コマンドの細かな挙動は[パーサー](include/mucom88/mml_parser.hpp)と
[シーケンサー](include/mucom88/mml_engine.hpp)の実装を参照してください。

## 組み込み時の注意点

- **互換性:** MUCOM88形式を扱うネイティブ実装で、オリジナルのZ80コンパイラ・ドライバーを実行する方式ではありません。
  すべてのMUCファイルやバックエンドで同一の再生結果を保証するものではありません。
- **入力の検証:** `parse()` は解析結果を返しますが、構文エラー一覧や成功フラグを返すAPIはありません。
  未知の指示の読み飛ばしや数値・展開量の扱いに制約があるため、利用側でも入力を制限してください。
- **スレッドと実時間性:** 再生中の状態変更は利用側で直列化します。`noexcept` はスレッド安全性や待機しないことの保証ではありません。
  付属ymfmアダプタは内部でmutexを使用するため、音声コールバックへの適合性はバックエンドを含めて確認してください。
- **追加ヘルパー:** logical stem mixとpost-chip処理は明示的に組み込む機能です。通常の再生経路へ自動的に追加されません。
  埋め込み回帰データ `test_muc_data.hpp` も、通常のコア利用には不要です。

## ビルドとテスト

リポジトリのルートで実行します。

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DLIBMUCOM88_BUILD_TOOLS=ON -DLIBMUCOM88_BUILD_TESTS=ON
cmake --build build --parallel 8
ctest --test-dir build --output-on-failure --no-tests=error
```

この構成では `drumkit_gen` と単体テストをビルドします。ツール・テストはそれぞれ
`LIBMUCOM88_BUILD_TOOLS` / `LIBMUCOM88_BUILD_TESTS` を `OFF` にして無効化できます。

単体テストはADPCM-A、音源較正、ミキシング、ポルタメント、回帰指標、WAV読み込みなどを検証します。
実際の音源バックエンド、楽曲、音声デバイスを組み合わせた確認は、利用側でも行ってください。

### WAVからリズムROMを作る

利用できるWAVファイルを用意して実行します。

```bash
./build/drumkit_gen -o my_drums.bin \
  -bd BD.wav -sd SD.wav -cy CY.wav \
  -hh HH.wav -tm TM.wav -rs RS.wav
```

生成物は8192バイトのADPCM-AリズムROMです。省略した楽器は無音になります。
`-base existing.bin` を指定すると、既存ROMの未指定スロットを保持して差し替えできます。

## 詳しい資料

| 目的 | 資料 |
| --- | --- |
| バックエンド・BGM・ボイス・効果音を組み込む | [組み込みガイド](docs/integration_guide.md) |
| 型とメソッドを調べる | [APIリファレンス](docs/api_reference.md) |
| パート別出力を加算する | [Logical Stem Mixing](docs/logical_stem_mixing.md) |
| ヘッダーと実装を読む | [include/mucom88](include/mucom88/) |
| 動作の検証例を読む | [tests](tests/) |
| 修正を提案する | [Issues](https://github.com/takamori-tech/libmucom88/issues) |

不具合の報告には、再現する最小のMML、使用バックエンド、サンプルレート、利用commit、
期待する結果と実際の結果を添えてください。再配布できない音色・PCM・楽曲は添付しないでください。

## ライセンスとクレジット

ライブラリのライセンスは [MIT](LICENSE) です。
エミュレータや音色・PCM・楽曲を組み合わせる場合は、それぞれのライセンス・利用条件も確認してください。

- MML形式: [MUCOM88](https://www.ancient.co.jp/~mucom88/) — 古代祐三 / Yuzo Koshiro
- パーサー・シーケンサー: takamori-tech（開発支援: Claude / Anthropic）

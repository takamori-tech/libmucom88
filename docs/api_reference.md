# libmucom88 APIリファレンス

公開ヘッダーの主要な型と、利用時に必要な契約をまとめます。
導入と実行例は [組み込みガイド](integration_guide.md)、旧APIの置き換えは
[APIの移行と互換性](api_cleanup.md) を参照してください。

## ヘッダーと名前空間

| ヘッダー | 主な提供内容 |
| --- | --- |
| [fm_common.hpp](../include/mucom88/fm_common.hpp) | `FmPatch`、音色データ解析、音高変換 |
| [mml_parser.hpp](../include/mucom88/mml_parser.hpp) | `MmlParser`、`MmlEvent`、`MmlEventType` |
| [mml_engine.hpp](../include/mucom88/mml_engine.hpp) | `MmlEngine`、BGM・SE・ボイス優先制御 |
| [fm_engine_interface.hpp](../include/mucom88/fm_engine_interface.hpp) | `IFmEngine` |
| [ymfm_engine.hpp](../include/mucom88/ymfm_engine.hpp) | 任意依存の `FmEngineYmfm` |
| [chip_backend_interface.hpp](../include/mucom88/chip_backend_interface.hpp) | `IChipBackend`、`ChipMode`、`ChipStemFrame` |
| [chip_calibration.hpp](../include/mucom88/chip_calibration.hpp) | エンジン別の較正値・レジスタ補正 |
| [chip_output_tuning.hpp](../include/mucom88/chip_output_tuning.hpp) | 出力プリセット・16bitリミッター |
| [normalizing_chip_backend.hpp](../include/mucom88/normalizing_chip_backend.hpp) | `IChipBackend` の較正ラッパー |
| [logical_stem_mixer.hpp](../include/mucom88/logical_stem_mixer.hpp) | パート別出力のdouble加算・float変換 |
| [post_chip_processor.hpp](../include/mucom88/post_chip_processor.hpp) | 明示的に接続する後段音声処理 |
| [adpcm_a_encode.hpp](../include/mucom88/adpcm_a_encode.hpp) / [adpcm_a_decode.hpp](../include/mucom88/adpcm_a_decode.hpp) | ADPCM-A変換 |
| [regression_metrics.hpp](../include/mucom88/regression_metrics.hpp) | PCM比較の指標 |

`MmlParser`、`MmlEngine`、`IFmEngine` などはグローバル名前空間にあります。
回帰指標は `mucom88` 名前空間にあります。CMakeの `mucom88::mucom88` はターゲット名です。
埋め込み楽曲・音色・PCM等を持つ `test_muc_data.hpp` は、通常のコア利用には不要です。

## 共通の呼び出し契約

- 初期化・解析・ファイルI/O・イベント設定は音声処理を停止した状態で行います。
  コンテナや文字列を使用するため、確保や解放が発生します。
- `MmlEngine` とそのバックエンドの操作は利用側で直列化します。状態取得も同じ契約です。
- `noexcept` は例外が呼び出し元へ伝播しない指定であり、ロックや確保がないという保証ではありません。
  付属ymfmアダプタにはmutexと、ボイス用RAMの再代入経路があります。
- バッファの容量、有効なポインタ、正のサンプルレート・クロック、有限の音量値を利用側で保証します。
  エラー戻り値はメソッドごとに意味が異なります。

## MmlParser

| 公開API | 動作 |
| --- | --- |
| `MucFile parse(const std::string& text)` | テキストからメタデータ、音色、11トラックのイベント列を生成 |
| `bool loadVoiceDat(const std::string& path)` | 解析時に参照する外部音色データをロード |
| `bool loadVoiceDatFromMemory(const uint8_t* data, size_t size)` | メモリから音色データをコピー |
| `bool hasVoiceDat() const` | 音色データを保持しているか |
| `static int trackCharToChannel(char c)` | A–Kを0–10へ変換。小文字も受理し、範囲外は−1 |

`parse()` は成功フラグや構文エラー一覧を返しません。未知の文字を読み飛ばす場合もあり、
空でない結果を得たことは完全に解釈できた証明にはなりません。
音色ローダーは最低32バイトを受理しますが、ファイル全体の厳密な形式検証ではありません。
`#voice` / `#pcm` のファイル名はメタデータです。自動的にファイルを開く機能ではありません。

### MucFile

型名は `MmlParser::MucFile` です。

| メンバー | 型・意味 |
| --- | --- |
| `title`, `composer`, `author`, `date`, `comment` | `std::string` のメタデータ |
| `voiceFile`, `pcmFile`, `driver` | `std::string` の指定値 |
| `chipMode` | `MmlParser::ChipMode`。既定は `OPNA` |
| `chipModeExplicit` | `#mode` の明示指定を表す `bool` |
| `wholeTick` | 全音符のtick数。既定128 |
| `channelEvents` | `std::array<std::vector<MmlEvent>, 11>` |
| `patches` | `std::unordered_map<int, Mucom88Patch>` |

`MmlParser::ChipMode::{OPNA, OPM, OPNB, OPN}` とグローバルの `ChipMode` は別のenumです。
必要な変換は列挙子ごとに行い、整数キャストへ依存しないでください。
`MmlEngine::loadFromParseResult()` は `chipMode` を使って音源を切り替えません。

定数は `PPQ=32`、`WHOLE_TICK=128`、`CLOCK_TICK=1`。
`MAX_NOTE_TICKS=65535`、`MAX_TIE_BOUNDARIES=1024` は一部の音長・タイ展開の上限であり、
入力ファイル全体の時間・メモリ上限ではありません。

### MmlEvent

`type` によって各フィールドの意味が変わります。

| フィールド | 型・用途 |
| --- | --- |
| `type` | `MmlEventType`。既定 `END` |
| `tick`, `duration` | `uint32_t`。発生位置と音長 |
| `note`, `velocity`, `value`, `channel` | `int`。ノート・パラメータ・トラック番号 |
| `vibDelay`, `vibRate`, `vibDepth`, `vibCount` | `int`。LFOなどのパラメータ |
| `envAL`, `envAR`, `envDR`, `envSL`, `envSR`, `envRR` | `int`。SSGエンベロープ |

イベント種別は次のとおりです。

| 分類 | `MmlEventType` |
| --- | --- |
| 音符・進行 | `NOTE_ON`, `NOTE_OFF`, `REST`, `TEMPO`, `LOOP_POINT`, `TIE_KEYOFF`, `END` |
| 音色・音量・音高 | `PATCH`, `VOLUME`, `PAN`, `STACCATO`, `DETUNE`, `KEY_TRANSPOSE`, `RHYTHM_LEVEL` |
| 変調 | `VIBRATO`, `VIBRATO_SWITCH`, `LFO_PARAM`, `PORTAMENTO`, `HARDWARE_LFO`, `CSM_MODE` |
| エンベロープ | `SSG_ENVELOPE`, `REVERB_ENVELOPE`, `REVERB_SWITCH`, `REVERB_MODE` |
| レジスタ | `REG_WRITE` |

`TEMPO.value` は**Timer-Bの値**です。`T120` のBPM指定も解析時にTimer-B値へ変換されます。
`PAN.value` はFM/ADPCM-Bでは0=無効、1=右、2=左、3=左右です。
リズムはパンと楽器番号を含む値を使い、SSGはモノラルです。
エコーは複数の通常イベントへ展開され、`ECHO` というenumはありません。
直接イベントを作る場合は、各種別を処理する `MmlEngine::processEvents()` のフィールド解釈も確認してください。

## FmPatch と音色ヘルパー

`Mucom88Patch` は `FmPatch` のエイリアスです。

| メンバー | 意味 |
| --- | --- |
| `patchNo`, `fb`, `al` | 音色番号、Feedback、Algorithm |
| `op[4]` | `ar`, `dr`, `sr`, `rr`, `sl`, `tl`, `ks`, `ml`, `dt`, `dt2`, `ame`, `ssgEg` |
| `isOpm`, `pms`, `ams` | OPM拡張用の情報 |
| `name` | 音色名。`std::string` |
| `source`, `valid` | `PatchSource` と有効フラグ |

`PatchSource` は `Unknown`, `VoiceDat`, `Inline`, `Edited`, `UserBank`, `External`。
型にOPM等の情報があることと、`MmlEngine` / 付属アダプタでそのチップを再生できることは別です。

| 関数 | 動作 |
| --- | --- |
| `parseVoiceDatEntry(data, dataSize, patchNo)` | 32バイト単位の音色を解析して `FmPatch` を返す。`valid` を確認 |
| `noteToFnum(noteNum, blockOut, chipClock=7987200)` | OPN系F-Numberを返し、Blockを出力引数へ設定 |
| `noteToSSGPeriod(noteNum, chipClock=7987200)` | SSGの周期値を返す |

ポインタは実際のデータを指している必要があります。音高ヘルパーは任意チップ・任意クロックへの
汎用チューニング機能ではありません。特にFMのクロック補正は低クロック時のBlock補正です。

## IFmEngine

`MmlEngine` が呼び出す音源インターフェースです。仮想デストラクタを持ちます。
必須メソッドは以下です。継承時は `noexcept` も一致させます。

```cpp
#include <mucom88/fm_engine_interface.hpp>

class MyFmEngine : public IFmEngine {
public:
    void init(uint32_t sampleRate) override;
    void writeReg(int port, uint8_t addr, uint8_t data) noexcept override;
    void generateInterleaved(int16_t* out, uint32_t frames) noexcept override;
    void reset() noexcept override;
    bool loadAdpcmRom(const std::string& path) override;
    bool loadAdpcmRomFromMemory(const uint8_t* data, size_t size) override;
    bool hasAdpcmRom() const noexcept override;
};
```

これは宣言例です。各メソッドの定義には実際のエミュレータを接続します。
`generateInterleaved()` は `frames * 2` 要素へ符号付き16bitのL/R PCMを出力します。
OPNAではport 0にFM1–3・SSG・リズム、port 1にFM4–6・ADPCM-Bのレジスタがあります。

| 任意メソッド | 既定実装 |
| --- | --- |
| `chipEngine() const noexcept` | `ChipEngine::Fmgen` |
| `loadVoiceTable(path)`, `loadVoiceTableFromMemory(data, size)`, `hasVoiceTable() const noexcept` | `false` |
| `playVoice(id, level=255) noexcept`, `stopVoice() noexcept`, `tickVoiceTimer(frames) noexcept`, `stopAdpcmB() noexcept` | 何もしない |
| `isVoicePlaying() const noexcept` | `false` |
| `loadPcmDataToAdpcmB(data, size)` | `false` |
| `setSsgMixScale(scale) noexcept`, `getSsgMixScale() const noexcept` | 設定は何もしない、取得は1.0 |
| `setCompatibilityOutput(enabled) noexcept`, `compatibilityOutputEnabled() const noexcept` | 設定は何もしない、取得は `false` |
| `applyPatch(fmIndex, patch) noexcept`, `setFrequency(fmIndex, note) noexcept`, `fmKeyOn(fmIndex) noexcept`, `fmKeyOff(fmIndex) noexcept` | `writeReg()` を使うOPNA向けの実装 |

任意メソッドがコンパイルできることは機能の実装を意味しません。ボイスを使う前に機能を確認してください。

## MmlEngine

### 初期化と楽曲設定

| メソッド | 契約 |
| --- | --- |
| `init(IFmEngine*, uint32_t sampleRate, uint32_t chipClock=7987200)` | 初期化済みバックエンドを参照し、音色・チャンネルを初期化。バックエンド自身の `init()` は呼ばない |
| `loadFromParseResult(const MmlParser::MucFile&)` | 音色を登録、全音符tickと全トラックを設定、`L` の有無からループを設定 |
| `setEvents(int ch, const std::vector<MmlEvent>&)` / `setEvents(int ch, std::vector<MmlEvent>&&)` | イベント列のコピー / ムーブ。chは0–10 |
| `setPatch(int patchNo, const FmPatch&)` | 音色を登録・上書き |
| `setWholeTick(int)` | 全音符tick数。0以下は128 |
| `setLoop(bool)` | 曲のループを指定。一回再生なら楽曲設定後に `false` |
| `setCommonEndTick(uint32_t)` | 比較用に共通ループ終端を上書き。0は上書きなし |
| `setChipClock(uint32_t) noexcept` | 以降の音高換算に使うクロックを更新。バックエンドのクロックやTimer-B周期は変更しない |

バックエンドの所有権は移りません。参照中はバックエンドを生存させてください。
`MmlEngine` はコピーできません。曲の差し替えはレンダリングを止めて行います。
`loadFromParseResult()` は古いイベントを消しますが、音色マップ全体は消去せず新しい音色を追加・上書きします。
ファイル名メタデータやPCMは自動ロードしません。

### 再生と状態

| メソッド | 動作 |
| --- | --- |
| `play() noexcept` | 先頭から開始。ランタイム状態・フェード・ダッキング状態を初期化し、全SEを停止 |
| `stop() noexcept` | 進行停止、消音、SE停止。フェード・ダッキング減衰をリセット |
| `pause() noexcept` / `resume() noexcept` | 位置を保持して消音 / 状態を復元して進行再開。波形のサンプル単位の保存・復元ではない |
| `advance(uint32_t frames) noexcept` | 演奏時刻とレジスタ制御を進める。PCM生成・ボイスタイマー・SE時間更新は行わない |
| `renderMixed(int16_t* out, uint32_t frames) noexcept` | 最大16フレームずつ進行・ボイスタイマー・SE・PCM生成・ミキシング |
| `isPlaying() const` | シーケンサーの進行状態。出力全体の無音判定ではない |
| `globalTick()`, `commonEndTick()`, `loopTickOffset()` | `uint32_t` のtick情報（いずれもconst） |
| `globalTempo() const` | 名前に注意。返す `int` はBPMではなくTimer-B値 |
| `loopCount() const` | 共通ループの回数 |

チャンネルの状態取得は `chNoteOn(ch)`, `chNote(ch)`, `chVolume(ch)`, `chPan(ch)`,
`chReverb(ch)`, `chNoteOnCount(ch)`。FM音色番号は `fmPatchNo(fi)` で取得します。
いずれもconstですが、並行読み取りを保証しません。
静的ヘルパー `isFM`, `isSSG`, `isRhythm`, `isADPCMB`, `toFMIndex`, `toSSGIndex` は、
呼び出し側で有効なトラック番号と対応音源を確認して使います。

### 音量・出力・フェード

| メソッド | 値・対象 |
| --- | --- |
| `setMasterVolume(float)` / `getMasterVolume() const` | 全体のレジスタ減衰。設定は0–1へclamp |
| `setBgmVolume(float)` / `getBgmVolume() const` | BGMの減衰 |
| `setSeVolume(float)` / `getSeVolume() const` | SEの減衰 |
| `setVoiceVolume(float)` / `getVoiceVolume() const` | 次回ボイス開始時の減衰 |
| `setSsgMixScale(float)` / `getSsgMixScale() const` | BGM・SEバックエンドへSSG倍率を渡す |
| `setOutputGain(float)` / `getOutputGain() const` | `renderMixed()` でBGMチップPCMに掛ける倍率 |
| `setOutputProfile(ChipOutputProfile)` / `outputProfile() const noexcept` | 出力プリセットを適用 / 取得。初期値 `Tuned` |
| `setDucking(int attTarget, float releaseSec=0.15f)` | ボイス時のBGM減衰量と復帰時間。att=0で無効 |
| `fadeOut(float seconds, FadeAction=FadeAction::None)` | 現在のフェード量から無音へ |
| `fadeIn(float seconds)` | 現在のフェード量から復帰 |
| `resetFade()` | フェードを解除 |
| `isFading() const`, `isFadeOutDone() const` | フェード進行 / 完了アクションの実行状態 |

音量の0–1はレジスタ減衰へ変換される設定値で、線形PCM振幅の割合ではありません。
音量設定と出力ゲインは `play()` / `stop()` で保持されます。
ダッキングの設定値も保持されますが、進行中の状態・減衰はリセットされます。
`FadeAction` は `None`, `Stop`, `StopAndReset`。`StopAndReset` はRichのSEチップもリセットします。
`isFadeOutDone()` は停止アクションの実行で立ち、単なる `None` のフェード終了検出には使えません。

### PCMとボイス

| メソッド | 戻り値・役割 |
| --- | --- |
| `bool loadPcmData(const uint8_t*, size_t)` | 0x400バイトのPCMテーブルを解析。バックエンド接続が必要。音声データはロードしない |
| `bool loadPcmBinary(const uint8_t*, size_t)` | テーブル解析とバックエンドへのADPCM-Bロードを試行 |
| `bool loadPcmBinaryFile(const std::string&)` | ファイルを読み、上の統合ロードを呼ぶ |
| `bool loadVoiceTable(path)`, `bool loadVoiceTableFromMemory(data, size)`, `bool hasVoiceTable() const` | バックエンドへの委譲 |
| `playVoice(int id) noexcept` | Kトラックを抑制してボイス開始。ボイス・マスター減衰からlevelを計算 |
| `stopVoice() noexcept`, `stopAdpcmB() noexcept` | バックエンドへ停止指示、優先制御とダッキング解除 |
| `bool isVoicePlaying() const` | バックエンドのボイス再生状態 |
| `tickVoiceTimer(uint32_t frames) noexcept` | バックエンドの残り時間とダッキング復帰を進める |

`loadPcmBinary()` は非nullかつsize>0x400なら `true` を返し、内部ローダーの失敗を伝播しません。
バックエンドのPCMロード成功を判定したい場合は個別のメソッドを呼びます。
`loadPcmBinaryFile()` も完全なファイル検証・再生保証には使えません。
ボイス・復帰中はKトラックのイベント処理が抑制されます。詳細は [ボイスの組み込み](integration_guide.md#ボイスとダッキング) を参照。

### 効果音

`MmlEngine::SeMode` は既定 `Classic`（BGMのFMを一時使用）、`Rich`（専用チップ）です。

| メソッド | 動作 |
| --- | --- |
| `setSeMode(SeMode, IFmEngine* seEngine=nullptr)` / `seMode() const` | モード設定 / 取得。切り替え時に全SE停止。Richには初期化済みの別バックエンドを渡す |
| `int playSe(const FmPatch&, int note, int velocity=15, int durationMs=0) noexcept` | SE開始。0–5のスロット、失敗は−1。duration=0は手動停止 |
| `int playSeSequence(const FmPatch&, const SeSequenceNote*, int count, int velocity=15) noexcept` | 最大8ノートのシーケンスをコピーして再生 |
| `playSeOnSlot(slot, patch, note, velocity=15, durationMs=0)` | 指定スロットで開始。戻り値はスロットまたは−1 |
| `playSeSequenceOnSlot(slot, patch, notes, count, velocity=15)` | 指定スロットでシーケンス開始 |
| `stopSe(int) noexcept`, `stopAllSe() noexcept`, `setSeFrequency(slot, note) noexcept` | 停止・音高変更 |
| `isSeActive(int) const`, `activeSeCount() const` | スロット使用状態 |
| `hijackChannel(int)`, `releaseChannel(int)`, `isChannelHijacked(int) const` | FM/SSGトラックの手動占有・解放・確認 |

`SeSequenceNote` は `startNote=60`, `endNote=-1`, `durationMs=100` が既定です。
endNote<0はスイープなし。スイープは整数ノートへ丸め、通常は `renderMixed()` の最大16フレーム単位で更新します。
SEの上限は6スロットで、満杯時の割り当てでは既存SEが置き換わり得ます。
`play()` / `stop()` は全SEを止めます。

## 出力プリセット

`chipOutputTuningFor(engine, profile)`、`defaultChipOutputTuningFor(engine)` が設定を返します。
`effectiveSsgMixScaleFor()` はSSGの較正係数も含めた倍率を返します。

| プロファイル | SSG調整値 | MmlEngineのoutputGain | バックエンド互換出力 |
| --- | --- | --- | --- |
| Native | 1.0 | 1.0 | 無効 |
| Tuned / Fmgen | 0.70794576（−3dB） | 1.0 | 無効 |
| Tuned / Ymfm | 0.63095737（−4dB、別途較正） | 1.33352143（+2.5dB） | 対応実装で1.9倍＋リミッター |

NativeにしてもバックエンドのDAC設定などを一括無効化するわけではありません。
RichではBGM×outputGainとSE等倍を加算して `softLimit16()` を適用します。
Classicではgain≠1のときに適用し、gain=1ならバックエンドPCMをコピーします。

## 拡張ヘルパー

`IChipBackend` は `IFmEngine` と異なる抽象です。前者はint32 PCM、チャンネルマスク、
任意のstem出力などを扱います。後者に自動接続されません。
`mixChunk()` の出力バッファはゼロ初期化して渡します。`mixStemChunk()` の既定値は `false`。
`ChannelMaskSpec` はtrue=可聴、既定は全可聴です。
`chipModeProfile()` の定義値はOPNA=7,987,200Hz/FM6/SSG3、OPM=7,987,200Hz/FM8、
OPNB=8,000,000Hz/FM4/SSG3、OPN=3,993,600Hz/FM3/SSG3です。
これはプロファイルの定義であり、全チップの再生実装・物理クロックの推奨値を保証しません。

`NormalizingChipBackend` は `std::unique_ptr<IChipBackend>` を所有し、較正を適用して委譲します。
`createNormalizingChipBackend()` はnull入力にnullを返します。
個別バックエンドへの追加機能の委譲範囲も、利用前にヘッダーで確認してください。

`LogicalStemAccumulator` と `writeLogicalStemFloatFrame()` は [専用ガイド](logical_stem_mixing.md) を参照。

`PostChipProcessor` は `prepare(sampleRate)` 後に `setConfig()` で設定し、
`processBlock(float* left, float* right, int samples) noexcept` または `processSample(float) noexcept` で使います。
ステレオは左右別バッファです。既定はDAC・基板LPF・飽和がON、ノイズ・キャビネットがOFF。
`PostChipConfig::antiAlias` は既存名ですが基板LPFを指し、汎用のアンチエイリアス保証ではありません。
通常の `renderMixed()` には接続されていません。

ADPCM-Aは `encodeAdpcmANibbles(const std::vector<int16_t>&)` がバイト列を返し、
`decodeAdpcmANibbles(src, numBytes, pcmOut) noexcept` は2×numBytes個のPCM要素へ復号します。
エンコードはメモリ確保を伴います。

`mucom88::calcInterleavedStereoRms()`、`firstSustainedSoundFrame()`、`averageAlignedRmsRatio()`、
`summarizeRmsRatios()` は回帰比較用です。音量の近さは波形や聴感の一致を証明しません。

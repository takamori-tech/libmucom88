# libmucom88 API リファレンス

## FmPatch（fm_common.hpp）

MUCOM88形式の FM音色定義。

```cpp
struct FmPatch {
    int patchNo;           // @番号
    int fb;                // Feedback (0-7)
    int al;                // Algorithm (0-7)
    struct Op {
        int ar, dr, sr, rr;    // ADSR レート
        int sl, tl;             // サステインレベル、トータルレベル
        int ks, ml, dt;         // キースケール、マルチプル、デチューン
        int dt2, ame, ssgEg;   // OPM拡張、AM Enable、SSG-EG
    } op[4];
    bool isOpm;            // OPM音色フラグ
    int pms, ams;          // OPM LFO感度
    std::string name;      // 音色名（6文字）
    PatchSource source;    // 出典（VoiceDat/Inline/Edited/UserBank/External）
    bool valid;
};
```

### voice.dat パース

```cpp
// 32バイト/エントリのMUCOM88 voice.datをパース
FmPatch parseVoiceDatEntry(const uint8_t* data, size_t dataSize, int patchNo);
```

### 周波数変換

```cpp
// MIDIノート → YM2608 F-Number + Block
uint16_t noteToFnum(int noteNum, int& blockOut);

// MIDIノート → SSG トーンピリオド
uint16_t noteToSSGPeriod(int noteNum, uint32_t chipClock = 7987200);
```

### 定数

```cpp
// オペレータスロットオフセット（op1=+0, op2=+8, op3=+4, op4=+12）
constexpr int kFmSlotOffset[4] = { 0, 8, 4, 12 };

// アルゴリズムごとのキャリアオペレータ判定
constexpr bool kFmCarrier[8][4] = { /* AL0-7 */ };
```

---

## ChipMode

```cpp
enum class ChipMode { OPNA, OPM, OPNB, OPN };
```

トップレベルの正準 `ChipMode` は `include/mucom88/chip_backend_interface.hpp` で定義。
MUCファイルの `#mode` ディレクティブで指定。
現時点では `OPNA` が標準。OPM/OPNB は将来のG2モード用。

`MmlParser::ChipMode`（`mml_parser.hpp` のネスト型）は、`#mode` ディレクティブの解析結果として parser が内部で使う同値だが別の型。

---

## ChannelMaskSpec

論理チャンネル可聴指定 POD。audible=1 セマンティクス。
チップ固有の mute polarity / bit 配置への変換は利用側 backend が行い、本型はチップ非依存。
デフォルト構築および `ChannelMaskSpec::allAudible()` は全チャンネル可聴。

---

## PatchSource

```cpp
enum class PatchSource : uint8_t {
    Unknown  = 0,
    VoiceDat = 1,   // voice.dat から読み込み
    Inline   = 2,   // MUCインライン定義 @N={...}
    Edited   = 3,   // Voice Editorで編集
    UserBank = 4,   // ユーザー保存バンク
    External = 5,   // MUC companion voice.dat
};
```

---

## IFmEngine（fm_engine_interface.hpp）

YM2608エミュレータの抽象インターフェース。ゲーム側で実装する。

### メソッド

| メソッド | 説明 |
|---------|------|
| `init(sampleRate)` | 初期化。出力サンプルレート指定 |
| `writeReg(port, addr, data)` | YM2608レジスタ書き込み。port 0/1 |
| `generateInterleaved(buf, frameCount)` | ステレオPCM生成（int16 L,R,L,R...） |
| `reset()` | チップリセット |
| `loadAdpcmRom(path)` | ADPCM-A ROM読み込み（リズム音源） |
| `loadAdpcmRomFromMemory(data, size)` | ADPCM-A ROMメモリ読み込み |
| `hasAdpcmRom()` | ADPCM-A ROMがロード済みか |
| `loadPcmDataToAdpcmB(data, size)` | ADPCM-B PCMデータロード（mucompcm.binのデータ部分）。デフォルト実装はfalse返却 |
| `loadVoiceTable(path)` | ボイステーブル読み込み |
| `loadVoiceTableFromMemory(data, size)` | ボイステーブルメモリ読み込み |
| `hasVoiceTable()` | ボイステーブルがロード済みか |
| `playVoice(voiceId)` | ボイス再生開始（ADPCM-B使用） |
| `stopVoice()` | ボイス停止 |
| `isVoicePlaying()` | ボイス再生中か |
| `tickVoiceTimer(frameCount)` | ボイス再生タイマー更新 |
| `stopAdpcmB()` | ADPCM-B強制停止（BGM + ボイス両方） |
| `applyPatch(fmIndex, patch)` | FM音色パッチ適用（STENV互換）。KEY_OFF→SL/RR最速→全OPパラメータ→FB/ALG→PAN。デフォルト実装はwriteReg()ベース |
| `setFrequency(fmIndex, noteNum)` | FM周波数設定。MIDIノート→F-Number/Block計算→0xA4/0xA0ラッチ。デフォルト実装はwriteReg()ベース |
| `fmKeyOn(fmIndex)` | FM KEY ON（全スロット）。デフォルト実装はwriteReg()ベース |
| `fmKeyOff(fmIndex)` | FM KEY OFF。デフォルト実装はwriteReg()ベース |
| `chipEngine()` | engine識別子を返す。既定は `ChipEngine::Fmgen`。共有出力プリセット選択に使用 |
| `setSsgMixScale(ssgScale)` | SSGミックスレベル設定（1.0=等倍、0.71≈-3dB）。デフォルト実装は何もしない |
| `getSsgMixScale()` | 現在のSSGスケール値（デフォルト1.0） |
| `setCompatibilityOutput(enabled)` | backend固有の互換出力段を有効/無効化。デフォルト実装は何もしない |
| `compatibilityOutputEnabled()` | 互換出力段の状態を返す。デフォルトfalse |

---

## FmEngineYmfm（ymfm_engine.hpp）

optional ymfm OPNA `IFmEngine` 互換アダプタ。`MmlEngine` からはfmgen等の既存 `IFmEngine` 実装と同じAPIで扱える。
このヘッダをincludeする利用側は、ymfmのヘッダとリンクを追加する必要がある。

### セットアップ

```cpp
#include <mucom88/ymfm_engine.hpp>

FmEngineYmfm fmEngine;
fmEngine.setDacModel(true);
fmEngine.setFidelity(FmEngineYmfm::FIDELITY_HIGH);  // 1 = high/MAX
fmEngine.init(44100);

MmlEngine engine;
engine.init(&fmEngine, 44100, FmEngineYmfm::CHIP_CLOCK);
```

### 定数

| 定数 | 説明 |
|------|------|
| `CHIP_MODE` | `ChipMode::OPNA` |
| `CHIP_ENGINE` | `ChipEngine::Ymfm` |
| `CHIP_CLOCK` | OPNA既定クロック `7987200` |
| `FIDELITY_MED` | libmucom88 fidelity値 `0`。ymfm `OPN_FIDELITY_MED` |
| `FIDELITY_HIGH` | libmucom88 fidelity値 `1`。ymfm `OPN_FIDELITY_MAX` |

### メソッド

| メソッド | 説明 |
|---------|------|
| `setDacModel(enabled)` | 後段YM3016 DACモデルを有効/無効化。既定はtrue |
| `setFidelity(fidelity)` | `0=MED`, `1=HIGH/MAX`。既定はHIGH。native rateが変わるため `init()` 前に設定する |
| `setCompatibilityOutput(enabled)` | Tuned互換出力段を有効/無効化。ymfmではbackend内で `1.9x` gain とsoft limiterを適用する |
| `compatibilityOutputEnabled()` | 互換出力段の状態を返す |
| `dacModelEnabled()` | DACモデル設定を返す |
| `fidelity()` | 現在のfidelity値を返す |

`FmEngineYmfm` は `loadAdpcmRom*`、`loadPcmDataToAdpcmB`、`loadVoiceTable*`、`playVoice`、`stopAdpcmB` も実装し、OPNAのADPCM-A/Bをymfmの外部メモリ読み出し経路で扱う。

---

## IChipBackend（chip_backend_interface.hpp）

単一チップバックエンドの抽象。具体実装（fmgen / ymfm 等）は利用側が提供する。

### オペレータ向けパラメータ（非純粋仮想、no-op 既定）

以下は利用者（オペレータ）が選択するチップ固有パラメータ。**いずれも非純粋仮想で no-op 既定実装を持つ**ため、fmgen やその他のバックエンドは override 不要で影響を受けない（vtable 末尾追加＝後方互換）。内部読出方式バックエンド（ymfm 等）のみ override する。

| メソッド | 説明 |
|---------|------|
| `setDacModel(bool enabled)` | 後段 YM3016 DAC モデル（companding 再量子化）の有効/無効。内部読出 backend（ymfm）のみ override。decoded 経路（fmgen 等）は no-op 既定を継承＝無改変。設定は audio パス外で行う |
| `setFidelity(int fidelity)` | ymfm リサンプリング忠実度（0=MED / 1=MAX 既定）。CPU 制約のある利用者（例: Raspberry Pi 4B 上のゲーム）が MED を選べる。**忠実度変更は native rate を変えるため backend 再init が前提**。fmgen 等は no-op 継承＝無変更 |
| `setCompatibilityOutput(bool enabled)` | Nativeでは無効、Tunedでは既存のfmgen/OpenMUCOM88向け補正を有効化する互換出力段。内部mix backend（ymfm等）のみoutput gain/limiterを切り替える |

---

## LogicalStemMixer（logical_stem_mixer.hpp）

`IChipBackend::mixStemChunk()` が返す `ChipStemFrame` を、FM/SSG/Rhythm/ADPCM-B の
論理 stem 単位で 64-bit double accumulation する opt-in helper。
既定では未使用で、consumer が明示的に include/call した場合だけ有効になる。

| 型 / 関数 | 説明 |
|----------|------|
| `LogicalStemMixOptions` | `enableDoubleStemSumming`、`outputScale`、`masterGain` を持つ。既定では `enableDoubleStemSumming=false` |
| `LogicalStemAccumulator` | `addStem()` / `addFallbackStereo()` で double accumulator に加算し、`left()` / `right()` で main sum を返す |
| `LogicalStemFloatFrame` | main、FM1-6、SSG1-3、Rhythm、ADPCM-B、fallback の final float frame |
| `writeLogicalStemFloatFrame(acc, options, out)` | `options.enableDoubleStemSumming` が true の時だけ `out` を書き、true を返す。false なら何もせず false |

詳細: [Logical Stem Mixing](logical_stem_mixing.md)

---

## ChipOutputTuning（chip_output_tuning.hpp）

engine別の出力プリセット定義。`MmlEngine` は既定で `ChipOutputProfile::Tuned` を適用する。

| 項目 | 説明 |
|------|------|
| `ChipOutputProfile::Native` | emulator/chip native出力。互換出力段、追加output gain、limiterを無効化 |
| `ChipOutputProfile::Tuned` | OpenMUCOM88/fmgen基準へ寄せる既定プリセット |
| `chipOutputTuningFor(engine, profile)` | engine/profile別のSSG mix、ADPCM A/B gain、output gain、互換出力段設定を返す |
| `effectiveSsgMixScaleFor(engine, profile)` | IFmEngine向けにL1 calibrationを畳み込んだSSG mix scaleを返す |

Tuned既定値:

| Engine | SSG Mix | Output Gain | Compatibility Output |
|--------|--------:|------------:|----------------------|
| `ChipEngine::Fmgen` | -3.0 dB | 1.0x | Off |
| `ChipEngine::Ymfm` | -4.0 dB | +2.5 dB | On (`1.9x` + soft limiter) |

---

## MmlParser（mml_parser.hpp）

MUCOM88互換MMLパーサー。MUCテキストからイベント列を生成する。

### メソッド

| メソッド | 説明 |
|---------|------|
| `MucFile parse(const std::string& muc)` | MUCテキストをパース |
| `bool loadVoiceDat(const std::string& path)` | voice.datをファイルから読み込み |
| `bool loadVoiceDatFromMemory(const uint8_t* data, size_t size)` | voice.datをメモリから読み込み |
| `int findPatchByName(const std::string& name)` | 音色名で検索（@"string"用） |

### MmlParser::MucFile 構造体

`parse()` の戻り値:

```cpp
struct MucFile {
    std::string title;       // #title
    std::string composer;    // #composer
    std::string voiceFile;   // #voice（voice.datファイル名）
    std::string pcmFile;     // #pcm（mucompcm.binファイル名）
    ChipMode chipMode;       // OPNA / OPM / OPNB
    bool chipModeExplicit;   // #mode が明示された場合のみ true（無指定の既定 OPNA と区別）
    int wholeTick;           // Cコマンド値（全音符クロック数、デフォルト128）

    std::array<std::vector<MmlEvent>, 11> channelEvents;  // A-K
    std::unordered_map<int, FmPatch> patches;              // @0-255
};
```

### MmlEvent 構造体

```cpp
struct MmlEvent {
    MmlEventType type;   // イベント種別
    uint32_t tick;       // 発火タイミング（Timer-Bティック）
    int note;            // MIDIノート番号（NOTE_ON時）
    int velocity;        // ベロシティ（NOTE_ON時）
    int duration;        // キーオン長（スタッカート適用後）
    int value;           // 汎用値（TEMPO=BPM, VOLUME=音量, PATCH=番号 等）
    // ... ビブラート、SSGエンベロープ等の追加フィールド
};
```

### MmlEventType（主要なもの）

| 値 | 説明 |
|----|------|
| `NOTE_ON` | ノートオン（note, velocity, duration） |
| `NOTE_OFF` | ノートオフ |
| `TEMPO` | テンポ変更（value = BPM） |
| `VOLUME` | 音量変更（value = 音量値） |
| `PATCH` | 音色変更（value = パッチ番号） |
| `PAN` | パン変更（value: 1=L, 2=R, 3=L+R） |
| `STACCATO` | スタッカート（value = q値） |
| `DETUNE` | デチューン（value = D値） |
| `VIBRATO` | ビブラート（vibDelay/Rate/Depth/Count） |
| `SSG_ENVELOPE` | SSGソフトウェアエンベロープ（envAL/AR/DR/SL/SR/RR） |
| `REVERB` | 擬似リバーブ（value = R値 0-15） |
| `LOOP_POINT` | Lコマンド（曲全体ループ位置） |
| `REGISTER_WRITE` | yコマンド（直接レジスタ書き込み） |
| `ECHO` | エコー（\コマンド） |
| `TIE_KEYOFF` | ^タイ境界KEY_OFF（リバーブ用） |

---

## MmlEngine（mml_engine.hpp）

MMLシーケンサー。Timer-B駆動で11チャンネルのイベントを処理し、
IFmEngine経由でYM2608レジスタに書き込む。

### 定数

```cpp
static constexpr int MAX_MML_CHANNELS = 11;   // A-K
static constexpr int MAX_FM_CHANNELS  = 6;    // A-C, H-J
static constexpr int MAX_SSG_CHANNELS = 3;    // D-F
```

### 初期化・イベント設定

| メソッド | 説明 |
|---------|------|
| `init(engine, sampleRate, chipClock=7987200)` | 初期化。IFmEngineポインタ、サンプルレート、チップクロックを指定 |
| `setEvents(ch, events)` | チャンネルにイベント列を設定（0-10） |
| `setPatch(patchNo, patch)` | 音色を登録（0-255） |
| `setWholeTick(wt)` | 全音符クロック数を設定 |
| `setLoop(loop)` | ループ ON/OFF（デフォルト: false。`loadFromParseResult()` でLコマンドがあれば自動的にtrueに設定される） |
| `setCommonEndTick(tick)` | ループ終端tickを外部指定（テスト用） |
| `loadFromParseResult(muc)` | MucFileから音色・全音符クロック・全チャンネルイベントを一括設定 |

### PCMデータ

| メソッド | 説明 |
|---------|------|
| `loadPcmBinary(data, size)` | mucompcm.binを統合ロード（PCMテーブル + ADPCM-Bデータを内部分割） |
| `loadPcmBinaryFile(path)` | mucompcm.binファイルを統合ロード |
| `loadPcmData(data, size)` | mucompcm.binのPCMアドレステーブルを解析（マルチサンプル情報のみ） |

### 再生制御

| メソッド | 説明 |
|---------|------|
| `play()` | 再生開始（全チャンネルリセット→tick 0のイベントを即時処理→Timer-B駆動開始） |
| `stop()` | 停止（全音消音） |
| `pause()` | 一時停止（全音消音、状態は保持） |
| `resume()` | 再開（音色再適用→再生続行） |
| `advance(frameCount)` | 指定サンプル数だけ時間を進める。16サンプル単位で内部処理 |

### ボイス再生

| メソッド | 説明 |
|---------|------|
| `loadVoiceTable(path)` | ボイステーブルをファイルから読み込み（IFmEngineにパススルー） |
| `loadVoiceTableFromMemory(data, size)` | メモリから読み込み |
| `hasVoiceTable()` | ボイステーブルがロード済みか |
| `playVoice(voiceId)` | ボイス再生（Kトラック自動抑制） |
| `stopVoice()` | ボイス停止（Kトラック復帰） |
| `isVoicePlaying()` | ボイス再生中か |
| `tickVoiceTimer(frameCount)` | タイマー更新（advance後に呼ぶ） |
| `stopAdpcmB()` | ADPCM-B強制停止（BGM + ボイス両方） |
| `setVoiceVolume(vol)` | ボイス音量設定。0.0=無音、1.0=最大。マスターボリュームと加算適用。play()/stop()でリセットされない |
| `getVoiceVolume()` | 現在のボイスボリューム（0.0-1.0） |

ボイス再生中は `m_voiceOverride` フラグによりBGMのKトラック（ch 10）の
イベント処理が抑制される。ボイス終了時に自動で解除される。

### ダッキング（ボイス再生中のBGM自動減衰）

| メソッド | 説明 |
|---------|------|
| `setDucking(attTarget, releaseSec=0.15)` | ダッキング設定。attTarget=FM TL加算値（20≈-15dB）、releaseSec=復帰時間。attTarget=0で無効 |

`setDucking()` を設定すると、`playVoice()` 呼び出し時にBGM全チャンネル（FM/SSG/ADPCM-A/ADPCM-B）が即座に減衰し、
ボイス終了後に releaseSec かけて元の音量に復帰する。
ボイス再生中は `recalcGlobalAtt()` のガードによりBGM経路のADPCM-Bレジスタ書き込みがスキップされ、
ボイスはフェード・ダッキングの影響を受けない。
ボイス自体の音量は `MmlEngine::playVoice()` が `255 - (masterAtt + voiceAtt) * 2`（0-255）を算出し、
再生開始時に `IFmEngine::playVoice(voiceId, level)` の `level` で ADPCM-B ボリューム（0x10B）へ一発書き込みする。
→ **マスターボリューム + ボイスボリューム（`setVoiceVolume()`）の両方が反映される**（#196 で修正。
旧実装は後追い `writeReg(1, 0x0B, ..)` が writeReg ガードに弾かれボイス音量が 0xFF 固定になっていた）。
デフォルト: **無効**（attTarget=0）。`play()` / `stop()` でリセットされる。

### チャンネルハイジャック（効果音割り込み用）

| メソッド | 説明 |
|---------|------|
| `hijackChannel(ch)` | チャンネルをSEモードに設定。BGMのKEY_OFFを実行し、以降のレジスタ書き込みを抑制。外部コードがIFmEngine::writeReg()で直接制御可能になる |
| `releaseChannel(ch)` | チャンネルをBGMモードに復帰。音色・音量・PANを現在のBGM状態に復元 |
| `isChannelHijacked(ch)` | ハイジャック中か |

**動作仕様:**
- ハイジャック中: BGMのイベント進行は継続（曲の再生位置を追跡）するが、レジスタ書き込み・LFO・ポルタメント・SSGエンベロープは実行しない
- 復帰時: FM音色・TL・PAN、SSGミキサーを現在のBGM状態に復元
- 対象: FM ch1-6 (A-C, H-J)、SSG ch1-3 (D-F)。Rhythm (G)、ADPCM-B (K) はハイジャック対象外

**レジスタ保護:** ハイジャック中のチャンネルは以下の全パスでレジスタ書き込みがスキップされ、SE再生中の音色/音量がBGM側から上書きされることはない:
- `advance()` 内のイベント処理・SOFENV・FMリバーブ
- `recalcGlobalAtt()` （フェードアウト・ダッキング変更時）
- `resume()` （一時停止→再開時）
- `globalLoopRestart()` / `perChannelRestart()` （BGMループ巻き戻し時）

### SE再生（効果音、Richモード対応）

#### SeMode

```cpp
enum class SeMode { Classic, Rich };
```

| 値 | 説明 |
|----|------|
| `Classic` | BGMチャンネルをハイジャックしてSE再生（デフォルト、従来方式） |
| `Rich` | 専用SEチップ（2台目のIFmEngine）のFM 6chでSE再生。BGMチャンネル不使用 |

#### SeSequenceNote 構造体

```cpp
struct SeSequenceNote {
    int startNote   = 60;   // 開始ノート番号
    int endNote     = -1;   // 終了ノート番号 (-1 = スイープなし)
    int durationMs  = 100;  // このノートのデュレーション(ms)
};
```

#### メソッド

| メソッド | 説明 |
|---------|------|
| `setSeMode(mode, seEngine=nullptr)` | SEモード設定。Rich時はinit()済みのIFmEngineを渡す。切り替え時に全SE停止 |
| `seMode()` | 現在のSEモード |
| `playSe(patch, noteNum, velocity=15, durationMs=0)` | SE発音。音色・ノート・音量を指定。durationMs>0で自動停止。戻り値: SEスロット番号(0-5)、-1=失敗 |
| `playSeSequence(patch, notes, noteCount, velocity=15)` | SEシーケンス再生（マルチノート + ピッチスイープ）。notes: SeSequenceNote配列、noteCount: ノート数(1-8)。戻り値: SEスロット番号(0-5)、-1=失敗 |
| `stopSe(seSlot)` | 指定スロットのSE停止（シーケンス再生中でも即時停止） |
| `stopAllSe()` | 全SE停止 |
| `setSeFrequency(seSlot, noteNum)` | アクティブなSEスロットのFM周波数を変更（F-Number更新のみ、パッチ再適用なし）。非アクティブスロットは無視 |
| `isSeActive(seSlot)` | 指定スロットがアクティブか |
| `activeSeCount()` | アクティブなSEスロット数 |
| `renderMixed(out, frameCount)` | BGM+SE混合レンダリング。advance()+tickVoiceTimer()+SE duration追跡+両チップPCM生成+ミキシングを一括実行 |

**動作仕様:**
- **Classic**: BGMのFMチャンネル（J,I,H,C,B,A優先）をhijackして発音。ノートオフ中のチャンネルを優先選択
- **Rich**: SEチップのFM 6チャンネルに割り当て。全スロット使用中は最古のSEを停止して再割り当て（oldest策略）
- **音量**: マスターボリューム（`m_masterAtt`）のみ適用。フェード・ダッキングはBGM専用でSEには影響しない
- **スロット数**: 最大6（`MAX_SE_SLOTS`）。スロット番号はRichモードではFMインデックスと1:1対応
- **スレッド契約**: `playSe()`/`playSeSequence()` を含む全 mutator は `advance()`/`renderMixed()` と同一オーディオスレッド（または外部でオーディオコールバックと相互排他）から呼ぶこと（`playVoice()` と同じ方針）。MmlEngine はスレッドセーフではなく、別スレッドからの呼び出しは未定義（`m_voiceDuckState` の atomic はクロススレッド安全を保証しない）

**SEシーケンス再生:**
- `playSeSequence()` はマルチノートSEを1つのスロットで再生する。各ノートのdurationMs経過後に自動的に次のノートへ遷移
- ノート遷移時はパッチ再適用なし（周波数変更 + KEY_ONのみ）でレガート遷移
- **ピッチスイープ**: `endNote != -1` のノートでは、durationMs間でstartNote→endNoteへ線形ピッチ補間（サンプル精度）
- 全ノート消費で通常のstopSe処理。`stopSe()` で途中停止も可能
- Classic/Richモード両対応（スロット確保は `playSe()` と同じロジック）

### マスターボリューム

| メソッド | 説明 |
|---------|------|
| `setMasterVolume(vol)` | マスターボリューム設定。0.0=無音、1.0=最大。ダッキング・フェードとは独立。play()/stop()でリセットされない |
| `getMasterVolume()` | 現在のマスターボリューム（0.0-1.0） |

マスターボリュームはBGM全チャンネル（FM/SSG/ADPCM-A/ADPCM-B）に加え、`playVoice()` のボイス再生、および`playSe()` のSE再生にも適用される。
デフォルト値: **1.0**（最大音量）。`play()` / `stop()` でリセットされない（ゲーム設定として永続）。

### BGMボリューム

| メソッド | 説明 |
|---------|------|
| `setBgmVolume(vol)` | BGMボリューム設定。0.0=無音、1.0=最大。マスターボリュームと加算適用。play()/stop()でリセットされない |
| `getBgmVolume()` | 現在のBGMボリューム（0.0-1.0） |

BGMボリュームはBGM全チャンネル（FM/SSG/ADPCM-A/ADPCM-B）専用の音量調整。SE・ボイスには影響しない。
実際のBGM減衰は `masterAtt + bgmAtt + fadeAtt + duckAtt` の合算で各チャンネルに適用される。
デフォルト値: **1.0**（最大音量）。`play()` / `stop()` でリセットされない（ゲーム設定として永続）。

### SEボリューム

| メソッド | 説明 |
|---------|------|
| `setSeVolume(vol)` | SEボリューム設定。0.0=無音、1.0=最大。マスターボリュームと加算適用。play()/stop()でリセットされない |
| `getSeVolume()` | 現在のSEボリューム（0.0-1.0） |

SEボリュームは `playSe()` で発音するSE専用の音量調整。マスターボリュームと独立に設定でき、
実際のSE減衰は `masterAtt + seAtt` の合算でキャリアTLに適用される。
フェード・ダッキングの影響は受けない。
デフォルト値: **1.0**（最大音量）。`play()` / `stop()` でリセットされない（ゲーム設定として永続）。

### ボイスボリューム

| メソッド | 説明 |
|---------|------|
| `setVoiceVolume(vol)` | ボイスボリューム設定。0.0=無音、1.0=最大。マスターボリュームと加算適用。play()/stop()でリセットされない |
| `getVoiceVolume()` | 現在のボイスボリューム（0.0-1.0） |

ボイスボリュームは `playVoice()` で再生するゲームボイス専用の音量調整。マスターボリュームと独立に設定でき、
実際のボイス減衰は `masterAtt + voiceAtt` の合算でADPCM-Bボリュームレジスタに適用される。
フェード・ダッキングの影響は受けない。
デフォルト値: **1.0**（最大音量）。`play()` / `stop()` でリセットされない（ゲーム設定として永続）。

### フェードアウト/イン

| メソッド | 説明 |
|---------|------|
| `fadeOut(seconds, onComplete)` | 指定秒数で無音までフェードアウト。seconds=0で即時無音。onComplete でフェード完了時の自動アクションを指定（デフォルト: FadeAction::None） |
| `fadeIn(seconds)` | 指定秒数でマスターボリュームまでフェードイン。seconds=0で即時復帰 |
| `resetFade()` | フェードを即座にキャンセルしマスターボリュームに復帰 |
| `isFading()` | フェード進行中か |
| `isFadeOutDone()` | フェードアウト完了後にFadeActionが実行されたか。`play()` でリセット |

フェードはサンプル単位で進行（テンポ非依存）。デフォルト: **フェードなし**（fadeAtt=0）。`play()` / `stop()` でリセットされる。

#### FadeAction

```cpp
enum class FadeAction {
    None,         // 何もしない（デフォルト、後方互換）
    Stop,         // BGM停止（stop() 呼び出し相当）
    StopAndReset  // BGM停止 + チップリセット（IFmEngine::reset()）
};
```

| 値 | 説明 |
|----|------|
| `None` | 何もしない（デフォルト）。従来の `fadeOut(seconds)` と同一動作 |
| `Stop` | フェードアウト完了時に `stop()` を自動呼び出し。全音消音しBGMを停止 |
| `StopAndReset` | `stop()` に加え、BGMチップとSEチップの `reset()` を実行。Richモード時はSEチップの初期状態（PAN/mixer/timer）も再確立 |

`isFadeOutDone()` は FadeAction 実行後に `true` を返す。`play()` で自動的に `false` にリセットされる。
`stop()` では `isFadeOutDone()` をリセットしない（呼び出し側がポーリングで検出するため）。

### 音量バランス・制御

| メソッド | 説明 |
|---------|------|
| `setSsgMixScale(ssgScale)` | SSG出力のリニアスケール設定。1.0=等倍、0.71≈-3dB（MUCOM88Vデフォルト）。IFmEngine にパススルー |
| `getSsgMixScale()` | 現在のSSGスケール値 |
| `setOutputProfile(profile)` | Native/Tuned出力プリセットを適用する。既定はTuned |
| `outputProfile()` | 現在の出力プリセット |
| `setOutputGain(gain)` | 出力ゲイン設定。`renderMixed()` でBGM PCMにゲインを掛け、最終出力を `softLimit16()` で処理する。Richモード時はBGMのみにゲインを適用しSEは等倍で加算。Tuned既定値はengine別に `chip_output_tuning.hpp` で定義。play()/stop()でリセットされない |
| `getOutputGain()` | 現在の出力ゲイン値 |
| `setGlobalAttenuation(att)` | ダッキング減衰設定。FM: TL加算(0-127)、SSG: att/4、ADPCM-A/B: スケーリング。マスターボリューム・フェードと独立に加算される |
| `globalAttenuation()` | 合算減衰値（masterAtt + bgmAtt + fadeAtt + duckAtt） |

**4層減衰アーキテクチャ:** マスターボリューム、BGMボリューム、フェード、ダッキングの4成分がFM TL単位（0=最大、127=無音）で独立に管理され、合算値 `globalAtt = clamp(masterAtt + bgmAtt + fadeAtt + duckAtt, 0, 127)` が各BGMチャンネルに適用される。SE・ボイスには `masterAtt + seAtt` / `masterAtt + voiceAtt` がそれぞれ独立に適用される。

チャンネル種別ごとのスケーリング:
- **FM**: キャリアTLに +globalAtt（TLは0=最大、127=無音）
- **SSG**: 振幅から -globalAtt/4（振幅は0-15）
- **ADPCM-A**: TLから -globalAtt*63/127（TLは0-63）
- **ADPCM-B (Kトラック)**: ボリュームから -globalAtt*2（レジスタは0=無音、255=最大）

ループリスタート時（`globalLoopRestart()` / `perChannelRestart()`）にもADPCM-Bボリュームが明示的に復元され、フェード中のループでも減衰が途切れない。

### 状態取得

| メソッド | 説明 |
|---------|------|
| `isPlaying()` | 再生中か |
| `globalTick()` | 現在のグローバルティック |
| `globalTempo()` | 現在のテンポ（BPM） |
| `chNoteOn(ch)` | チャンネルがノートオン中か |
| `chNote(ch)` | 現在のMIDIノート番号 |
| `chVolume(ch)` | 音量（FM/SSG: 0-15） |
| `chPan(ch)` | パン（1=L, 2=R, 3=L+R） |
| `chReverb(ch)` | リバーブ値（FM専用、Rコマンド値 0-15） |
| `chNoteOnCount(ch)` | ノートオンカウンター（UI用、ワンショット楽器検出） |
| `fmPatchNo(fi)` | FM音色番号（fi=FMインデックス 0-5） |
| `commonEndTick()` | 全チャンネルの最大endTick |
| `loopTickOffset()` | ループ開始位置のティック |
| `loopCount()` | 現在のループ回数 |

### チャンネル種別判定（static）

| メソッド | 対象 |
|---------|------|
| `isFM(ch)` | ch 0-2, 7-9 |
| `isSSG(ch)` | ch 3-5 |
| `isRhythm(ch)` | ch 6 |
| `isADPCMB(ch)` | ch 10 |
| `toFMIndex(ch)` | MMLチャンネル→FMインデックス(0-5) |
| `toSSGIndex(ch)` | MMLチャンネル→SSGインデックス(0-2) |

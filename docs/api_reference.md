# libmucom88 API リファレンス

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
| `loadVoiceTable(path)` | ボイステーブル読み込み |
| `loadVoiceTableFromMemory(data, size)` | ボイステーブルメモリ読み込み |
| `hasVoiceTable()` | ボイステーブルがロード済みか |
| `playVoice(voiceId)` | ボイス再生開始（ADPCM-B使用） |
| `stopVoice()` | ボイス停止 |
| `isVoicePlaying()` | ボイス再生中か |
| `tickVoiceTimer(frameCount)` | ボイス再生タイマー更新 |
| `stopAdpcmB()` | ADPCM-B強制停止（BGM + ボイス両方） |

---

## MmlParser（mml_parser.hpp）

MUCOM88互換MMLパーサー。MUCテキストからイベント列を生成する。

### MmlParser::MucFile 構造体

`parse()` の戻り値:

```cpp
struct MucFile {
    std::string title;       // #title
    std::string composer;    // #composer
    std::string voiceFile;   // #voice（voice.datファイル名）
    std::string pcmFile;     // #pcm（mucompcm.binファイル名）
    ChipMode chipMode;       // OPNA / OPM / OPNB
    int wholeTick;           // Cコマンド値（全音符クロック数、デフォルト128）

    std::array<std::vector<MmlEvent>, 11> channelEvents;  // A-K
    std::unordered_map<int, FmPatch> patches;              // @0-255
};
```

### メソッド

| メソッド | 説明 |
|---------|------|
| `MucFile parse(const std::string& muc)` | MUCテキストをパース |
| `bool loadVoiceDat(const std::string& path)` | voice.datをファイルから読み込み |
| `bool loadVoiceDatFromMemory(const uint8_t* data, size_t size)` | voice.datをメモリから読み込み |
| `int findPatchByName(const std::string& name)` | 音色名で検索（@"string"用） |

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
| `setLoop(loop)` | ループ ON/OFF |
| `setCommonEndTick(tick)` | ループ終端tickを外部指定（テスト用） |

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

ボイス再生中は `m_voiceOverride` フラグによりBGMのKトラック（ch 10）の
イベント処理が抑制される。ボイス終了時に自動で解除される。

### ダッキング（ボイス再生中のBGM自動減衰）

| メソッド | 説明 |
|---------|------|
| `setDucking(attTarget, releaseSec=0.15)` | ダッキング設定。attTarget=FM TL加算値（20≈-15dB）、releaseSec=復帰時間。attTarget=0で無効 |

`setDucking()` を設定すると、`playVoice()` 呼び出し時にFM/SSGが即座に減衰し、
ボイス終了後に releaseSec かけて元の音量に復帰する。
ADPCM-A（リズム）とADPCM-B（ボイス）には影響しない。

### 音量制御

| メソッド | 説明 |
|---------|------|
| `setGlobalAttenuation(att)` | グローバル減衰設定。FM: TL加算(0-127)、SSG: att/4で換算。ADPCM-A/Bには影響しない |
| `globalAttenuation()` | 現在の減衰値 |

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

### PCMデータ

| メソッド | 説明 |
|---------|------|
| `loadPcmData(data, size)` | mucompcm.binのPCMアドレステーブルを解析（マルチサンプル情報のみ） |

---

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
enum class ChipMode { OPNA, OPM, OPNB };
```

MUCファイルの `#mode` ディレクティブで指定。
現時点では `OPNA` が標準。OPM/OPNB は将来のG2モード用。

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

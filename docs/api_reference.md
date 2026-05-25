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
| `setSsgMixScale(ssgScale)` | SSGミックスレベル設定（1.0=等倍、0.71≈-3dB）。デフォルト実装は何もしない |
| `getSsgMixScale()` | 現在のSSGスケール値（デフォルト1.0） |

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

ボイス再生中は `m_voiceOverride` フラグによりBGMのKトラック（ch 10）の
イベント処理が抑制される。ボイス終了時に自動で解除される。

### ダッキング（ボイス再生中のBGM自動減衰）

| メソッド | 説明 |
|---------|------|
| `setDucking(attTarget, releaseSec=0.15)` | ダッキング設定。attTarget=FM TL加算値（20≈-15dB）、releaseSec=復帰時間。attTarget=0で無効 |

`setDucking()` を設定すると、`playVoice()` 呼び出し時にFM/SSGが即座に減衰し、
ボイス終了後に releaseSec かけて元の音量に復帰する。
ADPCM-B（ボイス）には影響しない。FM/SSG/ADPCM-A（リズム）が減衰対象。
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

#### メソッド

| メソッド | 説明 |
|---------|------|
| `setSeMode(mode, seEngine=nullptr)` | SEモード設定。Rich時はinit()済みのIFmEngineを渡す。切り替え時に全SE停止 |
| `seMode()` | 現在のSEモード |
| `playSe(patch, noteNum, velocity=15, durationMs=0)` | SE発音。音色・ノート・音量を指定。durationMs>0で自動停止。戻り値: SEスロット番号(0-5)、-1=失敗 |
| `stopSe(seSlot)` | 指定スロットのSE停止 |
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
- **スレッド安全性**: `playSe()`はオーディオスレッドから呼ぶこと（`playVoice()`と同じ方針）

### マスターボリューム

| メソッド | 説明 |
|---------|------|
| `setMasterVolume(vol)` | マスターボリューム設定。0.0=無音、1.0=最大。ダッキング・フェードとは独立。play()/stop()でリセットされない |
| `getMasterVolume()` | 現在のマスターボリューム（0.0-1.0） |

マスターボリュームはBGM全チャンネル（FM/SSG/ADPCM-A/ADPCM-B）に加え、`playVoice()` のボイス再生、および`playSe()` のSE再生にも適用される。
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

### フェードアウト/イン

| メソッド | 説明 |
|---------|------|
| `fadeOut(seconds)` | 指定秒数で無音までフェードアウト。seconds=0で即時無音 |
| `fadeIn(seconds)` | 指定秒数でマスターボリュームまでフェードイン。seconds=0で即時復帰 |
| `resetFade()` | フェードを即座にキャンセルしマスターボリュームに復帰 |
| `isFading()` | フェード進行中か |

フェードはサンプル単位で進行（テンポ非依存）。デフォルト: **フェードなし**（fadeAtt=0）。`play()` / `stop()` でリセットされる。

### 音量バランス・制御

| メソッド | 説明 |
|---------|------|
| `setSsgMixScale(ssgScale)` | SSG出力のリニアスケール設定。1.0=等倍、0.71≈-3dB（MUCOM88Vデフォルト）。IFmEngine にパススルー |
| `getSsgMixScale()` | 現在のSSGスケール値 |
| `setGlobalAttenuation(att)` | ダッキング減衰設定。FM: TL加算(0-127)、SSG: att/4、ADPCM-A/B: スケーリング。マスターボリューム・フェードと独立に加算される |
| `globalAttenuation()` | 合算減衰値（masterAtt + fadeAtt + duckAtt） |

**3層減衰アーキテクチャ:** マスターボリューム、フェード、ダッキングの3成分がFM TL単位（0=最大、127=無音）で独立に管理され、合算値 `globalAtt = clamp(masterAtt + fadeAtt + duckAtt, 0, 127)` が各チャンネルに適用される。

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

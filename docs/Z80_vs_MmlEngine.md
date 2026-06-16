# MUCOM88 Z80ドライバー vs MmlEngine 比較

OpenMUCOM88（Z80 VM + fmgen）と libmucom88（MmlParser + MmlEngine）の
アーキテクチャ・動作の差異をまとめたドキュメント。
修正や改変が入った際は本ドキュメントも更新すること。

> **正本について:** 本文書は libmucom88 の MmlEngine 内部挙動を Z80 正本（`music.asm`）と
> 突き合わせた MML 再現精度の参照表。MmlEngine の正本は本リポジトリ（libmucom88）なので、
> エンジン挙動の変更時は本文書を更新する。同一文書が mucom88v `docs/dev/Z80_vs_MmlEngine.md`
> にも存在するが、そちらは回帰テスト（`muc_regtest`）の数値検証に紐づくダウンストリーム控え。
> 差分検証は mucom88v 側の `build/muc_regtest` / `build/muc_compare` で行う（本リポジトリ単体にテストは無い）。

---

## アーキテクチャ

| | MUCOM88 (Z80 VM) | MmlEngine (libmucom88) |
|---|---|---|
| **コンパイラ** | muc88.asm — MMLテキスト→バイトコード(Z80メモリ上) | mml_parser.hpp — MMLテキスト→MmlEvent列(C++ vector) |
| **ランタイム** | music.asm — バイトコードインタープリタ | mml_engine.hpp — イベント列シーケンサー |
| **FM音源** | fmgen (Z80 I/Oポート経由) | fmgen (IFmEngine::writeReg直接呼び出し) |
| **CPU** | Z80エミュレータ(μPD780C-1相当) | ネイティブC++ |
| **メモリモデル** | Z80: 64KBフラットメモリ(0xC200〜にMMLデータ) | C++ヒープ(制約なし) |
| **タイマー** | YM2608 Timer-B割り込み(INT3→0xF308ベクタ) | ソフトウェアカウンタ(fmgen互換Timer-B周期計算) |
| **起動遅延** | predelay=4 (約96ms、Z80初期化+コンパイル時間) | なし(即時再生) |

### Z80側の処理フロー
```
MMLテキスト
  ↓ muc88.asm (コンパイラ)
バイトコード (Z80メモリ 0xC200〜)
  ↓ Timer-B INT3割り込み (毎tick)
music.asm DRIVE → FMENT/SSGENT (インタープリタ)
  ↓ FMSUB/SSGSUB/PLLFO
YM2608 レジスタ書き込み (I/Oポート経由)
```

### MmlEngine側の処理フロー
```
MMLテキスト
  ↓ mml_parser.hpp (パーサー、全展開)
MmlEvent列 (C++ vector<MmlEvent>)
  ↓ advance() (ソフトウェアTimer-Bカウンタ、毎tick)
processEvents → tickLfo → tickPortamento
  ↓ IFmEngine::writeReg()
YM2608 レジスタ書き込み (fmgen直接呼び出し)
```

---

## Timer-B / テンポ

| | MUCOM88 | MmlEngine |
|---|---|---|
| **Timer-B制御** | 毎tick 0x27に2回書き込み(OFF=0x38, ON=0x3A) | 0x27を1回のみ(初回テンポ設定時) |
| **tick発生** | fmgen Timer-Bカウンタ→INT3割り込み | 同一アルゴリズム(int truncation互換) |
| **テンポ変更** | tコマンド→Timer-B値レジスタ直書き | TEMPOイベント→recalcTimerB() |
| **Cコマンド** | コンパイラが音符長計算に使用 | パーサーがwholeTick設定、BPM変換に影響 |
| **tick精度** | fmgen内部カウンタ依存(int truncation) | 同一(fmgen互換int truncation再現) |

**0x27書き込み数差の理由:** Z80インタープリタはTimer-B割り込みを自己管理するため
毎tickでOFF/ONを書き込む。MmlEngineはソフトウェアカウンタで代替しているため不要。
この差はレジスタ書き込み回数のカウント差として現れるが、音質への影響はない。

---

## チャンネル処理

| | MUCOM88 | MmlEngine |
|---|---|---|
| **処理順** | FM1-3→SSG1-3→Rhythm→FM4-6→ADPCM-B | ch 0-10 順次(実質差なし) |
| **処理フロー** | FMENT→FMSUB→PLLFO / SSGENT→SSGSUB→PLLFO | processEvents→tickLfo→tickPortamento |
| **waitデクリメント** | バイトコード先頭バイト=残りwait数 | tick比較(eventのtickフィールド) |
| **qカット判定** | wait==q でKEY_OFF | NOTE_OFFイベント(パーサーが事前生成) |

---

## FM音源処理

| | MUCOM88 | MmlEngine |
|---|---|---|
| **音色適用(OTOPST)** | 25バイトバイトコード→レジスタ直書き | FmPatch構造体→writeReg |
| **音量(STVOL)** | イベント駆動5箇所のみ | イベント駆動(NOTE_ON, v, ), @, RF0) |
| **音量テーブル** | FMVDAT[TOTALV+v] (20エントリ) | FMVDAT[v+4] + m_globalAtt (全20エントリ保持, Issue #48) |
| **(/) クランプ** | VOLUPF: ADD A,(IX+6) クランプなし | FM: (コマンドでクランプなし(負値許容), )は上限15でクランプ (Issue #57) |
| **KEY_ON/OFF** | 0x28レジスタ直書き | 同一(0x28直書き) |
| **タイ判定** | KEYOFF_FLAG=0 かつ 同キーコード→KEY_ONスキップ | 同一(Issue #44で修正: タイ時KEY_OFFスキップ+同音判定) |

### STVOL呼び出しタイミング（Z80準拠5箇所）
1. KEYON直後（リバーブON時）: music.asm line 745-746
2. vコマンド（VOLPST）: line 1226
3. )コマンド（VOLUPF）: line 1019
4. @コマンド（OTOPST）内: line 1173
5. RF0（リバーブOFF）: line 916

---

## SSG処理

| | MUCOM88 | MmlEngine |
|---|---|---|
| **毎tick処理(SSSUB0)** | エンベロープ有効時のみ音量書き込み | 同一(ssgTickEnvelope) |
| **SOFENV** | Z80サブルーチン(ATTACK/DECAY/SUSTAIN/RELEASE) | 同一アルゴリズム(C++移植) |
| **@Nプリセット** | SSGDAT→E+P+M全適用(OTOSSG) | E+P+M全適用(ssgApplyPreset, Issue #43で修正) |
| **ミキサー(0x07)** | NOISE(P)コマンドで更新 | 同一 + @Nプリセットでも更新 |
| **音量計算** | ((vol+1)*envelope)>>8 | 同一 |
| **トーンレジスタ** | keyOn時 + PLLFO(LFO有効時毎tick) | 同一(ssgWriteFreq + updatePitch) |
| **KEY_ON tick** | SOFEV7のみ(エンベロープ進行なし) | 同一(ssgEnvKeyOnTickフラグ, Issue #29で修正) |

---

## LFO（ソフトウェアビブラート）

| | MUCOM88 | MmlEngine |
|---|---|---|
| **処理タイミング** | PLLFO: FMSUB/SSGSUBの後に毎tick | tickLfo: processEventsの後に毎tick |
| **パラメータ** | M delay,rate,depth,count | 同一(MmlEvent::vibDelay/Rate/Depth/Count) |
| **ML(振幅変更)** | IX+25/26 振幅値を変更 | 同一(LFO_PARAMイベント, #37クローズ済み) |
| **MC(レート変更)** | IX+21 クロック単位を変更 | 同一(LFO_PARAMイベント, #38クローズ済み) |
| **ピッチ反映** | SSG: 0x00+ch, FM: 0xA4+ch | FMは同一。SSGはZ80が深さを >>octave 縮小(SNUMGETL)するのに対し旧実装は1:1適用で高octaveで過大だった → libmucom88#63で修正(2026-06-16) |
| **SSGプリセットLFO** | @N適用時にM全パラメータ設定 | 同一(Issue #43で修正) |

---

## リバーブ

| | MUCOM88 | MmlEngine |
|---|---|---|
| **FM** | KEY_OFF代替: FS2→STV2(FMVDAT[(IX+6+R)>>1], 定数, 毎tick) | 同一(fmSetReverbVolume, Issue #48+#49で修正) |
| **SSG** | SOFEV7内: amp=(amp+R)>>1 (KEY_OFF後のみ) | 同一 |
| **Rmモード** | bit4: 0=休符含む, 1=qカットのみ | 同一(reverbQCutOnly) |
| **FM計算式** | FMVDAT[(IX+6 + R) >> 1]（IX+6=vol+4, STV2直接） | 同一(vol+4+R)>>1, Issue #49で修正) |
| **SSG計算式** | int((現在音量 + R) / 2) | 同一 |
| **FM KEYOFF差** | FMSUB3(タイ)+Rm1→FS3→KEYOFF / FMSUB0毎tick | ^タイ境界: TIE_KEYOFFイベントで再現(Issue #55で修正)。&タイ内自動分割は除外（KEY_ON欠落のため） |
| **FM NOTE_ON** | FMSUB1: SET KEYOFF_FLAG → FMSUB5: 必ずKEY_OFF → FMSUB4→FMSUB9: KEY_ON | 同一(Issue #44再修正: 同音スキップ削除、常にKEY_OFF+KEY_ON) |

---

## ADPCM

| | MUCOM88 | MmlEngine |
|---|---|---|
| **ADPCM-A(リズム)** | 0x10レジスタで6楽器同時発音 | 同一 |
| **ADPCM-B(Kトラック)** | PCMADR+PCMNMBデルタN計算 | 同一 |
| **PVMODE(V1)** | IX+6(baseVol)/IX+7(addVol)切替 | 同一(m_pcmVolMode/m_pcmAddVol) |
| **delta-N計算** | PCMNMBテーブル+Z80オフセット | 同一(adpcmbNoteToDeltaN) |
| **ADPCM-Bボリューム** | STV4→STV1: IX+6=user_vol（+4なし）, PLAY: TOTALV*4+IX+6 | 同一（+4不要と判明、Issue #18クローズ） |
| **ADPCM-Bパン** | STEREOルーチン: PCMFLG!=0→PCMLR変数に格納, PLAYでreg 0x01に書込 | 同一(doSetPan ADPCM-B分岐, Issue #72で修正) |
| **ADPCM-Aパン** | STE2: 楽器単位のIL(0x18-0x1D) PAN bit6-7, p $NNで設定 | 同一(doSetPan リズム分岐, m_rhythmIL) |

---

## コンパイラ vs パーサー

| | muc88.asm (Z80コンパイラ) | mml_parser.hpp |
|---|---|---|
| **出力形式** | バイトコード(Z80メモリ上) | MmlEvent列(C++ vector) |
| **ループ展開** | バイトコード内にループ命令残す(ランタイム展開) | パーサーが静的展開(ブラケットループ→イベント複製) |
| **曲長統一** | パディングなし（各チャンネル独立ループ） | per-channel独立ループ（endTickで即時リスタート、Issue #68で修正） |
| **yXXコマンド** | SETR4: slot 1-based(1-4), SETR8: +COMNOW(ch offset) | 同一(Issue #59で修正: slot-1, portBase+chOff) |
| **マクロ** | `*N{}`をコンパイル時展開 | collectMacros→expandMacros(同一) |
| **@"string"** | voice.dat name[6]検索→番号解決 | findPatchByName(Issue #42で実装) |
| **V(TV_OFS)** | コンパイル時ボリュームオフセット(SSG: v+V, FM: v+V+4) | 同一(tvOffset, Issue #47で修正) |
| **\エコー(SETBEF)** | STBF3: 0xFB(-M) + BEFCO(フル音長) + BEFTONE[N] + 0xFB(+M) | 同一(lastFullTicks, Issue #45で修正。FM VOLUPFクランプなし, Issue #58) |
| **\=N,M スコープ** | BFDAT/VDDATはグローバル変数（全チャンネル共有） | 同一(m_echoBufIdx/m_echoVolRedはMmlParserクラスメンバー, Issue #64で修正) |
| **\エコー+ループ** | ループ内でも毎回0xFB(VOLUPF)実行→累積変化反映 | エコーVOLUMEにnote=3マーカー→volDelta補正対象(Issue #58) |
| **ブラケットループ内(/)** | ランタイムで毎回実行→累積的に音量変化 | 同一(volDelta累積補正, Issue #51で修正) |
| **^タイ境界** | バイトコードのtie bit→FMSUB3(KEY_OFF/FS2) | TIE_KEYOFFイベント(Issue #55で実装) |
| **&タイ境界** | TIEコマンド(0xFB)→KEYOFF_FLAG解除→KEYON(エンベロープ再起動) | パーサーが結合（KEY_ON/OFFなし） |
| **長音符分割** | 7bit制限(max 127)で自動分割、tie bit付き | ^セグメント内のみ自動分割（&セグメント内は除外） |
| **/ブレーク(SETRJP)** | SETRJP: frame[8-9]にbreakTickを保存。SETLPE: breakTick==0を「ブレークなし」と誤判定（**Z80コンパイラバグ**、T_CLKのみ影響） | breakRelTick==0を正しくブレークとして処理。tcount差はZ80バグ由来（Issue #70） |

### ブラケットループの構造差
Z80コンパイラはループ命令(`[`=0xF5, `]`=0xF6)をバイトコード内に残し、
ランタイムインタープリタがloop counterをデクリメントしながら展開する。
MmlParserはパース時に静的に全展開してイベント列に複製するため、
tick計算自体は同一だが構造が異なる。

**ボリューム累積（Issue #51）:** Z80はループ内の`(`/`)`を毎回ランタイム実行し
音量が累積変化する（例: `[(2d+4]3` → 8→6→4→2）。MmlParserは1回目のイベント列を
コピーするため、VOLUMEイベントのabsolute valueにper-iterationのvolDeltaを加算する
補正が必要。修正により eternalmelody avgRMS 3.777→0.994。

**per-channel独立ループ（Issue #68: Z80 MaxCountパディング不在の発見）:**
Z80コンパイラはMaxCountパディングを行わない。muc88.asm CMPENDは各チャンネルの
最後に0x00エンドマーカーを1バイト書くのみ。パディング用休符追加コードは存在しない。
各チャンネルはendTick（自身のtcount）でend markerに到達し、独立にDATA TOPへジャンプ。
ループ周期 = endTick - loopTick（= Z80 lcount）。

MmlEngineでは:
- 各チャンネルがイベント消費済み(eventIdx >= events.size())になった時点で即時リスタート
- 最初にイベント消費されたチャンネルの時点でper-channelモードに移行
- per-channelモード移行時に全チャンネルのperChLoopLen(=endTick-loopTick)を初期化
- processEventsのchTick計算: perChTickBase（= restartTick - loopTick）を使用
- 同じendTickの複数チャンネルは同時にリスタート（Z80と同様）
- Issue #71: 初回per-channel移行時に、移行発動チャンネルと同tickでイベント消費済みの
  他チャンネルも即座にリスタート。perChTickBase = endTick - loopTick で1tick遅れ補正

**vコマンド含有ループ（Issue #54）:** ループ本体に`v`コマンド（絶対音量設定）が
含まれる場合、Z80は毎回vでボリュームをリセットし`(`/`)`は常にリセット値からの
差分として動作する。MmlParserはVOLUMEイベントのnoteフィールドで相対変更(note=2)と
絶対設定(note=0)を区別し、絶対設定がある場合はvolDelta=0とする。
修正により stk020 avgRMS 2.024→1.307。

---

## 差異の分類

### 許容する差異（Z80 CPU・VM由来、再現不要）

| 項目 | 理由 |
|---|---|
| Z80 VM起動遅延(~96ms) | ハードウェア初期化+コンパイル時間。DAWプラグインでは不要 |
| Timer-B 0x27レジスタ書き込み回数(毎tick 2回 vs 1回) | Z80がTimer-B割り込みを自己管理するための構造。音質差なし |
| Z80メモリ制約(64KB) | MmlEngineはC++ヒープで制約なし。出力に影響しない |
| バイトコードインタープリタ vs イベントシーケンサー | 内部構造の差。出力は同一であるべき |
| チャンネル処理順(FM1-3→SSG→Rhythm→FM4-6→ADPCM vs ch0-10順次) | 同一tick内の処理順。実質差なし |
| Z80 SETLPE T_CLKバグ: `/`ブレークtick==0の誤判定 | muc88.asm SETLPE line 2145-2147: `OR D; JR Z,SETLPE3` で breakTick==0 を「ブレークなし」と誤判定。ループ先頭に`/`がある場合、T_CLK(tcount)が bodyTicks 分多くカウントされる。外側ループで増幅される（stk023 Part C: 600tick差）。ランタイム再生は正常（バイトコードのループ命令は正常動作）。T_CLKのみ影響。MmlParserの計算が正しい |

### 修正すべき機能差（Issue化対象）

| 項目 | 影響 | 状態 | Issue |
|---|---|---|---|
| ~~ADPCM-B PVMODE+4ボリューム補正~~ | ~~+4不要と判明（STV4→STV1パスは+4加算なし）~~ | **クローズ（修正不要）** | #18 |
| ~~ML(LFO振幅変更)コマンド~~ | ~~LFO depth個別変更~~ | **実装済み(クローズ)** | #37 |
| ~~MC(LFOレート変更)コマンド~~ | ~~LFO rate個別変更~~ | **実装済み(クローズ)** | #38 |
| ~~FMタイ判定の差異~~ | ~~タイ時KEY_OFFスキップ~~ | **再修正済み（同音スキップ削除）** | #44 |
| ~~\\エコーのtick計算差~~ | ~~staccato適用後のdurationを使用→フル音長に修正~~ | **修正済み** | #45 |
| SSG-V毎tick書き込み頻度差 | ref側の方がSSG音量レジスタ書き込みが多い曲がある | **修正済み** | #46 |
| ~~\\=N,Mエコーパラメータがper-channel~~ | ~~Z80 BFDAT/VDDATはグローバル、パーサーはチャンネルState~~ | **修正済み（グローバル化）** | #64 |
| ~~V(TV_OFS)コマンド未実装~~ | ~~ボリュームオフセット~~ | **修正済み** | #47 |
| FM reverb FS2のIX+6 +4補正 | リバーブTL計算で+4(SETVOL加算分)が欠落 | **修正済み** | #49 |
| ~~FMリバーブ中KEY_OFF回数差~~ | ~~Rm1使用曲でMmlEngineのKEY_OFFがZ80より少ない~~ | **根本原因=#51** | #50 |
| ~~ブラケットループ内(/)ボリューム累積~~ | ~~ループ展開時にVOLUMEイベントのデルタが累積されない~~ | **修正済み** | #51 |
| ~~ブラケットループ内v+(/）累積~~ | ~~vコマンドを含むループでvolDeltaが不正に累積~~ | **修正済み** | #54 |
| ~~RコマンドがreverbEnabledを自動設定しない~~ | ~~Z80 REVERVEは常にRF1をSETするがMmlEngineはしない~~ | **クローズ（実装済み確認）** | #52 |
| ~~RF0がdoSetVolumeを呼ばない~~ | ~~Z80 REVSW(0)はSTVOLで通常音量復元するがMmlEngineはしない~~ | **クローズ（実装済み確認）** | #53 |
| ~~Rm1 + ^タイ境界のKEY_OFF欠落~~ | ~~Z80 FMSUB3→FS3はRm1時にKEY_OFF、MmlParserは^を単一イベントに展開~~ | **修正済み（TIE_KEYOFFイベント）** | #55 |
| ~~FM (/)ボリュームクランプ~~ | ~~パーサーが(で0にクランプ→Z80はVOLUPFでクランプなし~~ | **修正済み（FM負値許容）** | #57 |
| ~~エコー(\)FMクランプ+ループvolDelta+L無し残留音~~ | ~~STBF3のFMクランプなし、エコーVOLUMEのvolDelta未補正、allDoneゲート~~ | **修正済み** | #58 |
| ~~yXXスロット番号+loadPcmData+per-channelループ~~ | ~~yXX slot 0-based→1-based, chOff欠落, PCM16エントリ制限, 独立ループ欠如~~ | **修正済み** | #59 |
| ~~calcTicks &タイ%絶対tick~~ | ~~calcTicks ^/&ハンドラが%N未対応~~ | **修正済み** | #60 |
| ~~%N(SETDCO)がCOUNT設定ではなくtick直接加算~~ | ~~Z80はCOUNTを設定し次のノート/レストのデフォルト長にする。パーサーはst.tickに加算→二重カウント~~ | **修正済み** | #70 |

---

## オープンIssue一覧（機能差修正）

なし（全てクローズ済み）

---

## Z80全ルーチン網羅的照合表（Issue #81）

2026-04-09実施。music.asm（ランタイム）173ラベル、muc88.asm（コンパイラ）165ラベルの
全338ラベルを確認し、MmlParser/MmlEngineとの対応状況を分類した。

### music.asm ランタイムルーチン

| Z80ルーチン | 機能 | MmlEngine対応 | 状態 |
|---|---|---|---|
| **FM音源** ||||
| FMSUB0-9 | FM メインループ（wait/fetch/keyon/keyoff） | processEvents + fmKeyOn/fmKeyOff | ✅ 実装済み |
| KEYON/KEYON2 | FM KEY_ON (0x28レジスタ) | fmKeyOn() | ✅ 実装済み |
| KEYOFF | FM KEY_OFF (0x28レジスタ) | fmKeyOff() | ✅ 実装済み |
| STENV/ENVLP | FM音色パッチ適用（4OP全パラメータ書込） | fmApplyPatch/fmWritePatch | ✅ 実装済み |
| OTOPST | 音色設定コマンド (@) | doSetPatch() | ✅ 実装済み |
| STVOL/STV1/STV2 | FM音量設定 (FMVDAT参照) | fmSetVolume() + FMVDAT[20] | ✅ 実装済み(#48) |
| VOLPST | vコマンド（音量設定） | VOLUME event | ✅ 実装済み |
| VOLUPF | (/)コマンド（音量増減） | VOLUME event (note=2) | ✅ 修正済み(#57) |
| FMGFQ/FMS92 | FM周波数変換 (ノート→F-Number) | fmWriteFreq() | ✅ 実装済み |
| FPORT | FMポートレジスタ選択 | fmPort()/fmOffset() | ✅ 実装済み |
| EXMODE/EXMLP | FM ch3拡張モード（CSM/SE） | csmKeyOn() + CSM_MODE event | ✅ 実装済み |
| FRQ_DF/FD2 | デチューンコマンド (D) | DETUNE event + updatePitch() | ✅ 実装済み |
| SETQ | qコマンド（スタッカート） | STACCATO event | ✅ 実装済み |
| TIE/FLGSET | タイ(&)コマンド | パーサーがノート結合 | ✅ 実装済み(#44,#55) |
| AKYOFF/AKYOF2 | 全KEY_OFF | allSoundOff() | ✅ 実装済み |
| **SSG音源** ||||
| SSGSUB/SSSUB0-9 | SSGメインループ（wait/fetch/keyon/keyoff） | processEvents + ssgKeyOn/ssgKeyOff | ✅ 実装済み |
| SSSUBF/SSSUBG | SSG KEY_ON | ssgKeyOn() | ✅ 実装済み |
| SKYOFF | SSG KEY_OFF | ssgKeyOff() | ✅ 実装済み |
| SOFENV/SOFEV1-9 | SSGソフトウェアエンベロープ (ADSR) | ssgTickEnvelope() | ✅ 実装済み(#17,#29) |
| OTOSSG/OTOCAL | SSGプリセット (@N + SSGDAT) | ssgApplyPreset() | ✅ 実装済み(#43) |
| VOLUPS/PSGVOL | SSG音量設定 | ssgSetVolume() | ✅ 実装済み |
| NOISE/NOISEW | SSGノイズ周波数 (w) | REG_WRITE event (mixer) | ✅ 実装済み |
| ENVPST | SSGミキサーモード (P) | PAN event + SSG mixer | ✅ 実装済み |
| SSGOFF/SSGOF1 | SSG全消音 | allSoundOff() | ✅ 実装済み |
| SSSUB3/SETPT | SSGトーン周波数設定 | ssgWriteFreq() | ✅ 実装済み |
| **ソフトウェアLFO** ||||
| PLLFO/CTLFO | LFOメインループ（delay/rate/depth制御） | tickLfo() | ✅ 実装済み |
| PLS2/PLSKI2 | LFO→FM/SSGピッチ適用 | updatePitch() | ✅ 実装済み |
| LFOP3-6 | FM LFO周波数更新 | fmUpdateFreq() | ✅ 実装済み |
| SSLFO2 | SSG LFO周波数更新 | ssgUpdateFreq() | ✅ 実装済み |
| LFORST | LFO波形リセット | tickLfo() 初期化 | ✅ 実装済み |
| HLFOON | ハードウェアLFO設定 (H) | HARDWARE_LFO event | ✅ 実装済み |
| LFOON/LFOON2/LFOON3 | ソフトLFO設定 (M) | VIBRATO/VIBRATO_SWITCH/LFO_PARAM | ✅ 実装済み |
| SETDEL/SETCO/SETVCT/SETPEK | LFO個別パラメータ (MW/MC/ML/MD) | LFO_PARAM event | ✅ 実装済み(#37,#38) |
| **リバーブ** ||||
| FS2 | FMリバーブ音量 (FMVDAT直接参照) | fmSetReverbVolume() | ✅ 修正済み(#48,#49) |
| REVERVE | リバーブ値設定 (R) | REVERB_ENVELOPE event | ✅ 実装済み |
| REVSW | リバーブスイッチ (RF) | REVERB_SWITCH event | ✅ 実装済み(#52,#53) |
| REVMOD | リバーブモード (Rm) | REVERB_MODE event | ✅ 実装済み |
| **ADPCM** ||||
| PCMGFQ/ASUB7 | ADPCM-B周波数計算 (PCMNMB+shift) | adpcmbNoteToDeltaN() | ✅ 実装済み |
| PLAY/PL1/PL2 | ADPCM-B再生 (TOTALV*4+vol, clamp 250) | adpcmbKeyOn()/adpcmbSetVolume() | ✅ 実装済み |
| PCMOUT | ADPCM-Bレジスタ出力 | writeReg(1, ...) | ✅ 実装済み |
| PCMVOL/PCMV2 | ADPCM-B音量設定 | adpcmbSetVolume() | ✅ 実装済み |
| PVMCHG | ADPCM-B音量モード切替 | m_pcmVolMode | ✅ 実装済み |
| OTOPCM | ADPCM-B音色設定 (@) + PCMADRテーブル | doSetPatch(ch10) | ✅ 実装済み |
| PCMEND | ADPCM-B終端処理 | END event | ✅ 実装済み |
| **リズム(ADPCM-A)** ||||
| DKEYON | リズムKEY_ON (0x10レジスタ) | rhythmKeyOn() | ✅ 実装済み |
| DKEYOF | リズムKEY_OFF (Dump) | rhythmKeyOff() | ✅ 実装済み |
| DRMFQ | リズム楽器選択 | rhythmKeyOn() 内 | ✅ 実装済み |
| VOLDRM/DVOLSET | リズム音量設定 | rhythmSetVolume() | ✅ 実装済み |
| OTODRM | リズム音色設定 | doSetPatch(rhythm) | ✅ 実装済み |
| **パン** ||||
| STEREO/STER2 | FM/SSGパン設定 | doSetPan(FM) | ✅ 実装済み |
| STE2 | ADPCM-A パン(IL bit6-7) | doSetPan(rhythm) + m_rhythmIL | ✅ 実装済み |
| STEREO(PCM) | ADPCM-Bパン (PCMLR) | doSetPan(ADPCM-B) | ✅ 修正済み(#72) |
| **テンポ・タイマー** ||||
| STTMB/STTMB2 | Timer-B設定 | recalcTimerB() | ✅ 実装済み |
| **ループ** ||||
| REPSTF | ブラケットループ開始 (runtime 0xF5) | パーサー静的展開 | ✅ アーキテクチャ差（結果同一） |
| REPENF/REPENF2 | ブラケットループ終了 (runtime 0xF6) | パーサー静的展開 | ✅ アーキテクチャ差（結果同一） |
| RSKIP/RSKIP2 | /ブレーク (runtime 0xFE) | パーサー静的展開 | ✅ アーキテクチャ差（結果同一） |
| FMINIT/SSINIT | ループ再開 (DATA TOP) | per-channel restart | ✅ 修正済み(#62,#68,#71) |
| FMEND/SSGEND | チャンネル終端 (endマーカー) | END event | ✅ 実装済み |
| **フェードアウト** ||||
| FDOUT/FDO2/FDOFM/FDOSSG | フェードアウト (TOTALV漸増) | m_globalAtt (外部制御) | ✅ 実装済み |
| **初期化** ||||
| MSTART/START | 再生開始 | play() | ✅ 実装済み |
| MSTOP | 再生停止 | stop() | ✅ 実装済み |
| INT57/INT573 | 割込初期化 | init() | ✅ 実装済み |
| MONO/MONO2-5 | モノラル初期化 | init() 内 | ✅ 実装済み |
| WORKINIT/WI1-6 | ワーク初期化 | ChannelState初期化 | ✅ 実装済み |
| **再現不要** ||||
| ESC_PRC | ESCキー処理 | — | N/A（PC-8801 UI操作） |
| CUE | CUE同期命令 | — | N/A（ハードウェア同期） |
| CHK/STT1/STT2 | サウンドボード判別 | — | N/A（ハードウェア検出） |
| PSGOUT/PSGO4 | レジスタI/O出力 | — | N/A（writeReg()で代替） |
| W_REG | yコマンドI/O出力 | writeReg() | ✅ REG_WRITE経由 |
| TIME/PTIME/TSC | 再生時間カウンタ | — | N/A（UI表示用） |
| WKGET/PUTWK | ワークエリアアクセス | — | N/A（Z80メモリ管理） |

### muc88.asm コンパイラコマンド

| Z80コマンド | MML文字 | MmlParser対応 | 状態 |
|---|---|---|---|
| SETVOL/STV1-4 | v | parseChannelMML 'v' | ✅ 実装済み |
| SETVN/SETVN1-8 | @ | parseChannelMML '@' | ✅ 実装済み(#42: @"string"含む) |
| SETOCT/STO2 | o | parseChannelMML 'o' | ✅ 実装済み |
| SETOUP/SOU1 | > | parseChannelMML '>' | ✅ 実装済み |
| SETODW/SOD1 | < | parseChannelMML '<' | ✅ 実装済み |
| SETVUP/SVU2 | ) | parseChannelMML ')' | ✅ 実装済み |
| SETVDW/SVD2 | ( | parseChannelMML '(' | ✅ 実装済み |
| SETQLG | q | parseChannelMML 'q' | ✅ 実装済み |
| SETTMP/STTMP2-3 | T | parseTrack 'T' (BPM→Timer-B) | ✅ 実装済み |
| TIMERB/TIMEB2 | t | parseTrack 't' (Timer-B直値) | ✅ 実装済み |
| SETDT/STD2 | D/d | parseTrack 'D' | ✅ 実装済み |
| SETLR | p | parseChannelMML 'p' | ✅ 実装済み |
| SETRV/SRV2-4 | R/RF/Rm | parseTrack 'R' | ✅ 実装済み |
| SETHLF | H | parseTrack 'H' | ✅ 実装済み |
| SETWAV/SETMO1-2 | M/MF/MW/MC/ML/MD | parseChannelMML 'm' | ✅ 実装済み |
| SETSE/SETSE1 | S | parseTrack 'S' (CSM) | ✅ 実装済み |
| SETBEF/STBF3 | \ | echo処理 | ✅ 修正済み(#45,#58,#64) |
| SETTIE/SETTI1-2 | & | タイ結合 | ✅ 実装済み |
| SETJMP | L | LOOP_POINT event | ✅ 実装済み |
| SETLPS/SETLPS2 | [ | ループ開始 | ✅ 実装済み |
| SETLPE/SETLPE1-4 | ] | ループ終了 | ✅ 実装済み(#51,#54) |
| SETRJP | / | ブレークポイント | ✅ 実装済み |
| TOTALV/TV_OFS | V | tvOffset | ✅ 修正済み(#47) |
| SETKON | 連打モード | — | N/A（v1.5互換、通常MUCでは使用しない） |
| SETKST/SETKS2 | K | KEY_TRANSPOSE event | ✅ 実装済み |
| SETCLK | C | wholeTick設定 | ✅ 実装済み |
| SETDCO | % | defLen直接クロック設定 | ✅ 修正済み(#70) |
| SETREG/SETR1-9 | y | REG_WRITE event | ✅ 修正済み(#59) |
| SETLIZ/SETLI1-2 | l | defLen設定 | ✅ 実装済み |
| SETEV/STEV2 | E(SSG envelope) | SSG_ENVELOPE event | ✅ 実装済み |
| NOISE(コンパイラ) | w | noise REG_WRITE | ✅ 実装済み |
| SETCOL/STCL2-8 | チャンネル種別判定 | parseTrack チャンネル判定 | ✅ 実装済み |
| SETMAC/ENDMAC | *N{} マクロ | collectMacros/expandMacros | ✅ 実装済み |
| PVMCHG(コンパイラ) | V1(PVMODE切替) | VOLUME event note=1 | ✅ 実装済み |
| FCOMP17 | 長音符127tick分割 | ^セグメント自動分割 | ✅ 実装済み(#55) |

### 照合結果サマリー

- **music.asm 173ラベル**: 全ルーチン確認完了。音声出力に影響する全機能が実装済み
- **muc88.asm 165ラベル**: 全MMLコマンド確認完了。全コマンドがMmlParserに実装済み
- **未実装機能**: なし
- **再現不要**: ESC_PRC, CUE, CHK, TIME/PTIME/TSC, WKGET/PUTWK（ハードウェア/UI/メモリ管理）
- **アーキテクチャ差**: ブラケットループ（Z80: ランタイム展開 / MmlParser: 静的展開）→ 結果は同一
- **regtest確認**: 132曲 Mean=1.011, Median=1.001, >=0.8=132(100%)
- **2026-06-16 senior-architect 層A再検証**: 上記「全照合完了/未実装なし」は楽観的だった。Z80 asm↔C++ の敵対的再照合で**実差異16件**を検出し libmucom88#61-70 で修正(echo履歴/テンポ整数変換/SSG LFO octave縮小/ポルタメントoctave跨ぎ・SSG SNUMB/音量オーバーフロー巻戻し/K-k transpose分離/SSG R-RR共有/ADPCM音量・PCMテーブル/LFO MLリセット)。端ケースの多くは regtest非カバーで緑は傍証(手動 muc_compare 検証推奨)。

---

## 修正履歴

| 日付 | Issue | 内容 |
|------|-------|------|
| 2026-06-16 | libmucom88 #61-70 | senior-architect 層A静的検証(並列Opus+敵対的検証)で Z80非再現 実差異16件を検出・修正。#64 ポルタメントは同一octaveでも Z80 CULC 反復ratio乗算により ±1 LSB差(退行でなくZ80忠実化)。別途 dosburger 間欠非決定性(pre-existing/未初期化メモリ疑い)を mucom88v#256 に起票 |
| 2026-04-09 | #72 | ADPCM-B pコマンド（パン設定）のdoSetPan()対応。Z80 STEREOルーチンのPCMLR設定と互換。m_pcmPan = panToReg(pan)追加 |
| 2026-04-08 | #71 | per-channel初回移行時の累積ドリフト修正。イベント消費済みチャンネルのnextRestartTick未設定+perChTickBase 1tickズレ。iw_digicharat-partynight 600sec: avgRMS 1.012→1.002, keyOn差 226→1 |
| 2026-04-08 | #70 | %N(SETDCO)をZ80互換COUNT設定に修正。st.tick直接加算→defLen/defLenIsClock設定。directTicksパスの^/&ドット処理追加。レストハンドラのdefLenIsClockチェック追加。stk023: 5.031→1.019。残存600tick差はZ80コンパイラバグ（SETLPE breakTick==0誤判定、T_CLKのみ影響、ランタイム再生は正常）。regtest Mean: 0.999, Max: 1.078 |
| 2026-04-08 | #65 | Z80グローバル変数全体調査完了。BFDAT/VDDAT(#64で修正済み)以外にチャンネル間引き継ぎ変数なし。VOLUME初期値(Z80=0,Parser=12)とCOUNT初期値(Z80=24,Parser=4)の差はあるが全MUCファイルでv/lコマンドが先にあるため実影響ゼロ |
| 2026-04-08 | #68 | Z80互換per-channel独立ループ（MaxCountパディング不在の発見。各チャンネルがendTickで即時リスタート。gg204: 0.817→0.993, gg209: 0.768→0.971）|
| 2026-04-08 | #64 | \\=N,Mエコーパラメータをグローバル変数に（Z80 BFDAT/VDDAT互換、bare09 1.258→1.000）|
| 2026-04-08 | #62 | per-channel独立ループ実装（初回globalLoopRestart後、各チャンネルが独立周期でループ）+ muc_compare per-part override修正 |
| 2026-04-08 | #60 | calcTicks ^/&ハンドラ %N絶対tick対応 + per-channelループ撤回→globalLoopRestart統一(Z80 MaxCountパディング互換) |
| 2026-04-08 | #59 | yXXコマンド slot 1-based/chOff修正 + loadPcmData 32エントリ + per-channelループ(Z80 DATA TOP独立ジャンプ)→#60で撤回 |
| 2026-04-08 | #58 | エコー(\)FMクランプ除去 + ブラケットループ内エコーvolDelta補正(note=3マーカー) + L無し曲残留音修正(allDoneゲート) |
| 2026-04-07 | #57 | FM (/)ボリュームクランプ除去（Z80 VOLUPFはクランプなし、FM負値許容）|
| 2026-04-07 | #55 | ^タイ境界TIE_KEYOFF実装（Z80 FMSUB3互換: Rm1→KEYOFF, Rm0→FS2）+ Z80自動分割(127tick上限) |
| 2026-04-07 | #44再修正 | FM NOTE_ON常時KEY_OFF実行（Z80 FMSUB5互換: 同音スキップ削除） |
| 2026-04-07 | #54 | ブラケットループ内v+(/）累積修正（vコマンドありのループでvolDelta=0）|
| 2026-04-07 | #51 | ブラケットループ内(/)ボリューム累積修正（volDelta補正追加）|
| 2026-04-07 | #49 | FM reverb FS2: IX+6の+4補正追加（SETVOL加算分）|
| 2026-04-07 | #18 | ADPCM-B PVMODE+4: STV4→STV1パスは+4加算なしと判明、修正不要でクローズ |
| 2026-04-07 | #45 | \\エコー(SETBEF)のtick計算: staccato適用後duration→フル音長(BEFCO互換) |
| 2026-04-07 | #48 | FM reverb FS2: FMVDAT直接参照(STV2経由, TOTALV加算なし) + 定数TL書き込み |
| 2026-04-06 | #47 | V(TV_OFS) Total Volume Offset実装（SSG/FM vコマンドにオフセット加算） |
| 2026-04-06 | #44 | FMタイ判定修正（異キーコードタイ時KEY_OFFスキップ、Z80 FMSUB4互換） |
| 2026-04-06 | #37,#38 | ML/MC/MW/MDは実装済みと確認、クローズ |
| 2026-04-06 | #42 | @"string" 文字列音色名のvoice.dat検索を実装 |
| 2026-04-06 | #43 | SSGプリセット(@N)のLFO(M)/ミキサー(P)パラメータ適用を追加 |
| 2026-04-06 | #44-#46 | 比較表精査に基づく未解決差異のIssue化 |
| 2026-04-04 | #39 | 音源エンジン統一(MIDI/MUCプレビュー同一FM音源) |
| 2026-04-04 | #41 | OpnaEngine.hヘッダー分割リファクタリング |
| 2026-04-03 | #31 | ^タイ演算子のデフォルト音長修正 |
| 2026-04-03 | #30 | commonEndTick境界KEY_OFF(SSG残留音防止) |
| 2026-04-03 | #29 | SSG SOFENV位相修正(KEY_ON tickスキップ) |
| 2026-04-03 | #16 | FMVDATオフセット修正、commonEndTick打ち切り |
| 2026-04-03 | #17 | SSGソフトウェアエンベロープZ80完全互換 |
| 2026-04-02 | #7-#15 | MMLパーサーWikiリファレンス準拠全修正 |

---

*最終更新: 2026-06-16 (senior-architect 層A検証, libmucom88#61-70 修正)*
*更新者: Claude (Anthropic) + takamori-tech*

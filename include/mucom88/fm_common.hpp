// =============================================================================
// fm_common.hpp
// YM2608 (OPNA) / YM2151 (OPM) 共通定義
//
// CLAUDIUS (ゲーム), MUCOM88V (VST), mucom88ex (CLIツール) で共有。
// =============================================================================

#pragma once

#include <cstdint>
#include <cmath>
#include <algorithm>
#include <string>

// =============================================================================
// FmPatch: MUCOM88形式の音色定義
//
// MUCOM88のパラメーター順: AR,DR,SR,RR,SL,TL,KS,ML,DT
// fmgenへの書き込み順:      DT,ML,TL,KS,AR,DR,SR,SL,RR
// =============================================================================
// 音色の出典情報
enum class PatchSource : uint8_t {
    Unknown  = 0,   // 出典不明（初期値）
    VoiceDat = 1,   // voice.dat / voiceopm.dat（INT, ROM1 デフォルト）
    Inline   = 2,   // MUCインライン定義 @N={...}（MML, ROM2）
    Edited   = 3,   // Voice Editorで編集済み（RAM）
    UserBank = 4,   // ユーザー保存バンク（mucom88v #173, #177）
    External = 5,   // MUC companion voice.dat（EXT, mucom88v #180）
};

struct FmPatch {
    int  patchNo   = 0;   // @番号
    int  fb        = 0;   // Feedback (0-7)
    int  al        = 0;   // Algorithm (0-7)

    struct Op {
        int ar  = 31;  // Attack Rate  (0-31)
        int dr  = 0;   // Decay Rate   (0-31)
        int sr  = 0;   // Sustain Rate (0-31)
        int rr  = 7;   // Release Rate (0-15)
        int sl  = 0;   // Sustain Level(0-15)
        int tl  = 0;   // Total Level  (0-127, lower=louder)
        int ks  = 0;   // Key Scale    (0-3)
        int ml  = 1;   // Multiple     (0-15)
        int dt  = 0;   // Detune       (0-7, OPN系DT)
        int dt2 = 0;   // Detune2      (0-3, OPM固有 — 非整数倍音的金属的響き)
        int ame = 0;   // AM Enable    (0-1, LFOによる振幅変調を受けるか)
        int ssgEg = 0; // SSG-EG       (0-15, 0=OFF, 8-15=各パターン, OPN/OPNA固有)
    } op[4];

    // OPM拡張パラメータ（voice.dat バイト25-27、後方互換）
    bool isOpm = false;   // true=OPM音色, false=OPN音色
    int  pms   = 0;       // Phase Modulation Sensitivity (0-7, OPM LFO)
    int  ams   = 0;       // Amplitude Modulation Sensitivity (0-3, OPM LFO)

    std::string name;           // 音色名（voice.dat: 6文字、MUCインライン: ,"name"）

    PatchSource source = PatchSource::Unknown;  // 出典情報（RAM/ROM1/ROM2判別用）
    bool valid = false;
};

// 後方互換エイリアス（ゲーム側で使用）
using Mucom88Patch = FmPatch;

// =============================================================================
// FM F-Number 計算（MIDI note → YM2608 F-Number + Block）
//
// MUCOM88 FNUMB テーブル（Z80プレイヤー music.asm より）を使用。
// MUCOM88のオクターブo1-o8 = YM2608 block 0-7。
// MIDI note 番号からの変換:
//   key = noteNum % 12 (0=C, 1=C#, ..., 11=B)
//   block = noteNum / 12 - 1 (MIDI octave 0 = block -1, MIDI octave 1 = block 0)
// =============================================================================
inline uint16_t noteToFnum(int noteNum, int& blockOut)
{
    // MUCOM88 FNUMB テーブル: C, C#, D, D#, E, F, F#, G, G#, A, A#, B
    static const uint16_t fnumb[12] = {
        0x26A, 0x28F, 0x2B6, 0x2DF, 0x30B, 0x339,
        0x36A, 0x39E, 0x3D5, 0x410, 0x44E, 0x48F
    };

    int key = noteNum % 12;
    if (key < 0) key += 12;
    // MIDI note 24 = C1 = MUCOM88 o1 = block 0
    // MIDI note 36 = C2 = MUCOM88 o2 = block 1
    // ...
    // MIDI note 60 = C4 = MUCOM88 o4 = block 3
    int block = (noteNum / 12) - 2;
    block = std::clamp(block, 0, 7);

    blockOut = block;
    return fnumb[key];
}

// =============================================================================
// SSG トーンピリオド計算
// YM2608 SSG: freq = master_clock / (64 * TP)
//   → TP = chip_clock / (64 * freq)
// chipClock: YM2608マスタークロック（7987200=NTSC標準）
// Issue #22: クロック定数をハードコードせず引数で受け取る
// =============================================================================
inline uint16_t noteToSSGPeriod(int noteNum, uint32_t chipClock = 7987200)
{
    double freq = 440.0 * std::pow(2.0, (noteNum - 69) / 12.0);
    double divisor = chipClock / 64.0;
    uint16_t tp = (uint16_t)std::round(divisor / freq);
    return std::clamp(tp, (uint16_t)1, (uint16_t)4095);
}

// =============================================================================
// voice.dat パース（1エントリ 32バイト）
// MUCOM88_VOICEFORMAT準拠:
//   byte 0:     hed (ヘッダ、スキップ)
//   bytes 1-4:  DT/ML  (op1, op3, op2, op4 スロット順)
//   bytes 5-8:  TL
//   bytes 9-12: KS/AR
//   bytes 13-16: DR/AM
//   bytes 17-20: SR
//   bytes 21-24: SL/RR
//   byte 25:    FB(bit5-3) / AL(bit2-0)
//   bytes 26-31: 音色名(6文字)
// =============================================================================
inline FmPatch parseVoiceDatEntry(const uint8_t* voiceDat, size_t dataSize, int patchNo)
{
    FmPatch p;
    size_t off = (size_t)patchNo * 32;
    if (off + 32 > dataSize) return p;
    const uint8_t* rec = &voiceDat[off];

    // byte 0 はヘッダ（スキップ）。パラメータはbyte 1から。
    // スロット順: +0(op1), +1(op3), +2(op2), +3(op4)
    // → MUCOM88 op順(op1,op2,op3,op4)に変換: [0,2,1,3]
    static const int slotMap[4] = { 0, 2, 1, 3 };

    p.fb = (rec[25] >> 3) & 7;
    p.al =  rec[25] & 7;
    p.patchNo = patchNo;

    for (int oi = 0; oi < 4; oi++) {
        int s = slotMap[oi];
        auto& op = p.op[oi];
        uint8_t dt_ml = rec[1 + s];    // byte 1-4
        op.dt = (dt_ml >> 4) & 7;
        op.ml = dt_ml & 0x0F;
        op.tl = rec[5 + s] & 0x7F;     // byte 5-8
        uint8_t ks_ar = rec[9 + s];    // byte 9-12
        op.ks = (ks_ar >> 6) & 3;
        op.ar = ks_ar & 0x1F;
        op.ame = (rec[13 + s] >> 7) & 1; // byte 13-16 bit7: AM Enable
        op.dr = rec[13 + s] & 0x1F;    // byte 13-16 bit4-0: DR
        op.sr = rec[17 + s] & 0x1F;    // byte 17-20
        uint8_t sl_rr = rec[21 + s];   // byte 21-24
        op.sl = (sl_rr >> 4) & 0x0F;
        op.rr = sl_rr & 0x0F;
    }
    // OPM拡張フィールド（voice.dat バイト25-27、後方互換）
    // バイト25: OPMフラグ (0x00=OPN, 0x01=OPM)
    // バイト26: DT2パック (op1:bit1-0, op2:bit3-2, op3:bit5-4, op4:bit7-6)
    // バイト27: PMS(bit6-4) / AMS(bit1-0)
    // 注: バイト25はFB/ALと共用。OPNモードではFB/AL、OPMモードではフラグ。
    // OPM判定は音色名領域（byte 26-31）のパターンで行う。
    // 現時点ではisOpm=falseのまま（将来のG2モード用に予約）。

    // 音色名（byte 26-31, 6バイト）
    // MUCOM88ではShift_JIS半角カタカナ(0xA0-0xDF)等も使用されるためバイト列をそのまま保持
    // 末尾の0x00と0x20を除去
    for (int j = 0; j < 6; j++) {
        p.name += (char)rec[26 + j];
    }
    while (!p.name.empty() && (p.name.back() == ' ' || p.name.back() == '\0'))
        p.name.pop_back();

    p.source = PatchSource::VoiceDat;
    p.valid = true;
    return p;
}

// =============================================================================
// voice.dat バイト列への書き戻し（parseVoiceDatEntry の逆変換）
// 32バイト/エントリのMUCOM88互換形式で書き出す。
// 注: SSG-EG はMUCOM88 voice.dat 32バイト形式に含まれないため失われる。
// =============================================================================
inline void writeVoiceDatEntry(uint8_t* rec, const FmPatch& p)
{
    static const int slotMap[4] = { 0, 2, 1, 3 };

    rec[0] = 0;
    rec[25] = (uint8_t)(((p.fb & 7) << 3) | (p.al & 7));

    for (int oi = 0; oi < 4; oi++) {
        int s = slotMap[oi];
        const auto& op = p.op[oi];
        rec[1 + s]  = (uint8_t)(((op.dt & 7) << 4) | (op.ml & 0x0F));
        rec[5 + s]  = (uint8_t)(op.tl & 0x7F);
        rec[9 + s]  = (uint8_t)(((op.ks & 3) << 6) | (op.ar & 0x1F));
        rec[13 + s] = (uint8_t)(((op.ame & 1) << 7) | (op.dr & 0x1F));
        rec[17 + s] = (uint8_t)(op.sr & 0x1F);
        rec[21 + s] = (uint8_t)(((op.sl & 0x0F) << 4) | (op.rr & 0x0F));
    }

    // OPM拡張バイト（byte 25-27）
    // バイト25はFB/ALと共用のため、OPMフラグは音色名領域で判別。
    // ここではdt2/pms/amsを書き込む（OPN音色では全0）。
    // バイト26: DT2パック (op1:bit1-0, op2:bit3-2, op3:bit5-4, op4:bit7-6)
    // ただしバイト26-31は音色名と共用のため、OPMモード時のみ書き込む
    // （現時点ではisOpm=falseが前提、将来のG2モード用に予約）

    // 音色名 (byte 26-31, 6バイト固定)
    for (int j = 0; j < 6; j++) {
        char c = (j < (int)p.name.size()) ? p.name[j] : ' ';
        rec[26 + j] = (uint8_t)c;
    }
}

// =============================================================================
// MUCOM88 FMVDAT テーブル（Z80 music.asm:2700、全20エントリ）
// STV1(通常音量): index = TOTALV(=4) + vol → kFmVdat[index]
// FS2(リバーブ):  index = (vol+4+R) >> 1  → kFmVdat[index]
// =============================================================================
static constexpr int kFmVdat[20] = {
    0x36, 0x33, 0x30, 0x2D,  // [0-3]（STV1では通常使わない、FS2で使用）
    0x2A, 0x28, 0x25, 0x22,  // [4-7]  = vol 0-3
    0x20, 0x1D, 0x1A, 0x18,  // [8-11] = vol 4-7
    0x15, 0x12, 0x10, 0x0D,  // [12-15] = vol 8-11
    0x0A, 0x08, 0x05, 0x02   // [16-19] = vol 12-15
};

// MUCOM88 STV1互換: FMVDAT[TOTALV + vol]（通常の音量設定）
inline int fmvdatLookup(int vol15)
{
    return kFmVdat[std::clamp(vol15 + 4, 0, 19)];
}

// MUCOM88 FS2→STV2互換: FMVDAT[(vol+4+R) >> 1]（リバーブ用）
inline int fmReverbTL(int vol15, int R)
{
    return kFmVdat[std::clamp((vol15 + 4 + R) >> 1, 0, 19)];
}

// =============================================================================
// SSG プリセット（MUCOM88 Z80 music.asm:2162 SSGDAT互換、@0-@15）
// =============================================================================
struct SsgPresetParams {
    int env[6];     // {AL, AR, DR, SL, SR, RR}
    int mixerP;     // 0=トーンのみ, 1=トーンのみ(MUCOM88互換=1), 2=ノイズのみ, 3=トーン+ノイズ
    bool hasLfo;
    int lfoDelay, lfoRate, lfoDepth, lfoCount;
};

static constexpr SsgPresetParams kSsgPresets[16] = {
    {{255,255,255,255,  0,255}, 1, false, 0,0,0,0},       // @0: 持続
    {{255,255,255,200,  0, 10}, 1, false, 0,0,0,0},       // @1: 標準サステイン
    {{255,255,255,200,  1, 10}, 1, false, 0,0,0,0},       // @2: サステインレート1
    {{255,255,255,190,  0, 10}, 1, true,  16,1,25,4},     // @3: LFO付き
    {{255,255,255,190,  1, 10}, 1, true,  16,1,25,4},     // @4: LFO付き(SR=1)
    {{255,255,255,170,  0, 10}, 1, false, 0,0,0,0},       // @5: 速いリリース
    {{ 40, 70, 14,190,  0, 15}, 1, true,  16,1,24,5},     // @6: スローアタック+LFO
    {{120, 30,255,255,  0, 10}, 1, true,  16,1,25,4},     // @7: ベル風+LFO
    {{255,255,255,225,  8, 15}, 1, false, 0,0,0,0},       // @8: SR=8
    {{255,255,255,  1,255,255}, 2, false, 0,0,0,0},       // @9: ノイズ
    {{255,255,255,200,  8,255}, 2, false, 0,0,0,0},       // @10: ノイズ(SR=8)
    {{255,255,255,220, 20,  8}, 1, true,  1,1,300,-1},    // @11: 減衰ビブラート
    {{255,255,255,255,  0, 10}, 1, true,  1,1,-400,4},    // @12: ピッチダウン
    {{255,255,255,255,  0, 10}, 1, true,  1,1,80,-1},     // @13: ゆるいビブラート
    {{120, 80,255,255,  0,255}, 1, true,  1,1,-250,1},    // @14: 揺れ
    {{255,255,255,220,  0,255}, 1, true,  1,1,3000,-1},   // @15: 激しいビブラート
};

// =============================================================================
// YM2608 オペレータースロットオフセット
// op1=+0, op2=+8, op3=+4, op4=+12
// =============================================================================
static constexpr int kFmSlotOffset[4] = { 0, 8, 4, 12 };

// =============================================================================
// アルゴリズムごとのキャリアオペレータ判定
// =============================================================================
static constexpr bool kFmCarrier[8][4] = {
    {false, false, false, true },  // AL0: op4
    {false, false, false, true },  // AL1: op4
    {false, false, false, true },  // AL2: op4
    {false, false, false, true },  // AL3: op4
    {false, true,  false, true },  // AL4: op2, op4
    {false, true,  true,  true },  // AL5: op2, op3, op4
    {false, true,  true,  true },  // AL6: op2, op3, op4
    {true,  true,  true,  true },  // AL7: all
};

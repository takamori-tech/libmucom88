// SPDX-License-Identifier: MIT
// =============================================================================
// chip_backend_interface.hpp
// チップバックエンド抽象の共有インターフェース定義
//
// Phase3 で段階的にチップバックエンド抽象を追加していく。
// 現時点では ChipMode / ChannelMaskSpec を共有定義する。
//
// エミュレータ実装に依存しない公開ヘッダとして、fmgen 等は include しない。
// =============================================================================

#pragma once

#include <cstdint>
#include <memory>
#include <cstddef>

// =============================================================================
// ChipMode: 対象チップ種別
//
// 値は mucom88v の chipModeToIndex() と m_modeFiles[] のインデックスとして使うため、
// OPNA=0 / OPM=1 / OPNB=2 の順序を変更してはいけない。
// mml_parser.hpp の nested ChipMode と対応。OPN は末尾維持、変換は named-value のみで行い、
// static_cast による数値変換は禁止。
// OPN(YM2203) は FM ch1-3 + SSG ch1-3 のみ可聴。FM4-6/ADPCM-A(rhythm)/ADPCM-B/port1 は OPN では縮退し、
// loadRhythmRom/loadAdpcmBData/loadRhythmSample は既存の no-op/false 既定を継承する。
// =============================================================================
enum class ChipMode {
    OPNA = 0,   // YM2608
    OPM  = 1,   // YM2151拡張
    OPNB = 2,   // YM2610拡張
    OPN  = 3,   // YM2203（FM3ch + SSG3ch、ADPCM/rhythm/port1 なし）
};

// =============================================================================
// ChipProfile: チップ互換情報
//
// OPN の 3'993'600Hz は PC-88 OSC3/8 の真値。VGM 由来再生で 4'000'000Hz が
// 与えられる場合は setChipClock() で注入する（ヘッダ丸め、約 +2.77cent）。
// OPM/OPNB の値は #299 まで消費者なしの暫定値で inert。
// =============================================================================
struct ChipProfile {
    uint32_t defaultClock;
    int      numFmChannels;
    int      numSsgChannels;
    bool     hasRhythm;
    bool     hasAdpcmB;
};

[[nodiscard]] constexpr ChipProfile chipModeProfile(ChipMode mode) noexcept
{
    switch (mode) {
    case ChipMode::OPNA: return { 7987200u, 6, 3, true,  true  };
    case ChipMode::OPM:  return { 7987200u, 8, 0, false, false };
    case ChipMode::OPNB: return { 7987200u, 6, 3, true,  true  };
    case ChipMode::OPN:  return { 3993600u, 3, 3, false, false };
    }
    return chipModeProfile(ChipMode::OPNA);
}

static_assert(chipModeProfile(ChipMode::OPNA).defaultClock == 7987200u,
              "OPNA default clock must remain 7987200Hz");
static_assert(chipModeProfile(ChipMode::OPN).defaultClock == 3993600u &&
              chipModeProfile(ChipMode::OPN).numFmChannels == 3,
              "OPN profile must match YM2203 hardware limits");

// FM エンジン種別。mucom88v FmEngineType と値順一致。段2 で FmEngineType を
// 本 enum の alias へ縮退させ、independent enum を残さない。
enum class ChipEngine { Fmgen = 0, Ymfm = 1 };

// =============================================================================
// ChannelMaskSpec: チャンネル可聴指定 (audible=1 セマンティクス)
//
// fmgen 等の raw mask は「bit=1 で mute」かつ FM/SSG/ADPCM-B/rhythm の bit 配置が
// チップ固有。本 POD は論理チャンネル名で「可聴(true)」を指定し、チップ固有の
// mute polarity / bit pack への変換は利用側 backend (mucom88v 側 FmgenBackend) に
// 封じ込める。本ヘッダには fmgen 等チップ依存を一切漏らさない。
//
// 重要: デフォルト構築 (= allAudible()) は全 true (全可聴) でなければならない。
// 全 false (全 mute) を既定にすると disable 経路 (全可聴へ戻す) が全消音事故に
// なるため、必ず全 true を既定とする。C++17 集成体性は NSDMI でも保持される
// (P0017R1)。
// OPN(YM2203) では fm[0..2] と ssg[0..2] のみ有効。fm[3..5]/adpcmB/rhythm[*] は無視 (don't-care)。
// bit 配置は OPNA と互換のため lowering 関数は無改修で正しく作用する。
// =============================================================================
struct ChannelMaskSpec {
    bool fm[6]     { true, true, true, true, true, true };  // FM1..6 可聴
    bool ssg[3]    { true, true, true };                    // SSG1..3 可聴
    bool adpcmB    { true };                                // ADPCM-B 可聴
    bool rhythm[6] { true, true, true, true, true, true };  // ADPCM-A(rhythm)1..6 可聴

    // 全チャンネル可聴 (= デフォルト構築)。disable / 非パラレル経路で使用する。
    static constexpr ChannelMaskSpec allAudible() noexcept { return {}; }
};

struct ChipStemFrame {
    int32_t main[2] {};
    int32_t fm[6][2] {};
    int32_t ssg[3][2] {};
    int32_t rhythm[2] {};
    int32_t adpcmB[2] {};
};

// =============================================================================
// IChipBackend: 単一チップバックエンド抽象
//
// 純粋仮想インターフェース。fmgen 等の具体実装は利用側 (mucom88v FmgenBackend) が
// 提供し、本ヘッダにはチップ依存を一切漏らさない。生成関数 (createFmgenBackend 等) は
// エミュレータ依存のため利用側が宣言・実装する。
// =============================================================================
class IChipBackend {
public:
    virtual ~IChipBackend() = default;

    virtual void init(ChipMode mode, uint32_t hostSampleRate) = 0;
    virtual void reset() noexcept = 0;
    virtual bool hasChip() const noexcept = 0;

    virtual void writeReg(int port, uint8_t addr, uint8_t data) noexcept = 0;
    // interleavedLR は frameCount*2 要素の L/R インターリーブバッファを指す。
    // fmgen の Mix は加算合成のため、呼び出し前に必ずゼロ初期化すること。
    // frameCount>1 を呼ぶ場合も要素数・ゼロ初期化・位相連続性を保つ。
    virtual void mixChunk(int32_t* interleavedLR, uint32_t frameCount) noexcept = 0;
    [[nodiscard]] virtual bool mixStemChunk(ChipStemFrame* frames, uint32_t frameCount) noexcept
    {
        (void)frames;
        (void)frameCount;
        return false;
    }
    virtual void setSsgBalanceLinear(float ratio) noexcept = 0;
    virtual void setChannelMask(const ChannelMaskSpec& spec) noexcept = 0;

    // 渡した PCM バッファの所有権は実装側 (fmgen) が取るため、呼び出し側では delete しない。
    virtual void loadRhythmSample(int idx,
                                  const int16_t* pcm,
                                  uint32_t numSamples,
                                  uint32_t rate) = 0;
    // ADPCM-B RAM へ PCM データをロードする。容量超過分は実装側でクランプする。
    // data==nullptr / バッファ未確保なら何もせず false を返す。
    [[nodiscard]] virtual bool loadAdpcmBData(const uint8_t* data, std::size_t size) noexcept = 0;

    // 生 ADPCM-A(rhythm) ROM(標準 0x2000B)を内部読出方式 backend(ymfm 等)が参照できるよう取り込む。
    // 所有権は移譲せず実装側が内部コピーする(rom は呼び出し側が保持)。rom==nullptr / size==0 は何もしない。
    // decoded-PCM 経路の backend(fmgen 等)は loadRhythmSample で rhythm を扱うため override 不要(default no-op)。
    virtual void loadRhythmRom(const uint8_t* rom, std::size_t size) noexcept { (void)rom; (void)size; }

    // 後段 YM3016 DAC モデル(roundtrip_fp companding requantizer)の有効/無効。
    // 内部読出 backend(ymfm)のみ override。decoded 経路(fmgen 等)は no-op 既定継承=無改変。
    // RT 安全: 設定は audio パス外。実際の roundtrip 適用は利用側 backend の native-rate mix 内。
    virtual void setDacModel(bool enabled) noexcept { (void)enabled; }

    // ymfm リサンプリング忠実度(0=MED / 1=MAX 既定)。内部リサンプル backend(ymfm)のみ override。
    // fmgen 等は no-op 継承=無変更。fidelity 変更は native rate を変えるため backend 再init が前提。
    virtual void setFidelity(int fidelity) noexcept { (void)fidelity; }

    // 互換出力段。Native では無効、Tuned では既存の fmgen/OpenMUCOM88 向け補正を有効化する。
    // fmgen 等は no-op 継承=無変更。内部 mix backend(ymfm)のみ output gain/limiter を切り替える。
    virtual void setCompatibilityOutput(bool enabled) noexcept { (void)enabled; }

    // SSG セクション音量トリム。内部加算 backend(ymfm)のみ override し、decoded 経路(fmgen)は
    // no-op 継承=SSG 既存経路不変。reg 域ではなく method-domain の線形乗算に限定する。
    // RT 安全: 設定は audio パス外。gain==1.0f で実質無作用。
    virtual void setSectionGainSsg(float gain) noexcept { (void)gain; }

    // ADPCM-A/B セクション音量トリム。ymfm backend の register 境界でのみ適用し、
    // decoded 経路(fmgen 等)は no-op 継承で既存経路を保持する。
    // RT 安全: 設定は audio パス外。gain==1.0f で実質無作用。
    virtual void setSectionGainAdpcmA(float gain) noexcept { (void)gain; }
    virtual void setSectionGainAdpcmB(float gain) noexcept { (void)gain; }

    // チップ動作クロック(Hz)を注入する。未設定時は chipModeProfile() の既定値へフォールバックする。
    // OPN(YM2203) の正本既定は 3'993'600Hz。VGM ヘッダ由来の 4'000'000Hz 丸め値を
    // 反映する場合は setChipClock() で注入する。クロックの実反映点は各 backend の init() 内 Init/SetRate のみのため、
    // 必ず init() の前に呼ぶこと（init 後の呼び出しは次回 init まで効かない）。
    // RT 安全: 設定は audio パス外。fmgen 等 OPNA 専用 backend は no-op 継承で無変更。
    virtual void setChipClock(uint32_t hz) noexcept { (void)hz; }
};

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
// =============================================================================
enum class ChipMode {
    OPNA = 0,   // YM2608
    OPM  = 1,   // YM2151拡張
    OPNB = 2,   // YM2610拡張
};

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
// =============================================================================
struct ChannelMaskSpec {
    bool fm[6]     { true, true, true, true, true, true };  // FM1..6 可聴
    bool ssg[3]    { true, true, true };                    // SSG1..3 可聴
    bool adpcmB    { true };                                // ADPCM-B 可聴
    bool rhythm[6] { true, true, true, true, true, true };  // ADPCM-A(rhythm)1..6 可聴

    // 全チャンネル可聴 (= デフォルト構築)。disable / 非パラレル経路で使用する。
    static constexpr ChannelMaskSpec allAudible() noexcept { return {}; }
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
};

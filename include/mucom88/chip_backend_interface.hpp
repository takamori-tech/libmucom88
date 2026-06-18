// SPDX-License-Identifier: MIT
// =============================================================================
// chip_backend_interface.hpp
// チップバックエンド抽象の共有インターフェース定義
//
// Phase3 で段階的にチップバックエンド抽象を追加していく。
// 現時点では ChipMode / ChannelMaskSpec を共有定義する。
// IChipBackend クラスは後続段で追加する。
//
// エミュレータ実装に依存しない公開ヘッダとして、fmgen 等は include しない。
// =============================================================================

#pragma once

#include <cstdint>
#include <memory>

// =============================================================================
// ChipMode: 対象チップ種別
//
// 値は mucom88v の chipModeToIndex() と m_modeFiles[] のインデックスとして使うため、
// OPNA=0 / OPM=1 / OPNB=2 の順序を変更してはいけない。
//
// IChipBackend クラスは後続段で追加する。
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

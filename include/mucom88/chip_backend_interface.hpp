// SPDX-License-Identifier: MIT
// =============================================================================
// chip_backend_interface.hpp
// チップバックエンド抽象の共有インターフェース定義
//
// Phase3 で段階的に IChipBackend / ChannelMaskSpec を追加していく。
// 本コミット(段1)では ChipMode のみを定義する。
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
// 段1では ChipMode のみを共有定義する。
// IChipBackend クラスや ChannelMaskSpec は後続段で追加する。
// =============================================================================
enum class ChipMode {
    OPNA = 0,   // YM2608
    OPM  = 1,   // YM2151拡張
    OPNB = 2,   // YM2610拡張
};

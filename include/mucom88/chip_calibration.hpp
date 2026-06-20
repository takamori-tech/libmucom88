// SPDX-License-Identifier: MIT
// =============================================================================
// chip_calibration.hpp
// チップ間音量差を OpenMUCOM88+fmgen 基準へ正規化する L1 較正定数
// =============================================================================
#pragma once

#include "chip_backend_interface.hpp"

// チップ間音量差を OpenMUCOM88+fmgen 基準へ正規化する L1 較正定数。
// 非永続=版でコード追従し、consumer 非関与で既定バランスを保つ。
struct ChipCalibration {
    int   adpcmATlOffset = 0;     // reg0x11(PCMA_VOL) 生値から減算する step。正=減衰。
    float adpcmBGain     = 1.0f;  // reg0x0B(PCMB_VOL) 乗算係数。
    float ssgGain        = 1.0f;  // SSG L1 補正。段1 はデータのみで、適用は段2。
};

inline constexpr ChipCalibration kFmgenCalibration{ 0,  1.0f,   1.0f  };   // identity=bit-exact
inline constexpr ChipCalibration kYmfmCalibration { 12, 1.044f, 0.445f };   // 段0 grounded

// engine→calibration を中心化。default 無しで、新 engine 追加時の未処理検出を残す。
inline constexpr ChipCalibration calibrationFor(ChipEngine e) noexcept {
    switch (e) {
        case ChipEngine::Fmgen: return kFmgenCalibration;
        case ChipEngine::Ymfm:  return kYmfmCalibration;
    }
    return kFmgenCalibration;
}

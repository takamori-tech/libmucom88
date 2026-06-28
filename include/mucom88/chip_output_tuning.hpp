// SPDX-License-Identifier: MIT
// =============================================================================
// chip_output_tuning.hpp
// Engine-specific output-stage defaults shared by libmucom88 consumers.
// =============================================================================
#pragma once

#include "chip_calibration.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>

enum class ChipOutputProfile {
    Native = 0,
    Tuned  = 1,
};

struct ChipOutputTuning {
    // User-visible/L2 SSG balance. Backends with ChipCalibration apply L1 separately.
    float ssgMixScale = 1.0f;
    int rhythmMaster = 63;
    float adpcmAGain = 1.0f;
    float adpcmBGain = 1.0f;
    float outputGain = 1.0f;

    // Backend-local compatibility stage. Currently meaningful for ymfm.
    bool compatibilityOutput = false;
    float compatibilityOutputGain = 1.0f;
    bool outputLimiter = false;
};

inline constexpr float kFmgenTunedSsgMixScale = 0.70794576f;       // -3.0 dB
inline constexpr float kYmfmTunedSsgMixScale = 0.63095737f;        // -4.0 dB
inline constexpr float kYmfmTunedHostOutputGain = 1.33352143f;     // +2.5 dB
inline constexpr float kYmfmTunedCompatibilityOutputGain = 1.9f;

[[nodiscard]] inline constexpr ChipOutputProfile defaultChipOutputProfile() noexcept
{
    return ChipOutputProfile::Tuned;
}

[[nodiscard]] inline constexpr ChipOutputTuning
chipOutputTuningFor(ChipEngine engine, ChipOutputProfile profile) noexcept
{
    if (profile == ChipOutputProfile::Native)
        return {};

    switch (engine) {
    case ChipEngine::Fmgen:
        return { kFmgenTunedSsgMixScale, 63, 1.0f, 1.0f, 1.0f, false, 1.0f, false };
    case ChipEngine::Ymfm:
        return { kYmfmTunedSsgMixScale, 63, 1.0f, 1.0f, kYmfmTunedHostOutputGain,
                 true, kYmfmTunedCompatibilityOutputGain, true };
    }
    return {};
}

[[nodiscard]] inline constexpr ChipOutputTuning
defaultChipOutputTuningFor(ChipEngine engine) noexcept
{
    return chipOutputTuningFor(engine, defaultChipOutputProfile());
}

// IFmEngine implementations do not necessarily use NormalizingChipBackend, so
// game-side defaults need the L1 calibration folded into the SSG scale.
[[nodiscard]] inline constexpr float
effectiveSsgMixScaleFor(ChipEngine engine, ChipOutputProfile profile) noexcept
{
    const ChipOutputTuning tuning = chipOutputTuningFor(engine, profile);
    return profile == ChipOutputProfile::Native
        ? tuning.ssgMixScale
        : calibrationFor(engine).ssgGain * tuning.ssgMixScale;
}

[[nodiscard]] inline int64_t softLimit16(double x) noexcept
{
    constexpr double fullScale = 32767.0;
    constexpr double threshold = 0.75 * fullScale;
    const double magnitude = std::abs(x);
    double y = x;
    if (magnitude > threshold) {
        const double sign = (x < 0.0) ? -1.0 : 1.0;
        y = sign * (threshold + (fullScale - threshold)
            * std::tanh((magnitude - threshold) / (fullScale - threshold)));
    }
    return static_cast<int64_t>(std::llround(std::clamp(y, -fullScale, fullScale)));
}

[[nodiscard]] inline int16_t applyCompatibilityOutput16(double sample,
                                                        const ChipOutputTuning& tuning) noexcept
{
    double v = sample;
    if (tuning.compatibilityOutput) {
        v *= tuning.compatibilityOutputGain;
        if (tuning.outputLimiter)
            return static_cast<int16_t>(softLimit16(v));
    }
    return static_cast<int16_t>(std::clamp(static_cast<int64_t>(std::llround(v)),
                                          static_cast<int64_t>(-32768),
                                          static_cast<int64_t>(32767)));
}

// SPDX-License-Identifier: MIT
// =============================================================================
// logical_stem_mixer.hpp
// backend stem を論理 stem 単位で 64-bit double 合算する opt-in helper
// =============================================================================
#pragma once

#include "chip_backend_interface.hpp"

#include <cstdint>

struct LogicalStemMixOptions {
    // 既定 false。既存利用者はこの helper を明示呼び出ししない限り従来経路のまま。
    bool enableDoubleStemSumming = false;
    double outputScale = 1.0 / 32768.0;
    double masterGain = 1.0;
};

struct LogicalStemFloatFrame {
    float main[2] {};
    float fm[6][2] {};
    float ssg[3][2] {};
    float rhythm[2] {};
    float adpcmB[2] {};
    float fallback[2] {};
};

struct LogicalStemAccumulator {
    double fm[6][2] {};
    double ssg[3][2] {};
    double rhythm[2] {};
    double adpcmB[2] {};
    double fallback[2] {};

    void clear() noexcept
    {
        for (auto& ch : fm) {
            ch[0] = 0.0;
            ch[1] = 0.0;
        }
        for (auto& ch : ssg) {
            ch[0] = 0.0;
            ch[1] = 0.0;
        }
        rhythm[0] = 0.0;
        rhythm[1] = 0.0;
        adpcmB[0] = 0.0;
        adpcmB[1] = 0.0;
        fallback[0] = 0.0;
        fallback[1] = 0.0;
    }

    void addStem(const ChipStemFrame& stem, double gain = 1.0) noexcept
    {
        for (int ch = 0; ch < 6; ++ch) {
            fm[ch][0] += static_cast<double>(stem.fm[ch][0]) * gain;
            fm[ch][1] += static_cast<double>(stem.fm[ch][1]) * gain;
        }
        for (int ch = 0; ch < 3; ++ch) {
            ssg[ch][0] += static_cast<double>(stem.ssg[ch][0]) * gain;
            ssg[ch][1] += static_cast<double>(stem.ssg[ch][1]) * gain;
        }
        rhythm[0] += static_cast<double>(stem.rhythm[0]) * gain;
        rhythm[1] += static_cast<double>(stem.rhythm[1]) * gain;
        adpcmB[0] += static_cast<double>(stem.adpcmB[0]) * gain;
        adpcmB[1] += static_cast<double>(stem.adpcmB[1]) * gain;
    }

    void addFallbackStereo(int32_t left, int32_t right, double gain = 1.0) noexcept
    {
        fallback[0] += static_cast<double>(left) * gain;
        fallback[1] += static_cast<double>(right) * gain;
    }

    [[nodiscard]] double left() const noexcept
    {
        double v = fallback[0] + rhythm[0] + adpcmB[0];
        for (const auto& ch : fm)
            v += ch[0];
        for (const auto& ch : ssg)
            v += ch[0];
        return v;
    }

    [[nodiscard]] double right() const noexcept
    {
        double v = fallback[1] + rhythm[1] + adpcmB[1];
        for (const auto& ch : fm)
            v += ch[1];
        for (const auto& ch : ssg)
            v += ch[1];
        return v;
    }
};

[[nodiscard]] inline float logicalStemToFloat(double sample, const LogicalStemMixOptions& options) noexcept
{
    return static_cast<float>(sample * options.outputScale * options.masterGain);
}

inline void clearLogicalStemFloatFrame(LogicalStemFloatFrame& frame) noexcept
{
    frame = {};
}

[[nodiscard]] inline bool writeLogicalStemFloatFrame(const LogicalStemAccumulator& acc,
                                                     const LogicalStemMixOptions& options,
                                                     LogicalStemFloatFrame& out) noexcept
{
    if (!options.enableDoubleStemSumming)
        return false;

    out.main[0] = logicalStemToFloat(acc.left(), options);
    out.main[1] = logicalStemToFloat(acc.right(), options);

    for (int ch = 0; ch < 6; ++ch) {
        out.fm[ch][0] = logicalStemToFloat(acc.fm[ch][0], options);
        out.fm[ch][1] = logicalStemToFloat(acc.fm[ch][1], options);
    }
    for (int ch = 0; ch < 3; ++ch) {
        out.ssg[ch][0] = logicalStemToFloat(acc.ssg[ch][0], options);
        out.ssg[ch][1] = logicalStemToFloat(acc.ssg[ch][1], options);
    }
    out.rhythm[0] = logicalStemToFloat(acc.rhythm[0], options);
    out.rhythm[1] = logicalStemToFloat(acc.rhythm[1], options);
    out.adpcmB[0] = logicalStemToFloat(acc.adpcmB[0], options);
    out.adpcmB[1] = logicalStemToFloat(acc.adpcmB[1], options);
    out.fallback[0] = logicalStemToFloat(acc.fallback[0], options);
    out.fallback[1] = logicalStemToFloat(acc.fallback[1], options);
    return true;
}

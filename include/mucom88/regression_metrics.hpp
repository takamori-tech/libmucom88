// SPDX-License-Identifier: MIT
// =============================================================================
// regression_metrics.hpp
// MUC A/B・回帰テストで共有するPCM比較メトリクス。
//
// OpenMUCOM88 参照出力と consumer 側エンジン出力の比較に使う小さな
// header-only 基盤。エミュレータ実装や OpenMUCOM88 には依存しないため、
// mucom88v / CLAUDIUS などの consumer が同じ判定ロジックを再利用できる。
// =============================================================================

#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

namespace mucom88 {

struct RegressionThresholds {
    int sampleThreshold = 100;
    int sustainedWindowFrames = 2205;  // 50ms @ 44.1kHz
    int minLoudFrames = 64;
};

struct RmsSummary {
    std::size_t count = 0;
    double mean = 0.0;
    double median = 0.0;
    double min = 0.0;
    double max = 0.0;
    std::size_t greaterEqual08 = 0;
    std::size_t greaterEqual06 = 0;
};

[[nodiscard]] inline double calcInterleavedStereoRms(const int16_t* interleavedStereo,
                                                     int frames) noexcept
{
    if (interleavedStereo == nullptr || frames <= 0)
        return 0.0;

    double sum = 0.0;
    const int samples = frames * 2;
    for (int i = 0; i < samples; i++) {
        const double s = static_cast<double>(interleavedStereo[i]);
        sum += s * s;
    }
    return std::sqrt(sum / static_cast<double>(samples));
}

[[nodiscard]] inline int firstSustainedSoundFrame(const int16_t* interleavedStereo,
                                                  int64_t totalFrames,
                                                  RegressionThresholds thresholds = {}) noexcept
{
    if (interleavedStereo == nullptr || totalFrames <= 0)
        return 0;

    const int sampleThreshold = std::max(thresholds.sampleThreshold, 0);
    const int64_t windowFrames = std::max(thresholds.sustainedWindowFrames, 1);
    const int minLoudFrames = std::max(thresholds.minLoudFrames, 1);

    for (int64_t i = 0; i < totalFrames; i++) {
        const auto left = static_cast<int>(interleavedStereo[i * 2]);
        const auto right = static_cast<int>(interleavedStereo[i * 2 + 1]);
        if (std::abs(left) <= sampleThreshold && std::abs(right) <= sampleThreshold)
            continue;

        const int64_t end = std::min(i + windowFrames, totalFrames);
        int loud = 0;
        for (int64_t k = i; k < end; k++) {
            const auto l = static_cast<int>(interleavedStereo[k * 2]);
            const auto r = static_cast<int>(interleavedStereo[k * 2 + 1]);
            if (std::abs(l) > sampleThreshold || std::abs(r) > sampleThreshold)
                loud++;
        }

        if (loud >= minLoudFrames)
            return static_cast<int>(std::min<int64_t>(i, std::numeric_limits<int>::max()));

        i = end - 1;
    }
    return 0;
}

[[nodiscard]] inline double averageAlignedRmsRatio(const int16_t* reference,
                                                   const int16_t* target,
                                                   int64_t totalFrames,
                                                   int sampleRate,
                                                   double seconds,
                                                   RegressionThresholds thresholds = {}) noexcept
{
    if (reference == nullptr || target == nullptr || totalFrames <= 0 || sampleRate <= 0 || seconds <= 1.0)
        return 0.0;

    const int refStart = firstSustainedSoundFrame(reference, totalFrames, thresholds);
    const int targetStart = firstSustainedSoundFrame(target, totalFrames, thresholds);

    double sum = 0.0;
    int count = 0;
    const int wholeSeconds = static_cast<int>(seconds);
    for (int s = 0; s < wholeSeconds - 1; s++) {
        const int refOffset = refStart + s * sampleRate;
        const int targetOffset = targetStart + s * sampleRate;
        if (refOffset < 0 || targetOffset < 0)
            break;

        const int refLength = std::min(sampleRate, static_cast<int>(totalFrames) - refOffset);
        const int targetLength = std::min(sampleRate, static_cast<int>(totalFrames) - targetOffset);
        if (refLength <= 0 || targetLength <= 0)
            break;

        const double refRms = calcInterleavedStereoRms(reference + refOffset * 2, refLength);
        const double targetRms = calcInterleavedStereoRms(target + targetOffset * 2, targetLength);
        sum += (refRms > 1.0) ? targetRms / refRms : 1.0;
        count++;
    }
    return (count > 0) ? sum / static_cast<double>(count) : 0.0;
}

[[nodiscard]] inline RmsSummary summarizeRmsRatios(std::vector<double> ratios)
{
    RmsSummary summary;
    summary.count = ratios.size();
    if (ratios.empty())
        return summary;

    std::sort(ratios.begin(), ratios.end());
    double sum = 0.0;
    for (double ratio : ratios) {
        sum += ratio;
        if (ratio >= 0.8)
            summary.greaterEqual08++;
        if (ratio >= 0.6)
            summary.greaterEqual06++;
    }

    summary.mean = sum / static_cast<double>(ratios.size());
    summary.median = ratios[ratios.size() / 2U];
    summary.min = ratios.front();
    summary.max = ratios.back();
    return summary;
}

}  // namespace mucom88

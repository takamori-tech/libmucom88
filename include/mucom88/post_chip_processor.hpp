// SPDX-License-Identifier: MIT
// =============================================================================
// post_chip_processor.hpp
// YM2608 post-chip flavor processor shared by libmucom88 consumers.
// =============================================================================
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

enum class PostChipStage {
    DacModel = 0,
    BoardLpf,
    Saturation,
    NoiseFloor,
    Cabinet,
};

struct PostChipConfig {
    // Stage toggles. Default: cabinet/noise off, other flavor stages on.
    bool dacModel = true;
    bool antiAlias = true;      // Legacy public name for the board analog LPF.
    bool saturation = true;
    bool noiseFloor = false;
    bool cabinet = false;

    double sampleRate = 0.0;
    int dacBits = 15;           // Host-facing control retained for A/B presets.
    double lpfCutoffHz = 18000.0;
    double saturationDrive = 1.05;
    double noiseLevelDbfs = -90.0;
    double cabinetAmount = 0.0; // 0..1 blend amount.
};

class PostChipProcessor {
public:
    void prepare(double sampleRate) noexcept
    {
        m_config.sampleRate = sanitizeSampleRate(sampleRate);
        updateDerivedState();
        reset();
    }

    void reset() noexcept
    {
        m_boardLpfL.reset();
        m_boardLpfR.reset();
        for (auto& f : m_cabinetL)
            f.reset();
        for (auto& f : m_cabinetR)
            f.reset();
        m_noiseState = kInitialNoiseState;
    }

    void setConfig(const PostChipConfig& config) noexcept
    {
        const double currentSampleRate = m_config.sampleRate;
        m_config = sanitizeConfig(config);
        if (m_config.sampleRate <= 0.0)
            m_config.sampleRate = currentSampleRate > 0.0 ? currentSampleRate : kDefaultSampleRate;
        updateDerivedState();
    }

    [[nodiscard]] PostChipConfig config() const noexcept { return m_config; }

    void setStageEnabled(PostChipStage stage, bool enabled) noexcept
    {
        switch (stage) {
        case PostChipStage::DacModel: m_config.dacModel = enabled; break;
        case PostChipStage::BoardLpf: m_config.antiAlias = enabled; break;
        case PostChipStage::Saturation: m_config.saturation = enabled; break;
        case PostChipStage::NoiseFloor: m_config.noiseFloor = enabled; break;
        case PostChipStage::Cabinet: m_config.cabinet = enabled; break;
        }
    }

    [[nodiscard]] bool isStageEnabled(PostChipStage stage) const noexcept
    {
        switch (stage) {
        case PostChipStage::DacModel: return m_config.dacModel;
        case PostChipStage::BoardLpf: return m_config.antiAlias;
        case PostChipStage::Saturation: return m_config.saturation;
        case PostChipStage::NoiseFloor: return m_config.noiseFloor;
        case PostChipStage::Cabinet: return m_config.cabinet;
        }
        return false;
    }

    [[nodiscard]] bool bypassed() const noexcept
    {
        return !m_config.dacModel
            && !m_config.antiAlias
            && !m_config.saturation
            && !m_config.noiseFloor
            && !m_config.cabinet;
    }

    void processBlock(float* left, float* right, int numSamples) noexcept
    {
        if (left == nullptr || right == nullptr || numSamples <= 0 || bypassed())
            return;

        for (int i = 0; i < numSamples; ++i) {
            processStereoSample(left[i], right[i]);
        }
    }

    [[nodiscard]] float processSample(float x) noexcept
    {
        if (bypassed())
            return x;

        double y = static_cast<double>(x);
        processMono(y);
        return static_cast<float>(std::clamp(y, -1.0, 1.0));
    }

private:
    class Biquad {
    public:
        void reset() noexcept
        {
            z1 = 0.0;
            z2 = 0.0;
        }

        void setIdentity() noexcept
        {
            b0 = 1.0;
            b1 = 0.0;
            b2 = 0.0;
            a1 = 0.0;
            a2 = 0.0;
        }

        void setLowPass(double sampleRate, double cutoffHz, double q) noexcept
        {
            const double sr = sanitizeSampleRate(sampleRate);
            const double cutoff = std::clamp(cutoffHz, 10.0, sr * 0.45);
            const double omega = kTwoPi * cutoff / sr;
            const double sinOmega = std::sin(omega);
            const double cosOmega = std::cos(omega);
            const double alpha = sinOmega / (2.0 * std::max(q, 0.001));
            const double a0 = 1.0 + alpha;

            b0 = ((1.0 - cosOmega) * 0.5) / a0;
            b1 = (1.0 - cosOmega) / a0;
            b2 = ((1.0 - cosOmega) * 0.5) / a0;
            a1 = (-2.0 * cosOmega) / a0;
            a2 = (1.0 - alpha) / a0;
        }

        void setHighPass(double sampleRate, double cutoffHz, double q) noexcept
        {
            const double sr = sanitizeSampleRate(sampleRate);
            const double cutoff = std::clamp(cutoffHz, 10.0, sr * 0.45);
            const double omega = kTwoPi * cutoff / sr;
            const double sinOmega = std::sin(omega);
            const double cosOmega = std::cos(omega);
            const double alpha = sinOmega / (2.0 * std::max(q, 0.001));
            const double a0 = 1.0 + alpha;

            b0 = ((1.0 + cosOmega) * 0.5) / a0;
            b1 = -(1.0 + cosOmega) / a0;
            b2 = ((1.0 + cosOmega) * 0.5) / a0;
            a1 = (-2.0 * cosOmega) / a0;
            a2 = (1.0 - alpha) / a0;
        }

        void setPeaking(double sampleRate, double centerHz, double q, double gainDb) noexcept
        {
            const double sr = sanitizeSampleRate(sampleRate);
            const double center = std::clamp(centerHz, 10.0, sr * 0.45);
            const double omega = kTwoPi * center / sr;
            const double sinOmega = std::sin(omega);
            const double cosOmega = std::cos(omega);
            const double amp = std::pow(10.0, gainDb / 40.0);
            const double alpha = sinOmega / (2.0 * std::max(q, 0.001));
            const double a0 = 1.0 + alpha / amp;

            b0 = (1.0 + alpha * amp) / a0;
            b1 = (-2.0 * cosOmega) / a0;
            b2 = (1.0 - alpha * amp) / a0;
            a1 = (-2.0 * cosOmega) / a0;
            a2 = (1.0 - alpha / amp) / a0;
        }

        [[nodiscard]] double process(double x) noexcept
        {
            const double y = b0 * x + z1;
            z1 = b1 * x - a1 * y + z2;
            z2 = b2 * x - a2 * y;
            return squashDenormal(y);
        }

    private:
        double b0 = 1.0;
        double b1 = 0.0;
        double b2 = 0.0;
        double a1 = 0.0;
        double a2 = 0.0;
        double z1 = 0.0;
        double z2 = 0.0;
    };

    static constexpr double kDefaultSampleRate = 48000.0;
    static constexpr double kTwoPi = 6.283185307179586476925286766559;
    static constexpr double kButterworthQ = 0.7071067811865475244;
    static constexpr uint32_t kInitialNoiseState = 0x4d55434fu; // "MUCO"

    static double sanitizeSampleRate(double sampleRate) noexcept
    {
        return std::clamp(sampleRate, 8000.0, 384000.0);
    }

    static double squashDenormal(double x) noexcept
    {
        return std::abs(x) < 1.0e-24 ? 0.0 : x;
    }

    static PostChipConfig sanitizeConfig(PostChipConfig config) noexcept
    {
        config.sampleRate = config.sampleRate > 0.0 ? sanitizeSampleRate(config.sampleRate) : 0.0;
        config.dacBits = std::clamp(config.dacBits, 14, 16);
        config.lpfCutoffHz = std::clamp(config.lpfCutoffHz, 12000.0, 22000.0);
        config.saturationDrive = std::clamp(config.saturationDrive, 1.0, 2.0);
        config.noiseLevelDbfs = std::clamp(config.noiseLevelDbfs, -120.0, -70.0);
        config.cabinetAmount = std::clamp(config.cabinetAmount, 0.0, 1.0);
        return config;
    }

    static double softClip(double x, double drive) noexcept
    {
        if (drive <= 1.0)
            return std::clamp(x, -1.0, 1.0);
        return std::tanh(x * drive) / std::tanh(drive);
    }

    static double ym3016RoundTrip(double x, int hostBits) noexcept
    {
        const double clamped = std::clamp(x, -1.0, 1.0);
        const double sign = clamped < 0.0 ? -1.0 : 1.0;
        const double magnitude = std::abs(clamped);
        if (magnitude <= 0.0)
            return 0.0;

        // YM3016 is a floating-point DAC family. Approximate the missing
        // companding grid by quantizing to a 10-bit mantissa inside one of
        // seven binary exponent bands, then retain the legacy DAC-bits control
        // as a final host-facing grid trim.
        int exponent = 1;
        double scaled = magnitude * 2.0;
        while (scaled < 0.5 && exponent < 7) {
            scaled *= 2.0;
            ++exponent;
        }

        constexpr double mantissaSteps = 1023.0;
        const double mantissa = std::round(std::clamp(scaled, 0.0, 1.0) * mantissaSteps) / mantissaSteps;
        double y = std::ldexp(mantissa, -exponent);

        const double linearSteps = static_cast<double>((1 << (hostBits - 1)) - 1);
        y = std::round(y * linearSteps) / linearSteps;
        return std::clamp(sign * y, -1.0, 1.0);
    }

    [[nodiscard]] double nextNoise() noexcept
    {
        uint32_t x = m_noiseState;
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        m_noiseState = x == 0 ? kInitialNoiseState : x;
        const double unit = static_cast<double>(m_noiseState) / static_cast<double>(UINT32_MAX);
        return unit * 2.0 - 1.0;
    }

    void updateDerivedState() noexcept
    {
        m_config = sanitizeConfig(m_config);
        const double sr = m_config.sampleRate > 0.0 ? m_config.sampleRate : kDefaultSampleRate;
        m_boardLpfL.setLowPass(sr, m_config.lpfCutoffHz, kButterworthQ);
        m_boardLpfR.setLowPass(sr, m_config.lpfCutoffHz, kButterworthQ);

        m_cabinetL[0].setHighPass(sr, 350.0, kButterworthQ);
        m_cabinetR[0].setHighPass(sr, 350.0, kButterworthQ);
        m_cabinetL[1].setPeaking(sr, 1100.0, 0.9, 4.0);
        m_cabinetR[1].setPeaking(sr, 1100.0, 0.9, 4.0);
        m_cabinetL[2].setPeaking(sr, 3200.0, 0.9, -2.0);
        m_cabinetR[2].setPeaking(sr, 3200.0, 0.9, -2.0);
        m_cabinetL[3].setLowPass(sr, 7000.0, kButterworthQ);
        m_cabinetR[3].setLowPass(sr, 7000.0, kButterworthQ);
        m_noiseGain = std::pow(10.0, m_config.noiseLevelDbfs / 20.0);
    }

    void processMono(double& y) noexcept
    {
        if (m_config.dacModel)
            y = ym3016RoundTrip(y, m_config.dacBits);
        if (m_config.saturation)
            y = softClip(y, m_config.saturationDrive);
        if (m_config.noiseFloor)
            y += nextNoise() * m_noiseGain;
        y = std::clamp(y, -1.0, 1.0);
    }

    double processCabinet(double y, Biquad (&filters)[4]) noexcept
    {
        double wet = y;
        for (auto& f : filters)
            wet = f.process(wet);
        wet = softClip(wet, 1.2);
        const double amount = m_config.cabinetAmount;
        return y * (1.0 - amount) + wet * amount;
    }

    void processStereoSample(float& left, float& right) noexcept
    {
        double l = static_cast<double>(left);
        double r = static_cast<double>(right);

        if (m_config.dacModel) {
            l = ym3016RoundTrip(l, m_config.dacBits);
            r = ym3016RoundTrip(r, m_config.dacBits);
        }
        if (m_config.antiAlias) {
            l = m_boardLpfL.process(l);
            r = m_boardLpfR.process(r);
        }
        if (m_config.saturation) {
            l = softClip(l, m_config.saturationDrive);
            r = softClip(r, m_config.saturationDrive);
        }
        if (m_config.noiseFloor) {
            l += nextNoise() * m_noiseGain;
            r += nextNoise() * m_noiseGain;
        }
        if (m_config.cabinet && m_config.cabinetAmount > 0.0) {
            l = processCabinet(l, m_cabinetL);
            r = processCabinet(r, m_cabinetR);
        }

        left = static_cast<float>(std::clamp(l, -1.0, 1.0));
        right = static_cast<float>(std::clamp(r, -1.0, 1.0));
    }

    PostChipConfig m_config {};
    Biquad m_boardLpfL {};
    Biquad m_boardLpfR {};
    Biquad m_cabinetL[4] {};
    Biquad m_cabinetR[4] {};
    uint32_t m_noiseState = kInitialNoiseState;
    double m_noiseGain = 0.0;
};

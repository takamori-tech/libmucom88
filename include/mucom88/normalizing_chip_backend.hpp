// SPDX-License-Identifier: MIT
// =============================================================================
// normalizing_chip_backend.hpp
// チップ差分の L1 較正を writeReg 境界で適用する IChipBackend decorator
// =============================================================================
#pragma once

#include "chip_backend_interface.hpp"
#include "chip_calibration.hpp"
#include <cmath>
#include <cstdint>
#include <memory>
#include <utility>

class NormalizingChipBackend final : public IChipBackend {
public:
    NormalizingChipBackend(std::unique_ptr<IChipBackend> inner, const ChipCalibration& cal) noexcept
        : m_inner(std::move(inner)), m_cal(cal) {}

    void init(ChipMode mode, uint32_t hostSampleRate) override {
        m_mode = mode;
        m_inner->init(mode, hostSampleRate);
        // L1 chip 補正 × L2 を inner へ注入。identity(fmgen)では 1.0 注入=override 無し backend で no-op。
        // ChipInstance 再init で inner が作り直されても L2 を復元するため無条件(hasChip ガード禁止)。
        m_inner->setSectionGainSsg(m_cal.ssgGain * m_l2Ssg);
        m_inner->setSectionGainAdpcmA(m_l2AdpcmA);
        m_inner->setSectionGainAdpcmB(m_l2AdpcmB);
    }

    void reset() noexcept override {
        m_inner->reset();
        m_inner->setSectionGainSsg(m_cal.ssgGain * m_l2Ssg);  // 防御的(reset は clobber しないが冪等)
        m_inner->setSectionGainAdpcmA(m_l2AdpcmA);
        m_inner->setSectionGainAdpcmB(m_l2AdpcmB);
    }
    bool hasChip() const noexcept override { return m_inner->hasChip(); }

    void writeReg(int port, uint8_t addr, uint8_t data) noexcept override {
        // OPNA 固定: ADPCM-A total level=port0/0x11, ADPCM-B level=port1/0x0B。
        // OPNB は ADPCM-A が port1 へ移るため、段2+ で OPNB calibration を足す際に分岐する。
        static constexpr uint8_t kRegPcmaVol = 0x11;
        static constexpr uint8_t kRegPcmbVol = 0x0B;

        if (m_cal.adpcmATlOffset != 0 && port == 0 && addr == kRegPcmaVol) {
            const int v = static_cast<int>(data) - m_cal.adpcmATlOffset;
            data = static_cast<uint8_t>(v < 0 ? 0 : (v > 63 ? 63 : v));
        } else if (m_cal.adpcmBGain != 1.0f && port == 1 && addr == kRegPcmbVol) {
            const long v = std::lround(static_cast<float>(data) * m_cal.adpcmBGain);
            data = static_cast<uint8_t>(v < 0 ? 0 : (v > 255 ? 255 : v));
        }
        m_inner->writeReg(port, addr, data);
    }

    void mixChunk(int32_t* interleavedLR, uint32_t frameCount) noexcept override {
        m_inner->mixChunk(interleavedLR, frameCount);
    }

    void setSsgBalanceLinear(float ratio) noexcept override {
        m_inner->setSsgBalanceLinear(ratio);
    }

    void setChannelMask(const ChannelMaskSpec& spec) noexcept override {
        m_inner->setChannelMask(spec);
    }

    void loadRhythmSample(int idx,
                          const int16_t* pcm,
                          uint32_t numSamples,
                          uint32_t rate) override {
        m_inner->loadRhythmSample(idx, pcm, numSamples, rate);
    }

    [[nodiscard]] bool loadAdpcmBData(const uint8_t* data, std::size_t size) noexcept override {
        return m_inner->loadAdpcmBData(data, size);
    }

    void loadRhythmRom(const uint8_t* rom, std::size_t size) noexcept override {
        m_inner->loadRhythmRom(rom, size);
    }

    void setDacModel(bool enabled) noexcept override {
        m_inner->setDacModel(enabled);
    }

    void setFidelity(int fidelity) noexcept override {
        m_inner->setFidelity(fidelity);
    }

    void setSectionGainSsg(float gain) noexcept override {
        m_l2Ssg = gain;
        m_inner->setSectionGainSsg(m_cal.ssgGain * gain);
    }

    void setSectionGainAdpcmA(float gain) noexcept override {
        m_l2AdpcmA = gain;
        m_inner->setSectionGainAdpcmA(gain);
    }

    void setSectionGainAdpcmB(float gain) noexcept override {
        m_l2AdpcmB = gain;
        m_inner->setSectionGainAdpcmB(gain);
    }

private:
    std::unique_ptr<IChipBackend> m_inner;
    const ChipCalibration m_cal;
    ChipMode m_mode = ChipMode::OPNA;
    float m_l2Ssg = 1.0f;   // ユーザー区間ゲイン L2(段2-d で配線、現状 1.0 固定)。init/reset で cal×L2 を再注入するため保持。
    float m_l2AdpcmA = 1.0f; // ADPCM-A ユーザー区間ゲイン L2。cal とは合成せず backend へ pass-through する。
    float m_l2AdpcmB = 1.0f; // ADPCM-B ユーザー区間ゲイン L2。cal とは合成せず backend へ pass-through する。
};

// null inner は decorator を作らず nullptr を返す。
inline std::unique_ptr<IChipBackend> createNormalizingChipBackend(
    std::unique_ptr<IChipBackend> inner, const ChipCalibration& cal) {
    if (!inner) return nullptr;
    return std::make_unique<NormalizingChipBackend>(std::move(inner), cal);
}

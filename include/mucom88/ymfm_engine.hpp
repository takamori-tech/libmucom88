// SPDX-License-Identifier: MIT
// =============================================================================
// ymfm_engine.hpp
// Optional ymfm-backed YM2608 (OPNA) IFmEngine adapter.
//
// This header is intentionally optional: including it requires ymfm headers
// and linking ymfm sources in the consumer project. The rest of libmucom88
// remains header-only with no external dependency.
// =============================================================================

#pragma once

#include "chip_backend_interface.hpp"
#include "chip_output_tuning.hpp"
#include "fm_engine_interface.hpp"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

#include <ymfm.h>
#include <ymfm_opn.h>

class FmEngineYmfm final : public IFmEngine, private ymfm::ymfm_interface {
public:
    static constexpr ChipMode CHIP_MODE = ChipMode::OPNA;
    static constexpr ChipEngine CHIP_ENGINE = ChipEngine::Ymfm;
    static constexpr uint32_t CHIP_CLOCK = chipModeProfile(CHIP_MODE).defaultClock;
    static constexpr int FIDELITY_MED = 0;
    static constexpr int FIDELITY_HIGH = 1;
    static constexpr float DEFAULT_SSG_MIX_SCALE = 1.0f / 3.0f;

    FmEngineYmfm() : m_chip(*this) {}

    [[nodiscard]] ChipEngine chipEngine() const noexcept override { return CHIP_ENGINE; }

    void setDacModel(bool enabled) noexcept { m_dacModelEnabled = enabled; }

    // libmucom88 convention: 0=MED, 1=MAX/high. Values outside the range
    // are clamped to high for chip-faithful default behavior.
    void setFidelity(int fidelity) noexcept
    {
        m_fidelity = (fidelity == FIDELITY_MED) ? FIDELITY_MED : FIDELITY_HIGH;
    }

    [[nodiscard]] bool dacModelEnabled() const noexcept { return m_dacModelEnabled; }
    [[nodiscard]] int fidelity() const noexcept { return m_fidelity; }

    void setCompatibilityOutput(bool enabled) noexcept override
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        m_compatibilityOutput = enabled;
    }

    [[nodiscard]] bool compatibilityOutputEnabled() const noexcept override
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        return m_compatibilityOutput;
    }

    void setSsgMixScale(float ssgScale) noexcept override
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        m_ssgMixScale = ssgScale;
    }

    [[nodiscard]] float getSsgMixScale() const noexcept override
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        return m_ssgMixScale;
    }

    void init(uint32_t sampleRate) override
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        m_sampleRate = sampleRate;
        m_chip.set_fidelity(m_fidelity == FIDELITY_MED ? ymfm::OPN_FIDELITY_MED : ymfm::OPN_FIDELITY_MAX);
        m_chip.reset();
        m_chipRate = m_chip.sample_rate(CHIP_CLOCK);
        m_accumulator = 0;
        m_lastL = 0;
        m_lastR = 0;
        clearKeyDeferStateLocked();

        writeYm2608RegLocked(0, 0x2d, 0x00);  // Prescaler mode 0
        writeYm2608RegLocked(0, 0x29, 0x80);  // FM 6ch mode
        writeYm2608RegLocked(0, 0x07, 0x3F);  // SSG mixer: all disabled
        for (uint8_t i = 0; i < 3; ++i)
            writeYm2608RegLocked(0, static_cast<uint8_t>(0x08 + i), 0x00);
    }

    void writeReg(int port, uint8_t addr, uint8_t data) noexcept override
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        if (port == 1 && m_voiceRemainSamples.load(std::memory_order_acquire) > 0 && addr <= 0x0B)
            return;
        if (port == 0 && addr == 0x28 && deferKeyOnRetriggerLocked(data))
            return;
        writeYm2608RegLocked(port, addr, data);
    }

    void generateInterleaved(int16_t* buf, uint32_t frameCount) noexcept override
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        for (uint32_t i = 0; i < frameCount; ++i) {
            m_accumulator += m_chipRate;
            while (m_accumulator >= m_sampleRate) {
                m_accumulator -= m_sampleRate;
                applyDeferredKeyOnsLocked();
                ymfm::ym2608::output_data output;
                m_chip.generate(&output);

                int32_t fmAdpcmL = output.data[0];
                int32_t fmAdpcmR = output.data[1];
                if (m_dacModelEnabled) {
                    fmAdpcmL = ymfm::roundtrip_fp(fmAdpcmL);
                    fmAdpcmR = ymfm::roundtrip_fp(fmAdpcmR);
                }

                const int32_t ssg = scaleSsg(output.data[2]);
                const ChipOutputTuning tuning = m_compatibilityOutput
                    ? chipOutputTuningFor(CHIP_ENGINE, ChipOutputProfile::Tuned)
                    : ChipOutputTuning {};
                m_lastL = applyCompatibilityOutput16(static_cast<double>(fmAdpcmL) + ssg, tuning);
                m_lastR = applyCompatibilityOutput16(static_cast<double>(fmAdpcmR) + ssg, tuning);
            }

            buf[i * 2] = m_lastL;
            buf[i * 2 + 1] = m_lastR;
        }
    }

    void reset() noexcept override
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        m_chip.reset();
        m_accumulator = 0;
        m_lastL = 0;
        m_lastR = 0;
        clearKeyDeferStateLocked();
    }

    [[nodiscard]] bool loadAdpcmRom(const std::string& path) override
    {
        std::ifstream ifs(path, std::ios::binary | std::ios::ate);
        if (!ifs) return false;
        const auto size = ifs.tellg();
        if (size <= 0) return false;
        std::vector<uint8_t> rom(static_cast<size_t>(size));
        ifs.seekg(0);
        ifs.read(reinterpret_cast<char*>(rom.data()), size);
        return loadAdpcmRomFromMemory(rom.data(), rom.size());
    }

    [[nodiscard]] bool loadAdpcmRomFromMemory(const uint8_t* data, size_t size) override
    {
        if (!data || size < 0x2000) return false;
        std::lock_guard<std::mutex> lk(m_mutex);
        m_adpcmARom.assign(data, data + size);
        m_hasAdpcmRom = true;
        return true;
    }

    [[nodiscard]] bool hasAdpcmRom() const noexcept override { return m_hasAdpcmRom; }

    [[nodiscard]] bool loadPcmDataToAdpcmB(const uint8_t* data, size_t size) override
    {
        if (!data || size == 0) return false;
        std::lock_guard<std::mutex> lk(m_mutex);
        m_bgmPcmData.assign(data, data + size);
        loadAdpcmBRam(m_bgmPcmData);
        return true;
    }

    [[nodiscard]] bool loadVoiceTable(const std::string& path) override
    {
        std::ifstream ifs(path, std::ios::binary | std::ios::ate);
        if (!ifs) return false;
        const auto fileSize = ifs.tellg();
        if (fileSize < 4) return false;
        std::vector<uint8_t> data(static_cast<size_t>(fileSize));
        ifs.seekg(0);
        ifs.read(reinterpret_cast<char*>(data.data()), fileSize);
        return loadVoiceTableFromMemory(data.data(), data.size());
    }

    [[nodiscard]] bool loadVoiceTableFromMemory(const uint8_t* data, size_t dataSize) override
    {
        if (!data || dataSize < 4) return false;

        uint32_t numVoices = 0;
        std::memcpy(&numVoices, data, 4);
        if (numVoices == 0 || numVoices > 64) return false;

        const uint32_t headerSize = 4 + numVoices * 8;
        if (headerSize > dataSize) return false;

        std::lock_guard<std::mutex> lk(m_mutex);
        m_voiceEntries.resize(numVoices);
        for (uint32_t i = 0; i < numVoices; ++i) {
            std::memcpy(&m_voiceEntries[i].offset, data + 4 + i * 8, 4);
            std::memcpy(&m_voiceEntries[i].size, data + 4 + i * 8 + 4, 4);
        }

        m_voiceTableData.assign(data, data + dataSize);
        if (m_bgmPcmData.empty())
            loadAdpcmBRam(m_voiceTableData);
        return true;
    }

    [[nodiscard]] bool hasVoiceTable() const noexcept override { return !m_voiceEntries.empty(); }

    void playVoice(int voiceId, int level = 255) noexcept override
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        if (voiceId < 0 || voiceId >= static_cast<int>(m_voiceEntries.size()))
            return;

        const auto& entry = m_voiceEntries[static_cast<size_t>(voiceId)];
        if (entry.size == 0)
            return;

        const uint32_t adpcmSamples = entry.size * 2;
        m_voiceRemainSamples.store(adpcmSamples * m_sampleRate / 16000, std::memory_order_release);

        loadAdpcmBRam(m_voiceTableData);

        static constexpr int ADDR_SHIFT = 2;
        const uint32_t startAddr = entry.offset >> ADDR_SHIFT;
        const uint32_t endAddr = (entry.offset + entry.size - 1) >> ADDR_SHIFT;

        writeYm2608RegLocked(1, 0x01, 0x00);  // Pan mute
        writeYm2608RegLocked(1, 0x00, 0x01);  // Reset
        writeYm2608RegLocked(1, 0x02, static_cast<uint8_t>(startAddr & 0xFF));
        writeYm2608RegLocked(1, 0x03, static_cast<uint8_t>((startAddr >> 8) & 0xFF));
        writeYm2608RegLocked(1, 0x04, static_cast<uint8_t>(endAddr & 0xFF));
        writeYm2608RegLocked(1, 0x05, static_cast<uint8_t>((endAddr >> 8) & 0xFF));
        writeYm2608RegLocked(1, 0x09, 0xBA);  // delta-N low: 0x49BA ~= 16kHz
        writeYm2608RegLocked(1, 0x0A, 0x49);  // delta-N high
        writeYm2608RegLocked(1, 0x0B, static_cast<uint8_t>(std::clamp(level, 0, 255)));
        writeYm2608RegLocked(1, 0x00, 0xA0);  // Start
        writeYm2608RegLocked(1, 0x01, 0xC0);  // Pan L+R
    }

    void stopVoice() noexcept override
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        resetAdpcmBLocked();
    }

    void stopAdpcmB() noexcept override
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        resetAdpcmBLocked();
    }

    [[nodiscard]] bool isVoicePlaying() const noexcept override
    {
        return m_voiceRemainSamples.load(std::memory_order_acquire) > 0;
    }

    void tickVoiceTimer(uint32_t frameCount) noexcept override
    {
        const uint32_t remain = m_voiceRemainSamples.load(std::memory_order_relaxed);
        if (remain == 0)
            return;

        if (frameCount >= remain) {
            m_voiceRemainSamples.store(0, std::memory_order_release);
            std::lock_guard<std::mutex> lk(m_mutex);
            restoreBgmPcmBufferLocked();
        } else {
            m_voiceRemainSamples.store(remain - frameCount, std::memory_order_release);
        }
    }

    [[nodiscard]] uint32_t chipRate() const noexcept { return m_chipRate; }

private:
    static constexpr size_t ADPCM_B_BUF_SIZE = 0x40000;

    struct VoiceEntry {
        uint32_t offset;
        uint32_t size;
    };

    ymfm::ym2608 m_chip;
    uint32_t m_sampleRate = 44100;
    uint32_t m_chipRate = 998400;
    uint32_t m_accumulator = 0;
    int16_t m_lastL = 0;
    int16_t m_lastR = 0;
    mutable std::mutex m_mutex;
    bool m_hasAdpcmRom = false;
    bool m_dacModelEnabled = true;
    int m_fidelity = FIDELITY_HIGH;
    bool m_compatibilityOutput = false;
    float m_ssgMixScale = DEFAULT_SSG_MIX_SCALE;
    bool m_keySawOff[6] { false, false, false, false, false, false };
    bool m_keyPendingOn[6] { false, false, false, false, false, false };
    uint8_t m_keyPendingData[6] { 0, 0, 0, 0, 0, 0 };
    int m_keyDeferCountdown[6] { 0, 0, 0, 0, 0, 0 };

    std::vector<uint8_t> m_adpcmARom;
    std::vector<uint8_t> m_adpcmBRam;
    std::vector<VoiceEntry> m_voiceEntries;
    std::atomic<uint32_t> m_voiceRemainSamples { 0 };
    std::vector<uint8_t> m_voiceTableData;
    std::vector<uint8_t> m_bgmPcmData;

    static int16_t clamp16(int32_t value) noexcept
    {
        return static_cast<int16_t>(std::clamp(value, -32768, 32767));
    }

    int32_t scaleSsg(int32_t value) const noexcept
    {
        if (m_ssgMixScale == DEFAULT_SSG_MIX_SCALE)
            return value / 3;
        return static_cast<int32_t>(value * m_ssgMixScale);
    }

    void writeYm2608RegLocked(int port, uint8_t addr, uint8_t data) noexcept
    {
        const uint32_t base = (port == 0) ? 0u : 2u;
        m_chip.write(base, addr);
        m_chip.write(base + 1, data);
    }

    static int keyOnChannel(uint8_t data) noexcept
    {
        const int lo = static_cast<int>(data & 0x07);
        if (lo < 3)
            return lo;
        if (lo >= 4 && lo <= 6)
            return lo - 1;
        return -1;
    }

    void clearKeyDeferStateLocked() noexcept
    {
        for (int ch = 0; ch < 6; ++ch) {
            m_keySawOff[ch] = false;
            m_keyPendingOn[ch] = false;
            m_keyPendingData[ch] = 0;
            m_keyDeferCountdown[ch] = 0;
        }
    }

    bool deferKeyOnRetriggerLocked(uint8_t data) noexcept
    {
        const int ch = keyOnChannel(data);
        if (ch < 0)
            return false;

        const uint8_t slots = static_cast<uint8_t>(data & 0xF0);
        if (slots == 0) {
            m_keyPendingOn[ch] = false;
            m_keyDeferCountdown[ch] = 0;
            m_keySawOff[ch] = true;
            return false;
        }

        if (!m_keySawOff[ch])
            return false;

        m_keyPendingOn[ch] = true;
        m_keyPendingData[ch] = data;
        // ymfm samples FM key state once per FMSPO native generates.
        // Defer by FMSPO+1 so same-batch key-off is observed before re-keying.
        m_keyDeferCountdown[ch] = (m_fidelity == FIDELITY_HIGH) ? 19 : 7;
        return true;
    }

    void applyDeferredKeyOnsLocked() noexcept
    {
        for (int ch = 0; ch < 6; ++ch) {
            if (m_keyDeferCountdown[ch] > 0 && --m_keyDeferCountdown[ch] == 0) {
                if (m_keyPendingOn[ch]) {
                    writeYm2608RegLocked(0, 0x28, m_keyPendingData[ch]);
                    m_keyPendingOn[ch] = false;
                    m_keySawOff[ch] = false;
                }
            }
        }
    }

    void loadAdpcmBRam(const std::vector<uint8_t>& data)
    {
        if (m_adpcmBRam.size() != ADPCM_B_BUF_SIZE)
            m_adpcmBRam.resize(ADPCM_B_BUF_SIZE);
        std::fill(m_adpcmBRam.begin(), m_adpcmBRam.end(), 0);
        if (!data.empty())
            std::memcpy(m_adpcmBRam.data(), data.data(), std::min(data.size(), ADPCM_B_BUF_SIZE));
    }

    void restoreBgmPcmBufferLocked()
    {
        if (!m_bgmPcmData.empty())
            loadAdpcmBRam(m_bgmPcmData);
    }

    void resetAdpcmBLocked()
    {
        m_voiceRemainSamples.store(0, std::memory_order_release);
        writeYm2608RegLocked(1, 0x01, 0x00);  // Pan mute
        writeYm2608RegLocked(1, 0x0B, 0x00);  // Level 0
        writeYm2608RegLocked(1, 0x00, 0x01);  // Reset
        restoreBgmPcmBufferLocked();
    }

    uint8_t ymfm_external_read(ymfm::access_class type, uint32_t address) override
    {
        switch (type) {
        case ymfm::ACCESS_ADPCM_A:
            if (address < m_adpcmARom.size())
                return m_adpcmARom[address];
            return 0;
        case ymfm::ACCESS_ADPCM_B:
            if (address < m_adpcmBRam.size())
                return m_adpcmBRam[address];
            return 0;
        default:
            return 0;
        }
    }

    void ymfm_external_write(ymfm::access_class type, uint32_t address, uint8_t data) override
    {
        if (type == ymfm::ACCESS_ADPCM_B && address < m_adpcmBRam.size())
            m_adpcmBRam[address] = data;
    }
};

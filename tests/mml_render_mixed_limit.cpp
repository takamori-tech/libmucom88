// SPDX-License-Identifier: MIT

#include <mucom88/chip_output_tuning.hpp>
#include <mucom88/mml_engine.hpp>

#include <cstdint>
#include <string>

class ConstantEngine final : public IFmEngine {
public:
    explicit ConstantEngine(int16_t sample) noexcept : m_sample(sample) {}

    void init(uint32_t /*sampleRate*/) override {}
    void writeReg(int /*port*/, uint8_t /*addr*/, uint8_t /*data*/) noexcept override {}
    void generateInterleaved(int16_t* buf, uint32_t frameCount) noexcept override
    {
        for (uint32_t i = 0; i < frameCount * 2; ++i)
            buf[i] = m_sample;
    }
    void reset() noexcept override {}

    [[nodiscard]] bool loadAdpcmRom(const std::string& /*path*/) override { return false; }
    [[nodiscard]] bool loadAdpcmRomFromMemory(const uint8_t* /*data*/, size_t /*size*/) override { return false; }
    [[nodiscard]] bool hasAdpcmRom() const noexcept override { return false; }

private:
    int16_t m_sample = 0;
};

int main()
{
    {
        ConstantEngine bgm(20000);
        MmlEngine engine;
        engine.init(&bgm, 44100);
        engine.setOutputGain(2.0f);

        int16_t out[2] = {};
        engine.renderMixed(out, 1);

        const auto expected = static_cast<int16_t>(softLimit16(40000.0));
        if (out[0] != expected || out[1] != expected)
            return 1;
        if (out[0] == 32767 || out[1] == 32767)
            return 2;
    }

    {
        ConstantEngine bgm(24000);
        ConstantEngine se(24000);
        MmlEngine engine;
        engine.init(&bgm, 44100);
        engine.setOutputGain(1.0f);
        engine.setSeMode(MmlEngine::SeMode::Rich, &se);

        int16_t out[2] = {};
        engine.renderMixed(out, 1);

        const auto expected = static_cast<int16_t>(softLimit16(48000.0));
        if (out[0] != expected || out[1] != expected)
            return 3;
        if (out[0] == 32767 || out[1] == 32767)
            return 4;
    }

    return 0;
}

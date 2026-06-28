// SPDX-License-Identifier: MIT

#include <mucom88/chip_calibration.hpp>
#include <mucom88/normalizing_chip_backend.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>

class RecordingBackend final : public IChipBackend {
public:
    void init(ChipMode /*mode*/, uint32_t /*hostSampleRate*/) override {}
    void reset() noexcept override {}
    [[nodiscard]] bool hasChip() const noexcept override { return true; }

    void writeReg(int port, uint8_t addr, uint8_t data) noexcept override
    {
        lastPort = port;
        lastAddr = addr;
        lastData = data;
    }

    void mixChunk(int32_t* /*interleavedLR*/, uint32_t /*frameCount*/) noexcept override {}
    void setSsgBalanceLinear(float /*ratio*/) noexcept override {}
    void setChannelMask(const ChannelMaskSpec& /*spec*/) noexcept override {}
    void loadRhythmSample(int /*idx*/,
                          const int16_t* /*pcm*/,
                          uint32_t /*numSamples*/,
                          uint32_t /*rate*/) override {}
    [[nodiscard]] bool loadAdpcmBData(const uint8_t* /*data*/, std::size_t /*size*/) noexcept override
    {
        return false;
    }

    int lastPort = -1;
    uint8_t lastAddr = 0;
    uint8_t lastData = 0;
};

static bool expectEq(uint8_t actual, uint8_t expected) noexcept
{
    return actual == expected;
}

int main()
{
    if (!expectEq(calibrateOpnaAdpcmRegister(0, 0x11, 63, kFmgenCalibration), 63))
        return 1;
    if (!expectEq(calibrateOpnaAdpcmRegister(1, 0x0B, 200, kFmgenCalibration), 200))
        return 2;

    if (!expectEq(calibrateOpnaAdpcmRegister(0, 0x11, 63, kYmfmCalibration), 51))
        return 3;
    if (!expectEq(calibrateOpnaAdpcmRegister(0, 0x11, 12, kYmfmCalibration), 0))
        return 4;
    if (!expectEq(calibrateOpnaAdpcmRegister(0, 0x11, 8, kYmfmCalibration), 0))
        return 5;
    if (!expectEq(calibrateOpnaAdpcmRegister(0, 0x11, 0x7F, kYmfmCalibration), 63))
        return 6;

    if (!expectEq(calibrateOpnaAdpcmRegister(1, 0x0B, 100, kYmfmCalibration), 104))
        return 7;
    if (!expectEq(calibrateOpnaAdpcmRegister(1, 0x0B, 255, kYmfmCalibration), 255))
        return 8;
    if (!expectEq(calibrateOpnaAdpcmRegister(0, 0x10, 200, kYmfmCalibration), 200))
        return 9;

    auto inner = std::make_unique<RecordingBackend>();
    RecordingBackend* recorder = inner.get();
    NormalizingChipBackend backend(std::move(inner), kYmfmCalibration);

    backend.writeReg(0, 0x11, 63);
    if (recorder->lastPort != 0 || recorder->lastAddr != 0x11 || recorder->lastData != 51)
        return 10;

    backend.writeReg(1, 0x0B, 100);
    if (recorder->lastPort != 1 || recorder->lastAddr != 0x0B || recorder->lastData != 104)
        return 11;

    backend.writeReg(1, 0x0C, 100);
    if (recorder->lastPort != 1 || recorder->lastAddr != 0x0C || recorder->lastData != 100)
        return 12;

    return 0;
}

// SPDX-License-Identifier: MIT

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

namespace {

void writeU16(std::ofstream& ofs, uint16_t value)
{
    const char bytes[2] = {
        static_cast<char>(value & 0xffU),
        static_cast<char>((value >> 8U) & 0xffU),
    };
    ofs.write(bytes, 2);
}

void writeU32(std::ofstream& ofs, uint32_t value)
{
    const char bytes[4] = {
        static_cast<char>(value & 0xffU),
        static_cast<char>((value >> 8U) & 0xffU),
        static_cast<char>((value >> 16U) & 0xffU),
        static_cast<char>((value >> 24U) & 0xffU),
    };
    ofs.write(bytes, 4);
}

bool writeWav(const std::string& path, uint16_t audioFormat, uint16_t channels, uint32_t sampleRate,
              uint16_t bitsPerSample, uint32_t declaredDataSize, const std::vector<uint8_t>& data)
{
    std::ofstream ofs(path, std::ios::binary);
    if (!ofs) {
        return false;
    }

    const uint16_t blockAlign = static_cast<uint16_t>((channels * bitsPerSample) / 8U);
    const uint32_t byteRate = sampleRate * static_cast<uint32_t>(blockAlign);
    const uint32_t riffSize = 4U + (8U + 16U) + (8U + declaredDataSize);

    ofs.write("RIFF", 4);
    writeU32(ofs, riffSize);
    ofs.write("WAVE", 4);

    ofs.write("fmt ", 4);
    writeU32(ofs, 16);
    writeU16(ofs, audioFormat);
    writeU16(ofs, channels);
    writeU32(ofs, sampleRate);
    writeU32(ofs, byteRate);
    writeU16(ofs, blockAlign);
    writeU16(ofs, bitsPerSample);

    ofs.write("data", 4);
    writeU32(ofs, declaredDataSize);
    if (!data.empty()) {
        ofs.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    }
    if (data.size() == declaredDataSize && (declaredDataSize & 1U) != 0U) {
        const char pad = 0;
        ofs.write(&pad, 1);
    }

    return static_cast<bool>(ofs);
}

std::string tempPath(const char* suffix)
{
    static int counter = 0;
    const char* tmp = std::getenv("TMPDIR");
    std::string dir = (tmp && tmp[0] != '\0') ? tmp : "/tmp";
    if (!dir.empty() && dir.back() != '/') {
        dir.push_back('/');
    }
    return dir + "libmucom88_drumkit_gen_wavparse_" + std::to_string(std::rand()) + "_" +
           std::to_string(counter++) + "_" + suffix;
}

int runDrumkitGen(const std::string& toolPath, const std::string& outPath, const std::string& wavPath)
{
    const std::string command = "\"" + toolPath + "\" -o \"" + outPath + "\" -bd \"" + wavPath + "\"";
    return std::system(command.c_str());
}

bool fileHasSize(const std::string& path, std::streamoff expectedSize)
{
    std::ifstream ifs(path, std::ios::binary | std::ios::ate);
    return ifs && ifs.tellg() == expectedSize;
}

std::vector<uint8_t> makePcm16Samples(size_t sampleCount)
{
    std::vector<uint8_t> data;
    data.reserve(sampleCount * 2U);
    for (size_t i = 0; i < sampleCount; i++) {
        const int16_t sample = static_cast<int16_t>((i % 64U) * 512U);
        data.push_back(static_cast<uint8_t>(sample & 0xff));
        data.push_back(static_cast<uint8_t>((sample >> 8) & 0xff));
    }
    return data;
}

bool expectReject(const std::string& toolPath, const std::string& caseName, uint16_t audioFormat,
                  uint16_t channels, uint32_t sampleRate, uint16_t bitsPerSample, uint32_t declaredDataSize,
                  const std::vector<uint8_t>& data)
{
    const std::string wavPath = tempPath((caseName + ".wav").c_str());
    const std::string outPath = tempPath((caseName + ".bin").c_str());
    if (!writeWav(wavPath, audioFormat, channels, sampleRate, bitsPerSample, declaredDataSize, data)) {
        return false;
    }

    const int result = runDrumkitGen(toolPath, outPath, wavPath);
    std::remove(wavPath.c_str());
    std::remove(outPath.c_str());
    return result != 0;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc != 2) {
        return 1;
    }

    const std::string toolPath = argv[1];
    std::srand(0x4d55434f);

    if (!expectReject(toolPath, "truncated_data", 1, 1, 44100, 16, 4, {0x00})) {
        return 2;
    }
    if (!expectReject(toolPath, "zero_bit_depth", 1, 1, 44100, 0, 0, {})) {
        return 3;
    }
    if (!expectReject(toolPath, "unaligned_data", 1, 1, 44100, 16, 1, {0x00})) {
        return 4;
    }
    if (!expectReject(toolPath, "float16", 3, 1, 44100, 16, 2, {0x00, 0x00})) {
        return 5;
    }

    const std::string validWav = tempPath("valid.wav");
    const std::string validOut = tempPath("valid.bin");
    const auto validData = makePcm16Samples(4096);
    if (!writeWav(validWav, 1, 1, 44100, 16, static_cast<uint32_t>(validData.size()), validData)) {
        return 6;
    }

    const int validResult = runDrumkitGen(toolPath, validOut, validWav);
    const bool validOutputOk = validResult == 0 && fileHasSize(validOut, 8192);
    std::remove(validWav.c_str());
    std::remove(validOut.c_str());

    return validOutputOk ? 0 : 7;
}

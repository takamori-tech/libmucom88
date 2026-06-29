// =============================================================================
// drumkit_gen.cpp
// WAV → YM2608 ADPCM-A ドラムキット ROM 生成ツール
//
// 6つの WAV ファイルから ADPCM-A ROM バイナリ（8192バイト）を生成する。
// 生成された ROM は ym2608_adpcm_rom.bin と同じ形式で、
// -adpcm オプションでゲーム/プレイヤーにロードできる。
//
// 使い方:
//   drumkit_gen -o output.bin [-bd BD.wav] [-sd SD.wav] [-cy CY.wav]
//                              [-hh HH.wav] [-tm TM.wav] [-rs RS.wav]
//
// 省略された楽器は無音になる。
// =============================================================================

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>
#include <fstream>
#include <algorithm>
#include <limits>

#include <mucom88/adpcm_a_encode.hpp>

// =============================================================================
// WAV ローダー（wav2adpcm.cpp と同じ）
// =============================================================================

struct WavFile {
    uint16_t audioFormat  = 0;
    uint16_t numChannels  = 0;
    uint32_t sampleRate   = 0;
    uint16_t bitsPerSample = 0;
    std::vector<float> samples;
    std::string error;
};

static bool readExact(std::ifstream& ifs, char* dst, std::streamsize size, std::string& error, const char* what)
{
    if (!ifs.read(dst, size)) {
        error = std::string("truncated ") + what;
        return false;
    }
    return true;
}

static bool ensureBytesAvailable(std::ifstream& ifs, uint32_t size, std::string& error, const char* what)
{
    const auto current = ifs.tellg();
    if (current < std::streampos(0)) {
        error = std::string("cannot determine ") + what + " position";
        return false;
    }

    ifs.seekg(0, std::ios::end);
    const auto end = ifs.tellg();
    ifs.seekg(current);
    if (!ifs || end < current) {
        error = std::string("cannot determine ") + what + " length";
        return false;
    }

    const auto remaining = end - current;
    if (remaining < std::streamoff(size)) {
        error = std::string("truncated ") + what;
        return false;
    }
    return true;
}

static bool skipExact(std::ifstream& ifs, uint32_t size, std::string& error, const char* what)
{
    if (size == 0) {
        return true;
    }
    if (!ensureBytesAvailable(ifs, size, error, what)) {
        return false;
    }
    ifs.seekg(static_cast<std::streamoff>(size), std::ios::cur);
    if (!ifs) {
        error = std::string("truncated ") + what;
        return false;
    }
    return true;
}

static bool skipChunkPadding(std::ifstream& ifs, uint32_t chunkSize, std::string& error)
{
    if ((chunkSize & 1U) == 0U) {
        return true;
    }
    char pad = 0;
    return readExact(ifs, &pad, 1, error, "chunk padding");
}

static bool isSupportedWavEncoding(uint16_t audioFormat, uint16_t bitsPerSample)
{
    if (audioFormat == 1) {
        return bitsPerSample == 8 || bitsPerSample == 16 || bitsPerSample == 24 || bitsPerSample == 32;
    }
    if (audioFormat == 3) {
        return bitsPerSample == 32 || bitsPerSample == 64;
    }
    return false;
}

static WavFile loadWav(const char* path)
{
    WavFile wav;
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs) { wav.error = "cannot open file"; return wav; }

    char riff[4];
    if (!readExact(ifs, riff, 4, wav.error, "RIFF header")) return wav;
    if (std::memcmp(riff, "RIFF", 4) != 0) { wav.error = "not a RIFF file"; return wav; }
    uint32_t fileSize = 0;
    if (!readExact(ifs, reinterpret_cast<char*>(&fileSize), 4, wav.error, "RIFF size")) return wav;
    (void)fileSize;
    char wave[4];
    if (!readExact(ifs, wave, 4, wav.error, "WAVE header")) return wav;
    if (std::memcmp(wave, "WAVE", 4) != 0) { wav.error = "not a WAVE file"; return wav; }

    bool fmtFound = false, dataFound = false;
    uint32_t dataSize = 0;
    std::vector<uint8_t> rawData;

    while (true) {
        char chunkId[4];
        if (!ifs.read(chunkId, 4)) {
            if (ifs.eof() && ifs.gcount() == 0) break;
            wav.error = "truncated chunk header";
            return wav;
        }
        uint32_t chunkSize = 0;
        if (!readExact(ifs, reinterpret_cast<char*>(&chunkSize), 4, wav.error, "chunk size")) return wav;

        if (std::memcmp(chunkId, "fmt ", 4) == 0) {
            if (chunkSize < 16) { wav.error = "fmt chunk too small"; return wav; }
            if (!readExact(ifs, reinterpret_cast<char*>(&wav.audioFormat), 2, wav.error, "fmt audio format")) return wav;
            if (!readExact(ifs, reinterpret_cast<char*>(&wav.numChannels), 2, wav.error, "fmt channel count")) return wav;
            if (!readExact(ifs, reinterpret_cast<char*>(&wav.sampleRate), 4, wav.error, "fmt sample rate")) return wav;
            uint32_t byteRate = 0;
            if (!readExact(ifs, reinterpret_cast<char*>(&byteRate), 4, wav.error, "fmt byte rate")) return wav;
            uint16_t blockAlign = 0;
            if (!readExact(ifs, reinterpret_cast<char*>(&blockAlign), 2, wav.error, "fmt block align")) return wav;
            (void)byteRate;
            (void)blockAlign;
            if (!readExact(ifs, reinterpret_cast<char*>(&wav.bitsPerSample), 2, wav.error, "fmt bits per sample")) return wav;
            if (chunkSize > 16 && !skipExact(ifs, chunkSize - 16, wav.error, "fmt extension")) return wav;
            if (!skipChunkPadding(ifs, chunkSize, wav.error)) return wav;
            fmtFound = true;
        }
        else if (std::memcmp(chunkId, "data", 4) == 0) {
            dataSize = chunkSize;
            if (!ensureBytesAvailable(ifs, dataSize, wav.error, "data chunk")) return wav;
            rawData.resize(dataSize);
            if (dataSize > 0 && !readExact(ifs, reinterpret_cast<char*>(rawData.data()),
                                           static_cast<std::streamsize>(dataSize), wav.error, "data chunk")) {
                return wav;
            }
            if (!skipChunkPadding(ifs, chunkSize, wav.error)) return wav;
            dataFound = true;
        }
        else {
            if (!skipExact(ifs, chunkSize, wav.error, "chunk body")) return wav;
            if (!skipChunkPadding(ifs, chunkSize, wav.error)) return wav;
        }
    }

    if (!fmtFound) { wav.error = "fmt chunk not found"; return wav; }
    if (!dataFound) { wav.error = "data chunk not found"; return wav; }
    if (wav.numChannels == 0) { wav.error = "0 channels"; return wav; }
    if (wav.audioFormat != 1 && wav.audioFormat != 3) {
        wav.error = "unsupported format"; return wav;
    }
    if (wav.sampleRate == 0) { wav.error = "0 sample rate"; return wav; }
    if (!isSupportedWavEncoding(wav.audioFormat, wav.bitsPerSample)) {
        wav.error = "unsupported bit depth"; return wav;
    }

    const size_t bytesPerSample = static_cast<size_t>(wav.bitsPerSample / 8);
    const size_t frameSize = bytesPerSample * static_cast<size_t>(wav.numChannels);
    if (bytesPerSample == 0 || frameSize == 0) {
        wav.error = "invalid frame size"; return wav;
    }
    if (dataSize % frameSize != 0) {
        wav.error = "data chunk not frame aligned"; return wav;
    }
    const size_t numFrames = static_cast<size_t>(dataSize) / frameSize;
    if (numFrames > static_cast<size_t>(std::numeric_limits<int>::max())) {
        wav.error = "too many frames"; return wav;
    }
    wav.samples.resize(numFrames);

    for (size_t i = 0; i < numFrames; i++) {
        float monoSum = 0.0f;
        for (uint16_t ch = 0; ch < wav.numChannels; ch++) {
            const size_t offset = i * frameSize + static_cast<size_t>(ch) * bytesPerSample;
            if (offset + bytesPerSample > rawData.size()) {
                wav.error = "sample data out of range";
                return wav;
            }
            float sample = 0.0f;
            if (wav.audioFormat == 1) {
                switch (wav.bitsPerSample) {
                case 8: sample = (static_cast<float>(rawData[offset]) - 128.0f) / 128.0f; break;
                case 16: { int16_t v; std::memcpy(&v, &rawData[offset], 2); sample = static_cast<float>(v) / 32768.0f; break; }
                case 24: {
                    int32_t v = static_cast<int32_t>(rawData[offset])
                        | (static_cast<int32_t>(rawData[offset + 1]) << 8)
                        | (static_cast<int32_t>(rawData[offset + 2]) << 16);
                    if (v & 0x800000) v |= ~0xFFFFFF;
                    sample = static_cast<float>(v) / 8388608.0f; break;
                }
                case 32: { int32_t v; std::memcpy(&v, &rawData[offset], 4); sample = static_cast<float>(v) / 2147483648.0f; break; }
                }
            } else {
                if (wav.bitsPerSample == 32) { float v; std::memcpy(&v, &rawData[offset], 4); sample = v; }
                else { double v; std::memcpy(&v, &rawData[offset], 8); sample = static_cast<float>(v); }
            }
            monoSum += sample;
        }
        wav.samples[i] = monoSum / static_cast<float>(wav.numChannels);
    }
    return wav;
}

// =============================================================================
// リサンプラー（線形補間）
// =============================================================================

static std::vector<float> resample(const std::vector<float>& input,
                                   uint32_t srcRate, uint32_t dstRate)
{
    if (srcRate == dstRate) return input;
    double ratio = (double)srcRate / (double)dstRate;
    int outLen = (int)((double)input.size() / ratio);
    std::vector<float> output(outLen);
    for (int i = 0; i < outLen; i++) {
        double srcPos = i * ratio;
        int idx = (int)srcPos;
        double frac = srcPos - idx;
        float s0 = (idx < (int)input.size()) ? input[idx] : 0.0f;
        float s1 = (idx + 1 < (int)input.size()) ? input[idx + 1] : s0;
        output[i] = s0 + (float)(frac * (s1 - s0));
    }
    return output;
}

struct DrumSlot {
    const char* name;       // 楽器名
    const char* flag;       // コマンドラインフラグ
    uint32_t    startAddr;  // ROM内開始アドレス
    uint32_t    endAddr;    // ROM内終了アドレス
    uint32_t    maxBytes;   // 最大バイト数
};

static const DrumSlot SLOTS[] = {
    { "Bass Drum",  "-bd", 0x0000, 0x01BF,  448 },
    { "Snare Drum", "-sd", 0x01C0, 0x043F,  640 },
    { "Top Cymbal", "-cy", 0x0440, 0x1B7F, 5952 },
    { "Hi-Hat",     "-hh", 0x1B80, 0x1CFF,  384 },
    { "Tom Tom",    "-tm", 0x1D00, 0x1F7F,  640 },
    { "Rim Shot",   "-rs", 0x1F80, 0x1FFF,  128 },
};
static constexpr int NUM_SLOTS = 6;
static constexpr int ROM_SIZE  = 8192;

// ADPCM-A のサンプルレート（YM2608: CHIP_CLOCK / 432 ≈ 18519Hz @ 8MHz）
static constexpr uint32_t ADPCM_A_RATE = 18519;

// =============================================================================
// メイン
// =============================================================================

static void usage(const char* prog)
{
    std::fprintf(stderr, "Usage: %s -o <output.bin> [-bd BD.wav] [-sd SD.wav] [-cy CY.wav]\n", prog);
    std::fprintf(stderr, "                           [-hh HH.wav] [-tm TM.wav] [-rs RS.wav]\n\n");
    std::fprintf(stderr, "Generates YM2608 ADPCM-A drum kit ROM (8192 bytes).\n");
    std::fprintf(stderr, "Output is compatible with -adpcm option of mvp_game/muc_player.\n\n");
    std::fprintf(stderr, "Drum slots:\n");
    for (int i = 0; i < NUM_SLOTS; i++) {
        std::fprintf(stderr, "  %-4s %-12s  max %5d bytes (%5.1fms at 18.5kHz)\n",
                     SLOTS[i].flag, SLOTS[i].name, SLOTS[i].maxBytes,
                     (double)(SLOTS[i].maxBytes * 2) / ADPCM_A_RATE * 1000.0);
    }
    std::fprintf(stderr, "\nOptions:\n");
    std::fprintf(stderr, "  -base <rom.bin>  : base ROM to patch (omitted slots keep original data)\n");
    std::fprintf(stderr, "\nWithout -base, omitted slots are filled with silence.\n");
    std::fprintf(stderr, "WAV format: any rate/bits/channels (auto-converted to 18.5kHz mono)\n");
}

int main(int argc, char* argv[])
{
    const char* outPath = nullptr;
    const char* basePath = nullptr;
    const char* wavPaths[NUM_SLOTS] = {};

    for (int i = 1; i < argc; i++) {
        if (std::strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
            outPath = argv[++i];
            continue;
        }
        if (std::strcmp(argv[i], "-base") == 0 && i + 1 < argc) {
            basePath = argv[++i];
            continue;
        }
        for (int s = 0; s < NUM_SLOTS; s++) {
            if (std::strcmp(argv[i], SLOTS[s].flag) == 0 && i + 1 < argc) {
                wavPaths[s] = argv[++i];
                break;
            }
        }
    }

    if (!outPath) {
        usage(argv[0]);
        return 1;
    }

    // ROM バッファ初期化
    std::vector<uint8_t> rom(ROM_SIZE, 0x00);

    // -base 指定時は既存 ROM をベースとして読み込む
    if (basePath) {
        std::ifstream baseIfs(basePath, std::ios::binary);
        if (!baseIfs) {
            std::fprintf(stderr, "Error: cannot read base ROM '%s'\n", basePath);
            return 1;
        }
        baseIfs.read(reinterpret_cast<char*>(rom.data()), ROM_SIZE);
        auto bytesRead = baseIfs.gcount();
        if (bytesRead < ROM_SIZE) {
            std::fprintf(stderr, "Warning: base ROM is %lld bytes (expected %d), padding with zeros\n",
                         (long long)bytesRead, ROM_SIZE);
        }
    }

    std::printf("Drum Kit Generator - YM2608 ADPCM-A ROM\n");
    if (basePath)
        std::printf("Base  : %s\n", basePath);
    std::printf("Output: %s (%d bytes)\n\n", outPath, ROM_SIZE);

    int loadedCount = 0;

    for (int s = 0; s < NUM_SLOTS; s++) {
        const DrumSlot& slot = SLOTS[s];

        if (!wavPaths[s]) {
            std::printf("  %-12s : %s\n", slot.name,
                        basePath ? "(keep original)" : "(silence)");
            continue;
        }

        // WAV 読み込み
        WavFile wav = loadWav(wavPaths[s]);
        if (!wav.error.empty()) {
            std::fprintf(stderr, "  %-12s : ERROR: %s (%s)\n",
                         slot.name, wav.error.c_str(), wavPaths[s]);
            return 1;
        }

        // リサンプリング
        auto resampled = resample(wav.samples, wav.sampleRate, ADPCM_A_RATE);
        if (resampled.empty()) {
            std::fprintf(stderr, "  %-12s : ERROR: no samples after resampling (%s)\n",
                         slot.name, wavPaths[s]);
            return 1;
        }

        // 最大サンプル数に切り詰め（1バイト=2サンプル）
        uint32_t maxSamples = slot.maxBytes * 2;
        if (resampled.size() > maxSamples) {
            std::printf("  %-12s : WARNING: trimmed %.1fms -> %.1fms\n",
                        slot.name,
                        (double)resampled.size() / ADPCM_A_RATE * 1000.0,
                        (double)maxSamples / ADPCM_A_RATE * 1000.0);
            resampled.resize(maxSamples);
        }

        // ピーク正規化
        float peak = 0.0f;
        for (float v : resampled) peak = std::max(peak, std::abs(v));
        if (peak > 0.001f) {
            float scale = 0.9f / peak;
            for (float& v : resampled) v *= scale;
        }

        // float → int16
        std::vector<int16_t> pcm16(resampled.size());
        for (size_t i = 0; i < resampled.size(); i++) {
            float v = std::clamp(resampled[i], -1.0f, 1.0f);
            pcm16[i] = (int16_t)(v * 32767.0f);
        }

        // ADPCM エンコード
        auto adpcm = encodeAdpcmANibbles(pcm16);
        if (adpcm.empty()) {
            std::fprintf(stderr, "  %-12s : ERROR: no ADPCM output (%s)\n",
                         slot.name, wavPaths[s]);
            return 1;
        }

        // ROM にコピー
        size_t copySize = std::min(adpcm.size(), (size_t)slot.maxBytes);
        std::memcpy(&rom[slot.startAddr], adpcm.data(), copySize);

        double durationMs = (double)(pcm16.size()) / ADPCM_A_RATE * 1000.0;
        std::printf("  %-12s : %s -> %zu bytes (%.1fms) [0x%04X-0x%04X]\n",
                    slot.name, wavPaths[s], copySize, durationMs,
                    slot.startAddr, (uint32_t)(slot.startAddr + copySize - 1));
        loadedCount++;
    }

    // ROM 出力
    std::ofstream ofs(outPath, std::ios::binary);
    if (!ofs) {
        std::fprintf(stderr, "\nError: cannot write '%s'\n", outPath);
        return 1;
    }
    ofs.write(reinterpret_cast<const char*>(rom.data()), rom.size());

    std::printf("\n%d/%d slots loaded. ROM written: %s (%d bytes)\n",
                loadedCount, NUM_SLOTS, outPath, ROM_SIZE);
    std::printf("Use with: ./build/mvp_game -adpcm %s\n", outPath);
    return 0;
}

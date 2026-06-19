// SPDX-License-Identifier: MIT

#include <mucom88/adpcm_a_decode.hpp>
#include <mucom88/adpcm_a_encode.hpp>

#include <cmath>
#include <cstdint>
#include <vector>

int main() {
    constexpr int kSampleCount = 1024;
    constexpr double kPi = 3.14159265358979323846;

    std::vector<int16_t> pcm;
    pcm.reserve(kSampleCount);
    for (int i = 0; i < kSampleCount; i++) {
        const double t = static_cast<double>(i) / static_cast<double>(kSampleCount);
        const double env = 1.0 - t;
        const double sample = std::sin(2.0 * kPi * 12.0 * t) * env;
        pcm.push_back(static_cast<int16_t>(sample * 28000.0));
    }

    const auto adpcm = encodeAdpcmANibbles(pcm);
    if (adpcm.size() != (pcm.size() + 1) / 2) {
        return 1;
    }

    std::vector<int16_t> decoded(adpcm.size() * 2);
    decodeAdpcmANibbles(adpcm.data(), static_cast<uint32_t>(adpcm.size()), decoded.data());
    if (decoded.size() != adpcm.size() * 2) {
        return 2;
    }

    bool hasNonZero = false;
    for (int16_t v : decoded) {
        if (v < -8192 || v > 8188) {
            return 3;
        }
        if (v != 0) {
            hasNonZero = true;
        }
    }

    return hasNonZero ? 0 : 4;
}

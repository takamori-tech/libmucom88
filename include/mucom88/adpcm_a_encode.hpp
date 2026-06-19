// SPDX-License-Identifier: MIT
// =============================================================================
// adpcm_a_encode.hpp
// YM2608 ADPCM-A (リズム音源) エンコード。純整数・エミュレータ非依存。
// adpcm_a_decode.hpp の逆変換。12bit アキュムレータ。
// drumkit ROM 生成ツール (tools/drumkit_gen.cpp) が使用。
// =============================================================================
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

// int16 PCM を ADPCM-A 4bit nibble 列へエンコード（偶数 index=上位 nibble）。
// 純整数・状態を持たない（呼び出しごとに acc/stepIndex を 0 初期化）。
inline std::vector<uint8_t> encodeAdpcmANibbles(const std::vector<int16_t>& pcm) {
    static constexpr int16_t kStep[49] = {
        16,17,19,21,23,25,28,31,34,37,41,45,50,55,60,66,73,80,88,97,107,
        118,130,143,157,173,190,209,230,253,279,307,337,371,408,449,494,
        544,598,658,724,796,876,963,1060,1166,1282,1411,1552
    };
    static constexpr int8_t kInc[8] = { -1,-1,-1,-1, 2, 5, 7, 9 };
    std::vector<uint8_t> out;
    int32_t accumulator = 0;
    int stepIndex = 0;
    for (size_t i = 0; i < pcm.size(); i++) {
        int32_t sample12 = (static_cast<int32_t>(pcm[i]) * 2048) / 32768;
        int32_t diff = sample12 - accumulator;
        int step = kStep[stepIndex];
        uint8_t nibble = 0;
        if (diff < 0) { nibble = 8; diff = -diff; }
        if (diff >= step * 7 / 8) nibble |= 4;
        if (diff >= step * 3 / 8) nibble |= 2;
        if (diff >= step * 1 / 8) nibble |= 1;
        int32_t delta = (2 * (nibble & 7) + 1) * step / 8;
        if (nibble & 8) delta = -delta;
        accumulator = (accumulator + delta) & 0xFFF;
        if (accumulator & 0x800) accumulator |= ~0xFFF;
        stepIndex = std::clamp(stepIndex + kInc[nibble & 7], 0, 48);
        if (i % 2 == 0) out.push_back(static_cast<uint8_t>(nibble << 4));
        else out.back() |= nibble;
    }
    return out;
}

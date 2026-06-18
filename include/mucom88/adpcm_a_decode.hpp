// SPDX-License-Identifier: MIT
// =============================================================================
// adpcm_a_decode.hpp
// YM2608 ADPCM-A (リズム音源) ROM デコード。純整数・エミュレータ非依存。
//
// YM2608 リズム ROM の 6 音 (BD/SD/CY/HH/TM/RS) を int16 PCM へデコードする。
// fmgen の SetRhythmSampleDirect / IChipBackend::loadRhythmSample へ渡す前処理。
// デコード数学はチップ非依存のため共有ヘッダに集約 (#273 段4, #78)。
// =============================================================================
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>

// リズム ROM 内 6 音の開始/終了アドレス (BD,SD,CY,HH,TM,RS)
inline constexpr uint32_t kAdpcmARhythmEntries[6][2] = {
    {0x0000, 0x01BF}, {0x01C0, 0x043F}, {0x0440, 0x1B7F},
    {0x1B80, 0x1CFF}, {0x1D00, 0x1F7F}, {0x1F80, 0x1FFF},
};

// numBytes 個の ADPCM-A バイト (src) を numBytes*2 個の int16 PCM (pcmOut) へデコード。
// pcmOut は呼び出し側が numBytes*2 要素確保する。純整数・例外なし・状態を持たない。
inline void decodeAdpcmANibbles(const uint8_t* src, uint32_t numBytes, int16_t* pcmOut) noexcept {
    static constexpr int16_t kStep[49] = {
        16,17,19,21,23,25,28,31,34,37,41,45,50,55,60,66,73,80,88,97,107,
        118,130,143,157,173,190,209,230,253,279,307,337,371,408,449,494,
        544,598,658,724,796,876,963,1060,1166,1282,1411,1552
    };
    static constexpr int8_t kInc[8] = { -1,-1,-1,-1, 2, 5, 7, 9 };
    int32_t acc = 0;
    int si = 0;
    for (uint32_t j = 0; j < numBytes; j++) {
        uint8_t b = src[j];
        for (int ni = 0; ni < 2; ni++) {
            int nib = ni == 0 ? (b >> 4) : (b & 0x0F);
            int32_t delta = (2 * (nib & 7) + 1) * kStep[si] / 8;
            if (nib & 8) delta = -delta;
            acc = (acc + delta) & 0xFFF;
            if (acc & 0x800) acc |= ~0xFFF;
            si = std::clamp(si + kInc[nib & 7], 0, 48);
            pcmOut[static_cast<size_t>(j) * 2 + static_cast<size_t>(ni)] = static_cast<int16_t>(acc * 4);
        }
    }
}

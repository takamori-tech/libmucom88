// =============================================================================
// fm_engine_interface.hpp
// FM音源エンジン共通インターフェース
//
// fmgen版 (FmEngineFmgen) 等がこの
// インターフェースを実装する。mml_engine やゲーム本体は
// IFmEngine* を通じて操作し、実装を切り替え可能にする。
// =============================================================================

#pragma once

#include <cstdint>
#include <string>
#include "fm_common.hpp"

class IFmEngine
{
public:
    virtual ~IFmEngine() = default;

    // 初期化（出力サンプルレート指定）
    virtual void init(uint32_t sampleRate) = 0;

    // YM2608レジスタ書き込み
    // port=0: ポート0（FM ch1-3 / SSG）, port=1: ポート1（FM ch4-6）
    virtual void writeReg(int port, uint8_t addr, uint8_t data) noexcept = 0;

    // ステレオPCM生成（インターリーブ L,R,L,R...）
    virtual void generateInterleaved(int16_t* buf, uint32_t frameCount) noexcept = 0;

    // リセット
    virtual void reset() noexcept = 0;

    // ADPCM-A ROM ロード（リズム音源用）
    [[nodiscard]] virtual bool loadAdpcmRom(const std::string& path) = 0;
    [[nodiscard]] virtual bool loadAdpcmRomFromMemory(const uint8_t* data, size_t size) = 0;
    [[nodiscard]] virtual bool hasAdpcmRom() const noexcept = 0;

    // ADPCM-B ボイステーブル
    [[nodiscard]] virtual bool loadVoiceTable(const std::string& path) = 0;
    [[nodiscard]] virtual bool loadVoiceTableFromMemory(const uint8_t* data, size_t dataSize) = 0;
    [[nodiscard]] virtual bool hasVoiceTable() const noexcept = 0;
    // level: ADPCM-Bボリューム（0=無音、255=最大）。再生開始時に一発で書き込む
    virtual void playVoice(int voiceId, int level = 255) noexcept = 0;
    virtual void stopVoice() noexcept = 0;
    [[nodiscard]] virtual bool isVoicePlaying() const noexcept = 0;
    virtual void tickVoiceTimer(uint32_t frameCount) noexcept = 0;
    // BGM + ボイス両方のADPCM-Bを強制停止
    virtual void stopAdpcmB() noexcept = 0;

    // ADPCM-B PCMデータロード（mucompcm.binのデータ部分）
    [[nodiscard]] virtual bool loadPcmDataToAdpcmB(const uint8_t* /*data*/, size_t /*size*/) { return false; }

    // FM/SSG音量バランス（ミックスレベル調整）
    // ssgScale: SSG出力のリニアスケール（1.0=等倍、0.71≈-3dB、デフォルト）
    // fmgen: SetVolumePSG(dB) で実装
    virtual void setSsgMixScale(float /*ssgScale*/) noexcept {}
    virtual float getSsgMixScale() const noexcept { return 1.0f; }

    // ── FM音色適用（MUCOM88 STENV互換）──────────────────
    // fmIndex: FM index (0-5)。port/offsetは内部で計算。
    // KEY_OFF → SL/RR=0x0F → 全オペレータパラメータ → FB/ALG → PAN(L+R)
    virtual void applyPatch(int fmIndex, const FmPatch& patch) noexcept
    {
        if (fmIndex < 0 || fmIndex > 5) return;  // 範囲外ガード
        int port = (fmIndex < 3) ? 0 : 1;
        int off  = fmIndex % 3;
        // KEY_OFF
        uint8_t chKey = (fmIndex < 3) ? static_cast<uint8_t>(fmIndex) : static_cast<uint8_t>(fmIndex - 3 + 4);
        writeReg(0, 0x28, static_cast<uint8_t>(0x00 | chKey));
        // SL/RR = 0x0F（最速リリース）
        for (int oi = 0; oi < 4; oi++)
            writeReg(port, 0x80 + kFmSlotOffset[oi] + off, 0x0F);
        // 全オペレータパラメータ
        for (int oi = 0; oi < 4; oi++) {
            int base = kFmSlotOffset[oi] + off;
            const auto& op = patch.op[oi];
            writeReg(port, 0x30 + base, static_cast<uint8_t>(((op.dt & 0x07) << 4) | (op.ml & 0x0F)));
            writeReg(port, 0x40 + base, static_cast<uint8_t>(op.tl & 0x7F));
            writeReg(port, 0x50 + base, static_cast<uint8_t>(((op.ks & 0x03) << 6) | (op.ar & 0x1F)));
            writeReg(port, 0x60 + base, static_cast<uint8_t>(((op.ame & 1) << 7) | (op.dr & 0x1F)));
            writeReg(port, 0x70 + base, static_cast<uint8_t>(op.sr & 0x1F));
            writeReg(port, 0x80 + base, static_cast<uint8_t>(((op.sl & 0x0F) << 4) | (op.rr & 0x0F)));
        }
        // FB/ALG
        writeReg(port, 0xB0 + off, static_cast<uint8_t>(((patch.fb & 0x07) << 3) | (patch.al & 0x07)));
        // PAN: L+R
        writeReg(port, 0xB4 + off, 0xC0);
    }

    // ── FM周波数設定（F-Number + Block）──────────────────
    // fmIndex: FM index (0-5)。noteNum: MIDIノート番号。
    // noteToFnum()でF-Number/Block計算、0xA4→0xA0の順でラッチ。
    virtual void setFrequency(int fmIndex, int noteNum) noexcept
    {
        if (fmIndex < 0 || fmIndex > 5) return;  // 範囲外ガード
        int port = (fmIndex < 3) ? 0 : 1;
        int off  = fmIndex % 3;
        int block = 4;
        uint16_t fnum = noteToFnum(noteNum, block);
        writeReg(port, 0xA4 + off, static_cast<uint8_t>(((block & 0x07) << 3) | ((fnum >> 8) & 0x07)));
        writeReg(port, 0xA0 + off, static_cast<uint8_t>(fnum & 0xFF));
    }

    // ── FM KEY ON/OFF ────────────────────────────────────
    virtual void fmKeyOn(int fmIndex) noexcept
    {
        if (fmIndex < 0 || fmIndex > 5) return;  // 範囲外ガード
        uint8_t chKey = (fmIndex < 3) ? static_cast<uint8_t>(fmIndex) : static_cast<uint8_t>(fmIndex - 3 + 4);
        writeReg(0, 0x28, static_cast<uint8_t>(0xF0 | chKey));
    }
    virtual void fmKeyOff(int fmIndex) noexcept
    {
        if (fmIndex < 0 || fmIndex > 5) return;  // 範囲外ガード
        uint8_t chKey = (fmIndex < 3) ? static_cast<uint8_t>(fmIndex) : static_cast<uint8_t>(fmIndex - 3 + 4);
        writeReg(0, 0x28, static_cast<uint8_t>(0x00 | chKey));
    }
};

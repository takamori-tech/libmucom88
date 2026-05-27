// =============================================================================
// mml_engine.hpp  (revised)
// MML シーケンサー / YM2608 ドライバー（MUCOM88形式対応）
//
// 対応チャンネル:
//   A-C (0-2):  FM ch1-3（port 0）
//   D-F (3-5):  SSG ch1-3（PSG互換矩形波）
//   G   (6):    リズム音源（ADPCM-A: BD/SD/CY/HH/TM/RS）
//   H-J (7-9):  FM ch4-6（port 1）
//   K   (10):   ADPCM-B（未実装）
// =============================================================================

#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
#include <array>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <unordered_map>
#include <fstream>
#include "mml_parser.hpp"
#include "fm_engine_interface.hpp"

// FmPatch / Mucom88Patch は fm_common.hpp で定義済み（mml_parser.hpp 経由）

// ── デフォルト音色ファクトリ ─────────────────────────────
static Mucom88Patch makeDefaultPatch(int pno = 0)
{
    Mucom88Patch p;
    p.patchNo = pno; p.fb = 0; p.al = 4; p.valid = true;
    // AR DR SR RR SL TL KS ML DT
    p.op[0] = { 31, 5, 0, 5, 0, 28, 0, 1, 0 };
    p.op[1] = { 31, 5, 0, 5, 0, 28, 0, 1, 0 };
    p.op[2] = { 31, 5, 0, 5, 0, 28, 0, 1, 0 };
    p.op[3] = { 31, 5, 0, 5, 0,  0, 0, 1, 0 };
    return p;
}

// noteToFnum / noteToSSGPeriod は fm_common.hpp で定義済み（mml_parser.hpp 経由）

// =============================================================================
// MmlEngine
// =============================================================================
class MmlEngine
{
public:
    // MUCOM88 全11チャンネル (A-K)
    static constexpr int MAX_MML_CHANNELS = 11;
    // FM 6チャンネル (A-C=0-2, H-J=7-9 → FM index 0-5)
    static constexpr int MAX_FM_CHANNELS  = 6;
    // SSG 3チャンネル (D-F=3-5)
    static constexpr int MAX_SSG_CHANNELS = 3;

    // ボイス再生＋ダッキング状態マシン（スレッド安全）
    // ゲームスレッド(playVoice/stopVoice)とオーディオスレッド(tickVoiceTimer)間の
    // 安全な状態遷移を std::atomic + CAS で実現する。
    //   Idle → Playing:    playVoice()      [ゲームスレッド, store]
    //   Playing → Releasing: tickVoiceTimer() [オーディオスレッド, CAS]
    //   Releasing → Idle:  tickVoiceTimer()  [オーディオスレッド, store]
    //   Any → Playing:     playVoice() 連続呼び出し [ゲームスレッド, store]
    //   Any → Idle:        stopVoice()/stop() [exchange]
    enum class VoiceDuckState : int {
        Idle,       // ボイス未再生、ダッキングなし
        Playing,    // ボイス再生中（Kトラック抑制 + 音量減衰）
        Releasing   // ボイス終了、ダッキングリリース中（音量徐々に復帰）
    };

    // ── SE再生モード ──────────────────────────────────────
    // Classic: BGMチャンネルをハイジャックしてSE再生（従来方式）
    // Rich: 専用SEチップ（2台目のIFmEngine）でSE再生（BGMチャンネル不使用）
    enum class SeMode { Classic, Rich };

    // ── フェードアウト完了時の自動アクション ────────────────────
    // None: 何もしない（デフォルト、後方互換）
    // Stop: BGM停止（stop() 呼び出し相当）
    // StopAndReset: BGM停止 + チップリセット（IFmEngine::reset()）
    enum class FadeAction {
        None,
        Stop,
        StopAndReset
    };

    MmlEngine() : m_engine(nullptr), m_sampleRate(44100), m_chipClock(7987200), m_playing(false) {}

    // IFmEngineポインタ・atomicメンバを保持するためコピー禁止（C.21）
    MmlEngine(const MmlEngine&) = delete;
    MmlEngine& operator=(const MmlEngine&) = delete;

    // ── チャンネル種別判定 ────────────────────────────────
    static bool isFM(int ch)     { return ch <= 2 || (ch >= 7 && ch <= 9); }
    static bool isSSG(int ch)    { return ch >= 3 && ch <= 5; }
    static bool isRhythm(int ch) { return ch == 6; }
    static bool isADPCMB(int ch) { return ch == 10; }
    // FM index (0-5) from MML channel
    static int toFMIndex(int ch) { return (ch <= 2) ? ch : (ch - 7 + 3); }
    // SSG index (0-2) from MML channel
    static int toSSGIndex(int ch) { return ch - 3; }

    // ── 初期化 ─────────────────────────────────────────
    void init(IFmEngine* engine, uint32_t sampleRate, uint32_t chipClock = 7987200)
    {
        m_engine     = engine;
        m_sampleRate = sampleRate;
        m_chipClock  = chipClock;
        for (auto& ch : m_channels) ch = ChannelState{};
        m_patchMap.clear();
        m_fmPatchNo.fill(0);
        m_ssgMixer   = 0x3F;  // 全SSGトーン・ノイズ無効
        m_rhythmMask = 0;
        m_patchMap[0] = makeDefaultPatch(0);
        for (auto& slot : m_seSlots) slot = SeSlot{};
        m_seAllocCounter = 0;
    }

    // ── MML イベント列を直接セット（MucFile用）─────────
    void setEvents(int ch, const std::vector<MmlEvent>& evts)
    {
        if (ch < 0 || ch >= MAX_MML_CHANNELS) return;
        m_channels[ch].events      = evts;
        m_channels[ch].eventIdx    = 0;
        m_channels[ch].noteOn      = false;
    }

    // ── MML 読み込み（シングルチャンネル用）────────────
    void loadMml(const std::string& mml, int ch = 0)
    {
        if (ch < 0 || ch >= MAX_MML_CHANNELS) return;
        setEvents(ch, parseSingleChannelMml(mml, ch));
    }

    // ── 音色設定（音色番号ベース・0〜127）────────────────
    void setPatch(int patchNo, const FmPatch& patch)
    {
        m_patchMap[patchNo] = patch;
    }

    // ── Cコマンド（全音符クロック数）設定 ──────────────
    // パーサーのwholeTick値を渡す。テンポ計算のPPQに影響。
    void setWholeTick(int wt) { m_wholeTick = (wt > 0) ? wt : 128; }

    // ── パース結果の一括適用 ──────────────────────────
    // MucFileから音色・全音符クロック・全チャンネルイベントをまとめて設定。
    // init() 後、play() 前に呼ぶ。
    void loadFromParseResult(const MmlParser::MucFile& muc)
    {
        for (const auto& [no, patch] : muc.patches)
            setPatch(no, patch);
        setWholeTick(muc.wholeTick);
        // 旧曲のイベントをクリア（チャンネル数が減った場合の残留防止）
        for (int ch = 0; ch < MAX_MML_CHANNELS; ch++)
            m_channels[ch].events.clear();
        bool hasLoop = false;
        for (int ch = 0; ch < MAX_MML_CHANNELS; ch++) {
            if (!muc.channelEvents[ch].empty())
                setEvents(ch, muc.channelEvents[ch]);
            for (const auto& ev : muc.channelEvents[ch])
                if (ev.type == MmlEventType::LOOP_POINT) { hasLoop = true; break; }
        }
        m_loop = hasLoop;
    }

    // 曲全体のループ終端tickを外部から設定（パート分離比較用）
    // play()内のcommonEndTick計算を上書きする
    void setCommonEndTick(uint32_t tick) { m_overrideEndTick = tick; }

    // ── ループ設定 ──────────────────────────────────────
    void setLoop(bool loop) { m_loop = loop; }

    // ── 再生開始 ────────────────────────────────────────
    void play() noexcept
    {
        m_globalTick        = 0;
        m_globalSampleAccum = 0;
        m_audioLeftMs       = 0.0;
        m_globalTempo       = 120;
        m_loopTickOffset    = 0;
        m_perChannelLoop   = false;
        m_fadeAtt  = 0;
        m_duckAtt  = 0;
        m_fading   = false;
        m_fadeAction   = FadeAction::None;
        m_fadeOutDone  = false;
        m_globalAtt = m_masterAtt + m_bgmAtt;
        stopAllSe();
        m_seAllocCounter = 0;
        // ADPCM-B エンジンレベル状態リセット
        m_pcmVolMode    = 0;
        m_pcmAddVol     = 0;
        m_pcmCurrentNum = 0;
        m_pcmPan        = 0xC0;
        // リズム全体TLリセット（前曲のvコマンド残留防止）
        m_rhythmTL      = 0x3F;

        // 全チャンネルのランタイム状態をフルリセット（libmucom88-mml#2）
        // イベント列(events)は保持し、再生位置とランタイム状態のみ初期化
        for (int ch = 0; ch < MAX_MML_CHANNELS; ch++) {
            auto& st = m_channels[ch];
            auto savedEvents = std::move(st.events);
            st = ChannelState{};
            st.events = std::move(savedEvents);
        }
        // 最初のTEMPOイベントを探して初期テンポを設定
        for (int ch = 0; ch < MAX_MML_CHANNELS; ch++) {
            for (const auto& ev : m_channels[ch].events) {
                if (ev.type == MmlEventType::TEMPO) {
                    m_globalTempo = ev.value;
                    break;
                }
            }
        }
        // 曲全体ループ周期の計算（OpenMUCOM88 maxcount 互換）
        // Wiki: Lコマンド = "曲全体のループ位置指定"
        // イベント列を直接走査してLOOP_POINTを検出（processEvents実行前に必要）
        {
            m_commonEndTick = 0;
            m_commonLoopTick = UINT32_MAX;
            bool anyLoop = false;
            uint32_t maxEnd = 0;
            for (int ch = 0; ch < MAX_MML_CHANNELS; ch++) {
                auto& st = m_channels[ch];
                if (st.events.empty()) continue;
                // イベント列からLOOP_POINTを探してhasLoopPointを事前設定
                for (size_t i = 0; i < st.events.size(); i++) {
                    if (st.events[i].type == MmlEventType::LOOP_POINT) {
                        st.hasLoopPoint = true;
                        st.loopEventIdx = i + 1;
                        st.loopTick     = st.events[i].tick;
                        break;
                    }
                }
                uint32_t endTick = st.events.back().tick;
                st.perChEndTick = endTick;
                st.perChLoopOffset = 0;
                if (endTick > maxEnd) maxEnd = endTick;
                if (!st.hasLoopPoint) continue;
                anyLoop = true;
                if (st.loopTick < m_commonLoopTick)
                    m_commonLoopTick = st.loopTick;
            }
            if (anyLoop && maxEnd > 0)
                m_commonEndTick = maxEnd;
            if (m_commonLoopTick == UINT32_MAX)
                m_commonLoopTick = 0;
            // 外部からループ終端tickが指定されている場合はそちらを優先
            // （パート分離比較時にOpenMUCOM88のMaxCountと合わせるため）
            if (m_overrideEndTick > 0)
                m_commonEndTick = m_overrideEndTick;
        }
        // Timer-B 初期化
        recalcTimerB();
        m_timerBCount = m_timerBPeriod;

        if (m_engine) {
            allSoundOff();

            // OpenMUCOM88互換の初期化シーケンス
            // FM PAN: 全チャンネル L+R
            for (int fi = 0; fi < MAX_FM_CHANNELS; fi++) {
                int port = fmPort(fi);
                int off  = fmOffset(fi);
                m_engine->writeReg(port, 0xB4 + off, 0xC0);
            }
            // リズム楽器 IL 初期化: L+R + Level 31
            m_rhythmIL.fill(0xDF);
            for (int i = 0; i < 6; i++)
                m_engine->writeReg(0, 0x18 + i, m_rhythmIL[i]);
            // SSG トーンレジスタ初期化
            for (int i = 0; i < 6; i++)
                m_engine->writeReg(0, i, 0x00);
            // SSG ミキサー: トーン有効, ノイズ無効（MUCOM88互換）
            m_ssgMixer = 0x38;
            m_engine->writeReg(0, 0x07, m_ssgMixer);
            // SSG ノイズ周期
            m_engine->writeReg(0, 0x06, 0x00);
            // プリスケーラ + FM6chモード（reset()後にデフォルトに戻るため再設定必須）
            m_engine->writeReg(0, 0x2D, 0x00);  // プリスケーラ mode 0 (×6)
            m_engine->writeReg(0, 0x29, 0x80);  // FM6chモード有効（ch4-6使用に必要）
            // Timer制御: 通常モード（CSMモード解除）
            // Z80 PLSET2: reg 0x27 = 0x3A（Timer-B有効、CSMなし）
            m_engine->writeReg(0, 0x27, 0x3A);

            // FM音色を再適用
            for (int fi = 0; fi < MAX_FM_CHANNELS; fi++)
                fmApplyPatch(fi, m_fmPatchNo[fi]);
        }

        // Z80互換: tick 0 のイベントを即時処理
        // Z80ドライバーはplay()時にMMLデータを即座に読み込み、最初のノートを設定する。
        // Timer-Bの最初の発火を待たない。これにより1ティック分の遅延を回避。
        for (int ch = 0; ch < MAX_MML_CHANNELS; ch++) {
            auto& st = m_channels[ch];
            if (st.events.empty()) continue;
            if (isADPCMB(ch) && m_voiceDuckState.load(std::memory_order_acquire) != VoiceDuckState::Idle) continue;
            processEvents(ch, m_globalTick);  // m_globalTick = 0
        }

        m_playing = true;
    }

    // ── 停止 ────────────────────────────────────────────
    void stop() noexcept
    {
        m_playing = false;
        m_voiceDuckState.exchange(VoiceDuckState::Idle, std::memory_order_acq_rel);
        m_fadeAtt = 0;
        m_duckAtt = 0;
        m_fading  = false;
        m_globalAtt = m_masterAtt + m_bgmAtt;
        stopAllSe();
        if (m_engine) allSoundOff();
    }

    // ── 一時停止 ────────────────────────────────────────
    void pause() noexcept
    {
        m_playing = false;
        if (m_engine) allSoundOff();
    }

    // ── 再開 ────────────────────────────────────────────
    void resume() noexcept
    {
        if (m_engine) {
            // allSoundOff()がm_ssgMixerを0x3Fに上書きするため事前に退避
            uint8_t savedMixer = m_ssgMixer;
            allSoundOff();
            m_ssgMixer = savedMixer;
            // FM: 音色 + 音量 + PAN
            for (int fi = 0; fi < MAX_FM_CHANNELS; fi++) {
                int ch = fmMmlCh(fi);
                if (m_channels[ch].hijacked) continue;
                fmApplyPatch(fi, m_fmPatchNo[fi]);
                fmSetVolume(fi, m_channels[ch].volume);
                int port = fmPort(fi);
                int off  = fmOffset(fi);
                m_engine->writeReg(port, 0xB4 + off,
                    static_cast<uint8_t>(panToReg(m_channels[ch].pan)));
            }
            // SSG: ミキサー復元
            m_engine->writeReg(0, 0x07, m_ssgMixer);
            // リズム: TL + IL復元
            int rhythmAtt = m_globalAtt * 63 / 127;
            int adjustedTL = std::clamp(static_cast<int>(m_rhythmTL) - rhythmAtt, 0, 63);
            m_engine->writeReg(0, 0x11, static_cast<uint8_t>(adjustedTL & 0x3F));
            for (int i = 0; i < 6; i++)
                m_engine->writeReg(0, 0x18 + i, m_rhythmIL[i]);
            // ADPCM-B: ボリューム復元（ボイス再生中はスキップ）
            if (m_voiceDuckState.load(std::memory_order_acquire) == VoiceDuckState::Idle) {
                adpcmbSetVolume(m_channels[10].volume);
            }
        }
        m_playing = true;
    }

    [[nodiscard]] bool isPlaying() const { return m_playing; }
    [[nodiscard]] uint32_t globalTick() const { return m_globalTick; }
    [[nodiscard]] int globalTempo() const { return m_globalTempo; }
    [[nodiscard]] uint32_t commonEndTick() const { return m_commonEndTick; }
    [[nodiscard]] uint32_t loopTickOffset() const { return m_loopTickOffset; }
    [[nodiscard]] int loopCount() const {
        if (m_commonEndTick == 0 || m_commonEndTick <= m_commonLoopTick) return 0;
        uint32_t loopLen = m_commonEndTick - m_commonLoopTick;
        return static_cast<int>(m_loopTickOffset / loopLen);
    }

    // チャンネル状態取得（UI表示用）
    [[nodiscard]] bool chNoteOn(int ch) const { return (ch >= 0 && ch < MAX_MML_CHANNELS) ? m_channels[ch].noteOn : false; }
    [[nodiscard]] int  chNote(int ch) const { return (ch >= 0 && ch < MAX_MML_CHANNELS) ? m_channels[ch].currentNote : 0; }
    [[nodiscard]] int  chVolume(int ch) const { return (ch >= 0 && ch < MAX_MML_CHANNELS) ? m_channels[ch].volume : 0; }
    [[nodiscard]] int  chPan(int ch) const { return (ch >= 0 && ch < MAX_MML_CHANNELS) ? m_channels[ch].pan : 3; }
    [[nodiscard]] int  chReverb(int ch) const { return (ch >= 0 && ch < MAX_MML_CHANNELS) ? m_channels[ch].reverbValue : 0; }
    // noteOnトリガーカウンター（UI activity検出用、advance()毎にインクリメント）
    // chNoteOn()はワンショット楽器で一瞬falseになるため、カウンターで検出する
    [[nodiscard]] uint32_t chNoteOnCount(int ch) const { return (ch >= 0 && ch < MAX_MML_CHANNELS) ? m_channels[ch].noteOnCount : 0; }
    // FM パッチ番号取得（fi=FMインデックス 0-5）
    [[nodiscard]] int  fmPatchNo(int fi) const { return (fi >= 0 && fi < MAX_FM_CHANNELS) ? m_fmPatchNo[fi] : -1; }

    // ── チャンネルハイジャック（効果音割り込み用）──────────
    // BGM再生中のチャンネルをSE再生に一時的に奪う。
    // ハイジャック中はBGMのレジスタ書き込みを抑制し、
    // 外部コードがIFmEngine::writeReg()で直接制御可能になる。
    // BGMのイベント進行は継続し、releaseChannel()で状態復元して再開。
    void hijackChannel(int ch)
    {
        if (ch < 0 || ch >= MAX_MML_CHANNELS) return;
        if (isRhythm(ch) || isADPCMB(ch)) return;
        auto& st = m_channels[ch];
        st.hijacked = true;
        if (m_engine && st.noteOn) {
            if      (isFM(ch))  fmKeyOff(toFMIndex(ch));
            else if (isSSG(ch)) ssgKeyOff(toSSGIndex(ch));
        }
    }
    void releaseChannel(int ch)
    {
        if (ch < 0 || ch >= MAX_MML_CHANNELS) return;
        auto& st = m_channels[ch];
        if (!st.hijacked) return;
        st.hijacked = false;
        if (!m_engine) return;
        if (isFM(ch)) {
            int fi = toFMIndex(ch);
            fmApplyPatch(fi, m_fmPatchNo[fi]);
            fmSetVolume(fi, st.volume);
            int port = fmPort(fi);
            int off  = fmOffset(fi);
            m_engine->writeReg(port, 0xB4 + off, static_cast<uint8_t>(panToReg(st.pan)));
        } else if (isSSG(ch)) {
            m_engine->writeReg(0, 0x07, m_ssgMixer);
            if (st.ssgSoftEnv) {
                st.ssgEnvValue = st.ssgEnvAL;
                st.ssgEnvPhase = 1;
                st.ssgEnvKeyOnTick = true;
            }
        }
    }
    bool isChannelHijacked(int ch) const
    {
        return (ch >= 0 && ch < MAX_MML_CHANNELS) ? m_channels[ch].hijacked : false;
    }

    // ── ADPCM-B ボイス再生（IFmEngine パススルー + Kトラック優先制御）──
    // ゲームボイス再生中はBGMのKトラック(ch10)イベント処理を抑制し、
    // ADPCM-Bをボイス再生に専有させる。
    [[nodiscard]] bool hasVoiceTable() const {
        return m_engine ? m_engine->hasVoiceTable() : false;
    }
    [[nodiscard]] bool loadVoiceTable(const std::string& path) {
        return m_engine ? m_engine->loadVoiceTable(path) : false;
    }
    [[nodiscard]] bool loadVoiceTableFromMemory(const uint8_t* data, size_t size) {
        return m_engine ? m_engine->loadVoiceTableFromMemory(data, size) : false;
    }
    void playVoice(int voiceId) {
        if (!m_engine) return;
        // BGMのKトラック（ADPCM-B）を停止してからボイス再生を開始
        if (m_channels[10].noteOn) {
            adpcmbKeyOff();
            m_channels[10].noteOn = false;
        }
        // 状態遷移を先に行い、recalcGlobalAtt()内のADPCM-Bガードを有効化
        m_voiceDuckState.store(VoiceDuckState::Playing, std::memory_order_release);
        // ダッキング開始（ADPCM-Bはガードでスキップされる）
        if (m_duckEnabled) {
            setGlobalAttenuation(m_duckAttTarget);
        }
        // ボイス再生開始 + 音量設定。
        // 音量レベルは再生開始時に playVoice の引数で一発書き込む。
        // （後追いの writeReg(1,0x0B,..) は writeReg ガード
        //   「port1 で remainSamples>0 かつ addr<=0x0B は return」に弾かれ
        //   破棄されていたため廃止。#196）
        // voiceTotalAtt==0（最大音量）でも常に明示的に値を渡す（副因解消）。
        int voiceTotalAtt = m_masterAtt + m_voiceAtt;
        int voiceVol = std::clamp(255 - voiceTotalAtt * 2, 0, 255);
        m_engine->playVoice(voiceId, voiceVol);
    }
    void stopVoice() {
        if (!m_engine) return;
        m_engine->stopVoice();
        auto prev = m_voiceDuckState.exchange(VoiceDuckState::Idle, std::memory_order_acq_rel);
        if (prev != VoiceDuckState::Idle) {
            setGlobalAttenuation(0);
        }
    }
    [[nodiscard]] bool isVoicePlaying() const {
        return m_engine ? m_engine->isVoicePlaying() : false;
    }
    void tickVoiceTimer(uint32_t frameCount) noexcept {
        if (!m_engine) return;
        m_engine->tickVoiceTimer(frameCount);
        // ボイス終了検出 → ダッキングリリースまたは即時復帰
        // 判定は m_globalAtt > 0（実際にダッキングが適用されているか）で行う。
        // m_duckEnabled の動的変更に左右されない。
        auto state = m_voiceDuckState.load(std::memory_order_acquire);
        if (state == VoiceDuckState::Playing && !m_engine->isVoicePlaying()) {
            bool needRelease = (m_globalAtt > 0 && m_duckReleaseSamples > 0);
            auto next = needRelease ? VoiceDuckState::Releasing : VoiceDuckState::Idle;
            VoiceDuckState expected = VoiceDuckState::Playing;
            if (m_voiceDuckState.compare_exchange_strong(expected, next, std::memory_order_acq_rel)) {
                if (next == VoiceDuckState::Releasing) {
                    m_duckReleaseSamplesLeft = m_duckReleaseSamples;
                } else {
                    if (m_globalAtt != 0) setGlobalAttenuation(0);
                }
            }
        }
        // ダッキングリリース処理（徐々に減衰解除）
        state = m_voiceDuckState.load(std::memory_order_acquire);
        if (state == VoiceDuckState::Releasing) {
            if (frameCount >= m_duckReleaseSamplesLeft) {
                m_duckReleaseSamplesLeft = 0;
                m_voiceDuckState.store(VoiceDuckState::Idle, std::memory_order_release);
                setGlobalAttenuation(0);
            } else {
                m_duckReleaseSamplesLeft -= frameCount;
                float t = static_cast<float>(m_duckReleaseSamplesLeft) / m_duckReleaseSamples;
                int att = static_cast<int>(m_duckAttTarget * t);
                setGlobalAttenuation(att);
            }
        }
        // 安全弁: Idle状態なのにduckAttが残っている場合は強制復帰
        if (m_voiceDuckState.load(std::memory_order_acquire) == VoiceDuckState::Idle && m_duckAtt != 0) {
            setGlobalAttenuation(0);
        }
    }
    void stopAdpcmB() {
        if (!m_engine) return;
        m_engine->stopAdpcmB();
        auto prev = m_voiceDuckState.exchange(VoiceDuckState::Idle, std::memory_order_acq_rel);
        if (prev != VoiceDuckState::Idle) {
            setGlobalAttenuation(0);
        }
    }

    // ── SEモード設定 ──────────────────────────────────────
    // Classic: BGMチャンネルをハイジャックしてSE再生（デフォルト、後方互換）
    // Rich: 専用SEチップ（seEngine）でSE再生。seEngineはinit()済みであること。
    // 切り替え時に全SE停止。再生中でも呼び出し可能。
    void setSeMode(SeMode mode, IFmEngine* seEngine = nullptr)
    {
        stopAllSe();
        m_seMode = mode;
        if (mode == SeMode::Rich) {
            m_seEngine = seEngine;
            if (m_seEngine) {
                m_seEngine->reset();
                for (int fi = 0; fi < MAX_FM_CHANNELS; fi++) {
                    int port = fmPort(fi);
                    int off  = fmOffset(fi);
                    m_seEngine->writeReg(port, 0xB4 + off, 0xC0);
                }
                m_seEngine->writeReg(0, 0x07, 0x3F);
                m_seEngine->writeReg(0, 0x27, 0x3A);
                // BGMチップと同じSSGミックスバランスを適用
                if (m_engine)
                    m_seEngine->setSsgMixScale(m_engine->getSsgMixScale());
            }
        } else {
            m_seEngine = nullptr;
        }
    }
    [[nodiscard]] SeMode seMode() const { return m_seMode; }

    // ── SEシーケンスノート（マルチノート + ピッチスイープ）─
    struct SeSequenceNote {
        int startNote   = 60;   // 開始ノート番号
        int endNote     = -1;   // 終了ノート番号 (-1 = スイープなし)
        int durationMs  = 100;  // このノートのデュレーション(ms)
    };

    // ── 統合SE再生 ─────────────────────────────────────
    // Classic: BGMのFMチャンネルをハイジャックして再生
    // Rich: SEチップのFMチャンネルに割り当てて再生
    // patch: FM音色, noteNum: MIDIノート番号, velocity: 音量(0-15)
    // durationMs: 自動停止時間(ミリ秒)。0=手動停止のみ
    // 戻り値: SEスロット番号(0-5)。割り当て失敗時は -1
    [[nodiscard]] int playSe(const FmPatch& patch, int noteNum, int velocity = 15, int durationMs = 0)
    {
        if (m_seMode == SeMode::Rich)
            return playSeRich(patch, noteNum, velocity, durationMs);
        else
            return playSeClassic(patch, noteNum, velocity, durationMs);
    }

    // ── SEシーケンス再生（マルチノート + ピッチスイープ）─────
    // patch: FM音色, notes: ノート配列, noteCount: ノート数(1-8)
    // velocity: 音量(0-15)
    // 戻り値: SEスロット番号(0-5), -1=失敗
    [[nodiscard]] int playSeSequence(const FmPatch& patch, const SeSequenceNote* notes, int noteCount, int velocity = 15)
    {
        if (!notes || noteCount <= 0) return -1;
        noteCount = std::clamp(noteCount, 1, static_cast<int>(SeSlot::MAX_SEQ_NOTES));

        // 最初のノートのdurationMsでplaySe()と同じスロット確保・音色適用・キーオン
        int slotIdx = playSe(patch, notes[0].startNote, velocity, notes[0].durationMs);
        if (slotIdx < 0) return -1;

        // シーケンス情報を設定
        auto& slot = m_seSlots[slotIdx];
        slot.isSequence = true;
        slot.seqNoteCount = noteCount;
        slot.seqCurrentNote = 0;
        slot.seqVelocity = velocity;
        for (int i = 0; i < noteCount; i++)
            slot.seqNotes[i] = notes[i];

        return slotIdx;
    }

    // ── スロット指定SE発音 ─────────────────────────────────
    // slot: 使用するSEスロット番号(0-5)。使用中なら停止して上書き
    int playSeOnSlot(int slot, const FmPatch& patch, int noteNum, int velocity = 15, int durationMs = 0)
    {
        if (slot < 0 || slot >= MAX_SE_SLOTS) return -1;
        if (m_seMode == SeMode::Rich)
            return playSeRich(patch, noteNum, velocity, durationMs, slot);
        else
            return playSeClassic(patch, noteNum, velocity, durationMs, slot);
    }

    // ── スロット指定SEシーケンス再生 ───────────────────────
    // slot: 使用するSEスロット番号(0-5)。使用中なら停止して上書き
    int playSeSequenceOnSlot(int slot, const FmPatch& patch, const SeSequenceNote* notes, int noteCount, int velocity = 15)
    {
        if (!notes || noteCount <= 0) return -1;
        if (slot < 0 || slot >= MAX_SE_SLOTS) return -1;
        noteCount = std::clamp(noteCount, 1, (int)SeSlot::MAX_SEQ_NOTES);

        int slotIdx = playSeOnSlot(slot, patch, notes[0].startNote, velocity, notes[0].durationMs);
        if (slotIdx < 0) return -1;

        auto& s = m_seSlots[slotIdx];
        s.isSequence = true;
        s.seqNoteCount = noteCount;
        s.seqCurrentNote = 0;
        s.seqVelocity = velocity;
        for (int i = 0; i < noteCount; i++)
            s.seqNotes[i] = notes[i];

        return slotIdx;
    }

    void stopSe(int seSlot)
    {
        if (seSlot < 0 || seSlot >= MAX_SE_SLOTS) return;
        auto& slot = m_seSlots[seSlot];
        if (!slot.active) return;

        if (m_seMode == SeMode::Rich) {
            if (m_seEngine) m_seEngine->fmKeyOff(slot.fmIndex);
        } else {
            if (slot.mmlCh >= 0) {
                if (m_engine) fmKeyOff(toFMIndex(slot.mmlCh));
                releaseChannel(slot.mmlCh);
            }
        }
        slot = SeSlot{};
    }

    void stopAllSe()
    {
        for (int i = 0; i < MAX_SE_SLOTS; i++)
            stopSe(i);
    }

    void setSeFrequency(int seSlot, int noteNum)
    {
        if (seSlot < 0 || seSlot >= MAX_SE_SLOTS) return;
        auto& slot = m_seSlots[seSlot];
        if (!slot.active) return;
        IFmEngine* eng = (m_seMode == SeMode::Rich) ? m_seEngine : m_engine;
        if (eng) eng->setFrequency(slot.fmIndex, noteNum);
        slot.noteNum = noteNum;
    }

    [[nodiscard]] bool isSeActive(int seSlot) const
    {
        return (seSlot >= 0 && seSlot < MAX_SE_SLOTS) ? m_seSlots[seSlot].active : false;
    }

    [[nodiscard]] int activeSeCount() const
    {
        int n = 0;
        for (const auto& slot : m_seSlots)
            if (slot.active) n++;
        return n;
    }

    // ── ダッキング設定 ───────────────────────────────────
    // ボイス再生中にBGM全チャンネル（FM/SSG/ADPCM-A/ADPCM-B）の音量を自動減衰する。
    // attTarget: FM TL加算値（0=無効、20≈-15dB）。SSGはatt/4で換算。
    // releaseSec: ボイス終了後の減衰解除時間（秒）。0で即時復帰。
    // ボイス再生中はrecalcGlobalAtt()のガードでADPCM-Bレジスタ書き込みがスキップされ、
    // ボイスの音量はplayVoice()でmasterAttのみ適用される。
    void setDucking(int attTarget, float releaseSec = 0.15f) {
        m_duckAttTarget = attTarget;
        m_duckReleaseSamples = static_cast<uint32_t>(releaseSec * m_sampleRate);
        m_duckEnabled = (attTarget > 0);
    }

    // ── FM/SSG音量バランス（IFmEngineパススルー）──────────
    // ssgScale: SSG出力のリニアスケール（1.0=等倍、0.71≈-3dB）
    // MUCOM88Vデフォルト: 0.71（-3dB）
    void setSsgMixScale(float ssgScale) {
        if (m_engine) m_engine->setSsgMixScale(ssgScale);
        if (m_seEngine) m_seEngine->setSsgMixScale(ssgScale);
    }
    [[nodiscard]] float getSsgMixScale() const {
        return m_engine ? m_engine->getSsgMixScale() : 1.0f;
    }

    // ── 出力ゲイン ──────────────────────────────────
    // renderMixed() の最終段で PCM にゲインを適用しクリッピング。
    // fmgenの出力レベル補正（例: 2.0倍）等に使用。
    // play()/stop()でリセットされない（ゲームのオーディオ設定として永続）。
    void setOutputGain(float gain) { m_outputGain = gain; }
    [[nodiscard]] float getOutputGain() const { return m_outputGain; }

    // ── マスターボリューム ─────────────────────────────
    // vol: 0.0（無音）〜 1.0（最大）。FM TL減衰値に内部変換。
    // ダッキングやフェードとは独立。play()/stop()でリセットされない。
    void setMasterVolume(float vol) {
        m_masterAtt = static_cast<int>((1.0f - std::clamp(vol, 0.0f, 1.0f)) * 127.0f);
        recalcGlobalAtt();
        seRecalcVolume();
    }
    [[nodiscard]] float getMasterVolume() const {
        return 1.0f - static_cast<float>(m_masterAtt) / 127.0f;
    }

    // ── BGMボリューム ──────────────────────────────────
    // vol: 0.0（無音）〜 1.0（最大）。マスターボリュームと加算適用。
    // SE・ボイスには影響しない。play()/stop()でリセットされない。
    void setBgmVolume(float vol) {
        m_bgmAtt = static_cast<int>((1.0f - std::clamp(vol, 0.0f, 1.0f)) * 127.0f);
        recalcGlobalAtt();
    }
    [[nodiscard]] float getBgmVolume() const {
        return 1.0f - static_cast<float>(m_bgmAtt) / 127.0f;
    }

    // ── SEボリューム ───────────────────────────────────
    // vol: 0.0（無音）〜 1.0（最大）。マスターボリュームと加算適用。
    // play()/stop()でリセットされない。
    void setSeVolume(float vol) {
        m_seAtt = static_cast<int>((1.0f - std::clamp(vol, 0.0f, 1.0f)) * 127.0f);
        seRecalcVolume();
    }
    [[nodiscard]] float getSeVolume() const {
        return 1.0f - static_cast<float>(m_seAtt) / 127.0f;
    }

    // ── ボイスボリューム ──────────────────────────────────
    // vol: 0.0（無音）〜 1.0（最大）。マスターボリュームと加算適用。
    // play()/stop()でリセットされない（ゲーム設定として永続）。
    void setVoiceVolume(float vol) {
        m_voiceAtt = static_cast<int>((1.0f - std::clamp(vol, 0.0f, 1.0f)) * 127.0f);
    }
    [[nodiscard]] float getVoiceVolume() const {
        return 1.0f - static_cast<float>(m_voiceAtt) / 127.0f;
    }

    // ── フェードアウト/イン ──────────────────────────────
    // fadeOut: 指定秒数で無音まで減衰。フェード中にplay()/stop()するとリセット。
    // onComplete: フェードアウト完了時の自動アクション（デフォルト: None=何もしない）
    void fadeOut(float seconds, FadeAction onComplete = FadeAction::None) {
        m_fadeAction = onComplete;
        if (seconds <= 0.0f) {
            m_fadeAtt = 127;
            m_fading = false;
            recalcGlobalAtt();
            // 即時完了: FadeAction を実行
            if (m_fadeAction != FadeAction::None) {
                FadeAction action = m_fadeAction;
                m_fadeAction = FadeAction::None;
                executeFadeAction(action);
            }
            return;
        }
        m_fadeStartAtt  = m_fadeAtt;
        m_fadeTargetAtt = 127;
        m_fadeTotalSamples = static_cast<uint32_t>(seconds * m_sampleRate);
        m_fadeSamplesLeft  = m_fadeTotalSamples;
        m_fading = true;
    }
    // fadeIn: 指定秒数で現在のフェードレベルからマスターボリュームまで復帰。
    void fadeIn(float seconds) {
        if (seconds <= 0.0f) {
            m_fadeAtt = 0;
            m_fading = false;
            recalcGlobalAtt();
            return;
        }
        m_fadeStartAtt  = m_fadeAtt;
        m_fadeTargetAtt = 0;
        m_fadeTotalSamples = static_cast<uint32_t>(seconds * m_sampleRate);
        m_fadeSamplesLeft  = m_fadeTotalSamples;
        m_fading = true;
    }
    // resetFade: フェードを即座にキャンセルし、マスターボリュームに復帰。
    void resetFade() {
        m_fadeAtt = 0;
        m_fading = false;
        recalcGlobalAtt();
    }
    [[nodiscard]] bool isFading() const { return m_fading; }
    // フェードアウト完了後にFadeActionが実行されたかを返す。
    // play()でリセット。stop()ではリセットしない（呼び出し側がポーリングで検出するため）。
    [[nodiscard]] bool isFadeOutDone() const { return m_fadeOutDone; }

    // ── グローバル減衰（ダッキング用・後方互換）─────────────
    // att: FM TL加算値（0=通常、20≈-15dB）。ダッキング成分を設定。
    // マスターボリューム・フェードとは独立に加算される。
    void setGlobalAttenuation(int att)
    {
        m_duckAtt = att;
        recalcGlobalAtt();
    }
    [[nodiscard]] int globalAttenuation() const { return m_globalAtt; }

    // ── 時間を進める（MUCOM88互換: 全チャンネル同期クロック）─
    //
    // MUCOM88ではYM2608 Timer-BのINT3割り込みで全チャンネルが
    // 同期的に1クロック進む。これを再現するため、グローバルtickで
    // 全チャンネルのイベントを同時に処理する。
    //
    // Timer-B周期 = (256 - T) × 1152 / baseclock 秒
    // baseclock = 7987200 Hz (PC-8801)
    // samplesPerTick = (256 - T) × 1152 / 7987200 × sampleRate
    //               = (256 - T) × 1152 × sampleRate / 7987200
    void advance(uint32_t frameCount) noexcept
    {
        if (!m_playing || !m_engine) return;

        // OpenMUCOM88 完全互換 Timer-B タイミング
        //
        // fmgenの内部クロック計算:
        //   baseclock = 7987200 Hz (PC-8801 YM2608)
        //   fmgen SetRate() で clock /= 2 → 3993600
        //   fmclock = 3993600 / 6 / 12 = 55466.67
        //   timer_stepd = 1000.0 / fmclock * 16.0 = 0.28837... ms
        //
        // Timer-Bカウンタ:
        //   timerb = (int)((256 - T) * timer_stepd * 1024.0)
        //
        // OpenMUCOM88のCMucom::RenderAudio:
        //   16サンプルごとに AudioLeftMs += 16 * (1000.0 / sampleRate)
        //   整数ミリ秒分を UpdateTime(ms << 10) に渡す
        //   Timer-Bカウンタから (ms << 10) を減算、0以下でtick発生

        // フェード処理（サンプル単位で進行、テンポ非依存）
        if (m_fading) {
            if (frameCount >= m_fadeSamplesLeft) {
                m_fadeSamplesLeft = 0;
                m_fadeAtt = m_fadeTargetAtt;
                m_fading = false;
            } else {
                m_fadeSamplesLeft -= frameCount;
                float t = 1.0f - static_cast<float>(m_fadeSamplesLeft) / m_fadeTotalSamples;
                m_fadeAtt = m_fadeStartAtt + static_cast<int>((m_fadeTargetAtt - m_fadeStartAtt) * t);
            }
            recalcGlobalAtt();
        }
        // フェードアウト完了時の自動アクション実行
        // stop()がm_fadingをfalseにするため、FadeAction判定はstop()呼び出し前に行う
        if (!m_fading && m_fadeTargetAtt == 127 && m_fadeAction != FadeAction::None) {
            FadeAction action = m_fadeAction;
            m_fadeAction = FadeAction::None;
            executeFadeAction(action);
            return; // stop()済みのため、以降のTimer-B処理をスキップ
        }

        // 16サンプル単位で処理（OpenMUCOM88と同じ粒度）
        m_globalSampleAccum += frameCount;

        while (m_globalSampleAccum >= 16) {
            m_globalSampleAccum -= 16;
            m_audioLeftMs += 16.0 * 1000.0 / m_sampleRate;

            int ms = static_cast<int>(m_audioLeftMs);
            if (ms <= 0) continue;
            m_audioLeftMs -= ms;

            // Timer-B カウンタ更新（OpenMUCOM88 fmtimer.cpp Count() 互換）
            int tickUnits = ms << 10;  // ms * 1024 (TICK_SHIFT)
            m_timerBCount -= tickUnits;

            while (m_timerBCount <= 0) {
                m_timerBCount += m_timerBPeriod;
                m_globalTick++;

                // ── ループ判定 ──
                if (m_loop && m_commonEndTick > 0) {
                    if (m_perChannelLoop) {
                        // per-channel独立ループ（Issue #62）
                        // 初回globalLoopRestart後、各チャンネルは独立周期でループ。
                        // perChTickBase は perChannelRestart() 内で更新される
                        for (int ch2 = 0; ch2 < MAX_MML_CHANNELS; ch2++) {
                            auto& st2 = m_channels[ch2];
                            if (st2.events.empty() || !st2.hasLoopPoint) continue;
                            if (m_globalTick >= st2.nextRestartTick) {
                                perChannelRestart(ch2);
                                st2.nextRestartTick += st2.perChLoopLen;
                                // リスタート直後のイベントを即座に処理
                                processEvents(ch2, m_globalTick);
                            }
                        }
                    } else {
                        // 初回ループ: 全チャンネル同時リスタート（MaxCount互換）
                        uint32_t chTick = m_globalTick - m_loopTickOffset;
                        if (chTick >= m_commonEndTick) {
                            globalLoopRestart();
                            // ループ後のイベントを即座に処理
                            for (int ch2 = 0; ch2 < MAX_MML_CHANNELS; ch2++) {
                                if (m_channels[ch2].events.empty()) continue;
                                processEvents(ch2, m_globalTick);
                            }
                        }
                    }
                }

                // Z80 PLSET2: 毎tick Timer制御レジスタ書き込み（INT3ハンドラ先頭）
                // 通常モード: 0x3A、CSMエフェクトモード: 0x7A
                // Timer-Bオーバーフローフラグリセット + Timer制御の安定化
                if (m_engine) {
                    bool anyCsm = false;
                    for (int ch2 = 0; ch2 < MAX_MML_CHANNELS; ch2++)
                        if (m_channels[ch2].csmEnabled) { anyCsm = true; break; }
                    m_engine->writeReg(0, 0x27, anyCsm ? 0x7A : 0x3A);
                }

                // 全チャンネルを同じtickで同期処理（INT3割り込み相当）
                for (int ch = 0; ch < MAX_MML_CHANNELS; ch++) {
                    auto& st = m_channels[ch];
                    if (st.events.empty()) continue;
                    if (st.eventIdx >= st.events.size()) {
                        // Issue #68: Z80互換 — イベント消費済みチャンネルの即時リスタート
                        // Z80ドライバーは各チャンネルが独立にend marker到達→DATA TOPへジャンプ。
                        // MaxCountパディングは存在しない。globalLoopRestartを待たずに即時リスタート。
                        if (m_loop && st.hasLoopPoint && !m_perChannelLoop) {
                            // per-channelモードに移行（初回リスタート時）
                            // 全チャンネルのperChLoopLen/nextRestartTickを初期化
                            m_perChannelLoop = true;
                            for (int c = 0; c < MAX_MML_CHANNELS; c++) {
                                auto& sc = m_channels[c];
                                if (sc.events.empty() || !sc.hasLoopPoint) continue;
                                sc.perChLoopLen = sc.perChEndTick - sc.loopTick;
                                if (sc.perChLoopLen == 0) sc.perChLoopLen = 1;
                                // perChTickBase=0（初回パス: chTick = globalTick）
                                sc.perChTickBase = 0;
                                // nextRestartTickは自チャンネルのendTick
                                // （イベント消費済みチャンネルも含む — Issue #71）
                                sc.nextRestartTick = sc.perChEndTick;
                            }
                            // イベント消費済みの全チャンネルを即座にリスタート（Issue #71）
                            // Z80では各チャンネルが独立にend marker到達→DATA TOPジャンプ。
                            // 移行発動チャンネル(ch)だけでなく、同じtickで消費済みの他チャンネルも
                            // 即座にリスタートする。perChTickBaseはperChEndTick基準で計算し、
                            // イベント消費の検出が1tick遅れる問題を補正する。
                            for (int c = 0; c < MAX_MML_CHANNELS; c++) {
                                auto& sc = m_channels[c];
                                if (sc.events.empty() || !sc.hasLoopPoint) continue;
                                if (sc.eventIdx >= sc.events.size()) {
                                    perChannelRestart(c);
                                    // perChTickBase補正: perChannelRestart()は m_globalTick - loopTick を
                                    // 設定するが、イベント消費の検出が1tick遅れるため、実際のend tickを
                                    // 基準にした値に上書きする（Issue #71）
                                    sc.perChTickBase = sc.perChEndTick - sc.loopTick;
                                    sc.nextRestartTick = sc.perChEndTick + sc.perChLoopLen;
                                    processEvents(c, m_globalTick);
                                }
                            }
                        }
                        continue;
                    }
                    // ボイス再生中はKトラック(ch10)のイベント処理を抑制
                    if (!(isADPCMB(ch) && m_voiceDuckState.load(std::memory_order_acquire) != VoiceDuckState::Idle)) {
                        if (st.hijacked) {
                            advanceEventsSilent(ch, m_globalTick);
                        } else {
                            processEvents(ch, m_globalTick);
                            if (st.noteOn && st.lfoEnabled && st.lfoDepth != 0)
                                tickLfo(ch);
                            if (st.portaActive)
                                tickPortamento(ch);
                        }
                    }
                }

                // MUCOM88互換: SSGソフトウェアエンベロープ
                // 発音中: 音量を毎tick再書き込み（SOFENV互換）
                // リリース中: 音量を減衰させる（MUCOM88 SSSUBA互換）
                for (int ch = 0; ch < MAX_MML_CHANNELS; ch++) {
                    if (!isSSG(ch)) continue;
                    if (m_channels[ch].hijacked) continue;
                    auto& cst = m_channels[ch];
                    int si = toSSGIndex(ch);

                    if (cst.ssgSoftEnv) {
                        // ── MUCOM88 SOFENV互換 ADSRステートマシン ──
                        // Z80 SSSUB0: BIT 7,(IX+6) → エンベロープ有効フラグのみチェック
                        // Z80はnoteOnやphaseに関係なく、フラグが立っていれば毎tick書き込み。
                        // RELEASE完了後もenvValue=0のまま毎tick音量0を書き込み続ける。
                        // Z80互換: KEY_ON tickではSOFENV進行をスキップ（SOFEV7のみ）
                        // Z80 SSSUBG: envValue=AL → CALL SOFEV7（音量計算のみ）
                        // Z80 SSSUB0: CALL SOFENV（エンベロープ進行+音量計算）← 次tick以降
                        if (cst.ssgEnvKeyOnTick) {
                            cst.ssgEnvKeyOnTick = false;
                        } else {
                            ssgTickEnvelope(ch);
                        }
                        int vol = std::clamp(cst.volume - m_globalAtt / 4, 0, 15);
                        int amp = ((vol + 1) * cst.ssgEnvValue) >> 8;
                        // SOFEV7リバーブ（Z80 music.asm:2336-2342）:
                        // BIT 6,(IX+31): SSGではTIEフラグ（SET=発音中, RES=KEY_OFF後）
                        // RET NZ: TIEフラグSET(発音中)→リバーブ適用しない
                        // BIT 5,(IX+33): リバーブON→適用
                        // → KEY_OFF後(noteOn=false)かつリバーブONならamp = (amp+rv)>>1
                        if (cst.reverbEnabled && !cst.noteOn) {
                            amp = (amp + cst.reverbValue) >> 1;
                        }
                        if (amp > 15) amp = 15;
                        if (amp < 0) amp = 0;
                        m_engine->writeReg(0, 0x08 + si, static_cast<uint8_t>(amp));
                    } else if (cst.ssgReleasing) {
                        // Eコマンド未使用の簡易リリース
                        cst.ssgRelVol -= 2;
                        if (cst.ssgRelVol <= 0) {
                            cst.ssgRelVol = 0;
                            cst.ssgReleasing = false;
                        }
                        m_engine->writeReg(0, 0x08 + si, static_cast<uint8_t>(cst.ssgRelVol & 0x0F));
                    }
                }

                // MUCOM88互換: FMリバーブ毎tick TL書き込み（Z80 FS2互換）
                // Z80 FMSUB0: wait<q かつ リバーブON → FS2
                // FS2: C = ((IX+6) + (IX+17)) >> 1 → STV2（TOTALV加算なし）
                // IX+6は変更されない→毎tick同じ値を書く（定数、IIR減衰ではない）
                // Z80互換: チャンネルデータ終了後はFMSUB0が呼ばれないため停止
                for (int ch = 0; ch < MAX_MML_CHANNELS; ch++) {
                    if (!isFM(ch)) continue;
                    if (m_channels[ch].hijacked) continue;
                    auto& cst = m_channels[ch];
                    if (!cst.reverbActive) continue;
                    // チャンネル終了後は停止（Z80: データ終了後FMSUB0不呼び出し）
                    if (cst.eventIdx >= cst.events.size()) {
                        cst.reverbActive = false;
                        continue;
                    }
                    // Z80 FS2: C = (IX+6 + IX+17) >> 1 → STV2(FMVDAT[C])
                    // IX+6(volume)は不変→毎tick同じTL値を書く（定数、IIR減衰ではない）
                    fmSetReverbVolume(toFMIndex(ch), cst.volume, cst.reverbValue);
                }

                // テンポ変更 → Timer-B再計算
                recalcTimerB();
            }
        }

        // 全チャンネル終端 → 停止
        // - m_loop=false: 常に停止
        // - m_loop=true + commonEndTick>0: ループリスタートが処理するため停止しない
        // - m_loop=true + commonEndTick==0: L無し曲 → ループ不可、残留音防止のため停止
        if (!m_loop || m_commonEndTick == 0) {
            bool allDone = true;
            for (int ch = 0; ch < MAX_MML_CHANNELS; ch++) {
                if (m_channels[ch].events.empty()) continue;
                if (m_channels[ch].eventIdx < m_channels[ch].events.size()) {
                    allDone = false;
                    break;
                }
            }
            if (allDone) stop();
        }
    }

    // ── 混合レンダリング（BGM + SE）──────────────────────
    // advance() + tickVoiceTimer() + SE duration追跡 + 両チップPCM生成 + ミキシング。
    // 16サンプル単位で処理（OpenMUCOM88互換タイミング）。
    // ClassicモードでもRichモードでも使用可能。
    void renderMixed(int16_t* out, uint32_t frameCount) noexcept
    {
        uint32_t remaining = frameCount;
        uint32_t offset = 0;
        while (remaining > 0) {
            uint32_t n = std::min(remaining, static_cast<uint32_t>(16));
            advance(n);
            tickVoiceTimer(n);
            seTickDuration(n);

            int16_t bgmBuf[32] = {};
            if (m_engine)
                m_engine->generateInterleaved(bgmBuf, n);

            if (m_seMode == SeMode::Rich && m_seEngine) {
                // Richモード: BGM + SE ミキシング
                // ゲインはBGMのみに適用。SE は等倍で加算しヘッドルームを確保する。
                // SE音量は setSeVolume() + setMasterVolume() で調整可能。
                int16_t seBuf[32] = {};
                m_seEngine->generateInterleaved(seBuf, n);
                for (uint32_t i = 0; i < n * 2; i++) {
                    int32_t bgm = static_cast<int32_t>(bgmBuf[i]);
                    if (m_outputGain != 1.0f) bgm = static_cast<int32_t>(bgm * m_outputGain);
                    int32_t mixed = bgm + static_cast<int32_t>(seBuf[i]);
                    out[offset * 2 + i] = static_cast<int16_t>(std::clamp(mixed, static_cast<int32_t>(-32768), static_cast<int32_t>(32767)));
                }
            } else {
                // Classicモード: BGMのみ（+ ゲイン適用）
                if (m_outputGain != 1.0f) {
                    for (uint32_t i = 0; i < n * 2; i++) {
                        int32_t s = static_cast<int32_t>(bgmBuf[i] * m_outputGain);
                        out[offset * 2 + i] = static_cast<int16_t>(std::clamp(s, static_cast<int32_t>(-32768), static_cast<int32_t>(32767)));
                    }
                } else {
                    std::memcpy(out + offset * 2, bgmBuf, n * 2 * sizeof(int16_t));
                }
            }
            offset += n;
            remaining -= n;
        }
    }

    // ── 曲全体ループ: 全チャンネルをLポイントに同時巻き戻す ──
    // 注: Issue #68以降、advance()からは呼ばれない（per-channelループに移行）
    // 外部からの明示的呼び出し用に残す
    void globalLoopRestart()
    {
        // グローバルtickオフセット更新
        uint32_t loopLen = m_commonEndTick - m_commonLoopTick;
        if (loopLen == 0) loopLen = 1;
        m_loopTickOffset += loopLen;

        // SSGミキサーリセット（トーン有効、ノイズ無効）
        // チャンネルループの前に1回だけリセット（ループ内で上書きされるのを防止）
        m_ssgMixer = 0x38;

        // 全チャンネル KEY_OFF + 状態リセット + Lポイントへ巻き戻し
        for (int ch = 0; ch < MAX_MML_CHANNELS; ch++) {
            auto& st = m_channels[ch];
            if (st.events.empty()) continue;

            // KEY_OFF
            if (st.noteOn) {
                if      (isFM(ch))  fmKeyOff(toFMIndex(ch));
                else if (isSSG(ch)) ssgKeyOff(toSSGIndex(ch));
                st.noteOn = false;
            }

            // eventIdxをループポイントに設定
            if (st.hasLoopPoint) {
                st.eventIdx = st.loopEventIdx;
            } else {
                // Lコマンドがないチャンネル: ループせず沈黙を維持
                // Z80プレイヤーではLなしトラックはループ時に再生されない
                st.eventIdx = st.events.size();
            }
            // per-channelループオフセットをリセット（globalLoop時に全チャンネル同期）
            st.perChLoopOffset = 0;

            // ランタイム状態リセット（全可変状態をデフォルト値に復元）
            // Issue #19: ループ2周目以降のSSGピッチずれ修正
            st.currentNote  = 0;
            st.noteOnCount  = 0;  // UI activityカウンタもリセット
            st.reverbActive = false;
            st.portaActive  = false;
            st.csmEnabled   = false;
            st.csmDetune[0] = st.csmDetune[1] = st.csmDetune[2] = st.csmDetune[3] = 0;
            // ピッチ関連
            st.detune       = 0;
            st.lfoPitchOffset = 0;
            st.lfoDelayCounter = 0;
            st.lfoStepCounter  = 0;
            st.lfoRateCounter  = 0;
            st.lfoDirection    = 1;
            st.lfoEnabled   = false;
            st.lfoDelay     = 0;
            st.lfoRate      = 1;
            st.lfoDepth     = 0;
            st.lfoCount     = 0;
            // 音量・パン・スタッカート
            st.volume       = 12;
            st.pan          = 3;
            st.staccato     = 0;
            // SSGエンベロープ
            st.ssgSoftEnv   = false;
            st.ssgEnvMode   = false;
            st.ssgEnvAL = st.ssgEnvAR = st.ssgEnvDR = 0;
            st.ssgEnvSL = st.ssgEnvSR = st.ssgEnvRR = 0;
            st.ssgEnvPhase  = 0;
            st.ssgEnvValue  = 0;
            st.ssgEnvKeyOnTick = false;
            st.ssgReleasing = false;
            st.ssgRelVol    = 0;
            // リバーブ
            st.reverbValue    = 0;
            st.reverbEnabled  = false;
            st.reverbQCutOnly = false;

            // ループ再開位置までのイベントを走査して状態復元
            for (size_t i = 0; i < st.eventIdx; i++) {
                const auto& ev = st.events[i];
                switch (ev.type) {
                case MmlEventType::TEMPO:    m_globalTempo = ev.value; break;
                case MmlEventType::VOLUME:   st.volume = ev.value; break;
                case MmlEventType::PATCH:
                    if (isFM(ch))  m_fmPatchNo[toFMIndex(ch)] = ev.value;
                    if (isSSG(ch)) ssgApplyPreset(toSSGIndex(ch), ev.value);
                    break;
                case MmlEventType::PAN:       st.pan = ev.value; break;
                case MmlEventType::STACCATO:  st.staccato = ev.value; break;
                case MmlEventType::DETUNE:    st.detune = ev.value; break;
                case MmlEventType::VIBRATO:
                    st.lfoEnabled = true;
                    st.lfoDelay = ev.vibDelay; st.lfoRate = ev.vibRate;
                    st.lfoDepth = ev.vibDepth; st.lfoCount = ev.vibCount;
                    break;
                case MmlEventType::VIBRATO_SWITCH:
                    st.lfoEnabled = (ev.value != 0);
                    break;
                case MmlEventType::LFO_PARAM:
                    switch (ev.vibDelay) {
                        case 0: st.lfoDelay = ev.value; break;
                        case 1: st.lfoRate  = std::max(ev.value, 1); break;
                        case 2: st.lfoDepth = ev.value; break;
                        case 3: st.lfoCount = std::max(ev.value, 1); break;
                    }
                    break;
                case MmlEventType::REG_WRITE: {
                    int addr = ev.note;
                    int data = ev.value;
                    // SSGミキサー仮想アドレス（0xF0-0xF2）の復元
                    if (addr >= 0xF0 && addr <= 0xF2) {
                        int si = addr - 0xF0;
                        bool toneOn  = (data & 1) != 0;
                        bool noiseOn = (data & 2) != 0;
                        if (toneOn)  m_ssgMixer &= ~(1 << si);
                        else         m_ssgMixer |=  (1 << si);
                        if (noiseOn) m_ssgMixer &= ~(1 << (si+3));
                        else         m_ssgMixer |=  (1 << (si+3));
                    }
                    break;
                }
                case MmlEventType::PORTAMENTO: {
                    // ポルタメント状態の復元（processEvents側と同一ロジック）
                    int startNote = ev.note;
                    int endNote   = ev.value;
                    int dur       = static_cast<int>(ev.duration);
                    if (dur <= 0) dur = 1;
                    int sb = 0, eb = 0;
                    uint16_t sf = noteToFnum(startNote, sb);
                    uint16_t ef = noteToFnum(endNote, eb);
                    int startBF = (sb << 11) | sf;
                    int endBF   = (eb << 11) | ef;
                    st.portaActive      = true;
                    st.portaStartFnum   = startBF;
                    st.portaEndFnum     = endBF;
                    st.portaCurrentFnum = startBF;
                    st.portaTicksLeft   = dur;
                    st.portaStep        = (endBF - startBF) / dur;
                    break;
                }
                case MmlEventType::HARDWARE_LFO:
                    // HW LFO設定はレジスタ書き込みのみ（復元はplay後の再生で行われる）
                    // ここではst側の状態変更がないのでbreak
                    break;
                case MmlEventType::CSM_MODE:
                    st.csmDetune[0] = ev.vibDelay; st.csmDetune[1] = ev.vibRate;
                    st.csmDetune[2] = ev.vibDepth; st.csmDetune[3] = ev.vibCount;
                    st.csmEnabled = !(st.csmDetune[0] == 0 && st.csmDetune[1] == 0 &&
                                      st.csmDetune[2] == 0 && st.csmDetune[3] == 0);
                    break;
                case MmlEventType::REVERB_ENVELOPE:
                    st.reverbValue = ev.value; st.reverbEnabled = true; break;
                case MmlEventType::REVERB_SWITCH:
                    st.reverbEnabled = (ev.value != 0); break;
                case MmlEventType::REVERB_MODE:
                    st.reverbQCutOnly = (ev.value != 0); break;
                case MmlEventType::SSG_ENVELOPE:
                    st.ssgSoftEnv = true;
                    st.ssgEnvAL = ev.envAL; st.ssgEnvAR = ev.envAR;
                    st.ssgEnvDR = ev.envDR; st.ssgEnvSL = ev.envSL;
                    st.ssgEnvSR = ev.envSR; st.ssgEnvRR = ev.envRR;
                    break;
                default: break;
                }
            }
        }

        // Timer-B再計算 + 音色/PAN/音量復元（ハイジャック中チャンネルはスキップ）
        recalcTimerB();
        for (int fi = 0; fi < MAX_FM_CHANNELS; fi++) {
            if (m_channels[fmMmlCh(fi)].hijacked) continue;
            fmApplyPatch(fi, m_fmPatchNo[fi]);
        }
        for (int fi = 0; fi < MAX_FM_CHANNELS; fi++) {
            int ch = fmMmlCh(fi);
            if (m_channels[ch].hijacked) continue;
            fmSetVolume(fi, m_channels[ch].volume);
        }
        for (int ch = 0; ch < MAX_MML_CHANNELS; ch++) {
            if (isFM(ch)) {
                if (m_channels[ch].hijacked) continue;
                int fi = toFMIndex(ch);
                int port = fmPort(fi);
                int off  = fmOffset(fi);
                m_engine->writeReg(port, 0xB4 + off,
                    static_cast<uint8_t>(panToReg(m_channels[ch].pan)));
            }
        }
        m_engine->writeReg(0, 0x07, m_ssgMixer);
        // ADPCM-B: ボイス再生中はスキップ（ボイスの音量はplayVoice()で管理）
        if (m_voiceDuckState.load(std::memory_order_acquire) == VoiceDuckState::Idle) {
            adpcmbSetVolume(m_channels[10].volume);
        }

        // per-channel独立ループの初期化（Issue #62/#68）
        // 初回のglobalLoopRestartで全チャンネル一斉に巻き戻した後、
        // 2回目以降は各チャンネルが独立した周期（commonEndTick - loopTick）でループする。
        m_perChannelLoop = true;
        for (int ch = 0; ch < MAX_MML_CHANNELS; ch++) {
            auto& st = m_channels[ch];
            if (st.events.empty() || !st.hasLoopPoint) continue;
            st.perChLoopLen = m_commonEndTick - st.loopTick;
            if (st.perChLoopLen == 0) st.perChLoopLen = 1;
            // perChTickBase: tick - perChTickBase で chTick(=loopTick起点) を算出
            st.perChTickBase = m_globalTick - st.loopTick;
            st.nextRestartTick = m_globalTick + st.perChLoopLen;
        }
    }

    // ── per-channelリスタート: 単一チャンネルをLポイントに巻き戻す ──
    // Z80互換: 各チャンネルが独立にend marker到達→DATA TOPジャンプ
    void perChannelRestart(int ch)
    {
        auto& st = m_channels[ch];

        // KEY_OFF
        if (st.noteOn) {
            if      (isFM(ch))  fmKeyOff(toFMIndex(ch));
            else if (isSSG(ch)) ssgKeyOff(toSSGIndex(ch));
            st.noteOn = false;
        }

        // eventIdx巻き戻し
        st.eventIdx = st.loopEventIdx;

        // ランタイム状態リセット（globalLoopRestartと同一）
        st.currentNote  = 0;
        st.noteOnCount  = 0;
        st.reverbActive = false;
        st.portaActive  = false;
        st.csmEnabled   = false;
        st.csmDetune[0] = st.csmDetune[1] = st.csmDetune[2] = st.csmDetune[3] = 0;
        st.detune       = 0;
        st.lfoPitchOffset = 0;
        st.lfoDelayCounter = 0;
        st.lfoStepCounter  = 0;
        st.lfoRateCounter  = 0;
        st.lfoDirection    = 1;
        st.lfoEnabled   = false;
        st.lfoDelay     = 0;
        st.lfoRate      = 1;
        st.lfoDepth     = 0;
        st.lfoCount     = 0;
        st.volume       = 12;
        st.pan          = 3;
        st.staccato     = 0;
        st.ssgSoftEnv   = false;
        st.ssgEnvMode   = false;
        st.ssgEnvAL = st.ssgEnvAR = st.ssgEnvDR = 0;
        st.ssgEnvSL = st.ssgEnvSR = st.ssgEnvRR = 0;
        st.ssgEnvPhase  = 0;
        st.ssgEnvValue  = 0;
        st.ssgEnvKeyOnTick = false;
        st.ssgReleasing = false;
        st.ssgRelVol    = 0;
        st.reverbValue    = 0;
        st.reverbEnabled  = false;
        st.reverbQCutOnly = false;

        // SSGミキサーリセット（このチャンネルがSSGの場合）
        if (isSSG(ch)) {
            int si = toSSGIndex(ch);
            m_ssgMixer |= (1 << si);       // トーン無効
            m_ssgMixer |= (1 << (si + 3)); // ノイズ無効
        }

        // ループ再開位置までのイベントを走査して状態復元
        for (size_t i = 0; i < st.eventIdx; i++) {
            const auto& ev = st.events[i];
            switch (ev.type) {
            case MmlEventType::VOLUME:   st.volume = ev.value; break;
            case MmlEventType::PATCH:
                if (isFM(ch))  m_fmPatchNo[toFMIndex(ch)] = ev.value;
                if (isSSG(ch)) ssgApplyPreset(toSSGIndex(ch), ev.value);
                break;
            case MmlEventType::PAN:       st.pan = ev.value; break;
            case MmlEventType::STACCATO:  st.staccato = ev.value; break;
            case MmlEventType::DETUNE:    st.detune = ev.value; break;
            case MmlEventType::VIBRATO:
                st.lfoEnabled = true;
                st.lfoDelay = ev.vibDelay; st.lfoRate = ev.vibRate;
                st.lfoDepth = ev.vibDepth; st.lfoCount = ev.vibCount;
                break;
            case MmlEventType::VIBRATO_SWITCH:
                st.lfoEnabled = (ev.value != 0); break;
            case MmlEventType::LFO_PARAM:
                switch (ev.vibDelay) {
                    case 0: st.lfoDelay = ev.value; break;
                    case 1: st.lfoRate  = std::max(ev.value, 1); break;
                    case 2: st.lfoDepth = ev.value; break;
                    case 3: st.lfoCount = std::max(ev.value, 1); break;
                }
                break;
            case MmlEventType::REG_WRITE: {
                int addr = ev.note;
                int data = ev.value;
                if (addr >= 0xF0 && addr <= 0xF2) {
                    int si = addr - 0xF0;
                    bool toneOn  = (data & 1) != 0;
                    bool noiseOn = (data & 2) != 0;
                    if (toneOn)  m_ssgMixer &= ~(1 << si);
                    else         m_ssgMixer |=  (1 << si);
                    if (noiseOn) m_ssgMixer &= ~(1 << (si+3));
                    else         m_ssgMixer |=  (1 << (si+3));
                }
                break;
            }
            case MmlEventType::PORTAMENTO: {
                int startNote = ev.note;
                int endNote   = ev.value;
                int dur       = static_cast<int>(ev.duration);
                if (dur <= 0) dur = 1;
                int sb = 0, eb = 0;
                uint16_t sf = noteToFnum(startNote, sb);
                uint16_t ef = noteToFnum(endNote, eb);
                int startBF = (sb << 11) | sf;
                int endBF   = (eb << 11) | ef;
                st.portaActive      = true;
                st.portaStartFnum   = startBF;
                st.portaEndFnum     = endBF;
                st.portaCurrentFnum = startBF;
                st.portaTicksLeft   = dur;
                st.portaStep        = (endBF - startBF) / dur;
                break;
            }
            case MmlEventType::CSM_MODE:
                st.csmDetune[0] = ev.vibDelay; st.csmDetune[1] = ev.vibRate;
                st.csmDetune[2] = ev.vibDepth; st.csmDetune[3] = ev.vibCount;
                st.csmEnabled = !(st.csmDetune[0] == 0 && st.csmDetune[1] == 0 &&
                                  st.csmDetune[2] == 0 && st.csmDetune[3] == 0);
                break;
            case MmlEventType::REVERB_ENVELOPE:
                st.reverbValue = ev.value; st.reverbEnabled = true; break;
            case MmlEventType::REVERB_SWITCH:
                st.reverbEnabled = (ev.value != 0); break;
            case MmlEventType::REVERB_MODE:
                st.reverbQCutOnly = (ev.value != 0); break;
            case MmlEventType::SSG_ENVELOPE:
                st.ssgSoftEnv = true;
                st.ssgEnvAL = ev.envAL; st.ssgEnvAR = ev.envAR;
                st.ssgEnvDR = ev.envDR; st.ssgEnvSL = ev.envSL;
                st.ssgEnvSR = ev.envSR; st.ssgEnvRR = ev.envRR;
                break;
            default: break;
            }
        }

        // 音色/PAN/音量復元（ハイジャック中はスキップ）
        if (!st.hijacked) {
            if (isFM(ch)) {
                int fi = toFMIndex(ch);
                fmApplyPatch(fi, m_fmPatchNo[fi]);
                fmSetVolume(fi, st.volume);
                int port = fmPort(fi);
                int off  = fmOffset(fi);
                m_engine->writeReg(port, 0xB4 + off,
                    static_cast<uint8_t>(panToReg(st.pan)));
            }
            if (isSSG(ch)) {
                m_engine->writeReg(0, 0x07, m_ssgMixer);
            }
            if (isADPCMB(ch)) {
                if (m_voiceDuckState.load(std::memory_order_acquire) == VoiceDuckState::Idle) {
                    adpcmbSetVolume(st.volume);
                }
            }
        }

        // per-channel tick base更新
        st.perChTickBase = m_globalTick - st.loopTick;
    }

private:
    struct ChannelState {
        std::vector<MmlEvent> events;
        size_t   eventIdx    = 0;
        bool     noteOn      = false;
        uint32_t noteOnCount = 0;     // noteOnトリガーカウンター（UI activity検出用）
        int      currentNote = 0;
        int      staccato    = 0;
        int      volume      = 12;
        int      pan         = 3;   // パン（0=off, 1=right, 2=left, 3=center）
        bool     ssgEnvMode  = false; // SSGハードウェアエンベロープ（@N）
        // SSGソフトウェアエンベロープ（Eコマンド、MUCOM88 SOFENV互換）
        bool     ssgSoftEnv   = false; // Eコマンドで有効化
        int      ssgEnvAL     = 0;     // n1: Attack Level (初期値)
        int      ssgEnvAR     = 0;     // n2: Attack Rate
        int      ssgEnvDR     = 0;     // n3: Decay Rate
        int      ssgEnvSL     = 0;     // n4: Sustain Level (Decay目標値)
        int      ssgEnvSR     = 0;     // n5: Sustain Rate (毎tick減分, 0=保持)
        int      ssgEnvRR     = 0;     // n6: Release Rate
        // ADSR状態: 0=OFF, 1=ATTACK, 2=DECAY, 3=SUSTAIN, 4=RELEASE
        int      ssgEnvPhase  = 0;
        int      ssgEnvValue  = 0;     // 現在のエンベロープ値（0-255）
        bool     ssgEnvKeyOnTick = false; // Z80互換: KEY_ON tickではSOFENV進行をスキップ
        bool     ssgReleasing = false; // リリース中フラグ（Eコマンド未使用時の簡易版）
        int      ssgRelVol    = 0;     // リリース中の現在音量（簡易版）
        // デチューン: F-Numberオフセット（D コマンド）
        int      detune      = 0;
        // ソフトウェアLFO（M コマンド）
        bool     lfoEnabled  = false;   // MF1=true, MF0=false
        int      lfoDelay    = 0;       // 遅延（クロック数）
        int      lfoRate     = 1;       // クロック単位（何クロックごとに1ステップ）
        int      lfoDepth    = 0;       // 1ステップあたりのF-Number変化量
        int      lfoCount    = 0;       // 反転までのステップ数
        // LFO ランタイム状態
        int      lfoDelayCounter = 0;   // 遅延カウントダウン
        int      lfoStepCounter  = 0;   // 現在のステップ位置（0〜lfoCount）
        int      lfoRateCounter  = 0;   // レートカウンタ（0〜lfoRate）
        int      lfoDirection    = 1;   // 現在の進行方向（+1 or -1）
        int      lfoPitchOffset  = 0;   // 現在のピッチオフセット（F-Number単位）
        // ポルタメント（{}コマンド、MUCOM88 CULPTM互換）
        bool     portaActive    = false;   // ポルタメント中
        int      portaStartFnum = 0;       // 開始F-Number（block込み: block<<11 | fnum）
        int      portaEndFnum   = 0;       // 終了F-Number
        int      portaTicksLeft = 0;       // 残りtick数
        int      portaStep      = 0;       // 毎tickのF-Number増分（符号あり）
        int      portaCurrentFnum = 0;     // 現在のF-Number
        // リバーブ（Rコマンド、MUCOM88 REVERVE/REVSW/REVMOD互換）
        int      reverbValue    = 0;     // リバーブ音量加減値（IX+17）
        bool     reverbEnabled  = false; // リバーブON/OFF（IX+33 bit5）
        bool     reverbQCutOnly = false; // リバーブモード: true=qカット部分のみ（IX+33 bit4）
        bool     reverbActive   = false; // KEY_OFF済み=リバーブ減衰中
        int      reverbCurrentVol = 0;   // リバーブ減衰中の現在ボリューム（Z80 TOTALV互換、毎tick更新）
        // ループポイント（Lコマンド）
        size_t   loopEventIdx   = 0;   // ループ再開時のイベントインデックス
        uint32_t loopTick       = 0;   // ループ再開時のtick
        bool     hasLoopPoint   = false;
        // per-channelループ（Z80互換: 各チャンネルが独立してLポイントに戻る）
        uint32_t perChEndTick      = 0;  // チャンネル固有の終端tick
        uint32_t perChLoopOffset   = 0;  // per-channelループの累積tickオフセット
        // per-channel独立ループ（Issue #62: 初回globalLoopRestart後に有効化）
        uint32_t perChLoopLen      = 0;  // チャンネル固有のループ周期 (commonEndTick - loopTick)
        uint32_t perChTickBase     = 0;  // chTick計算用ベース (tick - perChTickBase = chTick - loopTick)
        uint32_t nextRestartTick   = 0;  // 次のper-channelリスタートの絶対tick
        // CSMモード（Sコマンド、FM ch3 エフェクトモード）
        // Z80 MDSET→TO_EFC/EXMODE: 毎tickで4オペレータ独立F-Number + 4回KEY ON
        bool     csmEnabled     = false;
        int      csmDetune[4]   = {0, 0, 0, 0};  // OP1-OP4 デチューンオフセット
        // チャンネルハイジャック（SE割り込み用）
        bool     hijacked       = false;
    };

    // ── SEスロット状態 ─────────────────────────────────
    struct SeSlot {
        bool     active         = false;   // スロット使用中
        uint32_t allocOrder     = 0;       // 割り当て順序（oldest判定用、単調増加）
        int      fmIndex        = -1;      // SEチップ上のFMインデックス (0-5)
        int      mmlCh          = -1;      // Classicモード: ハイジャック中のMMLチャンネル (-1=Rich)
        uint32_t durationSamples = 0;      // 自動停止までのサンプル数 (0=手動のみ)
        uint32_t samplesLeft    = 0;       // 残りサンプル数
        FmPatch  patch;                    // SE再生中の音色（TL再計算用）
        int      noteNum        = 0;       // 再生中のノート番号
        int      velocity       = 15;      // ベロシティ (0-15)
        // シーケンス再生（マルチノート + ピッチスイープ）
        static constexpr int MAX_SEQ_NOTES = 8;
        std::array<SeSequenceNote, MAX_SEQ_NOTES> seqNotes;  // シーケンスノート配列
        int  seqNoteCount   = 0;    // ノート数
        int  seqCurrentNote = 0;    // 現在再生中のノートインデックス
        int  seqVelocity    = 15;   // シーケンス全体のベロシティ
        bool isSequence     = false; // シーケンス再生中か
    };

    IFmEngine*  m_engine;
    uint32_t    m_sampleRate;
    uint32_t    m_chipClock;  // YM2608マスタークロック（Issue #22）
    bool        m_playing = false;
    bool        m_loop    = false;
    // グローバル同期クロック（MUCOM88 INT3割り込み相当）
    uint32_t    m_globalTick        = 0;
    uint32_t    m_globalSampleAccum = 0;
    int         m_globalTempo       = 120;  // Timer-B値（BPMではない）
    // OpenMUCOM88完全互換 Timer-Bカウンタ（int: fmgenのtruncation挙動を再現）
    double      m_audioLeftMs       = 0.0;
    int         m_timerBCount       = 0;
    int         m_timerBPeriod      = 0;  // (256-T) * timer_stepd * 1024 (int truncation)
    int         m_wholeTick         = 128;  // Cコマンド（デフォルトC128）
    // 曲全体ループ（OpenMUCOM88 maxcount 互換）
    uint32_t    m_commonEndTick     = 0;    // 全チャンネル共通のループ終端tick
    uint32_t    m_overrideEndTick  = 0;    // 外部から指定されたループ終端tick（0=未指定）
    uint32_t    m_commonLoopTick    = 0;    // 全チャンネル共通のLコマンドtick
    uint32_t    m_loopTickOffset    = 0;    // ループ巻き戻しの累積tickオフセット
    bool        m_perChannelLoop   = false; // per-channel独立ループモード（初回restart後true）
    std::array<ChannelState, MAX_MML_CHANNELS> m_channels;
    std::unordered_map<int, FmPatch>           m_patchMap;
    std::array<int, MAX_FM_CHANNELS>           m_fmPatchNo;   // FM音色番号
    uint8_t     m_ssgMixer   = 0x3F;  // SSG mixer shadow (active-low)
    uint8_t     m_rhythmMask = 0;     // リズム楽器マスク（@N で設定）
    uint8_t     m_rhythmTL   = 0x3F;  // リズム全体音量TL（vコマンドで設定、0x3F=最大）
    // 楽器別 Individual Level レジスタ（0x18-0x1D）: bit7-6=PAN, bit4-0=Level
    // yコマンドやpコマンドで更新される。rhythmKeyOnで毎回書き込む。
    std::array<uint8_t, 6> m_rhythmIL = {0xDF,0xDF,0xDF,0xDF,0xDF,0xDF}; // L+R + level 31
    int         m_globalAtt  = 0;     // 合算減衰値（FM TL加算値、0=通常）= masterAtt + fadeAtt + duckAtt
    // 3層減衰アーキテクチャ: 各成分は独立に設定され、合算値がレジスタ書き込みに使用される
    int         m_masterAtt  = 0;    // マスターボリューム減衰（0=最大、127=無音）
    int         m_bgmAtt     = 0;    // BGM専用減衰（0=最大、127=無音）
    int         m_seAtt      = 0;    // SE専用ボリューム減衰（0=最大、127=無音）
    int         m_voiceAtt   = 0;    // ボイス専用減衰（0=最大、127=無音）
    float       m_outputGain = 1.0f; // 出力ゲイン（renderMixed最終段、play()/stop()でリセットしない）
    int         m_fadeAtt    = 0;    // フェードアウト減衰（0=フェードなし、127=無音）
    int         m_duckAtt    = 0;    // ダッキング減衰（0=ダッキングなし）
    // フェードアウト/イン状態
    bool        m_fading           = false;
    int         m_fadeStartAtt     = 0;     // フェード開始時のfadeAtt値
    int         m_fadeTargetAtt    = 0;     // フェード目標のfadeAtt値
    uint32_t    m_fadeTotalSamples = 0;     // フェード全体のサンプル数
    uint32_t    m_fadeSamplesLeft  = 0;     // フェード残りサンプル数
    FadeAction  m_fadeAction       = FadeAction::None;  // フェードアウト完了時の自動アクション
    bool        m_fadeOutDone      = false;              // FadeAction実行済みフラグ（play()でリセット）
    // ボイス再生＋ダッキング状態マシン（スレッド安全）
    // ゲームスレッド: playVoice/stopVoice で store/exchange
    // オーディオスレッド: tickVoiceTimer で CAS 遷移
    std::atomic<VoiceDuckState> m_voiceDuckState{VoiceDuckState::Idle};
    static_assert(std::atomic<VoiceDuckState>::is_always_lock_free,
                  "VoiceDuckState must be lock-free for real-time audio thread");
    bool        m_duckEnabled = false;   // ダッキング機能ON/OFF
    int         m_duckAttTarget = 20;    // 減衰量（FM TL加算値、20≈-15dB）
    uint32_t    m_duckReleaseSamples = 0;     // リリース時間（サンプル数）
    uint32_t    m_duckReleaseSamplesLeft = 0;  // リリース残りサンプル数（オーディオスレッドのみ更新）
    // ── SE（効果音）再生 ────────────────────────────────
    static constexpr int MAX_SE_SLOTS = 6;
    std::array<SeSlot, MAX_SE_SLOTS> m_seSlots;
    IFmEngine*  m_seEngine       = nullptr;   // Richモード: SE専用チップ
    SeMode      m_seMode         = SeMode::Classic;
    uint32_t    m_seAllocCounter = 0;         // スロット割り当て順序（単調増加）
    int         m_pcmVolMode = 0;     // PVMODE: 0=IX+6のみ使用, 1=IX+6+IX+7
    int         m_pcmAddVol  = 0;     // ADPCM-B追加音量（PVMODE=1時のIX+7、V1→v設定）

    // ADPCM-B音楽チャンネル（Kトラック、ch10）
    // mucompcm.bin PCMADRテーブル: 8バイト/エントリ (startAddr, endAddr, reserved, param)
    static constexpr int MAX_PCM_VOICES = 32;
    struct PcmVoiceEntry {
        uint16_t startAddr = 0;
        uint16_t endAddr   = 0;
        uint16_t param     = 0;  // mucompcm.bin offset 26 のパラメータ
    };
    std::array<PcmVoiceEntry, MAX_PCM_VOICES> m_pcmTable;
    int      m_pcmVoiceCount = 0;
    bool     m_pcmLoaded     = false;
    int      m_pcmCurrentNum = 0;  // 現在のPCMサンプル番号（@Nで設定）
    uint8_t  m_pcmPan        = 0xC0;  // L+R

    // FM index → MML channel 逆引き
    static int fmMmlCh(int fi) { return (fi < 3) ? fi : (fi - 3 + 7); }

    // =====================================================================
    // イベント処理（チャンネル種別で分岐）
    // =====================================================================
    void processEvents(int ch, uint32_t tick)
    {
        auto& st = m_channels[ch];
      {
        // チャンネルtick計算: per-channelループモード時はチャンネル固有のtick baseを使用
        // per-channel: chTick = tick - perChTickBase で、loopTick起点にマッピング
        // global: chTick = tick - m_loopTickOffset
        uint32_t chTick = (m_perChannelLoop && st.hasLoopPoint)
                        ? (tick - st.perChTickBase)
                        : (tick - m_loopTickOffset);
        while (st.eventIdx < st.events.size()) {
            const MmlEvent& ev = st.events[st.eventIdx];
            // commonEndTickを超えるイベントはスキップ（非破壊打ち切り、libmucom88-mml#2）
            // 同一tickのイベントは処理する（libmucom88-mml#3: >= → >）
            if (m_commonEndTick > 0 && ev.tick > m_commonEndTick) {
                // libmucom88-mml#5: SSG残留音防止
                // ブラケットループ展開でcommonEndTickを超えるNOTE_OFFがスキップされ
                // SSGが発音したまま残る問題を修正。Z80ではglobalLoopRestartで
                // 全チャンネルがKEY_OFFされるため、ここで明示的に消音する。
                if (st.noteOn) {
                    if (isSSG(ch)) {
                        if (st.ssgSoftEnv) {
                            st.ssgEnvPhase = 4;  // RELEASE
                        } else {
                            ssgKeyOff(toSSGIndex(ch));
                        }
                        st.noteOn = false;
                    } else if (isFM(ch)) {
                        fmKeyOff(toFMIndex(ch));
                        st.noteOn = false;
                    }
                }
                st.eventIdx = st.events.size();  // 残りイベントを全スキップ
                break;
            }
            if (ev.tick > chTick) break;

            switch (ev.type) {
            case MmlEventType::NOTE_ON:
                // Z80 FMSUB5→FMSUB4 タイ判定（music.asm line 529-557）:
                // FMSUB1は毎回SET 6,(IX+31)でKEYOFF_FLAG=1を設定する。
                // KEYOFF_FLAG=1の場合:
                //   FMSUB5: CALL NZ,KEYOFF → KEY_OFF実行（常に）
                //   FMSUB4: JR NZ,FMSUB9 → KEY_ON実行（常に）
                // KEYOFF_FLAG=0は0xFDカウントオーバー後のみ（通常到達しない）。
                //
                // したがってFMチャンネルでは、NOTE_ON時に必ずKEY_OFF→KEY_ONを行う。
                // SSGはTIEフラグON時にキーオフせず周波数だけ変更（SSSUB6互換）。
                if (st.noteOn && isSSG(ch)) {
                    // SSG: key-offせず周波数と音量を更新するだけ
                    // Z80 SSSUB6: TIEフラグON時はキーオフせずに周波数だけ変更
                    doKeyOn(ch, ev.note, ev.velocity);
                } else {
                    // Z80 FMSUB5: BIT 6,(IX+31) / CALL NZ,KEYOFF
                    if (st.reverbActive && isFM(ch)) {
                        doKeyOff(ch);
                    } else if (st.noteOn) {
                        doKeyOff(ch);
                    }
                    // Z80: FMSUB5→FMSUB4→FMSUB7→KEYON
                    // KEYON後: リバーブON時のみ STVOL（music.asm line 745-746）
                    if (ch == 2 && st.csmEnabled) {
                        // CSMモード: EXMODE互換 — 4オペレータ独立F-Number + 4回KEY ON
                        csmKeyOn(ch, ev.note, ev.velocity);
                    } else {
                        doKeyOn(ch, ev.note, ev.velocity);
                    }
                    if (isFM(ch) && st.reverbEnabled) {
                        doSetVolume(ch, st.volume);
                    }
                }
                st.noteOn       = true;
                st.noteOnCount++;
                st.currentNote  = ev.note;
                st.ssgReleasing = false;
                st.reverbActive = false;
                // SSGソフトウェアエンベロープ: ATTACK開始
                // Z80互換: SSSUBG→SOFEV7（音量計算のみ、エンベロープ進行なし）
                // KEY_ON tickではSOFENV進行をスキップし、SOFEV7相当の計算のみ行う
                if (isSSG(ch) && st.ssgSoftEnv) {
                    st.ssgEnvValue = st.ssgEnvAL;
                    st.ssgEnvPhase = 1;  // ATTACK
                    st.ssgEnvKeyOnTick = true;  // このtickではSOFENV進行スキップ
                }
                break;
            case MmlEventType::NOTE_OFF:
                if (st.noteOn && st.currentNote == ev.note) {
                    // Z80 FMSUB: wait==0でFMSUB1に直行（q=0でもFMSUB0はスキップ）
                    // FMSUB1→FMSUB5で KEY_OFF が実行されるため、
                    // 同tickの NOTE_OFF→NOTE_ON では NOTE_OFF 側の KEY_OFF は冗長。
                    // SSG: 同tick KEY_OFF→KEY_ON でクリックノイズ防止のためスキップ。
                    bool skipKeyOff = false;
                    if (isSSG(ch) && st.eventIdx + 1 < st.events.size()) {
                        const auto& next = st.events[st.eventIdx + 1];
                        if (next.tick == ev.tick && next.type == MmlEventType::NOTE_ON)
                            skipKeyOff = true;
                    }
                    if (!skipKeyOff) {
                        if (isFM(ch) && st.reverbEnabled) {
                            // FM リバーブ: KEY_OFFの代わりにFS2で音量設定
                            // Z80 FS2: C = (IX+6 + IX+17) >> 1 → STV2（TOTALV加算なし）
                            // IX+6は変更されない→毎tick同じ値を書く（定数）
                            fmSetReverbVolume(toFMIndex(ch), st.volume, st.reverbValue);
                            st.reverbActive = true;
                            // Z80互換: KEYOFFフラグ(IX+31 bit6)セットのみ
                            // noteOnは変更しない（LFO/ピッチ更新を継続するため）
                            // 実際のKEY_OFFはFMSUB5で次のノート開始時に行われる
                        } else if (isSSG(ch) && st.ssgSoftEnv) {
                            // SSGソフトウェアエンベロープ: RELEASE開始
                            st.ssgEnvPhase = 4;  // RELEASE
                            st.noteOn = false;
                        } else if (isSSG(ch)) {
                            // Eコマンド未使用時の簡易版リリース
                            st.ssgReleasing = true;
                            st.ssgRelVol = std::clamp(st.volume - m_globalAtt / 4, 0, 15);
                            st.noteOn = false;
                        } else {
                            doKeyOff(ch);
                            st.noteOn = false;
                        }
                    }
                }
                break;
            case MmlEventType::TIE_KEYOFF:
                // Z80 FMSUB3互換: ^タイ境界でのKEY_OFF/FS2処理
                // Rm1(reverbQCutOnly): FS3→実KEY_OFF（エンベロープRELEASE開始）
                // Rm0(reverb有効, !Rm1): FS2→リバーブ音量設定（KEY_OFFなし）
                if (st.noteOn && isFM(ch)) {
                    if (st.reverbQCutOnly) {
                        // Z80 FMSUB3: BIT 4,(IX+33)=1 → FS3 → CALL KEYOFF
                        doKeyOff(ch);
                        st.reverbActive = true;
                        // noteOnは維持（Z80もKEY_OFF後にLFO/ピッチ更新は継続）
                    } else {
                        // Z80 FMSUB3: BIT 5,(IX+33)=1 → FS2 → リバーブ音量
                        fmSetReverbVolume(toFMIndex(ch), st.volume, st.reverbValue);
                        st.reverbActive = true;
                    }
                }
                break;
            case MmlEventType::TEMPO:
                m_globalTempo = ev.value;  // テンポは全チャンネル共有
                break;
            case MmlEventType::VOLUME:
                if (isADPCMB(ch) && ev.note == 1) {
                    // PVMODE=1: v値をIX+7(追加音量)に格納。IX+6(baseVol)は変更しない
                    m_pcmVolMode = 1;
                    m_pcmAddVol = ev.value;
                    adpcmbSetVolume(st.volume);  // baseVol+addVolで再計算
                } else {
                    if (isADPCMB(ch) && ev.note == 0) {
                        // PVMODE=0: v値をIX+6(baseVol)に格納。IX+7は変更しない
                        m_pcmVolMode = 0;
                    }
                    st.volume = ev.value;
                    doSetVolume(ch, ev.value);
                }
                break;
            case MmlEventType::PATCH:
                doSetPatch(ch, ev.value);
                break;
            case MmlEventType::STACCATO:
                st.staccato = ev.value;
                break;
            case MmlEventType::DETUNE:
                st.detune = ev.value;
                // 発音中ならピッチを即時更新
                if (st.noteOn) updatePitch(ch);
                break;
            case MmlEventType::VIBRATO:
                st.lfoDelay = ev.vibDelay;
                st.lfoRate  = std::max(ev.vibRate, 1);
                st.lfoDepth = ev.vibDepth;
                st.lfoCount = std::max(ev.vibCount, 1);
                st.lfoEnabled = true;
                // LFO設定変更時にランタイム状態はリセットしない
                // （ノートオン時にリセットされる）
                break;
            case MmlEventType::VIBRATO_SWITCH:
                st.lfoEnabled = (ev.value != 0);
                if (!st.lfoEnabled) {
                    // LFO OFF: ピッチオフセットをクリアし即時反映
                    st.lfoPitchOffset = 0;
                    if (st.noteOn) updatePitch(ch);
                }
                break;
            case MmlEventType::LFO_PARAM:
                // MW/MC/ML/MD: LFO個別パラメータ変更
                switch (ev.vibDelay) {
                    case 0: st.lfoDelay = ev.value; break;              // MW
                    case 1: st.lfoRate  = std::max(ev.value, 1); break; // MC
                    case 2: st.lfoDepth = ev.value; break;              // ML
                    case 3: st.lfoCount = std::max(ev.value, 1); break; // MD
                }
                break;
            case MmlEventType::PAN:
                st.pan = ev.value;
                doSetPan(ch, ev.value);
                break;
            case MmlEventType::REST:
                if (st.noteOn || st.reverbActive) {
                    // Rm0(reverbQCutOnly=false): 休符でもリバーブ適用
                    // Rm1(reverbQCutOnly=true): 休符ではリバーブ適用しない→通常KEY_OFF
                    if (isFM(ch) && st.reverbEnabled && !st.reverbQCutOnly && st.noteOn) {
                        // Z80 FMSUB3: 休符時にリバーブON → FS2
                        fmSetReverbVolume(toFMIndex(ch), st.volume, st.reverbValue);
                        st.reverbActive = true;
                        // noteOnは維持（LFO継続）
                    } else {
                        doKeyOff(ch);
                        st.noteOn = false;
                        st.reverbActive = false;
                    }
                }
                break;
            case MmlEventType::REG_WRITE: {
                int addr = ev.note;
                int data = ev.value;
                // SSGミキサー仮想アドレス（0xF0-0xF2）: PコマンドのSSGミキサー制御
                if (addr >= 0xF0 && addr <= 0xF2) {
                    int si = addr - 0xF0;
                    // P0=無音(tone off,noise off), P1=トーン, P2=ノイズ, P3=トーン+ノイズ
                    // レジスタ0x07: bit0-2=tone(active low), bit3-5=noise(active low)
                    bool toneOn  = (data & 1) != 0;  // P1 or P3
                    bool noiseOn = (data & 2) != 0;  // P2 or P3
                    if (toneOn)  m_ssgMixer &= ~(1 << si);      // tone enable (active low)
                    else         m_ssgMixer |=  (1 << si);       // tone disable
                    if (noiseOn) m_ssgMixer &= ~(1 << (si+3));  // noise enable
                    else         m_ssgMixer |=  (1 << (si+3));   // noise disable
                    m_engine->writeReg(0, 0x07, m_ssgMixer);
                    break;
                }
                // 通常のレジスタ書き込み（yコマンド）
                int port = (addr >= 0x100) ? 1 : 0;
                m_engine->writeReg(port, static_cast<uint8_t>(addr & 0xFF), static_cast<uint8_t>(data & 0xFF));
                // リズム楽器 IL レジスタ（0x18-0x1D）への書き込みを追跡
                if (addr >= 0x18 && addr <= 0x1D) {
                    m_rhythmIL[addr - 0x18] = static_cast<uint8_t>(data & 0xFF);
                }
                break;
            }
            case MmlEventType::KEY_TRANSPOSE:
                // パーサー側でノート番号に適用済み。エンジンでは無処理。
                break;
            case MmlEventType::SSG_ENVELOPE:
                // SSGソフトウェアエンベロープ設定（Eコマンド）
                st.ssgSoftEnv = true;
                st.ssgEnvAL = ev.envAL;
                st.ssgEnvAR = ev.envAR;
                st.ssgEnvDR = ev.envDR;
                st.ssgEnvSR = ev.envSR;
                st.ssgEnvSL = ev.envSL;
                st.ssgEnvRR = ev.envRR;
                break;
            case MmlEventType::PORTAMENTO: {
                // ポルタメント: 次のNOTE_ONで発音される音のピッチスライドを設定
                // Z80 CULPTM→PLLFO→PLSKI2: F-Number(block込み14bit)への毎tick加算
                int startNote = ev.note;
                int endNote   = ev.value;
                int dur       = static_cast<int>(ev.duration);
                if (dur <= 0) dur = 1;
                // F-Number 14bit = (block << 11) | fnum
                int sb = 0, eb = 0;
                uint16_t sf = noteToFnum(startNote, sb);
                uint16_t ef = noteToFnum(endNote, eb);
                int startBF = (sb << 11) | sf;
                int endBF   = (eb << 11) | ef;
                st.portaActive      = true;
                st.portaStartFnum   = startBF;
                st.portaEndFnum     = endBF;
                st.portaCurrentFnum = startBF;
                st.portaTicksLeft   = dur;
                st.portaStep        = (endBF - startBF) / dur;
                break;
            }
            case MmlEventType::REVERB_ENVELOPE:
                st.reverbValue = ev.value;
                st.reverbEnabled = true;  // R値設定時に自動ON（Z80: REVERVE→SET 5,(IX+33)）
                break;
            case MmlEventType::REVERB_SWITCH:
                st.reverbEnabled = (ev.value != 0);
                if (!st.reverbEnabled) {
                    // RF0: リバーブOFF→音量即時反映（Z80: REVSW→CALL STVOL）
                    st.reverbActive = false;
                    doSetVolume(ch, st.volume);
                }
                break;
            case MmlEventType::REVERB_MODE:
                st.reverbQCutOnly = (ev.value != 0);
                break;
            case MmlEventType::HARDWARE_LFO: {
                // ハードウェアLFO（Z80 HLFOON, music.asm:1033）
                // 0x22: LFO周波数(bit0-2) + ON(bit3)
                // 0xB4+ch: PAN(bit6-7) | AMS(bit4-5) | PMS(bit0-2)
                if (isFM(ch)) {
                    int freq = ev.vibDelay & 0x07;
                    int pms  = ev.vibRate & 0x07;
                    int ams  = ev.vibDepth & 0x03;
                    // レジスタ0x22: LFO ON + 周波数
                    m_engine->writeReg(0, 0x22, static_cast<uint8_t>(freq | 0x08));
                    // レジスタ0xB4+ch: PANビット保持 + AMS/PMS
                    int fi   = toFMIndex(ch);
                    int port = fmPort(fi);
                    int off  = fmOffset(fi);
                    int panBits = panToReg(st.pan) & 0xC0;
                    m_engine->writeReg(port, 0xB4 + off,
                        static_cast<uint8_t>(panBits | ((ams & 0x03) << 4) | (pms & 0x07)));
                }
                break;
            }
            case MmlEventType::CSM_MODE: {
                // FM ch3 CSMモード（Z80 MDSET→TO_EFC/EXMODE）
                // S n1,n2,n3,n4: OP1-OP4のデチューンオフセット設定
                // S0,0,0,0: 通常モード復帰（TO_NML）
                st.csmDetune[0] = ev.vibDelay;  // OP1
                st.csmDetune[1] = ev.vibRate;   // OP2
                st.csmDetune[2] = ev.vibDepth;  // OP3
                st.csmDetune[3] = ev.vibCount;  // OP4
                bool allZero = (st.csmDetune[0] == 0 && st.csmDetune[1] == 0 &&
                                st.csmDetune[2] == 0 && st.csmDetune[3] == 0);
                if (allZero) {
                    // TO_NML: 通常モード復帰（reg 0x27 = 0x3A）
                    st.csmEnabled = false;
                    if (m_engine)
                        m_engine->writeReg(0, 0x27, 0x3A);
                } else {
                    // TO_EFC: エフェクトモード有効化（reg 0x27 = 0x7A）
                    st.csmEnabled = true;
                    if (m_engine)
                        m_engine->writeReg(0, 0x27, 0x7A);
                }
                break;
            }
            case MmlEventType::LOOP_POINT:
                // ループ再開位置を記録（次のイベントから再開）
                st.hasLoopPoint = true;
                st.loopEventIdx = st.eventIdx + 1;
                st.loopTick     = ev.tick;
                break;
            case MmlEventType::END:
                st.eventIdx = st.events.size();
                return;
            default:
                break;
            }
            st.eventIdx++;
        }

        // Z80互換: イベント末尾到達時は無音待機
        // Z80コンパイラは全チャンネルをMaxCountにパディングするため、
        // 短いチャンネルもcommonEndTickまで無音で待機する。
        // globalLoopRestartで全チャンネルが一斉にLポイントへ戻る。
      }
    }

    // =====================================================================
    // ハイジャック中のイベント進行（レジスタ書き込みなし）
    // BGM状態（音量・音色・パン・テンポ等）を追跡しつつ、発音はしない。
    // releaseChannel() で現在のBGM状態をレジスタに復元して再開する。
    // =====================================================================
    void advanceEventsSilent(int ch, uint32_t tick)
    {
        auto& st = m_channels[ch];
        uint32_t chTick = (m_perChannelLoop && st.hasLoopPoint)
                        ? (tick - st.perChTickBase)
                        : (tick - m_loopTickOffset);
        while (st.eventIdx < st.events.size()) {
            const MmlEvent& ev = st.events[st.eventIdx];
            if (m_commonEndTick > 0 && ev.tick > m_commonEndTick) {
                st.noteOn = false;
                st.eventIdx = st.events.size();
                break;
            }
            if (ev.tick > chTick) break;
            switch (ev.type) {
            case MmlEventType::TEMPO:    m_globalTempo = ev.value; break;
            case MmlEventType::VOLUME:   st.volume = ev.value; break;
            case MmlEventType::PATCH:
                if (isFM(ch))  m_fmPatchNo[toFMIndex(ch)] = ev.value;
                if (isSSG(ch)) {
                    int idx = ev.value & 0x0F;
                    if (idx < 16) {
                        const auto& preset = kSsgPresets[idx];
                        st.ssgSoftEnv = true;
                        st.ssgEnvAL = preset.env[0]; st.ssgEnvAR = preset.env[1];
                        st.ssgEnvDR = preset.env[2]; st.ssgEnvSL = preset.env[3];
                        st.ssgEnvSR = preset.env[4]; st.ssgEnvRR = preset.env[5];
                    }
                }
                break;
            case MmlEventType::PAN:       st.pan = ev.value; break;
            case MmlEventType::STACCATO:  st.staccato = ev.value; break;
            case MmlEventType::DETUNE:    st.detune = ev.value; break;
            case MmlEventType::NOTE_ON:   st.currentNote = ev.note; st.noteOn = true; break;
            case MmlEventType::NOTE_OFF:  st.noteOn = false; break;
            case MmlEventType::REST:      st.noteOn = false; break;
            case MmlEventType::VIBRATO:
                st.lfoEnabled = true;
                st.lfoDelay = ev.vibDelay; st.lfoRate = ev.vibRate;
                st.lfoDepth = ev.vibDepth; st.lfoCount = ev.vibCount;
                break;
            case MmlEventType::VIBRATO_SWITCH:
                st.lfoEnabled = (ev.value != 0); break;
            case MmlEventType::REVERB_ENVELOPE:
                st.reverbValue = ev.value; st.reverbEnabled = true; break;
            case MmlEventType::REVERB_SWITCH:
                st.reverbEnabled = (ev.value != 0); break;
            case MmlEventType::SSG_ENVELOPE:
                st.ssgSoftEnv = true;
                st.ssgEnvAL = ev.envAL; st.ssgEnvAR = ev.envAR;
                st.ssgEnvDR = ev.envDR; st.ssgEnvSL = ev.envSL;
                st.ssgEnvSR = ev.envSR; st.ssgEnvRR = ev.envRR;
                break;
            case MmlEventType::LOOP_POINT:
                st.hasLoopPoint = true;
                st.loopEventIdx = st.eventIdx + 1;
                st.loopTick = ev.tick;
                break;
            case MmlEventType::END:
                st.eventIdx = st.events.size();
                return;
            default: break;
            }
            st.eventIdx++;
        }
    }

    // =====================================================================
    // 統合ディスパッチ（FM / SSG / Rhythm）
    // =====================================================================
    void doKeyOn(int ch, int noteNum, int velocity)
    {
        // LFOランタイム状態をリセット（ノートオンごとに遅延から再開）
        // Z80 LFORST+LFORST2: delay counter = delay, peak counter = peak/2(SRL A),
        // waveform position = initial depth, rate counter = rate
        auto& st = m_channels[ch];
        st.lfoDelayCounter = st.lfoDelay;
        st.lfoStepCounter  = st.lfoCount / 2;  // Z80 SETPEK: SRL A → peak/2
        st.lfoRateCounter  = 0;
        st.lfoDirection    = 1;
        st.lfoPitchOffset  = 0;

        if      (isFM(ch))     fmKeyOn(toFMIndex(ch), noteNum, velocity);
        else if (isSSG(ch))    ssgKeyOn(toSSGIndex(ch), noteNum);
        else if (isRhythm(ch)) rhythmKeyOn();
        else if (isADPCMB(ch)) adpcmbKeyOn(noteNum);
    }

    void doKeyOff(int ch)
    {
        if      (isFM(ch))     fmKeyOff(toFMIndex(ch));
        else if (isSSG(ch))    ssgKeyOff(toSSGIndex(ch));
        else if (isRhythm(ch)) rhythmKeyOff();
        else if (isADPCMB(ch)) adpcmbKeyOff();
    }

    void doSetVolume(int ch, int vol)
    {
        if      (isFM(ch))     fmSetVolume(toFMIndex(ch), vol);
        else if (isSSG(ch))    ssgSetVolume(toSSGIndex(ch), vol);
        else if (isRhythm(ch)) rhythmSetVolume(vol);
        else if (isADPCMB(ch)) adpcmbSetVolume(vol);
    }

    // MUCOM88 pan値 → YM2608レジスタ値変換
    // p0=off(0x00), p1=right(0x40), p2=left(0x80), p3=center(0xC0)
    static int panToReg(int pan) {
        static const int tbl[] = { 0x00, 0x40, 0x80, 0xC0 };
        return tbl[pan & 3];
    }

    void doSetPan(int ch, int pan)
    {
        if (isFM(ch)) {
            int fi = toFMIndex(ch);
            int port = fmPort(fi);
            int off  = fmOffset(fi);
            m_engine->writeReg(port, 0xB4 + off, static_cast<uint8_t>(panToReg(pan)));
        }
        // SSG: パンなし（モノラル）
        else if (isADPCMB(ch)) {
            // ADPCM-B: Z80 STEREOルーチン互換 — PCMLRにパン値を設定
            m_pcmPan = static_cast<uint8_t>(panToReg(pan));
        }
        else if (isRhythm(ch)) {
            // リズムPAN: MUCOM88形式 p $NN
            // bit4-5: L/R (0=off, 1=R, 2=L, 3=LR)
            // bit0-3: 楽器インデックス (0-5)
            int inst = pan & 0x0F;
            int lr   = (pan >> 4) & 0x03;
            if (inst < 6) {
                // ILレジスタのPANビットのみ更新（レベルは保持）
                m_rhythmIL[inst] = static_cast<uint8_t>((lr << 6) | (m_rhythmIL[inst] & 0x1F));
            }
        }
    }

    void doSetPatch(int ch, int patchNo)
    {
        if (isFM(ch)) {
            int fi = toFMIndex(ch);
            m_fmPatchNo[fi] = patchNo;
            fmApplyPatch(fi, patchNo);
            // Z80 OTOPST (line 1172): CALL STENV → CALL STVOL
            // パッチロード後にSTVOLで現在のvolumeをキャリアTLに反映
            fmSetVolume(fi, m_channels[ch].volume);
        } else if (isSSG(ch)) {
            // SSG: @N でソフトウェアエンベロープのプリセットを選択
            // Z80 OTOSSG: SSGDAT テーブルから6バイト(AL,AR,DR,SL,SR,RR)をロードし
            // ENVPST で IX+12..17 にコピー、IX+6 に bit7(softEnv)|bit4(envMode) セット
            ssgApplyPreset(toSSGIndex(ch), patchNo);
        } else if (isRhythm(ch)) {
            // リズム: @N で楽器ビットマスクを設定
            // bit0=BD, bit1=SD, bit2=CY, bit3=HH, bit4=TM, bit5=RS
            m_rhythmMask = static_cast<uint8_t>(patchNo & 0x3F);
        } else if (isADPCMB(ch)) {
            // ADPCM-B: @N でPCMサンプル番号を選択
            m_pcmCurrentNum = patchNo;
        }
    }

    // Timer-B周期再計算（OpenMUCOM88 fmtimer.cpp完全互換）
    // fmgenはclock_を整数除算で計算するため、同じ整数除算を使用
    // fmgen: clock_ = 7987200/2 = 3993600, prescale=6, ratio=12
    //        fmclock = 3993600 / 6 / 12 = 55466 (int, 余り8切り捨て)
    void recalcTimerB()
    {
        static constexpr int    FMCLOCK_INT = 7987200 / 2 / 6 / 12;  // 55466 (fmgen互換)
        static constexpr double TIMER_STEPD = 1000.0 / FMCLOCK_INT * 16.0;

        int tb = 256 - m_globalTempo;
        if (tb <= 0) tb = 1;
        double calc = static_cast<double>(tb) * TIMER_STEPD;
        m_timerBPeriod = static_cast<int>(calc * 1024.0);  // fmgen互換: int truncation
        if (m_timerBPeriod <= 0) m_timerBPeriod = 1;
    }

    // 3成分の合算減衰値を再計算し、全チャンネルのレジスタに即時反映
    void recalcGlobalAtt()
    {
        m_globalAtt = std::clamp(m_masterAtt + m_bgmAtt + m_fadeAtt + m_duckAtt, 0, 127);
        if (!m_engine) return;
        for (int fi = 0; fi < MAX_FM_CHANNELS; fi++) {
            int ch = fmMmlCh(fi);
            if (m_channels[ch].hijacked) continue;
            fmSetVolume(fi, m_channels[ch].volume);
        }
        for (int si = 0; si < MAX_SSG_CHANNELS; si++) {
            int ch = si + 3;
            if (m_channels[ch].hijacked) continue;
            if (m_channels[ch].noteOn) {
                int vol = std::clamp(m_channels[ch].volume - m_globalAtt / 4, 0, 15);
                m_engine->writeReg(0, 0x08 + si, static_cast<uint8_t>(vol & 0x0F));
            }
        }
        int rhythmAtt = m_globalAtt * 63 / 127;
        int adjustedTL = std::clamp(static_cast<int>(m_rhythmTL) - rhythmAtt, 0, 63);
        m_engine->writeReg(0, 0x11, static_cast<uint8_t>(adjustedTL & 0x3F));
        // ADPCM-B: ボイス再生中はスキップ（ボイスの音量はplayVoice()で管理）
        if (m_voiceDuckState.load(std::memory_order_acquire) == VoiceDuckState::Idle) {
            adpcmbSetVolume(m_channels[10].volume);
        }
    }

    // 全消音
    void allSoundOff()
    {
        if (!m_engine) return;
        // FM 全キーオフ
        for (int fi = 0; fi < MAX_FM_CHANNELS; fi++) fmKeyOff(fi);
        // SSG 全振幅0 + ミキサー無効化
        m_ssgMixer = 0x3F;
        m_engine->writeReg(0, 0x07, m_ssgMixer);
        for (int i = 0; i < MAX_SSG_CHANNELS; i++)
            m_engine->writeReg(0, 0x08 + i, 0x00);
        // リズム全停止（Dump bit=1）
        m_engine->writeReg(0, 0x10, 0x80 | 0x3F);
        // ADPCM-B: ボイス再生中はスキップ（ボイスはplayVoice()で独立管理）
        if (m_voiceDuckState.load(std::memory_order_acquire) == VoiceDuckState::Idle) {
            adpcmbKeyOff();
        }
    }

    // フェードアウト完了時の自動アクション実行
    // stop()がm_fading/m_fadeAttをリセットするため、呼び出し側で
    // m_fadeActionを事前にローカル変数に退避してからこのメソッドを呼ぶこと
    void executeFadeAction(FadeAction action)
    {
        if (action == FadeAction::Stop || action == FadeAction::StopAndReset) {
            stop();
        }
        if (action == FadeAction::StopAndReset) {
            if (m_engine) m_engine->reset();
            if (m_seEngine) {
                m_seEngine->reset();
                // Richモード: SEチップの初期状態を再確立
                if (m_seMode == SeMode::Rich) {
                    for (int fi = 0; fi < MAX_FM_CHANNELS; fi++) {
                        int port = fmPort(fi);
                        int off  = fmOffset(fi);
                        m_seEngine->writeReg(port, 0xB4 + off, 0xC0);
                    }
                    m_seEngine->writeReg(0, 0x07, 0x3F);
                    m_seEngine->writeReg(0, 0x27, 0x3A);
                    if (m_engine)
                        m_seEngine->setSsgMixScale(m_engine->getSsgMixScale());
                }
            }
        }
        m_fadeOutDone = true;
    }

    // =====================================================================
    // FM ドライバー（既存ロジックをFMインデックス(0-5)ベースに変更）
    // =====================================================================
    static int fmPort(int fi)   { return (fi < 3) ? 0 : 1; }
    static int fmOffset(int fi) { return fi % 3; }

    void fmApplyPatch(int fi, int patchNo)
    {
        auto it = m_patchMap.find(patchNo);
        if (it != m_patchMap.end() && it->second.valid) {
            fmWritePatch(fi, it->second);
        } else {
            auto def = m_patchMap.find(0);
            if (def != m_patchMap.end() && def->second.valid)
                fmWritePatch(fi, def->second);
        }
    }

    void fmWritePatch(int fi, const FmPatch& patch)
    {
        if (!m_engine) return;
        int port = fmPort(fi);
        int off  = fmOffset(fi);

        // MUCOM88 STENV互換: 音色ロード前にKEY OFFし、SL/RR=0x0Fで最速リリース
        static const int slotOff[] = { 0, 8, 4, 12 };
        fmKeyOff(fi);
        for (int oi = 0; oi < 4; oi++)
            m_engine->writeReg(port, 0x80 + slotOff[oi] + off, 0x0F);

        // 音色パラメータ書き込み（MUCOM88 STENV: 6パラメータ×4オペレータ）
        for (int oi = 0; oi < 4; oi++) {
            int base = slotOff[oi] + off;
            const auto& op = patch.op[oi];
            m_engine->writeReg(port, 0x30 + base,
                static_cast<uint8_t>(((op.dt & 0x07) << 4) | (op.ml & 0x0F)));
            m_engine->writeReg(port, 0x40 + base, static_cast<uint8_t>(op.tl & 0x7F));
            m_engine->writeReg(port, 0x50 + base,
                static_cast<uint8_t>(((op.ks & 0x03) << 6) | (op.ar & 0x1F)));
            m_engine->writeReg(port, 0x60 + base, static_cast<uint8_t>(((op.ame & 1) << 7) | (op.dr & 0x1F)));
            m_engine->writeReg(port, 0x70 + base, static_cast<uint8_t>(op.sr & 0x1F));
            m_engine->writeReg(port, 0x80 + base,
                static_cast<uint8_t>(((op.sl & 0x0F) << 4) | (op.rr & 0x0F)));
        }

        // FB/ALG
        m_engine->writeReg(port, 0xB0 + off,
            static_cast<uint8_t>(((patch.fb & 0x07) << 3) | (patch.al & 0x07)));
        // パン
        int mmlCh = (fi < 3) ? fi : (fi - 3 + 7);
        int panBits = panToReg(m_channels[mmlCh].pan);
        m_engine->writeReg(port, 0xB4 + off, static_cast<uint8_t>(panBits));
    }

    // ── ソフトウェアLFO tick処理 ──────────────────────────
    void tickLfo(int ch)
    {
        auto& st = m_channels[ch];
        // 遅延カウントダウン
        if (st.lfoDelayCounter > 0) {
            st.lfoDelayCounter--;
            return;
        }
        // レートカウンタ: lfoRate tick ごとに1ステップ進む
        st.lfoRateCounter++;
        if (st.lfoRateCounter < st.lfoRate) return;
        st.lfoRateCounter = 0;

        // ピッチオフセットを変化させる
        st.lfoPitchOffset += st.lfoDepth * st.lfoDirection;
        st.lfoStepCounter++;

        // 反転: lfoCount ステップ到達で方向反転
        if (st.lfoStepCounter >= st.lfoCount) {
            st.lfoDirection = -st.lfoDirection;
            st.lfoStepCounter = 0;
        }

        // ピッチ即時反映
        updatePitch(ch);
    }

    // ── ポルタメント tick更新（Z80 PLLFO→PLSKI2互換）──────
    void tickPortamento(int ch)
    {
        auto& st = m_channels[ch];
        if (!st.portaActive) return;

        // F-Number(14bit = block<<11 | fnum)をステップ加算
        st.portaCurrentFnum += st.portaStep;
        st.portaTicksLeft--;

        if (st.portaTicksLeft <= 0) {
            st.portaCurrentFnum = st.portaEndFnum;
            st.portaActive = false;
        }

        // 14bit値からblock/fnumを分離してレジスタに書き込み
        int bf = st.portaCurrentFnum;
        if (bf < 0) bf = 0;
        int block = (bf >> 11) & 0x07;
        int fnum  = bf & 0x7FF;

        if (isFM(ch)) {
            int fi   = toFMIndex(ch);
            int port = fmPort(fi);
            int off  = fmOffset(fi);
            m_engine->writeReg(port, 0xA4 + off,
                static_cast<uint8_t>(((block & 0x07) << 3) | ((fnum >> 8) & 0x07)));
            m_engine->writeReg(port, 0xA0 + off, static_cast<uint8_t>(fnum & 0xFF));
        } else if (isSSG(ch)) {
            // SSG: 14bit block|fnum → SSGトーンピリオドに変換
            // PLLFO PLSKI2のSSG部: fnum >> octave でスケーリング
            int si = toSSGIndex(ch);
            int tp = fnum;
            // blockからオクターブシフト（block大→ピリオド小）
            if (block > 0) tp >>= block;
            tp = std::clamp(tp, 1, 0xFFF);
            m_engine->writeReg(0, si * 2,     static_cast<uint8_t>(tp & 0xFF));
            m_engine->writeReg(0, si * 2 + 1, static_cast<uint8_t>((tp >> 8) & 0x0F));
        }
    }

    // ── FM ch3 CSMモード KEY ON（Z80 EXMODE互換）──────────
    // Z80 EXMODE: ノートイベント時にOP1-OP4の独立F-Numberを設定し、各々KEY ONする。
    // OP1: 0xA6/0xA2（通常ch3レジスタ）
    // OP2: 0xAC/0xA8, OP3: 0xAD/0xA9, OP4: 0xAE/0xAA（ch3特殊モードレジスタ）
    void csmKeyOn(int ch, int noteNum, int /*velocity*/)
    {
        if (!m_engine) return;
        auto& st = m_channels[ch];

        // LFOランタイム状態をリセット（ノートオンごと）
        st.lfoDelayCounter = st.lfoDelay;
        st.lfoStepCounter  = 0;
        st.lfoRateCounter  = 0;
        st.lfoDirection    = 1;
        st.lfoPitchOffset  = 0;
        int pitchOffset = st.detune + st.lfoPitchOffset;
        int block = 4;
        uint16_t fnum = noteToFnum(noteNum, block);
        int adjusted = static_cast<int>(fnum) + pitchOffset;
        while (adjusted > 0x7FF && block < 7) { adjusted >>= 1; block++; }
        while (adjusted < 0 && block > 0)     { adjusted <<= 1; block--; }
        if (adjusted < 0) adjusted = 0;
        if (adjusted > 0x7FF) adjusted = 0x7FF;
        fnum = static_cast<uint16_t>(adjusted);

        // 基準F-Number（16bit: block<<11 | fnum）
        int baseFnum = ((block & 0x07) << 11) | (fnum & 0x7FF);

        // OP1-OP4のF-Numberレジスタアドレス（ch3特殊モード）
        // Z80 EXMODE: FPORT=0xA4→(+IX+8=2)→0xA6 (OP1)
        //             FPORT=0xAA→0xAC (OP2), 0xAB→0xAD (OP3), 0xAC→0xAE (OP4)
        static const uint8_t msbRegs[4] = { 0xA6, 0xAC, 0xAD, 0xAE };
        static const uint8_t lsbRegs[4] = { 0xA2, 0xA8, 0xA9, 0xAA };

        // ch3 KEY ON: slot mask 0xF0 | ch3=2
        static const uint8_t keyOnData = 0xF0 | 0x02;

        for (int op = 0; op < 4; op++) {
            int opFnum = baseFnum + st.csmDetune[op];
            if (opFnum < 0) opFnum = 0;
            int opBlock = (opFnum >> 11) & 0x07;
            int opFn    = opFnum & 0x7FF;

            // F-Number書き込み（MSB→LSBの順、Z80 FMSUB6互換）
            m_engine->writeReg(0, msbRegs[op],
                static_cast<uint8_t>(((opBlock & 0x07) << 3) | ((opFn >> 8) & 0x07)));
            m_engine->writeReg(0, lsbRegs[op], static_cast<uint8_t>(opFn & 0xFF));

            // KEY ON（Z80 FMSUB6→FMSUB7→KEYON: 毎オペレータF-Number書き込み後にKEY ON）
            m_engine->writeReg(0, 0x28, keyOnData);
        }
    }

    // ── ピッチ更新（デチューン + LFOオフセット適用）─────
    void updatePitch(int ch)
    {
        auto& st = m_channels[ch];
        if (!st.noteOn) return;
        int offset = st.detune + st.lfoPitchOffset;
        if      (isFM(ch))  fmUpdateFreq(toFMIndex(ch), st.currentNote, offset);
        else if (isSSG(ch)) ssgUpdateFreq(toSSGIndex(ch), st.currentNote, offset);
    }

    // ── FM 周波数書き込み（オフセット付き）─────────────
    void fmWriteFreq(int fi, int noteNum, int pitchOffset)
    {
        if (!m_engine) return;
        int port = fmPort(fi);
        int off  = fmOffset(fi);

        int block = 4;
        uint16_t fnum = noteToFnum(noteNum, block);
        // ピッチオフセット適用（F-Number直接加算）
        int adjusted = static_cast<int>(fnum) + pitchOffset;
        // ブロック境界のキャリー処理
        while (adjusted > 0x7FF && block < 7) {
            adjusted >>= 1;
            block++;
        }
        while (adjusted < 0 && block > 0) {
            adjusted <<= 1;
            block--;
        }
        fnum = static_cast<uint16_t>(std::clamp(adjusted, 0, 0x7FF));

        m_engine->writeReg(port, 0xA4 + off,
            static_cast<uint8_t>(((block & 0x07) << 3) | ((fnum >> 8) & 0x07)));
        m_engine->writeReg(port, 0xA0 + off, static_cast<uint8_t>(fnum & 0xFF));
    }

    void fmUpdateFreq(int fi, int noteNum, int pitchOffset)
    {
        fmWriteFreq(fi, noteNum, pitchOffset);
    }

    void fmKeyOn(int fi, int noteNum, int /*velocity*/)
    {
        if (!m_engine) return;
        // デチューン + LFOオフセット適用
        int mmlCh = fmMmlCh(fi);
        int offset = m_channels[mmlCh].detune + m_channels[mmlCh].lfoPitchOffset;
        fmWriteFreq(fi, noteNum, offset);

        uint8_t chKey = (fi < 3) ? static_cast<uint8_t>(fi) : static_cast<uint8_t>(fi - 3 + 4);
        m_engine->writeReg(0, 0x28, static_cast<uint8_t>(0xF0 | chKey));
    }

    void fmKeyOff(int fi)
    {
        if (!m_engine) return;
        uint8_t chKey = (fi < 3) ? static_cast<uint8_t>(fi) : static_cast<uint8_t>(fi - 3 + 4);
        m_engine->writeReg(0, 0x28, static_cast<uint8_t>(0x00 | chKey));
    }

    static constexpr int carrierOffsets[8][4] = {
        {12, -1, -1, -1},  // AL0: op4
        {12, -1, -1, -1},  // AL1: op4
        {12, -1, -1, -1},  // AL2: op4
        {12, -1, -1, -1},  // AL3: op4
        { 8, 12, -1, -1},  // AL4: op2,op4
        { 8,  4, 12, -1},  // AL5: op2,op3,op4
        { 8,  4, 12, -1},  // AL6: op2,op3,op4
        { 0,  8,  4, 12},  // AL7: 全op
    };

    // キャリアTLにFMVDATテーブル値を書き込む共通関数
    void fmWriteCarrierTL(int fi, int tlBase)
    {
        if (!m_engine) return;
        int port = fmPort(fi);
        int off  = fmOffset(fi);
        auto it = m_patchMap.find(m_fmPatchNo[fi]);
        int al = (it != m_patchMap.end()) ? it->second.al : 4;

        for (int oi = 0; oi < 4; oi++) {
            int so = carrierOffsets[al & 7][oi];
            if (so < 0) break;
            int tl = std::clamp(tlBase + m_globalAtt, 0, 127);
            m_engine->writeReg(port, 0x40 + so + off, static_cast<uint8_t>(tl));
        }
    }

    void fmSetVolume(int fi, int vol)
    {
        fmWriteCarrierTL(fi, fmvdatLookup(vol));
    }

    void fmSetReverbVolume(int fi, int vol, int reverbValue)
    {
        fmWriteCarrierTL(fi, fmReverbTL(vol, reverbValue));
    }

    // =====================================================================
    // SSG ドライバー
    //
    // レジスタマップ（port 0）:
    //   0x00-0x01: Ch A トーンピリオド（12bit: 下位8bit / 上位4bit）
    //   0x02-0x03: Ch B
    //   0x04-0x05: Ch C
    //   0x06:      ノイズピリオド（5bit）
    //   0x07:      ミキサー（active-low: bit0-2=Tone A/B/C, bit3-5=Noise A/B/C）
    //   0x08-0x0A: Ch A/B/C 振幅（bit4=envelope mode, bit0-3=固定振幅 0-15）
    //   0x0B-0x0C: エンベロープ周期（16bit）
    //   0x0D:      エンベロープ形状（4bit）
    // =====================================================================
    // ── SSG 周波数書き込み（オフセット付き）────────────
    void ssgWriteFreq(int si, int noteNum, int pitchOffset)
    {
        if (!m_engine) return;
        uint16_t tp = noteToSSGPeriod(noteNum, m_chipClock);
        // SSG: ピリオド値にオフセット（符号反転: F-Number増=周波数上昇=ピリオド減少）
        int adjusted = static_cast<int>(tp) - pitchOffset;
        tp = static_cast<uint16_t>(std::clamp(adjusted, 1, 0xFFF));
        m_engine->writeReg(0, si * 2,     static_cast<uint8_t>(tp & 0xFF));
        m_engine->writeReg(0, si * 2 + 1, static_cast<uint8_t>((tp >> 8) & 0x0F));
    }

    void ssgUpdateFreq(int si, int noteNum, int pitchOffset)
    {
        ssgWriteFreq(si, noteNum, pitchOffset);
    }

    void ssgKeyOn(int si, int noteNum)
    {
        if (!m_engine) return;

        // トーンピリオド設定（デチューン + LFOオフセット適用）
        int mmlCh = si + 3;
        int offset = m_channels[mmlCh].detune + m_channels[mmlCh].lfoPitchOffset;
        ssgWriteFreq(si, noteNum, offset);

        // MUCOM88互換: ミキサーは初期化時に設定済み（0x38=トーン有効）
        // keyOn毎のミキサー操作はしない（MUCOM88のSSGはソフトウェアエンベロープで制御）

        // 振幅設定
        int vol = std::clamp(m_channels[mmlCh].volume - m_globalAtt / 4, 0, 15);
        uint8_t ampReg = static_cast<uint8_t>(vol & 0x0F);
        if (m_channels[mmlCh].ssgEnvMode) ampReg |= 0x10;
        m_engine->writeReg(0, 0x08 + si, ampReg);
    }

    // ── SSGソフトウェアエンベロープ tick更新（MUCOM88 SOFENV互換）──
    // Phase: 1=ATTACK, 2=DECAY, 3=SUSTAIN, 4=RELEASE
    void ssgTickEnvelope(int ch)
    {
        auto& st = m_channels[ch];
        int v = st.ssgEnvValue;
        switch (st.ssgEnvPhase) {
        case 1: // ATTACK: envelope += AR, 255でDECAYへ
            v += st.ssgEnvAR;
            if (v >= 255) { v = 255; st.ssgEnvPhase = 2; }
            break;
        case 2: // DECAY: envelope -= DR, SL以下でSUSTAINへ
            v -= st.ssgEnvDR;
            if (v <= st.ssgEnvSL) { v = st.ssgEnvSL; st.ssgEnvPhase = 3; }
            break;
        case 3: // SUSTAIN: envelope -= SR (SR=0なら保持)
            v -= st.ssgEnvSR;
            if (v < 0) v = 0;
            break;
        case 4: // RELEASE: envelope -= RR, 0で終了
            v -= st.ssgEnvRR;
            if (v <= 0) { v = 0; st.ssgEnvPhase = 0; }
            break;
        }
        st.ssgEnvValue = v;
    }

    void ssgKeyOff(int si)
    {
        if (!m_engine) return;
        // MUCOM88互換: ミキサーは触らない（トーンは有効のまま）
        // 音量を0にするだけで消音（MUCOM88ではソフトウェアエンベロープのRELEASE状態）
        m_engine->writeReg(0, 0x08 + si, 0x00);
    }

    void ssgSetVolume(int si, int vol)
    {
        if (!m_engine) return;
        int mmlCh = si + 3;
        if (m_channels[mmlCh].noteOn) {
            int v = std::clamp(vol - m_globalAtt / 4, 0, 15);
            uint8_t ampReg = static_cast<uint8_t>(v & 0x0F);
            if (m_channels[mmlCh].ssgEnvMode) ampReg |= 0x10;
            m_engine->writeReg(0, 0x08 + si, ampReg);
        }
    }

    void ssgApplyPreset(int si, int presetNo)
    {
        int mmlCh = si + 3;
        int idx = presetNo & 0x0F;
        if (idx >= 16) idx = 0;
        auto& st = m_channels[mmlCh];
        const auto& preset = kSsgPresets[idx];

        // エンベロープ
        st.ssgSoftEnv = true;
        st.ssgEnvAL = preset.env[0];
        st.ssgEnvAR = preset.env[1];
        st.ssgEnvDR = preset.env[2];
        st.ssgEnvSL = preset.env[3];
        st.ssgEnvSR = preset.env[4];
        st.ssgEnvRR = preset.env[5];
        // Z80 ENVPST: OR 10010000B → bit7=softEnv ON, bit4=envMode ON
        st.ssgEnvMode = true;

        // ミキサーモード(P): Z80 OTOSSG→NOISE
        bool toneOn  = (preset.mixerP & 1) != 0;
        bool noiseOn = (preset.mixerP & 2) != 0;
        if (toneOn)  m_ssgMixer &= ~(1 << si);
        else         m_ssgMixer |=  (1 << si);
        if (noiseOn) m_ssgMixer &= ~(1 << (si+3));
        else         m_ssgMixer |=  (1 << (si+3));
        if (m_engine) m_engine->writeReg(0, 0x07, m_ssgMixer);

        // LFO(M): Z80 OTOSSG→LFOON
        if (preset.hasLfo) {
            st.lfoEnabled  = true;
            st.lfoDelay    = preset.lfoDelay;
            st.lfoRate     = preset.lfoRate;
            st.lfoDepth    = preset.lfoDepth;
            st.lfoCount    = preset.lfoCount;
            st.lfoDelayCounter = preset.lfoDelay;
            st.lfoRateCounter  = 0;
            st.lfoStepCounter  = 0;
            st.lfoPitchOffset = 0;
            st.lfoDirection   = 1;
        } else {
            st.lfoEnabled = false;
        }
    }

    // =====================================================================
    // ADPCM-B 音楽チャンネル（Kトラック、ch10）
    // Z80 music.asm: PCMGFQ→PLAY, mucompcm.binからPCMデータ+テーブルロード
    // =====================================================================
public:
    // mucompcm.bin からPCMADRテーブルとPCMデータをロード
    // format: [0x000-0x3FF] info table (32 bytes × 最大16エントリ)
    //         [0x400+]      raw ADPCM-B data
    // info entry: [0-15]=name, [26-27]=param, [28-29]=startAddr, [30-31]=length
    [[nodiscard]] bool loadPcmData(const uint8_t* data, size_t size)
    {
        if (!data || size < 0x400 || !m_engine) return false;
        size_t infoSize = 0x400;
        (void)(size - infoSize);  // pcmSize: 今後のバリデーション用に予約

        // PCMADRテーブル構築
        m_pcmVoiceCount = 0;
        for (int i = 0; i < MAX_PCM_VOICES; i++) {
            const uint8_t* ent = data + i * 32;
            // 名前が空（最初のバイトが0）ならスキップ
            if (ent[0] == 0) continue;
            uint16_t startAddr = ent[28] | (ent[29] << 8);
            uint16_t length    = ent[30] | (ent[31] << 8);
            uint16_t param     = ent[26] | (ent[27] << 8);
            uint16_t endAddr   = startAddr + (length >> 2);  // mucomvm.cpp互換
            if (m_pcmVoiceCount < MAX_PCM_VOICES) {
                m_pcmTable[m_pcmVoiceCount].startAddr = startAddr;
                m_pcmTable[m_pcmVoiceCount].endAddr   = endAddr;
                m_pcmTable[m_pcmVoiceCount].param     = param;
                m_pcmVoiceCount++;
            }
        }

        // PCMデータのfmgenバッファへのロードは呼び出し側で行う
        // mmlEngine.loadPcmData() → テーブル解析のみ
        // fmgenEngine.loadPcmDataToAdpcmB(data + 0x400, size - 0x400) → PCMデータロード
        m_pcmLoaded = true;
        return true;
    }

    [[nodiscard]] bool loadPcmFile(const std::string& path)
    {
        std::ifstream ifs(path, std::ios::binary | std::ios::ate);
        if (!ifs) return false;
        size_t sz = static_cast<size_t>(ifs.tellg());
        ifs.seekg(0);
        std::vector<uint8_t> buf(sz);
        ifs.read(reinterpret_cast<char*>(buf.data()), sz);
        return loadPcmData(buf.data(), sz);
    }

    // ── mucompcm.bin 統合ロード ──────────────────────────
    // PCMアドレステーブル（自身で使用）とADPCM-Bオーディオデータ（IFmEngine経由）
    // をまとめてロード。利用側がヘッダー分割を意識する必要がない。
    [[nodiscard]] bool loadPcmBinary(const uint8_t* data, size_t size)
    {
        static constexpr size_t HEADER_SIZE = 0x400;
        if (!data || size <= HEADER_SIZE) return false;
        loadPcmData(data, size);
        if (m_engine)
            m_engine->loadPcmDataToAdpcmB(data + HEADER_SIZE, size - HEADER_SIZE);
        return true;
    }

    [[nodiscard]] bool loadPcmBinaryFile(const std::string& path)
    {
        std::ifstream ifs(path, std::ios::binary | std::ios::ate);
        if (!ifs) return false;
        size_t sz = static_cast<size_t>(ifs.tellg());
        ifs.seekg(0);
        std::vector<uint8_t> buf(sz);
        ifs.read(reinterpret_cast<char*>(buf.data()), sz);
        return loadPcmBinary(buf.data(), sz);
    }

private:

    // Z80 PCMNMBテーブル（music.asm:3012-3015）
    // DW 49BAH+200 は Z80アセンブラで 0x49BA + 200(10進) = 0x49BA + 0xC8
    // C, C#, D, D#, E, F, F#, G, G#, A, A#, B
    static constexpr uint16_t PCMNMB[12] = {
        0x49BA + 200, 0x4E1C + 200, 0x52C1 + 200, 0x57AD + 200,  // C, C#, D, D#
        0x5CE4 + 200, 0x626A + 200, 0x6844 + 200, 0x6E77 + 200,  // E, F, F#, G
        0x7509 + 200, 0x7BFE + 120, 0x835E + 200, 0x8B2D + 200   // G#, A(+120), A#, B
    };

    // ノート番号からADPCM-BデルタN値を計算（Z80 PCMGFQ互換）
    // Z80: note byte 上位ニブル=シフト回数, 下位ニブル=音名(0-11)
    // MMLのo1がADPCM-Bの基準オクターブ（shift=0、原速再生）。
    // noteNum = (octave+1)*12 + semi なので o1c=24。shift = noteNum/12 - 2。
    uint16_t adpcmbNoteToDeltaN(int noteNum)
    {
        // 負のノート番号によるPCMNMBテーブルの範囲外アクセスを防止
        noteNum = std::clamp(noteNum, 0, 127);
        int semi    = noteNum % 12;
        int shift   = noteNum / 12 - 2;  // o1 = shift 0
        uint32_t dn = PCMNMB[semi];
        if (shift > 0) dn >>= shift;
        else if (shift < 0) dn <<= (-shift);  // o1より高いオクターブ
        return static_cast<uint16_t>(dn & 0xFFFF);
    }

    void adpcmbKeyOn(int noteNum)
    {
        if (!m_engine) return;
        // PCMデータ未ロードでもレジスタ書き込みは実行（Z80互換）
        int idx = m_pcmCurrentNum - 1;  // Z80: DEC A (1-based → 0-based)
        if (idx < 0 || idx >= m_pcmVoiceCount) idx = 0;
        PcmVoiceEntry defaultPcm{};
        auto& pcm = m_pcmLoaded ? m_pcmTable[idx] : defaultPcm;

        uint16_t deltaN = adpcmbNoteToDeltaN(noteNum);
        int vol = m_channels[10].volume;
        // ADPCM-B vol register: 0=silent, 255=max。減衰はvolから減算。
        // m_globalAtt(0-127) → ADPCM-B scale: *2 (127*2=254 ≈ フルレンジ)
        int finalVol = vol - m_globalAtt * 2;
        if (m_pcmVolMode != 0) finalVol += m_pcmAddVol;
        if (finalVol > 250) finalVol = 250;
        if (finalVol < 0) finalVol = 0;

        // Z80 PLAY register sequence (port 1)
        m_engine->writeReg(1, 0x0B, 0x00);              // mute
        m_engine->writeReg(1, 0x01, 0x00);              // pan off
        m_engine->writeReg(1, 0x00, 0x21);              // reset
        m_engine->writeReg(1, 0x10, 0x08);              // flag
        m_engine->writeReg(1, 0x10, 0x80);              // flag
        m_engine->writeReg(1, 0x02, static_cast<uint8_t>(pcm.startAddr & 0xFF));
        m_engine->writeReg(1, 0x03, static_cast<uint8_t>(pcm.startAddr >> 8));
        m_engine->writeReg(1, 0x04, static_cast<uint8_t>(pcm.endAddr & 0xFF));
        m_engine->writeReg(1, 0x05, static_cast<uint8_t>(pcm.endAddr >> 8));
        m_engine->writeReg(1, 0x09, static_cast<uint8_t>(deltaN & 0xFF));
        m_engine->writeReg(1, 0x0A, static_cast<uint8_t>(deltaN >> 8));
        m_engine->writeReg(1, 0x00, 0xA0);              // start playback
        m_engine->writeReg(1, 0x0B, static_cast<uint8_t>(finalVol));  // volume
        m_engine->writeReg(1, 0x01, static_cast<uint8_t>(m_pcmPan)); // L/R
    }

    void adpcmbKeyOff()
    {
        if (!m_engine) return;
        m_engine->writeReg(1, 0x00, 0x01);  // stop playback
    }

    void adpcmbSetVolume(int vol)
    {
        if (!m_engine) return;
        // ADPCM-B vol register: 0=silent, 255=max。減衰はvolから減算。
        int finalVol = vol - m_globalAtt * 2;
        if (m_pcmVolMode != 0) finalVol += m_pcmAddVol;
        if (finalVol > 250) finalVol = 250;
        if (finalVol < 0) finalVol = 0;
        m_engine->writeReg(1, 0x0B, static_cast<uint8_t>(finalVol));
    }

    // =====================================================================
    // SE（効果音）ドライバー
    // =====================================================================

    // 指定エンジンのキャリアTLを書き込む（SE用、m_patchMap/m_globalAtt非依存）
    void seWriteCarrierTL(IFmEngine* engine, int fi, int al, int tl)
    {
        if (!engine) return;
        int port = fmPort(fi);
        int off  = fmOffset(fi);
        for (int oi = 0; oi < 4; oi++) {
            int so = carrierOffsets[al & 7][oi];
            if (so < 0) break;
            engine->writeReg(port, 0x40 + so + off, static_cast<uint8_t>(std::clamp(tl, 0, 127)));
        }
    }

    // SEスロット割り当て（oldest策略）
    int seAllocSlot()
    {
        for (int i = 0; i < MAX_SE_SLOTS; i++) {
            if (!m_seSlots[i].active) return i;
        }
        // 全スロット使用中 → 最古を奪う
        int oldest = 0;
        uint32_t oldestOrder = m_seSlots[0].allocOrder;
        for (int i = 1; i < MAX_SE_SLOTS; i++) {
            if (m_seSlots[i].allocOrder < oldestOrder) {
                oldest = i;
                oldestOrder = m_seSlots[i].allocOrder;
            }
        }
        stopSe(oldest);
        return oldest;
    }

    // SE発音共通処理（指定エンジンに対して音色適用→周波数→キーオン）
    void seApplyAndKeyOn(IFmEngine* engine, int fi, const FmPatch& patch, int noteNum, int velocity)
    {
        if (!engine) return;
        engine->applyPatch(fi, patch);
        int tlBase = fmvdatLookup(velocity);
        int tl = std::clamp(tlBase + m_masterAtt + m_seAtt, 0, 127);
        seWriteCarrierTL(engine, fi, patch.al, tl);
        engine->setFrequency(fi, noteNum);
        engine->fmKeyOn(fi);
    }

    // Classic SE: BGMチャンネルハイジャック方式
    // slotHint: -1=自動割り当て、0-5=指定スロット
    int playSeClassic(const FmPatch& patch, int noteNum, int velocity, int durationMs, int slotHint = -1)
    {
        int slotIdx = (slotHint >= 0 && slotHint < MAX_SE_SLOTS)
                    ? slotHint : seAllocSlot();
        if (m_seSlots[slotIdx].active) stopSe(slotIdx);

        // BGM FMチャンネル選択（J,I,H,C,B,A優先、ノートオフ中を優先）
        static const int fmChOrder[] = {9, 8, 7, 2, 1, 0};
        int bestCh = -1;
        for (int ch : fmChOrder) {
            if (isChannelHijacked(ch)) continue;
            bool usedBySe = false;
            for (const auto& s : m_seSlots) {
                if (s.active && s.mmlCh == ch) { usedBySe = true; break; }
            }
            if (usedBySe) continue;
            if (!m_channels[ch].noteOn) { bestCh = ch; break; }
            if (bestCh < 0) bestCh = ch;
        }
        if (bestCh < 0) return -1;

        hijackChannel(bestCh);
        int fi = toFMIndex(bestCh);
        seApplyAndKeyOn(m_engine, fi, patch, noteNum, velocity);

        auto& slot = m_seSlots[slotIdx];
        slot.active = true;
        slot.allocOrder = m_seAllocCounter++;
        slot.fmIndex = fi;
        slot.mmlCh = bestCh;
        slot.patch = patch;
        slot.noteNum = noteNum;
        slot.velocity = velocity;
        slot.durationSamples = (durationMs > 0) ? static_cast<uint32_t>(static_cast<uint64_t>(durationMs) * m_sampleRate / 1000) : 0;
        slot.samplesLeft = slot.durationSamples;
        return slotIdx;
    }

    // Rich SE: SE専用チップ方式
    // slotHint: -1=自動割り当て、0-5=指定スロット
    int playSeRich(const FmPatch& patch, int noteNum, int velocity, int durationMs, int slotHint = -1)
    {
        if (!m_seEngine) return -1;
        int slotIdx = (slotHint >= 0 && slotHint < MAX_SE_SLOTS)
                    ? slotHint : seAllocSlot();
        int fi = slotIdx;  // Rich: スロット番号 = FMインデックス（1:1対応）

        auto& slot = m_seSlots[slotIdx];
        if (slot.active) m_seEngine->fmKeyOff(slot.fmIndex);

        seApplyAndKeyOn(m_seEngine, fi, patch, noteNum, velocity);

        slot.active = true;
        slot.allocOrder = m_seAllocCounter++;
        slot.fmIndex = fi;
        slot.mmlCh = -1;
        slot.patch = patch;
        slot.noteNum = noteNum;
        slot.velocity = velocity;
        slot.durationSamples = (durationMs > 0) ? static_cast<uint32_t>(static_cast<uint64_t>(durationMs) * m_sampleRate / 1000) : 0;
        slot.samplesLeft = slot.durationSamples;
        return slotIdx;
    }

    // SE duration自動停止処理（シーケンス再生・ピッチスイープ対応）
    void seTickDuration(uint32_t frameCount) noexcept
    {
        for (int i = 0; i < MAX_SE_SLOTS; i++) {
            auto& slot = m_seSlots[i];
            if (!slot.active || slot.durationSamples == 0) continue;

            // ピッチスイープ: 現在ノートのendNote != -1 の場合、毎フレーム補間
            if (slot.isSequence) {
                const auto& curNote = slot.seqNotes[slot.seqCurrentNote];
                if (curNote.endNote >= 0 && slot.durationSamples > 0) {
                    // 進行率: 0.0（開始）→ 1.0（終了）
                    float progress = 1.0f - static_cast<float>(slot.samplesLeft) / static_cast<float>(slot.durationSamples);
                    progress = std::clamp(progress, 0.0f, 1.0f);
                    // startNote → endNote を浮動小数点で補間し、整数に丸める
                    float interpNote = static_cast<float>(curNote.startNote)
                                     + (static_cast<float>(curNote.endNote) - static_cast<float>(curNote.startNote)) * progress;
                    int newNote = static_cast<int>(std::round(interpNote));
                    if (newNote != slot.noteNum)
                        setSeFrequency(i, newNote);
                }
            }

            if (frameCount >= slot.samplesLeft) {
                // シーケンス再生: 次のノートへ遷移
                if (slot.isSequence && slot.seqCurrentNote + 1 < slot.seqNoteCount) {
                    slot.seqCurrentNote++;
                    const auto& nextNote = slot.seqNotes[slot.seqCurrentNote];
                    // 新しいノートのdurationを設定
                    slot.durationSamples = static_cast<uint32_t>(static_cast<uint64_t>(nextNote.durationMs) * m_sampleRate / 1000);
                    slot.samplesLeft = slot.durationSamples;
                    slot.noteNum = nextNote.startNote;
                    // パッチ再適用なしで周波数変更 + KEY_ON（レガート遷移）
                    IFmEngine* eng = (m_seMode == SeMode::Rich) ? m_seEngine : m_engine;
                    if (eng) {
                        eng->setFrequency(slot.fmIndex, nextNote.startNote);
                        eng->fmKeyOn(slot.fmIndex);
                    }
                } else {
                    stopSe(i);
                }
            } else {
                slot.samplesLeft -= frameCount;
            }
        }
    }

    // マスターボリューム変更時のSE音量再計算
    void seRecalcVolume()
    {
        for (int i = 0; i < MAX_SE_SLOTS; i++) {
            auto& slot = m_seSlots[i];
            if (!slot.active) continue;
            int tlBase = fmvdatLookup(slot.velocity);
            int tl = std::clamp(tlBase + m_masterAtt + m_seAtt, 0, 127);
            IFmEngine* engine = (m_seMode == SeMode::Rich) ? m_seEngine : m_engine;
            seWriteCarrierTL(engine, slot.fmIndex, slot.patch.al, tl);
        }
    }

    // =====================================================================
    // リズムドライバー（ADPCM-A: 6種の内蔵ドラム音源）
    //
    // レジスタマップ（port 0）:
    //   0x10: キーオン/Dump制御
    //         bit7=0: キーオン（bit0-5の楽器を発音開始）
    //         bit7=1: Dump（bit0-5の楽器を強制停止）
    //   0x11: 全体音量（TL: 6bit、0x3F=最大、0x00=無音 ※チップ内部で^0x3Fされる）
    //   0x18-0x1D: 各楽器パン&音量（bit7=L, bit6=R, bit4-0=個別音量）
    //
    // 楽器ビット: bit0=BD, bit1=SD, bit2=CY, bit3=HH, bit4=TM, bit5=RS
    //
    // NOTE: ADPCM-A は YM2608 内蔵ROMのドラムサンプルを使用。
    //       ROMデータが未ロード（ADPCM-A ROMデータが未ロード）の場合は無音。
    // =====================================================================
    void rhythmKeyOn()
    {
        if (!m_engine || m_rhythmMask == 0) return;
        // 各楽器の IL（Individual Level: PAN + Level）を書き込み
        // yコマンドやpコマンドで設定された値をそのまま使用
        for (int i = 0; i < 6; i++) {
            if (m_rhythmMask & (1 << i)) {
                m_engine->writeReg(0, 0x18 + i, m_rhythmIL[i]);
            }
        }
        // 全体音量TL（MUCOM88互換: vコマンドの全体音量値を毎回書き込み）
        // m_globalAtt を反映（マスターボリューム・BGMボリューム・フェード・ダッキング対応）
        int rhythmAtt = m_globalAtt * 63 / 127;
        int adjustedTL = std::clamp(static_cast<int>(m_rhythmTL) - rhythmAtt, 0, 63);
        m_engine->writeReg(0, 0x11, static_cast<uint8_t>(adjustedTL & 0x3F));
        // キーオン（bit7=0）
        m_engine->writeReg(0, 0x10, m_rhythmMask & 0x3F);
    }

    void rhythmKeyOff()
    {
        if (!m_engine) return;
        // Dump（bit7=1）で楽器を停止
        m_engine->writeReg(0, 0x10, 0x80 | (m_rhythmMask & 0x3F));
    }

    void rhythmSetVolume(int vol)
    {
        if (!m_engine) return;
        // MUCOM88 リズム音量: v 0-63（全体音量）
        m_rhythmTL = static_cast<uint8_t>(std::clamp(vol, 0, 63) & 0x3F);
        // globalAtt適用（recalcGlobalAtt()と同じスケーリング）
        int rhythmAtt = m_globalAtt * 63 / 127;
        int adjustedTL = std::clamp(static_cast<int>(m_rhythmTL) - rhythmAtt, 0, 63);
        m_engine->writeReg(0, 0x11, static_cast<uint8_t>(adjustedTL & 0x3F));
    }
};

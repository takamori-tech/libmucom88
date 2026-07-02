// SPDX-License-Identifier: MIT

#include <mucom88/mml_engine.hpp>
#include <mucom88/mml_parser.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace {

const MmlEvent* findPortamento(const std::vector<MmlEvent>& events)
{
    for (const auto& ev : events) {
        if (ev.type == MmlEventType::PORTAMENTO)
            return &ev;
    }
    return nullptr;
}

std::vector<const MmlEvent*> findNoteOns(const std::vector<MmlEvent>& events)
{
    std::vector<const MmlEvent*> notes;
    for (const auto& ev : events) {
        if (ev.type == MmlEventType::NOTE_ON)
            notes.push_back(&ev);
    }
    return notes;
}

bool expectPortamento(const std::string& muc, int startNote, int endNote)
{
    MmlParser parser;
    const auto parsed = parser.parse(muc);
    const auto& events = parsed.channelEvents[0];
    const MmlEvent* porta = findPortamento(events);
    return porta && porta->note == startNote && porta->value == endNote;
}

bool expectSecondNote(const std::string& muc, int note)
{
    MmlParser parser;
    const auto parsed = parser.parse(muc);
    const auto notes = findNoteOns(parsed.channelEvents[0]);
    return notes.size() >= 2 && notes[1]->note == note;
}

class CaptureEngine final : public IFmEngine {
public:
    CaptureEngine()
    {
        writes.reserve(2048);
    }

    struct Write {
        int port = 0;
        uint8_t addr = 0;
        uint8_t data = 0;
    };

    void init(uint32_t /*sampleRate*/) override {}
    void writeReg(int port, uint8_t addr, uint8_t data) noexcept override
    {
        writes.push_back(Write{port, addr, data});
    }
    void generateInterleaved(int16_t* buf, uint32_t frameCount) noexcept override
    {
        for (uint32_t i = 0; i < frameCount * 2; ++i)
            buf[i] = 0;
    }
    void reset() noexcept override {}

    [[nodiscard]] bool loadAdpcmRom(const std::string& /*path*/) override { return false; }
    [[nodiscard]] bool loadAdpcmRomFromMemory(const uint8_t* /*data*/, size_t /*size*/) override { return false; }
    [[nodiscard]] bool hasAdpcmRom() const noexcept override { return false; }

    std::vector<Write> writes;
};

std::vector<int> collectFmAChannelFnums(const std::vector<CaptureEngine::Write>& writes)
{
    std::vector<int> fnums;
    int high = -1;
    for (const auto& write : writes) {
        if (write.port != 0)
            continue;
        if (write.addr == 0xA4) {
            high = static_cast<int>(write.data);
        } else if (write.addr == 0xA0 && high >= 0) {
            const int value = ((high & 0x3F) << 8) | static_cast<int>(write.data);
            fnums.push_back(value);
            high = -1;
        }
    }
    return fnums;
}

bool hasDescendingPortamentoFnum()
{
    MmlParser parser;
    const auto parsed = parser.parse("A o3{c4<g}\n");

    CaptureEngine capture;
    MmlEngine engine;
    engine.init(&capture, 44100);
    engine.setEvents(0, parsed.channelEvents[0]);
    engine.play();
    engine.advance(44100);

    const auto fnums = collectFmAChannelFnums(capture.writes);
    if (fnums.size() < 3)
        return false;

    bool descended = false;
    for (size_t i = 1; i < fnums.size(); ++i) {
        if (fnums[i] < fnums[i - 1])
            descended = true;
    }

    return descended && fnums.back() < fnums.front();
}

} // 無名namespace

int main()
{
    // o3c=48, o2g=43。終了音の '<' で下行オクターブ跨ぎを表現できる。
    if (!expectPortamento("A o3{c4<g}\n", 48, 43))
        return 1;

    // STP22のOCTAVE更新は永続するため、{}後のgもo2で解釈される。
    if (!expectSecondNote("A o3{c4<g}g\n", 43))
        return 2;

    // 既存の同一オクターブ形は不変。
    if (!expectPortamento("A o3{c4g}\n", 48, 55))
        return 3;

    // '>' 側も対称にOCTAVEを上げ、{}後へ永続する。
    if (!expectPortamento("A o3{c4>g}\n", 48, 67))
        return 4;
    if (!expectSecondNote("A o3{c4>g}g\n", 67))
        return 5;

    // エンジン側で下行オクターブ跨ぎのFNUMが下降方向へ更新されることを確認する。
    if (!hasDescendingPortamentoFnum())
        return 6;

    return 0;
}

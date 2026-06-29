// SPDX-License-Identifier: MIT

#include <mucom88/logical_stem_mixer.hpp>

#include <cmath>

static bool nearFloat(float actual, float expected) noexcept
{
    return std::fabs(actual - expected) < 0.000001f;
}

int main()
{
    ChipStemFrame a {};
    a.fm[0][0] = 1000;
    a.fm[0][1] = -1000;
    a.ssg[1][0] = 200;
    a.ssg[1][1] = 300;
    a.rhythm[0] = 400;
    a.rhythm[1] = 500;

    ChipStemFrame b {};
    b.fm[0][0] = 3000;
    b.fm[0][1] = 1000;
    b.ssg[1][0] = 100;
    b.ssg[1][1] = -50;
    b.adpcmB[0] = 700;
    b.adpcmB[1] = 900;

    LogicalStemAccumulator acc {};
    acc.addStem(a);
    acc.addStem(b, 0.5);
    acc.addFallbackStereo(10, -20, 2.0);

    if (acc.fm[0][0] != 2500.0 || acc.fm[0][1] != -500.0)
        return 1;
    if (acc.ssg[1][0] != 250.0 || acc.ssg[1][1] != 275.0)
        return 2;
    if (acc.rhythm[0] != 400.0 || acc.rhythm[1] != 500.0)
        return 3;
    if (acc.adpcmB[0] != 350.0 || acc.adpcmB[1] != 450.0)
        return 4;
    if (acc.left() != 3520.0 || acc.right() != 685.0)
        return 5;

    LogicalStemFloatFrame out {};
    LogicalStemMixOptions disabled {};
    if (writeLogicalStemFloatFrame(acc, disabled, out))
        return 6;
    if (out.main[0] != 0.0f || out.main[1] != 0.0f)
        return 7;

    LogicalStemMixOptions enabled {};
    enabled.enableDoubleStemSumming = true;
    enabled.outputScale = 1.0 / 32768.0;
    enabled.masterGain = 0.5;
    if (!writeLogicalStemFloatFrame(acc, enabled, out))
        return 8;

    if (!nearFloat(out.main[0], static_cast<float>(3520.0 / 32768.0 * 0.5)))
        return 9;
    if (!nearFloat(out.main[1], static_cast<float>(685.0 / 32768.0 * 0.5)))
        return 10;
    if (!nearFloat(out.fm[0][0], static_cast<float>(2500.0 / 32768.0 * 0.5)))
        return 11;
    if (!nearFloat(out.adpcmB[1], static_cast<float>(450.0 / 32768.0 * 0.5)))
        return 12;

    acc.clear();
    if (acc.left() != 0.0 || acc.right() != 0.0)
        return 13;

    return 0;
}

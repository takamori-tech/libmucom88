#include <cmath>
#include <cstdio>
#include <vector>

#include <mucom88/regression_metrics.hpp>

namespace {

bool expect(bool condition, const char* message)
{
    if (condition)
        return true;

    std::fprintf(stderr, "regression_metrics: FAIL: %s\n", message);
    return false;
}

bool near(double actual, double expected, double epsilon, const char* message)
{
    if (std::fabs(actual - expected) <= epsilon)
        return true;

    std::fprintf(stderr,
                 "regression_metrics: FAIL: %s (actual=%.9f expected=%.9f)\n",
                 message,
                 actual,
                 expected);
    return false;
}

}  // namespace

int main()
{
    bool ok = true;

    const int16_t rmsInput[] = {
        3, 4,
        0, 0,
    };
    ok &= near(mucom88::calcInterleavedStereoRms(rmsInput, 2),
               std::sqrt(25.0 / 4.0),
               1.0e-9,
               "stereo RMS uses both channels");
    ok &= near(mucom88::calcInterleavedStereoRms(nullptr, 2), 0.0, 0.0, "null RMS is zero");
    ok &= near(mucom88::calcInterleavedStereoRms(rmsInput, 0), 0.0, 0.0, "empty RMS is zero");

    std::vector<int16_t> withPop(12 * 2, 0);
    withPop[0] = 1200;       // 孤立ポップ。持続音ではないため読み飛ばす。
    withPop[1] = -1200;
    for (int frame = 5; frame < 9; frame++) {
        withPop[static_cast<std::size_t>(frame * 2)] = 300;
        withPop[static_cast<std::size_t>(frame * 2 + 1)] = -300;
    }
    const mucom88::RegressionThresholds thresholds { 100, 3, 2 };
    ok &= expect(mucom88::firstSustainedSoundFrame(withPop.data(),
                                                   static_cast<int64_t>(withPop.size() / 2U),
                                                   thresholds) == 5,
                 "isolated startup pop is ignored");

    std::vector<int16_t> reference(8 * 2, 0);
    std::vector<int16_t> target(8 * 2, 0);
    for (int frame = 0; frame < 8; frame++) {
        reference[static_cast<std::size_t>(frame * 2)] = 1000;
        reference[static_cast<std::size_t>(frame * 2 + 1)] = 1000;
        target[static_cast<std::size_t>(frame * 2)] = 500;
        target[static_cast<std::size_t>(frame * 2 + 1)] = 500;
    }
    ok &= near(mucom88::averageAlignedRmsRatio(reference.data(),
                                              target.data(),
                                              8,
                                              4,
                                              3.0,
                                              thresholds),
               0.5,
               1.0e-9,
               "aligned average RMS ratio is target/reference");

    const auto summary = mucom88::summarizeRmsRatios({ 1.0, 0.5, 0.9 });
    ok &= expect(summary.count == 3, "summary count");
    ok &= near(summary.mean, 0.8, 1.0e-9, "summary mean");
    ok &= near(summary.median, 0.9, 1.0e-9, "summary median");
    ok &= near(summary.min, 0.5, 1.0e-9, "summary min");
    ok &= near(summary.max, 1.0, 1.0e-9, "summary max");
    ok &= expect(summary.greaterEqual08 == 2, "summary >=0.8");
    ok &= expect(summary.greaterEqual06 == 2, "summary >=0.6");

    if (!ok)
        return 1;

    std::printf("regression_metrics: PASS\n");
    return 0;
}

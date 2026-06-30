// SPDX-License-Identifier: MIT

#include <mucom88/post_chip_processor.hpp>

#include <cmath>

static bool exact(float actual, float expected) noexcept
{
    return actual == expected;
}

static bool near(float actual, float expected, float eps = 0.000001f) noexcept
{
    return std::fabs(actual - expected) <= eps;
}

static PostChipConfig bypassConfig() noexcept
{
    PostChipConfig config {};
    config.dacModel = false;
    config.antiAlias = false;
    config.saturation = false;
    config.noiseFloor = false;
    config.cabinet = false;
    return config;
}

int main()
{
    PostChipProcessor processor;
    processor.prepare(48000.0);

    {
        PostChipConfig config = bypassConfig();
        processor.setConfig(config);

        float left[4] { -0.75f, -0.125f, 0.25f, 0.875f };
        float right[4] { 0.5f, -0.5f, 0.0f, 0.03125f };
        processor.processBlock(left, right, 4);
        if (!exact(left[0], -0.75f) || !exact(left[1], -0.125f) ||
            !exact(left[2], 0.25f) || !exact(left[3], 0.875f))
            return 1;
        if (!exact(right[0], 0.5f) || !exact(right[1], -0.5f) ||
            !exact(right[2], 0.0f) || !exact(right[3], 0.03125f))
            return 2;
        if (!exact(processor.processSample(0.1234567f), 0.1234567f))
            return 3;
    }

    {
        processor.setStageEnabled(PostChipStage::DacModel, true);
        processor.setStageEnabled(PostChipStage::BoardLpf, false);
        processor.setStageEnabled(PostChipStage::Saturation, false);
        processor.setStageEnabled(PostChipStage::NoiseFloor, false);
        processor.setStageEnabled(PostChipStage::Cabinet, false);
        if (!processor.isStageEnabled(PostChipStage::DacModel) ||
            processor.isStageEnabled(PostChipStage::BoardLpf))
            return 4;

        const float y = processor.processSample(0.3333333f);
        if (exact(y, 0.3333333f))
            return 5;
        if (y < -1.0f || y > 1.0f)
            return 6;
    }

    {
        PostChipConfig config = bypassConfig();
        config.antiAlias = true;
        config.lpfCutoffHz = 12000.0;
        processor.setConfig(config);
        processor.reset();

        float left[3] { 1.0f, 0.0f, 0.0f };
        float right[3] { 1.0f, 0.0f, 0.0f };
        processor.processBlock(left, right, 3);
        if (!(left[0] > 0.0f && left[0] < 1.0f))
            return 7;

        processor.reset();
        float leftAgain[3] { 1.0f, 0.0f, 0.0f };
        float rightAgain[3] { 1.0f, 0.0f, 0.0f };
        processor.processBlock(leftAgain, rightAgain, 3);
        if (!near(leftAgain[0], left[0]) || !near(leftAgain[1], left[1]) ||
            !near(rightAgain[0], right[0]) || !near(rightAgain[1], right[1]))
            return 8;
    }

    {
        PostChipConfig config = bypassConfig();
        config.noiseFloor = true;
        config.noiseLevelDbfs = -90.0;
        processor.setConfig(config);
        processor.reset();

        float left[2] {};
        float right[2] {};
        processor.processBlock(left, right, 2);
        if (exact(left[0], 0.0f) || exact(right[0], 0.0f))
            return 9;

        processor.reset();
        float leftAgain[2] {};
        float rightAgain[2] {};
        processor.processBlock(leftAgain, rightAgain, 2);
        if (!near(leftAgain[0], left[0]) || !near(rightAgain[1], right[1]))
            return 10;
    }

    {
        PostChipConfig config {};
        config.cabinet = true;
        config.cabinetAmount = 1.0;
        processor.setConfig(config);
        processor.reset();

        float left[8] { 1.0f, -1.0f, 0.75f, -0.75f, 0.5f, -0.5f, 0.25f, -0.25f };
        float right[8] { -1.0f, 1.0f, -0.75f, 0.75f, -0.5f, 0.5f, -0.25f, 0.25f };
        processor.processBlock(left, right, 8);
        for (int i = 0; i < 8; ++i) {
            if (!std::isfinite(left[i]) || !std::isfinite(right[i]))
                return 11;
            if (left[i] < -1.0f || left[i] > 1.0f || right[i] < -1.0f || right[i] > 1.0f)
                return 12;
        }
    }

    return 0;
}

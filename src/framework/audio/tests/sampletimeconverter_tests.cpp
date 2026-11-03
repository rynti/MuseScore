/*
 * SPDX-License-Identifier: GPL-3.0-only
 * MuseScore-CLA-applies
 *
 * MuseScore
 * Music Composition & Notation
 *
 * Copyright (C) 2026 MuseScore Limited and others
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 3 as
 * published by the Free Software Foundation.
 */

#include "audio/engine/internal/sampletimeconverter.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>

namespace {
using namespace muse::audio;
using namespace muse::audio::engine;

TEST(SamplesToMicrosecondsConverterTests, TenMinuteProgressionRetainsFractionalRemainder)
{
    constexpr sample_rate_t sampleRate = 48000;
    constexpr samples_t tenMinuteSamples = sampleRate * 60 * 10;
    constexpr std::array<samples_t, 4> periods { 256, 1024, 2048, 4096 };

    for (const samples_t period : periods) {
        SamplesToMicrosecondsConverter converter;
        msecs_t elapsedMicroseconds = 0;
        samples_t elapsedSamples = 0;

        while (elapsedSamples + period <= tenMinuteSamples) {
            elapsedMicroseconds += converter.advance(period, sampleRate);
            elapsedSamples += period;
        }

        if (elapsedSamples < tenMinuteSamples) {
            const samples_t finalBlock = tenMinuteSamples - elapsedSamples;
            elapsedMicroseconds += converter.advance(finalBlock, sampleRate);
            elapsedSamples += finalBlock;
        }

        EXPECT_EQ(elapsedSamples, tenMinuteSamples) << "period=" << period;
        const uint64_t expected = elapsedSamples * 1000000 / sampleRate;
        EXPECT_EQ(elapsedMicroseconds, expected) << "period=" << period;
        EXPECT_LT(elapsedSamples * 1000000 - static_cast<uint64_t>(elapsedMicroseconds) * sampleRate,
                  sampleRate) << "period=" << period;
    }
}

TEST(SamplesToMicrosecondsConverterTests, ResetAndSampleRateChangeDiscardOldRemainder)
{
    SamplesToMicrosecondsConverter converter;

    EXPECT_EQ(converter.advance(256, 48000), 5333);
    EXPECT_EQ(converter.advance(256, 48000), 5333);

    converter.reset();
    EXPECT_EQ(converter.advance(256, 48000), 5333);

    EXPECT_EQ(converter.advance(256, 96000), 2666);
    EXPECT_EQ(converter.advance(256, 96000), 2667);
}
}

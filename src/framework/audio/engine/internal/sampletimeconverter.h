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

#pragma once

#include <cstdint>
#include <limits>

#include "audio/common/audiotypes.h"

namespace muse::audio::engine {
class SamplesToMicrosecondsConverter
{
public:
    msecs_t advance(samples_t samples, sample_rate_t sampleRate)
    {
        if (sampleRate == 0) {
            reset();
            return 0;
        }

        if (sampleRate != m_sampleRate) {
            m_sampleRate = sampleRate;
            m_remainder = 0;
        }

        constexpr uint64_t MICROSECONDS_PER_SECOND = 1000000;
        constexpr uint64_t MAXIMUM = std::numeric_limits<uint64_t>::max();
        constexpr msecs_t MAXIMUM_RESULT = std::numeric_limits<msecs_t>::max();

        const uint64_t wholeSeconds = samples / sampleRate;
        const uint64_t partialSamples = samples % sampleRate;
        if (wholeSeconds > static_cast<uint64_t>(MAXIMUM_RESULT) / MICROSECONDS_PER_SECOND
            || partialSamples > (MAXIMUM - m_remainder) / MICROSECONDS_PER_SECOND) {
            m_remainder = 0;
            return MAXIMUM_RESULT;
        }

        const uint64_t numerator = partialSamples * MICROSECONDS_PER_SECOND + m_remainder;
        const uint64_t wholeMicroseconds = wholeSeconds * MICROSECONDS_PER_SECOND;
        const uint64_t partialMicroseconds = numerator / sampleRate;
        m_remainder = numerator % sampleRate;

        if (wholeMicroseconds > static_cast<uint64_t>(MAXIMUM_RESULT) - partialMicroseconds) {
            m_remainder = 0;
            return MAXIMUM_RESULT;
        }

        return static_cast<msecs_t>(wholeMicroseconds + partialMicroseconds);
    }

    void reset()
    {
        m_sampleRate = 0;
        m_remainder = 0;
    }

private:
    sample_rate_t m_sampleRate = 0;
    uint64_t m_remainder = 0;
};
}

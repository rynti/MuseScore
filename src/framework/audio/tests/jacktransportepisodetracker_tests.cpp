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
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include <gtest/gtest.h>

#include "audio/driver/platform/jack/jacktransportepisodetracker.h"

using namespace muse::audio;

TEST(Audio_JackTransportEpisodeTrackerTests, RepeatedStartingReusesEpisode)
{
    JackTransportEpisodeTracker tracker;

    const auto first = tracker.observeStarting(1200);
    const auto repeated = tracker.observeStarting(1200);
    const auto laterSnapshot = tracker.observeStarting(1264);

    EXPECT_TRUE(first.isNew);
    EXPECT_NE(first.token, 0);
    EXPECT_FALSE(repeated.isNew);
    EXPECT_EQ(repeated.token, first.token);
    EXPECT_FALSE(laterSnapshot.isNew);
    EXPECT_EQ(laterSnapshot.token, first.token);
    EXPECT_EQ(laterSnapshot.frame, first.frame);
}

TEST(Audio_JackTransportEpisodeTrackerTests, SameFrameAcrossDistinctEpisodesGetsFreshTokens)
{
    JackTransportEpisodeTracker tracker;

    const auto episodeA = tracker.observeStarting(1200);
    tracker.observeNotStarting();
    const auto episodeB = tracker.observeStarting(2400);
    tracker.observeNotStarting();
    const auto episodeAAgain = tracker.observeStarting(1200);

    EXPECT_TRUE(episodeA.isNew);
    EXPECT_TRUE(episodeB.isNew);
    EXPECT_TRUE(episodeAAgain.isNew);
    EXPECT_NE(episodeA.token, episodeB.token);
    EXPECT_NE(episodeB.token, episodeAAgain.token);
    EXPECT_NE(episodeA.token, episodeAAgain.token);
    EXPECT_EQ(episodeA.frame, episodeAAgain.frame);
}

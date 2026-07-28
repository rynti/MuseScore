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

#include "audio/driver/platform/jack/jackaudiodriver.h"

#include <gtest/gtest.h>

#include <limits>

namespace muse::audio {
class JackAudioDriverTestAccess
{
public:
    static void prime(JackAudioDriver& driver, uint64_t generation)
    {
        driver.configureTransport(generation, true);
        driver.m_closing.store(false, std::memory_order_release);
        driver.synchronizeTransportEpoch();
    }

    static void observe(JackAudioDriver& driver, jack_transport_state_t state, jack_nframes_t frame,
                        bool authoritative = false)
    {
        jack_position_t position {};
        position.frame = frame;
        driver.observeTransport(state, position, authoritative);
    }

    static int synchronize(JackAudioDriver& driver, jack_transport_state_t state, jack_nframes_t frame)
    {
        jack_position_t position {};
        position.frame = frame;
        return driver.syncTransport(state, position);
    }

    static uint64_t releasedToken(const JackAudioDriver& driver)
    {
        return driver.m_releasedPreparationToken.load(std::memory_order_acquire);
    }

    static uint64_t renderEligibleToken(const JackAudioDriver& driver)
    {
        return driver.m_renderEligibleToken.load(std::memory_order_acquire);
    }

    static bool shouldRenderTransportState(const JackAudioDriver& driver, jack_transport_state_t state)
    {
        return driver.shouldRenderTransportState(state);
    }

    static void resetExpectedRenderFrame(JackAudioDriver& driver, jack_nframes_t frame)
    {
        driver.resetExpectedRenderFrame(frame);
    }

    static void observeRenderedTransportBlock(JackAudioDriver& driver, jack_nframes_t frame, jack_nframes_t nframes)
    {
        driver.observeRenderedTransportBlock(frame, nframes);
    }

    static void clearExpectedRenderFrame(JackAudioDriver& driver)
    {
        driver.clearExpectedRenderFrame();
    }

    static int reportXrun(JackAudioDriver& driver)
    {
        return JackAudioDriver::xrunCallback(&driver);
    }

    static void publishPlaybackLatencyRanges(JackAudioDriver& driver,
                                             jack_nframes_t leftMin, jack_nframes_t leftMax,
                                             jack_nframes_t rightMin, jack_nframes_t rightMax)
    {
        const jack_latency_range_t left { leftMin, leftMax };
        const jack_latency_range_t right { rightMin, rightMax };
        driver.publishPlaybackLatencyRanges(left, right);
    }

    static jack_nframes_t cachedPlaybackLatencyLead(const JackAudioDriver& driver)
    {
        return driver.m_playbackLatencyLead.load(std::memory_order_acquire);
    }

    static jack_nframes_t currentPreparationRenderLead(const JackAudioDriver& driver)
    {
        return driver.m_currentPreparationRenderLeadFrames.load(std::memory_order_relaxed);
    }
};
}

namespace {
using namespace muse::audio;

constexpr uint64_t GENERATION = 42;

JackAudioDriver::TransportPreparation beginStartingEpisode(JackAudioDriver& driver, jack_nframes_t frame)
{
    JackAudioDriverTestAccess::observe(driver, JackTransportStopped, frame);

    JackAudioDriver::TransportObservation stopped;
    EXPECT_TRUE(driver.takeTransportObservation(stopped));
    EXPECT_EQ(stopped.kind, JackAudioDriver::TransportObservationKind::Stopped);

    EXPECT_EQ(JackAudioDriverTestAccess::synchronize(driver, JackTransportStarting, frame), 0);

    JackAudioDriver::TransportPreparation preparation;
    EXPECT_TRUE(driver.takePendingTransportPreparation(preparation));
    return preparation;
}

void completeSuccessfulPreparation(JackAudioDriver& driver,
                                   const JackAudioDriver::TransportPreparation& preparation)
{
    ASSERT_TRUE(driver.activateTransportForPreparation(preparation.generation, preparation.token));
    driver.completeTransportPreparation(preparation.generation, preparation.token, true);
    ASSERT_EQ(JackAudioDriverTestAccess::synchronize(driver, JackTransportStarting, preparation.frame), 1);
}

TEST(JackAudioDriverCallbackTests, FreshStoppedArmsAndPublishes)
{
    JackAudioDriver driver;
    JackAudioDriverTestAccess::prime(driver, GENERATION);

    JackAudioDriverTestAccess::observe(driver, JackTransportStopped, 1200);

    EXPECT_TRUE(driver.transportCallbackArmed());

    JackAudioDriver::TransportObservation observation;
    ASSERT_TRUE(driver.takeTransportObservation(observation));
    EXPECT_EQ(observation.kind, JackAudioDriver::TransportObservationKind::Stopped);
    EXPECT_EQ(observation.generation, GENERATION);
    EXPECT_EQ(observation.token, 0);
    EXPECT_EQ(observation.frame, 1200u);
    EXPECT_FALSE(observation.authoritativeNewPosition);
    EXPECT_FALSE(driver.takeTransportObservation(observation));
}

TEST(JackAudioDriverCallbackTests, StartingPublishesOnePreparationAndProcessObservationCannotOverwriteIt)
{
    JackAudioDriver driver;
    JackAudioDriverTestAccess::prime(driver, GENERATION);
    JackAudioDriverTestAccess::observe(driver, JackTransportStopped, 1200);

    JackAudioDriver::TransportObservation stopped;
    ASSERT_TRUE(driver.takeTransportObservation(stopped));

    EXPECT_EQ(JackAudioDriverTestAccess::synchronize(driver, JackTransportStarting, 2400), 0);

    JackAudioDriver::TransportPreparation preparation;
    ASSERT_TRUE(driver.takePendingTransportPreparation(preparation));
    EXPECT_EQ(preparation.generation, GENERATION);
    EXPECT_NE(preparation.token, 0);
    EXPECT_EQ(preparation.frame, 2400u);

    JackAudioDriverTestAccess::observe(driver, JackTransportStarting, 2500);
    EXPECT_EQ(JackAudioDriverTestAccess::synchronize(driver, JackTransportStarting, 2500), 0);

    EXPECT_FALSE(driver.takePendingTransportPreparation(preparation));
    EXPECT_FALSE(driver.takeTransportObservation(stopped));
}

TEST(JackAudioDriverCallbackTests, PlaybackLatencyIsSnapshottedPerStartingEpisode)
{
    JackAudioDriver driver;
    JackAudioDriverTestAccess::prime(driver, GENERATION);

    JackAudioDriverTestAccess::publishPlaybackLatencyRanges(driver, 0, 0, 0, 0);
    const JackAudioDriver::TransportPreparation zeroLead = beginStartingEpisode(driver, 3000);
    EXPECT_EQ(zeroLead.renderLeadFrames, 0u);

    driver.cancelPendingTransportWork();
    JackAudioDriverTestAccess::publishPlaybackLatencyRanges(driver, 10, 80, 20, 60);
    const JackAudioDriver::TransportPreparation first = beginStartingEpisode(driver, 4000);
    EXPECT_EQ(first.renderLeadFrames, 80u);
    EXPECT_EQ(JackAudioDriverTestAccess::cachedPlaybackLatencyLead(driver), 80u);

    JackAudioDriverTestAccess::publishPlaybackLatencyRanges(driver, 30, 90, 40, 120);
    EXPECT_EQ(JackAudioDriverTestAccess::cachedPlaybackLatencyLead(driver), 120u);
    EXPECT_EQ(JackAudioDriverTestAccess::currentPreparationRenderLead(driver), 80u);

    completeSuccessfulPreparation(driver, first);
    ASSERT_EQ(JackAudioDriverTestAccess::synchronize(driver, JackTransportRolling, first.frame), 1);
    ASSERT_EQ(JackAudioDriverTestAccess::synchronize(driver, JackTransportStarting, 5000), 0);

    JackAudioDriver::TransportPreparation relocated;
    ASSERT_TRUE(driver.takePendingTransportPreparation(relocated));
    EXPECT_EQ(relocated.generation, GENERATION);
    EXPECT_NE(relocated.token, 0u);
    EXPECT_NE(relocated.token, first.token);
    EXPECT_EQ(relocated.frame, 5000u);
    EXPECT_EQ(relocated.renderLeadFrames, 120u);
}

TEST(JackAudioDriverCallbackTests, MaximumPlaybackLatencySurvivesPreparationPublication)
{
    JackAudioDriver driver;
    JackAudioDriverTestAccess::prime(driver, GENERATION);
    constexpr jack_nframes_t maximum = std::numeric_limits<jack_nframes_t>::max();
    JackAudioDriverTestAccess::publishPlaybackLatencyRanges(driver, maximum - 1, maximum - 1, maximum, maximum);

    const JackAudioDriver::TransportPreparation preparation = beginStartingEpisode(driver, maximum);
    EXPECT_EQ(preparation.generation, GENERATION);
    EXPECT_NE(preparation.token, 0u);
    EXPECT_EQ(preparation.frame, maximum);
    EXPECT_EQ(preparation.renderLeadFrames, maximum);
}

TEST(JackAudioDriverCallbackTests, CancellationAndDriverInvalidationClearEpisodeLead)
{
    JackAudioDriver driver;
    JackAudioDriverTestAccess::prime(driver, GENERATION);
    JackAudioDriverTestAccess::publishPlaybackLatencyRanges(driver, 0, 64, 0, 32);

    const JackAudioDriver::TransportPreparation preparation = beginStartingEpisode(driver, 6000);
    ASSERT_EQ(preparation.renderLeadFrames, 64u);

    driver.completeTransportPreparation(GENERATION + 1, preparation.token, false);
    EXPECT_EQ(JackAudioDriverTestAccess::currentPreparationRenderLead(driver), 64u);

    driver.cancelPendingTransportWork();
    EXPECT_EQ(JackAudioDriverTestAccess::currentPreparationRenderLead(driver), 0u);
    EXPECT_FALSE(driver.isTransportPreparationCurrent(preparation.generation, preparation.token));

    const JackAudioDriver::TransportPreparation replacement = beginStartingEpisode(driver, 7000);
    ASSERT_EQ(replacement.renderLeadFrames, 64u);
    driver.configureTransport(GENERATION + 1, true);
    EXPECT_EQ(JackAudioDriverTestAccess::currentPreparationRenderLead(driver), 0u);
    EXPECT_FALSE(driver.isTransportPreparationCurrent(replacement.generation, replacement.token));
}

TEST(JackAudioDriverCallbackTests, StaleCompletionIsIgnoredAndActivatedSuccessEnablesRolling)
{
    JackAudioDriver driver;
    JackAudioDriverTestAccess::prime(driver, GENERATION);
    const JackAudioDriver::TransportPreparation preparation = beginStartingEpisode(driver, 3600);
    ASSERT_NE(preparation.token, 0);

    driver.completeTransportPreparation(GENERATION + 1, preparation.token, true);
    driver.completeTransportPreparation(GENERATION, preparation.token + 1, true);
    EXPECT_EQ(JackAudioDriverTestAccess::synchronize(driver, JackTransportStarting, preparation.frame), 0);
    EXPECT_EQ(JackAudioDriverTestAccess::releasedToken(driver), 0);
    EXPECT_EQ(JackAudioDriverTestAccess::renderEligibleToken(driver), 0);

    ASSERT_TRUE(driver.activateTransportForPreparation(preparation.generation, preparation.token));
    driver.completeTransportPreparation(preparation.generation, preparation.token, true);
    EXPECT_EQ(JackAudioDriverTestAccess::synchronize(driver, JackTransportStarting, preparation.frame), 1);
    EXPECT_EQ(JackAudioDriverTestAccess::releasedToken(driver), preparation.token);
    EXPECT_EQ(JackAudioDriverTestAccess::renderEligibleToken(driver), preparation.token);

    EXPECT_EQ(JackAudioDriverTestAccess::synchronize(driver, JackTransportRolling, preparation.frame), 1);
    EXPECT_EQ(JackAudioDriverTestAccess::renderEligibleToken(driver), preparation.token);

    JackAudioDriver::TransportObservation observation;
    EXPECT_FALSE(driver.takeTransportObservation(observation));
}

TEST(JackAudioDriverCallbackTests, FailedPreparationClearsEpisodeAndDoesNotEnableRolling)
{
    JackAudioDriver driver;
    JackAudioDriverTestAccess::prime(driver, GENERATION);
    JackAudioDriverTestAccess::publishPlaybackLatencyRanges(driver, 0, 96, 0, 48);
    const JackAudioDriver::TransportPreparation preparation = beginStartingEpisode(driver, 4800);
    ASSERT_NE(preparation.token, 0);
    ASSERT_EQ(preparation.renderLeadFrames, 96u);

    ASSERT_TRUE(driver.activateTransportForPreparation(preparation.generation, preparation.token));
    driver.completeTransportPreparation(preparation.generation, preparation.token, false);
    EXPECT_EQ(JackAudioDriverTestAccess::synchronize(driver, JackTransportStarting, preparation.frame), 1);
    EXPECT_EQ(JackAudioDriverTestAccess::releasedToken(driver), 0);
    EXPECT_EQ(JackAudioDriverTestAccess::renderEligibleToken(driver), 0);
    EXPECT_EQ(JackAudioDriverTestAccess::currentPreparationRenderLead(driver), 0u);
    EXPECT_FALSE(driver.isTransportPreparationCurrent(preparation.generation, preparation.token));

    EXPECT_EQ(JackAudioDriverTestAccess::synchronize(driver, JackTransportRolling, preparation.frame), 1);

    JackAudioDriver::TransportObservation observation;
    ASSERT_TRUE(driver.takeTransportObservation(observation));
    EXPECT_EQ(observation.kind, JackAudioDriver::TransportObservationKind::RollingUnprepared);
    EXPECT_EQ(observation.generation, GENERATION);
    EXPECT_EQ(observation.token, 0u);
    EXPECT_EQ(observation.frame, preparation.frame);
    EXPECT_EQ(JackAudioDriverTestAccess::renderEligibleToken(driver), 0);
}

TEST(JackAudioDriverCallbackTests, UnpreparedRollingRejectsLateSuccess)
{
    JackAudioDriver driver;
    JackAudioDriverTestAccess::prime(driver, GENERATION);
    const JackAudioDriver::TransportPreparation preparation = beginStartingEpisode(driver, 6000);
    ASSERT_NE(preparation.token, 0);

    EXPECT_EQ(JackAudioDriverTestAccess::synchronize(driver, JackTransportRolling, preparation.frame), 1);

    JackAudioDriver::TransportObservation observation;
    ASSERT_TRUE(driver.takeTransportObservation(observation));
    EXPECT_EQ(observation.kind, JackAudioDriver::TransportObservationKind::RollingUnprepared);

    EXPECT_FALSE(driver.activateTransportForPreparation(preparation.generation, preparation.token));
    driver.completeTransportPreparation(preparation.generation, preparation.token, true);
    EXPECT_FALSE(driver.isTransportPreparationCurrent(preparation.generation, preparation.token));
    EXPECT_EQ(JackAudioDriverTestAccess::renderEligibleToken(driver), 0);
}

TEST(JackAudioDriverCallbackTests, RollingBeforeStoppedDoesNotArmOrPrepare)
{
    JackAudioDriver driver;
    JackAudioDriverTestAccess::prime(driver, GENERATION);

    EXPECT_EQ(JackAudioDriverTestAccess::synchronize(driver, JackTransportRolling, 7200), 1);
    EXPECT_FALSE(driver.transportCallbackArmed());

    JackAudioDriver::TransportObservation observation;
    ASSERT_TRUE(driver.takeTransportObservation(observation));
    EXPECT_EQ(observation.kind, JackAudioDriver::TransportObservationKind::RollingUnprepared);

    EXPECT_EQ(JackAudioDriverTestAccess::synchronize(driver, JackTransportStarting, 7200), 1);
    JackAudioDriver::TransportPreparation preparation;
    EXPECT_FALSE(driver.takePendingTransportPreparation(preparation));
}

TEST(JackAudioDriverCallbackTests, TransportStateGatePreservesLocalAndPreparedRendering)
{
    JackAudioDriver driver;
    EXPECT_TRUE(JackAudioDriverTestAccess::shouldRenderTransportState(driver, JackTransportStarting));

    JackAudioDriverTestAccess::prime(driver, GENERATION);
    EXPECT_TRUE(JackAudioDriverTestAccess::shouldRenderTransportState(driver, JackTransportRolling));

    const JackAudioDriver::TransportPreparation preparation = beginStartingEpisode(driver, 8400);
    ASSERT_NE(preparation.token, 0);
    EXPECT_TRUE(JackAudioDriverTestAccess::shouldRenderTransportState(driver, JackTransportStopped));
    EXPECT_FALSE(JackAudioDriverTestAccess::shouldRenderTransportState(driver, JackTransportStarting));
    EXPECT_FALSE(JackAudioDriverTestAccess::shouldRenderTransportState(driver, JackTransportRolling));
    EXPECT_FALSE(JackAudioDriverTestAccess::shouldRenderTransportState(
                     driver, static_cast<jack_transport_state_t>(99)));

    completeSuccessfulPreparation(driver, preparation);
    EXPECT_TRUE(JackAudioDriverTestAccess::shouldRenderTransportState(driver, JackTransportRolling));
    EXPECT_TRUE(JackAudioDriverTestAccess::shouldRenderTransportState(driver, JackTransportLooping));

    driver.setTransportSyncEnabled(false);
    EXPECT_TRUE(JackAudioDriverTestAccess::shouldRenderTransportState(driver, JackTransportStarting));
}

TEST(JackAudioDriverCallbackTests, FailedPreparationRemainsSilentAtFirstRolling)
{
    JackAudioDriver driver;
    JackAudioDriverTestAccess::prime(driver, GENERATION);
    const JackAudioDriver::TransportPreparation preparation = beginStartingEpisode(driver, 9600);
    ASSERT_NE(preparation.token, 0);

    ASSERT_TRUE(driver.activateTransportForPreparation(preparation.generation, preparation.token));
    driver.completeTransportPreparation(preparation.generation, preparation.token, false);
    ASSERT_EQ(JackAudioDriverTestAccess::synchronize(driver, JackTransportStarting, preparation.frame), 1);

    EXPECT_FALSE(JackAudioDriverTestAccess::shouldRenderTransportState(driver, JackTransportRolling));
}

TEST(JackAudioDriverCallbackTests, RenderLeadPreservesStartingSilenceAndLogicalFrameContinuity)
{
    constexpr jack_nframes_t frame = 12000;
    constexpr jack_nframes_t period = 256;
    constexpr jack_nframes_t renderLead = 6160;

    JackAudioDriver driver;
    JackAudioDriverTestAccess::prime(driver, GENERATION);
    JackAudioDriverTestAccess::publishPlaybackLatencyRanges(driver, 0, renderLead, 0, renderLead - 16);
    const JackAudioDriver::TransportPreparation preparation = beginStartingEpisode(driver, frame);
    ASSERT_EQ(preparation.renderLeadFrames, renderLead);
    EXPECT_FALSE(JackAudioDriverTestAccess::shouldRenderTransportState(driver, JackTransportStarting));
    completeSuccessfulPreparation(driver, preparation);

    ASSERT_TRUE(JackAudioDriverTestAccess::shouldRenderTransportState(driver, JackTransportRolling));
    JackAudioDriverTestAccess::observeRenderedTransportBlock(driver, frame, period);
    EXPECT_FALSE(driver.takeRuntimeStatus().transportFrameDiscontinuity);

    JackAudioDriverTestAccess::observeRenderedTransportBlock(driver, frame + period, period);
    JackAudioDriverTestAccess::observeRenderedTransportBlock(driver, frame + 2 * period, period);
    EXPECT_FALSE(driver.takeRuntimeStatus().transportFrameDiscontinuity);
}

TEST(JackAudioDriverCallbackTests, FrameDiscontinuitiesReportExactPairAndRebase)
{
    constexpr jack_nframes_t frame = 15000;
    constexpr jack_nframes_t period = 1024;

    JackAudioDriver driver;
    JackAudioDriverTestAccess::resetExpectedRenderFrame(driver, frame);

    JackAudioDriverTestAccess::observeRenderedTransportBlock(driver, frame + period, period);
    JackAudioDriver::RuntimeStatus status = driver.takeRuntimeStatus();
    ASSERT_TRUE(status.transportFrameDiscontinuity);
    EXPECT_EQ(status.expectedTransportFrame, frame);
    EXPECT_EQ(status.observedTransportFrame, frame + period);

    JackAudioDriverTestAccess::observeRenderedTransportBlock(driver, frame + 3 * period, period);
    status = driver.takeRuntimeStatus();
    ASSERT_TRUE(status.transportFrameDiscontinuity);
    EXPECT_EQ(status.expectedTransportFrame, frame + 2 * period);
    EXPECT_EQ(status.observedTransportFrame, frame + 3 * period);

    JackAudioDriverTestAccess::observeRenderedTransportBlock(driver, frame + 4 * period, period);
    EXPECT_FALSE(driver.takeRuntimeStatus().transportFrameDiscontinuity);
}

TEST(JackAudioDriverCallbackTests, StopLocateInvalidationAndClearDiscardFrameExpectation)
{
    constexpr jack_nframes_t frame = 20000;
    constexpr jack_nframes_t period = 2048;

    JackAudioDriver driver;
    JackAudioDriverTestAccess::prime(driver, GENERATION);

    JackAudioDriverTestAccess::resetExpectedRenderFrame(driver, frame);
    JackAudioDriverTestAccess::observeRenderedTransportBlock(driver, frame + period, period);
    JackAudioDriverTestAccess::observe(driver, JackTransportStopped, frame, true);
    EXPECT_FALSE(driver.takeRuntimeStatus().transportFrameDiscontinuity);

    JackAudioDriverTestAccess::resetExpectedRenderFrame(driver, frame);
    JackAudioDriverTestAccess::observeRenderedTransportBlock(driver, frame + period, period);
    driver.cancelPendingTransportWork();
    EXPECT_FALSE(driver.takeRuntimeStatus().transportFrameDiscontinuity);

    JackAudioDriverTestAccess::resetExpectedRenderFrame(driver, frame);
    JackAudioDriverTestAccess::observeRenderedTransportBlock(driver, frame + period, period);
    JackAudioDriverTestAccess::clearExpectedRenderFrame(driver);
    EXPECT_FALSE(driver.takeRuntimeStatus().transportFrameDiscontinuity);
}

TEST(JackAudioDriverCallbackTests, RenderFrameOverflowClearsExpectation)
{
    constexpr jack_nframes_t period = 256;
    constexpr jack_nframes_t frame = std::numeric_limits<jack_nframes_t>::max() - period + 1;

    JackAudioDriver driver;
    JackAudioDriverTestAccess::resetExpectedRenderFrame(driver, frame);
    JackAudioDriverTestAccess::observeRenderedTransportBlock(driver, frame, period);
    JackAudioDriverTestAccess::observeRenderedTransportBlock(driver, frame - period, period);

    EXPECT_FALSE(driver.takeRuntimeStatus().transportFrameDiscontinuity);
}

TEST(JackAudioDriverCallbackTests, XrunDoesNotSynthesizeTransportObservation)
{
    JackAudioDriver driver;
    JackAudioDriverTestAccess::prime(driver, GENERATION);

    EXPECT_EQ(JackAudioDriverTestAccess::reportXrun(driver), 0);

    JackAudioDriver::TransportObservation observation;
    EXPECT_FALSE(driver.takeTransportObservation(observation));
    EXPECT_EQ(driver.takeRuntimeStatus().xruns, 1u);
}
}

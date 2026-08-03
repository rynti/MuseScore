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

    static bool observeRenderedTransportBlock(JackAudioDriver& driver, jack_nframes_t frame, jack_nframes_t nframes)
    {
        return driver.observeRenderedTransportBlock(frame, nframes);
    }

    static int reportXrun(JackAudioDriver& driver)
    {
        return JackAudioDriver::xrunCallback(&driver);
    }

    static void reportShutdown(JackAudioDriver& driver)
    {
        JackAudioDriver::shutdownCallback(&driver);
    }

    static void applyTransportConfiguration(JackAudioDriver& driver)
    {
        driver.synchronizeTransportEpoch();
    }

    static bool renderBlockedByDiscontinuity(const JackAudioDriver& driver)
    {
        return driver.m_transportRenderBlockedByDiscontinuity.load(std::memory_order_acquire);
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

TEST(JackAudioDriverCallbackTests, PreparationCarriesOnlyTheLogicalStartingFrame)
{
    JackAudioDriver driver;
    JackAudioDriverTestAccess::prime(driver, GENERATION);

    const JackAudioDriver::TransportPreparation first = beginStartingEpisode(driver, 3000);
    EXPECT_EQ(first.generation, GENERATION);
    EXPECT_NE(first.token, 0u);
    EXPECT_EQ(first.frame, 3000u);

    driver.cancelPendingTransportWork();
    constexpr jack_nframes_t maximum = std::numeric_limits<jack_nframes_t>::max();
    const JackAudioDriver::TransportPreparation preparation = beginStartingEpisode(driver, maximum);
    EXPECT_EQ(preparation.generation, GENERATION);
    EXPECT_NE(preparation.token, 0u);
    EXPECT_NE(preparation.token, first.token);
    EXPECT_EQ(preparation.frame, maximum);
}

TEST(JackAudioDriverCallbackTests, CancellationAndDriverInvalidationClearEpisode)
{
    JackAudioDriver driver;
    JackAudioDriverTestAccess::prime(driver, GENERATION);

    const JackAudioDriver::TransportPreparation preparation = beginStartingEpisode(driver, 6000);

    driver.completeTransportPreparation(GENERATION + 1, preparation.token, false);
    EXPECT_TRUE(driver.isTransportPreparationCurrent(preparation.generation, preparation.token));

    driver.cancelPendingTransportWork();
    EXPECT_FALSE(driver.isTransportPreparationCurrent(preparation.generation, preparation.token));

    const JackAudioDriver::TransportPreparation replacement = beginStartingEpisode(driver, 7000);
    driver.configureTransport(GENERATION + 1, true);
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
    const JackAudioDriver::TransportPreparation preparation = beginStartingEpisode(driver, 4800);
    ASSERT_NE(preparation.token, 0);

    ASSERT_TRUE(driver.activateTransportForPreparation(preparation.generation, preparation.token));
    driver.completeTransportPreparation(preparation.generation, preparation.token, false);
    EXPECT_EQ(JackAudioDriverTestAccess::synchronize(driver, JackTransportStarting, preparation.frame), 1);
    EXPECT_EQ(JackAudioDriverTestAccess::releasedToken(driver), 0);
    EXPECT_EQ(JackAudioDriverTestAccess::renderEligibleToken(driver), 0);
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

TEST(JackAudioDriverCallbackTests, FirstRollingBlockStartsAtPreparedFrameAndContiguousBlocksRemainRenderable)
{
    constexpr jack_nframes_t frame = 12000;
    constexpr jack_nframes_t period = 256;

    JackAudioDriver driver;
    JackAudioDriverTestAccess::prime(driver, GENERATION);
    const JackAudioDriver::TransportPreparation preparation = beginStartingEpisode(driver, frame);
    ASSERT_EQ(preparation.frame, frame);
    EXPECT_FALSE(JackAudioDriverTestAccess::shouldRenderTransportState(driver, JackTransportStarting));
    completeSuccessfulPreparation(driver, preparation);

    ASSERT_TRUE(JackAudioDriverTestAccess::shouldRenderTransportState(driver, JackTransportRolling));
    EXPECT_TRUE(JackAudioDriverTestAccess::observeRenderedTransportBlock(driver, frame, period));
    EXPECT_FALSE(driver.takeRuntimeStatus().transportFrameDiscontinuity);

    EXPECT_TRUE(JackAudioDriverTestAccess::observeRenderedTransportBlock(driver, frame + period, period));
    EXPECT_TRUE(JackAudioDriverTestAccess::observeRenderedTransportBlock(driver, frame + 2 * period, period));
    EXPECT_TRUE(JackAudioDriverTestAccess::shouldRenderTransportState(driver, JackTransportRolling));
    EXPECT_FALSE(driver.takeRuntimeStatus().transportFrameDiscontinuity);
}

TEST(JackAudioDriverCallbackTests, FirstBlockDiscontinuityRevokesRenderingAndPreservesExactPair)
{
    constexpr jack_nframes_t frame = 15000;
    constexpr jack_nframes_t period = 1024;

    JackAudioDriver driver;
    JackAudioDriverTestAccess::prime(driver, GENERATION);
    const JackAudioDriver::TransportPreparation preparation = beginStartingEpisode(driver, frame);
    completeSuccessfulPreparation(driver, preparation);

    EXPECT_FALSE(JackAudioDriverTestAccess::observeRenderedTransportBlock(driver, frame + period, period));
    EXPECT_TRUE(JackAudioDriverTestAccess::renderBlockedByDiscontinuity(driver));
    EXPECT_FALSE(JackAudioDriverTestAccess::shouldRenderTransportState(driver, JackTransportRolling));

    // A later Rolling observation must not invalidate the episode and erase
    // the callback diagnostic before the main thread can consume it.
    JackAudioDriverTestAccess::observe(driver, JackTransportRolling, frame + 2 * period);
    const JackAudioDriver::RuntimeStatus status = driver.takeRuntimeStatus();
    ASSERT_TRUE(status.transportFrameDiscontinuity);
    EXPECT_EQ(status.expectedTransportFrame, frame);
    EXPECT_EQ(status.observedTransportFrame, frame + period);
    EXPECT_TRUE(JackAudioDriverTestAccess::renderBlockedByDiscontinuity(driver));
}

TEST(JackAudioDriverCallbackTests, LaterDiscontinuitySilencesCurrentAndSubsequentBlocks)
{
    constexpr jack_nframes_t frame = 18000;
    constexpr jack_nframes_t period = 256;

    JackAudioDriver driver;
    JackAudioDriverTestAccess::prime(driver, GENERATION);
    const JackAudioDriver::TransportPreparation preparation = beginStartingEpisode(driver, frame);
    completeSuccessfulPreparation(driver, preparation);

    EXPECT_TRUE(JackAudioDriverTestAccess::observeRenderedTransportBlock(driver, frame, period));
    EXPECT_TRUE(JackAudioDriverTestAccess::observeRenderedTransportBlock(driver, frame + period, period));
    EXPECT_FALSE(JackAudioDriverTestAccess::observeRenderedTransportBlock(driver, frame + 3 * period, period));
    EXPECT_FALSE(JackAudioDriverTestAccess::observeRenderedTransportBlock(driver, frame + 4 * period, period));

    const JackAudioDriver::RuntimeStatus status = driver.takeRuntimeStatus();
    ASSERT_TRUE(status.transportFrameDiscontinuity);
    EXPECT_EQ(status.expectedTransportFrame, frame + 2 * period);
    EXPECT_EQ(status.observedTransportFrame, frame + 3 * period);
    EXPECT_FALSE(JackAudioDriverTestAccess::shouldRenderTransportState(driver, JackTransportRolling));
}

TEST(JackAudioDriverCallbackTests, StopLocateInvalidationAndClearDiscardFrameExpectation)
{
    constexpr jack_nframes_t frame = 20000;
    constexpr jack_nframes_t period = 2048;

    JackAudioDriver driver;
    JackAudioDriverTestAccess::prime(driver, GENERATION);

    JackAudioDriverTestAccess::resetExpectedRenderFrame(driver, frame);
    EXPECT_FALSE(JackAudioDriverTestAccess::observeRenderedTransportBlock(driver, frame + period, period));
    JackAudioDriverTestAccess::observe(driver, JackTransportStopped, frame, true);
    EXPECT_FALSE(JackAudioDriverTestAccess::renderBlockedByDiscontinuity(driver));
    EXPECT_FALSE(driver.takeRuntimeStatus().transportFrameDiscontinuity);

    JackAudioDriverTestAccess::resetExpectedRenderFrame(driver, frame);
    EXPECT_FALSE(JackAudioDriverTestAccess::observeRenderedTransportBlock(driver, frame + period, period));
    EXPECT_EQ(JackAudioDriverTestAccess::synchronize(driver, JackTransportStarting, frame + 2 * period), 0);
    EXPECT_FALSE(JackAudioDriverTestAccess::renderBlockedByDiscontinuity(driver));
    EXPECT_FALSE(driver.takeRuntimeStatus().transportFrameDiscontinuity);

    JackAudioDriverTestAccess::resetExpectedRenderFrame(driver, frame);
    EXPECT_FALSE(JackAudioDriverTestAccess::observeRenderedTransportBlock(driver, frame + period, period));
    driver.cancelPendingTransportWork();
    EXPECT_FALSE(JackAudioDriverTestAccess::renderBlockedByDiscontinuity(driver));
    EXPECT_FALSE(driver.takeRuntimeStatus().transportFrameDiscontinuity);
}

TEST(JackAudioDriverCallbackTests, SyncDisableDriverReplacementAndServerLossClearBlockedEpisode)
{
    constexpr jack_nframes_t frame = 17000;
    constexpr jack_nframes_t period = 256;

    JackAudioDriver driver;
    JackAudioDriverTestAccess::prime(driver, GENERATION);

    JackAudioDriverTestAccess::resetExpectedRenderFrame(driver, frame);
    EXPECT_FALSE(JackAudioDriverTestAccess::observeRenderedTransportBlock(driver, frame + period, period));
    driver.configureTransport(GENERATION, false);
    JackAudioDriverTestAccess::applyTransportConfiguration(driver);
    EXPECT_FALSE(JackAudioDriverTestAccess::renderBlockedByDiscontinuity(driver));
    EXPECT_FALSE(driver.takeRuntimeStatus().transportFrameDiscontinuity);

    JackAudioDriverTestAccess::prime(driver, GENERATION);
    JackAudioDriverTestAccess::resetExpectedRenderFrame(driver, frame);
    EXPECT_FALSE(JackAudioDriverTestAccess::observeRenderedTransportBlock(driver, frame + period, period));
    driver.configureTransport(GENERATION + 1, true);
    JackAudioDriverTestAccess::applyTransportConfiguration(driver);
    EXPECT_FALSE(JackAudioDriverTestAccess::renderBlockedByDiscontinuity(driver));
    EXPECT_FALSE(driver.takeRuntimeStatus().transportFrameDiscontinuity);

    JackAudioDriverTestAccess::resetExpectedRenderFrame(driver, frame);
    EXPECT_FALSE(JackAudioDriverTestAccess::observeRenderedTransportBlock(driver, frame + period, period));
    JackAudioDriverTestAccess::reportShutdown(driver);
    EXPECT_FALSE(JackAudioDriverTestAccess::renderBlockedByDiscontinuity(driver));
    EXPECT_FALSE(driver.takeRuntimeStatus().transportFrameDiscontinuity);
}

TEST(JackAudioDriverCallbackTests, RenderFrameProgressionWrapsWithJackFrameType)
{
    constexpr jack_nframes_t period = 256;
    constexpr jack_nframes_t frame = std::numeric_limits<jack_nframes_t>::max() - period + 1;

    JackAudioDriver driver;
    JackAudioDriverTestAccess::resetExpectedRenderFrame(driver, frame);
    EXPECT_TRUE(JackAudioDriverTestAccess::observeRenderedTransportBlock(driver, frame, period));
    EXPECT_TRUE(JackAudioDriverTestAccess::observeRenderedTransportBlock(driver, 0, period));

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

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

    static int reportXrun(JackAudioDriver& driver)
    {
        return JackAudioDriver::xrunCallback(&driver);
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

TEST(JackAudioDriverCallbackTests, FailedPreparationReleasesButDoesNotEnableRolling)
{
    JackAudioDriver driver;
    JackAudioDriverTestAccess::prime(driver, GENERATION);
    const JackAudioDriver::TransportPreparation preparation = beginStartingEpisode(driver, 4800);
    ASSERT_NE(preparation.token, 0);

    ASSERT_TRUE(driver.activateTransportForPreparation(preparation.generation, preparation.token));
    driver.completeTransportPreparation(preparation.generation, preparation.token, false);
    EXPECT_EQ(JackAudioDriverTestAccess::synchronize(driver, JackTransportStarting, preparation.frame), 1);
    EXPECT_EQ(JackAudioDriverTestAccess::releasedToken(driver), preparation.token);
    EXPECT_EQ(JackAudioDriverTestAccess::renderEligibleToken(driver), 0);

    EXPECT_EQ(JackAudioDriverTestAccess::synchronize(driver, JackTransportRolling, preparation.frame), 1);

    JackAudioDriver::TransportObservation observation;
    ASSERT_TRUE(driver.takeTransportObservation(observation));
    EXPECT_EQ(observation.kind, JackAudioDriver::TransportObservationKind::RollingUnprepared);
    EXPECT_EQ(observation.generation, GENERATION);
    EXPECT_EQ(observation.token, preparation.token);
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

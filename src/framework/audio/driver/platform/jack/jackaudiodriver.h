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

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

#include <jack/jack.h>
#include <jack/transport.h>

#include "iaudiodriver.h"
#include "jacktransportepisodetracker.h"

namespace muse::audio {
class JackAudioDriverTestAccess;

class JackAudioDriver : public IAudioDriver
{
public:
    enum class TransportObservationKind : uint8_t {
        Stopped = 0,
        AuthoritativeLocate,
        RollingUnprepared,
    };

    struct TransportPreparation {
        uint64_t generation = 0;
        uint64_t token = 0;
        jack_nframes_t frame = 0;
        jack_nframes_t renderLeadFrames = 0;
    };

    struct TransportObservation {
        TransportObservationKind kind = TransportObservationKind::Stopped;
        uint64_t generation = 0;
        uint64_t token = 0;
        jack_nframes_t frame = 0;
        bool authoritativeNewPosition = false;
    };

    struct RuntimeStatus {
        bool serverShutdown = false;
        bool sampleRateChanged = false;
        bool bufferSizeChanged = false;
        bool bufferSizeTooLarge = false;
        jack_nframes_t bufferSize = 0;
        jack_nframes_t sampleRate = 0;
        uint32_t xruns = 0;
        bool transportFrameDiscontinuity = false;
        jack_nframes_t expectedTransportFrame = 0;
        jack_nframes_t observedTransportFrame = 0;
    };

    JackAudioDriver() = default;
    ~JackAudioDriver() override;

    void init() override;

    std::string name() const override;
    AudioDeviceID defaultDevice() const override;

    bool open(const Spec& spec, Spec* activeSpec) override;
    void close() override;
    bool isOpened() const override;

    const Spec& activeSpec() const override;
    async::Channel<Spec> activeSpecChanged() const override;

    std::vector<samples_t> availableOutputDeviceBufferSizes() const override;
    std::vector<sample_rate_t> availableOutputDeviceSampleRates() const override;
    AudioDeviceList availableOutputDevices() const override;
    async::Notification availableOutputDevicesChanged() const override;

    // JACK transport bridge. Configuration and requests are main-thread only;
    // take/complete methods exchange bounded scalar state with JACK callbacks.
    void configureTransport(uint64_t generation, bool requested);
    void setTransportSyncEnabled(bool enabled);
    uint64_t transportGeneration() const;
    bool transportSyncEnabled() const;
    bool transportCallbackArmed() const;

    bool takePendingTransportPreparation(TransportPreparation& preparation);
    bool takeTransportObservation(TransportObservation& observation);
    bool isTransportPreparationCurrent(uint64_t generation, uint64_t token) const;
    bool activateTransportForPreparation(uint64_t generation, uint64_t token);
    void completeTransportPreparation(uint64_t generation, uint64_t token, bool success);
    void cancelPendingTransportWork();

    bool requestPlay(jack_nframes_t frame);
    bool requestPause();
    bool requestStop(jack_nframes_t frame = 0);
    bool requestSeek(jack_nframes_t frame);

    RuntimeStatus takeRuntimeStatus();

private:
    friend class JackAudioDriverTestAccess;

    struct AtomicPreparationSlot {
        std::atomic<uint64_t> sequence { 0 };
        std::atomic<uint64_t> epoch { 0 };
        std::atomic<uint64_t> generation { 0 };
        std::atomic<uint64_t> token { 0 };
        std::atomic<jack_nframes_t> frame { 0 };
        std::atomic<jack_nframes_t> renderLeadFrames { 0 };
    };

    struct AtomicObservationSlot {
        std::atomic<uint64_t> sequence { 0 };
        std::atomic<uint64_t> epoch { 0 };
        std::atomic<uint64_t> generation { 0 };
        std::atomic<uint32_t> kind { 0 };
        std::atomic<uint64_t> token { 0 };
        std::atomic<jack_nframes_t> frame { 0 };
        std::atomic<bool> authoritativeNewPosition { false };
    };

    static int processCallback(jack_nframes_t nframes, void* context) noexcept;
    static int syncCallback(jack_transport_state_t state, jack_position_t* position, void* context) noexcept;
    static void shutdownCallback(void* context) noexcept;
    static void latencyCallback(jack_latency_callback_mode_t mode, void* context) noexcept;
    static int xrunCallback(void* context) noexcept;
    static int bufferSizeCallback(jack_nframes_t nframes, void* context) noexcept;
    static int sampleRateCallback(jack_nframes_t sampleRate, void* context) noexcept;

    int process(jack_nframes_t nframes) noexcept;
    int syncTransport(jack_transport_state_t state, const jack_position_t& position) noexcept;
    void updatePlaybackLatency(jack_latency_callback_mode_t mode) noexcept;
    void publishPlaybackLatencyRanges(const jack_latency_range_t& left, const jack_latency_range_t& right) noexcept;
    void observeTransport(jack_transport_state_t state, const jack_position_t& position, bool authoritative) noexcept;
    bool shouldRenderTransportState(jack_transport_state_t state) const noexcept;
    void resetExpectedRenderFrame(jack_nframes_t frame) noexcept;
    void observeRenderedTransportBlock(jack_nframes_t frame, jack_nframes_t nframes) noexcept;
    void clearExpectedRenderFrame() noexcept;
    void synchronizeTransportEpoch() noexcept;
    void publishPreparation(uint64_t generation, uint64_t token, jack_nframes_t frame,
                            jack_nframes_t renderLeadFrames) noexcept;
    void publishObservation(TransportObservationKind kind, uint64_t token, jack_nframes_t frame,
                            bool authoritativeNewPosition) noexcept;
    void beginStartingEpisode(const JackTransportEpisodeTracker::StartingEpisode& episode) noexcept;
    void invalidateTransportEpisode() noexcept;
    void cancelTransportEpisodeIfUnchanged(uint64_t token) noexcept;
    bool transportRequestsAvailable() const;
    void clearClientState();

    jack_client_t* m_client = nullptr;
    jack_port_t* m_leftOutputPort = nullptr;
    jack_port_t* m_rightOutputPort = nullptr;
    bool m_activated = false;

    std::vector<float> m_interleavedBuffer;
    std::size_t m_bufferCapacityFrames = 0;

    Spec m_activeSpec;
    async::Channel<Spec> m_activeSpecChanged;
    async::Notification m_availableOutputDevicesChanged;

    std::atomic<bool> m_closing { true };
    std::atomic<bool> m_serverShutdown { false };
    std::atomic<bool> m_sampleRateChanged { false };
    std::atomic<jack_nframes_t> m_reportedBufferSize { 0 };
    std::atomic<jack_nframes_t> m_reportedSampleRate { 0 };
    std::atomic<jack_nframes_t> m_openedSampleRate { 0 };
    std::atomic<uint32_t> m_xrunCount { 0 };
    std::atomic<bool> m_bufferSizeTooLarge { false };
    std::atomic<uint32_t> m_runtimeStatusFlags { 0 };
    std::atomic<bool> m_hasExpectedRenderFrame { false };
    std::atomic<jack_nframes_t> m_expectedRenderFrame { 0 };
    std::atomic<jack_nframes_t> m_transportFrameMismatchExpected { 0 };
    std::atomic<jack_nframes_t> m_transportFrameMismatchObserved { 0 };
    std::atomic<jack_nframes_t> m_leftPlaybackLatencyMin { 0 };
    std::atomic<jack_nframes_t> m_leftPlaybackLatencyMax { 0 };
    std::atomic<jack_nframes_t> m_rightPlaybackLatencyMin { 0 };
    std::atomic<jack_nframes_t> m_rightPlaybackLatencyMax { 0 };
    std::atomic<jack_nframes_t> m_playbackLatencyLead { 0 };

    std::atomic<uint64_t> m_transportGeneration { 0 };
    std::atomic<bool> m_transportRequested { false };
    std::atomic<uint64_t> m_transportRequestEpoch { 1 };
    std::atomic<bool> m_transportCallbackArmed { false };

    std::atomic<uint64_t> m_currentPreparationGeneration { 0 };
    std::atomic<uint64_t> m_currentPreparationToken { 0 };
    std::atomic<jack_nframes_t> m_currentPreparationFrame { 0 };
    std::atomic<jack_nframes_t> m_currentPreparationRenderLeadFrames { 0 };
    std::atomic<uint64_t> m_mainActivatedPreparationToken { 0 };
    std::atomic<uint64_t> m_completionGeneration { 0 };
    std::atomic<uint64_t> m_completionToken { 0 };
    std::atomic<uint32_t> m_completionResult { 0 };
    std::atomic<uint64_t> m_releasedPreparationToken { 0 };
    std::atomic<uint64_t> m_renderEligibleToken { 0 };

    AtomicPreparationSlot m_preparationSlot;
    AtomicObservationSlot m_observationSlot;
    uint64_t m_lastTakenPreparationSequence = 0;
    uint64_t m_lastTakenObservationSequence = 0;

    // Written only by JACK's sync/process callback domain and reset after
    // callback quiescence.
    uint64_t m_callbackTransportEpoch = 0;
    bool m_callbackHasObservation = false;
    jack_transport_state_t m_callbackLastState = JackTransportStopped;
    jack_nframes_t m_callbackLastFrame = 0;
    JackTransportEpisodeTracker m_callbackEpisodeTracker;
};
}

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

#include "jackaudiodriver.h"

#include <algorithm>
#include <limits>
#include <new>

#include "log.h"
#include "translation.h"

using namespace muse;
using namespace muse::audio;

namespace {
constexpr audioch_t JACK_OUTPUT_CHANNELS = 2;
constexpr char JACK_CLIENT_NAME[] = "MuseScore";
constexpr char JACK_LEFT_PORT_NAME[] = "audio_out_left";
constexpr char JACK_RIGHT_PORT_NAME[] = "audio_out_right";

constexpr uint32_t COMPLETION_NONE = 0;
constexpr uint32_t COMPLETION_SUCCESS = 1;
constexpr uint32_t COMPLETION_FAILURE = 2;

constexpr uint32_t STATUS_SERVER_SHUTDOWN = 1U << 0;
constexpr uint32_t STATUS_SAMPLE_RATE_CHANGED = 1U << 1;
constexpr uint32_t STATUS_BUFFER_SIZE_CHANGED = 1U << 2;
constexpr uint32_t STATUS_BUFFER_SIZE_TOO_LARGE = 1U << 3;
constexpr uint32_t STATUS_XRUN = 1U << 4;
constexpr uint32_t STATUS_TRANSPORT_FRAME_DISCONTINUITY = 1U << 5;

constexpr bool isTransportStarting(jack_transport_state_t state) noexcept
{
    // JackTransportNetStarting is a JACK2/NetJack extension that is absent
    // from JACK1 headers, including those used by Linux CI.
    return state == JackTransportStarting;
}

static_assert(std::atomic<bool>::is_always_lock_free);
static_assert(std::atomic<jack_nframes_t>::is_always_lock_free);
static_assert(std::atomic<uint32_t>::is_always_lock_free);
static_assert(std::atomic<uint64_t>::is_always_lock_free);
}

JackAudioDriver::~JackAudioDriver()
{
    close();
}

void JackAudioDriver::init()
{
    // JACK exposes one server-owned output target. There is no device list to
    // monitor inside MuseScore.
}

std::string JackAudioDriver::name() const
{
    return "JACK";
}

AudioDeviceID JackAudioDriver::defaultDevice() const
{
    return DEFAULT_DEVICE_ID;
}

bool JackAudioDriver::open(const Spec& spec, Spec* activeSpec)
{
    close();

    if (!spec.isValid()) {
        LOGE() << "Cannot open JACK with an invalid audio specification";
        return false;
    }

    if (spec.deviceId != DEFAULT_DEVICE_ID) {
        LOGE() << "JACK only supports the default pseudo-device, requested: " << spec.deviceId;
        return false;
    }

    if (spec.output.audioChannelCount != JACK_OUTPUT_CHANNELS) {
        LOGE() << "JACK v1 requires stereo output, requested channels: " << int(spec.output.audioChannelCount);
        return false;
    }

    jack_status_t status = JackFailure;
    m_client = jack_client_open(JACK_CLIENT_NAME, JackNoStartServer, &status);
    if (!m_client) {
        LOGE() << "Failed to open JACK client; is the JACK server running? Status: " << int(status);
        return false;
    }

    const jack_nframes_t serverSampleRate = jack_get_sample_rate(m_client);
    const jack_nframes_t serverPeriod = jack_get_buffer_size(m_client);
    if (serverSampleRate == 0 || serverPeriod == 0) {
        LOGE() << "JACK returned an invalid sample rate or period";
        close();
        return false;
    }

    m_bufferCapacityFrames = std::max<size_t>(serverPeriod, MAXIMUM_BUFFER_SIZE);
    constexpr size_t bytesPerFrame = JACK_OUTPUT_CHANNELS * sizeof(float);
    if (m_bufferCapacityFrames > static_cast<size_t>(std::numeric_limits<int>::max()) / bytesPerFrame
        || m_bufferCapacityFrames > std::numeric_limits<size_t>::max() / JACK_OUTPUT_CHANNELS) {
        LOGE() << "JACK period is too large for the audio callback interface: " << serverPeriod;
        close();
        return false;
    }

    try {
        m_interleavedBuffer.resize(m_bufferCapacityFrames * JACK_OUTPUT_CHANNELS);
    } catch (const std::bad_alloc&) {
        LOGE() << "Unable to allocate the JACK audio buffer for " << m_bufferCapacityFrames << " frames";
        close();
        return false;
    }

    m_leftOutputPort = jack_port_register(m_client, JACK_LEFT_PORT_NAME, JACK_DEFAULT_AUDIO_TYPE,
                                          JackPortIsOutput | JackPortIsTerminal, 0);
    if (!m_leftOutputPort) {
        LOGE() << "Failed to register JACK output port: " << JACK_LEFT_PORT_NAME;
        close();
        return false;
    }

    m_rightOutputPort = jack_port_register(m_client, JACK_RIGHT_PORT_NAME, JACK_DEFAULT_AUDIO_TYPE,
                                           JackPortIsOutput | JackPortIsTerminal, 0);
    if (!m_rightOutputPort) {
        LOGE() << "Failed to register JACK output port: " << JACK_RIGHT_PORT_NAME;
        close();
        return false;
    }

    m_activeSpec = spec;
    m_activeSpec.deviceId = DEFAULT_DEVICE_ID;
    m_activeSpec.output.sampleRate = serverSampleRate;
    m_activeSpec.output.samplesPerChannel = serverPeriod;
    m_activeSpec.output.audioChannelCount = JACK_OUTPUT_CHANNELS;

    m_serverShutdown.store(false, std::memory_order_relaxed);
    m_sampleRateChanged.store(false, std::memory_order_relaxed);
    m_reportedBufferSize.store(serverPeriod, std::memory_order_relaxed);
    m_reportedSampleRate.store(serverSampleRate, std::memory_order_relaxed);
    m_openedSampleRate.store(serverSampleRate, std::memory_order_relaxed);
    m_xrunCount.store(0, std::memory_order_relaxed);
    m_bufferSizeTooLarge.store(false, std::memory_order_relaxed);
    m_runtimeStatusFlags.store(0, std::memory_order_relaxed);
    m_transportCallbackArmed.store(false, std::memory_order_relaxed);
    invalidateTransportEpisode();

    if (jack_set_process_callback(m_client, &JackAudioDriver::processCallback, this) != 0
        || jack_set_sync_callback(m_client, &JackAudioDriver::syncCallback, this) != 0
        || jack_set_xrun_callback(m_client, &JackAudioDriver::xrunCallback, this) != 0
        || jack_set_buffer_size_callback(m_client, &JackAudioDriver::bufferSizeCallback, this) != 0
        || jack_set_sample_rate_callback(m_client, &JackAudioDriver::sampleRateCallback, this) != 0) {
        LOGE() << "Failed to register one or more JACK callbacks";
        close();
        return false;
    }

    if (jack_set_latency_callback && jack_port_get_latency_range) {
        if (jack_set_latency_callback(m_client, &JackAudioDriver::latencyCallback, this) != 0) {
            LOGE() << "Failed to register JACK playback-latency callback";
            close();
            return false;
        }
    } else {
        LOGW() << "JACK playback-latency API is unavailable; playback latency compensation is disabled";
    }

    jack_on_shutdown(m_client, &JackAudioDriver::shutdownCallback, this);

    // Everything observed by JACK callbacks is initialized before activation.
    // Keep rendering gated until activation and active-spec publication finish.
    if (jack_activate(m_client) != 0) {
        LOGE() << "Failed to activate JACK client";
        close();
        return false;
    }
    m_activated = true;

    if (m_serverShutdown.load(std::memory_order_acquire)) {
        LOGE() << "JACK server shut down while the client was being activated";
        close();
        return false;
    }

    if (activeSpec) {
        *activeSpec = m_activeSpec;
    }

    const char* actualClientName = jack_get_client_name(m_client);
    LOGI() << "Connected JACK client " << (actualClientName ? actualClientName : JACK_CLIENT_NAME)
           << " with period " << serverPeriod
           << ", sample rate " << serverSampleRate
           << ", channels " << int(JACK_OUTPUT_CHANNELS);

    m_activeSpecChanged.send(m_activeSpec);
    m_closing.store(false, std::memory_order_release);
    return true;
}

void JackAudioDriver::close()
{
    m_closing.store(true, std::memory_order_release);
    m_transportCallbackArmed.store(false, std::memory_order_release);
    invalidateTransportEpisode();
    m_transportRequestEpoch.fetch_add(1, std::memory_order_acq_rel);

    if (m_client) {
        if (m_activated && !m_serverShutdown.load(std::memory_order_acquire)) {
            if (jack_deactivate(m_client) != 0) {
                LOGW() << "Failed to deactivate JACK client cleanly";
            }
        }

        // Closing the client also releases its ports and waits for its JACK
        // callback activity to finish before callback-owned storage is freed.
        if (jack_client_close(m_client) != 0) {
            LOGW() << "Failed to close JACK client cleanly";
        }
    }

    clearClientState();
}

void JackAudioDriver::clearClientState()
{
    m_activated = false;
    m_client = nullptr;
    m_leftOutputPort = nullptr;
    m_rightOutputPort = nullptr;

    std::vector<float>().swap(m_interleavedBuffer);
    m_bufferCapacityFrames = 0;
    m_activeSpec = {};

    m_serverShutdown.store(false, std::memory_order_relaxed);
    m_sampleRateChanged.store(false, std::memory_order_relaxed);
    m_reportedBufferSize.store(0, std::memory_order_relaxed);
    m_reportedSampleRate.store(0, std::memory_order_relaxed);
    m_openedSampleRate.store(0, std::memory_order_relaxed);
    m_xrunCount.store(0, std::memory_order_relaxed);
    m_bufferSizeTooLarge.store(false, std::memory_order_relaxed);
    m_runtimeStatusFlags.store(0, std::memory_order_relaxed);
    m_leftPlaybackLatencyMin.store(0, std::memory_order_relaxed);
    m_leftPlaybackLatencyMax.store(0, std::memory_order_relaxed);
    m_rightPlaybackLatencyMin.store(0, std::memory_order_relaxed);
    m_rightPlaybackLatencyMax.store(0, std::memory_order_relaxed);
    m_playbackLatencyLead.store(0, std::memory_order_release);

    m_transportCallbackArmed.store(false, std::memory_order_relaxed);
    invalidateTransportEpisode();
    m_preparationSlot.sequence.store(0, std::memory_order_relaxed);
    m_observationSlot.sequence.store(0, std::memory_order_relaxed);
    m_lastTakenPreparationSequence = 0;
    m_lastTakenObservationSequence = 0;
    m_callbackTransportEpoch = 0;
    m_callbackHasObservation = false;
    m_callbackLastState = JackTransportStopped;
    m_callbackLastFrame = 0;
    m_callbackEpisodeTracker.observeNotStarting();
}

bool JackAudioDriver::isOpened() const
{
    return m_client != nullptr && m_activated && !m_serverShutdown.load(std::memory_order_acquire);
}

const JackAudioDriver::Spec& JackAudioDriver::activeSpec() const
{
    return m_activeSpec;
}

async::Channel<JackAudioDriver::Spec> JackAudioDriver::activeSpecChanged() const
{
    return m_activeSpecChanged;
}

std::vector<samples_t> JackAudioDriver::availableOutputDeviceBufferSizes() const
{
    const jack_nframes_t period = m_reportedBufferSize.load(std::memory_order_acquire);
    return period > 0 ? std::vector<samples_t> { period } : std::vector<samples_t> {};
}

std::vector<sample_rate_t> JackAudioDriver::availableOutputDeviceSampleRates() const
{
    const jack_nframes_t sampleRate = m_reportedSampleRate.load(std::memory_order_acquire);
    return sampleRate > 0 ? std::vector<sample_rate_t> { sampleRate } : std::vector<sample_rate_t> {};
}

AudioDeviceList JackAudioDriver::availableOutputDevices() const
{
    return { { DEFAULT_DEVICE_ID, muse::trc("audio", "JACK server") } };
}

async::Notification JackAudioDriver::availableOutputDevicesChanged() const
{
    return m_availableOutputDevicesChanged;
}

void JackAudioDriver::configureTransport(uint64_t generation, bool requested)
{
    if (m_transportGeneration.load(std::memory_order_acquire) == generation
        && m_transportRequested.load(std::memory_order_acquire) == requested) {
        return;
    }

    m_transportRequested.store(false, std::memory_order_release);
    m_transportGeneration.store(generation, std::memory_order_release);
    m_transportCallbackArmed.store(false, std::memory_order_release);
    invalidateTransportEpisode();
    m_transportRequestEpoch.fetch_add(1, std::memory_order_acq_rel);
    m_lastTakenPreparationSequence = m_preparationSlot.sequence.load(std::memory_order_acquire);
    m_lastTakenObservationSequence = m_observationSlot.sequence.load(std::memory_order_acquire);
    m_transportRequested.store(requested, std::memory_order_release);
}

void JackAudioDriver::setTransportSyncEnabled(bool enabled)
{
    if (m_transportRequested.load(std::memory_order_acquire) == enabled) {
        return;
    }

    m_transportRequested.store(false, std::memory_order_release);
    m_transportCallbackArmed.store(false, std::memory_order_release);
    invalidateTransportEpisode();
    m_transportRequestEpoch.fetch_add(1, std::memory_order_acq_rel);
    m_lastTakenPreparationSequence = m_preparationSlot.sequence.load(std::memory_order_acquire);
    m_lastTakenObservationSequence = m_observationSlot.sequence.load(std::memory_order_acquire);
    m_transportRequested.store(enabled, std::memory_order_release);
}

uint64_t JackAudioDriver::transportGeneration() const
{
    return m_transportGeneration.load(std::memory_order_acquire);
}

bool JackAudioDriver::transportSyncEnabled() const
{
    return m_transportRequested.load(std::memory_order_acquire);
}

bool JackAudioDriver::transportCallbackArmed() const
{
    return m_transportCallbackArmed.load(std::memory_order_acquire);
}

bool JackAudioDriver::takePendingTransportPreparation(TransportPreparation& preparation)
{
    // Slot operations are sequentially consistent so an unchanged even
    // sequence brackets one coherent set of independently lock-free fields.
    const uint64_t sequenceBefore = m_preparationSlot.sequence.load(std::memory_order_seq_cst);
    if (sequenceBefore == 0 || (sequenceBefore & 1U) != 0 || sequenceBefore == m_lastTakenPreparationSequence) {
        return false;
    }

    const uint64_t epoch = m_preparationSlot.epoch.load(std::memory_order_seq_cst);
    TransportPreparation snapshot;
    snapshot.generation = m_preparationSlot.generation.load(std::memory_order_seq_cst);
    snapshot.token = m_preparationSlot.token.load(std::memory_order_seq_cst);
    snapshot.frame = m_preparationSlot.frame.load(std::memory_order_seq_cst);
    snapshot.renderLeadFrames = m_preparationSlot.renderLeadFrames.load(std::memory_order_seq_cst);

    const uint64_t sequenceAfter = m_preparationSlot.sequence.load(std::memory_order_seq_cst);
    if (sequenceBefore != sequenceAfter || (sequenceAfter & 1U) != 0) {
        return false;
    }

    m_lastTakenPreparationSequence = sequenceAfter;
    if (epoch != m_transportRequestEpoch.load(std::memory_order_acquire)
        || snapshot.generation != transportGeneration()
        || !isTransportPreparationCurrent(snapshot.generation, snapshot.token)) {
        return false;
    }

    preparation = snapshot;
    return true;
}

bool JackAudioDriver::takeTransportObservation(TransportObservation& observation)
{
    const uint64_t sequenceBefore = m_observationSlot.sequence.load(std::memory_order_seq_cst);
    if (sequenceBefore == 0 || (sequenceBefore & 1U) != 0 || sequenceBefore == m_lastTakenObservationSequence) {
        return false;
    }

    const uint64_t epoch = m_observationSlot.epoch.load(std::memory_order_seq_cst);
    TransportObservation snapshot;
    snapshot.kind = static_cast<TransportObservationKind>(m_observationSlot.kind.load(std::memory_order_seq_cst));
    snapshot.generation = m_observationSlot.generation.load(std::memory_order_seq_cst);
    snapshot.token = m_observationSlot.token.load(std::memory_order_seq_cst);
    snapshot.frame = m_observationSlot.frame.load(std::memory_order_seq_cst);
    snapshot.authoritativeNewPosition = m_observationSlot.authoritativeNewPosition.load(std::memory_order_seq_cst);

    const uint64_t sequenceAfter = m_observationSlot.sequence.load(std::memory_order_seq_cst);
    if (sequenceBefore != sequenceAfter || (sequenceAfter & 1U) != 0) {
        return false;
    }

    m_lastTakenObservationSequence = sequenceAfter;
    if (epoch != m_transportRequestEpoch.load(std::memory_order_acquire)
        || snapshot.generation != transportGeneration()
        || !transportSyncEnabled()) {
        return false;
    }

    observation = snapshot;
    return true;
}

bool JackAudioDriver::isTransportPreparationCurrent(uint64_t generation, uint64_t token) const
{
    const uint64_t currentToken = m_currentPreparationToken.load(std::memory_order_acquire);
    return token != 0
           && transportSyncEnabled()
           && generation == transportGeneration()
           && token == currentToken
           && generation == m_currentPreparationGeneration.load(std::memory_order_relaxed);
}

bool JackAudioDriver::activateTransportForPreparation(uint64_t generation, uint64_t token)
{
    if (!isTransportPreparationCurrent(generation, token)) {
        return false;
    }

    m_mainActivatedPreparationToken.store(token, std::memory_order_release);
    if (!isTransportPreparationCurrent(generation, token)) {
        m_mainActivatedPreparationToken.store(0, std::memory_order_release);
        return false;
    }

    return true;
}

void JackAudioDriver::completeTransportPreparation(uint64_t generation, uint64_t token, bool success)
{
    if (!isTransportPreparationCurrent(generation, token)
        || (success && m_mainActivatedPreparationToken.load(std::memory_order_acquire) != token)) {
        return;
    }

    m_completionToken.store(0, std::memory_order_release);
    m_completionGeneration.store(generation, std::memory_order_relaxed);
    m_completionResult.store(success ? COMPLETION_SUCCESS : COMPLETION_FAILURE, std::memory_order_relaxed);
    m_completionToken.store(token, std::memory_order_release);
}

void JackAudioDriver::cancelPendingTransportWork()
{
    invalidateTransportEpisode();
}

bool JackAudioDriver::transportRequestsAvailable() const
{
    return m_client != nullptr
           && m_activated
           && !m_closing.load(std::memory_order_acquire)
           && !m_serverShutdown.load(std::memory_order_acquire)
           && transportSyncEnabled()
           && transportCallbackArmed();
}

bool JackAudioDriver::requestPlay(jack_nframes_t frame)
{
    if (!transportRequestsAvailable()) {
        return false;
    }

    const uint64_t observationFence = m_observationSlot.sequence.load(std::memory_order_acquire);
    const uint64_t previousToken = m_currentPreparationToken.load(std::memory_order_acquire);
    if (jack_transport_locate(m_client, frame) != 0) {
        return false;
    }

    cancelTransportEpisodeIfUnchanged(previousToken);
    jack_transport_start(m_client);
    m_lastTakenObservationSequence = observationFence;
    return true;
}

bool JackAudioDriver::requestPause()
{
    if (!transportRequestsAvailable()) {
        return false;
    }

    const uint64_t observationFence = m_observationSlot.sequence.load(std::memory_order_acquire);
    const uint64_t previousToken = m_currentPreparationToken.load(std::memory_order_acquire);
    jack_transport_stop(m_client);
    cancelTransportEpisodeIfUnchanged(previousToken);
    m_lastTakenObservationSequence = observationFence;
    return true;
}

bool JackAudioDriver::requestStop(jack_nframes_t frame)
{
    if (!transportRequestsAvailable()) {
        return false;
    }

    const uint64_t observationFence = m_observationSlot.sequence.load(std::memory_order_acquire);
    const uint64_t previousToken = m_currentPreparationToken.load(std::memory_order_acquire);
    jack_transport_stop(m_client);
    if (jack_transport_locate(m_client, frame) != 0) {
        cancelTransportEpisodeIfUnchanged(previousToken);
        return false;
    }

    cancelTransportEpisodeIfUnchanged(previousToken);
    m_lastTakenObservationSequence = observationFence;
    return true;
}

bool JackAudioDriver::requestSeek(jack_nframes_t frame)
{
    if (!transportRequestsAvailable()) {
        return false;
    }

    const uint64_t observationFence = m_observationSlot.sequence.load(std::memory_order_acquire);
    const uint64_t previousToken = m_currentPreparationToken.load(std::memory_order_acquire);
    if (jack_transport_locate(m_client, frame) != 0) {
        return false;
    }

    cancelTransportEpisodeIfUnchanged(previousToken);
    m_lastTakenObservationSequence = observationFence;
    return true;
}

JackAudioDriver::RuntimeStatus JackAudioDriver::takeRuntimeStatus()
{
    const uint32_t flags = m_runtimeStatusFlags.exchange(0, std::memory_order_acq_rel);

    RuntimeStatus status;
    status.serverShutdown = (flags & STATUS_SERVER_SHUTDOWN) != 0;
    status.sampleRateChanged = (flags & STATUS_SAMPLE_RATE_CHANGED) != 0;
    status.bufferSizeChanged = (flags & STATUS_BUFFER_SIZE_CHANGED) != 0;
    status.bufferSizeTooLarge = m_bufferSizeTooLarge.load(std::memory_order_acquire);
    status.bufferSize = m_reportedBufferSize.load(std::memory_order_acquire);
    status.sampleRate = m_reportedSampleRate.load(std::memory_order_acquire);
    status.xruns = m_xrunCount.exchange(0, std::memory_order_acq_rel);
    status.transportFrameDiscontinuity = (flags & STATUS_TRANSPORT_FRAME_DISCONTINUITY) != 0;
    if (status.transportFrameDiscontinuity) {
        status.expectedTransportFrame = m_transportFrameMismatchExpected.load(std::memory_order_relaxed);
        status.observedTransportFrame = m_transportFrameMismatchObserved.load(std::memory_order_relaxed);
    }

    if (status.bufferSizeChanged && !status.bufferSizeTooLarge && status.bufferSize > 0
        && m_activeSpec.isValid() && m_activeSpec.output.samplesPerChannel != status.bufferSize) {
        m_activeSpec.output.samplesPerChannel = status.bufferSize;
        m_activeSpecChanged.send(m_activeSpec);
    }

    return status;
}

int JackAudioDriver::processCallback(jack_nframes_t nframes, void* context) noexcept
{
    return static_cast<JackAudioDriver*>(context)->process(nframes);
}

int JackAudioDriver::syncCallback(jack_transport_state_t state, jack_position_t* position, void* context) noexcept
{
    if (!position) {
        return 1;
    }

    return static_cast<JackAudioDriver*>(context)->syncTransport(state, *position);
}

void JackAudioDriver::latencyCallback(jack_latency_callback_mode_t mode, void* context) noexcept
{
    static_cast<JackAudioDriver*>(context)->updatePlaybackLatency(mode);
}

void JackAudioDriver::updatePlaybackLatency(jack_latency_callback_mode_t mode) noexcept
{
    if (mode != JackPlaybackLatency || !jack_port_get_latency_range
        || !m_leftOutputPort || !m_rightOutputPort) {
        return;
    }

    jack_latency_range_t left {};
    jack_latency_range_t right {};
    jack_port_get_latency_range(m_leftOutputPort, JackPlaybackLatency, &left);
    jack_port_get_latency_range(m_rightOutputPort, JackPlaybackLatency, &right);
    publishPlaybackLatencyRanges(left, right);
}

void JackAudioDriver::publishPlaybackLatencyRanges(const jack_latency_range_t& left,
                                                   const jack_latency_range_t& right) noexcept
{
    m_leftPlaybackLatencyMin.store(left.min, std::memory_order_relaxed);
    m_leftPlaybackLatencyMax.store(left.max, std::memory_order_relaxed);
    m_rightPlaybackLatencyMin.store(right.min, std::memory_order_relaxed);
    m_rightPlaybackLatencyMax.store(right.max, std::memory_order_relaxed);
    m_playbackLatencyLead.store(std::max(left.max, right.max), std::memory_order_release);
}

void JackAudioDriver::synchronizeTransportEpoch() noexcept
{
    const uint64_t epoch = m_transportRequestEpoch.load(std::memory_order_acquire);
    if (epoch == m_callbackTransportEpoch) {
        return;
    }

    m_callbackTransportEpoch = epoch;
    m_callbackHasObservation = false;
    m_callbackLastState = JackTransportStopped;
    m_callbackLastFrame = 0;
    m_callbackEpisodeTracker.observeNotStarting();
    m_transportCallbackArmed.store(false, std::memory_order_release);
    invalidateTransportEpisode();
}

void JackAudioDriver::publishPreparation(uint64_t generation, uint64_t token, jack_nframes_t frame,
                                         jack_nframes_t renderLeadFrames) noexcept
{
    uint64_t sequence = m_preparationSlot.sequence.load(std::memory_order_seq_cst);
    if ((sequence & 1U) != 0) {
        ++sequence;
    }

    m_preparationSlot.sequence.store(sequence + 1, std::memory_order_seq_cst);
    m_preparationSlot.epoch.store(m_callbackTransportEpoch, std::memory_order_seq_cst);
    m_preparationSlot.generation.store(generation, std::memory_order_seq_cst);
    m_preparationSlot.token.store(token, std::memory_order_seq_cst);
    m_preparationSlot.frame.store(frame, std::memory_order_seq_cst);
    m_preparationSlot.renderLeadFrames.store(renderLeadFrames, std::memory_order_seq_cst);
    m_preparationSlot.sequence.store(sequence + 2, std::memory_order_seq_cst);
}

void JackAudioDriver::publishObservation(TransportObservationKind kind, uint64_t token, jack_nframes_t frame,
                                         bool authoritativeNewPosition) noexcept
{
    uint64_t sequence = m_observationSlot.sequence.load(std::memory_order_seq_cst);
    if ((sequence & 1U) != 0) {
        ++sequence;
    }

    m_observationSlot.sequence.store(sequence + 1, std::memory_order_seq_cst);
    m_observationSlot.epoch.store(m_callbackTransportEpoch, std::memory_order_seq_cst);
    m_observationSlot.generation.store(m_transportGeneration.load(std::memory_order_acquire), std::memory_order_seq_cst);
    m_observationSlot.kind.store(static_cast<uint32_t>(kind), std::memory_order_seq_cst);
    m_observationSlot.token.store(token, std::memory_order_seq_cst);
    m_observationSlot.frame.store(frame, std::memory_order_seq_cst);
    m_observationSlot.authoritativeNewPosition.store(authoritativeNewPosition, std::memory_order_seq_cst);
    m_observationSlot.sequence.store(sequence + 2, std::memory_order_seq_cst);
}

void JackAudioDriver::beginStartingEpisode(const JackTransportEpisodeTracker::StartingEpisode& episode) noexcept
{
    invalidateTransportEpisode();

    const uint64_t generation = m_transportGeneration.load(std::memory_order_acquire);
    const jack_nframes_t renderLeadFrames = m_playbackLatencyLead.load(std::memory_order_acquire);
    m_currentPreparationGeneration.store(generation, std::memory_order_relaxed);
    m_currentPreparationFrame.store(static_cast<jack_nframes_t>(episode.frame), std::memory_order_relaxed);
    m_currentPreparationRenderLeadFrames.store(renderLeadFrames, std::memory_order_relaxed);
    m_currentPreparationToken.store(episode.token, std::memory_order_release);

    publishPreparation(generation, episode.token, static_cast<jack_nframes_t>(episode.frame), renderLeadFrames);
}

void JackAudioDriver::invalidateTransportEpisode() noexcept
{
    clearExpectedRenderFrame();
    // The token is the authoritative identity and is cleared last. If this
    // races publication of a new callback-domain episode, the final token
    // store gives the two operations a safe last-writer-wins linearization:
    // either the new episode is complete, or cancellation leaves no identity.
    m_mainActivatedPreparationToken.store(0, std::memory_order_release);
    m_completionToken.store(0, std::memory_order_release);
    m_completionGeneration.store(0, std::memory_order_relaxed);
    m_completionResult.store(COMPLETION_NONE, std::memory_order_relaxed);
    m_releasedPreparationToken.store(0, std::memory_order_release);
    m_renderEligibleToken.store(0, std::memory_order_release);
    m_currentPreparationGeneration.store(0, std::memory_order_relaxed);
    m_currentPreparationFrame.store(0, std::memory_order_relaxed);
    m_currentPreparationRenderLeadFrames.store(0, std::memory_order_relaxed);
    m_currentPreparationToken.store(0, std::memory_order_release);
}

void JackAudioDriver::cancelTransportEpisodeIfUnchanged(uint64_t token) noexcept
{
    clearExpectedRenderFrame();
    if (token == 0) {
        return;
    }

    // A successful JACK request may synchronously cause the callback domain to
    // publish its replacement Starting token before the call returns. Cancel
    // only the identity that preceded the request; stale ancillary markers are
    // harmless because every consumer gates on the exact current token.
    m_currentPreparationToken.compare_exchange_strong(token, 0,
                                                       std::memory_order_acq_rel,
                                                       std::memory_order_acquire);
}

int JackAudioDriver::syncTransport(jack_transport_state_t state, const jack_position_t& position) noexcept
{
    if (m_closing.load(std::memory_order_acquire)
        || m_serverShutdown.load(std::memory_order_acquire)
        || !m_transportRequested.load(std::memory_order_acquire)) {
        return 1;
    }

    synchronizeTransportEpoch();
    if (!m_transportRequested.load(std::memory_order_acquire)) {
        return 1;
    }

    const bool starting = isTransportStarting(state);
    const bool rolling = state == JackTransportRolling || state == JackTransportLooping;

    if (state == JackTransportStopped) {
        // This callback is JACK's authoritative notification of a requested
        // position. Updating the query baseline here prevents the process
        // callback in the same cycle from replacing it with an ordinary event.
        invalidateTransportEpisode();
        m_callbackEpisodeTracker.observeNotStarting();
        m_transportCallbackArmed.store(true, std::memory_order_release);
        publishObservation(TransportObservationKind::AuthoritativeLocate, 0, position.frame, true);
        m_callbackHasObservation = true;
        m_callbackLastState = state;
        m_callbackLastFrame = position.frame;
        return 1;
    }

    const bool armed = m_transportCallbackArmed.load(std::memory_order_acquire);
    if (starting) {
        m_callbackHasObservation = true;
        m_callbackLastState = state;
        m_callbackLastFrame = position.frame;

        if (!armed) {
            m_callbackEpisodeTracker.observeNotStarting();
            return 1;
        }

        const JackTransportEpisodeTracker::StartingEpisode episode
            = m_callbackEpisodeTracker.observeStarting(position.frame);
        uint64_t token = m_currentPreparationToken.load(std::memory_order_acquire);
        if (episode.isNew) {
            beginStartingEpisode(episode);
            token = episode.token;
        }

        if (token == 0) {
            // Main-thread cancellation must never leave the global JACK graph
            // waiting for an episode MuseScore no longer owns.
            return 1;
        }

        const uint64_t completedToken = m_completionToken.load(std::memory_order_acquire);
        if (completedToken == token
            && m_completionGeneration.load(std::memory_order_relaxed)
            == m_currentPreparationGeneration.load(std::memory_order_relaxed)
            && m_currentPreparationToken.load(std::memory_order_acquire) == token) {
            const uint32_t result = m_completionResult.load(std::memory_order_relaxed);
            if (result == COMPLETION_FAILURE) {
                invalidateTransportEpisode();
                return 1;
            }
            if (result == COMPLETION_SUCCESS) {
                m_releasedPreparationToken.store(token, std::memory_order_release);
                resetExpectedRenderFrame(m_currentPreparationFrame.load(std::memory_order_relaxed));
                m_renderEligibleToken.store(token, std::memory_order_release);
                return 1;
            }
        }

        return 0;
    }

    if (rolling) {
        const uint64_t token = m_currentPreparationToken.load(std::memory_order_acquire);
        const bool renderEligible = armed
                                    && token != 0
                                    && m_currentPreparationGeneration.load(std::memory_order_relaxed)
                                    == m_transportGeneration.load(std::memory_order_acquire)
                                    && m_releasedPreparationToken.load(std::memory_order_acquire) == token
                                    && m_renderEligibleToken.load(std::memory_order_acquire) == token;
        const bool changed = !m_callbackHasObservation
                             || (m_callbackLastState != JackTransportRolling
                                 && m_callbackLastState != JackTransportLooping);

        if (!renderEligible) {
            if (changed) {
                publishObservation(TransportObservationKind::RollingUnprepared, token, position.frame, true);
            }
            invalidateTransportEpisode();
        }

        m_callbackEpisodeTracker.observeNotStarting();
        m_callbackHasObservation = true;
        m_callbackLastState = state;
        m_callbackLastFrame = position.frame;
        return 1;
    }

    // Unknown/legacy transport states are not held by this client. Treat them
    // as an unprepared moving state once, then wait for Stopped to arm again.
    const bool changed = !m_callbackHasObservation || m_callbackLastState != state;
    if (changed) {
        publishObservation(TransportObservationKind::RollingUnprepared,
                           m_currentPreparationToken.load(std::memory_order_acquire), position.frame, true);
    }
    invalidateTransportEpisode();
    m_callbackEpisodeTracker.observeNotStarting();
    m_callbackHasObservation = true;
    m_callbackLastState = state;
    m_callbackLastFrame = position.frame;
    return 1;
}

void JackAudioDriver::observeTransport(jack_transport_state_t state, const jack_position_t& position,
                                       bool authoritative) noexcept
{
    const bool starting = isTransportStarting(state);
    const bool rolling = state == JackTransportRolling || state == JackTransportLooping;

    if (state == JackTransportStopped) {
        const bool changed = !m_callbackHasObservation
                             || m_callbackLastState != JackTransportStopped
                             || m_callbackLastFrame != position.frame;
        invalidateTransportEpisode();
        m_callbackEpisodeTracker.observeNotStarting();
        m_transportCallbackArmed.store(true, std::memory_order_release);
        if (changed) {
            publishObservation(authoritative ? TransportObservationKind::AuthoritativeLocate
                                             : TransportObservationKind::Stopped,
                               0, position.frame, authoritative);
        }
    } else if (starting) {
        // Preparation is owned exclusively by the sync callback. The process
        // callback records the state only, avoiding a second slot writer/path.
    } else if (rolling) {
        const bool armed = m_transportCallbackArmed.load(std::memory_order_acquire);
        const uint64_t token = m_currentPreparationToken.load(std::memory_order_acquire);
        const bool renderEligible = armed && shouldRenderTransportState(state);
        const bool changed = !m_callbackHasObservation
                             || (m_callbackLastState != JackTransportRolling
                                 && m_callbackLastState != JackTransportLooping);
        if (!renderEligible) {
            if (changed) {
                publishObservation(TransportObservationKind::RollingUnprepared, token, position.frame, authoritative);
            }
            invalidateTransportEpisode();
        }
        m_callbackEpisodeTracker.observeNotStarting();
    } else {
        const bool changed = !m_callbackHasObservation || m_callbackLastState != state;
        if (changed) {
            publishObservation(TransportObservationKind::RollingUnprepared,
                               m_currentPreparationToken.load(std::memory_order_acquire), position.frame, authoritative);
        }
        invalidateTransportEpisode();
        m_callbackEpisodeTracker.observeNotStarting();
    }

    m_callbackHasObservation = true;
    m_callbackLastState = state;
    m_callbackLastFrame = position.frame;
}

bool JackAudioDriver::shouldRenderTransportState(jack_transport_state_t state) const noexcept
{
    if (!m_transportRequested.load(std::memory_order_acquire)
        || !m_transportCallbackArmed.load(std::memory_order_acquire)
        || state == JackTransportStopped) {
        return true;
    }

    if (isTransportStarting(state)) {
        return false;
    }

    if (state == JackTransportRolling || state == JackTransportLooping) {
        const uint64_t token = m_currentPreparationToken.load(std::memory_order_acquire);
        return token != 0
               && m_currentPreparationGeneration.load(std::memory_order_relaxed)
               == m_transportGeneration.load(std::memory_order_acquire)
               && m_releasedPreparationToken.load(std::memory_order_acquire) == token
               && m_renderEligibleToken.load(std::memory_order_acquire) == token;
    }

    return false;
}

void JackAudioDriver::resetExpectedRenderFrame(jack_nframes_t frame) noexcept
{
    m_expectedRenderFrame.store(frame, std::memory_order_relaxed);
    m_hasExpectedRenderFrame.store(true, std::memory_order_release);
}

void JackAudioDriver::observeRenderedTransportBlock(jack_nframes_t frame, jack_nframes_t nframes) noexcept
{
    if (!m_hasExpectedRenderFrame.load(std::memory_order_acquire)) {
        return;
    }

    if (nframes > std::numeric_limits<jack_nframes_t>::max() - frame) {
        clearExpectedRenderFrame();
        return;
    }

    const jack_nframes_t expected = m_expectedRenderFrame.load(std::memory_order_relaxed);
    if (frame != expected) {
        m_transportFrameMismatchExpected.store(expected, std::memory_order_relaxed);
        m_transportFrameMismatchObserved.store(frame, std::memory_order_relaxed);
        m_runtimeStatusFlags.fetch_or(STATUS_TRANSPORT_FRAME_DISCONTINUITY, std::memory_order_release);
    }

    m_expectedRenderFrame.store(frame + nframes, std::memory_order_relaxed);
}

void JackAudioDriver::clearExpectedRenderFrame() noexcept
{
    m_hasExpectedRenderFrame.store(false, std::memory_order_release);
    m_expectedRenderFrame.store(0, std::memory_order_relaxed);
    m_transportFrameMismatchExpected.store(0, std::memory_order_relaxed);
    m_transportFrameMismatchObserved.store(0, std::memory_order_relaxed);
    m_runtimeStatusFlags.fetch_and(~STATUS_TRANSPORT_FRAME_DISCONTINUITY, std::memory_order_acq_rel);
}

int JackAudioDriver::process(jack_nframes_t nframes) noexcept
{
    auto* left = static_cast<jack_default_audio_sample_t*>(jack_port_get_buffer(m_leftOutputPort, nframes));
    auto* right = static_cast<jack_default_audio_sample_t*>(jack_port_get_buffer(m_rightOutputPort, nframes));
    if (left) {
        std::fill_n(left, nframes, 0.0f);
    }
    if (right) {
        std::fill_n(right, nframes, 0.0f);
    }

    if (!left || !right) {
        return 0;
    }

    if (nframes > m_bufferCapacityFrames) {
        m_reportedBufferSize.store(nframes, std::memory_order_release);
        m_bufferSizeTooLarge.store(true, std::memory_order_release);
        m_runtimeStatusFlags.fetch_or(STATUS_BUFFER_SIZE_CHANGED | STATUS_BUFFER_SIZE_TOO_LARGE,
                                      std::memory_order_relaxed);
        return 0;
    }

    if (m_closing.load(std::memory_order_acquire)
        || m_serverShutdown.load(std::memory_order_acquire)
        || m_sampleRateChanged.load(std::memory_order_acquire)
        || m_bufferSizeTooLarge.load(std::memory_order_acquire)) {
        return 0;
    }

    if (m_transportRequested.load(std::memory_order_acquire)) {
        synchronizeTransportEpoch();
        if (m_transportRequested.load(std::memory_order_acquire)) {
            jack_position_t position {};
            const jack_transport_state_t state = jack_transport_query(m_client, &position);
            observeTransport(state, position, false);

            if (!shouldRenderTransportState(state)) {
                return 0;
            }

            if (m_transportCallbackArmed.load(std::memory_order_acquire)
                && (state == JackTransportRolling || state == JackTransportLooping)) {
                observeRenderedTransportBlock(position.frame, nframes);
            }
        }
    }

    const size_t byteCount = static_cast<size_t>(nframes) * JACK_OUTPUT_CHANNELS * sizeof(float);
    m_activeSpec.callback(reinterpret_cast<uint8_t*>(m_interleavedBuffer.data()), static_cast<int>(byteCount));

    const float* source = m_interleavedBuffer.data();
    for (jack_nframes_t frame = 0; frame < nframes; ++frame) {
        left[frame] = source[frame * JACK_OUTPUT_CHANNELS];
        right[frame] = source[frame * JACK_OUTPUT_CHANNELS + 1];
    }

    return 0;
}

void JackAudioDriver::shutdownCallback(void* context) noexcept
{
    JackAudioDriver* driver = static_cast<JackAudioDriver*>(context);
    driver->m_serverShutdown.store(true, std::memory_order_release);
    driver->m_transportCallbackArmed.store(false, std::memory_order_release);
    driver->invalidateTransportEpisode();
    driver->m_runtimeStatusFlags.fetch_or(STATUS_SERVER_SHUTDOWN, std::memory_order_relaxed);
}

int JackAudioDriver::xrunCallback(void* context) noexcept
{
    JackAudioDriver* driver = static_cast<JackAudioDriver*>(context);
    driver->m_xrunCount.fetch_add(1, std::memory_order_relaxed);
    driver->m_runtimeStatusFlags.fetch_or(STATUS_XRUN, std::memory_order_relaxed);
    return 0;
}

int JackAudioDriver::bufferSizeCallback(jack_nframes_t nframes, void* context) noexcept
{
    JackAudioDriver* driver = static_cast<JackAudioDriver*>(context);
    driver->m_reportedBufferSize.store(nframes, std::memory_order_release);
    uint32_t flags = STATUS_BUFFER_SIZE_CHANGED;
    if (nframes > driver->m_bufferCapacityFrames) {
        driver->m_bufferSizeTooLarge.store(true, std::memory_order_release);
        flags |= STATUS_BUFFER_SIZE_TOO_LARGE;
    }
    driver->m_runtimeStatusFlags.fetch_or(flags, std::memory_order_relaxed);
    return 0;
}

int JackAudioDriver::sampleRateCallback(jack_nframes_t sampleRate, void* context) noexcept
{
    JackAudioDriver* driver = static_cast<JackAudioDriver*>(context);
    driver->m_reportedSampleRate.store(sampleRate, std::memory_order_release);
    const jack_nframes_t openedSampleRate = driver->m_openedSampleRate.load(std::memory_order_acquire);
    if (openedSampleRate != 0 && openedSampleRate != sampleRate) {
        driver->m_sampleRateChanged.store(true, std::memory_order_release);
        driver->m_runtimeStatusFlags.fetch_or(STATUS_SAMPLE_RATE_CHANGED, std::memory_order_relaxed);
    }
    return 0;
}

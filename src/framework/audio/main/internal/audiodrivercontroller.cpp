/*
 * SPDX-License-Identifier: GPL-3.0-only
 * MuseScore-CLA-applies
 *
 * MuseScore
 * Music Composition & Notation
 *
 * Copyright (C) 2025 MuseScore Limited and others
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

#include "audiodrivercontroller.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include "global/async/async.h"

#include "muse_framework_config.h"

#if defined(MUSE_MODULE_AUDIO_JACK) && defined(Q_OS_LINUX)
#include "audio/driver/platform/jack/jackaudiodriver.h"
#endif

#if defined(Q_OS_LINUX) || defined(Q_OS_FREEBSD)
#include <QtEnvironmentVariables>
#include "audio/driver/platform/lin/alsaaudiodriver.h"
#ifdef MUSE_MODULE_AUDIO_PIPEWIRE
#include "audio/driver/platform/lin/pwaudiodriver.h"
#endif
#endif

#ifdef Q_OS_WIN
#include "audio/driver/platform/win/wasapiaudiodriver.h"
#ifdef MUSE_MODULE_AUDIO_ASIO
#include "audio/driver/platform/win/asio/asioaudiodriver.h"
#endif
#endif

#ifdef Q_OS_MACOS
#include "audio/driver/platform/osx/osxaudiodriver.h"
#endif

#ifdef Q_OS_WASM
#include "audio/driver/platform/web/webaudiodriver.h"
#endif

#include "log.h"

using namespace muse;
using namespace muse::audio;
using namespace muse::audio::rpc;

void AudioDriverController::init()
{
    configuration()->useJackTransportChanged().onReceive(this, [this](bool) {
        if (!m_transportConfigurationGuard) {
            applyRequestedTransportState();
        }
    });
}

void AudioDriverController::poll()
{
    pollJackStatus();
    pollJackTransport();
}

JackAudioDriver* AudioDriverController::jackDriver() const
{
#if defined(MUSE_MODULE_AUDIO_JACK) && defined(Q_OS_LINUX)
    return dynamic_cast<JackAudioDriver*>(m_audioDriver.get());
#else
    return nullptr;
#endif
}

uint64_t AudioDriverController::nextDriverGeneration()
{
    uint64_t generation = m_nextDriverGeneration++;
    if (generation == 0) {
        generation = m_nextDriverGeneration++;
    }
    return generation;
}

bool AudioDriverController::isTransportSyncAvailable() const
{
#if defined(MUSE_MODULE_AUDIO_JACK) && defined(Q_OS_LINUX)
    JackAudioDriver* driver = jackDriver();
    return driver && driver->isOpened() && m_transportSyncState != AudioDriverTransportSyncState::Unavailable;
#else
    return false;
#endif
}

bool AudioDriverController::transportSyncRequested() const
{
    return configuration()->useJackTransport();
}

AudioDriverTransportSyncState AudioDriverController::transportSyncState() const
{
    return m_transportSyncState;
}

async::Notification AudioDriverController::transportSyncStateChanged() const
{
    return m_transportSyncStateChanged;
}

async::Channel<AudioDriverTransportEvent> AudioDriverController::transportEvent() const
{
    return m_transportEvent;
}

void AudioDriverController::setTransportSyncState(AudioDriverTransportSyncState state)
{
    if (m_transportSyncState == state) {
        return;
    }

    m_transportSyncState = state;
    m_transportSyncStateChanged.notify();
}

void AudioDriverController::sendTransportEvent(AudioDriverTransportEventType type, uint64_t token, secs_t position,
                                               const std::string& message)
{
    AudioDriverTransportEvent event;
    event.type = type;
    event.driverGeneration = m_driverGeneration;
    event.token = token;
    event.position = position;
    event.message = message;
    m_transportEvent.send(event);
}

uint64_t AudioDriverController::secondsToTransportFrame(secs_t position) const
{
    const sample_rate_t sampleRate = activeSpec().output.sampleRate;
#if defined(MUSE_MODULE_AUDIO_JACK) && defined(Q_OS_LINUX)
    constexpr uint64_t MAXIMUM_JACK_FRAME = std::numeric_limits<jack_nframes_t>::max();
#else
    constexpr uint64_t MAXIMUM_JACK_FRAME = std::numeric_limits<uint64_t>::max();
#endif

    if (sampleRate == 0 || std::isnan(position) || position <= 0.0) {
        return 0;
    }
    if (!std::isfinite(position)) {
        return MAXIMUM_JACK_FRAME;
    }

    const long double frames = std::round(static_cast<long double>(position) * sampleRate);
    const long double maximum = static_cast<long double>(MAXIMUM_JACK_FRAME);
    return frames >= maximum ? MAXIMUM_JACK_FRAME : static_cast<uint64_t>(frames);
}

secs_t AudioDriverController::transportFrameToSeconds(uint64_t frame) const
{
    const sample_rate_t sampleRate = activeSpec().output.sampleRate;
    return sampleRate > 0
           ? secs_t(static_cast<double>(frame) / static_cast<double>(sampleRate))
           : secs_t(0.0);
}

void AudioDriverController::configureTransportBeforeOpen(const IAudioDriverPtr& driver, uint64_t generation)
{
#if defined(MUSE_MODULE_AUDIO_JACK) && defined(Q_OS_LINUX)
    if (auto* jack = dynamic_cast<JackAudioDriver*>(driver.get())) {
        jack->configureTransport(generation, transportSyncRequested());
    }
#else
    UNUSED(driver);
    UNUSED(generation);
#endif
}

void AudioDriverController::resetTransportForDriverChange()
{
#if defined(MUSE_MODULE_AUDIO_JACK) && defined(Q_OS_LINUX)
    if (JackAudioDriver* driver = jackDriver()) {
        driver->cancelPendingTransportWork();
        // Invalidate the callback-domain identity before the old client can be
        // closed or destroyed. A later async completion can no longer release
        // transport work belonging to this driver.
        driver->configureTransport(0, false);
    }
#endif

    m_preparationToken = 0;
    m_pendingLocalTransportFrame.reset();
    m_pendingStopRequest = false;
    m_pendingPauseRequest = false;
    m_driverGeneration = 0;
    m_jackRuntimeFaulted = false;
    setTransportSyncState(AudioDriverTransportSyncState::Off);
}

void AudioDriverController::applyRequestedTransportState()
{
#if defined(MUSE_MODULE_AUDIO_JACK) && defined(Q_OS_LINUX)
    JackAudioDriver* driver = jackDriver();
    if (!driver) {
        m_preparationToken = 0;
        m_pendingLocalTransportFrame.reset();
        m_pendingStopRequest = false;
        m_pendingPauseRequest = false;
        setTransportSyncState(AudioDriverTransportSyncState::Off);
        return;
    }

    if (!driver->isOpened() || m_jackRuntimeFaulted) {
        m_preparationToken = 0;
        m_pendingLocalTransportFrame.reset();
        m_pendingStopRequest = false;
        m_pendingPauseRequest = false;
        setTransportSyncState(AudioDriverTransportSyncState::Unavailable);
        return;
    }

    const bool requested = transportSyncRequested();
    if (!requested) {
        if (driver->transportSyncEnabled()) {
            driver->setTransportSyncEnabled(false);
        } else {
            driver->cancelPendingTransportWork();
        }

        m_preparationToken = 0;
        m_pendingLocalTransportFrame.reset();
        m_pendingStopRequest = false;
        m_pendingPauseRequest = false;
        setTransportSyncState(AudioDriverTransportSyncState::Off);
        return;
    }

    const bool needsFreshEpoch = driver->transportGeneration() != m_driverGeneration
                                 || !driver->transportSyncEnabled();
    if (driver->transportGeneration() != m_driverGeneration) {
        driver->configureTransport(m_driverGeneration, true);
    } else if (!driver->transportSyncEnabled()) {
        driver->setTransportSyncEnabled(true);
    }

    if (needsFreshEpoch
        || m_transportSyncState == AudioDriverTransportSyncState::Off
        || m_transportSyncState == AudioDriverTransportSyncState::Unavailable) {
        m_jackRuntimeFaulted = false;
        m_preparationToken = 0;
        m_pendingLocalTransportFrame.reset();
        m_pendingStopRequest = false;
        m_pendingPauseRequest = false;
        setTransportSyncState(AudioDriverTransportSyncState::Pending);
    }
#else
    m_preparationToken = 0;
    m_pendingLocalTransportFrame.reset();
    m_pendingStopRequest = false;
    m_pendingPauseRequest = false;
    setTransportSyncState(AudioDriverTransportSyncState::Off);
#endif
}

void AudioDriverController::setTransportSyncEnabled(bool enabled)
{
    if (configuration()->useJackTransport() != enabled) {
        m_transportConfigurationGuard = true;
        configuration()->setUseJackTransport(enabled);
        m_transportConfigurationGuard = false;
    }

    applyRequestedTransportState();
}

bool AudioDriverController::requestTransportPlay(secs_t position)
{
#if defined(MUSE_MODULE_AUDIO_JACK) && defined(Q_OS_LINUX)
    JackAudioDriver* driver = jackDriver();
    if (!driver || m_transportSyncState != AudioDriverTransportSyncState::Effective) {
        return false;
    }

    const uint64_t frame = secondsToTransportFrame(position);
    if (driver->requestPlay(static_cast<jack_nframes_t>(frame))) {
        m_preparationToken = 0;
        m_pendingLocalTransportFrame = frame;
        m_pendingStopRequest = false;
        m_pendingPauseRequest = false;
    } else {
        m_pendingLocalTransportFrame.reset();
        m_pendingStopRequest = false;
        m_pendingPauseRequest = false;
        sendTransportEvent(AudioDriverTransportEventType::Error, 0, position,
                           "JACK rejected the requested Play position");
    }

    // Effective synchronization owns the command even when JACK rejects it;
    // falling back locally would start an independent timeline.
    return true;
#else
    UNUSED(position);
    return false;
#endif
}

bool AudioDriverController::requestTransportPause()
{
#if defined(MUSE_MODULE_AUDIO_JACK) && defined(Q_OS_LINUX)
    JackAudioDriver* driver = jackDriver();
    if (!driver || m_transportSyncState != AudioDriverTransportSyncState::Effective) {
        return false;
    }

    if (driver->requestPause()) {
        m_preparationToken = 0;
        m_pendingLocalTransportFrame.reset();
        m_pendingStopRequest = false;
        m_pendingPauseRequest = true;
    } else {
        m_pendingLocalTransportFrame.reset();
        m_pendingStopRequest = false;
        m_pendingPauseRequest = false;
        sendTransportEvent(AudioDriverTransportEventType::Error, 0, 0.0,
                           "JACK rejected the requested Pause");
    }

    return true;
#else
    return false;
#endif
}

bool AudioDriverController::requestTransportStop()
{
#if defined(MUSE_MODULE_AUDIO_JACK) && defined(Q_OS_LINUX)
    JackAudioDriver* driver = jackDriver();
    if (!driver || m_transportSyncState != AudioDriverTransportSyncState::Effective) {
        return false;
    }

    constexpr uint64_t STOP_FRAME = 0;
    if (driver->requestStop(static_cast<jack_nframes_t>(STOP_FRAME))) {
        m_preparationToken = 0;
        m_pendingLocalTransportFrame = STOP_FRAME;
        m_pendingStopRequest = true;
        m_pendingPauseRequest = false;
    } else {
        // requestStop() may already have stopped JACK before a subsequent
        // locate failure, so no old preparation can safely remain current.
        m_preparationToken = 0;
        m_pendingLocalTransportFrame.reset();
        m_pendingStopRequest = false;
        m_pendingPauseRequest = false;
        sendTransportEvent(AudioDriverTransportEventType::Error, 0, 0.0,
                           "JACK rejected the requested Stop position");
    }

    return true;
#else
    return false;
#endif
}

bool AudioDriverController::requestTransportSeek(secs_t position)
{
#if defined(MUSE_MODULE_AUDIO_JACK) && defined(Q_OS_LINUX)
    JackAudioDriver* driver = jackDriver();
    if (!driver || m_transportSyncState != AudioDriverTransportSyncState::Effective) {
        return false;
    }

    const uint64_t frame = secondsToTransportFrame(position);
    if (driver->requestSeek(static_cast<jack_nframes_t>(frame))) {
        m_preparationToken = 0;
        m_pendingLocalTransportFrame = frame;
        m_pendingStopRequest = false;
        m_pendingPauseRequest = false;
    } else {
        m_pendingLocalTransportFrame.reset();
        m_pendingStopRequest = false;
        m_pendingPauseRequest = false;
        sendTransportEvent(AudioDriverTransportEventType::Error, 0, position,
                           "JACK rejected the requested Seek position");
    }

    return true;
#else
    UNUSED(position);
    return false;
#endif
}

bool AudioDriverController::isTransportPreparationCurrent(uint64_t driverGeneration, uint64_t token) const
{
#if defined(MUSE_MODULE_AUDIO_JACK) && defined(Q_OS_LINUX)
    JackAudioDriver* driver = jackDriver();
    return driver
           && m_transportSyncState != AudioDriverTransportSyncState::Off
           && m_transportSyncState != AudioDriverTransportSyncState::Unavailable
           && driverGeneration != 0
           && token != 0
           && driverGeneration == m_driverGeneration
           && token == m_preparationToken
           && driver->transportGeneration() == driverGeneration
           && driver->isTransportPreparationCurrent(driverGeneration, token);
#else
    UNUSED(driverGeneration);
    UNUSED(token);
    return false;
#endif
}

bool AudioDriverController::activateTransportForPreparation(uint64_t driverGeneration, uint64_t token)
{
#if defined(MUSE_MODULE_AUDIO_JACK) && defined(Q_OS_LINUX)
    JackAudioDriver* driver = jackDriver();
    if (!driver || !isTransportPreparationCurrent(driverGeneration, token)) {
        return false;
    }

    if (!driver->activateTransportForPreparation(driverGeneration, token)) {
        sendTransportEvent(AudioDriverTransportEventType::Error, token, 0.0,
                           "JACK transport preparation expired before playback could start");
        return false;
    }

    // Pending becomes authoritative only after local pause/seek/preparation
    // completed. Publish Effective before PlaybackController starts the player.
    if (m_transportSyncState == AudioDriverTransportSyncState::Pending) {
        setTransportSyncState(AudioDriverTransportSyncState::Effective);
    }
    return true;
#else
    UNUSED(driverGeneration);
    UNUSED(token);
    return false;
#endif
}

bool AudioDriverController::completeTransportPreparation(uint64_t driverGeneration, uint64_t token, bool success)
{
#if defined(MUSE_MODULE_AUDIO_JACK) && defined(Q_OS_LINUX)
    JackAudioDriver* driver = jackDriver();
    if (!driver || !isTransportPreparationCurrent(driverGeneration, token)) {
        return false;
    }

    driver->completeTransportPreparation(driverGeneration, token, success);
    if (!driver->isTransportPreparationCurrent(driverGeneration, token)) {
        m_preparationToken = 0;
    }
    return true;
#else
    UNUSED(driverGeneration);
    UNUSED(token);
    UNUSED(success);
    return false;
#endif
}

void AudioDriverController::cancelPendingTransportWork()
{
#if defined(MUSE_MODULE_AUDIO_JACK) && defined(Q_OS_LINUX)
    if (JackAudioDriver* driver = jackDriver()) {
        driver->cancelPendingTransportWork();
    }
#endif

    m_preparationToken = 0;
    m_pendingLocalTransportFrame.reset();
    m_pendingStopRequest = false;
    m_pendingPauseRequest = false;
}

void AudioDriverController::pollJackTransport()
{
#if defined(MUSE_MODULE_AUDIO_JACK) && defined(Q_OS_LINUX)
    JackAudioDriver* driver = jackDriver();
    if (!driver
        || m_transportSyncState == AudioDriverTransportSyncState::Off
        || m_transportSyncState == AudioDriverTransportSyncState::Unavailable
        || driver->transportGeneration() != m_driverGeneration
        || !driver->transportSyncEnabled()) {
        return;
    }

    JackAudioDriver::TransportPreparation preparation;
    if (driver->takePendingTransportPreparation(preparation)
        && preparation.generation == m_driverGeneration
        && preparation.token != 0) {
        m_preparationToken = preparation.token;

        // A Starting preparation is authoritative and supersedes a local
        // locate still waiting for an ordinary stopped-query acknowledgement.
        m_pendingLocalTransportFrame.reset();
        m_pendingStopRequest = false;
        m_pendingPauseRequest = false;

        sendTransportEvent(AudioDriverTransportEventType::Prepare, preparation.token,
                           transportFrameToSeconds(preparation.frame));
    }

    JackAudioDriver::TransportObservation observation;
    if (!driver->takeTransportObservation(observation)
        || observation.generation != m_driverGeneration) {
        return;
    }

    if (m_preparationToken != 0) {
        if (driver->isTransportPreparationCurrent(m_driverGeneration, m_preparationToken)) {
            // The observation slot can still contain the stopped/new-position
            // publication which preceded this priority Prepare. With one
            // callback-domain writer, any genuinely newer stop/locate first
            // invalidates this token in the driver, so it is safe to discard.
            return;
        }
        m_preparationToken = 0;
    }

    const uint64_t frame = observation.frame;
    const secs_t position = transportFrameToSeconds(frame);

    if (observation.kind == JackAudioDriver::TransportObservationKind::RollingUnprepared) {
        if (m_transportSyncState == AudioDriverTransportSyncState::Pending
            && !driver->transportCallbackArmed()) {
            // Enabling synchronization while JACK is already Rolling must not
            // disturb ordinary local playback. Stay Pending until a fresh
            // Stopped observation arms callbacks (or a later armed Starting
            // episode enters preparation).
            return;
        }

        if (observation.token == 0
            && (m_pendingLocalTransportFrame.has_value() || m_pendingPauseRequest)) {
            // An accepted local command invalidates the preceding Rolling
            // episode before JACK applies its stop/locate. A process query in
            // that short window reports tokenless RollingUnprepared; it is not
            // a failed new episode and must not erase Stop or Pause-and-select
            // semantics while the authoritative acknowledgment is in flight.
            return;
        }

        m_pendingLocalTransportFrame.reset();
        m_pendingStopRequest = false;
        m_pendingPauseRequest = false;
        sendTransportEvent(AudioDriverTransportEventType::RollingUnprepared, observation.token, position,
                           "JACK is rolling without a prepared MuseScore transport episode");
        return;
    }

    const bool authoritativeLocate
        = observation.kind == JackAudioDriver::TransportObservationKind::AuthoritativeLocate
          || observation.authoritativeNewPosition;

    if (m_pendingLocalTransportFrame.has_value()) {
        const bool reachedLocalTarget = frame == m_pendingLocalTransportFrame.value();
        if (!authoritativeLocate && !reachedLocalTarget) {
            // This can be an ordinary stopped observation queued before the
            // accepted local locate. Wait for its canonical JACK-frame target.
            return;
        }

        const bool applyExplicitStop = m_pendingStopRequest && reachedLocalTarget;
        m_pendingLocalTransportFrame.reset();
        m_pendingStopRequest = false;
        m_pendingPauseRequest = false;

        sendTransportEvent(applyExplicitStop ? AudioDriverTransportEventType::Stop
                                             : AudioDriverTransportEventType::Locate,
                           observation.token, position);
        return;
    }

    m_pendingPauseRequest = false;
    if (authoritativeLocate) {
        sendTransportEvent(AudioDriverTransportEventType::Locate, observation.token, position);
    } else {
        sendTransportEvent(AudioDriverTransportEventType::Stopped, observation.token, position);
    }

    if (m_transportSyncState == AudioDriverTransportSyncState::Pending) {
        // Sending the event is synchronous on the main thread, so the player's
        // pause then seek requests are enqueued before Effective is published.
        setTransportSyncState(AudioDriverTransportSyncState::Effective);
    }
#endif
}

void AudioDriverController::pollJackStatus()
{
#if defined(MUSE_MODULE_AUDIO_JACK) && defined(Q_OS_LINUX)
    JackAudioDriver* driver = jackDriver();
    if (!driver) {
        return;
    }

    const JackAudioDriver::RuntimeStatus status = driver->takeRuntimeStatus();

    if (status.xruns > 0) {
        LOGW() << "JACK reported " << status.xruns << " xrun(s); audio continues on the next period";
    }

    if (status.transportFrameDiscontinuity) {
        LOGW() << "JACK transport frame discontinuity: expected " << status.expectedTransportFrame
               << ", observed " << status.observedTransportFrame
               << "; stop and restart transport if playback is audibly out of sync";
    }

    std::string fault;
    if (status.serverShutdown) {
        fault = "The JACK server shut down; restart the server and reselect JACK";
    } else if (status.sampleRateChanged) {
        fault = "The JACK sample rate changed; restart MuseScore to use the new rate";
    } else if (status.bufferSizeTooLarge) {
        fault = "The JACK period exceeds MuseScore's callback capacity; restart MuseScore after choosing a smaller period";
    }

    if (!fault.empty()) {
        driver->setTransportSyncEnabled(false);
        driver->cancelPendingTransportWork();
        m_preparationToken = 0;
        m_pendingLocalTransportFrame.reset();
        m_pendingStopRequest = false;
        m_pendingPauseRequest = false;

        const bool newlyFaulted = !m_jackRuntimeFaulted;
        m_jackRuntimeFaulted = true;
        setTransportSyncState(AudioDriverTransportSyncState::Unavailable);
        if (newlyFaulted) {
            LOGE() << fault;
            sendTransportEvent(AudioDriverTransportEventType::Unavailable, 0, 0.0, fault);
        }
        return;
    }

    if (status.bufferSizeChanged) {
        LOGI() << "JACK period changed to " << status.bufferSize;
        // takeRuntimeStatus() applies an in-capacity period to the driver's
        // active spec and emits activeSpecChanged(), which follows the usual
        // controller -> engine/UI publication path installed in setNewDriver().
    }
#endif
}

std::string AudioDriverController::canonicalAudioApi(const std::string& name) const
{
#if defined(Q_OS_LINUX) || defined(Q_OS_FREEBSD)
    if (qEnvironmentVariableIsSet("MUSESCORE_FORCE_ALSA")) {
        return "ALSA";
    }
#endif

    return name;
}

IAudioDriverPtr AudioDriverController::createDriver(const std::string& name) const
{
#ifdef Q_OS_WIN
    if (name == "ASIO") {
#ifdef MUSE_MODULE_AUDIO_ASIO
        return std::shared_ptr<IAudioDriver>(new AsioAudioDriver());
#else
        LOGW() << "ASIO is required but is not available, WASAPI will be used";
#endif
    }

    // required WASAPI or fallback
    return std::shared_ptr<IAudioDriver>(new WasapiAudioDriver());
#endif

#if defined(Q_OS_LINUX) || defined(Q_OS_FREEBSD)
    if (qEnvironmentVariableIsSet("MUSESCORE_FORCE_ALSA")) {
        if (name != "ALSA") {
            LOGW() << "required: " << name << ", but is set MUSESCORE_FORCE_ALSA";
        }
        return std::make_shared<AlsaAudioDriver>();
    }

    if (name == "ALSA") {
        return std::make_shared<AlsaAudioDriver>();
    }

    if (name == "PipeWire") {
#ifdef MUSE_MODULE_AUDIO_PIPEWIRE
        auto driver = std::make_shared<PwAudioDriver>();
        if (driver->connectedToPwServer()) {
            return driver;
        } else {
            LOGE() << "PipeWire driver failed to connect to server";
        }
#else
        LOGW() << "PipeWire is required but is not available";
#endif

        return nullptr;
    }

#if defined(MUSE_MODULE_AUDIO_JACK) && defined(Q_OS_LINUX)
    if (name == "JACK") {
        return std::make_shared<JackAudioDriver>();
    }
#endif

    LOGE() << "Unknown or unavailable audio driver: " << name;
    return nullptr;
#endif

#ifdef Q_OS_MACOS
    UNUSED(name);
    return std::shared_ptr<IAudioDriver>(new OSXAudioDriver());
#endif

#ifdef Q_OS_WASM
    UNUSED(name);
    return std::shared_ptr<IAudioDriver>(new WebAudioDriver());
#endif

    return nullptr;
}

std::vector<std::string> AudioDriverController::availableAudioApiList() const
{
    std::vector<std::string> names;
#ifdef Q_OS_WIN
    names.push_back("WASAPI");
#ifdef MUSE_MODULE_AUDIO_ASIO
    names.push_back("ASIO");
#endif

    return names;
#endif // Q_OS_WIN

#if defined(Q_OS_LINUX) || defined(Q_OS_FREEBSD)
    names.push_back("ALSA");
    if (qEnvironmentVariableIsSet("MUSESCORE_FORCE_ALSA")) {
        return names;
    }

#ifdef MUSE_MODULE_AUDIO_PIPEWIRE
    names.push_back("PipeWire");
#endif

#if defined(MUSE_MODULE_AUDIO_JACK) && defined(Q_OS_LINUX)
    names.push_back("JACK");
#endif

    return names;
#endif // Q_OS_LINUX

#ifdef Q_OS_MACOS
    names.push_back("CoreAudio");
    return names;
#endif

#ifdef Q_OS_WASM
    names.push_back("WASM");
    return names;
#endif


    return names;
}

bool AudioDriverController::tryOpenDriver(const std::string& name, const IAudioDriver::Spec& spec, bool tryDefaultDevice,
                                          OpenedDriver& result)
{
    IAudioDriverPtr driver = createDriver(canonicalAudioApi(name));
    if (!driver) {
        return false;
    }

    driver->init();

    const uint64_t generation = driver->name() == "JACK" ? nextDriverGeneration() : 0;
    configureTransportBeforeOpen(driver, generation);

    IAudioDriver::Spec requestedSpec = spec;
    IAudioDriver::Spec activeSpec;

    LOGI() << "Trying to open audio driver: " << driver->name() << ", device: " << requestedSpec.deviceId;
    bool opened = driver->open(requestedSpec, &activeSpec);

    if (!opened) {
        driver->close();

        const AudioDeviceID defaultDevice = driver->defaultDevice();
        if (tryDefaultDevice && !defaultDevice.empty() && defaultDevice != requestedSpec.deviceId) {
            requestedSpec.deviceId = defaultDevice;
            activeSpec = {};
            configureTransportBeforeOpen(driver, generation);
            LOGW() << "Failed to open requested device, retrying the default: " << defaultDevice;
            opened = driver->open(requestedSpec, &activeSpec);
        }
    }

    if (!opened) {
        driver->close();
        return false;
    }

    if (!activeSpec.isValid()) {
        activeSpec = driver->activeSpec();
    }

    if (!activeSpec.isValid()) {
        LOGE() << "Audio driver returned an invalid active specification: " << driver->name();
        driver->close();
        return false;
    }

    result.driver = std::move(driver);
    result.spec = std::move(activeSpec);
    result.generation = generation;
    return true;
}

void AudioDriverController::installOpenedDriver(const OpenedDriver& openedDriver, const std::string& previousApi)
{
    const std::string oldApi = previousApi.empty() ? currentAudioApi() : previousApi;

    resetTransportForDriverChange();
    setNewDriver(openedDriver.driver);
    m_driverGeneration = openedDriver.generation;
    publishActiveSpec(openedDriver.spec);
    applyRequestedTransportState();

    if (oldApi != currentAudioApi()) {
        m_currentAudioApiChanged.notify();
    }

    m_availableOutputDevicesChanged.notify();
}

void AudioDriverController::setNewDriver(IAudioDriverPtr newDriver)
{
    if (m_audioDriver) {
        // unsubscribe
        m_audioDriver->availableOutputDevicesChanged().disconnect(this);
        m_audioDriver->activeSpecChanged().disconnect(this);
    }

    m_audioDriver = newDriver;

    if (m_audioDriver) {
        IAudioDriver* subscribedDriver = m_audioDriver.get();

        // subscribe
        m_audioDriver->availableOutputDevicesChanged().onNotify(this, [this, subscribedDriver]() {
            async::Async::call(this, [this, subscribedDriver]() {
                if (!m_audioDriver || m_audioDriver.get() != subscribedDriver) {
                    return;
                }

                LOGI() << "Available output devices changed, checking connection...";
                handleOutputDeviceChange();
                m_availableOutputDevicesChanged.notify();
            });
        });

        m_audioDriver->activeSpecChanged().onReceive(this, [this, subscribedDriver](const IAudioDriver::Spec& spec) {
            if (!m_audioDriver || m_audioDriver.get() != subscribedDriver) {
                return;
            }

            m_activeSpecChanged.send(spec);

            async::Async::call(this, [this, subscribedDriver, spec]() {
                if (!m_audioDriver || m_audioDriver.get() != subscribedDriver) {
                    return;
                }

                configuration()->setAudioOutputDeviceId(spec.deviceId);

                m_outputDeviceChanged.notify();
                m_outputDeviceBufferSizeChanged.notify();
                m_outputDeviceSampleRateChanged.notify();

                updateOutputSpec(spec.output);
            });
        });
    }
}

void AudioDriverController::publishActiveSpec(const IAudioDriver::Spec& spec)
{
    m_activeSpecChanged.send(spec);

    configuration()->setAudioOutputDeviceId(spec.deviceId);
    m_outputDeviceChanged.notify();
    m_outputDeviceBufferSizeChanged.notify();
    m_outputDeviceSampleRateChanged.notify();

    updateOutputSpec(spec.output);
}

std::string AudioDriverController::currentAudioApi() const
{
    if (m_audioDriver) {
        return m_audioDriver->name();
    }

    return canonicalAudioApi(configuration()->currentAudioApi());
}

bool AudioDriverController::changeCurrentAudioApi(const std::string& name)
{
    IF_ASSERT_FAILED(m_audioDriver) {
        return false;
    }

    const std::string requestedApi = canonicalAudioApi(name);
    const std::string previousApi = m_audioDriver->name();
    IAudioDriver::Spec previousSpec = m_audioDriver->activeSpec();

    if (requestedApi == previousApi && m_audioDriver->isOpened()) {
        return true;
    }

    if (!previousSpec.isValid()) {
        previousSpec.deviceId = DEFAULT_DEVICE_ID;
        previousSpec.output = configuration()->desiredOutputSpec();
        previousSpec.callback = m_callback;
    }

    resetTransportForDriverChange();
    m_audioDriver->close();
    setNewDriver(nullptr);

    IAudioDriver::Spec requestedSpec;
    const bool restoringPreferredApi = canonicalAudioApi(configuration()->currentAudioApi()) == requestedApi;
    requestedSpec.deviceId = restoringPreferredApi ? configuration()->audioOutputDeviceId() : DEFAULT_DEVICE_ID;
    if (requestedSpec.deviceId.empty()) {
        requestedSpec.deviceId = DEFAULT_DEVICE_ID;
    }
    requestedSpec.output = configuration()->desiredOutputSpec();
    requestedSpec.callback = m_callback;

    OpenedDriver requestedDriver;
    if (tryOpenDriver(requestedApi, requestedSpec, restoringPreferredApi, requestedDriver)) {
        installOpenedDriver(requestedDriver, previousApi);
        LOGI() << "Used audio driver: " << currentAudioApi();
        return true;
    }

    LOGE() << "Failed to open requested audio driver: " << requestedApi;

    OpenedDriver restoredDriver;
    if (tryOpenDriver(previousApi, previousSpec, false, restoredDriver)) {
        installOpenedDriver(restoredDriver, previousApi);
        configuration()->setCurrentAudioApi(currentAudioApi());
        LOGI() << "Restored audio driver: " << currentAudioApi();
        return false;
    }

    LOGE() << "Failed to restore previous audio driver: " << previousApi;

    const std::string defaultApi = canonicalAudioApi(configuration()->defaultAudioApi());
    if (defaultApi != previousApi) {
        IAudioDriver::Spec fallbackSpec;
        fallbackSpec.deviceId = DEFAULT_DEVICE_ID;
        fallbackSpec.output = configuration()->defaultOutputSpec();
        fallbackSpec.callback = m_callback;

        OpenedDriver fallbackDriver;
        if (tryOpenDriver(defaultApi, fallbackSpec, false, fallbackDriver)) {
            installOpenedDriver(fallbackDriver, previousApi);
            configuration()->setCurrentAudioApi(currentAudioApi());
            LOGW() << "Fell back to audio driver: " << currentAudioApi();
            return false;
        }
    }

    LOGE() << "Failed to open a replacement audio driver";
    return false;
}

async::Notification AudioDriverController::currentAudioApiChanged() const
{
    return m_currentAudioApiChanged;
}

AudioDeviceList AudioDriverController::availableOutputDevices() const
{
    IF_ASSERT_FAILED(m_audioDriver) {
        return {};
    }
    return m_audioDriver->availableOutputDevices();
}

async::Notification AudioDriverController::availableOutputDevicesChanged() const
{
    return m_availableOutputDevicesChanged;
}

bool AudioDriverController::open(const IAudioDriver::Spec& spec, IAudioDriver::Spec* activeSpec)
{
    m_callback = spec.callback;

    const std::string preferredApi = canonicalAudioApi(configuration()->currentAudioApi());
    OpenedDriver openedDriver;
    bool opened = tryOpenDriver(preferredApi, spec, true, openedDriver);

    if (!opened) {
        const std::string defaultApi = canonicalAudioApi(configuration()->defaultAudioApi());
        if (defaultApi != preferredApi) {
            IAudioDriver::Spec fallbackSpec = spec;
            fallbackSpec.deviceId = DEFAULT_DEVICE_ID;
            fallbackSpec.output = configuration()->defaultOutputSpec();
            opened = tryOpenDriver(defaultApi, fallbackSpec, false, openedDriver);
        }
    }

    if (!opened) {
        LOGE() << "Failed to open any audio driver";
        return false;
    }

    installOpenedDriver(openedDriver, preferredApi);
    configuration()->setCurrentAudioApi(currentAudioApi());

    if (activeSpec) {
        *activeSpec = openedDriver.spec;
    }

    LOGI() << "Opened audio driver: " << currentAudioApi()
           << ", device: " << openedDriver.spec.deviceId;

    return true;
}

void AudioDriverController::close()
{
    if (m_audioDriver) {
        resetTransportForDriverChange();
        m_audioDriver->close();
    }
}

bool AudioDriverController::isOpened() const
{
    IF_ASSERT_FAILED(m_audioDriver) {
        return false;
    }
    return m_audioDriver->isOpened();
}

const IAudioDriver::Spec& AudioDriverController::activeSpec() const
{
    IF_ASSERT_FAILED(m_audioDriver) {
        static IAudioDriver::Spec dummy;
        return dummy;
    }
    return m_audioDriver->activeSpec();
}

async::Channel<IAudioDriver::Spec> AudioDriverController::activeSpecChanged() const
{
    return m_activeSpecChanged;
}

AudioDeviceID AudioDriverController::outputDevice() const
{
    IF_ASSERT_FAILED(m_audioDriver) {
        return AudioDeviceID();
    }
    return m_audioDriver->activeSpec().deviceId;
}

bool AudioDriverController::selectOutputDevice(const AudioDeviceID& deviceId)
{
    IF_ASSERT_FAILED(m_audioDriver) {
        return false;
    }

    if (m_audioDriver->name() == "JACK" && deviceId != DEFAULT_DEVICE_ID) {
        LOGW() << "JACK exposes only the default server device";
        return false;
    }

    if (!m_audioDriver->isOpened()) {
        configuration()->setAudioOutputDeviceId(deviceId);
        return true;
    }

    const IAudioDriver::Spec oldSpec = m_audioDriver->activeSpec();
    if (deviceId == oldSpec.deviceId) {
        // In particular, JACK exposes only this one pseudo-device. Reopening
        // the same live client here would discard its fresh transport epoch
        // without passing through the controller's generation/state install.
        return true;
    }

    LOGI() << "Trying to change output device"
           << " from: " << oldSpec.deviceId
           << ", to: " << deviceId;

    IAudioDriver::Spec spec;
    spec.deviceId = deviceId;
    spec.callback = oldSpec.callback;
    spec.output = configuration()->desiredOutputSpec();

    m_audioDriver->close();
    bool ok = m_audioDriver->open(spec, nullptr);
    if (!ok) {
        m_audioDriver->close();
        LOGE() << "Failed to select device: " << deviceId << ", returning to: " << oldSpec.deviceId;
        bool restored = m_audioDriver->open(oldSpec, nullptr);
        if (!restored) {
            LOGE() << "Failed to restore previous device: " << oldSpec.deviceId;
        }
    }
    return ok;
}

async::Notification AudioDriverController::outputDeviceChanged() const
{
    return m_outputDeviceChanged;
}

void AudioDriverController::handleOutputDeviceChange()
{
    if (!m_audioDriver->isOpened() && !m_retryOpenDevice) {
        return;
    }

    IAudioDriver::Spec spec = m_audioDriver->activeSpec();
    LOGI() << "Checking output device: " << spec.deviceId;
    m_audioDriver->close();
    bool ok = m_audioDriver->open(spec, nullptr);
    if (!ok) {
        m_audioDriver->close();
        // reset to default device
        LOGW() << "Failed to reopen device: " << spec.deviceId << ", falling back to default";
        spec.deviceId = DEFAULT_DEVICE_ID;
        ok = m_audioDriver->open(spec, nullptr);
        if (!ok) {
            m_audioDriver->close();
            LOGE() << "Failed to reopen default device on " << m_audioDriver->name() << ", switching to default audio driver";
            switchToDefaultAudioDriver();
        }
    }

    m_retryOpenDevice = !ok;
}

bool AudioDriverController::switchToDefaultAudioDriver(IAudioDriver::Spec* activeSpec)
{
    const std::string defaultAudioApi = canonicalAudioApi(configuration()->defaultAudioApi());
    const std::string installedAudioApi = currentAudioApi();

    if (defaultAudioApi == installedAudioApi) {
        LOGE() << "Already on the default audio driver: " << defaultAudioApi << ", cannot fall back further";
        return false;
    }

    LOGW() << "Switching from " << installedAudioApi << " to default audio driver: " << defaultAudioApi;

    if (m_audioDriver) {
        resetTransportForDriverChange();
        m_audioDriver->close();
        setNewDriver(nullptr);
    }

    IAudioDriver::Spec defSpec;
    defSpec.deviceId = DEFAULT_DEVICE_ID;
    defSpec.output = configuration()->defaultOutputSpec();
    defSpec.callback = m_callback;

    OpenedDriver openedDriver;
    if (!tryOpenDriver(defaultAudioApi, defSpec, false, openedDriver)) {
        LOGE() << "Failed to open default audio driver: " << defaultAudioApi;
        return false;
    }

    installOpenedDriver(openedDriver, installedAudioApi);
    configuration()->setCurrentAudioApi(currentAudioApi());

    if (activeSpec) {
        *activeSpec = openedDriver.spec;
    }

    LOGI() << "Successfully switched to default audio driver: " << currentAudioApi();
    return true;
}

void AudioDriverController::updateOutputSpec(const OutputSpec& spec)
{
    if (!spec.isValid()) {
        return;
    }

    rpcChannel()->send(rpc::make_request(Method::SetOutputSpec, RpcPacker::pack(spec)));
}

std::vector<samples_t> AudioDriverController::availableOutputDeviceBufferSizes() const
{
    IF_ASSERT_FAILED(m_audioDriver) {
        return {};
    }
    return m_audioDriver->availableOutputDeviceBufferSizes();
}

void AudioDriverController::changeBufferSize(samples_t samples)
{
    IF_ASSERT_FAILED(m_audioDriver) {
        return;
    }

    if (currentAudioApi() == "JACK") {
        LOGW() << "JACK buffer size is controlled by the JACK server";
        return;
    }

    LOGI() << "Trying to change buffer size: " << samples;
    bool ok = true;

    if (m_audioDriver->isOpened()) {
        IAudioDriver::Spec spec = m_audioDriver->activeSpec();
        m_audioDriver->close();
        spec.output.samplesPerChannel = samples;
        ok = m_audioDriver->open(spec, &spec);
    }

    if (ok) {
        if (m_audioDriver->isOpened()) {
            updateOutputSpec(m_audioDriver->activeSpec().output);
        }
        configuration()->setDriverBufferSize(samples);
        m_outputDeviceBufferSizeChanged.notify();
    } else {
        LOGE() << "Failed to change buffer size to: " << samples;
    }
}

async::Notification AudioDriverController::outputDeviceBufferSizeChanged() const
{
    return m_outputDeviceBufferSizeChanged;
}

std::vector<sample_rate_t> AudioDriverController::availableOutputDeviceSampleRates() const
{
    IF_ASSERT_FAILED(m_audioDriver) {
        return {};
    }
    return m_audioDriver->availableOutputDeviceSampleRates();
}

void AudioDriverController::changeSampleRate(sample_rate_t sampleRate)
{
    IF_ASSERT_FAILED(m_audioDriver) {
        return;
    }

    if (currentAudioApi() == "JACK") {
        LOGW() << "JACK sample rate is controlled by the JACK server";
        return;
    }

    LOGI() << "Trying to change sampleRate: " << sampleRate;
    bool ok = true;

    if (m_audioDriver->isOpened()) {
        IAudioDriver::Spec spec = m_audioDriver->activeSpec();
        m_audioDriver->close();
        spec.output.sampleRate = sampleRate;
        ok = m_audioDriver->open(spec, &spec);
    }

    if (ok) {
        if (m_audioDriver->isOpened()) {
            updateOutputSpec(m_audioDriver->activeSpec().output);
        }
        configuration()->setSampleRate(sampleRate);
        m_outputDeviceSampleRateChanged.notify();
    } else {
        LOGE() << "Failed to change sample rate to: " << sampleRate;
    }
}

async::Notification AudioDriverController::outputDeviceSampleRateChanged() const
{
    return m_outputDeviceSampleRateChanged;
}

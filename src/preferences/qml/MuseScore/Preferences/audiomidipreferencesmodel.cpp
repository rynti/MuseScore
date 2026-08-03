/*
 * SPDX-License-Identifier: GPL-3.0-only
 * MuseScore-Studio-CLA-applies
 *
 * MuseScore Studio
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

#include "audiomidipreferencesmodel.h"

#include "translation.h"
#include "log.h"

#if defined(MUSE_MODULE_AUDIO_JACK) && defined(Q_OS_LINUX)
#include "audio/common/workmode.h"
#endif

using namespace mu::preferences;
using namespace muse::audio;
using namespace muse::midi;

#if defined(MUSE_MODULE_AUDIO_JACK) && defined(Q_OS_LINUX)
static constexpr const char* JACK_AUDIO_API = "JACK";
#endif

AudioMidiPreferencesModel::AudioMidiPreferencesModel(QObject* parent)
    : QObject(parent), muse::Contextable(muse::iocCtxForQmlObject(this))
{
}

int AudioMidiPreferencesModel::currentAudioApiIndex() const
{
    QString currentApi = QString::fromStdString(audioDriverController()->currentAudioApi());
    return audioApiList().indexOf(currentApi);
}

bool AudioMidiPreferencesModel::audioApiSelectionEnabled() const
{
#if defined(MUSE_MODULE_AUDIO_JACK) && defined(Q_OS_LINUX)
    const mu::context::IPlaybackStatePtr playbackState = globalContext()->playbackState();
    return playbackState && playbackState->playbackStatus() == PlaybackStatus::Stopped;
#else
    return true;
#endif
}

bool AudioMidiPreferencesModel::jackWorkerRpcWarningVisible() const
{
#if defined(MUSE_MODULE_AUDIO_JACK) && defined(Q_OS_LINUX)
    return audioDriverController()->currentAudioApi() == JACK_AUDIO_API
           && workmode::mode() != workmode::WorkerRpcMode;
#else
    return false;
#endif
}

QString AudioMidiPreferencesModel::jackWorkModeName() const
{
#if defined(MUSE_MODULE_AUDIO_JACK) && defined(Q_OS_LINUX)
    switch (workmode::mode()) {
    case workmode::WorkerMode:
        return QStringLiteral("Worker mode");
    case workmode::DriverMode:
        return QStringLiteral("Driver mode");
    case workmode::WorkerRpcMode:
        return QStringLiteral("Worker RPC mode");
    case workmode::Undefined:
        break;
    }
#endif

    return QStringLiteral("Unknown audio work mode");
}

void AudioMidiPreferencesModel::useWorkerRpcAndRestart()
{
#if defined(MUSE_MODULE_AUDIO_JACK) && defined(Q_OS_LINUX)
    if (!jackWorkerRpcWarningVisible()) {
        return;
    }

    workmode::setMode(workmode::WorkerRpcMode);
    emit applyAndRestartRequested();
#endif
}

void AudioMidiPreferencesModel::restartApplication()
{
    dispatcher()->dispatch("restart");
}

void AudioMidiPreferencesModel::setCurrentAudioApiIndex(int index)
{
    if (!audioApiSelectionEnabled()) {
        interactive()->warning("", muse::trc("preferences", "Stop playback before changing the audio driver."));
        emit currentAudioApiIndexChanged(currentAudioApiIndex());
        return;
    }

    std::vector<std::string> apiList = audioDriverController()->availableAudioApiList();
    if (index < 0 || index >= static_cast<int>(apiList.size())) {
        return;
    }

    const std::string requestedApi = apiList.at(index);
    if (requestedApi == audioDriverController()->currentAudioApi()) {
        return;
    }

#if defined(MUSE_MODULE_AUDIO_JACK) && defined(Q_OS_LINUX)
    if (requestedApi == JACK_AUDIO_API && workmode::mode() != workmode::WorkerRpcMode) {
        emit currentAudioApiIndexChanged(currentAudioApiIndex());
        showJackWorkModeWarning(requestedApi);
        return;
    }
#endif

    switchAudioApi(requestedApi);
}

bool AudioMidiPreferencesModel::switchAudioApi(const std::string& requestedApi)
{
    m_reconcilingAudioApi = true;
    const bool switched = audioDriverController()->changeCurrentAudioApi(requestedApi);
    const std::string actualApi = audioDriverController()->currentAudioApi();
    audioConfiguration()->setCurrentAudioApi(actualApi);
    m_reconcilingAudioApi = false;

    emit currentAudioApiIndexChanged(currentAudioApiIndex());

    if (!switched) {
        showAudioApiSwitchError(requestedApi);
    }

    return switched;
}

#if defined(MUSE_MODULE_AUDIO_JACK) && defined(Q_OS_LINUX)
void AudioMidiPreferencesModel::showJackWorkModeWarning(const std::string& requestedApi)
{
    const std::string title = muse::trc("preferences", "Worker RPC mode is recommended for JACK");
    const std::string text = muse::qtrc(
        "preferences",
        "MuseScore Studio is currently using %1. JACK will still work, but latency-correct recording is not guaranteed. "
        "Switch to Worker RPC mode for accurate recording.").arg(jackWorkModeName()).toStdString();

    const muse::IInteractive::ButtonData cancelButton = interactive()->buttonData(muse::IInteractive::Button::Cancel);
    const muse::IInteractive::ButtonData useAnywayButton(
        muse::IInteractive::Button::Ignore,
        muse::trc("preferences", "Use JACK anyway"),
        false,
        false,
        muse::IInteractive::ButtonRole::ContinueRole);
    const muse::IInteractive::ButtonData switchButton(
        muse::IInteractive::Button::Apply,
        muse::trc("preferences", "Switch to Worker RPC and restart"),
        true,
        false,
        muse::IInteractive::ButtonRole::ApplyRole);

    interactive()->warning(title, text, { cancelButton, useAnywayButton, switchButton }, switchButton.btn)
    .onResolve(this, [this, requestedApi](const muse::IInteractive::Result& result) {
        if (result.isButton(muse::IInteractive::Button::Ignore)) {
            switchAudioApi(requestedApi);
            return;
        }

        if (result.isButton(muse::IInteractive::Button::Apply) && switchAudioApi(requestedApi)) {
            workmode::setMode(workmode::WorkerRpcMode);
            emit applyAndRestartRequested();
            return;
        }

        emit currentAudioApiIndexChanged(currentAudioApiIndex());
    });
}
#endif

void AudioMidiPreferencesModel::showAudioApiSwitchError(const std::string& requestedApi) const
{
    interactive()->error(
        "",
        muse::qtrc("preferences", "The %1 audio driver could not be opened. MuseScore Studio restored %2.")
        .arg(QString::fromStdString(requestedApi), QString::fromStdString(audioDriverController()->currentAudioApi())).toStdString());
}

void AudioMidiPreferencesModel::reconcilePreferredAudioApi()
{
#if defined(MUSE_MODULE_AUDIO_JACK) && defined(Q_OS_LINUX)
    if (m_reconcilingAudioApi) {
        return;
    }

    const std::string preferredApi = audioConfiguration()->currentAudioApi();
    if (preferredApi == audioDriverController()->currentAudioApi()) {
        return;
    }

    if (!audioApiSelectionEnabled()) {
        emit currentAudioApiIndexChanged(currentAudioApiIndex());
        interactive()->warning(
            "",
            muse::trc("preferences", "The audio driver cannot be changed during playback. The saved driver will be used after restart."));
        return;
    }

    m_reconcilingAudioApi = true;
    const bool switched = audioDriverController()->changeCurrentAudioApi(preferredApi);
    const std::string actualApi = audioDriverController()->currentAudioApi();
    if (audioConfiguration()->currentAudioApi() != actualApi) {
        audioConfiguration()->setCurrentAudioApi(actualApi);
    }
    m_reconcilingAudioApi = false;

    emit currentAudioApiIndexChanged(currentAudioApiIndex());

    if (!switched) {
        showAudioApiSwitchError(preferredApi);
    }
#endif
}

QString AudioMidiPreferencesModel::midiInputDeviceId() const
{
    return QString::fromStdString(midiInPort()->deviceID());
}

void AudioMidiPreferencesModel::inputDeviceSelected(const QString& deviceId)
{
    midiConfiguration()->setMidiInputDeviceId(deviceId.toStdString());
}

QString AudioMidiPreferencesModel::midiOutputDeviceId() const
{
    return QString::fromStdString(midiOutPort()->deviceID());
}

void AudioMidiPreferencesModel::outputDeviceSelected(const QString& deviceId)
{
    midiConfiguration()->setMidiOutputDeviceId(deviceId.toStdString());
}

void AudioMidiPreferencesModel::init()
{
#if defined(MUSE_MODULE_AUDIO_JACK) && defined(Q_OS_LINUX)
    const mu::context::IPlaybackStatePtr playbackState = globalContext()->playbackState();
    if (playbackState) {
        playbackState->playbackStatusChanged().onReceive(this, [this](PlaybackStatus) {
            emit audioApiSelectionEnabledChanged();
        });
    }

    audioConfiguration()->currentAudioApiChanged().onNotify(this, [this]() {
        reconcilePreferredAudioApi();
    });
#endif

    midiInPort()->availableDevicesChanged().onNotify(this, [this]() {
        emit midiInputDevicesChanged();
    });

    midiInPort()->deviceChanged().onNotify(this, [this]() {
        emit midiInputDeviceIdChanged();
    });

    midiOutPort()->availableDevicesChanged().onNotify(this, [this]() {
        emit midiOutputDevicesChanged();
    });

    midiOutPort()->deviceChanged().onNotify(this, [this]() {
        emit midiOutputDeviceIdChanged();
    });

    midiConfiguration()->useMIDI20OutputChanged().onReceive(this, [this](bool) {
        emit useMIDI20OutputChanged();
    });

    playbackConfiguration()->muteHiddenInstrumentsChanged().onReceive(this, [this](bool mute) {
        emit muteHiddenInstrumentsChanged(mute);
    });

    playbackConfiguration()->shouldShowOnlineSoundsProcessingErrorChanged().onNotify(this, [this]() {
        emit shouldShowOnlineSoundsProcessingErrorChanged();
    });

    playbackConfiguration()->onlineSoundsShowProgressBarModeChanged().onNotify(this, [this]() {
        emit onlineSoundsShowProgressBarModeChanged();
    });

    audioDriverController()->currentAudioApiChanged().onNotify(this, [this]() {
        emit currentAudioApiIndexChanged(currentAudioApiIndex());
        emit jackWorkerRpcWarningVisibleChanged();
    });

    audioConfiguration()->autoProcessOnlineSoundsInBackgroundChanged().onReceive(this, [this](bool) {
        emit autoProcessOnlineSoundsInBackgroundChanged();
    });

    audioConfiguration()->useSoundFontLowPassFilterChanged().onReceive(this, [this](bool) {
        emit useSoundFontLowPassFilterChanged();
    });
}

QStringList AudioMidiPreferencesModel::audioApiList() const
{
    const std::vector<std::string> apiList = audioDriverController()->availableAudioApiList();

    QStringList result;
    result.reserve(apiList.size());

    for (const std::string& api: apiList) {
        result.emplace_back(QString::fromStdString(api));
    }

    return result;
}

void AudioMidiPreferencesModel::restartAudioAndMidiDevices()
{
    NOT_IMPLEMENTED;
}

bool AudioMidiPreferencesModel::onlineSoundsSectionVisible() const
{
#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
    return true;
#else
    return false;
#endif
}

QVariantList AudioMidiPreferencesModel::midiInputDevices() const
{
    std::vector<MidiDevice> devices = midiInPort()->availableDevices();

    QVariantList result;
    result.reserve(devices.size());

    for (const MidiDevice& device : devices) {
        QVariantMap obj;
        obj["value"] = QString::fromStdString(device.id);
        obj["text"] = QString::fromStdString(device.name);

        result << obj;
    }

    return result;
}

QVariantList AudioMidiPreferencesModel::midiOutputDevices() const
{
    std::vector<MidiDevice> devices = midiOutPort()->availableDevices();

    QVariantList result;
    result.reserve(devices.size());

    for (const MidiDevice& device : devices) {
        QVariantMap obj;
        obj["value"] = QString::fromStdString(device.id);
        obj["text"] = QString::fromStdString(device.name);

        result << obj;
    }

    return result;
}

bool AudioMidiPreferencesModel::isMIDI20OutputSupported() const
{
    return midiOutPort()->supportsMIDI20Output();
}

bool AudioMidiPreferencesModel::useMIDI20Output() const
{
    return midiConfiguration()->useMIDI20Output();
}

void AudioMidiPreferencesModel::setUseMIDI20Output(bool use)
{
    if (use == useMIDI20Output()) {
        return;
    }

    midiConfiguration()->setUseMIDI20Output(use);
}

void AudioMidiPreferencesModel::showMidiError(const MidiDeviceID& deviceId, const std::string& text) const
{
    // todo: display error
    LOGE() << "failed connect to device, deviceID: " << deviceId << ", err: " << text;
}

bool AudioMidiPreferencesModel::muteHiddenInstruments() const
{
    return playbackConfiguration()->muteHiddenInstruments();
}

void AudioMidiPreferencesModel::setMuteHiddenInstruments(bool mute)
{
    if (mute == muteHiddenInstruments()) {
        return;
    }

    playbackConfiguration()->setMuteHiddenInstruments(mute);
}

bool AudioMidiPreferencesModel::shouldShowOnlineSoundsProcessingError() const
{
    return playbackConfiguration()->shouldShowOnlineSoundsProcessingError();
}

void AudioMidiPreferencesModel::setShouldShowOnlineSoundsProcessingError(bool value)
{
    if (value == shouldShowOnlineSoundsProcessingError()) {
        return;
    }

    playbackConfiguration()->setShouldShowOnlineSoundsProcessingError(value);
}

bool AudioMidiPreferencesModel::autoProcessOnlineSoundsInBackground() const
{
    return audioConfiguration()->autoProcessOnlineSoundsInBackground();
}

void AudioMidiPreferencesModel::setAutoProcessOnlineSoundsInBackground(bool value)
{
    if (value == autoProcessOnlineSoundsInBackground()) {
        return;
    }

    audioConfiguration()->setAutoProcessOnlineSoundsInBackground(value);

    if (!value) {
        if (playbackConfiguration()->onlineSoundsShowProgressBarMode() == playback::OnlineSoundsShowProgressBarMode::DuringPlayback) {
            playbackConfiguration()->setOnlineSoundsShowProgressBarMode(playback::OnlineSoundsShowProgressBarMode::Always);
        }
    }
}

int AudioMidiPreferencesModel::onlineSoundsShowProgressBarMode() const
{
    return static_cast<int>(playbackConfiguration()->onlineSoundsShowProgressBarMode());
}

void AudioMidiPreferencesModel::setOnlineSoundsShowProgressBarMode(int mode)
{
    if (mode == onlineSoundsShowProgressBarMode()) {
        return;
    }

    playbackConfiguration()->setOnlineSoundsShowProgressBarMode(static_cast<playback::OnlineSoundsShowProgressBarMode>(mode));
}

bool AudioMidiPreferencesModel::useSoundFontLowPassFilter() const
{
    return audioConfiguration()->useSoundFontLowPassFilter();
}

void AudioMidiPreferencesModel::setUseSoundFontLowPassFilter(bool value)
{
    if (value == useSoundFontLowPassFilter()) {
        return;
    }

    audioConfiguration()->setUseSoundFontLowPassFilter(value);
}

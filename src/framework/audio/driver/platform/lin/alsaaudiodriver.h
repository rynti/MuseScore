/*
 * SPDX-License-Identifier: GPL-3.0-only
 * MuseScore-CLA-applies
 *
 * MuseScore
 * Music Composition & Notation
 *
 * Copyright (C) 2021 MuseScore BVBA and others
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

#include "async/asyncable.h"

#include "iaudiodriver.h"

#include "platform/lin/audiodeviceslistener.h"
#include "playback/iplaybackconfiguration.h"
#include "playback/iplaybackcontroller.h"

namespace muse::audio {
<<<<<<<< HEAD:src/framework/audio/driver/platform/lin/alsaaudiodriver.h
class AlsaAudioDriver : public IAudioDriver, public async::Asyncable
========
class AudioMidiManager : public IAudioDriver, public async::Asyncable
>>>>>>>> c2e15fb9b1 (Apply lyrra's jack diff):src/framework/audio/internal/audiomidimanager.h
{
    Inject<mu::playback::IPlaybackConfiguration> playbackConfiguration;
    Inject<mu::playback::IPlaybackController> playbackController;

public:
<<<<<<<< HEAD:src/framework/audio/driver/platform/lin/alsaaudiodriver.h
    AlsaAudioDriver();
    ~AlsaAudioDriver();
========
    AudioMidiManager();
    ~AudioMidiManager();
>>>>>>>> c2e15fb9b1 (Apply lyrra's jack diff):src/framework/audio/internal/audiomidimanager.h

    void init() override;

    std::string name() const override;
    bool open(const Spec& spec, Spec* activeSpec) override;
    void close() override;
    bool isOpened() const override;

    const Spec& activeSpec() const override;

    AudioDeviceID outputDevice() const override;
    bool selectOutputDevice(const AudioDeviceID& deviceId) override;
    bool resetToDefaultOutputDevice() override;
    muse::async::Notification outputDeviceChanged() const override;

    AudioDeviceList availableOutputDevices() const override;
    muse::async::Notification availableOutputDevicesChanged() const override;

    unsigned int outputDeviceBufferSize() const override;
    bool setOutputDeviceBufferSize(unsigned int bufferSize) override;
    muse::async::Notification outputDeviceBufferSizeChanged() const override;

    std::vector<unsigned int> availableOutputDeviceBufferSizes() const override;

    int audioDelayCompensate() const override;
    void setAudioDelayCompensate(const int frames) override;

    unsigned int outputDeviceSampleRate() const override;
    bool setOutputDeviceSampleRate(unsigned int sampleRate) override;
    muse::async::Notification outputDeviceSampleRateChanged() const override;

    std::vector<unsigned int> availableOutputDeviceSampleRates() const override;

    void isPlayingChanged();
    void positionChanged(muse::audio::secs_t secs, muse::midi::tick_t tick);

    bool isPlaying() const override;
    void remotePlayOrStop(bool) const override;
    void remoteSeek(msecs_t) const override;

    void resume() override;
    void suspend() override;

private:
    bool makeDevice(const AudioDeviceID& deviceId);
    bool reopen(const AudioDeviceID& deviceId, Spec newSpec);
    muse::async::Notification m_outputDeviceChanged;

    mutable std::mutex m_devicesMutex;
#ifndef Q_OS_MACOS
    AudioDevicesListener m_devicesListener;
#endif
    muse::async::Notification m_availableOutputDevicesChanged;

    std::string m_deviceId;

    muse::async::Notification m_bufferSizeChanged;
    muse::async::Notification m_sampleRateChanged;
    int m_audioDelayCompensate;

    struct IAudioDriver::Spec m_spec;
    std::unique_ptr<AudioDriverState> m_current_audioDriverState;
};
}

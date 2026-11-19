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

#pragma once

#include "iaudiodriver.h"

#include "modularity/imoduleinterface.h"

namespace muse::audio {
class IAudioDriverController : MODULE_CONTEXT_INTERFACE
{
    INTERFACE_ID(IAudioDriverController)

public:
    virtual ~IAudioDriverController() = default;

    // Api
    virtual std::vector<std::string> availableAudioApiList() const = 0;

    virtual std::string currentAudioApi() const = 0;
    virtual bool changeCurrentAudioApi(const std::string& name) = 0;
    virtual async::Notification currentAudioApiChanged() const = 0;

    // Current driver operation
    virtual AudioDeviceList availableOutputDevices() const = 0;
    virtual async::Notification availableOutputDevicesChanged() const = 0;

    virtual bool open(const IAudioDriver::Spec& spec, IAudioDriver::Spec* activeSpec) = 0;
    virtual void close() = 0;
    virtual bool isOpened() const = 0;

    virtual const IAudioDriver::Spec& activeSpec() const = 0;
    virtual async::Channel<IAudioDriver::Spec> activeSpecChanged() const = 0;

    virtual AudioDeviceID outputDevice() const = 0;
    virtual bool selectOutputDevice(const AudioDeviceID& deviceId) = 0;
    virtual async::Notification outputDeviceChanged() const = 0;

    virtual std::vector<samples_t> availableOutputDeviceBufferSizes() const = 0;
    virtual void changeBufferSize(samples_t samples) = 0;
    virtual async::Notification outputDeviceBufferSizeChanged() const = 0;

    virtual std::vector<sample_rate_t> availableOutputDeviceSampleRates() const = 0;
    virtual void changeSampleRate(sample_rate_t sampleRate) = 0;
    virtual async::Notification outputDeviceSampleRateChanged() const = 0;

    // Optional shared transport. The default implementation keeps existing
    // non-JACK controllers and test doubles source-compatible.
    virtual bool isTransportSyncAvailable() const { return false; }
    virtual bool transportSyncRequested() const { return false; }
    virtual AudioDriverTransportSyncState transportSyncState() const { return AudioDriverTransportSyncState::Off; }
    virtual async::Notification transportSyncStateChanged() const { return {}; }
    virtual async::Channel<AudioDriverTransportEvent> transportEvent() const { return {}; }

    virtual void setTransportSyncEnabled(bool) {}
    virtual bool requestTransportPlay(secs_t) { return false; }
    virtual bool requestTransportPause() { return false; }
    virtual bool requestTransportStop() { return false; }
    virtual bool requestTransportSeek(secs_t) { return false; }

    virtual bool isTransportPreparationCurrent(uint64_t, uint64_t) const { return false; }
    virtual bool activateTransportForPreparation(uint64_t, uint64_t) { return false; }
    virtual bool completeTransportPreparation(uint64_t, uint64_t, bool) { return false; }
    virtual void cancelPendingTransportWork() {}
};
}

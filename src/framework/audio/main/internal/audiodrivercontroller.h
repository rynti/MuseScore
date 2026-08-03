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

#include <optional>

#include "global/async/asyncable.h"

#include "audio/iaudiodrivercontroller.h"

#include "modularity/ioc.h"
#include "audio/main/iaudioconfiguration.h"
#include "audio/common/rpc/irpcchannel.h"

namespace muse::audio {
class JackAudioDriver;

class AudioDriverController : public IAudioDriverController, public Contextable, public async::Asyncable
{
    GlobalInject<IAudioConfiguration> configuration;
    ContextInject<rpc::IRpcChannel> rpcChannel = { this };

public:
    AudioDriverController(const modularity::ContextPtr& iocCtx)
        : Contextable(iocCtx) {}

    void init();
    void poll();

    // Api
    std::vector<std::string> availableAudioApiList() const override;

    std::string currentAudioApi() const override;
    bool changeCurrentAudioApi(const std::string& name) override;
    async::Notification currentAudioApiChanged() const override;

    // Current driver operation
    AudioDeviceList availableOutputDevices() const override;
    async::Notification availableOutputDevicesChanged() const override;

    bool open(const IAudioDriver::Spec& spec, IAudioDriver::Spec* activeSpec) override;
    void close() override;
    bool isOpened() const override;

    const IAudioDriver::Spec& activeSpec() const override;
    async::Channel<IAudioDriver::Spec> activeSpecChanged() const override;

    AudioDeviceID outputDevice() const override;
    bool selectOutputDevice(const AudioDeviceID& deviceId) override;
    async::Notification outputDeviceChanged() const override;

    std::vector<samples_t> availableOutputDeviceBufferSizes() const override;
    void changeBufferSize(samples_t samples) override;
    async::Notification outputDeviceBufferSizeChanged() const override;

    std::vector<sample_rate_t> availableOutputDeviceSampleRates() const override;
    void changeSampleRate(sample_rate_t sampleRate) override;
    async::Notification outputDeviceSampleRateChanged() const override;

    bool isTransportSyncAvailable() const override;
    bool transportSyncRequested() const override;
    AudioDriverTransportSyncState transportSyncState() const override;
    async::Notification transportSyncStateChanged() const override;
    async::Channel<AudioDriverTransportEvent> transportEvent() const override;

    void setTransportSyncEnabled(bool enabled) override;
    bool requestTransportPlay(secs_t position) override;
    bool requestTransportPause() override;
    bool requestTransportStop() override;
    bool requestTransportSeek(secs_t position) override;

    bool isTransportPreparationCurrent(uint64_t driverGeneration, uint64_t token) const override;
    bool activateTransportForPreparation(uint64_t driverGeneration, uint64_t token) override;
    bool completeTransportPreparation(uint64_t driverGeneration, uint64_t token, bool success) override;
    void cancelPendingTransportWork() override;

private:
    struct OpenedDriver {
        IAudioDriverPtr driver;
        IAudioDriver::Spec spec;
        uint64_t generation = 0;
    };

    std::string canonicalAudioApi(const std::string& name) const;
    IAudioDriverPtr createDriver(const std::string& name) const;
    bool tryOpenDriver(const std::string& name, const IAudioDriver::Spec& spec, bool tryDefaultDevice, OpenedDriver& result);
    void installOpenedDriver(const OpenedDriver& openedDriver, const std::string& previousApi = {});
    void setNewDriver(IAudioDriverPtr newDriver);
    void publishActiveSpec(const IAudioDriver::Spec& spec);

    void handleOutputDeviceChange();
    bool switchToDefaultAudioDriver(IAudioDriver::Spec* activeSpec = nullptr);
    void updateOutputSpec(const OutputSpec& spec);

    JackAudioDriver* jackDriver() const;
    uint64_t nextDriverGeneration();
    void configureTransportBeforeOpen(const IAudioDriverPtr& driver, uint64_t generation);
    void resetTransportForDriverChange();
    void applyRequestedTransportState();
    void setTransportSyncState(AudioDriverTransportSyncState state);
    void sendTransportEvent(AudioDriverTransportEventType type, uint64_t token = 0, secs_t position = 0.0,
                            const std::string& message = {});
    void pollJackTransport();
    void pollJackStatus();
    uint64_t secondsToTransportFrame(secs_t position) const;
    secs_t transportFrameToSeconds(uint64_t frame) const;

    IAudioDriver::Callback m_callback;
    IAudioDriverPtr m_audioDriver;
    async::Notification m_currentAudioApiChanged;
    async::Notification m_availableOutputDevicesChanged;
    async::Channel<IAudioDriver::Spec> m_activeSpecChanged;
    async::Notification m_outputDeviceChanged;
    async::Notification m_outputDeviceBufferSizeChanged;
    async::Notification m_outputDeviceSampleRateChanged;

    async::Notification m_transportSyncStateChanged;
    async::Channel<AudioDriverTransportEvent> m_transportEvent;

    AudioDriverTransportSyncState m_transportSyncState = AudioDriverTransportSyncState::Off;
    uint64_t m_nextDriverGeneration = 1;
    uint64_t m_driverGeneration = 0;
    uint64_t m_preparationToken = 0;
    std::optional<uint64_t> m_pendingLocalTransportFrame;
    bool m_pendingStopRequest = false;
    bool m_pendingPauseRequest = false;
    bool m_transportConfigurationGuard = false;
    bool m_jackRuntimeFaulted = false;

    bool m_retryOpenDevice = false;
};
}

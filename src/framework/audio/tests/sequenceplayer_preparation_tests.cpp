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
 */

#include "audio/engine/internal/sequenceplayer.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "audio/common/audiosanitizer.h"
#include "audio/engine/internal/mixer.h"
#include "audio/engine/internal/track.h"
#include "global/modularity/ioc.h"

namespace muse::audio::engine {
class SequencePlayerTestAccess {
public:
  static void prepareTracks(SequencePlayer &player,
                            std::function<void()> onReady) {
    player.prepareAllTracksToPlay(std::move(onReady));
  }
};

class MixerTestAccess {
public:
  static void setTrackFx(MixerChannel &channel, IFxProcessorPtr processor) {
    channel.m_fxProcessors = {std::move(processor)};
  }

  static void setMasterFx(Mixer &mixer, IFxProcessorPtr processor) {
    mixer.m_masterFxProcessors = {std::move(processor)};
  }

  static void setMuted(MixerChannel &channel, bool muted) {
    channel.m_params.muted = muted;
    channel.m_mutedChanged.notify();
  }
};
} // namespace muse::audio::engine

namespace {
using namespace muse;
using namespace muse::audio;
using namespace muse::audio::engine;

class FakeClock final : public IClock {
public:
  msecs_t currentTime() const override { return m_time; }

  void forward(const msecs_t nextMsecs) override {
    m_time += nextMsecs;
    m_timeChanged.send(microsecsToSecs(m_time));
  }

  void start() override { setStatus(PlaybackStatus::Running); }
  void reset() override { relocate(0, ActionType::Seek); }
  void stop() override { setStatus(PlaybackStatus::Stopped); }
  void pause() override { setStatus(PlaybackStatus::Paused); }
  void resume() override { setStatus(PlaybackStatus::Running); }
  void seek(const msecs_t msecs) override { relocate(msecs, ActionType::Seek); }
  bool isRunning() const override {
    return m_status == PlaybackStatus::Running;
  }

  PlaybackStatus status() const override { return m_status; }
  async::Channel<PlaybackStatus> statusChanged() const override {
    return m_statusChanged;
  }

  msecs_t timeDuration() const override { return m_duration; }
  void setTimeDuration(const msecs_t duration) override {
    m_duration = duration;
  }
  Ret setTimeLoop(const msecs_t fromMsec, const msecs_t toMsec) override {
    m_loopFrom = fromMsec;
    m_loopTo = toMsec;
    return make_ok();
  }
  void resetTimeLoop() override {
    m_loopFrom = 0;
    m_loopTo = 0;
  }

  void setCountDown(const msecs_t duration) override { m_countDown = duration; }
  async::Notification countDownEnded() const override {
    return m_countDownEnded;
  }
  async::Channel<secs_t> timeChanged() const override { return m_timeChanged; }

  void setOnAction(OnActionFunc func) override { m_onAction = std::move(func); }

  void relocate(msecs_t time, ActionType action) {
    m_time = time;
    m_timeChanged.send(microsecsToSecs(m_time));
    if (m_onAction) {
      m_onAction(action, time);
    }
  }

private:
  void setStatus(PlaybackStatus status) {
    m_status = status;
    m_statusChanged.send(status);
  }

  msecs_t m_time = 0;
  msecs_t m_duration = 0;
  msecs_t m_loopFrom = 0;
  msecs_t m_loopTo = 0;
  msecs_t m_countDown = 0;
  PlaybackStatus m_status = PlaybackStatus::Stopped;
  OnActionFunc m_onAction;
  async::Channel<PlaybackStatus> m_statusChanged;
  async::Notification m_countDownEnded;
  async::Channel<secs_t> m_timeChanged;
};

class FakeTrackInput final : public ITrackAudioInput {
public:
  struct SeekCall {
    msecs_t position = 0;
    bool flushSound = false;
  };

  bool isActive() const override { return m_active; }
  void setIsActive(bool active) override { m_active = active; }
  void setOutputSpec(const OutputSpec &spec) override { m_outputSpec = spec; }
  unsigned int audioChannelsCount() const override { return 2; }
  async::Channel<unsigned int> audioChannelsCountChanged() const override {
    return {};
  }
  samples_t process(float *buffer, samples_t samplesPerChannel) override {
    const size_t sampleCount =
        static_cast<size_t>(samplesPerChannel * audioChannelsCount());
    std::fill(buffer, buffer + sampleCount, 1.f);
    return samplesPerChannel;
  }

  void seek(const msecs_t newPositionMsecs,
            const bool flushSound = true) override {
    seeks.push_back({newPositionMsecs, flushSound});
    events.push_back("seek");
  }
  void flush() override { events.push_back("flush"); }

  const AudioInputParams &inputParams() const override { return m_inputParams; }
  void applyInputParams(const AudioInputParams &requiredParams) override {
    m_inputParams = requiredParams;
  }
  async::Channel<AudioInputParams> inputParamsChanged() const override {
    return m_inputParamsChanged;
  }

  void prepareToPlay() override {
    ++prepareCount;
    events.push_back("prepare");
  }
  bool readyToPlay() const override { return ready; }
  async::Notification readyToPlayChanged() const override {
    return m_readyChanged;
  }

  bool hasPendingChunks() const override { return false; }
  void processInput() override { ++offStreamProcessCount; }
  InputProcessingProgress inputProcessingProgress() const override {
    return {};
  }
  void clearCache() override {}

  void setReady(bool value) {
    ready = value;
    m_readyChanged.notify();
  }

  std::vector<SeekCall> seeks;
  std::vector<std::string> events;
  bool ready = true;
  int prepareCount = 0;
  int offStreamProcessCount = 0;

private:
  bool m_active = true;
  OutputSpec m_outputSpec;
  AudioInputParams m_inputParams;
  async::Channel<AudioInputParams> m_inputParamsChanged;
  async::Notification m_readyChanged;
};

class FakeGetTracks final : public IGetTracks {
public:
  TrackPtr track(const TrackId id) const override {
    auto it = tracks.find(id);
    return it == tracks.end() ? nullptr : it->second;
  }

  const TracksMap &allTracks() const override { return tracks; }
  async::Channel<TrackPtr> trackAboutToBeAdded() const override { return {}; }
  async::Channel<TrackPtr> trackAboutToBeRemoved() const override { return {}; }

  void add(TrackId id, const std::shared_ptr<FakeTrackInput> &input) {
    auto track = std::make_shared<EventTrack>();
    track->id = id;
    track->inputHandler = input;
    tracks.emplace(id, std::move(track));
  }

  TracksMap tracks;
};

class FakeFxProcessor final : public IFxProcessor {
public:
  explicit FakeFxProcessor(AudioFxChainOrder order) {
    m_params.chainOrder = order;
    m_params.active = true;
  }

  AudioFxType type() const override { return AudioFxType::Undefined; }
  const AudioFxParams &params() const override { return m_params; }
  async::Channel<AudioFxParams> paramsChanged() const override {
    return m_paramsChanged;
  }
  void setOutputSpec(const OutputSpec &spec) override { m_outputSpec = spec; }
  bool active() const override { return m_active; }
  void setActive(bool active) override { m_active = active; }
  void setPlaying(bool playing) override { m_playing = playing; }
  bool shouldProcessDuringSilence() const override { return false; }
  void process(float *, samples_t, samples_t playbackPositionSamples) override {
    positions.push_back(playbackPositionSamples);
  }

  std::vector<samples_t> positions;

private:
  AudioFxParams m_params;
  OutputSpec m_outputSpec;
  bool m_active = true;
  bool m_playing = false;
  async::Channel<AudioFxParams> m_paramsChanged;
};

class FakeAudioEngine final : public IAudioEngine {
public:
  explicit FakeAudioEngine(MixerPtr mixer) : m_mixer(std::move(mixer)) {}

  void setOutputSpec(const OutputSpec &outputSpec) override {
    m_outputSpec = outputSpec;
  }
  OutputSpec outputSpec() const override { return m_outputSpec; }
  async::Channel<OutputSpec> outputSpecChanged() const override {
    return m_outputSpecChanged;
  }
  RenderMode mode() const override { return m_mode; }
  void setMode(const RenderMode newMode) override {
    m_mode = newMode;
    m_modeChanged.send(newMode);
  }
  async::Channel<RenderMode> modeChanged() const override {
    return m_modeChanged;
  }
  void execOperation(OperationType type, const Operation &func) override {
    m_operation = type;
    func();
    m_operation = OperationType::NoOperation;
  }
  OperationType operation() const override { return m_operation; }
  MixerPtr mixer() const override { return m_mixer; }
  void processAudioData() override {}
  samples_t process(float *buffer, samples_t samplesPerChannel) override {
    return m_mixer->process(buffer, samplesPerChannel);
  }
  void popAudioData(float *, size_t) override {}

private:
  MixerPtr m_mixer;
  OutputSpec m_outputSpec;
  RenderMode m_mode = RenderMode::IdleMode;
  OperationType m_operation = OperationType::NoOperation;
  async::Channel<OutputSpec> m_outputSpecChanged;
  async::Channel<RenderMode> m_modeChanged;
};

class SequencePlayerPreparationTests : public ::testing::Test {
protected:
  void SetUp() override {
    AudioSanitizer::setupEngineThread();
    m_context = std::make_shared<modularity::Context>(4242);
    m_mixer = std::make_shared<Mixer>(m_context);
    m_mixer->init(1, 100);
    m_engine = std::make_shared<FakeAudioEngine>(m_mixer);

    modularity::ioc(m_context)->registerExport<IAudioEngine>(
        "sequence-player-preparation-tests", m_engine);

    m_outputSpec.sampleRate = 48000;
    m_outputSpec.samplesPerChannel = 480;
    m_outputSpec.audioChannelCount = 2;
    m_engine->setOutputSpec(m_outputSpec);
    m_mixer->setOutputSpec(m_outputSpec);

    m_clock = std::make_shared<FakeClock>();
    m_mixer->addClock(m_clock);
    m_player = std::make_unique<SequencePlayer>(&m_tracks, m_clock, m_context);
  }

  void TearDown() override {
    m_player.reset();
    m_mixer->removeClock(m_clock);
    m_engine.reset();
    m_mixer.reset();
    modularity::removeIoC(m_context);
    m_context.reset();
  }

  std::shared_ptr<FakeTrackInput> addTrack(TrackId id = 1) {
    auto input = std::make_shared<FakeTrackInput>();
    input->setOutputSpec(m_outputSpec);
    m_tracks.add(id, input);
    return input;
  }

  modularity::ContextPtr m_context;
  MixerPtr m_mixer;
  std::shared_ptr<FakeAudioEngine> m_engine;
  std::shared_ptr<FakeClock> m_clock;
  FakeGetTracks m_tracks;
  std::unique_ptr<SequencePlayer> m_player;
  OutputSpec m_outputSpec;
};

TEST_F(SequencePlayerPreparationTests,
       ImmediatePreparationFlushesAndReanchorsAtLogicalPosition) {
  auto input = addTrack();
  m_player->seek(3.0, true);
  input->events.clear();
  input->seeks.clear();

  bool resolved = false;
  SequencePlayerTestAccess::prepareTracks(*m_player,
                                          [&resolved]() { resolved = true; });

  EXPECT_TRUE(resolved);
  ASSERT_EQ(input->events.size(), 2u);
  EXPECT_EQ(input->events[0], "prepare");
  EXPECT_EQ(input->events[1], "seek");
  ASSERT_EQ(input->seeks.size(), 1u);
  EXPECT_EQ(input->seeks.back().position, 3000000);
  EXPECT_TRUE(input->seeks.back().flushSound);
}

TEST_F(SequencePlayerPreparationTests,
       AsyncPreparationReanchorsToANewerLocateBeforeResolving) {
  auto input = addTrack();
  input->ready = false;
  m_player->seek(3.0, true);
  input->events.clear();
  input->seeks.clear();

  bool resolved = false;
  SequencePlayerTestAccess::prepareTracks(*m_player,
                                          [&resolved]() { resolved = true; });
  EXPECT_FALSE(resolved);
  ASSERT_EQ(input->events, std::vector<std::string>({"prepare"}));

  m_player->seek(4.0, true);
  ASSERT_FALSE(input->seeks.empty());
  EXPECT_EQ(input->seeks.back().position, 4000000);

  input->setReady(true);
  EXPECT_TRUE(resolved);
  ASSERT_GE(input->seeks.size(), 2u);
  EXPECT_EQ(input->seeks.back().position, 4000000);
  EXPECT_TRUE(input->seeks.back().flushSound);
  EXPECT_DOUBLE_EQ(m_player->playbackPosition().to_double(), 4.0);
}

TEST_F(SequencePlayerPreparationTests,
       SamePositionRestartStillSeeksPreparesAndReseeksSources) {
  auto input = addTrack();
  m_player->seek(5.0, true);
  input->events.clear();
  input->seeks.clear();

  // SequencePlayer deliberately forwards a same-position seek even if the
  // clock itself treats it as a no-op.
  m_player->seek(5.0, true);
  bool resolved = false;
  SequencePlayerTestAccess::prepareTracks(*m_player,
                                          [&resolved]() { resolved = true; });

  EXPECT_TRUE(resolved);
  ASSERT_EQ(input->events,
            std::vector<std::string>({"seek", "prepare", "seek"}));
  ASSERT_EQ(input->seeks.size(), 2u);
  EXPECT_EQ(input->seeks[0].position, 5000000);
  EXPECT_EQ(input->seeks[1].position, 5000000);
  EXPECT_TRUE(input->seeks[0].flushSound);
  EXPECT_TRUE(input->seeks[1].flushSound);
}

TEST_F(SequencePlayerPreparationTests,
       NewAndUnmutedSourcesSeekToCurrentLogicalPosition) {
  m_player->seek(2.0, true);

  auto source = std::make_shared<FakeTrackInput>();
  const RetVal<MixerChannelPtr> result = m_mixer->addChannel(20, source);
  ASSERT_TRUE(result.ret);
  ASSERT_EQ(source->seeks.size(), 1u);
  EXPECT_EQ(source->seeks.back().position, 2000000);

  MixerTestAccess::setMuted(*result.val, true);

  m_player->seek(3.0, true);
  MixerTestAccess::setMuted(*result.val, false);
  ASSERT_GE(source->seeks.size(), 2u);
  EXPECT_EQ(source->seeks.back().position, 3000000);
}

TEST_F(SequencePlayerPreparationTests,
       PlayerSourcesAndFxUseOneLogicalTimeline) {
  auto input = addTrack();
  m_player->seek(1.0, true);
  ASSERT_FALSE(input->seeks.empty());
  EXPECT_EQ(input->seeks.back().position, 1000000);

  const RetVal<MixerChannelPtr> channelResult = m_mixer->addChannel(1, input);
  ASSERT_TRUE(channelResult.ret);

  auto trackFx = std::make_shared<FakeFxProcessor>(0);
  auto masterFx = std::make_shared<FakeFxProcessor>(0);
  MixerTestAccess::setTrackFx(*channelResult.val, trackFx);
  MixerTestAccess::setMasterFx(*m_mixer, masterFx);

  std::vector<float> buffer(m_outputSpec.samplesPerChannel *
                            m_outputSpec.audioChannelCount);
  ASSERT_EQ(m_mixer->process(buffer.data(), m_outputSpec.samplesPerChannel),
            m_outputSpec.samplesPerChannel);
  ASSERT_EQ(m_mixer->process(buffer.data(), m_outputSpec.samplesPerChannel),
            m_outputSpec.samplesPerChannel);

  ASSERT_EQ(trackFx->positions.size(), 2u);
  ASSERT_EQ(masterFx->positions.size(), 2u);
  EXPECT_EQ(trackFx->positions, masterFx->positions);
  EXPECT_EQ(trackFx->positions[1] - trackFx->positions[0], 480u);

  const samples_t logicalSamples = static_cast<samples_t>(
      m_clock->currentTime() * m_outputSpec.sampleRate / 1000000);
  EXPECT_EQ(trackFx->positions.back(), logicalSamples);
  EXPECT_DOUBLE_EQ(m_player->playbackPosition().to_double(), 1.02);
}
} // namespace

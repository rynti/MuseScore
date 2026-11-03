/*
 * SPDX-License-Identifier: GPL-3.0-only
 * MuseScore-Studio-CLA-applies
 */

#include <gtest/gtest.h>

#include <QtEnvironmentVariables>

#include "muse_framework_config.h"
#include "audio/main/internal/audiodriverfactory.h"

namespace muse::audio {
#ifdef Q_OS_LINUX
class Audio_AudioDriverFactoryTests : public ::testing::Test
{
protected:
    void SetUp() override
    {
        m_hadForceAlsa = qEnvironmentVariableIsSet("MUSESCORE_FORCE_ALSA");
        m_forceAlsa = qgetenv("MUSESCORE_FORCE_ALSA");
        qunsetenv("MUSESCORE_FORCE_ALSA");
    }

    void TearDown() override
    {
        if (m_hadForceAlsa) {
            qputenv("MUSESCORE_FORCE_ALSA", m_forceAlsa);
        } else {
            qunsetenv("MUSESCORE_FORCE_ALSA");
        }
    }

    AudioDriverFactory m_factory;
    bool m_hadForceAlsa = false;
    QByteArray m_forceAlsa;
};

TEST_F(Audio_AudioDriverFactoryTests, ListsAllCompiledLinuxBackends)
{
    std::vector<std::string> expected { "ALSA" };
#ifdef MUSE_MODULE_AUDIO_PIPEWIRE
    expected.push_back("PipeWire");
#endif
#ifdef MUSE_MODULE_AUDIO_JACK
    expected.push_back("JACK");
#endif
    EXPECT_EQ(m_factory.availableAudioDrivers(), expected);
}

TEST_F(Audio_AudioDriverFactoryTests, CreatesOnlyRequestedBackend)
{
    auto alsa = m_factory.createDriver("ALSA");
    ASSERT_TRUE(alsa);
    EXPECT_EQ(alsa->name(), "ALSA");
    EXPECT_FALSE(m_factory.createDriver("unavailable-backend"));
#ifdef MUSE_MODULE_AUDIO_JACK
    auto jack = m_factory.createDriver("JACK");
    ASSERT_TRUE(jack);
    EXPECT_EQ(jack->name(), "JACK");
#else
    EXPECT_FALSE(m_factory.createDriver("JACK"));
#endif
}

TEST_F(Audio_AudioDriverFactoryTests, ForcedAlsaOverridesJack)
{
    qputenv("MUSESCORE_FORCE_ALSA", "1");
    EXPECT_EQ(m_factory.availableAudioDrivers(), std::vector<std::string> { "ALSA" });
    auto driver = m_factory.createDriver("JACK");
    ASSERT_TRUE(driver);
    EXPECT_EQ(driver->name(), "ALSA");
}
#endif
}

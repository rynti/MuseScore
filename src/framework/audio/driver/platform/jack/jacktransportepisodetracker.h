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

#include <cstdint>

namespace muse::audio {
// Callback-domain state only. JACK position fields (including frame and the
// unique_1/unique_2 consistency pair) are deliberately not episode identity.
class JackTransportEpisodeTracker
{
public:
    struct StartingEpisode {
        bool isNew = false;
        uint64_t token = 0;
        uint64_t frame = 0;
    };

    StartingEpisode observeStarting(uint64_t frame) noexcept
    {
        if (m_inStarting) {
            return { false, m_token, m_frame };
        }

        m_inStarting = true;
        m_frame = frame;
        m_token = nextToken();
        return { true, m_token, m_frame };
    }

    void observeNotStarting() noexcept
    {
        m_inStarting = false;
        m_token = 0;
        m_frame = 0;
    }

private:
    uint64_t nextToken() noexcept
    {
        uint64_t token = m_nextToken++;
        if (token == 0) {
            token = m_nextToken++;
        }
        return token;
    }

    uint64_t m_nextToken = 1;
    uint64_t m_token = 0;
    uint64_t m_frame = 0;
    bool m_inStarting = false;
};
}

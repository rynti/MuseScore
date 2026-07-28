# SPDX-License-Identifier: GPL-3.0-only
# MuseScore-CLA-applies
#
# MuseScore
# Music Composition & Notation
#
# Copyright (C) 2025 MuseScore Limited and others
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License version 3 as
# published by the Free Software Foundation.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program.  If not, see <https://www.gnu.org/licenses/>.

set(MODULE_TEST muse_audio_tests)

set(MODULE_TEST_SRC
    ${CMAKE_CURRENT_LIST_DIR}/rpcpacker_tests.cpp
    ${CMAKE_CURRENT_LIST_DIR}/alignbuffer_tests.cpp
    ${CMAKE_CURRENT_LIST_DIR}/jacktransportepisodetracker_tests.cpp
    ${CMAKE_CURRENT_LIST_DIR}/sequenceplayer_renderlead_tests.cpp
)

set(MODULE_TEST_LINK muse_audio_engine)

include(SetupGTest)

if (OS_IS_LIN AND MUSE_MODULE_AUDIO_JACK)
    set(MODULE_TEST muse_audio_jack_tests)
    set(MODULE_TEST_SRC
        ${CMAKE_CURRENT_LIST_DIR}/jackaudiodriver_callback_tests.cpp
    )
    set(MODULE_TEST_INCLUDE
        ${CMAKE_CURRENT_LIST_DIR}/..
    )
    set(MODULE_TEST_LINK muse_audio_driver)

    include(SetupGTest)
endif()

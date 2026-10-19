##  Copyright (C) 2021 Arthur Benilov <arthur.benilov@gmail.com>
##
##  This program is free software; you can redistribute it and/or modify
##  it under the terms of the GNU General Public License as published by
##  the Free Software Foundation; either version 3 of the License, or
##  (at your option) any later version.
##
##  This program is distributed in the hope that it will be useful,
##  but WITHOUT ANY WARRANTY; without even the implied warranty of
##  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
##  GNU General Public License for more details.
##
##  You should have received a copy of the GNU General Public License
##  along with this program.  If not, see <http://www.gnu.org/licenses/>.

cmake_minimum_required(VERSION 3.20)

project(Aeolus VERSION 0.3.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>" CACHE STRING "Runtime Library")
endif()

option(WITH_ASIO "Enable ASIO audio interface" OFF)
option(WITH_MULTIBUS_OUTPUT "Enable multibus output" OFF)
option(AEOLUS_ENABLE_SPATIAL_NEON "Enable paired-ear ARM64 NEON spatial filters" ON)
option(AEOLUS_ENABLE_NEON "Enable ARM64 NEON arithmetic dispatch" ON)
option(AEOLUS_BOUNDED_VOICE_CLEAR "Clear only readable delay history (OFF restores full clears for comparison)" ON)
option(AEOLUS_PROFILE_BUILD "Use a separate standalone name/settings for profiling" OFF)

option(AEOLUS_ENABLE_IPO "Enable Release link-time optimization (no fast-math)" OFF)
if(AEOLUS_ENABLE_IPO)
    include(CheckIPOSupported)
    check_ipo_supported(RESULT aeolus_ipo_supported OUTPUT aeolus_ipo_error)
    if(NOT aeolus_ipo_supported)
        message(FATAL_ERROR "Link-time optimization unavailable: ${aeolus_ipo_error}")
    endif()
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
endif()

add_subdirectory(JUCE)

set(plugin_formats
    Standalone
)

if(WITH_MULTIBUS_OUTPUT)
    set(TARGET "${PROJECT_NAME}-multibus")
    set(PRODUCT_NAME "${PROJECT_NAME}-multibus")
    set(PLUGIN_DESCRIPTION "${PRODUCT_NAME} multibus")
    set(PLUGIN_CODE "E6hf")
else()
    set(TARGET "${PROJECT_NAME}")
    set(PRODUCT_NAME "${PROJECT_NAME}")
    set(PLUGIN_DESCRIPTION "${PRODUCT_NAME} stereo")
    set(PLUGIN_CODE "E6he")
endif()

if(AEOLUS_PROFILE_BUILD)
    set(PRODUCT_NAME "${PRODUCT_NAME}-Profile")
endif()

juce_add_plugin(${TARGET}
    VERSION "0.3.0"
    PLUGIN_MANUFACTURER_CODE "Arbe"
    PLUGIN_CODE ${PLUGIN_CODE}
    PLUGIN_DESCRIPTION ${PLUGIN_DESCRIPTION}
    PRODUCT_NAME ${PRODUCT_NAME}
    COMPANY_NAME "Arthur Benilov"
    BUNDLE_ID "com.ArthurBenilov.Aeolus"
    IS_SYNTH TRUE
    IS_MIDI_EFFECT FALSE
    NEEDS_MIDI_INPUT TRUE
    FORMATS ${plugin_formats}
    ICON_BIG "${CMAKE_CURRENT_SOURCE_DIR}/Resources/icons/icon256.png"
    ICON_SMALL "${CMAKE_CURRENT_SOURCE_DIR}/Resources/icons/icon64.png"
)


target_link_libraries(${TARGET}
    PRIVATE
        juce::juce_core
        juce::juce_data_structures
        juce::juce_audio_basics
        juce::juce_audio_utils
        juce::juce_audio_plugin_client
        juce::juce_gui_basics
        juce::juce_gui_extra
    PUBLIC
        juce::juce_recommended_config_flags
)

juce_generate_juce_header(${TARGET})

target_include_directories(${TARGET}
    PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/Source"
)

file(GLOB_RECURSE src
    "${CMAKE_CURRENT_SOURCE_DIR}/Source/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/Source/*.cpp"
)

# Preserve rejected research implementations in the checkout, but never link
# them into the production renderer or its regression target.
list(FILTER src EXCLUDE REGEX "/(spatialbatch|voicebatch|voicesnapshot|spatial_packet)\\.(cpp|h)$")

source_group(Source
    FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/Source/PluginProcessor.h
        ${CMAKE_CURRENT_SOURCE_DIR}/Source/PluginProcessor.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Source/PluginEditor.h
        ${CMAKE_CURRENT_SOURCE_DIR}/Source/PluginEditor.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Source/Parameters.h
        ${CMAKE_CURRENT_SOURCE_DIR}/Source/Parameters.cpp
)

source_group(Source\\mts
    FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/Source/mts/libMTSClient.h
        ${CMAKE_CURRENT_SOURCE_DIR}/Source/mts/libMTSClient.cpp
)

source_group(Source\\ui
    FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/Source/ui/CustomLookAndFeel.h
        ${CMAKE_CURRENT_SOURCE_DIR}/Source/ui/CustomLookAndFeel.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Source/ui/CustomMidiKeyboard.h
        ${CMAKE_CURRENT_SOURCE_DIR}/Source/ui/CustomMidiKeyboard.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Source/ui/DivisionControlPanel.h
        ${CMAKE_CURRENT_SOURCE_DIR}/Source/ui/DivisionControlPanel.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Source/ui/DivisionView.h
        ${CMAKE_CURRENT_SOURCE_DIR}/Source/ui/DivisionView.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Source/ui/GlobalTuningComponent.h
        ${CMAKE_CURRENT_SOURCE_DIR}/Source/ui/GlobalTuningComponent.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Source/ui/FxComponent.h
        ${CMAKE_CURRENT_SOURCE_DIR}/Source/ui/FxComponent.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Source/ui/SettingsComponent.h
        ${CMAKE_CURRENT_SOURCE_DIR}/Source/ui/SettingsComponent.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Source/ui/MidiChannelsComponent.h
        ${CMAKE_CURRENT_SOURCE_DIR}/Source/ui/MidiChannelsComponent.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Source/ui/MidiChannelsSelectComponent.h
        ${CMAKE_CURRENT_SOURCE_DIR}/Source/ui/MidiChannelsSelectComponent.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Source/ui/LevelIndicator.h
        ${CMAKE_CURRENT_SOURCE_DIR}/Source/ui/LevelIndicator.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Source/ui/OverlayComponent.h
        ${CMAKE_CURRENT_SOURCE_DIR}/Source/ui/OverlayComponent.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Source/ui/ParameterSlider.h
        ${CMAKE_CURRENT_SOURCE_DIR}/Source/ui/ParameterSlider.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Source/ui/SequencerView.h
        ${CMAKE_CURRENT_SOURCE_DIR}/Source/ui/SequencerView.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Source/ui/StopButton.h
        ${CMAKE_CURRENT_SOURCE_DIR}/Source/ui/StopButton.cpp
)

source_group(Source\\aeolus
    FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/Source/aeolus/addsynth.h
        ${CMAKE_CURRENT_SOURCE_DIR}/Source/aeolus/addsynth.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Source/aeolus/audioparam.h
        ${CMAKE_CURRENT_SOURCE_DIR}/Source/aeolus/audioparam.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Source/aeolus/division.h
        ${CMAKE_CURRENT_SOURCE_DIR}/Source/aeolus/division.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Source/aeolus/engine.h
        ${CMAKE_CURRENT_SOURCE_DIR}/Source/aeolus/engine.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Source/aeolus/globals.h
        ${CMAKE_CURRENT_SOURCE_DIR}/Source/aeolus/globals.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Source/aeolus/levelmeter.h
        ${CMAKE_CURRENT_SOURCE_DIR}/Source/aeolus/levelmeter.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Source/aeolus/list.h
        ${CMAKE_CURRENT_SOURCE_DIR}/Source/aeolus/memory.h
        ${CMAKE_CURRENT_SOURCE_DIR}/Source/aeolus/rankwave.h
        ${CMAKE_CURRENT_SOURCE_DIR}/Source/aeolus/rankwave.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Source/aeolus/ringbuffer.h
        ${CMAKE_CURRENT_SOURCE_DIR}/Source/aeolus/scale.h
        ${CMAKE_CURRENT_SOURCE_DIR}/Source/aeolus/scale.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Source/aeolus/sema.h
        ${CMAKE_CURRENT_SOURCE_DIR}/Source/aeolus/sema.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Source/aeolus/sequencer.h
        ${CMAKE_CURRENT_SOURCE_DIR}/Source/aeolus/sequencer.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Source/aeolus/simd.h
        ${CMAKE_CURRENT_SOURCE_DIR}/Source/aeolus/simd.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Source/aeolus/stop.h
        ${CMAKE_CURRENT_SOURCE_DIR}/Source/aeolus/stop.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Source/aeolus/voice.h
        ${CMAKE_CURRENT_SOURCE_DIR}/Source/aeolus/voice.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Source/aeolus/worker.h
        ${CMAKE_CURRENT_SOURCE_DIR}/Source/aeolus/worker.cpp
)

source_group(Source\\aeolus\\dsp
    FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/Source/aeolus/dsp/adsrenv.h
        ${CMAKE_CURRENT_SOURCE_DIR}/Source/aeolus/dsp/adsrenv.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Source/aeolus/dsp/chiff.h
        ${CMAKE_CURRENT_SOURCE_DIR}/Source/aeolus/dsp/chiff.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Source/aeolus/dsp/convolve.h
        ${CMAKE_CURRENT_SOURCE_DIR}/Source/aeolus/dsp/convolver.h
        ${CMAKE_CURRENT_SOURCE_DIR}/Source/aeolus/dsp/convolver.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Source/aeolus/dsp/delay.h
        ${CMAKE_CURRENT_SOURCE_DIR}/Source/aeolus/dsp/delay.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Source/aeolus/dsp/fft.h
        ${CMAKE_CURRENT_SOURCE_DIR}/Source/aeolus/dsp/fft.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Source/aeolus/dsp/filter.h
        ${CMAKE_CURRENT_SOURCE_DIR}/Source/aeolus/dsp/filter.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Source/aeolus/dsp/interpolator.h
        ${CMAKE_CURRENT_SOURCE_DIR}/Source/aeolus/dsp/interpolator.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Source/aeolus/dsp/limiter.h
        ${CMAKE_CURRENT_SOURCE_DIR}/Source/aeolus/dsp/limiter.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Source/aeolus/dsp/spatial.h
        ${CMAKE_CURRENT_SOURCE_DIR}/Source/aeolus/dsp/spatial.cpp
)

file(GLOB_RECURSE res "${CMAKE_CURRENT_SOURCE_DIR}/Resources/*.*")

juce_add_binary_data(${TARGET}_res SOURCES ${res})
target_link_libraries(${TARGET} PUBLIC ${TARGET}_res)

target_sources(${TARGET} PRIVATE ${src})
if(AEOLUS_ENABLE_IPO AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    # Keep GCC's internal LTO linker parallelism modest on small SBCs as well.
    target_link_options(${TARGET} PUBLIC -flto=2)
endif()

if(NOT AEOLUS_BOUNDED_VOICE_CLEAR)
    target_compile_definitions(${TARGET} PRIVATE AEOLUS_FULL_VOICE_CLEAR=1)
endif()

if(AEOLUS_ENABLE_SPATIAL_NEON)
    target_compile_definitions(${TARGET} PUBLIC AEOLUS_SPATIAL_NEON=1)
endif()
if(NOT AEOLUS_ENABLE_NEON)
    target_compile_definitions(${TARGET} PUBLIC AEOLUS_DISABLE_NEON=1)
endif()
# Keep separate multiply/add rounding; never contract these kernels to FMA.
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    set_source_files_properties(Source/aeolus/simd_neon.cpp Source/aeolus/dsp/spatial_neon.cpp PROPERTIES COMPILE_OPTIONS "-ffp-contract=off")
endif()

target_compile_definitions(${TARGET}
    PUBLIC
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_VST3_CAN_REPLACE_VST2=0
)

if(WITH_ASIO)
    target_include_directories(${TARGET}
        PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/asiosdk_2.3.3_2019-06-14/common"
    )

    target_compile_definitions(${TARGET} PUBLIC JUCE_ASIO=1)
else()
    target_compile_definitions(${TARGET} PUBLIC JUCE_ASIO=0)
endif()

if(WITH_MULTIBUS_OUTPUT)
    target_compile_definitions(${TARGET} PUBLIC AEOLUS_MULTIBUS_OUTPUT=1)
else()
    target_compile_definitions(${TARGET} PUBLIC AEOLUS_MULTIBUS_OUTPUT=0)
endif()

if(APPLE)
    target_compile_definitions(${TARGET} PUBLIC JUCE_AU=1)
endif()

# Opt-in integration tests use a separate profile; production builds have no
# test-directory override. The tests link the same shared code as Standalone.
# The production Linux companion is unprivileged and uses Python's pidfd APIs.
# Keep it adjacent to Standalone for relocatable local/USB bundles.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    find_program(AEOLUS_SAFETY_PYTHON NAMES python3 REQUIRED)
    target_compile_definitions(${TARGET} PRIVATE AEOLUS_SAFETY_PYTHON="python3")
    add_custom_target(aeolus_safety_helpers
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:${TARGET}_Standalone>/helpers"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/aeolus_safety.py"
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/aeolus_panic_client.py"
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/vim4_test_watchdog.py"
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/vim4_panic_panel.py"
            "$<TARGET_FILE_DIR:${TARGET}_Standalone>/helpers")
    add_dependencies(${TARGET}_Standalone aeolus_safety_helpers)
endif()

option(AEOLUS_BUILD_TESTS "Build audio/MIDI regression tests" OFF)
if(AEOLUS_BUILD_TESTS)
    enable_testing()
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        # JUCE's standalone hardcodes ~/.config and does not consult the XDG
        # environment variable. Generate a test-only entry point; leave the
        # dependency and production standalone unchanged.
        set(test_standalone_source "${CMAKE_CURRENT_BINARY_DIR}/AeolusTestStandalone.cpp")
        file(READ "${CMAKE_CURRENT_SOURCE_DIR}/JUCE/modules/juce_audio_plugin_client/juce_audio_plugin_client_Standalone.cpp" test_standalone)
        string(FIND "${test_standalone}" "options.folderName          = \"~/.config\";" test_settings_offset)
        if(test_settings_offset EQUAL -1)
            message(FATAL_ERROR "JUCE standalone settings code changed; review test preference isolation")
        endif()
        string(REPLACE "options.folderName          = \"~/.config\";"
            "options.folderName = juce::SystemStats::getEnvironmentVariable (\"AEOLUS_TEST_SETTINGS_DIRECTORY\", \"\");\n        if (! juce::File::isAbsolutePath (options.folderName))\n            throw std::runtime_error (\"Test standalone requires an absolute AEOLUS_TEST_SETTINGS_DIRECTORY\");"
            test_standalone "${test_standalone}")
        file(WRITE "${test_standalone_source}"
            "#undef JUCE_USE_CUSTOM_PLUGIN_STANDALONE_APP\n#define JUCE_USE_CUSTOM_PLUGIN_STANDALONE_APP 0\n#undef JUCE_USE_CUSTOM_PLUGIN_STANDALONE_ENTRYPOINT\n#define JUCE_USE_CUSTOM_PLUGIN_STANDALONE_ENTRYPOINT 0\n#include <stdexcept>\n${test_standalone}")
        target_compile_definitions(${TARGET}_Standalone PRIVATE
            JUCE_USE_CUSTOM_PLUGIN_STANDALONE_APP=1 JUCE_USE_CUSTOM_PLUGIN_STANDALONE_ENTRYPOINT=1)
        target_sources(${TARGET}_Standalone PRIVATE "${test_standalone_source}")
    endif()
    add_executable(aeolus_neon_tests tests/NeonTests.cpp Source/aeolus/simd_neon.cpp)
    target_compile_features(aeolus_neon_tests PRIVATE cxx_std_17)
    target_include_directories(aeolus_neon_tests PRIVATE Source)
    if(NOT AEOLUS_ENABLE_NEON)
        target_compile_definitions(aeolus_neon_tests PRIVATE AEOLUS_DISABLE_NEON=1)
    endif()
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(aeolus_neon_tests PRIVATE -ffp-contract=off)
    endif()
    add_test(NAME neon_arithmetic COMMAND aeolus_neon_tests)
    target_compile_definitions(${TARGET} PRIVATE AEOLUS_TESTING=1 JUCE_MODAL_LOOPS_PERMITTED=1)
    add_executable(aeolus_tests tests/AudioTests.cpp)
    target_link_libraries(aeolus_tests PRIVATE ${TARGET})
    target_include_directories(aeolus_tests PRIVATE $<TARGET_PROPERTY:${TARGET},INCLUDE_DIRECTORIES>)
    target_compile_definitions(aeolus_tests PRIVATE $<TARGET_PROPERTY:${TARGET},COMPILE_DEFINITIONS>)
    add_test(NAME audio_regressions COMMAND aeolus_tests)
    set_tests_properties(audio_regressions PROPERTIES
        ENVIRONMENT "AEOLUS_TEST_DATA_DIRECTORY=${CMAKE_CURRENT_BINARY_DIR}/test-data;UBSAN_OPTIONS=halt_on_error=1"
        TIMEOUT 180)
endif()

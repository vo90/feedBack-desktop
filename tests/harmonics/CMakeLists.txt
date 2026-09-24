cmake_minimum_required(VERSION 3.22)
project(natural_harmonics_test CXX)
set(CMAKE_CXX_STANDARD 20)
set(JUCE_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../JUCE" CACHE PATH "Prepared JUCE source")
add_subdirectory("${JUCE_SOURCE_DIR}" juce_build EXCLUDE_FROM_ALL)
add_executable(natural_harmonics_test test.cpp ../../src/audio/ChordScorer.cpp
    ../../src/audio/NoteVerifier.cpp ../../src/audio/OnsetDetector.cpp)
target_include_directories(natural_harmonics_test PRIVATE ../../src/audio)
target_compile_definitions(natural_harmonics_test PRIVATE JUCE_STANDALONE_APPLICATION=1 JUCE_USE_CURL=0 JUCE_WEB_BROWSER=0)
target_link_libraries(natural_harmonics_test PRIVATE juce::juce_core juce::juce_audio_basics juce::juce_dsp)
enable_testing()
add_test(NAME natural_harmonics COMMAND natural_harmonics_test)
add_executable(whammy_test ../whammy/test.cpp ../../src/audio/ChordScorer.cpp ../../src/audio/NoteVerifier.cpp ../../src/audio/OnsetDetector.cpp)
target_include_directories(whammy_test PRIVATE ../../src/audio)
target_compile_definitions(whammy_test PRIVATE JUCE_STANDALONE_APPLICATION=1 JUCE_USE_CURL=0 JUCE_WEB_BROWSER=0)
target_link_libraries(whammy_test PRIVATE juce::juce_core juce::juce_audio_basics juce::juce_dsp)
add_test(NAME whammy COMMAND whammy_test)

# Optional compilation of the real IPC entry points using already-prepared
# headers. No downloads, installations or writes to a dependency checkout.
set(NAPI_HEADERS "" CACHE PATH "Prepared node-addon-api headers")
set(NODE_HEADERS "" CACHE PATH "Prepared Node/Electron headers")
set(AUDIO_THIRD_PARTY "" CACHE PATH "Prepared audio third_party headers")
if(NAPI_HEADERS AND NODE_HEADERS)
    add_library(harmonic_bridge_compile OBJECT ../../src/audio/SourceChain.cpp
        ../../src/audio/addon/DeviceBindings.cpp ../../src/audio/addon/DetectionBindings.cpp)
    target_include_directories(harmonic_bridge_compile PRIVATE ../../src/audio
        "${JUCE_SOURCE_DIR}/modules" "${NAPI_HEADERS}" "${NODE_HEADERS}"
        "${AUDIO_THIRD_PARTY}" "${AUDIO_THIRD_PARTY}/signalsmith-stretch")
    target_compile_definitions(harmonic_bridge_compile PRIVATE
        JUCE_GLOBAL_MODULE_SETTINGS_INCLUDED=1 JUCE_STANDALONE_APPLICATION=1
        JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_MODAL_LOOPS_PERMITTED=1
        JUCE_PLUGINHOST_VST3=1 SLOPSMITH_NAM_SUPPORT=0 SLOPSMITH_ONNX_SUPPORT=0
        NAPI_VERSION=9 NAPI_DISABLE_CPP_EXCEPTIONS SLOPSMITH_AUDIO_ADDON=1)
endif()

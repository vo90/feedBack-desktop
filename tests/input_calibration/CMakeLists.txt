cmake_minimum_required(VERSION 3.22)
project(input_calibration_tests LANGUAGES C CXX)
# No devices, Node/Electron runtime, downloads or dependency installs. Reuse the
# explicitly supplied pinned JUCE tree; all generated files belong in -B.
if(NOT EXISTS "${JUCE_SOURCE_DIR}/CMakeLists.txt")
    message(FATAL_ERROR "Supply -DJUCE_SOURCE_DIR=<existing pinned JUCE checkout>")
endif()
add_subdirectory("${JUCE_SOURCE_DIR}" "${CMAKE_BINARY_DIR}/juce" EXCLUDE_FROM_ALL)
enable_testing()
set(AUDIO "${CMAKE_CURRENT_SOURCE_DIR}/../../src/audio")
set(SANDBOX "${AUDIO}/Sandbox")
add_executable(source_calibration_test
    source_calibration_test.cpp
    "${AUDIO}/SourceChain.cpp" "${AUDIO}/SignalChain.cpp"
    "${AUDIO}/NoiseGate.cpp" "${AUDIO}/TonePolish.cpp"
    "${AUDIO}/PitchDetector.cpp" "${AUDIO}/MlNoteDetector.cpp"
    "${AUDIO}/ChordScorer.cpp" "${AUDIO}/NoteVerifier.cpp" "${AUDIO}/OnsetDetector.cpp"
    "${SANDBOX}/SandboxedProcessor.cpp" "${SANDBOX}/SandboxFactory_shared.cpp"
    "${SANDBOX}/AudioChannel_shared.cpp" "${SANDBOX}/ControlChannel_shared.cpp"
    "${SANDBOX}/Protocol.cpp" "${SANDBOX}/CrashAttribution.cpp")
if(WIN32)
    target_sources(source_calibration_test PRIVATE
        "${SANDBOX}/SandboxFactory_win.cpp" "${SANDBOX}/AudioChannel_win.cpp"
        "${SANDBOX}/ControlChannel_win.cpp" "${SANDBOX}/SubprocessHandle_win.cpp")
else()
    target_sources(source_calibration_test PRIVATE
        "${SANDBOX}/SandboxFactory_posix.cpp" "${SANDBOX}/AudioChannel_posix.cpp"
        "${SANDBOX}/ControlChannel_posix.cpp" "${SANDBOX}/SubprocessHandle_posix.cpp")
endif()
target_compile_features(source_calibration_test PRIVATE cxx_std_20)
target_compile_definitions(source_calibration_test PRIVATE
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_STANDALONE_APPLICATION=1
    SLOPSMITH_NAM_SUPPORT=0 SLOPSMITH_ONNX_SUPPORT=0)
target_link_libraries(source_calibration_test PRIVATE
    juce::juce_audio_processors juce::juce_audio_devices juce::juce_audio_formats juce::juce_dsp)
add_test(NAME source_calibration COMMAND source_calibration_test)
add_executable(input_calibration_gain_test ../engine_units/input_calibration_gain_test.cpp)
target_compile_features(input_calibration_gain_test PRIVATE cxx_std_17)
add_test(NAME input_calibration_gain COMMAND input_calibration_gain_test)

# engine_units — home for the per-unit tests of the audio-engine decomposition
# (docs/audio-engine-tlc.md Part IV §4/0.c). JUCE-free targets only: units are
# extracted so they can be tested against a state struct + fake ring, without a
# real device. One executable per unit keeps failures attributable.
add_executable(gain_sanitize_test gain_sanitize_test.cpp)
target_compile_features(gain_sanitize_test PRIVATE cxx_std_17)
add_test(NAME gain_sanitize COMMAND gain_sanitize_test)

# PackedStereoRing uses std::bit_cast (C++20) and std::thread.
add_executable(packed_stereo_ring_test packed_stereo_ring_test.cpp)
target_compile_features(packed_stereo_ring_test PRIVATE cxx_std_20)
find_package(Threads REQUIRED)
target_link_libraries(packed_stereo_ring_test PRIVATE Threads::Threads)
add_test(NAME packed_stereo_ring COMMAND packed_stereo_ring_test)

add_executable(engine_state_test engine_state_test.cpp)
target_compile_features(engine_state_test PRIVATE cxx_std_17)
add_test(NAME engine_state COMMAND engine_state_test)

add_executable(renderer_bus_test renderer_bus_test.cpp)
target_compile_features(renderer_bus_test PRIVATE cxx_std_20)
add_test(NAME renderer_bus COMMAND renderer_bus_test)

add_executable(rate_match_test rate_match_test.cpp)
target_compile_features(rate_match_test PRIVATE cxx_std_17)
add_test(NAME rate_match COMMAND rate_match_test)

add_executable(backing_clock_snapshot_test backing_clock_snapshot_test.cpp)
target_compile_features(backing_clock_snapshot_test PRIVATE cxx_std_17)
target_link_libraries(backing_clock_snapshot_test PRIVATE Threads::Threads)
add_test(NAME backing_clock_snapshot COMMAND backing_clock_snapshot_test)

add_executable(backing_sources_test backing_sources_test.cpp)
target_compile_features(backing_sources_test PRIVATE cxx_std_17)
target_link_libraries(backing_sources_test PRIVATE Threads::Threads)
add_test(NAME backing_sources COMMAND backing_sources_test)

# Available in the full addon build; the standalone pure-helper harness has no JUCE.
if(TARGET juce::juce_audio_formats)
    add_executable(prepared_backing_sources_test prepared_backing_sources_test.cpp
        ../../src/audio/engine/PreparedBackingSources.cpp)
    target_compile_features(prepared_backing_sources_test PRIVATE cxx_std_20)
    target_compile_definitions(prepared_backing_sources_test PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
    target_link_libraries(prepared_backing_sources_test PRIVATE
        juce::juce_audio_formats juce::juce_audio_basics)
    add_test(NAME prepared_backing_sources COMMAND prepared_backing_sources_test)
    add_executable(device_probe_test device_probe_test.cpp
        ../../src/audio/engine/DeviceSetup.cpp)
    target_compile_features(device_probe_test PRIVATE cxx_std_20)
    target_compile_definitions(device_probe_test PRIVATE
        JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 SLOPSMITH_NAM_SUPPORT=0 SLOPSMITH_ONNX_SUPPORT=0)
    target_link_libraries(device_probe_test PRIVATE juce::juce_audio_utils juce::juce_dsp)
    add_test(NAME device_probe COMMAND device_probe_test)

    add_executable(backing_player_clock_test backing_player_clock_test.cpp
        ../../src/audio/engine/BackingPlayer.cpp)
    target_compile_features(backing_player_clock_test PRIVATE cxx_std_20)
    target_include_directories(backing_player_clock_test PRIVATE
        ../../src/audio/third_party/signalsmith-stretch ../../src/audio/third_party)
    target_compile_definitions(backing_player_clock_test PRIVATE
        JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
    target_link_libraries(backing_player_clock_test PRIVATE
        juce::juce_audio_devices juce::juce_audio_formats)
    add_test(NAME backing_player_clock COMMAND backing_player_clock_test)
endif()

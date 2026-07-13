# engine_units — home for the per-unit tests of the audio-engine decomposition
# (docs/audio-engine-tlc.md Part IV §4/0.c). JUCE-free targets only: units are
# extracted so they can be tested against a state struct + fake ring, without a
# real device. One executable per unit keeps failures attributable.
add_executable(gain_sanitize_test gain_sanitize_test.cpp)
target_compile_features(gain_sanitize_test PRIVATE cxx_std_17)
add_test(NAME gain_sanitize COMMAND gain_sanitize_test)

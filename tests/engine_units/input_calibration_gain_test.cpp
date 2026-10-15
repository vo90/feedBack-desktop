#include "../../src/audio/engine/InputCalibrationGain.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <limits>
#include <iostream>

static bool near(float a, float b) { return std::abs(a - b) < 0.0001f; }

int main()
{
    slopsmith::InputCalibrationGain gain;
    gain.prepare(48000);
    assert(gain.getGain() == 1.0f && !gain.beginBlock());
    for (double invalid : {0.0, -1.0, 0.099999999, 5.00000001,
                           std::numeric_limits<double>::quiet_NaN(),
                           std::numeric_limits<double>::infinity()})
    {
        assert(!gain.setGain(invalid));
        assert(!gain.beginBlock()); // invalid writes must not activate profiles
    }
    assert(gain.setGain(3.0) && gain.getGain() == 3.0f);
    assert(gain.beginBlock());
    float previous = 1.0f;
    for (int i = 0; i < 480; ++i)
    {
        const float current = gain.nextGain();
        assert(current >= previous && current - previous < 0.0043f);
        previous = current;
    }
    assert(gain.nextGain() == 3.0f); // exactly reaches target after 10 ms
    assert(!gain.setGain(8.0) && gain.getGain() == 3.0f);
    assert(gain.setGain(0.1)); gain.beginBlock();
    for (int i = 0; i < 240; ++i) gain.nextGain();
    assert(near(gain.nextGain(), 1.5439583f));
    // Retarget from the current ramp value; do not jump back to the old target.
    assert(gain.setGain(5.0)); gain.beginBlock();
    const float first = gain.nextGain();
    assert(first > 1.54f && first < 1.56f);
    for (int i = 1; i < 480; ++i) gain.nextGain();
    assert(gain.nextGain() == 5.0f);
    gain.prepare(96000); // audio device restart retains explicit activation + target
    assert(gain.getGain() == 5.0f && gain.beginBlock() && gain.nextGain() == 5.0f);
    assert(gain.setGain(1.0)); gain.beginBlock();
    for (int i = 0; i < 959; ++i) gain.nextGain();
    assert(gain.nextGain() == 1.0f);
    std::cout << "Input calibration smoothing and validation passed\n";
}

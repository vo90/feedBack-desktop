// Pins the Phase 0.b compat decision (docs/audio-engine-tlc.md §4): native
// gain clamp bounds are 0..32 — matching the audio-effects executor's JS-side
// clampGain so a legit high rig gain is never under-shot — with NaN/Inf
// rejected universally; stream/renderer-bus gains keep the tighter 0..8.

#include "../../src/audio/GainSanitize.h"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <limits>

int main()
{
    using slopsmith::sanitizeMasterGain;
    using slopsmith::sanitizeStreamGain;

    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();

    struct Case { float in, master, stream; };
    const Case cases[] = {
        { 0.0f,   0.0f,  0.0f },
        { 1.0f,   1.0f,  1.0f },
        { 8.0f,   8.0f,  8.0f },
        { 8.5f,   8.5f,  8.0f },   // executor range beyond the stream clamp
        { 32.0f, 32.0f,  8.0f },   // upper compat bound must not under-shoot
        { 33.0f, 32.0f,  8.0f },
        { 1e9f,  32.0f,  8.0f },
        { -1.0f,  0.0f,  0.0f },
        { -0.0f,  0.0f,  0.0f },
        { nan,    0.0f,  0.0f },   // non-finite → silence, never a poisoned mix
        { inf,    0.0f,  0.0f },
        { -inf,   0.0f,  0.0f },
    };

    for (const auto& c : cases)
    {
        const float m = sanitizeMasterGain(c.in);
        const float s = sanitizeStreamGain(c.in);
        assert(std::isfinite(m) && std::isfinite(s));
        assert(m == c.master);
        assert(s == c.stream);
    }

    std::puts("gain_sanitize: all cases passed");
    return 0;
}

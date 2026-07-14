#pragma once

// Gain-argument containment (audio-engine TLC, deep-read §2).
//
// N-API's Number coercion lets NaN/Infinity from JS reach the engine's gain
// atomics raw — a NaN master gain multiplies the whole device output to NaN
// (buffer.applyGain) and poisons the peak meters, and nothing downstream
// scrubs it (the per-source NaN scrub runs before the master gain). Clamping
// at the engine setters is the single choke point that fixes every caller:
// audio:setGain, the source-indexed API, and the audio-effects executor.
//
// Bounds: 0..32 for input/chain/output/backing — matching the executor's
// JS-side clampGain so a legit high rig gain is never under-shot (compat pin,
// docs/audio-engine-tlc.md Phase 0.b). The stream/renderer-bus gains keep
// their tighter historical 0..8 (previously sanitizeStreamGain).
//
// JUCE-free on purpose, like AudioSanitize.h, so tests/engine_units can test
// it without a device.

#include <cmath>

namespace slopsmith {

// Non-finite → 0 (silence beats a poisoned mix); otherwise clamp to [0, max].
inline float sanitizeGain(float g, float maxGain) noexcept
{
    if (!std::isfinite(g)) return 0.0f;
    if (g < 0.0f) return 0.0f;
    if (g > maxGain) return maxGain;
    return g;
}

inline float sanitizeMasterGain(float g) noexcept { return sanitizeGain(g, 32.0f); }
inline float sanitizeStreamGain(float g) noexcept { return sanitizeGain(g, 8.0f); }

} // namespace slopsmith

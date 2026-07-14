#pragma once

// NapiHelpers — typed N-API argument extractors (TLC plan phase 6 / §3.2).
// Generalizes the getValidatedSource pattern so argument validation is
// structural, not per-binding: Int32Value() silently coerces NaN/Infinity
// into a valid index (NaN → 0), which let a malformed slot id hit a REAL
// slot (deep-read §2). Every extractor returns nullopt for a missing /
// non-Number / non-finite / out-of-range argument, and the binding no-ops —
// fail-soft, matching the addon's NAPI_DISABLE_CPP_EXCEPTIONS posture.
//
// New bindings should have no raw As<Napi::Number>() path to copy.

#include <napi.h>

#include <cmath>
#include <limits>
#include <optional>

namespace slopsmith::addon {

// Finite integer in [minV, maxV]. The 4096 default ceiling keeps the cast
// well-defined for index-shaped args (channel/branch indices and the like).
inline std::optional<int> argInt(const Napi::CallbackInfo& info, size_t i,
                                 int minV = 0, int maxV = 4096)
{
    if (i >= info.Length() || ! info[i].IsNumber()) return std::nullopt;
    const double raw = info[i].As<Napi::Number>().DoubleValue();
    if (! std::isfinite(raw) || raw != std::floor(raw)) return std::nullopt;
    if (raw < (double) minV || raw > (double) maxV) return std::nullopt;
    return (int) raw;
}

// Slot / source / param-index ids: finite non-negative integers.
//
// NOT bounded by argInt's 4096 index ceiling. A slot id is a monotonic HANDLE
// from SignalChain::nextSlotId, which increments on every addProcessor and is
// never reset by clear() — a long session (each song load and mid-song tone
// switch rebuilds a chainful of slots) walks past 4096, and a ceiling here
// would then make every guarded binding — setBypass, setParameter, remove/
// moveProcessor, open/closePluginEditor — silently no-op for the rest of the
// run. Ids that don't name a live slot are rejected by SignalChain's own
// findSlotIndex; the job here is only to keep the NaN/Inf/fractional class out
// (Int32Value() coerces NaN to 0, i.e. a real slot).
inline std::optional<int> argSlotId(const Napi::CallbackInfo& info, size_t i)
{
    return argInt(info, i, 0, std::numeric_limits<int>::max());
}

// Finite float (parameter values, gains, pans). Range clamping stays with
// the engine-side sanitizers (GainSanitize.h) — this only rejects the
// NaN/Inf class that coercion would otherwise let through.
inline std::optional<float> argFiniteFloat(const Napi::CallbackInfo& info, size_t i)
{
    if (i >= info.Length() || ! info[i].IsNumber()) return std::nullopt;
    const double raw = info[i].As<Napi::Number>().DoubleValue();
    if (! std::isfinite(raw)) return std::nullopt;
    return (float) raw;
}

inline std::optional<bool> argBool(const Napi::CallbackInfo& info, size_t i)
{
    if (i >= info.Length() || ! info[i].IsBoolean()) return std::nullopt;
    return info[i].As<Napi::Boolean>().Value();
}

// MIDI channel: JUCE expects 1..16 and asserts otherwise.
inline std::optional<int> argMidiChannel(const Napi::CallbackInfo& info, size_t i)
{
    return argInt(info, i, 1, 16);
}
// MIDI data byte (program / controller / value): 0..127.
inline std::optional<int> argMidiByte(const Napi::CallbackInfo& info, size_t i)
{
    return argInt(info, i, 0, 127);
}

} // namespace slopsmith::addon

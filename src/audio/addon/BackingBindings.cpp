// Backing track bindings - moved verbatim from NodeAddon.cpp (TLC phase 7b
// binding split). Registered by NodeAddon's export table via Bindings.h.

#include "Bindings.h"

#include "AddonContext.h"
#include "NapiHelpers.h"
#include "ChainOps.h"
#include "../AudioEngine.h"
#include "../VSTHost.h"
#include "../VSTTrace.h"

#include <cmath>
#include <cstdio>
#include <limits>
#include <string>

namespace slopsmith::addon {

// ── Backing Track ─────────────────────────────────────────────────────────────

Napi::Value LoadBackingTrack(const Napi::CallbackInfo& info)
{
    auto env = info.Env();
    auto liveEngine = snapshotEngine();
    if (!liveEngine || info.Length() < 1) return Napi::Boolean::New(env, false);

    auto path = info[0].As<Napi::String>().Utf8Value();
    bool result = liveEngine->loadBackingTrack(juce::File(juce::String(path)));
    return Napi::Boolean::New(env, result);
}

Napi::Value StartBacking(const Napi::CallbackInfo& info)
{
    if (auto liveEngine = snapshotEngine()) liveEngine->startBacking();
    return info.Env().Undefined();
}

Napi::Value StopBacking(const Napi::CallbackInfo& info)
{
    if (auto liveEngine = snapshotEngine()) liveEngine->stopBacking();
    return info.Env().Undefined();
}

Napi::Value SeekBacking(const Napi::CallbackInfo& info)
{
    auto liveEngine = snapshotEngine();
    if (liveEngine && info.Length() > 0)
        liveEngine->setBackingPosition(info[0].As<Napi::Number>().DoubleValue());
    return info.Env().Undefined();
}

Napi::Value GetBackingPosition(const Napi::CallbackInfo& info)
{
    auto liveEngine = snapshotEngine();
    double pos = liveEngine ? liveEngine->getBackingPosition() : 0.0;
    return Napi::Number::New(info.Env(), pos);
}

Napi::Value GetBackingDuration(const Napi::CallbackInfo& info)
{
    auto liveEngine = snapshotEngine();
    double dur = liveEngine ? liveEngine->getBackingDuration() : 0.0;
    return Napi::Number::New(info.Env(), dur);
}

Napi::Value GetBackingSnapshot(const Napi::CallbackInfo& info)
{
    const auto env = info.Env();
    const auto liveEngine = snapshotEngine();
    const auto s = liveEngine ? liveEngine->getBackingSnapshot() : BackingClockSample{};
    auto result = Napi::Object::New(env);
    result.Set("version", 1);
    result.Set("valid", s.valid);
    result.Set("position", s.position);
    result.Set("ageMs", juce::jmax(0.0, juce::Time::getMillisecondCounterHiRes() - s.sampledAtMs));
    result.Set("sequence", static_cast<double>(s.sequence));
    result.Set("generation", static_cast<double>(s.generation));
    result.Set("rate", s.rate);
    result.Set("playing", s.playing);
    result.Set("ended", s.ended);
    return result;
}

Napi::Value IsBackingPlaying(const Napi::CallbackInfo& info)
{
    auto liveEngine = snapshotEngine();
    bool playing = liveEngine ? liveEngine->isBackingPlaying() : false;
    return Napi::Boolean::New(info.Env(), playing);
}

Napi::Value SetBackingSpeed(const Napi::CallbackInfo& info)
{
    auto env = info.Env();
    if (info.Length() < 1 || !info[0].IsNumber())
    {
        Napi::TypeError::New(env, "setBackingSpeed(speed) requires a number")
            .ThrowAsJavaScriptException();
        return env.Undefined();
    }
    // (Was a bare `engine` dereference — the one binding that dodged the
    // file's own snapshot rule; surfaced by the phase-6 move.)
    if (auto liveEngine = snapshotEngine())
        liveEngine->setBackingSpeed(info[0].As<Napi::Number>().DoubleValue());
    return env.Undefined();
}


} // namespace slopsmith::addon

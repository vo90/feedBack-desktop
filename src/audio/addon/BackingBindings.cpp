// Backing track bindings - moved verbatim from NodeAddon.cpp (TLC phase 7b
// binding split). Registered by NodeAddon's export table via Bindings.h.

#include "Bindings.h"

#include "AddonContext.h"
#include "NapiHelpers.h"
#include "ChainOps.h"
#include "../AudioEngine.h"
#include "../VSTHost.h"
#include "../VSTTrace.h"
#include "../engine/BackingTiming.h"

#include <cmath>
#include <cstdio>
#include <limits>
#include <string>

namespace slopsmith::addon {

// ── Backing Track ─────────────────────────────────────────────────────────────

namespace {
class BackingWaitWorker final : public Napi::AsyncWorker {
public:
    BackingWaitWorker(Napi::Env env, std::shared_ptr<AudioEngine> engine, std::uint64_t id)
        : AsyncWorker(env), deferred(Napi::Promise::Deferred::New(env)), live(std::move(engine)),
          request(id), generation(currentEngineGeneration()) {}
    Napi::Promise promise() const { return deferred.Promise(); }
    void Execute() override {
        result = live->waitForBackingRequest(request);
        // If shutdown removed the last global owner, retire on this worker.
        live.reset();
    }
    void OnOK() override { deferred.Resolve(Napi::Boolean::New(Env(), result && generation == currentEngineGeneration())); }
    void OnError(const Napi::Error&) override { deferred.Resolve(Napi::Boolean::New(Env(), false)); }
private:
    Napi::Promise::Deferred deferred;
    std::shared_ptr<AudioEngine> live;
    std::uint64_t request, generation;
    bool result = false;
};
Napi::Value waitForBacking(Napi::Env env, const std::shared_ptr<AudioEngine>& engine, std::uint64_t id) {
    if (!engine || !id) return Napi::Boolean::New(env, false);
    auto* worker = new BackingWaitWorker(env, engine, id);
    auto promise = worker->promise(); worker->Queue(); return promise;
}
bool readGains(const Napi::Value& value, std::vector<float>& result) {
    if (!value.IsArray()) return false;
    auto array = value.As<Napi::Array>();
    if (array.Length() == 0 || array.Length() > BackingSourceQueue::maxSources) return false;
    for (std::uint32_t i = 0; i < array.Length(); ++i) {
        auto item = array.Get(i);
        if (!item.IsNumber()) return false;
        const auto number = item.As<Napi::Number>().DoubleValue();
        if (!std::isfinite(number) || number < 0 || number > 2) return false;
        result.push_back(static_cast<float>(number));
    }
    return true;
}
}

Napi::Value LoadBackingTrack(const Napi::CallbackInfo& info)
{
    auto env = info.Env();
    auto liveEngine = snapshotEngine();
    if (!liveEngine || info.Length() < 1 || !info[0].IsString()
        || (info.Length() > 1 && !info[1].IsBoolean())) return Napi::Boolean::New(env, false);

    auto path = info[0].As<Napi::String>().Utf8Value();
    if (!juce::File::isAbsolutePath(path)) return Napi::Boolean::New(env, false);
    return waitForBacking(env, liveEngine, liveEngine->beginBackingSession({juce::File(juce::String(path))}, {1}, false,
        info.Length() < 2 || info[1].As<Napi::Boolean>().Value()));
}

Napi::Value LoadBackingSession(const Napi::CallbackInfo& info) {
    const auto env = info.Env(); const auto live = snapshotEngine();
    std::vector<float> gains;
    if (!live || info.Length() < 2 || !info[0].IsArray() || !readGains(info[1], gains)
        || (info.Length() > 2 && !info[2].IsBoolean()))
        return Napi::Boolean::New(env, false);
    const auto array = info[0].As<Napi::Array>();
    if (array.Length() != gains.size()) return Napi::Boolean::New(env, false);
    std::vector<juce::File> files;
    for (std::uint32_t i = 0; i < array.Length(); ++i) {
        const auto item = array.Get(i);
        if (!item.IsString()) return Napi::Boolean::New(env, false);
        const auto path = juce::String(item.As<Napi::String>().Utf8Value());
        if (!juce::File::isAbsolutePath(path)) return Napi::Boolean::New(env, false);
        files.emplace_back(path);
    }
    return waitForBacking(env, live, live->beginBackingSession(files, gains, info.Length() > 2 && info[2].As<Napi::Boolean>().Value()));
}

Napi::Value SetBackingSourceGains(const Napi::CallbackInfo& info) {
    const auto live = snapshotEngine(); std::vector<float> gains;
    return Napi::Boolean::New(info.Env(), live && info.Length() == 1 && readGains(info[0], gains)
        && live->setBackingSourceGains(gains));
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
    if (!liveEngine || info.Length() != 1 || !info[0].IsNumber()) return Napi::Boolean::New(info.Env(), false);
    return waitForBacking(info.Env(), liveEngine, liveEngine->beginBackingSeek(info[0].As<Napi::Number>().DoubleValue()));
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

Napi::Value GetBackingAnalysis(const Napi::CallbackInfo& info) {
    const auto live = snapshotEngine();
    const auto snapshot = live ? live->getBackingAnalysis() : BackingAnalysis::Snapshot{};
    auto samples = Napi::Float32Array::New(info.Env(), BackingAnalysis::capacity);
    const float gain = live ? live->getBackingVolume() * live->getOutputGain() : 0;
    for (unsigned i = 0; i < BackingAnalysis::capacity; ++i) samples[i] = snapshot.samples[i] * gain;
    return samples;
}

Napi::Value GetBackingSnapshot(const Napi::CallbackInfo& info)
{
    const auto env = info.Env();
    const auto liveEngine = snapshotEngine();
    const auto s = liveEngine ? liveEngine->getBackingSnapshot() : BackingClockSample{};
    const double now = juce::Time::getMillisecondCounterHiRes();
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
    result.Set("failed", s.failed);
    auto render = Napi::Object::New(env);
    render.Set("version", 1);
    render.Set("valid", s.valid && s.render.valid);
    render.Set("routeGeneration", static_cast<double>(s.render.routeGeneration));
    render.Set("startedAgeMs", juce::jmax(0.0, juce::Time::getMillisecondCounterHiRes() - s.render.startedAtMs));
    render.Set("sourcePositionAfterRender", s.render.sourcePositionAfterRender);
    render.Set("sampleRate", s.render.sampleRate);
    render.Set("frames", s.render.frames);
    // The driver report already includes backend-specific buffering. Do not
    // blindly add another callback period or any input/monitor-ring estimate.
    render.Set("reportedOutputLatencyFrames", s.render.outputLatencyFrames);
    render.Set("stretchInputLatencyFrames", s.render.stretchInputLatencyFrames);
    render.Set("stretchOutputLatencyFrames", s.render.stretchOutputLatencyFrames);
    render.Set("timestampMeaning", "output-callback-entry");
    result.Set("renderTiming", render);
    auto presentation = Napi::Object::New(env);
    presentation.Set("version", 2);
    presentation.Set("latencyKnown", s.render.outputLatencyFrames >= 0);
    double position = s.position;
    if (s.valid && s.render.valid && s.render.sampleRate > 0) {
        const BackingPresentationAnchor anchor { s.render.firstFramePosition, s.render.startedAtMs,
            std::max(0, s.render.outputLatencyFrames) * 1000.0 / s.render.sampleRate,
            s.rate, s.generation, s.render.routeGeneration };
        if (const auto projected = backingSongTimeAt(anchor, now, 0, s.generation, s.render.routeGeneration))
            position = s.ended ? *projected : std::min(s.position, *projected);
    }
    presentation.Set("position", position);
    presentation.Set("ageMs", 0);
    presentation.Set("playing", s.render.valid && (s.playing || position < s.position));
    presentation.Set("routeGeneration", static_cast<double>(s.render.routeGeneration));
    result.Set("presentation", presentation);
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
        return waitForBacking(env, liveEngine, liveEngine->beginBackingRate(info[0].As<Napi::Number>().DoubleValue()));
    return Napi::Boolean::New(env, false);
}


} // namespace slopsmith::addon

#pragma once

// ChainOps — the native chain-mutation serialization point (TLC plan phase 7
// / §3.3, deep-read §1).
//
// The five chain-mutating async workers (LoadPreset/LoadVST/LoadNAM/LoadIR/
// ReplaceIR) queue on the libuv threadpool with no mutual exclusion, and
// SignalChain locks per-operation only — so two overlapping loadPreset calls
// could interleave clear()/addProcessor() and merge both presets into
// garbage (the documented rig_builder-vs-bundle "~1ms later" race). One
// mutex held across each worker's FULL Execute() — and across the
// synchronous mutators (clearChain / remove / move) — converts that
// corruption into last-writer-wins.
//
// chainGeneration is bumped on every completed mutation and returned in the
// load results (and via getChainGeneration), so JS-side owners (the
// audio-effects executor's stageSlots map) can detect that another writer
// changed the chain under them and re-sync instead of flipping bypass/params
// on the wrong slots.
//
// The full worker bodies migrate into this unit with the phase-7 binding
// split; the serializer lands first so the storm gate flips.

#include <napi.h>

#include <juce_core/juce_core.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>

class AudioEngine;
namespace juce { class AudioProcessor; }

namespace slopsmith::addon {

// Held for the FULL clear+rebuild (or single-slot mutation). Control/worker
// threads only — never the audio thread.
std::mutex& chainMutationMutex();

// Monotonic, bumped AFTER a completed mutation (under the mutex). 0 = never
// mutated.
uint64_t bumpChainGeneration();
uint64_t currentChainGeneration();

// Usage in a mutator:
//   std::lock_guard<std::mutex> chainLock(chainMutationMutex());
//   ... clear/rebuild/add ...
//   const uint64_t gen = bumpChainGeneration();   // still under the lock
//   (return gen in the result object)
//
// ...but ONLY from a libuv worker thread. NOTHING may take this mutex with a
// blocking lock on the N-API/JS thread: LoadPresetWorker holds it across the
// full clear+rebuild, which includes an unbounded in-process plugin init
// (loadVstSandboxAware's done->wait() has no timeout — a slow first-run plugin
// is allowed to take as long as it needs). A blocking lock on the JS thread
// would therefore freeze the whole Electron main process — every IPC channel
// with it — for the duration of a plugin load, and on macOS (where the N-API
// thread IS the JUCE message thread — see AddonContext's startJuceMessageThread)
// it would deadlock the very pump that load is waiting on. The message-thread
// call sites in EditorWindows use try_to_lock for exactly this reason; the
// synchronous chain mutators go through queueChainMutation instead.
//
// Run `mutate` on a libuv worker under chainMutationMutex(), bump the chain
// generation, and resolve the returned promise with true (false if the engine
// went away). When `releasesRebuildBarrier`, the worker calls endChainRebuild()
// on every exit path — the caller must have armed it with beginChainRebuild()
// before tearing editors down.
Napi::Value queueChainMutation(Napi::Env env,
                               std::function<void(AudioEngine&)> mutate,
                               bool releasesRebuildBarrier = false);

// ── Rebuild barrier (editor-open gate) ────────────────────────────────────
// A chain clear/rebuild is a two-step dance: editors are torn down on the
// message thread FIRST, then the mutation runs (synchronously for ClearChain,
// on a queued AsyncWorker for LoadPreset). Between those steps the mutation
// mutex is NOT yet held, so an editor opened in that window would point at a
// processor the imminent clear is about to free (#56). Callers bracket the
// whole teardown+mutation with begin/end; OpenPluginEditor refuses to open
// while any rebuild is pending. Counter (not bool): overlapping LoadPreset +
// ClearChain must not un-gate each other early.
void beginChainRebuild();
void endChainRebuild();
bool isChainRebuildPending();

// ── Shared load helpers (used by the workers here and SetSlotState) ──────
// Decode a state blob in EITHER base64 flavour (JUCE-proprietary first,
// standard RFC-4648 fallback when `allowStandard` — IR/NAM slots only).
bool decodeStateBlob(const juce::String& s, juce::MemoryBlock& mb, bool allowStandard);
double loadSafeSampleRate(const AudioEngine& eng);
int loadSafeBlockSize(const AudioEngine& eng);
// Load a VST3 through the out-of-process sandbox when shouldSandbox() says
// so, else in-process via the async message-pumping path. See the .cpp for
// the threading contract.
std::unique_ptr<juce::AudioProcessor> loadVstSandboxAware(
    const juce::String& pluginPath, double sr, int bs,
    juce::String& error, bool& sandboxRequired);

// ── N-API handlers (registered by NodeAddon's export table) ──────────────
Napi::Value LoadVST(const Napi::CallbackInfo& info);
Napi::Value LoadNAMModel(const Napi::CallbackInfo& info);
Napi::Value LoadIR(const Napi::CallbackInfo& info);
Napi::Value ReplaceIR(const Napi::CallbackInfo& info);
Napi::Value LoadPreset(const Napi::CallbackInfo& info);

} // namespace slopsmith::addon

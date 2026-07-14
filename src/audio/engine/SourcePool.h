#pragma once

// SourcePool — the fixed pool of per-input SourceChains plus the add/remove/
// reclaim lifecycle and the per-deviceKey callback-quiescence handshake (TLC
// plan phase 5 / §2.2). Moved verbatim from AudioEngine.
//
// Pool invariants (unchanged):
//  - ALL chains are constructed up front; add/removeSource never reassigns a
//    pointer the audio thread reads — they only flip an atomic `active` flag.
//  - chain 0 is the permanent legacy default input, active from construction.
//  - removal uses the per-deviceKey callbacksInFlight counter handshake;
//    wedged callbacks defer the release (pendingRelease[]) instead of
//    blocking, reclaimed when that key's body is quiescent.
//
// Boundary: device callbacks hold a CallbackGuard for their body and call
// mixForDevice(); control threads use add/remove/list/get and the
// per-deviceKey prepare/release helpers the device hooks need.

#include "EngineState.h"
#include "../SourceChain.h"

#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <vector>

namespace slopsmith {

class SourcePool
{
public:
    static constexpr int kMaxSources = 8;
    // Max ADDITIONAL input devices (beyond the primary); sizes the per-key
    // in-flight counters (key 0 = primary).
    static constexpr int kMaxExtraInputDevices = 3;

    explicit SourcePool(EngineState& engineState)
    {
        // Construct the full pool up front so the audio thread never observes
        // a pointer swap. Each chain reads deviceRunning / currentSampleRate
        // by reference. Chain 0 active from the start; the rest inactive (no
        // threads — NoteVerifier's worker only starts in prepare()).
        for (int i = 0; i < kMaxSources; ++i)
            sources[(size_t) i] = std::make_unique<SourceChain>(
                i, engineState.deviceRunning, engineState.currentSampleRate);
        sources[0]->setActive(true);
    }

    // ── RT side ───────────────────────────────────────────────────────────
    // Publishes that a device callback body is executing for `deviceKey`, so
    // remove()/reclaim know when that key is quiescent. previousInFlight is
    // exposed for the duplicate-registration diagnostic.
    struct CallbackGuard
    {
        CallbackGuard(SourcePool& p, int deviceKey)
            : pool(p), key((size_t) deviceKey),
              previousInFlight(p.callbacksInFlight[key].fetch_add(1, std::memory_order_acq_rel)) {}
        ~CallbackGuard() { pool.callbacksInFlight[key].fetch_sub(1, std::memory_order_acq_rel); }
        SourcePool& pool;
        const size_t key;
        const int previousInFlight;
    };

    // Mix every active source bound to `deviceKey` into `mixBuf` (using the
    // caller-owned `monitorScratch` for the N>1 render so concurrent device
    // threads never share scratch). Returns the active source count.
    int mixForDevice(int deviceKey, const float* const* inputData, int numInputChannels,
                     juce::AudioBuffer<float>& mixBuf, juce::AudioBuffer<float>& monitorScratch,
                     int effectiveOutputChannels, int numSamples) noexcept;

    // ── Control threads ───────────────────────────────────────────────────
    // Activate a pooled chain (device info pre-resolved by the engine facade,
    // which owns the extra-device registry). Returns the slot id or -1.
    int addResolved(int inputChannel, int deviceKey,
                    bool deviceReady, double sr, int bs, double latencyDeltaSec);
    // Deactivate + release (id != 0). Defers the release when the source's
    // device callback stays in-flight past the bounded wait.
    bool remove(int id);

    SourceChain* get(int id)
    {
        if (id < 0 || id >= kMaxSources) return nullptr;
        SourceChain& src = *sources[(size_t) id];
        return (id == 0 || src.isActive()) ? &src : nullptr;
    }
    SourceChain& chain0() { return *sources[0]; }
    const SourceChain& chain0() const { return *sources[0]; }

    struct Info { int id = -1; int inputChannel = -1; int deviceKey = 0; bool active = false; };
    std::vector<Info> list() const;

    // Fan an operation to every pooled chain (active or not) — plain atomic
    // stores on fixed pointers, race-free off the control thread.
    template <typename Fn> void forEach(Fn&& fn)
    {
        for (auto& s : sources)
            if (s) fn(*s);
    }
    template <typename Fn> void forEachActive(Fn&& fn)
    {
        for (auto& s : sources)
            if (s && s->isActive()) fn(*s);
    }

    // Run `fn` on every active source bound to `deviceKey`, under the pool
    // lock — for the extra-device close/unbind paths' bespoke sequences.
    template <typename Fn> void withDeviceSources(int deviceKey, Fn&& fn)
    {
        std::lock_guard<std::mutex> lock(mutex);
        for (auto& s : sources)
            if (s->isActive() && s->getDeviceKey() == deviceKey) fn(*s);
    }

    // Device hooks: prepare / release every active source bound to a key,
    // under the pool lock. Mirrors the per-device halves of the old
    // about-to-start / stopped handlers; release also retries deferred
    // reclamation (that key's callback is now quiescent).
    void prepareDeviceSources(int deviceKey, double sr, int bs, double verifierAutoOffsetSec,
                              bool applyOffset);
    void releaseDeviceSources(int deviceKey, bool resetMeters, bool deactivate);

    // Retry deferred releases whose device is quiescent. Public form takes the
    // pool lock (used by the primary device-stopped path).
    void reclaimPending()
    {
        std::lock_guard<std::mutex> lock(mutex);
        reclaimPendingLocked();
    }

private:
    void reclaimPendingLocked();

    std::array<std::unique_ptr<SourceChain>, kMaxSources> sources;
    // Serialises add/remove (control threads only — never the audio thread,
    // which just reads each slot's atomic `active`).
    std::mutex mutex;
    // How many device-callback bodies are currently executing per deviceKey
    // (0 = primary, 1.. = extras). Incremented/decremented by CallbackGuard at
    // the body's real entry/exit; remove() observing 0 (acquire) proves the
    // source's device is not inside processBlock.
    std::array<std::atomic<int>, kMaxExtraInputDevices + 1> callbacksInFlight{};
    // A remove() that timed out waiting for quiescence parks the release here;
    // reclaimed under `mutex` at the next add/remove and on device-stop paths.
    std::array<bool, kMaxSources> pendingRelease{};
};

} // namespace slopsmith

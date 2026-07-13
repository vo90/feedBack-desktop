// SourcePool implementation — moved verbatim from AudioEngine.cpp (TLC plan
// phase 5 / §2.2). The extra-device resolution that addSource performed
// in-line (reading the InputDeviceSlot registry) stays on the AudioEngine
// facade, which passes the resolved values into addResolved().

#include "SourcePool.h"

#include <chrono>
#include <thread>

namespace slopsmith {

int SourcePool::addResolved(int inputChannel, int deviceKey,
                            bool deviceReady, double sr, int bs, double latencyDeltaSec)
{
    std::lock_guard<std::mutex> lock(mutex);
    reclaimPendingLocked();  // free up any slot whose release was deferred

    // Find a free pooled slot (slot 0 is the permanent default). Skip a slot whose
    // release is still pending — its chain/worker hasn't been torn down yet, so
    // re-preparing it would double-start the verifier thread.
    int slot = -1;
    for (int i = 1; i < kMaxSources; ++i)
        if (! sources[(size_t) i]->isActive() && ! pendingRelease[(size_t) i]) { slot = i; break; }
    if (slot < 0)
        return -1;  // pool full

    SourceChain& src = *sources[(size_t) slot];
    src.setInputChannel(inputChannel);
    src.setDeviceKey(deviceKey);
    // Inherit the bound device's capture-latency correction (0 for the primary).
    src.setVerifierAutoOffset(latencyDeltaSec);
    // Clear any MANUAL offset left on this pooled chain by a previous player — a
    // freshly added source starts with no user fine-tune (the renderer re-applies
    // its own via setSourceVerifierOffset). releaseResources() doesn't touch it.
    src.setVerifierUserOffset(0.0);
    // Likewise clear stale meters so this source doesn't briefly report the previous
    // player's level/peak through getSourceLevels() until fresh audio arrives.
    src.resetInputMeters();

    // Prepare fully BEFORE making it visible to the audio thread, so the first
    // callback that observes active==true sees a ready chain + rings. When audio
    // isn't running yet, the relevant about-to-start hook prepares it later. An
    // EXTRA-device source must be prepared with ITS device's sample rate / block
    // size, not the primary's — the caller resolved those.
    if (deviceReady && sr > 0.0 && bs > 0)
        src.prepare(sr, bs);
    src.setActive(true);  // release-store: now picked up by the audio callback
    return slot;
}

bool SourcePool::remove(int id)
{
    if (id <= 0 || id >= kMaxSources)
        return false;  // 0 is permanent; out-of-range rejected

    std::lock_guard<std::mutex> lock(mutex);
    reclaimPendingLocked();  // opportunistically reclaim earlier deferrals

    SourceChain& src = *sources[(size_t) id];
    if (! src.isActive())
        return false;

    // Hide it from the audio callback first; subsequent blocks snapshot active
    // once and skip it. It is logically removed from here on, regardless of when
    // its resources are reclaimed.
    src.setActive(false);

    // Reclaim now if we can confirm THIS SOURCE's device callback is not executing.
    // Only the callback for the source's own deviceKey can touch it; that counter is
    // decremented at the callback's real exit (release store), so observing 0
    // (acquire) proves it is not inside processBlock right now; any callback that
    // starts afterwards snapshots active and skips this (now-inactive) source — so
    // releasing cannot race the audio thread. Keying on the source's deviceKey (not a
    // global all-callbacks-idle check) is what lets removals reclaim during steady
    // multi-device playback, when callbacks on independent clocks are never all idle
    // at once. Bounded so a wedged device can't hang this thread.
    const size_t dk = (size_t) src.getDeviceKey();
    for (int spins = 0; spins < 200; ++spins)  // ~200 ms cap
    {
        if (callbacksInFlight[dk].load(std::memory_order_acquire) == 0)
        {
            src.releaseResources();  // stops its threads + releases its chain
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    // A callback stayed wedged in-flight past the wait (a >200 ms block would be a
    // catastrophic stall). Do NOT force a release that could race it — DEFER it.
    // The source is already inactive so no future callback touches it; reclaim it
    // later (next add/remove, or a device-stopped hook) when the body is quiet.
    pendingRelease[(size_t) id] = true;
    return true;
}

void SourcePool::reclaimPendingLocked()
{
    // Caller holds `mutex`. A deferred source is inactive (future callbacks skip
    // it); releasing it is safe once the callback for ITS deviceKey is not in a body.
    // We key per-deviceKey (decremented by each callback at its real exit) so a
    // pending release frees as soon as its OWN device is quiescent — not only when
    // every device callback happens to be idle simultaneously (which, on independent
    // clocks during steady multi-device playback, may never occur and would strand
    // the slot until full stop). On the device-stopped path the relevant callback
    // already left its count at 0.
    for (int i = 1; i < kMaxSources; ++i)
    {
        if (! pendingRelease[(size_t) i]) continue;
        const size_t dk = (size_t) sources[(size_t) i]->getDeviceKey();
        if (callbacksInFlight[dk].load(std::memory_order_acquire) != 0)
            continue;  // this source's device is mid-body — try again later
        sources[(size_t) i]->releaseResources();
        pendingRelease[(size_t) i] = false;
    }
}

std::vector<SourcePool::Info> SourcePool::list() const
{
    std::vector<Info> out;
    for (int i = 0; i < kMaxSources; ++i)
    {
        const SourceChain& src = *sources[(size_t) i];
        if (! src.isActive()) continue;
        Info info;
        info.id = src.getId();
        info.inputChannel = src.getInputChannel();
        info.deviceKey = src.getDeviceKey();
        info.active = true;
        out.push_back(info);
    }
    return out;
}

void SourcePool::prepareDeviceSources(int deviceKey, double sr, int bs,
                                      double verifierAutoOffsetSec, bool applyOffset)
{
    std::lock_guard<std::mutex> lock(mutex);
    for (auto& src : sources)
        if (src->isActive() && src->getDeviceKey() == deviceKey)
        {
            src->prepare(sr, bs);
            if (applyOffset) src->setVerifierAutoOffset(verifierAutoOffsetSec);
        }
}

void SourcePool::releaseDeviceSources(int deviceKey, bool resetMeters, bool deactivate)
{
    std::lock_guard<std::mutex> lock(mutex);
    for (auto& src : sources)
        if (src->isActive() && src->getDeviceKey() == deviceKey)
        {
            src->releaseResources();
            // Zero the meters so getSourceLevels() reports silence while the
            // device is gone — otherwise the renderer's per-source silence gate
            // treats a stopped/unplugged input as still hearing audio (the last
            // non-zero level latches). releaseResources() doesn't touch them.
            if (resetMeters) src->resetInputMeters();
            // PERMANENT unbind: the slot will never re-open, so DEACTIVATE its
            // sources too — leaving them "active" would strand pooled slots no
            // callback can ever service (a ghost detector in listSources).
            if (deactivate) src->setActive(false);
        }
    // Retry any remove() cleanup deferred waiting for callbacks to drain. With
    // this device's callback now stopped, callbacksInFlight may finally be 0.
    reclaimPendingLocked();
}

int SourcePool::mixForDevice(int deviceKey, const float* const* inputData, int numInputChannels,
                             juce::AudioBuffer<float>& mixBuf, juce::AudioBuffer<float>& monitorScratch,
                             int effectiveOutputChannels, int numSamples) noexcept
{
    // Snapshot each source's active flag ONCE so the count and the process/mix
    // passes are consistent within this block — a concurrent add/remove flipping
    // a flag between two reads must not change which branch runs. remove() waits
    // for all callback bodies to drain before releasing, so a source snapshotted
    // active here is safe even if deactivated an instant later.
    bool act[kMaxSources];
    int firstActive = -1, activeCount = 0;
    for (int i = 0; i < kMaxSources; ++i)
    {
        act[i] = sources[(size_t) i]->isActive()
                 && sources[(size_t) i]->getDeviceKey() == deviceKey;
        if (act[i]) { ++activeCount; if (firstActive < 0) firstActive = i; }
    }

    if (activeCount == 0)
    {
        // No source on this device → silence. An extra device with no bound source
        // contributes nothing to the output sum. The primary always has chain 0
        // (deviceKey 0, active from construction), so it never reaches this branch.
        for (int ch = 0; ch < effectiveOutputChannels; ++ch)
            mixBuf.clear(ch, 0, numSamples);
        return 0;
    }

    if (activeCount == 1)
    {
        // Fast path — exactly one source: process in place on mixBuf, byte-
        // identical to the single-pipeline engine (channel select / mono mix +
        // input gain, metering, ML + ring feed, gate, YIN, tone chain, monitor).
        sources[(size_t) firstActive]
            ->processBlock(inputData, numInputChannels, mixBuf, effectiveOutputChannels, numSamples);
        return 1;
    }

    // Multi-source: each renders its own 2-channel monitor into monitorScratch
    // (each builds its mono from its bound channel + feeds its own rings /
    // detectors / verifier), summed to STEREO (0/1). A >2-channel output keeps
    // channels 2+ silent in multi-source mode (the fast path still broadcasts).
    for (int ch = 0; ch < effectiveOutputChannels; ++ch)
        mixBuf.clear(ch, 0, numSamples);
    const int mixCh = juce::jmin(effectiveOutputChannels, 2);
    const int n = juce::jmin(numSamples, monitorScratch.getNumSamples());
    for (int i = 0; i < kMaxSources; ++i)
    {
        if (! act[i]) continue;
        sources[(size_t) i]->processBlock(inputData, numInputChannels, monitorScratch, 2, n);
        for (int ch = 0; ch < mixCh; ++ch)
            mixBuf.addFrom(ch, 0, monitorScratch, ch, 0, n);
    }
    return activeCount;
}

} // namespace slopsmith

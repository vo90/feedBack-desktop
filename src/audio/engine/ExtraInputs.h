#pragma once

// ExtraInputs — the additional-physical-input-device registry (TLC plan
// phase 5 / §2.3, was "Phase 2: additional input devices" inside AudioEngine).
// Each ADDITIONAL device (a 2nd/3rd USB interface) gets its own
// AudioDeviceManager + callback running on its OWN hardware clock, packing
// its sources' mixed monitor into its own SPSC ring. The engine's split
// output callback drains + sums every active ring (drop-oldest absorbs each
// device's drift independently — no cross-device resampling). deviceKey 0 =
// the primary input manager; deviceKeys 1..kMaxExtraInputDevices map to
// slots[deviceKey-1]. When any extra device is active the engine runs split.
//
// Moved verbatim from AudioEngine. The slots array stays PUBLIC so the split
// output callback keeps its ring-drain loop unchanged; sources are prepared/
// released through the bound SourcePool; engine format/run state through
// EngineState; the primary manager reference serves the primary-device
// checks (duplicate binding, latency delta, bindable enumeration).

#include "EngineState.h"
#include "PackedStereoRing.h"
#include "SourcePool.h"

#include <juce_audio_devices/juce_audio_devices.h>

#include <array>
#include <atomic>
#include <vector>

namespace slopsmith {

class ExtraInputs
{
public:
    static constexpr int kMaxExtraInputDevices = SourcePool::kMaxExtraInputDevices;
    // Ring capacity matches the engine's split-mode ring.
    static constexpr int kRingFrames = 4096;

    // Forwards a JUCE device callback to the registry, tagged with the slot index.
    struct SlotCallback : juce::AudioIODeviceCallback
    {
        ExtraInputs* owner = nullptr;
        int slot = -1;  // index into slots (deviceKey - 1)
        void audioDeviceIOCallbackWithContext(const float* const* inputData, int numInputChannels,
                                              float* const* outputData, int numOutputChannels,
                                              int numSamples,
                                              const juce::AudioIODeviceCallbackContext&) override
        {
            juce::ignoreUnused(outputData, numOutputChannels);
            if (owner) owner->slotCallback(slot, inputData, numInputChannels, numSamples);
        }
        void audioDeviceAboutToStart(juce::AudioIODevice* d) override { if (owner) owner->slotAboutToStart(slot, d); }
        void audioDeviceStopped() override { if (owner) owner->slotStopped(slot); }
    };

    struct InputDeviceSlot
    {
        juce::AudioDeviceManager manager;
        SlotCallback callback;
        PackedStereoRing<kRingFrames> ring;
        std::atomic<uint64_t> overflowCount{0};
        std::atomic<bool> active{false};      // a device is bound + running
        std::atomic<double> sampleRate{48000.0};
        std::atomic<int> blockSize{256};
        // (extra input latency − primary input latency) in seconds — applied to
        // this device's sources' verifiers so their capture aligns with the
        // primary-corrected playhead. Computed when the device starts.
        std::atomic<double> latencyDeltaSec{0.0};
        // Audio-thread scratch — one set per slot since each slot's callback runs
        // on its own thread (can't share the primary's sourceMonitorScratch).
        juce::AudioBuffer<float> fanScratch;       // the 2ch mix target
        juce::AudioBuffer<float> monitorScratch;   // per-source render in the N>1 path
        int deviceKey = 0;                         // deviceKey this slot serves (slot+1)
        // The device the user WANTS bound here — persistent INTENT, distinct from
        // the transient `active` (currently open). Set by bind(), cleared only by a
        // user unbind. stopAudio()/reconfigure close the device but keep this so
        // startAudio() re-opens it; this is what survives a device change.
        // Mutated + read on the control thread only.
        juce::String desiredDeviceName;
        // Whether the NEXT slotStopped() for this slot is a PERMANENT unbind
        // (deactivate its sources) vs a transient close (keep them to resume). An
        // atomic the control thread sets and the device thread reads, so the
        // permanent-vs-transient decision never races on the juce::String above.
        std::atomic<bool> permanentUnbind { false };
    };

    ExtraInputs(SourcePool& sourcePool, EngineState& engineState,
                juce::AudioDeviceManager& primaryInputManager)
        : pool(sourcePool), state(engineState), primaryManager(primaryInputManager)
    {
        for (int i = 0; i < kMaxExtraInputDevices; ++i)
        {
            slots[(size_t) i].callback.owner = this;
            slots[(size_t) i].callback.slot = i;
            slots[(size_t) i].deviceKey = i + 1;
        }
    }

    // ── Control thread ────────────────────────────────────────────────────
    juce::String bind(int deviceKey, const juce::String& deviceName);
    bool unbind(int deviceKey);
    // Close a slot's device but KEEP desiredDeviceName (transient close for
    // stop/reconfigure); reopenDesired() restores them after a (re)start.
    bool closeSlot(int slot);
    void reopenDesired();
    // Shutdown path: stop every slot device FIRST so no slot callback can fire
    // into a half-destroyed engine. closeAudioDevice blocks for the callback.
    void closeAllForShutdown()
    {
        for (auto& s : slots)
        {
            s.manager.closeAudioDevice();
            s.manager.removeAudioCallback(&s.callback);
        }
    }

    int activeCount() const
    {
        int n = 0;
        for (const auto& s : slots)
            if (s.active.load(std::memory_order_acquire)) ++n;
        return n;
    }

    struct Bindable { juce::String typeName; juce::String name; };
    std::vector<Bindable> listBindable();

    // Resolution for SourcePool::addResolved — the per-slot readiness/format/
    // latency the pool needs, plus whether the key is usable at all.
    struct Resolved { bool usable = false; bool ready = false; double sr = 0.0; int bs = 0; double latencyDelta = 0.0; };
    Resolved resolveForSource(int deviceKey) const
    {
        Resolved r;
        if (deviceKey < 1 || deviceKey > kMaxExtraInputDevices) return r;
        const InputDeviceSlot& es = slots[(size_t) (deviceKey - 1)];
        // Bound — either currently open (active) or DEFERRED (validated + desired
        // while the engine is stopped, to be reopened by startAudio()).
        r.usable = es.active.load(std::memory_order_acquire) || es.desiredDeviceName.isNotEmpty();
        r.ready = es.active.load(std::memory_order_acquire);
        r.sr = es.sampleRate.load(std::memory_order_relaxed);
        r.bs = es.blockSize.load(std::memory_order_relaxed);
        r.latencyDelta = es.latencyDeltaSec.load(std::memory_order_relaxed);
        return r;
    }

    // PUBLIC: the split output callback drains every active slot's ring in
    // place (same loop as before the move).
    std::array<InputDeviceSlot, kMaxExtraInputDevices> slots;

private:
    // Per-slot device-callback hooks (audio + device-management threads).
    void slotCallback(int slot, const float* const* inputData, int numInputChannels, int numSamples);
    void slotAboutToStart(int slot, juce::AudioIODevice* device);
    void slotStopped(int slot);

    SourcePool& pool;
    EngineState& state;
    juce::AudioDeviceManager& primaryManager;
};

} // namespace slopsmith

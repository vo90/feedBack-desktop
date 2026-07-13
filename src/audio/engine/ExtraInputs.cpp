// ExtraInputs implementation — moved verbatim from AudioEngine.cpp (TLC plan
// phase 5 / §2.3). Member renames only: extraInputs[...] → slots[...],
// inputDeviceManager → primaryManager, engine atomics → EngineState, source
// loops → SourcePool helpers (same locking as the engine sites had).

#include "ExtraInputs.h"

#include <cmath>
#include <cstdio>
#include <memory>

namespace slopsmith {

void ExtraInputs::slotCallback(int slot, const float* const* inputData, int numInputChannels, int numSamples)
{
    if (slot < 0 || slot >= kMaxExtraInputDevices) return;
    InputDeviceSlot& s = slots[(size_t) slot];
    if (! s.active.load(std::memory_order_acquire)) return;

    const SourcePool::CallbackGuard cbGuard(pool, s.deviceKey);

    // Clamp to the per-slot scratch sized in slotAboutToStart so the hot
    // loop never allocates if a reconfig race delivers a larger block.
    const int cap = s.fanScratch.getNumSamples();
    if (numSamples > cap) numSamples = cap;

    juce::AudioBuffer<float> mix;
    mix.setDataToReferTo(s.fanScratch.getArrayOfWritePointers(), 2, numSamples);
    pool.mixForDevice(s.deviceKey, inputData, numInputChannels, mix, s.monitorScratch, 2, numSamples);
    s.ring.push(mix.getReadPointer(0), mix.getReadPointer(1), numSamples);
}

void ExtraInputs::slotAboutToStart(int slot, juce::AudioIODevice* device)
{
    if (slot < 0 || slot >= kMaxExtraInputDevices || device == nullptr) return;
    InputDeviceSlot& s = slots[(size_t) slot];
    const int bs = device->getCurrentBufferSizeSamples();
    s.blockSize.store(bs, std::memory_order_relaxed);
    // Prepare against this DEVICE's actual sample rate — the source of truth.
    // bind() forces it to (and verifies it equals) the engine rate, so the
    // verifier (which reads the engine-wide currentSampleRate) and the detectors
    // agree. Reading the device here rather than assuming currentSampleRate keeps
    // the prepare correct even if a future path opens it differently.
    double sr = device->getCurrentSampleRate();
    if (sr <= 0.0) sr = state.currentSampleRate.load(std::memory_order_relaxed);
    s.sampleRate.store(sr, std::memory_order_relaxed);
    // Size per-slot scratch generously (cold-start guard) on this device-management
    // thread — never the RT thread.
    const int cap = juce::jmax(bs, 2048);
    s.fanScratch.setSize(2, cap, false, false, true);
    s.monitorScratch.setSize(2, cap, false, false, true);
    s.fanScratch.clear();
    s.monitorScratch.clear();
    s.ring.reset();

    // Capture-latency correction: the renderer's playhead is aligned to the PRIMARY
    // device's input latency, but this extra device captures with a different
    // latency, so its audio sits at a different song-time than the playhead assumes.
    // Set its sources' verifier offset to (extra − primary) input latency so they
    // match this device's just-captured audio against the right chart notes.
    int extraLatSamples = device->getInputLatencyInSamples();
    int primaryLatSamples = 0;
    if (auto* pdev = primaryManager.getCurrentAudioDevice())
        primaryLatSamples = pdev->getInputLatencyInSamples();
    // (extra − primary) reported input latency. On JACK/PipeWire this is 0 (no
    // latency reported); the residual per-device offset is instead dialed in by the
    // user via setSourceVerifierOffset (a stable auto-measure isn't possible — the
    // value is device-specific and signal-level-confounded). 0 here = no auto shift.
    const double deltaSec = (sr > 0.0) ? (double) (extraLatSamples - primaryLatSamples) / sr : 0.0;
    s.latencyDeltaSec.store(deltaSec, std::memory_order_relaxed);

    // Prepare each source bound to this device so its verifier/detectors run, and
    // apply the latency correction.
    pool.prepareDeviceSources(s.deviceKey, sr, bs, deltaSec, true);
    s.active.store(true, std::memory_order_release);
}

void ExtraInputs::slotStopped(int slot)
{
    if (slot < 0 || slot >= kMaxExtraInputDevices) return;
    InputDeviceSlot& s = slots[(size_t) slot];
    // JUCE blocks for this slot's callback thread before firing this, so the
    // slot's body is quiescent. Hide it from the output sum, then release ITS
    // sources (no other callback touches them — they all filter by deviceKey).
    s.active.store(false, std::memory_order_release);
    // PERMANENT unbind (user removed this device) deactivates its sources too;
    // a TRANSIENT close (stopAudio/reconfigure/unplug) only releases them so
    // startAudio()'s re-open resumes them in place. Read the atomic flag (set
    // by the control-thread unbind) rather than the juce::String
    // desiredDeviceName, which this device-thread path must not race on.
    pool.releaseDeviceSources(s.deviceKey, true,
                              s.permanentUnbind.load(std::memory_order_acquire));
    s.ring.resetIndices();
}

juce::String ExtraInputs::bind(int deviceKey, const juce::String& deviceName)
{
    if (deviceKey < 1 || deviceKey > kMaxExtraInputDevices)
        return "deviceKey out of range";
    const int slot = deviceKey - 1;
    InputDeviceSlot& s = slots[(size_t) slot];
    if (s.active.load(std::memory_order_acquire))
        return "device slot already bound";

    // Reject binding the SAME physical device into a second slot. Two callbacks
    // reading one interface is wasteful (and fails outright on exclusive drivers);
    // multiple sources that want this device should share its one deviceKey and pick
    // different channels instead. Checks both open + deferred (desired) slots.
    for (int other = 0; other < kMaxExtraInputDevices; ++other)
        if (other != slot && slots[(size_t) other].desiredDeviceName == deviceName)
            return "device already bound to another input slot";

    // Reject binding the device that is the PRIMARY input — it is already "Main", and
    // opening it on this slot's manager too would double-open one interface on two
    // managers (fatal on exclusive backends). Critically this also guards the REOPEN
    // path: if the user makes a bound extra device the new main input, the preserved
    // intent must NOT resurrect it as an extra (reopenDesired() then drops the
    // now-invalid binding via its failure handling).
    if (auto* primary = primaryManager.getCurrentAudioDevice())
        if (primary->getName() == deviceName)
            return "device is the primary input — use Main, not an extra slot";

    // An extra input device requires SPLIT mode: the output callback owns the mix +
    // backing + gain and sums every device ring. In DUPLEX the primary device owns
    // both directions and the output manager is closed, so we cannot just flip the
    // flag — that would leave the output mix path absent (silent / unrouted). Reject
    // here so the renderer reconfigures to a separate output device first. Checked
    // BEFORE the deferred path below — startAudio()'s reopen also skips duplex, so a
    // deferred bind in duplex would silently never come up while reporting success.
    if (state.duplexMode.load(std::memory_order_relaxed))
        return "extra input requires split mode — select a separate output device first";

    // Deregister any STALE callback BEFORE touching the manager. An earlier unplanned
    // stop (USB unplug / backend restart) leaves s.callback registered; if we opened
    // the manager (initialise / setAudioDeviceSetup) with it still attached, JUCE
    // could dispatch it on the default/new device mid-setup — processing the wrong
    // hardware, or even firing slotAboutToStart() during a stopped-engine
    // validation open. Idempotent no-op when not registered.
    s.manager.removeAudioCallback(&s.callback);

    // Open `deviceName` input-only on this slot's own manager. initialise first so
    // the manager has a device type, then switch to the requested input device with
    // all its channels (the source picks a channel within).
    s.manager.initialiseWithDefaultDevices(2, 0);

    // The device name may belong to a device TYPE (ALSA / JACK / CoreAudio / …)
    // different from the slot manager's default — a JACK device name won't resolve
    // under ALSA and vice-versa ("No such device"). Find the type that actually
    // lists this input device and switch the slot manager to it. Prefer the primary
    // manager's current type (the devices the user already sees working).
    juce::String chosenType;
    if (auto* pt = primaryManager.getCurrentDeviceTypeObject())
    {
        pt->scanForDevices();
        if (pt->getDeviceNames(true).contains(deviceName))
            chosenType = pt->getTypeName();
    }
    if (chosenType.isEmpty())
        for (auto* t : s.manager.getAvailableDeviceTypes())
        {
            t->scanForDevices();
            if (t->getDeviceNames(true).contains(deviceName)) { chosenType = t->getTypeName(); break; }
        }
    // setCurrentAudioDeviceType can THROW from inside some JUCE backends (ASIO, and
    // misconfigured JACK/CoreAudio) — setAudioDevices() guards it for the primary, so
    // this path must too, or a bad backend terminates the process instead of
    // returning an error to the renderer. Close the slot manager on failure.
    if (chosenType.isNotEmpty())
    {
        try { s.manager.setCurrentAudioDeviceType(chosenType, true); }
        catch (...) { s.manager.closeAudioDevice(); return "extra-input setCurrentAudioDeviceType threw"; }
    }

    juce::AudioDeviceManager::AudioDeviceSetup setup;
    s.manager.getAudioDeviceSetup(setup);
    setup.inputDeviceName = deviceName;
    setup.outputDeviceName = "";
    // Open ALL of the device's capture channels (not just the default first pair),
    // so a source bound to channel 2+ of a multi-channel extra interface actually
    // receives audio — mirrors the primary device's explicit full-range open.
    int inputChannelCount = 0;
    if (auto* t = s.manager.getCurrentDeviceTypeObject())
    {
        std::unique_ptr<juce::AudioIODevice> probe(t->createDevice({}, deviceName));
        if (probe) inputChannelCount = probe->getInputChannelNames().size();
    }
    if (inputChannelCount <= 0) inputChannelCount = 2;
    setup.inputChannels.setRange(0, inputChannelCount, true);
    setup.useDefaultInputChannels = false;
    setup.useDefaultOutputChannels = false;
    // Force the extra device to the ENGINE's sample rate. Each SourceChain's
    // verifier/detectors read the engine-wide currentSampleRate (bound by
    // reference at construction), so an extra input running at a different rate
    // (e.g. a 44.1 kHz device in a 48 kHz engine) would be scored on the wrong
    // clock — skewing pitch/timing for every source bound to it. Matching the
    // engine rate here (the OS/driver resamples if needed) keeps them coherent; a
    // device that cannot do this rate fails the setup below and is rejected.
    const double engineSr = state.currentSampleRate.load(std::memory_order_relaxed);
    if (engineSr > 0.0)
        setup.sampleRate = engineSr;
    // initialiseWithDefaultDevices above may have opened a default capture device on
    // this slot manager; every failure path below must close it, or a failed bind
    // leaves the interface captured until engine teardown (fatal on exclusive
    // backends + breaks retries / other apps).
    juce::String err;
    try { err = s.manager.setAudioDeviceSetup(setup, true); }
    catch (...) { s.manager.closeAudioDevice(); return "extra-input setAudioDeviceSetup threw"; }
    if (err.isNotEmpty())
    {
        s.manager.closeAudioDevice();
        return "extra input: " + err + (chosenType.isEmpty() ? " (no type lists this device)" : " (type " + chosenType + ")");
    }
    auto* extraDev = s.manager.getCurrentAudioDevice();
    if (extraDev == nullptr)
    {
        s.manager.closeAudioDevice();
        return "extra input device did not open";
    }

    // Some backends accept the rate request but actually open at a different rate.
    // Since the SourceChain verifier reads the engine-wide currentSampleRate, a
    // mismatch would score this device on the wrong clock — reject rather than
    // ship silently-wrong timing. (Tolerant of a sub-Hz rounding difference.)
    if (engineSr > 0.0 && std::abs(extraDev->getCurrentSampleRate() - engineSr) > 1.0)
    {
        const juce::String got = juce::String(extraDev->getCurrentSampleRate());
        s.manager.closeAudioDevice();
        return "extra input opened at " + got + " Hz, not the engine rate " + juce::String(engineSr) + " Hz";
    }

    // The device opened + validated. Record the INTENT now (not before the fallible
    // open above), so it drives re-open across a reconfigure without lingering after
    // a failed attach. Clear the permanent-unbind flag: a future stop on this slot is
    // transient (resume) until the user explicitly unbinds again.
    s.desiredDeviceName = deviceName;
    s.permanentUnbind.store(false, std::memory_order_release);

    // If the engine is not running, we opened only to VALIDATE eagerly (so an
    // unplugged / wrong-rate device fails the bind NOW instead of silently dropping
    // at the next startAudio). Close it again so a stopped engine never leaves an
    // interface capturing in the background; reopenDesired() re-opens it (and
    // re-attaches the callback) when the engine next starts.
    if (! state.deviceRunning.load(std::memory_order_relaxed))
    {
        s.manager.closeAudioDevice();
        return {};
    }

    // Attach the callback — fires slotAboutToStart (prepares + flips active).
    // Any stale registration was already removed before the open above, so this
    // registers exactly once.
    s.manager.addAudioCallback(&s.callback);
    return {};
}

bool ExtraInputs::unbind(int deviceKey)
{
    if (deviceKey < 1 || deviceKey > kMaxExtraInputDevices)
        return false;
    const int slot = deviceKey - 1;
    // User-initiated unbind: mark it PERMANENT (the device thread's slotStopped
    // reads the flag) and clear the intent so no restore path resurrects a device
    // deliberately removed.
    slots[(size_t) slot].permanentUnbind.store(true, std::memory_order_release);
    slots[(size_t) slot].desiredDeviceName = {};
    // If the device is open, closing it fires slotStopped(), which — with the
    // intent now cleared — deactivates this deviceKey's sources. If it was ALREADY
    // closed (e.g. a prior stopAudio() kept the intent + left the sources active for
    // a resume that will now never come), slotStopped() will NOT run, so we
    // must deactivate them here — otherwise they linger as ghost sources stranding
    // pool slots and showing in listSources().
    if (! closeSlot(slot))
    {
        pool.withDeviceSources(deviceKey, [](SourceChain& s) {
            s.releaseResources();
            s.setActive(false);
        });
    }
    return true;
}

// Close the device open on a slot WITHOUT forgetting desiredDeviceName, so
// startAudio() re-opens it. Used by stopAudio()/reconfigure (transient close) — the
// public unbind() clears the intent first (permanent removal).
bool ExtraInputs::closeSlot(int slot)
{
    if (slot < 0 || slot >= kMaxExtraInputDevices)
        return false;
    InputDeviceSlot& s = slots[(size_t) slot];
    const bool wasActive = s.active.load(std::memory_order_acquire);
    // Close + deregister UNCONDITIONALLY (not gated on `active`). An UNPLANNED stop
    // (USB unplug / backend restart) fires slotStopped() — flipping active
    // false — yet leaves the manager owning a (possibly auto-recovering) device and
    // s.callback still registered. If we no-oped on !active, stopAudio()/reconfigure
    // would never release it and the backend could resume callbacks after the engine
    // is supposedly stopped. Both calls are idempotent when already closed/absent.
    // For an ACTIVE slot, closeAudioDevice() blocks for the callback thread then fires
    // audioDeviceStopped → slotStopped (releases this device's sources).
    s.manager.closeAudioDevice();
    s.manager.removeAudioCallback(&s.callback);
    return wasActive;
}

// Re-open every slot that has a desiredDeviceName but is not currently active — the
// post-(re)start restore of extra inputs. No-op in duplex (extras need split) and
// when nothing is desired (the single-device path). Called from startAudio().
void ExtraInputs::reopenDesired()
{
    if (state.duplexMode.load(std::memory_order_relaxed))
    {
        // Duplex has no consumer for extra-device rings, so the desired extras cannot
        // open right now. PRESERVE their intent (so a later switch back to split
        // auto-restores them — setAudioDevices() promises bindings survive a device
        // change) and keep their sources active to resume in place; but ZERO their
        // meters so getSourceLevels() reports silence while the device is gone (the
        // renderer's per-source silence gate then won't treat a temporarily-unavailable
        // source as still hearing audio, and there is no false detection). The sources
        // are not "ghosts": split-restore reopens the device and they resume.
        for (int dk = 1; dk <= kMaxExtraInputDevices; ++dk)
        {
            if (slots[(size_t) (dk - 1)].desiredDeviceName.isEmpty())
                continue;
            pool.withDeviceSources(dk, [](SourceChain& s) { s.resetInputMeters(); });
        }
        return;
    }
    for (int dk = 1; dk <= kMaxExtraInputDevices; ++dk)
    {
        InputDeviceSlot& s = slots[(size_t) (dk - 1)];
        if (s.desiredDeviceName.isEmpty() || s.active.load(std::memory_order_acquire))
            continue;
        const juce::String err = bind(dk, s.desiredDeviceName);  // re-sets desired (idempotent)
        if (err.isNotEmpty())
        {
            // Reopen failed — the interface was unplugged, or no longer supports the
            // engine rate. The transient close kept this slot's sources ACTIVE to
            // resume; since they now never will, give up cleanly: drop the intent and
            // deactivate them so they do not linger as ghost sources stranding pool
            // slots. The renderer re-binds + re-adds if the device returns.
            s.desiredDeviceName = {};
            pool.withDeviceSources(dk, [](SourceChain& src) { src.setActive(false); });
        }
    }
}

std::vector<ExtraInputs::Bindable> ExtraInputs::listBindable()
{
    std::vector<Bindable> out;
    // The device already open as the primary input IS "Main" — don't offer it as
    // an extra (would double-open the same hardware on two managers).
    juce::String primaryName;
    if (auto* dev = primaryManager.getCurrentAudioDevice())
        primaryName = dev->getName();
    // Enumerate across ALL device types, not just the primary's current one —
    // bind() can open a device under any backend (JACK/ALSA/CoreAudio/…), so an
    // extra interface exposed under a DIFFERENT backend than the primary must
    // still be offered, or the multi-device path is unreachable from the picker.
    //
    // KNOWN LIMITATION: identity is the display name. JUCE opens input devices BY
    // NAME, so two interfaces sharing a label (e.g. two identical USB cables) cannot
    // be distinguished or independently opened without a backend-specific device-id
    // rework — they collapse to one entry here. The SAME root cause makes a device
    // exposed under MULTIPLE backends (e.g. ALSA + JACK/PipeWire on Linux) ambiguous:
    // we dedup by name and bind() re-derives the backend (preferring the primary's),
    // so we may bind the wrong backend if only another would open. A real fix needs
    // (typeName, name) identity threaded through bind/reopen. Distinct-name,
    // single-backend rigs (the common case, and the validated GP-5 + Spark setup) are
    // unaffected.
    juce::StringArray seen;
    for (auto* t : primaryManager.getAvailableDeviceTypes())
    {
        if (!t) continue;
        t->scanForDevices();
        const juce::String typeName = t->getTypeName();
        for (const auto& name : t->getDeviceNames(true))
        {
            if (name == primaryName || seen.contains(name)) continue;  // dedup across backends
            // Skip monitor / loopback pseudo-inputs — not instrument inputs, only
            // confuse the picker.
            const juce::String lower = name.toLowerCase();
            if (lower.contains("monitor") || lower.contains("loopback")) continue;
            seen.add(name);
            out.push_back({ typeName, name });
        }
    }
    return out;
}

} // namespace slopsmith

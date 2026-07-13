#pragma once

// EngineState — the audio engine's shared run-state atomics (TLC plan
// phase 1 / §2.8). Every extracted engine unit takes an EngineState& instead
// of reaching back into AudioEngine, which is what keeps them unit-testable
// without JUCE devices. AudioEngine itself binds these members by reference
// under their historical names, so existing call sites are untouched.
//
// The deliberate fix homed here (deep-read §3/§6): the old single
// `audioRunning` flag conflated USER INTENT ("the user pressed Start") with
// DEVICE STATE ("a device callback is live") — audioDeviceStopped() clears it
// on transient stops (WASAPI exclusive opens routinely fire one mid-start),
// so setAudioDevices' restart decision read a racy answer. The two are now
// separate atomics:
//
//   userWantsAudio — intent. Written ONLY by startAudio()/stopAudio().
//   deviceRunning  — state. Written by startAudio()/stopAudio() AND the
//                    device callbacks (aboutToStart/stopped), i.e. the exact
//                    semantics the old audioRunning had. isAudioRunning()
//                    keeps reporting THIS one (Phase 0.b compat pin).
//
// Until the phase-8 fix, nothing reads userWantsAudio — writing it here first
// keeps that later commit a one-line read-side change in setAudioDevices.

#include <atomic>

namespace slopsmith {

struct EngineState
{
    // Sample rate is written from the JUCE device callbacks (audio thread /
    // device-management thread) and read from arbitrary callers including the
    // JS thread, so a plain double would be a C++ data race. atomic<double>
    // is lock-free on the platforms we ship; hot reads use relaxed since the
    // consumer just wants the latest observable value, not a sync point.
    std::atomic<double> currentSampleRate{48000.0};
    // Split mode allows different input vs output block sizes; the ring
    // absorbs the asymmetry. DSP prepares against input; backing resampler
    // against output.
    std::atomic<int> inputBlockSize{256};
    std::atomic<int> outputBlockSize{256};
    // Duplex: one device manager owns both directions. Split: input-only +
    // output-only managers with an SPSC ring between them.
    std::atomic<bool> duplexMode{true};

    // Intent: the user asked for audio to run. start/stopAudio only.
    std::atomic<bool> userWantsAudio{false};
    // State: toggled from startAudio()/stopAudio() (main/device-management
    // threads) and the device callbacks, read from isAudioRunning() on the JS
    // thread. Plain bool would be a data race; relaxed-atomic compiles to a
    // plain MOV.
    std::atomic<bool> deviceRunning{false};
};

} // namespace slopsmith

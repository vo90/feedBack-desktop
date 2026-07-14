// Phase 1 unit test for EngineState (docs/audio-engine-tlc.md §5): the
// intent/state transition table. Mirrors how AudioEngine drives the two
// flags — startAudio/stopAudio write BOTH (intent + state), the device
// callbacks write deviceRunning ONLY — and pins the Phase 0.b compat
// decision that isAudioRunning() reports DEVICE STATE: a transient device
// stop flips it false even though the user never pressed Stop.

#include "../../src/audio/engine/EngineState.h"

#include <cassert>
#include <cstdio>

using slopsmith::EngineState;

// The write sets, as AudioEngine performs them.
static void userStart(EngineState& s)  { s.userWantsAudio.store(true);  s.deviceRunning.store(true); }
static void userStop(EngineState& s)   { s.userWantsAudio.store(false); s.deviceRunning.store(false); }
static void deviceAboutToStart(EngineState& s) { s.deviceRunning.store(true); }
static void deviceStopped(EngineState& s)      { s.deviceRunning.store(false); }
// isAudioRunning() facade == deviceRunning (compat pin).
static bool isAudioRunning(const EngineState& s) { return s.deviceRunning.load(); }

int main()
{
    EngineState s;
    assert(!s.userWantsAudio.load() && !isAudioRunning(s));

    // User starts audio.
    userStart(s);
    assert(s.userWantsAudio.load() && isAudioRunning(s));

    // Transient device stop (WASAPI exclusive mid-start hiccup): device state
    // drops, intent survives — this is the split that fixes deep-read §3.
    deviceStopped(s);
    assert(s.userWantsAudio.load() && "transient stop must not erase user intent");
    assert(!isAudioRunning(s) && "compat pin: isAudioRunning reports device state");

    // JUCE auto-restart brings the device back without user action.
    deviceAboutToStart(s);
    assert(s.userWantsAudio.load() && isAudioRunning(s));

    // Explicit user stop clears both.
    userStop(s);
    assert(!s.userWantsAudio.load() && !isAudioRunning(s));

    // A stray device start (auto-restart after user stop) must not fabricate
    // intent: device state true, intent still false.
    deviceAboutToStart(s);
    assert(!s.userWantsAudio.load() && isAudioRunning(s));

    std::puts("engine_state: all transitions passed");
    return 0;
}

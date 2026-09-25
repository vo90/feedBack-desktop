# Backing clock snapshot v1

`audio.getBackingSnapshot()` supplements the existing position getter. It returns
one coherent observation: `version`, `valid`, `position`, `ageMs`, `sequence`,
`generation`, `rate`, `playing`, and `ended`. The preload returns `null` when a
downlevel addon lacks the capability. An uninitialized engine or contended reader
returns `valid: false`; callers must discard that observation.

Position retains BackingPlayer's existing seconds/latency convention. The addon
computes sample age using the same native monotonic clock used at publication.
The renderer must not subtract this clock from `performance.now()`; bracket the
IPC request and map age onto that bracket, accounting for round-trip uncertainty.
Neither reading the snapshot nor forwarding it through IPC refreshes its age.

All publishers already hold BackingPlayer's lock: controls acquire it, while audio
callbacks publish only after their existing try-lock succeeds. Atomic payload
fields plus an odd/even sequence produce a coherent read without a C++ data race.
The writer adds no allocation, lock, wait or retry. The reader attempts at most
three times. Publication sequence advances for each observation; generation also
advances on load, seek, play/stop, device preparation and adoption of a new rate.
EOF publishes `playing: false, ended: true`; a subsequent seek/load clears ended.

Core uses poll ownership to reject responses from earlier transport commands and
uses an explicit presentation epoch for deliberate visual repositioning. Native
generation is observation ordering metadata, not a request to snap the renderer
on every speed change or device reconfiguration.

Tests cover concurrent publications, IPC capability fallback, and the real
BackingPlayer through load, play, seek, rate, device preparation, stop, EOF and
failed load. The latter uses a temporary WAV and manual audio blocks without
opening an audio device. Configure the full JUCE build to run it:

```
cmake --build <build-dir> --config Release --target backing_clock_snapshot_test backing_player_clock_test
ctest --test-dir <build-dir> -C Release -R "backing_(clock_snapshot|player_clock)$" --output-on-failure
```

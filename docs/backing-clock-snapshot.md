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
The main process adds `clockId` (its monotonic time origin), `readAtMs` (the native
read's midpoint in that clock) and `readUncertaintyMs` (half the read duration).
Core calibrates the offset to its own monotonic clock from request bounds. This
lets a delayed response retain its actual observation time. Neither reading the
snapshot nor forwarding it through IPC refreshes its native age.

`audio.subscribeBackingSnapshots(callback)` starts a 50 ms main-thread publisher
and returns an idempotent unsubscribe function. It supports one transport owner
per renderer. Increasing subscription tokens protect replacement subscriptions
from late stop messages; the preload filters queued packets by token. Pausing,
seeking or changing songs unsubscribes. Main-frame navigation, renderer loss and
WebContents destruction release timers and listeners. The publisher never runs
on the real-time audio thread. Polled observations remain available for clock
calibration and compatibility with Core builds without subscriptions.

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

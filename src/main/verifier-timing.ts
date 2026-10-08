// Resolve native judgment time at IPC receipt from the output clock. Message
// transit must not become extra input latency. Browser callers keep their clock.
export function resolveVerifierTiming(audio: any, songTime: number, playing: boolean, rate: unknown, timing: any) {
    if (timing == null) return { songTime, playing, rate };
    const frozen = { songTime, playing: false, rate };
    if (timing.version !== 2 || !Number.isSafeInteger(timing.generation)
        || !Number.isSafeInteger(timing.routeGeneration)
        || !Number.isFinite(timing.songOffset) || Math.abs(timing.songOffset) > 86400
        || !Number.isFinite(timing.avOffsetMs) || Math.abs(timing.avOffsetMs) > 1000
        || !Number.isFinite(timing.inputAgeMs) || timing.inputAgeMs < 0 || timing.inputAgeMs > 1000) return frozen;
    const snapshot = audio?.getBackingSnapshot?.(), p = snapshot?.presentation;
    if (!snapshot?.valid || snapshot.generation !== timing.generation
        || p?.version !== 2 || p.routeGeneration !== timing.routeGeneration
        || !Number.isFinite(p.position) || !Number.isFinite(snapshot.rate)
        || snapshot.rate <= 0 || snapshot.rate > 4) return frozen;
    const presentation = p.position + timing.avOffsetMs * snapshot.rate / 1000;
    const outputTail = snapshot.ended === true && presentation < snapshot.position;
    return { songTime: p.position + timing.songOffset
        + (timing.avOffsetMs - timing.inputAgeMs) * snapshot.rate / 1000,
        playing: playing && (p.playing === true || outputTail), rate: snapshot.rate };
}

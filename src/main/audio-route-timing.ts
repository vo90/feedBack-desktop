import type { createRendererAudioPortBridge } from './renderer-audio-port';

type NativeAudio = Record<string, (...args: any[]) => any> | null;
type Transport = ReturnType<ReturnType<typeof createRendererAudioPortBridge>['snapshot']>;

// On-demand diagnostics only: no polling, PCM copies, device changes or audio
// callback work. Native counters are cumulative and reads are not atomic with
// respect to the output callback. Never sum these fields into a latency claim.
export function readAudioRouteTiming(audio: NativeAudio, transport: Transport,
    rendererBusControlEpoch: number, now: () => number = () => performance.now()) {
    const errors: string[] = [];
    function read(method: string) {
        if (typeof audio?.[method] !== 'function') return null;
        try { return audio[method](); }
        catch { errors.push(method); return null; }
    }
    const running = read('isAudioRunning');
    return {
        schemaVersion: 1,
        sampledAtMs: now(), clock: 'electron-main-monotonic-ms',
        running, rendererBusControlEpoch, transport,
        device: running === true ? read('getCurrentDevice') : null,
        rendererBus: read('getRendererBusMetrics'),
        // Includes heuristic monitor/split-ring estimates and driver reports;
        // rendererBusMs is occupancy duration, not the age of any sample.
        nativeLatencyEstimate: running === true ? read('getLatencyBreakdown') : null,
        counterScope: 'native-engine-lifetime',
        snapshotConsistency: 'sequential-non-atomic',
        measuredEndToEndMs: null,
        errors,
    };
}

export type AudioRouteTiming = ReturnType<typeof readAudioRouteTiming>;

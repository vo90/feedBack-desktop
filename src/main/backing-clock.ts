import { performance } from 'node:perf_hooks';
import type { WebContents } from 'electron';
import type { createClockTrace } from './backing-clock-diagnostics';

type SnapshotSource = { getBackingSnapshot?: () => Record<string, unknown> | null };
type MonotonicClock = { now(): number; timeOrigin: number };

// This runs on the main thread, outside the real-time audio callback. Keep the
// native sample age intact and bracket its read in one named monotonic domain.
// The renderer calibrates that domain using request/response bounds; it must
// not interpret readAtMs as a renderer timestamp or a wall-clock timestamp.
export function readBackingSnapshot(
    audio: SnapshotSource | null,
    clock: MonotonicClock = performance,
): Record<string, unknown> | null {
    if (typeof audio?.getBackingSnapshot !== 'function') return null;
    const before = clock.now();
    const snapshot = audio.getBackingSnapshot();
    const after = clock.now();
    if (snapshot == null) return null;
    return {
        ...snapshot,
        clockId: clock.timeOrigin,
        readAtMs: (before + after) / 2,
        readUncertaintyMs: Math.max(0, after - before) / 2,
    };
}

// One bounded publisher per renderer. Subscription tokens keep an old stop
// message from removing a newer subscription after a rapid pause/resume.
export function createBackingClockPublisher(
    read: () => Record<string, unknown> | null,
    schedule = setInterval,
    cancel = clearInterval,
    traceFor?: (sender: WebContents) => ReturnType<typeof createClockTrace> | undefined,
    clock: MonotonicClock = performance,
) {
    const active = new Map<number, {token: number; stop: () => void}>();
    return {
        start(sender: WebContents, token: number) {
            if (!Number.isSafeInteger(token) || token <= 0 || sender.isDestroyed()) return;
            if ((active.get(sender.id)?.token ?? 0) >= token) return;
            active.get(sender.id)?.stop();
            let timer: ReturnType<typeof setInterval>;
            const stop = () => {
                cancel(timer);
                sender.removeListener('destroyed', stop);
                sender.removeListener('render-process-gone', stop);
                sender.removeListener('did-start-navigation', navigate);
                if (active.get(sender.id)?.stop === stop) active.delete(sender.id);
            };
            const navigate = (_event: unknown, _url: string, inPlace: boolean, mainFrame: boolean) => {
                if (mainFrame && !inPlace) stop();
            };
            let dueAtMs: number | null = null, traceCapture = 0;
            const tick = () => {
                if (sender.isDestroyed()) { stop(); return; }
                const trace = traceFor?.(sender);
                const observed = trace?.enabled;
                const startedAtMs = observed ? clock.now() : 0;
                const expectedAtMs = traceCapture === trace?.capture ? dueAtMs : null;
                traceCapture = trace?.capture ?? 0;
                dueAtMs = observed ? startedAtMs + 50 : null;
                let readEndedAtMs: number | null = null, dispatchAtMs: number | null = null;
                let snapshot: Record<string, unknown> | null = null, failed = false;
                try {
                    snapshot = read();
                    if (observed) readEndedAtMs = clock.now();
                    if (snapshot?.valid) {
                        if (observed) dispatchAtMs = clock.now();
                        sender.send('audio:backingSnapshot', token, snapshot);
                    }
                } catch { failed = true; stop(); }
                finally {
                    if (observed) trace.record({ kind: 'stream-publish', token, expectedAtMs, startedAtMs,
                        readEndedAtMs, dispatchAtMs, endedAtMs: clock.now(), failed,
                        valid: snapshot?.valid === true,
                        sequence: typeof snapshot?.sequence === 'number' ? snapshot.sequence : null,
                        generation: typeof snapshot?.generation === 'number' ? snapshot.generation : null,
                        position: typeof snapshot?.position === 'number' ? snapshot.position : null,
                        readAtMs: typeof snapshot?.readAtMs === 'number' ? snapshot.readAtMs : null,
                        readUncertaintyMs: typeof snapshot?.readUncertaintyMs === 'number' ? snapshot.readUncertaintyMs : null,
                        nativeAgeMs: typeof snapshot?.ageMs === 'number' ? snapshot.ageMs : null });
                }
            };
            active.set(sender.id, {token, stop});
            sender.once('destroyed', stop);
            sender.once('render-process-gone', stop);
            sender.on('did-start-navigation', navigate);
            timer = schedule(tick, 50);
            timer.unref();
            tick();
        },
        stop(sender: WebContents, token: number) {
            const subscription = active.get(sender.id);
            if (subscription?.token === token) subscription.stop();
        },
    };
}

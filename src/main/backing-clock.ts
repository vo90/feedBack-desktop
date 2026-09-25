import { performance } from 'node:perf_hooks';
import type { WebContents } from 'electron';

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
            const tick = () => {
                if (sender.isDestroyed()) { stop(); return; }
                try {
                    const snapshot = read();
                    if (snapshot?.valid) sender.send('audio:backingSnapshot', token, snapshot);
                } catch { stop(); }
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

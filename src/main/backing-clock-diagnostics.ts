import { performance } from 'node:perf_hooks';
import type { WebContents } from 'electron';

export type ClockTraceRow = Record<string, number | string | boolean | null | undefined>;

// Diagnostics are opt-in, bounded and pulled after a capture. No timer, logging
// or per-observation IPC is introduced by this recorder.
export function createClockTrace(capacity = 4096) {
    if (!Number.isSafeInteger(capacity) || capacity < 1 || capacity > 4096) throw new RangeError('Invalid clock trace capacity');
    let enabled = false, total = 0, capture = 0;
    const rows: ClockTraceRow[] = [];
    return {
        get enabled() { return enabled; },
        get capture() { return capture; },
        setEnabled(value: boolean) {
            enabled = value;
            if (value) { rows.length = 0; total = 0; capture++; }
        },
        record(row: ClockTraceRow, expectedCapture = capture) {
            if (!enabled || expectedCapture !== capture) return;
            rows[total % capacity] = row;
            total++;
        },
        read() {
            const count = Math.min(total, capacity), first = total > capacity ? total % capacity : 0;
            return { enabled, capture, capacity, total, dropped: total - count,
                rows: Array.from({ length: count }, (_, i) => ({ ...rows[(first + i) % capacity] })) };
        },
    };
}

export function clockSnapshotFields(snapshot: Record<string, unknown> | null | undefined): ClockTraceRow {
    const number = (key: string) => typeof snapshot?.[key] === 'number' ? snapshot[key] as number : null;
    return { valid: snapshot?.valid === true, sequence: number('sequence'), generation: number('generation'),
        position: number('position'), nativeAgeMs: number('ageMs'), readAtMs: number('readAtMs'),
        readUncertaintyMs: number('readUncertaintyMs'), clockId: number('clockId') };
}

export function createBackingClockTraceRegistry(clock = performance) {
    const traces = new Map<number, ReturnType<typeof createClockTrace>>();
    return {
        forSender(sender: WebContents) { return traces.get(sender.id); },
        setEnabled(sender: WebContents, enabled: unknown) {
            if (typeof enabled !== 'boolean' || sender.isDestroyed()) return false;
            let trace = traces.get(sender.id);
            if (!trace && enabled) {
                trace = createClockTrace(); traces.set(sender.id, trace);
                const clear = () => {
                    traces.delete(sender.id);
                    sender.removeListener('destroyed', clear);
                    sender.removeListener('render-process-gone', clear);
                    sender.removeListener('did-start-navigation', navigate);
                };
                const navigate = (_event: unknown, _url: string, inPlace: boolean, mainFrame: boolean) => {
                    if (mainFrame && !inPlace) clear();
                };
                sender.once('destroyed', clear);
                sender.once('render-process-gone', clear);
                sender.on('did-start-navigation', navigate);
            }
            trace?.setEnabled(enabled);
            return true;
        },
        read(sender: WebContents, read: () => Record<string, unknown> | null) {
            const trace = traces.get(sender.id);
            if (!trace?.enabled) return read();
            const startedAtMs = clock.now();
            let snapshot: Record<string, unknown> | null = null, failed = true;
            try { snapshot = read(); failed = false; return snapshot; }
            finally { trace.record({ kind: 'poll-read', startedAtMs, endedAtMs: clock.now(), failed,
                ...clockSnapshotFields(snapshot) }); }
        },
        snapshot(sender: WebContents) {
            return { clockId: clock.timeOrigin, ...(traces.get(sender.id)?.read() ?? createClockTrace().read()) };
        },
    };
}

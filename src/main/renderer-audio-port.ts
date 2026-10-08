// A transferred AudioWorklet port delivers PCM without a renderer-UI hop.
// Keep a single owner: replacing/stopping a capture cannot leave an old producer
// feeding the same native SPSC bus. This module is independent of Electron for tests.
interface AudioPort {
    on(event: string, listener: (...args: any[]) => void): unknown;
    start(): void;
    close(): void;
}
type PacketTiming = { version: 1; sequence: number; firstFrame: number; endFrame: number };
function readTiming(value: any, frames: number): PacketTiming | null {
    if (value?.version !== 1 || ![value.sequence, value.firstFrame, value.endFrame]
        .every(n => Number.isSafeInteger(n) && n >= 0) || value.endFrame - value.firstFrame !== frames) return null;
    return { version: 1, sequence: value.sequence, firstFrame: value.firstFrame, endFrame: value.endFrame };
}
type Session = {
    owner: number; id: string; port: AudioPort; epoch: number; attachedAtMs: number; lastPacketAt: number;
    packetsReceived: number; framesReceived: number; sampleRate: number | null;
    rejectedPackets: number; packetsWithTiming: number; invalidTimingPackets: number; discontinuities: number;
    maxReceiptIntervalMs: number | null; lastTiming: PacketTiming | null;
};
export function createRendererAudioPortBridge(push: (pcm: Float32Array, rate: number) => void,
    now: () => number = () => performance.now()) {
    let active: Session | null = null;
    let epoch = 0;
    function close(owner: number, id?: string) {
        if (!active || active.owner !== owner || (id !== undefined && active.id !== id)) return false;
        const previous = active; active = null; previous.port.close(); return true;
    }
    return {
        close,
        // The worklet clocks silence too. A connected but dead producer must
        // not leave the game claiming successful routing indefinitely.
        has: (owner: number, id: string) => active?.owner === owner && active.id === id
            && now() - active.lastPacketAt < 2000,
        snapshot(owner: number) {
            if (!active || active.owner !== owner) return null;
            const s = active;
            return {
                captureId: s.id, captureEpoch: s.epoch, clock: 'electron-main-monotonic-ms',
                attachedAtMs: s.attachedAtMs, lastReceivedAtMs: s.packetsReceived ? s.lastPacketAt : null,
                packetsReceived: s.packetsReceived, framesReceived: s.framesReceived,
                sampleRate: s.sampleRate, rejectedPackets: s.rejectedPackets,
                packetsWithTiming: s.packetsWithTiming,
                invalidTimingPackets: s.invalidTimingPackets, discontinuities: s.discontinuities,
                maxReceiptIntervalMs: s.maxReceiptIntervalMs,
                sourceClock: 'capture-audio-context-frames',
                lastSourcePacket: s.lastTiming ? { ...s.lastTiming } : null,
                // No shared clock correlation or device presentation marker yet.
                sourceToReceiptMs: null,
            };
        },
        attach(owner: number, id: unknown, port: AudioPort) {
            if (typeof id !== 'string' || !id || id.length > 128) { port.close(); return false; }
            if (active) close(active.owner);
            const attachedAtMs = now();
            const session: Session = active = { owner, id, port, epoch: ++epoch, attachedAtMs,
                lastPacketAt: attachedAtMs, packetsReceived: 0, framesReceived: 0, sampleRate: null,
                rejectedPackets: 0, packetsWithTiming: 0, invalidTimingPackets: 0, discontinuities: 0,
                maxReceiptIntervalMs: null, lastTiming: null };
            port.on('message', ({ data }: { data: any }) => {
                if (active !== session) return;
                const pcm = data?.pcm, rate = data?.sampleRate;
                if (!(pcm instanceof Float32Array) || pcm.length < 2 || pcm.length > 32768
                    || pcm.length % 2 || typeof rate !== 'number' || !Number.isFinite(rate)
                    || rate < 8000 || rate > 384000) { session.rejectedPackets++; return; }
                for (let i = 0; i < pcm.length; ++i) if (!Number.isFinite(pcm[i])) pcm[i] = 0;
                const receivedAt = now();
                if (session.packetsReceived) session.maxReceiptIntervalMs = Math.max(
                    session.maxReceiptIntervalMs ?? 0, receivedAt - session.lastPacketAt);
                const timing = readTiming(data.timing, pcm.length / 2);
                if (timing) session.packetsWithTiming++;
                if (data.timing != null && !timing) session.invalidTimingPackets++;
                if (timing && session.lastTiming && (timing.sequence !== session.lastTiming.sequence + 1
                    || timing.firstFrame !== session.lastTiming.endFrame || rate !== session.sampleRate)) session.discontinuities++;
                session.lastTiming = timing;
                session.lastPacketAt = receivedAt;
                session.packetsReceived++;
                session.framesReceived += pcm.length / 2;
                session.sampleRate = rate;
                push(pcm, rate);
            });
            port.on('close', () => { if (active === session) active = null; });
            port.start();
            return true;
        },
    };
}

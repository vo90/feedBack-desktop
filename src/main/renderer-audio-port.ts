// A transferred AudioWorklet port delivers PCM without a renderer-UI hop.
// Keep a single owner: replacing/stopping a capture cannot leave an old producer
// feeding the same native SPSC bus. This module is independent of Electron for tests.
interface AudioPort {
    on(event: string, listener: (...args: any[]) => void): unknown;
    start(): void;
    close(): void;
}
export function createRendererAudioPortBridge(push: (pcm: Float32Array, rate: number) => void,
    now: () => number = () => performance.now()) {
    let active: { owner: number; id: string; port: AudioPort; lastPacketAt: number } | null = null;
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
        attach(owner: number, id: unknown, port: AudioPort) {
            if (typeof id !== 'string' || !id || id.length > 128) { port.close(); return false; }
            if (active) close(active.owner);
            const session = active = { owner, id, port, lastPacketAt: now() };
            port.on('message', ({ data }: { data: any }) => {
                if (active !== session) return;
                const pcm = data?.pcm, rate = data?.sampleRate;
                if (!(pcm instanceof Float32Array) || pcm.length < 2 || pcm.length > 32768
                    || pcm.length % 2 || typeof rate !== 'number' || !Number.isFinite(rate)
                    || rate < 8000 || rate > 384000) return;
                for (let i = 0; i < pcm.length; ++i) if (!Number.isFinite(pcm[i])) pcm[i] = 0;
                session.lastPacketAt = now();
                push(pcm, rate);
            });
            port.on('close', () => { if (active === session) active = null; });
            port.start();
            return true;
        },
    };
}

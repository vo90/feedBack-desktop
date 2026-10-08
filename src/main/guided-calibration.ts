// A temporary, exclusive owner of the existing native backing player. Song
// commands and restoration share one queue; the realtime callback is unchanged.
import { randomUUID } from 'crypto';

type Audio = Record<string, (...args: any[]) => any>;
export function createGuidedCalibration(deps: {
    audio: () => Audio | null; profile: () => any; asset: () => string;
    snapshot: () => any; save: (key: string, offset: number) => boolean;
    now?: () => number;
}) {
    let chain: Promise<any> = Promise.resolve();
    let song: { method: string; args: any[] } | null = null;
    let speed = 1, gain = 1, gains: number[] | null = null;
    let session: any = null;
    const now = deps.now || Date.now;
    const queue = (fn: () => any) => {
        const next = chain.then(fn); chain = next.catch(() => {}); return next;
    };
    const identity = () => {
        const a = deps.audio(), p = deps.profile();
        return a?.isAudioRunning() && p?.output ? JSON.stringify([
            p.output.routeKey, p.output.key, p.perOutputSetup,
            a.getCurrentDevice()?.routeGeneration,
        ]) : null;
    };
    async function call(method: string, ...args: any[]) {
        const a = deps.audio();
        if (!a || typeof a[method] !== 'function' || await a[method](...args) === false)
            throw new Error(`Audio operation failed: ${method}`);
    }
    async function restore() {
        const s = session;
        if (!s) return true;
        try {
            await call('stopBacking');
            if (s.song) {
                await call(s.song.method, ...s.song.args);
                if (s.gains) await call('setBackingSourceGains', s.gains);
                await call('setBackingSpeed', s.speed);
                await call('seekBacking', s.position);
            }
            await call('setGain', 'backing', s.gain);
            return true; // Restored paused, never resume music unexpectedly.
        } finally { session = null; }
    }
    function owns(token: string, sender: number) {
        return session && session.token === token && session.sender === sender;
    }
    return {
        get active() { return !!session; },
        // All ordinary backing mutations enter here, including asynchronous loads.
        run(method: string, ...args: any[]) {
            if (session) return Promise.resolve(false);
            return queue(async () => {
                if (session) return false;
                const a = deps.audio();
                if (!a || typeof a[method] !== 'function') return false;
                const result = await a[method](...args);
                if (result === false) return false;
                if (method === 'loadBackingTrack' || method === 'loadBackingSession') {
                    song = {method, args: JSON.parse(JSON.stringify(args))};
                    gains = null; speed = 1;
                }
                if (method === 'setBackingSpeed') speed = args[0];
                if (method === 'setBackingSourceGains') gains = [...args[0]];
                if (method === 'setGain') gain = args[1];
                return result;
            });
        },
        begin(sender: number) {
            return queue(async () => {
                if (session) throw new Error('Calibration is already open.');
                const route = identity(), profile = deps.profile();
                if (!route || !profile.output.persistent) throw new Error('Apply an unambiguous output device before calibrating.');
                session = {token: randomUUID(), sender, route, profile, touched: now(),
                    song, speed, gain, gains, position: deps.audio()?.getBackingPosition() || 0};
                try {
                    await call('stopBacking');
                    await call('loadBackingTrack', deps.asset());
                    await call('setBackingSpeed', 1);
                    await call('setGain', 'backing', .35);
                    if (identity() !== route) throw new Error('Output changed. Start calibration again.');
                    return {token: session.token, profile, cues: [2, 5, 8], duration: 10};
                } catch (error) { await restore(); throw error; }
            });
        },
        trial(token: string, sender: number, volume: number) {
            return queue(async () => {
                if (!owns(token, sender) || identity() !== session.route) throw new Error('Output changed. Start calibration again.');
                session.touched = now();
                await call('stopBacking');
                await call('seekBacking', 0);
                await call('setGain', 'backing', Math.max(.02, Math.min(.6, Number.isFinite(volume) ? volume : .35)));
                await call('startBacking');
                return true;
            });
        },
        poll(token: string, sender: number) {
            if (!owns(token, sender)) return null;
            session.touched = now();
            if (identity() !== session.route) return null;
            return deps.snapshot();
        },
        finish(token: string, sender: number, offset?: number) {
            return queue(async () => {
                if (!owns(token, sender)) throw new Error('Calibration session expired.');
                if (offset !== undefined) {
                    if (!Number.isFinite(offset) || Math.abs(offset) > 1000 || identity() !== session.route)
                        throw new Error('Output changed or calibration is invalid. Start again.');
                    // A failed save leaves the session open for Retry or Cancel.
                    if (!deps.save(session.profile.output.key, offset)) throw new Error('Could not save calibration.');
                }
                const profile = deps.profile();
                const restored = await restore();
                return {profile, restored};
            });
        },
        abandon(sender?: number) {
            return queue(() => session && (sender === undefined || session.sender === sender) ? restore() : true);
        },
        expire() {
            if (session && now() - session.touched > 5000) return queue(() => session && now() - session.touched > 5000 ? restore() : true);
            return Promise.resolve(true);
        },
    };
}

// Authored calibration signal: three short, gently enveloped clicks separated
// by 3 seconds, with enough leading/trailing silence for +/-1000ms correction.
export function calibrationWave(): Buffer {
    const sr = 48000, frames = sr * 10, out = Buffer.alloc(44 + frames * 2);
    out.write('RIFF'); out.writeUInt32LE(out.length - 8, 4); out.write('WAVEfmt ', 8);
    out.writeUInt32LE(16, 16); out.writeUInt16LE(1, 20); out.writeUInt16LE(1, 22);
    out.writeUInt32LE(sr, 24); out.writeUInt32LE(sr * 2, 28); out.writeUInt16LE(2, 32);
    out.writeUInt16LE(16, 34); out.write('data', 36); out.writeUInt32LE(frames * 2, 40);
    for (const second of [2, 5, 8]) for (let i = 0; i < 720; i++) {
        const envelope = Math.min(1, i / 24) * Math.exp(-i / 140);
        out.writeInt16LE(Math.round(15000 * envelope * Math.sin(2 * Math.PI * 1600 * i / sr)), 44 + (second * sr + i) * 2);
    }
    return out;
}

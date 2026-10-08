import * as fs from 'fs';
import * as path from 'path';

type Profile = { offsetMs: number; checked: boolean };
type Store = { version: 2; legacyAvMs?: number; output: Record<string, Profile>; input: Record<string, Profile> };
export function calibrationRoute(device: any, direction: 'input' | 'output', channel = -1) {
    if (!device) return null;
    const backend = device[direction + 'Type'] || device.type;
    const endpoint = device[direction + 'Id'] || device[direction];
    const sampleRate = Number(device[direction + 'SampleRate'] || device.sampleRate);
    const blockSize = Number(device[direction + 'BlockSize'] || device.blockSize);
    if (typeof backend !== 'string' || !backend || typeof endpoint !== 'string' || !endpoint
        || !Number.isFinite(sampleRate) || sampleRate < 8000 || sampleRate > 384000
        || !Number.isInteger(blockSize) || blockSize < 1 || blockSize > 16384) return null;
    // JUCE's portable public device API exposes names, not stable endpoint IDs.
    // Names are exact (no lossy normalization); rename/format change gets a new
    // profile. Default is resolved by getCurrentDevice before reaching here.
    const identity = device[direction + 'Id'] ? 'endpoint-id' : 'exact-device-name';
    const parts: any[] = [2, direction, backend, identity, endpoint, sampleRate, blockSize, direction === 'input' ? channel : null];
    const persistent = !!device[direction + 'Id'] || device[direction + 'Ambiguous'] !== true;
    if (!persistent) parts.push(device.routeGeneration);
    return { key: JSON.stringify(parts), persistent,
        label: `${device[direction] || endpoint} · ${backend} · ${sampleRate} Hz · ${blockSize} samples`, identity };
}

export function createCalibrationStore(filename: string) {
    let store: Store = { version: 2, output: {}, input: {} };
    try {
        const parsed = JSON.parse(fs.readFileSync(filename, 'utf8'));
        if (parsed.version === 2) {
            for (const direction of ['output', 'input'] as const) {
                for (const [key, value] of Object.entries(parsed[direction] || {}).slice(0, 256)) {
                    const p = value as Profile;
                    if (typeof key === 'string' && key.length <= 4096 && p && Number.isFinite(p.offsetMs)
                        && p.offsetMs >= (direction === 'output' ? -1000 : 0) && p.offsetMs <= (direction === 'output' ? 1000 : 250))
                        store[direction][key] = { offsetMs: p.offsetMs, checked: p.checked === true };
                }
            }
            if (Number.isFinite(parsed.legacyAvMs)) store.legacyAvMs = parsed.legacyAvMs;
        }
    } catch (error: any) { if (error.code !== 'ENOENT') console.warn('[audio] Calibration settings unavailable:', error.message); }
    function persist(next: Store) {
        fs.mkdirSync(path.dirname(filename), { recursive: true });
        const temporary = filename + '.tmp';
        fs.writeFileSync(temporary, JSON.stringify(next, null, 2), 'utf8');
        fs.renameSync(temporary, filename);
        store = next;
    }
    return {
        read(device: any, legacyAvMs?: number, channel = -1, legacyInputMs?: number) {
            if (store.legacyAvMs === undefined && Number.isFinite(legacyAvMs))
                persist({ ...store, legacyAvMs: Math.max(-1000, Math.min(1000, legacyAvMs!)) });
            const output = calibrationRoute(device, 'output'), input = calibrationRoute(device, 'input', channel);
            if (input?.persistent && !Object.keys(store.input).length && Number.isFinite(legacyInputMs)
                && legacyInputMs! >= 0 && legacyInputMs! <= 250)
                persist({ ...store, input: { [input.key]: { offsetMs: legacyInputMs!, checked: false } } });
            return { version: 2, legacyAvMs: store.legacyAvMs,
                output: output ? { ...output, ...(store.output[output.key] || { offsetMs: 0, checked: false }) } : null,
                input: input ? { ...input, ...(store.input[input.key] || { offsetMs: 80, checked: false }) } : null };
        },
        save(direction: 'input' | 'output', key: string, offsetMs: number) {
            if (!['input', 'output'].includes(direction) || typeof key !== 'string' || key.length > 4096
                || !Number.isFinite(offsetMs) || offsetMs < (direction === 'output' ? -1000 : 0)
                || offsetMs > (direction === 'output' ? 1000 : 250)) return false;
            // Only keys built from a resolved route are accepted, never arbitrary
            // object properties supplied by a renderer.
            let parts; try { parts = JSON.parse(key); } catch { return false; }
            if (!Array.isArray(parts) || parts.length !== 8 || parts[0] !== 2 || parts[1] !== direction) return false;
            const profiles = { ...store[direction], [key]: { offsetMs, checked: true } };
            if (Object.keys(profiles).length > 256) return false;
            persist({ ...store, [direction]: profiles }); return true;
        },
    };
}

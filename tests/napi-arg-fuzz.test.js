// Phase 6 gate (docs/audio-engine-tlc.md §5, deep-read §2): table-driven
// argument fuzz against the real addon. Every chain-mutating binding must
// treat NaN/Infinity/negative/string/missing/object arguments as a clean
// no-op — historically Int32Value() coerced NaN → 0 and mutated SLOT 0.
// Quarantined behind the addon being built (CI native lane), auto-skips
// otherwise. Uses no audio device (engine constructed, never started).
'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');

const ADDON = path.join(__dirname, '..', 'build', 'Release', 'slopsmith_audio.node');
const HAVE_ADDON = fs.existsSync(ADDON);

function writeImpulseWav(file) {
    const buf = Buffer.alloc(44 + 128);
    buf.write('RIFF', 0); buf.writeUInt32LE(36 + 128, 4); buf.write('WAVE', 8);
    buf.write('fmt ', 12); buf.writeUInt32LE(16, 16); buf.writeUInt16LE(1, 20);
    buf.writeUInt16LE(1, 22); buf.writeUInt32LE(48000, 24); buf.writeUInt32LE(96000, 28);
    buf.writeUInt16LE(2, 32); buf.writeUInt16LE(16, 34);
    buf.write('data', 36); buf.writeUInt32LE(128, 40);
    buf.writeInt16LE(32767, 44);
    fs.writeFileSync(file, buf);
}

const GARBAGE = [NaN, Infinity, -Infinity, -1, 1.5, 4097, 'x', null, undefined, {}, []];

test('chain-mutating bindings no-op on garbage args and never touch slot 0', { skip: !HAVE_ADDON && 'addon not built' }, async () => {
    const audio = require(ADDON);
    audio.init();
    const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'arg-fuzz-'));
    const ir = path.join(tmp, 'i.wav');
    writeImpulseWav(ir);
    try {
        const res = await audio.loadPreset(JSON.stringify({
            chain: [{ type: 2, name: 'fuzz-anchor', path: ir, bypassed: false }],
        }));
        assert.ok(res?.success, 'anchor preset must load');
        const before = JSON.stringify(audio.getChainState());

        for (const g of GARBAGE) {
            audio.setBypass(g, true);
            audio.setBypass(0 /* valid id shape */, g);
            audio.removeProcessor(g);
            audio.moveProcessor(g, 0);
            audio.moveProcessor(0, g);
            audio.setParameter(g, 0, 0.5);
            audio.setParameter(1, g, 0.5);
            audio.setParameter(1, 0, g);
            audio.setPan?.(g, 0);
            audio.sendMidiToSlot(g, 0, 1, 0);
            audio.sendMidiToSlot(1, g, 1, 0);
            audio.sendMidiToSlot(1, 0, g, 0);
            audio.sendMidiToSlot(1, 0, 1, g);
            audio.setMultiBypass([{ slotId: g, bypassed: true }, g, null]);
            audio.setGain('output', g);
            audio.setGain(g, 1);
        }

        const after = JSON.stringify(audio.getChainState());
        assert.equal(after, before, 'garbage args must not mutate any slot');
        // Sanity: a VALID call still works after the fuzz storm.
        audio.setBypass(1, true);
        const st = audio.getChainState();
        assert.equal(st[0]?.bypassed, true, 'valid call after fuzz must apply');
        audio.setBypass(1, false);
    } finally {
        await audio.clearChain?.();
        audio.shutdown?.();
        fs.rmSync(tmp, { recursive: true, force: true });
    }
});

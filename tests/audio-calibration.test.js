const {test} = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs'), os = require('node:os'), path = require('node:path');
const {loadTs} = require('./_load-ts');
const {createCalibrationStore, calibrationRoute} = loadTs('src/main/audio-calibration.ts');
const {resolveVerifierTiming} = loadTs('src/main/verifier-timing.ts');

test('output profiles survive restart, resolve actual endpoints, and keep legacy evidence and input separate', () => {
    const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'feedback-calibration-'));
    const file = path.join(dir, 'profiles.json');
    try {
        const store = createCalibrationStore(file);
        const asio = {inputType: 'ASIO', outputType: 'ASIO', input: 'Interface', output: 'Interface', sampleRate: 48000, blockSize: 128};
        const tv = {...asio, outputType: 'Windows Audio', output: 'TV', outputBlockSize: 480};
        const a = store.read(asio, -100), b = store.read(tv);
        assert.equal(a.legacyAvMs, -100); assert.equal(a.output.offsetMs, 0); assert.equal(a.output.checked, false);
        assert.notEqual(a.output.key, b.output.key); assert.equal(a.input.key, b.input.key);
        assert.equal(store.save('output', a.output.key, -5), true);
        assert.equal(store.save('output', b.output.key, -100), true);
        assert.equal(store.save('input', a.input.key, 2), true);
        const restored = createCalibrationStore(file);
        assert.equal(restored.read(asio).output.offsetMs, -5);
        assert.equal(restored.read(tv).output.offsetMs, -100);
        assert.equal(restored.read(tv).input.offsetMs, 2);
        assert.equal(restored.read({...tv, outputBlockSize: 960}).output.offsetMs, 0);
        assert.notEqual(calibrationRoute({...tv, output: 'New default TV'}, 'output').key, b.output.key);
        assert.equal(calibrationRoute({...tv, output: ''}, 'output'), null);
        assert.equal(store.save('input', a.output.key, 2), false);
        assert.equal(store.save('output', a.output.key, NaN), false);
    } finally { fs.rmSync(dir, {recursive: true, force: true}); }
});

for (const rate of [.5, .75, 1, 1.5, 2]) for (const av of [0, -100, -500, 100])
test(`native scoring separates 2ms input from ${av}ms output correction at ${rate}x`, () => {
    const nowPosition = 10 + (2 - av) * rate / 1000;
    const audio = {getBackingSnapshot: () => ({valid: true, rate, generation: 4,
        presentation: {version: 2, routeGeneration: 3, position: nowPosition, playing: true}})};
    const timing = {version: 2, generation: 4, routeGeneration: 3, avOffsetMs: av, songOffset: 0, inputAgeMs: 2};
    const result = resolveVerifierTiming(audio, 8, true, rate, timing); // intentionally old renderer position
    assert.ok(Math.abs(result.songTime - 10) < 1e-10); assert.equal(result.playing, true);
    assert.equal(resolveVerifierTiming(audio, 8, true, rate, {...timing, generation: 2}).playing, false);
    assert.equal(resolveVerifierTiming(audio, 8, false, rate, timing).playing, false);
});

test('ambiguous names cannot persist a calibration onto an indistinguishable endpoint', () => {
    const device = {type: 'Windows Audio', output: 'Speakers', sampleRate: 48000, blockSize: 256, outputAmbiguous: true, routeGeneration: 1};
    const a = calibrationRoute(device, 'output');
    assert.equal(a.persistent, false);
    assert.notEqual(a.key, calibrationRoute({...device, routeGeneration: 2}, 'output').key);
    assert.equal(calibrationRoute({...device, outputId: 'endpoint-1'}, 'output').persistent, true);
});

test('judgments remain active through calibrated output tail after native EOF', () => {
    let position = 10.2;
    const audio = {getBackingSnapshot: () => ({valid:true, ended:true, position:10, rate:1, generation:1,
        presentation:{version:2,routeGeneration:1,position,playing:false}})};
    const timing = {version:2,generation:1,routeGeneration:1,songOffset:0,avOffsetMs:-500,inputAgeMs:2};
    const result = resolveVerifierTiming(audio,10,true,1,timing);
    assert.equal(result.playing,true); assert.ok(Math.abs(result.songTime - 9.698) < 1e-10);
    position = 10.6;
    assert.equal(resolveVerifierTiming(audio,10,true,1,timing).playing,false);
});

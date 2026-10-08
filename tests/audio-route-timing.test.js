const { test } = require('node:test');
const assert = require('node:assert/strict');
const { loadTs } = require('./_load-ts');
const { readAudioRouteTiming } = loadTs('src/main/audio-route-timing.ts');

test('diagnostics separate native estimates from transport and unmeasured end-to-end latency', () => {
    const device = { outputType: 'Windows Audio', sampleRate: 48000, inputBlockSize: 256, outputBlockSize: 480 };
    const bus = { enabled: true, fillFrames: 960, consumedFrames: 4000, underflowCount: 3 };
    const estimates = { rendererBusMs: 20, monitorTotalMs: 120 };
    const transport = { captureEpoch: 2, sourceToReceiptMs: null };
    const native = { isAudioRunning: () => true, getCurrentDevice: () => device,
        getRendererBusMetrics: () => bus, getLatencyBreakdown: () => estimates };
    const result = readAudioRouteTiming(native, transport, 4, () => 1234);
    assert.equal(result.schemaVersion, 1); assert.equal(result.sampledAtMs, 1234);
    assert.equal(result.transport, transport); assert.equal(result.device, device);
    assert.equal(result.rendererBus, bus); assert.equal(result.nativeLatencyEstimate, estimates);
    assert.equal(result.rendererBusControlEpoch, 4); assert.equal(result.measuredEndToEndMs, null);
    assert.equal(result.snapshotConsistency, 'sequential-non-atomic'); assert.deepEqual(result.errors, []);
});

test('missing addon reports unavailable, never zero latency', () => {
    const result = readAudioRouteTiming(null, null, 0);
    for (const key of ['running', 'device', 'rendererBus', 'nativeLatencyEstimate', 'measuredEndToEndMs']) assert.equal(result[key], null);
});

test('stopped engine does not publish stale device latency', () => {
    const result = readAudioRouteTiming({ isAudioRunning: () => false,
        getCurrentDevice: () => { throw Error('must not read'); },
        getLatencyBreakdown: () => { throw Error('must not read'); } }, null, 0);
    assert.equal(result.running, false); assert.equal(result.device, null);
    assert.equal(result.nativeLatencyEstimate, null); assert.deepEqual(result.errors, []);
});

test('a failed diagnostic method does not suppress other metrics or expose exception data', () => {
    const result = readAudioRouteTiming({ isAudioRunning: () => true,
        getCurrentDevice: () => { throw Error('private data'); },
        getRendererBusMetrics: () => ({ enabled: true }) }, null, 0);
    assert.equal(result.device, null); assert.equal(result.rendererBus.enabled, true);
    assert.deepEqual(result.errors, ['getCurrentDevice']);
});

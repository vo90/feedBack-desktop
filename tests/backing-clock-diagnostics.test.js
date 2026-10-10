const {test} = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const Module = require('node:module');
const vm = require('node:vm');
const {EventEmitter} = require('node:events');
const ts = require('typescript');

function load(name) {
    const filename = path.join(__dirname, '../src/main/', name + '.ts');
    const compiled = ts.transpileModule(fs.readFileSync(filename, 'utf8'), {
        compilerOptions: {module: ts.ModuleKind.CommonJS, target: ts.ScriptTarget.ES2022},
    }).outputText;
    const mod = new Module(filename, module);
    mod.filename = filename; mod.paths = Module._nodeModulePaths(path.dirname(filename));
    mod._compile(compiled, filename);
    return mod.exports;
}
const {createClockTrace, createBackingClockTraceRegistry, clockSnapshotFields} = load('backing-clock-diagnostics');
const {createBackingClockPublisher} = load('backing-clock');
const sender = (id = 1) => Object.assign(new EventEmitter(), {id, isDestroyed: () => false});
const sample = {valid: true, sequence: 47, generation: 22, position: 123.5,
    ageMs: 1.2, readAtMs: 101, readUncertaintyMs: .01, clockId: 8000};

test('trace is off by default, bounded, chronological, retained on stop and reset on restart', () => {
    const trace = createClockTrace(3);
    trace.record({n: 0}); assert.equal(trace.read().total, 0);
    trace.setEnabled(true);
    const capture = trace.capture;
    for (let n = 0; n < 5; n++) trace.record({n});
    const result = trace.read();
    assert.equal(result.total, 5); assert.equal(result.dropped, 2);
    assert.deepEqual(result.rows, [{n: 2}, {n: 3}, {n: 4}]);
    result.rows[0].n = -1;
    assert.equal(trace.read().rows[0].n, 2, 'pull cannot change retained observations');
    trace.setEnabled(false); trace.record({n: 5});
    assert.equal(trace.read().total, 5);
    trace.setEnabled(true); trace.record({n: 6}, capture);
    assert.equal(trace.read().total, 0, 'late responses from a previous capture are ignored');
    trace.record({n: 7}); assert.deepEqual(trace.read().rows, [{n: 7}]);
    for (const capacity of [0, 4097, 1.5, Infinity]) assert.throws(() => createClockTrace(capacity), RangeError);
});

test('main poll tracing preserves results, isolates renderers and does no clock work while disabled', () => {
    let now = 100, clockReads = 0;
    const registry = createBackingClockTraceRegistry({timeOrigin: 8000, now: () => {clockReads++; return now;}});
    const a = sender(), b = sender(2);
    const read = () => {now += 2; return sample;};
    assert.equal(registry.read(a, read), sample); assert.equal(clockReads, 0);
    assert.equal(registry.setEnabled(a, 'yes'), false);
    assert.equal(registry.setEnabled(a, true), true);
    assert.equal(registry.read(b, read), sample); assert.equal(clockReads, 0);
    assert.equal(registry.read(a, read), sample);
    assert.deepEqual(registry.snapshot(a).rows, [{kind: 'poll-read', startedAtMs: 104, endedAtMs: 106,
        failed: false, ...clockSnapshotFields(sample)}]);
    assert.equal(registry.snapshot(b).total, 0);
    const error = new Error('native read failed');
    assert.throws(() => registry.read(a, () => {now += 8; throw error;}), value => value === error);
    assert.equal(registry.snapshot(a).rows[1].failed, true);
    assert.equal(registry.snapshot(a).rows[1].endedAtMs, 114);
    registry.setEnabled(a, false); const reads = clockReads;
    registry.read(a, read); assert.equal(clockReads, reads);
    assert.equal(registry.snapshot(a).total, 2);
});

for (const event of ['destroyed', 'render-process-gone', 'navigation']) {
    test(`main trace releases retained rows and all listeners on ${event}`, () => {
        const registry = createBackingClockTraceRegistry(), target = sender();
        registry.setEnabled(target, true); registry.forSender(target).record({n: 1});
        registry.setEnabled(target, true);
        assert.equal(target.listenerCount('destroyed'), 1, 'restart does not accumulate lifecycle listeners');
        if (event === 'navigation') {
            target.emit('did-start-navigation', {}, 'url', true, true);
            target.emit('did-start-navigation', {}, 'url', false, false);
            assert.ok(registry.forSender(target));
            target.emit('did-start-navigation', {}, 'url', false, true);
        } else target.emit(event);
        assert.equal(registry.forSender(target), undefined);
        assert.equal(target.eventNames().length, 0);
    });
}

test('publisher separates late timer entry, slow native read and send cost without adding messages', () => {
    let now = 100, readCost = 2, seq = 1, tick, clockReads = 0;
    const clock = {timeOrigin: 8000, now: () => {clockReads++; return now;}};
    const trace = createClockTrace(), target = sender(), messages = [];
    target.send = (...args) => {messages.push(args); now += 3;};
    const publisher = createBackingClockPublisher(() => {
        now += readCost; return {...sample, sequence: seq++, readAtMs: now - readCost / 2};
    }, callback => {tick = callback; return {unref() {}};}, () => {}, () => trace, clock);
    publisher.start(target, 1);
    assert.equal(clockReads, 0); assert.equal(trace.read().total, 0);
    trace.setEnabled(true); now = 150; tick();
    now = 400; tick();
    now = 450; readCost = 220; tick();
    const [first, late, slow] = trace.read().rows;
    assert.equal(first.expectedAtMs, null);
    assert.equal(late.startedAtMs - late.expectedAtMs, 200, 'late event-loop entry remains distinct from read duration');
    assert.equal(late.readEndedAtMs - late.startedAtMs, 2);
    assert.equal(slow.startedAtMs - slow.expectedAtMs, 0);
    assert.equal(slow.readEndedAtMs - slow.startedAtMs, 220);
    assert.equal(slow.endedAtMs - slow.dispatchAtMs, 3);
    assert.equal(messages.length, 4, 'one existing stream packet per valid native read');
    assert.deepEqual(messages.at(-1), ['audio:backingSnapshot', 1,
        {...sample, sequence: 4, readAtMs: 560}]);
    trace.setEnabled(true); now = 800; tick();
    assert.equal(trace.read().rows[0].expectedAtMs, null, 'a new capture has no inherited timer deadline');
    trace.setEnabled(false); const reads = clockReads; tick(); assert.equal(clockReads, reads);
    publisher.stop(target, 1);
});

function preloadHarness() {
    const source = fs.readFileSync(path.join(__dirname, '../src/main/preload.ts'), 'utf8');
    const start = source.indexOf('        setBackingClockDiagnostics:');
    const end = source.indexOf('        getBackingDuration:', start);
    assert.ok(start > 0 && end > start);
    const code = ts.transpileModule('let backingSnapshotToken = 0; var api = {' + source.slice(start, end) + '};', {
        compilerOptions: {target: ts.ScriptTarget.ES2022},
    }).outputText;
    let now = 10;
    const ipcRenderer = new EventEmitter(), sent = [], invoked = [];
    const trace = createClockTrace();
    ipcRenderer.send = (...args) => sent.push(args);
    const h = {trace, ipcRenderer, sent, invoked, now(value) {now = value;}, reply: () => Promise.resolve(sample)};
    ipcRenderer.invoke = (...args) => {
        invoked.push(args);
        if (args[0] === 'audio:setBackingClockDiagnostics') return Promise.resolve(true);
        if (args[0] === 'audio:getBackingClockDiagnostics') return Promise.resolve({clockId: 8000, rows: []});
        return h.reply();
    };
    const context = vm.createContext({ipcRenderer, backingClockTrace: trace, clockSnapshotFields,
        performance: {timeOrigin: 9000, now: () => now}});
    vm.runInContext(code, context); h.api = context.api;
    return h;
}

test('preload preserves stream tokens and payload while recording receipt and callback bounds', async () => {
    const h = preloadHarness(), received = [];
    await h.api.setBackingClockDiagnostics(true);
    const stop = h.api.subscribeBackingSnapshots(value => {received.push(value); h.now(14);});
    const queued = h.ipcRenderer.listeners('audio:backingSnapshot')[0];
    h.ipcRenderer.emit('audio:backingSnapshot', {}, 1, sample);
    h.ipcRenderer.emit('audio:backingSnapshot', {}, 999, sample);
    stop(); queued({}, 1, sample);
    assert.equal(received.length, 1); assert.equal(received[0], sample);
    const rows = h.trace.read().rows;
    assert.deepEqual(rows.map(row => row.acceptedToken), [true, false, false]);
    assert.equal(rows[0].receivedAtMs, 10); assert.equal(rows[0].callbackEndedAtMs, 14);
    assert.equal(rows[0].sequence, sample.sequence); assert.equal(rows[0].readAtMs, sample.readAtMs);
    assert.equal(h.sent.length, 2, 'diagnostic observations do not emit extra IPC');
    await h.api.setBackingClockDiagnostics(false);
    const result = await h.api.getBackingClockDiagnostics();
    assert.equal(result.renderer.clockId, 9000); assert.equal(result.main.clockId, 8000);
    assert.equal(result.renderer.total, 3); assert.equal(result.renderer.enabled, false);
});

test('preload poll tracing preserves errors and drops late responses across capture boundaries', async () => {
    const h = preloadHarness();
    const original = Promise.resolve(sample); h.reply = () => original;
    assert.equal(h.api.getBackingSnapshot(), original, 'disabled path returns the original IPC promise');
    await h.api.setBackingClockDiagnostics(true);
    let resolve; h.reply = () => new Promise(done => {resolve = done;});
    h.now(20); const pending = h.api.getBackingSnapshot(); h.now(35); resolve(sample);
    assert.equal(await pending, sample);
    assert.equal(h.trace.read().rows[0].sentAtMs, 20);
    assert.equal(h.trace.read().rows[0].receivedAtMs, 35);
    const old = h.api.getBackingSnapshot();
    await h.api.setBackingClockDiagnostics(false); await h.api.setBackingClockDiagnostics(true);
    resolve(sample); assert.equal(await old, sample); assert.equal(h.trace.read().total, 0);
    const error = new Error('IPC failed'); h.reply = () => Promise.reject(error);
    await assert.rejects(h.api.getBackingSnapshot(), value => value === error);
    assert.equal(h.trace.read().rows[0].kind, 'poll-error');
    assert.equal(await h.api.setBackingClockDiagnostics('bad'), false);
});

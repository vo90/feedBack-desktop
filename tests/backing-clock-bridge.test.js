const {test} = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const Module = require('node:module');
const vm = require('node:vm');
const {EventEmitter} = require('node:events');
const ts = require('typescript');
const helperPath = path.join(__dirname, '../src/main/backing-clock.ts');
const compiled = ts.transpileModule(fs.readFileSync(helperPath, 'utf8'), {
    compilerOptions: {module: ts.ModuleKind.CommonJS, target: ts.ScriptTarget.ES2022},
}).outputText;
const mod = new Module(helperPath, module);
mod.filename = helperPath; mod.paths = Module._nodeModulePaths(path.dirname(helperPath));
mod._compile(compiled, helperPath);
const {readBackingSnapshot, createBackingClockPublisher} = mod.exports;
const source = fs.readFileSync(path.join(__dirname, '../src/main/audio-bridge.ts'), 'utf8');
const handler = source.match(/ipcMain\.handle\('audio:getBackingSnapshot', (.+)\);/)[1];
const read = (audio, clock) => new Function('audio', 'readBackingSnapshot', `return (${handler})();`)(
    audio, source => readBackingSnapshot(source, clock));
test('snapshot capability is null before engine load and for downlevel addons', () => {
    assert.equal(read(null), null);
    assert.equal(read({getBackingPosition: () => 42}), null);
});
test('snapshot IPC preserves the native observation and brackets its read in the main clock', () => {
    const sample = {version: 1, valid: true, position: 42, ageMs: 3.5,
        sequence: 7, generation: 2, rate: .5, playing: true, ended: false};
    let calls = 0;
    const readings = [100, 104];
    const result = read({getBackingSnapshot() { calls++; return sample; }},
        {now: () => readings.shift(), timeOrigin: 5000});
    assert.deepEqual(result, {...sample, clockId: 5000, readAtMs: 102, readUncertaintyMs: 2});
    assert.equal(sample.readAtMs, undefined, 'the native payload is not modified');
    assert.equal(calls, 1);
});

test('null native observations remain a capability fallback', () => {
    assert.equal(read({getBackingSnapshot: () => null}), null);
});

function publisherHarness() {
    const timers = new Set(), messages = [];
    const sender = Object.assign(new EventEmitter(), {id: 1, isDestroyed: () => false,
        send: (...args) => messages.push(args)});
    const publisher = createBackingClockPublisher(() => ({valid: true, sequence: 1}),
        (fn, ms) => {assert.equal(ms, 50); const t = {fn, unref() {}}; timers.add(t); return t;},
        t => timers.delete(t));
    return {publisher, sender, timers, messages};
}

test('clock stream has one publisher and an obsolete token cannot stop or replace it', () => {
    const h = publisherHarness(); h.publisher.start(h.sender, 1); h.publisher.start(h.sender, 2);
    assert.equal(h.timers.size, 1); assert.equal(h.messages.at(-1)[1], 2);
    h.publisher.start(h.sender, 1); h.publisher.stop(h.sender, 1);
    assert.equal(h.timers.size, 1);
    h.publisher.stop(h.sender, 2); assert.equal(h.timers.size, 0);
    assert.equal(h.sender.eventNames().length, 0);
});

for (const event of ['destroyed', 'render-process-gone', 'navigation']) {
    test(`clock publisher releases its timer and listeners on ${event}`, () => {
        const h = publisherHarness(); h.publisher.start(h.sender, 1);
        if (event === 'navigation') {
            h.sender.emit('did-start-navigation', {}, 'url', true, true);
            assert.equal(h.timers.size, 1, 'in-page navigation preserves the subscription');
            h.sender.emit('did-start-navigation', {}, 'url', false, true);
        } else h.sender.emit(event);
        assert.equal(h.timers.size, 0); assert.equal(h.sender.eventNames().length, 0);
    });
}

test('preload filters subscription tokens and removes callbacks exactly once', () => {
    const preload = fs.readFileSync(path.join(__dirname, '../src/main/preload.ts'), 'utf8');
    const start = preload.indexOf('        subscribeBackingSnapshots:');
    const end = preload.indexOf('        getBackingDuration:', start);
    assert.ok(start > 0 && end > start);
    const code = ts.transpileModule('let backingSnapshotToken = 0; var api = {' + preload.slice(start, end) + '};', {
        compilerOptions: {target: ts.ScriptTarget.ES2022},
    }).outputText;
    const ipcRenderer = new EventEmitter(), sent = [], observations = [];
    ipcRenderer.send = (...args) => sent.push(args);
    const context = vm.createContext({ipcRenderer}); vm.runInContext(code, context);
    const stop = context.api.subscribeBackingSnapshots(value => observations.push(value));
    const queued = ipcRenderer.listeners('audio:backingSnapshot')[0];
    ipcRenderer.emit('audio:backingSnapshot', {}, 999, 'wrong');
    ipcRenderer.emit('audio:backingSnapshot', {}, 1, 'right');
    stop(); stop(); queued({}, 1, 'late');
    assert.deepEqual(observations, ['right']);
    assert.equal(ipcRenderer.listenerCount('audio:backingSnapshot'), 0);
    const nextStop = context.api.subscribeBackingSnapshots(() => {});
    nextStop();
    assert.deepEqual(sent, [['audio:subscribeBackingSnapshots', 1], ['audio:unsubscribeBackingSnapshots', 1],
        ['audio:subscribeBackingSnapshots', 2], ['audio:unsubscribeBackingSnapshots', 2]]);
});

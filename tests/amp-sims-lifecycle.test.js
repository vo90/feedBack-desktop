const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const screen = fs.readFileSync(path.join(__dirname, '../src/renderer/screen.js'), 'utf8');
const tick = () => new Promise(resolve => setImmediate(resolve));

function sourceFunction(name) {
    const match = new RegExp(`(?:async )?function ${name}\\(`).exec(screen);
    assert.ok(match, name);
    let cursor = screen.indexOf('(', match.index) + 1, parentheses = 1;
    while (parentheses) { if (screen[cursor] === '(') parentheses++; if (screen[cursor] === ')') parentheses--; cursor++; }
    const open = screen.indexOf('{', cursor);
    let depth = 1, end = open + 1;
    while (depth) { if (screen[end] === '{') depth++; if (screen[end] === '}') depth--; end++; }
    return screen.slice(match.index, end);
}

function fixture({ enabled = true, ready = true } = {}) {
    let state = { applicable: true, ready, enabled, generation: 1 };
    const calls = [];
    let finishLoad;
    const native = {
        loadPreset: () => { calls.push('load'); return new Promise(resolve => { finishLoad = resolve; }); },
        clearChain: () => { calls.push('clear'); return true; },
        startAudio: () => { calls.push('start'); return true; },
        setInputChannel: () => { calls.push('channel'); },
        getBackingState: () => ({ playing: true }),
    };
    const context = vm.createContext({ nativeAudio: native, snapshot: () => state, calls,
        window: { _aeStopToneMonitor: () => calls.push('stop-poll'),
            _aeEndChainRebuildGuard: () => calls.push('release-mute-hold') },
        stopToneMonitor() {}, clearInterval() {}, clearTimeout() {},
        hookState: {}, ampSimsCheckbox: {}, ampPanelReady: true, ampWasEnabled: true,
        toneSwitcher: {}, _renderEmptyChain: () => calls.push('render-empty'),
        loadDefaultPreset: async () => { calls.push('default'); return false; },
        aeRestoreSavedChain: async () => calls.push('restore-saved'),
        getCurrentSongKey: () => '', applyToneMappingsNow: async () => calls.push('map'),
    });
    vm.runInContext(sourceFunction('createDesktopAmpSimGuard') + '\n'
        + 'const ampGuard = createDesktopAmpSimGuard(nativeAudio, snapshot);\n'
        + sourceFunction('aeDrainAmpSims') + '\n' + sourceFunction('aeClearAmpSims') + '\n'
        + sourceFunction('aeSuspendAmpSims') + '\n' + sourceFunction('aeAmpSimsChanged'), context);
    return { calls, context, guard: vm.runInContext('ampGuard', context),
        state: () => state,
        set(next) { state = { ...state, ...next, generation: state.generation + 1 }; },
        finishLoad: value => finishLoad(value),
        async change() {
            await context.aeAmpSimsChanged(state);
            if (state.applicable && (!state.ready || !state.enabled)) await context.aeClearAmpSims(state);
        },
    };
}

test('OFF waits for an issued native load and clears afterward without touching audio route or backing', async () => {
    const f = fixture();
    const result = f.guard.audio(f.state().generation).loadPreset('saved-tone');
    const cancelled = assert.rejects(result, { code: 'AMP_SIMS_CANCELLED' });
    await tick();
    f.set({ enabled: false });
    let settled = false;
    const off = f.change().then(() => { settled = true; });
    await tick();
    assert.equal(settled, false);
    assert.equal(f.calls.includes('clear'), false);
    f.finishLoad({ success: true });
    await Promise.all([off, cancelled]);
    assert.deepEqual(f.calls, ['load', 'stop-poll', 'stop-poll', 'release-mute-hold', 'clear', 'render-empty']);
    assert.equal(f.guard.audio().getBackingState().playing, true);
});

test('OFF blocks all effect loading; a delayed old generation stays blocked after ON', async () => {
    const f = fixture();
    const stale = f.guard.audio(f.state().generation);
    f.set({ enabled: false });
    await f.change();
    await assert.rejects(stale.loadPreset('old'), { code: 'AMP_SIMS_CANCELLED' });
    f.set({ enabled: true });
    await f.change();
    await assert.rejects(stale.loadPreset('late-response'), { code: 'AMP_SIMS_CANCELLED' });
    assert.equal(f.calls.includes('load'), false);
    assert.equal(f.calls.filter(value => value === 'restore-saved').length, 1);
    await f.change();
    assert.equal(f.calls.filter(value => value === 'restore-saved').length, 1);
    const fresh = f.guard.audio(f.state().generation).loadPreset('new');
    await tick();
    f.finishLoad({ success: true });
    assert.equal((await fresh).success, true);
});

test('unknown preference is fail-closed; pure browser applicability stays compatible', async () => {
    const f = fixture({ ready: false, enabled: false });
    await assert.rejects(f.guard.audio().loadPreset('tone'), { code: 'AMP_SIMS_CANCELLED' });
    f.set({ applicable: false });
    assert.equal(f.guard.allowed(), true);
});

test('OFF also drains a whole delayed lookup before final clear; frozen bridge methods remain callable', async () => {
    const f = fixture();
    Object.freeze(f.context.nativeAudio);
    const generation = f.state().generation;
    let finishLookup;
    const lookup = new Promise(resolve => { finishLookup = resolve; });
    const work = f.guard.task(async () => {
        await lookup;
        return f.guard.audio(generation).loadPreset('late-lookup');
    })();
    const cancelled = assert.rejects(work, { code: 'AMP_SIMS_CANCELLED' });
    f.set({ enabled: false });
    let settled = false;
    const off = f.change().then(() => { settled = true; });
    await tick();
    assert.equal(settled, false);
    finishLookup();
    await Promise.all([off, cancelled]);
    assert.equal(f.calls.includes('load'), false);
    assert.equal(f.calls.filter(value => value === 'clear').length, 1);
});

test('startup OFF clears an inherited live chain without restoring or saving the user chain', async () => {
    const f = fixture({ enabled: false });
    f.context.ampWasEnabled = null;
    f.context.ampPanelReady = false;
    await f.change();
    assert.equal(f.calls.filter(value => value === 'clear').length, 1);
    assert.equal(f.calls.includes('restore-saved'), false);
    assert.equal(f.calls.includes('default'), false);
    // This is the actual suspension function: settings/chain persistence and
    // transport mutation are unavailable in the fixture and would throw.
    assert.equal(f.context.window._toneSwitcher, null);
});

test('a refused native clear is reported instead of claiming suspension succeeded', async () => {
    const f = fixture({ enabled: false });
    f.context.nativeAudio.clearChain = () => false;
    await assert.rejects(f.change(), /Unable to clear/);
    assert.equal(f.calls.includes('render-empty'), false);
});

for (const rejection of ['result', 'throw']) {
    test(`OFF during failed-preset rollback (${rejection}) preserves the saved chain`, async () => {
        const f = fixture();
        const saved = new Map([['slopsmith-signal-chain', 'user-preserved-chain']]);
        let rollbackEntered, releaseRollback;
        const entered = new Promise(resolve => { rollbackEntered = resolve; });
        f.context.nativeAudio.loadPreset = async () => {
            if (rejection === 'throw') throw new Error('load failed');
            return { success: false };
        };
        f.context.localStorage = { setItem: (key, value) => saved.set(key, value) };
        f.context.aeStripRigBuilderFromNativePreset = value => value;
        f.context._restorePresetBlob = async () => {
            rollbackEntered();
            await new Promise(resolve => { releaseRollback = resolve; });
        };
        vm.runInContext('const aeAmpSimAudio = () => ampGuard.audio(ampGuard.generation());\n'
            + sourceFunction('replaceChainWithPresetBlob'), f.context);
        const replacing = f.context.replaceChainWithPresetBlob({ nativePreset: '{"chain":[]}' }, 'test', { snapshot: false });
        await entered;
        f.set({ enabled: false });
        releaseRollback();
        assert.equal(await replacing, false);
        assert.equal(saved.get('slopsmith-signal-chain'), 'user-preserved-chain');
        assert.equal(f.calls.includes('render-empty'), false);
    });
}

test('Desktop identifies its Python backend for the existing amp-sim preference policy', () => {
    const python = fs.readFileSync(path.join(__dirname, '../src/main/python.ts'), 'utf8');
    assert.match(python, /\.\.\.process\.env as Record<string, string>,\s+FEEDBACK_DESKTOP: '1'/);
});

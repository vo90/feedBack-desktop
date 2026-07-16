// PR #112 review: syncSelectedInputSource() pushes the applied device into the
// audio-session capability's persisted selection, so a stale key can't re-open
// a dead device mid-session.
//
// The bug these cases pin: it treated an empty device name as "nameless device,
// invalidate the selection" and called removeItem(). But the input dropdown's
// FIRST option is `<option value="">Default</option>` — so "" is the ordinary
// "use the OS default" choice, not a nameless device. init()'s auto-apply calls
// this on every startup, so a user sitting on Default would have their
// capability selection (made in a DIFFERENT ui — the input_setup / tuner
// picker) deleted at each launch. And audioInputOpenHandler deliberately
// refuses to guess a device, so a cleared selection leaves plugins with no
// input at all — the same dead-guitar symptom the PR exists to fix.
//
// Also pins the key format against registerAudioSessionInputSources: the two
// sides must emit byte-identical keys or a synced selection resolves to
// nothing. They now share inputSourceNameKey() — these cases keep it that way.

'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');
const path = require('node:path');
const fs = require('node:fs');
const vm = require('node:vm');

const ROOT = path.join(__dirname, '..');
const SCREEN_JS = fs.readFileSync(path.join(ROOT, 'src', 'renderer', 'screen.js'), 'utf8');

function extractFunction(src, name) {
    const sig = `function ${name}(`;
    const start = src.indexOf(sig);
    assert.ok(start !== -1, `function '${name}' not found`);
    let i = src.indexOf('{', src.indexOf(')', start));
    let depth = 1;
    i++;
    while (i < src.length && depth > 0) {
        if (src[i] === '{') depth++;
        else if (src[i] === '}') depth--;
        i++;
    }
    assert.ok(depth === 0, `unbalanced braces in '${name}'`);
    return src.slice(start, i);
}

// `capability` decides what selectInputSource does: 'handled' (source resolved
// and the capability persisted it itself), 'degraded' (loaded, but this source
// isn't registered yet), 'throws', or 'absent' (capability not loaded at all).
function makeHarness({ capability = 'handled' } = {}) {
    const selectCalls = [];
    const storage = new Map();
    const storageOps = [];

    const selectInputSource = (query, requester) => {
        selectCalls.push({ query, requester });
        if (capability === 'throws') throw new Error('capability boom');
        if (capability === 'degraded') return { outcome: 'degraded', reason: 'Input source is unavailable' };
        return { outcome: 'handled' };
    };

    const ctx = {
        window: {
            slopsmith: capability === 'absent' ? {} : { audioSession: { selectInputSource } },
            localStorage: {
                setItem: (k, v) => { storageOps.push({ op: 'set', k, v }); storage.set(k, String(v)); },
                removeItem: (k) => { storageOps.push({ op: 'remove', k }); storage.delete(k); },
                getItem: (k) => (storage.has(k) ? storage.get(k) : null),
            },
        },
        console: { warn() {}, error() {} },
    };
    vm.createContext(ctx);
    vm.runInContext(
        extractFunction(SCREEN_JS, 'safeKeyPart')
        + '\n' + extractFunction(SCREEN_JS, 'inputSourceNameKey')
        + '\n' + extractFunction(SCREEN_JS, 'syncSelectedInputSource')
        + '\nglobalThis.__sync = syncSelectedInputSource;'
        + '\nglobalThis.__key = inputSourceNameKey;',
        ctx,
    );

    return {
        sync: (type, device) => ctx.__sync(type, device),
        key: (type, device) => ctx.__key(type, device),
        selectCalls,
        storageOps,
        stored: () => storage.get(STORAGE_KEY),
    };
}

const STORAGE_KEY = 'feedBack.audioInput.selectedLogicalSourceKey';

test('"Default" (empty device value) must not clear the capability selection', () => {
    const h = makeHarness();
    // The user's real pick, made in the input_setup / tuner picker.
    h.storageOps.length = 0;

    h.sync('ASIO', '');

    assert.deepEqual(h.storageOps, [], 'Default must not touch persisted storage');
    assert.deepEqual(h.selectCalls, [], 'Default must not re-select a source');
});

test('"Default" is not confused with a nameless device', () => {
    const h = makeHarness();
    // Whitespace-only is the same non-choice, and must not wipe either.
    h.sync('ASIO', '   ');
    h.sync('ASIO', null);
    h.sync('ASIO', undefined);

    assert.deepEqual(h.storageOps.filter((o) => o.op === 'remove'), [], 'nothing may be removed');
});

test('a named device selects through the capability, which persists it', () => {
    const h = makeHarness({ capability: 'handled' });

    h.sync('ASIO', 'M-Audio M-Track Solo');

    assert.equal(h.selectCalls.length, 1);
    assert.equal(h.selectCalls[0].query.logicalSourceKey,
        'desktop-audio:asio:input:name:M-Audio%20M-Track%20Solo');
    assert.equal(h.selectCalls[0].requester, 'audio_engine');
    // The capability owns persistence on the handled path — no direct write.
    assert.deepEqual(h.storageOps, [], 'handled select must not also write storage');
});

test('an unregistered source falls back to persisting the key directly', () => {
    const h = makeHarness({ capability: 'degraded' });

    h.sync('ASIO', 'M-Audio M-Track Solo');

    assert.equal(h.selectCalls.length, 1, 'capability is still tried first');
    assert.equal(h.stored(), 'desktop-audio:asio:input:name:M-Audio%20M-Track%20Solo');
});

test('a missing capability still persists the key', () => {
    const h = makeHarness({ capability: 'absent' });

    h.sync('ASIO', 'M-Audio M-Track Solo');

    assert.equal(h.selectCalls.length, 0, 'nothing to call');
    assert.equal(h.stored(), 'desktop-audio:asio:input:name:M-Audio%20M-Track%20Solo');
});

test('a throwing capability still persists the key', () => {
    const h = makeHarness({ capability: 'throws' });

    h.sync('ASIO', 'M-Audio M-Track Solo');

    assert.equal(h.stored(), 'desktop-audio:asio:input:name:M-Audio%20M-Track%20Solo');
});

test('key format survives characters that would break the handler parser', () => {
    const h = makeHarness();
    // The open handler splits on ':', so a name containing one must stay
    // encoded (%3A) or the key parses into the wrong device.
    const key = h.key('Windows Audio', 'Line 1: Focusrite (2- Scarlett)');
    assert.ok(!key.slice('desktop-audio:windows-audio:input:name:'.length).includes(':'),
        'raw ":" must not survive into the name segment');
    assert.equal(key, 'desktop-audio:windows-audio:input:name:'
        + encodeURIComponent('Line 1: Focusrite (2- Scarlett)'));
});

test('a nameless device yields no key — callers decide, this one does not guess', () => {
    const h = makeHarness();
    assert.equal(h.key('ASIO', ''), '');
    assert.equal(h.key('ASIO', '  '), '');
});

// The registration side (registerAudioSessionInputSources) and the selection
// side must build the key the same way forever. Sharing one builder is what
// guarantees that — so fail if a second CONSTRUCTION site reappears. (The
// literal also appears in audioInputOpenHandler's parser regex, which is a
// reader, not a builder — hence matching the interpolation specifically.)
test('the name-key is built in exactly one place', () => {
    const sites = SCREEN_JS.split(':input:name:${').length - 1;
    assert.equal(sites, 1,
        'the ":input:name:" key must only be built inside inputSourceNameKey()');
});

// Builder and parser are the two halves of the same contract: audioInputOpenHandler
// splits the key on ':' to recover the type and the encoded device name. A key
// this builder emits must round-trip through that parser back to the exact
// device name, or the handler resolves the wrong device (or none).
test('a built key round-trips through the open handler parser', () => {
    const h = makeHarness();
    // The handler's own regex, read out of the source rather than retyped, so
    // this fails if the two sides ever drift.
    const parserSrc = /const nameMatch = (\/\^desktop-audio.*?\/)\.exec\(source\)/.exec(SCREEN_JS);
    assert.ok(parserSrc, "could not find audioInputOpenHandler's nameMatch regex");
    const parser = eval(parserSrc[1]);

    for (const name of ['M-Audio M-Track Solo', 'Line 1: Focusrite (2- Scarlett)', 'Mikrofon (Realtek® Audio)']) {
        const key = h.key('Windows Audio', name);
        const m = parser.exec(key);
        assert.ok(m, `handler regex must match the key built for "${name}"`);
        assert.equal(m[1], 'windows-audio', 'type segment must survive');
        assert.equal(decodeURIComponent(m[2]), name, 'device name must round-trip exactly');
    }
});

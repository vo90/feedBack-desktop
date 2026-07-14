// Phase 0.a gate (docs/audio-engine-tlc.md §4): the public audio surface —
// addon exports, IPC channels, preload API keys — must not change during the
// decomposition phases. Removals/renames fail here; deliberate additions
// require regenerating the snapshots in the same commit:
//   node tests/contracts/extract.js
'use strict';

const test = require('node:test');
const assert = require('node:assert');
const fs = require('fs');
const path = require('path');

const { extractAddonExports, extractIpcChannels, extractPreloadApi } = require('./contracts/extract.js');

function loadSnapshot(name) {
    return JSON.parse(fs.readFileSync(path.join(__dirname, 'contracts', name), 'utf8'));
}

test('addon export table matches snapshot', (t) => {
    const current = extractAddonExports();
    if (!current) {
        t.skip('slopsmith_audio.node not built');
        return;
    }
    assert.deepStrictEqual(current, loadSnapshot('addon-exports.json'));
});

test('audio-bridge IPC channels match snapshot', () => {
    assert.deepStrictEqual(extractIpcChannels(), loadSnapshot('ipc-channels.json'));
});

test('preload audio/audioEffects API keys match snapshot', () => {
    assert.deepStrictEqual(extractPreloadApi(), loadSnapshot('preload-audio-api.json'));
});

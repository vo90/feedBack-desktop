const {test} = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const source = fs.readFileSync(path.join(__dirname, '../src/main/audio-bridge.ts'), 'utf8');
const handler = source.match(/ipcMain\.handle\('audio:getBackingSnapshot', (.+)\);/)[1];
const read = audio => new Function('audio', `return (${handler})();`)(audio);
test('snapshot capability is null before engine load and for downlevel addons', () => {
    assert.equal(read(null), null);
    assert.equal(read({getBackingPosition: () => 42}), null);
});
test('snapshot IPC forwards one coherent native observation without retimestamping', () => {
    const sample = {version: 1, valid: true, position: 42, ageMs: 3.5,
        sequence: 7, generation: 2, rate: .5, playing: true, ended: false};
    let calls = 0;
    assert.equal(read({getBackingSnapshot() { calls++; return sample; }}), sample);
    assert.equal(calls, 1);
});

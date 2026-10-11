const {test} = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');

// Exercise the actual startup block without importing Electron or starting it.
const source = fs.readFileSync(path.join(__dirname, '../src/main/main.ts'), 'utf8');
const match = source.match(/    if \(!app\.isPackaged && process\.env\.FEEDBACK_DEV_NO_DEVTOOLS !== '1' && !process\.env\.FEEDBACK_PAIRED_CONFIG\) \{\r?\n        mainWindow\.webContents\.openDevTools\(\{ mode: 'detach' \}\);\r?\n    \}/);
assert.ok(match, 'Expected one reviewed development DevTools startup block');
assert.equal((source.match(/mainWindow\.webContents\.openDevTools\(/g) || []).length, 1);
function startup(isPackaged, value, present = true, pairedConfig) {
    const calls = [];
    const env = present ? {FEEDBACK_DEV_NO_DEVTOOLS: value} : {};
    if (pairedConfig !== undefined) env.FEEDBACK_PAIRED_CONFIG = pairedConfig;
    vm.runInNewContext(match[0], {app: {isPackaged}, process: {env},
        mainWindow: {webContents: {openDevTools: options => calls.push(options.mode)}}});
    return calls;
}
test('ordinary developer launch still opens detached DevTools', () => {
    assert.deepEqual(startup(false, undefined, false), ['detach']);
    for (const value of ['', '0', 'true', ' 1', '1 ', 'invalid'])
        assert.deepEqual(startup(false, value), ['detach']);
});
test('only explicit development opt-out suppresses automatic opening', () => {
    assert.deepEqual(startup(false, '1'), []);
});
test('packaged launch never automatically opens DevTools, regardless of opt-out', () => {
    for (const value of [undefined, '', '0', '1', 'invalid'])
        assert.deepEqual(startup(true, value), []);
});

test('guarded paired launches suppress automatic DevTools through the existing nonempty config marker', () => {
    for (const pairedConfig of ['C:/fixture/config.json', '0', ' '])
        for (const value of [undefined, '', '0', '1', 'invalid'])
            assert.deepEqual(startup(false, value, value !== undefined, pairedConfig), []);
});
test('absent or empty paired marker preserves ordinary developer behavior', () => {
    for (const pairedConfig of [undefined, '']) {
        assert.deepEqual(startup(false, undefined, false, pairedConfig), ['detach']);
        assert.deepEqual(startup(false, '0', true, pairedConfig), ['detach']);
        assert.deepEqual(startup(false, '1', true, pairedConfig), []);
        assert.deepEqual(startup(true, undefined, false, pairedConfig), []);
    }
});

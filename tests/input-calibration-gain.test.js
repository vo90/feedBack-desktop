const {test} = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs'), path = require('node:path'), vm = require('node:vm');
const ts = require('typescript');
const root = path.join(__dirname, '..');

function harness(audio) {
    const handlers = new Map();
    const source = fs.readFileSync(path.join(root, 'src/main/audio-bridge.ts'), 'utf8');
    const start = source.indexOf("    ipcMain.handle('audio:setGain'");
    const end = source.indexOf("    ipcMain.handle('audio:setInputChannel'", start);
    assert.ok(start >= 0 && end > start);
    const code = ts.transpileModule(source.slice(start, end), {
        compilerOptions: {target: ts.ScriptTarget.ES2022}
    }).outputText;
    vm.runInNewContext(code, {audio, ipcMain: {handle: (key, fn) => handlers.set(key, fn)},
        guided: {run: () => assert.fail('calibration must not enter backing calibration')}});
    return (name, ...args) => handlers.get('audio:' + name)({}, ...args);
}

test('calibration IPC rejects invalid numbers and downlevel addons without falling back to tone gain', () => {
    let writes = 0;
    const call = harness({setInputCalibrationGain: () => {writes++; return true;}});
    for (const value of [undefined, null, '2', NaN, Infinity, -Infinity, 0, -1, 0.099999999, 5.00000001])
        assert.equal(call('setInputCalibrationGain', value), false);
    assert.equal(writes, 0);
    for (const addon of [null, {}, {setInputCalibrationGain: () => {throw Error('unavailable');}}]) {
        const missing = harness(addon);
        assert.equal(missing('setInputCalibrationGain', 2), false);
        assert.equal(missing('getInputCalibrationGain'), 1);
    }
});

test('preset input drive cannot overwrite capture calibration and explicit unity activates it', () => {
    let gain = 1, toneDrive = 1, writes = 0;
    const call = harness({setInputCalibrationGain: value => {gain = value; writes++; return true;},
        getInputCalibrationGain: () => gain, setGain: (which, value) => {toneDrive = value;}});
    for (const value of [1, 0.1, 5, 2.5]) {
        assert.equal(call('setInputCalibrationGain', value), true);
        call('setGain', 'input', 8);
        assert.equal(call('getInputCalibrationGain'), value);
        assert.equal(toneDrive, 8);
    }
    assert.equal(writes, 4);
});

test('getter sanitizes unavailable or malformed native replies', () => {
    for (const value of [undefined, '2', NaN, Infinity, -1, 0, 9])
        assert.equal(harness({getInputCalibrationGain: () => value})('getInputCalibrationGain'), 1);
});

test('preload exposes independent calibration methods and forwards gain unchanged', async () => {
    const source = fs.readFileSync(path.join(root, 'src/main/preload.ts'), 'utf8');
    const lines = source.split('\n').filter(line => /^\s*(setInputCalibrationGain|getInputCalibrationGain):/.test(line));
    assert.equal(lines.length, 2);
    const calls = [];
    const scope = {ipcRenderer: {invoke: async (...args) => {calls.push(args); return args.length > 1 ? true : 2;}}};
    const code = ts.transpileModule('globalThis.api = ({\n' + lines.join('\n') + '\n});', {
        compilerOptions: {target: ts.ScriptTarget.ES2022}
    }).outputText;
    vm.runInNewContext(code, scope);
    assert.equal(await scope.api.setInputCalibrationGain(2), true);
    assert.equal(await scope.api.getInputCalibrationGain(), 2);
    assert.deepEqual(calls, [['audio:setInputCalibrationGain', 2], ['audio:getInputCalibrationGain']]);
});

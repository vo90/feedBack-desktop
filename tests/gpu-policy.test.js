const {test} = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const Module = require('node:module');
const vm = require('node:vm');
const ts = require('typescript');

const helperPath = path.join(__dirname, '../src/main/gpu-policy.ts');
const compiled = ts.transpileModule(fs.readFileSync(helperPath, 'utf8'), {
    compilerOptions: {module: ts.ModuleKind.CommonJS, target: ts.ScriptTarget.ES2022},
}).outputText;
const mod = new Module(helperPath, module);
mod.filename = helperPath;
mod._compile(compiled, helperPath);
const {selectGpuPolicy} = mod.exports;
const options = (overrides = {}) => ({platform: 'win32', isPackaged: false,
    requested: undefined, hasHighPerformanceSwitch: false, hasLowPowerSwitch: false, ...overrides});

test('unset and empty development policy keep the high-performance default', () => {
    for (const requested of [undefined, '']) {
        assert.deepEqual(selectGpuPolicy(options({requested})), {name: 'high-performance',
            switchName: 'force_high_performance_gpu', developmentOverride: false});
    }
});

test('only explicit development policies select a labelled arm', () => {
    for (const [requested, switchName] of [['low-power', 'force_low_power_gpu'],
        ['high-performance', 'force_high_performance_gpu']]) {
        assert.deepEqual(selectGpuPolicy(options({requested})), {name: requested,
            switchName, developmentOverride: true});
    }
});

test('packaged Windows ignores the diagnostic variable, even invalid values', () => {
    for (const requested of [undefined, '', 'low-power', 'high-performance', 'garbage']) {
        assert.deepEqual(selectGpuPolicy(options({requested, isPackaged: true})), {
            name: 'high-performance', switchName: 'force_high_performance_gpu', developmentOverride: false});
    }
});

test('non-Windows platforms do not select a force switch', () => {
    for (const platform of ['linux', 'darwin']) {
        for (const isPackaged of [true, false]) {
            for (const requested of [undefined, 'low-power', 'garbage']) {
                assert.equal(selectGpuPolicy(options({platform, isPackaged, requested})), null);
            }
        }
    }
});

test('invalid development requests fail instead of mislabelling an arm', () => {
    for (const requested of ['LOW-POWER', ' low-power', 'low-power ', 'default', '--disable-gpu', '0']) {
        assert.throws(() => selectGpuPolicy(options({requested})), /must be high-performance or low-power/);
    }
});

test('explicit arms reject the opposing command-line force, including both forces', () => {
    for (const requested of ['low-power', 'high-performance']) {
        for (const samePresent of [false, true]) {
            const hasHighPerformanceSwitch = requested === 'low-power' || samePresent;
            const hasLowPowerSwitch = requested === 'high-performance' || samePresent;
            assert.throws(() => selectGpuPolicy(options({requested,
                hasHighPerformanceSwitch, hasLowPowerSwitch})), /conflicts/);
        }
    }
});

test('an existing matching force is allowed and ordinary startup behavior is preserved', () => {
    assert.equal(selectGpuPolicy(options({requested: 'low-power', hasLowPowerSwitch: true})).name, 'low-power');
    assert.equal(selectGpuPolicy(options({requested: 'high-performance', hasHighPerformanceSwitch: true})).name, 'high-performance');
    assert.equal(selectGpuPolicy(options({hasLowPowerSwitch: true})).name, 'high-performance');
    assert.equal(selectGpuPolicy(options({isPackaged: true, requested: 'low-power', hasLowPowerSwitch: true})).name, 'high-performance');
});

const main = fs.readFileSync(path.join(__dirname, '../src/main/main.ts'), 'utf8');
const start = main.indexOf("if (process.platform === 'win32') {", main.indexOf("app.commandLine.appendSwitch('disable-gpu-process-crash-limit')"));
const end = main.indexOf('// Prevent error dialogs', start);
assert.ok(start >= 0 && end > start);
const startupBlock = ts.transpileModule(main.slice(start, end), {
    compilerOptions: {target: ts.ScriptTarget.ES2022},
}).outputText;

function startup({platform = 'win32', isPackaged = false, requested, existing = []} = {}) {
    const switches = new Set(existing), calls = [], logs = [];
    const app = {isPackaged, commandLine: {
        hasSwitch: name => switches.has(name),
        appendSwitch: name => {calls.push(name); switches.add(name);},
    }};
    vm.runInNewContext(startupBlock, {app, selectGpuPolicy,
        process: {platform, env: {FEEDBACK_DEV_GPU_POLICY: requested}},
        console: {log: (...args) => logs.push(args)}});
    return {switches: [...switches], calls, logs};
}

test('actual startup block selects one switch and logs once before window/readiness code', () => {
    assert.ok(main.includes("import { selectGpuPolicy } from './gpu-policy'"));
    const stripped = main.replace(/\/\*[\s\S]*?\*\/|\/\/[^\n]*/g, '');
    assert.ok(stripped.indexOf('const gpuPolicy = selectGpuPolicy(') < stripped.indexOf('new BrowserWindow('));
    assert.ok(stripped.indexOf('const gpuPolicy = selectGpuPolicy(') < stripped.indexOf('app.whenReady()'));
    for (const requested of [undefined, 'high-performance', 'low-power']) {
        const result = startup({requested});
        assert.deepEqual(result.calls, [requested === 'low-power' ? 'force_low_power_gpu' : 'force_high_performance_gpu']);
        assert.deepEqual(result.switches, result.calls);
        assert.equal(result.logs.length, 1);
        assert.equal(JSON.parse(result.logs[0][1]).selected, requested || 'high-performance');
    }
});

test('actual startup ignores packaged overrides and does nothing on other platforms', () => {
    const packaged = startup({isPackaged: true, requested: 'low-power'});
    assert.deepEqual(packaged.calls, ['force_high_performance_gpu']);
    assert.deepEqual(packaged.logs, []);
    assert.deepEqual(startup({platform: 'linux', requested: 'low-power'}).calls, []);
});

test('actual startup aborts conflicts before appending a second GPU force', () => {
    assert.throws(() => startup({requested: 'low-power', existing: ['force_high_performance_gpu']}), /conflicts/);
});

const {test} = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs'), path = require('node:path'), vm = require('node:vm');
const source = fs.readFileSync(path.join(__dirname, '../src/renderer/screen.js'), 'utf8');

function deviceHarness(refresh) {
    let apply;
    const calls = [];
    const select = value => ({value, disabled:false});
    const scope = {applyingDeviceSettings:false, deviceOptionsCompatible:true, audioRunning:false,
        deviceTypeSelect:select('ASIO'),outputDeviceTypeSelect:select('ASIO'),inputDeviceSelect:select('Interface'),outputDeviceSelect:select('Speakers'),
        sampleRateSelect:select('48000'),bufferSizeSelect:select('128'),inputChannelSelect:select('1'),toggleBtn:{},
        monitorMuteCheckbox:{checked:true},monitorKillCheckbox:{checked:true},
        applyDeviceBtn:{addEventListener:(_,fn)=>{apply=fn;}},statusText:{},statusDot:{},
        window:{feedBack:{guitarProfiles:{refresh:async()=>{calls.push('gain');await refresh();}}},dispatchEvent(){}},CustomEvent:class {},
        api:{getCurrentDevice:async()=>null,setDevice:async()=>({ok:true}),
            setInputChannel:async()=>{calls.push('channel');},setMonitorMute:async()=>{},setMonitorKill:async()=>{},
            startAudio:async()=>{calls.push('start');}},
        aeApplyNoiseGateToEngine(){},aeApplyTonePolishToEngine(){},syncSelectedInputSource(){},rememberAppliedDeviceSettings(){return {};},
        saveDeviceSettings:async()=>{calls.push('saved');}};
    const start = source.indexOf("        applyDeviceBtn.addEventListener('click', async () => {");
    vm.runInNewContext(source.slice(start, source.indexOf('\n        // Input channel', start)), scope);
    return {scope,calls,apply};
}

test('device Apply waits for the newly opened route gain before reporting success',async()=>{
    let release;
    const h = deviceHarness(()=>new Promise(resolve=>{release=resolve;}));
    const pending = h.apply();
    while (!release) await new Promise(resolve=>setImmediate(resolve));
    assert.deepEqual(h.calls,['channel','start','gain']);
    assert.equal(h.scope.statusText.textContent,'Configuring device...');
    release(); await pending;
    assert.equal(h.scope.statusText.textContent,'Audio running');
    assert.deepEqual(h.calls,['channel','start','gain','saved']);
});

test('failed profile application is shown and does not persist successful device setup',async()=>{
    const h = deviceHarness(async()=>{throw new Error('Guitar gain unavailable');});
    await h.apply();
    assert.match(h.scope.statusText.textContent,/Guitar gain unavailable/);
    assert.equal(h.calls.includes('saved'),false);
    assert.equal(h.scope.applyDeviceBtn.disabled,false);
});

test('live channel change recalls its guitar gain before saving the selected channel',async()=>{
    let change;
    const calls = [];
    const scope = {inputChannelSelect:{value:'1',addEventListener:(_,fn)=>{change=fn;}},
        isDeviceFormApplied:()=>true,statusText:{},
        api:{setInputChannel:async channel=>{calls.push(['channel',channel]);}},
        window:{feedBack:{guitarProfiles:{refresh:async()=>{calls.push(['gain']);}}}},
        saveAppliedDeviceSettings:async()=>{calls.push(['saved']);}};
    const start = source.indexOf("        inputChannelSelect.addEventListener('change', async () => {");
    vm.runInNewContext(source.slice(start, source.indexOf('\n        // Monitor mute', start)),scope);
    await change();
    assert.deepEqual(calls,[['channel',1],['gain'],['saved']]);
});

test('a failed startup gain refresh reports the error without aborting remaining panel initialization',async()=>{
    let synchronized = false;
    const scope = {inputChannelSelect:{value:'0'},audioRunning:false,toggleBtn:{},statusDot:{},statusText:{},result:{ok:true},
        deviceTypeSelect:{value:'ASIO'},inputDeviceSelect:{value:'Interface'},
        api:{setInputChannel:async()=>{},startAudio:async()=>{}},
        window:{feedBack:{guitarProfiles:{refresh:async()=>{throw new Error('Temporary profile error');}}}},
        aeApplyNoiseGateToEngine(){},rememberAppliedDeviceSettings(){},aeApplyTonePolishToEngine(){},
        syncSelectedInputSource(){synchronized=true;}};
    const init = source.indexOf('    async function init() {');
    const start = source.indexOf('                    const inputChannel = parseInt(inputChannelSelect.value);',init);
    const end = source.indexOf('\n                }',start);
    await vm.runInNewContext('(async()=>{' + source.slice(start,end) + '})()',scope);
    assert.equal(scope.audioRunning,true);
    assert.equal(scope.toggleBtn.textContent,'Stop');
    assert.equal(scope.statusText.textContent,'Temporary profile error');
    assert.match(scope.statusDot.className,/red/);
    assert.equal(synchronized,true);
});

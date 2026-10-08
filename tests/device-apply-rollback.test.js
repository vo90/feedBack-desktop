const {test} = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs'), path = require('node:path'), vm = require('node:vm');
const source = fs.readFileSync(path.join(__dirname, '../src/renderer/screen.js'), 'utf8');
const start = source.indexOf("        applyDeviceBtn.addEventListener('click', async () => {");
const end = source.indexOf('\n        // Input channel', start);
test('failed Apply restores the actual previous endpoint without saving the failed selection', async () => {
    let apply, opened = [], running = true;
    const prior = {inputType:'ASIO', input:'Interface', outputType:'Windows Audio', output:'TV', sampleRate:48000, inputBlockSize:128, outputBlockSize:480};
    const select = value => ({value, disabled:false});
    const scope = {applyingDeviceSettings:false, deviceOptionsCompatible:true, audioRunning:true,
        deviceTypeSelect:select('ASIO'),outputDeviceTypeSelect:select('ASIO'),inputDeviceSelect:select('Interface'),outputDeviceSelect:select(''),
        sampleRateSelect:select('96000'),bufferSizeSelect:select('64'),inputChannelSelect:select('0'),toggleBtn:{},
        applyDeviceBtn:{addEventListener:(_,fn)=>{apply=fn;}},statusText:{},statusDot:{},
        window:{dispatchEvent(){}},CustomEvent:class {},
        api:{getCurrentDevice:async()=>prior,stopAudio:async()=>{running=false;},
            setDevice:async c=>{opened.push(c);return opened.length === 1 ? {ok:false,error:'unsupported'} : {ok:true};},
            startAudio:async()=>{running=true;}, isAudioRunning:async()=>running},
        saveDeviceSettings:()=>assert.fail('must not persist a rejected configuration')};
    vm.runInNewContext(source.slice(start,end),scope);
    await apply();
    assert.equal(opened.length,2);
    assert.equal(opened[1].outputDevice,'TV'); assert.equal(opened[1].sampleRate,48000); assert.equal(opened[1].bufferSize,128);
    assert.equal(scope.audioRunning,true); assert.equal(scope.toggleBtn.textContent,'Stop');
    assert.match(scope.statusText.textContent,/Previous audio setup restored/);
    assert.equal(scope.applyDeviceBtn.disabled,false);
});

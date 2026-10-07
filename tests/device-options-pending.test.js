const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const source = fs.readFileSync(path.join(__dirname, '../src/renderer/screen.js'), 'utf8');
const start = source.indexOf('    async function refreshDeviceOptions(');
const end = source.indexOf('\n    // ── Noise gate', start);
function harness() {
    const pending = [];
    const select = value => ({value});
    const context = vm.createContext({
        latestDeviceOptionsRequest: 0, deviceOptionsCompatible: true, applyingDeviceSettings: false,
        applyDeviceBtn: {disabled:false}, deviceTypeSelect:select('ASIO'), outputDeviceTypeSelect:select('Windows Audio'),
        inputDeviceSelect:select('Interface'), outputDeviceSelect:select('Speakers'),
        srMismatchWarning:{classList:{toggle(){}},textContent:''},
        renderInputChannelOptions(){},renderSampleRateOptions(){},renderBufferSizeOptions(){},console,
        api:{probeDeviceOptions:()=>new Promise(resolve=>pending.push(resolve))}
    });
    vm.runInContext(source.slice(start,end),context);
    return {context,pending};
}
test('Apply stays disabled until the current selection finishes probing', async()=>{
    const {context:c,pending:p}=harness();
    const old=c.refreshDeviceOptions();
    assert.equal(c.applyDeviceBtn.disabled,true);
    assert.equal(c.deviceOptionsCompatible,false);
    c.outputDeviceTypeSelect.value='ASIO';
    const current=c.refreshDeviceOptions();
    p[0]({compatible:true}); await old;
    assert.equal(c.applyDeviceBtn.disabled,true);
    p[1]({compatible:false,error:'Device unavailable'});await current;
    assert.equal(c.applyDeviceBtn.disabled,true);
    assert.equal(c.deviceOptionsCompatible,false);
});
test('a successful latest probe enables Apply, unless an apply is in progress',async()=>{
    const {context:c,pending:p}=harness();
    let result=c.refreshDeviceOptions();p[0]({compatible:true});await result;
    assert.equal(c.applyDeviceBtn.disabled,false);
    c.applyingDeviceSettings=true;
    result=c.refreshDeviceOptions();p[1]({compatible:true});await result;
    assert.equal(c.applyDeviceBtn.disabled,true);
});

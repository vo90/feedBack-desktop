const {test} = require('node:test');
const assert = require('node:assert/strict');
const {EventEmitter} = require('node:events');
const {loadTs} = require('./_load-ts');
const {createRendererAudioPortBridge} = loadTs('src/main/renderer-audio-port.ts');
class Port extends EventEmitter {
    start() { this.started = true; }
    close() { this.closed = true; this.emit('close'); }
    send(pcm = new Float32Array([.25, -.25]), sampleRate = 48000) { this.emit('message', {data:{pcm,sampleRate}}); }
}
function harness() {
    const pushed = [], bridge = createRendererAudioPortBridge((pcm, rate) => pushed.push({pcm:[...pcm],rate}));
    return {pushed, bridge};
}
test('direct worklet packets reach native output once with their source rate', () => {
    const {bridge,pushed} = harness(), port = new Port();
    assert.equal(bridge.attach(1,'one',port),true); assert.equal(port.started,true);
    port.send(); assert.deepEqual(pushed,[{pcm:[.25,-.25],rate:48000}]);
});
test('replacing a producer closes the old port and ignores its queued packets and close', () => {
    const {bridge,pushed} = harness(), old = new Port(), current = new Port();
    bridge.attach(1,'old',old); bridge.attach(1,'new',current);
    assert.equal(old.closed,true); old.send(); old.emit('close');
    assert.equal(bridge.close(1,'old'),false); assert.equal(bridge.has(1,'new'),true);
    current.send(); assert.equal(pushed.length,1);
});
test('only the owning capture can close its port; stopping discards later packets', () => {
    const {bridge,pushed} = harness(), port = new Port(); bridge.attach(1,'one',port);
    assert.equal(bridge.close(2,'one'),false); assert.equal(bridge.has(1,'one'),true);
    assert.equal(bridge.close(1,'one'),true); port.send();
    assert.equal(pushed.length,0); assert.equal(bridge.has(1,'one'),false);
});
test('a new window replaces the single native producer and old window teardown is inert', () => {
    const {bridge,pushed} = harness(), old = new Port(), current = new Port();
    bridge.attach(1,'old',old); bridge.attach(2,'new',current); bridge.close(1);
    old.send(); current.send(); assert.equal(pushed.length,1); assert.equal(bridge.has(2,'new'),true);
});
test('peer closure is visible so the renderer can recover', () => {
    const {bridge} = harness(), port = new Port(); bridge.attach(1,'one',port); port.close();
    assert.equal(bridge.has(1,'one'),false);
});
test('malformed rates, PCM and IDs cannot reach the native addon', () => {
    const {bridge,pushed} = harness(), invalid = new Port(), port = new Port();
    assert.equal(bridge.attach(1,{},invalid),false); assert.equal(invalid.closed,true);
    bridge.attach(1,'one',port);
    for(const rate of [0,NaN,Infinity,-48000,1,1e9,'48000']) port.send(undefined,rate);
    for(const pcm of [[],new Uint8Array(8),new Float32Array(3),new Float32Array(32770),null]) port.send(pcm);
    port.emit('message',{data:null}); assert.equal(pushed.length,0);
    port.send(new Float32Array([NaN,Infinity])); assert.deepEqual(pushed[0].pcm,[0,0]);
});

test('preload only transfers the local page port and acknowledges without exposing IPC events', () => {
    const fs=require('node:fs'),path=require('node:path'),vm=require('node:vm');
    const source=fs.readFileSync(path.join(__dirname,'../src/main/preload.ts'),'utf8');
    const code=source.slice(source.indexOf("window.addEventListener('message'"),source.indexOf("import type { StartupStatus"))
        .replace('_event: unknown, id: string, ok: boolean','_event, id, ok');
    const events={}, sent=[], replies=[], ipc={postMessage:(...args)=>sent.push(args),on:(name,fn)=>events[name]=fn};
    const win={location:{origin:'http://localhost:8000'},addEventListener:(name,fn)=>events[name]=fn,postMessage:(...args)=>replies.push(args)};
    vm.runInNewContext(code,{window:win,ipcRenderer:ipc});
    const port={},event={source:win,origin:win.location.origin,data:{type:'feedback-renderer-audio-port',id:'one'},ports:[port]};
    events.message({...event,source:{}}); events.message({...event,origin:'https://other.example'});
    assert.equal(sent.length,0); events.message(event);
    assert.equal(sent[0][0],'audio:attachRendererAudioPort'); assert.equal(sent[0][2][0],port);
    events['audio:rendererAudioPortReady']({secret:true},'one',true);
    assert.equal(replies[0][0].ok,true); assert.equal(replies[0][0].secret,undefined);
});

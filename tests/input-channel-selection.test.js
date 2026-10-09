const { test } = require('node:test');
const assert = require('node:assert/strict');
const { loadTs } = require('./_load-ts');
const { confirmInputChannel } = loadTs('src/main/input-channel-selection.ts');
function fixture(outputType = 'ASIO') {
    let selected = 1, saved;
    const state = { generation: 4, running: true, channels: Array.from({length:8}, (_,index)=>({index})) };
    const audio = {
        getInputChannelSnapshot: () => ({...state, selected}),
        getCurrentDevice: () => ({inputType:'ASIO', outputType, input:'Interface',output:'Output',sampleRate:48000,blockSize:256}),
        selectInputChannel: (ch, gen, previous) => {
            if (gen !== state.generation || previous !== selected) return false;
            selected = ch; return true;
        }
    };
    return { audio, state, read: () => ({monitorMute:true,monitorKill:true}),
        write: s => { saved = s; return true; }, selected: () => selected, saved: () => saved };
}
test('confirms channel 7 and persists it for ASIO and split Windows output without changing monitoring', () => {
    for (const backend of ['ASIO','Windows Audio']) {
        const f = fixture(backend);
        assert.equal(confirmInputChannel(f.audio,6,4,1,f.read,f.write).ok,true);
        assert.equal(f.selected(),6); assert.equal(f.saved().inputChannel,'6');
        assert.equal(f.saved().outputType,backend); assert.equal(f.saved().monitorKill,true);
    }
});
test('invalid indices, stale scans, changed selections and disconnected devices cannot save', () => {
    for (const [ch,gen,previous] of [[8,4,1],[-2,4,1],[NaN,4,1],[1.5,4,1],[6,3,1],[6,4,0]]) {
        const f=fixture(); assert.equal(confirmInputChannel(f.audio,ch,gen,previous,f.read,f.write).ok,false);
        assert.equal(f.selected(),1); assert.equal(f.saved(),undefined);
    }
    const f=fixture();f.state.running=false;
    assert.equal(confirmInputChannel(f.audio,6,4,1,f.read,f.write).ok,false);
    const empty=fixture();empty.state.channels=[];
    assert.equal(confirmInputChannel(empty.audio,-1,4,1,empty.read,empty.write).ok,false);
});
test('save failure rolls back live selection and leaves persisted settings alone', () => {
    const f=fixture();
    assert.equal(confirmInputChannel(f.audio,6,4,1,f.read,()=>false).ok,false);
    assert.equal(f.selected(),1); assert.equal(f.saved(),undefined);
});

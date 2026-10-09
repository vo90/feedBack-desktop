const {test} = require('node:test');
const assert = require('node:assert/strict');
const {loadTs} = require('./_load-ts');
const {createGuidedCalibration, calibrationWave} = loadTs('src/main/guided-calibration.ts');
function fixture() {
    const calls = [], saved = [];
    let generation = 1, now = 0, failSave = false;
    const audio = {isAudioRunning:()=>true, getCurrentDevice:()=>({routeGeneration:generation}), getBackingPosition:()=>42};
    for (const method of ['stopBacking','loadBackingTrack','loadBackingSession','setBackingSpeed','setBackingSourceGains','seekBacking','setGain','startBacking'])
        audio[method] = async (...args) => {calls.push([method,...args]);return true;};
    const c = createGuidedCalibration({audio:()=>audio, profile:()=>({perOutputSetup:true,output:{key:'tv',routeKey:'tv',persistent:true,offsetMs:-100}}),
        asset:()=>'/clicks.wav', snapshot:()=>({valid:true}), now:()=>now,
        save:(key,offset)=>{if(failSave) throw Error('disk full');saved.push([key,offset]);return true;}});
    return {c,calls,saved,audio,route:()=>generation++,time:n=>now=n,fail:()=>failSave=true};
}
test('preserves multistem file selection, gains, speed and position; restores paused', async () => {
    const f = fixture();
    await f.c.run('loadBackingSession',['/a','/b'],[.2,.8],false);
    await f.c.run('setBackingSpeed',.75); await f.c.run('setBackingSourceGains',[.4,.6]); await f.c.run('setGain','backing',.65);
    const s = await f.c.begin(1); assert.equal(await f.c.run('startBacking'),false);
    assert.ok(f.calls.some(c => c[0] === 'loadBackingTrack' && c[1] === '/clicks.wav' && c[2] === false));
    await f.c.trial(s.token,1,.2); await f.c.finish(s.token,1);
    assert.deepEqual(f.calls.slice(-5),[['loadBackingSession',['/a','/b'],[.2,.8],false],['setBackingSourceGains',[.4,.6]],['setBackingSpeed',.75],['seekBacking',42],['setGain','backing',.65]]);
    assert.deepEqual(f.saved,[]); assert.equal(f.c.active,false);
});
test('stale route and wrong renderer cannot save; cancel restores without writing', async () => {
    const f=fixture(),s=await f.c.begin(1);
    await assert.rejects(f.c.finish(s.token,2,-80));
    f.route(); assert.equal(f.c.poll(s.token,1),null);
    await assert.rejects(f.c.finish(s.token,1,-80));
    await f.c.finish(s.token,1); assert.deepEqual(f.saved,[]);
});
test('failed persistence keeps the session available to cancel', async () => {
    const f=fixture(),s=await f.c.begin(1); f.fail();
    await assert.rejects(f.c.finish(s.token,1,-80)); assert.equal(f.c.active,true);
    await f.c.finish(s.token,1); assert.equal(f.c.active,false);
});
test('lost renderer heartbeat stops clicks and restores song', async () => {
    const f=fixture(); await f.c.run('loadBackingTrack','/song');
    await f.c.begin(1); f.time(6000); await f.c.expire();
    assert.equal(f.c.active,false); assert.ok(f.calls.some(c=>c[0]==='loadBackingTrack' && c[1]==='/song'));
});
test('begin waits for the prior native load before preserving it', async () => {
    const f=fixture(); let release;
    f.audio.loadBackingTrack = name => name === '/song' ? new Promise(resolve=>release=resolve) : true;
    const load=f.c.run('loadBackingTrack','/song'); await Promise.resolve();
    const begin=f.c.begin(1); release(true); await load; const s=await begin;
    f.audio.loadBackingTrack=async name=>{f.calls.push(['restored',name]);return true;};
    await f.c.finish(s.token,1); assert.ok(f.calls.some(c=>c[0]==='restored'&&c[1]==='/song'));
});
test('authored PCM cue has bounded amplitude, exact duration and silence between clicks', () => {
    const b=calibrationWave(); assert.equal(b.length,44+48000*10*2); assert.equal(b.toString('ascii',0,4),'RIFF');
    for (const seconds of [0,1,3,4,6,7,9]) assert.equal(b.readInt16LE(44+seconds*48000*2),0);
    assert.notEqual(b.readInt16LE(44+(2*48000+24)*2),0);
    const cue = seconds => b.subarray(44 + seconds * 48000 * 2, 44 + (seconds * 48000 + 720) * 2);
    assert.deepEqual(cue(2), cue(5)); assert.deepEqual(cue(2), cue(8));
});

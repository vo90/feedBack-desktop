// Run with matching Electron, ELECTRON_RUN_AS_NODE=1, and an external timeout.
// Arguments: addon path, ASIO device, Windows output. Does not persist settings.
const assert = require('node:assert/strict');
const audio = require(require('node:path').resolve(process.argv[2]));
const input = process.argv[3], output = process.argv[4];
const log = (phase, result) => console.log(JSON.stringify({time:new Date().toISOString(),phase,result}));
const wait = ms => new Promise(resolve=>setTimeout(resolve,ms));
async function apply(inputType,inputDevice,outputType,outputDevice,rate=48000,buffer=256,allowDeviceError=false) {
  const config={inputType,inputDevice,outputType,outputDevice,sampleRate:rate,bufferSize:buffer};
  log('begin',config);
  const options=audio.probeDeviceOptions(inputType,inputDevice,outputType,outputDevice);
  if(!options.compatible) { log('unavailable',options);return; }
  if(!options.sampleRates.includes(rate)) config.sampleRate=options.sampleRates[0];
  if(!options.bufferSizes.includes(buffer)) config.bufferSize=options.bufferSizes[0];
  const result=audio.setDevice(config);log('applied',result);
  if (!result.ok && allowDeviceError) { log('driver-rejected', {config,error:result.error}); return; }
  assert.equal(result.ok,true,result.error);
  if(inputType==='ASIO' && outputType==='ASIO') assert.equal(result.duplex,true);
  audio.startAudio();await wait(350);
  for(const channel of [-1,0,options.inputChannels.length-1]) audio.setInputChannel(channel);
  assert.equal(audio.isAudioRunning(),true);
  for(let i=0;i<3;i++) assert.equal(audio.probeDeviceOptions(inputType,inputDevice,outputType,outputDevice).compatible,true);
  log('running',audio.getCurrentDevice());
}
async function main() {
  audio.init();audio.setMonitorKill(true);audio.setMonitorMute(true);
  const types=audio.getDeviceTypes();log('types',types);
  await apply('ASIO',input,'Windows Audio',output);
  for(let i=0;i<3;i++) {
    await apply('ASIO',input,'ASIO',''); // reported hang
    await apply('ASIO','','ASIO',input);
    await apply('ASIO','','ASIO','');
    await apply('ASIO',input,'ASIO',input);
    await apply('ASIO',input,'Windows Audio',output);
  }
  const duplex=audio.probeDeviceOptions('ASIO',input,'ASIO',input);
  for(const rate of duplex.sampleRates) await apply('ASIO',input,'ASIO',input,rate);
  for(const buffer of duplex.bufferSizes) await apply('ASIO',input,'ASIO',input,48000,buffer);
  for(const type of types.filter(t=>t.name!=='ASIO')) {
    if(type.outputs.length) {
      await apply('ASIO',input,type.name,'',48000,256,true);
      const named=type.outputs.find(d=>d===output)||type.outputs[0];
      await apply('ASIO',input,type.name,named,48000,256,true);
    }
    if(type.inputs.length) await apply(type.name,'','ASIO',input,48000,256,true);
  }
  await apply('ASIO',input,'Windows Audio',output);
  const streamError=audio.setStreamOutputDevice('ASIO','');
  log('stream-duplicate',streamError);assert.match(streamError,/already in use/);
  assert.equal(audio.isAudioRunning(),true);
  const stale=audio.setDevice({inputType:'ASIO',inputDevice:'__missing_test_device__',outputType:'Windows Audio',outputDevice:output,sampleRate:48000,bufferSize:256});
  assert.equal(stale.ok,false);assert.equal(audio.isAudioRunning(),true);
  assert.equal(audio.probeDeviceOptions('ASIO','__missing_test_device__','Windows Audio',output).compatible,false);
  audio.stopAudio();audio.startAudio();assert.equal(audio.isAudioRunning(),true);
  audio.shutdown();log('passed',true);
}
main().catch(e=>{console.error(e);process.exitCode=1;try{audio.shutdown();}catch{}});

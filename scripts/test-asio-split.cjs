// Hardware regression: run with the addon's matching Electron and
// ELECTRON_RUN_AS_NODE=1. Arguments: addon, ASIO device, Windows output, [cycles].
// Guitar monitoring stays disabled. No settings or song-library files are written.
const assert = require('node:assert/strict');
const audio = require(require('node:path').resolve(process.argv[2]));
const input = process.argv[3];
const output = process.argv[4];
const cycles = Number(process.argv[5] || 3);
const log = (phase, result) => console.log(JSON.stringify({time: new Date().toISOString(), phase, result}));
const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));
async function main() {
  audio.init();
  audio.setMonitorKill(true);
  audio.setMonitorMute(true);
  log('devices', audio.getDeviceTypes());
  for (let cycle = 0; cycle < cycles; ++cycle) {
    const bufferSize = [256, 128, 512][cycle % 3];
    log('probe-before', audio.probeDeviceOptions('ASIO', input, 'Windows Audio', output));
    log('apply-begin', {cycle, bufferSize});
    const result = audio.setDevice({inputType: 'ASIO', inputDevice: input,
      outputType: 'Windows Audio', outputDevice: output, sampleRate: 48000, bufferSize});
    log('apply-end', result);
    assert.equal(result.ok, true, result.error);
    assert.equal(result.duplex, false);
    assert.equal(result.inputBlockSize, bufferSize);
    audio.startAudio();
    for (let i = 0; i < 5; ++i) {
      const options = audio.probeDeviceOptions('ASIO', input, 'Windows Audio', output);
      assert.equal(options.compatible, true, options.error);
      assert.equal(options.inputChannels.length, 8);
      assert.equal(audio.isAudioRunning(), true);
      await sleep(200);
    }
    log('running', {device: audio.getCurrentDevice(), metrics: audio.getDeviceMetrics(),
      levels: audio.getLevels(), monitor: audio.getMonitorMuteState()});
    audio.stopAudio();
    audio.startAudio();
    assert.equal(audio.isAudioRunning(), true);
  }
  audio.shutdown();
  log('passed', {cycles});
}
main().catch(error => { console.error(error); try { audio.shutdown(); } catch {} process.exitCode = 1; });

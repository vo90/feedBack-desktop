const {test}=require('node:test');
const assert=require('node:assert/strict');
const fs=require('node:fs'),path=require('node:path');
const source=fs.readFileSync(path.join(__dirname,'../src/main/audio-bridge.ts'),'utf8');
const body=source.match(/ipcMain\.handle\('audio:harmonicTargetVersion', \(\) => \{([\s\S]*?)\n    \}\);/)[1];
const capability=audio=>new Function('audio',body)(audio);
test('harmonic capability preserves known versions and fails closed for unsupported addons',()=>{
    assert.equal(capability(null),0);
    assert.equal(capability({}),0);
    for(const v of [1,2])assert.equal(capability({harmonicTargetVersion:()=>v}),v);
    for(const v of [0,3,null,undefined,'2',NaN])assert.equal(capability({harmonicTargetVersion:()=>v}),0);
});

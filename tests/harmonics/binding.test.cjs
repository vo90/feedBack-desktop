const assert=require('node:assert/strict');
const addon=require(process.argv[2]);
const fallback={kind:'artificial',node:null,interval:24,policy:'harmonic',basis:'source-playback-fallback'};
const canonical={kind:'artificial',node:5,interval:24,policy:'harmonic'};
const read=(harmonic_target,f=10)=>addon.read({f,harmonic_target});
assert.deepEqual(read(fallback),{ok:true,node:null,interval:24,basis:'source-playback-fallback'});
assert.deepEqual(read(canonical),{ok:true,node:5,interval:24,basis:''});
assert.deepEqual(addon.read({f:10}),{ok:true});
const missing={...fallback};delete missing.basis;
const inherited=Object.assign(Object.create({node:null}),fallback);delete inherited.node;
for(const value of [missing,inherited,null,[],true,10,{...fallback,node:0},{...fallback,node:5},
    {...fallback,node:undefined},{...fallback,basis:undefined},{...fallback,basis:'unknown'},
    {...fallback,kind:'tapped'},{...fallback,policy:'mixed'},{...fallback,interval:12},
    {...fallback,interval:'24'},{...fallback,interval:24.5},{...fallback,interval:NaN},
    {...fallback,extra:1},{...canonical,node:null},{...canonical,node:NaN},
    {...canonical,basis:'source-playback-fallback'}])
    assert.equal(read(value).ok,false,JSON.stringify(value));
for(const fret of [null,'10',10.5,-1,49,NaN,Infinity])assert.equal(read(fallback,fret).ok,false);
console.log('Production N-API harmonic target validation passed');

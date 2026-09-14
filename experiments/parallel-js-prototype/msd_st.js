const fs=require('fs');
function rng(x){return()=>{x=(x*1664525+1013904223)>>>0;return x/0x100000000;};}
const N=4000000;
const A=8; const al=x=>(x+7)&~7;
const dataOff=0, workOff=al(N*4), partOff=al(workOff+N*4), srcOff=al(partOff+N*4), dstOff=al(srcOff+N*4), Hoff=al(dstOff+N*4), BNDoff=al(Hoff+4*257*4), end=al(BNDoff+257*4);
const pages=Math.ceil(end/65536)+4, MAXP=16384;
const memory=new WebAssembly.Memory({initial:pages, maximum:MAXP, shared:true});
const inst=new WebAssembly.Instance(new WebAssembly.Module(fs.readFileSync('kernel.wasm')),{env:{memory}});
const {bias,unbias,partition_top,radix_u32}=inst.exports;
const I32=new Int32Array(memory.buffer), U32=new Uint32Array(memory.buffer);
const ok=(a,lo,n)=>{for(let i=lo+1;i<lo+n;i++)if(a[i-1]>a[i])return false;return true;};

function pipeline(){
  bias(dataOff, N, workOff);
  partition_top(workOff, N, partOff, Hoff, BNDoff);
  const bnd=BNDoff>>2;
  for(let b=0;b<256;b++){
    const lo=U32[bnd+b], hi=U32[bnd+b+1], len=hi-lo;
    if(len>1) radix_u32(partOff+lo*4, len, srcOff+lo*4, dstOff+lo*4, Hoff);
  }
  unbias(partOff, N, dataOff);
}
// correctness
const r=rng(7); const base=new Int32Array(N); for(let i=0;i<N;i++)base[i]=(r()*4e9-2e9)|0;
I32.set(base, dataOff>>2);
pipeline();
const out=I32.subarray(dataOff>>2, (dataOff>>2)+N);
const ref=Int32Array.from(base).sort();
let match=true; for(let i=0;i<N;i++) if(out[i]!==ref[i]){match=false;break;}
console.log('MSD pipeline correct (sorted & matches reference):', ok(I32, dataOff>>2, N) && match);

// timing vs V8 + plain whole-array radix (reuse radix.wasm)
function bench(fn){for(let k=0;k<3;k++){I32.set(base,dataOff>>2);fn();}const t=[];for(let k=0;k<6;k++){I32.set(base,dataOff>>2);const t0=performance.now();fn();t.push(performance.now()-t0);}t.sort((a,b)=>a-b);return t[3];}
const tpipe=bench(pipeline);
function benchV8(){const t=[];for(let k=0;k<5;k++){const w=Int32Array.from(base);const t0=performance.now();w.sort();t.push(performance.now()-t0);}t.sort((a,b)=>a-b);return t[2];}
const v8=benchV8();
console.log('n=4M:  V8 .sort '+v8.toFixed(1)+'ms   MSD-pipeline WASM (1 thread) '+tpipe.toFixed(1)+'ms   '+(v8/tpipe).toFixed(2)+'x vs V8');
console.log('(single-threaded MSD adds a partition pass vs plain radix; its real purpose is to make the bucket sorts independent for the parallel phase.)');

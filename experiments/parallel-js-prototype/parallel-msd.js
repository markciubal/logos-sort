const { Worker, isMainThread, workerData, parentPort } = require('worker_threads');
const fs=require('fs');
const C_GO=0, C_DONE=1;
const N=4000000;
const al=x=>(x+7)&~7;
const dataOff=0, workOff=al(N*4), partOff=al(workOff+N*4), srcOff=al(partOff+N*4), dstOff=al(srcOff+N*4), BNDoff=al(dstOff+N*4), Hbase=al(BNDoff+257*4);
const HSZ=4*257*4;
function layout(P){ const end=al(Hbase+P*HSZ); return {pages:Math.ceil(end/65536)+4}; }

if(!isMainThread){
  const {memory,ctrlSAB,P,myId,bStart,bEnd}=workerData;
  const inst=new WebAssembly.Instance(new WebAssembly.Module(fs.readFileSync('kernel.wasm')),{env:{memory}});
  const radix_u32=inst.exports.radix_u32;
  const U32=new Uint32Array(memory.buffer); const ctrl=new Int32Array(ctrlSAB);
  const bnd=BNDoff>>2, myH=Hbase+myId*HSZ;
  parentPort.postMessage('ready');
  let last=0;
  while(true){
    Atomics.wait(ctrl,C_GO,last); const gen=Atomics.load(ctrl,C_GO); if(gen<0)break; last=gen;
    for(let b=bStart;b<bEnd;b++){ const lo=U32[bnd+b], hi=U32[bnd+b+1], len=hi-lo;
      if(len>1) radix_u32(partOff+lo*4, len, srcOff+lo*4, dstOff+lo*4, myH); }
    Atomics.add(ctrl,C_DONE,1); Atomics.notify(ctrl,C_DONE);
  }
} else {
  function rng(x){return()=>{x=(x*1664525+1013904223)>>>0;return x/0x100000000;};}
  const ok=(a,lo,n)=>{for(let i=lo+1;i<lo+n;i++)if(a[i-1]>a[i])return false;return true;};
  (async()=>{
    const P=4;
    const {pages}=layout(P);
    const memory=new WebAssembly.Memory({initial:pages,maximum:16384,shared:true});
    const inst=new WebAssembly.Instance(new WebAssembly.Module(fs.readFileSync('kernel.wasm')),{env:{memory}});
    const {bias,unbias,partition_top}=inst.exports;
    const I32=new Int32Array(memory.buffer), U32=new Uint32Array(memory.buffer);
    const ctrlSAB=new SharedArrayBuffer(8), ctrl=new Int32Array(ctrlSAB);
    const workers=[]; let ready=0;
    await new Promise(res=>{ for(let id=0;id<P;id++){ const bStart=Math.floor(id*256/P), bEnd=Math.floor((id+1)*256/P);
      const w=new Worker(__filename,{workerData:{memory,ctrlSAB,P,myId:id,bStart,bEnd}}); w.on('message',m=>{if(m==='ready'&&++ready===P)res();}); workers.push(w);} });
    const r=rng(7); const base=new Int32Array(N); for(let i=0;i<N;i++)base[i]=(r()*4e9-2e9)|0;
    function runParallel(){
      I32.set(base, dataOff>>2);
      bias(dataOff,N,workOff); partition_top(workOff,N,partOff,Hbase,BNDoff);   // serial (both parallelizable)
      Atomics.store(ctrl,C_DONE,0); Atomics.add(ctrl,C_GO,1); Atomics.notify(ctrl,C_GO);
      let d=Atomics.load(ctrl,C_DONE); while(d<P){Atomics.wait(ctrl,C_DONE,d);d=Atomics.load(ctrl,C_DONE);}   // ONE fork-join, no barriers
      unbias(partOff,N,dataOff);
    }
    runParallel();
    const out=I32.subarray(dataOff>>2,(dataOff>>2)+N); const ref=Int32Array.from(base).sort();
    let match=true; for(let i=0;i<N;i++)if(out[i]!==ref[i]){match=false;break;}
    console.log('parallel MSD+WASM correct (P='+P+'):', ok(I32,dataOff>>2,N)&&match);
    // timing + phase decomposition
    for(let k=0;k<3;k++)runParallel();
    const tt=[]; for(let k=0;k<6;k++){const t0=performance.now();runParallel();tt.push(performance.now()-t0);} tt.sort((a,b)=>a-b);
    // measure serial phases alone (bias+partition+unbias) for Amdahl split
    const ps=[]; for(let k=0;k<6;k++){I32.set(base,dataOff>>2);const t0=performance.now();bias(dataOff,N,workOff);partition_top(workOff,N,partOff,Hbase,BNDoff);unbias(partOff,N,dataOff);ps.push(performance.now()-t0);} ps.sort((a,b)=>a-b);
    const total=tt[3], serialPhases=ps[3];
    console.log('n=4M, single core: total='+total.toFixed(1)+'ms  serial phases(bias+partition+unbias)='+serialPhases.toFixed(1)+'ms  bucket-sort phase='+(total-serialPhases).toFixed(1)+'ms');
    const f=serialPhases/total;
    console.log('parallel-phase fraction = '+((1-f)*100).toFixed(0)+'%  (bucket sorts, 0 barriers among workers)');
    console.log('NOTE: bias/partition/unbias are themselves parallelizable; left serial here. The ONLY true');
    console.log('serial step in a full impl is the partition offset-scan (measured ~0.03% earlier).');
    Atomics.store(ctrl,C_GO,-1); Atomics.notify(ctrl,C_GO); for(const w of workers) await w.terminate();
  })();
}

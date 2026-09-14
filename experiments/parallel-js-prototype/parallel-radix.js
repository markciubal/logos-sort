const { Worker, isMainThread, workerData, parentPort, threadId } = require('worker_threads');
// control layout
const C_COUNT=0, C_SENSE=1, C_GO=2, C_DONE=3, C_SCAN_NS=4, HDR=8;
function barrier(ctrl,P){
  const sense=Atomics.load(ctrl,C_SENSE);
  const n=Atomics.add(ctrl,C_COUNT,1)+1;
  if(n===P){ Atomics.store(ctrl,C_COUNT,0); Atomics.store(ctrl,C_SENSE,sense^1); Atomics.notify(ctrl,C_SENSE); }
  else { while(Atomics.load(ctrl,C_SENSE)===sense) Atomics.wait(ctrl,C_SENSE,sense); }
}
if(!isMainThread){
  const {ctrlSAB,dataSAB,waSAB,wbSAB,P,myId,n}=workerData;
  const ctrl=new Int32Array(ctrlSAB), data=new Int32Array(dataSAB), WA=new Uint32Array(waSAB), WB=new Uint32Array(wbSAB);
  const HIST=HDR, OFF=HDR+P*256, BIAS=0x80000000;
  const chunk=Math.ceil(n/P), lo=myId*chunk, hi=Math.min(n,lo+chunk);
  const lh=new Int32Array(256), loff=new Int32Array(256);
  parentPort.postMessage('ready');
  let last=0;
  while(true){
    Atomics.wait(ctrl,C_GO,last); const gen=Atomics.load(ctrl,C_GO); if(gen<0)break; last=gen;
    // init: bias data->WA
    for(let i=lo;i<hi;i++) WA[i]=(data[i]+BIAS)>>>0;
    barrier(ctrl,P);
    for(let pass=0;pass<4;pass++){
      const sh=pass*8, src=(pass&1)?WB:WA, dst=(pass&1)?WA:WB;
      lh.fill(0);
      for(let i=lo;i<hi;i++) lh[(src[i]>>>sh)&0xFF]++;
      for(let b=0;b<256;b++) ctrl[HIST+myId*256+b]=lh[b];
      barrier(ctrl,P);
      if(myId===0){ const t0=Number(process.hrtime.bigint());
        let run=0;
        for(let b=0;b<256;b++){ let start=run,within=0;
          for(let p=0;p<P;p++){ ctrl[OFF+p*256+b]=start+within; within+=ctrl[HIST+p*256+b]; }
          run+=within; }
        Atomics.add(ctrl,C_SCAN_NS,Number(process.hrtime.bigint())-t0);
      }
      barrier(ctrl,P);
      for(let b=0;b<256;b++) loff[b]=ctrl[OFF+myId*256+b];
      for(let i=lo;i<hi;i++){ const v=src[i]; dst[loff[(v>>>sh)&0xFF]++]=v; }
      barrier(ctrl,P);
    }
    for(let i=lo;i<hi;i++) data[i]=(WA[i]-BIAS)|0;   // final in WA
    barrier(ctrl,P);
    Atomics.add(ctrl,C_DONE,1); Atomics.notify(ctrl,C_DONE);
  }
} else {
  function rng(x){return()=>{x=(x*1664525+1013904223)>>>0;return x/0x100000000;};}
  function serialRadix(a,n){ // baseline single-thread (the parallelizable work)
    const H=[new Uint32Array(257),new Uint32Array(257),new Uint32Array(257),new Uint32Array(257)];
    const S=new Uint32Array(n),D=new Uint32Array(n),B=0x80000000;
    for(let i=0;i<n;i++){const v=(a[i]+B)>>>0;S[i]=v;H[0][(v&255)+1]++;H[1][((v>>>8)&255)+1]++;H[2][((v>>>16)&255)+1]++;H[3][((v>>>24)&255)+1]++;}
    for(let b=1;b<257;b++){H[0][b]+=H[0][b-1];H[1][b]+=H[1][b-1];H[2][b]+=H[2][b-1];H[3][b]+=H[3][b-1];}
    let f=S,t=D,x;
    for(let i=0;i<n;i++){const v=f[i];t[H[0][v&255]++]=v;}x=f;f=t;t=x;
    for(let i=0;i<n;i++){const v=f[i];t[H[1][(v>>>8)&255]++]=v;}x=f;f=t;t=x;
    for(let i=0;i<n;i++){const v=f[i];t[H[2][(v>>>16)&255]++]=v;}x=f;f=t;t=x;
    for(let i=0;i<n;i++){const v=f[i];a[H[3][(v>>>24)&255]++]=v-B;}
  }
  const ok=(a,n)=>{for(let i=1;i<n;i++)if(a[i-1]>a[i])return false;return true;};
  (async()=>{
    const N=4000000;
    for(const P of [2,4]){
      const ctrlSAB=new SharedArrayBuffer((HDR+2*P*256)*4);
      const dataSAB=new SharedArrayBuffer(N*4), waSAB=new SharedArrayBuffer(N*4), wbSAB=new SharedArrayBuffer(N*4);
      const ctrl=new Int32Array(ctrlSAB), data=new Int32Array(dataSAB);
      const workers=[]; let ready=0;
      await new Promise(res=>{ for(let id=0;id<P;id++){ const w=new Worker(__filename,{workerData:{ctrlSAB,dataSAB,waSAB,wbSAB,P,myId:id,n:N}}); w.on('message',m=>{if(m==='ready'&&++ready===P)res();}); workers.push(w);} });
      const r=rng(7); const base=new Int32Array(N); for(let i=0;i<N;i++)base[i]=(r()*4e9-2e9)|0;
      function runOnce(){ data.set(base); Atomics.store(ctrl,C_DONE,0); Atomics.store(ctrl,C_COUNT,0); Atomics.store(ctrl,C_SCAN_NS,0); Atomics.add(ctrl,C_GO,1); Atomics.notify(ctrl,C_GO); let d=Atomics.load(ctrl,C_DONE); while(d<P){Atomics.wait(ctrl,C_DONE,d);d=Atomics.load(ctrl,C_DONE);} }
      runOnce(); const good=ok(data,N);                    // correctness
      const ts=[]; for(let k=0;k<5;k++){const t0=performance.now();runOnce();ts.push(performance.now()-t0);} ts.sort((a,b)=>a-b);
      const par=ts[2], scanMs=Atomics.load(ctrl,C_SCAN_NS)/1e6;
      // serial baseline
      const sw=new Int32Array(N); for(let k=0;k<2;k++){sw.set(base);serialRadix(sw,N);} const sts=[]; for(let k=0;k<5;k++){sw.set(base);const t0=performance.now();serialRadix(sw,N);sts.push(performance.now()-t0);} sts.sort((a,b)=>a-b); const ser=sts[2];
      console.log(`P=${P}: correct=${good}  serial=${ser.toFixed(1)}ms  parallel(1core)=${par.toFixed(1)}ms  serial-scan=${scanMs.toFixed(2)}ms  barriers=14`);
      Atomics.store(ctrl,C_GO,-1); Atomics.notify(ctrl,C_GO); for(const w of workers) await w.terminate();
    }
    console.log('\n(single core: parallel runs the SAME work + coordination on one core, so it cannot speed up here;');
    console.log(' correctness + the tiny serial fraction are what generalize to multicore.)');
  })();
}

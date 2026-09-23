// bench.js — the proposed Moebius DLL sort against the JavaScript engine (npm logos-sort), paired, on the shapes it was proposed for.
//
// Every cell is the MEDIAN over `pairs` adjacent pairs of t(opponent) / t(moebius), the order alternated inside each pair, so
// ABOVE 1 MEANS THE MOEBIUS SORT IS FASTER. Both sides get a fresh copy of the same plain Array<number> (the proposed code's input
// type) and must return it sorted; every output is compared with a Float64Array sort of the input. A second column pairs it with
// V8's own Array.prototype.sort (TimSort: a natural merge sort with galloping, i.e. the same family done carefully).
//
// PREDICTION, written before the first run (n = 1M, node 24): against logos-sort, every row below 1: the noisy sine 0.1-0.3x (the
// noise cuts it into runs of about two, so the merge does ~20 levels over arrays that grow by push), the clean sine 0.3-0.8x (65
// runs, 7 levels, but a million node objects built first), uniform 0.1-0.2x, near-sorted 0.1-0.3x, shuffled blocks 0.1-0.3x,
// descending teeth 0.2-0.6x. Against Array.prototype.sort: 0.2-0.7x everywhere, since TimSort finds the same runs without a list.
// STOP RULE: it replaces nothing unless a row clears 1.05 against logos-sort.
//
// MEASURED, 2026-09-21, node 24 (15 pairs at 1M, 30 at 100k); every number below 1 is how much slower the Moebius sort is:
//                           vs logos-sort 1M / 100k    vs Array.prototype.sort 1M / 100k
//   sine + slope, clean       0.13 / 0.24                0.20 / 0.38
//   sine + slope, noise 50    0.11 / 0.12                0.71 / 0.81
//   descending teeth x64      0.15 / 0.30                0.76 / 0.86
//   uniform                   0.08 / 0.07                1.18 / 1.33
//   near_sorted 1%            0.12 / 0.12                0.55 / 0.54
//   block_perm 64             0.09 / 0.19                0.19 / 0.38
//   few_distinct              0.07 / 0.10                0.49 / 0.79
// Against the engine it is 3-14x slower on every row, the sine rows included. GRADED: every row below 1 as predicted, but LOWER
// than predicted on the clean sine (0.3-0.8 predicted) because the million node objects and the arrays grown by push cost more
// than the 7 merge rounds. Against V8's sort the prediction (0.2-0.7) was WRONG on uniform: 1.18-1.33, because V8 calls the
// comparator function per compare and this merge compares numbers inline; and noise and teeth came in higher (0.71-0.86).
//
// usage: node --expose-gc experiments/moebius_dll/bench.js [pairs] [n]
'use strict';
const path = require('path');
const { moebiusSort } = require('./moebius_dll.js');
const logos = require(path.join(__dirname, '..', '..', 'engines', 'js', 'index.js'));

const pairs = +(process.argv[2] || 15), n = +(process.argv[3] || 1000000);
let S = 88172645463325252 >>> 0;                   // a 32-bit xorshift for the generators
function fr() { S ^= S << 13; S >>>= 0; S ^= S >>> 17; S ^= S << 5; S >>>= 0; return S / 4294967296; }
function shape(name) {
    S = 2463534242 >>> 0;
    const a = new Array(n);
    const T = n / 32, A = 0.6 * T / 2;
    switch (name) {
    case 'sine + slope, clean':     for (let i = 0; i < n; i++) a[i] = i + A * Math.sin(2 * Math.PI * i / T); break;
    case 'sine + slope, noise 50':  for (let i = 0; i < n; i++) a[i] = i + A * Math.sin(2 * Math.PI * i / T) + (fr() * 2 - 1) * 50; break;
    case 'descending teeth x64': { const tl = Math.ceil(n / 64); for (let i = 0; i < n; i++) a[i] = (tl - 1 - (i % tl)) + Math.floor(i / tl) * 1e-4 + fr() * 1e-5; } break;
    case 'uniform':                 for (let i = 0; i < n; i++) a[i] = fr() * 2e9 - 1e9; break;
    case 'near_sorted 1%':          for (let i = 0; i < n; i++) a[i] = i + fr() * 0.5; for (let k = 0; k < n / 100; k++) { const x = (fr() * n) | 0, y = (fr() * n) | 0; const t = a[x]; a[x] = a[y]; a[y] = t; } break;
    case 'block_perm 64': { const nb = 64, blk = Math.ceil(n / nb), perm = [...Array(nb).keys()];
        for (let k = nb - 1; k > 0; k--) { const r = (fr() * (k + 1)) | 0; [perm[k], perm[r]] = [perm[r], perm[k]]; }
        for (let i = 0; i < n; i++) a[i] = perm[(i / blk) | 0] * blk + (i % blk) + fr() * 0.5; } break;
    case 'few_distinct':            for (let i = 0; i < n; i++) a[i] = ((fr() * 5) | 0) * 1e6; break;
    }
    return a;
}
const SHAPES = ['sine + slope, clean', 'sine + slope, noise 50', 'descending teeth x64', 'uniform', 'near_sorted 1%', 'block_perm 64', 'few_distinct'];
const now = () => Number(process.hrtime.bigint()) / 1e6;
const gc = global.gc || (() => {});
function sameAsRef(out, ref) { if (out.length !== ref.length) return false; for (let i = 0; i < ref.length; i++) if (out[i] !== ref[i]) return false; return true; }
function paired(base, opp, ref) {                     // median of t(opp) / t(moebius)
    const r = []; let ok = true;
    for (let i = 0; i < pairs; i++) {
        let tm = 0, to = 0;
        for (let k = 0; k < 2; k++) {
            const moebiusFirst = (i & 1) ? k === 1 : k === 0;
            const a = base.slice(); gc();
            const s = now();
            const out = moebiusFirst ? moebiusSort(a) : opp(a);
            const t = now() - s;
            if (moebiusFirst) tm = t; else to = t;
            if (!sameAsRef(out, ref)) ok = false;
        }
        r.push(to / tm);
    }
    r.sort((x, y) => x - y);
    const med = pairs & 1 ? r[pairs >> 1] : (r[pairs / 2 - 1] + r[pairs / 2]) / 2;
    return { med, p10: r[Math.floor(0.1 * pairs)], p90: r[Math.floor(0.9 * pairs)], ok };
}
const vsLogos = (a) => logos.sort(a);
const vsTim = (a) => a.sort((x, y) => x - y);
console.log(`MOEBIUS DLL SORT (as proposed) vs the JS engine, n = ${n}, ${pairs} pairs, node ${process.version}${global.gc ? '' : ' (no --expose-gc: GC lands where it lands)'}`);
console.log('each cell: median of t(opponent) / t(moebius) [p10 p90]. ABOVE 1 = THE MOEBIUS SORT IS FASTER.\n');
console.log('  ' + 'shape'.padEnd(26) + '| ' + 'vs logos-sort (JS engine)'.padEnd(28) + '| vs Array.prototype.sort (V8 TimSort)');
for (const name of SHAPES) {
    const base = shape(name);
    const ref = Float64Array.from(base).sort();
    const L = paired(base, vsLogos, ref), T = paired(base, vsTim, ref);
    const cell = (x) => `${x.med.toFixed(3)} [${x.p10.toFixed(2)} ${x.p90.toFixed(2)}]${x.ok ? '  ' : ' WRONG'}`;
    console.log('  ' + name.padEnd(26) + '| ' + cell(L).padEnd(28) + '| ' + cell(T));
}
console.log('\nEvery output compared with a Float64Array sort of the input.');

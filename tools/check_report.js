// check_report.js — run a generated report's inline script under a stub DOM and fail if it does not render.
//
// Usage: node tools/check_report.js reports/scale-report.html
//        (make profile / make scale-full run it automatically when node is present)
//
// The scale report draws with uPlot into a canvas, so there is no SVG left to count. Instead uPlot is
// replaced by a stub that ASSERTS the contract every chart must satisfy: one data array per series,
// all the same length, x strictly positive and ascending, no NaN, nothing non-positive on a log axis,
// every series carrying a stroke and a label, and every formatter callable including on a null. That
// catches the mistakes a browser would only show as a blank panel. Both axis modes are exercised.
const fs = require('fs');
const path = process.argv[2];
if (!path) { console.error('usage: check_report.js <report.html>'); process.exit(1); }
const html = fs.readFileSync(path, 'utf8');

const scripts = [...html.matchAll(/<script>([\s\S]*?)<\/script>/g)].map(m => m[1]);
if (!scripts.length) { console.error('no script'); process.exit(1); }
// The report's own script is the one that reads DATA; earlier blocks are vendored libraries.
let src = scripts.find(s => /\bconst DATA\s*=/.test(s)) || scripts[0];

// ---------------------------------------------------------------- stub DOM
const els = {}, made = [];
function mkEl(id) {
  const e = {
    id, tag: id, innerHTML: '', textContent: '', className: '', clientWidth: 520, clientHeight: 300,
    children: [], dataset: {}, style: {}, onclick: null,
    setAttribute(k, v) { this.dataset['attr_' + k] = v; }, getAttribute(k) { return this.dataset['attr_' + k]; },
    appendChild(c) { this.children.push(c); return c; },
    querySelectorAll(sel) { return made.filter(x => x.tag === 'button' && (!/\[data-ax\]/.test(sel) || x.dataset.ax)); },
    addEventListener() {}, getBoundingClientRect() { return { width: 520, height: 300, left: 0, top: 0 }; },
  };
  made.push(e);
  return e;
}
function el(id) { if (!els[id]) els[id] = mkEl(id); return els[id]; }
global.document = {
  documentElement: mkEl('html'),
  getElementById: id => el(id),
  querySelector: sel => el(sel),
  createElement: tag => { const e = mkEl(tag); e.tag = tag; return e; },
  addEventListener() {},
};
global.getComputedStyle = () => ({ getPropertyValue: () => '#2a78d6' });
global.matchMedia = () => ({ matches: false, addEventListener() {}, addListener() {} });
global.addEventListener = () => {};
global.removeEventListener = () => {};

// ---------------------------------------------------------------- validating uPlot stub
const CREATED = [];
let destroyed = 0;
function uPlot(opts, data, holder) {
  const where = `chart #${CREATED.length}`;
  const die = m => { throw new Error(`${where}: ${m}`); };
  if (!opts || !opts.series || !Array.isArray(data)) die('bad arguments');
  if (opts.series.length !== data.length) die(`${opts.series.length} series but ${data.length} data arrays`);
  if (!holder) die('no container');
  const xs = data[0];
  if (!Array.isArray(xs) || !xs.length) die('empty x axis');
  for (let i = 0; i < xs.length; i++) {
    if (!Number.isFinite(xs[i]) || xs[i] <= 0) die(`x[${i}] is ${xs[i]}`);
    if (i && xs[i] <= xs[i - 1]) die(`x is not ascending at ${i}`);
  }
  const logY = opts.scales && opts.scales.y && opts.scales.y.distr === 3;
  for (let s = 1; s < data.length; s++) {
    if (!Array.isArray(data[s]) || data[s].length !== xs.length) die(`series ${s} has ${data[s] && data[s].length} points, x has ${xs.length}`);
    for (const v of data[s]) {
      if (v === null) continue;
      if (!Number.isFinite(v)) die(`series ${s} contains ${v}`);
      if (logY && v <= 0) die(`series ${s} has ${v} on a log axis`);
    }
    const cfg = opts.series[s];
    if (!cfg.stroke || typeof cfg.stroke !== 'string') die(`series ${s} has no stroke colour`);
    if (!cfg.label || typeof cfg.label !== 'string') die(`series ${s} has no label`);
    if (typeof cfg.value === 'function') { cfg.value(null, data[s].find(v => v !== null) ?? null); cfg.value(null, null); }
  }
  if (!(opts.width > 0) || !(opts.height > 0)) die(`size ${opts.width}x${opts.height}`);
  // uPlot hands a log axis's `values` a splits array with NULL in every slot it wants gridded but
  // not labelled. Formatters that assume a number throw there, and the whole canvas comes up blank
  // with only the legend drawn. Exercise that shape explicitly, in both directions.
  (opts.axes || []).forEach((a, i) => {
    if (typeof a.values !== 'function') return;
    const plain = i ? [0.001, 0.01, 1, 10, 1000] : xs;
    const withNulls = plain.map((v, j) => (j % 2 ? null : v));
    [plain, withNulls].forEach(sp => {
      const out = a.values(null, sp);
      if (!Array.isArray(out) || out.length !== sp.length) die(`axis ${i} values() returned ${out && out.length} labels for ${sp.length} splits`);
      out.forEach((lbl, j) => { if (lbl != null && typeof lbl !== 'string' && typeof lbl !== 'number') die(`axis ${i} label ${j} is ${typeof lbl}`); });
    });
  });
  CREATED.push({ opts, data, nseries: data.length - 1 });
  this.destroy = () => { destroyed++; };
  this.setSize = sz => { if (!sz || !(sz.width > 0)) throw new Error('setSize got a bad size'); };
  this.setData = () => {};
  this.data = data;
}
global.uPlot = uPlot;

// ---------------------------------------------------------------- run
function run(label) {
  const before = CREATED.length;
  try { new Function(src)(); } catch (e) { console.error(`SCRIPT THREW (${label}):`, e.stack); process.exit(2); }
  return CREATED.length - before;
}
const isScale = /new uPlot\(/.test(src);   // the uPlot scale report; anything else is the profile report
const n1 = run('linear axes');
console.log('sub:', el('sub').textContent);

if (isScale && CREATED.length) {
  // flip to the log-log axes through the real button handler and re-validate every chart
  const logBtn = made.find(e => e.tag === 'button' && /log-log/.test(e.textContent));
  if (!logBtn || typeof logBtn.onclick !== 'function') { console.error('no log-log toggle found'); process.exit(3); }
  const before = CREATED.length;
  try { logBtn.onclick(); } catch (e) { console.error('LOG TOGGLE THREW:', e.stack); process.exit(2); }
  const n2 = CREATED.length - before;
  const series = CREATED.slice(0, n1).reduce((a, c) => a + c.nseries, 0);
  console.log(`scale charts: ${n1} linear + ${n2} log, ${series} series total, ${destroyed} destroyed on re-render`);
  if (n1 < 2 || n2 !== n1) { console.error('RENDER INCOMPLETE'); process.exit(3); }
  const tables = made.filter(e => /<table/.test(e.innerHTML)).length;
  console.log('tables:', tables);
  if (tables < n1) { console.error('RENDER INCOMPLETE: a panel has no data table'); process.exit(3); }
} else if (isScale) {
  console.error('RENDER INCOMPLETE: no charts created'); process.exit(3);
} else {
  const count = (s, re) => (s.match(re) || []).length;
  const sum = el('#sum tbody').innerHTML, cards = el('cards').innerHTML, ff1 = el('ff1').innerHTML, ffP = el('ffP').innerHTML;
  console.log('summary rows:', count(sum, /<tr>/g));
  console.log('cards:', count(cards, /<div class="card">/g), ' treemaps:', count(cards, /class="tm"/g), ' thread-bar svgs:', count(cards, /class="bars"/g));
  console.log('fair-fight bars 1T:', count(ff1, /<rect/g), ' PT:', count(ffP, /<rect/g));
  if (count(sum, /<tr>/g) < 2 || !cards.includes('class="tm"') || !ff1.includes('<rect')) { console.error('RENDER INCOMPLETE'); process.exit(3); }
}
console.log('OK');

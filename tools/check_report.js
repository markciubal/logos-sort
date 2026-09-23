// check_report.js — run a generated report's inline script under a stub DOM and fail if it does not render.
//
// Usage: node tools/check_report.js reports/scale-report.html
//        (make profile / make scale-full run it automatically when node is present)
//
// The scale report draws with uPlot into a canvas, so there is no SVG left to count. Instead uPlot is
// replaced by a stub that ASSERTS the contract every chart must satisfy: one data array per series,
// all the same length, x ascending and never negative, no NaN, nothing non-positive on a log axis (n = 0
// is a real row since 2026-09: legal on a linear n axis, fatal on a log one), the ns-per-element charts
// never on a log value axis, every series carrying a stroke and a label, and every formatter callable
// including on a null. That catches the mistakes a browser would only show as a blank panel. Both axis
// modes and every size preset are exercised.
//
// Since 2026-09 the charts zoom and pan, so the stub also KEEPS SCALE STATE and drives every chart through the
// report's interaction layer (window.__SCALE = {CH, IX}): zoom, pan to the edge, zoom until it stops, the value
// axis alone, reset, a drag selection through the mouse binding and the setSelect hook, the wheel with and
// without Ctrl, the keys, the box by the pointer, a series hidden while zoomed. Every range the stub is handed is
// asserted on. Two things it had to learn from the real library, after tools/browser_check.py found what it had
// passed: uPlot rounds an explicit log x window (far end up to the next decade) unless the scale brings its own
// range function, and its log ticks loop forever on a window whose lower end is below 1 with a mantissa of 9.5+,
// so a log axis must bring its own splits(). A fake of a library has to reproduce its awkward behaviour.
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
  const cls = new Set();
  const e = {
    id, tag: id, innerHTML: '', textContent: '', className: '', clientWidth: 520, clientHeight: 300, offsetWidth: 0, offsetHeight: 0,
    children: [], dataset: {}, style: {}, onclick: null, listeners: {},
    classList: { add: c => cls.add(c), remove: c => cls.delete(c), contains: c => cls.has(c), toggle: c => (cls.has(c) ? (cls.delete(c), false) : (cls.add(c), true)) },
    setAttribute(k, v) { this.dataset['attr_' + k] = v; }, getAttribute(k) { return this.dataset['attr_' + k]; },
    appendChild(c) { this.children.push(c); return c; },
    // 'button[data-ax]' and friends: the buttons that carry that data attribute
    querySelectorAll(sel) { const m = /\[data-(\w+)\]/.exec(sel); return made.filter(x => x.tag === 'button' && (m ? x.dataset[m[1]] : true)); },
    querySelector(sel) { return mkEl(sel); },
    addEventListener(ev, fn) { (this.listeners[ev] = this.listeners[ev] || []).push(fn); }, removeEventListener() {},
    focus() {}, blur() {}, scrollIntoView() {},
    getBoundingClientRect() { return { width: 520, height: 300, left: 0, top: 0 }; },
  };
  made.push(e);
  return e;
}
function el(id) { if (!els[id]) els[id] = mkEl(id); return els[id]; }
global.document = {
  documentElement: mkEl('html'),
  activeElement: null,
  getElementById: id => el(id),
  querySelector: sel => el(sel),
  createElement: tag => { const e = mkEl(tag); e.tag = tag; return e; },
  addEventListener() {}, removeEventListener() {},
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
  const logX = opts.scales && opts.scales.x && opts.scales.x.distr === 3;
  for (let i = 0; i < xs.length; i++) {
    if (!Number.isFinite(xs[i]) || xs[i] < 0) die(`x[${i}] is ${xs[i]}`);
    if (logX && xs[i] <= 0) die(`x[${i}] is ${xs[i]} on a log axis`);
    if (i && xs[i] <= xs[i - 1]) die(`x is not ascending at ${i}`);
  }
  const logY = opts.scales && opts.scales.y && opts.scales.y.distr === 3;
  if (opts.class === 'ch-nsel' && logY) die('the ns-per-element chart is on a log value axis; it must stay linear');
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
  CREATED.push({ opts, data, nseries: data.length - 1, logX, kind: opts.class === 'ch-nsel' ? 'nsel' : 'time' });
  this.destroy = () => { destroyed++; };
  this.setSize = sz => { if (!sz || !(sz.width > 0)) throw new Error('setSize got a bad size'); };
  this.setData = () => {};
  this.data = data;

  // ---- what the report's interaction layer (its IX block, 2026-09) drives. The stub keeps real scale state, maps
  // pixels to values the way uPlot does (linearly in log10 on a log axis), and ASSERTS on every range it is handed:
  // finite, not empty, positive on a log axis, never outside the measured sizes, never below zero on a linear value
  // axis. A zoom that would leave any of those shows up here rather than as a blank canvas.
  const self = this, hooks = opts.hooks || {}, PW = 500, PH = 280;       // the plot, in CSS pixels
  for (const k in hooks) if (!Array.isArray(hooks[k]) || hooks[k].some(f => typeof f !== 'function')) die(`hooks.${k} is not an array of functions`);
  this.hooks = hooks;
  this.over = mkEl('u-over'); this.over.clientWidth = PW; this.over.clientHeight = PH;
  this.over.getBoundingClientRect = () => ({ left: 60, top: 10, width: PW, height: PH });
  this.wrap = mkEl('u-wrap'); this.root = mkEl('uplot'); this.root.querySelector = () => self.wrap;
  this.series = opts.series.map(s => Object.assign({ show: true }, s));
  const yext = () => { let mn = Infinity, mx = -Infinity;
    for (let s = 1; s < data.length; s++) { if (!self.series[s].show) continue; for (const v of data[s]) if (v !== null) { if (v < mn) mn = v; if (v > mx) mx = v; } }
    return [mn, mx]; };
  const ysc = (opts.scales && opts.scales.y) || {};
  const autoY = () => { const [mn, mx] = yext();
    if (typeof ysc.range === 'function') return ysc.range(self, mn, mx, 'y');
    return logY ? [Math.pow(10, Math.floor(Math.log10(mn))), Math.pow(10, Math.ceil(Math.log10(mx)))] : [mn, mx]; };
  const y0 = autoY();
  this.scales = { x: { distr: logX ? 3 : 1, min: xs[0], max: xs[xs.length - 1] }, y: { distr: logY ? 3 : 1, min: y0[0], max: y0[1] } };
  this.cursor = { left: -10, top: -10, idx: null };
  this.select = { left: 0, top: 0, width: 0, height: 0 };
  this.batch = fn => fn(self);
  // uPlot passes EVERY n window through the x scale's range function, explicit ones too (a value window it takes at its
  // word). Its default for a log scale rounds the near end down to the step of its decade and the far end UP to the next
  // decade, so 100 .. 25118 comes back as 100 .. 100000 and nothing inside one decade can be zoomed. The first cut of
  // the interaction layer passed this stub and failed in the browser for exactly that; the stub rounds the same way now.
  const xsc = (opts.scales && opts.scales.x) || {};
  const xRange = (mn, mx) => {
    if (typeof xsc.range === 'function') return xsc.range(self, mn, mx, 'x');
    if (!logX) return [mn, mx];
    const lo = Math.pow(10, Math.floor(Math.log10(mn))), hi = Math.pow(10, Math.ceil(Math.log10(mx)));
    return [Math.floor(mn / lo) * lo, Math.ceil(mx / hi) * hi];
  };
  { const h = xRange(xs[0], xs[xs.length - 1]); self.scales.x.min = h[0]; self.scales.x.max = h[1]; }   // the first view goes through it as well

  // A log axis must bring its own ticks. uPlot's walk up from the scale's lower end in steps of that end's decade and
  // LOOP FOREVER when the end lies below 1 with a mantissa of 9.5 or more (9.6e-6 + 1e-6 rounds to 1.1e-5, which becomes a
  // step it has no decimals for, and every later tick rounds to 0): the browser dies of 'Invalid array length'. Only a
  // zoomed or panned window has such an end, so only the real browser found it. Every window below is one that did, or
  // nearly did; the ticks must be finite, ascending, inside the window and few, the labels a subset that is not empty.
  (opts.axes || []).forEach((a, i) => {
    const key = i ? 'y' : 'x', sc = self.scales[key];
    if (sc.distr !== 3) return;
    if (typeof a.splits !== 'function') die(`axis ${i} sits on a log scale without its own splits(): uPlot's default loops forever on a panned window`);
    const keep = [sc.min, sc.max];
    for (const [mn, mx] of [[9.6268e-6, 0.61], [7.58, 1904.05], [12.27, 20.46], [1, 5e7], [0.95, 1.05], [9.99e-7, 1.5e-6], [3, 10], [1e-7, 1e4], [6.3e-4, 6.4e-4]]) {
      sc.min = mn; sc.max = mx;
      const sp = a.splits(self, i, mn, mx, 1, 50);
      if (!Array.isArray(sp) || !sp.length || sp.length > 150) die(`axis ${i}: ${sp && sp.length} ticks for the window ${mn} .. ${mx}`);
      sp.forEach((v, j) => { if (!Number.isFinite(v) || v < mn * (1 - 1e-9) || v > mx * (1 + 1e-9) || (j && v <= sp[j - 1])) die(`axis ${i}: tick ${j} is ${v} in the window ${mn} .. ${mx}`); });
      const lab = typeof a.filter === 'function' ? a.filter(self, sp, i, 50, 1) : sp;
      if (!Array.isArray(lab) || lab.length !== sp.length || lab.some((v, j) => v != null && v !== sp[j])) die(`axis ${i}: filter() did not return the ticks with nulls for the unlabelled ones`);
      const shown = lab.filter(v => v != null).length;
      if (!shown || shown > 40) die(`axis ${i}: ${shown} labels for the window ${mn} .. ${mx}`);
      if (typeof a.values === 'function' && a.values(self, lab).length !== lab.length) die(`axis ${i}: values() dropped a tick`);
    }
    sc.min = keep[0]; sc.max = keep[1];
  });
  this.setScale = (key, r) => {
    const sc = self.scales[key];
    if (!sc || !r) die(`setScale('${key}') on an unknown scale or without a range`);
    if (r.min == null && r.max == null) {
      if (key !== 'y') die('only the value axis may be handed back to the automatic range');
      const a = autoY(); sc.min = a[0]; sc.max = a[1]; return;
    }
    if (key === 'x' && Number.isFinite(r.min) && Number.isFinite(r.max)) { const a = xRange(r.min, r.max); r = { min: a[0], max: a[1] }; }
    if (!Number.isFinite(r.min) || !Number.isFinite(r.max)) die(`setScale('${key}') got ${r.min} .. ${r.max}`);
    if (!(r.max > r.min)) die(`setScale('${key}') got the empty range ${r.min} .. ${r.max}`);
    if (sc.distr === 3 && r.min <= 0) die(`setScale('${key}') got ${r.min} on a log axis`);
    const last = xs[xs.length - 1], eps = 1e-9 * Math.max(1, Math.abs(last));
    if (key === 'x' && (r.min < xs[0] - eps || r.max > last + eps)) die(`the n window ${r.min} .. ${r.max} leaves the measured sizes ${xs[0]} .. ${last}`);
    if (key === 'y' && sc.distr !== 3 && r.min < 0) die(`the value axis went below zero: ${r.min}`);
    sc.min = r.min; sc.max = r.max;
  };
  this.setSelect = (s, fire) => { Object.assign(self.select, s); if (fire !== false) (hooks.setSelect || []).forEach(h => h(self)); };
  this.posToVal = (px, key) => {
    const sc = self.scales[key], f = key === 'x' ? px / PW : 1 - px / PH;
    if (sc.distr === 3) { const a = Math.log10(sc.min), b = Math.log10(sc.max); return Math.pow(10, a + f * (b - a)); }
    return sc.min + f * (sc.max - sc.min);
  };
  this.syncRect = () => {};
  // the mouse bindings must hand uPlot a listener; keep them, the exercise below sends events through them
  this.listeners = {}; this.handled = 0;
  const bind = (opts.cursor && opts.cursor.bind) || {};
  for (const ev in bind) { const l = bind[ev](self, self.over, () => { self.handled++; }, true); if (typeof l !== 'function') die(`cursor.bind.${ev} did not return a listener`); self.listeners[ev] = l; }
  if (opts.cursor && opts.cursor.sync && !opts.cursor.sync.key) die('cursor.sync has no key');
  // uPlot fires 'ready' from its first commit, which is a microtask: after the constructor has returned
  READY.push(() => (hooks.ready || []).forEach(h => h(self)));
}
const READY = [];
const drainReady = () => { while (READY.length) READY.shift()(); };
global.uPlot = uPlot;

// ---------------------------------------------------------------- the interaction layer, through its own entry points
// Every chart, in whatever mode was just rendered: zoom, pan to the edge, zoom until the window cannot narrow, the value
// axis alone, reset, a drag selection through the mouse binding and the setSelect hook, the wheel with and without
// Ctrl, the keys, the pointer box, and a series hidden while zoomed. The stub's setScale asserts on every range.
const hasIX = /__SCALE/.test(src);      // a report written before the interaction layer still has to render; it is just not driven
function exercise(label) {
  if (!hasIX) return 0;
  // the stub's setScale throws on a range no chart should be handed: report it as what it is, not as a stack
  try { return exercise1(label); } catch (e) { console.error(`INTERACTION (${label}): ${e.message}`); process.exit(3); }
}
function exercise1(label) {
  const S = global.__SCALE;
  const fail = m => { console.error(`INTERACTION (${label}): ${m}`); process.exit(3); };
  if (!S || !S.IX || !Array.isArray(S.CH)) fail('no __SCALE test hook: the interaction layer cannot be checked');
  const { IX, CH } = S;
  const near = (a, b) => Math.abs(a - b) <= 1e-9 * Math.max(1, Math.abs(a), Math.abs(b));
  const evt = o => Object.assign({ deltaX: 0, deltaY: 0, deltaMode: 0, ctrlKey: false, metaKey: false, altKey: false, shiftKey: false, button: 0,
    clientX: 60 + 250, clientY: 10 + 140, prevented: false, preventDefault() { this.prevented = true; } }, o);
  for (const c of CH) {
    const u = c.u, lg = u.scales.x.distr === 3, T = v => (lg ? Math.log10(v) : v);
    const span = () => T(u.scales.x.max) - T(u.scales.x.min), X = () => [u.scales.x.min, u.scales.x.max];
    const home = X(), homeY = [u.scales.y.min, u.scales.y.max], s0 = span(), sibs = u._sibs || [];
    const atHome = what => { if (!near(u.scales.x.min, home[0]) || !near(u.scales.x.max, home[1])) fail(`${what}: the n window is ${X()}, home is ${home}`);
      if (!near(u.scales.y.min, homeY[0]) || !near(u.scales.y.max, homeY[1])) fail(`${what}: the value axis is ${u.scales.y.min} .. ${u.scales.y.max}, home is ${homeY}`); };
    const follow = what => sibs.forEach(v => { if (!near(v.scales.x.min, u.scales.x.min) || !near(v.scales.x.max, u.scales.x.max)) fail(`${what}: the panel twin shows ${v.scales.x.min} .. ${v.scales.x.max}, this chart ${X()}`); });
    if (!sibs.length) fail('a chart has no panel twin to share its n window with');
    if (u.data[0].length < 3) continue;                       // two sizes: nothing to zoom into

    for (let k = 0; k < 2; k++) IX.zoomX(u, null, 0.6);
    if (!(span() < s0)) fail('zoomX did not narrow the n window'); follow('zoom');
    const s1 = span(); IX.panX(u, 0.3); if (!near(span(), s1)) fail(`panX changed the width of the window: ${s1} -> ${span()}`); follow('pan');
    IX.panX(u, -50); if (!near(u.scales.x.min, home[0])) fail('a long pan to the left did not stop at the smallest size'); follow('pan to the edge');
    for (let k = 0; k < 80; k++) IX.zoomX(u, null, 0.5);
    if (!(span() > 0) || !(u.scales.y.max > u.scales.y.min)) fail('a deep zoom left an empty axis'); follow('deep zoom');
    const xb = X(); IX.zoomY(u, null, 0.5);
    if (!near(u.scales.x.min, xb[0]) || !near(u.scales.x.max, xb[1])) fail('zoomY moved the n window');
    IX.reset(u); atHome('reset'); follow('reset');

    // a drag selection as wide as 150..350 px of the 500 px plot and as tall as the plot: a range of n
    u.listeners.mousedown(evt({ target: u.over }));
    if (!u.handled) fail("a plain mousedown did not reach uPlot's own handler");
    const want = [u.posToVal(150, 'x'), u.posToVal(350, 'x')];
    u.setSelect({ left: 150, top: 0, width: 200, height: 280 });
    if (u.select.width !== 0 || u.select.height !== 0) fail('the selection box was left on the chart');
    // the window is the selection, or the smallest window allowed around its middle where the selection was narrower than that
    const ww = T(want[1]) - T(want[0]), mid = (T(want[0]) + T(want[1])) / 2;
    if (!(span() < s0) || span() < ww - 1e-9 * s0) fail(`a drag selection of ${want} gave ${X()}`);
    if (near(span(), ww) ? !near(T(u.scales.x.min), T(want[0])) : !(T(u.scales.x.min) <= mid && mid <= T(u.scales.x.max))) fail(`a drag selection of ${want} landed at ${X()}`);
    follow('drag selection');
    // the twin mirrors that box and fires the same hook: it must not act on it
    const twin = sibs[0], tx = [twin.scales.x.min, twin.scales.x.max];
    twin.setSelect({ left: 0, top: 0, width: 100, height: 280 });
    if (!near(twin.scales.x.min, tx[0]) || !near(twin.scales.x.max, tx[1]) || twin.select.width !== 0) fail('the panel twin acted on its mirrored selection');
    u.listeners.dblclick(evt({ target: u.over })); atHome('double-click');

    // the wheel: plain over an inactive chart is the page's; with Ctrl it zooms around the pointer; with Shift it moves along n
    let e = evt({ deltaY: -120 }); u.wrap.listeners.wheel[0](e);
    if (e.prevented || !near(span(), s0)) fail('a plain wheel over an inactive chart was taken from the page');
    e = evt({ deltaY: -240, ctrlKey: true }); u.wrap.listeners.wheel[0](e);
    if (!e.prevented || !(span() < s0)) fail('Ctrl + wheel did not zoom'); follow('Ctrl + wheel');
    const before = X(); e = evt({ deltaY: 120, shiftKey: true }); u.wrap.listeners.wheel[0](e);
    if (!e.prevented || near(u.scales.x.min, before[0])) fail('Shift + wheel did not move along n'); follow('Shift + wheel');
    e = evt({ deltaY: -120, ctrlKey: true, clientX: 30 }); const xk = X(), yk = u.scales.y.max - u.scales.y.min; u.wrap.listeners.wheel[0](e);
    if (!near(u.scales.x.min, xk[0]) || !(u.scales.y.max - u.scales.y.min < yk)) fail('Ctrl + wheel over the value axis did not zoom that axis alone');
    // the keys
    const key = k => { const ev = evt({ key: k }); u.over.listeners.keydown[0](ev); return ev; };
    key('0'); atHome("the '0' key");
    if (!key('+').prevented || !(span() < s0)) fail("the '+' key did not zoom");
    const kb = X(); key('ArrowRight'); if (near(u.scales.x.min, kb[0]) && near(u.scales.x.max, kb[1]) && !near(u.scales.x.max, home[1])) fail('ArrowRight did not move');
    if (key('a').prevented) fail('an unused key was swallowed');
    // a series hidden while zoomed: the value axis refits without it (uPlot has already cleared its show flag when the hook fires)
    u.series[1].show = false; (u.hooks.setSeries || []).forEach(h => h(u, 1, { show: false }));
    if (!(u.scales.y.max > u.scales.y.min)) fail('hiding a series while zoomed left an empty value axis');
    u.series[1].show = true; (u.hooks.setSeries || []).forEach(h => h(u, 1, { show: true }));
    // the box by the pointer
    u._in = true; u.cursor = { left: 120, top: 60, idx: Math.min(2, u.data[0].length - 1) }; IX.tip(u);
    if (!u._tip || u._tip.style.display !== 'block' || !/n = /.test(u._tip.innerHTML) || !/<tr>/.test(u._tip.innerHTML)) fail('the box by the pointer did not render');
    u._in = false; IX.tip(u); if (u._tip.style.display !== 'none') fail('the box by the pointer stayed up after the pointer left');
    IX.reset(u); atHome('the last reset'); follow('the last reset');
  }
  return CH.length;
}

// ---------------------------------------------------------------- run
function run(label) {
  const before = CREATED.length;
  try { new Function(src)(); } catch (e) { console.error(`SCRIPT THREW (${label}):`, e.stack); process.exit(2); }
  return CREATED.length - before;
}
const isScale = /new uPlot\(/.test(src);   // the uPlot scale report; anything else is the profile report
const n1 = run('linear axes');
drainReady();
let driven = 0, views = 0;
if (isScale && CREATED.length) { driven += exercise('linear axes'); views++; }
console.log('sub:', el('sub').textContent);

if (isScale && CREATED.length) {
  // flip to the log-log axes through the real button handler and re-validate every chart
  const logBtn = made.find(e => e.tag === 'button' && /log-log/.test(e.textContent));
  if (!logBtn || typeof logBtn.onclick !== 'function') { console.error('no log-log toggle found'); process.exit(3); }
  const before = CREATED.length;
  try { logBtn.onclick(); } catch (e) { console.error('LOG TOGGLE THREW:', e.stack); process.exit(2); }
  drainReady(); driven += exercise('log-log axes'); views++;
  const n2 = CREATED.length - before;
  const series = CREATED.slice(0, n1).reduce((a, c) => a + c.nseries, 0);
  console.log(`scale charts: ${n1} linear + ${n2} log, ${series} series total, ${destroyed} destroyed on re-render`);
  if (n1 < 2 || n2 !== n1) { console.error('RENDER INCOMPLETE'); process.exit(3); }
  // a panel is one time chart, one data table and (in reports that have them) one ns-per-element chart
  const lin = CREATED.slice(0, n1), timeCharts = lin.filter(c => c.kind === 'time').length, nsCharts = n1 - timeCharts;
  const tables = made.filter(e => /<table/.test(e.innerHTML)).length;
  console.log(`panels: ${timeCharts} time charts, ${nsCharts} ns-per-element charts, ${tables} tables`);
  if (tables < timeCharts) { console.error('RENDER INCOMPLETE: a panel has no data table'); process.exit(3); }
  if (nsCharts && nsCharts !== timeCharts) { console.error('RENDER INCOMPLETE: a panel has no ns-per-element chart'); process.exit(3); }
  // n = 0: if the data has it, the linear view must actually draw it and the log view must not
  if (/"n":0,/.test(html)) {
    const zl = lin.filter(c => c.data[0][0] === 0).length, zg = CREATED.slice(before, before + n2).filter(c => c.data[0][0] === 0).length;
    console.log(`n = 0: drawn in ${zl} linear charts, ${zg} log charts`);
    if (!zl || zg) { console.error('n = 0 must appear on the linear n axis and never on the log one'); process.exit(3); }
  }
  // every size preset, in both axis modes, through the real button handlers (the stub validates each chart)
  const presets = made.filter(e => e.tag === 'button' && e.dataset.rg && e.dataset.rg !== 'all');
  const linBtn = made.find(e => e.tag === 'button' && e.dataset.ax === 'lin'), allBtn = made.find(e => e.tag === 'button' && e.dataset.rg === 'all');
  for (const axis of [logBtn, linBtn]) {
    if (!axis) continue;
    for (const b of presets) {
      const at = CREATED.length;
      try { axis.onclick(); b.onclick(); } catch (e) { console.error(`PRESET '${b.textContent}' THREW:`, e.stack); process.exit(2); }
      const made2 = CREATED.length - at;
      drainReady(); driven += exercise(`preset '${b.textContent}' on ${axis.dataset.ax} axes`); views++;
      console.log(`preset '${b.textContent}' on ${axis.dataset.ax} axes: ${made2} charts`);
      if (made2 < 2) { console.error('RENDER INCOMPLETE: a size preset drew nothing'); process.exit(3); }
    }
  }
  if (allBtn) allBtn.onclick();
  console.log(hasIX ? `interaction: ${driven} charts driven through zoom, pan, selection, wheel, keys and the pointer box, in ${views} views`
                    : 'interaction: this report predates the interaction layer (no __SCALE hook), so its charts were not driven');
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

// check_index.js -- run index.html's hover script under a stub DOM and assert what the tooltip says.
//
//   node tools/check_index.js index.html
//
// The page's charts are static SVG; the one piece of script is the pointer layer (a cursor line and a tooltip listing
// every sort at the nearest n, fastest first, each with its ratio to logos). A chart nobody has hovered is a chart
// nobody has tested, so this feeds that script stub <svg> elements built from the page's own data attributes, moves a
// pointer across each one, and checks that the tooltip names the right n and that its rows are sorted and carry the
// ratio the data implies. Each panel has two charts (time per element, total time) over the same data: both are driven, the
// tooltip must carry the TOTAL time of every sort, and the switch between the two views is clicked. Exit 1 on any failure.
const fs = require('fs');
const path = process.argv[2];
if (!path) { console.error('usage: check_index.js <index.html>'); process.exit(1); }
const html = fs.readFileSync(path, 'utf8');
const un = s => s.replace(/&quot;/g, '"').replace(/&#x27;/g, "'").replace(/&lt;/g, '<').replace(/&gt;/g, '>').replace(/&amp;/g, '&');
const svgs = [...html.matchAll(/<svg class="ch m-(?:ns|tot)" id="([^"]+)" viewBox="0 0 (\d+) (\d+)" role="img" aria-label="([^"]*)" data-n="([^"]*)" data-s="([^"]*)" data-nm="([^"]*)" data-ml="(\d+)" data-mr="(\d+)">/g)];
if (!svgs.length) { console.error('no charts found'); process.exit(1); }
const script = (/<script>([\s\S]*?)<\/script>/.exec(html) || [])[1];
if (!script) { console.error('no script'); process.exit(1); }

let fails = 0, events = 0;
const tip = { hidden: true, innerHTML: '', style: {}, offsetWidth: 200, offsetHeight: 120 };
const stubs = svgs.map(m => {
  const el = {
    dataset: { n: un(m[5]), s: un(m[6]), nm: un(m[7]), ml: m[8], mr: m[9] },
    viewBox: { baseVal: { width: +m[2], height: +m[3] } }, listeners: {}, cur: { attrs: {}, setAttribute(k, v) { this.attrs[k] = v; } },
    getAttribute(k) { return k === 'aria-label' ? un(m[4]) : null; },
    querySelector(sel) { if (sel === '.cur') return this.cur; throw new Error('unexpected selector ' + sel); },
    addEventListener(ev, fn) { (this.listeners[ev] = this.listeners[ev] || []).push(fn); },
    getBoundingClientRect() { return { left: 100, top: 100, width: 560, height: 280 }; },
    id: m[1],
  };
  return el;
});
const mkBtn = m => ({ dataset: { m }, attrs: {}, listeners: {}, setAttribute(k, v) { this.attrs[k] = v; }, addEventListener(ev, fn) { (this.listeners[ev] = this.listeners[ev] || []).push(fn); } });
const btns = [...html.matchAll(/<button type="button" data-m="(ns|tot)"/g)].map(m => mkBtn(m[1]));
if (btns.length !== 2) { console.error('expected the two view buttons, found ' + btns.length); process.exit(1); }
const gd = { checked: true, listeners: {}, addEventListener(ev, fn) { (this.listeners[ev] = this.listeners[ev] || []).push(fn); } };
global.document = {
  body: { dataset: {} },
  getElementById: id => { if (id === 'tip') return tip; if (id === 'gd') return gd; throw new Error('unexpected id ' + id); },
  querySelectorAll: sel => { if (sel === 'svg.ch') return stubs; if (sel === '.seg button') return btns; throw new Error('unexpected selector ' + sel); },
  addEventListener() {},
};
const fmtMs = v => v >= 1000 ? (v / 1000).toFixed(v >= 10000 ? 1 : 2) + ' s' : v >= 10 ? v.toFixed(0) + ' ms' : v >= 1 ? v.toFixed(1) + ' ms' : v >= 1e-3 ? (v * 1e3).toFixed(v >= 1e-2 ? 0 : 1) + ' µs' : (v * 1e6).toFixed(1) + ' ns';
global.innerWidth = 1200; global.innerHeight = 900;
try { new Function(script)(); } catch (e) { console.error('script threw:', e); process.exit(1); }
if (document.body.dataset.metric !== 'ns') { console.error('the page did not open on time per element'); fails++; }
for (const want of ['tot', 'ns']) {
  const b = btns.find(x => x.dataset.m === want); (b.listeners.click || []).forEach(fn => fn({}));
  if (document.body.dataset.metric !== want || b.attrs['aria-pressed'] !== 'true' || btns.find(x => x !== b).attrs['aria-pressed'] !== 'false') { console.error('the view switch did not move to ' + want); fails++; }
}
// growth guides (O(1) .. O(n^2)): every chart carries a group of them, each with all five labels, and the checkbox hides them
if (document.body.dataset.guides !== 'on') { console.error('the growth guides did not open switched on'); fails++; }
for (const want of [false, true]) {
  gd.checked = want; (gd.listeners.change || []).forEach(fn => fn({}));
  if (document.body.dataset.guides !== (want ? 'on' : 'off')) { console.error('the guides checkbox did not switch ' + (want ? 'on' : 'off')); fails++; }
}
const groups = [...html.matchAll(/<g class="gd" aria-hidden="true">([\s\S]*?)<\/g>/g)];
if (groups.length !== svgs.length) { console.error('growth guides on ' + groups.length + ' of ' + svgs.length + ' charts'); fails++; }
for (const gr of groups) for (const nm of ['O(1)', 'O(log n)', 'O(n)', 'O(n log n)', 'O(n²)'])
  if (!gr[1].includes('>' + nm + '<')) { console.error('a chart lacks the ' + nm + ' guide'); fails++; break; }
if (svgs.filter(m => m[0].includes('m-tot')).length * 2 !== svgs.length) { console.error('every panel needs both charts: ' + svgs.length + ' in all'); fails++; }

for (const s of stubs) {
  const ns = JSON.parse(s.dataset.n), S = JSON.parse(s.dataset.s), NM = JSON.parse(s.dataset.nm);
  if (!s.listeners.pointermove || !s.listeners.pointerleave) { console.error(s.id + ': no pointer listeners'); fails++; continue; }
  const W = 560, ml = +s.dataset.ml, mr = +s.dataset.mr, x0 = Math.log10(ns[0]), x1 = Math.log10(ns[ns.length - 1]);
  const px = n => ml + (Math.log10(n) - x0) / ((x1 - x0) || 1) * (W - ml - mr);
  for (let i = 0; i < ns.length; i++) {
    const ev = { clientX: 100 + px(ns[i]), clientY: 150 };
    s.listeners.pointermove.forEach(fn => fn(ev)); events++;
    if (tip.hidden) { console.error(s.id + ': tooltip hidden after move'); fails++; break; }
    const nTxt = Number(ns[i]).toLocaleString('en-US');
    if (!tip.innerHTML.includes('n = ' + nTxt)) { console.error(s.id + ': tooltip does not name n = ' + nTxt + ': ' + tip.innerHTML.slice(0, 120)); fails++; break; }
    const want = Object.keys(S).map(k => [k, S[k][i]]).filter(r => r[1] != null).sort((a, b) => a[1] - b[1]);
    const names = [...tip.innerHTML.matchAll(/<\/i>([^<]+)<\/td>/g)].map(m => m[1]);
    if (names.join('|') !== want.map(r => NM[r[0]]).join('|')) { console.error(s.id + ' n=' + ns[i] + ': order ' + names.join('|') + ' expected ' + want.map(r => NM[r[0]]).join('|')); fails++; break; }
    for (const [k, v] of want) { const t = '<td>' + fmtMs(v * ns[i] / 1e6) + '</td>'; if (!tip.innerHTML.includes(t)) { console.error(s.id + ' n=' + ns[i] + ': missing total time ' + t + ' for ' + NM[k]); fails++; break; } }
    const lg = S['1'][i];
    for (const [k, v] of want) if (k !== '1' && lg) { const r = (v / lg).toFixed(2) + '× logos'; if (!tip.innerHTML.includes(r)) { console.error(s.id + ' n=' + ns[i] + ': missing ratio ' + r + ' for ' + NM[k]); fails++; break; } }
    const cx = +s.cur.attrs.x1; if (Math.abs(cx - px(ns[i])) > 0.01) { console.error(s.id + ': cursor at ' + cx + ' expected ' + px(ns[i])); fails++; break; }
  }
  // between two points the nearer wins; off the right edge the last point
  s.listeners.pointermove.forEach(fn => fn({ clientX: 100 + W + 50, clientY: 150 }));
  if (!tip.innerHTML.includes('n = ' + Number(ns[ns.length - 1]).toLocaleString('en-US'))) { console.error(s.id + ': far right did not pick the last n'); fails++; }
  s.listeners.pointerleave.forEach(fn => fn({}));
  if (!tip.hidden || s.cur.attrs.x1 !== -10) { console.error(s.id + ': leave did not hide the tooltip and park the cursor'); fails++; }
}
console.log('check_index: ' + stubs.length + ' charts, ' + events + ' pointer events, ' + fails + ' failures');
process.exit(fails ? 1 : 0);

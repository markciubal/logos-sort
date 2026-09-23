#!/usr/bin/env python3
"""Render a generated report in a real headless browser, fail on any console error, and USE its charts.

`check_report.js` runs the report's script under a stub DOM, which is fast and catches most things,
but a stub only produces the inputs you thought to produce. It passed a report whose log-log charts
came up completely blank, because the real uPlot hands a log axis a splits array with NULL in every
slot it does not want labelled, and the stub only ever passed numbers. This tool has no such blind
spot: it is the actual browser, with an error trap installed before the report's own script runs.

Since 2026-09 the scale report's charts zoom and pan, and a chart nobody has dragged is a chart nobody
has tested: a wheel listener that swallows the page's scroll, a selection that lands beside the mouse,
a twin chart that does not follow, a log axis that cannot zoom, a pan that kills the tab (the last two
were real, inside uPlot, and had passed the stub: engines/c/CHANGELOG.md). So in every mode
this tool also DRIVES the first panel with synthetic events through the real uPlot: a plain wheel
(must be left to the page), Ctrl + wheel, Shift + wheel, a sideways scroll, a wheel over the value
axis, the keys, the wheel on an active chart, the panel's buttons and its expand switch, a drag
selection after the layout has moved, a double-click, both ways of panning, the box by the pointer,
a series hidden while zoomed, a zoom pushed until it stops, a pinch and a one-finger drag. Every
expectation that fails is reported as an error, next to anything the page threw.

Usage:
    python tools/browser_check.py reports/scale-report.html
    python tools/browser_check.py reports/scale-report.html --shot out.png

Exit code 0 means every mode rendered and behaved with zero errors. Skips cleanly (exit 0) when no
Chromium-family browser is installed, so it is safe to wire into a Makefile.
"""
import os
import re
import subprocess
import sys
import tempfile

CANDIDATES = [
    r"C:\Program Files\Google\Chrome\Application\chrome.exe",
    r"C:\Program Files (x86)\Google\Chrome\Application\chrome.exe",
    r"C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe",
    r"C:\Program Files\Microsoft\Edge\Application\msedge.exe",
    "/usr/bin/google-chrome", "/usr/bin/chromium", "/usr/bin/chromium-browser",
    "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome",
]

TRAP = ("<script>window.__errs=[];"
        "window.onerror=function(m,s,l,c,e){window.__errs.push(String(m)+' @'+l+':'+c);return false;};"
        "window.addEventListener('unhandledrejection',function(ev){window.__errs.push('reject: '+ev.reason);});"
        "</script>")
# The dump waits for the driver below (it sets __ixdone), or gives up after nine virtual seconds.
DUMP = ("<script>(function(){var t0=Date.now(),iv=setInterval(function(){"
        "if(!window.__ixdone&&Date.now()-t0<9000)return;clearInterval(iv);"
        "if(!window.__ixdone)window.__errs.push('the interaction driver never finished');"
        "var d=document.createElement('div');d.id='ERRDUMP';"
        "d.textContent='ERRORS['+window.__errs.length+']: '+window.__errs.join(' ||| ');"
        "document.body.appendChild(d);},100);})();</script>")

# Drives the first panel of a uPlot report through window.__SCALE = {CH, IX}, which the scale report publishes for
# this purpose. A report without uPlot (the profile report) is left alone. Notes for whoever extends it:
#   - uPlot reads its scales back only after a microtask commit, so every step awaits before it looks;
#   - while a drag is on, uPlot IGNORES a mousemove whose movementX and movementY are both zero, which is what a
#     synthetic event carries unless told otherwise;
#   - mouseenter and mouseleave do not bubble, and uPlot's listeners want the plot itself as the event's target.
DRIVER = r"""<script>(async function(){
 var E=function(m){window.__errs.push('interaction: '+m);};
 try{
  if(typeof uPlot==='undefined')return;
  var S=window.__SCALE;
  if(!S||!S.CH||!S.CH.length||!S.IX){E('no __SCALE test hook, or no charts');return;}
  var tick=function(ms){return new Promise(function(r){setTimeout(r,ms||25);});};
  await tick(80);
  var c0=S.CH[0],u=c0.u,tw=S.CH.filter(function(c){return c!==c0&&c.card===c0.card;})[0];
  if(!tw){E('the first panel has a single chart');return;}
  if(u.data[0].length<3)return;
  var v=tw.u,o=u.over,wrap=u.root.querySelector('.u-wrap');
  o.scrollIntoView({block:'center'});await tick(60);
  var lg=u.scales.x.distr===3,T=function(a){return lg?Math.log10(a):a;};
  var X=function(w){return[w.scales.x.min,w.scales.x.max];},Y=function(w){return[w.scales.y.min,w.scales.y.max];};
  var span=function(r){return T(r[1])-T(r[0]);};
  var near=function(a,b){return Math.abs(a-b)<=1e-6*Math.max(1e-9,Math.abs(a),Math.abs(b));};
  var same=function(a,b){return near(a[0],b[0])&&near(a[1],b[1]);};
  var P=function(fx,fy,extra){var r=o.getBoundingClientRect(),d={clientX:r.left+r.width*fx,clientY:r.top+r.height*fy,bubbles:true,cancelable:true,view:window};
   for(var k in (extra||{}))d[k]=extra[k];return d;};
  var home=X(u),homeY=Y(u),H=span(home);
  var follows=function(what){if(!same(X(v),X(u)))E(what+': the panel twin shows '+X(v)+', the chart '+X(u));};
  var isHome=function(what){if(!same(X(u),home))E(what+': n window '+X(u)+' instead of '+home);if(!same(Y(u),homeY))E(what+': value axis '+Y(u)+' instead of '+homeY);};
  var wheel=async function(fx,fy,extra,target){var e=new WheelEvent('wheel',P(fx,fy,extra));(target||o).dispatchEvent(e);await tick();return e;};
  var key=async function(k){var e=new KeyboardEvent('keydown',{key:k,bubbles:true,cancelable:true});o.dispatchEvent(e);await tick();return e;};
  var drag=async function(from,to,extra,onDoc){
   var n=6,x;o.dispatchEvent(new MouseEvent('mousedown',P(from,.5,Object.assign({button:0},extra||{}))));
   for(var i=1;i<=n;i++){x=from+(to-from)*i/n;(onDoc?document:o).dispatchEvent(new MouseEvent('mousemove',P(x,.5+0.004*i,{movementX:to>from?9:-9,movementY:1})));await tick(3);}
   (onDoc?document:o).dispatchEvent(new MouseEvent('mouseup',P(to,.5+0.004*n,{button:0})));await tick(40);};

  /* the wheel */
  var e=await wheel(.5,.5,{deltaY:-120});
  if(e.defaultPrevented)E('a plain wheel over an inactive chart was taken from the page');
  if(!same(X(u),home))E('a plain wheel zoomed an inactive chart');
  e=await wheel(.5,.5,{deltaY:-240,ctrlKey:true});var z1=X(u);
  if(!e.defaultPrevented)E('Ctrl + wheel was left to the browser');
  if(!(span(z1)<H*0.9))E('Ctrl + wheel did not zoom in: '+z1);
  if(!(Y(u)[1]>Y(u)[0]))E('an empty value axis after a zoom');
  follows('Ctrl + wheel');
  await wheel(.5,.5,{deltaY:120,shiftKey:true});var z2=X(u);
  if(same(z2,z1))E('Shift + wheel did not move along n');
  if(!near(span(z2),span(z1)))E('Shift + wheel changed the width of the window');
  follows('Shift + wheel');
  await wheel(.5,.5,{deltaX:-120});
  if(same(X(u),z2))E('a sideways scroll did not move along n');
  var r0=o.getBoundingClientRect(),xb=X(u),yb=Y(u);
  wrap.dispatchEvent(new WheelEvent('wheel',{clientX:r0.left-20,clientY:r0.top+r0.height/2,deltaY:-240,ctrlKey:true,bubbles:true,cancelable:true,view:window}));await tick();
  if(!same(X(u),xb))E('a wheel over the value axis moved the n window');
  if(!(Y(u)[1]-Y(u)[0]<yb[1]-yb[0]))E('a wheel over the value axis did not zoom it');

  /* the keys, and the wheel on the chart that has the focus */
  o.focus();await tick();
  await key('0');isHome("the '0' key");follows("the '0' key");
  e=await key('+');if(!e.defaultPrevented||!(span(X(u))<H))E("the '+' key did not zoom");
  var kb=X(u);await key('ArrowRight');if(same(X(u),kb))E('ArrowRight did not move');follows('ArrowRight');
  for(var i=0;i<4;i++)await key('-');
  isHome('zooming all the way out with the keys');
  if((await key('a')).defaultPrevented)E('a key the chart does not use was swallowed');
  if(document.activeElement===o){
   e=await wheel(.5,.5,{deltaY:120});
   if(e.defaultPrevented)E('an active chart at its home view kept a wheel it had no use for');
   e=await wheel(.5,.5,{deltaY:-120});
   if(!e.defaultPrevented||!(span(X(u))<H))E('a plain wheel did not zoom the chart that has the focus');
  }else E('the chart could not take the focus');
  o.blur();await tick();
  await key('0');

  /* the panel's buttons */
  var B=c0.card.querySelectorAll('button.t');
  if(B.length<4){E('the panel has '+B.length+' buttons, not 4');return;}
  B[1].click();await tick();if(!(span(X(u))<H))E('the + button did not zoom');follows('the + button');
  B[2].click();await tick();isHome('the reset button');
  B[1].click();await tick();B[0].click();await tick();B[0].click();await tick();isHome('the - button');
  var w0=u.width;B[3].click();await tick(80);
  if(!/wide/.test(c0.card.className))E('expand did not mark the panel');
  if(window.innerWidth>900&&!(u.width>w0))E('expand did not resize the chart: '+w0+' -> '+u.width);

  /* a drag selection, AFTER the layout moved: it must land where the mouse was */
  o.scrollIntoView({block:'center'});await tick(60);
  var wa=u.posToVal(o.clientWidth*0.25,'x'),wb=u.posToVal(o.clientWidth*0.55,'x');
  await drag(.25,.55);
  var zs=X(u);
  if(!(span(zs)<H*0.5))E('a sideways drag did not zoom: '+zs);
  else if(Math.abs(T(zs[0])-T(wa))>0.02*H||Math.abs(T(zs[1])-T(wb))>0.02*H)E('a sideways drag from '+wa+' to '+wb+' gave '+zs+': a stale plot rectangle?');
  if(u.select.width>0||v.select.width>0)E('a selection box was left on a chart');
  follows('a drag selection');
  o.dispatchEvent(new MouseEvent('dblclick',P(.5,.5,{button:0})));await tick();
  isHome('a double-click');follows('a double-click');

  /* panning: the switch, then Shift + drag */
  var pb=document.querySelector('button[data-dm="pan"]'),zb=document.querySelector('button[data-dm="zoom"]');
  if(!pb||!zb)E('no drag-mode switch');
  else{
   B[1].click();await tick();var p0=X(u);
   pb.click();await tick();
   if(!/pan/.test(document.getElementById('charts').className))E('drag pans: the charts did not take the hand cursor');
   await drag(.6,.4,null,true);var p1=X(u);
   if(!(T(p1[0])>T(p0[0])))E('dragging left with drag-pans on did not move the window right: '+p0+' -> '+p1);
   if(!near(span(p1),span(p0)))E('a pan changed the width of the window');
   follows('a pan');
   zb.click();await tick();
   await drag(.4,.6,{shiftKey:true},true);
   if(!(T(X(u)[0])<T(p1[0])))E('Shift + drag did not pan');
  }

  /* the box by the pointer */
  o.dispatchEvent(new MouseEvent('mouseenter',P(.5,.5,{bubbles:false})));
  o.dispatchEvent(new MouseEvent('mousemove',P(.5,.5,{movementX:1})));await tick(40);
  var tipEl=o.querySelector('.tip');
  if(!tipEl||getComputedStyle(tipEl).display==='none'||!/n = /.test(tipEl.textContent))E('the box by the pointer did not show');
  o.dispatchEvent(new MouseEvent('mouseleave',P(1.3,.5,{bubbles:false})));await tick();
  if(tipEl&&getComputedStyle(tipEl).display!=='none')E('the box by the pointer stayed up after the pointer left');

  /* a series hidden while zoomed: both charts of the panel drop it and the value axis is still an axis */
  var lab=u.root.querySelectorAll('.u-series th');
  if(lab.length>1){
   lab[1].click();await tick(40);
   if(!isFinite(Y(u)[0])||!(Y(u)[1]>Y(u)[0]))E('hiding a series while zoomed broke the value axis: '+Y(u));
   if(v.series[1].show!==false)E('the panel twin did not hide the same series');
   lab[1].click();await tick(40);
  }

  /* a zoom pushed until it stops, between two sizes */
  for(i=0;i<40;i++){o.dispatchEvent(new WheelEvent('wheel',P(.37,.5,{deltaY:-300,ctrlKey:true})));await tick(2);}
  await tick(40);
  var dz=X(u),dy=Y(u);
  if(!(span(dz)>0)||!isFinite(dy[0])||!isFinite(dy[1])||!(dy[1]>dy[0]))E('a deep zoom left an empty axis: n '+dz+', values '+dy);
  follows('a deep zoom');
  await key('0');isHome('the reset after a deep zoom');

  /* touch */
  if(window.Touch&&window.TouchEvent){
   var mk=function(id,fx){var p=P(fx,.5);return new Touch({identifier:id,target:o,clientX:p.clientX,clientY:p.clientY});};
   var fire=async function(type,ts){o.dispatchEvent(new TouchEvent(type,{touches:ts,targetTouches:ts,changedTouches:ts,bubbles:true,cancelable:true}));await tick(8);};
   await fire('touchstart',[mk(1,.45),mk(2,.55)]);await fire('touchmove',[mk(1,.30),mk(2,.70)]);await fire('touchend',[]);
   if(!(span(X(u))<H*0.5))E('a pinch did not zoom: '+X(u));
   follows('a pinch');
   var t0=X(u);await fire('touchstart',[mk(3,.6)]);await fire('touchmove',[mk(3,.4)]);await fire('touchend',[]);
   if(!(T(X(u)[0])>T(t0[0])))E('a one-finger sideways drag did not move the zoomed window');
   await key('0');isHome('the reset after the touches');
  }

  /* the page's own controls */
  var jump=document.querySelector('select.f');
  if(!jump||jump.options.length<2)E('no jump-to-a-shape list');
  if(!document.getElementById('shape-0'))E('the shapes have no anchors to jump to');
  if(getComputedStyle(document.querySelector('.bar')).position!=='sticky')E('the control bar does not stay in view');
 }catch(err){E('the driver threw: '+(err&&err.stack||err));}
 finally{window.__ixdone=true;}
})();</script>"""


def browser():
    for p in CANDIDATES:
        if os.path.exists(p):
            return p
    return None


def variant(html, mode, tmpdir, drive=True):
    # a mode is "<axes>" or "<axes>+<size preset>", e.g. "log+t10": both are initial values in the report's script
    axes, _, preset = mode.partition("+")
    if axes == "log":
        html = html.replace("let MODE='lin';", "let MODE='log';", 1)
    if preset:
        html = html.replace("let RANGE='all';", "let RANGE='%s';" % preset, 1)
    tail = (DRIVER if drive else "<script>window.__ixdone=true;</script>") + DUMP
    html = html.replace("<body>", "<body>" + TRAP, 1).replace("</body>", tail + "</body>", 1)
    path = os.path.join(tmpdir, "check-%s%s.html" % (mode.replace("+", "-"), "" if drive else "-still"))
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(html)
    return path


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    if not args:
        sys.exit("usage: browser_check.py <report.html> [--shot out.png]")
    report = args[0]
    shot = None
    if "--shot" in sys.argv:
        shot = sys.argv[sys.argv.index("--shot") + 1]

    exe = browser()
    if not exe:
        print("browser_check: no Chromium-family browser found, skipping")
        return 0
    html = open(report, encoding="utf-8").read()
    modes = ["lin", "log"] if "let MODE=" in html else ["lin"]
    if "let RANGE='all';" in html:                  # the size presets: the small end has n = 0, which a log axis cannot hold
        modes += ["lin+t10", "log+t10", "lin+k10", "log+k10"]
    driven = "__SCALE" in html

    bad = 0
    with tempfile.TemporaryDirectory() as tmp:
        for mode in modes:
            page = variant(html, mode, tmp)
            # a window wide enough for the two-column layout: 'expand' has nothing to widen in one column
            cmd = [exe, "--headless=new", "--disable-gpu", "--hide-scrollbars", "--window-size=1400,1000",
                   "--virtual-time-budget=15000", "--dump-dom", "file:///" + page.replace("\\", "/")]
            # errors="replace": the dumped DOM is UTF-8 and Windows would otherwise decode the
            # pipe as cp1252 and die on the first non-Latin-1 character in the report.
            out = subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8",
                                 errors="replace", timeout=300).stdout or ""
            m = re.search(r"ERRORS\[(\d+)\]:([^<]*)", out)
            if not m:
                print("  %-8s FAILED: the page never finished running" % mode)
                bad += 1
                continue
            n, detail = int(m.group(1)), m.group(2).strip()
            ok = "clean, charts driven" if driven else "clean"
            print("  %-8s %s" % (mode, ok if not n else "%d error(s): %s" % (n, detail[:1500])))
            bad += n
            if shot and mode == modes[-1]:
                still = variant(html, mode, tmp, drive=False)       # the page as it opens, not as the driver left it
                subprocess.run([exe, "--headless=new", "--disable-gpu", "--hide-scrollbars",
                                "--virtual-time-budget=5000", "--window-size=1500,1000",
                                "--screenshot=" + os.path.abspath(shot),
                                "file:///" + still.replace("\\", "/")], capture_output=True, timeout=180)
                print("  wrote %s" % shot)
    if bad:
        print("browser_check: FAILED")
        return 2
    print("browser_check: OK (%s)" % ", ".join(modes))
    return 0


if __name__ == "__main__":
    sys.exit(main())

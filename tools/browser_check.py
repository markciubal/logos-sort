#!/usr/bin/env python3
"""Render a generated report in a real headless browser and fail on any console error.

`check_report.js` runs the report's script under a stub DOM, which is fast and catches most things,
but a stub only produces the inputs you thought to produce. It passed a report whose log-log charts
came up completely blank, because the real uPlot hands a log axis a splits array with NULL in every
slot it does not want labelled, and the stub only ever passed numbers. This tool has no such blind
spot: it is the actual browser, with an error trap installed before the report's own script runs.

Usage:
    python tools/browser_check.py reports/scale-report.html
    python tools/browser_check.py reports/scale-report.html --shot out.png

Exit code 0 means every mode rendered with zero uncaught errors. Skips cleanly (exit 0) when no
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
DUMP = ("<script>setTimeout(function(){var d=document.createElement('div');d.id='ERRDUMP';"
        "d.textContent='ERRORS['+window.__errs.length+']: '+window.__errs.join(' ||| ');"
        "document.body.appendChild(d);},700);</script>")


def browser():
    for p in CANDIDATES:
        if os.path.exists(p):
            return p
    return None


def variant(html, mode, tmpdir):
    if mode == "log":
        html = html.replace("let MODE='lin';", "let MODE='log';", 1)
    html = html.replace("<body>", "<body>" + TRAP, 1).replace("</body>", DUMP + "</body>", 1)
    path = os.path.join(tmpdir, "check-%s.html" % mode)
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

    bad = 0
    with tempfile.TemporaryDirectory() as tmp:
        for mode in modes:
            page = variant(html, mode, tmp)
            cmd = [exe, "--headless=new", "--disable-gpu", "--hide-scrollbars",
                   "--virtual-time-budget=6000", "--dump-dom", "file:///" + page.replace("\\", "/")]
            # errors="replace": the dumped DOM is UTF-8 and Windows would otherwise decode the
            # pipe as cp1252 and die on the first non-Latin-1 character in the report.
            out = subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8",
                                 errors="replace", timeout=180).stdout or ""
            m = re.search(r"ERRORS\[(\d+)\]:([^<]*)", out)
            if not m:
                print("  %-4s FAILED: the page never finished running" % mode)
                bad += 1
                continue
            n, detail = int(m.group(1)), m.group(2).strip()
            print("  %-4s %s" % (mode, "clean" if not n else "%d uncaught error(s): %s" % (n, detail[:300])))
            bad += n
            if shot and mode == modes[-1]:
                subprocess.run([exe, "--headless=new", "--disable-gpu", "--hide-scrollbars",
                                "--virtual-time-budget=5000", "--window-size=1500,1000",
                                "--screenshot=" + os.path.abspath(shot),
                                "file:///" + page.replace("\\", "/")], capture_output=True, timeout=180)
                print("  wrote %s" % shot)
    if bad:
        print("browser_check: FAILED")
        return 2
    print("browser_check: OK (%s)" % ", ".join(modes))
    return 0


if __name__ == "__main__":
    sys.exit(main())

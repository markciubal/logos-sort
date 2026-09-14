# Vendored third-party assets

## uPlot 1.6.31 (MIT, https://github.com/leeoniya/uPlot)

`uplot.min.js` (50 KB) and `uplot.min.css` (1.9 KB), the IIFE build.

**Why it is here and not on a CDN.** Every generated report in this project is a
single self-contained HTML file that gets opened from disk, copied into
`reports/`, and mailed around. None of them may depend on a network fetch. So
uPlot is baked into the binary that writes the report: `tools/embed_uplot.py`
turns these two files into `../uplot_embed.h`, and `scalebench.cpp` splices them
into the page.

**To bump the version.** Replace both files, then:

```sh
python tools/embed_uplot.py && make scalebench
```

The generated header is checked in so the build needs no Python.

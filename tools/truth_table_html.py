#!/usr/bin/env python3
"""truth_table_html.py — render TRUTH_TABLE.md as reports/truth-table.html.

Usage:  python tools/truth_table_html.py        (from anywhere; paths are relative to this file)

The markdown is the source of truth. This turns it into the styled page: status
words in the tables become pills, H/J avenues become articles with the
prediction highlighted, `## X. Title` sections get letter anchors that the
navigation in tools/truth_table_head.html links to. No dependencies.
"""
import re, html, os

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
SRC = os.path.join(ROOT, 'TRUTH_TABLE.md')
HEAD = os.path.join(HERE, 'truth_table_head.html')
DST = os.path.join(ROOT, 'reports', 'truth-table.html')

src = open(SRC, encoding='utf-8').read()

def inline(s):
    s = html.escape(s, quote=False)
    s = re.sub(r'`([^`]+)`', r'<code>\1</code>', s)
    s = re.sub(r'\*\*([^*]+)\*\*', r'<strong>\1</strong>', s)
    s = re.sub(r'(?<![\w*])\*([^*]+)\*(?![\w*])', r'<em>\1</em>', s)
    return s

def status_pill(txt):
    t = txt.strip(); low = t.lower()
    if low.startswith('false'): k = 'false'
    elif low.startswith('measured') and 'loaded' in low: k = 'loaded'
    elif low.startswith('measured'): k = 'measured'
    elif low.startswith('inferred'): k = 'inferred'
    elif low.startswith('untested'): k = 'untested'
    elif low.startswith('partial'): k = 'partial'
    elif low.startswith('known'): k = 'known'
    elif low.startswith('rejected') or low.startswith('withdrawn'): k = 'false'
    else: return None
    return f'<span class="pill pill-{k}">{inline(t)}</span>'

out = []; i = 0; lines = src.split('\n'); open_sec = False
while i < len(lines):
    L = lines[i]
    if L.startswith('# '):
        i += 1; continue
    if L.startswith('## '):
        if open_sec: out.append('</section>')
        open_sec = True
        m = re.match(r'## ([A-Z])\. (.*)', L)
        if m: out.append(f'<section id="sec-{m.group(1)}"><h2><span class="sec-letter">{m.group(1)}</span>{inline(m.group(2))}</h2>')
        else: out.append(f'<section><h2>{inline(L[3:])}</h2>')
        i += 1; continue
    if L.startswith('### '):
        m = re.match(r'### ([HJ]\d+[a-z]?)\. (.*)', L)
        if m: out.append(f'<article class="avenue"><h3><span class="hid">{m.group(1)}</span>{inline(m.group(2))}</h3>')
        elif L.startswith('### Tier'): out.append(f'<h3 class="tier">{inline(L[4:])}</h3>')
        else: out.append(f'<article class="avenue closed"><h3>{inline(L[4:])}</h3>')
        i += 1; continue
    if L.startswith('---'):
        i += 1; continue
    if L.startswith('|'):
        rows = []
        while i < len(lines) and lines[i].startswith('|'):
            rows.append(lines[i]); i += 1
        hdr = [c.strip() for c in rows[0].strip('|').split('|')]
        body_rows = rows[2:]
        ledger = hdr == ['#', 'idea', 'result', 'why']
        status_col = hdr.index('status') if 'status' in hdr else (hdr.index('result') if ledger else -1)
        t = ['<div class="tw"><table><thead><tr>' + ''.join(f'<th>{inline(h)}</th>' for h in hdr) + '</tr></thead><tbody>']
        for r in body_rows:
            cells = [c.strip() for c in r.strip('|').split('|')]
            tds = []
            for j, c in enumerate(cells):
                cls = ''
                if j == 0 and hdr[0] == '#': cls = ' class="id"'
                if j < len(hdr) and hdr[j] in ('evidence', 'measured effect', 'result') and not ledger: cls = ' class="num"'
                if j == status_col:
                    p = status_pill(c)
                    if p: tds.append(f'<td class="st">{p}</td>'); continue
                    if ledger: tds.append(f'<td class="num">{inline(c)}</td>'); continue
                if hdr == ['status', 'meaning'] and j == 0:
                    p = status_pill(c.strip('*'))
                    tds.append(f'<td class="st">{p or inline(c)}</td>'); continue
                tds.append(f'<td{cls}>{inline(c)}</td>')
            t.append('<tr>' + ''.join(tds) + '</tr>')
        t.append('</tbody></table></div>')
        out.append(''.join(t)); continue
    if re.match(r'^\d+\. ', L):
        items = []
        while i < len(lines) and re.match(r'^\d+\. ', lines[i]):
            item = lines[i][lines[i].index(' ') + 1:]; i += 1
            while i < len(lines) and lines[i].startswith('   '):
                item += ' ' + lines[i].strip(); i += 1
            items.append(f'<li>{inline(item)}</li>')
        out.append('<ol class="order">' + ''.join(items) + '</ol>'); continue
    if L.strip() == '':
        i += 1; continue
    para = []
    while i < len(lines) and lines[i].strip() != '' and not lines[i].startswith(('|', '#', '---')) and not re.match(r'^\d+\. ', lines[i]):
        para.append(lines[i].strip()); i += 1
    text = ' '.join(para)
    parts = [p for p in re.split(r'(?=\*\*[A-Za-z /()]+:\*\*)', text) if p.strip()]
    if len(parts) > 1 or (parts and parts[0].startswith('**') and ':**' in parts[0][:40]):
        out.append('<dl class="fields">')
        for p in parts:
            m = re.match(r'\*\*([^*]+):\*\*\s*(.*)', p, re.S)
            if m:
                lab = m.group(1); cls = ' class="pred"' if lab.lower().startswith('prediction') else ''
                out.append(f'<div{cls}><dt>{inline(lab)}</dt><dd>{inline(m.group(2))}</dd></div>')
            else:
                out.append(f'<div><dt></dt><dd>{inline(p)}</dd></div>')
        out.append('</dl>')
    else:
        out.append(f'<p>{inline(text)}</p>')
if open_sec: out.append('</section>')

# close articles before the next article, a tier heading, or the section end
res = []; open_art = False
for chunk in '\n'.join(out).split('\n'):
    if chunk.startswith('<article'):
        if open_art: res.append('</article>')
        open_art = True
    elif chunk.startswith('<h3 class="tier"') or chunk.startswith('</section>'):
        if open_art: res.append('</article>'); open_art = False
    res.append(chunk)
body = '\n'.join(res)

page = open(HEAD, encoding='utf-8').read() + body + '</div>\n'
os.makedirs(os.path.dirname(DST), exist_ok=True)
open(DST, 'w', encoding='utf-8').write(page)
print(f'wrote {os.path.relpath(DST, ROOT)}: {len(page)} bytes, {page.count("<section")} sections, {page.count("<article")} avenues')

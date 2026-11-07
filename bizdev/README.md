# bizdev/ — the business-development assessor's folder

This folder translates the engine's measured state into commercial terms and feeds commercial priorities back to
engineering. The assessor does not modify the algorithm; it may propose benchmark contenders, harness options and
scripts under `bizdev/` or `engines/` by pull request, only with the engineering agent's or the author's approval.

## Standing rules

1. A claim exists only if a row in the latest scale-report data supports it. Every claim names the input shape,
   n, thread count, the rival it beats, the machine, and peak heap per element for both sides. The worst row is
   shown beside the best. Differences under 5% are "level", never a win.
2. Memory is part of every number: speed and scratch together, never speed alone.
3. The author's own harness numbers outrank anything from the web; anything unverified is labelled so; market
   facts carry links.
4. The assessor never publishes, posts, files, emails or contacts anyone. It produces drafts; the author decides.
5. When something only the author knows is needed, the question goes to `QUESTIONS.md` with the assumption the
   assessor proceeds on.
6. Plain prose, tables for numbers, no marketing language; a bad result is said in the first sentence.

## Files

| file | what it is | regenerate when |
|---|---|---|
| `00-STATE.md` | API surface, build and portability, repository facts, test posture, the measured picture | a new scale report; an API change |
| `CLAIMS.md` | the claims sheet, graded green / amber / red, with a do-not-claim list | every scale report run |
| `BUYERS.md` | the buyer map by segment with a verdict per target | a buyer fact changes; a GAPS item lands |
| `GAPS.md` | the engineering backlog ranked by commercial value, tagged with buyers and claims | weekly, and when a run changes a claim |
| `IP.md` | license options, the patent clock, authorship, prior art, questions for counsel | any public step; any license decision |
| `PAPER.md` | venue plan, missing experiments, outline | a second machine; a new rival |
| `PITCH-<target>.md` | one per target, only after three green rows relevant to it | the claims it rests on change |
| `QUESTIONS.md` | open questions for the author with the assumptions in force | the author answers |
| `LOG.md` | dated entries: what changed in the engine, what it did to the claims, what was re-ranked | every session |
| `tools/parse_report.py` | parses the report's DATA array into logos-vs-best-rival rows with memory | |

## After every benchmark run

```sh
python -I bizdev/tools/parse_report.py reports/scale-report.html rows.md rows.json
```

Then regrade `CLAIMS.md` from `rows.md`, post a dated note to `LOG.md` in commercial terms, re-rank `GAPS.md`, and
flag any regression the same day. When engineering proposes an optimisation, answer in writing: which buyer
notices, and which claim changes grade.

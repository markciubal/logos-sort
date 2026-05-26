module.exports = (() => {
// ============================================================
// LogosAdaptive findOutliers — anomaly detection via sort resistance
// ------------------------------------------------------------
// findOutliers(arr, options) -> { sorted, outliers, budget }
//
// Single forward pass of insertion sort with a per-element movement budget.
// Two detection signals are combined to catch both types of outliers:
//
//   1. STUCK FLAG (low outliers): When an element exhausts its movement
//      budget mid-insertion, it stops short of its true sorted position.
//      Low outliers (values much smaller than their input neighborhood)
//      try to migrate far leftward and hit this limit. Flagged directly
//      when budget is exhausted.
//
//   2. MIGRATION DISTANCE (high outliers): High outliers don't move during
//      their own insertion (they're already larger than the prior prefix),
//      but every subsequent insertion shifts them rightward by one. After
//      the pass, their original→final position distance is large. Flagged
//      when |finalPos - originalPos| > budget.
//
// The two-signal design handles the directional asymmetry of insertion
// sort: an earlier two-pass attempt (forward + descending) over-flagged
// because every element in an ascending array "wants to move" in
// descending order — the wrong signal entirely.
//
// Best for: mostly-sorted data with isolated anomalies (logs, sensors,
// sorted CSVs with corrupted rows). Useless on random data; if you flag
// more than ~10% of elements, the data didn't have detectable structure.
//
// Mutates arr in place (becomes roughly-sorted). Returns outlier indices
// referring to the input's original ordering.
//
// Auxiliary memory: O(n) Int32Array for index tracking + O(n) Uint8Array
// for outlier flags. Complexity: O(n × budget). Default budget: max(3, sqrt(n)).
// Works for any type comparable with `<` — numbers, strings, dates.
// ============================================================

function findOutliers(arr, options) {
  options = options || {};
  const n = arr.length;
  if (n < 2) return { sorted: arr, outliers: [], budget: 0 };

  const budget = options.budget != null ? options.budget : Math.max(3, Math.floor(Math.sqrt(n)));
  const flagged = new Uint8Array(n);

  // origIdx[i] = original index of the value currently at arr[i].
  const origIdx = new Int32Array(n);
  for (let i = 0; i < n; i++) origIdx[i] = i;

  // Forward insertion sort with per-element movement budget.
  // Signal 1: elements that hit budget are low outliers (stuck mid-migration).
  for (let i = 1; i < n; i++) {
    const kV = arr[i], kI = origIdx[i];
    let j = i - 1, moved = 0, stuck = false;
    while (j >= 0 && arr[j] > kV) {
      if (moved >= budget) { stuck = true; break; }
      arr[j + 1] = arr[j];
      origIdx[j + 1] = origIdx[j];
      j--; moved++;
    }
    arr[j + 1] = kV;
    origIdx[j + 1] = kI;
    if (stuck) flagged[kI] = 1;
  }

  // Signal 2: large migration distance indicates a high outlier (or, less
  // commonly, an element pushed around by many high outliers passing through —
  // false positives here scale with outlier count, not with input size).
  for (let i = 0; i < n; i++) {
    const dist = i - origIdx[i];
    if (dist > budget || dist < -budget) flagged[origIdx[i]] = 1;
  }

  const outliers = [];
  for (let i = 0; i < n; i++) if (flagged[i]) outliers.push(i);

  return { sorted: arr, outliers, budget };
}

return { findOutliers };
})()

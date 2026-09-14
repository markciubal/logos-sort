// ============================================================
// LogosAdaptive Sort — combined entry point
// ------------------------------------------------------------
// Dispatches by element type. The two impls MUST live in
// separate modules: bundling them in one file kills V8's
// optimization budget per script, costing 5-6× on string sorts.
// Keep these three files separate.
//
// API: sort(arr) → arr
//   - numbers (int32 / int53 / float64) → ./sort
//   - strings                            → ./sort_strings
//   - empty / single-element             → returned as-is
//   - mixed-type arrays are NOT supported (Array.prototype.sort
//     stringifies; this library does not — it would be slow and
//     surprising. Pre-stringify if you need that behavior.)
// ============================================================

const { sort: sortNumbers }   = require('./sort');
const { sortStrings }         = require('./sort_strings');

const sort = (arr) => {
  if (arr.length <= 1) return arr;
  return typeof arr[0] === 'string' ? sortStrings(arr) : sortNumbers(arr);
};

module.exports = { sort };

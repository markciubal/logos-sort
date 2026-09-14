// logos-sort — main entry
//
// Licensed under the LogosAdaptive Sort Ethical Source License v1.1.
// Dual-licensed: free for personal, research, educational, non-profit, and
// open-source use; commercial use requires a paid license. See LICENSE for
// full terms and prohibited uses. NOT an OSI-approved open source license.
//
// API surface:
//
//   Pure JS (always available):
//     sort, sortInplace          — adaptive sort for numbers/strings
//     argSort, sortWithIndices,
//     applyPermutation, inversePermutation, restoreOrder
//     findOutliers               — anomaly detection
//
//   Native (when the compiled addon is present):
//     sortNativeF64              — zero-copy sort on Float64Array
//     sortNativeNumbers          — copy-in/copy-out sort on Array<number>
//     nativeAvailable            — boolean
//     nativeVersion              — C library version string or null

const sort         = require('./lib/sort.js');
const sortInplace  = require('./lib/sort-inplace.js');
const argsortMod   = require('./lib/argsort.js');
const outliersMod  = require('./lib/find-outliers.js');
const proxyMod     = require('./lib/sort-with-proxy.js');
const native       = require('./lib/native.js');

module.exports = {
  // Primary sorts (pure JS)
  sort,
  sortInplace,

  // Typed-array fast paths (mandatory representation at very large n)
  sortF64:            sort.sortF64,
  sortI32:            sort.sortI32,

  // Companion utilities
  argSort:            argsortMod.argSort,
  sortWithIndices:    argsortMod.sortWithIndices,
  applyPermutation:   argsortMod.applyPermutation,
  inversePermutation: argsortMod.inversePermutation,
  restoreOrder:       argsortMod.restoreOrder,

  // Anomaly detection
  findOutliers:       outliersMod.findOutliers,

  // Expensive-comparator sort (LLM-as-judge, human ranking, costly assays)
  sortWithProxy:      proxyMod.sortWithProxy,

  // Native addon (may be null if the C build failed at install time)
  sortNativeF64:      native.sortFloat64Array,
  sortNativeNumbers:  native.sortNumberArray,
  nativeAvailable:    native.available,
  nativeVersion:      native.version,
  nativeLoadError:    native.loadError,
};

// Default export = sort
module.exports.default = sort;

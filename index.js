// logos-sort — main entry
//
// Licensed under the LogosAdaptive Sort Ethical Source License v1.1.
// Dual-licensed: free for personal, research, educational, non-profit, and
// open-source use; commercial use requires a paid license. See LICENSE for
// full terms and prohibited uses. NOT an OSI-approved open source license.
//
// Three sort modules + two analysis utilities:
//   sort         buffered sort (auxiliary memory pool, fastest on most workloads)
//   sortInplace  in-place sort (~3KB scratch, no per-call allocation)
//   argSort      sort indices instead of values
//   findOutliers detect anomalous values via sort resistance
//
// All sort functions accept arrays of numbers OR strings (auto-detected from
// the first element). Numeric arrays may mix integers and floats. String
// arrays must contain only strings.

const sort         = require('./lib/sort.js');
const sortInplace  = require('./lib/sort-inplace.js');
const argsortMod   = require('./lib/argsort.js');
const outliersMod  = require('./lib/find-outliers.js');

module.exports = {
  // Primary sorts
  sort,
  sortInplace,

  // Companion utilities
  argSort:            argsortMod.argSort,
  sortWithIndices:    argsortMod.sortWithIndices,
  applyPermutation:   argsortMod.applyPermutation,
  inversePermutation: argsortMod.inversePermutation,
  restoreOrder:       argsortMod.restoreOrder,

  // Anomaly detection
  findOutliers:       outliersMod.findOutliers,
};

// Default export = sort, for `const sort = require('logos-sort')`-style usage
module.exports.default = sort;

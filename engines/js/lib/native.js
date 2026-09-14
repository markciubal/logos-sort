// lib/native.js
//
// Loads the compiled native addon (build/Release/logos_sort_native.node).
// If loading fails — typically because the user couldn't run node-gyp at
// install time (missing C toolchain, unsupported platform, etc.) — this
// module exports `null` and `available = false`, and callers should fall
// back to the pure-JS implementation.
//
// The native addon exports:
//
//   sortFloat64Array(arr): mutates a Float64Array in place using the C
//                          flash-sort + introsort path. Zero-copy.
//                          Returns the same array.
//
//   sortNumberArray(arr):  sorts a regular Array of numbers by copying
//                          into a Float64Array, sorting natively, copying
//                          back. ~O(n) copy overhead; usually still faster
//                          than pure JS at large n, but use the F64 path
//                          if you can hold your data in a Float64Array.
//
//   version:               C library version string.

let addon = null;
let loadError = null;

try {
  // Standard node-gyp output path
  addon = require('../build/Release/logos_sort_native.node');
} catch (e) {
  loadError = e;
  try {
    // Debug build location, just in case
    addon = require('../build/Debug/logos_sort_native.node');
    loadError = null;
  } catch (e2) {
    // Both failed — native addon is unavailable on this install
    addon = null;
  }
}

module.exports = {
  available: addon !== null,
  loadError,
  sortFloat64Array: addon ? addon.sortFloat64Array : null,
  sortNumberArray:  addon ? addon.sortNumberArray  : null,
  version:          addon ? addon.version()        : null,
};

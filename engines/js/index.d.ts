// Type definitions for logos-sort

/** Sort an array of numbers or strings in ascending order. Returns the same
 *  array reference, sorted in place using auxiliary memory for speed. */
export function sort<T extends number | string>(arr: T[]): T[];

export namespace sort {
  /** Pre-allocate the internal buffer pool for arrays up to this size. */
  function preallocate(n: number): void;
  /** Release internal buffers, freeing memory. They will be reallocated as needed. */
  function releaseBuffers(): void;
  /** Sort a Float64Array in place via the numeric engine. Typed reads avoid the
   *  boxed-Array tax; the only viable representation at very large n. */
  function sortF64(arr: Float64Array): Float64Array;
  /** Sort an Int32Array in place. Assumes int32 values, so it skips the
   *  integer-detection pass and goes straight to counting/radix (~1.4x the
   *  boxed-Array radix path). */
  function sortI32(arr: Int32Array): Int32Array;
}

/** Sort a Float64Array in place (typed fast path; see `sort.sortF64`). */
export function sortF64(arr: Float64Array): Float64Array;
/** Sort an Int32Array in place (lean integer fast path; see `sort.sortI32`). */
export function sortI32(arr: Int32Array): Int32Array;

/** Sort in place with minimal additional memory (~3KB scratch buffers).
 *  Slightly slower than `sort` on most workloads but suitable for
 *  memory-constrained or streaming contexts. */
export function sortInplace<T extends number | string>(arr: T[]): T[];

/** Return a permutation `p` such that `p[i]` is the original index of the
 *  element that ends up at sorted position `i`. The input is not modified. */
export function argSort<T extends number | string>(arr: T[]): Uint32Array;

/** Sort and also return the permutation used (combination of sort + argSort). */
export function sortWithIndices<T extends number | string>(arr: T[]): {
  sorted: T[];
  indices: Uint32Array;
};

/** Apply a permutation to an array, returning a new array. */
export function applyPermutation<T>(arr: T[], perm: Uint32Array | number[]): T[];

/** Compute the inverse of a permutation. */
export function inversePermutation(perm: Uint32Array | number[]): Uint32Array;

/** Undo a sort: given a sorted array and the permutation, restore the original order. */
export function restoreOrder<T>(sorted: T[], perm: Uint32Array | number[]): T[];

export interface OutlierResult<T> {
  /** The sorted array (input is not modified). */
  sorted: T[];
  /** Indices in the *original* (unsorted) array that were identified as outliers. */
  outliers: number[];
  /** The displacement budget used (default: max(3, sqrt(n))). */
  budget: number;
}

/** Detect anomalous values via their resistance to sorting. Elements that
 *  must travel further than the budget to reach their sorted position are
 *  flagged as outliers. Useful for fraud detection, sensor glitches, etc. */
export function findOutliers<T extends number | string>(
  arr: T[],
  options?: { budget?: number }
): OutlierResult<T>;

export interface ProxySortResult<T> {
  /** The items sorted by the expensive comparator. */
  sorted: T[];
  /** How many expensive comparator calls were made. */
  oracleCalls: number;
  /** Which refinement strategy was chosen ('insertion' | 'adaptive' | 'trivial'). */
  strategy: string;
}

/** Two-tier comparator sort for EXPENSIVE comparisons (LLM-as-judge, human
 *  ranking, costly assays). Sorts first by a cheap approximate `proxyKey`,
 *  then refines with the expensive `oracleCmp`, minimizing expensive calls.
 *  Output is always exactly sorted by `oracleCmp`; the proxy only affects how
 *  many expensive calls happen.
 *
 *  @param items     items to sort
 *  @param proxyKey  cheap approximate sort key per item
 *  @param oracleCmp expensive exact comparator, (a,b) => -1 | 0 | 1
 */
export function sortWithProxy<T>(
  items: T[],
  proxyKey: (item: T) => number | string,
  oracleCmp: (a: T, b: T) => number
): ProxySortResult<T>;

// ---------- Native addon (optional) ----------

/** Zero-copy native sort of a Float64Array. Mutates the underlying buffer
 *  in place and returns the same array. Throws TypeError if the argument
 *  is not a Float64Array. May be null if the native addon was not built.
 *
 *  This is the only native path that reliably wins against pure-JS sort,
 *  and only at medium sizes (n ~10k); at large n the paths converge.
 */
export const sortNativeF64: ((arr: Float64Array) => Float64Array) | null;

/** Native sort of a regular Array<number>. Copies elements across the JS/C
 *  boundary, which is typically ~5–10× slower than the pure-JS sort —
 *  do not use unless you have measured a real win on your data. */
export const sortNativeNumbers: ((arr: number[]) => number[]) | null;

/** True if the native addon was built and loaded successfully. */
export const nativeAvailable: boolean;

/** C library version string, or null if the native addon is unavailable. */
export const nativeVersion: string | null;

/** The Error that occurred while attempting to load the native addon, or
 *  null if either the addon loaded successfully or no attempt was made. */
export const nativeLoadError: Error | null;

declare const _default: typeof sort;
export default _default;

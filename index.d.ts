// Type definitions for logos-sort

/** Sort an array of numbers or strings in ascending order. Returns the same
 *  array reference, sorted in place using auxiliary memory for speed. */
export function sort<T extends number | string>(arr: T[]): T[];

export namespace sort {
  /** Pre-allocate the internal buffer pool for arrays up to this size. */
  function preallocate(n: number): void;
  /** Release internal buffers, freeing memory. They will be reallocated as needed. */
  function releaseBuffers(): void;
}

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

declare const _default: typeof sort;
export default _default;

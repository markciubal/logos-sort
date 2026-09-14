/* logos-sort: adaptive sort for C.
 *
 * Licensed under the LogosAdaptive Sort Ethical Source License v1.1.
 * Dual-licensed: free for personal, research (incl. industrial R&D),
 * educational, non-profit, evaluation, and open-source dependency use.
 * Commercial use requires a paid license. See LICENSE for full terms and
 * prohibited uses (military, surveillance, non-consensual human genetic
 * research, human-rights violations are absolute prohibitions).
 * NOT an OSI-approved open source license.
 *
 * Reference implementation of the core LogosAdaptive dispatcher pattern
 * for sorting arrays of double-precision floats. The C library is a focused
 * subset of the JS library — it implements:
 *
 *   - flash sort (Neubert) for uniform/normal distributions
 *   - dual-pivot introsort fallback (quicksort + heapsort)
 *   - insertion sort for small ranges and bucket cleanup
 *   - cheap entry-probe dispatcher (asc/desc/equal counts, range check)
 *
 * String sorting, counting sort for bounded integers, integer-specific
 * radix passes, and many numeric specializations from the JS library are
 * NOT ported here. This is a reference of the design pattern, not a full
 * port.
 *
 * Behavior with NaN values is undefined (as with C's qsort).
 *
 * Thread safety: ls_sort_double() is reentrant and allocates its own scratch
 * memory. ls_sort_double_with_buffer() lets the caller provide scratch.
 */

#ifndef LOGOS_SORT_H
#define LOGOS_SORT_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Sort an array of doubles in ascending order. Allocates ~8*n bytes of
 * scratch internally (freed on return). Returns 0 on success, nonzero on
 * allocation failure (in which case the array is unchanged). */
int ls_sort_double(double *arr, size_t n);

/* Same as ls_sort_double but uses caller-provided scratch space. The buffer
 * must hold at least n doubles. Returns 0 on success, -1 if the buffer is
 * too small. */
int ls_sort_double_with_buffer(double *arr, size_t n,
                               double *scratch, size_t scratch_size);

/* Library version (semantic versioning string). */
const char *ls_sort_version(void);

#ifdef __cplusplus
}
#endif

#endif /* LOGOS_SORT_H */

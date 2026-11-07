/* logos_xss.cpp -- OPTIONAL: Intel x86-simd-sort's quicksort as the engine's unstructured kernel (2026-09-28).
 *
 * Open direction 2, the structure router: logos decides, and what it finds no structure in goes to the vectorized
 * quicksort WHOLE. Two entries, both behind -DLOGOS_XSS_LEAF and both called from avx2_qsort_f64_p, which has already
 * declined on a NaN and made every zero +0.0 (the library's networks are min/max, which return the same operand for
 * -0.0 against +0.0), so hasnan is false:
 *
 *   logos_xss_qsort_f64      one thread: the sequential leaf, in place of the in-house copy of the design (1.18-1.20x)
 *   logos_xss_qsort_par_f64  P threads through the library's OpenMP tasks: the hybrid's parallel half, for the LOW
 *                            thread counts where the engine's partition has not yet paid for its fixed costs
 *
 * It is NOT the "SIMD leaf under the parallel partition" of the ledger (measured worse twice): nothing is scattered
 * before either entry runs.
 *
 * The thread count is set before EVERY call, the one-thread entry included: built with XSS_USE_OPENMP the library opens
 * a parallel region sized by OpenMP's own default, which is every core on the machine.
 *
 * Build: needs the clone (make xssleafbench XSS=../x86-simd-sort/src) and -fopenmp. The engine without the flag does not
 * know this file exists. The library is C++, header-only, BSD 3-Clause (Intel); keep its notice with any build that
 * links it. */
#include <cstddef>
#include <omp.h>
#include "x86simdsort-static-incl.h"

extern "C" void logos_xss_qsort_f64(double *a, size_t n) {
    omp_set_num_threads(1);
    x86simdsortStatic::qsort(a, n, false, false);
}
extern "C" void logos_xss_qsort_par_f64(double *a, size_t n, int threads) {
    omp_set_num_threads(threads > 0 ? threads : 1);
    x86simdsortStatic::qsort(a, n, false, false);
}

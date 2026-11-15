/* kernwrap.c -- exposes the C engine's static AVX2 kernel to parity/kernbench.cpp. The engine is included whole, with its
 * public entry points renamed so that this object can sit beside the engine proper. */
#define logos_sort_f64 ckw_sort_f64
#define logos_sort_f64_batch ckw_sort_f64_batch
#define logos_sort_strings_mt ckw_sort_strings_mt
#define logos_argsort_f64_mt ckw_argsort_f64_mt
#define logos_sort_i32 ckw_sort_i32
#define logos_num_cpus ckw_num_cpus
#define logos_release_buffers ckw_release_buffers
#include "../../c/logos_psort.c"
int ck_kernel(double *a, size_t n) { return avx2_qsort_f64(a, n); }

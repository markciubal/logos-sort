/* avkernbench's second copy of the library, compiled with the engine's own target (-mavx2 -mfma, no -march=native),
 * so the kernel comparison can separate compiler tuning from algorithm. One thread, no OpenMP. */
#include <cstddef>
#include "x86simdsort-static-incl.h"
extern "C" void xss_base_qsort(double *a, size_t n) { x86simdsortStatic::qsort(a, n, false, false); }

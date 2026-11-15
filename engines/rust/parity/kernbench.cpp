/* kernbench.cpp -- the two AVX2 quicksort KERNELS alone, C against Rust, across range sizes (the Rust twin of make avkernbench).
 * One array of N uniform doubles is cut into chunks of a given size and every chunk is sorted by one kernel; the two kernels
 * alternate on fresh copies. At 64 and below a chunk is one base-case network; each doubling above adds one partition level,
 * so the table says whether a gap is in the networks or in the partition.
 * Build and run: bash parity/kernbench.sh [N] [pairs] */
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <ctime>
#include <vector>
#include <algorithm>
extern "C" {
int ck_kernel(double *, size_t);                 /* kernwrap.c: avx2_qsort_f64 of the C engine */
int logos_rs_kernel_f64(double *, size_t);       /* src/capi.rs */
}
static uint64_t RS = 7;
static uint64_t nr() { RS ^= RS << 13; RS ^= RS >> 7; RS ^= RS << 17; return RS; }
static double fr() { return (double)(nr() >> 11) / 9007199254740992.0; }
static double now_ms() { timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }
int main(int argc, char **argv) {
    size_t N = argc > 1 ? strtoull(argv[1], 0, 10) : 8388608;
    int pairs = argc > 2 ? atoi(argv[2]) : 8;
    std::vector<double> in(N), w(N);
    for (auto &v : in) v = fr() * 2e9 - 1e9;
    printf("kernels alone, uniform doubles, N = %zu in chunks; ns per element (minimum of %d), ratio = median t(C) / t(Rust)\n", N, pairs);
    printf("  %9s %8s %8s %7s\n", "chunk", "C", "Rust", "ratio");
    for (size_t c : { (size_t)8, (size_t)16, (size_t)24, (size_t)32, (size_t)48, (size_t)64, (size_t)128, (size_t)256, (size_t)511, (size_t)512, (size_t)1024, (size_t)4096, (size_t)65536, (size_t)1048576, N }) {
        if (c > N) continue;
        double bc = 1e300, br = 1e300; std::vector<double> ratio; bool ok = true;
        for (int r = 0; r < pairs; r++) {
            double tc = 0, tr = 0;
            for (int q = 0; q < 2; q++) {
                memcpy(w.data(), in.data(), N * 8);
                bool rust = (q ^ r) & 1;
                double t = now_ms();
                if (rust) for (size_t o = 0; o + c <= N; o += c) logos_rs_kernel_f64(w.data() + o, c);
                else      for (size_t o = 0; o + c <= N; o += c) ck_kernel(w.data() + o, c);
                t = now_ms() - t;
                if (rust) tr = t; else tc = t;
                if (r == 0) for (size_t o = 0; o + c <= N; o += c) for (size_t i = 1; i < c; i++) if (w[o + i] < w[o + i - 1]) ok = false;
            }
            bc = std::min(bc, tc); br = std::min(br, tr); ratio.push_back(tc / tr);
        }
        std::sort(ratio.begin(), ratio.end());
        size_t done = N / c * c;
        printf("  %9zu %8.2f %8.2f %7.3f%s\n", c, bc * 1e6 / (double)done, br * 1e6 / (double)done, ratio[pairs / 2], ok ? "" : "  WRONG");
        fflush(stdout);
    }
    return 0;
}

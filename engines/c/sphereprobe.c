/* sphereprobe: is there ANY per-element budget for a geometric transform before a sort?
 *
 * The proposal: project each element onto a sphere of radius n with x = value, y = a prime, z = the index,
 * hash the result with primes, look the hash up in a table of precomputed values, and re-project whenever a
 * new maximum is found while walking a linked list, in parallel.
 *
 * Whatever the ordering scheme that follows, EVERY variant pays its transform once per element. So the whole
 * family is decided by one number: what does the engine spend per element today, and what does the cheapest
 * honest form of each step cost? A step that exceeds the entire sort cannot be paid for by a better order.
 *
 * PREDICTION, written before the first run (ns per element, one thread unless said):
 *   engine, quantized, 14 threads      ~0.8      (6.6 ms for 8.4M, measured this session)
 *   engine, uniform,   14 threads      ~2.1
 *   engine, uniform,    1 thread      ~12
 *   sphere projection, scalar         2-4       one sqrt + 3 divides per element
 *   sphere projection, vectorised     0.6-1.5   4 doubles at a time
 *   prime MODULUS hash                5-15      integer division latency
 *   multiply-shift hash               0.3-0.8   what the counting route already uses
 *   precomputed-table lookup          20-80     a random read into 64+ MB is a DRAM miss
 * So: the projection alone is 1-5x a whole parallel sort, and the table lookup 10-100x. The verdict should be
 * that there is no budget, and that the cheapest step (multiply-shift) is the one the engine already does.
 *
 * A second, non-timing objection this probe also checks: a projection onto a sphere of FIXED radius is
 * many-to-one. It discards the radius, and the radius is where the magnitude lives, so two values that must
 * compare differently can land on the same point. The probe counts those collisions.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <time.h>

static uint64_t RS = 1;
static uint64_t nr(void) { RS ^= RS << 13; RS ^= RS >> 7; RS ^= RS << 17; return RS; }
static double now_ms(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6; }

/* keep the compiler from deleting work whose result is unused */
static volatile double g_sink_d; static volatile uint64_t g_sink_u;

int main(int argc, char **argv) {
    size_t n = argc > 1 ? (size_t)atol(argv[1]) : 8388608;
    int reps = argc > 2 ? atoi(argv[2]) : 5;
    double *a = malloc(n * sizeof(double));
    double *out = malloc(n * sizeof(double));
    uint32_t *tab = malloc((n / 2) * sizeof(uint32_t));      /* the "file of precomputed values", in RAM: the best case */
    if (!a || !out || !tab) return 2;
    RS = 7717;
    for (size_t i = 0; i < n; i++) a[i] = (double)(nr() % 40000) * 0.25;
    for (size_t i = 0; i < n / 2; i++) tab[i] = (uint32_t)nr();
    const double R = (double)n;
    const double PRIME_Y = 1000003.0;

    printf("sphereprobe: n = %zu, %d reps, best of each. ns per element.\n\n", n, reps);
    printf("  %-38s %10s\n", "step", "ns/elem");

    /* ---- 1. the projection, scalar: normalise (value, prime, index) to radius n */
    double best = 1e18;
    for (int r = 0; r < reps; r++) {
        double t0 = now_ms(); double acc = 0;
        for (size_t i = 0; i < n; i++) {
            double x = a[i], y = PRIME_Y, z = (double)i;
            double s = R / sqrt(x * x + y * y + z * z);
            acc += x * s + y * s + z * s;
        }
        double d = now_ms() - t0; g_sink_d = acc; if (d < best) best = d;
    }
    printf("  %-38s %10.2f\n", "sphere projection (scalar sqrt)", best * 1e6 / (double)n);

    /* ---- 2. the projection with a reciprocal-sqrt friendly form, auto-vectorisable */
    best = 1e18;
    for (int r = 0; r < reps; r++) {
        double t0 = now_ms();
        for (size_t i = 0; i < n; i++) {
            double x = a[i], y = PRIME_Y, z = (double)i;
            double s = R / sqrt(x * x + y * y + z * z);
            out[i] = x * s;
        }
        double d = now_ms() - t0; g_sink_d = out[n - 1]; if (d < best) best = d;
    }
    printf("  %-38s %10.2f\n", "sphere projection (store x only)", best * 1e6 / (double)n);

    /* ---- 3. a prime MODULUS hash */
    best = 1e18;
    for (int r = 0; r < reps; r++) {
        double t0 = now_ms(); uint64_t acc = 0;
        for (size_t i = 0; i < n; i++) {
            uint64_t u; memcpy(&u, &a[i], 8);
            acc += (u ^ (i * 2654435761u)) % 1000003u;
        }
        double d = now_ms() - t0; g_sink_u = acc; if (d < best) best = d;
    }
    printf("  %-38s %10.2f\n", "prime modulus hash (integer %)", best * 1e6 / (double)n);

    /* ---- 4. the multiply-shift hash the counting route already uses */
    best = 1e18;
    for (int r = 0; r < reps; r++) {
        double t0 = now_ms(); uint64_t acc = 0;
        for (size_t i = 0; i < n; i++) {
            uint64_t u; memcpy(&u, &a[i], 8);
            acc += (u * 0x9E3779B97F4A7C15ULL) >> 52;
        }
        double d = now_ms() - t0; g_sink_u = acc; if (d < best) best = d;
    }
    printf("  %-38s %10.2f\n", "multiply-shift hash (the engine's)", best * 1e6 / (double)n);

    /* ---- 5. one random lookup per element into the precomputed table */
    best = 1e18;
    for (int r = 0; r < reps; r++) {
        double t0 = now_ms(); uint64_t acc = 0;
        for (size_t i = 0; i < n; i++) {
            uint64_t u; memcpy(&u, &a[i], 8);
            size_t h = (size_t)((u * 0x9E3779B97F4A7C15ULL) >> 33) % (n / 2);
            acc += tab[h];
        }
        double d = now_ms() - t0; g_sink_u = acc; if (d < best) best = d;
    }
    printf("  %-38s %10.2f\n", "precomputed-table lookup (random)", best * 1e6 / (double)n);

    /* ---- 6. one multiply-add: what the engine actually does to address an element */
    best = 1e18;
    double lo = 0.0, scale = 2048.0 / 10000.0;
    for (int r = 0; r < reps; r++) {
        double t0 = now_ms(); uint64_t acc = 0;
        for (size_t i = 0; i < n; i++) acc += (uint64_t)(long)((a[i] - lo) * scale);
        double d = now_ms() - t0; g_sink_u = acc; if (d < best) best = d;
    }
    printf("  %-38s %10.2f\n", "one multiply-add (engine addressing)", best * 1e6 / (double)n);

    /* ---- the correctness objection: a fixed-radius projection is many-to-one */
    size_t coll = 0, checked = 0;
    for (size_t i = 0; i + 1 < n && checked < 2000000; i += 7, checked++) {
        double x1 = a[i], x2 = a[i + 1];
        if (x1 == x2) continue;
        double z = (double)i;
        double s1 = R / sqrt(x1 * x1 + PRIME_Y * PRIME_Y + z * z);
        double s2 = R / sqrt(x2 * x2 + PRIME_Y * PRIME_Y + z * z);
        if ((x1 * s1) == (x2 * s2)) coll++;       /* different values, same projected x, same index */
    }
    printf("\n  distinct values collapsing to the same projected x, at the SAME index: %zu of %zu pairs\n", coll, checked);
    free(a); free(out); free(tab);
    return 0;
}

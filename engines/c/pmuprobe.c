/* Does this machine expose hardware performance counters to a user process? (2026-09-28)
 *
 * WSL2 has no `perf` tool installed and Hyper-V does not always virtualise the PMU, so this asks the kernel directly
 * through perf_event_open, one event at a time, around a loop whose counts are known in advance (1e8 iterations of an
 * add and a predictable branch: about 3e8 instructions, about 1e8 branches, almost no misses). An event that cannot be
 * opened, or that opens and reads zero, is reported as such. It decides whether "what makes a sort fast" can be
 * answered with counts (instructions, branch misses, cache misses per element) or only with clocks.
 *
 * usage: ./pmuprobe */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <linux/perf_event.h>

static int ev_open(uint32_t type, uint64_t config) {
    struct perf_event_attr a; memset(&a, 0, sizeof a);
    a.size = sizeof a; a.type = type; a.config = config;
    a.disabled = 1; a.exclude_kernel = 1; a.exclude_hv = 1;
    return (int)syscall(SYS_perf_event_open, &a, 0, -1, -1, 0);
}
#define CACHE(c, o, r) ((uint64_t)(c) | ((uint64_t)(o) << 8) | ((uint64_t)(r) << 16))

int main(void) {
    struct { const char *name; uint32_t type; uint64_t config; } E[] = {
        { "instructions",        PERF_TYPE_HARDWARE, PERF_COUNT_HW_INSTRUCTIONS },
        { "cycles",              PERF_TYPE_HARDWARE, PERF_COUNT_HW_CPU_CYCLES },
        { "branches",            PERF_TYPE_HARDWARE, PERF_COUNT_HW_BRANCH_INSTRUCTIONS },
        { "branch-misses",       PERF_TYPE_HARDWARE, PERF_COUNT_HW_BRANCH_MISSES },
        { "cache-references",    PERF_TYPE_HARDWARE, PERF_COUNT_HW_CACHE_REFERENCES },
        { "cache-misses (LLC)",  PERF_TYPE_HARDWARE, PERF_COUNT_HW_CACHE_MISSES },
        { "L1d load misses",     PERF_TYPE_HW_CACHE, CACHE(PERF_COUNT_HW_CACHE_L1D, PERF_COUNT_HW_CACHE_OP_READ, PERF_COUNT_HW_CACHE_RESULT_MISS) },
        { "dTLB load misses",    PERF_TYPE_HW_CACHE, CACHE(PERF_COUNT_HW_CACHE_DTLB, PERF_COUNT_HW_CACHE_OP_READ, PERF_COUNT_HW_CACHE_RESULT_MISS) },
        { "task-clock (sw, ns)", PERF_TYPE_SOFTWARE, PERF_COUNT_SW_TASK_CLOCK },
        { "page-faults (sw)",    PERF_TYPE_SOFTWARE, PERF_COUNT_SW_PAGE_FAULTS },
    };
    int usable = 0;
    printf("%-22s %-10s %s\n", "event", "status", "count over 1e8 iterations of an add and a branch");
    for (size_t k = 0; k < sizeof E / sizeof *E; k++) {
        int fd = ev_open(E[k].type, E[k].config);
        if (fd < 0) { printf("%-22s %-10s %s\n", E[k].name, "NO", strerror(errno)); continue; }
        ioctl(fd, PERF_EVENT_IOC_RESET, 0); ioctl(fd, PERF_EVENT_IOC_ENABLE, 0);
        volatile uint64_t acc = 0;
        for (uint64_t i = 0; i < 100000000ULL; i++) { if (i & 1) acc += i; }
        ioctl(fd, PERF_EVENT_IOC_DISABLE, 0);
        uint64_t v = 0; ssize_t got = read(fd, &v, sizeof v); close(fd);
        int ok = got == (ssize_t)sizeof v && v > 0;
        if (ok && E[k].type != PERF_TYPE_SOFTWARE) usable++;
        printf("%-22s %-10s %llu\n", E[k].name, ok ? "yes" : "opens, reads 0", (unsigned long long)v);
    }
    printf("\n%d hardware event(s) usable\n", usable);
    return usable ? 0 : 1;
}

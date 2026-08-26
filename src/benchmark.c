#include "../include/benchmark.h"
#include <stdio.h>
#include <time.h>
#include <sys/resource.h>

double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1000000.0;
}

void fill_usage(BenchmarkResult *result) {
    /*
     * Aggregate resource usage from the current process and its waited-for
     * children. This provides a fuller picture of CPU time and context
     * switches consumed by a parent/child benchmark pair without
     * double-counting: RUSAGE_SELF returns usage for this process, and
     * RUSAGE_CHILDREN returns cumulative usage for terminated child
     * processes that have been waited for. We sum them to report total
     * CPU time and switches attributable to the benchmark run.
     */
    struct rusage ru_self, ru_children;
    if (getrusage(RUSAGE_SELF, &ru_self) != 0) {
        ru_self.ru_utime.tv_sec = ru_self.ru_utime.tv_usec = 0;
        ru_self.ru_stime.tv_sec = ru_self.ru_stime.tv_usec = 0;
        ru_self.ru_nvcsw = ru_self.ru_nivcsw = 0;
    }
    if (getrusage(RUSAGE_CHILDREN, &ru_children) != 0) {
        ru_children.ru_utime.tv_sec = ru_children.ru_utime.tv_usec = 0;
        ru_children.ru_stime.tv_sec = ru_children.ru_stime.tv_usec = 0;
        ru_children.ru_nvcsw = ru_children.ru_nivcsw = 0;
    }

    double user_ms = (double)(ru_self.ru_utime.tv_sec + ru_children.ru_utime.tv_sec) * 1000.0
                   + (double)(ru_self.ru_utime.tv_usec + ru_children.ru_utime.tv_usec) / 1000.0;
    double sys_ms = (double)(ru_self.ru_stime.tv_sec + ru_children.ru_stime.tv_sec) * 1000.0
                  + (double)(ru_self.ru_stime.tv_usec + ru_children.ru_stime.tv_usec) / 1000.0;

    result->user_ms = user_ms;
    result->sys_ms = sys_ms;
    result->voluntary_ctx_switches = ru_self.ru_nvcsw + ru_children.ru_nvcsw;
    result->involuntary_ctx_switches = ru_self.ru_nivcsw + ru_children.ru_nivcsw;
}

void print_result(const char *method, size_t bytes, const BenchmarkResult *result) {
    printf("\n=== %s RESULT ===\n", method);
    printf("Transferred            : %zu bytes\n", bytes);
    printf("Elapsed time           : %.3f ms\n", result->elapsed_ms);
    printf("Throughput             : %.3f MB/s\n", result->throughput_mbps);
    printf("Voluntary ctx switches : %ld\n", result->voluntary_ctx_switches);
    printf("Involuntary ctx switch : %ld\n", result->involuntary_ctx_switches);
}

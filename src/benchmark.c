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
    struct rusage ru;
    if (getrusage(RUSAGE_SELF, &ru) == 0) {
        result->voluntary_ctx_switches = ru.ru_nvcsw;
        result->involuntary_ctx_switches = ru.ru_nivcsw;
    }
}

void print_result(const char *method, size_t bytes, const BenchmarkResult *result) {
    printf("\n=== %s RESULT ===\n", method);
    printf("Transferred            : %zu bytes\n", bytes);
    printf("Elapsed time           : %.3f ms\n", result->elapsed_ms);
    printf("Throughput             : %.3f MB/s\n", result->throughput_mbps);
    printf("Voluntary ctx switches : %ld\n", result->voluntary_ctx_switches);
    printf("Involuntary ctx switch : %ld\n", result->involuntary_ctx_switches);
}

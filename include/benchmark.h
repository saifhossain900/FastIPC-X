#ifndef BENCHMARK_H
#define BENCHMARK_H

#include <stddef.h>

typedef struct {
    double elapsed_ms;
    double throughput_mbps;
    long voluntary_ctx_switches;
    long involuntary_ctx_switches;
} BenchmarkResult;

double now_ms(void);
void fill_usage(BenchmarkResult *result);
void print_result(const char *method, size_t bytes, const BenchmarkResult *result);

#endif

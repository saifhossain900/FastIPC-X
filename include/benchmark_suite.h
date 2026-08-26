#ifndef BENCHMARK_SUITE_H
#define BENCHMARK_SUITE_H

#include <stddef.h>
#include "benchmark.h"

typedef struct {
    double elapsed_ms;
    double throughput_mbps;
    double user_ms;
    double sys_ms;
    long voluntary_ctx_switches;
    long involuntary_ctx_switches;
} BenchmarkTrial;

typedef struct {
    const char *method;
    size_t payload_bytes;
    size_t chunk_bytes;
    size_t trials;
    double min_ms;
    double max_ms;
    double mean_ms;
    double median_ms;
    double mean_throughput_mbps;
    double median_throughput_mbps;
    double mean_user_ms;
    double mean_sys_ms;
    double mean_voluntary_ctx_switches;
    double mean_involuntary_ctx_switches;
} BenchmarkAggregate;

int run_benchmark_suite(size_t payload_bytes, size_t trials, size_t chunk_bytes, const char *out_csv);

#endif

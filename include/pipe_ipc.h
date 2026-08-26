#ifndef PIPE_IPC_H
#define PIPE_IPC_H

#include <stddef.h>
#include "benchmark.h"

int run_pipe_benchmark(size_t total_bytes, size_t chunk_size, BenchmarkResult *result);

#endif

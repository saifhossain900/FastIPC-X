#ifndef FIFO_IPC_H
#define FIFO_IPC_H

#include <stddef.h>
#include "benchmark.h"

int run_fifo_benchmark(size_t total_bytes, size_t chunk_size, BenchmarkResult *result);

#endif

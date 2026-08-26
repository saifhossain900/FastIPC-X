#ifndef SHM_IPC_H
#define SHM_IPC_H

#include <stddef.h>
#include "benchmark.h"

int run_shm_benchmark(size_t total_bytes, size_t chunk_size, BenchmarkResult *result);

#endif

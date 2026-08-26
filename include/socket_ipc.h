#ifndef SOCKET_IPC_H
#define SOCKET_IPC_H

#include <stddef.h>
#include "benchmark.h"

int run_socket_benchmark(size_t total_bytes, size_t chunk_size, BenchmarkResult *result);

#endif

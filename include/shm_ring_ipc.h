#ifndef SHM_RING_IPC_H
#define SHM_RING_IPC_H

#include <stddef.h>
#include "benchmark.h"

/*
 * Normal production SHM-RING benchmark.
 *
 * No CPU affinity is forced. Linux is free to schedule
 * producer and consumer on any CPU allowed for the process.
 */
int run_shm_ring_benchmark(
    size_t total_bytes,
    size_t chunk_size,
    BenchmarkResult *result
);

/*
 * Same production SHM-RING implementation, but optionally
 * pins the producer and/or consumer to specified logical CPUs.
 *
 * producer_cpu < 0  -> producer remains unpinned
 * consumer_cpu < 0  -> consumer remains unpinned
 *
 * This function exists for controlled scheduler/affinity
 * experiments. The normal production function above remains
 * scheduler-controlled.
 */
int run_shm_ring_benchmark_affinity(
    size_t total_bytes,
    size_t chunk_size,
    int producer_cpu,
    int consumer_cpu,
    BenchmarkResult *result
);

#endif
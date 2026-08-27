#ifndef CPU_AFFINITY_ANALYZER_H
#define CPU_AFFINITY_ANALYZER_H

#include <stddef.h>

int run_cpu_affinity_analysis(
    size_t total_bytes,
    size_t chunk_size,
    size_t trials
);

#endif
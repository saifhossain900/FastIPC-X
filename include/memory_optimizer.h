#ifndef MEMORY_OPTIMIZER_H
#define MEMORY_OPTIMIZER_H

#include <stddef.h>

int run_memory_optimizer(
    size_t total_bytes,
    size_t chunk_size,
    size_t trials
);

#endif
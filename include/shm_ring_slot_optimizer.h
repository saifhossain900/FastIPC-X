#ifndef SHM_RING_SLOT_OPTIMIZER_H
#define SHM_RING_SLOT_OPTIMIZER_H

#include <stddef.h>

int run_shm_ring_slot_optimizer(
    size_t total_bytes,
    size_t chunk_size,
    size_t trials
);

#endif
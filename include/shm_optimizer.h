#ifndef SHM_OPTIMIZER_H
#define SHM_OPTIMIZER_H

#include <stddef.h>

int run_shm_optimization(size_t payload_bytes, size_t chunk_size, size_t trials);

#endif

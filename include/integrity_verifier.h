#ifndef INTEGRITY_VERIFIER_H
#define INTEGRITY_VERIFIER_H

#include <stddef.h>

int run_integrity_verification(
    const char *method,
    size_t payload_mb,
    size_t chunk_kb
);

#endif
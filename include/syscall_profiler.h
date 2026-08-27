#ifndef SYSCALL_PROFILER_H
#define SYSCALL_PROFILER_H

#include <stddef.h>

int run_syscall_profile(
    const char *method,
    size_t payload_mb,
    size_t chunk_kb
);

int compare_syscall_profiles(
    const char *baseline_method,
    const char *optimized_method,
    size_t payload_mb,
    size_t chunk_kb
);

#endif
#include "../include/benchmark.h"
#include "../include/pipe_ipc.h"
#include "../include/fifo_ipc.h"
#include "../include/socket_ipc.h"
#include "../include/shm_ipc.h"
#include "../include/shm_ring_ipc.h"
#include "../include/benchmark_suite.h"
#include "../include/optimizer.h"
#include "../include/shm_optimizer.h"
#include "../include/adaptive_selector.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void usage(const char *program) {
    printf("FastIPC-X - Adaptive IPC Optimization Engine\n\n");
    printf("Usage:\n");
    printf("  %s pipe <size_mb> <chunk_kb>\n", program);
    printf("  %s fifo <size_mb> <chunk_kb>\n", program);
    printf("  %s socket <size_mb> <chunk_kb>\n\n", program);
    printf("  %s shm <size_mb> <chunk_kb>\n\n", program);
    printf("Example:\n");
    printf("  %s pipe 100 64\n", program);
}

static int parse_positive(const char *text, unsigned long long *out) {
    char *end = NULL;
    errno = 0;
    unsigned long long value = strtoull(text, &end, 10);

    if (errno != 0 || !end || *end != '\0' || value == 0) {
        return -1;
    }

    *out = value;
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        usage(argv[0]);
        return 1;
    }

    const char *cmd = argv[1];
    unsigned long long size_mb = 0;
    unsigned long long chunk_kb = 0;

    /* Special handling for commands that take only size_mb argument */
    if (strcmp(cmd, "recommend") == 0 || strcmp(cmd, "auto") == 0) {
        if (argc != 3) { 
            fprintf(stderr, "Usage: %s %s <size_mb>\n", argv[0], cmd);
            return 1; 
        }
        if (parse_positive(argv[2], &size_mb) != 0) {
            fprintf(stderr, "Size must be a positive integer.\n");
            return 1;
        }
        size_t total_bytes = (size_t)size_mb * 1024ULL * 1024ULL;
        if (strcmp(cmd, "recommend") == 0) {
            if (adaptive_recommend(total_bytes) != 0) return 2;
            return 0;
        } else {
            if (adaptive_auto(total_bytes) != 0) return 2;
            return 0;
        }
    }

    /* Validate argument counts per command */
    if (strcmp(cmd, "optimize-shm") == 0) {
        if (argc != 5) { usage(argv[0]); return 1; }
        if (parse_positive(argv[2], &size_mb) != 0 || parse_positive(argv[3], &chunk_kb) != 0) {
            fprintf(stderr, "Size and chunk must be positive integers.\n");
            return 1;
        }
    } else {
        /* Most commands expect 3 arguments after program */
        if (argc != 4) { usage(argv[0]); return 1; }
        if (parse_positive(argv[2], &size_mb) != 0 || parse_positive(argv[3], &chunk_kb) != 0) {
            fprintf(stderr, "Size and chunk must be positive integers.\n");
            return 1;
        }
    }

    size_t total_bytes = (size_t)size_mb * 1024ULL * 1024ULL;
    size_t chunk_size = (size_t)chunk_kb * 1024ULL;

    BenchmarkResult result = {0};

    if (strcmp(argv[1], "pipe") == 0) {
        if (run_pipe_benchmark(total_bytes, chunk_size, &result) != 0) {
            return 2;
        }

        print_result("PIPE", total_bytes, &result);
        return 0;
    }

    if (strcmp(argv[1], "fifo") == 0) {
        if (run_fifo_benchmark(total_bytes, chunk_size, &result) != 0) {
            return 2;
        }

        print_result("FIFO", total_bytes, &result);
        return 0;
    }

    if (strcmp(argv[1], "socket") == 0) {
        if (run_socket_benchmark(total_bytes, chunk_size, &result) != 0) {
            return 2;
        }

        print_result("SOCKET", total_bytes, &result);
        return 0;
    }

    if (strcmp(argv[1], "shm") == 0) {
        if (run_shm_benchmark(total_bytes, chunk_size, &result) != 0) {
            return 2;
        }

        print_result("SHM", total_bytes, &result);
        return 0;
    }

    if (strcmp(argv[1], "shm-opt") == 0) {
        if (run_shm_ring_benchmark(total_bytes, chunk_size, &result) != 0) {
            return 2;
        }

        print_result("SHM-OPT", total_bytes, &result);
        return 0;
    }

    if (strcmp(argv[1], "optimize-shm") == 0) {
        if (argc != 5) {
            fprintf(stderr, "Usage: %s optimize-shm <size_mb> <chunk_kb> <trials>\n", argv[0]);
            return 1;
        }
        unsigned long long trials = 0;
        if (parse_positive(argv[4], &trials) != 0) {
            fprintf(stderr, "trials must be a positive integer.\n");
            return 1;
        }

        if (run_shm_optimization(total_bytes, chunk_size, (size_t)trials) != 0) {
            return 2;
        }
        return 0;
    }

    if (strcmp(argv[1], "benchmark") == 0) {
        unsigned long long trials = 0;
        if (parse_positive(argv[3], &trials) != 0) {
            fprintf(stderr, "trials must be a positive integer.\n");
            return 1;
        }

        /* Use default chunk size 64KB for benchmark command */
        size_t default_chunk = 64 * 1024;
        char csvpath[256];
        snprintf(csvpath, sizeof(csvpath), "results/benchmark_%zuMB.csv", (size_t)size_mb);
        if (run_benchmark_suite(total_bytes, (size_t)trials, default_chunk, csvpath) != 0) {
            return 2;
        }
        return 0;
    }

    if (strcmp(argv[1], "optimize-chunk") == 0) {
        unsigned long long trials = 0;
        if (parse_positive(argv[3], &trials) != 0) {
            fprintf(stderr, "trials must be a positive integer.\n");
            return 1;
        }

        if (run_chunk_optimizer(total_bytes, (size_t)trials) != 0) {
            return 2;
        }
        return 0;
    }

    fprintf(stderr, "Unknown IPC method: %s\n", argv[1]);
    usage(argv[0]);
    return 1;
}

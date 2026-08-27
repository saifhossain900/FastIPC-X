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
#include "../include/syscall_profiler.h"
#include "../include/integrity_verifier.h"
#include "../include/workload_profiler.h"
#include "../include/shm_ring_slot_optimizer.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


/* =========================================================
   Usage / Help
   ========================================================= */

static void usage(const char *program)
{
    printf("\n");
    printf("FastIPC-X - Adaptive IPC Optimization Engine\n");
    printf("============================================================\n\n");

    printf("Basic IPC Benchmarks:\n");
    printf("  %s pipe <size_mb> <chunk_kb>\n", program);
    printf("  %s fifo <size_mb> <chunk_kb>\n", program);
    printf("  %s socket <size_mb> <chunk_kb>\n", program);
    printf("  %s shm <size_mb> <chunk_kb>\n", program);
    printf("  %s shm-opt <size_mb> <chunk_kb>\n", program);

    printf("\nBenchmark / Optimization:\n");
    printf("  %s benchmark <size_mb> <trials>\n", program);
    printf("  %s optimize-chunk <size_mb> <trials>\n", program);
    printf(
        "  %s optimize-shm <size_mb> <chunk_kb> <trials>\n",
        program
    );
    printf(
        "  %s optimize-ring-slots "
        "<size_mb> <chunk_kb> <trials>\n",
        program
    );

    printf("\nAdaptive Selection:\n");
    printf("  %s recommend <size_mb>\n", program);
    printf("  %s auto <size_mb>\n", program);

    printf("\nMulti-Workload Adaptive Analysis:\n");
    printf(
        "  %s build-workloads <trials>\n",
        program
    );

    printf("\nSystem Call Analysis:\n");
    printf(
        "  %s profile <method> <size_mb> <chunk_kb>\n",
        program
    );
    printf(
        "  %s compare-syscalls <baseline> <optimized> "
        "<size_mb> <chunk_kb>\n",
        program
    );

    printf("\nCorrectness Verification:\n");
    printf(
        "  %s verify <method> <size_mb> <chunk_kb>\n",
        program
    );

    printf("\nSupported profile methods:\n");
    printf("  pipe fifo socket shm shm-opt\n");

    printf("\nSupported verification methods:\n");
    printf("  pipe fifo socket shm shm-opt\n");

    printf("\nExamples:\n");
    printf("  %s pipe 100 64\n", program);
    printf("  %s benchmark 100 5\n", program);
    printf("  %s optimize-chunk 100 5\n", program);
    printf("  %s optimize-shm 100 64 5\n", program);
    printf(
        "  %s optimize-ring-slots 100 64 5\n",
        program
    );
    printf("  %s recommend 100\n", program);
    printf("  %s auto 100\n", program);
    printf("  %s build-workloads 3\n", program);
    printf("  %s profile pipe 10 64\n", program);
    printf(
        "  %s compare-syscalls shm shm-opt 100 64\n",
        program
    );
    printf(
        "  %s verify shm-opt 100 64\n",
        program
    );

    printf("\n");
}


/* =========================================================
   Positive integer parser
   ========================================================= */

static int parse_positive(
    const char *text,
    unsigned long long *out
)
{
    char *end = NULL;

    errno = 0;

    unsigned long long value =
        strtoull(
            text,
            &end,
            10
        );

    if (
        errno != 0 ||
        end == text ||
        end == NULL ||
        *end != '\0' ||
        value == 0
    ) {
        return -1;
    }

    *out = value;

    return 0;
}


/* =========================================================
   Main
   ========================================================= */

int main(int argc, char **argv)
{
    if (argc < 2) {
        usage(argv[0]);
        return 1;
    }

    const char *cmd = argv[1];


    /* =====================================================
       ADAPTIVE RECOMMEND

       Example:
       ./fastipc recommend 100
       ===================================================== */

    if (strcmp(cmd, "recommend") == 0) {

        if (argc != 3) {

            fprintf(
                stderr,
                "Usage: %s recommend <size_mb>\n",
                argv[0]
            );

            return 1;
        }

        unsigned long long size_mb = 0;

        if (
            parse_positive(
                argv[2],
                &size_mb
            ) != 0
        ) {
            fprintf(
                stderr,
                "Size must be a positive integer.\n"
            );

            return 1;
        }

        size_t total_bytes =
            (size_t)size_mb *
            1024ULL *
            1024ULL;

        if (
            adaptive_recommend(
                total_bytes
            ) != 0
        ) {
            return 2;
        }

        return 0;
    }


    /* =====================================================
       ADAPTIVE AUTO

       Example:
       ./fastipc auto 100
       ===================================================== */

    if (strcmp(cmd, "auto") == 0) {

        if (argc != 3) {

            fprintf(
                stderr,
                "Usage: %s auto <size_mb>\n",
                argv[0]
            );

            return 1;
        }

        unsigned long long size_mb = 0;

        if (
            parse_positive(
                argv[2],
                &size_mb
            ) != 0
        ) {
            fprintf(
                stderr,
                "Size must be a positive integer.\n"
            );

            return 1;
        }

        size_t total_bytes =
            (size_t)size_mb *
            1024ULL *
            1024ULL;

        if (
            adaptive_auto(
                total_bytes
            ) != 0
        ) {
            return 2;
        }

        return 0;
    }


    /* =====================================================
       MULTI-WORKLOAD ADAPTIVE PROFILER

       Example:
       ./fastipc build-workloads 3
       ===================================================== */

    if (
        strcmp(
            cmd,
            "build-workloads"
        ) == 0
    ) {

        if (argc != 3) {

            fprintf(
                stderr,
                "Usage: %s build-workloads <trials>\n",
                argv[0]
            );

            return 1;
        }

        unsigned long long trials = 0;

        if (
            parse_positive(
                argv[2],
                &trials
            ) != 0
        ) {
            fprintf(
                stderr,
                "Trials must be a positive integer.\n"
            );

            return 1;
        }

        if (
            run_workload_profiles(
                (size_t)trials
            ) != 0
        ) {
            return 2;
        }

        return 0;
    }


    /* =====================================================
       SYSTEM CALL PROFILE

       Example:
       ./fastipc profile pipe 10 64
       ===================================================== */

    if (strcmp(cmd, "profile") == 0) {

        if (argc != 5) {

            fprintf(
                stderr,
                "Usage: %s profile "
                "<method> <size_mb> <chunk_kb>\n",
                argv[0]
            );

            fprintf(
                stderr,
                "Methods: pipe fifo socket shm shm-opt\n"
            );

            return 1;
        }

        const char *method =
            argv[2];

        unsigned long long size_mb = 0;
        unsigned long long chunk_kb = 0;

        if (
            parse_positive(
                argv[3],
                &size_mb
            ) != 0
        ) {
            fprintf(
                stderr,
                "Size must be a positive integer.\n"
            );

            return 1;
        }

        if (
            parse_positive(
                argv[4],
                &chunk_kb
            ) != 0
        ) {
            fprintf(
                stderr,
                "Chunk size must be a positive integer.\n"
            );

            return 1;
        }

        if (
            run_syscall_profile(
                method,
                (size_t)size_mb,
                (size_t)chunk_kb
            ) != 0
        ) {
            return 2;
        }

        return 0;
    }


    /* =====================================================
       SYSTEM CALL COMPARISON

       Example:
       ./fastipc compare-syscalls shm shm-opt 100 64
       ===================================================== */

    if (
        strcmp(
            cmd,
            "compare-syscalls"
        ) == 0
    ) {

        if (argc != 6) {

            fprintf(
                stderr,
                "Usage: %s compare-syscalls "
                "<baseline> <optimized> "
                "<size_mb> <chunk_kb>\n",
                argv[0]
            );

            fprintf(
                stderr,
                "Example:\n"
                "  %s compare-syscalls "
                "shm shm-opt 100 64\n",
                argv[0]
            );

            return 1;
        }

        const char *baseline_method =
            argv[2];

        const char *optimized_method =
            argv[3];

        unsigned long long size_mb = 0;
        unsigned long long chunk_kb = 0;

        if (
            parse_positive(
                argv[4],
                &size_mb
            ) != 0
        ) {
            fprintf(
                stderr,
                "Size must be a positive integer.\n"
            );

            return 1;
        }

        if (
            parse_positive(
                argv[5],
                &chunk_kb
            ) != 0
        ) {
            fprintf(
                stderr,
                "Chunk size must be a positive integer.\n"
            );

            return 1;
        }

        if (
            compare_syscall_profiles(
                baseline_method,
                optimized_method,
                (size_t)size_mb,
                (size_t)chunk_kb
            ) != 0
        ) {
            return 2;
        }

        return 0;
    }


    /* =====================================================
       DATA INTEGRITY VERIFICATION

       Example:
       ./fastipc verify shm-opt 100 64
       ===================================================== */

    if (strcmp(cmd, "verify") == 0) {

        if (argc != 5) {

            fprintf(
                stderr,
                "Usage: %s verify "
                "<method> <size_mb> <chunk_kb>\n",
                argv[0]
            );

            fprintf(
                stderr,
                "Methods: pipe fifo socket shm shm-opt\n"
            );

            return 1;
        }

        const char *method =
            argv[2];

        unsigned long long size_mb = 0;
        unsigned long long chunk_kb = 0;

        if (
            parse_positive(
                argv[3],
                &size_mb
            ) != 0
        ) {
            fprintf(
                stderr,
                "Size must be a positive integer.\n"
            );

            return 1;
        }

        if (
            parse_positive(
                argv[4],
                &chunk_kb
            ) != 0
        ) {
            fprintf(
                stderr,
                "Chunk size must be a positive integer.\n"
            );

            return 1;
        }

        if (
            run_integrity_verification(
                method,
                (size_t)size_mb,
                (size_t)chunk_kb
            ) != 0
        ) {
            return 2;
        }

        return 0;
    }


    /* =====================================================
       BENCHMARK SUITE

       Example:
       ./fastipc benchmark 100 5
       ===================================================== */

    if (strcmp(cmd, "benchmark") == 0) {

        if (argc != 4) {

            fprintf(
                stderr,
                "Usage: %s benchmark "
                "<size_mb> <trials>\n",
                argv[0]
            );

            return 1;
        }

        unsigned long long size_mb = 0;
        unsigned long long trials = 0;

        if (
            parse_positive(
                argv[2],
                &size_mb
            ) != 0
        ) {
            fprintf(
                stderr,
                "Size must be a positive integer.\n"
            );

            return 1;
        }

        if (
            parse_positive(
                argv[3],
                &trials
            ) != 0
        ) {
            fprintf(
                stderr,
                "Trials must be a positive integer.\n"
            );

            return 1;
        }

        size_t total_bytes =
            (size_t)size_mb *
            1024ULL *
            1024ULL;

        size_t default_chunk =
            64 * 1024;

        char csvpath[256];

        snprintf(
            csvpath,
            sizeof(csvpath),
            "results/benchmark_%zuMB.csv",
            (size_t)size_mb
        );

        if (
            run_benchmark_suite(
                total_bytes,
                (size_t)trials,
                default_chunk,
                csvpath
            ) != 0
        ) {
            return 2;
        }

        return 0;
    }


    /* =====================================================
       CHUNK SIZE OPTIMIZER

       Example:
       ./fastipc optimize-chunk 100 5
       ===================================================== */

    if (
        strcmp(
            cmd,
            "optimize-chunk"
        ) == 0
    ) {

        if (argc != 4) {

            fprintf(
                stderr,
                "Usage: %s optimize-chunk "
                "<size_mb> <trials>\n",
                argv[0]
            );

            return 1;
        }

        unsigned long long size_mb = 0;
        unsigned long long trials = 0;

        if (
            parse_positive(
                argv[2],
                &size_mb
            ) != 0
        ) {
            fprintf(
                stderr,
                "Size must be a positive integer.\n"
            );

            return 1;
        }

        if (
            parse_positive(
                argv[3],
                &trials
            ) != 0
        ) {
            fprintf(
                stderr,
                "Trials must be a positive integer.\n"
            );

            return 1;
        }

        size_t total_bytes =
            (size_t)size_mb *
            1024ULL *
            1024ULL;

        if (
            run_chunk_optimizer(
                total_bytes,
                (size_t)trials
            ) != 0
        ) {
            return 2;
        }

        return 0;
    }


    /* =====================================================
       SHM SYNCHRONIZATION OPTIMIZER

       Example:
       ./fastipc optimize-shm 100 64 5
       ===================================================== */

    if (
        strcmp(
            cmd,
            "optimize-shm"
        ) == 0
    ) {

        if (argc != 5) {

            fprintf(
                stderr,
                "Usage: %s optimize-shm "
                "<size_mb> <chunk_kb> <trials>\n",
                argv[0]
            );

            return 1;
        }

        unsigned long long size_mb = 0;
        unsigned long long chunk_kb = 0;
        unsigned long long trials = 0;

        if (
            parse_positive(
                argv[2],
                &size_mb
            ) != 0
        ) {
            fprintf(
                stderr,
                "Size must be a positive integer.\n"
            );

            return 1;
        }

        if (
            parse_positive(
                argv[3],
                &chunk_kb
            ) != 0
        ) {
            fprintf(
                stderr,
                "Chunk size must be a positive integer.\n"
            );

            return 1;
        }

        if (
            parse_positive(
                argv[4],
                &trials
            ) != 0
        ) {
            fprintf(
                stderr,
                "Trials must be a positive integer.\n"
            );

            return 1;
        }

        size_t total_bytes =
            (size_t)size_mb *
            1024ULL *
            1024ULL;

        size_t chunk_size =
            (size_t)chunk_kb *
            1024ULL;

        if (
            run_shm_optimization(
                total_bytes,
                chunk_size,
                (size_t)trials
            ) != 0
        ) {
            return 2;
        }

        return 0;
    }


    /* =====================================================
       SHM RING SLOT OPTIMIZER

       Example:
       ./fastipc optimize-ring-slots 100 64 5
       ===================================================== */

    if (
        strcmp(
            cmd,
            "optimize-ring-slots"
        ) == 0
    ) {

        if (argc != 5) {

            fprintf(
                stderr,
                "Usage: %s optimize-ring-slots "
                "<size_mb> <chunk_kb> <trials>\n",
                argv[0]
            );

            return 1;
        }

        unsigned long long size_mb = 0;
        unsigned long long chunk_kb = 0;
        unsigned long long trials = 0;


        if (
            parse_positive(
                argv[2],
                &size_mb
            ) != 0
        ) {

            fprintf(
                stderr,
                "Size must be a positive integer.\n"
            );

            return 1;
        }


        if (
            parse_positive(
                argv[3],
                &chunk_kb
            ) != 0
        ) {

            fprintf(
                stderr,
                "Chunk size must be a positive integer.\n"
            );

            return 1;
        }


        if (
            parse_positive(
                argv[4],
                &trials
            ) != 0
        ) {

            fprintf(
                stderr,
                "Trials must be a positive integer.\n"
            );

            return 1;
        }


        size_t total_bytes =
            (size_t)size_mb *
            1024ULL *
            1024ULL;


        size_t chunk_size =
            (size_t)chunk_kb *
            1024ULL;


        if (
            run_shm_ring_slot_optimizer(
                total_bytes,
                chunk_size,
                (size_t)trials
            ) != 0
        ) {

            return 2;
        }


        return 0;
    }


    /* =====================================================
       STANDARD IPC COMMANDS

       pipe
       fifo
       socket
       shm
       shm-opt
       ===================================================== */

    if (
        strcmp(cmd, "pipe") == 0 ||
        strcmp(cmd, "fifo") == 0 ||
        strcmp(cmd, "socket") == 0 ||
        strcmp(cmd, "shm") == 0 ||
        strcmp(cmd, "shm-opt") == 0
    ) {

        if (argc != 4) {

            fprintf(
                stderr,
                "Usage: %s %s "
                "<size_mb> <chunk_kb>\n",
                argv[0],
                cmd
            );

            return 1;
        }

        unsigned long long size_mb = 0;
        unsigned long long chunk_kb = 0;

        if (
            parse_positive(
                argv[2],
                &size_mb
            ) != 0
        ) {
            fprintf(
                stderr,
                "Size must be a positive integer.\n"
            );

            return 1;
        }

        if (
            parse_positive(
                argv[3],
                &chunk_kb
            ) != 0
        ) {
            fprintf(
                stderr,
                "Chunk size must be a positive integer.\n"
            );

            return 1;
        }

        size_t total_bytes =
            (size_t)size_mb *
            1024ULL *
            1024ULL;

        size_t chunk_size =
            (size_t)chunk_kb *
            1024ULL;

        BenchmarkResult result = {0};


        /* ---------------- PIPE ---------------- */

        if (strcmp(cmd, "pipe") == 0) {

            if (
                run_pipe_benchmark(
                    total_bytes,
                    chunk_size,
                    &result
                ) != 0
            ) {
                return 2;
            }

            print_result(
                "PIPE",
                total_bytes,
                &result
            );

            return 0;
        }


        /* ---------------- FIFO ---------------- */

        if (strcmp(cmd, "fifo") == 0) {

            if (
                run_fifo_benchmark(
                    total_bytes,
                    chunk_size,
                    &result
                ) != 0
            ) {
                return 2;
            }

            print_result(
                "FIFO",
                total_bytes,
                &result
            );

            return 0;
        }


        /* ---------------- SOCKET ---------------- */

        if (strcmp(cmd, "socket") == 0) {

            if (
                run_socket_benchmark(
                    total_bytes,
                    chunk_size,
                    &result
                ) != 0
            ) {
                return 2;
            }

            print_result(
                "SOCKET",
                total_bytes,
                &result
            );

            return 0;
        }


        /* ---------------- SHM ---------------- */

        if (strcmp(cmd, "shm") == 0) {

            if (
                run_shm_benchmark(
                    total_bytes,
                    chunk_size,
                    &result
                ) != 0
            ) {
                return 2;
            }

            print_result(
                "SHM",
                total_bytes,
                &result
            );

            return 0;
        }


        /* ---------------- SHM OPT ---------------- */

        if (strcmp(cmd, "shm-opt") == 0) {

            if (
                run_shm_ring_benchmark(
                    total_bytes,
                    chunk_size,
                    &result
                ) != 0
            ) {
                return 2;
            }

            print_result(
                "SHM-OPT",
                total_bytes,
                &result
            );

            return 0;
        }
    }


    /* =====================================================
       UNKNOWN COMMAND
       ===================================================== */

    fprintf(
        stderr,
        "Unknown FastIPC-X command: %s\n",
        cmd
    );

    usage(argv[0]);

    return 1;
}
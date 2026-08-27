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
#include "../include/environment_profiler.h"
#include "../include/run_manifest.h"
#include "../include/cpu_affinity_analyzer.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


#define RESULT_PATH_BUFFER 2048


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

    printf("\nScheduler / CPU Affinity:\n");

    printf(
        "  %s analyze-affinity "
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

    printf("\nSystem / Environment:\n");
    printf("  %s environment\n", program);

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

    printf("\nReproducibility:\n");
    printf(
        "  Experiment commands automatically create run manifests.\n"
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

    printf(
        "  %s analyze-affinity 100 64 5\n",
        program
    );

    printf("  %s recommend 100\n", program);
    printf("  %s auto 100\n", program);
    printf("  %s build-workloads 3\n", program);
    printf("  %s environment\n", program);
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
   Run-manifest helper
   ========================================================= */

static void record_manifest(
    int argc,
    char **argv,
    const char *category,
    const char *result_files,
    int command_exit_code
)
{
    if (
        write_run_manifest(
            argc,
            argv,
            category,
            result_files,
            command_exit_code
        ) != 0
    ) {
        fprintf(
            stderr,
            "Warning: experiment completed, but "
            "run manifest could not be recorded.\n"
        );
    }
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

    const char *cmd =
        argv[1];


    /* =====================================================
       ADAPTIVE RECOMMEND
       ===================================================== */

    if (
        strcmp(
            cmd,
            "recommend"
        ) == 0
    ) {
        if (argc != 3) {
            fprintf(
                stderr,
                "Usage: %s recommend <size_mb>\n",
                argv[0]
            );

            return 1;
        }

        unsigned long long size_mb =
            0;

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

        char result_files[
            RESULT_PATH_BUFFER
        ];

        snprintf(
            result_files,
            sizeof(result_files),
            "results/adaptive_profile_%lluMB.csv",
            size_mb
        );

        int rc =
            adaptive_recommend(
                total_bytes
            );

        record_manifest(
            argc,
            argv,
            "adaptive-recommendation",
            result_files,
            rc == 0 ? 0 : 2
        );

        if (rc != 0) {
            return 2;
        }

        return 0;
    }


    /* =====================================================
       ADAPTIVE AUTO
       ===================================================== */

    if (
        strcmp(
            cmd,
            "auto"
        ) == 0
    ) {
        if (argc != 3) {
            fprintf(
                stderr,
                "Usage: %s auto <size_mb>\n",
                argv[0]
            );

            return 1;
        }

        unsigned long long size_mb =
            0;

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

        char result_files[
            RESULT_PATH_BUFFER
        ];

        snprintf(
            result_files,
            sizeof(result_files),
            "results/adaptive_profile_%lluMB.csv",
            size_mb
        );

        int rc =
            adaptive_auto(
                total_bytes
            );

        record_manifest(
            argc,
            argv,
            "adaptive-auto",
            result_files,
            rc == 0 ? 0 : 2
        );

        if (rc != 0) {
            return 2;
        }

        return 0;
    }


    /* =====================================================
       MULTI-WORKLOAD ADAPTIVE PROFILER
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

        unsigned long long trials =
            0;

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

        const char *result_files =
            "results/workload_adaptive_summary.csv;"
            "results/adaptive_profile_1MB.csv;"
            "results/adaptive_profile_10MB.csv;"
            "results/adaptive_profile_100MB.csv;"
            "results/adaptive_profile_500MB.csv";

        int rc =
            run_workload_profiles(
                (size_t)trials
            );

        record_manifest(
            argc,
            argv,
            "multi-workload-adaptive-profile",
            result_files,
            rc == 0 ? 0 : 2
        );

        if (rc != 0) {
            return 2;
        }

        return 0;
    }


    /* =====================================================
       SYSTEM / ENVIRONMENT PROFILER
       ===================================================== */

    if (
        strcmp(
            cmd,
            "environment"
        ) == 0
    ) {
        if (argc != 2) {
            fprintf(
                stderr,
                "Usage: %s environment\n",
                argv[0]
            );

            return 1;
        }

        const char *result_files =
            "results/system_environment.txt;"
            "results/system_environment.csv";

        int rc =
            run_environment_profiler();

        record_manifest(
            argc,
            argv,
            "system-environment-profile",
            result_files,
            rc == 0 ? 0 : 2
        );

        if (rc != 0) {
            return 2;
        }

        return 0;
    }


    /* =====================================================
       CPU AFFINITY / SCHEDULER ANALYSIS

       Example:
       ./fastipc analyze-affinity 100 64 5
       ===================================================== */

    if (
        strcmp(
            cmd,
            "analyze-affinity"
        ) == 0
    ) {
        if (argc != 5) {
            fprintf(
                stderr,
                "Usage: %s analyze-affinity "
                "<size_mb> <chunk_kb> <trials>\n",
                argv[0]
            );

            return 1;
        }

        unsigned long long size_mb =
            0;

        unsigned long long chunk_kb =
            0;

        unsigned long long trials =
            0;

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

        char result_files[
            RESULT_PATH_BUFFER
        ];

        snprintf(
            result_files,
            sizeof(result_files),
            "results/cpu_affinity_trials_%lluMB_%lluKB.csv;"
            "results/cpu_affinity_summary_%lluMB_%lluKB.csv",
            size_mb,
            chunk_kb,
            size_mb,
            chunk_kb
        );

        int rc =
            run_cpu_affinity_analysis(
                total_bytes,
                chunk_size,
                (size_t)trials
            );

        record_manifest(
            argc,
            argv,
            "cpu-affinity-analysis",
            result_files,
            rc == 0 ? 0 : 2
        );

        if (rc != 0) {
            return 2;
        }

        return 0;
    }


    /* =====================================================
       SYSTEM CALL PROFILE
       ===================================================== */

    if (
        strcmp(
            cmd,
            "profile"
        ) == 0
    ) {
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

        unsigned long long size_mb =
            0;

        unsigned long long chunk_kb =
            0;

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

        char result_files[
            RESULT_PATH_BUFFER
        ];

        snprintf(
            result_files,
            sizeof(result_files),
            "results/syscall_profile_%s_%lluMB_%lluKB.csv",
            method,
            size_mb,
            chunk_kb
        );

        int rc =
            run_syscall_profile(
                method,
                (size_t)size_mb,
                (size_t)chunk_kb
            );

        record_manifest(
            argc,
            argv,
            "system-call-profile",
            result_files,
            rc == 0 ? 0 : 2
        );

        if (rc != 0) {
            return 2;
        }

        return 0;
    }


    /* =====================================================
       SYSTEM CALL COMPARISON
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

            return 1;
        }

        const char *baseline_method =
            argv[2];

        const char *optimized_method =
            argv[3];

        unsigned long long size_mb =
            0;

        unsigned long long chunk_kb =
            0;

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

        char result_files[
            RESULT_PATH_BUFFER
        ];

        snprintf(
            result_files,
            sizeof(result_files),
            "results/syscall_comparison_%s_vs_%s_"
            "%lluMB_%lluKB.csv",
            baseline_method,
            optimized_method,
            size_mb,
            chunk_kb
        );

        int rc =
            compare_syscall_profiles(
                baseline_method,
                optimized_method,
                (size_t)size_mb,
                (size_t)chunk_kb
            );

        record_manifest(
            argc,
            argv,
            "system-call-comparison",
            result_files,
            rc == 0 ? 0 : 2
        );

        if (rc != 0) {
            return 2;
        }

        return 0;
    }


    /* =====================================================
       DATA INTEGRITY VERIFICATION
       ===================================================== */

    if (
        strcmp(
            cmd,
            "verify"
        ) == 0
    ) {
        if (argc != 5) {
            fprintf(
                stderr,
                "Usage: %s verify "
                "<method> <size_mb> <chunk_kb>\n",
                argv[0]
            );

            return 1;
        }

        const char *method =
            argv[2];

        unsigned long long size_mb =
            0;

        unsigned long long chunk_kb =
            0;

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

        char result_files[
            RESULT_PATH_BUFFER
        ];

        snprintf(
            result_files,
            sizeof(result_files),
            "results/integrity_%s_%lluMB_%lluKB.csv",
            method,
            size_mb,
            chunk_kb
        );

        int rc =
            run_integrity_verification(
                method,
                (size_t)size_mb,
                (size_t)chunk_kb
            );

        record_manifest(
            argc,
            argv,
            "data-integrity-verification",
            result_files,
            rc == 0 ? 0 : 2
        );

        if (rc != 0) {
            return 2;
        }

        return 0;
    }


    /* =====================================================
       BENCHMARK SUITE
       ===================================================== */

    if (
        strcmp(
            cmd,
            "benchmark"
        ) == 0
    ) {
        if (argc != 4) {
            fprintf(
                stderr,
                "Usage: %s benchmark "
                "<size_mb> <trials>\n",
                argv[0]
            );

            return 1;
        }

        unsigned long long size_mb =
            0;

        unsigned long long trials =
            0;

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
            64 *
            1024;

        char csvpath[
            256
        ];

        snprintf(
            csvpath,
            sizeof(csvpath),
            "results/benchmark_%zuMB.csv",
            (size_t)size_mb
        );

        int rc =
            run_benchmark_suite(
                total_bytes,
                (size_t)trials,
                default_chunk,
                csvpath
            );

        record_manifest(
            argc,
            argv,
            "benchmark-suite",
            csvpath,
            rc == 0 ? 0 : 2
        );

        if (rc != 0) {
            return 2;
        }

        return 0;
    }


    /* =====================================================
       CHUNK SIZE OPTIMIZER
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

        unsigned long long size_mb =
            0;

        unsigned long long trials =
            0;

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

        char result_files[
            RESULT_PATH_BUFFER
        ];

        snprintf(
            result_files,
            sizeof(result_files),
            "results/chunk_optimization_%lluMB.csv;"
            "results/chunk_optimization_summary_%lluMB.csv",
            size_mb,
            size_mb
        );

        int rc =
            run_chunk_optimizer(
                total_bytes,
                (size_t)trials
            );

        record_manifest(
            argc,
            argv,
            "chunk-size-optimization",
            result_files,
            rc == 0 ? 0 : 2
        );

        if (rc != 0) {
            return 2;
        }

        return 0;
    }


    /* =====================================================
       SHM SYNCHRONIZATION OPTIMIZER
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

        unsigned long long size_mb =
            0;

        unsigned long long chunk_kb =
            0;

        unsigned long long trials =
            0;

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

        char result_files[
            RESULT_PATH_BUFFER
        ];

        snprintf(
            result_files,
            sizeof(result_files),
            "results/shm_baseline_%lluMB_%lluKB.csv;"
            "results/shm_opt_%lluMB_%lluKB.csv;"
            "results/shm_sync_optimization_%lluMB_%lluKB.csv",
            size_mb,
            chunk_kb,
            size_mb,
            chunk_kb,
            size_mb,
            chunk_kb
        );

        int rc =
            run_shm_optimization(
                total_bytes,
                chunk_size,
                (size_t)trials
            );

        record_manifest(
            argc,
            argv,
            "shm-synchronization-optimization",
            result_files,
            rc == 0 ? 0 : 2
        );

        if (rc != 0) {
            return 2;
        }

        return 0;
    }


    /* =====================================================
       SHM RING SLOT OPTIMIZER
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

        unsigned long long size_mb =
            0;

        unsigned long long chunk_kb =
            0;

        unsigned long long trials =
            0;

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

        char result_files[
            RESULT_PATH_BUFFER
        ];

        snprintf(
            result_files,
            sizeof(result_files),
            "results/shm_ring_slot_trials_%lluMB_%lluKB.csv;"
            "results/shm_ring_slot_summary_%lluMB_%lluKB.csv",
            size_mb,
            chunk_kb,
            size_mb,
            chunk_kb
        );

        int rc =
            run_shm_ring_slot_optimizer(
                total_bytes,
                chunk_size,
                (size_t)trials
            );

        record_manifest(
            argc,
            argv,
            "shm-ring-slot-optimization",
            result_files,
            rc == 0 ? 0 : 2
        );

        if (rc != 0) {
            return 2;
        }

        return 0;
    }


    /* =====================================================
       STANDARD IPC COMMANDS
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

        unsigned long long size_mb =
            0;

        unsigned long long chunk_kb =
            0;

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

        BenchmarkResult result =
            {0};

        int rc =
            -1;

        const char *display_name =
            NULL;


        if (
            strcmp(
                cmd,
                "pipe"
            ) == 0
        ) {
            display_name =
                "PIPE";

            rc =
                run_pipe_benchmark(
                    total_bytes,
                    chunk_size,
                    &result
                );
        }
        else if (
            strcmp(
                cmd,
                "fifo"
            ) == 0
        ) {
            display_name =
                "FIFO";

            rc =
                run_fifo_benchmark(
                    total_bytes,
                    chunk_size,
                    &result
                );
        }
        else if (
            strcmp(
                cmd,
                "socket"
            ) == 0
        ) {
            display_name =
                "SOCKET";

            rc =
                run_socket_benchmark(
                    total_bytes,
                    chunk_size,
                    &result
                );
        }
        else if (
            strcmp(
                cmd,
                "shm"
            ) == 0
        ) {
            display_name =
                "SHM";

            rc =
                run_shm_benchmark(
                    total_bytes,
                    chunk_size,
                    &result
                );
        }
        else if (
            strcmp(
                cmd,
                "shm-opt"
            ) == 0
        ) {
            display_name =
                "SHM-OPT";

            rc =
                run_shm_ring_benchmark(
                    total_bytes,
                    chunk_size,
                    &result
                );
        }


        if (
            rc == 0 &&
            display_name != NULL
        ) {
            print_result(
                display_name,
                total_bytes,
                &result
            );
        }


        record_manifest(
            argc,
            argv,
            "basic-ipc-benchmark",
            "Console benchmark output only",
            rc == 0 ? 0 : 2
        );


        if (rc != 0) {
            return 2;
        }


        return 0;
    }


    /* =====================================================
       UNKNOWN COMMAND
       ===================================================== */

    fprintf(
        stderr,
        "Unknown FastIPC-X command: %s\n",
        cmd
    );

    usage(
        argv[0]
    );

    return 1;
}
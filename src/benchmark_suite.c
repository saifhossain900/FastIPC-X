#include "../include/benchmark_suite.h"
#include "../include/pipe_ipc.h"
#include "../include/fifo_ipc.h"
#include "../include/socket_ipc.h"
#include "../include/shm_ipc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/resource.h>
#include <sys/types.h>
#include <errno.h>
#include <unistd.h>

/* Helper: compare doubles for qsort */
static int cmp_double(const void *a, const void *b) {
    double da = *(const double *)a;
    double db = *(const double *)b;
    if (da < db) return -1;
    if (da > db) return 1;
    return 0;
}

/* Run one method function pointer type */
typedef int (*ipc_runner)(size_t total_bytes, size_t chunk_size, BenchmarkResult *result);

/* Helper: perform trials for given method name and runner */
static int perform_trials(const char *method, ipc_runner runner,
                          size_t payload_bytes, size_t trials, size_t chunk_bytes,
                          BenchmarkAggregate *agg, BenchmarkTrial *trials_out) {
    size_t i;
    double *elapsed = malloc(sizeof(double) * trials);
    double *through = malloc(sizeof(double) * trials);
    if (!elapsed || !through) {
        free(elapsed); free(through);
        return -1;
    }

    for (i = 0; i < trials; ++i) {
        BenchmarkResult res = {0};

        /*
         * Capture resource usage snapshots BEFORE the trial. We must
         * difference RUSAGE_CHILDREN because it is cumulative across all
         * waited-for children; taking before/after snapshots yields the
         * per-trial delta.
         */
        struct rusage self_before, children_before, self_after, children_after;
        if (getrusage(RUSAGE_SELF, &self_before) != 0) {
            perror("getrusage self before");
            free(elapsed); free(through);
            return -1;
        }
        if (getrusage(RUSAGE_CHILDREN, &children_before) != 0) {
            perror("getrusage children before");
            free(elapsed); free(through);
            return -1;
        }

        int rc = runner(payload_bytes, chunk_bytes, &res);
        if (rc != 0) {
            fprintf(stderr, "%s trial %zu failed\n", method, i);
            free(elapsed); free(through);
            return -1;
        }

        if (getrusage(RUSAGE_SELF, &self_after) != 0) {
            perror("getrusage self after");
            free(elapsed); free(through);
            return -1;
        }
        if (getrusage(RUSAGE_CHILDREN, &children_after) != 0) {
            perror("getrusage children after");
            free(elapsed); free(through);
            return -1;
        }

        /* compute deltas (convert timeval to milliseconds) */
        double self_user_ms = (double)self_after.ru_utime.tv_sec * 1000.0 + (double)self_after.ru_utime.tv_usec / 1000.0;
        double self_sys_ms = (double)self_after.ru_stime.tv_sec * 1000.0 + (double)self_after.ru_stime.tv_usec / 1000.0;
        double child_user_ms = (double)children_after.ru_utime.tv_sec * 1000.0 + (double)children_after.ru_utime.tv_usec / 1000.0;
        double child_sys_ms = (double)children_after.ru_stime.tv_sec * 1000.0 + (double)children_after.ru_stime.tv_usec / 1000.0;

        double self_user_ms_before = (double)self_before.ru_utime.tv_sec * 1000.0 + (double)self_before.ru_utime.tv_usec / 1000.0;
        double self_sys_ms_before = (double)self_before.ru_stime.tv_sec * 1000.0 + (double)self_before.ru_stime.tv_usec / 1000.0;
        double child_user_ms_before = (double)children_before.ru_utime.tv_sec * 1000.0 + (double)children_before.ru_utime.tv_usec / 1000.0;
        double child_sys_ms_before = (double)children_before.ru_stime.tv_sec * 1000.0 + (double)children_before.ru_stime.tv_usec / 1000.0;

        double user_ms = (self_user_ms - self_user_ms_before) + (child_user_ms - child_user_ms_before);
        double sys_ms = (self_sys_ms - self_sys_ms_before) + (child_sys_ms - child_sys_ms_before);

        long vol_before = self_before.ru_nvcsw + children_before.ru_nvcsw;
        long invol_before = self_before.ru_nivcsw + children_before.ru_nivcsw;
        long vol_after = self_after.ru_nvcsw + children_after.ru_nvcsw;
        long invol_after = self_after.ru_nivcsw + children_after.ru_nivcsw;

        long vol_delta = vol_after - vol_before;
        long invol_delta = invol_after - invol_before;

        trials_out[i].elapsed_ms = res.elapsed_ms;
        trials_out[i].throughput_mbps = res.throughput_mbps;
        trials_out[i].user_ms = user_ms;
        trials_out[i].sys_ms = sys_ms;
        trials_out[i].voluntary_ctx_switches = vol_delta;
        trials_out[i].involuntary_ctx_switches = invol_delta;

        elapsed[i] = res.elapsed_ms;
        through[i] = res.throughput_mbps;
    }

    /* compute stats */
    qsort(elapsed, trials, sizeof(double), cmp_double);
    qsort(through, trials, sizeof(double), cmp_double);

    double sum_ms = 0.0, sum_th = 0.0, sum_user = 0.0, sum_sys = 0.0;
    double sum_vol = 0.0, sum_invol = 0.0;
    for (size_t j = 0; j < trials; ++j) {
        sum_ms += elapsed[j];
        sum_th += through[j];
        sum_user += trials_out[j].user_ms;
        sum_sys += trials_out[j].sys_ms;
        sum_vol += (double)trials_out[j].voluntary_ctx_switches;
        sum_invol += (double)trials_out[j].involuntary_ctx_switches;
    }

    agg->method = method;
    agg->payload_bytes = payload_bytes;
    agg->chunk_bytes = chunk_bytes;
    agg->trials = trials;
    agg->min_ms = elapsed[0];
    agg->max_ms = elapsed[trials - 1];
    agg->mean_ms = sum_ms / (double)trials;
    if (trials % 2 == 1) agg->median_ms = elapsed[trials / 2];
    else agg->median_ms = (elapsed[trials/2 - 1] + elapsed[trials/2]) / 2.0;

    agg->mean_throughput_mbps = sum_th / (double)trials;
    if (trials % 2 == 1) agg->median_throughput_mbps = through[trials/2];
    else agg->median_throughput_mbps = (through[trials/2 -1] + through[trials/2]) / 2.0;

    agg->mean_user_ms = sum_user / (double)trials;
    agg->mean_sys_ms = sum_sys / (double)trials;
    agg->mean_voluntary_ctx_switches = sum_vol / (double)trials;
    agg->mean_involuntary_ctx_switches = sum_invol / (double)trials;

    free(elapsed);
    free(through);
    return 0;
}

int run_benchmark_suite(size_t payload_bytes, size_t trials, size_t chunk_bytes, const char *out_csv) {
    const char *methods[] = {"PIPE", "FIFO", "SOCKET", "SHM"};
    ipc_runner runners[] = {run_pipe_benchmark, run_fifo_benchmark, run_socket_benchmark, run_shm_benchmark};
    const size_t method_count = sizeof(methods) / sizeof(methods[0]);

    if (trials == 0) {
        fprintf(stderr, "trials must be > 0\n");
        return -1;
    }

    BenchmarkAggregate aggs[4];
    BenchmarkTrial *trial_buffers[4];
    for (size_t m = 0; m < method_count; ++m) {
        trial_buffers[m] = calloc(trials, sizeof(BenchmarkTrial));
        if (!trial_buffers[m]) {
            fprintf(stderr, "alloc failure\n");
            for (size_t k = 0; k < m; ++k) free(trial_buffers[k]);
            return -1;
        }

        if (perform_trials(methods[m], runners[m], payload_bytes, trials, chunk_bytes, &aggs[m], trial_buffers[m]) != 0) {
            for (size_t k = 0; k < method_count; ++k) free(trial_buffers[k]);
            return -1;
        }
    }

    /* Ensure results directory exists */
    if (mkdir("results", 0755) != 0) {
        if (errno != EEXIST) {
            perror("mkdir results");
            for (size_t k = 0; k < method_count; ++k) free(trial_buffers[k]);
            return -1;
        }
    }

    FILE *f = fopen(out_csv, "w");
    if (!f) {
        perror("fopen csv");
        for (size_t k = 0; k < method_count; ++k) free(trial_buffers[k]);
        return -1;
    }

    fprintf(f, "method,payload_mb,chunk_kb,trials,min_ms,max_ms,mean_ms,median_ms,mean_throughput_mbps,median_throughput_mbps,mean_user_cpu_ms,mean_system_cpu_ms,mean_voluntary_ctx_switches,mean_involuntary_ctx_switches\n");
    for (size_t m = 0; m < method_count; ++m) {
        fprintf(f, "%s,%zu,%zu,%zu,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.2f,%.2f\n",
                aggs[m].method,
                aggs[m].payload_bytes / (1024*1024),
                aggs[m].chunk_bytes / 1024,
                aggs[m].trials,
                aggs[m].min_ms,
                aggs[m].max_ms,
                aggs[m].mean_ms,
                aggs[m].median_ms,
                aggs[m].mean_throughput_mbps,
                aggs[m].median_throughput_mbps,
                aggs[m].mean_user_ms,
                aggs[m].mean_sys_ms,
                aggs[m].mean_voluntary_ctx_switches,
                aggs[m].mean_involuntary_ctx_switches);
    }
    fclose(f);

    /* Print terminal summary table */
    printf("\n=== BENCHMARK SUMMARY (payload %zu MB, chunk %zu KB, trials %zu) ===\n", payload_bytes / (1024*1024), chunk_bytes/1024, trials);
    printf("Method      Median ms    Median MB/s    User ms    Sys ms    Vol CS\n");
    for (size_t m = 0; m < method_count; ++m) {
        printf("%-11s %10.3f %13.3f %9.3f %8.3f %7.0f\n",
               aggs[m].method,
               aggs[m].median_ms,
               aggs[m].median_throughput_mbps,
               aggs[m].mean_user_ms,
               aggs[m].mean_sys_ms,
               aggs[m].mean_voluntary_ctx_switches);
    }

    /* Determine fastest by median_ms */
    size_t best_idx = 0;
    for (size_t m = 1; m < method_count; ++m) {
        if (aggs[m].median_ms < aggs[best_idx].median_ms) best_idx = m;
    }
    printf("\nFastest measured method for this benchmark: %s\n", aggs[best_idx].method);

    for (size_t k = 0; k < method_count; ++k) free(trial_buffers[k]);
    return 0;
}

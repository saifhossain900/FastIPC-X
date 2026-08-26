#include "../include/shm_optimizer.h"
#include "../include/shm_ipc.h"
#include "../include/shm_ring_ipc.h"
#include "../include/benchmark_suite.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/resource.h>

static int cmp_double(const void *a, const void *b) {
    double da = *(const double *)a;
    double db = *(const double *)b;
    if (da < db) return -1;
    if (da > db) return 1;
    return 0;
}

int run_shm_optimization(size_t payload_bytes, size_t chunk_size, size_t trials) {
    /* Run baseline (shm) and optimized (shm-opt) each trials times and aggregate using existing suite code. */
    char base_csv[256];
    char opt_csv[256];
    size_t payload_mb = payload_bytes / (1024*1024);
    snprintf(base_csv, sizeof(base_csv), "results/shm_baseline_%zuMB_%zuKB.csv", payload_mb, chunk_size/1024);
    snprintf(opt_csv, sizeof(opt_csv), "results/shm_opt_%zuMB_%zuKB.csv", payload_mb, chunk_size/1024);

    /* reuse run_benchmark_suite but it benchmarks all methods; instead call each runner directly multiple times
     * and compute medians. Simpler: call run_shm_benchmark and run_shm_ring_benchmark trials times and collect results.
     */
    double *base_ms = calloc(trials, sizeof(double));
    double *base_th = calloc(trials, sizeof(double));
    double *base_user = calloc(trials, sizeof(double));
    double *base_sys = calloc(trials, sizeof(double));
    double *opt_ms = calloc(trials, sizeof(double));
    double *opt_th = calloc(trials, sizeof(double));
    double *opt_user = calloc(trials, sizeof(double));
    double *opt_sys = calloc(trials, sizeof(double));
    long *base_vol = calloc(trials, sizeof(long));
    long *base_invol = calloc(trials, sizeof(long));
    long *opt_vol = calloc(trials, sizeof(long));
    long *opt_invol = calloc(trials, sizeof(long));
    if (!base_ms || !opt_ms) { perror("alloc"); return -1; }

    for (size_t i = 0; i < trials; ++i) {
        BenchmarkResult r = {0};
        /* capture rusage before baseline trial */
        struct rusage self_before, children_before, self_after, children_after;
        if (getrusage(RUSAGE_SELF, &self_before) != 0) { perror("getrusage self before"); return -1; }
        if (getrusage(RUSAGE_CHILDREN, &children_before) != 0) { perror("getrusage children before"); return -1; }

        if (run_shm_benchmark(payload_bytes, chunk_size, &r) != 0) { fprintf(stderr, "baseline shm trial failed\n"); return -1; }

        if (getrusage(RUSAGE_SELF, &self_after) != 0) { perror("getrusage self after"); return -1; }
        if (getrusage(RUSAGE_CHILDREN, &children_after) != 0) { perror("getrusage children after"); return -1; }

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

        base_ms[i] = r.elapsed_ms;
        base_th[i] = r.throughput_mbps;
        base_user[i] = user_ms;
        base_sys[i] = sys_ms;
        base_vol[i] = vol_delta;
        base_invol[i] = invol_delta;

        /* now optimized trial with its own snapshots */
        if (getrusage(RUSAGE_SELF, &self_before) != 0) { perror("getrusage self before"); return -1; }
        if (getrusage(RUSAGE_CHILDREN, &children_before) != 0) { perror("getrusage children before"); return -1; }

        if (run_shm_ring_benchmark(payload_bytes, chunk_size, &r) != 0) { fprintf(stderr, "shm-opt trial failed\n"); return -1; }

        if (getrusage(RUSAGE_SELF, &self_after) != 0) { perror("getrusage self after"); return -1; }
        if (getrusage(RUSAGE_CHILDREN, &children_after) != 0) { perror("getrusage children after"); return -1; }

        self_user_ms = (double)self_after.ru_utime.tv_sec * 1000.0 + (double)self_after.ru_utime.tv_usec / 1000.0;
        self_sys_ms = (double)self_after.ru_stime.tv_sec * 1000.0 + (double)self_after.ru_stime.tv_usec / 1000.0;
        child_user_ms = (double)children_after.ru_utime.tv_sec * 1000.0 + (double)children_after.ru_utime.tv_usec / 1000.0;
        child_sys_ms = (double)children_after.ru_stime.tv_sec * 1000.0 + (double)children_after.ru_stime.tv_usec / 1000.0;

        self_user_ms_before = (double)self_before.ru_utime.tv_sec * 1000.0 + (double)self_before.ru_utime.tv_usec / 1000.0;
        self_sys_ms_before = (double)self_before.ru_stime.tv_sec * 1000.0 + (double)self_before.ru_stime.tv_usec / 1000.0;
        child_user_ms_before = (double)children_before.ru_utime.tv_sec * 1000.0 + (double)children_before.ru_utime.tv_usec / 1000.0;
        child_sys_ms_before = (double)children_before.ru_stime.tv_sec * 1000.0 + (double)children_before.ru_stime.tv_usec / 1000.0;

        user_ms = (self_user_ms - self_user_ms_before) + (child_user_ms - child_user_ms_before);
        sys_ms = (self_sys_ms - self_sys_ms_before) + (child_sys_ms - child_sys_ms_before);

        vol_before = self_before.ru_nvcsw + children_before.ru_nvcsw;
        invol_before = self_before.ru_nivcsw + children_before.ru_nivcsw;
        vol_after = self_after.ru_nvcsw + children_after.ru_nvcsw;
        invol_after = self_after.ru_nivcsw + children_after.ru_nivcsw;

        vol_delta = vol_after - vol_before;
        invol_delta = invol_after - invol_before;

        opt_ms[i] = r.elapsed_ms;
        opt_th[i] = r.throughput_mbps;
        opt_user[i] = user_ms;
        opt_sys[i] = sys_ms;
        opt_vol[i] = vol_delta;
        opt_invol[i] = invol_delta;
    }

    /* sort and compute medians */
    qsort(base_ms, trials, sizeof(double), cmp_double);
    qsort(opt_ms, trials, sizeof(double), cmp_double);

    double base_median_ms = (trials % 2) ? base_ms[trials/2] : (base_ms[trials/2 -1] + base_ms[trials/2]) / 2.0;
    double opt_median_ms = (trials % 2) ? opt_ms[trials/2] : (opt_ms[trials/2 -1] + opt_ms[trials/2]) / 2.0;

    /* compute mean throughput etc */
    double base_mean_th = 0, opt_mean_th = 0, base_mean_user = 0, base_mean_sys = 0, opt_mean_user = 0, opt_mean_sys = 0;
    double base_mean_vol = 0, opt_mean_vol = 0;
    for (size_t i = 0; i < trials; ++i) {
        base_mean_th += base_th[i]; opt_mean_th += opt_th[i];
        base_mean_user += base_user[i]; base_mean_sys += base_sys[i];
        opt_mean_user += opt_user[i]; opt_mean_sys += opt_sys[i];
        base_mean_vol += base_vol[i]; opt_mean_vol += opt_vol[i];
    }
    base_mean_th /= (double)trials; opt_mean_th /= (double)trials;
    base_mean_user /= (double)trials; base_mean_sys /= (double)trials;
    opt_mean_user /= (double)trials; opt_mean_sys /= (double)trials;
    base_mean_vol /= (double)trials; opt_mean_vol /= (double)trials;

    /* write CSV */
    if (mkdir("results", 0755) != 0 && errno != EEXIST) { perror("mkdir results"); }
    FILE *f = fopen(base_csv, "w");
    if (f) {
        fprintf(f, "trial,elapsed_ms,throughput_mbps,user_ms,sys_ms,vol_cs,invol_cs\n");
        for (size_t i = 0; i < trials; ++i) fprintf(f, "%zu,%.6f,%.6f,%.6f,%.6f,%.0f,%.0f\n", i, base_ms[i], base_th[i], base_user[i], base_sys[i], (double)base_vol[i], (double)base_invol[i]);
        fclose(f);
    }
    FILE *g = fopen(opt_csv, "w");
    if (g) {
        fprintf(g, "trial,elapsed_ms,throughput_mbps,user_ms,sys_ms,vol_cs,invol_cs\n");
        for (size_t i = 0; i < trials; ++i) fprintf(g, "%zu,%.6f,%.6f,%.6f,%.6f,%.0f,%.0f\n", i, opt_ms[i], opt_th[i], opt_user[i], opt_sys[i], (double)opt_vol[i], (double)opt_invol[i]);
        fclose(g);
    }

    /* write combined summary CSV with baseline and optimized rows */
    char combined_csv[256];
    snprintf(combined_csv, sizeof(combined_csv), "results/shm_sync_optimization_%zuMB_%zuKB.csv", payload_mb, chunk_size/1024);
    FILE *c = fopen(combined_csv, "w");
    if (c) {
        fprintf(c, "variant,payload_mb,chunk_kb,median_ms,median_throughput_mbps,mean_user_cpu_ms,mean_system_cpu_ms,mean_voluntary_ctx_switches,mean_involuntary_ctx_switches\n");
        fprintf(c, "baseline,%zu,%zu,%.6f,%.6f,%.6f,%.6f,%.2f,%.2f\n", payload_mb, chunk_size/1024, base_median_ms, base_mean_th, base_mean_user, base_mean_sys, base_mean_vol, 0.0);
        fprintf(c, "ringbuffer,%zu,%zu,%.6f,%.6f,%.6f,%.6f,%.2f,%.2f\n", payload_mb, chunk_size/1024, opt_median_ms, opt_mean_th, opt_mean_user, opt_mean_sys, opt_mean_vol, 0.0);
        fclose(c);
    }

    printf("\nSHM SYNCHRONIZATION OPTIMIZATION\n\n");
    printf("Metric              Baseline       Ring Buffer      Change\n");
    printf("Median time         %12.3f %12.3f %8.2f%%\n", base_median_ms, opt_median_ms, ((base_median_ms - opt_median_ms)/base_median_ms)*100.0);
    printf("Median throughput   %12.3f %12.3f %8.2f%%\n", base_mean_th, opt_mean_th, ((opt_mean_th - base_mean_th)/base_mean_th)*100.0);
    printf("System CPU (ms)     %12.3f %12.3f\n", base_mean_sys, opt_mean_sys);
    printf("Voluntary CS        %12.2f %12.2f\n", base_mean_vol, opt_mean_vol);

    free(base_ms); free(opt_ms); free(base_th); free(opt_th);
    free(base_user); free(base_sys); free(opt_user); free(opt_sys);
    free(base_vol); free(opt_vol); free(base_invol); free(opt_invol);
    return 0;
}

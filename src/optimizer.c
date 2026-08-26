#include "../include/optimizer.h"
#include "../include/benchmark_suite.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>

/*
 * Chunk optimizer: runs the benchmark suite across a set of chunk sizes by
 * invoking run_benchmark_suite() (which already performs and aggregates
 * trials for all IPC methods). It parses the CSV produced for each chunk
 * size and collects per-method metrics, then writes a combined CSV and a
 * summary CSV and prints per-method tables to the terminal.
 */

static const size_t chunks_kb[] = {1, 4, 16, 64, 256, 1024};
static const size_t nchunks = sizeof(chunks_kb) / sizeof(chunks_kb[0]);
static const char *methods[] = {"PIPE","FIFO","SOCKET","SHM"};
static const size_t nmethods = 4;

typedef struct {
    char method[32];
    size_t payload_mb;
    size_t chunk_kb;
    size_t trials;
    double min_ms, max_ms, mean_ms, median_ms;
    double mean_throughput_mbps, median_throughput_mbps;
    double mean_user_ms, mean_sys_ms;
    double mean_voluntary_ctx_switches, mean_involuntary_ctx_switches;
    int is_best;
} OptRow;

static int parse_row_csv(const char *line, OptRow *row) {
    /* CSV format from benchmark_suite: 14 columns */
    char *dup = strdup(line);
    if (!dup) return -1;
    char *saveptr = NULL;
    char *p = dup;
    char *cols[16];
    size_t c = 0;
    char *token = strtok_r(p, ",", &saveptr);
    while (token && c < 16) {
        cols[c++] = token;
        token = strtok_r(NULL, ",", &saveptr);
    }
    if (c < 14) { free(dup); return -1; }

    strncpy(row->method, cols[0], sizeof(row->method)-1);
    row->payload_mb = (size_t)strtoul(cols[1], NULL, 10);
    row->chunk_kb = (size_t)strtoul(cols[2], NULL, 10);
    row->trials = (size_t)strtoul(cols[3], NULL, 10);
    row->min_ms = strtod(cols[4], NULL);
    row->max_ms = strtod(cols[5], NULL);
    row->mean_ms = strtod(cols[6], NULL);
    row->median_ms = strtod(cols[7], NULL);
    row->mean_throughput_mbps = strtod(cols[8], NULL);
    row->median_throughput_mbps = strtod(cols[9], NULL);
    row->mean_user_ms = strtod(cols[10], NULL);
    row->mean_sys_ms = strtod(cols[11], NULL);
    row->mean_voluntary_ctx_switches = strtod(cols[12], NULL);
    row->mean_involuntary_ctx_switches = strtod(cols[13], NULL);
    row->is_best = 0;
    free(dup);
    return 0;
}

int run_chunk_optimizer(size_t payload_bytes, size_t trials) {
    size_t payload_mb = payload_bytes / (1024*1024);
    OptRow *rows = calloc(nmethods * nchunks, sizeof(OptRow));
    if (!rows) return -1;

    /* For each chunk, run the benchmark suite which produces a CSV in results/ */
    for (size_t ci = 0; ci < nchunks; ++ci) {
        size_t chunk_kb = chunks_kb[ci];
        size_t chunk_bytes = chunk_kb * 1024;
        char tmp_csv[256];
        snprintf(tmp_csv, sizeof(tmp_csv), "results/tmp_chunk_%zuKB.csv", chunk_kb);

        /* run benchmark suite for this chunk size; it writes tmp_csv */
        if (run_benchmark_suite(payload_bytes, trials, chunk_bytes, tmp_csv) != 0) {
            fprintf(stderr, "benchmark suite failed for chunk %zu KB\n", chunk_kb);
            free(rows);
            return -1;
        }

        /* parse CSV */
        FILE *f = fopen(tmp_csv, "r");
        if (!f) {
            perror("fopen tmp_csv");
            free(rows);
            return -1;
        }

        char line[1024];
        /* skip header */
        if (!fgets(line, sizeof(line), f)) { fclose(f); free(rows); return -1; }
        size_t row_idx = ci * nmethods;
        for (size_t m = 0; m < nmethods; ++m) {
            if (!fgets(line, sizeof(line), f)) break;
            OptRow r = {0};
            if (parse_row_csv(line, &r) != 0) {
                fprintf(stderr, "failed to parse tmp csv line\n");
                fclose(f); free(rows); return -1;
            }
            rows[row_idx + m] = r;
        }
        fclose(f);
        /* remove tmp file */
        unlink(tmp_csv);
    }

    /* For each method, find best median_ms (primary metric) and mark rows */
    for (size_t m = 0; m < nmethods; ++m) {
        double best_median = -1.0;
        size_t best_ci = 0;
        for (size_t ci = 0; ci < nchunks; ++ci) {
            OptRow *r = &rows[ci * nmethods + m];
            if (best_median < 0.0 || r->median_ms < best_median) {
                best_median = r->median_ms;
                best_ci = ci;
            }
        }
        rows[best_ci * nmethods + m].is_best = 1;
    }

    /* Write detailed CSV */
    char out_csv[256];
    snprintf(out_csv, sizeof(out_csv), "results/chunk_optimization_%zuMB.csv", payload_mb);
    FILE *out = fopen(out_csv, "w");
    if (!out) { perror("fopen out_csv"); free(rows); return -1; }
    fprintf(out, "method,payload_mb,chunk_kb,trials,min_ms,max_ms,mean_ms,median_ms,mean_throughput_mbps,median_throughput_mbps,mean_user_cpu_ms,mean_system_cpu_ms,mean_voluntary_ctx_switches,mean_involuntary_ctx_switches,is_best_for_method\n");
    for (size_t ci = 0; ci < nchunks; ++ci) {
        for (size_t m = 0; m < nmethods; ++m) {
            OptRow *r = &rows[ci * nmethods + m];
            fprintf(out, "%s,%zu,%zu,%zu,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.2f,%.2f,%d\n",
                    r->method, r->payload_mb, r->chunk_kb, r->trials,
                    r->min_ms, r->max_ms, r->mean_ms, r->median_ms,
                    r->mean_throughput_mbps, r->median_throughput_mbps,
                    r->mean_user_ms, r->mean_sys_ms,
                    r->mean_voluntary_ctx_switches, r->mean_involuntary_ctx_switches,
                    r->is_best);
        }
    }
    fclose(out);

    /* Write summary CSV */
    char sum_csv[256];
    snprintf(sum_csv, sizeof(sum_csv), "results/chunk_optimization_summary_%zuMB.csv", payload_mb);
    FILE *sout = fopen(sum_csv, "w");
    if (!sout) { perror("fopen sum_csv"); free(rows); return -1; }
    fprintf(sout, "method,baseline_chunk_kb,best_chunk_kb,baseline_median_ms,best_median_ms,latency_improvement_percent,baseline_median_throughput_mbps,best_median_throughput_mbps,throughput_improvement_percent\n");

    for (size_t m = 0; m < nmethods; ++m) {
        OptRow *baseline = NULL;
        OptRow *best = NULL;
        for (size_t ci = 0; ci < nchunks; ++ci) {
            OptRow *r = &rows[ci * nmethods + m];
            if (r->chunk_kb == 64) baseline = r;
            if (r->is_best) best = r;
        }
        if (!baseline || !best) { fprintf(stderr, "missing baseline/best data\n"); continue; }

        double improvement = 0.0;
        if (best->median_ms < baseline->median_ms) {
            improvement = ((baseline->median_ms - best->median_ms) / baseline->median_ms) * 100.0;
        }
        double th_improv = 0.0;
        if (best->median_throughput_mbps > baseline->median_throughput_mbps) {
            th_improv = ((best->median_throughput_mbps - baseline->median_throughput_mbps) / baseline->median_throughput_mbps) * 100.0;
        }

        fprintf(sout, "%s,%zu,%zu,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f\n",
                methods[m], baseline->chunk_kb, best->chunk_kb,
                baseline->median_ms, best->median_ms, improvement,
                baseline->median_throughput_mbps, best->median_throughput_mbps, th_improv);
    }
    fclose(sout);

    /* Print per-method tables to terminal */
    for (size_t m = 0; m < nmethods; ++m) {
        printf("\n%s\n", methods[m]);
        printf("Chunk KB   Median ms   Median MB/s   Sys CPU ms   Vol CS\n");
        for (size_t ci = 0; ci < nchunks; ++ci) {
            OptRow *r = &rows[ci * nmethods + m];
            printf("%4zu %12.3f %13.3f %12.3f %8.0f %s\n",
                   r->chunk_kb, r->median_ms, r->median_throughput_mbps, r->mean_sys_ms, r->mean_voluntary_ctx_switches,
                   r->is_best ? "<- best" : "");
        }
        /* find best and baseline for message */
        OptRow *baseline = NULL; OptRow *best = NULL;
        for (size_t ci = 0; ci < nchunks; ++ci) {
            OptRow *r = &rows[ci * nmethods + m];
            if (r->chunk_kb == 64) baseline = r;
            if (r->is_best) best = r;
        }
        if (baseline && best) {
            if (best->median_ms < baseline->median_ms)
                printf("Best measured %s chunk size: %zu KB (%.3f ms vs baseline %.3f ms)\n", methods[m], best->chunk_kb, best->median_ms, baseline->median_ms);
            else
                printf("No measurable improvement over the 64 KB baseline for %s.\n", methods[m]);
        }
    }

    free(rows);
    return 0;
}

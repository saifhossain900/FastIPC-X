#include "../include/adaptive_selector.h"
#include "../include/benchmark_suite.h"
#include "../include/pipe_ipc.h"
#include "../include/fifo_ipc.h"
#include "../include/socket_ipc.h"
#include "../include/shm_ring_ipc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>


typedef struct {
    char method[64];
    size_t chunk_kb;

    double median_ms;
    double median_throughput_mbps;

    double system_cpu_ms;
    double voluntary_cs;

    int selected;
} AdaptiveRow;


/* ---------------------------------------------------------
   Helpers
   --------------------------------------------------------- */

static int file_exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0;
}


/*
 * Load real system CPU and voluntary context-switch metrics
 * from:
 *
 * results/chunk_optimization_<payload>MB.csv
 *
 * CSV format:
 *
 * method,
 * payload_mb,
 * chunk_kb,
 * trials,
 * min_ms,
 * max_ms,
 * mean_ms,
 * median_ms,
 * mean_throughput_mbps,
 * median_throughput_mbps,
 * mean_user_cpu_ms,
 * mean_system_cpu_ms,
 * mean_voluntary_ctx_switches,
 * mean_involuntary_ctx_switches,
 * is_best_for_method
 */
static int load_chunk_metrics(
    size_t payload_mb,
    const char *method,
    size_t chunk_kb,
    double *out_sys_cpu,
    double *out_vol_cs
)
{
    char path[256];

    snprintf(
        path,
        sizeof(path),
        "results/chunk_optimization_%zuMB.csv",
        payload_mb
    );

    if (!file_exists(path)) {
        return -1;
    }

    FILE *f = fopen(path, "r");

    if (!f) {
        return -1;
    }

    char line[1024];

    /* Skip header */
    if (!fgets(line, sizeof(line), f)) {
        fclose(f);
        return -1;
    }

    while (fgets(line, sizeof(line), f)) {

        char csv_method[32];

        size_t csv_payload = 0;
        size_t csv_chunk = 0;

        int csv_trials = 0;

        double min_ms = 0.0;
        double max_ms = 0.0;
        double mean_ms = 0.0;
        double median_ms = 0.0;

        double mean_throughput = 0.0;
        double median_throughput = 0.0;

        double mean_user_cpu = 0.0;
        double mean_system_cpu = 0.0;

        double mean_voluntary_cs = 0.0;
        double mean_involuntary_cs = 0.0;

        int is_best = 0;

        int scanned = sscanf(
            line,
            "%31[^,],%zu,%zu,%d,"
            "%lf,%lf,%lf,%lf,"
            "%lf,%lf,"
            "%lf,%lf,"
            "%lf,%lf,%d",
            csv_method,
            &csv_payload,
            &csv_chunk,
            &csv_trials,
            &min_ms,
            &max_ms,
            &mean_ms,
            &median_ms,
            &mean_throughput,
            &median_throughput,
            &mean_user_cpu,
            &mean_system_cpu,
            &mean_voluntary_cs,
            &mean_involuntary_cs,
            &is_best
        );

        if (scanned != 15) {
            continue;
        }

        if (
            csv_payload == payload_mb &&
            csv_chunk == chunk_kb &&
            strcmp(csv_method, method) == 0
        ) {
            *out_sys_cpu = mean_system_cpu;
            *out_vol_cs = mean_voluntary_cs;

            fclose(f);
            return 0;
        }
    }

    fclose(f);
    return -1;
}


/* ---------------------------------------------------------
   Load chunk optimization summary
   --------------------------------------------------------- */

static int load_chunk_summary(
    size_t payload_mb,
    AdaptiveRow rows[],
    size_t *count
)
{
    char path[256];

    snprintf(
        path,
        sizeof(path),
        "results/chunk_optimization_summary_%zuMB.csv",
        payload_mb
    );

    if (!file_exists(path)) {
        return -1;
    }

    FILE *f = fopen(path, "r");

    if (!f) {
        return -1;
    }

    char line[512];

    /* Skip header */
    if (!fgets(line, sizeof(line), f)) {
        fclose(f);
        return -1;
    }

    size_t idx = 0;

    while (fgets(line, sizeof(line), f)) {

        if (idx >= 4) {
            break;
        }

        char method[32];

        size_t baseline_chunk = 0;
        size_t best_chunk = 0;

        double baseline_median = 0.0;
        double best_median = 0.0;

        double baseline_throughput = 0.0;
        double best_throughput = 0.0;

        /*
         * Summary CSV:
         *
         * method,
         * baseline_chunk_kb,
         * best_chunk_kb,
         * baseline_median_ms,
         * best_median_ms,
         * latency_improvement_percent,
         * baseline_median_throughput_mbps,
         * best_median_throughput_mbps,
         * throughput_improvement_percent
         */
        int scanned = sscanf(
            line,
            "%31[^,],%zu,%zu,%lf,%lf,%*f,%lf,%lf,%*f",
            method,
            &baseline_chunk,
            &best_chunk,
            &baseline_median,
            &best_median,
            &baseline_throughput,
            &best_throughput
        );

        if (scanned != 7) {
            continue;
        }

        AdaptiveRow r = {0};

        strncpy(
            r.method,
            method,
            sizeof(r.method) - 1
        );

        r.method[sizeof(r.method) - 1] = '\0';

        r.chunk_kb = best_chunk;
        r.median_ms = best_median;
        r.median_throughput_mbps = best_throughput;

        /*
         * Get real supporting metrics from the detailed
         * chunk optimization CSV.
         */
        double sys_cpu = 0.0;
        double vol_cs = 0.0;

        if (
            load_chunk_metrics(
                payload_mb,
                method,
                best_chunk,
                &sys_cpu,
                &vol_cs
            ) == 0
        ) {
            r.system_cpu_ms = sys_cpu;
            r.voluntary_cs = vol_cs;
        }
        else {
            /*
             * Do not invent measurements.
             * 0 means detailed metrics could not be loaded.
             */
            r.system_cpu_ms = 0.0;
            r.voluntary_cs = 0.0;
        }

        r.selected = 0;

        rows[idx++] = r;
    }

    fclose(f);

    *count = idx;

    return (idx > 0) ? 0 : -1;
}


/* ---------------------------------------------------------
   Load SHM Ring Buffer optimization result
   --------------------------------------------------------- */

static int load_shm_ring(
    size_t payload_mb,
    AdaptiveRow *out
)
{
    DIR *d = opendir("results");

    if (!d) {
        return -1;
    }

    struct dirent *ent;

    char prefix[128];

    snprintf(
        prefix,
        sizeof(prefix),
        "shm_sync_optimization_%zuMB_",
        payload_mb
    );

    int found = 0;

    AdaptiveRow best_row = {0};

    while ((ent = readdir(d)) != NULL) {

        if (
            strncmp(
                ent->d_name,
                prefix,
                strlen(prefix)
            ) != 0
        ) {
            continue;
        }

        char path[512];

        snprintf(
            path,
            sizeof(path),
            "results/%s",
            ent->d_name
        );

        FILE *f = fopen(path, "r");

        if (!f) {
            continue;
        }

        char line[512];

        /* Skip header */
        if (!fgets(line, sizeof(line), f)) {
            fclose(f);
            continue;
        }

        while (fgets(line, sizeof(line), f)) {

            if (
                strncmp(
                    line,
                    "ringbuffer,",
                    11
                ) != 0
            ) {
                continue;
            }

            char label[32];

            size_t payload = 0;
            size_t chunk_kb = 0;

            double median_ms = 0.0;
            double median_throughput = 0.0;

            double mean_user_cpu = 0.0;
            double mean_system_cpu = 0.0;

            double mean_voluntary = 0.0;
            double mean_involuntary = 0.0;

            int scanned = sscanf(
                line,
                "%31[^,],%zu,%zu,"
                "%lf,%lf,%lf,%lf,%lf,%lf",
                label,
                &payload,
                &chunk_kb,
                &median_ms,
                &median_throughput,
                &mean_user_cpu,
                &mean_system_cpu,
                &mean_voluntary,
                &mean_involuntary
            );

            if (scanned != 9) {
                continue;
            }

            if (payload != payload_mb) {
                continue;
            }

            AdaptiveRow current = {0};

            strncpy(
                current.method,
                "SHM-RING",
                sizeof(current.method) - 1
            );

            current.method[
                sizeof(current.method) - 1
            ] = '\0';

            current.chunk_kb = chunk_kb;

            current.median_ms = median_ms;

            current.median_throughput_mbps =
                median_throughput;

            current.system_cpu_ms =
                mean_system_cpu;

            current.voluntary_cs =
                mean_voluntary;

            current.selected = 0;

            /*
             * If multiple SHM ring optimization files exist,
             * keep the one with the lowest median latency.
             */
            if (
                !found ||
                current.median_ms < best_row.median_ms
            ) {
                best_row = current;
                found = 1;
            }
        }

        fclose(f);
    }

    closedir(d);

    if (!found) {
        return -1;
    }

    *out = best_row;

    return 0;
}


/* ---------------------------------------------------------
   Adaptive profile CSV
   --------------------------------------------------------- */

static int save_adaptive_csv(
    size_t payload_mb,
    AdaptiveRow rows[],
    size_t n
)
{
    if (
        mkdir("results", 0755) != 0 &&
        errno != EEXIST
    ) {
        return -1;
    }

    char path[256];

    snprintf(
        path,
        sizeof(path),
        "results/adaptive_profile_%zuMB.csv",
        payload_mb
    );

    FILE *f = fopen(path, "w");

    if (!f) {
        return -1;
    }

    fprintf(
        f,
        "method,payload_mb,chunk_kb,"
        "median_ms,median_throughput_mbps,"
        "system_cpu_ms,"
        "voluntary_ctx_switches,"
        "selected\n"
    );

    for (size_t i = 0; i < n; ++i) {

        fprintf(
            f,
            "%s,%zu,%zu,"
            "%.6f,%.6f,"
            "%.6f,%.2f,%d\n",
            rows[i].method,
            payload_mb,
            rows[i].chunk_kb,
            rows[i].median_ms,
            rows[i].median_throughput_mbps,
            rows[i].system_cpu_ms,
            rows[i].voluntary_cs,
            rows[i].selected
        );
    }

    fclose(f);

    return 0;
}


/* ---------------------------------------------------------
   Print adaptive recommendation
   --------------------------------------------------------- */

static void print_recommendation_report(
    size_t payload_mb,
    AdaptiveRow cand[],
    size_t best,
    const char *profile_source
)
{
    printf(
        "FASTIPC-X ADAPTIVE MODE\n"
        "================================================\n\n"
    );

    printf(
        "Payload: %zu MB\n",
        payload_mb
    );

    printf(
        "Profile source: %s\n\n",
        profile_source
    );

    printf(
        "Candidate       Chunk KB    Median ms    Median MB/s\n"
        "----------------------------------------------------\n"
    );

    for (size_t i = 0; i < 4; ++i) {

        printf(
            "%-14s %8zu %12.3f %12.3f\n",
            cand[i].method,
            cand[i].chunk_kb,
            cand[i].median_ms,
            cand[i].median_throughput_mbps
        );
    }

    printf(
        "\nSelected method: %s\n",
        cand[best].method
    );

    printf(
        "Selected chunk: %zu KB\n\n",
        cand[best].chunk_kb
    );

    printf(
        "Reason: Lowest measured median elapsed time "
        "for this workload profile.\n"
    );
}


/* ---------------------------------------------------------
   Load existing adaptive profile
   --------------------------------------------------------- */

static int load_adaptive_profile(
    size_t payload_mb,
    AdaptiveRow cand[],
    size_t *best_idx
)
{
    char profile[256];

    snprintf(
        profile,
        sizeof(profile),
        "results/adaptive_profile_%zuMB.csv",
        payload_mb
    );

    if (!file_exists(profile)) {
        return -1;
    }

    FILE *f = fopen(profile, "r");

    if (!f) {
        return -1;
    }

    char line[512];

    /* Skip header */
    if (!fgets(line, sizeof(line), f)) {
        fclose(f);
        return -1;
    }

    size_t idx = 0;
    size_t selected_count = 0;

    while (
        fgets(line, sizeof(line), f) &&
        idx < 4
    ) {

        char method[32];

        size_t row_payload = 0;
        size_t chunk_kb = 0;

        double median_ms = 0.0;
        double median_throughput = 0.0;

        double system_cpu = 0.0;
        double voluntary_cs = 0.0;

        int selected = 0;

        int scanned = sscanf(
            line,
            "%31[^,],%zu,%zu,"
            "%lf,%lf,%lf,%lf,%d",
            method,
            &row_payload,
            &chunk_kb,
            &median_ms,
            &median_throughput,
            &system_cpu,
            &voluntary_cs,
            &selected
        );

        if (scanned != 8) {
            fclose(f);
            return -1;
        }

        if (row_payload != payload_mb) {
            fclose(f);
            return -1;
        }

        strncpy(
            cand[idx].method,
            method,
            sizeof(cand[idx].method) - 1
        );

        cand[idx].method[
            sizeof(cand[idx].method) - 1
        ] = '\0';

        cand[idx].chunk_kb = chunk_kb;

        cand[idx].median_ms =
            median_ms;

        cand[idx].median_throughput_mbps =
            median_throughput;

        cand[idx].system_cpu_ms =
            system_cpu;

        cand[idx].voluntary_cs =
            voluntary_cs;

        cand[idx].selected =
            selected;

        if (selected == 1) {
            *best_idx = idx;
            selected_count++;
        }

        idx++;
    }

    fclose(f);

    /*
     * Valid profile:
     * - exactly four candidates
     * - exactly one selected winner
     */
    if (
        idx != 4 ||
        selected_count != 1
    ) {
        return -1;
    }

    return 0;
}


/* ---------------------------------------------------------
   RECOMMEND MODE
   --------------------------------------------------------- */

int adaptive_recommend(size_t payload_bytes)
{
    size_t payload_mb =
        payload_bytes / (1024 * 1024);

    char profile_path[256];

    snprintf(
        profile_path,
        sizeof(profile_path),
        "results/adaptive_profile_%zuMB.csv",
        payload_mb
    );


    /* -----------------------------------------------------
       Reuse existing valid adaptive profile
       ----------------------------------------------------- */

    if (file_exists(profile_path)) {

        AdaptiveRow cand[4] = {0};

        size_t best_idx = 0;

        if (
            load_adaptive_profile(
                payload_mb,
                cand,
                &best_idx
            ) == 0
        ) {
            char source[256];

            snprintf(
                source,
                sizeof(source),
                "existing adaptive_profile_%zuMB.csv",
                payload_mb
            );

            print_recommendation_report(
                payload_mb,
                cand,
                best_idx,
                source
            );

            return 0;
        }

        /*
         * Existing profile is invalid.
         * Rebuild it from measured optimization data.
         */
        fprintf(
            stderr,
            "Existing adaptive profile is invalid. "
            "Rebuilding profile...\n"
        );
    }


    /* -----------------------------------------------------
       Generate profile from measured optimization data
       ----------------------------------------------------- */

    AdaptiveRow rows[4] = {0};

    size_t n = 0;

    if (
        load_chunk_summary(
            payload_mb,
            rows,
            &n
        ) != 0
    ) {
        printf(
            "No chunk-optimization summary found "
            "for %zu MB.\n",
            payload_mb
        );

        printf(
            "Run:\n"
            "./fastipc optimize-chunk %zu 5\n",
            payload_mb
        );

        return -1;
    }


    /* Confirm PIPE/FIFO/SOCKET exist */

    const char *wanted[] = {
        "PIPE",
        "FIFO",
        "SOCKET"
    };

    for (size_t w = 0; w < 3; ++w) {

        int found = 0;

        for (size_t i = 0; i < n; ++i) {

            if (
                strcmp(
                    rows[i].method,
                    wanted[w]
                ) == 0
            ) {
                found = 1;
                break;
            }
        }

        if (!found) {

            printf(
                "Incomplete chunk optimization data. "
                "Missing %s.\n",
                wanted[w]
            );

            return -1;
        }
    }


    /* -----------------------------------------------------
       Load optimized SHM ring result
       ----------------------------------------------------- */

    AdaptiveRow shm_ring = {0};

    int has_shm_ring =
        (load_shm_ring(
            payload_mb,
            &shm_ring
        ) == 0);


    /*
     * If no SHM-RING optimization file exists,
     * fall back to the best measured SHM configuration.
     *
     * We rename it SHM-RING only so the execution mapping
     * remains consistent with the optimized candidate slot.
     */
    if (!has_shm_ring) {

        for (size_t i = 0; i < n; ++i) {

            if (
                strcmp(
                    rows[i].method,
                    "SHM"
                ) == 0
            ) {
                strncpy(
                    shm_ring.method,
                    "SHM-RING",
                    sizeof(shm_ring.method) - 1
                );

                shm_ring.method[
                    sizeof(shm_ring.method) - 1
                ] = '\0';

                shm_ring.chunk_kb =
                    rows[i].chunk_kb;

                shm_ring.median_ms =
                    rows[i].median_ms;

                shm_ring.median_throughput_mbps =
                    rows[i].median_throughput_mbps;

                shm_ring.system_cpu_ms =
                    rows[i].system_cpu_ms;

                shm_ring.voluntary_cs =
                    rows[i].voluntary_cs;

                shm_ring.selected = 0;

                has_shm_ring = 1;

                break;
            }
        }
    }


    /* -----------------------------------------------------
       Build final candidate list
       ----------------------------------------------------- */

    AdaptiveRow cand[4] = {0};

    size_t ci = 0;

    for (
        size_t i = 0;
        i < n && ci < 3;
        ++i
    ) {

        if (
            strcmp(
                rows[i].method,
                "SHM"
            ) == 0
        ) {
            continue;
        }

        cand[ci++] = rows[i];
    }


    if (
        has_shm_ring &&
        ci < 4
    ) {
        cand[ci++] = shm_ring;
    }


    if (ci != 4) {

        printf(
            "Insufficient adaptive candidate data "
            "(found %zu candidates).\n",
            ci
        );

        return -1;
    }


    /* -----------------------------------------------------
       Select lowest measured median latency
       ----------------------------------------------------- */

    size_t best = 0;

    for (size_t i = 1; i < 4; ++i) {

        if (
            cand[i].median_ms <
            cand[best].median_ms
        ) {
            best = i;
        }
    }


    /* Exactly one selected winner */

    for (size_t i = 0; i < 4; ++i) {
        cand[i].selected = 0;
    }

    cand[best].selected = 1;


    /* -----------------------------------------------------
       Save adaptive profile
       ----------------------------------------------------- */

    if (
        save_adaptive_csv(
            payload_mb,
            cand,
            4
        ) != 0
    ) {
        perror(
            "save adaptive profile"
        );

        return -1;
    }


    /* -----------------------------------------------------
       Print newly generated recommendation
       ----------------------------------------------------- */

    const char *source =
        has_shm_ring
        ? "chunk optimization + SHM synchronization optimization"
        : "chunk optimization data";


    print_recommendation_report(
        payload_mb,
        cand,
        best,
        source
    );

    return 0;
}


/* ---------------------------------------------------------
   AUTO MODE
   --------------------------------------------------------- */

int adaptive_auto(size_t payload_bytes)
{
    size_t payload_mb =
        payload_bytes / (1024 * 1024);

    char profile_path[256];

    snprintf(
        profile_path,
        sizeof(profile_path),
        "results/adaptive_profile_%zuMB.csv",
        payload_mb
    );


    AdaptiveRow cand[4] = {0};

    size_t best_idx = 0;


    /* -----------------------------------------------------
       Reuse existing profile if valid
       ----------------------------------------------------- */

    if (
        file_exists(profile_path) &&
        load_adaptive_profile(
            payload_mb,
            cand,
            &best_idx
        ) == 0
    ) {
        char source[256];

        snprintf(
            source,
            sizeof(source),
            "existing adaptive_profile_%zuMB.csv",
            payload_mb
        );

        print_recommendation_report(
            payload_mb,
            cand,
            best_idx,
            source
        );
    }
    else {

        /*
         * Generate profile.
         * adaptive_recommend() also prints the report.
         */
        if (
            adaptive_recommend(
                payload_bytes
            ) != 0
        ) {
            return -1;
        }

        /*
         * Load newly generated profile so we know
         * which strategy to execute.
         */
        memset(
            cand,
            0,
            sizeof(cand)
        );

        best_idx = 0;

        if (
            load_adaptive_profile(
                payload_mb,
                cand,
                &best_idx
            ) != 0
        ) {
            fprintf(
                stderr,
                "Failed to load generated "
                "adaptive profile.\n"
            );

            return -1;
        }
    }


    AdaptiveRow selected =
        cand[best_idx];


    printf(
        "\nExecuting selected strategy...\n"
    );


    /* -----------------------------------------------------
       Execute one real transfer
       ----------------------------------------------------- */

    BenchmarkResult result = {0};

    size_t chunk_bytes =
        selected.chunk_kb * 1024;


    int rc = -1;


    if (
        strcmp(
            selected.method,
            "PIPE"
        ) == 0
    ) {
        rc = run_pipe_benchmark(
            payload_bytes,
            chunk_bytes,
            &result
        );
    }
    else if (
        strcmp(
            selected.method,
            "FIFO"
        ) == 0
    ) {
        rc = run_fifo_benchmark(
            payload_bytes,
            chunk_bytes,
            &result
        );
    }
    else if (
        strcmp(
            selected.method,
            "SOCKET"
        ) == 0
    ) {
        rc = run_socket_benchmark(
            payload_bytes,
            chunk_bytes,
            &result
        );
    }
    else if (
        strcmp(
            selected.method,
            "SHM-RING"
        ) == 0
    ) {
        rc = run_shm_ring_benchmark(
            payload_bytes,
            chunk_bytes,
            &result
        );
    }
    else {

        fprintf(
            stderr,
            "Unknown adaptive method: %s\n",
            selected.method
        );

        return -1;
    }


    if (rc != 0) {

        fprintf(
            stderr,
            "Execution of selected "
            "IPC strategy failed.\n"
        );

        return -1;
    }


    printf(
        "\nExecuted %s with chunk %zu KB\n",
        selected.method,
        selected.chunk_kb
    );


    print_result(
        selected.method,
        payload_bytes,
        &result
    );


    return 0;
}
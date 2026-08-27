#include "../include/workload_profiler.h"
#include "../include/optimizer.h"
#include "../include/shm_optimizer.h"
#include "../include/adaptive_selector.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>


#define WORKLOAD_COUNT 4
#define DEFAULT_SHM_CHUNK_KB 64


typedef struct {
    size_t payload_mb;

    char workload_class[32];
    char selected_method[64];

    size_t chunk_kb;

    double median_ms;
    double median_throughput_mbps;

    double system_cpu_ms;
    double voluntary_ctx_switches;

    char profile_source[32];
} WorkloadResult;


/* =========================================================
   Helpers
   ========================================================= */

static int file_exists(const char *path)
{
    struct stat st;

    return stat(path, &st) == 0;
}


static int ensure_results_directory(void)
{
    if (mkdir("results", 0755) != 0) {

        if (errno != EEXIST) {
            perror("mkdir results");
            return -1;
        }
    }

    return 0;
}


static const char *classify_workload(size_t payload_mb)
{
    if (payload_mb <= 1) {
        return "SMALL";
    }

    if (payload_mb <= 10) {
        return "MEDIUM";
    }

    if (payload_mb <= 100) {
        return "LARGE";
    }

    return "VERY-LARGE";
}


/* =========================================================
   Adaptive profile path
   ========================================================= */

static void build_profile_path(
    char *buffer,
    size_t buffer_size,
    size_t payload_mb
)
{
    snprintf(
        buffer,
        buffer_size,
        "results/adaptive_profile_%zuMB.csv",
        payload_mb
    );
}


/* =========================================================
   Validate/read one adaptive profile

   We require:
   - exactly four candidate rows
   - exactly one selected=1 row
   - matching payload size
   ========================================================= */

static int read_selected_profile(
    size_t payload_mb,
    WorkloadResult *result
)
{
    char path[256];

    build_profile_path(
        path,
        sizeof(path),
        payload_mb
    );

    FILE *f =
        fopen(
            path,
            "r"
        );

    if (!f) {
        return -1;
    }


    char line[512];


    /* Skip header */
    if (
        !fgets(
            line,
            sizeof(line),
            f
        )
    ) {
        fclose(f);
        return -1;
    }


    size_t row_count = 0;
    size_t selected_count = 0;

    WorkloadResult selected = {0};


    while (
        fgets(
            line,
            sizeof(line),
            f
        )
    ) {

        char method[64];

        size_t row_payload = 0;
        size_t chunk_kb = 0;

        double median_ms = 0.0;
        double median_throughput = 0.0;
        double system_cpu = 0.0;
        double voluntary_cs = 0.0;

        int is_selected = 0;


        int scanned =
            sscanf(
                line,
                "%63[^,],%zu,%zu,"
                "%lf,%lf,%lf,%lf,%d",
                method,
                &row_payload,
                &chunk_kb,
                &median_ms,
                &median_throughput,
                &system_cpu,
                &voluntary_cs,
                &is_selected
            );


        if (scanned != 8) {
            fclose(f);
            return -1;
        }


        if (row_payload != payload_mb) {
            fclose(f);
            return -1;
        }


        row_count++;


        if (is_selected == 1) {

            selected_count++;


            selected.payload_mb =
                payload_mb;


            snprintf(
                selected.workload_class,
                sizeof(selected.workload_class),
                "%s",
                classify_workload(payload_mb)
            );


            snprintf(
                selected.selected_method,
                sizeof(selected.selected_method),
                "%s",
                method
            );


            selected.chunk_kb =
                chunk_kb;


            selected.median_ms =
                median_ms;


            selected.median_throughput_mbps =
                median_throughput;


            selected.system_cpu_ms =
                system_cpu;


            selected.voluntary_ctx_switches =
                voluntary_cs;
        }
    }


    fclose(f);


    if (
        row_count != 4 ||
        selected_count != 1
    ) {
        return -1;
    }


    *result = selected;

    return 0;
}


/* =========================================================
   Generate missing optimization data

   For each payload:

   1. chunk optimization
   2. SHM baseline-vs-ring optimization at controlled 64 KB
   3. adaptive profile generation

   Existing valid adaptive profiles are preserved.
   ========================================================= */

static int generate_workload_profile(
    size_t payload_mb,
    size_t trials,
    WorkloadResult *result
)
{
    char profile_path[256];

    build_profile_path(
        profile_path,
        sizeof(profile_path),
        payload_mb
    );


    /*
     * First try to reuse an existing valid profile.
     * This is especially important for the existing
     * high-quality 100 MB / 5-trial evidence.
     */
    if (
        file_exists(profile_path) &&
        read_selected_profile(
            payload_mb,
            result
        ) == 0
    ) {

        snprintf(
            result->profile_source,
            sizeof(result->profile_source),
            "%s",
            "REUSED"
        );


        printf(
            "\nExisting valid adaptive profile found "
            "for %zu MB. Reusing it.\n",
            payload_mb
        );


        return 0;
    }


    printf(
        "\n============================================================\n"
    );

    printf(
        "BUILDING WORKLOAD PROFILE: %zu MB (%s)\n",
        payload_mb,
        classify_workload(payload_mb)
    );

    printf(
        "============================================================\n"
    );


    size_t payload_bytes =
        payload_mb *
        1024ULL *
        1024ULL;


    /* -----------------------------------------------------
       Step 1 — Chunk-size optimization
       ----------------------------------------------------- */

    printf(
        "\n[1/3] Running chunk-size optimization...\n"
    );


    if (
        run_chunk_optimizer(
            payload_bytes,
            trials
        ) != 0
    ) {

        fprintf(
            stderr,
            "Chunk optimization failed "
            "for %zu MB.\n",
            payload_mb
        );

        return -1;
    }


    /* -----------------------------------------------------
       Step 2 — SHM synchronization optimization

       Controlled comparison at 64 KB.
       ----------------------------------------------------- */

    printf(
        "\n[2/3] Running SHM synchronization optimization...\n"
    );


    size_t shm_chunk_bytes =
        DEFAULT_SHM_CHUNK_KB *
        1024ULL;


    if (
        run_shm_optimization(
            payload_bytes,
            shm_chunk_bytes,
            trials
        ) != 0
    ) {

        fprintf(
            stderr,
            "SHM optimization failed "
            "for %zu MB.\n",
            payload_mb
        );

        return -1;
    }


    /* -----------------------------------------------------
       Step 3 — Adaptive selector

       Since no valid profile existed, adaptive_recommend()
       will construct and save a fresh profile from the
       measured optimization data.
       ----------------------------------------------------- */

    printf(
        "\n[3/3] Building adaptive recommendation...\n"
    );


    if (
        adaptive_recommend(
            payload_bytes
        ) != 0
    ) {

        fprintf(
            stderr,
            "Adaptive recommendation failed "
            "for %zu MB.\n",
            payload_mb
        );

        return -1;
    }


    /* -----------------------------------------------------
       Load the generated selected row
       ----------------------------------------------------- */

    if (
        read_selected_profile(
            payload_mb,
            result
        ) != 0
    ) {

        fprintf(
            stderr,
            "Generated adaptive profile for %zu MB "
            "is invalid.\n",
            payload_mb
        );

        return -1;
    }


    snprintf(
        result->profile_source,
        sizeof(result->profile_source),
        "%s",
        "GENERATED"
    );


    return 0;
}


/* =========================================================
   Save multi-workload summary
   ========================================================= */

static int save_summary_csv(
    const WorkloadResult results[],
    size_t count,
    size_t trials
)
{
    if (
        ensure_results_directory() != 0
    ) {
        return -1;
    }


    const char *path =
        "results/workload_adaptive_summary.csv";


    FILE *f =
        fopen(
            path,
            "w"
        );


    if (!f) {

        perror(
            "fopen workload summary"
        );

        return -1;
    }


    fprintf(
        f,
        "payload_mb,"
        "workload_class,"
        "selected_method,"
        "chunk_kb,"
        "median_ms,"
        "median_throughput_mbps,"
        "system_cpu_ms,"
        "voluntary_ctx_switches,"
        "profile_source,"
        "requested_trials\n"
    );


    for (
        size_t i = 0;
        i < count;
        ++i
    ) {

        fprintf(
            f,
            "%zu,%s,%s,%zu,"
            "%.6f,%.6f,"
            "%.6f,%.2f,"
            "%s,%zu\n",
            results[i].payload_mb,
            results[i].workload_class,
            results[i].selected_method,
            results[i].chunk_kb,
            results[i].median_ms,
            results[i].median_throughput_mbps,
            results[i].system_cpu_ms,
            results[i].voluntary_ctx_switches,
            results[i].profile_source,
            trials
        );
    }


    fclose(f);


    printf(
        "\nSummary CSV: %s\n",
        path
    );


    return 0;
}


/* =========================================================
   Print final adaptive workload matrix
   ========================================================= */

static void print_summary(
    const WorkloadResult results[],
    size_t count
)
{
    printf(
        "\n"
        "FASTIPC-X MULTI-WORKLOAD ADAPTIVE PROFILE\n"
        "==========================================================================\n\n"
    );


    printf(
        "%-10s %-12s %-14s %-10s %-12s %-14s\n",
        "Payload",
        "Class",
        "Selected IPC",
        "Chunk KB",
        "Median ms",
        "Median MB/s"
    );


    printf(
        "--------------------------------------------------------------------------\n"
    );


    for (
        size_t i = 0;
        i < count;
        ++i
    ) {

        char payload_text[32];

        snprintf(
            payload_text,
            sizeof(payload_text),
            "%zu MB",
            results[i].payload_mb
        );


        printf(
            "%-10s %-12s %-14s %-10zu %-12.3f %-14.3f\n",
            payload_text,
            results[i].workload_class,
            results[i].selected_method,
            results[i].chunk_kb,
            results[i].median_ms,
            results[i].median_throughput_mbps
        );
    }


    printf(
        "\nProfile Sources:\n"
    );


    for (
        size_t i = 0;
        i < count;
        ++i
    ) {

        printf(
            "  %zu MB : %s\n",
            results[i].payload_mb,
            results[i].profile_source
        );
    }
}


/* =========================================================
   Public command
   ========================================================= */

int run_workload_profiles(size_t trials)
{
    if (trials == 0) {

        fprintf(
            stderr,
            "Trials must be greater than zero.\n"
        );

        return -1;
    }


    if (
        ensure_results_directory() != 0
    ) {
        return -1;
    }


    /*
     * Workload classes:
     *
     * 1 MB   = small
     * 10 MB  = medium
     * 100 MB = large
     * 500 MB = very large
     */
    const size_t workloads[
        WORKLOAD_COUNT
    ] = {
        1,
        10,
        100,
        500
    };


    WorkloadResult results[
        WORKLOAD_COUNT
    ];


    memset(
        results,
        0,
        sizeof(results)
    );


    printf(
        "\n"
        "FASTIPC-X MULTI-WORKLOAD PROFILE BUILDER\n"
        "============================================================\n"
    );


    printf(
        "Requested trials for new profiles: %zu\n",
        trials
    );


    printf(
        "Workloads: 1 MB, 10 MB, 100 MB, 500 MB\n"
    );


    printf(
        "Existing valid profiles will be reused.\n"
    );


    /*
     * Build/reuse each workload independently.
     */
    for (
        size_t i = 0;
        i < WORKLOAD_COUNT;
        ++i
    ) {

        if (
            generate_workload_profile(
                workloads[i],
                trials,
                &results[i]
            ) != 0
        ) {

            fprintf(
                stderr,
                "\nMulti-workload profiling stopped "
                "at %zu MB.\n",
                workloads[i]
            );

            return -1;
        }
    }


    print_summary(
        results,
        WORKLOAD_COUNT
    );


    if (
        save_summary_csv(
            results,
            WORKLOAD_COUNT,
            trials
        ) != 0
    ) {
        return -1;
    }


    printf(
        "\nMulti-workload adaptive profiling complete.\n"
    );


    return 0;
}
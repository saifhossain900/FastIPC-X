#define _GNU_SOURCE

#include "../include/memory_optimizer.h"
#include "../include/benchmark.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>

#include <time.h>
#include <unistd.h>


#define MEMORY_MODE_COUNT 3


/* =========================================================
   Memory experiment modes
   ========================================================= */

typedef enum {
    MEMORY_DEMAND = 0,
    MEMORY_PREFAULT = 1,
    MEMORY_WILLNEED = 2
} MemoryMode;


/* =========================================================
   One experimental trial
   ========================================================= */

typedef struct {
    double setup_ms;
    double timed_ms;
    double total_ms;

    double throughput_mbps;

    double setup_user_ms;
    double setup_system_ms;

    double timed_user_ms;
    double timed_system_ms;

    long setup_minor_faults;
    long setup_major_faults;

    long timed_minor_faults;
    long timed_major_faults;

    long timed_voluntary_ctx_switches;
    long timed_involuntary_ctx_switches;
} MemoryTrial;


/* =========================================================
   Resource-usage snapshot
   ========================================================= */

typedef struct {
    struct rusage usage;
} UsageSnapshot;


/* =========================================================
   Mode names
   ========================================================= */

static const char *memory_mode_name(
    MemoryMode mode
)
{
    switch (mode) {

        case MEMORY_DEMAND:
            return "DEMAND";

        case MEMORY_PREFAULT:
            return "PREFAULT";

        case MEMORY_WILLNEED:
            return "MADVISE-WILLNEED";

        default:
            return "UNKNOWN";
    }
}


/* =========================================================
   Results directory
   ========================================================= */

static int ensure_results_directory(void)
{
    if (
        mkdir(
            "results",
            0755
        ) != 0 &&
        errno != EEXIST
    ) {
        perror(
            "mkdir results"
        );

        return -1;
    }

    return 0;
}


/* =========================================================
   timeval -> milliseconds
   ========================================================= */

static double timeval_to_ms(
    const struct timeval *value
)
{
    if (!value) {
        return 0.0;
    }

    return
        (
            (double)value->tv_sec *
            1000.0
        ) +
        (
            (double)value->tv_usec /
            1000.0
        );
}


/* =========================================================
   Resource snapshot
   ========================================================= */

static int take_usage_snapshot(
    UsageSnapshot *snapshot
)
{
    if (!snapshot) {
        return -1;
    }

    if (
        getrusage(
            RUSAGE_SELF,
            &snapshot->usage
        ) != 0
    ) {
        perror(
            "getrusage"
        );

        return -1;
    }

    return 0;
}


/* =========================================================
   Calculate resource delta
   ========================================================= */

static void calculate_usage_delta(
    const UsageSnapshot *before,
    const UsageSnapshot *after,
    double *user_ms,
    double *system_ms,
    long *minor_faults,
    long *major_faults,
    long *voluntary_ctx_switches,
    long *involuntary_ctx_switches
)
{
    if (
        !before ||
        !after
    ) {
        return;
    }


    if (user_ms) {

        *user_ms =
            timeval_to_ms(
                &after->usage.ru_utime
            ) -
            timeval_to_ms(
                &before->usage.ru_utime
            );
    }


    if (system_ms) {

        *system_ms =
            timeval_to_ms(
                &after->usage.ru_stime
            ) -
            timeval_to_ms(
                &before->usage.ru_stime
            );
    }


    if (minor_faults) {

        *minor_faults =
            after->usage.ru_minflt -
            before->usage.ru_minflt;
    }


    if (major_faults) {

        *major_faults =
            after->usage.ru_majflt -
            before->usage.ru_majflt;
    }


    if (voluntary_ctx_switches) {

        *voluntary_ctx_switches =
            after->usage.ru_nvcsw -
            before->usage.ru_nvcsw;
    }


    if (involuntary_ctx_switches) {

        *involuntary_ctx_switches =
            after->usage.ru_nivcsw -
            before->usage.ru_nivcsw;
    }
}


/* =========================================================
   qsort comparison
   ========================================================= */

static int compare_double(
    const void *left,
    const void *right
)
{
    double a =
        *(const double *)left;

    double b =
        *(const double *)right;


    if (a < b) {
        return -1;
    }

    if (a > b) {
        return 1;
    }

    return 0;
}


/* =========================================================
   Median
   ========================================================= */

static int median_value(
    const double *values,
    size_t count,
    double *result
)
{
    if (
        !values ||
        count == 0 ||
        !result
    ) {
        return -1;
    }


    double *copy =
        malloc(
            count *
            sizeof(double)
        );


    if (!copy) {

        perror(
            "malloc median"
        );

        return -1;
    }


    memcpy(
        copy,
        values,
        count *
        sizeof(double)
    );


    qsort(
        copy,
        count,
        sizeof(double),
        compare_double
    );


    if (
        count %
        2 ==
        0
    ) {

        *result =
            (
                copy[
                    count / 2 - 1
                ] +
                copy[
                    count / 2
                ]
            ) /
            2.0;
    }
    else {

        *result =
            copy[
                count / 2
            ];
    }


    free(
        copy
    );


    return 0;
}


/* =========================================================
   Pre-fault every memory page

   One byte is written on every page before the critical-path
   timer begins.

   Important:
   This does not make page-fault work disappear. It moves
   much of that work into the setup phase.
   ========================================================= */

static void prefault_pages(
    void *map,
    size_t total_bytes,
    size_t page_size
)
{
    volatile unsigned char *bytes =
        (volatile unsigned char *)map;


    for (
        size_t offset = 0;
        offset < total_bytes;
        offset += page_size
    ) {

        bytes[offset] =
            0;
    }
}


/* =========================================================
   Timed memory workload

   Writes the complete mapping in chunk-sized blocks.

   Every mode executes exactly this same critical-path
   workload. Only the memory preparation strategy changes.
   ========================================================= */

static void run_memory_workload(
    void *map,
    size_t total_bytes,
    size_t chunk_size,
    unsigned char pattern
)
{
    unsigned char *bytes =
        (unsigned char *)map;


    size_t completed =
        0;


    while (
        completed <
        total_bytes
    ) {

        size_t remaining =
            total_bytes -
            completed;


        size_t amount =
            remaining <
            chunk_size
            ?
            remaining
            :
            chunk_size;


        memset(
            bytes +
            completed,
            pattern,
            amount
        );


        completed +=
            amount;
    }
}


/* =========================================================
   Create unique POSIX shared-memory mapping

   Creation/mmap cost is intentionally outside the strategy
   setup timer. Every mode receives a fresh mapping, so the
   memory-residency preparation itself is what is compared.
   ========================================================= */

static int create_shared_mapping(
    size_t total_bytes,
    MemoryMode mode,
    size_t trial_number,
    void **map_out,
    int *fd_out
)
{
    if (
        total_bytes == 0 ||
        !map_out ||
        !fd_out
    ) {
        return -1;
    }


    struct timespec ts;


    if (
        clock_gettime(
            CLOCK_REALTIME,
            &ts
        ) != 0
    ) {

        perror(
            "clock_gettime"
        );

        return -1;
    }


    char name[
        256
    ];


    snprintf(
        name,
        sizeof(name),
        "/fastipc_vm_%d_%d_%zu_%ld_%ld",
        (int)getpid(),
        (int)mode,
        trial_number,
        ts.tv_sec,
        ts.tv_nsec
    );


    int fd =
        shm_open(
            name,
            O_CREAT |
            O_EXCL |
            O_RDWR,
            0600
        );


    if (
        fd < 0
    ) {

        perror(
            "shm_open memory optimizer"
        );

        return -1;
    }


    if (
        ftruncate(
            fd,
            (off_t)total_bytes
        ) != 0
    ) {

        perror(
            "ftruncate memory optimizer"
        );


        shm_unlink(
            name
        );


        close(
            fd
        );


        return -1;
    }


    void *map =
        mmap(
            NULL,
            total_bytes,
            PROT_READ |
            PROT_WRITE,
            MAP_SHARED,
            fd,
            0
        );


    if (
        map ==
        MAP_FAILED
    ) {

        perror(
            "mmap memory optimizer"
        );


        shm_unlink(
            name
        );


        close(
            fd
        );


        return -1;
    }


    /*
     * Immediately unlink the name.
     *
     * The open descriptor and mapping remain valid.
     * This prevents temporary POSIX SHM names from being left
     * behind after normal execution.
     */
    if (
        shm_unlink(
            name
        ) != 0
    ) {

        perror(
            "shm_unlink memory optimizer"
        );


        munmap(
            map,
            total_bytes
        );


        close(
            fd
        );


        return -1;
    }


    *map_out =
        map;


    *fd_out =
        fd;


    return 0;
}


/* =========================================================
   Destroy one temporary mapping
   ========================================================= */

static int destroy_shared_mapping(
    void *map,
    size_t total_bytes,
    int fd
)
{
    int rc =
        0;


    if (
        map &&
        map != MAP_FAILED
    ) {

        if (
            munmap(
                map,
                total_bytes
            ) != 0
        ) {

            perror(
                "munmap memory optimizer"
            );

            rc =
                -1;
        }
    }


    if (
        fd >= 0
    ) {

        if (
            close(
                fd
            ) != 0
        ) {

            perror(
                "close memory optimizer"
            );

            rc =
                -1;
        }
    }


    return rc;
}


/* =========================================================
   Run one memory-mode trial
   ========================================================= */

static int run_one_memory_trial(
    size_t total_bytes,
    size_t chunk_size,
    size_t page_size,
    MemoryMode mode,
    size_t trial_number,
    MemoryTrial *trial
)
{
    if (
        total_bytes == 0 ||
        chunk_size == 0 ||
        page_size == 0 ||
        !trial
    ) {
        return -1;
    }


    memset(
        trial,
        0,
        sizeof(*trial)
    );


    void *map =
        NULL;


    int fd =
        -1;


    if (
        create_shared_mapping(
            total_bytes,
            mode,
            trial_number,
            &map,
            &fd
        ) != 0
    ) {
        return -1;
    }


    UsageSnapshot before_setup;
    UsageSnapshot after_setup;
    UsageSnapshot after_timed;


    if (
        take_usage_snapshot(
            &before_setup
        ) != 0
    ) {

        destroy_shared_mapping(
            map,
            total_bytes,
            fd
        );

        return -1;
    }


    double setup_start =
        now_ms();


    /* =====================================================
       MODE-SPECIFIC MEMORY PREPARATION
       ===================================================== */

    if (
        mode ==
        MEMORY_PREFAULT
    ) {

        prefault_pages(
            map,
            total_bytes,
            page_size
        );
    }

    else if (
        mode ==
        MEMORY_WILLNEED
    ) {

        /*
         * MADV_WILLNEED is a hint.
         *
         * Linux is allowed to treat it differently depending
         * on mapping type, kernel and environment. Therefore
         * this experiment measures its effect instead of
         * assuming that it populates the mapping.
         */
        if (
            madvise(
                map,
                total_bytes,
                MADV_WILLNEED
            ) != 0
        ) {

            perror(
                "madvise MADV_WILLNEED"
            );


            destroy_shared_mapping(
                map,
                total_bytes,
                fd
            );


            return -1;
        }
    }


    double setup_end =
        now_ms();


    if (
        take_usage_snapshot(
            &after_setup
        ) != 0
    ) {

        destroy_shared_mapping(
            map,
            total_bytes,
            fd
        );


        return -1;
    }


    /* =====================================================
       IDENTICAL TIMED CRITICAL PATH FOR EVERY MODE
       ===================================================== */

    double timed_start =
        now_ms();


    run_memory_workload(
        map,
        total_bytes,
        chunk_size,
        (unsigned char)(
            0x40 +
            (unsigned char)mode
        )
    );


    double timed_end =
        now_ms();


    if (
        take_usage_snapshot(
            &after_timed
        ) != 0
    ) {

        destroy_shared_mapping(
            map,
            total_bytes,
            fd
        );


        return -1;
    }


    /*
     * Observable read after the timed write pass.
     *
     * It is intentionally outside the critical-path timer.
     */
    volatile unsigned char sink =
        (
            (volatile unsigned char *)map
        )[
            total_bytes - 1
        ];


    (void)sink;


    trial->setup_ms =
        setup_end -
        setup_start;


    trial->timed_ms =
        timed_end -
        timed_start;


    /*
     * Strategy total:
     *
     * setup preparation + critical path.
     *
     * Shared mapping creation is common to all modes and is
     * intentionally excluded from this comparison.
     */
    trial->total_ms =
        trial->setup_ms +
        trial->timed_ms;


    double seconds =
        trial->timed_ms /
        1000.0;


    trial->throughput_mbps =
        seconds >
        0.0
        ?
        (
            (
                (double)total_bytes /
                (
                    1024.0 *
                    1024.0
                )
            ) /
            seconds
        )
        :
        0.0;


    long unused_setup_voluntary =
        0;


    long unused_setup_involuntary =
        0;


    calculate_usage_delta(
        &before_setup,
        &after_setup,
        &trial->setup_user_ms,
        &trial->setup_system_ms,
        &trial->setup_minor_faults,
        &trial->setup_major_faults,
        &unused_setup_voluntary,
        &unused_setup_involuntary
    );


    calculate_usage_delta(
        &after_setup,
        &after_timed,
        &trial->timed_user_ms,
        &trial->timed_system_ms,
        &trial->timed_minor_faults,
        &trial->timed_major_faults,
        &trial->timed_voluntary_ctx_switches,
        &trial->timed_involuntary_ctx_switches
    );


    if (
        destroy_shared_mapping(
            map,
            total_bytes,
            fd
        ) != 0
    ) {

        return -1;
    }


    return 0;
}


/* =========================================================
   Free all mode data
   ========================================================= */

static void free_trial_data(
    MemoryTrial *data[
        MEMORY_MODE_COUNT
    ]
)
{
    for (
        size_t i = 0;
        i < MEMORY_MODE_COUNT;
        ++i
    ) {

        free(
            data[i]
        );


        data[i] =
            NULL;
    }
}


/* =========================================================
   Public memory optimizer
   ========================================================= */

int run_memory_optimizer(
    size_t total_bytes,
    size_t chunk_size,
    size_t trials
)
{
    if (
        total_bytes == 0 ||
        chunk_size == 0 ||
        trials == 0
    ) {

        fprintf(
            stderr,
            "Invalid memory optimizer arguments.\n"
        );


        return -1;
    }


    if (
        ensure_results_directory() != 0
    ) {

        return -1;
    }


    long page_size_long =
        sysconf(
            _SC_PAGESIZE
        );


    if (
        page_size_long <= 0
    ) {

        fprintf(
            stderr,
            "Unable to determine system page size.\n"
        );


        return -1;
    }


    size_t page_size =
        (size_t)page_size_long;


    size_t page_count =
        (
            total_bytes +
            page_size -
            1
        ) /
        page_size;


    MemoryTrial *data[
        MEMORY_MODE_COUNT
    ] = {
        NULL,
        NULL,
        NULL
    };


    for (
        size_t mode = 0;
        mode < MEMORY_MODE_COUNT;
        ++mode
    ) {

        data[mode] =
            calloc(
                trials,
                sizeof(
                    MemoryTrial
                )
            );


        if (!data[mode]) {

            perror(
                "calloc memory trials"
            );


            free_trial_data(
                data
            );


            return -1;
        }
    }


    size_t payload_mb =
        total_bytes /
        (
            1024ULL *
            1024ULL
        );


    size_t chunk_kb =
        chunk_size /
        1024ULL;


    char trials_path[
        256
    ];


    char summary_path[
        256
    ];


    snprintf(
        trials_path,
        sizeof(trials_path),
        "results/memory_trials_%zuMB_%zuKB.csv",
        payload_mb,
        chunk_kb
    );


    snprintf(
        summary_path,
        sizeof(summary_path),
        "results/memory_summary_%zuMB_%zuKB.csv",
        payload_mb,
        chunk_kb
    );


    FILE *trials_file =
        fopen(
            trials_path,
            "w"
        );


    if (!trials_file) {

        perror(
            "fopen memory trials"
        );


        free_trial_data(
            data
        );


        return -1;
    }


    fprintf(
        trials_file,
        "trial,"
        "execution_order,"
        "mode,"
        "setup_ms,"
        "timed_ms,"
        "total_ms,"
        "throughput_mbps,"
        "setup_user_ms,"
        "setup_system_ms,"
        "timed_user_ms,"
        "timed_system_ms,"
        "setup_minor_faults,"
        "setup_major_faults,"
        "timed_minor_faults,"
        "timed_major_faults,"
        "timed_voluntary_ctx_switches,"
        "timed_involuntary_ctx_switches\n"
    );


    printf(
        "\n"
        "FASTIPC-X VIRTUAL MEMORY / PAGE-FAULT ANALYSIS\n"
        "============================================================\n"
    );


    printf(
        "Payload               : %zu MB\n",
        payload_mb
    );


    printf(
        "Chunk size            : %zu KB\n",
        chunk_kb
    );


    printf(
        "Page size             : %zu bytes\n",
        page_size
    );


    printf(
        "Pages in mapping      : %zu\n",
        page_count
    );


    printf(
        "Trials per mode       : %zu\n\n",
        trials
    );


    printf(
        "Modes\n"
        "------------------------------------------------------------\n"
    );


    printf(
        "DEMAND                : first-touch during timed workload\n"
    );


    printf(
        "PREFAULT              : every page touched before timing\n"
    );


    printf(
        "MADVISE-WILLNEED      : kernel receives MADV_WILLNEED hint\n\n"
    );


    /*
     * Rotate execution order:
     *
     * Trial 1:
     * DEMAND -> PREFAULT -> WILLNEED
     *
     * Trial 2:
     * PREFAULT -> WILLNEED -> DEMAND
     *
     * Trial 3:
     * WILLNEED -> DEMAND -> PREFAULT
     *
     * This reduces systematic first/last-run bias.
     */
    for (
        size_t trial_index = 0;
        trial_index < trials;
        ++trial_index
    ) {

        printf(
            "Trial %zu/%zu\n",
            trial_index + 1,
            trials
        );


        for (
            size_t order = 0;
            order < MEMORY_MODE_COUNT;
            ++order
        ) {

            size_t mode =
                (
                    trial_index +
                    order
                ) %
                MEMORY_MODE_COUNT;


            if (
                run_one_memory_trial(
                    total_bytes,
                    chunk_size,
                    page_size,
                    (MemoryMode)mode,
                    trial_index + 1,
                    &data[mode][trial_index]
                ) != 0
            ) {

                fclose(
                    trials_file
                );


                free_trial_data(
                    data
                );


                return -1;
            }


            printf(
                "  %-18s "
                "setup %8.3f ms  "
                "timed %8.3f ms  "
                "%10.3f MB/s  "
                "minor faults %ld -> %ld  "
                "major %ld -> %ld\n",
                memory_mode_name(
                    (MemoryMode)mode
                ),
                data[mode][trial_index].setup_ms,
                data[mode][trial_index].timed_ms,
                data[mode][trial_index].throughput_mbps,
                data[mode][trial_index].setup_minor_faults,
                data[mode][trial_index].timed_minor_faults,
                data[mode][trial_index].setup_major_faults,
                data[mode][trial_index].timed_major_faults
            );


            fprintf(
                trials_file,
                "%zu,%zu,%s,"
                "%.6f,%.6f,%.6f,%.6f,"
                "%.6f,%.6f,%.6f,%.6f,"
                "%ld,%ld,%ld,%ld,%ld,%ld\n",
                trial_index + 1,
                order + 1,
                memory_mode_name(
                    (MemoryMode)mode
                ),
                data[mode][trial_index].setup_ms,
                data[mode][trial_index].timed_ms,
                data[mode][trial_index].total_ms,
                data[mode][trial_index].throughput_mbps,
                data[mode][trial_index].setup_user_ms,
                data[mode][trial_index].setup_system_ms,
                data[mode][trial_index].timed_user_ms,
                data[mode][trial_index].timed_system_ms,
                data[mode][trial_index].setup_minor_faults,
                data[mode][trial_index].setup_major_faults,
                data[mode][trial_index].timed_minor_faults,
                data[mode][trial_index].timed_major_faults,
                data[mode][trial_index].timed_voluntary_ctx_switches,
                data[mode][trial_index].timed_involuntary_ctx_switches
            );
        }


        printf(
            "\n"
        );
    }


    fclose(
        trials_file
    );


    /* =====================================================
       Summary arrays
       ===================================================== */

    double median_setup[
        MEMORY_MODE_COUNT
    ];


    double median_timed[
        MEMORY_MODE_COUNT
    ];


    double median_total[
        MEMORY_MODE_COUNT
    ];


    double median_throughput[
        MEMORY_MODE_COUNT
    ];


    double average_setup_minor[
        MEMORY_MODE_COUNT
    ];


    double average_timed_minor[
        MEMORY_MODE_COUNT
    ];


    double average_setup_major[
        MEMORY_MODE_COUNT
    ];


    double average_timed_major[
        MEMORY_MODE_COUNT
    ];


    double average_timed_system[
        MEMORY_MODE_COUNT
    ];


    double average_timed_voluntary[
        MEMORY_MODE_COUNT
    ];


    double average_timed_involuntary[
        MEMORY_MODE_COUNT
    ];


    for (
        size_t mode = 0;
        mode < MEMORY_MODE_COUNT;
        ++mode
    ) {

        double *setup_values =
            malloc(
                trials *
                sizeof(double)
            );


        double *timed_values =
            malloc(
                trials *
                sizeof(double)
            );


        double *total_values =
            malloc(
                trials *
                sizeof(double)
            );


        double *throughput_values =
            malloc(
                trials *
                sizeof(double)
            );


        if (
            !setup_values ||
            !timed_values ||
            !total_values ||
            !throughput_values
        ) {

            perror(
                "malloc memory summary"
            );


            free(
                setup_values
            );


            free(
                timed_values
            );


            free(
                total_values
            );


            free(
                throughput_values
            );


            free_trial_data(
                data
            );


            return -1;
        }


        double setup_minor_sum =
            0.0;


        double timed_minor_sum =
            0.0;


        double setup_major_sum =
            0.0;


        double timed_major_sum =
            0.0;


        double timed_system_sum =
            0.0;


        double timed_voluntary_sum =
            0.0;


        double timed_involuntary_sum =
            0.0;


        for (
            size_t trial = 0;
            trial < trials;
            ++trial
        ) {

            setup_values[trial] =
                data[mode][trial].setup_ms;


            timed_values[trial] =
                data[mode][trial].timed_ms;


            total_values[trial] =
                data[mode][trial].total_ms;


            throughput_values[trial] =
                data[mode][trial].throughput_mbps;


            setup_minor_sum +=
                (double)data[mode][trial].setup_minor_faults;


            timed_minor_sum +=
                (double)data[mode][trial].timed_minor_faults;


            setup_major_sum +=
                (double)data[mode][trial].setup_major_faults;


            timed_major_sum +=
                (double)data[mode][trial].timed_major_faults;


            timed_system_sum +=
                data[mode][trial].timed_system_ms;


            timed_voluntary_sum +=
                (double)data[mode][trial].timed_voluntary_ctx_switches;


            timed_involuntary_sum +=
                (double)data[mode][trial].timed_involuntary_ctx_switches;
        }


        if (
            median_value(
                setup_values,
                trials,
                &median_setup[mode]
            ) != 0 ||
            median_value(
                timed_values,
                trials,
                &median_timed[mode]
            ) != 0 ||
            median_value(
                total_values,
                trials,
                &median_total[mode]
            ) != 0 ||
            median_value(
                throughput_values,
                trials,
                &median_throughput[mode]
            ) != 0
        ) {

            free(
                setup_values
            );


            free(
                timed_values
            );


            free(
                total_values
            );


            free(
                throughput_values
            );


            free_trial_data(
                data
            );


            return -1;
        }


        average_setup_minor[mode] =
            setup_minor_sum /
            (double)trials;


        average_timed_minor[mode] =
            timed_minor_sum /
            (double)trials;


        average_setup_major[mode] =
            setup_major_sum /
            (double)trials;


        average_timed_major[mode] =
            timed_major_sum /
            (double)trials;


        average_timed_system[mode] =
            timed_system_sum /
            (double)trials;


        average_timed_voluntary[mode] =
            timed_voluntary_sum /
            (double)trials;


        average_timed_involuntary[mode] =
            timed_involuntary_sum /
            (double)trials;


        free(
            setup_values
        );


        free(
            timed_values
        );


        free(
            total_values
        );


        free(
            throughput_values
        );
    }


    /* =====================================================
       Select winners
       ===================================================== */

    size_t best_critical =
        MEMORY_DEMAND;


    size_t best_total =
        MEMORY_DEMAND;


    for (
        size_t mode = 1;
        mode < MEMORY_MODE_COUNT;
        ++mode
    ) {

        if (
            median_timed[mode] <
            median_timed[best_critical]
        ) {

            best_critical =
                mode;
        }


        if (
            median_total[mode] <
            median_total[best_total]
        ) {

            best_total =
                mode;
        }
    }


    /* =====================================================
       Save summary CSV
       ===================================================== */

    FILE *summary_file =
        fopen(
            summary_path,
            "w"
        );


    if (!summary_file) {

        perror(
            "fopen memory summary"
        );


        free_trial_data(
            data
        );


        return -1;
    }


    fprintf(
        summary_file,
        "mode,"
        "median_setup_ms,"
        "median_timed_ms,"
        "median_total_ms,"
        "median_throughput_mbps,"
        "average_setup_minor_faults,"
        "average_timed_minor_faults,"
        "average_setup_major_faults,"
        "average_timed_major_faults,"
        "average_timed_system_ms,"
        "average_timed_voluntary_ctx_switches,"
        "average_timed_involuntary_ctx_switches,"
        "selected_critical_path,"
        "selected_strategy_total\n"
    );


    for (
        size_t mode = 0;
        mode < MEMORY_MODE_COUNT;
        ++mode
    ) {

        fprintf(
            summary_file,
            "%s,"
            "%.6f,%.6f,%.6f,%.6f,"
            "%.2f,%.2f,%.2f,%.2f,"
            "%.6f,%.2f,%.2f,%d,%d\n",
            memory_mode_name(
                (MemoryMode)mode
            ),
            median_setup[mode],
            median_timed[mode],
            median_total[mode],
            median_throughput[mode],
            average_setup_minor[mode],
            average_timed_minor[mode],
            average_setup_major[mode],
            average_timed_major[mode],
            average_timed_system[mode],
            average_timed_voluntary[mode],
            average_timed_involuntary[mode],
            mode ==
            best_critical
            ?
            1
            :
            0,
            mode ==
            best_total
            ?
            1
            :
            0
        );
    }


    fclose(
        summary_file
    );


    /* =====================================================
       Console summary
       ===================================================== */

    printf(
        "VIRTUAL MEMORY SUMMARY\n"
        "============================================================\n"
    );


    printf(
        "%-18s %-11s %-11s %-11s %-12s %-12s\n",
        "Mode",
        "Setup(ms)",
        "Timed(ms)",
        "Total(ms)",
        "TimedFaults",
        "Critical"
    );


    printf(
        "--------------------------------------------------------------------------\n"
    );


    for (
        size_t mode = 0;
        mode < MEMORY_MODE_COUNT;
        ++mode
    ) {

        printf(
            "%-18s %-11.3f %-11.3f %-11.3f %-12.2f %-12s\n",
            memory_mode_name(
                (MemoryMode)mode
            ),
            median_setup[mode],
            median_timed[mode],
            median_total[mode],
            average_timed_minor[mode],
            mode ==
            best_critical
            ?
            "YES"
            :
            ""
        );
    }


    double critical_improvement =
        0.0;


    double total_improvement =
        0.0;


    if (
        median_timed[MEMORY_DEMAND] >
        0.0
    ) {

        critical_improvement =
            (
                median_timed[MEMORY_DEMAND] -
                median_timed[best_critical]
            ) /
            median_timed[MEMORY_DEMAND] *
            100.0;
    }


    if (
        median_total[MEMORY_DEMAND] >
        0.0
    ) {

        total_improvement =
            (
                median_total[MEMORY_DEMAND] -
                median_total[best_total]
            ) /
            median_total[MEMORY_DEMAND] *
            100.0;
    }


    printf(
        "\nBest critical-path mode : %s\n",
        memory_mode_name(
            (MemoryMode)best_critical
        )
    );


    printf(
        "Best strategy-total mode: %s\n",
        memory_mode_name(
            (MemoryMode)best_total
        )
    );


    printf(
        "Critical-path change    : %.2f%% improvement vs DEMAND\n",
        critical_improvement
    );


    printf(
        "Strategy-total change   : %.2f%% improvement vs DEMAND\n",
        total_improvement
    );


    if (
        best_critical !=
        best_total
    ) {

        printf(
            "Trade-off               : Critical-path winner differs "
            "from strategy-total winner.\n"
        );


        printf(
            "                          Memory preparation shifts work "
            "outside the timed path.\n"
        );
    }


    printf(
        "\nPage-fault interpretation\n"
        "------------------------------------------------------------\n"
    );


    printf(
        "DEMAND timed minor faults   : %.2f average\n",
        average_timed_minor[
            MEMORY_DEMAND
        ]
    );


    printf(
        "PREFAULT setup minor faults : %.2f average\n",
        average_setup_minor[
            MEMORY_PREFAULT
        ]
    );


    printf(
        "PREFAULT timed minor faults : %.2f average\n",
        average_timed_minor[
            MEMORY_PREFAULT
        ]
    );


    printf(
        "WILLNEED timed minor faults : %.2f average\n",
        average_timed_minor[
            MEMORY_WILLNEED
        ]
    );


    printf(
        "\nTrials CSV             : %s\n",
        trials_path
    );


    printf(
        "Summary CSV            : %s\n",
        summary_path
    );


    free_trial_data(
        data
    );


    return 0;
}
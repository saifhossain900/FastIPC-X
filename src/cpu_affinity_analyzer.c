#define _GNU_SOURCE

#include "../include/cpu_affinity_analyzer.h"
#include "../include/shm_ring_ipc.h"

#include <errno.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sys/resource.h>
#include <sys/stat.h>


#define MODE_COUNT 3
#define CPU_TEXT_SIZE 128


typedef enum {
    AFFINITY_UNPINNED = 0,
    AFFINITY_SAME_CPU = 1,
    AFFINITY_SEPARATE_CPU = 2
} AffinityMode;


typedef struct {
    double elapsed_ms;
    double throughput_mbps;

    double user_ms;
    double sys_ms;

    long voluntary_ctx_switches;
    long involuntary_ctx_switches;
} AffinityTrial;


typedef struct {
    struct rusage self;
    struct rusage children;
} UsageSnapshot;


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
   Mode names
   ========================================================= */

static const char *mode_name(
    AffinityMode mode
)
{
    switch (mode) {

        case AFFINITY_UNPINNED:
            return "UNPINNED";

        case AFFINITY_SAME_CPU:
            return "SAME-CPU";

        case AFFINITY_SEPARATE_CPU:
            return "SEPARATE-CPU";

        default:
            return "UNKNOWN";
    }
}


/* =========================================================
   timeval -> milliseconds
   ========================================================= */

static double timeval_ms(
    const struct timeval *value
)
{
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
            &snapshot->self
        ) != 0
    ) {
        perror(
            "getrusage self"
        );

        return -1;
    }


    if (
        getrusage(
            RUSAGE_CHILDREN,
            &snapshot->children
        ) != 0
    ) {
        perror(
            "getrusage children"
        );

        return -1;
    }


    return 0;
}


/* =========================================================
   Convert cumulative resource counters into one-trial deltas
   ========================================================= */

static void calculate_usage_delta(
    const UsageSnapshot *before,
    const UsageSnapshot *after,
    AffinityTrial *trial
)
{
    double before_user =
        timeval_ms(
            &before->self.ru_utime
        ) +
        timeval_ms(
            &before->children.ru_utime
        );


    double after_user =
        timeval_ms(
            &after->self.ru_utime
        ) +
        timeval_ms(
            &after->children.ru_utime
        );


    double before_sys =
        timeval_ms(
            &before->self.ru_stime
        ) +
        timeval_ms(
            &before->children.ru_stime
        );


    double after_sys =
        timeval_ms(
            &after->self.ru_stime
        ) +
        timeval_ms(
            &after->children.ru_stime
        );


    long before_voluntary =
        before->self.ru_nvcsw +
        before->children.ru_nvcsw;


    long after_voluntary =
        after->self.ru_nvcsw +
        after->children.ru_nvcsw;


    long before_involuntary =
        before->self.ru_nivcsw +
        before->children.ru_nivcsw;


    long after_involuntary =
        after->self.ru_nivcsw +
        after->children.ru_nivcsw;


    trial->user_ms =
        after_user -
        before_user;


    trial->sys_ms =
        after_sys -
        before_sys;


    trial->voluntary_ctx_switches =
        after_voluntary -
        before_voluntary;


    trial->involuntary_ctx_switches =
        after_involuntary -
        before_involuntary;
}


/* =========================================================
   Read integer from sysfs
   ========================================================= */

static int read_integer_file(
    const char *path,
    int *value
)
{
    FILE *file;

    if (
        !path ||
        !value
    ) {
        return -1;
    }


    file =
        fopen(
            path,
            "r"
        );


    if (!file) {
        return -1;
    }


    if (
        fscanf(
            file,
            "%d",
            value
        ) != 1
    ) {
        fclose(
            file
        );

        return -1;
    }


    fclose(
        file
    );


    return 0;
}


/* =========================================================
   CPU topology
   ========================================================= */

static int get_cpu_topology(
    int cpu,
    int *core_id,
    int *package_id
)
{
    char path[
        256
    ];


    snprintf(
        path,
        sizeof(path),
        "/sys/devices/system/cpu/cpu%d/topology/core_id",
        cpu
    );


    if (
        read_integer_file(
            path,
            core_id
        ) != 0
    ) {
        return -1;
    }


    snprintf(
        path,
        sizeof(path),
        "/sys/devices/system/cpu/cpu%d/topology/physical_package_id",
        cpu
    );


    if (
        read_integer_file(
            path,
            package_id
        ) != 0
    ) {
        return -1;
    }


    return 0;
}


/* =========================================================
   Select CPUs available to this process

   CPU A:
       first allowed logical CPU

   CPU B:
       preferably another allowed logical CPU on a
       different physical core.

   Fallback:
       any other allowed logical CPU.
   ========================================================= */

static int choose_experiment_cpus(
    int *cpu_a,
    int *cpu_b,
    int *different_physical_core
)
{
    cpu_set_t allowed;


    if (
        !cpu_a ||
        !cpu_b ||
        !different_physical_core
    ) {
        return -1;
    }


    CPU_ZERO(
        &allowed
    );


    if (
        sched_getaffinity(
            0,
            sizeof(allowed),
            &allowed
        ) != 0
    ) {
        perror(
            "sched_getaffinity"
        );

        return -1;
    }


    *cpu_a =
        -1;

    *cpu_b =
        -1;

    *different_physical_core =
        0;


    for (
        int cpu = 0;
        cpu < CPU_SETSIZE;
        ++cpu
    ) {

        if (
            CPU_ISSET(
                cpu,
                &allowed
            )
        ) {

            *cpu_a =
                cpu;

            break;
        }
    }


    if (
        *cpu_a < 0
    ) {

        fprintf(
            stderr,
            "No allowed CPU was found.\n"
        );

        return -1;
    }


    int first_core =
        -1;

    int first_package =
        -1;


    int topology_available =
        get_cpu_topology(
            *cpu_a,
            &first_core,
            &first_package
        ) == 0;


    /*
     * Prefer a CPU belonging to another physical core.
     */
    if (
        topology_available
    ) {

        for (
            int cpu = 0;
            cpu < CPU_SETSIZE;
            ++cpu
        ) {

            if (
                cpu ==
                *cpu_a
            ) {
                continue;
            }


            if (
                !CPU_ISSET(
                    cpu,
                    &allowed
                )
            ) {
                continue;
            }


            int core =
                -1;

            int package =
                -1;


            if (
                get_cpu_topology(
                    cpu,
                    &core,
                    &package
                ) != 0
            ) {
                continue;
            }


            if (
                package !=
                first_package ||
                core !=
                first_core
            ) {

                *cpu_b =
                    cpu;

                *different_physical_core =
                    1;

                break;
            }
        }
    }


    /*
     * Fallback: any second logical CPU.
     */
    if (
        *cpu_b < 0
    ) {

        for (
            int cpu = 0;
            cpu < CPU_SETSIZE;
            ++cpu
        ) {

            if (
                cpu ==
                *cpu_a
            ) {
                continue;
            }


            if (
                CPU_ISSET(
                    cpu,
                    &allowed
                )
            ) {

                *cpu_b =
                    cpu;

                break;
            }
        }
    }


    if (
        *cpu_b < 0
    ) {

        fprintf(
            stderr,
            "CPU affinity analysis requires at least "
            "two allowed logical CPUs.\n"
        );

        return -1;
    }


    return 0;
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

static double median_value(
    const double *values,
    size_t count
)
{
    if (
        !values ||
        count == 0
    ) {
        return 0.0;
    }


    double *copy =
        malloc(
            count *
            sizeof(double)
        );


    if (!copy) {
        return 0.0;
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


    double result;


    if (
        count %
        2 ==
        0
    ) {

        result =
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

        result =
            copy[
                count / 2
            ];
    }


    free(
        copy
    );


    return result;
}


/* =========================================================
   Trial runner
   ========================================================= */

static int run_one_trial(
    size_t total_bytes,
    size_t chunk_size,
    int producer_cpu,
    int consumer_cpu,
    AffinityTrial *trial
)
{
    UsageSnapshot before;
    UsageSnapshot after;

    BenchmarkResult result =
        {0};


    if (
        take_usage_snapshot(
            &before
        ) != 0
    ) {
        return -1;
    }


    if (
        run_shm_ring_benchmark_affinity(
            total_bytes,
            chunk_size,
            producer_cpu,
            consumer_cpu,
            &result
        ) != 0
    ) {
        return -1;
    }


    if (
        take_usage_snapshot(
            &after
        ) != 0
    ) {
        return -1;
    }


    trial->elapsed_ms =
        result.elapsed_ms;


    trial->throughput_mbps =
        result.throughput_mbps;


    calculate_usage_delta(
        &before,
        &after,
        trial
    );


    return 0;
}


/* =========================================================
   Main affinity analyzer
   ========================================================= */

int run_cpu_affinity_analysis(
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
            "Invalid CPU affinity analysis arguments.\n"
        );

        return -1;
    }


    if (
        ensure_results_directory() != 0
    ) {
        return -1;
    }


    int cpu_a =
        -1;

    int cpu_b =
        -1;

    int different_physical_core =
        0;


    if (
        choose_experiment_cpus(
            &cpu_a,
            &cpu_b,
            &different_physical_core
        ) != 0
    ) {
        return -1;
    }


    int producer_cpu[
        MODE_COUNT
    ] = {
        -1,
        cpu_a,
        cpu_a
    };


    int consumer_cpu[
        MODE_COUNT
    ] = {
        -1,
        cpu_a,
        cpu_b
    };


    AffinityTrial *data[
        MODE_COUNT
    ] = {
        NULL,
        NULL,
        NULL
    };


    for (
        size_t mode = 0;
        mode < MODE_COUNT;
        ++mode
    ) {

        data[mode] =
            calloc(
                trials,
                sizeof(
                    AffinityTrial
                )
            );


        if (!data[mode]) {

            perror(
                "calloc affinity trials"
            );


            for (
                size_t i = 0;
                i < MODE_COUNT;
                ++i
            ) {

                free(
                    data[i]
                );
            }


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
        "results/cpu_affinity_trials_%zuMB_%zuKB.csv",
        payload_mb,
        chunk_kb
    );


    snprintf(
        summary_path,
        sizeof(summary_path),
        "results/cpu_affinity_summary_%zuMB_%zuKB.csv",
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
            "fopen affinity trials"
        );


        for (
            size_t i = 0;
            i < MODE_COUNT;
            ++i
        ) {

            free(
                data[i]
            );
        }


        return -1;
    }


    fprintf(
        trials_file,
        "trial,"
        "execution_order,"
        "mode,"
        "producer_cpu,"
        "consumer_cpu,"
        "elapsed_ms,"
        "throughput_mbps,"
        "user_ms,"
        "system_ms,"
        "voluntary_ctx_switches,"
        "involuntary_ctx_switches\n"
    );


    printf(
        "\n"
        "FASTIPC-X CPU AFFINITY / SCHEDULER ANALYSIS\n"
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
        "Trials per mode       : %zu\n",
        trials
    );


    printf(
        "Production ring slots : 8\n\n"
    );


    printf(
        "Selected CPU topology\n"
        "------------------------------------------------------------\n"
    );


    printf(
        "Reference CPU         : CPU %d\n",
        cpu_a
    );


    printf(
        "Second CPU            : CPU %d\n",
        cpu_b
    );


    printf(
        "CPU relationship      : %s\n\n",
        different_physical_core
        ?
        "different physical cores"
        :
        "different logical CPUs"
    );


    printf(
        "Modes\n"
        "------------------------------------------------------------\n"
    );


    printf(
        "UNPINNED              : scheduler controlled\n"
    );


    printf(
        "SAME-CPU              : producer CPU %d, consumer CPU %d\n",
        cpu_a,
        cpu_a
    );


    printf(
        "SEPARATE-CPU          : producer CPU %d, consumer CPU %d\n\n",
        cpu_a,
        cpu_b
    );


    /*
     * Rotate execution order between trials:
     *
     * Trial 1: unpinned -> same -> separate
     * Trial 2: same -> separate -> unpinned
     * Trial 3: separate -> unpinned -> same
     *
     * This reduces systematic bias caused by always running
     * one mode first or last.
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
            order < MODE_COUNT;
            ++order
        ) {

            size_t mode =
                (
                    trial_index +
                    order
                ) %
                MODE_COUNT;


            if (
                run_one_trial(
                    total_bytes,
                    chunk_size,
                    producer_cpu[mode],
                    consumer_cpu[mode],
                    &data[mode][trial_index]
                ) != 0
            ) {

                fclose(
                    trials_file
                );


                for (
                    size_t i = 0;
                    i < MODE_COUNT;
                    ++i
                ) {

                    free(
                        data[i]
                    );
                }


                return -1;
            }


            printf(
                "  %-13s "
                "%8.3f ms  "
                "%10.3f MB/s  "
                "sys %8.3f ms  "
                "vol %ld\n",
                mode_name(
                    (AffinityMode)mode
                ),
                data[mode][trial_index].elapsed_ms,
                data[mode][trial_index].throughput_mbps,
                data[mode][trial_index].sys_ms,
                data[mode][trial_index].voluntary_ctx_switches
            );


            fprintf(
                trials_file,
                "%zu,%zu,%s,%d,%d,"
                "%.6f,%.6f,%.6f,%.6f,%ld,%ld\n",
                trial_index + 1,
                order + 1,
                mode_name(
                    (AffinityMode)mode
                ),
                producer_cpu[mode],
                consumer_cpu[mode],
                data[mode][trial_index].elapsed_ms,
                data[mode][trial_index].throughput_mbps,
                data[mode][trial_index].user_ms,
                data[mode][trial_index].sys_ms,
                data[mode][trial_index].voluntary_ctx_switches,
                data[mode][trial_index].involuntary_ctx_switches
            );
        }


        printf(
            "\n"
        );
    }


    fclose(
        trials_file
    );


    double median_ms[
        MODE_COUNT
    ];


    double median_throughput[
        MODE_COUNT
    ];


    double average_user[
        MODE_COUNT
    ];


    double average_sys[
        MODE_COUNT
    ];


    double average_voluntary[
        MODE_COUNT
    ];


    double average_involuntary[
        MODE_COUNT
    ];


    for (
        size_t mode = 0;
        mode < MODE_COUNT;
        ++mode
    ) {

        double *elapsed_values =
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
            !elapsed_values ||
            !throughput_values
        ) {

            free(
                elapsed_values
            );

            free(
                throughput_values
            );


            for (
                size_t i = 0;
                i < MODE_COUNT;
                ++i
            ) {

                free(
                    data[i]
                );
            }


            return -1;
        }


        double user_sum =
            0.0;


        double sys_sum =
            0.0;


        long voluntary_sum =
            0;


        long involuntary_sum =
            0;


        for (
            size_t trial = 0;
            trial < trials;
            ++trial
        ) {

            elapsed_values[trial] =
                data[mode][trial].elapsed_ms;


            throughput_values[trial] =
                data[mode][trial].throughput_mbps;


            user_sum +=
                data[mode][trial].user_ms;


            sys_sum +=
                data[mode][trial].sys_ms;


            voluntary_sum +=
                data[mode][trial].voluntary_ctx_switches;


            involuntary_sum +=
                data[mode][trial].involuntary_ctx_switches;
        }


        median_ms[mode] =
            median_value(
                elapsed_values,
                trials
            );


        median_throughput[mode] =
            median_value(
                throughput_values,
                trials
            );


        average_user[mode] =
            user_sum /
            (double)trials;


        average_sys[mode] =
            sys_sum /
            (double)trials;


        average_voluntary[mode] =
            (double)voluntary_sum /
            (double)trials;


        average_involuntary[mode] =
            (double)involuntary_sum /
            (double)trials;


        free(
            elapsed_values
        );


        free(
            throughput_values
        );
    }


    size_t best_mode =
        0;


    for (
        size_t mode = 1;
        mode < MODE_COUNT;
        ++mode
    ) {

        if (
            median_ms[mode] <
            median_ms[best_mode]
        ) {

            best_mode =
                mode;
        }
    }


    FILE *summary_file =
        fopen(
            summary_path,
            "w"
        );


    if (!summary_file) {

        perror(
            "fopen affinity summary"
        );


        for (
            size_t i = 0;
            i < MODE_COUNT;
            ++i
        ) {

            free(
                data[i]
            );
        }


        return -1;
    }


    fprintf(
        summary_file,
        "mode,"
        "producer_cpu,"
        "consumer_cpu,"
        "median_ms,"
        "median_throughput_mbps,"
        "average_user_ms,"
        "average_system_ms,"
        "average_voluntary_ctx_switches,"
        "average_involuntary_ctx_switches,"
        "selected\n"
    );


    for (
        size_t mode = 0;
        mode < MODE_COUNT;
        ++mode
    ) {

        fprintf(
            summary_file,
            "%s,%d,%d,"
            "%.6f,%.6f,%.6f,%.6f,%.2f,%.2f,%d\n",
            mode_name(
                (AffinityMode)mode
            ),
            producer_cpu[mode],
            consumer_cpu[mode],
            median_ms[mode],
            median_throughput[mode],
            average_user[mode],
            average_sys[mode],
            average_voluntary[mode],
            average_involuntary[mode],
            mode ==
            best_mode
            ?
            1
            :
            0
        );
    }


    fclose(
        summary_file
    );


    printf(
        "CPU AFFINITY SUMMARY\n"
        "============================================================\n"
    );


    printf(
        "%-14s %-12s %-12s %-12s %-12s %-10s\n",
        "Mode",
        "Median(ms)",
        "MB/s",
        "Sys(ms)",
        "Vol CS",
        "Selected"
    );


    printf(
        "------------------------------------------------------------\n"
    );


    for (
        size_t mode = 0;
        mode < MODE_COUNT;
        ++mode
    ) {

        printf(
            "%-14s %-12.3f %-12.3f %-12.3f %-12.2f %-10s\n",
            mode_name(
                (AffinityMode)mode
            ),
            median_ms[mode],
            median_throughput[mode],
            average_sys[mode],
            average_voluntary[mode],
            mode ==
            best_mode
            ?
            "YES"
            :
            ""
        );
    }


    double latency_change =
        0.0;


    double throughput_change =
        0.0;


    if (
        median_ms[AFFINITY_UNPINNED] >
        0.0
    ) {

        latency_change =
            (
                median_ms[AFFINITY_UNPINNED] -
                median_ms[best_mode]
            ) /
            median_ms[AFFINITY_UNPINNED] *
            100.0;
    }


    if (
        median_throughput[AFFINITY_UNPINNED] >
        0.0
    ) {

        throughput_change =
            (
                median_throughput[best_mode] -
                median_throughput[AFFINITY_UNPINNED]
            ) /
            median_throughput[AFFINITY_UNPINNED] *
            100.0;
    }


    printf(
        "\nBest measured mode     : %s\n",
        mode_name(
            (AffinityMode)best_mode
        )
    );


    if (
        best_mode ==
        AFFINITY_UNPINNED
    ) {

        printf(
            "Scheduler decision     : Linux scheduler-controlled "
            "placement performed best.\n"
        );


        printf(
            "Production decision    : KEEP UNPINNED\n"
        );
    }
    else {

        printf(
            "Latency change vs free : %.2f%% improvement\n",
            latency_change
        );


        printf(
            "Throughput change      : %+.2f%%\n",
            throughput_change
        );


        printf(
            "Production decision    : EXPERIMENTAL CANDIDATE ONLY\n"
        );


        printf(
            "                         Production remains unpinned "
            "for portability.\n"
        );
    }


    printf(
        "\nTrials CSV            : %s\n",
        trials_path
    );


    printf(
        "Summary CSV           : %s\n",
        summary_path
    );


    for (
        size_t i = 0;
        i < MODE_COUNT;
        ++i
    ) {

        free(
            data[i]
        );
    }


    return 0;
}
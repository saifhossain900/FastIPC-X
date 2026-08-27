#include "../include/shm_ring_slot_optimizer.h"

#include <errno.h>
#include <fcntl.h>
#include <semaphore.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>

#include <time.h>
#include <unistd.h>


#define SLOT_OPTION_COUNT 6
#define CURRENT_RING_SLOTS 8


/* =========================================================
   Trial / summary structures
   ========================================================= */

typedef struct {
    sem_t start_sem;
    sem_t empty_slots;
    sem_t filled_slots;

    size_t write_index;
    size_t read_index;
} SlotRingHeader;


typedef struct {
    double elapsed_ms;
    double throughput_mbps;

    double system_cpu_ms;
    double voluntary_ctx_switches;
} SlotTrialResult;


typedef struct {
    size_t slot_count;

    double median_ms;
    double median_throughput_mbps;

    double average_system_cpu_ms;
    double average_voluntary_ctx_switches;
} SlotSummary;


/* =========================================================
   Result directory
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
        perror("mkdir results");
        return -1;
    }

    return 0;
}


/* =========================================================
   Semaphore helper
   ========================================================= */

static int wait_sem(sem_t *sem)
{
    while (
        sem_wait(sem) != 0
    ) {

        if (errno == EINTR) {
            continue;
        }

        return -1;
    }

    return 0;
}


/* =========================================================
   Resource measurement helpers
   ========================================================= */

static double timeval_to_ms(
    struct timeval value
)
{
    return
        ((double)value.tv_sec * 1000.0) +
        ((double)value.tv_usec / 1000.0);
}


static double rusage_system_ms(
    const struct rusage *before,
    const struct rusage *after
)
{
    return
        timeval_to_ms(
            after->ru_stime
        ) -
        timeval_to_ms(
            before->ru_stime
        );
}


static long rusage_nvcsw_delta(
    const struct rusage *before,
    const struct rusage *after
)
{
    return
        after->ru_nvcsw -
        before->ru_nvcsw;
}


static double elapsed_ms_between(
    const struct timespec *start,
    const struct timespec *end
)
{
    double seconds =
        (double)(
            end->tv_sec -
            start->tv_sec
        );

    double nanoseconds =
        (double)(
            end->tv_nsec -
            start->tv_nsec
        );

    return
        (seconds * 1000.0) +
        (nanoseconds / 1000000.0);
}


/* =========================================================
   Median helpers
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


static double median_of(
    const double values[],
    size_t count
)
{
    double *copy =
        malloc(
            count *
            sizeof(*copy)
        );


    if (!copy) {
        return -1.0;
    }


    memcpy(
        copy,
        values,
        count * sizeof(*copy)
    );


    qsort(
        copy,
        count,
        sizeof(*copy),
        compare_double
    );


    double result;


    if (
        (count % 2U) == 0U
    ) {

        result =
            (
                copy[
                    (count / 2U) - 1U
                ] +
                copy[
                    count / 2U
                ]
            ) /
            2.0;
    }
    else {

        result =
            copy[
                count / 2U
            ];
    }


    free(copy);

    return result;
}


/* =========================================================
   Dynamic shared-memory layout

   Memory layout:

   [header]
   [length array: one size_t per slot]
   [slot 0 data]
   [slot 1 data]
   ...
   ========================================================= */

static size_t *ring_lengths(
    SlotRingHeader *ring
)
{
    return
        (size_t *)(
            (unsigned char *)ring +
            sizeof(*ring)
        );
}


static unsigned char *ring_data(
    SlotRingHeader *ring,
    size_t slots
)
{
    return
        (unsigned char *)(
            ring_lengths(ring) +
            slots
        );
}


/* =========================================================
   Source data

   The source buffer is prepared before the timed region.
   Therefore data generation does not contaminate the
   transfer benchmark.
   ========================================================= */

static void fill_source(
    unsigned char *buffer,
    size_t length
)
{
    for (
        size_t i = 0;
        i < length;
        ++i
    ) {

        buffer[i] =
            (unsigned char)(
                (
                    (i * 131U) +
                    17U
                ) &
                0xFFU
            );
    }
}


/* =========================================================
   Consumer memory access

   The receiver touches every cache line so the consumer
   genuinely observes transferred shared-memory data.

   This is intentionally much lighter than full checksum
   verification because integrity checking has its own
   separate FastIPC-X command.
   ========================================================= */

static void touch_slot_data(
    const unsigned char *data,
    size_t length,
    volatile uint64_t *sink
)
{
    for (
        size_t i = 0;
        i < length;
        i += 64U
    ) {

        *sink ^=
            (uint64_t)data[i];
    }


    if (length > 0) {

        *sink ^=
            (uint64_t)data[
                length - 1U
            ];
    }
}


/* =========================================================
   Run one ring-buffer trial
   ========================================================= */

static int run_ring_trial(
    size_t total_bytes,
    size_t chunk_size,
    size_t slots,
    SlotTrialResult *result
)
{
    if (
        slots == 0 ||
        chunk_size == 0 ||
        total_bytes == 0 ||
        result == NULL
    ) {
        return -1;
    }


    /* -----------------------------------------------------
       Overflow protection
       ----------------------------------------------------- */

    if (
        slots >
        SIZE_MAX /
        sizeof(size_t)
    ) {
        return -1;
    }


    size_t lengths_bytes =
        slots *
        sizeof(size_t);


    if (
        slots >
        SIZE_MAX /
        chunk_size
    ) {
        return -1;
    }


    size_t data_bytes =
        slots *
        chunk_size;


    if (
        sizeof(SlotRingHeader) >
        SIZE_MAX -
        lengths_bytes
    ) {
        return -1;
    }


    if (
        sizeof(SlotRingHeader) +
        lengths_bytes >
        SIZE_MAX -
        data_bytes
    ) {
        return -1;
    }


    size_t map_size =
        sizeof(SlotRingHeader) +
        lengths_bytes +
        data_bytes;


    /* -----------------------------------------------------
       Prepare source data outside timed region
       ----------------------------------------------------- */

    unsigned char *source =
        malloc(chunk_size);


    if (!source) {
        return -1;
    }


    fill_source(
        source,
        chunk_size
    );


    /* -----------------------------------------------------
       POSIX shared memory
       ----------------------------------------------------- */

    char shm_name[128];


    snprintf(
        shm_name,
        sizeof(shm_name),
        "/fastipcx_slotopt_%ld",
        (long)getpid()
    );


    shm_unlink(shm_name);


    int shm_fd =
        shm_open(
            shm_name,
            O_CREAT |
            O_EXCL |
            O_RDWR,
            0600
        );


    if (shm_fd < 0) {

        perror(
            "shm_open slot optimizer"
        );

        free(source);

        return -1;
    }


    if (
        ftruncate(
            shm_fd,
            (off_t)map_size
        ) != 0
    ) {

        perror(
            "ftruncate slot optimizer"
        );

        close(shm_fd);

        shm_unlink(
            shm_name
        );

        free(source);

        return -1;
    }


    SlotRingHeader *ring =
        mmap(
            NULL,
            map_size,
            PROT_READ |
            PROT_WRITE,
            MAP_SHARED,
            shm_fd,
            0
        );


    close(shm_fd);


    if (
        ring == MAP_FAILED
    ) {

        perror(
            "mmap slot optimizer"
        );

        shm_unlink(
            shm_name
        );

        free(source);

        return -1;
    }


    /*
     * Remove the shared-memory name immediately.
     * Existing mappings remain valid.
     */
    shm_unlink(
        shm_name
    );


    memset(
        ring,
        0,
        map_size
    );


    /* -----------------------------------------------------
       Process-shared synchronization
       ----------------------------------------------------- */

    if (
        sem_init(
            &ring->start_sem,
            1,
            0
        ) != 0
    ) {

        perror(
            "sem_init start"
        );

        munmap(
            ring,
            map_size
        );

        free(source);

        return -1;
    }


    if (
        sem_init(
            &ring->empty_slots,
            1,
            (unsigned int)slots
        ) != 0
    ) {

        perror(
            "sem_init empty slots"
        );

        sem_destroy(
            &ring->start_sem
        );

        munmap(
            ring,
            map_size
        );

        free(source);

        return -1;
    }


    if (
        sem_init(
            &ring->filled_slots,
            1,
            0
        ) != 0
    ) {

        perror(
            "sem_init filled slots"
        );

        sem_destroy(
            &ring->start_sem
        );

        sem_destroy(
            &ring->empty_slots
        );

        munmap(
            ring,
            map_size
        );

        free(source);

        return -1;
    }


    /* -----------------------------------------------------
       Fork consumer
       ----------------------------------------------------- */

    pid_t child =
        fork();


    if (child < 0) {

        perror(
            "fork slot optimizer"
        );

        sem_destroy(
            &ring->start_sem
        );

        sem_destroy(
            &ring->empty_slots
        );

        sem_destroy(
            &ring->filled_slots
        );

        munmap(
            ring,
            map_size
        );

        free(source);

        return -1;
    }


    /* =====================================================
       CHILD = CONSUMER
       ===================================================== */

    if (child == 0) {

        volatile uint64_t sink = 0;

        int ok = 1;


        /*
         * Child waits until parent begins timed region.
         */
        if (
            wait_sem(
                &ring->start_sem
            ) != 0
        ) {
            _exit(2);
        }


        size_t *lengths =
            ring_lengths(ring);


        unsigned char *data =
            ring_data(
                ring,
                slots
            );


        for (;;) {

            if (
                wait_sem(
                    &ring->filled_slots
                ) != 0
            ) {

                ok = 0;

                break;
            }


            size_t index =
                ring->read_index;


            size_t length =
                lengths[index];


            /*
             * Zero length is the termination marker.
             */
            if (length > 0) {

                const unsigned char *slot =
                    data +
                    (
                        index *
                        chunk_size
                    );


                touch_slot_data(
                    slot,
                    length,
                    &sink
                );
            }


            ring->read_index =
                (
                    index + 1U
                ) %
                slots;


            if (
                sem_post(
                    &ring->empty_slots
                ) != 0
            ) {

                ok = 0;

                break;
            }


            if (length == 0) {
                break;
            }
        }


        /*
         * Keep volatile sink alive.
         */
        (void)sink;


        _exit(
            ok ? 0 : 3
        );
    }


    /* =====================================================
       PARENT = PRODUCER
       ===================================================== */

    struct rusage self_before;
    struct rusage self_after;

    struct rusage children_before;
    struct rusage children_after;

    struct timespec start_time;
    struct timespec end_time;


    int ok = 1;


    /*
     * Snapshot cumulative resource counters BEFORE
     * the timed transfer.
     */
    if (
        getrusage(
            RUSAGE_SELF,
            &self_before
        ) != 0 ||
        getrusage(
            RUSAGE_CHILDREN,
            &children_before
        ) != 0 ||
        clock_gettime(
            CLOCK_MONOTONIC,
            &start_time
        ) != 0
    ) {

        ok = 0;
    }


    /*
     * Release consumer only after timing starts.
     */
    if (
        ok &&
        sem_post(
            &ring->start_sem
        ) != 0
    ) {

        ok = 0;
    }


    size_t *lengths =
        ring_lengths(ring);


    unsigned char *data =
        ring_data(
            ring,
            slots
        );


    size_t transferred = 0;


    /* -----------------------------------------------------
       Producer transfer loop
       ----------------------------------------------------- */

    while (
        ok &&
        transferred <
        total_bytes
    ) {

        size_t remaining =
            total_bytes -
            transferred;


        size_t current =
            remaining <
            chunk_size
            ?
            remaining
            :
            chunk_size;


        if (
            wait_sem(
                &ring->empty_slots
            ) != 0
        ) {

            ok = 0;

            break;
        }


        size_t index =
            ring->write_index;


        unsigned char *slot =
            data +
            (
                index *
                chunk_size
            );


        memcpy(
            slot,
            source,
            current
        );


        lengths[index] =
            current;


        ring->write_index =
            (
                index + 1U
            ) %
            slots;


        if (
            sem_post(
                &ring->filled_slots
            ) != 0
        ) {

            ok = 0;

            break;
        }


        transferred +=
            current;
    }


    /* -----------------------------------------------------
       Send zero-length termination slot
       ----------------------------------------------------- */

    if (ok) {

        if (
            wait_sem(
                &ring->empty_slots
            ) != 0
        ) {

            ok = 0;
        }
        else {

            size_t index =
                ring->write_index;


            lengths[index] =
                0;


            ring->write_index =
                (
                    index + 1U
                ) %
                slots;


            if (
                sem_post(
                    &ring->filled_slots
                ) != 0
            ) {

                ok = 0;
            }
        }
    }


    /* -----------------------------------------------------
       Wait for consumer
       ----------------------------------------------------- */

    int status = 0;


    if (!ok) {

        /*
         * Prevent deadlock if producer fails.
         */
        kill(
            child,
            SIGKILL
        );
    }


    if (
        waitpid(
            child,
            &status,
            0
        ) < 0
    ) {

        ok = 0;
    }
    else if (
        !WIFEXITED(status) ||
        WEXITSTATUS(status) != 0
    ) {

        ok = 0;
    }


    /* -----------------------------------------------------
       Final timing/resource snapshots
       ----------------------------------------------------- */

    if (
        clock_gettime(
            CLOCK_MONOTONIC,
            &end_time
        ) != 0 ||
        getrusage(
            RUSAGE_SELF,
            &self_after
        ) != 0 ||
        getrusage(
            RUSAGE_CHILDREN,
            &children_after
        ) != 0
    ) {

        ok = 0;
    }


    /* -----------------------------------------------------
       Calculate trial metrics
       ----------------------------------------------------- */

    if (ok) {

        double elapsed_ms =
            elapsed_ms_between(
                &start_time,
                &end_time
            );


        double self_sys =
            rusage_system_ms(
                &self_before,
                &self_after
            );


        double child_sys =
            rusage_system_ms(
                &children_before,
                &children_after
            );


        long self_nvcsw =
            rusage_nvcsw_delta(
                &self_before,
                &self_after
            );


        long child_nvcsw =
            rusage_nvcsw_delta(
                &children_before,
                &children_after
            );


        result->elapsed_ms =
            elapsed_ms;


        result->throughput_mbps =
            (
                (double)total_bytes /
                (
                    1024.0 *
                    1024.0
                )
            ) /
            (
                elapsed_ms /
                1000.0
            );


        result->system_cpu_ms =
            self_sys +
            child_sys;


        result->voluntary_ctx_switches =
            (double)(
                self_nvcsw +
                child_nvcsw
            );
    }


    /* -----------------------------------------------------
       Cleanup
       ----------------------------------------------------- */

    sem_destroy(
        &ring->start_sem
    );

    sem_destroy(
        &ring->empty_slots
    );

    sem_destroy(
        &ring->filled_slots
    );


    munmap(
        ring,
        map_size
    );


    free(source);


    return
        ok ? 0 : -1;
}


/* =========================================================
   Summarize all trials for one slot count
   ========================================================= */

static int summarize_slot(
    size_t slot_count,
    const SlotTrialResult trials_data[],
    size_t trials,
    SlotSummary *summary
)
{
    double *elapsed =
        malloc(
            trials *
            sizeof(*elapsed)
        );


    double *throughput =
        malloc(
            trials *
            sizeof(*throughput)
        );


    if (
        !elapsed ||
        !throughput
    ) {

        free(elapsed);
        free(throughput);

        return -1;
    }


    double sys_total = 0.0;
    double vcs_total = 0.0;


    for (
        size_t i = 0;
        i < trials;
        ++i
    ) {

        elapsed[i] =
            trials_data[i].
            elapsed_ms;


        throughput[i] =
            trials_data[i].
            throughput_mbps;


        sys_total +=
            trials_data[i].
            system_cpu_ms;


        vcs_total +=
            trials_data[i].
            voluntary_ctx_switches;
    }


    summary->slot_count =
        slot_count;


    summary->median_ms =
        median_of(
            elapsed,
            trials
        );


    summary->median_throughput_mbps =
        median_of(
            throughput,
            trials
        );


    summary->average_system_cpu_ms =
        sys_total /
        (double)trials;


    summary->average_voluntary_ctx_switches =
        vcs_total /
        (double)trials;


    free(elapsed);
    free(throughput);


    if (
        summary->median_ms < 0.0 ||
        summary->median_throughput_mbps < 0.0
    ) {
        return -1;
    }


    return 0;
}


/* =========================================================
   Save raw trials
   ========================================================= */

static int save_trial_csv(
    size_t payload_mb,
    size_t chunk_kb,
    const size_t slot_options[],
    SlotTrialResult *all_trials,
    size_t trials
)
{
    char path[256];


    snprintf(
        path,
        sizeof(path),
        "results/"
        "shm_ring_slot_trials_%zuMB_%zuKB.csv",
        payload_mb,
        chunk_kb
    );


    FILE *f =
        fopen(
            path,
            "w"
        );


    if (!f) {

        perror(
            "fopen slot trial csv"
        );

        return -1;
    }


    fprintf(
        f,
        "slot_count,"
        "trial,"
        "elapsed_ms,"
        "throughput_mbps,"
        "system_cpu_ms,"
        "voluntary_ctx_switches\n"
    );


    for (
        size_t s = 0;
        s < SLOT_OPTION_COUNT;
        ++s
    ) {

        for (
            size_t t = 0;
            t < trials;
            ++t
        ) {

            SlotTrialResult *result =
                &all_trials[
                    (s * trials) +
                    t
                ];


            fprintf(
                f,
                "%zu,%zu,"
                "%.6f,%.6f,"
                "%.6f,%.2f\n",
                slot_options[s],
                t + 1U,
                result->elapsed_ms,
                result->throughput_mbps,
                result->system_cpu_ms,
                result->voluntary_ctx_switches
            );
        }
    }


    fclose(f);


    printf(
        "Raw trial CSV        : %s\n",
        path
    );


    return 0;
}


/* =========================================================
   Save slot summary
   ========================================================= */

static int save_summary_csv(
    size_t payload_mb,
    size_t chunk_kb,
    const SlotSummary summaries[],
    size_t best_index
)
{
    char path[256];


    snprintf(
        path,
        sizeof(path),
        "results/"
        "shm_ring_slot_summary_%zuMB_%zuKB.csv",
        payload_mb,
        chunk_kb
    );


    FILE *f =
        fopen(
            path,
            "w"
        );


    if (!f) {

        perror(
            "fopen slot summary csv"
        );

        return -1;
    }


    fprintf(
        f,
        "slot_count,"
        "median_ms,"
        "median_throughput_mbps,"
        "average_system_cpu_ms,"
        "average_voluntary_ctx_switches,"
        "selected\n"
    );


    for (
        size_t i = 0;
        i < SLOT_OPTION_COUNT;
        ++i
    ) {

        fprintf(
            f,
            "%zu,"
            "%.6f,"
            "%.6f,"
            "%.6f,"
            "%.2f,"
            "%d\n",
            summaries[i].slot_count,
            summaries[i].median_ms,
            summaries[i].
            median_throughput_mbps,
            summaries[i].
            average_system_cpu_ms,
            summaries[i].
            average_voluntary_ctx_switches,
            i == best_index ? 1 : 0
        );
    }


    fclose(f);


    printf(
        "Summary CSV          : %s\n",
        path
    );


    return 0;
}


/* =========================================================
   Public slot optimizer
   ========================================================= */

int run_shm_ring_slot_optimizer(
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
            "Payload, chunk size, and trials "
            "must be greater than zero.\n"
        );

        return -1;
    }


    if (
        ensure_results_directory() != 0
    ) {
        return -1;
    }


    /*
     * Controlled slot-count search space.
     */
    const size_t slot_options[
        SLOT_OPTION_COUNT
    ] = {
        1,
        2,
        4,
        8,
        16,
        32
    };


    /* -----------------------------------------------------
       Allocation overflow protection
       ----------------------------------------------------- */

    if (
        trials >
        SIZE_MAX /
        SLOT_OPTION_COUNT
    ) {

        fprintf(
            stderr,
            "Trial count is too large.\n"
        );

        return -1;
    }


    if (
        (
            SLOT_OPTION_COUNT *
            trials
        ) >
        SIZE_MAX /
        sizeof(SlotTrialResult)
    ) {

        fprintf(
            stderr,
            "Trial count is too large.\n"
        );

        return -1;
    }


    SlotTrialResult *all_trials =
        calloc(
            SLOT_OPTION_COUNT *
            trials,
            sizeof(*all_trials)
        );


    if (!all_trials) {
        return -1;
    }


    SlotSummary summaries[
        SLOT_OPTION_COUNT
    ];


    memset(
        summaries,
        0,
        sizeof(summaries)
    );


    size_t payload_mb =
        total_bytes /
        (
            1024U *
            1024U
        );


    size_t chunk_kb =
        chunk_size /
        1024U;


    printf(
        "\n"
        "SHM RING SLOT OPTIMIZATION\n"
        "============================================================\n"
    );


    printf(
        "Payload             : %zu MB\n",
        payload_mb
    );


    printf(
        "Chunk size          : %zu KB\n",
        chunk_kb
    );


    printf(
        "Trials per setting  : %zu\n",
        trials
    );


    printf(
        "Slot counts         : "
        "1, 2, 4, 8, 16, 32\n"
    );


    printf(
        "Current ring slots  : %d\n\n",
        CURRENT_RING_SLOTS
    );


    /* =====================================================
       Test every slot count
       ===================================================== */

    for (
        size_t s = 0;
        s < SLOT_OPTION_COUNT;
        ++s
    ) {

        size_t slots =
            slot_options[s];


        printf(
            "Testing %zu slot%s...\n",
            slots,
            slots == 1
            ? ""
            : "s"
        );


        for (
            size_t t = 0;
            t < trials;
            ++t
        ) {

            SlotTrialResult *trial =
                &all_trials[
                    (s * trials) +
                    t
                ];


            if (
                run_ring_trial(
                    total_bytes,
                    chunk_size,
                    slots,
                    trial
                ) != 0
            ) {

                fprintf(
                    stderr,
                    "Ring slot trial failed "
                    "for %zu slots, trial %zu.\n",
                    slots,
                    t + 1U
                );


                free(
                    all_trials
                );

                return -1;
            }


            printf(
                "  Trial %zu: "
                "%.3f ms, "
                "%.3f MB/s\n",
                t + 1U,
                trial->elapsed_ms,
                trial->throughput_mbps
            );
        }


        if (
            summarize_slot(
                slots,
                &all_trials[
                    s * trials
                ],
                trials,
                &summaries[s]
            ) != 0
        ) {

            free(
                all_trials
            );

            return -1;
        }
    }


    /* =====================================================
       Find lowest median latency
       ===================================================== */

    size_t best_index = 0;

    size_t current_index = 0;


    for (
        size_t i = 0;
        i < SLOT_OPTION_COUNT;
        ++i
    ) {

        if (
            summaries[i].
            median_ms <
            summaries[
                best_index
            ].median_ms
        ) {

            best_index = i;
        }


        if (
            summaries[i].
            slot_count ==
            CURRENT_RING_SLOTS
        ) {

            current_index = i;
        }
    }


    /* =====================================================
       Print final comparison
       ===================================================== */

    printf(
        "\n"
        "%-8s "
        "%-12s "
        "%-14s "
        "%-12s "
        "%-12s\n",
        "Slots",
        "Median ms",
        "Median MB/s",
        "Sys CPU ms",
        "Vol CS"
    );


    printf(
        "---------------------------------------------------------------\n"
    );


    for (
        size_t i = 0;
        i < SLOT_OPTION_COUNT;
        ++i
    ) {

        printf(
            "%-8zu "
            "%-12.3f "
            "%-14.3f "
            "%-12.3f "
            "%-12.2f"
            "%s%s\n",
            summaries[i].
            slot_count,
            summaries[i].
            median_ms,
            summaries[i].
            median_throughput_mbps,
            summaries[i].
            average_system_cpu_ms,
            summaries[i].
            average_voluntary_ctx_switches,
            i == current_index
            ? "  [current]"
            : "",
            i == best_index
            ? "  <- best"
            : ""
        );
    }


    const SlotSummary *best =
        &summaries[
            best_index
        ];


    const SlotSummary *current =
        &summaries[
            current_index
        ];


    double latency_improvement =
        (
            (
                current->median_ms -
                best->median_ms
            ) /
            current->median_ms
        ) *
        100.0;


    double throughput_improvement =
        (
            (
                best->
                median_throughput_mbps -
                current->
                median_throughput_mbps
            ) /
            current->
            median_throughput_mbps
        ) *
        100.0;


    printf(
        "\n"
        "Best measured slot count: %zu\n",
        best->slot_count
    );


    printf(
        "Current production count : %d\n",
        CURRENT_RING_SLOTS
    );


    printf(
        "Latency improvement vs 8 : %.2f%%\n",
        latency_improvement
    );


    printf(
        "Throughput change vs 8   : %.2f%%\n",
        throughput_improvement
    );


    /*
     * Do not automatically modify production configuration.
     */
    if (
        best->slot_count ==
        CURRENT_RING_SLOTS
    ) {

        printf(
            "Recommendation           : "
            "Keep the current 8-slot configuration.\n"
        );
    }
    else {

        printf(
            "Recommendation           : "
            "%zu slots is the measured candidate;\n",
            best->slot_count
        );


        printf(
            "                           "
            "validate with repeated runs "
            "before changing production.\n"
        );
    }


    /* =====================================================
       Save evidence
       ===================================================== */

    int save_ok = 0;


    if (
        save_trial_csv(
            payload_mb,
            chunk_kb,
            slot_options,
            all_trials,
            trials
        ) != 0
    ) {

        save_ok = -1;
    }


    if (
        save_summary_csv(
            payload_mb,
            chunk_kb,
            summaries,
            best_index
        ) != 0
    ) {

        save_ok = -1;
    }


    free(
        all_trials
    );


    return save_ok;
}
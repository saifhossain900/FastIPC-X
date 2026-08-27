#define _GNU_SOURCE

#include "../include/shm_ring_ipc.h"

#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <semaphore.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>

#include <time.h>
#include <unistd.h>


/*
 * Production SHM-RING slot count.
 *
 * The earlier ring-slot experiment tested alternative depths,
 * but production validation showed that the 8-slot version
 * remained faster in the real implementation.
 */
#define RING_SLOTS 8


/* =========================================================
   Shared ring control structure
   ========================================================= */

struct ring_header {
    sem_t empty_slots;
    sem_t filled_slots;

    size_t head;
    size_t tail;

    size_t slot_size;
    size_t remaining_bytes;

    /*
     * Slot storage immediately follows this structure:
     *
     * slot_size * RING_SLOTS bytes
     */
};


/* =========================================================
   In-memory data movement
   ========================================================= */

static ssize_t write_all_mem(
    void *dest,
    const void *src,
    size_t count
)
{
    memcpy(
        dest,
        src,
        count
    );

    return (ssize_t)count;
}


static ssize_t read_all_mem(
    void *dest,
    const void *src,
    size_t count
)
{
    memcpy(
        dest,
        src,
        count
    );

    return (ssize_t)count;
}


/* =========================================================
   EINTR-safe semaphore wait
   ========================================================= */

static int sem_wait_intr(
    sem_t *sem
)
{
    while (1) {

        if (
            sem_wait(
                sem
            ) == 0
        ) {
            return 0;
        }

        if (
            errno == EINTR
        ) {
            continue;
        }

        return -1;
    }
}


/* =========================================================
   CPU affinity helper
   ========================================================= */

static int pin_current_process_to_cpu(
    int cpu
)
{
    cpu_set_t set;

    if (cpu < 0) {
        return 0;
    }

    CPU_ZERO(
        &set
    );

    CPU_SET(
        cpu,
        &set
    );

    if (
        sched_setaffinity(
            0,
            sizeof(set),
            &set
        ) != 0
    ) {
        return -1;
    }

    return 0;
}


/* =========================================================
   Restore original parent affinity
   ========================================================= */

static int restore_affinity(
    const cpu_set_t *original_affinity
)
{
    if (!original_affinity) {
        return -1;
    }

    if (
        sched_setaffinity(
            0,
            sizeof(*original_affinity),
            original_affinity
        ) != 0
    ) {
        return -1;
    }

    return 0;
}


/* =========================================================
   Emergency child cleanup
   ========================================================= */

static void terminate_child(
    pid_t pid
)
{
    if (pid <= 0) {
        return;
    }

    if (
        kill(
            pid,
            SIGTERM
        ) != 0 &&
        errno != ESRCH
    ) {
        perror(
            "kill"
        );
    }

    while (
        waitpid(
            pid,
            NULL,
            0
        ) < 0
    ) {
        if (
            errno == EINTR
        ) {
            continue;
        }

        break;
    }
}


/* =========================================================
   Internal production SHM-RING implementation

   The only experimental difference is optional process
   affinity. The actual ring-buffer algorithm, slot count,
   semaphores and memcpy data path remain the same.
   ========================================================= */

static int run_shm_ring_internal(
    size_t total_bytes,
    size_t chunk_size,
    int producer_cpu,
    int consumer_cpu,
    BenchmarkResult *result
)
{
    char name[
        256
    ];

    struct timespec ts;

    cpu_set_t original_affinity;

    int original_affinity_valid =
        0;

    int parent_affinity_changed =
        0;


    if (
        total_bytes == 0 ||
        chunk_size == 0 ||
        !result
    ) {
        fprintf(
            stderr,
            "Invalid SHM-RING benchmark arguments.\n"
        );

        return -1;
    }


    /*
     * Save the parent's scheduler affinity before changing it.
     *
     * This is essential because every experimental trial must
     * start from the same scheduler state.
     */
    if (
        sched_getaffinity(
            0,
            sizeof(original_affinity),
            &original_affinity
        ) != 0
    ) {
        perror(
            "sched_getaffinity"
        );

        return -1;
    }

    original_affinity_valid =
        1;


    clock_gettime(
        CLOCK_REALTIME,
        &ts
    );


    snprintf(
        name,
        sizeof(name),
        "/fastipc_shm_ring_%d_%ld_%ld",
        (int)getpid(),
        ts.tv_sec,
        ts.tv_nsec
    );


    size_t header_size =
        sizeof(
            struct ring_header
        );


    size_t slot_area =
        (size_t)RING_SLOTS *
        chunk_size;


    size_t map_size =
        header_size +
        slot_area;


    int fd =
        shm_open(
            name,
            O_CREAT |
            O_EXCL |
            O_RDWR,
            0600
        );


    if (fd < 0) {

        perror(
            "shm_open"
        );

        return -1;
    }


    if (
        ftruncate(
            fd,
            (off_t)map_size
        ) < 0
    ) {

        perror(
            "ftruncate"
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
            map_size,
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
            "mmap"
        );

        shm_unlink(
            name
        );

        close(
            fd
        );

        return -1;
    }


    struct ring_header *hdr =
        (struct ring_header *)map;


    hdr->head =
        0;

    hdr->tail =
        0;

    hdr->slot_size =
        chunk_size;

    hdr->remaining_bytes =
        total_bytes;


    if (
        sem_init(
            &hdr->empty_slots,
            1,
            RING_SLOTS
        ) != 0
    ) {

        perror(
            "sem_init empty"
        );

        munmap(
            map,
            map_size
        );

        shm_unlink(
            name
        );

        close(
            fd
        );

        return -1;
    }


    if (
        sem_init(
            &hdr->filled_slots,
            1,
            0
        ) != 0
    ) {

        perror(
            "sem_init filled"
        );

        sem_destroy(
            &hdr->empty_slots
        );

        munmap(
            map,
            map_size
        );

        shm_unlink(
            name
        );

        close(
            fd
        );

        return -1;
    }


    pid_t pid =
        fork();


    if (pid < 0) {

        perror(
            "fork"
        );

        sem_destroy(
            &hdr->empty_slots
        );

        sem_destroy(
            &hdr->filled_slots
        );

        munmap(
            map,
            map_size
        );

        shm_unlink(
            name
        );

        close(
            fd
        );

        return -1;
    }


    char *slots_base =
        (char *)map +
        header_size;


    char *local_buf =
        malloc(
            chunk_size
        );


    if (!local_buf) {

        perror(
            "malloc"
        );


        if (pid == 0) {

            munmap(
                map,
                map_size
            );

            close(
                fd
            );

            _exit(
                7
            );
        }


        terminate_child(
            pid
        );

        sem_destroy(
            &hdr->empty_slots
        );

        sem_destroy(
            &hdr->filled_slots
        );

        munmap(
            map,
            map_size
        );

        shm_unlink(
            name
        );

        close(
            fd
        );

        return -1;
    }


    /* =====================================================
       CHILD = CONSUMER
       ===================================================== */

    if (pid == 0) {

        close(
            fd
        );


        if (
            consumer_cpu >= 0
        ) {

            if (
                pin_current_process_to_cpu(
                    consumer_cpu
                ) != 0
            ) {

                perror(
                    "sched_setaffinity consumer"
                );

                free(
                    local_buf
                );

                munmap(
                    map,
                    map_size
                );

                _exit(
                    6
                );
            }
        }


        size_t received =
            0;


        while (
            received <
            total_bytes
        ) {

            if (
                sem_wait_intr(
                    &hdr->filled_slots
                ) != 0
            ) {

                perror(
                    "sem_wait filled"
                );

                free(
                    local_buf
                );

                munmap(
                    map,
                    map_size
                );

                _exit(
                    2
                );
            }


            size_t idx =
                hdr->tail %
                RING_SLOTS;


            char *slot =
                slots_base +
                idx *
                chunk_size;


            size_t remaining =
                total_bytes -
                received;


            size_t want =
                remaining <
                chunk_size
                ?
                remaining
                :
                chunk_size;


            if (
                read_all_mem(
                    local_buf,
                    slot,
                    want
                ) < 0
            ) {

                perror(
                    "read mem"
                );

                free(
                    local_buf
                );

                munmap(
                    map,
                    map_size
                );

                _exit(
                    3
                );
            }


            received +=
                want;


            hdr->tail =
                (
                    hdr->tail +
                    1
                ) %
                RING_SLOTS;


            if (
                sem_post(
                    &hdr->empty_slots
                ) != 0
            ) {

                perror(
                    "sem_post empty"
                );

                free(
                    local_buf
                );

                munmap(
                    map,
                    map_size
                );

                _exit(
                    4
                );
            }
        }


        free(
            local_buf
        );


        munmap(
            map,
            map_size
        );


        _exit(
            received ==
            total_bytes
            ?
            0
            :
            5
        );
    }


    /* =====================================================
       PARENT = PRODUCER
       ===================================================== */

    if (
        producer_cpu >= 0
    ) {

        if (
            pin_current_process_to_cpu(
                producer_cpu
            ) != 0
        ) {

            perror(
                "sched_setaffinity producer"
            );

            terminate_child(
                pid
            );

            free(
                local_buf
            );

            sem_destroy(
                &hdr->empty_slots
            );

            sem_destroy(
                &hdr->filled_slots
            );

            munmap(
                map,
                map_size
            );

            shm_unlink(
                name
            );

            close(
                fd
            );

            return -1;
        }

        parent_affinity_changed =
            1;
    }


    /*
     * Timing starts only after producer affinity is ready.
     */
    double start =
        now_ms();


    size_t sent =
        0;


    while (
        sent <
        total_bytes
    ) {

        size_t remaining =
            total_bytes -
            sent;


        size_t amount =
            remaining <
            chunk_size
            ?
            remaining
            :
            chunk_size;


        if (
            sem_wait_intr(
                &hdr->empty_slots
            ) != 0
        ) {

            perror(
                "sem_wait empty"
            );

            terminate_child(
                pid
            );

            free(
                local_buf
            );


            if (
                parent_affinity_changed &&
                original_affinity_valid
            ) {
                restore_affinity(
                    &original_affinity
                );
            }


            sem_destroy(
                &hdr->empty_slots
            );

            sem_destroy(
                &hdr->filled_slots
            );

            munmap(
                map,
                map_size
            );

            shm_unlink(
                name
            );

            close(
                fd
            );

            return -1;
        }


        size_t idx =
            hdr->head %
            RING_SLOTS;


        char *slot =
            slots_base +
            idx *
            chunk_size;


        if (
            write_all_mem(
                slot,
                local_buf,
                amount
            ) < 0
        ) {

            perror(
                "write mem"
            );

            terminate_child(
                pid
            );

            free(
                local_buf
            );


            if (
                parent_affinity_changed &&
                original_affinity_valid
            ) {
                restore_affinity(
                    &original_affinity
                );
            }


            sem_destroy(
                &hdr->empty_slots
            );

            sem_destroy(
                &hdr->filled_slots
            );

            munmap(
                map,
                map_size
            );

            shm_unlink(
                name
            );

            close(
                fd
            );

            return -1;
        }


        hdr->head =
            (
                hdr->head +
                1
            ) %
            RING_SLOTS;


        if (
            sem_post(
                &hdr->filled_slots
            ) != 0
        ) {

            perror(
                "sem_post filled"
            );

            terminate_child(
                pid
            );

            free(
                local_buf
            );


            if (
                parent_affinity_changed &&
                original_affinity_valid
            ) {
                restore_affinity(
                    &original_affinity
                );
            }


            sem_destroy(
                &hdr->empty_slots
            );

            sem_destroy(
                &hdr->filled_slots
            );

            munmap(
                map,
                map_size
            );

            shm_unlink(
                name
            );

            close(
                fd
            );

            return -1;
        }


        sent +=
            amount;
    }


    int status =
        0;


    while (
        waitpid(
            pid,
            &status,
            0
        ) < 0
    ) {

        if (
            errno ==
            EINTR
        ) {
            continue;
        }


        perror(
            "waitpid"
        );

        status =
            -1;

        break;
    }


    double end =
        now_ms();


    /*
     * Restore the parent to the exact scheduler mask it had
     * before this benchmark. Without this, one affinity trial
     * would contaminate every experiment that follows it.
     */
    if (
        parent_affinity_changed &&
        original_affinity_valid
    ) {

        if (
            restore_affinity(
                &original_affinity
            ) != 0
        ) {

            perror(
                "restore sched_setaffinity"
            );

            free(
                local_buf
            );

            sem_destroy(
                &hdr->empty_slots
            );

            sem_destroy(
                &hdr->filled_slots
            );

            munmap(
                map,
                map_size
            );

            shm_unlink(
                name
            );

            close(
                fd
            );

            return -1;
        }
    }


    free(
        local_buf
    );


    if (
        sem_destroy(
            &hdr->empty_slots
        ) != 0
    ) {
        perror(
            "sem_destroy empty"
        );
    }


    if (
        sem_destroy(
            &hdr->filled_slots
        ) != 0
    ) {
        perror(
            "sem_destroy filled"
        );
    }


    munmap(
        map,
        map_size
    );


    shm_unlink(
        name
    );


    close(
        fd
    );


    if (
        status == -1 ||
        !WIFEXITED(
            status
        ) ||
        WEXITSTATUS(
            status
        ) != 0
    ) {

        fprintf(
            stderr,
            "SHM-RING consumer failed.\n"
        );

        return -1;
    }


    result->elapsed_ms =
        end -
        start;


    double seconds =
        result->elapsed_ms /
        1000.0;


    result->throughput_mbps =
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


    /*
     * Keep normal FastIPC-X usage accounting for compatibility.
     *
     * The affinity analyzer will independently snapshot
     * RUSAGE_SELF + RUSAGE_CHILDREN around every individual
     * trial and replace these values with per-trial deltas.
     */
    result->user_ms =
        0.0;

    result->sys_ms =
        0.0;

    result->voluntary_ctx_switches =
        0;

    result->involuntary_ctx_switches =
        0;


    fill_usage(
        result
    );


    return 0;
}


/* =========================================================
   Normal production entry point
   ========================================================= */

int run_shm_ring_benchmark(
    size_t total_bytes,
    size_t chunk_size,
    BenchmarkResult *result
)
{
    return
        run_shm_ring_internal(
            total_bytes,
            chunk_size,
            -1,
            -1,
            result
        );
}


/* =========================================================
   Affinity experiment entry point
   ========================================================= */

int run_shm_ring_benchmark_affinity(
    size_t total_bytes,
    size_t chunk_size,
    int producer_cpu,
    int consumer_cpu,
    BenchmarkResult *result
)
{
    return
        run_shm_ring_internal(
            total_bytes,
            chunk_size,
            producer_cpu,
            consumer_cpu,
            result
        );
}
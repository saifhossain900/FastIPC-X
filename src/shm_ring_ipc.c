#include "../include/shm_ring_ipc.h"

#include <errno.h>
#include <fcntl.h>
#include <semaphore.h>
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
 * Shared memory ring buffer IPC.
 * Design:
 * - Fixed number of slots (RING_SLOTS)
 * - Each slot holds up to chunk_size bytes
 * - Shared control structure contains semaphores and indices
 * - Semaphores: empty_slots (count of free slots), filled_slots (count of used slots)
 * - The producer waits on empty_slots, writes into slot at head, advances head, posts filled_slots
 * - The consumer waits on filled_slots, reads from slot at tail, advances tail, posts empty_slots
 * - No per-slot locking required because producer/consumer coordinate via semaphores and single producer/consumer
 */

#define RING_SLOTS 8

struct ring_header {
    sem_t empty_slots; /* available slots for producer */
    sem_t filled_slots; /* available filled slots for consumer */
    size_t head; /* next write index */
    size_t tail; /* next read index */
    size_t slot_size; /* size of each slot buffer */
    size_t remaining_bytes; /* remaining bytes to transfer (for convenience) */
    /* Followed by slot_size * RING_SLOTS bytes of buffers */
};

static ssize_t write_all_mem(void *dest, const void *src, size_t count) {
    memcpy(dest, src, count);
    return (ssize_t)count;
}

static ssize_t read_all_mem(void *dest, const void *src, size_t count) {
    memcpy(dest, src, count);
    return (ssize_t)count;
}

static inline int sem_wait_intr(sem_t *s) {
    while (1) {
        if (sem_wait(s) == 0) return 0;
        if (errno == EINTR) continue;
        return -1;
    }
}

int run_shm_ring_benchmark(size_t total_bytes, size_t chunk_size, BenchmarkResult *result) {
    char name[256];
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    snprintf(name, sizeof(name), "/fastipc_shm_ring_%d_%ld_%ld", (int)getpid(), ts.tv_sec, ts.tv_nsec);

    size_t header_size = sizeof(struct ring_header);
    size_t slot_area = (size_t)RING_SLOTS * chunk_size;
    size_t map_size = header_size + slot_area;

    int fd = shm_open(name, O_CREAT | O_EXCL | O_RDWR, 0600);
    if (fd < 0) { perror("shm_open"); return -1; }
    if (ftruncate(fd, (off_t)map_size) < 0) { perror("ftruncate"); shm_unlink(name); close(fd); return -1; }

    void *map = mmap(NULL, map_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) { perror("mmap"); shm_unlink(name); close(fd); return -1; }

    struct ring_header *hdr = (struct ring_header *)map;
    /* initialize header */
    hdr->head = 0; hdr->tail = 0; hdr->slot_size = chunk_size; hdr->remaining_bytes = total_bytes;
    if (sem_init(&hdr->empty_slots, 1, RING_SLOTS) != 0) { perror("sem_init empty"); munmap(map, map_size); shm_unlink(name); close(fd); return -1; }
    if (sem_init(&hdr->filled_slots, 1, 0) != 0) { perror("sem_init filled"); sem_destroy(&hdr->empty_slots); munmap(map, map_size); shm_unlink(name); close(fd); return -1; }

    pid_t pid = fork();
    if (pid < 0) { perror("fork"); sem_destroy(&hdr->empty_slots); sem_destroy(&hdr->filled_slots); munmap(map, map_size); shm_unlink(name); close(fd); return -1; }

    char *slots_base = (char *)map + header_size;
    char *local_buf = malloc(chunk_size);
    if (!local_buf) { perror("malloc"); if (pid != 0) { sem_destroy(&hdr->empty_slots); sem_destroy(&hdr->filled_slots); munmap(map, map_size); shm_unlink(name); close(fd);} return -1; }

    if (pid == 0) {
        /* child: consumer */
        close(fd);
        size_t received = 0;
        while (received < total_bytes) {
            if (sem_wait_intr(&hdr->filled_slots) != 0) { perror("sem_wait filled"); free(local_buf); munmap(map, map_size); _exit(2); }

            size_t idx = hdr->tail % RING_SLOTS;
            char *slot = slots_base + idx * chunk_size;
            size_t remaining = total_bytes - received;
            size_t want = remaining < chunk_size ? remaining : chunk_size;

            if (read_all_mem(local_buf, slot, want) < 0) { perror("read mem"); free(local_buf); munmap(map, map_size); _exit(3); }
            received += want;

            hdr->tail = (hdr->tail + 1) % RING_SLOTS;

            if (sem_post(&hdr->empty_slots) != 0) { perror("sem_post empty"); free(local_buf); munmap(map, map_size); _exit(4); }
        }

        free(local_buf);
        munmap(map, map_size);
        _exit(received == total_bytes ? 0 : 5);
    }

    /* parent: producer */
    double start = now_ms();
    size_t sent = 0;
    while (sent < total_bytes) {
        size_t remaining = total_bytes - sent;
        size_t amount = remaining < chunk_size ? remaining : chunk_size;

        if (sem_wait_intr(&hdr->empty_slots) != 0) { perror("sem_wait empty"); free(local_buf); sem_destroy(&hdr->empty_slots); sem_destroy(&hdr->filled_slots); munmap(map, map_size); shm_unlink(name); close(fd); waitpid(pid, NULL, 0); return -1; }

        size_t idx = hdr->head % RING_SLOTS;
        char *slot = slots_base + idx * chunk_size;

        if (write_all_mem(slot, local_buf, amount) < 0) { perror("write mem"); free(local_buf); sem_destroy(&hdr->empty_slots); sem_destroy(&hdr->filled_slots); munmap(map, map_size); shm_unlink(name); close(fd); waitpid(pid, NULL, 0); return -1; }

        hdr->head = (hdr->head + 1) % RING_SLOTS;

        if (sem_post(&hdr->filled_slots) != 0) { perror("sem_post filled"); free(local_buf); sem_destroy(&hdr->empty_slots); sem_destroy(&hdr->filled_slots); munmap(map, map_size); shm_unlink(name); close(fd); waitpid(pid, NULL, 0); return -1; }

        sent += amount;
    }

    double end;
    int status = 0;
    waitpid(pid, &status, 0);
    end = now_ms();

    free(local_buf);
    if (sem_destroy(&hdr->empty_slots) != 0) perror("sem_destroy empty");
    if (sem_destroy(&hdr->filled_slots) != 0) perror("sem_destroy filled");
    munmap(map, map_size);
    shm_unlink(name);
    close(fd);

    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fprintf(stderr, "Child failed.\n");
        return -1;
    }

    result->elapsed_ms = end - start;
    double seconds = result->elapsed_ms / 1000.0;
    result->throughput_mbps = seconds > 0.0 ? ((double)total_bytes / (1024.0 * 1024.0)) / seconds : 0.0;

    result->user_ms = 0.0;
    result->sys_ms = 0.0;
    result->voluntary_ctx_switches = 0;
    result->involuntary_ctx_switches = 0;
    fill_usage(result);

    return 0;
}

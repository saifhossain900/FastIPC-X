#include "../include/shm_ipc.h"

#define _POSIX_C_SOURCE 200809L

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
#include <stddef.h>

struct shm_region {
    sem_t empty;
    sem_t full;
    size_t valid;
    char buffer[];
};

static inline ssize_t safe_sem_wait(sem_t *s) {
    while (1) {
        if (sem_wait(s) == 0) return 0;
        if (errno == EINTR) continue;
        return -1;
    }
}

int run_shm_benchmark(size_t total_bytes, size_t chunk_size, BenchmarkResult *result) {
    char name[256];
    int fd = -1;
    struct shm_region *region = NULL;
    size_t map_size;
    pid_t pid;

    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    snprintf(name, sizeof(name), "/fastipc_shm_%d_%ld_%ld", (int)getpid(), ts.tv_sec, ts.tv_nsec);

    map_size = offsetof(struct shm_region, buffer) + chunk_size;

    fd = shm_open(name, O_CREAT | O_EXCL | O_RDWR, 0600);
    if (fd < 0) {
        perror("shm_open");
        return -1;
    }

    if (ftruncate(fd, (off_t)map_size) < 0) {
        perror("ftruncate");
        shm_unlink(name);
        close(fd);
        return -1;
    }

    region = mmap(NULL, map_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (region == MAP_FAILED) {
        perror("mmap");
        shm_unlink(name);
        close(fd);
        return -1;
    }

    /* Initialize synchronization */
    region->valid = 0;
    if (sem_init(&region->empty, 1, 1) != 0) {
        perror("sem_init empty");
        munmap(region, map_size);
        shm_unlink(name);
        close(fd);
        return -1;
    }
    if (sem_init(&region->full, 1, 0) != 0) {
        perror("sem_init full");
        sem_destroy(&region->empty);
        munmap(region, map_size);
        shm_unlink(name);
        close(fd);
        return -1;
    }

    pid = fork();
    if (pid < 0) {
        perror("fork");
        sem_destroy(&region->full);
        sem_destroy(&region->empty);
        munmap(region, map_size);
        shm_unlink(name);
        close(fd);
        return -1;
    }

    char *local_buf = malloc(chunk_size);
    if (!local_buf) {
        perror("malloc");
        if (pid != 0) {
            sem_destroy(&region->full);
            sem_destroy(&region->empty);
            munmap(region, map_size);
            shm_unlink(name);
            close(fd);
        }
        return -1;
    }

    if (pid == 0) {
        /* Child: consumer */
        close(fd);
        size_t received = 0;
        while (received < total_bytes) {
            if (safe_sem_wait(&region->full) != 0) {
                perror("sem_wait full");
                free(local_buf);
                _exit(2);
            }

            size_t have = region->valid;
            if (have > 0) {
                size_t to_copy = have;
                if (received + to_copy > total_bytes) to_copy = total_bytes - received;
                memcpy(local_buf, region->buffer, to_copy);
                received += to_copy;
            }

            if (sem_post(&region->empty) != 0) {
                perror("sem_post empty");
                free(local_buf);
                _exit(3);
            }
        }

        free(local_buf);
        /* Child does not destroy semaphores; parent will clean up */
        munmap(region, map_size);
        _exit(received == total_bytes ? 0 : 4);
    }

    /* Parent: producer */
    size_t sent = 0;
    double start = now_ms();
    while (sent < total_bytes) {
        if (safe_sem_wait(&region->empty) != 0) {
            perror("sem_wait empty");
            sem_destroy(&region->full);
            sem_destroy(&region->empty);
            munmap(region, map_size);
            shm_unlink(name);
            close(fd);
            free(local_buf);
            waitpid(pid, NULL, 0);
            return -1;
        }

        size_t remaining = total_bytes - sent;
        size_t amount = remaining < chunk_size ? remaining : chunk_size;
        memcpy(region->buffer, local_buf, amount);
        region->valid = amount;

        if (sem_post(&region->full) != 0) {
            perror("sem_post full");
            sem_destroy(&region->full);
            sem_destroy(&region->empty);
            munmap(region, map_size);
            shm_unlink(name);
            close(fd);
            free(local_buf);
            waitpid(pid, NULL, 0);
            return -1;
        }

        sent += amount;
    }

    int status = 0;
    waitpid(pid, &status, 0);
    double end = now_ms();

    /* Cleanup */
    if (sem_destroy(&region->full) != 0) perror("sem_destroy full");
    if (sem_destroy(&region->empty) != 0) perror("sem_destroy empty");
    munmap(region, map_size);
    shm_unlink(name);
    close(fd);
    free(local_buf);

    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fprintf(stderr, "Child process failed.\n");
        return -1;
    }

    result->elapsed_ms = end - start;
    double seconds = result->elapsed_ms / 1000.0;
    result->throughput_mbps =
        seconds > 0.0 ? ((double)total_bytes / (1024.0 * 1024.0)) / seconds : 0.0;

    result->voluntary_ctx_switches = 0;
    result->involuntary_ctx_switches = 0;
    fill_usage(result);

    return 0;
}

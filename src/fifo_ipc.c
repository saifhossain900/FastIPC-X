#include "../include/fifo_ipc.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static ssize_t write_all(int fd, const void *buffer, size_t count) {
    const char *p = (const char *)buffer;
    size_t total = 0;

    while (total < count) {
        ssize_t n = write(fd, p + total, count - total);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        total += (size_t)n;
    }
    return (ssize_t)total;
}

static ssize_t read_all(int fd, void *buffer, size_t count) {
    char *p = (char *)buffer;
    size_t total = 0;

    while (total < count) {
        ssize_t n = read(fd, p + total, count - total);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) break; /* EOF */
        total += (size_t)n;
    }
    return (ssize_t)total;
}

int run_fifo_benchmark(size_t total_bytes, size_t chunk_size, BenchmarkResult *result) {
    char path[256];
    pid_t pid;

    /* Create a reasonably unique FIFO path using pid and timestamp */
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    snprintf(path, sizeof(path), "/tmp/fastipc_fifo_%d_%ld_%ld", (int)getpid(), ts.tv_sec, ts.tv_nsec);

    if (mkfifo(path, 0600) == -1) {
        perror("mkfifo");
        return -1;
    }

    pid = fork();
    if (pid < 0) {
        perror("fork");
        unlink(path);
        return -1;
    }

    char *buffer = malloc(chunk_size);
    if (!buffer) {
        perror("malloc");
        if (pid != 0) unlink(path);
        return -1;
    }
    memset(buffer, 'A', chunk_size);

    if (pid == 0) {
        /* Child: reader */
        int fd = -1;
        while (1) {
            fd = open(path, O_RDONLY);
            if (fd >= 0) break;
            if (errno == EINTR) continue;
            perror("open (child)");
            free(buffer);
            _exit(2);
        }

        size_t received = 0;
        while (received < total_bytes) {
            size_t remaining = total_bytes - received;
            size_t want = remaining < chunk_size ? remaining : chunk_size;

            ssize_t n = read_all(fd, buffer, want);
            if (n < 0) {
                perror("read");
                free(buffer);
                close(fd);
                _exit(3);
            }
            if (n == 0) break;
            received += (size_t)n;
        }

        free(buffer);
        close(fd);
        _exit(received == total_bytes ? 0 : 4);
    }

    /* Parent: writer */
    int wfd = -1;
    while (1) {
        wfd = open(path, O_WRONLY);
        if (wfd >= 0) break;
        if (errno == EINTR) continue;
        perror("open (parent)");
        free(buffer);
        unlink(path);
        waitpid(pid, NULL, 0);
        return -1;
    }

    double start = now_ms();

    size_t sent = 0;
    while (sent < total_bytes) {
        size_t remaining = total_bytes - sent;
        size_t amount = remaining < chunk_size ? remaining : chunk_size;

        if (write_all(wfd, buffer, amount) < 0) {
            perror("write");
            free(buffer);
            close(wfd);
            unlink(path);
            waitpid(pid, NULL, 0);
            return -1;
        }
        sent += amount;
    }

    close(wfd);

    int status = 0;
    waitpid(pid, &status, 0);

    double end = now_ms();

    free(buffer);
    unlink(path);

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

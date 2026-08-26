#include "../include/pipe_ipc.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
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

int run_pipe_benchmark(size_t total_bytes, size_t chunk_size, BenchmarkResult *result) {
    int fds[2];

    if (pipe(fds) == -1) {
        perror("pipe");
        return -1;
    }

    pid_t pid = fork();

    if (pid < 0) {
        perror("fork");
        close(fds[0]);
        close(fds[1]);
        return -1;
    }

    char *buffer = malloc(chunk_size);
    if (!buffer) {
        perror("malloc");
        close(fds[0]);
        close(fds[1]);
        return -1;
    }
    memset(buffer, 'A', chunk_size);

    if (pid == 0) {
        close(fds[1]);

        size_t received = 0;
        while (received < total_bytes) {
            size_t remaining = total_bytes - received;
            size_t want = remaining < chunk_size ? remaining : chunk_size;

            ssize_t n = read(fds[0], buffer, want);
            if (n < 0) {
                if (errno == EINTR) continue;
                perror("read");
                free(buffer);
                close(fds[0]);
                _exit(2);
            }
            if (n == 0) break;

            received += (size_t)n;
        }

        free(buffer);
        close(fds[0]);
        _exit(received == total_bytes ? 0 : 3);
    }

    close(fds[0]);

    double start = now_ms();

    size_t sent = 0;
    while (sent < total_bytes) {
        size_t remaining = total_bytes - sent;
        size_t amount = remaining < chunk_size ? remaining : chunk_size;

        if (write_all(fds[1], buffer, amount) < 0) {
            perror("write");
            free(buffer);
            close(fds[1]);
            waitpid(pid, NULL, 0);
            return -1;
        }
        sent += amount;
    }

    close(fds[1]);

    int status = 0;
    waitpid(pid, &status, 0);

    double end = now_ms();

    free(buffer);

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

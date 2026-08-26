#include "../include/socket_ipc.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
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
        if (n == 0) break;
        total += (size_t)n;
    }
    return (ssize_t)total;
}

int run_socket_benchmark(size_t total_bytes, size_t chunk_size, BenchmarkResult *result) {
    char path[108];
    struct sockaddr_un addr;
    int listen_fd = -1;
    pid_t pid;

    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    snprintf(path, sizeof(path), "/tmp/fastipc_sock_%d_%ld_%ld", (int)getpid(), ts.tv_sec, ts.tv_nsec);

    listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        perror("socket");
        return -1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);

    unlink(path); /* ignore error */

    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(listen_fd);
        return -1;
    }

    if (listen(listen_fd, 1) < 0) {
        perror("listen");
        close(listen_fd);
        unlink(path);
        return -1;
    }

    pid = fork();
    if (pid < 0) {
        perror("fork");
        close(listen_fd);
        unlink(path);
        return -1;
    }

    char *buffer = malloc(chunk_size);
    if (!buffer) {
        perror("malloc");
        close(listen_fd);
        unlink(path);
        return -1;
    }
    memset(buffer, 'A', chunk_size);

    if (pid == 0) {
        /* Child: client (reader) */
        int client_fd = -1;
        client_fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (client_fd < 0) {
            perror("socket (child)");
            free(buffer);
            _exit(2);
        }

        /* Close listening fd inherited from parent */
        close(listen_fd);

        while (1) {
            if (connect(client_fd, (struct sockaddr *)&addr, sizeof(addr)) == 0) break;
            if (errno == EINTR) continue;
            perror("connect");
            close(client_fd);
            free(buffer);
            _exit(3);
        }

        size_t received = 0;
        while (received < total_bytes) {
            size_t remaining = total_bytes - received;
            size_t want = remaining < chunk_size ? remaining : chunk_size;

            ssize_t n = read_all(client_fd, buffer, want);
            if (n < 0) {
                perror("read");
                close(client_fd);
                free(buffer);
                _exit(4);
            }
            if (n == 0) break;
            received += (size_t)n;
        }

        close(client_fd);
        free(buffer);
        _exit(received == total_bytes ? 0 : 5);
    }

    /* Parent: server (writer) */
    int accepted = -1;
    while (1) {
        accepted = accept(listen_fd, NULL, NULL);
        if (accepted >= 0) break;
        if (errno == EINTR) continue;
        perror("accept");
        free(buffer);
        close(listen_fd);
        unlink(path);
        waitpid(pid, NULL, 0);
        return -1;
    }

    /* Once accepted, can remove listening socket file */
    close(listen_fd);

    double start = now_ms();

    size_t sent = 0;
    while (sent < total_bytes) {
        size_t remaining = total_bytes - sent;
        size_t amount = remaining < chunk_size ? remaining : chunk_size;

        if (write_all(accepted, buffer, amount) < 0) {
            perror("write");
            free(buffer);
            close(accepted);
            unlink(path);
            waitpid(pid, NULL, 0);
            return -1;
        }
        sent += amount;
    }

    close(accepted);
    unlink(path);

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

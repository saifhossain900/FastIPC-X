#include "../include/integrity_verifier.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <semaphore.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>

#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>

#include <unistd.h>


#define VERIFY_RING_SLOTS 8

#define FNV_OFFSET_BASIS UINT64_C(14695981039346656037)
#define FNV_PRIME        UINT64_C(1099511628211)


/* =========================================================
   Verification structures
   ========================================================= */

typedef struct {
    size_t bytes_received;
    uint64_t checksum;
    int success;
} ChildReport;


typedef struct {
    size_t bytes_sent;
    size_t bytes_received;

    uint64_t sender_checksum;
    uint64_t receiver_checksum;
} VerificationStats;


/* One-slot shared memory verification structure */
typedef struct {
    sem_t empty;
    sem_t full;

    size_t length;

    unsigned char data[];
} VerifyShmSlot;


/* Ring-buffer shared memory verification structure */
typedef struct {
    sem_t empty_slots;
    sem_t filled_slots;

    size_t write_index;
    size_t read_index;

    size_t lengths[VERIFY_RING_SLOTS];

    unsigned char data[];
} VerifyShmRing;


/* =========================================================
   Utility helpers
   ========================================================= */

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


static int method_is_valid(const char *method)
{
    return (
        strcmp(method, "pipe") == 0 ||
        strcmp(method, "fifo") == 0 ||
        strcmp(method, "socket") == 0 ||
        strcmp(method, "shm") == 0 ||
        strcmp(method, "shm-opt") == 0
    );
}


static const char *display_method(const char *method)
{
    if (strcmp(method, "pipe") == 0) {
        return "PIPE";
    }

    if (strcmp(method, "fifo") == 0) {
        return "FIFO";
    }

    if (strcmp(method, "socket") == 0) {
        return "SOCKET";
    }

    if (strcmp(method, "shm") == 0) {
        return "SHM";
    }

    if (strcmp(method, "shm-opt") == 0) {
        return "SHM-RING";
    }

    return method;
}


/* =========================================================
   Deterministic data generator

   Every byte is generated from its global stream offset.

   This means sender and receiver are checking the actual
   transferred byte stream, not just the byte count.
   ========================================================= */

static void fill_pattern(
    unsigned char *buffer,
    size_t length,
    size_t global_offset
)
{
    for (size_t i = 0; i < length; ++i) {

        size_t position =
            global_offset + i;

        buffer[i] =
            (unsigned char)(
                ((position * 131U) + 17U)
                & 0xFFU
            );
    }
}


/* =========================================================
   64-bit FNV-1a checksum

   Used only for correctness verification.

   It is intentionally NOT included in performance benchmark
   timing, so integrity checking cannot distort benchmark
   results.
   ========================================================= */

static uint64_t checksum_update(
    uint64_t hash,
    const unsigned char *data,
    size_t length
)
{
    for (size_t i = 0; i < length; ++i) {

        hash ^= (uint64_t)data[i];

        hash *= FNV_PRIME;
    }

    return hash;
}


/* =========================================================
   Reliable read/write helpers
   ========================================================= */

static int write_all(
    int fd,
    const void *buffer,
    size_t length
)
{
    const unsigned char *ptr =
        (const unsigned char *)buffer;

    size_t written = 0;

    while (written < length) {

        ssize_t rc =
            write(
                fd,
                ptr + written,
                length - written
            );

        if (rc < 0) {

            if (errno == EINTR) {
                continue;
            }

            return -1;
        }

        if (rc == 0) {
            return -1;
        }

        written +=
            (size_t)rc;
    }

    return 0;
}


static int read_all(
    int fd,
    void *buffer,
    size_t length
)
{
    unsigned char *ptr =
        (unsigned char *)buffer;

    size_t received = 0;

    while (received < length) {

        ssize_t rc =
            read(
                fd,
                ptr + received,
                length - received
            );

        if (rc < 0) {

            if (errno == EINTR) {
                continue;
            }

            return -1;
        }

        if (rc == 0) {
            return -1;
        }

        received +=
            (size_t)rc;
    }

    return 0;
}


static int wait_sem(sem_t *sem)
{
    while (sem_wait(sem) != 0) {

        if (errno == EINTR) {
            continue;
        }

        return -1;
    }

    return 0;
}


static int wait_child_success(pid_t pid)
{
    int status = 0;

    if (waitpid(pid, &status, 0) < 0) {
        return -1;
    }

    if (!WIFEXITED(status)) {
        return -1;
    }

    if (WEXITSTATUS(status) != 0) {
        return -1;
    }

    return 0;
}


/* =========================================================
   Stream sender
   ========================================================= */

static int send_stream(
    int fd,
    size_t payload_bytes,
    size_t chunk_bytes,
    size_t *bytes_sent,
    uint64_t *checksum
)
{
    unsigned char *buffer =
        malloc(chunk_bytes);

    if (!buffer) {
        return -1;
    }

    size_t total = 0;

    uint64_t hash =
        FNV_OFFSET_BASIS;

    while (total < payload_bytes) {

        size_t remaining =
            payload_bytes - total;

        size_t current =
            remaining < chunk_bytes
            ? remaining
            : chunk_bytes;

        fill_pattern(
            buffer,
            current,
            total
        );

        hash =
            checksum_update(
                hash,
                buffer,
                current
            );

        if (
            write_all(
                fd,
                buffer,
                current
            ) != 0
        ) {
            free(buffer);
            return -1;
        }

        total += current;
    }

    free(buffer);

    *bytes_sent = total;
    *checksum = hash;

    return 0;
}


/* =========================================================
   Stream receiver executed by child
   ========================================================= */

static int receive_stream(
    int fd,
    size_t chunk_bytes,
    ChildReport *report
)
{
    unsigned char *buffer =
        malloc(chunk_bytes);

    if (!buffer) {
        return -1;
    }

    size_t total = 0;

    uint64_t hash =
        FNV_OFFSET_BASIS;

    for (;;) {

        ssize_t rc =
            read(
                fd,
                buffer,
                chunk_bytes
            );

        if (rc < 0) {

            if (errno == EINTR) {
                continue;
            }

            free(buffer);
            return -1;
        }

        if (rc == 0) {
            break;
        }

        hash =
            checksum_update(
                hash,
                buffer,
                (size_t)rc
            );

        total +=
            (size_t)rc;
    }

    free(buffer);

    report->bytes_received = total;
    report->checksum = hash;
    report->success = 1;

    return 0;
}


/* =========================================================
   Receive child report
   ========================================================= */

static int receive_child_report(
    int report_fd,
    pid_t child,
    ChildReport *report
)
{
    int report_ok =
        read_all(
            report_fd,
            report,
            sizeof(*report)
        );

    close(report_fd);

    int child_ok =
        wait_child_success(child);

    if (
        report_ok != 0 ||
        child_ok != 0 ||
        report->success != 1
    ) {
        return -1;
    }

    return 0;
}


/* =========================================================
   PIPE verification
   ========================================================= */

static int verify_pipe(
    size_t payload_bytes,
    size_t chunk_bytes,
    VerificationStats *stats
)
{
    int data_pipe[2];
    int report_pipe[2];

    if (pipe(data_pipe) != 0) {
        perror("pipe");
        return -1;
    }

    if (pipe(report_pipe) != 0) {
        perror("pipe report");

        close(data_pipe[0]);
        close(data_pipe[1]);

        return -1;
    }

    pid_t child = fork();

    if (child < 0) {

        perror("fork");

        close(data_pipe[0]);
        close(data_pipe[1]);

        close(report_pipe[0]);
        close(report_pipe[1]);

        return -1;
    }


    if (child == 0) {

        close(data_pipe[1]);

        close(report_pipe[0]);

        ChildReport report = {0};

        int rc =
            receive_stream(
                data_pipe[0],
                chunk_bytes,
                &report
            );

        close(data_pipe[0]);

        if (rc != 0) {

            report.success = 0;

            (void)write_all(
                report_pipe[1],
                &report,
                sizeof(report)
            );

            close(report_pipe[1]);

            _exit(1);
        }

        if (
            write_all(
                report_pipe[1],
                &report,
                sizeof(report)
            ) != 0
        ) {
            close(report_pipe[1]);
            _exit(1);
        }

        close(report_pipe[1]);

        _exit(0);
    }


    close(data_pipe[0]);

    close(report_pipe[1]);

    size_t bytes_sent = 0;

    uint64_t sender_checksum = 0;

    int send_rc =
        send_stream(
            data_pipe[1],
            payload_bytes,
            chunk_bytes,
            &bytes_sent,
            &sender_checksum
        );

    close(data_pipe[1]);

    ChildReport report = {0};

    int report_rc =
        receive_child_report(
            report_pipe[0],
            child,
            &report
        );

    if (
        send_rc != 0 ||
        report_rc != 0
    ) {
        return -1;
    }

    stats->bytes_sent =
        bytes_sent;

    stats->bytes_received =
        report.bytes_received;

    stats->sender_checksum =
        sender_checksum;

    stats->receiver_checksum =
        report.checksum;

    return 0;
}


/* =========================================================
   FIFO verification
   ========================================================= */

static int verify_fifo(
    size_t payload_bytes,
    size_t chunk_bytes,
    VerificationStats *stats
)
{
    char fifo_path[128];

    snprintf(
        fifo_path,
        sizeof(fifo_path),
        "/tmp/fastipc_verify_fifo_%ld",
        (long)getpid()
    );

    unlink(fifo_path);

    if (
        mkfifo(
            fifo_path,
            0600
        ) != 0
    ) {
        perror("mkfifo");
        return -1;
    }

    int report_pipe[2];

    if (pipe(report_pipe) != 0) {

        perror("pipe report");

        unlink(fifo_path);

        return -1;
    }

    pid_t child =
        fork();

    if (child < 0) {

        perror("fork");

        close(report_pipe[0]);
        close(report_pipe[1]);

        unlink(fifo_path);

        return -1;
    }


    if (child == 0) {

        close(report_pipe[0]);

        int fd =
            open(
                fifo_path,
                O_RDONLY
            );

        if (fd < 0) {

            ChildReport report = {0};

            (void)write_all(
                report_pipe[1],
                &report,
                sizeof(report)
            );

            close(report_pipe[1]);

            _exit(1);
        }

        ChildReport report = {0};

        int rc =
            receive_stream(
                fd,
                chunk_bytes,
                &report
            );

        close(fd);

        if (rc != 0) {
            report.success = 0;
        }

        (void)write_all(
            report_pipe[1],
            &report,
            sizeof(report)
        );

        close(report_pipe[1]);

        _exit(
            rc == 0 ? 0 : 1
        );
    }


    close(report_pipe[1]);

    int fd =
        open(
            fifo_path,
            O_WRONLY
        );

    if (fd < 0) {

        perror("open FIFO");

        close(report_pipe[0]);

        waitpid(child, NULL, 0);

        unlink(fifo_path);

        return -1;
    }

    size_t bytes_sent = 0;

    uint64_t sender_checksum = 0;

    int send_rc =
        send_stream(
            fd,
            payload_bytes,
            chunk_bytes,
            &bytes_sent,
            &sender_checksum
        );

    close(fd);

    ChildReport report = {0};

    int report_rc =
        receive_child_report(
            report_pipe[0],
            child,
            &report
        );

    unlink(fifo_path);

    if (
        send_rc != 0 ||
        report_rc != 0
    ) {
        return -1;
    }

    stats->bytes_sent =
        bytes_sent;

    stats->bytes_received =
        report.bytes_received;

    stats->sender_checksum =
        sender_checksum;

    stats->receiver_checksum =
        report.checksum;

    return 0;
}


/* =========================================================
   UNIX DOMAIN SOCKET verification
   ========================================================= */

static int verify_socket(
    size_t payload_bytes,
    size_t chunk_bytes,
    VerificationStats *stats
)
{
    char socket_path[96];

    snprintf(
        socket_path,
        sizeof(socket_path),
        "/tmp/fastipc_verify_socket_%ld.sock",
        (long)getpid()
    );

    unlink(socket_path);

    int listener =
        socket(
            AF_UNIX,
            SOCK_STREAM,
            0
        );

    if (listener < 0) {
        perror("socket");
        return -1;
    }

    struct sockaddr_un address;

    memset(
        &address,
        0,
        sizeof(address)
    );

    address.sun_family =
        AF_UNIX;

    strncpy(
        address.sun_path,
        socket_path,
        sizeof(address.sun_path) - 1
    );

    address.sun_path[
        sizeof(address.sun_path) - 1
    ] = '\0';


    if (
        bind(
            listener,
            (struct sockaddr *)&address,
            sizeof(address)
        ) != 0
    ) {
        perror("bind");

        close(listener);
        unlink(socket_path);

        return -1;
    }


    if (
        listen(
            listener,
            1
        ) != 0
    ) {
        perror("listen");

        close(listener);
        unlink(socket_path);

        return -1;
    }


    int report_pipe[2];

    if (pipe(report_pipe) != 0) {

        perror("pipe report");

        close(listener);
        unlink(socket_path);

        return -1;
    }


    pid_t child =
        fork();

    if (child < 0) {

        perror("fork");

        close(listener);

        close(report_pipe[0]);
        close(report_pipe[1]);

        unlink(socket_path);

        return -1;
    }


    if (child == 0) {

        close(listener);

        close(report_pipe[0]);

        int fd =
            socket(
                AF_UNIX,
                SOCK_STREAM,
                0
            );

        ChildReport report = {0};

        if (fd < 0) {

            (void)write_all(
                report_pipe[1],
                &report,
                sizeof(report)
            );

            close(report_pipe[1]);

            _exit(1);
        }


        if (
            connect(
                fd,
                (struct sockaddr *)&address,
                sizeof(address)
            ) != 0
        ) {

            close(fd);

            (void)write_all(
                report_pipe[1],
                &report,
                sizeof(report)
            );

            close(report_pipe[1]);

            _exit(1);
        }


        int rc =
            receive_stream(
                fd,
                chunk_bytes,
                &report
            );

        close(fd);

        if (rc != 0) {
            report.success = 0;
        }

        (void)write_all(
            report_pipe[1],
            &report,
            sizeof(report)
        );

        close(report_pipe[1]);

        _exit(
            rc == 0 ? 0 : 1
        );
    }


    close(report_pipe[1]);

    int connection =
        accept(
            listener,
            NULL,
            NULL
        );

    close(listener);

    if (connection < 0) {

        perror("accept");

        close(report_pipe[0]);

        waitpid(child, NULL, 0);

        unlink(socket_path);

        return -1;
    }


    size_t bytes_sent = 0;

    uint64_t sender_checksum = 0;

    int send_rc =
        send_stream(
            connection,
            payload_bytes,
            chunk_bytes,
            &bytes_sent,
            &sender_checksum
        );

    shutdown(
        connection,
        SHUT_WR
    );

    close(connection);


    ChildReport report = {0};

    int report_rc =
        receive_child_report(
            report_pipe[0],
            child,
            &report
        );

    unlink(socket_path);


    if (
        send_rc != 0 ||
        report_rc != 0
    ) {
        return -1;
    }


    stats->bytes_sent =
        bytes_sent;

    stats->bytes_received =
        report.bytes_received;

    stats->sender_checksum =
        sender_checksum;

    stats->receiver_checksum =
        report.checksum;

    return 0;
}


/* =========================================================
   SHARED MEMORY baseline verification
   ========================================================= */

static int verify_shm(
    size_t payload_bytes,
    size_t chunk_bytes,
    VerificationStats *stats
)
{
    char shm_name[128];

    snprintf(
        shm_name,
        sizeof(shm_name),
        "/fastipc_verify_shm_%ld",
        (long)getpid()
    );

    shm_unlink(shm_name);

    int shm_fd =
        shm_open(
            shm_name,
            O_CREAT | O_EXCL | O_RDWR,
            0600
        );

    if (shm_fd < 0) {
        perror("shm_open");
        return -1;
    }


    size_t map_size =
        offsetof(
            VerifyShmSlot,
            data
        ) +
        chunk_bytes;


    if (
        ftruncate(
            shm_fd,
            (off_t)map_size
        ) != 0
    ) {

        perror("ftruncate");

        close(shm_fd);
        shm_unlink(shm_name);

        return -1;
    }


    VerifyShmSlot *shared =
        mmap(
            NULL,
            map_size,
            PROT_READ | PROT_WRITE,
            MAP_SHARED,
            shm_fd,
            0
        );


    close(shm_fd);


    if (shared == MAP_FAILED) {

        perror("mmap");

        shm_unlink(shm_name);

        return -1;
    }


    shm_unlink(shm_name);


    memset(
        shared,
        0,
        map_size
    );


    if (
        sem_init(
            &shared->empty,
            1,
            1
        ) != 0
    ) {

        perror("sem_init empty");

        munmap(
            shared,
            map_size
        );

        return -1;
    }


    if (
        sem_init(
            &shared->full,
            1,
            0
        ) != 0
    ) {

        perror("sem_init full");

        sem_destroy(
            &shared->empty
        );

        munmap(
            shared,
            map_size
        );

        return -1;
    }


    int report_pipe[2];

    if (pipe(report_pipe) != 0) {

        perror("pipe report");

        sem_destroy(
            &shared->empty
        );

        sem_destroy(
            &shared->full
        );

        munmap(
            shared,
            map_size
        );

        return -1;
    }


    pid_t child =
        fork();


    if (child < 0) {

        perror("fork");

        close(report_pipe[0]);
        close(report_pipe[1]);

        sem_destroy(
            &shared->empty
        );

        sem_destroy(
            &shared->full
        );

        munmap(
            shared,
            map_size
        );

        return -1;
    }


    if (child == 0) {

        close(report_pipe[0]);

        ChildReport report = {0};

        uint64_t hash =
            FNV_OFFSET_BASIS;

        size_t total = 0;

        int success = 1;


        for (;;) {

            if (
                wait_sem(
                    &shared->full
                ) != 0
            ) {
                success = 0;
                break;
            }


            size_t length =
                shared->length;


            if (length == 0) {

                sem_post(
                    &shared->empty
                );

                break;
            }


            hash =
                checksum_update(
                    hash,
                    shared->data,
                    length
                );


            total += length;


            if (
                sem_post(
                    &shared->empty
                ) != 0
            ) {
                success = 0;
                break;
            }
        }


        report.bytes_received =
            total;

        report.checksum =
            hash;

        report.success =
            success;


        (void)write_all(
            report_pipe[1],
            &report,
            sizeof(report)
        );


        close(report_pipe[1]);


        _exit(
            success ? 0 : 1
        );
    }


    close(report_pipe[1]);


    unsigned char *buffer =
        malloc(chunk_bytes);


    if (!buffer) {

        close(report_pipe[0]);

        waitpid(child, NULL, 0);

        sem_destroy(
            &shared->empty
        );

        sem_destroy(
            &shared->full
        );

        munmap(
            shared,
            map_size
        );

        return -1;
    }


    size_t total = 0;

    uint64_t sender_hash =
        FNV_OFFSET_BASIS;

    int producer_ok = 1;


    while (total < payload_bytes) {

        size_t remaining =
            payload_bytes - total;

        size_t current =
            remaining < chunk_bytes
            ? remaining
            : chunk_bytes;


        fill_pattern(
            buffer,
            current,
            total
        );


        sender_hash =
            checksum_update(
                sender_hash,
                buffer,
                current
            );


        if (
            wait_sem(
                &shared->empty
            ) != 0
        ) {
            producer_ok = 0;
            break;
        }


        memcpy(
            shared->data,
            buffer,
            current
        );


        shared->length =
            current;


        if (
            sem_post(
                &shared->full
            ) != 0
        ) {
            producer_ok = 0;
            break;
        }


        total += current;
    }


    free(buffer);


    if (producer_ok) {

        if (
            wait_sem(
                &shared->empty
            ) != 0
        ) {
            producer_ok = 0;
        }
        else {

            shared->length = 0;

            if (
                sem_post(
                    &shared->full
                ) != 0
            ) {
                producer_ok = 0;
            }
        }
    }


    ChildReport report = {0};

    int report_rc =
        receive_child_report(
            report_pipe[0],
            child,
            &report
        );


    sem_destroy(
        &shared->empty
    );

    sem_destroy(
        &shared->full
    );


    munmap(
        shared,
        map_size
    );


    if (
        !producer_ok ||
        report_rc != 0
    ) {
        return -1;
    }


    stats->bytes_sent =
        total;

    stats->bytes_received =
        report.bytes_received;

    stats->sender_checksum =
        sender_hash;

    stats->receiver_checksum =
        report.checksum;

    return 0;
}


/* =========================================================
   SHM ring-buffer verification
   ========================================================= */

static int verify_shm_ring(
    size_t payload_bytes,
    size_t chunk_bytes,
    VerificationStats *stats
)
{
    char shm_name[128];

    snprintf(
        shm_name,
        sizeof(shm_name),
        "/fastipc_verify_ring_%ld",
        (long)getpid()
    );

    shm_unlink(shm_name);


    int shm_fd =
        shm_open(
            shm_name,
            O_CREAT | O_EXCL | O_RDWR,
            0600
        );


    if (shm_fd < 0) {
        perror("shm_open ring");
        return -1;
    }


    size_t data_size =
        VERIFY_RING_SLOTS *
        chunk_bytes;


    size_t map_size =
        offsetof(
            VerifyShmRing,
            data
        ) +
        data_size;


    if (
        ftruncate(
            shm_fd,
            (off_t)map_size
        ) != 0
    ) {

        perror("ftruncate ring");

        close(shm_fd);
        shm_unlink(shm_name);

        return -1;
    }


    VerifyShmRing *shared =
        mmap(
            NULL,
            map_size,
            PROT_READ | PROT_WRITE,
            MAP_SHARED,
            shm_fd,
            0
        );


    close(shm_fd);


    if (shared == MAP_FAILED) {

        perror("mmap ring");

        shm_unlink(shm_name);

        return -1;
    }


    shm_unlink(shm_name);


    memset(
        shared,
        0,
        map_size
    );


    if (
        sem_init(
            &shared->empty_slots,
            1,
            VERIFY_RING_SLOTS
        ) != 0
    ) {

        perror("sem_init empty_slots");

        munmap(
            shared,
            map_size
        );

        return -1;
    }


    if (
        sem_init(
            &shared->filled_slots,
            1,
            0
        ) != 0
    ) {

        perror("sem_init filled_slots");

        sem_destroy(
            &shared->empty_slots
        );

        munmap(
            shared,
            map_size
        );

        return -1;
    }


    int report_pipe[2];


    if (pipe(report_pipe) != 0) {

        perror("pipe report");

        sem_destroy(
            &shared->empty_slots
        );

        sem_destroy(
            &shared->filled_slots
        );

        munmap(
            shared,
            map_size
        );

        return -1;
    }


    pid_t child =
        fork();


    if (child < 0) {

        perror("fork");

        close(report_pipe[0]);
        close(report_pipe[1]);

        sem_destroy(
            &shared->empty_slots
        );

        sem_destroy(
            &shared->filled_slots
        );

        munmap(
            shared,
            map_size
        );

        return -1;
    }


    if (child == 0) {

        close(report_pipe[0]);

        ChildReport report = {0};

        uint64_t hash =
            FNV_OFFSET_BASIS;

        size_t total = 0;

        int success = 1;


        for (;;) {

            if (
                wait_sem(
                    &shared->filled_slots
                ) != 0
            ) {
                success = 0;
                break;
            }


            size_t index =
                shared->read_index;


            size_t length =
                shared->lengths[index];


            if (length > 0) {

                unsigned char *slot =
                    shared->data +
                    (
                        index *
                        chunk_bytes
                    );


                hash =
                    checksum_update(
                        hash,
                        slot,
                        length
                    );


                total += length;
            }


            shared->read_index =
                (
                    index + 1
                ) %
                VERIFY_RING_SLOTS;


            if (
                sem_post(
                    &shared->empty_slots
                ) != 0
            ) {
                success = 0;
                break;
            }


            if (length == 0) {
                break;
            }
        }


        report.bytes_received =
            total;

        report.checksum =
            hash;

        report.success =
            success;


        (void)write_all(
            report_pipe[1],
            &report,
            sizeof(report)
        );


        close(report_pipe[1]);


        _exit(
            success ? 0 : 1
        );
    }


    close(report_pipe[1]);


    unsigned char *buffer =
        malloc(chunk_bytes);


    if (!buffer) {

        close(report_pipe[0]);

        waitpid(child, NULL, 0);

        sem_destroy(
            &shared->empty_slots
        );

        sem_destroy(
            &shared->filled_slots
        );

        munmap(
            shared,
            map_size
        );

        return -1;
    }


    size_t total = 0;

    uint64_t sender_hash =
        FNV_OFFSET_BASIS;

    int producer_ok = 1;


    while (total < payload_bytes) {

        size_t remaining =
            payload_bytes - total;

        size_t current =
            remaining < chunk_bytes
            ? remaining
            : chunk_bytes;


        fill_pattern(
            buffer,
            current,
            total
        );


        sender_hash =
            checksum_update(
                sender_hash,
                buffer,
                current
            );


        if (
            wait_sem(
                &shared->empty_slots
            ) != 0
        ) {
            producer_ok = 0;
            break;
        }


        size_t index =
            shared->write_index;


        unsigned char *slot =
            shared->data +
            (
                index *
                chunk_bytes
            );


        memcpy(
            slot,
            buffer,
            current
        );


        shared->lengths[index] =
            current;


        shared->write_index =
            (
                index + 1
            ) %
            VERIFY_RING_SLOTS;


        if (
            sem_post(
                &shared->filled_slots
            ) != 0
        ) {
            producer_ok = 0;
            break;
        }


        total += current;
    }


    free(buffer);


    /* Send zero-length termination slot */
    if (producer_ok) {

        if (
            wait_sem(
                &shared->empty_slots
            ) != 0
        ) {
            producer_ok = 0;
        }
        else {

            size_t index =
                shared->write_index;


            shared->lengths[index] =
                0;


            shared->write_index =
                (
                    index + 1
                ) %
                VERIFY_RING_SLOTS;


            if (
                sem_post(
                    &shared->filled_slots
                ) != 0
            ) {
                producer_ok = 0;
            }
        }
    }


    ChildReport report = {0};


    int report_rc =
        receive_child_report(
            report_pipe[0],
            child,
            &report
        );


    sem_destroy(
        &shared->empty_slots
    );

    sem_destroy(
        &shared->filled_slots
    );


    munmap(
        shared,
        map_size
    );


    if (
        !producer_ok ||
        report_rc != 0
    ) {
        return -1;
    }


    stats->bytes_sent =
        total;

    stats->bytes_received =
        report.bytes_received;

    stats->sender_checksum =
        sender_hash;

    stats->receiver_checksum =
        report.checksum;

    return 0;
}


/* =========================================================
   Save verification evidence
   ========================================================= */

static int save_verification_csv(
    const char *method,
    size_t payload_mb,
    size_t chunk_kb,
    const VerificationStats *stats,
    int passed
)
{
    if (
        ensure_results_directory() != 0
    ) {
        return -1;
    }


    char path[512];


    snprintf(
        path,
        sizeof(path),
        "results/integrity_%s_%zuMB_%zuKB.csv",
        method,
        payload_mb,
        chunk_kb
    );


    FILE *f =
        fopen(
            path,
            "w"
        );


    if (!f) {
        return -1;
    }


    fprintf(
        f,
        "method,payload_mb,chunk_kb,"
        "bytes_sent,bytes_received,"
        "sender_checksum,receiver_checksum,"
        "result\n"
    );


    fprintf(
        f,
        "%s,%zu,%zu,%zu,%zu,"
        "0x%016" PRIx64 ","
        "0x%016" PRIx64 ","
        "%s\n",
        display_method(method),
        payload_mb,
        chunk_kb,
        stats->bytes_sent,
        stats->bytes_received,
        stats->sender_checksum,
        stats->receiver_checksum,
        passed ? "PASS" : "FAIL"
    );


    fclose(f);


    printf(
        "Result CSV          : %s\n",
        path
    );


    return 0;
}


/* =========================================================
   Public verification command
   ========================================================= */

int run_integrity_verification(
    const char *method,
    size_t payload_mb,
    size_t chunk_kb
)
{
    if (!method_is_valid(method)) {

        fprintf(
            stderr,
            "Unsupported verification method: %s\n",
            method
        );

        fprintf(
            stderr,
            "Supported methods: "
            "pipe fifo socket shm shm-opt\n"
        );

        return -1;
    }


    if (
        payload_mb == 0 ||
        chunk_kb == 0
    ) {

        fprintf(
            stderr,
            "Payload and chunk size "
            "must be greater than zero.\n"
        );

        return -1;
    }


    if (
        payload_mb >
        SIZE_MAX /
        (
            1024U *
            1024U
        )
    ) {

        fprintf(
            stderr,
            "Payload size is too large.\n"
        );

        return -1;
    }


    if (
        chunk_kb >
        SIZE_MAX /
        1024U
    ) {

        fprintf(
            stderr,
            "Chunk size is too large.\n"
        );

        return -1;
    }


    size_t payload_bytes =
        payload_mb *
        1024U *
        1024U;


    size_t chunk_bytes =
        chunk_kb *
        1024U;


    VerificationStats stats = {0};


    printf(
        "\nRunning data integrity verification...\n"
    );


    int rc = -1;


    if (
        strcmp(
            method,
            "pipe"
        ) == 0
    ) {

        rc =
            verify_pipe(
                payload_bytes,
                chunk_bytes,
                &stats
            );
    }
    else if (
        strcmp(
            method,
            "fifo"
        ) == 0
    ) {

        rc =
            verify_fifo(
                payload_bytes,
                chunk_bytes,
                &stats
            );
    }
    else if (
        strcmp(
            method,
            "socket"
        ) == 0
    ) {

        rc =
            verify_socket(
                payload_bytes,
                chunk_bytes,
                &stats
            );
    }
    else if (
        strcmp(
            method,
            "shm"
        ) == 0
    ) {

        rc =
            verify_shm(
                payload_bytes,
                chunk_bytes,
                &stats
            );
    }
    else if (
        strcmp(
            method,
            "shm-opt"
        ) == 0
    ) {

        rc =
            verify_shm_ring(
                payload_bytes,
                chunk_bytes,
                &stats
            );
    }


    if (rc != 0) {

        fprintf(
            stderr,
            "Integrity verification transfer failed.\n"
        );

        return -1;
    }


    int passed =
        (
            stats.bytes_sent ==
            payload_bytes
        ) &&
        (
            stats.bytes_received ==
            payload_bytes
        ) &&
        (
            stats.bytes_sent ==
            stats.bytes_received
        ) &&
        (
            stats.sender_checksum ==
            stats.receiver_checksum
        );


    printf(
        "\n"
        "FASTIPC-X DATA INTEGRITY VERIFICATION\n"
        "============================================================\n\n"
    );


    printf(
        "Method              : %s\n",
        display_method(method)
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
        "Checksum algorithm  : FNV-1a 64-bit\n\n"
    );


    printf(
        "Bytes sent          : %zu\n",
        stats.bytes_sent
    );


    printf(
        "Bytes received      : %zu\n\n",
        stats.bytes_received
    );


    printf(
        "Sender checksum     : 0x%016" PRIx64 "\n",
        stats.sender_checksum
    );


    printf(
        "Receiver checksum   : 0x%016" PRIx64 "\n\n",
        stats.receiver_checksum
    );


    printf(
        "DATA INTEGRITY      : %s\n\n",
        passed ? "PASS" : "FAIL"
    );


    (void)save_verification_csv(
        method,
        payload_mb,
        chunk_kb,
        &stats,
        passed
    );


    /*
     * Return success only when the transfer itself succeeded
     * AND both byte count and checksum match.
     */
    return passed ? 0 : -1;
}
#include "../include/syscall_profiler.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <unistd.h>

#define MAX_SYSCALLS 256
#define MAX_NAME_LEN 64


typedef struct {
    char name[MAX_NAME_LEN];
    unsigned long long calls;
    unsigned long long errors;
    double time_percent;
    double seconds;
} SyscallStat;


typedef struct {
    SyscallStat stats[MAX_SYSCALLS];
    size_t count;
    unsigned long long total_calls;
    unsigned long long total_errors;
} SyscallProfile;


/* Check supported IPC method */
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


/* Create results directory if needed */
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


/* Build raw strace filename */
static void build_raw_path(
    char *buffer,
    size_t buffer_size,
    const char *method,
    size_t payload_mb,
    size_t chunk_kb
)
{
    snprintf(
        buffer,
        buffer_size,
        "results/syscall_profile_%s_%zuMB_%zuKB.txt",
        method,
        payload_mb,
        chunk_kb
    );
}


/* Build parsed CSV filename */
static void build_csv_path(
    char *buffer,
    size_t buffer_size,
    const char *method,
    size_t payload_mb,
    size_t chunk_kb
)
{
    snprintf(
        buffer,
        buffer_size,
        "results/syscall_profile_%s_%zuMB_%zuKB.csv",
        method,
        payload_mb,
        chunk_kb
    );
}


/* Run FastIPC-X under strace */
static int execute_strace(
    const char *method,
    size_t payload_mb,
    size_t chunk_kb,
    const char *output_path
)
{
    char payload_text[32];
    char chunk_text[32];

    snprintf(
        payload_text,
        sizeof(payload_text),
        "%zu",
        payload_mb
    );

    snprintf(
        chunk_text,
        sizeof(chunk_text),
        "%zu",
        chunk_kb
    );

    pid_t pid = fork();

    if (pid < 0) {
        perror("fork");
        return -1;
    }

    if (pid == 0) {

        execlp(
            "strace",
            "strace",
            "-f",
            "-c",
            "-o",
            output_path,
            "./fastipc",
            method,
            payload_text,
            chunk_text,
            (char *)NULL
        );

        perror("execlp strace");
        _exit(127);
    }

    int status = 0;

    if (waitpid(pid, &status, 0) < 0) {
        perror("waitpid");
        return -1;
    }

    if (!WIFEXITED(status)) {
        fprintf(
            stderr,
            "strace terminated abnormally.\n"
        );

        return -1;
    }

    if (WEXITSTATUS(status) != 0) {
        fprintf(
            stderr,
            "strace exited with status %d.\n",
            WEXITSTATUS(status)
        );

        return -1;
    }

    return 0;
}


/* Parse one strace -c row */
static int parse_summary_line(
    char *line,
    SyscallStat *stat
)
{
    char copy[1024];

    strncpy(
        copy,
        line,
        sizeof(copy) - 1
    );

    copy[sizeof(copy) - 1] = '\0';

    char *tokens[16];

    size_t token_count = 0;

    char *token = strtok(
        copy,
        " \t\r\n"
    );

    while (
        token != NULL &&
        token_count < 16
    ) {
        tokens[token_count++] = token;

        token = strtok(
            NULL,
            " \t\r\n"
        );
    }

    /*
     * strace -c rows can be:
     *
     * %time seconds usecs/call calls syscall
     *
     * OR
     *
     * %time seconds usecs/call calls errors syscall
     */

    if (
        token_count != 5 &&
        token_count != 6
    ) {
        return -1;
    }

    const char *syscall_name =
        tokens[token_count - 1];

    if (
        strcmp(syscall_name, "total") == 0
    ) {
        return -1;
    }

    char *end = NULL;

    double percent =
        strtod(tokens[0], &end);

    if (
        end == tokens[0] ||
        *end != '\0'
    ) {
        return -1;
    }

    double seconds =
        strtod(tokens[1], &end);

    if (
        end == tokens[1] ||
        *end != '\0'
    ) {
        return -1;
    }

    unsigned long long calls =
        strtoull(
            tokens[3],
            &end,
            10
        );

    if (
        end == tokens[3] ||
        *end != '\0'
    ) {
        return -1;
    }

    unsigned long long errors = 0;

    if (token_count == 6) {

        errors =
            strtoull(
                tokens[4],
                &end,
                10
            );

        if (
            end == tokens[4] ||
            *end != '\0'
        ) {
            return -1;
        }
    }

    memset(
        stat,
        0,
        sizeof(*stat)
    );

    strncpy(
        stat->name,
        syscall_name,
        sizeof(stat->name) - 1
    );

    stat->name[
        sizeof(stat->name) - 1
    ] = '\0';

    stat->calls = calls;
    stat->errors = errors;
    stat->time_percent = percent;
    stat->seconds = seconds;

    return 0;
}


/* Parse complete strace output */
static int parse_strace_output(
    const char *path,
    SyscallProfile *profile
)
{
    FILE *f = fopen(path, "r");

    if (!f) {
        perror("fopen strace output");
        return -1;
    }

    memset(
        profile,
        0,
        sizeof(*profile)
    );

    char line[1024];

    while (
        fgets(
            line,
            sizeof(line),
            f
        )
    ) {

        if (
            strstr(line, "syscall") != NULL
        ) {
            continue;
        }

        if (
            strncmp(
                line,
                "------",
                6
            ) == 0
        ) {
            continue;
        }

        SyscallStat stat;

        if (
            parse_summary_line(
                line,
                &stat
            ) != 0
        ) {
            continue;
        }

        if (
            profile->count >=
            MAX_SYSCALLS
        ) {
            break;
        }

        profile->stats[
            profile->count++
        ] = stat;

        profile->total_calls +=
            stat.calls;

        profile->total_errors +=
            stat.errors;
    }

    fclose(f);

    return 0;
}


/* Save parsed system-call information */
static int save_profile_csv(
    const char *path,
    const SyscallProfile *profile
)
{
    FILE *f = fopen(path, "w");

    if (!f) {
        perror("fopen CSV");
        return -1;
    }

    fprintf(
        f,
        "syscall,calls,errors,time_percent,seconds\n"
    );

    for (
        size_t i = 0;
        i < profile->count;
        ++i
    ) {
        const SyscallStat *s =
            &profile->stats[i];

        fprintf(
            f,
            "%s,%llu,%llu,%.4f,%.6f\n",
            s->name,
            s->calls,
            s->errors,
            s->time_percent,
            s->seconds
        );
    }

    fprintf(
        f,
        "TOTAL,%llu,%llu,100.0000,0.000000\n",
        profile->total_calls,
        profile->total_errors
    );

    fclose(f);

    return 0;
}


/* Find one syscall */
static const SyscallStat *find_syscall(
    const SyscallProfile *profile,
    const char *name
)
{
    for (
        size_t i = 0;
        i < profile->count;
        ++i
    ) {
        if (
            strcmp(
                profile->stats[i].name,
                name
            ) == 0
        ) {
            return &profile->stats[i];
        }
    }

    return NULL;
}


/* Print profiler output */
static void print_profile(
    const char *method,
    size_t payload_mb,
    size_t chunk_kb,
    const SyscallProfile *profile,
    const char *csv_path,
    const char *raw_path
)
{
    printf(
        "\nFASTIPC-X SYSTEM CALL PROFILE\n"
        "============================================================\n\n"
    );

    printf(
        "Method         : %s\n",
        method
    );

    printf(
        "Payload        : %zu MB\n",
        payload_mb
    );

    printf(
        "Chunk size     : %zu KB\n\n",
        chunk_kb
    );

    printf(
        "%-20s %12s %12s %12s\n",
        "Syscall",
        "Calls",
        "Errors",
        "Time %"
    );

    printf(
        "------------------------------------------------------------\n"
    );

    for (
        size_t i = 0;
        i < profile->count;
        ++i
    ) {
        const SyscallStat *s =
            &profile->stats[i];

        printf(
            "%-20s %12llu %12llu %11.2f%%\n",
            s->name,
            s->calls,
            s->errors,
            s->time_percent
        );
    }

    printf(
        "------------------------------------------------------------\n"
    );

    printf(
        "%-20s %12llu %12llu\n",
        "TOTAL",
        profile->total_calls,
        profile->total_errors
    );

    const SyscallStat *futex =
        find_syscall(
            profile,
            "futex"
        );

    if (futex != NULL) {

        printf(
            "\nSynchronization activity:\n"
        );

        printf(
            "futex calls    : %llu\n",
            futex->calls
        );

        printf(
            "futex errors   : %llu\n",
            futex->errors
        );
    }

    printf(
        "\nParsed CSV     : %s\n",
        csv_path
    );

    printf(
        "Raw strace     : %s\n",
        raw_path
    );
}


/* Collect one complete system-call profile */
static int collect_profile(
    const char *method,
    size_t payload_mb,
    size_t chunk_kb,
    SyscallProfile *profile,
    int show_report
)
{
    if (!method_is_valid(method)) {

        fprintf(
            stderr,
            "Unsupported IPC method: %s\n",
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
            "Payload and chunk size must be greater than zero.\n"
        );

        return -1;
    }

    if (
        ensure_results_directory() != 0
    ) {
        return -1;
    }

    char raw_path[512];
    char csv_path[512];

    build_raw_path(
        raw_path,
        sizeof(raw_path),
        method,
        payload_mb,
        chunk_kb
    );

    build_csv_path(
        csv_path,
        sizeof(csv_path),
        method,
        payload_mb,
        chunk_kb
    );

    printf(
        "\nProfiling %s using strace...\n",
        method
    );

    if (
        execute_strace(
            method,
            payload_mb,
            chunk_kb,
            raw_path
        ) != 0
    ) {
        return -1;
    }

    if (
        parse_strace_output(
            raw_path,
            profile
        ) != 0
    ) {
        return -1;
    }

    if (profile->count == 0) {

        fprintf(
            stderr,
            "No syscall statistics were parsed.\n"
        );

        return -1;
    }

    if (
        save_profile_csv(
            csv_path,
            profile
        ) != 0
    ) {
        return -1;
    }

    if (show_report) {

        print_profile(
            method,
            payload_mb,
            chunk_kb,
            profile,
            csv_path,
            raw_path
        );
    }

    return 0;
}


/* Public profile command */
int run_syscall_profile(
    const char *method,
    size_t payload_mb,
    size_t chunk_kb
)
{
    SyscallProfile profile;

    return collect_profile(
        method,
        payload_mb,
        chunk_kb,
        &profile,
        1
    );
}


/* Calculate reduction percentage */
static double reduction_percent(
    double baseline,
    double optimized
)
{
    if (baseline <= 0.0) {
        return 0.0;
    }

    return (
        (baseline - optimized)
        / baseline
    ) * 100.0;
}


/* Compare two system-call profiles */
int compare_syscall_profiles(
    const char *baseline_method,
    const char *optimized_method,
    size_t payload_mb,
    size_t chunk_kb
)
{
    SyscallProfile baseline;
    SyscallProfile optimized;

    printf(
        "\nCollecting baseline syscall profile...\n"
    );

    if (
        collect_profile(
            baseline_method,
            payload_mb,
            chunk_kb,
            &baseline,
            0
        ) != 0
    ) {
        return -1;
    }

    printf(
        "\nCollecting optimized syscall profile...\n"
    );

    if (
        collect_profile(
            optimized_method,
            payload_mb,
            chunk_kb,
            &optimized,
            0
        ) != 0
    ) {
        return -1;
    }

    const SyscallStat *baseline_futex =
        find_syscall(
            &baseline,
            "futex"
        );

    const SyscallStat *optimized_futex =
        find_syscall(
            &optimized,
            "futex"
        );

    unsigned long long baseline_futex_calls =
        baseline_futex ?
        baseline_futex->calls :
        0;

    unsigned long long optimized_futex_calls =
        optimized_futex ?
        optimized_futex->calls :
        0;

    unsigned long long baseline_futex_errors =
        baseline_futex ?
        baseline_futex->errors :
        0;

    unsigned long long optimized_futex_errors =
        optimized_futex ?
        optimized_futex->errors :
        0;

    printf(
        "\nFASTIPC-X SYSCALL OPTIMIZATION COMPARISON\n"
        "====================================================================\n\n"
    );

    printf(
        "Payload : %zu MB\n",
        payload_mb
    );

    printf(
        "Chunk   : %zu KB\n\n",
        chunk_kb
    );

    printf(
        "%-28s %14s %14s\n",
        "Metric",
        baseline_method,
        optimized_method
    );

    printf(
        "--------------------------------------------------------------------\n"
    );

    printf(
        "%-28s %14llu %14llu\n",
        "Total syscalls",
        baseline.total_calls,
        optimized.total_calls
    );

    printf(
        "%-28s %14llu %14llu\n",
        "Total syscall errors",
        baseline.total_errors,
        optimized.total_errors
    );

    printf(
        "%-28s %14llu %14llu\n",
        "futex calls",
        baseline_futex_calls,
        optimized_futex_calls
    );

    printf(
        "%-28s %14llu %14llu\n",
        "futex errors",
        baseline_futex_errors,
        optimized_futex_errors
    );

    printf(
        "\nOptimization effect:\n"
    );

    printf(
        "Total syscall reduction : %.2f%%\n",
        reduction_percent(
            (double)baseline.total_calls,
            (double)optimized.total_calls
        )
    );

    if (baseline_futex_calls > 0) {

        printf(
            "futex call reduction    : %.2f%%\n",
            reduction_percent(
                (double)baseline_futex_calls,
                (double)optimized_futex_calls
            )
        );
    }

    if (baseline_futex_errors > 0) {

        printf(
            "futex error reduction   : %.2f%%\n",
            reduction_percent(
                (double)baseline_futex_errors,
                (double)optimized_futex_errors
            )
        );
    }

    char comparison_path[512];

    snprintf(
        comparison_path,
        sizeof(comparison_path),
        "results/syscall_comparison_%s_vs_%s_%zuMB_%zuKB.csv",
        baseline_method,
        optimized_method,
        payload_mb,
        chunk_kb
    );

    FILE *f = fopen(
        comparison_path,
        "w"
    );

    if (f) {

        fprintf(
            f,
            "metric,baseline,optimized,reduction_percent\n"
        );

        fprintf(
            f,
            "total_syscalls,%llu,%llu,%.6f\n",
            baseline.total_calls,
            optimized.total_calls,
            reduction_percent(
                (double)baseline.total_calls,
                (double)optimized.total_calls
            )
        );

        fprintf(
            f,
            "total_errors,%llu,%llu,%.6f\n",
            baseline.total_errors,
            optimized.total_errors,
            reduction_percent(
                (double)baseline.total_errors,
                (double)optimized.total_errors
            )
        );

        fprintf(
            f,
            "futex_calls,%llu,%llu,%.6f\n",
            baseline_futex_calls,
            optimized_futex_calls,
            reduction_percent(
                (double)baseline_futex_calls,
                (double)optimized_futex_calls
            )
        );

        fprintf(
            f,
            "futex_errors,%llu,%llu,%.6f\n",
            baseline_futex_errors,
            optimized_futex_errors,
            reduction_percent(
                (double)baseline_futex_errors,
                (double)optimized_futex_errors
            )
        );

        fclose(f);

        printf(
            "\nComparison CSV: %s\n",
            comparison_path
        );
    }

    return 0;
}
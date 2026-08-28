#define _POSIX_C_SOURCE 200809L

#include "../include/final_summary.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>


#define LINE_BUFFER 8192
#define PATH_BUFFER 512
#define MAX_FIELDS 64
#define MAX_INTEGRITY_FILES 64


/* =========================================================
   Writer
   ========================================================= */

typedef struct {
    FILE *txt;
    FILE *csv;
    int missing_sections;
} SummaryWriter;


/* =========================================================
   Generic row structures
   ========================================================= */

typedef struct {
    char method[64];
    double median_ms;
    double throughput_mbps;
} BaselineRow;


typedef struct {
    char method[64];

    int baseline_chunk_kb;
    int best_chunk_kb;

    double baseline_ms;
    double best_ms;
    double latency_improvement;

    double baseline_throughput;
    double best_throughput;
    double throughput_improvement;
} ChunkRow;


typedef struct {
    char variant[64];

    double median_ms;
    double throughput_mbps;

    double user_ms;
    double system_ms;

    double voluntary_ctx_switches;
    double involuntary_ctx_switches;
} ShmSyncRow;


typedef struct {
    int slot_count;

    double median_ms;
    double throughput_mbps;

    double system_ms;
    double voluntary_ctx_switches;

    int selected;
} RingSlotRow;


typedef struct {
    char mode[64];

    int producer_cpu;
    int consumer_cpu;

    double median_ms;
    double throughput_mbps;

    double user_ms;
    double system_ms;

    double voluntary_ctx_switches;
    double involuntary_ctx_switches;

    int selected;
} AffinityRow;


typedef struct {
    char mode[64];

    double setup_ms;
    double timed_ms;
    double total_ms;
    double throughput_mbps;

    double setup_minor_faults;
    double timed_minor_faults;

    double setup_major_faults;
    double timed_major_faults;

    double timed_system_ms;

    double voluntary_ctx_switches;
    double involuntary_ctx_switches;

    int selected_critical;
    int selected_strategy_total;
} MemoryRow;


typedef struct {
    char filename[PATH_BUFFER];

    char method[64];

    int payload_mb;
    int chunk_kb;

    unsigned long long bytes_sent;
    unsigned long long bytes_received;

    char sender_checksum[128];
    char receiver_checksum[128];

    char result[32];
} IntegrityResult;


/* =========================================================
   Small string helpers
   ========================================================= */

static void trim_newline(
    char *text
)
{
    if (!text) {
        return;
    }

    size_t length =
        strlen(
            text
        );

    while (
        length > 0 &&
        (
            text[length - 1] == '\n' ||
            text[length - 1] == '\r'
        )
    ) {
        text[length - 1] =
            '\0';

        --length;
    }
}


static void trim_spaces(
    char *text
)
{
    if (!text) {
        return;
    }

    char *start =
        text;

    while (
        *start == ' ' ||
        *start == '\t'
    ) {
        ++start;
    }

    if (
        start != text
    ) {
        memmove(
            text,
            start,
            strlen(start) + 1
        );
    }

    size_t length =
        strlen(
            text
        );

    while (
        length > 0 &&
        (
            text[length - 1] == ' ' ||
            text[length - 1] == '\t'
        )
    ) {
        text[length - 1] =
            '\0';

        --length;
    }
}


static void strip_outer_quotes(
    char *text
)
{
    if (!text) {
        return;
    }

    size_t length =
        strlen(
            text
        );

    if (
        length >= 2 &&
        text[0] == '"' &&
        text[length - 1] == '"'
    ) {
        memmove(
            text,
            text + 1,
            length - 2
        );

        text[length - 2] =
            '\0';
    }
}


static int starts_with(
    const char *text,
    const char *prefix
)
{
    if (
        !text ||
        !prefix
    ) {
        return 0;
    }

    size_t prefix_length =
        strlen(
            prefix
        );

    return
        strncmp(
            text,
            prefix,
            prefix_length
        ) == 0;
}


static int ends_with(
    const char *text,
    const char *suffix
)
{
    if (
        !text ||
        !suffix
    ) {
        return 0;
    }

    size_t text_length =
        strlen(
            text
        );

    size_t suffix_length =
        strlen(
            suffix
        );

    if (
        suffix_length >
        text_length
    ) {
        return 0;
    }

    return
        strcmp(
            text +
            text_length -
            suffix_length,
            suffix
        ) == 0;
}


/* =========================================================
   Simple CSV field splitter

   FastIPC-X experiment CSVs do not contain embedded commas
   inside benchmark values. Environment fields are quoted but
   also do not currently contain embedded commas.
   ========================================================= */

static size_t split_csv(
    char *line,
    char *fields[],
    size_t max_fields
)
{
    if (
        !line ||
        !fields ||
        max_fields == 0
    ) {
        return 0;
    }

    trim_newline(
        line
    );

    size_t count =
        0;

    char *current =
        line;

    while (
        current &&
        *current != '\0' &&
        count < max_fields
    ) {
        fields[count++] =
            current;

        char *comma =
            strchr(
                current,
                ','
            );

        if (!comma) {
            break;
        }

        *comma =
            '\0';

        current =
            comma + 1;
    }

    for (
        size_t i = 0;
        i < count;
        ++i
    ) {
        trim_spaces(
            fields[i]
        );

        strip_outer_quotes(
            fields[i]
        );
    }

    return count;
}


/* =========================================================
   CSV output escaping
   ========================================================= */

static void write_csv_value(
    FILE *file,
    const char *value
)
{
    if (!file) {
        return;
    }

    if (!value) {
        value =
            "";
    }

    fputc(
        '"',
        file
    );

    for (
        const char *p = value;
        *p != '\0';
        ++p
    ) {
        if (
            *p == '"'
        ) {
            fputc(
                '"',
                file
            );

            fputc(
                '"',
                file
            );
        }
        else {
            fputc(
                *p,
                file
            );
        }
    }

    fputc(
        '"',
        file
    );
}


/* =========================================================
   Summary output helpers
   ========================================================= */

static void begin_section(
    SummaryWriter *writer,
    const char *title
)
{
    if (
        !writer ||
        !writer->txt ||
        !title
    ) {
        return;
    }

    fprintf(
        writer->txt,
        "\n%s\n",
        title
    );

    fprintf(
        writer->txt,
        "------------------------------------------------------------\n"
    );
}


static void emit_metric(
    SummaryWriter *writer,
    const char *section,
    const char *metric,
    const char *value,
    const char *unit,
    const char *note
)
{
    if (
        !writer ||
        !writer->txt ||
        !writer->csv
    ) {
        return;
    }

    if (!section) {
        section =
            "";
    }

    if (!metric) {
        metric =
            "";
    }

    if (!value) {
        value =
            "";
    }

    if (!unit) {
        unit =
            "";
    }

    if (!note) {
        note =
            "";
    }

    fprintf(
        writer->txt,
        "%-31s : %s",
        metric,
        value
    );

    if (
        unit[0] != '\0'
    ) {
        fprintf(
            writer->txt,
            " %s",
            unit
        );
    }

    if (
        note[0] != '\0'
    ) {
        fprintf(
            writer->txt,
            "  [%s]",
            note
        );
    }

    fprintf(
        writer->txt,
        "\n"
    );


    write_csv_value(
        writer->csv,
        section
    );

    fputc(
        ',',
        writer->csv
    );

    write_csv_value(
        writer->csv,
        metric
    );

    fputc(
        ',',
        writer->csv
    );

    write_csv_value(
        writer->csv,
        value
    );

    fputc(
        ',',
        writer->csv
    );

    write_csv_value(
        writer->csv,
        unit
    );

    fputc(
        ',',
        writer->csv
    );

    write_csv_value(
        writer->csv,
        note
    );

    fputc(
        '\n',
        writer->csv
    );
}


static void emit_double(
    SummaryWriter *writer,
    const char *section,
    const char *metric,
    double value,
    const char *unit,
    const char *note
)
{
    char buffer[
        128
    ];

    snprintf(
        buffer,
        sizeof(buffer),
        "%.6f",
        value
    );

    emit_metric(
        writer,
        section,
        metric,
        buffer,
        unit,
        note
    );
}


static void emit_integer(
    SummaryWriter *writer,
    const char *section,
    const char *metric,
    long long value,
    const char *unit,
    const char *note
)
{
    char buffer[
        128
    ];

    snprintf(
        buffer,
        sizeof(buffer),
        "%lld",
        value
    );

    emit_metric(
        writer,
        section,
        metric,
        buffer,
        unit,
        note
    );
}


static void mark_missing(
    SummaryWriter *writer,
    const char *section,
    const char *path
)
{
    if (!writer) {
        return;
    }

    ++writer->missing_sections;

    begin_section(
        writer,
        section
    );

    emit_metric(
        writer,
        section,
        "Status",
        "UNAVAILABLE",
        "",
        path
    );
}


/* =========================================================
   Percentage helpers
   ========================================================= */

static double reduction_percent(
    double baseline,
    double optimized
)
{
    if (
        baseline == 0.0
    ) {
        return 0.0;
    }

    return
        (
            baseline -
            optimized
        ) /
        baseline *
        100.0;
}


static double improvement_percent(
    double baseline,
    double optimized
)
{
    if (
        baseline == 0.0
    ) {
        return 0.0;
    }

    return
        (
            optimized -
            baseline
        ) /
        baseline *
        100.0;
}


/* =========================================================
   Baseline IPC comparison
   ========================================================= */

static int summarize_baseline(
    SummaryWriter *writer
)
{
    const char *path =
        "results/benchmark_100MB.csv";

    FILE *file =
        fopen(
            path,
            "r"
        );

    if (!file) {
        mark_missing(
            writer,
            "Baseline IPC Comparison",
            path
        );

        return -1;
    }

    char line[
        LINE_BUFFER
    ];

    if (
        !fgets(
            line,
            sizeof(line),
            file
        )
    ) {
        fclose(file);

        mark_missing(
            writer,
            "Baseline IPC Comparison",
            path
        );

        return -1;
    }

    BaselineRow rows[
        32
    ];

    size_t count =
        0;

    while (
        fgets(
            line,
            sizeof(line),
            file
        ) &&
        count <
        sizeof(rows) /
        sizeof(rows[0])
    ) {
        char *fields[
            MAX_FIELDS
        ];

        size_t fields_count =
            split_csv(
                line,
                fields,
                MAX_FIELDS
            );

        if (
            fields_count <
            10
        ) {
            continue;
        }

        snprintf(
            rows[count].method,
            sizeof(rows[count].method),
            "%s",
            fields[0]
        );

        rows[count].median_ms =
            strtod(
                fields[7],
                NULL
            );

        rows[count].throughput_mbps =
            strtod(
                fields[9],
                NULL
            );

        ++count;
    }

    fclose(
        file
    );

    if (
        count == 0
    ) {
        mark_missing(
            writer,
            "Baseline IPC Comparison",
            path
        );

        return -1;
    }

    size_t best =
        0;

    for (
        size_t i = 1;
        i < count;
        ++i
    ) {
        if (
            rows[i].median_ms <
            rows[best].median_ms
        ) {
            best =
                i;
        }
    }

    begin_section(
        writer,
        "Baseline IPC Comparison"
    );

    for (
        size_t i = 0;
        i < count;
        ++i
    ) {
        char metric[
            128
        ];

        snprintf(
            metric,
            sizeof(metric),
            "%.63s median latency",
            rows[i].method
        );

        emit_double(
            writer,
            "Baseline IPC Comparison",
            metric,
            rows[i].median_ms,
            "ms",
            i == best
            ?
            "lowest baseline latency"
            :
            ""
        );

        snprintf(
            metric,
            sizeof(metric),
            "%.63s median throughput",
            rows[i].method
        );

        emit_double(
            writer,
            "Baseline IPC Comparison",
            metric,
            rows[i].throughput_mbps,
            "MB/s",
            ""
        );
    }

    emit_metric(
        writer,
        "Baseline IPC Comparison",
        "Baseline latency winner",
        rows[best].method,
        "",
        "100 MB, 64 KB baseline experiment"
    );

    return 0;
}


/* =========================================================
   Chunk optimization
   ========================================================= */

static int summarize_chunk_optimization(
    SummaryWriter *writer
)
{
    const char *path =
        "results/chunk_optimization_summary_100MB.csv";

    FILE *file =
        fopen(
            path,
            "r"
        );

    if (!file) {
        mark_missing(
            writer,
            "Chunk-Size Optimization",
            path
        );

        return -1;
    }

    char line[
        LINE_BUFFER
    ];

    if (
        !fgets(
            line,
            sizeof(line),
            file
        )
    ) {
        fclose(file);

        mark_missing(
            writer,
            "Chunk-Size Optimization",
            path
        );

        return -1;
    }

    begin_section(
        writer,
        "Chunk-Size Optimization"
    );

    size_t rows =
        0;

    while (
        fgets(
            line,
            sizeof(line),
            file
        )
    ) {
        char *fields[
            MAX_FIELDS
        ];

        size_t count =
            split_csv(
                line,
                fields,
                MAX_FIELDS
            );

        if (
            count <
            9
        ) {
            continue;
        }

        ChunkRow row;

        memset(
            &row,
            0,
            sizeof(row)
        );

        snprintf(
            row.method,
            sizeof(row.method),
            "%s",
            fields[0]
        );

        row.baseline_chunk_kb =
            atoi(
                fields[1]
            );

        row.best_chunk_kb =
            atoi(
                fields[2]
            );

        row.baseline_ms =
            strtod(
                fields[3],
                NULL
            );

        row.best_ms =
            strtod(
                fields[4],
                NULL
            );

        row.latency_improvement =
            strtod(
                fields[5],
                NULL
            );

        row.baseline_throughput =
            strtod(
                fields[6],
                NULL
            );

        row.best_throughput =
            strtod(
                fields[7],
                NULL
            );

        row.throughput_improvement =
            strtod(
                fields[8],
                NULL
            );


        char metric[
            128
        ];

        char value[
            128
        ];


        snprintf(
            metric,
            sizeof(metric),
            "%s best chunk",
            row.method
        );

        snprintf(
            value,
            sizeof(value),
            "%d",
            row.best_chunk_kb
        );

        emit_metric(
            writer,
            "Chunk-Size Optimization",
            metric,
            value,
            "KB",
            ""
        );


        snprintf(
            metric,
            sizeof(metric),
            "%s latency improvement",
            row.method
        );

        emit_double(
            writer,
            "Chunk-Size Optimization",
            metric,
            row.latency_improvement,
            "%",
            ""
        );


        snprintf(
            metric,
            sizeof(metric),
            "%s throughput improvement",
            row.method
        );

        emit_double(
            writer,
            "Chunk-Size Optimization",
            metric,
            row.throughput_improvement,
            "%",
            ""
        );

        ++rows;
    }

    fclose(
        file
    );

    return
        rows > 0
        ?
        0
        :
        -1;
}


/* =========================================================
   SHM synchronization optimization
   ========================================================= */

static int summarize_shm_sync(
    SummaryWriter *writer
)
{
    const char *path =
        "results/shm_sync_optimization_100MB_64KB.csv";

    FILE *file =
        fopen(
            path,
            "r"
        );

    if (!file) {
        mark_missing(
            writer,
            "SHM Synchronization Optimization",
            path
        );

        return -1;
    }

    char line[
        LINE_BUFFER
    ];

    if (
        !fgets(
            line,
            sizeof(line),
            file
        )
    ) {
        fclose(file);

        mark_missing(
            writer,
            "SHM Synchronization Optimization",
            path
        );

        return -1;
    }

    ShmSyncRow baseline;
    ShmSyncRow ring;

    memset(
        &baseline,
        0,
        sizeof(baseline)
    );

    memset(
        &ring,
        0,
        sizeof(ring)
    );

    int have_baseline =
        0;

    int have_ring =
        0;

    while (
        fgets(
            line,
            sizeof(line),
            file
        )
    ) {
        char *fields[
            MAX_FIELDS
        ];

        size_t count =
            split_csv(
                line,
                fields,
                MAX_FIELDS
            );

        if (
            count <
            9
        ) {
            continue;
        }

        ShmSyncRow row;

        memset(
            &row,
            0,
            sizeof(row)
        );

        snprintf(
            row.variant,
            sizeof(row.variant),
            "%s",
            fields[0]
        );

        row.median_ms =
            strtod(
                fields[3],
                NULL
            );

        row.throughput_mbps =
            strtod(
                fields[4],
                NULL
            );

        row.user_ms =
            strtod(
                fields[5],
                NULL
            );

        row.system_ms =
            strtod(
                fields[6],
                NULL
            );

        row.voluntary_ctx_switches =
            strtod(
                fields[7],
                NULL
            );

        row.involuntary_ctx_switches =
            strtod(
                fields[8],
                NULL
            );

        if (
            strcmp(
                row.variant,
                "baseline"
            ) == 0
        ) {
            baseline =
                row;

            have_baseline =
                1;
        }
        else if (
            strcmp(
                row.variant,
                "ringbuffer"
            ) == 0
        ) {
            ring =
                row;

            have_ring =
                1;
        }
    }

    fclose(
        file
    );

    if (
        !have_baseline ||
        !have_ring
    ) {
        mark_missing(
            writer,
            "SHM Synchronization Optimization",
            path
        );

        return -1;
    }

    begin_section(
        writer,
        "SHM Synchronization Optimization"
    );

    emit_double(
        writer,
        "SHM Synchronization Optimization",
        "Baseline median latency",
        baseline.median_ms,
        "ms",
        ""
    );

    emit_double(
        writer,
        "SHM Synchronization Optimization",
        "SHM-RING median latency",
        ring.median_ms,
        "ms",
        ""
    );

    emit_double(
        writer,
        "SHM Synchronization Optimization",
        "Latency reduction",
        reduction_percent(
            baseline.median_ms,
            ring.median_ms
        ),
        "%",
        ""
    );

    emit_double(
        writer,
        "SHM Synchronization Optimization",
        "Throughput improvement",
        improvement_percent(
            baseline.throughput_mbps,
            ring.throughput_mbps
        ),
        "%",
        ""
    );

    emit_double(
        writer,
        "SHM Synchronization Optimization",
        "System CPU reduction",
        reduction_percent(
            baseline.system_ms,
            ring.system_ms
        ),
        "%",
        ""
    );

    emit_double(
        writer,
        "SHM Synchronization Optimization",
        "Voluntary context-switch reduction",
        reduction_percent(
            baseline.voluntary_ctx_switches,
            ring.voluntary_ctx_switches
        ),
        "%",
        ""
    );

    return 0;
}


/* =========================================================
   System-call comparison
   ========================================================= */

static int summarize_syscalls(
    SummaryWriter *writer
)
{
    const char *path =
        "results/syscall_comparison_shm_vs_shm-opt_100MB_64KB.csv";

    FILE *file =
        fopen(
            path,
            "r"
        );

    if (!file) {
        mark_missing(
            writer,
            "System-Call Reduction",
            path
        );

        return -1;
    }

    char line[
        LINE_BUFFER
    ];

    if (
        !fgets(
            line,
            sizeof(line),
            file
        )
    ) {
        fclose(file);

        mark_missing(
            writer,
            "System-Call Reduction",
            path
        );

        return -1;
    }

    begin_section(
        writer,
        "System-Call Reduction"
    );

    size_t rows =
        0;

    while (
        fgets(
            line,
            sizeof(line),
            file
        )
    ) {
        char *fields[
            MAX_FIELDS
        ];

        size_t count =
            split_csv(
                line,
                fields,
                MAX_FIELDS
            );

        if (
            count <
            4
        ) {
            continue;
        }

        char value[
            256
        ];

        char note[
            256
        ];

        snprintf(
            value,
            sizeof(value),
            "%s -> %s",
            fields[1],
            fields[2]
        );

        snprintf(
            note,
            sizeof(note),
            "%s%% reduction",
            fields[3]
        );

        emit_metric(
            writer,
            "System-Call Reduction",
            fields[0],
            value,
            "",
            note
        );

        ++rows;
    }

    fclose(
        file
    );

    return
        rows > 0
        ?
        0
        :
        -1;
}


/* =========================================================
   Adaptive workload selection
   ========================================================= */

static int summarize_workloads(
    SummaryWriter *writer
)
{
    const char *path =
        "results/workload_adaptive_summary.csv";

    FILE *file =
        fopen(
            path,
            "r"
        );

    if (!file) {
        mark_missing(
            writer,
            "Adaptive Workload Selection",
            path
        );

        return -1;
    }

    char line[
        LINE_BUFFER
    ];

    if (
        !fgets(
            line,
            sizeof(line),
            file
        )
    ) {
        fclose(file);

        mark_missing(
            writer,
            "Adaptive Workload Selection",
            path
        );

        return -1;
    }

    begin_section(
        writer,
        "Adaptive Workload Selection"
    );

    size_t rows =
        0;

    while (
        fgets(
            line,
            sizeof(line),
            file
        )
    ) {
        char *fields[
            MAX_FIELDS
        ];

        size_t count =
            split_csv(
                line,
                fields,
                MAX_FIELDS
            );

        if (
            count <
            10
        ) {
            continue;
        }

        char metric[
            128
        ];

        char value[
            256
        ];

        char note[
            256
        ];

        snprintf(
            metric,
            sizeof(metric),
            "%s MB workload",
            fields[0]
        );

        snprintf(
            value,
            sizeof(value),
            "%s / %s KB",
            fields[2],
            fields[3]
        );

        snprintf(
            note,
            sizeof(note),
            "%s, median %s ms, %s MB/s",
            fields[1],
            fields[4],
            fields[5]
        );

        emit_metric(
            writer,
            "Adaptive Workload Selection",
            metric,
            value,
            "",
            note
        );

        ++rows;
    }

    fclose(
        file
    );

    return
        rows > 0
        ?
        0
        :
        -1;
}


/* =========================================================
   Integrity verification
   ========================================================= */

static int compare_integrity_filename(
    const void *left,
    const void *right
)
{
    const IntegrityResult *a =
        (const IntegrityResult *)left;

    const IntegrityResult *b =
        (const IntegrityResult *)right;

    return
        strcmp(
            a->filename,
            b->filename
        );
}


static int summarize_integrity(
    SummaryWriter *writer
)
{
    DIR *directory =
        opendir(
            "results"
        );

    if (!directory) {
        mark_missing(
            writer,
            "Data Integrity",
            "results/"
        );

        return -1;
    }

    IntegrityResult results[
        MAX_INTEGRITY_FILES
    ];

    size_t count =
        0;

    struct dirent *entry;

    while (
        (
            entry =
            readdir(
                directory
            )
        ) != NULL
    ) {
        if (
            !starts_with(
                entry->d_name,
                "integrity_"
            ) ||
            !ends_with(
                entry->d_name,
                ".csv"
            )
        ) {
            continue;
        }

        if (
            count >=
            MAX_INTEGRITY_FILES
        ) {
            break;
        }

        char path[
            PATH_BUFFER
        ];

        snprintf(
            path,
            sizeof(path),
            "results/%s",
            entry->d_name
        );

        FILE *file =
            fopen(
                path,
                "r"
            );

        if (!file) {
            continue;
        }

        char line[
            LINE_BUFFER
        ];

        if (
            !fgets(
                line,
                sizeof(line),
                file
            ) ||
            !fgets(
                line,
                sizeof(line),
                file
            )
        ) {
            fclose(file);

            continue;
        }

        fclose(
            file
        );

        char *fields[
            MAX_FIELDS
        ];

        size_t fields_count =
            split_csv(
                line,
                fields,
                MAX_FIELDS
            );

        if (
            fields_count <
            8
        ) {
            continue;
        }

        IntegrityResult *result =
            &results[count];

        memset(
            result,
            0,
            sizeof(*result)
        );

        snprintf(
            result->filename,
            sizeof(result->filename),
            "%s",
            entry->d_name
        );

        snprintf(
            result->method,
            sizeof(result->method),
            "%s",
            fields[0]
        );

        result->payload_mb =
            atoi(
                fields[1]
            );

        result->chunk_kb =
            atoi(
                fields[2]
            );

        result->bytes_sent =
            strtoull(
                fields[3],
                NULL,
                10
            );

        result->bytes_received =
            strtoull(
                fields[4],
                NULL,
                10
            );

        snprintf(
            result->sender_checksum,
            sizeof(result->sender_checksum),
            "%s",
            fields[5]
        );

        snprintf(
            result->receiver_checksum,
            sizeof(result->receiver_checksum),
            "%s",
            fields[6]
        );

        snprintf(
            result->result,
            sizeof(result->result),
            "%s",
            fields[7]
        );

        ++count;
    }

    closedir(
        directory
    );

    if (
        count == 0
    ) {
        mark_missing(
            writer,
            "Data Integrity",
            "results/integrity_*.csv"
        );

        return -1;
    }

    qsort(
        results,
        count,
        sizeof(results[0]),
        compare_integrity_filename
    );

    begin_section(
        writer,
        "Data Integrity"
    );

    size_t pass_count =
        0;

    for (
        size_t i = 0;
        i < count;
        ++i
    ) {
        if (
            strcmp(
                results[i].result,
                "PASS"
            ) == 0
        ) {
            ++pass_count;
        }

        char metric[
            160
        ];

        char value[
            128
        ];

        char note[
            256
        ];

        snprintf(
            metric,
            sizeof(metric),
            "%s %d MB / %d KB",
            results[i].method,
            results[i].payload_mb,
            results[i].chunk_kb
        );

        snprintf(
            value,
            sizeof(value),
            "%s",
            results[i].result
        );

        snprintf(
            note,
            sizeof(note),
            "sent=%llu received=%llu checksum=%s",
            results[i].bytes_sent,
            results[i].bytes_received,
            results[i].sender_checksum
        );

        emit_metric(
            writer,
            "Data Integrity",
            metric,
            value,
            "",
            note
        );
    }

    char value[
        128
    ];

    snprintf(
        value,
        sizeof(value),
        "%zu/%zu",
        pass_count,
        count
    );

    emit_metric(
        writer,
        "Data Integrity",
        "Verification passes",
        value,
        "",
        pass_count == count
        ?
        "all recorded integrity tests passed"
        :
        "one or more recorded tests failed"
    );

    return 0;
}


/* =========================================================
   Ring-slot experiment
   ========================================================= */

static int summarize_ring_slots(
    SummaryWriter *writer
)
{
    const char *path =
        "results/shm_ring_slot_summary_100MB_64KB.csv";

    FILE *file =
        fopen(
            path,
            "r"
        );

    if (!file) {
        mark_missing(
            writer,
            "SHM Ring Slot Sweep",
            path
        );

        return -1;
    }

    char line[
        LINE_BUFFER
    ];

    if (
        !fgets(
            line,
            sizeof(line),
            file
        )
    ) {
        fclose(file);

        mark_missing(
            writer,
            "SHM Ring Slot Sweep",
            path
        );

        return -1;
    }

    RingSlotRow selected;

    memset(
        &selected,
        0,
        sizeof(selected)
    );

    int found =
        0;

    while (
        fgets(
            line,
            sizeof(line),
            file
        )
    ) {
        char *fields[
            MAX_FIELDS
        ];

        size_t count =
            split_csv(
                line,
                fields,
                MAX_FIELDS
            );

        if (
            count <
            6
        ) {
            continue;
        }

        RingSlotRow row;

        memset(
            &row,
            0,
            sizeof(row)
        );

        row.slot_count =
            atoi(
                fields[0]
            );

        row.median_ms =
            strtod(
                fields[1],
                NULL
            );

        row.throughput_mbps =
            strtod(
                fields[2],
                NULL
            );

        row.system_ms =
            strtod(
                fields[3],
                NULL
            );

        row.voluntary_ctx_switches =
            strtod(
                fields[4],
                NULL
            );

        row.selected =
            atoi(
                fields[5]
            );

        if (
            row.selected
        ) {
            selected =
                row;

            found =
                1;
        }
    }

    fclose(
        file
    );

    if (!found) {
        mark_missing(
            writer,
            "SHM Ring Slot Sweep",
            path
        );

        return -1;
    }

    begin_section(
        writer,
        "SHM Ring Slot Sweep"
    );

    emit_integer(
        writer,
        "SHM Ring Slot Sweep",
        "Controlled sweep candidate",
        selected.slot_count,
        "slots",
        "microbenchmark candidate only"
    );

    emit_double(
        writer,
        "SHM Ring Slot Sweep",
        "Candidate median latency",
        selected.median_ms,
        "ms",
        ""
    );

    emit_double(
        writer,
        "SHM Ring Slot Sweep",
        "Candidate median throughput",
        selected.throughput_mbps,
        "MB/s",
        ""
    );

    emit_metric(
        writer,
        "SHM Ring Slot Sweep",
        "Interpretation",
        "CONTROLLED MICROBENCHMARK",
        "",
        "do not equate slot-sweep selection with automatic production adoption"
    );

    return 0;
}


/* =========================================================
   CPU affinity
   ========================================================= */

static int summarize_affinity(
    SummaryWriter *writer
)
{
    const char *path =
        "results/cpu_affinity_summary_100MB_64KB.csv";

    FILE *file =
        fopen(
            path,
            "r"
        );

    if (!file) {
        mark_missing(
            writer,
            "CPU Affinity / Scheduler Analysis",
            path
        );

        return -1;
    }

    char line[
        LINE_BUFFER
    ];

    if (
        !fgets(
            line,
            sizeof(line),
            file
        )
    ) {
        fclose(file);

        mark_missing(
            writer,
            "CPU Affinity / Scheduler Analysis",
            path
        );

        return -1;
    }

    AffinityRow unpinned;
    AffinityRow selected;

    memset(
        &unpinned,
        0,
        sizeof(unpinned)
    );

    memset(
        &selected,
        0,
        sizeof(selected)
    );

    int have_unpinned =
        0;

    int have_selected =
        0;

    while (
        fgets(
            line,
            sizeof(line),
            file
        )
    ) {
        char *fields[
            MAX_FIELDS
        ];

        size_t count =
            split_csv(
                line,
                fields,
                MAX_FIELDS
            );

        if (
            count <
            10
        ) {
            continue;
        }

        AffinityRow row;

        memset(
            &row,
            0,
            sizeof(row)
        );

        snprintf(
            row.mode,
            sizeof(row.mode),
            "%s",
            fields[0]
        );

        row.producer_cpu =
            atoi(
                fields[1]
            );

        row.consumer_cpu =
            atoi(
                fields[2]
            );

        row.median_ms =
            strtod(
                fields[3],
                NULL
            );

        row.throughput_mbps =
            strtod(
                fields[4],
                NULL
            );

        row.user_ms =
            strtod(
                fields[5],
                NULL
            );

        row.system_ms =
            strtod(
                fields[6],
                NULL
            );

        row.voluntary_ctx_switches =
            strtod(
                fields[7],
                NULL
            );

        row.involuntary_ctx_switches =
            strtod(
                fields[8],
                NULL
            );

        row.selected =
            atoi(
                fields[9]
            );

        if (
            strcmp(
                row.mode,
                "UNPINNED"
            ) == 0
        ) {
            unpinned =
                row;

            have_unpinned =
                1;
        }

        if (
            row.selected
        ) {
            selected =
                row;

            have_selected =
                1;
        }
    }

    fclose(
        file
    );

    if (
        !have_unpinned ||
        !have_selected
    ) {
        mark_missing(
            writer,
            "CPU Affinity / Scheduler Analysis",
            path
        );

        return -1;
    }

    begin_section(
        writer,
        "CPU Affinity / Scheduler Analysis"
    );

    emit_metric(
        writer,
        "CPU Affinity / Scheduler Analysis",
        "Best measured mode",
        selected.mode,
        "",
        "experimental candidate"
    );

    emit_double(
        writer,
        "CPU Affinity / Scheduler Analysis",
        "Selected median latency",
        selected.median_ms,
        "ms",
        ""
    );

    emit_double(
        writer,
        "CPU Affinity / Scheduler Analysis",
        "Selected median throughput",
        selected.throughput_mbps,
        "MB/s",
        ""
    );

    emit_double(
        writer,
        "CPU Affinity / Scheduler Analysis",
        "Latency reduction vs unpinned",
        reduction_percent(
            unpinned.median_ms,
            selected.median_ms
        ),
        "%",
        ""
    );

    emit_double(
        writer,
        "CPU Affinity / Scheduler Analysis",
        "Throughput improvement vs unpinned",
        improvement_percent(
            unpinned.throughput_mbps,
            selected.throughput_mbps
        ),
        "%",
        ""
    );

    emit_double(
        writer,
        "CPU Affinity / Scheduler Analysis",
        "Selected voluntary context switches",
        selected.voluntary_ctx_switches,
        "switches",
        ""
    );

    emit_double(
        writer,
        "CPU Affinity / Scheduler Analysis",
        "Selected involuntary context switches",
        selected.involuntary_ctx_switches,
        "switches",
        ""
    );

    emit_metric(
        writer,
        "CPU Affinity / Scheduler Analysis",
        "Production interpretation",
        "EXPERIMENTAL",
        "",
        "affinity result is hardware, kernel, topology and workload dependent"
    );

    return 0;
}


/* =========================================================
   Virtual memory / page faults
   ========================================================= */

static int summarize_memory(
    SummaryWriter *writer
)
{
    const char *path =
        "results/memory_summary_100MB_64KB.csv";

    FILE *file =
        fopen(
            path,
            "r"
        );

    if (!file) {
        mark_missing(
            writer,
            "Virtual Memory / Page Faults",
            path
        );

        return -1;
    }

    char line[
        LINE_BUFFER
    ];

    if (
        !fgets(
            line,
            sizeof(line),
            file
        )
    ) {
        fclose(file);

        mark_missing(
            writer,
            "Virtual Memory / Page Faults",
            path
        );

        return -1;
    }

    MemoryRow demand;
    MemoryRow critical;
    MemoryRow strategy;

    memset(
        &demand,
        0,
        sizeof(demand)
    );

    memset(
        &critical,
        0,
        sizeof(critical)
    );

    memset(
        &strategy,
        0,
        sizeof(strategy)
    );

    int have_demand =
        0;

    int have_critical =
        0;

    int have_strategy =
        0;

    while (
        fgets(
            line,
            sizeof(line),
            file
        )
    ) {
        char *fields[
            MAX_FIELDS
        ];

        size_t count =
            split_csv(
                line,
                fields,
                MAX_FIELDS
            );

        if (
            count <
            14
        ) {
            continue;
        }

        MemoryRow row;

        memset(
            &row,
            0,
            sizeof(row)
        );

        snprintf(
            row.mode,
            sizeof(row.mode),
            "%s",
            fields[0]
        );

        row.setup_ms =
            strtod(
                fields[1],
                NULL
            );

        row.timed_ms =
            strtod(
                fields[2],
                NULL
            );

        row.total_ms =
            strtod(
                fields[3],
                NULL
            );

        row.throughput_mbps =
            strtod(
                fields[4],
                NULL
            );

        row.setup_minor_faults =
            strtod(
                fields[5],
                NULL
            );

        row.timed_minor_faults =
            strtod(
                fields[6],
                NULL
            );

        row.setup_major_faults =
            strtod(
                fields[7],
                NULL
            );

        row.timed_major_faults =
            strtod(
                fields[8],
                NULL
            );

        row.timed_system_ms =
            strtod(
                fields[9],
                NULL
            );

        row.voluntary_ctx_switches =
            strtod(
                fields[10],
                NULL
            );

        row.involuntary_ctx_switches =
            strtod(
                fields[11],
                NULL
            );

        row.selected_critical =
            atoi(
                fields[12]
            );

        row.selected_strategy_total =
            atoi(
                fields[13]
            );

        if (
            strcmp(
                row.mode,
                "DEMAND"
            ) == 0
        ) {
            demand =
                row;

            have_demand =
                1;
        }

        if (
            row.selected_critical
        ) {
            critical =
                row;

            have_critical =
                1;
        }

        if (
            row.selected_strategy_total
        ) {
            strategy =
                row;

            have_strategy =
                1;
        }
    }

    fclose(
        file
    );

    if (
        !have_demand ||
        !have_critical ||
        !have_strategy
    ) {
        mark_missing(
            writer,
            "Virtual Memory / Page Faults",
            path
        );

        return -1;
    }

    begin_section(
        writer,
        "Virtual Memory / Page Faults"
    );

    emit_metric(
        writer,
        "Virtual Memory / Page Faults",
        "Critical-path winner",
        critical.mode,
        "",
        ""
    );

    emit_double(
        writer,
        "Virtual Memory / Page Faults",
        "Critical-path latency",
        critical.timed_ms,
        "ms",
        ""
    );

    emit_double(
        writer,
        "Virtual Memory / Page Faults",
        "Critical-path reduction vs demand",
        reduction_percent(
            demand.timed_ms,
            critical.timed_ms
        ),
        "%",
        ""
    );

    emit_double(
        writer,
        "Virtual Memory / Page Faults",
        "Demand timed minor faults",
        demand.timed_minor_faults,
        "faults",
        ""
    );

    emit_double(
        writer,
        "Virtual Memory / Page Faults",
        "Critical winner setup minor faults",
        critical.setup_minor_faults,
        "faults",
        ""
    );

    emit_double(
        writer,
        "Virtual Memory / Page Faults",
        "Critical winner timed minor faults",
        critical.timed_minor_faults,
        "faults",
        "fault work may have shifted into setup"
    );

    emit_metric(
        writer,
        "Virtual Memory / Page Faults",
        "Strategy-total winner",
        strategy.mode,
        "",
        "measured total of strategy setup + timed workload"
    );

    emit_double(
        writer,
        "Virtual Memory / Page Faults",
        "Strategy-total latency",
        strategy.total_ms,
        "ms",
        ""
    );

    emit_double(
        writer,
        "Virtual Memory / Page Faults",
        "Strategy-total reduction vs demand",
        reduction_percent(
            demand.total_ms,
            strategy.total_ms
        ),
        "%",
        "measured result, not proof of page pre-population"
    );

    if (
        strcmp(
            strategy.mode,
            "MADVISE-WILLNEED"
        ) == 0 &&
        strategy.timed_minor_faults >=
        demand.timed_minor_faults *
        0.95
    ) {
        emit_metric(
            writer,
            "Virtual Memory / Page Faults",
            "MADV_WILLNEED fault behavior",
            "NO MATERIAL PREFAULTING OBSERVED",
            "",
            "timed minor-fault count remained approximately equal to DEMAND"
        );
    }

    emit_double(
        writer,
        "Virtual Memory / Page Faults",
        "Demand major faults",
        demand.timed_major_faults,
        "faults",
        ""
    );

    return 0;
}


/* =========================================================
   Environment profile
   ========================================================= */

static int summarize_environment(
    SummaryWriter *writer
)
{
    const char *path =
        "results/system_environment.csv";

    FILE *file =
        fopen(
            path,
            "r"
        );

    if (!file) {
        mark_missing(
            writer,
            "Experimental Environment",
            path
        );

        return -1;
    }

    char line[
        LINE_BUFFER
    ];

    if (
        !fgets(
            line,
            sizeof(line),
            file
        ) ||
        !fgets(
            line,
            sizeof(line),
            file
        )
    ) {
        fclose(file);

        mark_missing(
            writer,
            "Experimental Environment",
            path
        );

        return -1;
    }

    fclose(
        file
    );

    char *fields[
        MAX_FIELDS
    ];

    size_t count =
        split_csv(
            line,
            fields,
            MAX_FIELDS
        );

    if (
        count <
        18
    ) {
        mark_missing(
            writer,
            "Experimental Environment",
            path
        );

        return -1;
    }

    begin_section(
        writer,
        "Experimental Environment"
    );

    emit_metric(
        writer,
        "Experimental Environment",
        "Operating system",
        fields[1],
        "",
        ""
    );

    emit_metric(
        writer,
        "Experimental Environment",
        "Kernel",
        fields[3],
        "",
        ""
    );

    emit_metric(
        writer,
        "Experimental Environment",
        "Architecture",
        fields[5],
        "",
        ""
    );

    emit_metric(
        writer,
        "Experimental Environment",
        "WSL detected",
        atoi(fields[7])
        ?
        "YES"
        :
        "NO",
        "",
        ""
    );

    emit_metric(
        writer,
        "Experimental Environment",
        "CPU",
        fields[8],
        "",
        ""
    );

    emit_metric(
        writer,
        "Experimental Environment",
        "Logical CPUs",
        fields[9],
        "",
        ""
    );

    emit_metric(
        writer,
        "Experimental Environment",
        "Page size",
        fields[10],
        "bytes",
        ""
    );

    char compiler[
        256
    ];

    snprintf(
        compiler,
        sizeof(compiler),
        "%s %s",
        fields[13],
        fields[14]
    );

    emit_metric(
        writer,
        "Experimental Environment",
        "Compiler",
        compiler,
        "",
        ""
    );

    emit_metric(
        writer,
        "Experimental Environment",
        "C standard",
        fields[15],
        "",
        ""
    );

    emit_metric(
        writer,
        "Experimental Environment",
        "Optimization",
        fields[16],
        "",
        fields[17]
    );

    return 0;
}


/* =========================================================
   Run-manifest evidence
   ========================================================= */

static int summarize_manifests(
    SummaryWriter *writer
)
{
    DIR *directory =
        opendir(
            "results"
        );

    if (!directory) {
        mark_missing(
            writer,
            "Reproducibility Evidence",
            "results/"
        );

        return -1;
    }

    size_t manifest_count =
        0;

    size_t clean_count =
        0;

    struct dirent *entry;

    while (
        (
            entry =
            readdir(
                directory
            )
        ) != NULL
    ) {
        if (
            !starts_with(
                entry->d_name,
                "run_manifest_"
            ) ||
            !ends_with(
                entry->d_name,
                ".txt"
            )
        ) {
            continue;
        }

        ++manifest_count;

        char path[
            PATH_BUFFER
        ];

        snprintf(
            path,
            sizeof(path),
            "results/%s",
            entry->d_name
        );

        FILE *file =
            fopen(
                path,
                "r"
            );

        if (!file) {
            continue;
        }

        char line[
            LINE_BUFFER
        ];

        while (
            fgets(
                line,
                sizeof(line),
                file
            )
        ) {
            if (
                strstr(
                    line,
                    "Source-tree state"
                ) &&
                strstr(
                    line,
                    "CLEAN"
                )
            ) {
                ++clean_count;

                break;
            }
        }

        fclose(
            file
        );
    }

    closedir(
        directory
    );

    begin_section(
        writer,
        "Reproducibility Evidence"
    );

    emit_integer(
        writer,
        "Reproducibility Evidence",
        "TXT manifests available",
        (long long)manifest_count,
        "manifests",
        ""
    );

    emit_integer(
        writer,
        "Reproducibility Evidence",
        "CLEAN manifests available",
        (long long)clean_count,
        "manifests",
        ""
    );

    emit_metric(
        writer,
        "Reproducibility Evidence",
        "Manifest policy",
        "ENABLED",
        "",
        "important experiments record command, commit, branch, source state and result files"
    );

    emit_metric(
        writer,
        "Reproducibility Evidence",
        "Manifest count timing",
        "AT SUMMARY GENERATION",
        "",
        "this final-summary command's own manifest is recorded after the summary is generated"
    );

    return 0;
}


/* =========================================================
   Scientific interpretation notes
   ========================================================= */

static void write_scientific_notes(
    SummaryWriter *writer
)
{
    begin_section(
        writer,
        "Scientific Interpretation Notes"
    );

    emit_metric(
        writer,
        "Scientific Interpretation Notes",
        "Baseline vs optimized results",
        "SEPARATE EXPERIMENTS",
        "",
        "do not mix conditions when claiming speedups"
    );

    emit_metric(
        writer,
        "Scientific Interpretation Notes",
        "strace timing",
        "NOT PRIMARY PERFORMANCE EVIDENCE",
        "",
        "system-call tracing changes wall-clock behavior"
    );

    emit_metric(
        writer,
        "Scientific Interpretation Notes",
        "Ring-slot sweep",
        "CONTROLLED MICROBENCHMARK",
        "",
        "candidate selection is not automatic production adoption"
    );

    emit_metric(
        writer,
        "Scientific Interpretation Notes",
        "CPU affinity",
        "EXPERIMENTAL",
        "",
        "scheduler placement may change across hardware, kernels and workloads"
    );

    emit_metric(
        writer,
        "Scientific Interpretation Notes",
        "Pre-faulting",
        "WORK SHIFT",
        "",
        "page-fault work can move from the timed critical path into setup"
    );

    emit_metric(
        writer,
        "Scientific Interpretation Notes",
        "MADV_WILLNEED",
        "KERNEL HINT",
        "",
        "a successful call does not guarantee page pre-population"
    );
}


/* =========================================================
   Main final-summary generator
   ========================================================= */

int run_final_summary(void)
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

    const char *txt_path =
        "results/final_summary.txt";

    const char *csv_path =
        "results/final_summary.csv";


    FILE *txt =
        fopen(
            txt_path,
            "w"
        );

    if (!txt) {
        perror(
            "fopen final_summary.txt"
        );

        return -1;
    }


    FILE *csv =
        fopen(
            csv_path,
            "w"
        );

    if (!csv) {
        perror(
            "fopen final_summary.csv"
        );

        fclose(
            txt
        );

        return -1;
    }


    SummaryWriter writer;

    writer.txt =
        txt;

    writer.csv =
        csv;

    writer.missing_sections =
        0;


    fprintf(
        csv,
        "\"section\",\"metric\",\"value\",\"unit\",\"note\"\n"
    );


    time_t now =
        time(
            NULL
        );

    struct tm local_tm;

    char timestamp[
        128
    ];

    if (
        localtime_r(
            &now,
            &local_tm
        ) != NULL
    ) {
        strftime(
            timestamp,
            sizeof(timestamp),
            "%Y-%m-%d %H:%M:%S %z",
            &local_tm
        );
    }
    else {
        snprintf(
            timestamp,
            sizeof(timestamp),
            "unknown"
        );
    }


    fprintf(
        txt,
        "FASTIPC-X FINAL EXPERIMENT SUMMARY\n"
        "============================================================\n\n"
    );

    fprintf(
        txt,
        "Generated              : %s\n",
        timestamp
    );

    fprintf(
        txt,
        "Source                 : existing FastIPC-X result CSVs\n"
    );

    fprintf(
        txt,
        "Primary policy         : derive results; do not hardcode metrics\n"
    );


    summarize_baseline(
        &writer
    );

    summarize_chunk_optimization(
        &writer
    );

    summarize_shm_sync(
        &writer
    );

    summarize_syscalls(
        &writer
    );

    summarize_workloads(
        &writer
    );

    summarize_integrity(
        &writer
    );

    summarize_ring_slots(
        &writer
    );

    summarize_affinity(
        &writer
    );

    summarize_memory(
        &writer
    );

    summarize_environment(
        &writer
    );

    summarize_manifests(
        &writer
    );

    write_scientific_notes(
        &writer
    );


    begin_section(
        &writer,
        "Summary Generation Status"
    );

    if (
        writer.missing_sections == 0
    ) {
        emit_metric(
            &writer,
            "Summary Generation Status",
            "Status",
            "COMPLETE",
            "",
            "all expected result sections were found"
        );
    }
    else {
        char missing[
            64
        ];

        snprintf(
            missing,
            sizeof(missing),
            "%d",
            writer.missing_sections
        );

        emit_metric(
            &writer,
            "Summary Generation Status",
            "Status",
            "PARTIAL",
            "",
            "one or more expected result files were unavailable"
        );

        emit_metric(
            &writer,
            "Summary Generation Status",
            "Missing sections",
            missing,
            "",
            ""
        );
    }


    fclose(
        csv
    );

    fclose(
        txt
    );


    printf(
        "\n"
        "FASTIPC-X FINAL SUMMARY GENERATED\n"
        "============================================================\n"
    );

    printf(
        "TXT summary            : %s\n",
        txt_path
    );

    printf(
        "CSV summary            : %s\n",
        csv_path
    );

    printf(
        "Missing sections       : %d\n",
        writer.missing_sections
    );

    printf(
        "Status                 : %s\n",
        writer.missing_sections == 0
        ?
        "COMPLETE"
        :
        "PARTIAL"
    );


    return 0;
}
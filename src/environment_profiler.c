#include "../include/environment_profiler.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sys/stat.h>
#include <sys/sysinfo.h>
#include <sys/utsname.h>

#include <time.h>
#include <unistd.h>


#ifndef FASTIPCX_BUILD_FLAGS
#define FASTIPCX_BUILD_FLAGS "not recorded"
#endif


#define TEXT_BUFFER_SIZE 512


/* =========================================================
   Results directory
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
   String helpers
   ========================================================= */

static void trim_newline(char *text)
{
    size_t length;

    if (!text) {
        return;
    }

    length = strlen(text);

    while (
        length > 0 &&
        (
            text[length - 1] == '\n' ||
            text[length - 1] == '\r'
        )
    ) {
        text[length - 1] = '\0';
        --length;
    }
}


static void trim_spaces(char *text)
{
    char *start;
    size_t length;

    if (!text) {
        return;
    }

    start = text;

    while (
        *start != '\0' &&
        isspace(
            (unsigned char)*start
        )
    ) {
        ++start;
    }

    if (start != text) {
        memmove(
            text,
            start,
            strlen(start) + 1
        );
    }

    length = strlen(text);

    while (
        length > 0 &&
        isspace(
            (unsigned char)text[length - 1]
        )
    ) {
        text[length - 1] = '\0';
        --length;
    }
}


static void remove_surrounding_quotes(char *text)
{
    size_t length;

    if (!text) {
        return;
    }

    length = strlen(text);

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

        text[length - 2] = '\0';
    }
}


/* =========================================================
   Case-insensitive substring helper
   ========================================================= */

static int contains_case_insensitive(
    const char *text,
    const char *needle
)
{
    size_t needle_length;

    if (
        !text ||
        !needle
    ) {
        return 0;
    }

    needle_length =
        strlen(needle);

    if (needle_length == 0) {
        return 1;
    }

    for (
        size_t i = 0;
        text[i] != '\0';
        ++i
    ) {
        size_t j = 0;

        while (
            text[i + j] != '\0' &&
            needle[j] != '\0' &&
            tolower(
                (unsigned char)text[i + j]
            ) ==
            tolower(
                (unsigned char)needle[j]
            )
        ) {
            ++j;
        }

        if (j == needle_length) {
            return 1;
        }
    }

    return 0;
}


/* =========================================================
   Generic /proc reader
   ========================================================= */

static int read_value_after_prefix(
    const char *path,
    const char *prefix,
    char *output,
    size_t output_size
)
{
    FILE *file;
    char line[1024];

    if (
        !path ||
        !prefix ||
        !output ||
        output_size == 0
    ) {
        return -1;
    }

    file =
        fopen(
            path,
            "r"
        );

    if (!file) {
        return -1;
    }

    while (
        fgets(
            line,
            sizeof(line),
            file
        ) != NULL
    ) {
        size_t prefix_length =
            strlen(prefix);

        if (
            strncmp(
                line,
                prefix,
                prefix_length
            ) == 0
        ) {
            char *value =
                line +
                prefix_length;

            trim_newline(value);
            trim_spaces(value);

            snprintf(
                output,
                output_size,
                "%s",
                value
            );

            fclose(file);

            return 0;
        }
    }

    fclose(file);

    return -1;
}


/* =========================================================
   CPU information
   ========================================================= */

static void get_cpu_model(
    char *output,
    size_t output_size
)
{
    if (
        read_value_after_prefix(
            "/proc/cpuinfo",
            "model name\t:",
            output,
            output_size
        ) == 0
    ) {
        return;
    }

    if (
        read_value_after_prefix(
            "/proc/cpuinfo",
            "model name :",
            output,
            output_size
        ) == 0
    ) {
        return;
    }

    if (
        read_value_after_prefix(
            "/proc/cpuinfo",
            "Hardware\t:",
            output,
            output_size
        ) == 0
    ) {
        return;
    }

    snprintf(
        output,
        output_size,
        "Unknown"
    );
}


/* =========================================================
   Operating-system information
   ========================================================= */

static void get_os_name(
    char *output,
    size_t output_size
)
{
    FILE *file;
    char line[1024];

    file =
        fopen(
            "/etc/os-release",
            "r"
        );

    if (!file) {
        snprintf(
            output,
            output_size,
            "Unknown Linux distribution"
        );

        return;
    }

    while (
        fgets(
            line,
            sizeof(line),
            file
        ) != NULL
    ) {
        if (
            strncmp(
                line,
                "PRETTY_NAME=",
                12
            ) == 0
        ) {
            char *value =
                line + 12;

            trim_newline(value);
            trim_spaces(value);
            remove_surrounding_quotes(value);

            snprintf(
                output,
                output_size,
                "%s",
                value
            );

            fclose(file);

            return;
        }
    }

    fclose(file);

    snprintf(
        output,
        output_size,
        "Unknown Linux distribution"
    );
}


/* =========================================================
   WSL detection
   ========================================================= */

static int detect_wsl(
    const struct utsname *system_info
)
{
    FILE *file;
    char buffer[1024];

    if (system_info) {

        if (
            contains_case_insensitive(
                system_info->release,
                "microsoft"
            ) ||
            contains_case_insensitive(
                system_info->version,
                "microsoft"
            ) ||
            contains_case_insensitive(
                system_info->release,
                "wsl"
            )
        ) {
            return 1;
        }
    }

    file =
        fopen(
            "/proc/version",
            "r"
        );

    if (!file) {
        return 0;
    }

    if (
        fgets(
            buffer,
            sizeof(buffer),
            file
        ) != NULL
    ) {
        fclose(file);

        if (
            contains_case_insensitive(
                buffer,
                "microsoft"
            ) ||
            contains_case_insensitive(
                buffer,
                "wsl"
            )
        ) {
            return 1;
        }

        return 0;
    }

    fclose(file);

    return 0;
}


/* =========================================================
   Memory information
   ========================================================= */

static unsigned long long get_available_memory_bytes(void)
{
    FILE *file;
    char line[1024];

    file =
        fopen(
            "/proc/meminfo",
            "r"
        );

    if (!file) {
        return 0;
    }

    while (
        fgets(
            line,
            sizeof(line),
            file
        ) != NULL
    ) {
        unsigned long long value_kb = 0;

        if (
            sscanf(
                line,
                "MemAvailable: %llu kB",
                &value_kb
            ) == 1
        ) {
            fclose(file);

            return
                value_kb *
                1024ULL;
        }
    }

    fclose(file);

    return 0;
}


/* =========================================================
   Compiler / C standard
   ========================================================= */

static const char *compiler_name(void)
{
#if defined(__clang__)
    return "Clang";
#elif defined(__GNUC__)
    return "GCC";
#else
    return "Unknown compiler";
#endif
}


static const char *compiler_version(void)
{
#ifdef __VERSION__
    return __VERSION__;
#else
    return "Unknown";
#endif
}


static const char *c_standard_name(void)
{
#if defined(__STDC_VERSION__)
    #if __STDC_VERSION__ >= 202311L
        return "C23 or newer";
    #elif __STDC_VERSION__ >= 201710L
        return "C17";
    #elif __STDC_VERSION__ >= 201112L
        return "C11";
    #elif __STDC_VERSION__ >= 199901L
        return "C99";
    #else
        return "Pre-C99";
    #endif
#else
    return "Unknown";
#endif
}


static const char *optimization_status(void)
{
#ifdef __OPTIMIZE__
    return "Enabled";
#else
    return "Disabled";
#endif
}


/* =========================================================
   CSV escaping
   ========================================================= */

static void csv_write_field(
    FILE *file,
    const char *text
)
{
    if (
        !file ||
        !text
    ) {
        return;
    }

    fputc(
        '"',
        file
    );

    for (
        size_t i = 0;
        text[i] != '\0';
        ++i
    ) {
        if (text[i] == '"') {
            fputc(
                '"',
                file
            );
        }

        fputc(
            text[i],
            file
        );
    }

    fputc(
        '"',
        file
    );
}


/* =========================================================
   Save text report
   ========================================================= */

static int save_text_report(
    const char *timestamp,
    const char *os_name,
    const struct utsname *system_info,
    int is_wsl,
    const char *cpu_model,
    long logical_cpus,
    long page_size,
    unsigned long long total_memory,
    unsigned long long available_memory
)
{
    const char *path =
        "results/system_environment.txt";

    FILE *file =
        fopen(
            path,
            "w"
        );

    if (!file) {
        perror("fopen system_environment.txt");
        return -1;
    }

    fprintf(
        file,
        "FASTIPC-X SYSTEM / ENVIRONMENT PROFILE\n"
    );

    fprintf(
        file,
        "============================================================\n\n"
    );

    fprintf(
        file,
        "Timestamp              : %s\n",
        timestamp
    );

    fprintf(
        file,
        "Operating system       : %s\n",
        os_name
    );

    fprintf(
        file,
        "Kernel name            : %s\n",
        system_info->sysname
    );

    fprintf(
        file,
        "Kernel release         : %s\n",
        system_info->release
    );

    fprintf(
        file,
        "Kernel version         : %s\n",
        system_info->version
    );

    fprintf(
        file,
        "Architecture           : %s\n",
        system_info->machine
    );

    fprintf(
        file,
        "Hostname               : %s\n",
        system_info->nodename
    );

    fprintf(
        file,
        "WSL detected           : %s\n",
        is_wsl ? "YES" : "NO"
    );

    fprintf(
        file,
        "\nCPU\n"
        "------------------------------------------------------------\n"
    );

    fprintf(
        file,
        "CPU model              : %s\n",
        cpu_model
    );

    fprintf(
        file,
        "Logical CPUs           : %ld\n",
        logical_cpus
    );

    fprintf(
        file,
        "\nMemory\n"
        "------------------------------------------------------------\n"
    );

    fprintf(
        file,
        "Page size              : %ld bytes\n",
        page_size
    );

    fprintf(
        file,
        "Total memory           : %.2f GiB\n",
        (double)total_memory /
        (
            1024.0 *
            1024.0 *
            1024.0
        )
    );

    fprintf(
        file,
        "Available memory       : %.2f GiB\n",
        (double)available_memory /
        (
            1024.0 *
            1024.0 *
            1024.0
        )
    );

    fprintf(
        file,
        "\nBuild\n"
        "------------------------------------------------------------\n"
    );

    fprintf(
        file,
        "Compiler               : %s\n",
        compiler_name()
    );

    fprintf(
        file,
        "Compiler version       : %s\n",
        compiler_version()
    );

    fprintf(
        file,
        "C standard             : %s\n",
        c_standard_name()
    );

    fprintf(
        file,
        "Compiler optimization  : %s\n",
        optimization_status()
    );

    fprintf(
        file,
        "Build flags            : %s\n",
        FASTIPCX_BUILD_FLAGS
    );

    fclose(file);

    printf(
        "Text report           : %s\n",
        path
    );

    return 0;
}


/* =========================================================
   Save CSV report
   ========================================================= */

static int save_csv_report(
    const char *timestamp,
    const char *os_name,
    const struct utsname *system_info,
    int is_wsl,
    const char *cpu_model,
    long logical_cpus,
    long page_size,
    unsigned long long total_memory,
    unsigned long long available_memory
)
{
    const char *path =
        "results/system_environment.csv";

    FILE *file =
        fopen(
            path,
            "w"
        );

    if (!file) {
        perror("fopen system_environment.csv");
        return -1;
    }

    fprintf(
        file,
        "timestamp,"
        "operating_system,"
        "kernel_name,"
        "kernel_release,"
        "kernel_version,"
        "architecture,"
        "hostname,"
        "wsl_detected,"
        "cpu_model,"
        "logical_cpus,"
        "page_size_bytes,"
        "total_memory_bytes,"
        "available_memory_bytes,"
        "compiler,"
        "compiler_version,"
        "c_standard,"
        "optimization,"
        "build_flags\n"
    );

    csv_write_field(
        file,
        timestamp
    );

    fputc(',', file);

    csv_write_field(
        file,
        os_name
    );

    fputc(',', file);

    csv_write_field(
        file,
        system_info->sysname
    );

    fputc(',', file);

    csv_write_field(
        file,
        system_info->release
    );

    fputc(',', file);

    csv_write_field(
        file,
        system_info->version
    );

    fputc(',', file);

    csv_write_field(
        file,
        system_info->machine
    );

    fputc(',', file);

    csv_write_field(
        file,
        system_info->nodename
    );

    fprintf(
        file,
        ",%d,",
        is_wsl ? 1 : 0
    );

    csv_write_field(
        file,
        cpu_model
    );

    fprintf(
        file,
        ",%ld,%ld,%llu,%llu,",
        logical_cpus,
        page_size,
        total_memory,
        available_memory
    );

    csv_write_field(
        file,
        compiler_name()
    );

    fputc(',', file);

    csv_write_field(
        file,
        compiler_version()
    );

    fputc(',', file);

    csv_write_field(
        file,
        c_standard_name()
    );

    fputc(',', file);

    csv_write_field(
        file,
        optimization_status()
    );

    fputc(',', file);

    csv_write_field(
        file,
        FASTIPCX_BUILD_FLAGS
    );

    fputc(
        '\n',
        file
    );

    fclose(file);

    printf(
        "CSV report            : %s\n",
        path
    );

    return 0;
}


/* =========================================================
   Public environment profiler
   ========================================================= */

int run_environment_profiler(void)
{
    struct utsname system_info;
    struct sysinfo memory_info;

    char cpu_model[
        TEXT_BUFFER_SIZE
    ];

    char os_name[
        TEXT_BUFFER_SIZE
    ];

    char timestamp[
        128
    ];

    time_t now;
    struct tm local_time;

    long logical_cpus;
    long page_size;

    unsigned long long total_memory;
    unsigned long long available_memory;

    int is_wsl;


    if (
        ensure_results_directory() != 0
    ) {
        return -1;
    }


    if (
        uname(
            &system_info
        ) != 0
    ) {
        perror("uname");
        return -1;
    }


    if (
        sysinfo(
            &memory_info
        ) != 0
    ) {
        perror("sysinfo");
        return -1;
    }


    get_cpu_model(
        cpu_model,
        sizeof(cpu_model)
    );


    get_os_name(
        os_name,
        sizeof(os_name)
    );


    is_wsl =
        detect_wsl(
            &system_info
        );


    logical_cpus =
        sysconf(
            _SC_NPROCESSORS_ONLN
        );


    page_size =
        sysconf(
            _SC_PAGESIZE
        );


    total_memory =
        (unsigned long long)
        memory_info.totalram *
        (unsigned long long)
        memory_info.mem_unit;


    available_memory =
        get_available_memory_bytes();


    now =
        time(NULL);


    if (
        localtime_r(
            &now,
            &local_time
        ) == NULL
    ) {
        snprintf(
            timestamp,
            sizeof(timestamp),
            "Unknown"
        );
    }
    else {
        strftime(
            timestamp,
            sizeof(timestamp),
            "%Y-%m-%d %H:%M:%S %z",
            &local_time
        );
    }


    printf(
        "\n"
        "FASTIPC-X SYSTEM / ENVIRONMENT PROFILE\n"
        "============================================================\n"
    );


    printf(
        "Timestamp             : %s\n",
        timestamp
    );


    printf(
        "Operating system      : %s\n",
        os_name
    );


    printf(
        "Kernel                : %s %s\n",
        system_info.sysname,
        system_info.release
    );


    printf(
        "Architecture          : %s\n",
        system_info.machine
    );


    printf(
        "Hostname              : %s\n",
        system_info.nodename
    );


    printf(
        "WSL detected          : %s\n",
        is_wsl ? "YES" : "NO"
    );


    printf(
        "\nCPU\n"
        "------------------------------------------------------------\n"
    );


    printf(
        "CPU model             : %s\n",
        cpu_model
    );


    printf(
        "Logical CPUs          : %ld\n",
        logical_cpus
    );


    printf(
        "\nMemory\n"
        "------------------------------------------------------------\n"
    );


    printf(
        "Page size             : %ld bytes\n",
        page_size
    );


    printf(
        "Total memory          : %.2f GiB\n",
        (double)total_memory /
        (
            1024.0 *
            1024.0 *
            1024.0
        )
    );


    printf(
        "Available memory      : %.2f GiB\n",
        (double)available_memory /
        (
            1024.0 *
            1024.0 *
            1024.0
        )
    );


    printf(
        "\nBuild\n"
        "------------------------------------------------------------\n"
    );


    printf(
        "Compiler              : %s\n",
        compiler_name()
    );


    printf(
        "Compiler version      : %s\n",
        compiler_version()
    );


    printf(
        "C standard            : %s\n",
        c_standard_name()
    );


    printf(
        "Optimization enabled  : %s\n",
        optimization_status()
    );


    printf(
        "Build flags           : %s\n\n",
        FASTIPCX_BUILD_FLAGS
    );


    if (
        save_text_report(
            timestamp,
            os_name,
            &system_info,
            is_wsl,
            cpu_model,
            logical_cpus,
            page_size,
            total_memory,
            available_memory
        ) != 0
    ) {
        return -1;
    }


    if (
        save_csv_report(
            timestamp,
            os_name,
            &system_info,
            is_wsl,
            cpu_model,
            logical_cpus,
            page_size,
            total_memory,
            available_memory
        ) != 0
    ) {
        return -1;
    }


    printf(
        "\nEnvironment profiling complete.\n"
    );


    return 0;
}
#include "../include/run_manifest.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sys/stat.h>
#include <time.h>
#include <unistd.h>


#define RUN_ID_SIZE 128
#define TEXT_SIZE 1024
#define COMMAND_SIZE 2048
#define PATH_SIZE 512


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

    length =
        strlen(text);

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


static int starts_with(
    const char *text,
    const char *prefix
)
{
    size_t prefix_length;

    if (
        !text ||
        !prefix
    ) {
        return 0;
    }

    prefix_length =
        strlen(prefix);

    return
        strncmp(
            text,
            prefix,
            prefix_length
        ) == 0;
}


/* =========================================================
   Run command and capture first output line
   ========================================================= */

static int capture_command_output(
    const char *command,
    char *output,
    size_t output_size
)
{
    FILE *pipe;

    if (
        !command ||
        !output ||
        output_size == 0
    ) {
        return -1;
    }

    output[0] = '\0';

    pipe =
        popen(
            command,
            "r"
        );

    if (!pipe) {
        return -1;
    }

    if (
        fgets(
            output,
            output_size,
            pipe
        ) == NULL
    ) {
        pclose(pipe);
        return -1;
    }

    trim_newline(output);

    if (
        pclose(pipe) == -1
    ) {
        return -1;
    }

    return 0;
}


/* =========================================================
   Git commit
   ========================================================= */

static void get_git_commit(
    char *output,
    size_t output_size
)
{
    if (
        capture_command_output(
            "git rev-parse HEAD 2>/dev/null",
            output,
            output_size
        ) != 0
    ) {
        snprintf(
            output,
            output_size,
            "Unknown"
        );
    }
}


/* =========================================================
   Git branch
   ========================================================= */

static void get_git_branch(
    char *output,
    size_t output_size
)
{
    if (
        capture_command_output(
            "git rev-parse --abbrev-ref HEAD 2>/dev/null",
            output,
            output_size
        ) != 0
    ) {
        snprintf(
            output,
            output_size,
            "Unknown"
        );
    }
}


/* =========================================================
   Git source-code state

   Benchmark output files under results/ are ignored when
   determining whether the source tree is clean.

   This prevents a newly generated CSV from incorrectly
   making an otherwise clean source revision appear dirty.
   ========================================================= */

static void get_git_code_state(
    char *output,
    size_t output_size
)
{
    FILE *pipe;

    char line[
        TEXT_SIZE
    ];

    int source_dirty =
        0;


    pipe =
        popen(
            "git status --porcelain "
            "--untracked-files=normal 2>/dev/null",
            "r"
        );


    if (!pipe) {

        snprintf(
            output,
            output_size,
            "UNKNOWN"
        );

        return;
    }


    while (
        fgets(
            line,
            sizeof(line),
            pipe
        ) != NULL
    ) {
        char *path;

        trim_newline(line);


        if (
            strlen(line) < 4
        ) {
            continue;
        }


        /*
         * Porcelain format normally begins:
         *
         * XY path
         *
         * so the path starts after the first
         * three characters.
         */
        path =
            line + 3;


        /*
         * For renamed files:
         *
         * old -> new
         *
         * use the destination path.
         */
        {
            char *arrow =
                strstr(
                    path,
                    " -> "
                );

            if (arrow) {
                path =
                    arrow + 4;
            }
        }


        /*
         * Result files are experimental output,
         * not source-code modifications.
         */
        if (
            starts_with(
                path,
                "results/"
            )
        ) {
            continue;
        }


        source_dirty =
            1;

        break;
    }


    pclose(pipe);


    snprintf(
        output,
        output_size,
        "%s",
        source_dirty
        ?
        "DIRTY"
        :
        "CLEAN"
    );
}


/* =========================================================
   Build command string from argv
   ========================================================= */

static void build_command_string(
    int argc,
    char *const argv[],
    char *output,
    size_t output_size
)
{
    size_t used =
        0;


    if (
        !output ||
        output_size == 0
    ) {
        return;
    }


    output[0] =
        '\0';


    for (
        int i = 0;
        i < argc;
        ++i
    ) {
        const char *argument =
            argv[i]
            ?
            argv[i]
            :
            "";


        int needs_quotes =
            strchr(
                argument,
                ' '
            ) != NULL;


        int written;


        if (i > 0) {

            written =
                snprintf(
                    output + used,
                    output_size - used,
                    " "
                );


            if (
                written < 0 ||
                (size_t)written >=
                output_size - used
            ) {
                output[
                    output_size - 1
                ] = '\0';

                return;
            }


            used +=
                (size_t)written;
        }


        if (needs_quotes) {

            written =
                snprintf(
                    output + used,
                    output_size - used,
                    "\"%s\"",
                    argument
                );
        }
        else {

            written =
                snprintf(
                    output + used,
                    output_size - used,
                    "%s",
                    argument
                );
        }


        if (
            written < 0 ||
            (size_t)written >=
            output_size - used
        ) {
            output[
                output_size - 1
            ] = '\0';

            return;
        }


        used +=
            (size_t)written;
    }
}


/* =========================================================
   Timestamp / run ID
   ========================================================= */

static void build_timestamp_and_run_id(
    char *timestamp,
    size_t timestamp_size,
    char *run_id,
    size_t run_id_size
)
{
    time_t now;

    struct tm local_time;

    char compact_time[
        64
    ];


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
            timestamp_size,
            "Unknown"
        );


        snprintf(
            compact_time,
            sizeof(compact_time),
            "unknown"
        );
    }
    else {

        strftime(
            timestamp,
            timestamp_size,
            "%Y-%m-%d %H:%M:%S %z",
            &local_time
        );


        strftime(
            compact_time,
            sizeof(compact_time),
            "%Y%m%d_%H%M%S",
            &local_time
        );
    }


    snprintf(
        run_id,
        run_id_size,
        "%s_%ld",
        compact_time,
        (long)getpid()
    );
}


/* =========================================================
   Environment-profile reference
   ========================================================= */

static void get_environment_reference(
    char *output,
    size_t output_size
)
{
    if (
        access(
            "results/system_environment.txt",
            R_OK
        ) == 0
    ) {

        snprintf(
            output,
            output_size,
            "results/system_environment.txt"
        );
    }
    else {

        snprintf(
            output,
            output_size,
            "NOT FOUND"
        );
    }
}


/* =========================================================
   CSV field escaping
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

        if (
            text[i] == '"'
        ) {

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
   Write text manifest
   ========================================================= */

static int write_text_manifest(
    const char *path,
    const char *run_id,
    const char *timestamp,
    const char *category,
    const char *command,
    const char *git_commit,
    const char *git_branch,
    const char *git_state,
    const char *environment_reference,
    const char *result_files,
    int command_exit_code
)
{
    FILE *file =
        fopen(
            path,
            "w"
        );


    if (!file) {

        perror(
            "fopen run manifest txt"
        );

        return -1;
    }


    fprintf(
        file,
        "FASTIPC-X EXPERIMENT RUN MANIFEST\n"
    );


    fprintf(
        file,
        "============================================================\n\n"
    );


    fprintf(
        file,
        "Run ID                 : %s\n",
        run_id
    );


    fprintf(
        file,
        "Timestamp              : %s\n",
        timestamp
    );


    fprintf(
        file,
        "Category               : %s\n",
        category
    );


    fprintf(
        file,
        "Command                : %s\n",
        command
    );


    fprintf(
        file,
        "Command exit code      : %d\n",
        command_exit_code
    );


    fprintf(
        file,
        "Command status         : %s\n",
        command_exit_code == 0
        ?
        "SUCCESS"
        :
        "FAILED"
    );


    fprintf(
        file,
        "\nGit / Source Revision\n"
        "------------------------------------------------------------\n"
    );


    fprintf(
        file,
        "Git commit             : %s\n",
        git_commit
    );


    fprintf(
        file,
        "Git branch             : %s\n",
        git_branch
    );


    fprintf(
        file,
        "Source-tree state      : %s\n",
        git_state
    );


    fprintf(
        file,
        "\nEnvironment\n"
        "------------------------------------------------------------\n"
    );


    fprintf(
        file,
        "Environment profile    : %s\n",
        environment_reference
    );


    fprintf(
        file,
        "\nExperimental Output\n"
        "------------------------------------------------------------\n"
    );


    fprintf(
        file,
        "Result files           : %s\n",
        result_files
    );


    fclose(file);


    return 0;
}


/* =========================================================
   Write CSV manifest
   ========================================================= */

static int write_csv_manifest(
    const char *path,
    const char *run_id,
    const char *timestamp,
    const char *category,
    const char *command,
    const char *git_commit,
    const char *git_branch,
    const char *git_state,
    const char *environment_reference,
    const char *result_files,
    int command_exit_code
)
{
    FILE *file =
        fopen(
            path,
            "w"
        );


    if (!file) {

        perror(
            "fopen run manifest csv"
        );

        return -1;
    }


    fprintf(
        file,
        "run_id,"
        "timestamp,"
        "category,"
        "command,"
        "command_exit_code,"
        "command_status,"
        "git_commit,"
        "git_branch,"
        "source_tree_state,"
        "environment_profile,"
        "result_files\n"
    );


    csv_write_field(
        file,
        run_id
    );

    fputc(',', file);


    csv_write_field(
        file,
        timestamp
    );

    fputc(',', file);


    csv_write_field(
        file,
        category
    );

    fputc(',', file);


    csv_write_field(
        file,
        command
    );


    fprintf(
        file,
        ",%d,",
        command_exit_code
    );


    csv_write_field(
        file,
        command_exit_code == 0
        ?
        "SUCCESS"
        :
        "FAILED"
    );

    fputc(',', file);


    csv_write_field(
        file,
        git_commit
    );

    fputc(',', file);


    csv_write_field(
        file,
        git_branch
    );

    fputc(',', file);


    csv_write_field(
        file,
        git_state
    );

    fputc(',', file);


    csv_write_field(
        file,
        environment_reference
    );

    fputc(',', file);


    csv_write_field(
        file,
        result_files
    );


    fputc(
        '\n',
        file
    );


    fclose(file);


    return 0;
}


/* =========================================================
   Public run-manifest writer
   ========================================================= */

int write_run_manifest(
    int argc,
    char *const argv[],
    const char *category,
    const char *result_files,
    int command_exit_code
)
{
    char timestamp[
        128
    ];

    char run_id[
        RUN_ID_SIZE
    ];

    char command[
        COMMAND_SIZE
    ];

    char git_commit[
        TEXT_SIZE
    ];

    char git_branch[
        TEXT_SIZE
    ];

    char git_state[
        64
    ];

    char environment_reference[
        PATH_SIZE
    ];

    char text_path[
        PATH_SIZE
    ];

    char csv_path[
        PATH_SIZE
    ];


    if (
        !category ||
        !result_files
    ) {

        fprintf(
            stderr,
            "Invalid run manifest arguments.\n"
        );

        return -1;
    }


    if (
        ensure_results_directory() != 0
    ) {
        return -1;
    }


    build_timestamp_and_run_id(
        timestamp,
        sizeof(timestamp),
        run_id,
        sizeof(run_id)
    );


    build_command_string(
        argc,
        argv,
        command,
        sizeof(command)
    );


    get_git_commit(
        git_commit,
        sizeof(git_commit)
    );


    get_git_branch(
        git_branch,
        sizeof(git_branch)
    );


    get_git_code_state(
        git_state,
        sizeof(git_state)
    );


    get_environment_reference(
        environment_reference,
        sizeof(environment_reference)
    );


    snprintf(
        text_path,
        sizeof(text_path),
        "results/"
        "run_manifest_%s.txt",
        run_id
    );


    snprintf(
        csv_path,
        sizeof(csv_path),
        "results/"
        "run_manifest_%s.csv",
        run_id
    );


    if (
        write_text_manifest(
            text_path,
            run_id,
            timestamp,
            category,
            command,
            git_commit,
            git_branch,
            git_state,
            environment_reference,
            result_files,
            command_exit_code
        ) != 0
    ) {
        return -1;
    }


    if (
        write_csv_manifest(
            csv_path,
            run_id,
            timestamp,
            category,
            command,
            git_commit,
            git_branch,
            git_state,
            environment_reference,
            result_files,
            command_exit_code
        ) != 0
    ) {
        return -1;
    }


    printf(
        "\n"
        "Run manifest recorded.\n"
    );


    printf(
        "Run ID                : %s\n",
        run_id
    );


    printf(
        "Git commit            : %s\n",
        git_commit
    );


    printf(
        "Git branch            : %s\n",
        git_branch
    );


    printf(
        "Source-tree state     : %s\n",
        git_state
    );


    printf(
        "Manifest TXT          : %s\n",
        text_path
    );


    printf(
        "Manifest CSV          : %s\n",
        csv_path
    );


    return 0;
}
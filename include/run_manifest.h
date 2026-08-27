#ifndef RUN_MANIFEST_H
#define RUN_MANIFEST_H

int write_run_manifest(
    int argc,
    char *const argv[],
    const char *category,
    const char *result_files,
    int command_exit_code
);

#endif
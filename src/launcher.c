#define _GNU_SOURCE
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv) {
    if (argc == 2 && (!strcmp(argv[1], "--help") || !strcmp(argv[1], "-h"))) {
        printf("Usage: %s [--config <file.toml>] --directory <new-directory>\n"
               "       %s --list-metrics [cpu|gpu [backend-directory]]\n"
               "Generate and compile a configured executable; run it separately.\n", argv[0], argv[0]);
        return 0;
    }
    int listing = argc >= 2 && !strcmp(argv[1], "--list-metrics");
    int default_config = argc == 3 && !strcmp(argv[1], "--directory");
    if (!listing && !default_config && (argc != 5 || strcmp(argv[1], "--config") || strcmp(argv[3], "--directory"))) {
        fprintf(stderr, "[ERROR] A TOML configuration and new output directory are required.\n"
                        "Usage: %s [--config <file.toml>] --directory <new-directory>\n", argv[0]);
        return 1;
    }
    char executable[PATH_MAX], script[PATH_MAX];
    ssize_t length = readlink("/proc/self/exe", executable, sizeof(executable) - 1);
    if (length < 0 || (size_t)length >= sizeof(executable) - 1) {
        fprintf(stderr, "[ERROR] Unable to locate configuration generator.\n");
        return 1;
    }
    executable[length] = '\0';
    char *slash = strrchr(executable, '/');
    if (!slash) {
        fprintf(stderr, "[ERROR] Unable to locate configuration generator.\n");
        return 1;
    }
    *slash = '\0';
    int written = snprintf(script, sizeof(script), listing ? "%s/list-metrics" : "%s/../scripts/generate_config.py", executable);
    if (written < 0 || (size_t)written >= sizeof(script)) {
        fprintf(stderr, "[ERROR] Configuration generator path is too long.\n");
        return 1;
    }
    if (listing) {
        argv[1] = script;
        execv(script, &argv[1]);
        perror("[ERROR] Unable to start backend metric listing");
        return 1;
    }
    char configuration[PATH_MAX];
    written = snprintf(configuration, sizeof(configuration), "%s/../configuration.toml", executable);
    if (written < 0 || (size_t)written >= sizeof(configuration)) {
        fprintf(stderr, "[ERROR] Default configuration path is too long.\n");
        return 1;
    }
    char *arguments[] = {"python3", script, default_config ? configuration : argv[2],
                         "--directory", default_config ? argv[2] : argv[4], NULL};
    execvp(arguments[0], arguments);
    perror("[ERROR] Unable to start Python configuration generator");
    return 1;
}

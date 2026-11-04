#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#include "../include/energy-backend.h"

/* Runs the command once and returns the peak RSS (KiB) of the process tree
   reported by the kernel, or -1 on failure. No external tools are required. */
int platform_measure_memory(const char *command) {
    if (!command || strlen(command) == 0) return -1;

    pid_t child_pid = fork();
    if (child_pid < 0) {
        perror("[ERROR] Failed to fork for memory measurement");
        return -1;
    }
    if (child_pid == 0) {
        int null_fd = open("/dev/null", O_WRONLY);
        if (null_fd >= 0) {
            dup2(null_fd, STDOUT_FILENO);
            dup2(null_fd, STDERR_FILENO);
        }
        execl("/bin/sh", "sh", "-c", command, (char *)NULL);
        _exit(127);
    }

    int status = 0;
    struct rusage usage;
    pid_t result;
    do {
        result = wait4(child_pid, &status, 0, &usage);
    } while (result < 0 && errno == EINTR);
    if (result < 0) return -1;

    /* ru_maxrss covers waited-for descendants. It is in KiB on Linux, bytes on macOS. */
#ifdef __APPLE__
    return (int)(usage.ru_maxrss / 1024);
#else
    return (int)usage.ru_maxrss;
#endif
}

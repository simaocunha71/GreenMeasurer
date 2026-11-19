#define _GNU_SOURCE
#include "system.h"
#include "gm_aspect.h"
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#define MAX_CORES 256
#define MAX_PROCESS_TREE 1024

/* Files are opened once and re-read with pread(), which avoids an open/close pair per sample. */
static int open_once(int *slot, const char *path) {
    if (*slot == -1) {
        *slot = open(path, O_RDONLY | O_CLOEXEC);
        if (*slot < 0) *slot = -2;   /* remember the failure, do not retry every sample */
    }
    return *slot;
}

static int pread_text(int fd, char *buffer, size_t size) {
    if (fd < 0) return -1;
    ssize_t n = pread(fd, buffer, size - 1, 0);
    if (n <= 0) return -1;
    buffer[n] = '\0';
    return 0;
}

/* sysconf(_SC_NPROCESSORS_ONLN) reads /sys on every call, so the value is cached. */
static int core_count(void) {
    static int cached = -1;
    if (cached < 0) {
        long online = sysconf(_SC_NPROCESSORS_ONLN);
        cached = online > 0 ? (int)online : 0;
    }
    return cached;
}

double system_get_core_count(void) { return core_count(); }

static double core_frequency_mhz(int core) {
    static int fds[MAX_CORES];
    static int initialised;
    if (core < 0 || core >= MAX_CORES) return -1.0;
    if (!initialised) {
        for (int i = 0; i < MAX_CORES; i++) fds[i] = -1;
        initialised = 1;
    }
    char path[128], text[32];
    snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_cur_freq", core);
    if (pread_text(open_once(&fds[core], path), text, sizeof(text)) != 0) return -1.0;
    return (double)strtoul(text, NULL, 10) / 1000.0;
}

double system_get_core_frequency_mhz(void) {
    int cores = core_count();
    double sum = 0.0;
    int valid = 0;
    for (int core = 0; core < cores; core++) {
        double mhz = core_frequency_mhz(core);
        if (mhz >= 0.0) { sum += mhz; valid++; }
    }
    return valid ? sum / valid : -1.0;
}

double system_get_running_processes(void) {
    static int fd = -1;
    char text[128];
    if (pread_text(open_once(&fd, "/proc/loadavg"), text, sizeof(text)) != 0) return -1.0;
    double l1, l5, l15;
    unsigned running = 0, total = 0;
    return sscanf(text, "%lf %lf %lf %u/%u", &l1, &l5, &l15, &running, &total) == 5
        && running <= INT_MAX ? (double)running : -1.0;
}

/* Collects root_pid and all of its descendants. Returns how many were stored. */
static size_t collect_process_tree(pid_t root_pid, pid_t *ids, size_t capacity) {
    pid_t pending[MAX_PROCESS_TREE];
    size_t count = 0, pending_count = 0;
    if (root_pid <= 0 || capacity == 0) return 0;
    pending[pending_count++] = root_pid;

    while (pending_count > 0 && count < capacity) {
        pid_t pid = pending[--pending_count];
        int seen = 0;
        for (size_t i = 0; i < count; i++) {
            if (ids[i] == pid) { seen = 1; break; }
        }
        if (seen) continue;
        ids[count++] = pid;

        char path[128];
        snprintf(path, sizeof(path), "/proc/%d/task/%d/children", pid, pid);
        FILE *children = fopen(path, "r");
        if (!children) continue;
        int child;
        while (pending_count < MAX_PROCESS_TREE && fscanf(children, "%d", &child) == 1) {
            if (child > 0) pending[pending_count++] = (pid_t)child;
        }
        fclose(children);
    }
    return count;
}

double system_get_workload_processes(void) {
    pid_t ids[MAX_PROCESS_TREE];
    size_t count = collect_process_tree(gm_workload_pid, ids, MAX_PROCESS_TREE);
    return count > 0 ? (double)count : -1.0;
}

static long process_rss_kb(pid_t pid) {
    /* statm is a single short line: "size resident shared ..." in pages. */
    static long page_kb;
    if (page_kb == 0) {
        long page_size = sysconf(_SC_PAGESIZE);
        page_kb = page_size / 1024;
        if (page_kb <= 0) { page_kb = 0; return -1; }
    }
    char path[64], text[128];
    snprintf(path, sizeof(path), "/proc/%d/statm", pid);
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    int failed = pread_text(fd, text, sizeof(text)) != 0;
    close(fd);
    unsigned long size = 0, resident = 0;
    if (failed || sscanf(text, "%lu %lu", &size, &resident) != 2) return -1;
    if (resident > (unsigned long)LONG_MAX / (unsigned long)page_kb) return -1;
    return (long)resident * page_kb;
}

double system_get_workload_memory_kb(void) {
    pid_t ids[MAX_PROCESS_TREE];
    size_t count = collect_process_tree(gm_workload_pid, ids, MAX_PROCESS_TREE);
    long total = 0;
    int found = 0;
    for (size_t i = 0; i < count; i++) {
        long kb = process_rss_kb(ids[i]);
        if (kb >= 0) {
            if (total > LONG_MAX - kb) return -1.0;
            total += kb;
            found = 1;
        }
    }
    return found ? (double)total : -1.0;
}

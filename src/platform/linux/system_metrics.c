#define _GNU_SOURCE
#include "system_metrics.h"
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>
#include <limits.h>

/* sysconf(_SC_NPROCESSORS_ONLN) reads /sys on every call, so the value is cached. */
int sys_core_count(void) {
    static int cached = -1;
    int cores = __atomic_load_n(&cached, __ATOMIC_RELAXED);
    if (cores < 0) {
        long online = sysconf(_SC_NPROCESSORS_ONLN);
        cores = online > 0 ? (int)online : 0;
        __atomic_store_n(&cached, cores, __ATOMIC_RELAXED);
    }
    return cores;
}

/* Files are opened once and re-read with pread(), which avoids an open/close pair per sample. */
static int sys_open_once(int *slot, const char *path) {
    int fd = __atomic_load_n(slot, __ATOMIC_ACQUIRE);
    if (fd != -1) return fd;
    int opened = open(path, O_RDONLY | O_CLOEXEC);
    if (opened < 0) opened = -2; /* remember the failure, do not retry every sample */
    int expected = -1;
    if (!__atomic_compare_exchange_n(slot, &expected, opened, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        if (opened >= 0) close(opened);
        return expected;
    }
    return opened;
}

static int sys_pread_text(int fd, char *buffer, size_t size) {
    if (fd < 0) return -1;
    ssize_t n = pread(fd, buffer, size - 1, 0);
    if (n <= 0) return -1;
    buffer[n] = '\0';
    return 0;
}

static int sys_freq_fds[SYS_MAX_CORES];
static int sys_loadavg_fd = -1;
static int sys_fds_initialised;

static void sys_init_fd_slots(void) {
    if (__atomic_exchange_n(&sys_fds_initialised, 1, __ATOMIC_ACQ_REL)) return;
    for (int i = 0; i < SYS_MAX_CORES; i++) sys_freq_fds[i] = -1;
}

double sys_core_frequency_mhz(int core) {
    if (core < 0 || core >= SYS_MAX_CORES) return SYS_METRIC_NOT_SUPPORTED;
    sys_init_fd_slots();
    char path[128], text[32];
    snprintf(path, sizeof(path),
             "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_cur_freq", core);
    int fd = sys_open_once(&sys_freq_fds[core], path);
    if (sys_pread_text(fd, text, sizeof(text)) != 0) return SYS_METRIC_NOT_SUPPORTED;
    return (double)strtoul(text, NULL, 10) / 1000.0;
}

int sys_running_processes(void) {
    int fd = sys_open_once(&sys_loadavg_fd, "/proc/loadavg");
    char text[128];
    if (sys_pread_text(fd, text, sizeof(text)) != 0) return SYS_METRIC_NOT_SUPPORTED;
    double l1, l5, l15;
    unsigned running = 0, total = 0;
    return sscanf(text, "%lf %lf %lf %u/%u", &l1, &l5, &l15, &running, &total) == 5
        && running <= INT_MAX ? (int)running : SYS_METRIC_NOT_SUPPORTED;
}

size_t sys_collect_process_tree(pid_t root_pid, pid_t *ids, size_t capacity) {
    pid_t pending[SYS_MAX_PROCESS_TREE];
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
        FILE *fp = fopen(path, "r");
        if (!fp) continue;
        int child;
        while (pending_count < SYS_MAX_PROCESS_TREE && fscanf(fp, "%d", &child) == 1) {
            if (child > 0) pending[pending_count++] = (pid_t)child;
        }
        fclose(fp);
    }
    return count;
}

int sys_workload_process_count(pid_t root_pid) {
    pid_t ids[SYS_MAX_PROCESS_TREE];
    size_t count = sys_collect_process_tree(root_pid, ids, SYS_MAX_PROCESS_TREE);
    return count > 0 ? (int)count : SYS_METRIC_NOT_SUPPORTED;
}

static long process_rss_kb(pid_t pid) {
    /* statm is a single short line: "size resident shared ..." in pages. */
    static long page_kb = 0;
    if (page_kb == 0) {
        long page_size = sysconf(_SC_PAGESIZE);
        if (page_size <= 0) return SYS_METRIC_NOT_SUPPORTED;
        page_kb = page_size / 1024;
        if (page_kb == 0) return SYS_METRIC_NOT_SUPPORTED;
    }
    char path[64], text[128];
    snprintf(path, sizeof(path), "/proc/%d/statm", pid);
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return SYS_METRIC_NOT_SUPPORTED;
    int failed = sys_pread_text(fd, text, sizeof(text)) != 0;
    close(fd);
    unsigned long size = 0, resident = 0;
    if (failed || sscanf(text, "%lu %lu", &size, &resident) != 2) return SYS_METRIC_NOT_SUPPORTED;
    if (resident > (unsigned long)LONG_MAX / (unsigned long)page_kb)
        return SYS_METRIC_NOT_SUPPORTED;
    return (long)resident * page_kb;
}

long sys_workload_memory_kb(pid_t root_pid) {
    pid_t ids[SYS_MAX_PROCESS_TREE];
    size_t count = sys_collect_process_tree(root_pid, ids, SYS_MAX_PROCESS_TREE);
    long total = 0;
    int found = 0;
    for (size_t i = 0; i < count; i++) {
        long kb = process_rss_kb(ids[i]);
        if (kb >= 0) {
            if (total > LONG_MAX - kb) return SYS_METRIC_NOT_SUPPORTED;
            total += kb;
            found = 1;
        }
    }
    return found ? total : SYS_METRIC_NOT_SUPPORTED;
}

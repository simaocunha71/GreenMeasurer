#define _GNU_SOURCE
#include "system_metrics.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int sys_core_count(void) {
    long cores = sysconf(_SC_NPROCESSORS_ONLN);
    return cores > 0 ? (int)cores : 0;
}

double sys_core_frequency_mhz(int core) {
    char path[128];
    snprintf(path, sizeof(path),
             "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_cur_freq", core);
    FILE *fp = fopen(path, "r");
    if (!fp) return SYS_METRIC_NOT_SUPPORTED;
    unsigned long khz = 0;
    int ok = fscanf(fp, "%lu", &khz) == 1;
    fclose(fp);
    return ok ? (double)khz / 1000.0 : SYS_METRIC_NOT_SUPPORTED;
}

double sys_running_processes(void) {
    FILE *fp = fopen("/proc/loadavg", "r");
    if (!fp) return SYS_METRIC_NOT_SUPPORTED;
    double l1, l5, l15;
    unsigned running = 0, total = 0;
    int ok = fscanf(fp, "%lf %lf %lf %u/%u", &l1, &l5, &l15, &running, &total) == 5;
    fclose(fp);
    return ok ? (double)running : SYS_METRIC_NOT_SUPPORTED;
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

double sys_workload_process_count(pid_t root_pid) {
    pid_t ids[SYS_MAX_PROCESS_TREE];
    size_t count = sys_collect_process_tree(root_pid, ids, SYS_MAX_PROCESS_TREE);
    return count > 0 ? (double)count : SYS_METRIC_NOT_SUPPORTED;
}

static double process_rss_kb(pid_t pid) {
    char path[64], line[256];
    snprintf(path, sizeof(path), "/proc/%d/status", pid);
    FILE *fp = fopen(path, "r");
    if (!fp) return SYS_METRIC_NOT_SUPPORTED;
    double kb = SYS_METRIC_NOT_SUPPORTED;
    while (fgets(line, sizeof(line), fp)) {
        unsigned long value;
        if (sscanf(line, "VmRSS: %lu kB", &value) == 1) { kb = (double)value; break; }
    }
    fclose(fp);
    return kb;
}

double sys_workload_memory_kb(pid_t root_pid) {
    pid_t ids[SYS_MAX_PROCESS_TREE];
    size_t count = sys_collect_process_tree(root_pid, ids, SYS_MAX_PROCESS_TREE);
    double total = 0.0;
    int found = 0;
    for (size_t i = 0; i < count; i++) {
        double kb = process_rss_kb(ids[i]);
        if (kb >= 0.0) { total += kb; found = 1; }
    }
    return found ? total : SYS_METRIC_NOT_SUPPORTED;
}

#include "../include/system_metrics.h"
#include <stdlib.h>
#include <string.h>

static int selected(const char *name) {
    const char *failure = getenv("GREEN_TEST_SYSTEM_FAILURE");
    return failure && strcmp(failure, name) == 0;
}

int __wrap_sys_core_count(void) {
    static int reads;
    reads++;
    return selected("cores-preflight") || (selected("cores-runtime") && reads >= 3) ? 0 : 2;
}

int __wrap_sys_workload_process_count(pid_t pid) {
    (void)pid;
    static int reads;
    reads++;
    return selected("workload-runtime") && reads >= 2 ? SYS_METRIC_NOT_SUPPORTED : 2;
}

long __wrap_sys_workload_memory_kb(pid_t pid) {
    (void)pid;
    static int reads;
    reads++;
    return selected("memory-runtime") && reads >= 2 ? SYS_METRIC_NOT_SUPPORTED : 1024;
}

int __wrap_sys_running_processes(void) {
    static int reads;
    reads++;
    if (selected("running-preflight") || (selected("running-runtime") && reads >= 3) ||
        (selected("running-transient") && reads == 3))
        return SYS_METRIC_NOT_SUPPORTED;
    return 7;
}

double __wrap_sys_core_frequency_mhz(int core) {
    static int reads[2];
    if (core < 0 || core >= 2) return SYS_METRIC_NOT_SUPPORTED;
    reads[core]++;
    if ((selected("frequency-preflight") && core == 0) ||
        (selected("frequency-runtime") && core == 0 && reads[core] >= 3) ||
        (selected("frequency-transient") && core == 0 && reads[core] == 3))
        return SYS_METRIC_NOT_SUPPORTED;
    return 1000 + core;
}

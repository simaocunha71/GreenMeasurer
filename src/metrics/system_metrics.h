#ifndef SYSTEM_METRICS_H
#define SYSTEM_METRICS_H

#include <stddef.h>
#include <sys/types.h>

#define SYS_METRIC_NOT_SUPPORTED -1.0
#define SYS_MAX_CORES 512
#define SYS_MAX_PROCESS_TREE 1024

/* Number of logical cores currently online (0 on failure). */
int sys_core_count(void);

/* Current frequency of a logical core in MHz, or SYS_METRIC_NOT_SUPPORTED. */
double sys_core_frequency_mhz(int core);

/* Runnable processes system-wide (first field of the /proc/loadavg 4th column). */
double sys_running_processes(void);

/* Collects root_pid and all of its descendants. Returns how many were stored. */
size_t sys_collect_process_tree(pid_t root_pid, pid_t *ids, size_t capacity);

/* Number of live processes in the workload process tree. */
double sys_workload_process_count(pid_t root_pid);

/* Resident set size (KiB) summed over the workload process tree. */
double sys_workload_memory_kb(pid_t root_pid);

#endif

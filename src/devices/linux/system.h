#ifndef GM_DEVICES_SYSTEM_H
#define GM_DEVICES_SYSTEM_H

/* System metrics. Readers take no arguments and return a negative value when unavailable.
 * The workload ones use gm_workload_pid, which the runtime sets when the workload starts. */

double system_get_core_count(void);
double system_get_core_frequency_mhz(void);   /* mean over all online cores */
double system_get_running_processes(void);    /* runnable processes system-wide */
double system_get_workload_processes(void);   /* live processes in the workload tree */
double system_get_workload_memory_kb(void);   /* resident memory of the workload tree */

#endif

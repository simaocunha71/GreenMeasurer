#ifndef GM_DEVICES_INTEL_CPU_POWERCAP_H
#define GM_DEVICES_INTEL_CPU_POWERCAP_H

/* Applies a package power limit: -1 restores the saved baseline, 0 does nothing,
 * and positive values set a limit in watts. Returns 0 on success. */
int intel_cpu_set_powercap(int watts);
/* Restores the saved limits and releases the powercap context. */
int intel_cpu_destroy_powercap(void);
void intel_cpu_powercap_end(void);

int intel_cpu_calibrate_powercap(int ntimes, int core, const char *command,
                                 const char *program, const char *calib_csv_file,
                                 const char *process_script, int min_powercap, int max_powercap,
                                 int time_out_limit, float temp_threshold, int variance, int memory);

#endif

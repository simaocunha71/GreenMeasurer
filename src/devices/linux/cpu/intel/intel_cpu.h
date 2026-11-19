#ifndef GM_DEVICES_INTEL_CPU_H
#define GM_DEVICES_INTEL_CPU_H

/* Intel CPU metrics and RAPL power capping. Metric readers take no arguments and return a
 * negative value when the quantity is unavailable. Call intel_cpu_begin() before reading and
 * intel_cpu_end() afterwards. */

#include <stdio.h>

#define CPU_SANDYBRIDGE             42
#define CPU_SANDYBRIDGE_EP          45
#define CPU_IVYBRIDGE               58
#define CPU_IVYBRIDGE_EP            62
#define CPU_HASWELL                 60
#define CPU_HASWELL_ULT             69
#define CPU_HASWELL_GT3E            70
#define CPU_HASWELL_EP              63
#define CPU_BROADWELL               61
#define CPU_BROADWELL_GT3E          71
#define CPU_BROADWELL_EP            79
#define CPU_BROADWELL_DE            86
#define CPU_SKYLAKE                 78
#define CPU_SKYLAKE_HS              94
#define CPU_SKYLAKE_X               85
#define CPU_KNIGHTS_LANDING         87
#define CPU_KNIGHTS_MILL            133
#define CPU_KABYLAKE_MOBILE         142
#define CPU_KABYLAKE                158
#define CPU_ATOM_SILVERMONT         55
#define CPU_ATOM_AIRMONT            76
#define CPU_ATOM_MERRIFIELD         74
#define CPU_ATOM_MOOREFIELD         90
#define CPU_ATOM_GOLDMONT           92
#define CPU_ATOM_GEMINI_LAKE        122
#define CPU_ATOM_DENVERTON          95
#define CANNONLAKE_L                102
#define ICELAKE_L                   126
#define ICELAKE                     125
#define COMETLAKE_L                 166
#define COMETLAKE                   165
#define TIGERLAKE_L                 140
#define TIGERLAKE                   141
#define ALDERLAKE                   151
#define ALDERLAKE_L                 154
#define ALDERLAKE_N                 190
#define RAPTORLAKE                  183
#define RAPTORLAKE_P                186
#define RAPTORLAKE_S                191
#define METEORLAKE                  172
#define METEORLAKE_L                170
#define ICELAKE_D                   108
#define ICELAKE_X                   106
#define SAPPHIRERAPIDS_X            143
#define EMERALDRAPIDS_X             207

/* Non-zero when this is not a supported Intel CPU, or when neither RAPL nor temperature work. */
int intel_cpu_begin(void);
/* Restores any changed power limit, then releases the MSR device and lm-sensors. */
void intel_cpu_end(void);

double intel_cpu_get_package(void);      /* cumulative joules (RAPL package) */
double intel_cpu_get_core(void);         /* cumulative joules (RAPL PP0) */
double intel_cpu_get_dram(void);         /* cumulative joules (RAPL DRAM) */
double intel_cpu_get_temperature(void);  /* Celsius, mean of the coretemp "Core N" inputs */

/* Applies a package power limit: -1 restores this machine's saved baseline, 0 does nothing and a
 * positive value is a limit in watts. Returns 0 on success. The first call on a machine saves the
 * current limits as the baseline in /var/lib/greenmeasurer. */
int intel_cpu_set_powercap(int watts);
/* Restores the baseline limits and releases the powercap context. Returns 0 on success. */
int intel_cpu_destroy_powercap(void);

/* Implemented by the engine; measures the workload ntimes at the given powercap and writes a CSV
 * (header and rows) to measurements_file. Returns 0 on success. */
extern int runTesting(int ntimes, int core, const char *command, const char *language,
                      const char *program, const char *measurements_file, int powercap,
                      int time_out_limit, float temp_threshold, int variance, int memory);

/* Runs runTesting() for powercap -1 and then min_powercap..max_powercap watts in 1 W steps, and
 * merges the per-powercap CSVs into calib_csv_file (default "powercap_calibration.csv"). If
 * process_script exists, it is run with python3 on the merged CSV. Returns 0 on success. */
int intel_cpu_calibrate_powercap(int ntimes, int core, const char *command, const char *language,
                                 const char *program, const char *calib_csv_file,
                                 const char *process_script, int min_powercap, int max_powercap,
                                 int time_out_limit, float temp_threshold, int variance, int memory);

const char *intel_cpu_diagnose(void);

#endif

#ifndef GM_DEVICES_INTEL_CPU_H
#define GM_DEVICES_INTEL_CPU_H

/* Intel CPU metrics. Metric readers take no arguments and return a
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

/* Non-zero when this is not a supported Intel CPU. Unavailable MSR readings return negative. */
int intel_cpu_begin(void);
/* Releases the MSR device. */
void intel_cpu_end(void);

double intel_cpu_get_package(void);      /* cumulative joules (RAPL package) */
double intel_cpu_get_core(void);         /* cumulative joules (RAPL PP0) */
double intel_cpu_get_dram(void);         /* cumulative joules (RAPL DRAM) */

const char *intel_cpu_diagnose(void);

#endif

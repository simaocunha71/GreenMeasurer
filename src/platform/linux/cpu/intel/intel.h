#ifndef INTEL_H
#define INTEL_H

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <inttypes.h>
#include <unistd.h>
#include <math.h>
#include <string.h>

#include <powercap/powercap.h>
#include <raplcap/raplcap.h>

#include "../../../../include/energy-backend.h"

#define MSR_RAPL_POWER_UNIT         0x606

#define MSR_PKG_RAPL_POWER_LIMIT    0x610
#define MSR_PKG_ENERGY_STATUS       0x611
#define MSR_PKG_PERF_STATUS         0x613
#define MSR_PKG_POWER_INFO          0x614

#define MSR_PP0_POWER_LIMIT         0x638
#define MSR_PP0_ENERGY_STATUS       0x639
#define MSR_PP0_POLICY              0x63A
#define MSR_PP0_PERF_STATUS         0x63B

#define MSR_DRAM_POWER_LIMIT        0x618
#define MSR_DRAM_ENERGY_STATUS      0x619
#define MSR_DRAM_PERF_STATUS        0x61B
#define MSR_DRAM_POWER_INFO         0x61C

#define POWER_UNIT_OFFSET           0
#define POWER_UNIT_MASK             0x0F
#define ENERGY_UNIT_OFFSET          0x08
#define ENERGY_UNIT_MASK            0x1F00
#define TIME_UNIT_OFFSET            0x10
#define TIME_UNIT_MASK              0xF000

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

extern CpuBackend intel_cpu_backend;

int intel_calibrate_powercap(int ntimes, int core, const char *command, const char *language,
                            const char *program, const char *calib_csv_file,
                            const char *process_script, int min_powercap, int max_powercap,
                            int time_out_limit, float temp_threshold, int variance, int memory);

#endif /* INTEL_H */
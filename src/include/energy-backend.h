#ifndef ENERGY_BACKEND_H
#define ENERGY_BACKEND_H

#include <stdio.h>
#include <stdint.h>

/**
 * @brief Energy consumed during a measurement window (in Joules).
 */
typedef struct {
    double package;   /* Total socket / Package energy */
    double core;      /* CPU Cores energy */
    double gpu;       /* GPU energy (integrated or dedicated) */
    double dram;      /* DRAM / Memory energy */
} EnergyMetrics;

/**
 * @brief Unified interface for hardware energy measurement and platform management.
 */
typedef struct {
    const char *name;

    int (*detect)(void);
    int (*init)(int core);
    int (*before)(int core);
    int (*after)(int core, EnergyMetrics *out);
    void (*cleanup)(void);

    int (*measure_memory)(const char *command);

    int (*set_powercap)(int watts);
    int (*destroy_powercap)(void);
    
    void (*calibrate_powercap)(int ntimes, int core, const char *command, const char *language, 
                                const char *program, const char *calib_csv_file,
                                const char *process_script, int min_powercap, int max_powercap, 
                                int time_out_limit, float temp_threshold, int variance, int memory);
} EnergyBackend;

#if defined(__GNUC__) || defined(__clang__)
    #define WEAK_DRIVER __attribute__((weak))
#else
    #define WEAK_DRIVER
#endif

WEAK_DRIVER extern EnergyBackend intel_backend;
WEAK_DRIVER extern EnergyBackend amd_backend;
WEAK_DRIVER extern EnergyBackend mac_backend;
WEAK_DRIVER extern EnergyBackend nvidia_backend;

#endif /* ENERGY_BACKEND_H */
#ifndef ENERGY_BACKEND_H
#define ENERGY_BACKEND_H

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>

#define METRIC_NOT_SUPPORTED -1.0

/* =========================================================================
   CPU BACKEND INTERFACE (vtable pattern)
   ========================================================================= */
typedef struct cpu_backend {
    const char *name;

    int  (*detect)(void);
    int  (*init)(int core);
    void (*cleanup)(void);

    /* Temporal sampling triggers */
    int  (*sample_start)(int core);
    int  (*sample_stop)(int core);

    /* Required callbacks: return METRIC_NOT_SUPPORTED when a metric is unavailable. */
    double (*get_package_energy)(int core);
    double (*get_core_energy)(int core);
    double (*get_dram_energy)(int core);

    /* Telemetry and power capping */
    float  (*get_temperature)(int core);
    int    (*set_powercap)(int watts);
    int    (*destroy_powercap)(void);

    /* Powercap calibration routine */
    void (*calibrate_powercap)(int ntimes, int core, const char *command, const char *language, 
                                const char *program, const char *calib_csv_file,
                                const char *process_script, int min_powercap, int max_powercap, 
                                int time_out_limit, float temp_threshold, int variance, int memory);

    /* Optional: short human-readable hint explaining why detect() failed (may be NULL). */
    const char *(*diagnose)(void);
} CpuBackend;

/* =========================================================================
   GPU BACKEND INTERFACE (vtable pattern)
   ========================================================================= */
typedef struct gpu_backend {
    const char *name;

    int  (*detect)(void);
    int  (*init)(int device_idx);
    void (*cleanup)(void);

    /* Temporal sampling triggers */
    int  (*sample_start)(int device_idx);
    int  (*sample_stop)(int device_idx);

    /* get_energy is required; unsupported optional telemetry returns METRIC_NOT_SUPPORTED. */
    double (*get_energy)(int device_idx);      /* Joules consumed during window */
    double (*get_power_draw)(int device_idx);   /* Instantaneous power in Watts */
    float  (*get_temperature)(int device_idx);  /* Core temperature in Celsius */
    double (*get_utilization)(int device_idx);  /* Load percentage (0.0 to 100.0) */

    /* Optional: short human-readable hint explaining why detect() failed (may be NULL). */
    const char *(*diagnose)(void);
} GpuBackend;

typedef enum {
    ENERGY_BACKEND_CPU,
    ENERGY_BACKEND_GPU
} EnergyBackendType;

typedef struct energy_backend_descriptor {
    EnergyBackendType type;
    int priority;
    union {
        CpuBackend *cpu;
        GpuBackend *gpu;
    } backend;
} EnergyBackendDescriptor;

/* Linux Resident Set Size memory profiler */
int platform_measure_memory(const char *command);

#define ENERGY_BACKEND_DESCRIPTOR_SYMBOL "energy_backend_descriptor"

#endif /* ENERGY_BACKEND_H */
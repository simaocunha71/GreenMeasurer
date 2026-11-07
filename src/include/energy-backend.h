#ifndef ENERGY_BACKEND_H
#define ENERGY_BACKEND_H

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

#define METRIC_NOT_SUPPORTED -1.0

typedef enum {
    ENERGY_METRIC_WINDOW,
    ENERGY_METRIC_GAUGE,
    ENERGY_METRIC_PEAK
} EnergyMetricKind;

typedef enum {
    ENERGY_METRIC_DEVICE,
    ENERGY_METRIC_PROCESS
} EnergyMetricScope;

typedef enum {
    ENERGY_METRIC_FLAG_NONE = 0,
    ENERGY_METRIC_FLAG_CPU_TEMPERATURE = 1u << 0,
    ENERGY_METRIC_FLAG_GPU_TEMPERATURE = 1u << 1
} EnergyMetricFlags;

typedef struct energy_metric_descriptor {
    /* Stable CSV-safe key, e.g. "Package" or "gpu_utilization". */
    const char *name;
    const char *unit;
    EnergyMetricKind kind;
    EnergyMetricScope scope;
    unsigned int flags;
} EnergyMetricDescriptor;

typedef struct energy_metric_function {
    const char *name;
    double (*read)(int device, pid_t process_id);
} EnergyMetricFunction;

typedef struct energy_metric_function_map {
    uint32_t version;
    size_t count;
    const EnergyMetricFunction *functions;
} EnergyMetricFunctionMap;

#define ENERGY_METRIC_FUNCTION_MAP_VERSION 1u
#define ENERGY_METRIC_FUNCTION_MAP_SYMBOL "energy_metric_function_map"
#define ENERGY_METRIC_FUNCTION(function) {#function, function}

typedef struct energy_metric_info {
    const char *description;
    const char *origin;
} EnergyMetricInfo;

typedef struct energy_metric_info_map {
    uint32_t version;
    size_t count;
    const EnergyMetricInfo *entries;
} EnergyMetricInfoMap;

#define ENERGY_METRIC_INFO_MAP_VERSION 1u
#define ENERGY_METRIC_INFO_MAP_SYMBOL "energy_metric_info_map"
#define ENERGY_METRIC_DESCRIPTOR_ENTRY(name, unit, kind, scope, flags, function, description, origin) \
    {name, unit, kind, scope, flags},
#define ENERGY_METRIC_FUNCTION_ENTRY(name, unit, kind, scope, flags, function, description, origin) \
    {#function, function},
#define ENERGY_METRIC_INFO_ENTRY(name, unit, kind, scope, flags, function, description, origin) \
    {description, origin},
#define ENERGY_DEFINE_METRICS(prefix, list) \
    static const EnergyMetricDescriptor prefix##_metrics[] = { list(ENERGY_METRIC_DESCRIPTOR_ENTRY) }; \
    static const EnergyMetricFunction prefix##_metric_functions[] = { list(ENERGY_METRIC_FUNCTION_ENTRY) }; \
    static const EnergyMetricInfo prefix##_metric_info[] = { list(ENERGY_METRIC_INFO_ENTRY) }; \
    const EnergyMetricFunctionMap energy_metric_function_map = { \
        ENERGY_METRIC_FUNCTION_MAP_VERSION, \
        sizeof(prefix##_metric_functions) / sizeof(prefix##_metric_functions[0]), prefix##_metric_functions \
    }; \
    const EnergyMetricInfoMap energy_metric_info_map = { \
        ENERGY_METRIC_INFO_MAP_VERSION, \
        sizeof(prefix##_metric_info) / sizeof(prefix##_metric_info[0]), prefix##_metric_info \
    }; \
    static double prefix##_get_metric(size_t index, int device, pid_t pid) { \
        return index < energy_metric_function_map.count \
            ? prefix##_metric_functions[index].read(device, pid) : METRIC_NOT_SUPPORTED; \
    }

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

    /* Every reported metric is declared in metrics and read through get_metric. */
    const EnergyMetricDescriptor *metrics;
    size_t metric_count;
    double (*get_metric)(size_t metric_index, int core, pid_t process_id);

    /* Power capping */
    int    (*set_powercap)(int watts);
    int    (*destroy_powercap)(void);

    /* Powercap calibration routine */
    int  (*calibrate_powercap)(int ntimes, int core, const char *command, const char *language,
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

    /* Every reported metric is declared in metrics and read through get_metric. */
    const EnergyMetricDescriptor *metrics;
    size_t metric_count;
    double (*get_metric)(size_t metric_index, int device_idx, pid_t process_id);

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

/* Optional plugin symbol: close a window and advance its baseline to the
   previous stop on the next call, without losing energy between windows. */
#define ENERGY_BACKEND_SAMPLE_NEXT_SYMBOL "energy_backend_sample_next"

/* Write framework metadata columns followed by metrics from selected backends. */
void energy_backend_write_csv_header(FILE *stream, int overtime);

#endif /* ENERGY_BACKEND_H */
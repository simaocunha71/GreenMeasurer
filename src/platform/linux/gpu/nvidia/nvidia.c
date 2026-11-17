#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <nvml.h>
#include "../../../../include/energy-backend.h"

static double nvidia_metric_energy(int device, pid_t pid);
static double nvidia_metric_power(int device, pid_t pid);
static double nvidia_metric_temperature(int device, pid_t pid);
static double nvidia_metric_utilization(int device, pid_t pid);
static double nvidia_metric_vram(int device, pid_t pid);

static const EnergyMetricDefinition nvidia_metric_definitions[] = {
    {
        .descriptor = {.name = "GPU", .unit = "J", .kind = ENERGY_METRIC_WINDOW,
                       .scope = ENERGY_METRIC_DEVICE, .flags = ENERGY_METRIC_FLAG_NONE},
        .function = ENERGY_METRIC_FUNCTION(nvidia_metric_energy),
        .info = {.description = "GPU energy over the measurement window", .origin = "NVML total energy counter"}
    },
    {
        .descriptor = {.name = "Power", .unit = "W", .kind = ENERGY_METRIC_GAUGE,
                       .scope = ENERGY_METRIC_DEVICE, .flags = ENERGY_METRIC_FLAG_NONE},
        .function = ENERGY_METRIC_FUNCTION(nvidia_metric_power),
        .info = {.description = "GPU power draw", .origin = "NVML power usage"}
    },
    {
        .descriptor = {.name = "Temperature", .unit = "C", .kind = ENERGY_METRIC_GAUGE,
                       .scope = ENERGY_METRIC_DEVICE, .flags = ENERGY_METRIC_FLAG_GPU_TEMPERATURE},
        .function = ENERGY_METRIC_FUNCTION(nvidia_metric_temperature),
        .info = {.description = "GPU temperature", .origin = "NVML GPU temperature sensor"}
    },
    {
        .descriptor = {.name = "Utilization", .unit = "%", .kind = ENERGY_METRIC_GAUGE,
                       .scope = ENERGY_METRIC_DEVICE, .flags = ENERGY_METRIC_FLAG_NONE},
        .function = ENERGY_METRIC_FUNCTION(nvidia_metric_utilization),
        .info = {.description = "GPU utilization", .origin = "NVML utilization rates"}
    },
    {
        .descriptor = {.name = "VRAM", .unit = "MiB", .kind = ENERGY_METRIC_PEAK,
                       .scope = ENERGY_METRIC_PROCESS, .flags = ENERGY_METRIC_FLAG_NONE},
        .function = ENERGY_METRIC_FUNCTION(nvidia_metric_vram),
        .info = {.description = "Workload GPU memory usage", .origin = "NVML compute and graphics process lists"}
    }
};
ENERGY_DEFINE_METRIC_TABLE(nvidia, nvidia_metric_definitions)

static nvmlDevice_t nvml_device;
static unsigned long long energy_before_mj = 0;
static unsigned long long energy_after_mj = 0;
static int energy_before_valid = 0;
static int energy_after_valid = 0;
static int nvml_initialized = 0;
static int next_sample_has_previous = 0;

static int nvidia_detect(void) {
    if (nvmlInit() != NVML_SUCCESS) return 0;

    unsigned int device_count = 0;
    nvmlReturn_t status = nvmlDeviceGetCount(&device_count);
    nvmlShutdown();
    return status == NVML_SUCCESS && device_count > 0;
}

static const char *nvidia_diagnose(void) {
    return "NVML could not be initialized or no device was found (check the driver)";
}

static int nvidia_init(int device_idx) {
    if (!nvml_initialized && nvmlInit() != NVML_SUCCESS) return -1;
    if (nvmlDeviceGetHandleByIndex(device_idx, &nvml_device) != NVML_SUCCESS) {
        if (!nvml_initialized) nvmlShutdown();
        return -1;
    }
    nvml_initialized = 1;
    return 0;
}

static int nvidia_sample_start(int device_idx) {
    (void)device_idx;
    next_sample_has_previous = 0;
    if (!nvml_initialized) return -1;
    energy_before_valid = 0;
    if (nvmlDeviceGetTotalEnergyConsumption(nvml_device, &energy_before_mj) != NVML_SUCCESS) {
        return -1;
    }
    energy_before_valid = 1;
    return 0;
}

static int nvidia_sample_stop(int device_idx) {
    (void)device_idx;
    if (!nvml_initialized) return -1;
    energy_after_valid = 0;
    if (nvmlDeviceGetTotalEnergyConsumption(nvml_device, &energy_after_mj) != NVML_SUCCESS) {
        return -1;
    }
    energy_after_valid = 1;
    return 0;
}

double energy_backend_read_counter(size_t metric_index, int device, pid_t pid) {
    (void)device;
    (void)pid;
    if (metric_index >= sizeof(nvidia_metric_definitions) / sizeof(nvidia_metric_definitions[0]) ||
        nvidia_metric_definitions[metric_index].function.read != nvidia_metric_energy ||
        !nvml_initialized || !energy_after_valid)
        return METRIC_NOT_SUPPORTED;
    return (double)energy_after_mj / 1000.0;
}

int energy_backend_sample_next(int device_idx) {
    if (next_sample_has_previous) {
        energy_before_mj = energy_after_mj;
        energy_before_valid = energy_after_valid;
    }
    int result = nvidia_sample_stop(device_idx);
    next_sample_has_previous = result == 0;
    return result;
}

static double nvidia_get_energy(int device_idx) {
    (void)device_idx;
    if (!nvml_initialized || !energy_before_valid || !energy_after_valid ||
        energy_after_mj < energy_before_mj) {
        return METRIC_NOT_SUPPORTED;
    }
    return (double)(energy_after_mj - energy_before_mj) / 1000.0;
}

static double nvidia_get_power_draw(int device_idx) {
    (void)device_idx;
    if (!nvml_initialized) return METRIC_NOT_SUPPORTED;
    unsigned int milliwatts = 0;
    if (nvmlDeviceGetPowerUsage(nvml_device, &milliwatts) != NVML_SUCCESS) {
        return METRIC_NOT_SUPPORTED;
    }
    return (double)milliwatts / 1000.0;
}

static float nvidia_get_temperature(int device_idx) {
    (void)device_idx;
    if (!nvml_initialized) return -1.0f;
    unsigned int temp_c = 0;
    if (nvmlDeviceGetTemperature(nvml_device, NVML_TEMPERATURE_GPU, &temp_c) != NVML_SUCCESS) {
        return -1.0f;
    }
    return (float)temp_c;
}

static double nvidia_get_utilization(int device_idx) {
    (void)device_idx;
    if (!nvml_initialized) return METRIC_NOT_SUPPORTED;
    nvmlUtilization_t util;
    if (nvmlDeviceGetUtilizationRates(nvml_device, &util) != NVML_SUCCESS) {
        return METRIC_NOT_SUPPORTED;
    }
    return (double)util.gpu;
}

static double nvidia_get_process_memory_usage(nvmlProcessInfo_t *processes,
                                              unsigned int process_count,
                                              int process_id) {
    double memory_mib = METRIC_NOT_SUPPORTED;
    for (unsigned int i = 0; i < process_count; i++) {
        if (processes[i].pid == (unsigned int)process_id &&
            processes[i].usedGpuMemory != (unsigned long long)NVML_VALUE_NOT_AVAILABLE) {
            double current_mib =
                (double)processes[i].usedGpuMemory / (1024.0 * 1024.0);
            if (memory_mib < current_mib) memory_mib = current_mib;
        }
    }
    return memory_mib;
}

static double nvidia_query_process_memory(nvmlReturn_t (*query)(
                                              nvmlDevice_t, unsigned int *,
                                              nvmlProcessInfo_t *),
                                          int process_id) {
    unsigned int process_count = 0;
    nvmlReturn_t status = query(nvml_device, &process_count, NULL);
    if (status != NVML_SUCCESS && status != NVML_ERROR_INSUFFICIENT_SIZE) {
        return METRIC_NOT_SUPPORTED;
    }
    if (process_count == 0) return METRIC_NOT_SUPPORTED;

    nvmlProcessInfo_t *processes = calloc(process_count, sizeof(*processes));
    if (!processes) return METRIC_NOT_SUPPORTED;

    status = query(nvml_device, &process_count, processes);
    double memory_mib = status == NVML_SUCCESS
        ? nvidia_get_process_memory_usage(processes, process_count, process_id)
        : METRIC_NOT_SUPPORTED;
    free(processes);
    return memory_mib;
}

static double nvidia_get_memory_usage(int device_idx, int process_id) {
    (void)device_idx;
    if (!nvml_initialized || process_id <= 0) return METRIC_NOT_SUPPORTED;

    double compute_memory =
        nvidia_query_process_memory(nvmlDeviceGetComputeRunningProcesses,
                                    process_id);
    double graphics_memory =
        nvidia_query_process_memory(nvmlDeviceGetGraphicsRunningProcesses,
                                    process_id);

    if (compute_memory < 0.0) return graphics_memory;
    if (graphics_memory < 0.0) return compute_memory;
    return compute_memory > graphics_memory ? compute_memory : graphics_memory;
}

static double nvidia_metric_energy(int device, pid_t pid) {
    (void)pid;
    return nvidia_get_energy(device);
}
static double nvidia_metric_power(int device, pid_t pid) {
    (void)pid;
    return nvidia_get_power_draw(device);
}
static double nvidia_metric_temperature(int device, pid_t pid) {
    (void)pid;
    return nvidia_get_temperature(device);
}
static double nvidia_metric_utilization(int device, pid_t pid) {
    (void)pid;
    return nvidia_get_utilization(device);
}
static double nvidia_metric_vram(int device, pid_t pid) {
    return nvidia_get_memory_usage(device, (int)pid);
}
static void nvidia_cleanup(void) {
    if (nvml_initialized) {
        nvmlShutdown();
        nvml_initialized = 0;
    }
}

GpuBackend nvidia_gpu_backend = {
    .name = "NVIDIA NVML GPU",
    .detect = nvidia_detect,
    .init = nvidia_init,
    .cleanup = nvidia_cleanup,
    .sample_start = nvidia_sample_start,
    .sample_stop = nvidia_sample_stop,
    .metrics = nvidia_metrics,
    .metric_count = sizeof(nvidia_metrics) / sizeof(nvidia_metrics[0]),
    .get_metric = nvidia_get_metric,
    .diagnose = nvidia_diagnose
};

const EnergyBackendDescriptor energy_backend_descriptor = {
    .type = ENERGY_BACKEND_GPU,
    .priority = 100,
    .backend.gpu = &nvidia_gpu_backend
};
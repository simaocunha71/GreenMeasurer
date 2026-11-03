#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <nvml.h>
#include "../../../../include/energy-backend.h"

static nvmlDevice_t nvml_device;
static unsigned long long energy_before_mj = 0;
static unsigned long long energy_after_mj = 0;
static int energy_before_valid = 0;
static int energy_after_valid = 0;
static int nvml_initialized = 0;

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

static const EnergyMetricDescriptor nvidia_metrics[] = {
    {"GPU", "J", ENERGY_METRIC_WINDOW, ENERGY_METRIC_DEVICE, ENERGY_METRIC_FLAG_NONE},
    {"Power", "W", ENERGY_METRIC_GAUGE, ENERGY_METRIC_DEVICE, ENERGY_METRIC_FLAG_NONE},
    {"Temperature", "C", ENERGY_METRIC_GAUGE, ENERGY_METRIC_DEVICE,
     ENERGY_METRIC_FLAG_GPU_TEMPERATURE},
    {"Utilization", "%", ENERGY_METRIC_GAUGE, ENERGY_METRIC_DEVICE,
     ENERGY_METRIC_FLAG_NONE},
    {"VRAM", "MiB", ENERGY_METRIC_PEAK, ENERGY_METRIC_PROCESS, ENERGY_METRIC_FLAG_NONE}
};

static double nvidia_get_metric(size_t metric_index, int device_idx,
                                pid_t process_id) {
    switch (metric_index) {
        case 0: return nvidia_get_energy(device_idx);
        case 1: return nvidia_get_power_draw(device_idx);
        case 2: return (double)nvidia_get_temperature(device_idx);
        case 3: return nvidia_get_utilization(device_idx);
        case 4: return nvidia_get_memory_usage(device_idx, (int)process_id);
        default: return METRIC_NOT_SUPPORTED;
    }
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
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
    .get_energy = nvidia_get_energy,
    .get_power_draw = nvidia_get_power_draw,
    .get_temperature = nvidia_get_temperature,
    .get_utilization = nvidia_get_utilization,
    .diagnose = nvidia_diagnose
};

const EnergyBackendDescriptor energy_backend_descriptor = {
    .type = ENERGY_BACKEND_GPU,
    .priority = 100,
    .backend.gpu = &nvidia_gpu_backend
};
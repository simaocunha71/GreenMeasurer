#include "nvidia.h"
#include <stdio.h>
#include <unistd.h>
#include <nvml.h>

static nvmlDevice_t nvml_device;
static unsigned long long energy_before_mj = 0;
static int nvml_initialized = 0;

static int nvidia_detect(void) {
    return (access("/usr/bin/nvidia-smi", X_OK) == 0) ? 1 : 0;
}

static int nvidia_init(int device_idx) {
    if (nvmlInit() != NVML_SUCCESS) {
        return -1;
    }
    if (nvmlDeviceGetHandleByIndex(device_idx, &nvml_device) != NVML_SUCCESS) {
        nvmlShutdown();
        return -1;
    }
    nvml_initialized = 1;
    return 0;
}

static int nvidia_before(int device_idx) {
    (void)device_idx;
    if (!nvml_initialized) return -1;
    if (nvmlDeviceGetTotalEnergyConsumption(nvml_device, &energy_before_mj) != NVML_SUCCESS) {
        return -1;
    }
    return 1;
}

static int nvidia_after(int device_idx, EnergyMetrics *out) {
    (void)device_idx;
    if (!nvml_initialized || !out) return -1;

    unsigned long long energy_after_mj = 0;
    if (nvmlDeviceGetTotalEnergyConsumption(nvml_device, &energy_after_mj) != NVML_SUCCESS) {
        out->gpu = -1.0; /* NOT_SUPPORTED if NVML query fails */
        return -1;
    }

    /* Convert millijoules to Joules */
    double delta_joules = (double)(energy_after_mj - energy_before_mj) / 1000.0;
    out->gpu = delta_joules;

    /* CPU metrics remain unchanged */
    out->package = -1.0;
    out->core = -1.0;
    out->dram = -1.0;
    return 1;
}

static void nvidia_cleanup(void) {
    if (nvml_initialized) {
        nvmlShutdown();
        nvml_initialized = 0;
    }
}

EnergyBackend nvidia_backend = {
    .name = "NVIDIA NVML GPU Energy Meter",
    .detect = nvidia_detect,
    .init = nvidia_init,
    .before = nvidia_before,
    .after = nvidia_after,
    .cleanup = nvidia_cleanup,
    .measure_memory = NULL,
    .set_powercap = NULL,
    .destroy_powercap = NULL
};
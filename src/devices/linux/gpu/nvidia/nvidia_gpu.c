#include "nvidia_gpu.h"
#include "gm_device.h"
#include <nvml.h>
#include <stdlib.h>

static nvmlDevice_t device;
static int nvml_initialized;

int nvidia_begin(void) {
    if (nvmlInit() != NVML_SUCCESS) return 1;
    if (nvmlDeviceGetHandleByIndex(0, &device) != NVML_SUCCESS) {
        nvmlShutdown();
        return 1;
    }
    nvml_initialized = 1;
    return 0;
}

void nvidia_end(void) {
    if (nvml_initialized) nvmlShutdown();
    nvml_initialized = 0;
}

double nvidia_get_energy(void) {
    unsigned long long millijoules;
    if (!nvml_initialized || nvmlDeviceGetTotalEnergyConsumption(device, &millijoules) != NVML_SUCCESS)
        return -1.0;
    return (double)millijoules / 1000.0;
}

double nvidia_get_power(void) {
    unsigned int milliwatts;
    if (!nvml_initialized || nvmlDeviceGetPowerUsage(device, &milliwatts) != NVML_SUCCESS) return -1.0;
    return (double)milliwatts / 1000.0;
}

double nvidia_get_temperature(void) {
    unsigned int celsius;
    if (!nvml_initialized || nvmlDeviceGetTemperature(device, NVML_TEMPERATURE_GPU, &celsius) != NVML_SUCCESS)
        return -1.0;
    return (double)celsius;
}

double nvidia_get_utilization(void) {
    nvmlUtilization_t utilization;
    if (!nvml_initialized || nvmlDeviceGetUtilizationRates(device, &utilization) != NVML_SUCCESS) return -1.0;
    return (double)utilization.gpu;
}

static double process_memory_mib(const nvmlProcessInfo_t *processes, unsigned int count, pid_t pid) {
    double memory_mib = -1.0;
    for (unsigned int i = 0; i < count; i++) {
        if (processes[i].pid != (unsigned int)pid ||
            processes[i].usedGpuMemory == (unsigned long long)NVML_VALUE_NOT_AVAILABLE) continue;
        double current = (double)processes[i].usedGpuMemory / (1024.0 * 1024.0);
        if (memory_mib < current) memory_mib = current;
    }
    return memory_mib;
}

static double query_process_memory(nvmlReturn_t (*query)(nvmlDevice_t, unsigned int *, nvmlProcessInfo_t *),
                                   pid_t pid) {
    unsigned int count = 0;
    nvmlReturn_t status = query(device, &count, NULL);
    if ((status != NVML_SUCCESS && status != NVML_ERROR_INSUFFICIENT_SIZE) || count == 0) return -1.0;

    nvmlProcessInfo_t *processes = calloc(count, sizeof(*processes));
    if (!processes) return -1.0;
    double memory_mib = query(device, &count, processes) == NVML_SUCCESS
        ? process_memory_mib(processes, count, pid) : -1.0;
    free(processes);
    return memory_mib;
}

double nvidia_get_vram(void) {
    pid_t pid = gm_workload_pid;
    if (!nvml_initialized || pid <= 0) return -1.0;
    double compute = query_process_memory(nvmlDeviceGetComputeRunningProcesses, pid);
    double graphics = query_process_memory(nvmlDeviceGetGraphicsRunningProcesses, pid);
    if (compute < 0.0) return graphics;
    if (graphics < 0.0) return compute;
    return compute > graphics ? compute : graphics;
}

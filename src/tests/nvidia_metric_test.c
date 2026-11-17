#include "../platform/linux/gpu/nvidia/nvidia.c"
#include <assert.h>

static unsigned long long energy_mj = 1000;

nvmlReturn_t nvmlInit(void) { return NVML_SUCCESS; }
nvmlReturn_t nvmlShutdown(void) { return NVML_SUCCESS; }
nvmlReturn_t nvmlDeviceGetCount(unsigned int *count) { *count = 1; return NVML_SUCCESS; }
nvmlReturn_t nvmlDeviceGetHandleByIndex(unsigned int index, nvmlDevice_t *device) {
    (void)index; *device = &energy_mj; return NVML_SUCCESS;
}
nvmlReturn_t nvmlDeviceGetTotalEnergyConsumption(nvmlDevice_t device, unsigned long long *energy) {
    (void)device; *energy = energy_mj; return NVML_SUCCESS;
}
nvmlReturn_t nvmlDeviceGetPowerUsage(nvmlDevice_t device, unsigned int *power) {
    (void)device; *power = 25000; return NVML_SUCCESS;
}
nvmlReturn_t nvmlDeviceGetTemperature(nvmlDevice_t device, int sensor, unsigned int *temperature) {
    (void)device; (void)sensor; *temperature = 55; return NVML_SUCCESS;
}
nvmlReturn_t nvmlDeviceGetUtilizationRates(nvmlDevice_t device, nvmlUtilization_t *utilization) {
    (void)device; utilization->gpu = 75; return NVML_SUCCESS;
}
nvmlReturn_t nvmlDeviceGetComputeRunningProcesses(nvmlDevice_t device, unsigned int *count,
                                                   nvmlProcessInfo_t *processes) {
    (void)device;
    if (!processes) { *count = 1; return NVML_ERROR_INSUFFICIENT_SIZE; }
    processes[0].pid = 42; processes[0].usedGpuMemory = 2ULL * 1024 * 1024;
    *count = 1;
    return NVML_SUCCESS;
}
nvmlReturn_t nvmlDeviceGetGraphicsRunningProcesses(nvmlDevice_t device, unsigned int *count,
                                                    nvmlProcessInfo_t *processes) {
    (void)device; (void)processes; *count = 0; return NVML_SUCCESS;
}

int main(void) {
    const char *names[] = {"GPU", "Power", "Temperature", "Utilization", "VRAM"};
    const char *functions[] = {"nvidia_metric_energy", "nvidia_metric_power",
                               "nvidia_metric_temperature", "nvidia_metric_utilization",
                               "nvidia_metric_vram"};
    const char *units[] = {"J", "W", "C", "%", "MiB"};
    const EnergyMetricKind kinds[] = {ENERGY_METRIC_WINDOW, ENERGY_METRIC_GAUGE,
                                      ENERGY_METRIC_GAUGE, ENERGY_METRIC_GAUGE,
                                      ENERGY_METRIC_PEAK};
    const char *descriptions[] = {"GPU energy over the measurement window", "GPU power draw",
                                  "GPU temperature", "GPU utilization", "Workload GPU memory usage"};
    const char *origins[] = {"NVML total energy counter", "NVML power usage",
                             "NVML GPU temperature sensor", "NVML utilization rates",
                             "NVML compute and graphics process lists"};
    assert(nvidia_gpu_backend.metric_count == 5);
    for (size_t i = 0; i < 5; i++) {
        assert(strcmp(nvidia_gpu_backend.metrics[i].name, names[i]) == 0);
        assert(strcmp(nvidia_gpu_backend.metrics[i].unit, units[i]) == 0);
        assert(nvidia_gpu_backend.metrics[i].kind == kinds[i]);
        assert(strcmp(energy_metric_function_map.functions[i].name, functions[i]) == 0);
        assert(strcmp(energy_metric_info_map.entries[i].description, descriptions[i]) == 0);
        assert(strcmp(energy_metric_info_map.entries[i].origin, origins[i]) == 0);
    }
    assert(nvidia_gpu_backend.metrics[2].flags == ENERGY_METRIC_FLAG_GPU_TEMPERATURE);
    assert(nvidia_gpu_backend.metrics[4].scope == ENERGY_METRIC_PROCESS);
    assert(nvidia_gpu_backend.detect() == 1 && nvidia_gpu_backend.init(0) == 0);
    assert(nvidia_gpu_backend.sample_start(0) == 0);
    energy_mj = 3500;
    assert(nvidia_gpu_backend.sample_stop(0) == 0);
    assert(nvidia_gpu_backend.get_metric(0, 0, 0) == 2.5);
    assert(energy_backend_read_counter(0, 0, 0) == 3.5);
    assert(energy_backend_read_counter(1, 0, 0) == METRIC_NOT_SUPPORTED);
    assert(energy_backend_sample_next(0) == 0);
    assert(energy_backend_read_counter(0, 0, 0) == 3.5);
    energy_mj = 4000;
    assert(energy_backend_sample_next(0) == 0);
    assert(energy_backend_read_counter(0, 0, 0) == 4.0);
    assert(nvidia_gpu_backend.get_metric(0, 0, 0) == 0.5);
    assert(nvidia_gpu_backend.get_metric(1, 0, 0) == 25);
    assert(nvidia_gpu_backend.get_metric(2, 0, 0) == 55);
    assert(nvidia_gpu_backend.get_metric(3, 0, 0) == 75);
    assert(nvidia_gpu_backend.get_metric(4, 0, 42) == 2);
    assert(nvidia_gpu_backend.get_metric(5, 0, 0) == METRIC_NOT_SUPPORTED);
    nvidia_gpu_backend.cleanup();
    puts("NVIDIA metric registry and readings passed.");
    return 0;
}

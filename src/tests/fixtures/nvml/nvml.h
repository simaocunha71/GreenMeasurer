#ifndef TEST_NVML_H
#define TEST_NVML_H

typedef void *nvmlDevice_t;
typedef int nvmlReturn_t;
typedef struct { unsigned int gpu; unsigned int memory; } nvmlUtilization_t;
typedef struct { unsigned int pid; unsigned long long usedGpuMemory; } nvmlProcessInfo_t;

#define NVML_SUCCESS 0
#define NVML_ERROR_INSUFFICIENT_SIZE 7
#define NVML_TEMPERATURE_GPU 0
#define NVML_VALUE_NOT_AVAILABLE (~0ULL)

nvmlReturn_t nvmlInit(void);
nvmlReturn_t nvmlShutdown(void);
nvmlReturn_t nvmlDeviceGetCount(unsigned int *count);
nvmlReturn_t nvmlDeviceGetHandleByIndex(unsigned int index, nvmlDevice_t *device);
nvmlReturn_t nvmlDeviceGetTotalEnergyConsumption(nvmlDevice_t device, unsigned long long *energy);
nvmlReturn_t nvmlDeviceGetPowerUsage(nvmlDevice_t device, unsigned int *power);
nvmlReturn_t nvmlDeviceGetTemperature(nvmlDevice_t device, int sensor, unsigned int *temperature);
nvmlReturn_t nvmlDeviceGetUtilizationRates(nvmlDevice_t device, nvmlUtilization_t *utilization);
nvmlReturn_t nvmlDeviceGetComputeRunningProcesses(nvmlDevice_t device, unsigned int *count,
                                                   nvmlProcessInfo_t *processes);
nvmlReturn_t nvmlDeviceGetGraphicsRunningProcesses(nvmlDevice_t device, unsigned int *count,
                                                    nvmlProcessInfo_t *processes);

#endif

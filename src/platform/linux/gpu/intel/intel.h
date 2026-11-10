#ifndef GREENMEASURER_INTEL_GPU_H
#define GREENMEASURER_INTEL_GPU_H

#include "../../../../include/energy-backend.h"

float intel_gpu_read_hwmon_temperature(void);
int intel_gpu_hardware_present(void);

extern GpuBackend intel_gpu_backend;

#endif

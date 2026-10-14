#ifndef SENSORS_H
#define SENSORS_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *name;
    int (*detect)(void);
    float (*get_temperature)(void);
} CpuSensorDriver;

typedef struct {
    const char *name;
    int (*detect)(void);
    float (*get_temperature)(void);
} GpuSensorDriver;

#if defined(__GNUC__) || defined(__clang__)
    #define WEAK_SENSOR __attribute__((weak))
#else
    #define WEAK_SENSOR
#endif

extern WEAK_SENSOR CpuSensorDriver intel_sensor_driver;
extern WEAK_SENSOR CpuSensorDriver amd_sensor_driver;
extern WEAK_SENSOR CpuSensorDriver mac_sensor_driver;
extern WEAK_SENSOR GpuSensorDriver nvidia_sensor_driver;
extern WEAK_SENSOR GpuSensorDriver intel_gpu_sensor_driver;

float getTemperature(int cpu_legacy_id);
float getInitialTemperature(const char *calibration_file);
float getGpuTemperature(void);

/* Plataform-agnostic temperature calibration dump */
int dump_temperature_baseline(const char *target_calib_file);

#ifdef __cplusplus
}
#endif

#endif /* SENSORS_H */
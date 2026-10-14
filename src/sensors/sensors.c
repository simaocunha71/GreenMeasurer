#include "../include/sensors.h"
#include <unistd.h>

static CpuSensorDriver *active_cpu_sensor = NULL;
static GpuSensorDriver *active_gpu_sensor = NULL;

static CpuSensorDriver* resolve_cpu_sensor(void) {
    if (active_cpu_sensor != NULL) {
        return active_cpu_sensor;
    }

    CpuSensorDriver *drivers[] = {
        &intel_sensor_driver,
        &amd_sensor_driver,
        &mac_sensor_driver,
        NULL
    };

    for (int i = 0; drivers[i] != NULL; i++) {
        /* Verifica se o driver foi ligado no binário e detetado no host */
        if (drivers[i] && drivers[i]->detect && drivers[i]->detect() == 1) {
            active_cpu_sensor = drivers[i];
            return active_cpu_sensor;
        }
    }
    return NULL;
}

static GpuSensorDriver* resolve_gpu_sensor(void) {
    if (active_gpu_sensor != NULL) {
        return active_gpu_sensor;
    }

    GpuSensorDriver *drivers[] = {
        &nvidia_sensor_driver,
        &intel_gpu_sensor_driver,
        NULL
    };

    for (int i = 0; drivers[i] != NULL; i++) {
        if (drivers[i] && drivers[i]->detect && drivers[i]->detect() == 1) {
            active_gpu_sensor = drivers[i];
            return active_gpu_sensor;
        }
    }
    return NULL;
}

float getTemperature(int cpu_legacy_id) {
    (void)cpu_legacy_id;
    CpuSensorDriver *driver = resolve_cpu_sensor();
    if (driver && driver->get_temperature) {
        return driver->get_temperature();
    }
    return -1.0f;
}

float getInitialTemperature(const char *calibration_file) {
    if (!calibration_file || strlen(calibration_file) == 0) {
        return -1.0f;
    }

    FILE *file = fopen(calibration_file, "r");
    if (!file) {
        return -1.0f;
    }

    char line[256];
    float result = -1.0f;

    while (fgets(line, sizeof(line), file)) {
        if (strstr(line, "Average temperature:") != NULL) {
            char *colon = strchr(line, ':');
            if (colon) {
                char *comma = strchr(colon, ',');
                if (comma) {
                    *comma = '.';
                }

                float val = 0.0f;
                if (sscanf(colon + 1, "%f", &val) == 1) {
                    result = val;
                    break;
                }
            }
        }
    }

    fclose(file);
    return result;
}

float getGpuTemperature(void) {
    GpuSensorDriver *driver = resolve_gpu_sensor();
    if (driver && driver->get_temperature) {
        float t = driver->get_temperature();
        if (t > 0.0f) {
            return t;
        }
    }

    /* If the CPU is Intel and there is no dedicated GPU, the iGPU temperature is the CPU temperature 
    https://community.intel.com/t5/Graphics/Hello-how-can-i-monitor-HD-620-GPU-temperature/td-p/662714 */
    CpuSensorDriver *cpu_driver = resolve_cpu_sensor();
    if (cpu_driver != NULL) {
        return getTemperature(0);
    }

    return -1.0f;
}

int dump_temperature_baseline(const char *target_calib_file) {
#if defined(__linux__)
    char dump_cmd[600];
    snprintf(dump_cmd, sizeof(dump_cmd), "sensors > %s 2>/dev/null", target_calib_file);
    if (system(dump_cmd) != 0) {
        /* Fallback caso lm-sensors não esteja instalado: cria ficheiro via sysfs/C API */
        FILE *fp = fopen(target_calib_file, "w");
        if (fp) {
            fprintf(fp, "# Linux sysfs thermal dump\n");
            fclose(fp);
        }
    }
    return 0;
#elif defined(__APPLE__)
    FILE *fp = fopen(target_calib_file, "w");
    if (fp) {
        fprintf(fp, "# Apple Silicon / macOS thermal baseline\n");
        fclose(fp);
    }
    return 0;
#else
    FILE *fp = fopen(target_calib_file, "w");
    if (fp) {
        fprintf(fp, "# Generic platform thermal baseline\n");
        fclose(fp);
    }
    return 0;
#endif
}
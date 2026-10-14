#include "../include/sensors.h"
#include <regex.h>

static int intel_sensor_probe(void) {
    FILE *f = fopen("/proc/cpuinfo", "r");
    if (!f) return 0;

    char line[256];
    int is_intel = 0;
    while (fgets(line, sizeof(line), f)) {
        if (strstr(line, "GenuineIntel")) {
            is_intel = 1;
            break;
        }
    }
    fclose(f);
    return is_intel;
}

static float intel_sensor_get_temp(void) {
    FILE *fp = popen("sensors", "r");
    if (!fp) {
        perror("[ERROR] Failed to run sensors command");
        return -1.0f;
    }

    char line[256];
    double sum = 0.0;
    int count = 0;

    while (fgets(line, sizeof(line), fp)) {
        if (strncmp(line, "Core ", 5) == 0) {
            char *plus = strchr(line, '+');
            if (plus) {
                for (char *p = plus; *p != '\0'; p++) {
                    if (*p == ',') *p = '.';
                }

                float temp = 0.0f;
                if (sscanf(plus + 1, "%f", &temp) == 1 && temp > 0.0f) {
                    sum += temp;
                    count++;
                }
            }
        }
    }
    pclose(fp);

    if (count > 0) {
        return (float)(sum / count);
    }

    return -1.0f;
}

CpuSensorDriver intel_sensor_driver = {
    .name = "Intel Coretemp",
    .detect = intel_sensor_probe,
    .get_temperature = intel_sensor_get_temp
};

static int intel_gpu_sensor_probe(void) {
    return 1;
}

static float intel_gpu_sensor_get_temp(void) {
    FILE *fp = popen("sensors", "r");
    if (!fp) {
        return -1.0f;
    }

    char line[256];
    float pkg_temp = -1.0f;

    while (fgets(line, sizeof(line), fp)) {
        if (strstr(line, "Package id 0:") != NULL) {
            char *plus = strchr(line, '+');
            if (plus) {
                for (char *p = plus; *p != '\0'; p++) {
                    if (*p == ',') *p = '.';
                }
                if (sscanf(plus + 1, "%f", &pkg_temp) == 1) {
                    break;
                }
            }
        }
    }
    pclose(fp);
    return pkg_temp;
}

GpuSensorDriver intel_gpu_sensor_driver = {
    .name = "Intel Integrated GPU (Package Temp)",
    .detect = intel_gpu_sensor_probe,
    .get_temperature = intel_gpu_sensor_get_temp
};
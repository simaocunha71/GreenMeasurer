#include "nvidia.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static int nvidia_sensor_detect(void) {
    return (access("/usr/bin/nvidia-smi", X_OK) == 0) ? 1 : 0;
}

static float nvidia_sensor_get_temp(void) {
    const char *cmd = "nvidia-smi --query-gpu=temperature.gpu --format=csv,noheader,nounits 2>/dev/null";
    FILE *fp = popen(cmd, "r");
    if (!fp) {
        return -1.0f;
    }

    char buf[32] = "";
    if (!fgets(buf, sizeof(buf), fp)) {
        pclose(fp);
        return -1.0f;
    }
    pclose(fp);

    return (float)atof(buf);
}

GpuSensorDriver nvidia_sensor_driver = {
    .name = "NVIDIA GPU Thermal Sensor",
    .detect = nvidia_sensor_detect,
    .get_temperature = nvidia_sensor_get_temp
};
#include "../include/sensors.h"

static int amd_sensor_probe(void) {
    FILE *f = fopen("/proc/cpuinfo", "r");
    if (!f) return 0;

    char line[256];
    int is_amd = 0;
    while (fgets(line, sizeof(line), f)) {
        if (strstr(line, "AuthenticAMD")) {
            is_amd = 1;
            break;
        }
    }
    fclose(f);
    return is_amd;
}

static float amd_sensor_get_temp(void) {
    const char *cmd = "sensors | awk -F: '/^(Tdie|Tctl):/ { split($2, a, \"+\"); split(a[2], b, \"°\"); printf \"%%.2f\\n\", b[1]; exit }'";

    FILE *fp = popen(cmd, "r");
    if (!fp) {
        perror("[ERROR] Failed to run AMD sensors command");
        return -1.0f;
    }

    char buf[64] = "";
    if (!fgets(buf, sizeof(buf), fp)) {
        pclose(fp);
        return -1.0f;
    }
    pclose(fp);

    return (float)atof(buf);
}

CpuSensorDriver amd_sensor_driver = {
    .name = "AMD k10temp",
    .detect = amd_sensor_probe,
    .get_temperature = amd_sensor_get_temp
};
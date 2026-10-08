#include "sensors.h"

float getTemperature(int cpu) {
    char cmd[512];

    if (cpu == 0) {
        // Intel: filtra linhas "Core X:", isola o valor após o primeiro '+' antes de '°' e calcula a média
        snprintf(cmd, sizeof(cmd),
            "sensors | awk -F: '/^Core [0-9]+:/ { split($2, a, \"+\"); split(a[2], b, \"°\"); sum += b[1]; count++ } END { if (count > 0) printf \"%%.2f\\n\", sum / count; else print -1 }'");
    } else {
        // AMD: filtra linhas "Tdie:" (ou "Tctl:"), isola o primeiro número após '+' e sai logo
        snprintf(cmd, sizeof(cmd),
            "sensors | awk -F: '/^(Tdie|Tctl):/ { split($2, a, \"+\"); split(a[2], b, \"°\"); printf \"%%.2f\\n\", b[1]; exit }'");
    }

    FILE *fp = popen(cmd, "r");
    if (!fp) {
        perror("[ERROR] Failed to run sensors command");
        return -1.0f;
    }

    char buf[64] = "";
    if (!fgets(buf, sizeof(buf), fp)) {
        pclose(fp);
        return -1.0f;
    }
    pclose(fp);

    float avgTemp = atof(buf);
    return avgTemp;
}

float getInitialTemperature(int cpu) {
    (void)cpu;
    FILE *file = fopen("/tmp/cores_temperatures.txt", "r");
    if (!file) {
        perror("[ERROR] Error opening file /tmp/cores_temperatures.txt");
        return -1.0f;
    }

    char line[256];
    float result = -1.0f;

    while (fgets(line, sizeof(line), file)) {
        if (strstr(line, "Average temperature:") != NULL) {
            char *colon = strchr(line, ':');
            if (colon) {
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
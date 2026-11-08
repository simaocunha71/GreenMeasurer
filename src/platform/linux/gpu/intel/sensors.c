#include "intel.h"
#include <dirent.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

static float read_hwmon_temperature(const char *root) {
    DIR *hwmon_dir = opendir(root);
    if (!hwmon_dir) return -1.0f;

    struct dirent *entry;
    float temperature = -1.0f;
    while ((entry = readdir(hwmon_dir)) != NULL) {
        if (entry->d_name[0] == '.') continue;

        char device_path[PATH_MAX];
        int written = snprintf(device_path, sizeof(device_path), "%s/%s", root, entry->d_name);
        if (written < 0 || (size_t)written >= sizeof(device_path)) continue;

        DIR *device_dir = opendir(device_path);
        if (!device_dir) continue;

        struct dirent *sensor_entry;
        while ((sensor_entry = readdir(device_dir)) != NULL) {
            size_t name_length = strlen(sensor_entry->d_name);
            if (name_length <= strlen("_label") ||
                strcmp(sensor_entry->d_name + name_length - strlen("_label"), "_label") != 0) {
                continue;
            }

            char label_path[PATH_MAX];
            char input_path[PATH_MAX];
            written = snprintf(label_path, sizeof(label_path), "%s/%s",
                               device_path, sensor_entry->d_name);
            if (written < 0 || (size_t)written >= sizeof(label_path)) continue;

            FILE *label_file = fopen(label_path, "r");
            if (!label_file) continue;

            char label[64] = "";
            int has_label = fgets(label, sizeof(label), label_file) != NULL;
            fclose(label_file);
            if (!has_label || strncasecmp(label, "GPU", 3) != 0) continue;

            size_t input_name_length = name_length - strlen("_label");
            written = snprintf(input_path, sizeof(input_path), "%s/%.*s_input",
                               device_path, (int)input_name_length, sensor_entry->d_name);
            if (written < 0 || (size_t)written >= sizeof(input_path)) continue;

            FILE *input_file = fopen(input_path, "r");
            if (!input_file) continue;

            long temperature_millidegrees;
            int read_ok = fscanf(input_file, "%ld", &temperature_millidegrees) == 1;
            fclose(input_file);
            if (read_ok && temperature_millidegrees > 0 &&
                temperature_millidegrees < 150000) {
                temperature = (float)temperature_millidegrees / 1000.0f;
                closedir(device_dir);
                closedir(hwmon_dir);
                return temperature;
            }
        }
        closedir(device_dir);
    }

    closedir(hwmon_dir);
    return temperature;
}

float intel_gpu_read_hwmon_temperature(void) {
    return read_hwmon_temperature("/sys/class/hwmon");
}

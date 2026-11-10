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

static int intel_gpu_device_present(const char *root) {
    DIR *devices_dir = opendir(root);
    if (!devices_dir) return 0;

    struct dirent *entry;
    while ((entry = readdir(devices_dir)) != NULL) {
        if (entry->d_name[0] == '.') continue;

        char vendor_path[PATH_MAX];
        char class_path[PATH_MAX];
        int written = snprintf(vendor_path, sizeof(vendor_path), "%s/%s/vendor",
                               root, entry->d_name);
        if (written < 0 || (size_t)written >= sizeof(vendor_path)) continue;
        written = snprintf(class_path, sizeof(class_path), "%s/%s/class",
                           root, entry->d_name);
        if (written < 0 || (size_t)written >= sizeof(class_path)) continue;

        FILE *vendor_file = fopen(vendor_path, "r");
        FILE *class_file = fopen(class_path, "r");
        if (!vendor_file || !class_file) {
            if (vendor_file) fclose(vendor_file);
            if (class_file) fclose(class_file);
            continue;
        }

        unsigned int vendor;
        unsigned int device_class;
        int read_ok = fscanf(vendor_file, "%x", &vendor) == 1 &&
                      fscanf(class_file, "%x", &device_class) == 1;
        fclose(vendor_file);
        fclose(class_file);
        if (read_ok && vendor == 0x8086 && (device_class >> 16) == 0x03) {
            closedir(devices_dir);
            return 1;
        }
    }

    closedir(devices_dir);
    return 0;
}

float intel_gpu_read_hwmon_temperature(void) {
    return read_hwmon_temperature("/sys/class/hwmon");
}

int intel_gpu_hardware_present(void) {
    return intel_gpu_device_present("/sys/bus/pci/devices");
}

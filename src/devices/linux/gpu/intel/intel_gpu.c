#include "intel_gpu.h"
#include "msr.h"
#include <dirent.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#define MEASURED_CORE 0
#define HWMON_ROOT "/sys/class/hwmon"

static RaplMsr msr = {.fd = -1};
static RaplDomain pp1 = RAPL_DOMAIN(MSR_PP1_ENERGY_STATUS);

double intel_gpu_get_temperature(void) {
    DIR *hwmon_dir = opendir(HWMON_ROOT);
    if (!hwmon_dir) return -1.0;

    struct dirent *entry;
    double temperature = -1.0;
    while (temperature < 0.0 && (entry = readdir(hwmon_dir)) != NULL) {
        if (entry->d_name[0] == '.') continue;

        char device_path[PATH_MAX];
        int written = snprintf(device_path, sizeof(device_path), "%s/%s", HWMON_ROOT, entry->d_name);
        if (written < 0 || (size_t)written >= sizeof(device_path)) continue;
        DIR *device_dir = opendir(device_path);
        if (!device_dir) continue;

        struct dirent *sensor;
        while (temperature < 0.0 && (sensor = readdir(device_dir)) != NULL) {
            size_t name_length = strlen(sensor->d_name);
            if (name_length <= strlen("_label") ||
                strcmp(sensor->d_name + name_length - strlen("_label"), "_label") != 0) continue;

            char label_path[PATH_MAX], input_path[PATH_MAX], label[64] = "";
            if (snprintf(label_path, sizeof(label_path), "%s/%s", device_path, sensor->d_name)
                    >= (int)sizeof(label_path)) continue;
            FILE *label_file = fopen(label_path, "r");
            if (!label_file) continue;
            int has_label = fgets(label, sizeof(label), label_file) != NULL;
            fclose(label_file);
            if (!has_label || strncasecmp(label, "GPU", 3) != 0) continue;

            if (snprintf(input_path, sizeof(input_path), "%s/%.*s_input", device_path,
                         (int)(name_length - strlen("_label")), sensor->d_name)
                    >= (int)sizeof(input_path)) continue;
            FILE *input_file = fopen(input_path, "r");
            if (!input_file) continue;
            long millidegrees;
            int read_ok = fscanf(input_file, "%ld", &millidegrees) == 1;
            fclose(input_file);
            if (read_ok && millidegrees > 0 && millidegrees < 150000)
                temperature = (double)millidegrees / 1000.0;
        }
        closedir(device_dir);
    }
    closedir(hwmon_dir);
    return temperature;
}

/* True when a PCI display controller (class 0x03) from Intel (vendor 0x8086) is present. */
int intel_gpu_hardware_present(void) {
    const char *root = "/sys/bus/pci/devices";
    DIR *devices_dir = opendir(root);
    if (!devices_dir) return 0;

    struct dirent *entry;
    while ((entry = readdir(devices_dir)) != NULL) {
        if (entry->d_name[0] == '.') continue;

        char vendor_path[PATH_MAX], class_path[PATH_MAX];
        if (snprintf(vendor_path, sizeof(vendor_path), "%s/%s/vendor", root, entry->d_name)
                >= (int)sizeof(vendor_path) ||
            snprintf(class_path, sizeof(class_path), "%s/%s/class", root, entry->d_name)
                >= (int)sizeof(class_path)) continue;

        FILE *vendor_file = fopen(vendor_path, "r");
        FILE *class_file = fopen(class_path, "r");
        unsigned int vendor = 0, device_class = 0;
        int read_ok = vendor_file && class_file &&
                      fscanf(vendor_file, "%x", &vendor) == 1 &&
                      fscanf(class_file, "%x", &device_class) == 1;
        if (vendor_file) fclose(vendor_file);
        if (class_file) fclose(class_file);
        if (read_ok && vendor == 0x8086 && (device_class >> 16) == 0x03) {
            closedir(devices_dir);
            return 1;
        }
    }
    closedir(devices_dir);
    return 0;
}

int intel_gpu_begin(void) {
    int has_rapl = rapl_msr_open(&msr, MEASURED_CORE) == 0 && rapl_msr_energy(&msr, &pp1) >= 0.0;
    int unavailable = !has_rapl && intel_gpu_get_temperature() < 0.0 && !intel_gpu_hardware_present();
    if (unavailable) rapl_msr_close(&msr);
    return unavailable;
}

void intel_gpu_end(void) { rapl_msr_close(&msr); }

double intel_gpu_get_energy(void) { return rapl_msr_energy(&msr, &pp1); }

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <math.h>
#include <errno.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include <limits.h>
#include "../../../../include/energy-backend.h"

#define MSR_RAPL_POWER_UNIT   0x606
#define MSR_PP1_ENERGY_STATUS 0x641

static double energy_units = 0.0;
static int energy_units_valid = 0;
static uint32_t pp1_before = 0;
static uint32_t pp1_after = 0;
static int pp1_before_valid = 0;
static int pp1_after_valid = 0;
static int igpu_supported = 0;

static float intel_gpu_read_hwmon_temperature(void) {
    DIR *hwmon_dir = opendir("/sys/class/hwmon");
    if (!hwmon_dir) return -1.0f;

    struct dirent *entry;
    float temperature = -1.0f;
    while ((entry = readdir(hwmon_dir)) != NULL) {
        if (entry->d_name[0] == '.') continue;

        char device_path[PATH_MAX];
        int written = snprintf(device_path, sizeof(device_path),
                               "/sys/class/hwmon/%s", entry->d_name);
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

static int open_msr(int core) {
    char msr_filename[64];
    snprintf(msr_filename, sizeof(msr_filename), "/dev/cpu/%d/msr", core);
    return open(msr_filename, O_RDONLY);
}

static uint64_t read_msr(int fd, int which) {
    uint64_t data = (uint64_t)-1;
    if (pread(fd, &data, sizeof(data), (off_t)which) != sizeof(data)) {
        return (uint64_t)-1;
    }
    return data;
}

static int intel_gpu_detect(void) {
    int has_gpu_temperature = intel_gpu_read_hwmon_temperature() > 0.0f;
    int fd = open_msr(0);
    if (fd >= 0) {
        uint64_t pwr_unit = read_msr(fd, MSR_RAPL_POWER_UNIT);
        uint64_t pp1_val = read_msr(fd, MSR_PP1_ENERGY_STATUS);
        close(fd);

        if (pwr_unit != (uint64_t)-1 && pp1_val != (uint64_t)-1) {
            igpu_supported = 1;
        }
    }

    return igpu_supported || has_gpu_temperature;
}

static const char *intel_gpu_diagnose(void) {
    if (access("/dev/cpu/0/msr", F_OK) != 0) {
        return "MSR device not found (try: sudo modprobe msr)";
    }
    if (access("/dev/cpu/0/msr", R_OK) != 0) {
        return "no read access to /dev/cpu/0/msr (run with sudo)";
    }
    return "no integrated GPU RAPL counter or GPU temperature sensor found";
}

static int intel_gpu_init(int device_idx) {
    (void)device_idx;
    energy_units_valid = 0;
    int fd = open_msr(0);
    if (fd >= 0) {
        uint64_t result = read_msr(fd, MSR_RAPL_POWER_UNIT);
        close(fd);
        if (result != (uint64_t)-1) {
            energy_units = pow(0.5, (double)((result >> 8) & 0x1f));
            energy_units_valid = 1;
        }
    }
    return energy_units_valid || intel_gpu_read_hwmon_temperature() > 0.0f ? 0 : -1;
}

static int intel_gpu_sample_start(int device_idx) {
    (void)device_idx;
    pp1_before_valid = 0;
    int fd = open_msr(0);
    if (fd < 0) return -1;

    uint64_t result = read_msr(fd, MSR_PP1_ENERGY_STATUS);
    close(fd);

    if (result == (uint64_t)-1) return -1;
    pp1_before = (uint32_t)result;
    pp1_before_valid = 1;
    return 0;
}

static int intel_gpu_sample_stop(int device_idx) {
    (void)device_idx;
    pp1_after_valid = 0;
    int fd = open_msr(0);
    if (fd < 0) return -1;

    uint64_t result = read_msr(fd, MSR_PP1_ENERGY_STATUS);
    close(fd);

    if (result == (uint64_t)-1) return -1;
    pp1_after = (uint32_t)result;
    pp1_after_valid = 1;
    return 0;
}

static double intel_gpu_get_energy(int device_idx) {
    (void)device_idx;
    if (!igpu_supported || !energy_units_valid ||
        !pp1_before_valid || !pp1_after_valid) {
        return METRIC_NOT_SUPPORTED;
    }

    return (double)(uint32_t)(pp1_after - pp1_before) * energy_units;
}

static float intel_gpu_get_temperature(int device_idx) {
    (void)device_idx;
    return intel_gpu_read_hwmon_temperature();
}

static const EnergyMetricDescriptor intel_gpu_metrics[] = {
    {"GPU", "J", ENERGY_METRIC_WINDOW, ENERGY_METRIC_DEVICE, ENERGY_METRIC_FLAG_NONE},
    {"Temperature", "C", ENERGY_METRIC_GAUGE, ENERGY_METRIC_DEVICE,
     ENERGY_METRIC_FLAG_GPU_TEMPERATURE}
};

static double intel_gpu_get_metric(size_t metric_index, int device_idx,
                                   pid_t process_id) {
    (void)process_id;
    switch (metric_index) {
        case 0: return intel_gpu_get_energy(device_idx);
        case 1: return (double)intel_gpu_get_temperature(device_idx);
        default: return METRIC_NOT_SUPPORTED;
    }
}

static void intel_gpu_cleanup(void) {}

GpuBackend intel_gpu_backend = {
    .name = "Intel Integrated GPU",
    .detect = intel_gpu_detect,
    .init = intel_gpu_init,
    .cleanup = intel_gpu_cleanup,
    .sample_start = intel_gpu_sample_start,
    .sample_stop = intel_gpu_sample_stop,
    .metrics = intel_gpu_metrics,
    .metric_count = sizeof(intel_gpu_metrics) / sizeof(intel_gpu_metrics[0]),
    .get_metric = intel_gpu_get_metric,
    .diagnose = intel_gpu_diagnose
};

const EnergyBackendDescriptor energy_backend_descriptor = {
    .type = ENERGY_BACKEND_GPU,
    .priority = 0,
    .backend.gpu = &intel_gpu_backend
};
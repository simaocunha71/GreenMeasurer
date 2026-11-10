#include "intel.h"
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <math.h>
#include "../../../../include/energy-counter.h"

static double intel_gpu_metric_energy(int device, pid_t pid);
static double intel_gpu_metric_temperature(int device, pid_t pid);

static const EnergyMetricDefinition intel_gpu_metric_definitions[] = {
    {
        .descriptor = {.name = "GPU", .unit = "J", .kind = ENERGY_METRIC_WINDOW,
                       .scope = ENERGY_METRIC_DEVICE, .flags = ENERGY_METRIC_FLAG_NONE},
        .function = ENERGY_METRIC_FUNCTION(intel_gpu_metric_energy),
        .info = {.description = "Integrated GPU energy", .origin = "Intel RAPL PP1 MSR"}
    },
    {
        .descriptor = {.name = "Temperature", .unit = "C", .kind = ENERGY_METRIC_GAUGE,
                       .scope = ENERGY_METRIC_DEVICE, .flags = ENERGY_METRIC_FLAG_GPU_TEMPERATURE},
        .function = ENERGY_METRIC_FUNCTION(intel_gpu_metric_temperature),
        .info = {.description = "Integrated GPU temperature", .origin = "Linux GPU hwmon"}
    }
};
ENERGY_DEFINE_METRIC_TABLE(intel_gpu, intel_gpu_metric_definitions)

#define MSR_RAPL_POWER_UNIT   0x606
#define MSR_PP1_ENERGY_STATUS 0x641

static double energy_units = 0.0;
static int energy_units_valid = 0;
static uint32_t pp1_before = 0;
static uint32_t pp1_after = 0;
static int pp1_before_valid = 0;
static int pp1_after_valid = 0;
static int igpu_supported = 0;
static int next_sample_has_previous = 0;

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
    int has_intel_gpu = intel_gpu_hardware_present();
    int fd = open_msr(0);
    if (fd >= 0) {
        uint64_t pwr_unit = read_msr(fd, MSR_RAPL_POWER_UNIT);
        uint64_t pp1_val = read_msr(fd, MSR_PP1_ENERGY_STATUS);
        close(fd);

        if (pwr_unit != (uint64_t)-1 && pp1_val != (uint64_t)-1) {
            igpu_supported = 1;
        }
    }

    return igpu_supported || has_gpu_temperature || has_intel_gpu;
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
        if (result != (uint64_t)-1 && result != 0) {
            energy_units = pow(0.5, (double)((result >> 8) & 0x1f));
            energy_units_valid = 1;
        }
    }
    return energy_units_valid || intel_gpu_read_hwmon_temperature() > 0.0f ? 0 : -1;
}

static int intel_gpu_sample_start(int device_idx) {
    (void)device_idx;
    next_sample_has_previous = 0;
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
    // A counter that stays at 0 means the domain is absent or stubbed (e.g. in a VM).
    if (pp1_before == 0 && pp1_after == 0) pp1_before_valid = pp1_after_valid = 0;
    return 0;
}

static double intel_gpu_get_energy(int device_idx) {
    (void)device_idx;
    if (!igpu_supported || !energy_units_valid ||
        !pp1_before_valid || !pp1_after_valid) {
        return METRIC_NOT_SUPPORTED;
    }

    return (double)energy_counter_delta32(pp1_after, pp1_before) * energy_units;
}

int energy_backend_sample_next(int device_idx) {
    if (next_sample_has_previous) {
        pp1_before = pp1_after;
        pp1_before_valid = pp1_after_valid;
    }
    int result = intel_gpu_sample_stop(device_idx);
    next_sample_has_previous = result == 0;
    return result;
}

static float intel_gpu_get_temperature(int device_idx) {
    (void)device_idx;
    return intel_gpu_read_hwmon_temperature();
}

static double intel_gpu_metric_energy(int device, pid_t pid) {
    (void)pid;
    return intel_gpu_get_energy(device);
}
static double intel_gpu_metric_temperature(int device, pid_t pid) {
    (void)pid;
    return intel_gpu_get_temperature(device);
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
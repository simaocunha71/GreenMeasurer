/* TOY_GPU: a minimal, complete GPU backend used as a tutorial.
 *
 * It has no real GPU. It pretends the device draws a constant TOY_GPU_WATTS
 * and reports the resident memory of the measured process as "GPU memory", so
 * the numbers are not real measurements. It shows a process-scoped peak
 * metric, which the CPU example does not.
 *
 * Metrics:
 *   gpu.Energy_J    window  fake energy (power x elapsed time)
 *   gpu.Power_W     gauge   constant fake power
 *   gpu.Memory_MiB  peak    resident memory of the measured process tree
 */
#include <stdio.h>
#include <time.h>

#include "energy-backend.h"

static double toy_gpu_metric_energy(int device, pid_t pid);
static double toy_gpu_metric_power(int device, pid_t pid);
static double toy_gpu_metric_memory(int device, pid_t pid);

static const EnergyMetricDefinition toy_gpu_metric_definitions[] = {
    {
        .descriptor = {.name = "Energy", .unit = "J", .kind = ENERGY_METRIC_WINDOW,
                       .scope = ENERGY_METRIC_DEVICE, .flags = ENERGY_METRIC_FLAG_NONE},
        .function = ENERGY_METRIC_FUNCTION(toy_gpu_metric_energy),
        .info = {.description = "Simulated GPU energy", .origin = "Elapsed time multiplied by simulated 5 W"}
    },
    {
        .descriptor = {.name = "Power", .unit = "W", .kind = ENERGY_METRIC_GAUGE,
                       .scope = ENERGY_METRIC_DEVICE, .flags = ENERGY_METRIC_FLAG_NONE},
        .function = ENERGY_METRIC_FUNCTION(toy_gpu_metric_power),
        .info = {.description = "Simulated GPU power", .origin = "Constant simulated 5 W"}
    },
    {
        .descriptor = {.name = "Memory", .unit = "MiB", .kind = ENERGY_METRIC_PEAK,
                       .scope = ENERGY_METRIC_PROCESS, .flags = ENERGY_METRIC_FLAG_NONE},
        .function = ENERGY_METRIC_FUNCTION(toy_gpu_metric_memory),
        .info = {.description = "Simulated GPU memory from process RSS", .origin = "Linux /proc/<pid>/status VmRSS"}
    }
};
ENERGY_DEFINE_METRIC_TABLE(toy_gpu, toy_gpu_metric_definitions)

#define TOY_GPU_WATTS 5.0

static struct timespec window_start;
static struct timespec window_end;
static double window_seconds = 0.0;
static int window_valid = 0;

static double seconds_between(const struct timespec *a, const struct timespec *b) {
    return (double)(b->tv_sec - a->tv_sec) + (double)(b->tv_nsec - a->tv_nsec) / 1e9;
}

static int toy_gpu_detect(void) {
    struct timespec now;
    return clock_gettime(CLOCK_MONOTONIC, &now) == 0;
}

static const char *toy_gpu_diagnose(void) {
    return "CLOCK_MONOTONIC is not available";
}

static int toy_gpu_init(int device) {
    (void)device;
    window_valid = 0;
    return 0;
}

static void toy_gpu_cleanup(void) {}

static int toy_gpu_sample_start(int device) {
    (void)device;
    window_valid = 0;
    return clock_gettime(CLOCK_MONOTONIC, &window_start);
}

static int toy_gpu_sample_stop(int device) {
    (void)device;
    if (clock_gettime(CLOCK_MONOTONIC, &window_end) != 0) return -1;
    window_seconds = seconds_between(&window_start, &window_end);
    window_valid = 1;
    return 0;
}

int energy_backend_sample_next(int device) {
    int result = toy_gpu_sample_stop(device);
    if (result == 0) window_start = window_end;
    return result;
}

/* Resident memory of one process, in MiB (Linux only). */
static double process_memory_mib(pid_t process_id) {
    char path[64], line[256];
    snprintf(path, sizeof(path), "/proc/%d/status", (int)process_id);
    FILE *file = fopen(path, "r");
    if (!file) return METRIC_NOT_SUPPORTED;

    double result = METRIC_NOT_SUPPORTED;
    while (fgets(line, sizeof(line), file)) {
        long kib;
        if (sscanf(line, "VmRSS: %ld kB", &kib) == 1) {
            result = (double)kib / 1024.0;
            break;
        }
    }
    fclose(file);
    return result;
}

static double toy_gpu_metric_energy(int device, pid_t pid) {
    (void)device;
    (void)pid;
    return window_valid ? TOY_GPU_WATTS * window_seconds : METRIC_NOT_SUPPORTED;
}
static double toy_gpu_metric_power(int device, pid_t pid) {
    (void)device;
    (void)pid;
    return TOY_GPU_WATTS;
}
static double toy_gpu_metric_memory(int device, pid_t pid) {
    (void)device;
    return pid > 0 ? process_memory_mib(pid) : METRIC_NOT_SUPPORTED;
}
GpuBackend toy_gpu_backend = {
    .name = "TOY_GPU",
    .detect = toy_gpu_detect,
    .init = toy_gpu_init,
    .cleanup = toy_gpu_cleanup,
    .sample_start = toy_gpu_sample_start,
    .sample_stop = toy_gpu_sample_stop,
    .metrics = toy_gpu_metrics,
    .metric_count = sizeof(toy_gpu_metrics) / sizeof(toy_gpu_metrics[0]),
    .get_metric = toy_gpu_get_metric,
    .diagnose = toy_gpu_diagnose
};

const EnergyBackendDescriptor energy_backend_descriptor = {
    .type = ENERGY_BACKEND_GPU,
    .priority = 0,
    .backend.gpu = &toy_gpu_backend
};

/* TOY_CPU: a minimal, complete CPU backend used as a tutorial.
 *
 * It has no real energy counter. It pretends the CPU draws a constant
 * TOY_CPU_WATTS and integrates that over time, so the numbers are not real
 * measurements. What matters here is the structure every backend must have.
 *
 * Metrics:
 *   cpu.Energy_J    window  fake energy (power x elapsed time)
 *   cpu.LoadAvg_1m  gauge   1-minute load average (real, from getloadavg)
 */
#include <stdlib.h>
#include <time.h>

#include "energy-backend.h"

#define TOY_CPU_WATTS 10.0

static struct timespec window_start;
static struct timespec window_end;
static double window_seconds = 0.0;
static int window_valid = 0;

static double seconds_between(const struct timespec *a, const struct timespec *b) {
    return (double)(b->tv_sec - a->tv_sec) + (double)(b->tv_nsec - a->tv_nsec) / 1e9;
}

/* 1 = this backend can run on this machine, 0 = it cannot. */
static int toy_cpu_detect(void) {
    struct timespec now;
    return clock_gettime(CLOCK_MONOTONIC, &now) == 0;
}

/* Shown in the startup table when detect() returns 0. */
static const char *toy_cpu_diagnose(void) {
    return "CLOCK_MONOTONIC is not available";
}

static int toy_cpu_init(int core) {
    (void)core;
    window_valid = 0;
    return 0;
}

static void toy_cpu_cleanup(void) {}

/* Open a measurement window. Return 0 on success. */
static int toy_cpu_sample_start(int core) {
    (void)core;
    window_valid = 0;
    return clock_gettime(CLOCK_MONOTONIC, &window_start);
}

/* Close the window. A non-zero return makes the metrics NOT_SUPPORTED. */
static int toy_cpu_sample_stop(int core) {
    (void)core;
    if (clock_gettime(CLOCK_MONOTONIC, &window_end) != 0) return -1;
    window_seconds = seconds_between(&window_start, &window_end);
    window_valid = 1;
    return 0;
}

int energy_backend_sample_next(int core) {
    int result = toy_cpu_sample_stop(core);
    if (result == 0) window_start = window_end;
    return result;
}

static double toy_cpu_metric_energy(int core, pid_t pid) {
    (void)core;
    (void)pid;
    return window_valid ? TOY_CPU_WATTS * window_seconds : METRIC_NOT_SUPPORTED;
}

static double toy_cpu_metric_load(int core, pid_t pid) {
    (void)core;
    (void)pid;
    double load[1];
    return getloadavg(load, 1) == 1 ? load[0] : METRIC_NOT_SUPPORTED;
}

#define TOY_CPU_METRICS(X) \
    X("Energy", "J", ENERGY_METRIC_WINDOW, ENERGY_METRIC_DEVICE, ENERGY_METRIC_FLAG_NONE, toy_cpu_metric_energy, "Simulated CPU energy", "Elapsed time multiplied by simulated 10 W") \
    X("LoadAvg", "1m", ENERGY_METRIC_GAUGE, ENERGY_METRIC_DEVICE, ENERGY_METRIC_FLAG_NONE, toy_cpu_metric_load, "One-minute system load average", "getloadavg")
ENERGY_DEFINE_METRICS(toy_cpu, TOY_CPU_METRICS)

CpuBackend toy_cpu_backend = {
    .name = "TOY_CPU",
    .detect = toy_cpu_detect,
    .init = toy_cpu_init,
    .cleanup = toy_cpu_cleanup,
    .sample_start = toy_cpu_sample_start,
    .sample_stop = toy_cpu_sample_stop,
    .metrics = toy_cpu_metrics,
    .metric_count = sizeof(toy_cpu_metrics) / sizeof(toy_cpu_metrics[0]),
    .get_metric = toy_cpu_get_metric,
    .set_powercap = NULL,
    .destroy_powercap = NULL,
    .calibrate_powercap = NULL,
    .diagnose = toy_cpu_diagnose
};

/* The core looks this symbol up with dlsym. Highest priority wins among the
   detected backends of the same category. */
const EnergyBackendDescriptor energy_backend_descriptor = {
    .type = ENERGY_BACKEND_CPU,
    .priority = 0,
    .backend.cpu = &toy_cpu_backend
};

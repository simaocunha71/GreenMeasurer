#include "../../../../include/energy-backend.h"
#include "../../powermetrics.h"

static PmEnergy before, after;
static int before_valid = 0, after_valid = 0;

static int apple_cpu_detect(void) {
    return pm_unavailable_reason() == NULL;
}

static const char *apple_cpu_diagnose(void) {
    const char *reason = pm_unavailable_reason();
    return reason ? reason : "powermetrics is available";
}

static int apple_cpu_init(int core) {
    (void)core;
    return pm_start();
}

static void apple_cpu_cleanup(void) {
    pm_shutdown();
}

static int apple_cpu_sample_start(int core) {
    (void)core;
    before = pm_snapshot();
    before_valid = 1;
    after_valid = 0;
    return 0;
}

/* A window is valid once at least one full powermetrics sample has arrived. */
static int apple_cpu_sample_stop(int core) {
    (void)core;
    after = pm_snapshot();
    after_valid = after.samples > before.samples;
    if (!after_valid) pm_report_if_exited();
    return after_valid ? 0 : -1;
}

static const EnergyMetricDescriptor apple_cpu_metrics[] = {
    {"CPU", "J", ENERGY_METRIC_WINDOW, ENERGY_METRIC_DEVICE, ENERGY_METRIC_FLAG_NONE},
    {"ANE", "J", ENERGY_METRIC_WINDOW, ENERGY_METRIC_DEVICE, ENERGY_METRIC_FLAG_NONE}
};

static double apple_cpu_get_metric(size_t metric_index, int core, pid_t process_id) {
    (void)core;
    (void)process_id;
    if (!before_valid || !after_valid) return METRIC_NOT_SUPPORTED;
    switch (metric_index) {
        case 0: return after.cpu_j - before.cpu_j;
        case 1: return after.ane_j - before.ane_j;
        default: return METRIC_NOT_SUPPORTED;
    }
}

CpuBackend apple_cpu_backend = {
    .name = "Apple Silicon CPU (powermetrics)",
    .detect = apple_cpu_detect,
    .init = apple_cpu_init,
    .cleanup = apple_cpu_cleanup,
    .sample_start = apple_cpu_sample_start,
    .sample_stop = apple_cpu_sample_stop,
    .metrics = apple_cpu_metrics,
    .metric_count = sizeof(apple_cpu_metrics) / sizeof(apple_cpu_metrics[0]),
    .get_metric = apple_cpu_get_metric,
    .set_powercap = NULL,
    .destroy_powercap = NULL,
    .calibrate_powercap = NULL,
    .diagnose = apple_cpu_diagnose
};

const EnergyBackendDescriptor energy_backend_descriptor = {
    .type = ENERGY_BACKEND_CPU,
    .priority = 0,
    .backend.cpu = &apple_cpu_backend
};

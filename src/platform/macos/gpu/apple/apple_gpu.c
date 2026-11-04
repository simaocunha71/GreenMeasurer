#include "../../../../include/energy-backend.h"
#include "../../powermetrics.h"

static PmEnergy before, after;
static int before_valid = 0, after_valid = 0;

static int apple_gpu_detect(void) {
    return pm_unavailable_reason() == NULL;
}

static const char *apple_gpu_diagnose(void) {
    const char *reason = pm_unavailable_reason();
    return reason ? reason : "powermetrics is available";
}

static int apple_gpu_init(int device) {
    (void)device;
    return pm_start();
}

static void apple_gpu_cleanup(void) {
    pm_shutdown();
}

static int apple_gpu_sample_start(int device) {
    (void)device;
    before = pm_snapshot();
    before_valid = 1;
    after_valid = 0;
    return 0;
}

/* Samples are counted when the CPU power line is parsed, so a window is valid
   once at least one full powermetrics sample has arrived. */
static int apple_gpu_sample_stop(int device) {
    (void)device;
    after = pm_snapshot();
    after_valid = after.samples > before.samples;
    if (!after_valid) pm_report_if_exited();
    return after_valid ? 0 : -1;
}

static const EnergyMetricDescriptor apple_gpu_metrics[] = {
    {"GPU", "J", ENERGY_METRIC_WINDOW, ENERGY_METRIC_DEVICE, ENERGY_METRIC_FLAG_NONE}
};

static double apple_gpu_get_metric(size_t metric_index, int device, pid_t process_id) {
    (void)device;
    (void)process_id;
    if (metric_index != 0 || !before_valid || !after_valid) return METRIC_NOT_SUPPORTED;
    return after.gpu_j - before.gpu_j;
}

GpuBackend apple_gpu_backend = {
    .name = "Apple Silicon GPU (powermetrics)",
    .detect = apple_gpu_detect,
    .init = apple_gpu_init,
    .cleanup = apple_gpu_cleanup,
    .sample_start = apple_gpu_sample_start,
    .sample_stop = apple_gpu_sample_stop,
    .metrics = apple_gpu_metrics,
    .metric_count = sizeof(apple_gpu_metrics) / sizeof(apple_gpu_metrics[0]),
    .get_metric = apple_gpu_get_metric,
    .diagnose = apple_gpu_diagnose
};

const EnergyBackendDescriptor energy_backend_descriptor = {
    .type = ENERGY_BACKEND_GPU,
    .priority = 0,
    .backend.gpu = &apple_gpu_backend
};

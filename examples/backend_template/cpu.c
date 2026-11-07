#include "energy-backend.h"

static int backend_detect(void) { return 0; }
static int backend_init(int device) { (void)device; return -1; }
static void backend_cleanup(void) {}
static int backend_sample_start(int device) { (void)device; return -1; }
static int backend_sample_stop(int device) { (void)device; return -1; }

static double backend_energy(int device, pid_t pid) {
    (void)device;
    (void)pid;
    return METRIC_NOT_SUPPORTED;
}

#define BACKEND_METRICS(X) \
    X("Energy", "J", ENERGY_METRIC_WINDOW, ENERGY_METRIC_DEVICE, ENERGY_METRIC_FLAG_NONE, backend_energy, "Device energy", "Implement hardware counter source")
ENERGY_DEFINE_METRICS(backend, BACKEND_METRICS)

static CpuBackend backend = {
    .name = "CPU template",
    .detect = backend_detect,
    .init = backend_init,
    .cleanup = backend_cleanup,
    .sample_start = backend_sample_start,
    .sample_stop = backend_sample_stop,
    .metrics = backend_metrics,
    .metric_count = sizeof(backend_metrics) / sizeof(backend_metrics[0]),
    .get_metric = backend_get_metric
};

const EnergyBackendDescriptor energy_backend_descriptor = {
    .type = ENERGY_BACKEND_CPU,
    .priority = 0,
    .backend.cpu = &backend
};

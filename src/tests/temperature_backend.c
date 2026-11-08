#include "../include/energy-backend.h"

static int detect(void) { return 1; }
static int initialize(int core) { (void)core; return 0; }
static int sample(int core) { (void)core; return 0; }
static double temperature(size_t index, int core, pid_t pid) {
    (void)index;
    (void)core;
    (void)pid;
    return 40.0;
}

static const EnergyMetricDescriptor metrics[] = {
    {"Temperature", "C", ENERGY_METRIC_GAUGE, ENERGY_METRIC_DEVICE,
     ENERGY_METRIC_FLAG_CPU_TEMPERATURE}
};

static CpuBackend backend = {
    .name = "Test temperature",
    .detect = detect,
    .init = initialize,
    .sample_start = sample,
    .sample_stop = sample,
    .metrics = metrics,
    .metric_count = 1,
    .get_metric = temperature
};

const EnergyBackendDescriptor energy_backend_descriptor = {
    .type = ENERGY_BACKEND_CPU,
    .priority = -1000,
    .backend.cpu = &backend
};

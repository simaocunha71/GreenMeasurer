/* CPU test backend that records powercap operations in the file named by POWERCAP_LOG. */
#include <stdio.h>
#include <stdlib.h>
#include "../include/energy-backend.h"

static void record(const char *text, int watts) {
    const char *path = getenv("POWERCAP_LOG");
    FILE *file = path ? fopen(path, "a") : NULL;
    if (!file) return;
    fprintf(file, "%s %d\n", text, watts);
    fclose(file);
}

static int detect(void) { return 1; }
static int initialize(int core) { (void)core; return 0; }
static int sample(int core) { (void)core; return 0; }
static double read_metric(size_t index, int core, pid_t pid) {
    (void)core; (void)pid;
    if (index == 1 && getenv("POWERCAP_LOG_TEMPERATURE")) record("temperature", 0);
    return index == 0 ? 1.5 : 40.0;
}
static int set_powercap(int watts) { record("set", watts); return 0; }
static int destroy_powercap(void) { record("restore", 0); return 0; }

static const EnergyMetricDescriptor metrics[] = {
    {"Package", "J", ENERGY_METRIC_WINDOW, ENERGY_METRIC_DEVICE, ENERGY_METRIC_FLAG_NONE},
    {"Temperature", "C", ENERGY_METRIC_GAUGE, ENERGY_METRIC_DEVICE,
     ENERGY_METRIC_FLAG_CPU_TEMPERATURE}
};

static CpuBackend backend = {
    .name = "Powercap recorder",
    .detect = detect,
    .init = initialize,
    .sample_start = sample,
    .sample_stop = sample,
    .metrics = metrics,
    .metric_count = 2,
    .get_metric = read_metric,
    .set_powercap = set_powercap,
    .destroy_powercap = destroy_powercap
};

const EnergyBackendDescriptor energy_backend_descriptor = {
    .type = ENERGY_BACKEND_CPU, .priority = 100, .backend.cpu = &backend
};

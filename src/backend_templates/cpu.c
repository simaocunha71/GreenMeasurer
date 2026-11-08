#include "cpu.h"

static const EnergyMetricDefinition backend_metric_definitions[] = {
    {
        .descriptor = {.name = "Energy", .unit = "J", .kind = ENERGY_METRIC_WINDOW,
                       .scope = ENERGY_METRIC_DEVICE, .flags = ENERGY_METRIC_FLAG_NONE},
        .function = ENERGY_METRIC_FUNCTION(backend_energy),
        .info = {.description = "Device energy", .origin = "Implement hardware counter source"}
    },
    {
        .descriptor = {.name = "Temperature", .unit = "C", .kind = ENERGY_METRIC_GAUGE,
                       .scope = ENERGY_METRIC_DEVICE, .flags = ENERGY_METRIC_FLAG_CPU_TEMPERATURE},
        .function = ENERGY_METRIC_FUNCTION(backend_temperature),
        .info = {.description = "CPU temperature", .origin = "Implement hardware temperature source"}
    }
};
ENERGY_DEFINE_METRIC_TABLE(backend, backend_metric_definitions)

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

static double backend_temperature(int device, pid_t pid) {
    (void)device;
    (void)pid;
    return METRIC_NOT_SUPPORTED;
}

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

/* Configurable fake backend plugin used by run_tests.sh.
   Build-time knobs: FAKE_NAME (string), FAKE_PRIORITY, FAKE_DETECT (0/1),
   FAKE_GPU (define for a GPU plugin), FAKE_INVALID (define to omit callbacks). */
#include "../include/energy-backend.h"

static int fake_detect(void) { return FAKE_DETECT; }
static int fake_init(int id) { (void)id; return 0; }
static int fake_sample(int id) { (void)id; return 0; }
#ifndef FAKE_NO_NEXT
int energy_backend_sample_next(int id) { return fake_sample(id); }
#endif
static double fake_metric(size_t metric_index, int id, pid_t process_id) {
    (void)id;
    if (metric_index == 0) return 1.0;
#ifdef FAKE_GPU
    if (metric_index == 1 && process_id > 0) return 8.0;
#else
    if (metric_index == 1) return 42.0;
    if (metric_index == 2) return 8.5;
#endif
    return METRIC_NOT_SUPPORTED;
}
static const char *fake_diagnose(void) { return "fake backend is disabled"; }
#ifdef FAKE_POWER_CAP_FAILURE
static int fake_set_powercap(int watts) { (void)watts; return -1; }
static int fake_calibrate_powercap(int ntimes, int core, const char *command,
                                   const char *language, const char *program,
                                   const char *calib_csv_file, const char *process_script,
                                   int min_powercap, int max_powercap, int time_out_limit,
                                   float temp_threshold, int variance, int memory) {
    (void)ntimes; (void)core; (void)command; (void)language; (void)program;
    (void)calib_csv_file; (void)process_script; (void)min_powercap; (void)max_powercap;
    (void)time_out_limit; (void)temp_threshold; (void)variance; (void)memory;
    return -1;
}
#endif

#ifdef FAKE_GPU
static const EnergyMetricDescriptor fake_metrics[] = {
    {"GPU", "J", ENERGY_METRIC_WINDOW, ENERGY_METRIC_DEVICE, ENERGY_METRIC_FLAG_NONE},
    {"VRAM", "MiB", ENERGY_METRIC_PEAK, ENERGY_METRIC_PROCESS, ENERGY_METRIC_FLAG_NONE}
};
static GpuBackend backend = {
    .name = FAKE_NAME,
    .detect = fake_detect,
#ifndef FAKE_INVALID
    .init = fake_init,
    .sample_start = fake_sample,
    .sample_stop = fake_sample,
    .metrics = fake_metrics,
    .metric_count = sizeof(fake_metrics) / sizeof(fake_metrics[0]),
    .get_metric = fake_metric,
#endif
    .diagnose = fake_diagnose
};
const EnergyBackendDescriptor energy_backend_descriptor = {
    .type = ENERGY_BACKEND_GPU, .priority = FAKE_PRIORITY, .backend.gpu = &backend
};
#else
static const EnergyMetricDescriptor fake_metrics[] = {
    {"Package", "J", ENERGY_METRIC_WINDOW, ENERGY_METRIC_DEVICE, ENERGY_METRIC_FLAG_NONE},
    {"Temperature", "C", ENERGY_METRIC_GAUGE, ENERGY_METRIC_DEVICE,
     ENERGY_METRIC_FLAG_CPU_TEMPERATURE},
    {"BoardPower", "W", ENERGY_METRIC_GAUGE, ENERGY_METRIC_DEVICE,
     ENERGY_METRIC_FLAG_NONE}
};
static CpuBackend backend = {
    .name = FAKE_NAME,
    .detect = fake_detect,
    .init = fake_init,
    .sample_start = fake_sample,
    .sample_stop = fake_sample,
    .metrics = fake_metrics,
    .metric_count = sizeof(fake_metrics) / sizeof(fake_metrics[0]),
    .get_metric = fake_metric,
#ifdef FAKE_POWER_CAP_FAILURE
    .set_powercap = fake_set_powercap,
    .calibrate_powercap = fake_calibrate_powercap,
#endif
    .diagnose = fake_diagnose
};
const EnergyBackendDescriptor energy_backend_descriptor = {
    .type = ENERGY_BACKEND_CPU, .priority = FAKE_PRIORITY, .backend.cpu = &backend
};
#endif

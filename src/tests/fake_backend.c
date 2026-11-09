/* Configurable fake backend plugin used by run_tests.sh.
   Build-time knobs: FAKE_NAME (string), FAKE_PRIORITY, FAKE_DETECT (0/1),
   FAKE_GPU (define for a GPU plugin), FAKE_INVALID (define to omit callbacks). */
#include "../include/energy-backend.h"
#include <math.h>
#include <stdio.h>

static int fake_detect(void) { return FAKE_DETECT; }
static int fake_init(int id) {
    (void)id;
#ifdef FAKE_INIT_FAILURE
    return -1;
#else
    return 0;
#endif
}
static int fake_sample(int id) { (void)id; return 0; }
#ifdef FAKE_SAMPLE_FAILURE
static int fake_next(int id) { (void)id; return -1; }
#endif
#ifndef FAKE_NO_NEXT
int energy_backend_sample_next(int id) {
#ifdef FAKE_SAMPLE_FAILURE
    return fake_next(id);
#elif defined(FAKE_SAMPLE_FAIL_AFTER_ONE)
    static int samples;
    if (samples++ > 0) return -1;
    return fake_sample(id);
#else
    return fake_sample(id);
#endif
}
#endif
static double fake_metric(size_t metric_index, int id, pid_t process_id) {
    (void)id;
    (void)process_id;
    if (metric_index == 0) {
#ifdef FAKE_METRIC_UNAVAILABLE
        return METRIC_NOT_SUPPORTED;
#elif defined(FAKE_METRIC_NAN)
        return NAN;
#elif defined(FAKE_METRIC_FAIL_AFTER_TWO)
        static int reads;
        if (reads++ >= 2) return METRIC_NOT_SUPPORTED;
        return 1.0;
#else
        return 1.0;
#endif
    }
#ifdef FAKE_GPU
    if (metric_index == 1 && process_id > 0) {
#ifdef FAKE_PROCESS_UNAVAILABLE
        return METRIC_NOT_SUPPORTED;
#else
        return 8.0;
#endif
    }
#else
    if (metric_index == 1) {
#ifdef FAKE_TEMPERATURE_UNAVAILABLE
        return METRIC_NOT_SUPPORTED;
#elif defined(FAKE_TEMPERATURE_NAN)
        return NAN;
#elif defined(FAKE_TEMPERATURE_INCREASING)
        static int temperature_reads;
        return 40.0 + temperature_reads++;
#elif defined(FAKE_TEMPERATURE_FAIL_AFTER_ONE)
        static int temperature_reads;
        return temperature_reads++ == 0 ? 42.0 : METRIC_NOT_SUPPORTED;
#else
        return 42.0;
#endif
    }
    if (metric_index == 2) return 8.5;
#ifdef FAKE_DEVICE_PEAK
    if (metric_index == 3) {
        static double value = 10.0;
        return value--;
    }
#endif
#endif
    return METRIC_NOT_SUPPORTED;
}
static const char *fake_diagnose(void) { return "fake backend is disabled"; }
#if defined(FAKE_POWER_CAP_FAILURE) || defined(FAKE_POWER_CAP_SUCCESS)
static int fake_destroy_powercap(void) { return 0; }
static int fake_set_powercap(int watts) {
    (void)watts;
#ifdef FAKE_POWER_CAP_FAILURE
    return -1;
#else
    return 0;
#endif
}
static int fake_calibrate_powercap(int ntimes, int core, const char *command,
                                   const char *language, const char *program,
                                   const char *calib_csv_file, const char *process_script,
                                   int min_powercap, int max_powercap, int time_out_limit,
                                   float temp_threshold, int variance, int memory) {
    (void)core; (void)command; (void)language; (void)program;
    (void)process_script; (void)temp_threshold; (void)memory;
#ifdef FAKE_POWER_CAP_SUCCESS
    FILE *file = fopen(calib_csv_file, "w");
    if (!file) return -1;
    int failed = fprintf(file, "%d,%d,%d,%d,%d\n", ntimes, min_powercap,
                         max_powercap, time_out_limit, variance) < 0;
    if (fclose(file) != 0) failed = 1;
    return failed ? -1 : 0;
#else
    (void)ntimes; (void)calib_csv_file; (void)min_powercap; (void)max_powercap;
    (void)time_out_limit; (void)variance;
    return -1;
#endif
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
#ifdef FAKE_DEVICE_PEAK
    ,{"DevicePeak", "units", ENERGY_METRIC_PEAK, ENERGY_METRIC_DEVICE,
      ENERGY_METRIC_FLAG_NONE}
#endif
};
#ifdef FAKE_INVALID_FUNCTION_MAP
static const EnergyMetricFunction fake_functions[] = {
    {"Package", NULL}, {"Package", NULL}, {"Package", NULL}
};
const EnergyMetricFunctionMap energy_metric_function_map = {
    ENERGY_METRIC_FUNCTION_MAP_VERSION, 3, fake_functions
};
#endif
static CpuBackend backend = {
    .name = FAKE_NAME,
    .detect = fake_detect,
    .init = fake_init,
    .sample_start = fake_sample,
    .sample_stop = fake_sample,
    .metrics = fake_metrics,
    .metric_count = sizeof(fake_metrics) / sizeof(fake_metrics[0]),
    .get_metric = fake_metric,
#if defined(FAKE_POWER_CAP_FAILURE) || defined(FAKE_POWER_CAP_SUCCESS)
    .set_powercap = fake_set_powercap,
    .destroy_powercap = fake_destroy_powercap,
    .calibrate_powercap = fake_calibrate_powercap,
#endif
    .diagnose = fake_diagnose
};
const EnergyBackendDescriptor energy_backend_descriptor = {
    .type = ENERGY_BACKEND_CPU, .priority = FAKE_PRIORITY, .backend.cpu = &backend
};
#endif

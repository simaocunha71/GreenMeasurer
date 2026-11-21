/* Woven by scripts/weave.py from intel-cpu-temperature.toml; do not edit. */
#include <stdint.h>
#include <stddef.h>
#include "gm_aspect.h"
#include "intel_cpu_temperature.h"

extern pthread_mutex_t gm_reader_lock_1;

static GmSampler gm_IntelCPU_Temperature_sampler;
static pthread_t gm_IntelCPU_Temperature_thread;
static int gm_IntelCPU_Temperature_thread_running;
static unsigned char gm_IntelCPU_Temperature_selected[1];
static size_t gm_IntelCPU_Temperature_count;
static int gm_IntelCPU_Temperature_overflow;
static double gm_IntelCPU_Temperature_timestamps[GM_MAX_SAMPLES];
static double gm_IntelCPU_Temperature_samples_IntelCPU_Temperature[GM_MAX_SAMPLES];

static double gm_IntelCPU_Temperature_read_value_0(void) {
    pthread_mutex_lock(&gm_reader_lock_1);
    double value = intel_cpu_get_temperature();
    pthread_mutex_unlock(&gm_reader_lock_1);
    return value;
}
static double gm_IntelCPU_Temperature_read_0(void) { return (double)gm_IntelCPU_Temperature_read_value_0(); }
static double gm_IntelCPU_Temperature_sample_0(size_t row) { return (double)gm_IntelCPU_Temperature_samples_IntelCPU_Temperature[row]; }

static const GmMetric gm_IntelCPU_Temperature_metrics[] = {
    {"IntelCPU.Temperature", "C", GM_KIND_GAUGE, 0, gm_IntelCPU_Temperature_read_0, gm_IntelCPU_Temperature_sample_0},
};

/* One sample per interval; a last sample is taken once the stop is requested. */
static void *gm_IntelCPU_Temperature_run(void *unused) {
    (void)unused;
    for (;;) {
        int last = gm_sampler_stopping(&gm_IntelCPU_Temperature_sampler);
        if (gm_IntelCPU_Temperature_count >= GM_MAX_SAMPLES) {
            __atomic_store_n(&gm_IntelCPU_Temperature_overflow, 1, __ATOMIC_RELEASE);
            break;
        }
        gm_IntelCPU_Temperature_timestamps[gm_IntelCPU_Temperature_count] = gm_sampler_elapsed_ms(&gm_IntelCPU_Temperature_sampler);
        if (gm_IntelCPU_Temperature_selected[0]) gm_IntelCPU_Temperature_samples_IntelCPU_Temperature[gm_IntelCPU_Temperature_count] = gm_IntelCPU_Temperature_read_value_0();
        gm_IntelCPU_Temperature_count++;
        if (last) break;
        gm_sampler_wait(&gm_IntelCPU_Temperature_sampler);
    }
    return NULL;
}

static int gm_IntelCPU_Temperature_start(const unsigned char *selected, unsigned interval_ms,
                    const struct timespec *origin) {
    for (size_t i = 0; i < 1; i++) gm_IntelCPU_Temperature_selected[i] = selected[i];
    gm_IntelCPU_Temperature_count = 0;
    __atomic_store_n(&gm_IntelCPU_Temperature_overflow, 0, __ATOMIC_RELEASE);
    gm_sampler_init(&gm_IntelCPU_Temperature_sampler, interval_ms, origin);
    if (pthread_create(&gm_IntelCPU_Temperature_thread, NULL, gm_IntelCPU_Temperature_run, NULL) != 0) {
        gm_sampler_destroy(&gm_IntelCPU_Temperature_sampler);
        return -1;
    }
    gm_IntelCPU_Temperature_thread_running = 1;
    return 0;
}

static void gm_IntelCPU_Temperature_stop(void) {
    if (!gm_IntelCPU_Temperature_thread_running) return;
    gm_sampler_request_stop(&gm_IntelCPU_Temperature_sampler);
    pthread_join(gm_IntelCPU_Temperature_thread, NULL);
    gm_sampler_destroy(&gm_IntelCPU_Temperature_sampler);
    gm_IntelCPU_Temperature_thread_running = 0;
}

static size_t gm_IntelCPU_Temperature_get_count(void) { return gm_IntelCPU_Temperature_count; }
static double gm_IntelCPU_Temperature_get_timestamp(size_t row) { return gm_IntelCPU_Temperature_timestamps[row]; }
static int gm_IntelCPU_Temperature_get_overflow(void) { return __atomic_load_n(&gm_IntelCPU_Temperature_overflow, __ATOMIC_ACQUIRE); }

static int gm_IntelCPU_Temperature_setup(void) { return intel_cpu_temperature_begin() != 0; }

static void gm_IntelCPU_Temperature_teardown(void) { intel_cpu_temperature_end(); }

static double gm_IntelCPU_Temperature_thermal(void) { return gm_IntelCPU_Temperature_read_0(); }
static double gm_IntelCPU_Temperature_thermal_calibrate(unsigned seconds) { return (double)intel_cpu_calibrate_temperature(seconds); }

static const GmAspect gm_IntelCPU_Temperature_aspect = {
    "IntelCPU.Temperature", 1, gm_IntelCPU_Temperature_metrics,
    gm_IntelCPU_Temperature_setup,
    gm_IntelCPU_Temperature_teardown,
    "intel_cpu_temperature_begin",
    NULL, NULL,
    gm_IntelCPU_Temperature_start, gm_IntelCPU_Temperature_stop, gm_IntelCPU_Temperature_get_count, gm_IntelCPU_Temperature_get_timestamp, gm_IntelCPU_Temperature_get_overflow,
    gm_IntelCPU_Temperature_thermal, 0, gm_IntelCPU_Temperature_thermal_calibrate,
    NULL,
    NULL,
    NULL
};

const GmAspect *gm_aspect_IntelCPU_Temperature(void) { return &gm_IntelCPU_Temperature_aspect; }

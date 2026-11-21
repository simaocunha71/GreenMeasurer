/* Woven by scripts/weave.py from intel-cpu.toml; do not edit. */
#include <stdint.h>
#include <stddef.h>
#include "gm_aspect.h"
#include "intel_cpu.h"

extern pthread_mutex_t gm_reader_lock_2;

static GmSampler gm_IntelCPU_sampler;
static pthread_t gm_IntelCPU_thread;
static int gm_IntelCPU_thread_running;
static unsigned char gm_IntelCPU_selected[1];
static size_t gm_IntelCPU_count;
static int gm_IntelCPU_overflow;
static double gm_IntelCPU_timestamps[GM_MAX_SAMPLES];
static double gm_IntelCPU_samples_IntelCPU_Package[GM_MAX_SAMPLES];

static double gm_IntelCPU_read_value_0(void) {
    pthread_mutex_lock(&gm_reader_lock_2);
    double value = intel_cpu_get_package();
    pthread_mutex_unlock(&gm_reader_lock_2);
    return value;
}
static double gm_IntelCPU_read_0(void) { return (double)gm_IntelCPU_read_value_0(); }
static double gm_IntelCPU_sample_0(size_t row) { return (double)gm_IntelCPU_samples_IntelCPU_Package[row]; }

static const GmMetric gm_IntelCPU_metrics[] = {
    {"IntelCPU.Package", "J", GM_KIND_COUNTER, 0, gm_IntelCPU_read_0, gm_IntelCPU_sample_0},
};

/* One sample per interval; a last sample is taken once the stop is requested. */
static void *gm_IntelCPU_run(void *unused) {
    (void)unused;
    for (;;) {
        int last = gm_sampler_stopping(&gm_IntelCPU_sampler);
        if (gm_IntelCPU_count >= GM_MAX_SAMPLES) {
            __atomic_store_n(&gm_IntelCPU_overflow, 1, __ATOMIC_RELEASE);
            break;
        }
        gm_IntelCPU_timestamps[gm_IntelCPU_count] = gm_sampler_elapsed_ms(&gm_IntelCPU_sampler);
        if (gm_IntelCPU_selected[0]) gm_IntelCPU_samples_IntelCPU_Package[gm_IntelCPU_count] = gm_IntelCPU_read_value_0();
        gm_IntelCPU_count++;
        if (last) break;
        gm_sampler_wait(&gm_IntelCPU_sampler);
    }
    return NULL;
}

static int gm_IntelCPU_start(const unsigned char *selected, unsigned interval_ms,
                    const struct timespec *origin) {
    for (size_t i = 0; i < 1; i++) gm_IntelCPU_selected[i] = selected[i];
    gm_IntelCPU_count = 0;
    __atomic_store_n(&gm_IntelCPU_overflow, 0, __ATOMIC_RELEASE);
    gm_sampler_init(&gm_IntelCPU_sampler, interval_ms, origin);
    if (pthread_create(&gm_IntelCPU_thread, NULL, gm_IntelCPU_run, NULL) != 0) {
        gm_sampler_destroy(&gm_IntelCPU_sampler);
        return -1;
    }
    gm_IntelCPU_thread_running = 1;
    return 0;
}

static void gm_IntelCPU_stop(void) {
    if (!gm_IntelCPU_thread_running) return;
    gm_sampler_request_stop(&gm_IntelCPU_sampler);
    pthread_join(gm_IntelCPU_thread, NULL);
    gm_sampler_destroy(&gm_IntelCPU_sampler);
    gm_IntelCPU_thread_running = 0;
}

static size_t gm_IntelCPU_get_count(void) { return gm_IntelCPU_count; }
static double gm_IntelCPU_get_timestamp(size_t row) { return gm_IntelCPU_timestamps[row]; }
static int gm_IntelCPU_get_overflow(void) { return __atomic_load_n(&gm_IntelCPU_overflow, __ATOMIC_ACQUIRE); }

static int gm_IntelCPU_setup(void) { return intel_cpu_begin() != 0; }

static void gm_IntelCPU_teardown(void) { intel_cpu_end(); }

static const GmAspect gm_IntelCPU_aspect = {
    "IntelCPU", 1, gm_IntelCPU_metrics,
    gm_IntelCPU_setup,
    gm_IntelCPU_teardown,
    "intel_cpu_begin",
    NULL, NULL,
    gm_IntelCPU_start, gm_IntelCPU_stop, gm_IntelCPU_get_count, gm_IntelCPU_get_timestamp, gm_IntelCPU_get_overflow,
    NULL, -1, NULL,
    NULL,
    NULL,
    NULL
};

const GmAspect *gm_aspect_IntelCPU(void) { return &gm_IntelCPU_aspect; }

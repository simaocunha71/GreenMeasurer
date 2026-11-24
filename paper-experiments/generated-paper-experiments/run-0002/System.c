/* Woven by scripts/weave.py from system.toml; do not edit. */
#include <stdint.h>
#include <stddef.h>
#include "gm_aspect.h"
#include "system.h"

extern pthread_mutex_t gm_reader_lock_3;

static GmSampler gm_System_sampler;
static pthread_t gm_System_thread;
static int gm_System_thread_running;
static unsigned char gm_System_selected[1];
static size_t gm_System_count;
static int gm_System_overflow;
static double gm_System_timestamps[GM_MAX_SAMPLES];
static double gm_System_samples_System_RunningProcesses[GM_MAX_SAMPLES];

static double gm_System_read_value_0(void) {
    pthread_mutex_lock(&gm_reader_lock_3);
    double value = system_get_running_processes();
    pthread_mutex_unlock(&gm_reader_lock_3);
    return value;
}
static double gm_System_read_0(void) { return (double)gm_System_read_value_0(); }
static double gm_System_sample_0(size_t row) { return (double)gm_System_samples_System_RunningProcesses[row]; }

static const GmMetric gm_System_metrics[] = {
    {"System.RunningProcesses", "processes", GM_KIND_GAUGE, 0, gm_System_read_0, gm_System_sample_0},
};

/* One sample per interval; a last sample is taken once the stop is requested. */
static void *gm_System_run(void *unused) {
    (void)unused;
    for (;;) {
        int last = gm_sampler_stopping(&gm_System_sampler);
        if (gm_System_count >= GM_MAX_SAMPLES) {
            __atomic_store_n(&gm_System_overflow, 1, __ATOMIC_RELEASE);
            break;
        }
        gm_System_timestamps[gm_System_count] = gm_sampler_elapsed_ms(&gm_System_sampler);
        if (gm_System_selected[0]) gm_System_samples_System_RunningProcesses[gm_System_count] = gm_System_read_value_0();
        gm_System_count++;
        if (last) break;
        gm_sampler_wait(&gm_System_sampler);
    }
    return NULL;
}

static int gm_System_start(const unsigned char *selected, unsigned interval_ms,
                    const struct timespec *origin) {
    for (size_t i = 0; i < 1; i++) gm_System_selected[i] = selected[i];
    gm_System_count = 0;
    __atomic_store_n(&gm_System_overflow, 0, __ATOMIC_RELEASE);
    gm_sampler_init(&gm_System_sampler, interval_ms, origin);
    if (pthread_create(&gm_System_thread, NULL, gm_System_run, NULL) != 0) {
        gm_sampler_destroy(&gm_System_sampler);
        return -1;
    }
    gm_System_thread_running = 1;
    return 0;
}

static void gm_System_stop(void) {
    if (!gm_System_thread_running) return;
    gm_sampler_request_stop(&gm_System_sampler);
    pthread_join(gm_System_thread, NULL);
    gm_sampler_destroy(&gm_System_sampler);
    gm_System_thread_running = 0;
}

static size_t gm_System_get_count(void) { return gm_System_count; }
static double gm_System_get_timestamp(size_t row) { return gm_System_timestamps[row]; }
static int gm_System_get_overflow(void) { return __atomic_load_n(&gm_System_overflow, __ATOMIC_ACQUIRE); }

static const GmAspect gm_System_aspect = {
    "System", 1, gm_System_metrics,
    NULL,
    NULL,
    NULL,
    NULL, NULL,
    gm_System_start, gm_System_stop, gm_System_get_count, gm_System_get_timestamp, gm_System_get_overflow,
    NULL, -1, NULL,
    NULL,
    NULL,
    NULL
};

const GmAspect *gm_aspect_System(void) { return &gm_System_aspect; }

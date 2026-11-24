/* Woven by scripts/weave.py from time.toml; do not edit. */
#include <stdint.h>
#include <stddef.h>
#include "gm_aspect.h"
#include "wall_clock_time.h"

extern pthread_mutex_t gm_reader_lock_4;

static GmSampler gm_Time_sampler;
static pthread_t gm_Time_thread;
static int gm_Time_thread_running;
static unsigned char gm_Time_selected[1];
static size_t gm_Time_count;
static int gm_Time_overflow;
static double gm_Time_timestamps[GM_MAX_SAMPLES];
static double gm_Time_samples_Time_WallClock[GM_MAX_SAMPLES];

static double gm_Time_read_value_0(void) {
    pthread_mutex_lock(&gm_reader_lock_4);
    double value = gm_wall_clock_time_read();
    pthread_mutex_unlock(&gm_reader_lock_4);
    return value;
}
static double gm_Time_read_0(void) { return (double)gm_Time_read_value_0(); }
static double gm_Time_sample_0(size_t row) { return (double)gm_Time_samples_Time_WallClock[row]; }

static const GmMetric gm_Time_metrics[] = {
    {"Time.WallClock", "ms", GM_KIND_DURATION, 0, gm_Time_read_0, gm_Time_sample_0},
};

/* One sample per interval; a last sample is taken once the stop is requested. */
static void *gm_Time_run(void *unused) {
    (void)unused;
    for (;;) {
        int last = gm_sampler_stopping(&gm_Time_sampler);
        if (gm_Time_count >= GM_MAX_SAMPLES) {
            __atomic_store_n(&gm_Time_overflow, 1, __ATOMIC_RELEASE);
            break;
        }
        gm_Time_timestamps[gm_Time_count] = gm_sampler_elapsed_ms(&gm_Time_sampler);
        if (gm_Time_selected[0]) gm_Time_samples_Time_WallClock[gm_Time_count] = gm_Time_read_value_0();
        gm_Time_count++;
        if (last) break;
        gm_sampler_wait(&gm_Time_sampler);
    }
    return NULL;
}

static int gm_Time_start(const unsigned char *selected, unsigned interval_ms,
                    const struct timespec *origin) {
    for (size_t i = 0; i < 1; i++) gm_Time_selected[i] = selected[i];
    gm_Time_count = 0;
    __atomic_store_n(&gm_Time_overflow, 0, __ATOMIC_RELEASE);
    gm_sampler_init(&gm_Time_sampler, interval_ms, origin);
    if (pthread_create(&gm_Time_thread, NULL, gm_Time_run, NULL) != 0) {
        gm_sampler_destroy(&gm_Time_sampler);
        return -1;
    }
    gm_Time_thread_running = 1;
    return 0;
}

static void gm_Time_stop(void) {
    if (!gm_Time_thread_running) return;
    gm_sampler_request_stop(&gm_Time_sampler);
    pthread_join(gm_Time_thread, NULL);
    gm_sampler_destroy(&gm_Time_sampler);
    gm_Time_thread_running = 0;
}

static size_t gm_Time_get_count(void) { return gm_Time_count; }
static double gm_Time_get_timestamp(size_t row) { return gm_Time_timestamps[row]; }
static int gm_Time_get_overflow(void) { return __atomic_load_n(&gm_Time_overflow, __ATOMIC_ACQUIRE); }

static int gm_Time_measurement_start(void) {
    pthread_mutex_lock(&gm_reader_lock_4);
    int result = gm_wall_clock_time_start();
    pthread_mutex_unlock(&gm_reader_lock_4);
    return result;
}

static int gm_Time_measurement_stop(void) {
    pthread_mutex_lock(&gm_reader_lock_4);
    int result = gm_wall_clock_time_stop();
    pthread_mutex_unlock(&gm_reader_lock_4);
    return result;
}

static const GmAspect gm_Time_aspect = {
    "Time", 1, gm_Time_metrics,
    NULL,
    NULL,
    NULL,
    gm_Time_measurement_start, gm_Time_measurement_stop,
    gm_Time_start, gm_Time_stop, gm_Time_get_count, gm_Time_get_timestamp, gm_Time_get_overflow,
    NULL, -1, NULL,
    NULL,
    NULL,
    NULL
};

const GmAspect *gm_aspect_Time(void) { return &gm_Time_aspect; }

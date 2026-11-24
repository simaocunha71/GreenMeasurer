#include "wall_clock_time.h"

#include <time.h>

static struct timespec origin;
static double elapsed;
static int running;

static double elapsed_ms_since(const struct timespec *start) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return -1.0;
    return (double)(now.tv_sec - start->tv_sec) * 1000.0 +
           (double)(now.tv_nsec - start->tv_nsec) / 1e6;
}

int gm_wall_clock_time_start(void) {
    if (clock_gettime(CLOCK_MONOTONIC, &origin) != 0) return -1;
    elapsed = 0.0;
    running = 1;
    return 0;
}

int gm_wall_clock_time_stop(void) {
    elapsed = elapsed_ms_since(&origin);
    running = 0;
    return elapsed < 0.0 ? -1 : 0;
}

double gm_wall_clock_time_read(void) {
    return running ? elapsed_ms_since(&origin) : elapsed;
}

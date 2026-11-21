#include "toy.h"

#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>

static unsigned long energy;
static int calls;

static void log_event(const char *event) {
    const char *path = getenv("TOY_LOG");
    FILE *fp = path ? fopen(path, "a") : NULL;
    if (!fp) return;
    fprintf(fp, "%s\n", event);
    fclose(fp);
}

int toy_setup(void) {
    log_event("setup");
    return getenv("TOY_FAIL_SETUP") != NULL;
}

void toy_teardown(void) { log_event("teardown"); }

unsigned long toy_energy(void) { return energy += 10; }

float toy_temperature(void) {
    if (getenv("TOY_FAIL_TEMPERATURE") || (getenv("TOY_FAIL_TEMPERATURE_AFTER_FIRST") && calls > 0))
        return -1.0f;
    return 40.5f + (float)(calls++ % 3);
}

int toy_processes(void) { return 3; }

double toy_thermal(void) {
    static int cpu_calls;
    if (getenv("TOY_CPU_COOLS") && cpu_calls++ < 2) return 42.0;
    return 40.0;
}

double toy_calibrate_thermal(unsigned seconds) {
    if (seconds) sleep(seconds);
    return toy_thermal();
}

float toy_calibrate_temperature(unsigned seconds) {
    if (seconds) sleep(seconds);
    return toy_temperature();
}

int toy_cap_set(int watts) {
    char event[32];
    snprintf(event, sizeof(event), "set:%d", watts);
    log_event(event);
    return getenv("TOY_FAIL_CAP_SET") ? -1 : 0;
}

int toy_cap_restore(void) {
    log_event("restore");
    return getenv("TOY_FAIL_CAP_RESTORE") ? -1 : 0;
}

/* Same contract as intel_cpu_calibrate_powercap: -1, then min..max, through the engine's runTesting. */
extern int runTesting(int, int, const char *, const char *, const char *, int, int,
                      float, int, int);

int toy_calibrate(int ntimes, int core, const char *command,
                  const char *program, const char *csv, const char *script, int min_powercap,
                  int max_powercap, int timeout, float temperature, int variance, int memory) {
    (void)script;
    if (getenv("TOY_LOG_TIMEOUT")) {
        char event[32];
        snprintf(event, sizeof(event), "timeout:%d", timeout);
        log_event(event);
    }
    for (int cap = -1; cap <= max_powercap; cap = cap == -1 ? min_powercap : cap + 1) {
        toy_cap_set(cap);
        int result = runTesting(ntimes, core, command, program, csv, cap, timeout,
                                temperature, variance, memory);
        toy_cap_restore();
        if (result != 0) return -1;
    }
    return 0;
}

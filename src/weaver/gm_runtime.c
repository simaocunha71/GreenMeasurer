/* Runtime of the generated GreenMeasurer: runs the workload, drives the generated aspect
   threads and writes the CSV. It knows nothing about any concrete metric. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "gm_aspect.h"

#include <errno.h>
#include <limits.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define GM_UNAVAILABLE "ERROR"
#define GM_MAX_DEVICES 64
#define GM_MAX_METRICS_PER_DEVICE 64

/* ---------------------------------------------------------------- sampler */

void gm_sampler_init(GmSampler *sampler, unsigned interval_ms, const struct timespec *origin) {
    pthread_condattr_t attr;
    pthread_mutex_init(&sampler->lock, NULL);
    pthread_condattr_init(&attr);
    pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
    pthread_cond_init(&sampler->wake, &attr);
    pthread_condattr_destroy(&attr);
    sampler->stop = 0;
    sampler->interval_ms = interval_ms;
    sampler->origin = *origin;
}

void gm_sampler_destroy(GmSampler *sampler) {
    pthread_cond_destroy(&sampler->wake);
    pthread_mutex_destroy(&sampler->lock);
}

void gm_sampler_request_stop(GmSampler *sampler) {
    pthread_mutex_lock(&sampler->lock);
    sampler->stop = 1;
    pthread_cond_broadcast(&sampler->wake);
    pthread_mutex_unlock(&sampler->lock);
}

int gm_sampler_stopping(GmSampler *sampler) {
    pthread_mutex_lock(&sampler->lock);
    int stop = sampler->stop;
    pthread_mutex_unlock(&sampler->lock);
    return stop;
}

static double elapsed_ms_since(const struct timespec *origin) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (double)(now.tv_sec - origin->tv_sec) * 1000.0 +
           (double)(now.tv_nsec - origin->tv_nsec) / 1e6;
}

double gm_sampler_elapsed_ms(const GmSampler *sampler) {
    return elapsed_ms_since(&sampler->origin);
}

void gm_sampler_wait(GmSampler *sampler) {
    struct timespec deadline;
    clock_gettime(CLOCK_MONOTONIC, &deadline);
    deadline.tv_sec += sampler->interval_ms / 1000;
    deadline.tv_nsec += (long)(sampler->interval_ms % 1000) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec++;
        deadline.tv_nsec -= 1000000000L;
    }
    pthread_mutex_lock(&sampler->lock);
    while (!sampler->stop &&
           pthread_cond_timedwait(&sampler->wake, &sampler->lock, &deadline) != ETIMEDOUT) {
    }
    pthread_mutex_unlock(&sampler->lock);
}

/* ---------------------------------------------------------------- run state */

volatile pid_t gm_workload_pid;

static unsigned char selected_mask[GM_MAX_DEVICES][GM_MAX_METRICS_PER_DEVICE];
static unsigned char device_active[GM_MAX_DEVICES];
static unsigned char powercap_selected[GM_MAX_DEVICES];
static unsigned char setup_done[GM_MAX_DEVICES];
static unsigned char hooks_disabled[GM_MAX_DEVICES];
static unsigned char device_unavailable[GM_MAX_DEVICES];
static unsigned char sampler_started[GM_MAX_DEVICES];
static unsigned char thermal_control[GM_MAX_DEVICES];
static unsigned char thermal_disabled[GM_MAX_DEVICES];
static int temperature_unavailable;
static double thermal_baselines[GM_MAX_DEVICES];

/* State shared by the measurement helpers, set once by gm_main. */
static const GmAspect *ctx_devices;
static size_t ctx_count;
static const sigset_t *ctx_mask;
static const GmConfig *ctx_config;
static unsigned ctx_timeout_s; /* 0: no timeout */
static int powercap_dev = -1, calibration_dev = -1;
static int temperature_calibration, powercap_calibration;
static volatile sig_atomic_t terminate_requested;

static void on_sigint(int signum) {
    (void)signum;
    terminate_requested = 1;
}

static int setup_devices(const GmAspect *aspects, size_t count) {
    for (size_t d = 0; d < count; d++) {
        if (!device_active[d] && !thermal_control[d] && !powercap_selected[d]) continue;
        int failed = 0;
        if (aspects[d].lifecycle) {
            size_t previous = 0;
            while (previous < d && (!aspects[previous].lifecycle ||
                   strcmp(aspects[previous].lifecycle, aspects[d].lifecycle) ||
                   (!setup_done[previous] && !hooks_disabled[previous]))) previous++;
            if (previous < d) {
                if (!hooks_disabled[previous]) continue;
                failed = 1;
            }
        }
        if (!failed && aspects[d].setup && aspects[d].setup() != 0) failed = 1;
        if (failed) {
            if ((temperature_calibration && device_active[d] && aspects[d].thermal) ||
                (powercap_calibration && powercap_selected[d])) {
                fprintf(stderr, "[ERROR] Setup of aspect '%s' failed; calibration cannot proceed.\n",
                        aspects[d].name);
                goto failure;
            }
            fprintf(stderr, "[WARNING] Setup of aspect '%s' failed; continuing without it.\n",
                    aspects[d].name);
            device_unavailable[d] = hooks_disabled[d] = 1;
            device_active[d] = powercap_selected[d] = 0;
            continue;
        }
        setup_done[d] = 1;
    }
    powercap_dev = calibration_dev = -1;
    for (size_t d = 0; d < count; d++) {
        if (hooks_disabled[d]) continue;
        if (powercap_selected[d] && powercap_dev < 0 && aspects[d].powercap_set && aspects[d].powercap_restore)
            powercap_dev = (int)d;
        if (powercap_selected[d] && calibration_dev < 0 && aspects[d].powercap_calibrate) calibration_dev = (int)d;
    }
    return 0;
failure:
    for (size_t u = count; u-- > 0;)
        if (setup_done[u] && aspects[u].teardown) aspects[u].teardown();
    memset(setup_done, 0, sizeof(setup_done));
    return -1;
}

static void teardown_devices(const GmAspect *aspects, size_t count) {
    for (size_t d = count; d-- > 0;)
        if (setup_done[d] && aspects[d].teardown) aspects[d].teardown();
    memset(setup_done, 0, sizeof(setup_done));
    powercap_dev = calibration_dev = -1;
}

static int start_samplers(const GmAspect *aspects, size_t count, unsigned interval_ms,
                          const struct timespec *origin) {
    for (size_t d = 0; d < count; d++) {
        if (!device_active[d] || device_unavailable[d]) continue;
        if (aspects[d].start(selected_mask[d], interval_ms, origin) != 0) {
            fprintf(stderr, "[ERROR] Failed to start the sampling thread of '%s'.\n",
                    aspects[d].name);
            for (size_t u = 0; u < d; u++)
                if (sampler_started[u]) aspects[u].stop();
            memset(sampler_started, 0, sizeof(sampler_started));
            return -1;
        }
        sampler_started[d] = 1;
    }
    return 0;
}

static void stop_samplers(const GmAspect *aspects, size_t count) {
    for (size_t d = 0; d < count; d++)
        if (sampler_started[d]) aspects[d].stop();
    memset(sampler_started, 0, sizeof(sampler_started));
}

static int finish_measurement(unsigned char *started) {
    int result = 0;
    for (size_t d = ctx_count; d-- > 0;) {
        if (!started[d]) continue;
        if (ctx_devices[d].measurement_stop() != 0) {
            fprintf(stderr, "[ERROR] Failed to stop measurement for aspect '%s'.\n", ctx_devices[d].name);
            result = -1;
        }
        started[d] = 0;
    }
    return result;
}

static int begin_measurement(unsigned char *started) {
    for (size_t d = 0; d < ctx_count; d++) {
        if (!device_active[d] || device_unavailable[d] || !ctx_devices[d].measurement_start) continue;
        if (ctx_devices[d].measurement_start() != 0) {
            fprintf(stderr, "[ERROR] Failed to start measurement for aspect '%s'.\n", ctx_devices[d].name);
            finish_measurement(started);
            return -1;
        }
        started[d] = 1;
    }
    return 0;
}

static pid_t start_workload(const char *command, const sigset_t *old_mask) {
    pid_t pid = fork();
    if (pid == 0) {
        sigprocmask(SIG_SETMASK, old_mask, NULL);
        execl("/bin/sh", "sh", "-c", command, (char *)NULL);
        _exit(127);
    }
    gm_workload_pid = pid > 0 ? pid : 0;
    return pid;
}

/* Waits for the workload; aborts it when an aspect runs out of static sample capacity or the
   timeout expires. Returns the exit status, or -1 on failure. */
static int wait_workload(pid_t pid, const GmAspect *aspects, size_t count, unsigned timeout_s) {
    sigset_t chld;
    sigemptyset(&chld);
    sigaddset(&chld, SIGCHLD);
    struct timespec begin;
    clock_gettime(CLOCK_MONOTONIC, &begin);
    for (;;) {
        struct timespec timeout = {0, 50 * 1000000L};
        sigtimedwait(&chld, NULL, &timeout);
        int status;
        pid_t done = waitpid(pid, &status, WNOHANG);
        if (done == pid) return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
        if (done < 0 && errno != EINTR) return -1;
        if (timeout_s && elapsed_ms_since(&begin) > (double)timeout_s * 1000.0) {
            fprintf(stderr, "[WARNING] Timeout: execution exceeded %u seconds.\n", timeout_s);
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
            return -1;
        }
        for (size_t d = 0; d < count; d++) {
            if (!sampler_started[d] || !aspects[d].overflowed()) continue;
            fprintf(stderr, "[ERROR] Sample capacity (%d) of aspect '%s' exceeded; "
                    "measurement aborted.\n", GM_MAX_SAMPLES, aspects[d].name);
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
            return -1;
        }
    }
}

/* ---------------------------------------------------------------- thermal control */

static int read_temperature(size_t aspect, double *temperature) {
    double value = ctx_devices[aspect].thermal();
    if (!isfinite(value) || value <= 0.0) return -1;
    *temperature = value;
    return 0;
}

static void disable_temperature(size_t aspect) {
    fprintf(stderr, "[WARNING] Temperature aspect '%s' is unavailable; thermal control for it "
            "is disabled.\n", ctx_devices[aspect].name);
    thermal_disabled[aspect] = 1;
    temperature_unavailable = 1;
}

static double read_baseline_file(const char *path) {
    FILE *file = fopen(path, "r");
    if (!file) {
        perror("[ERROR] Failed to open temperature calibration file");
        return -1.0;
    }
    char line[256];
    double result = -1.0;
    while (fgets(line, sizeof(line), file)) {
        if (!strstr(line, "Average temperature:")) continue;
        char *colon = strchr(line, ':');
        char *comma = colon ? strchr(colon, ',') : NULL;
        if (comma) *comma = '.';
        double value;
        if (colon && sscanf(colon + 1, "%lf", &value) == 1) {
            result = value;
            break;
        }
    }
    fclose(file);
    return result;
}

static int same_calibration_file(const char *a, const char *b) {
    struct stat first, second;
    return !strcmp(a, b) ||
           (stat(a, &first) == 0 && stat(b, &second) == 0 &&
            first.st_dev == second.st_dev && first.st_ino == second.st_ino);
}

/* Asks the device library to stabilize and read the sensor; stores the result in the
   selected aspect's calibration file. */
static int calibrate_temperature(size_t aspect, const char *path, unsigned sleep_seconds) {
    printf("[INFO] Sleeping %u seconds to stabilize and calibrate %s...\n",
           sleep_seconds, ctx_devices[aspect].name);
    double mean = ctx_devices[aspect].thermal_calibrate(sleep_seconds);
    if (!isfinite(mean) || mean <= 0.0) return 1;
    FILE *snapshot = fopen(path, "w");
    if (!snapshot) {
        perror("[ERROR] Failed to open calibration file");
        return -1;
    }
    int failed = fprintf(snapshot, "Average temperature: %.1f\xC2\xB0" "C\n\n", mean) < 0;
    if (fclose(snapshot) != 0 || failed) {
        perror("[ERROR] Failed to write calibration file");
        return -1;
    }
    printf("[INFO] Calibrated baseline temperature saved to %s: %.1f\xC2\xB0" "C\n", path, mean);
    return 0;
}

/* Baseline from the calibration file, calibrating first when the file does not exist. */
static int resolve_baseline(size_t aspect, const char *path, unsigned sleep_seconds, double *baseline) {
    *baseline = NAN;
    if (access(path, F_OK) != 0) {
        if (errno != ENOENT) {
            perror("[ERROR] Cannot access temperature calibration file");
            return -1;
        }
        int status = calibrate_temperature(aspect, path, sleep_seconds);
        if (status != 0) return status;
    }
    double value = read_baseline_file(path);
    if (!isfinite(value) || value <= 0.0) {
        fprintf(stderr, "[ERROR] Temperature calibration for '%s' is invalid.\n",
                ctx_devices[aspect].name);
        return -1;
    }
    *baseline = value;
    return 0;
}

static int wait_for_temperature(unsigned variance) {
    while (!terminate_requested) {
        int ready = 1;
        for (size_t d = 0; d < ctx_count; d++) {
            if (!thermal_control[d] || thermal_disabled[d]) continue;
            double temperature;
            if (read_temperature(d, &temperature) != 0) {
                disable_temperature(d);
                continue;
            }
            if (temperature > thermal_baselines[d] + variance) {
                printf("[INFO] Waiting for temperature %s: %.1f C; baseline %.1f C; variance %u C.\n",
                       ctx_devices[d].name, temperature, thermal_baselines[d], variance);
                ready = 0;
            }
        }
        if (ready) return 0;
        sleep(1);
    }
    if (terminate_requested) {
        fprintf(stderr, "[ERROR] Measurement interrupted while waiting for temperature.\n");
        return -1;
    }
    return 0;
}

/* ---------------------------------------------------------------- powercap */

static int apply_powercap(int powercap) {
    if (powercap_dev < 0) {
        if (powercap > 0) {
            fprintf(stderr, "[ERROR] No selected aspect declares powercap_set/powercap_restore; "
                            "cannot apply %d W.\n", powercap);
            return -1;
        }
        return 0;
    }
    if (ctx_devices[powercap_dev].powercap_set(powercap) != 0) {
        fprintf(stderr, "[ERROR] Failed to apply powercap state for %d W; measurement aborted.\n",
                powercap);
        if (ctx_devices[powercap_dev].powercap_restore() != 0)
            fprintf(stderr, "[ERROR] Failed to restore powercap after setup failure.\n");
        return -1;
    }
    return 0;
}

static int release_powercap(void) {
    if (powercap_dev < 0) return 0;
    if (ctx_devices[powercap_dev].powercap_restore() != 0) {
        fprintf(stderr, "[ERROR] Failed to restore the RAPL powercap state.\n");
        return -1;
    }
    return 0;
}

/* ---------------------------------------------------------------- CSV */

static void write_value(FILE *fp, const GmMetric *metric, double value) {
    if (!isfinite(value) || value < 0.0) fputs(GM_UNAVAILABLE, fp);
    else if (metric->is_integer) fprintf(fp, "%lld", (long long)value);
    else fprintf(fp, "%.9g", value);
}

static void write_header(FILE *fp, const char *time_column, const GmAspect *aspects, size_t count) {
    fputs("Program,Powercap", fp);
    if (time_column) fprintf(fp, ",%s", time_column);
    for (size_t d = 0; d < count; d++) {
        if (!device_active[d]) continue;
        for (size_t m = 0; m < aspects[d].metric_count; m++) {
            if (!selected_mask[d][m]) continue;
            const GmMetric *metric = &aspects[d].metrics[m];
            fprintf(fp, ",%s", metric->name);
            if (metric->unit && *metric->unit) fprintf(fp, "_%s", metric->unit);
        }
    }
    fputc('\n', fp);
}

static FILE *open_output(const char *path, const char *time_column,
                         const GmAspect *aspects, size_t count) {
    FILE *fp = fopen(path, "a+");
    if (!fp) {
        perror("[ERROR] Failed to open the output CSV");
        return NULL;
    }
    struct stat info;
    if (fstat(fileno(fp), &info) != 0) {
        perror("[ERROR] Failed to inspect the output CSV");
        fclose(fp);
        return NULL;
    }
    if (info.st_size == 0) {
        write_header(fp, time_column, aspects, count);
    } else {
        char *expected = NULL, *actual = NULL;
        size_t expected_size = 0, actual_size = 0;
        FILE *header = open_memstream(&expected, &expected_size);
        if (!header) {
            perror("[ERROR] Failed to prepare the CSV header");
            fclose(fp);
            return NULL;
        }
        write_header(header, time_column, aspects, count);
        int header_error = fclose(header);
        rewind(fp);
        ssize_t length = getline(&actual, &actual_size, fp);
        if (header_error != 0 || length < 0 || strcmp(actual, expected) != 0) {
            fprintf(stderr, "[ERROR] Existing CSV header does not match the selected aspects and mode.\n");
            free(actual);
            free(expected);
            fclose(fp);
            return NULL;
        }
        free(actual);
        free(expected);
    }
    return fp;
}

/* Every aspect samples on its own thread, so rows are aligned on a common time grid:
   an aspect contributes its latest sample whose time is within half an interval of the
   row. The final row, at the workload exit time, takes the last sample of each aspect.
   Counters report the consumption of each interval: the reading taken at that instant
   minus the reading of the previous row, starting from the execution's first sample.
   Summing a run's rows therefore yields aggregate mode's end - start for that run.
   Gauges and durations report the raw sample. */
static int dump_over_time(const char *path, const char *program,
                          int powercap, const GmAspect *aspects, size_t count,
                          unsigned interval_ms, double workload_ms) {
    FILE *fp = open_output(path, "Timestamp_ms", aspects, count);
    if (!fp) return -1;
    size_t next[GM_MAX_DEVICES] = {0};
    static double previous_counter[GM_MAX_DEVICES][GM_MAX_METRICS_PER_DEVICE];
    double total = workload_ms;
    for (size_t d = 0; d < count; d++) {
        size_t n = device_active[d] && !device_unavailable[d] ? aspects[d].count() : 0;
        if (n && aspects[d].timestamp(n - 1) > total) total = aspects[d].timestamp(n - 1);
        for (size_t m = 0; n && m < aspects[d].metric_count; m++)
            if (selected_mask[d][m] && aspects[d].metrics[m].kind == GM_KIND_COUNTER)
                previous_counter[d][m] = aspects[d].metrics[m].sample(0);
    }
    double step = (double)interval_ms;
    for (size_t k = 1;; k++) {
        double t = (double)k * step;
        int final_row = t >= total;
        if (final_row) t = total;
        fprintf(fp, "%s,%d,%.6f", program, powercap, t);
        for (size_t d = 0; d < count; d++) {
            if (!device_active[d]) continue;
            size_t n = device_unavailable[d] ? 0 : aspects[d].count();
            while (next[d] < n && (final_row || aspects[d].timestamp(next[d]) < t + step / 2.0))
                next[d]++;
            for (size_t m = 0; m < aspects[d].metric_count; m++) {
                if (!selected_mask[d][m]) continue;
                fputc(',', fp);
                if (next[d] == 0) {
                    fputs(GM_UNAVAILABLE, fp);
                    continue;
                }
                double value = aspects[d].metrics[m].sample(next[d] - 1);
                if (aspects[d].metrics[m].kind == GM_KIND_COUNTER) {
                    double start = previous_counter[d][m];
                    previous_counter[d][m] = value;
                    value = start < 0.0 || value < 0.0 || value < start ? -1.0 : value - start;
                }
                write_value(fp, &aspects[d].metrics[m], value);
            }
        }
        fputc('\n', fp);
        if (final_row) break;
    }
    return fclose(fp) == 0 ? 0 : -1;
}

/* ---------------------------------------------------------------- modes */

static int run_over_time_once(const char *command, const char *output, int powercap) {
    int result = 1;
    struct timespec origin;
    clock_gettime(CLOCK_MONOTONIC, &origin);
    unsigned char started[GM_MAX_DEVICES] = {0};
    if (begin_measurement(started) != 0) return 1;
    pid_t pid = start_workload(command, ctx_mask);
    if (pid < 0) {
        perror("[ERROR] fork");
        finish_measurement(started);
        return 1;
    }
    if (start_samplers(ctx_devices, ctx_count, ctx_config->interval_ms, &origin) != 0) {
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
        finish_measurement(started);
        return 1;
    }
    int status = wait_workload(pid, ctx_devices, ctx_count, ctx_timeout_s);
    int measurement_status = finish_measurement(started);
    double workload_ms = elapsed_ms_since(&origin);
    stop_samplers(ctx_devices, ctx_count);
    if (measurement_status != 0) return 1;
    if (status != 0) {
        fprintf(stderr, "[ERROR] The workload failed (status %d); no CSV was written.\n", status);
        return 1;
    }
    result = dump_over_time(output, ctx_config->program, powercap,
                            ctx_devices, ctx_count, ctx_config->interval_ms, workload_ms) == 0 ? 0 : 1;
    return result;
}

/* Aggregate mode has no sampling threads: counters report end - start and gauges the
   reading taken when the workload ends. */
static double summarize(const GmMetric *metric, double start_value, double end_value) {
    if (metric->kind != GM_KIND_COUNTER) return end_value;
    double delta = end_value - start_value;
    return start_value < 0.0 || end_value < 0.0 || delta < 0.0 ? -1.0 : delta;
}

/* Runs the workload once and appends one row to fp. */
static int measure_aggregate(const char *command, FILE *fp,
                            const char *program, int powercap, unsigned timeout_s) {
    static double start_values[GM_MAX_DEVICES][GM_MAX_METRICS_PER_DEVICE];
    static double end_values[GM_MAX_DEVICES][GM_MAX_METRICS_PER_DEVICE];
    for (size_t d = 0; d < ctx_count; d++)
        for (size_t m = 0; device_active[d] && m < ctx_devices[d].metric_count; m++)
            if (!device_unavailable[d] && selected_mask[d][m] &&
                ctx_devices[d].metrics[m].kind == GM_KIND_COUNTER)
                start_values[d][m] = ctx_devices[d].metrics[m].read();
    unsigned char started[GM_MAX_DEVICES] = {0};
    if (begin_measurement(started) != 0) return 1;
    pid_t pid = start_workload(command, ctx_mask);
    if (pid < 0) {
        perror("[ERROR] fork");
        finish_measurement(started);
        return 1;
    }
    int status = wait_workload(pid, ctx_devices, ctx_count, timeout_s);
    if (finish_measurement(started) != 0) return 1;
    for (size_t d = 0; d < ctx_count; d++)
        for (size_t m = 0; device_active[d] && m < ctx_devices[d].metric_count; m++)
            if (!device_unavailable[d] && selected_mask[d][m])
                end_values[d][m] = ctx_devices[d].metrics[m].read();
    if (status != 0) {
        fprintf(stderr, "[ERROR] The workload failed (status %d).\n", status);
        return 1;
    }
    fprintf(fp, "%s,%d", program, powercap);
    for (size_t d = 0; d < ctx_count; d++) {
        for (size_t m = 0; device_active[d] && m < ctx_devices[d].metric_count; m++) {
            if (!selected_mask[d][m]) continue;
            fputc(',', fp);
            double value = device_unavailable[d] ? -1.0 :
                summarize(&ctx_devices[d].metrics[m], start_values[d][m], end_values[d][m]);
            write_value(fp, &ctx_devices[d].metrics[m], value);
        }
    }
    fputc('\n', fp);
    return fflush(fp) == 0 ? 0 : 1;
}

/* Called by the aspect libraries' powercap calibration: measures the workload ntimes in
   aggregate mode at the given powercap and appends the rows to measurements_file. */
int runTesting(int ntimes, int core, const char *command,
               const char *program, const char *measurements_file, int powercap,
               int time_out_limit, float temp_threshold, int variance, int memory) {
    (void)core;
    (void)memory;
    (void)temp_threshold;
    unsigned timeout_s = time_out_limit > 0 ? (unsigned)time_out_limit : 0;

    /* The workload must run correctly before it is measured. */
    pid_t pid = start_workload(command, ctx_mask);
    if (pid < 0) {
        perror("[ERROR] fork");
        return -1;
    }
    if (wait_workload(pid, ctx_devices, ctx_count, timeout_s) != 0) {
        fprintf(stderr, "[ERROR] The program failed its pre-check run (powercap %d).\n", powercap);
        return -1;
    }

    FILE *fp = open_output(measurements_file, NULL, ctx_devices, ctx_count);
    if (!fp) return -1;
    int result = 0;
    for (int i = 0; i < ntimes && result == 0; i++) {
        if (wait_for_temperature(variance < 0 ? 0 : (unsigned)variance) != 0 ||
            measure_aggregate(command, fp, program, powercap, timeout_s) != 0)
            result = -1;
    }
    if (fclose(fp) != 0) result = -1;
    return result;
}

/* Applies each powercap in turn, restoring the saved state as soon as its measurement ends. */
static int run_over_time(const char *command, const char *output) {
    int result = 0;
    for (size_t c = 0; c < ctx_config->powercap_count && result == 0; c++) {
        int powercap = ctx_config->powercaps[c];
        if (apply_powercap(powercap) != 0) return 1;
        for (unsigned run = 0; run < ctx_config->repetitions && result == 0; run++) {
            if (wait_for_temperature(ctx_config->variance) != 0) result = 1;
            if (result == 0) result = run_over_time_once(command, output, powercap);
        }
        if (release_powercap() != 0) result = 1;
        if (result == 0 && c + 1 < ctx_config->powercap_count && !temperature_unavailable) sleep(1);
    }
    return result;
}

static int run_aggregate(const char *command, const char *output) {
    FILE *fp = open_output(output, NULL, ctx_devices, ctx_count);
    if (!fp) return 1;
    int result = 0;
    for (size_t c = 0; c < ctx_config->powercap_count && result == 0; c++) {
        int powercap = ctx_config->powercaps[c];
        if (apply_powercap(powercap) != 0) {
            result = 1;
            break;
        }
        for (unsigned run = 0; run < ctx_config->repetitions && result == 0; run++) {
            if (wait_for_temperature(ctx_config->variance) != 0 ||
                measure_aggregate(command, fp, ctx_config->program,
                                 powercap, ctx_timeout_s) != 0)
                result = 1;
        }
        if (release_powercap() != 0) result = 1;
    }
    if (fclose(fp) != 0) result = 1;
    return result;
}

/* Summarizes a sweep when exactly one selected millisecond duration is available. */
static void run_powercap_processor(const char *csv) {
#ifdef GM_PROCESSOR
    const GmMetric *duration = NULL;
    for (size_t d = 0; d < ctx_count; d++) {
        if (!device_active[d] || device_unavailable[d]) continue;
        for (size_t m = 0; m < ctx_devices[d].metric_count; m++) {
            const GmMetric *metric = &ctx_devices[d].metrics[m];
            if (!selected_mask[d][m] || metric->kind != GM_KIND_DURATION ||
                !metric->unit || strcmp(metric->unit, "ms")) continue;
            if (duration) {
                printf("[INFO] Powercap summary skipped: select one duration in milliseconds.\n");
                return;
            }
            duration = metric;
        }
    }
    char header[8192] = "";
    FILE *input = fopen(csv, "r");
    if (input) {
        if (!fgets(header, sizeof(header), input)) header[0] = '\0';
        fclose(input);
    }
    if (!strstr(header, ".Package_") || !duration) {
        printf("[INFO] Powercap sweep saved to %s. The speedup/greenup summary needs a Package "
               "energy metric and one duration in milliseconds, so it was skipped.\n", csv);
        return;
    }
    char *time_column = NULL;
    if (asprintf(&time_column, "%s_ms", duration->name) < 0) {
        fprintf(stderr, "[WARNING] Unable to allocate the duration column name.\n");
        return;
    }
    size_t csv_length = strlen(csv);
    if (csv_length > 4 && !strcmp(csv + csv_length - 4, ".csv")) csv_length -= 4;
    char *optimal_file = NULL;
    if (asprintf(&optimal_file, "%.*s-optimal-powercap.txt", (int)csv_length, csv) < 0) {
        fprintf(stderr, "[WARNING] Unable to allocate the optimal powercap file name.\n");
        free(time_column);
        return;
    }
    printf("[INFO] Processing calibration CSV via: python3 %s\n", GM_PROCESSOR);
    pid_t child = fork();
    if (child == 0) {
        execlp("python3", "python3", GM_PROCESSOR, csv, "--time-column", time_column,
               "--optimal-file", optimal_file, (char *)NULL);
        _exit(127);
    }
    if (child < 0) {
        perror("[WARNING] Unable to start calibration post-processing");
        free(time_column);
        free(optimal_file);
        return;
    }
    int status;
    pid_t waited;
    do { waited = waitpid(child, &status, 0); } while (waited < 0 && errno == EINTR);
    if (waited < 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0)
        fprintf(stderr, "[WARNING] Calibration post-processing script exited with error.\n");
    free(time_column);
    free(optimal_file);
#else
    printf("[INFO] Powercap sweep saved to %s.\n", csv);
#endif
}

static int run_powercap_calibration(const char *command, const char *csv, int min_powercap,
                                    int max_powercap, double baseline, unsigned timeout_s) {
    if (calibration_dev < 0) {
        fprintf(stderr, "[ERROR] No selected aspect declares powercap_calibration.\n");
        return 1;
    }
    int result = ctx_devices[calibration_dev].powercap_calibrate(
        (int)ctx_config->repetitions, 0, command, ctx_config->program, csv,
        NULL, min_powercap, max_powercap, (int)timeout_s, (float)baseline,
        (int)ctx_config->variance, 0);
    if (result != 0) {
        fprintf(stderr, "[ERROR] Powercap calibration failed; see preceding diagnostics.\n");
        return 1;
    }
    run_powercap_processor(csv);
    return 0;
}

/* ---------------------------------------------------------------- arguments */

static void show_help(const char *bin, const char *mode, const GmFlag *flags, size_t flag_count) {
    printf("This executable was woven for %s measurements.\n"
           "Usage: %s -command <cmd> [-n_times <N>]\n"
           "       %s -command <cmd> -program <name>\n"
           "          -output <csv> -n_times <N> -sleep-secs <s> [-powercap <W[,W...]>]\n"
           "          -timeout-hours <hours> (--no-metrics|metric flags) [powercap aspect flag] [-interval-ms <ms>]\n"
           "          [-temperature-control <metric> -calib-file <metric>=<path> ... -variance <C>]\n"
           "       %s --temperature-calibration -sleep-secs <s> <temperature metric flags>\n"
           "          -calib-file <metric>=<path> ...\n"
           "       %s --powercap-calibration -range <min>-<max> -command <cmd>\n"
           "          -program <name> -output <csv> -n_times <N>\n"
           "          -sleep-secs <s> -timeout-hours <hours> (--no-metrics|metric flags) <powercap aspect flag>\n"
           "          [-compile_cmd <cmd>] [-temperature-control <metric> -calib-file <metric>=<path> ... -variance <C>]\n\n"
           "-powercap is required when a powercap control aspect is selected.\n"
           "Metric flags (select explicitly):\n",
           mode, bin, bin, bin, bin);
    for (size_t i = 0; i < flag_count; i++) printf("  %s\n", flags[i].flag);
}

static int parse_unsigned(const char *text, unsigned min, unsigned max, unsigned *out) {
    char *end;
    errno = 0;
    unsigned long value = strtoul(text, &end, 10);
    if (errno || *text == '\0' || *end || *text == '-' || value < min || value > max) return -1;
    *out = (unsigned)value;
    return 0;
}

#define GM_MAX_POWERCAPS 128

/* Comma-separated list of unique limits, each -1 (saved baseline) or at least 1 W. */
static int parse_powercaps(const char *text, int *caps, int capacity) {
    int count = 0;
    const char *cursor = text;
    for (;;) {
        char *end;
        errno = 0;
        long value = strtol(cursor, &end, 10);
        if (errno || end == cursor || value < -1 || value == 0 || value > INT_MAX) return -1;
        for (int i = 0; i < count; i++)
            if (caps[i] == (int)value) return -1;
        if (count >= capacity) return -1;
        caps[count++] = (int)value;
        if (*end == '\0') return count;
        if (*end != ',') return -1;
        cursor = end + 1;
    }
}

static int parse_range(const char *text, int *minimum, int *maximum) {
    char *end;
    errno = 0;
    long first = strtol(text, &end, 10);
    if (errno || end == text || *end != '-' || first < 1 || first > INT_MAX) return -1;
    const char *second_start = end + 1;
    errno = 0;
    long second = strtol(second_start, &end, 10);
    if (errno || end == second_start || *end || second < first || second > INT_MAX ||
        second - first + 1 >= GM_MAX_POWERCAPS) return -1;
    *minimum = (int)first;
    *maximum = (int)second;
    return 0;
}

typedef enum { ACTION_MEASURE, ACTION_TEMPERATURE, ACTION_POWERCAP } Action;

static int run_plain_command(const char *command, unsigned repetitions) {
    for (unsigned run = 0; run < repetitions; run++) {
        int status = system(command);
        if (status == -1) {
            perror("[ERROR] Failed to execute command");
            return 1;
        }
        if (WIFEXITED(status)) {
            int code = WEXITSTATUS(status);
            if (code != 0) {
                fprintf(stderr, "[ERROR] Command exited with status %d.\n", code);
                return code;
            }
        } else {
            fprintf(stderr, "[ERROR] Command terminated abnormally.\n");
            return 1;
        }
    }
    return 0;
}

int gm_main(int argc, char **argv, const GmAspect *aspects, size_t device_count,
            const GmFlag *flags, size_t flag_count, const char *mode) {
    GmConfig config = {0};
    config.mode = mode;
    const char *command = NULL, *output = NULL, *compile_command = NULL;
    const char *calibration_files[GM_MAX_DEVICES] = {0};
    int metric_flags_given = 0, no_metrics = 0, min_powercap = -1, max_powercap = -1;
    int selected_powercap = -1;
    int mode_given = 0, sleep_given = 0, repetitions_given = 0, interval_given = 0;
    int powercap_given = 0, timeout_given = 0, variance_given = 0, range_given = 0;
    unsigned timeout_hours = 0;
    Action action = ACTION_MEASURE;
    static unsigned char chosen[GM_MAX_DEVICES][GM_MAX_METRICS_PER_DEVICE];
    static int powercaps[GM_MAX_POWERCAPS];

    if (device_count > GM_MAX_DEVICES) {
        fprintf(stderr, "[ERROR] At most %d aspects are supported.\n", GM_MAX_DEVICES);
        return 1;
    }
    for (size_t d = 0; d < device_count; d++)
        if (aspects[d].metric_count > GM_MAX_METRICS_PER_DEVICE) {
            fprintf(stderr, "[ERROR] At most %d metrics per aspect are supported.\n",
                    GM_MAX_METRICS_PER_DEVICE);
            return 1;
        }

    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];
        int has_value = i + 1 < argc;
        if (!strcmp(arg, "--help") || !strcmp(arg, "-h")) {
            show_help(argv[0], mode, flags, flag_count);
            return 0;
        } else if (!strcmp(arg, "--temperature-calibration") ||
                   !strcmp(arg, "--powercap-calibration")) {
            if (mode_given++) {
                fprintf(stderr, "[ERROR] Select at most one calibration mode.\n");
                return 1;
            }
            if (!strcmp(arg, "--temperature-calibration")) action = ACTION_TEMPERATURE;
            else action = ACTION_POWERCAP;
        } else if (!strcmp(arg, "--no-metrics")) {
            no_metrics = 1;
            metric_flags_given = 1;
        }
        else if ((!strcmp(arg, "-command") || !strcmp(arg, "-run_cmd")) && has_value)
            command = argv[++i];
        else if (!strcmp(arg, "-compile_cmd") && has_value) compile_command = argv[++i];
        else if (!strcmp(arg, "-program") && has_value) config.program = argv[++i];
        else if (!strcmp(arg, "-output") && has_value) {
            output = argv[++i];
        } else if (!strcmp(arg, "-temperature-control") && has_value) {
            const char *name = argv[++i];
            size_t d = 0;
            while (d < device_count &&
                   !(aspects[d].thermal && aspects[d].thermal_metric >= 0 &&
                     !strcmp(aspects[d].metrics[aspects[d].thermal_metric].name, name))) d++;
            if (d == device_count || thermal_control[d]) {
                fprintf(stderr, "[ERROR] -temperature-control requires a unique temperature metric name.\n");
                return 1;
            }
            thermal_control[d] = 1;
        } else if (!strcmp(arg, "-calib-file") && has_value) {
            const char *value = argv[++i], *equal = strchr(value, '=');
            size_t d = 0;
            while (equal && d < device_count &&
                   !(aspects[d].thermal && aspects[d].thermal_metric >= 0 &&
                     strlen(aspects[d].metrics[aspects[d].thermal_metric].name) == (size_t)(equal - value) &&
                     !strncmp(aspects[d].metrics[aspects[d].thermal_metric].name, value,
                              (size_t)(equal - value)))) d++;
            if (!equal || !equal[1] || d == device_count || calibration_files[d]) {
                fprintf(stderr, "[ERROR] -calib-file requires a unique temperature metric and path: <metric>=<path>.\n");
                return 1;
            }
            calibration_files[d] = equal + 1;
        }
        else if (!strcmp(arg, "-range") && has_value) {
            if (parse_range(argv[++i], &min_powercap, &max_powercap) != 0) {
                fprintf(stderr, "[ERROR] Invalid -range; expected a positive range of fewer than 128 watts.\n");
                return 1;
            }
            range_given = 1;
        } else if (!strcmp(arg, "-interval-ms") && has_value) {
            if (parse_unsigned(argv[++i], 1, 4294967u, &config.interval_ms) != 0) {
                fprintf(stderr, "[ERROR] -interval-ms must be between 1 and 4294967.\n");
                return 1;
            }
            interval_given = 1;
        } else if (!strcmp(arg, "-n_times") && has_value) {
            if (parse_unsigned(argv[++i], 1, 1000000u, &config.repetitions) != 0) {
                fprintf(stderr, "[ERROR] -n_times must be a positive integer.\n");
                return 1;
            }
            repetitions_given = 1;
        } else if ((!strcmp(arg, "-sleep-secs") || !strcmp(arg, "-sleep_time") ||
                    !strcmp(arg, "-secs_to_sleep")) && has_value) {
            if (parse_unsigned(argv[++i], 0, 86400u, &config.sleep_seconds) != 0) {
                fprintf(stderr, "[ERROR] -sleep-secs must be between 0 and 86400.\n");
                return 1;
            }
            sleep_given = 1;
        } else if (!strcmp(arg, "-variance") && has_value) {
            if (parse_unsigned(argv[++i], 0, 1000u, &config.variance) != 0) {
                fprintf(stderr, "[ERROR] -variance must be between 0 and 1000.\n");
                return 1;
            }
            variance_given = 1;
        } else if ((!strcmp(arg, "-timeout-hours") || !strcmp(arg, "-time_out_limit")) && has_value) {
            if (parse_unsigned(argv[++i], 0, 1000u, &timeout_hours) != 0) {
                fprintf(stderr, "[ERROR] -timeout-hours must be between 0 and 1000 hours.\n");
                return 1;
            }
            timeout_given = 1;
        } else if (!strcmp(arg, "-powercap") && has_value) {
            int count = parse_powercaps(argv[++i], powercaps, GM_MAX_POWERCAPS);
            if (count < 0) {
                fprintf(stderr, "[ERROR] -powercap must be a comma-separated list of unique "
                                "values, each -1 (saved baseline) or at least 1 W.\n");
                return 1;
            }
            config.powercaps = powercaps;
            config.powercap_count = (size_t)count;
            powercap_given = 1;
        } else {
            size_t f = 0;
            while (f < flag_count && strcmp(arg, flags[f].flag) != 0) f++;
            if (f == flag_count) {
                fprintf(stderr, "[ERROR] Unknown or incomplete argument '%s'.\n", arg);
                return 1;
            }
            size_t d = flags[f].aspect;
            if (aspects[d].metric_count == 0) {
                if (selected_powercap >= 0) {
                    fprintf(stderr, "[ERROR] Select at most one powercap control aspect.\n");
                    return 1;
                }
                selected_powercap = (int)d;
                powercap_selected[d] = 1;
            } else {
                chosen[d][flags[f].metric] = 1;
                metric_flags_given = 1;
            }
        }
    }
    if ((argc == 3 || (argc == 5 && repetitions_given)) && command && *command && !mode_given)
        return run_plain_command(command, repetitions_given ? config.repetitions : 1);
    if (!metric_flags_given) {
        fprintf(stderr, "[ERROR] Select at least one metric (or --no-metrics).\n");
        return 1;
    }

    memcpy(selected_mask, chosen, sizeof(selected_mask));
    for (size_t d = 0; d < device_count; d++)
        for (size_t m = 0; m < aspects[d].metric_count; m++)
            if (selected_mask[d][m]) device_active[d] = 1;

    int thermal_count = 0, controlled_count = 0, selected_count = 0;
    for (size_t d = 0; d < device_count; d++) {
        if (device_active[d]) selected_count++;
        if (device_active[d] && aspects[d].thermal) thermal_count++;
        if (thermal_control[d]) {
            controlled_count++;
        }
        if ((action == ACTION_TEMPERATURE && device_active[d] && aspects[d].thermal) ||
            (action != ACTION_TEMPERATURE && thermal_control[d])) {
            const char *thermal_name = aspects[d].thermal_metric >= 0 ?
                aspects[d].metrics[aspects[d].thermal_metric].name : aspects[d].name;
            if (!calibration_files[d]) {
                fprintf(stderr, "[ERROR] Temperature metric '%s' requires -calib-file %s=<path>.\n",
                        thermal_name, thermal_name);
                return 1;
            }
            if (output && same_calibration_file(calibration_files[d], output)) {
                fprintf(stderr, "[ERROR] Output CSV and temperature calibration files must be separate.\n");
                return 1;
            }
            for (size_t p = 0; p < d; p++)
                if (calibration_files[p] && same_calibration_file(calibration_files[d], calibration_files[p])) {
                    fprintf(stderr, "[ERROR] Temperature aspects require separate calibration files.\n");
                    return 1;
                }
        } else if (calibration_files[d]) {
            fprintf(stderr, "[ERROR] Calibration file supplied for an unselected or uncontrolled aspect '%s'.\n",
                    aspects[d].name);
            return 1;
        }
    }
    if (no_metrics && selected_count) {
        fprintf(stderr, "[ERROR] --no-metrics cannot be combined with metric flags.\n");
        return 1;
    }
    if (selected_powercap >= 0) {
        const GmAspect *control = &aspects[selected_powercap];
        if (action == ACTION_TEMPERATURE ||
            (action == ACTION_MEASURE && (!control->powercap_set || !control->powercap_restore)) ||
            (action == ACTION_POWERCAP && !control->powercap_calibrate)) {
            fprintf(stderr, "[ERROR] Powercap aspect '%s' does not support the selected mode.\n",
                    control->name);
            return 1;
        }
    } else if (action == ACTION_POWERCAP) {
        fprintf(stderr, "[ERROR] Select a powercap control aspect for calibration.\n");
        return 1;
    } else if (action == ACTION_MEASURE) {
        for (size_t c = 0; c < config.powercap_count; c++) {
            if (config.powercaps[c] > 0) {
                fprintf(stderr, "[ERROR] Select a powercap control aspect to apply a positive limit.\n");
                return 1;
            }
        }
    }
    if (!sleep_given || (action != ACTION_TEMPERATURE &&
        (!repetitions_given || !timeout_given ||
         !config.program || !*config.program || !output || !*output))) {
        fprintf(stderr, "[ERROR] Missing required execution options; see --help.\n");
        return 1;
    }
    if (action == ACTION_TEMPERATURE && (!thermal_count || selected_count != thermal_count)) {
        fprintf(stderr, "[ERROR] Temperature calibration requires selected temperature aspects only.\n");
        return 1;
    }
    if (action != ACTION_TEMPERATURE && controlled_count && !variance_given) {
        fprintf(stderr, "[ERROR] Controlled temperature aspects require -variance.\n");
        return 1;
    }
    if (action == ACTION_MEASURE) {
        if (selected_powercap >= 0 && !powercap_given) {
            fprintf(stderr, "[ERROR] A selected powercap control aspect requires -powercap.\n");
            return 1;
        }
        if (!strcmp(config.mode, "over-time") && !interval_given) {
            fprintf(stderr, "[ERROR] Over-time measurement requires -interval-ms.\n");
            return 1;
        }
        if (!powercap_given) {
            powercaps[0] = -1;
            config.powercaps = powercaps;
            config.powercap_count = 1;
        }
    }
    if ((compile_command && !*compile_command) ||
        (action != ACTION_POWERCAP && (range_given || compile_command)) ||
        (action == ACTION_POWERCAP && (powercap_given || interval_given)) ||
        (action == ACTION_TEMPERATURE && (command || output || config.program ||
          repetitions_given || timeout_given || powercap_given || interval_given || variance_given || controlled_count)) ||
        (action != ACTION_TEMPERATURE && !controlled_count && variance_given) ||
        (action == ACTION_MEASURE && !strcmp(config.mode, "aggregate") && interval_given)) {
        fprintf(stderr, "[ERROR] An option is not valid for the selected mode.\n");
        return 1;
    }
    if (action != ACTION_TEMPERATURE &&
        strpbrk(config.program, ",\r\n\"")) {
        fprintf(stderr, "[ERROR] The program label cannot contain CSV delimiters or newlines.\n");
        return 1;
    }
    if (action != ACTION_TEMPERATURE && (!command || !*command)) {
        fprintf(stderr, "[ERROR] No command to run; use -command.\n");
        return 1;
    }
    if (action == ACTION_POWERCAP && (!range_given || min_powercap < 1 ||
        max_powercap < min_powercap || (long long)max_powercap - min_powercap + 1 >= GM_MAX_POWERCAPS)) {
        fprintf(stderr, "[ERROR] --powercap-calibration requires -range with a minimum >= 1 W.\n");
        return 1;
    }
    if (action == ACTION_POWERCAP && compile_command && *compile_command) {
        printf("[INFO] Compiling benchmark target: %s\n", compile_command);
        if (system(compile_command) != 0) {
            fprintf(stderr, "[ERROR] Compilation command failed.\n");
            return 1;
        }
    }

    sigset_t block, old_mask;
    sigemptyset(&block);
    sigaddset(&block, SIGCHLD);
    sigprocmask(SIG_BLOCK, &block, &old_mask);
    void (*old_sigint)(int) = signal(SIGINT, on_sigint);
    ctx_devices = aspects;
    ctx_count = device_count;
    ctx_mask = &old_mask;
    ctx_config = &config;
    ctx_timeout_s = timeout_hours * 3600u;
    temperature_calibration = action == ACTION_TEMPERATURE;
    powercap_calibration = action == ACTION_POWERCAP;

    int result = 1;
    int run_plain = 0;
    double baseline = NAN;
    if (setup_devices(aspects, device_count) == 0) {
        result = 0;
        if (selected_powercap >= 0 && !powercap_selected[selected_powercap]) {
            fprintf(stderr, "[WARNING] Powercap limits will not be applied.\n");
            powercaps[0] = -1;
            config.powercaps = powercaps;
            config.powercap_count = 1;
        }
        for (size_t d = 0; d < device_count; d++) {
            if (action == ACTION_TEMPERATURE ?
                (!device_active[d] || !aspects[d].thermal) : !thermal_control[d]) continue;
            double temperature;
            if (device_unavailable[d] || read_temperature(d, &temperature) != 0) {
                if (action == ACTION_TEMPERATURE) {
                    fprintf(stderr, "[ERROR] Temperature aspect '%s' is unavailable.\n", aspects[d].name);
                    result = 1;
                } else {
                    disable_temperature(d);
                }
            }
        }
        for (size_t d = 0; d < device_count && result == 0; d++) {
            thermal_baselines[d] = NAN;
            if ((action == ACTION_TEMPERATURE ?
                 (!device_active[d] || !aspects[d].thermal) : !thermal_control[d]) ||
                thermal_disabled[d]) continue;
            for (size_t p = 0; p < d; p++)
                if (calibration_files[p] && same_calibration_file(calibration_files[d], calibration_files[p])) {
                    fprintf(stderr, "[ERROR] Temperature aspects require separate calibration files.\n");
                    result = 1;
                    break;
                }
            if (result) break;
            if (action == ACTION_TEMPERATURE) {
                int status = calibrate_temperature(d, calibration_files[d], config.sleep_seconds);
                if (status > 0)
                    fprintf(stderr, "[ERROR] Temperature aspect '%s' is unavailable.\n", aspects[d].name);
                result = status != 0;
            } else {
                int status = resolve_baseline(d, calibration_files[d],
                                              temperature_unavailable ? 0 : config.sleep_seconds,
                                              &thermal_baselines[d]);
                if (status > 0) {
                    disable_temperature(d);
                    continue;
                }
                result = status < 0;
                if (!result && same_calibration_file(calibration_files[d], output)) {
                    fprintf(stderr, "[ERROR] Output CSV and temperature calibration files must be separate.\n");
                    result = 1;
                }
                if (!isfinite(baseline)) baseline = thermal_baselines[d];
            }
        }
        if (result == 0 && action != ACTION_TEMPERATURE) {
            int active = 0;
            for (size_t d = 0; d < device_count; d++)
                if (device_active[d] || powercap_selected[d] ||
                    (thermal_control[d] && !thermal_disabled[d])) active = 1;
            if (action == ACTION_MEASURE && !active) {
                fprintf(stderr, "[WARNING] No active aspects; running the command once without "
                                "measurement, repetitions, timeout or CSV.\n");
                run_plain = 1;
            } else if (action == ACTION_POWERCAP)
                result = run_powercap_calibration(command, output, min_powercap,
                                                  max_powercap, baseline, ctx_timeout_s);
            else if (!strcmp(config.mode, "aggregate"))
                result = run_aggregate(command, output);
            else
                result = run_over_time(command, output);
        }
        teardown_devices(aspects, device_count);
    }
    sigprocmask(SIG_SETMASK, &old_mask, NULL);
    if (old_sigint != SIG_ERR) signal(SIGINT, old_sigint);
    if (run_plain) return run_plain_command(command, 1);
    return result;
}

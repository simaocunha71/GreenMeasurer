/* Runtime of the generated GreenMeasurer: runs the workload, drives the generated device
   threads and writes the CSV. It knows nothing about any concrete metric. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "gm_device.h"

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

#define GM_UNAVAILABLE "NOT_SUPPORTED"
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
static unsigned char setup_done[GM_MAX_DEVICES];
static unsigned char hooks_disabled[GM_MAX_DEVICES];
static unsigned char device_unavailable[GM_MAX_DEVICES];
static unsigned char sampler_started[GM_MAX_DEVICES];

/* State shared by the measurement helpers, set once by gm_main. */
static const GmDevice *ctx_devices;
static size_t ctx_count;
static const sigset_t *ctx_mask;
static const GmConfig *ctx_config;
static unsigned ctx_timeout_s; /* 0: no timeout */
static int thermal_dev = -1, powercap_dev = -1, calibration_dev = -1;
static volatile sig_atomic_t terminate_requested;

static void on_sigint(int signum) {
    (void)signum;
    terminate_requested = 1;
}

static int has_hooks(const GmDevice *device) {
    return device->thermal || device->powercap_set || device->powercap_restore ||
           device->powercap_calibrate;
}

static int setup_devices(const GmDevice *devices, size_t count) {
    for (size_t d = 0; d < count; d++) {
        if (!device_active[d] && !has_hooks(&devices[d])) continue;
        if (devices[d].setup && devices[d].setup() != 0) {
            if (!device_active[d]) {
                fprintf(stderr, "[WARNING] Setup of device '%s' failed; its thermal and powercap "
                        "hooks are disabled.\n", devices[d].name);
                hooks_disabled[d] = 1;
                continue;
            }
            if (devices[d].optional) {
                fprintf(stderr, "[WARNING] Setup of optional device '%s' failed; its metrics "
                                "are unavailable and its hooks are disabled.\n", devices[d].name);
                device_unavailable[d] = 1;
                hooks_disabled[d] = 1;
                continue;
            }
            fprintf(stderr, "[ERROR] Setup of device '%s' failed.\n", devices[d].name);
            for (size_t u = d; u-- > 0;)
                if (setup_done[u] && devices[u].teardown) devices[u].teardown();
            memset(setup_done, 0, sizeof(setup_done));
            return -1;
        }
        setup_done[d] = 1;
    }
    thermal_dev = powercap_dev = calibration_dev = -1;
    for (size_t d = 0; d < count; d++) {
        if (hooks_disabled[d]) continue;
        if (thermal_dev < 0 && devices[d].thermal) thermal_dev = (int)d;
        if (powercap_dev < 0 && devices[d].powercap_set && devices[d].powercap_restore)
            powercap_dev = (int)d;
        if (calibration_dev < 0 && devices[d].powercap_calibrate) calibration_dev = (int)d;
    }
    return 0;
}

static void teardown_devices(const GmDevice *devices, size_t count) {
    for (size_t d = count; d-- > 0;)
        if (setup_done[d] && devices[d].teardown) devices[d].teardown();
    memset(setup_done, 0, sizeof(setup_done));
    thermal_dev = powercap_dev = calibration_dev = -1;
}

static int start_samplers(const GmDevice *devices, size_t count, unsigned interval_ms,
                          const struct timespec *origin) {
    for (size_t d = 0; d < count; d++) {
        if (!device_active[d] || device_unavailable[d]) continue;
        if (devices[d].start(selected_mask[d], interval_ms, origin) != 0) {
            fprintf(stderr, "[ERROR] Failed to start the sampling thread of '%s'.\n",
                    devices[d].name);
            for (size_t u = 0; u < d; u++)
                if (sampler_started[u]) devices[u].stop();
            memset(sampler_started, 0, sizeof(sampler_started));
            return -1;
        }
        sampler_started[d] = 1;
    }
    return 0;
}

static void stop_samplers(const GmDevice *devices, size_t count) {
    for (size_t d = 0; d < count; d++)
        if (sampler_started[d]) devices[d].stop();
    memset(sampler_started, 0, sizeof(sampler_started));
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

/* Waits for the workload; aborts it when a device runs out of static sample capacity or the
   timeout expires. Returns the exit status, or -1 on failure. */
static int wait_workload(pid_t pid, const GmDevice *devices, size_t count, unsigned timeout_s) {
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
            if (!sampler_started[d] || !devices[d].overflowed()) continue;
            fprintf(stderr, "[ERROR] Sample capacity (%d) of device '%s' exceeded; "
                    "measurement aborted.\n", GM_MAX_SAMPLES, devices[d].name);
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
            return -1;
        }
    }
}

/* ---------------------------------------------------------------- thermal control */

static int read_temperature(double *temperature) {
    double value = ctx_devices[thermal_dev].thermal();
    if (!isfinite(value) || value <= 0.0) {
        fprintf(stderr, "[ERROR] CPU temperature is unavailable; measurement cannot run.\n");
        return -1;
    }
    *temperature = value;
    return 0;
}

static double read_baseline_file(const char *path) {
    FILE *file = fopen(path, "r");
    if (!file) return -1.0;
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

/* Stores the "sensors" snapshot and the current temperature in the calibration file. */
static int calibrate_temperature(const char *path, unsigned sleep_seconds) {
    if (thermal_dev < 0) {
        fprintf(stderr, "[ERROR] No available device declares a thermal sensor; "
                        "temperature calibration cannot run.\n");
        return -1;
    }
    printf("[INFO] Sleeping %u seconds to stabilize and calibrate CPU temperature...\n",
           sleep_seconds);
    if (sleep_seconds && sleep(sleep_seconds) != 0) {
        fprintf(stderr, "[ERROR] Temperature calibration interrupted during stabilization.\n");
        return -1;
    }

    double mean;
    if (read_temperature(&mean) != 0) return -1;
    FILE *snapshot = fopen(path, "w");
    if (!snapshot) {
        perror("[ERROR] Failed to open calibration file");
        return -1;
    }
    pid_t child = fork();
    if (child == 0) {
        if (dup2(fileno(snapshot), STDOUT_FILENO) < 0) _exit(127);
        execlp("sensors", "sensors", (char *)NULL);
        _exit(127);
    }
    if (child < 0) {
        perror("[WARNING] Failed to start sensors");
    } else {
        int status;
        pid_t waited;
        do { waited = waitpid(child, &status, 0); } while (waited < 0 && errno == EINTR);
        if (waited < 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0)
            fprintf(stderr, "[WARNING] Unable to collect sensors snapshot.\n");
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
static int resolve_baseline(const char *path, unsigned sleep_seconds, double *baseline) {
    *baseline = NAN;
    if (thermal_dev < 0) return 0;
    if (access(path, F_OK) != 0 && calibrate_temperature(path, sleep_seconds) != 0) return -1;
    double value = read_baseline_file(path);
    if (!isfinite(value) || value <= 0.0) {
        fprintf(stderr, "[WARNING] CPU temperature calibration is invalid; "
                        "using the current temperature.\n");
        if (read_temperature(&value) != 0) return -1;
    }
    *baseline = value;
    return 0;
}

static int wait_for_temperature(double baseline, unsigned variance) {
    if (thermal_dev < 0 || !isfinite(baseline)) return 0;
    double temperature;
    if (read_temperature(&temperature) != 0) return -1;
    while (temperature > baseline + variance && !terminate_requested) {
        printf("[INFO] Waiting for temperature: current %.1f\xC2\xB0" "C; baseline %.1f\xC2\xB0"
               "C; variance %u\xC2\xB0" "C (limit %.1f\xC2\xB0" "C)...\n",
               temperature, baseline, variance, baseline + variance);
        sleep(1);
        if (read_temperature(&temperature) != 0) return -1;
    }
    if (terminate_requested) {
        fprintf(stderr, "[ERROR] Measurement interrupted while waiting for CPU temperature.\n");
        return -1;
    }
    return 0;
}

/* ---------------------------------------------------------------- powercap */

static int apply_powercap(int powercap) {
    if (powercap_dev < 0) {
        if (powercap > 0) {
            fprintf(stderr, "[ERROR] No device declares powercap_set/powercap_restore; "
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

static void write_header(FILE *fp, const char *time_column, const GmDevice *devices, size_t count) {
    fprintf(fp, "Language,Program,Powercap,%s", time_column);
    for (size_t d = 0; d < count; d++) {
        if (!device_active[d]) continue;
        for (size_t m = 0; m < devices[d].metric_count; m++) {
            if (!selected_mask[d][m]) continue;
            const GmMetric *metric = &devices[d].metrics[m];
            fprintf(fp, ",%s.%s", devices[d].name, metric->name);
            if (metric->unit && *metric->unit) fprintf(fp, "_%s", metric->unit);
        }
    }
    fputc('\n', fp);
}

static FILE *open_output(const char *path, const char *time_column,
                         const GmDevice *devices, size_t count) {
    FILE *fp = fopen(path, "a");
    if (!fp) {
        perror("[ERROR] Failed to open the output CSV");
        return NULL;
    }
    struct stat info;
    if (fstat(fileno(fp), &info) == 0 && info.st_size == 0)
        write_header(fp, time_column, devices, count);
    return fp;
}

/* Every device samples on its own thread, so rows are aligned on a common time grid:
   a device contributes its latest sample whose time is within half an interval of the
   row. The final row, at the workload exit time, takes the last sample of each device. */
static int dump_over_time(const char *path, const char *language, const char *program,
                          int powercap, const GmDevice *devices, size_t count,
                          unsigned interval_ms, double workload_ms) {
    FILE *fp = open_output(path, "Timestamp_ms", devices, count);
    if (!fp) return -1;
    size_t next[GM_MAX_DEVICES] = {0};
    double total = workload_ms;
    for (size_t d = 0; d < count; d++) {
        size_t n = device_active[d] && !device_unavailable[d] ? devices[d].count() : 0;
        if (n && devices[d].timestamp(n - 1) > total) total = devices[d].timestamp(n - 1);
    }
    double step = (double)interval_ms;
    for (size_t k = 1;; k++) {
        double t = (double)k * step;
        int final_row = t >= total;
        if (final_row) t = total;
        fprintf(fp, "%s,%s,%d,%.6f", language, program, powercap, t);
        for (size_t d = 0; d < count; d++) {
            if (!device_active[d]) continue;
            size_t n = device_unavailable[d] ? 0 : devices[d].count();
            while (next[d] < n && (final_row || devices[d].timestamp(next[d]) < t + step / 2.0))
                next[d]++;
            for (size_t m = 0; m < devices[d].metric_count; m++) {
                if (!selected_mask[d][m]) continue;
                fputc(',', fp);
                if (next[d] == 0) fputs(GM_UNAVAILABLE, fp);
                else write_value(fp, &devices[d].metrics[m], devices[d].metrics[m].sample(next[d] - 1));
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
    pid_t pid = start_workload(command, ctx_mask);
    if (pid < 0) {
        perror("[ERROR] fork");
        return 1;
    }
    if (start_samplers(ctx_devices, ctx_count, ctx_config->interval_ms, &origin) != 0) {
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
        return 1;
    }
    int status = wait_workload(pid, ctx_devices, ctx_count, ctx_timeout_s);
    double workload_ms = elapsed_ms_since(&origin);
    stop_samplers(ctx_devices, ctx_count);
    if (status != 0) {
        fprintf(stderr, "[ERROR] The workload failed (status %d); no CSV was written.\n", status);
        return 1;
    }
    result = dump_over_time(output, ctx_config->language, ctx_config->program, powercap,
                            ctx_devices, ctx_count, ctx_config->interval_ms, workload_ms) == 0 ? 0 : 1;
    return result;
}

/* Standard mode has no sampling threads: counters report end - start and gauges the
   reading taken when the workload ends. */
static double summarize(const GmMetric *metric, double start_value, double end_value) {
    if (metric->kind == GM_KIND_GAUGE) return end_value;
    double delta = end_value - start_value;
    return start_value < 0.0 || end_value < 0.0 || delta < 0.0 ? -1.0 : delta;
}

/* Runs the workload once and appends one row to fp. */
static int measure_standard(const char *command, FILE *fp, const char *language,
                            const char *program, int powercap, unsigned timeout_s) {
    static double start_values[GM_MAX_DEVICES][GM_MAX_METRICS_PER_DEVICE];
    static double end_values[GM_MAX_DEVICES][GM_MAX_METRICS_PER_DEVICE];
    struct timespec origin;
    for (size_t d = 0; d < ctx_count; d++)
        for (size_t m = 0; device_active[d] && m < ctx_devices[d].metric_count; m++)
            if (!device_unavailable[d] && selected_mask[d][m] &&
                ctx_devices[d].metrics[m].kind == GM_KIND_COUNTER)
                start_values[d][m] = ctx_devices[d].metrics[m].read();
    clock_gettime(CLOCK_MONOTONIC, &origin);
    pid_t pid = start_workload(command, ctx_mask);
    if (pid < 0) {
        perror("[ERROR] fork");
        return 1;
    }
    int status = wait_workload(pid, ctx_devices, ctx_count, timeout_s);
    double workload_ms = elapsed_ms_since(&origin);
    for (size_t d = 0; d < ctx_count; d++)
        for (size_t m = 0; device_active[d] && m < ctx_devices[d].metric_count; m++)
            if (!device_unavailable[d] && selected_mask[d][m])
                end_values[d][m] = ctx_devices[d].metrics[m].read();
    if (status != 0) {
        fprintf(stderr, "[ERROR] The workload failed (status %d).\n", status);
        return 1;
    }
    fprintf(fp, "%s,%s,%d,%.6f", language, program, powercap, workload_ms);
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

/* Called by the device libraries' powercap calibration: measures the workload ntimes in
   standard mode at the given powercap and appends the rows to measurements_file. */
int runTesting(int ntimes, int core, const char *command, const char *language,
               const char *program, const char *measurements_file, int powercap,
               int time_out_limit, float temp_threshold, int variance, int memory) {
    (void)core;
    (void)memory;
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

    FILE *fp = open_output(measurements_file, "Time_ms", ctx_devices, ctx_count);
    if (!fp) return -1;
    int result = 0;
    for (int i = 0; i < ntimes && result == 0; i++) {
        if (wait_for_temperature((double)temp_threshold, variance < 0 ? 0 : (unsigned)variance) != 0 ||
            measure_standard(command, fp, language, program, powercap, timeout_s) != 0)
            result = -1;
        if (result == 0 && i + 1 < ntimes && ctx_config->sleep_seconds)
            sleep(ctx_config->sleep_seconds);
    }
    if (fclose(fp) != 0) result = -1;
    return result;
}

/* Applies each powercap in turn, restoring the saved state as soon as its measurement ends. */
static int run_over_time(const char *command, const char *output, double baseline) {
    int result = 0;
    for (size_t c = 0; c < ctx_config->powercap_count && result == 0; c++) {
        int powercap = ctx_config->powercaps[c];
        if (apply_powercap(powercap) != 0) return 1;
        for (unsigned run = 0; run < ctx_config->repetitions && result == 0; run++) {
            if (wait_for_temperature(baseline, ctx_config->variance) != 0) result = 1;
            if (result == 0) result = run_over_time_once(command, output, powercap);
            if (result == 0 && run + 1 < ctx_config->repetitions && ctx_config->sleep_seconds)
                sleep(ctx_config->sleep_seconds);
        }
        if (release_powercap() != 0) result = 1;
        if (result == 0 && c + 1 < ctx_config->powercap_count) sleep(1);
    }
    return result;
}

static int run_standard(const char *command, const char *output, double baseline) {
    FILE *fp = open_output(output, "Time_ms", ctx_devices, ctx_count);
    if (!fp) return 1;
    int result = 0;
    for (size_t c = 0; c < ctx_config->powercap_count && result == 0; c++) {
        int powercap = ctx_config->powercaps[c];
        if (apply_powercap(powercap) != 0) {
            result = 1;
            break;
        }
        for (unsigned run = 0; run < ctx_config->repetitions && result == 0; run++) {
            if (wait_for_temperature(baseline, ctx_config->variance) != 0 ||
                measure_standard(command, fp, ctx_config->language, ctx_config->program,
                                 powercap, ctx_timeout_s) != 0)
                result = 1;
            if (result == 0 && run + 1 < ctx_config->repetitions && ctx_config->sleep_seconds)
                sleep(ctx_config->sleep_seconds);
        }
        if (release_powercap() != 0) result = 1;
    }
    if (fclose(fp) != 0) result = 1;
    return result;
}

/* Summarizes a powercap sweep with the Python post-processor when the CSV has the columns it
   needs (a Package energy metric and Time_ms). */
static void run_powercap_processor(const char *csv) {
#ifdef GM_PROCESSOR
    char header[8192] = "";
    FILE *input = fopen(csv, "r");
    if (input) {
        if (!fgets(header, sizeof(header), input)) header[0] = '\0';
        fclose(input);
    }
    if (!strstr(header, ".Package_") || !strstr(header, "Time_ms")) {
        printf("[INFO] Powercap sweep saved to %s. The speedup/greenup summary needs a Package "
               "energy metric, so it was skipped.\n", csv);
        return;
    }
    printf("[INFO] Processing calibration CSV via: python3 %s\n", GM_PROCESSOR);
    pid_t child = fork();
    if (child == 0) {
        execlp("python3", "python3", GM_PROCESSOR, csv, (char *)NULL);
        _exit(127);
    }
    if (child < 0) {
        perror("[WARNING] Unable to start calibration post-processing");
        return;
    }
    int status;
    pid_t waited;
    do { waited = waitpid(child, &status, 0); } while (waited < 0 && errno == EINTR);
    if (waited < 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0)
        fprintf(stderr, "[WARNING] Calibration post-processing script exited with error.\n");
#else
    printf("[INFO] Powercap sweep saved to %s.\n", csv);
#endif
}

static int run_powercap_calibration(const char *command, const char *csv, int min_powercap,
                                    int max_powercap, double baseline, unsigned timeout_s) {
    if (calibration_dev < 0) {
        fprintf(stderr, "[ERROR] No device declares powercap_calibration.\n");
        return 1;
    }
    int result = ctx_devices[calibration_dev].powercap_calibrate(
        (int)ctx_config->repetitions, 0, command, ctx_config->language, ctx_config->program, csv,
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

static void show_help(const char *bin, const GmFlag *flags, size_t flag_count) {
    printf("Usage: %s [--over-time|--standard] [-command <cmd>] [-language <lang>] "
           "[-program <name>] [-output <csv>]\n"
           "          [-interval-ms <ms>] [-n_times <N>] [-sleep-secs <s>] [-powercap <W[,W...]>]\n"
           "          [-variance <C>] [-calib-file <path>] [-time_out_limit <hours>]\n"
           "          [--no-metrics] [metric flags]\n"
           "       %s --temperature-calibration [-sleep-secs <s>] [-calib-file <path>]\n"
           "       %s --powercap-calibration -range <min>-<max> -command <cmd> [-compile_cmd <cmd>]\n"
           "          [-n_times <N>] [-variance <C>] [-time_out_limit <hours>] [-output <csv>]\n\n"
           "Metric flags (when none is given, the ones declared in the TOML are used):\n",
           bin, bin, bin);
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

typedef enum { ACTION_MEASURE, ACTION_TEMPERATURE, ACTION_POWERCAP } Action;

int gm_main(int argc, char **argv, const GmConfig *defaults,
            const GmDevice *devices, size_t device_count,
            const GmFlag *flags, size_t flag_count) {
    GmConfig config = *defaults;
    const char *command = defaults->command, *output = defaults->output;
    const char *compile_command = defaults->compile_command;
    const char *calibration_file = defaults->calibration_file;
    const char *calibration_csv = "powercap_calibration.csv";
    int metric_flags_given = 0, min_powercap = -1, max_powercap = -1;
    unsigned timeout_hours = defaults->timeout_hours;
    Action action = !strcmp(config.mode, "temperature-calibration") ?
                    ACTION_TEMPERATURE : ACTION_MEASURE;
    static unsigned char chosen[GM_MAX_DEVICES][GM_MAX_METRICS_PER_DEVICE];
    static int powercaps[GM_MAX_POWERCAPS];

    if (device_count > GM_MAX_DEVICES) {
        fprintf(stderr, "[ERROR] At most %d devices are supported.\n", GM_MAX_DEVICES);
        return 1;
    }
    for (size_t d = 0; d < device_count; d++)
        if (devices[d].metric_count > GM_MAX_METRICS_PER_DEVICE) {
            fprintf(stderr, "[ERROR] At most %d metrics per device are supported.\n",
                    GM_MAX_METRICS_PER_DEVICE);
            return 1;
        }

    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];
        int has_value = i + 1 < argc;
        if (!strcmp(arg, "--help") || !strcmp(arg, "-h")) {
            show_help(argv[0], flags, flag_count);
            return 0;
        } else if (!strcmp(arg, "--over-time")) config.mode = "over-time";
        else if (!strcmp(arg, "--standard")) config.mode = "standard";
        else if (!strcmp(arg, "--temperature-calibration")) action = ACTION_TEMPERATURE;
        else if (!strcmp(arg, "--powercap-calibration")) action = ACTION_POWERCAP;
        else if (!strcmp(arg, "--no-metrics")) metric_flags_given = 1;
        else if ((!strcmp(arg, "-command") || !strcmp(arg, "-run_cmd")) && has_value)
            command = argv[++i];
        else if (!strcmp(arg, "-compile_cmd") && has_value) compile_command = argv[++i];
        else if (!strcmp(arg, "-language") && has_value) config.language = argv[++i];
        else if (!strcmp(arg, "-program") && has_value) config.program = argv[++i];
        else if (!strcmp(arg, "-output") && has_value) {
            output = argv[++i];
            calibration_csv = output;
        } else if (!strcmp(arg, "-calib-file") && has_value) calibration_file = argv[++i];
        else if (!strcmp(arg, "-range") && has_value) {
            char trailing;
            if (sscanf(argv[++i], "%d-%d%c", &min_powercap, &max_powercap, &trailing) != 2) {
                fprintf(stderr, "[ERROR] Invalid -range; expected <min>-<max> in watts.\n");
                return 1;
            }
        } else if (!strcmp(arg, "-interval-ms") && has_value) {
            if (parse_unsigned(argv[++i], 1, 4294967u, &config.interval_ms) != 0) {
                fprintf(stderr, "[ERROR] -interval-ms must be between 1 and 4294967.\n");
                return 1;
            }
        } else if (!strcmp(arg, "-n_times") && has_value) {
            if (parse_unsigned(argv[++i], 1, 1000000u, &config.repetitions) != 0) {
                fprintf(stderr, "[ERROR] -n_times must be a positive integer.\n");
                return 1;
            }
        } else if ((!strcmp(arg, "-sleep-secs") || !strcmp(arg, "-sleep_time") ||
                    !strcmp(arg, "-secs_to_sleep")) && has_value) {
            if (parse_unsigned(argv[++i], 0, 86400u, &config.sleep_seconds) != 0) {
                fprintf(stderr, "[ERROR] -sleep-secs must be between 0 and 86400.\n");
                return 1;
            }
        } else if (!strcmp(arg, "-variance") && has_value) {
            if (parse_unsigned(argv[++i], 0, 1000u, &config.variance) != 0) {
                fprintf(stderr, "[ERROR] -variance must be between 0 and 1000.\n");
                return 1;
            }
        } else if (!strcmp(arg, "-time_out_limit") && has_value) {
            if (parse_unsigned(argv[++i], 0, 1000u, &timeout_hours) != 0) {
                fprintf(stderr, "[ERROR] -time_out_limit must be between 0 and 1000 hours.\n");
                return 1;
            }
        } else if (!strcmp(arg, "-powercap") && has_value) {
            int count = parse_powercaps(argv[++i], powercaps, GM_MAX_POWERCAPS);
            if (count < 0) {
                fprintf(stderr, "[ERROR] -powercap must be a comma-separated list of unique "
                                "values, each -1 (saved baseline) or at least 1 W.\n");
                return 1;
            }
            config.powercaps = powercaps;
            config.powercap_count = (size_t)count;
        } else {
            size_t f = 0;
            while (f < flag_count && strcmp(arg, flags[f].flag) != 0) f++;
            if (f == flag_count) {
                fprintf(stderr, "[ERROR] Unknown or incomplete argument '%s'.\n", arg);
                return 1;
            }
            chosen[flags[f].device][flags[f].metric] = 1;
            metric_flags_given = 1;
        }
    }
    if (!metric_flags_given)
        for (size_t f = 0; f < flag_count; f++)
            if (flags[f].default_on) chosen[flags[f].device][flags[f].metric] = 1;

    memcpy(selected_mask, chosen, sizeof(selected_mask));
    for (size_t d = 0; d < device_count; d++)
        for (size_t m = 0; m < devices[d].metric_count; m++)
            if (selected_mask[d][m]) device_active[d] = 1;

    if (action != ACTION_TEMPERATURE && (!command || !*command)) {
        fprintf(stderr, "[ERROR] No command to run; use -command or measurement.command.\n");
        return 1;
    }
    if (action == ACTION_POWERCAP && (min_powercap < 1 || max_powercap < min_powercap)) {
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
    signal(SIGINT, on_sigint);
    ctx_devices = devices;
    ctx_count = device_count;
    ctx_mask = &old_mask;
    ctx_config = &config;
    ctx_timeout_s = action == ACTION_POWERCAP && timeout_hours == 0 ? 3600u : timeout_hours * 3600u;

    int result = 1;
    double baseline;
    if (setup_devices(devices, device_count) == 0) {
        if (action == ACTION_TEMPERATURE) {
            result = calibrate_temperature(calibration_file, config.sleep_seconds) == 0 ? 0 : 1;
        } else if (resolve_baseline(calibration_file, config.sleep_seconds, &baseline) == 0) {
            if (action == ACTION_POWERCAP)
                result = run_powercap_calibration(command, calibration_csv, min_powercap,
                                                  max_powercap, baseline, ctx_timeout_s);
            else if (!strcmp(config.mode, "standard"))
                result = run_standard(command, output, baseline);
            else
                result = run_over_time(command, output, baseline);
        }
        teardown_devices(devices, device_count);
    }
    sigprocmask(SIG_SETMASK, &old_mask, NULL);
    return result;
}

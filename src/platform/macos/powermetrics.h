#ifndef GREENMEASURER_POWERMETRICS_H
#define GREENMEASURER_POWERMETRICS_H

/* Shared reader for the macOS `powermetrics` tool (Apple Silicon).
 *
 * powermetrics reports average power (mW) per sampling interval. This reader
 * runs it in the background, writes its output to a temporary file, and
 * integrates each reported power over the reported interval to obtain energy.
 * Energy is therefore only known at the granularity of the sampling interval
 * (default 100 ms, GREENMEASURER_POWERMETRICS_INTERVAL_MS to change it).
 *
 * powermetrics requires root. GREENMEASURER_POWERMETRICS may point to another
 * executable that accepts the same options (used by the tests).
 * Each backend that includes this header keeps its own private copy of the
 * state, so the CPU and GPU backends each run one powermetrics process.
 */

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

typedef struct {
    double cpu_j;
    double gpu_j;
    double ane_j;
    unsigned long samples;
} PmEnergy;

static pthread_mutex_t pm_lock = PTHREAD_MUTEX_INITIALIZER;
static PmEnergy pm_total;
static pid_t pm_pid = -1;
static pthread_t pm_thread;
static int pm_thread_started = 0;
static volatile int pm_stop_flag = 0;
static char pm_output_path[128] = "";

static const char *pm_executable(void) {
    const char *override_path = getenv("GREENMEASURER_POWERMETRICS");
    return (override_path && override_path[0]) ? override_path : "/usr/bin/powermetrics";
}

static int pm_interval_ms(void) {
    const char *value = getenv("GREENMEASURER_POWERMETRICS_INTERVAL_MS");
    int interval = value ? atoi(value) : 100;
    return interval >= 10 ? interval : 100;
}

/* Returns NULL when usable, otherwise a reason. */
static const char *pm_unavailable_reason(void) {
#if defined(__APPLE__) && !defined(__arm64__)
    return "only Apple Silicon Macs are supported";
#else
    if (access(pm_executable(), X_OK) != 0) return "powermetrics was not found";
    if (!getenv("GREENMEASURER_POWERMETRICS") && geteuid() != 0) {
        return "powermetrics requires root (run with sudo)";
    }
    return NULL;
#endif
}

/* Parses "(123.45ms elapsed)" from the sample header; returns seconds or 0. */
static double pm_parse_elapsed_seconds(const char *line) {
    const char *marker = strstr(line, "ms elapsed");
    if (!marker) return 0.0;
    const char *open_paren = marker;
    while (open_paren > line && *open_paren != '(') open_paren--;
    double milliseconds = 0.0;
    if (*open_paren != '(' || sscanf(open_paren + 1, "%lf", &milliseconds) != 1) return 0.0;
    return milliseconds > 0.0 ? milliseconds / 1000.0 : 0.0;
}

typedef struct {
    double interval_s;
    int seen_cpu, seen_gpu, seen_ane;
} PmParseState;

static void pm_parse_line(const char *line, PmParseState *state) {
    if (strstr(line, "Sampled system activity")) {
        double elapsed = pm_parse_elapsed_seconds(line);
        state->interval_s = elapsed > 0.0 ? elapsed : pm_interval_ms() / 1000.0;
        state->seen_cpu = state->seen_gpu = state->seen_ane = 0;
        return;
    }

    double milliwatts = 0.0;
    if (!strncmp(line, "CPU Power:", 10) && !state->seen_cpu &&
        sscanf(line + 10, "%lf", &milliwatts) == 1) {
        state->seen_cpu = 1;
        pthread_mutex_lock(&pm_lock);
        pm_total.cpu_j += milliwatts / 1000.0 * state->interval_s;
        pm_total.samples++;
        pthread_mutex_unlock(&pm_lock);
    } else if (!strncmp(line, "GPU Power:", 10) && !state->seen_gpu &&
               sscanf(line + 10, "%lf", &milliwatts) == 1) {
        state->seen_gpu = 1;
        pthread_mutex_lock(&pm_lock);
        pm_total.gpu_j += milliwatts / 1000.0 * state->interval_s;
        pthread_mutex_unlock(&pm_lock);
    } else if (!strncmp(line, "ANE Power:", 10) && !state->seen_ane &&
               sscanf(line + 10, "%lf", &milliwatts) == 1) {
        state->seen_ane = 1;
        pthread_mutex_lock(&pm_lock);
        pm_total.ane_j += milliwatts / 1000.0 * state->interval_s;
        pthread_mutex_unlock(&pm_lock);
    }
}

/* Follows the output file like `tail -f`, handling lines written in pieces. */
static void *pm_reader_thread(void *unused) {
    (void)unused;
    FILE *file = fopen(pm_output_path, "r");
    if (!file) return NULL;

    PmParseState state = { pm_interval_ms() / 1000.0, 0, 0, 0 };
    char line[512];
    size_t length = 0;

    while (!pm_stop_flag) {
        if (!fgets(line + length, (int)(sizeof(line) - length), file)) {
            clearerr(file);
            usleep(10000);
            continue;
        }
        length = strlen(line);
        if (length > 0 && line[length - 1] != '\n' && length < sizeof(line) - 1) {
            continue;
        }
        pm_parse_line(line, &state);
        length = 0;
    }
    fclose(file);
    return NULL;
}

static void pm_shutdown(void) {
    pm_stop_flag = 1;
    if (pm_pid > 0) {
        kill(pm_pid, SIGTERM);
        waitpid(pm_pid, NULL, 0);
        pm_pid = -1;
    }
    if (pm_thread_started) {
        pthread_join(pm_thread, NULL);
        pm_thread_started = 0;
    }
    if (pm_output_path[0]) {
        unlink(pm_output_path);
        pm_output_path[0] = '\0';
    }
}

/* Starts powermetrics once; later calls are no-ops. Returns 0 on success. */
static int pm_start(void) {
    if (pm_pid > 0) return 0;

    snprintf(pm_output_path, sizeof(pm_output_path), "/tmp/greenmeasurer_pm_XXXXXX");
    int fd = mkstemp(pm_output_path);
    if (fd < 0) {
        perror("[ERROR] Unable to create powermetrics output file");
        pm_output_path[0] = '\0';
        return -1;
    }
    close(fd);

    char interval[16];
    snprintf(interval, sizeof(interval), "%d", pm_interval_ms());
    memset(&pm_total, 0, sizeof(pm_total));
    pm_stop_flag = 0;

    pid_t child = fork();
    if (child < 0) {
        perror("[ERROR] Unable to start powermetrics");
        pm_shutdown();
        return -1;
    }
    if (child == 0) {
        int null_fd = open("/dev/null", O_RDWR);
        if (null_fd >= 0) {
            dup2(null_fd, STDOUT_FILENO);
            dup2(null_fd, STDERR_FILENO);
        }
        execl(pm_executable(), "powermetrics", "--samplers", "cpu_power,gpu_power",
              "-i", interval, "-o", pm_output_path, (char *)NULL);
        _exit(127);
    }
    pm_pid = child;

    if (pthread_create(&pm_thread, NULL, pm_reader_thread, NULL) != 0) {
        pm_shutdown();
        return -1;
    }
    pm_thread_started = 1;

    return 0;
}

/* init() must not block (the core starts the CPU and GPU windows one after the
   other), so a failed start is detected here, once no sample has arrived. */
static void pm_report_if_exited(void) {
    int status = 0;
    if (pm_pid > 0 && waitpid(pm_pid, &status, WNOHANG) == pm_pid) {
        pm_pid = -1;
        fprintf(stderr, "[ERROR] powermetrics exited (status %d). Is it running as root?\n",
                WIFEXITED(status) ? WEXITSTATUS(status) : -1);
    }
}

static PmEnergy pm_snapshot(void) {
    pthread_mutex_lock(&pm_lock);
    PmEnergy copy = pm_total;
    pthread_mutex_unlock(&pm_lock);
    return copy;
}

#endif

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <pthread.h>
#include <unistd.h>
#include <signal.h>
#include <string.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <locale.h>
#include <errno.h>
#include <dirent.h>
#include <dlfcn.h>
#include <limits.h>
#include <sys/stat.h>
#include <sys/types.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#define BACKEND_PLATFORM_DIR "platform/macos"
#define BACKEND_INSTALLED_DIR "../lib/greenmeasurer/platform/macos"
#else
#define BACKEND_PLATFORM_DIR "platform/linux"
#define BACKEND_INSTALLED_DIR "../lib/greenmeasurer/platform/linux"
#endif

#include "include/energy-backend.h"

typedef struct {
    void *handle;
    const EnergyBackendDescriptor *descriptor;
    int report_index;
} LoadedBackend;

typedef struct {
    const EnergyMetricDescriptor *descriptor;
    EnergyBackendType backend_type;
    size_t backend_metric_index;
} ActiveMetric;

static CpuBackend *active_cpu_backend = NULL;
static GpuBackend *active_gpu_backend = NULL;
static ActiveMetric *active_metrics = NULL;
static size_t active_metric_count = 0;
static LoadedBackend *loaded_backends = NULL;
static size_t loaded_backend_count = 0;
static char backend_root[PATH_MAX];
static int header_only_mode = 0;

/* Configurable calibration and measurement files */
static char calib_file[512] = "/tmp/cores_temperatures.txt";
static char measurements_file[512] = "measurements.csv";

static volatile sig_atomic_t global_terminate = 0;
static void sigint_handler(int signum) { 
    (void)signum; 
    global_terminate = 1; 
}

static struct { int returnValue; } threadData;

/* Standardized CSV metric formatters */
static void write_energy_metric(FILE *fp, double val) {
    if (val < 0.0) {
        fprintf(fp, "NOT_SUPPORTED");
    } else {
        fprintf(fp, "%.18f", val);
    }
}

static double backend_get_metric(EnergyBackendType type, size_t metric_index,
                                pid_t process_id) {
    if (type == ENERGY_BACKEND_CPU && active_cpu_backend &&
        active_cpu_backend->get_metric) {
        return active_cpu_backend->get_metric(metric_index, 0, process_id);
    }
    if (type == ENERGY_BACKEND_GPU && active_gpu_backend &&
        active_gpu_backend->get_metric) {
        return active_gpu_backend->get_metric(metric_index, 0, process_id);
    }
    return METRIC_NOT_SUPPORTED;
}

static double read_active_metric(size_t metric_index, pid_t process_id) {
    if (metric_index >= active_metric_count) return METRIC_NOT_SUPPORTED;
    const ActiveMetric *metric = &active_metrics[metric_index];
    if (metric->descriptor->scope == ENERGY_METRIC_DEVICE) {
        return backend_get_metric(metric->backend_type,
                                  metric->backend_metric_index, 0);
    }
    if (process_id <= 0) return METRIC_NOT_SUPPORTED;
    return backend_get_metric(metric->backend_type,
                              metric->backend_metric_index, process_id);
}

static double read_process_tree_metric(size_t metric_index, pid_t root_pid) {
    if (metric_index >= active_metric_count || root_pid <= 0) {
        return METRIC_NOT_SUPPORTED;
    }
    const ActiveMetric *metric = &active_metrics[metric_index];
    if (metric->descriptor->scope != ENERGY_METRIC_PROCESS) {
        return read_active_metric(metric_index, 0);
    }

    enum { MAX_PROCESS_TREE_SIZE = 1024 };
    pid_t process_ids[MAX_PROCESS_TREE_SIZE];
    pid_t pending_ids[MAX_PROCESS_TREE_SIZE];
    size_t process_count = 0;
    size_t pending_count = 0;
    pending_ids[pending_count++] = root_pid;

    while (pending_count > 0 && process_count < MAX_PROCESS_TREE_SIZE) {
        pid_t process_id = pending_ids[--pending_count];
        int already_seen = 0;
        for (size_t i = 0; i < process_count; i++) {
            if (process_ids[i] == process_id) {
                already_seen = 1;
                break;
            }
        }
        if (already_seen) continue;

        process_ids[process_count++] = process_id;
        char children_path[128];
        int written = snprintf(children_path, sizeof(children_path),
                               "/proc/%d/task/%d/children", process_id, process_id);
        if (written < 0 || (size_t)written >= sizeof(children_path)) continue;

        FILE *children_file = fopen(children_path, "r");
        if (!children_file) continue;

        int child_id;
        while (pending_count < MAX_PROCESS_TREE_SIZE &&
               fscanf(children_file, "%d", &child_id) == 1) {
            if (child_id > 0) pending_ids[pending_count++] = (pid_t)child_id;
        }
        fclose(children_file);
    }

    double total_memory_mib = 0.0;
    int found_memory = 0;
    for (size_t i = 0; i < process_count; i++) {
        double process_memory = read_active_metric(metric_index, process_ids[i]);
        if (process_memory >= 0.0) {
            total_memory_mib += process_memory;
            found_memory = 1;
        }
    }
    return found_memory ? total_memory_mib : METRIC_NOT_SUPPORTED;
}

static double get_temperature_metric(EnergyBackendType type, unsigned int flag) {
    for (size_t i = 0; i < active_metric_count; i++) {
        if (active_metrics[i].backend_type == type &&
            (active_metrics[i].descriptor->flags & flag) != 0) {
            return read_active_metric(i, 0);
        }
    }
    return METRIC_NOT_SUPPORTED;
}

void energy_backend_write_csv_header(FILE *fp, int overtime) {
    fprintf(fp, "Language,Program,Powercap,%s", overtime ? "Timestamp_ms" : "Time_ms");
    if (!overtime) fprintf(fp, ",CPU_Memory_KB");
    for (size_t i = 0; i < active_metric_count; i++) {
        const ActiveMetric *metric = &active_metrics[i];
        const char *category = metric->backend_type == ENERGY_BACKEND_CPU ? "cpu" : "gpu";
        fprintf(fp, ",%s.%s_%s", category, metric->descriptor->name,
                metric->descriptor->unit);
    }
    fputc('\n', fp);
}

static void write_active_metric_value(FILE *fp, double value) {
    write_energy_metric(fp, value);
}

static void writeErrorMessage(const char *language, const char *program, const char *errorMsg, 
                              const char *out_file, int powercap) {
    FILE *fp = fopen(out_file, "a");
    if (!fp) return;
    fprintf(fp, "%s, %s, %d, %s, %s", language, program, powercap,
            errorMsg, errorMsg);
    for (size_t i = 0; i < active_metric_count; i++) {
        fprintf(fp, ", %s", errorMsg);
    }
    fputc('\n', fp);
    fclose(fp);
}

static float get_current_cpu_temp(int core) {
    (void)core;
    return (float)get_temperature_metric(ENERGY_BACKEND_CPU,
                                        ENERGY_METRIC_FLAG_CPU_TEMPERATURE);
}

static float getInitialTemperature(const char *calibration_file) {
    if (!calibration_file || strlen(calibration_file) == 0) return -1.0f;

    FILE *file = fopen(calibration_file, "r");
    if (!file) return -1.0f;

    char line[256];
    float result = -1.0f;

    while (fgets(line, sizeof(line), file)) {
        if (strstr(line, "Average temperature:") != NULL) {
            char *colon = strchr(line, ':');
            if (colon) {
                char *comma = strchr(colon, ',');
                if (comma) *comma = '.';

                float val = 0.0f;
                if (sscanf(colon + 1, "%f", &val) == 1) {
                    result = val;
                    break;
                }
            }
        }
    }
    fclose(file);
    return result;
}

static double calibrateTemperature(const char *target_calib_file, int sleep_seconds) {
    printf("[INFO] Sleeping %d seconds to stabilize and calibrate CPU temperature...\n", sleep_seconds);
    if (sleep_seconds > 0) {
        sleep(sleep_seconds);
    }

    /* Save a snapshot to the calibration file. */
    char dump_cmd[600];
    snprintf(dump_cmd, sizeof(dump_cmd), "sensors > %s 2>/dev/null || true", target_calib_file);
    system(dump_cmd);

    float mean_temp = get_current_cpu_temp(0);
    if (mean_temp <= 0.0f) {
        fprintf(stderr, "[WARNING] Sensor read returned <= 0.0, defaulting baseline to 40.0°C\n");
        mean_temp = 40.0f;
    }

    FILE *output_file = fopen(target_calib_file, "a");
    if (output_file != NULL) {
        fprintf(output_file, "Average temperature: %.1f°C\n\n", mean_temp);
        fclose(output_file);
        printf("[INFO] Calibrated baseline temperature saved to %s: %.1f°C\n", target_calib_file, mean_temp);
    } else {
        perror("[ERROR] Failed to write calibration file");
    }

    return (double)mean_temp;
}

#define BACKEND_DIR_ENV "GREENMEASURER_BACKEND_DIR"
#define MAX_BACKEND_REPORTS 64

typedef struct {
    const char *category;
    char label[128];
    const char *status;
    char detail[256];
} BackendReport;

static BackendReport backend_reports[MAX_BACKEND_REPORTS];
static size_t backend_report_count = 0;

static int add_backend_report(const char *category, const char *label,
                              const char *status, const char *detail) {
    if (backend_report_count >= MAX_BACKEND_REPORTS) return -1;
    BackendReport *report = &backend_reports[backend_report_count];
    report->category = category;
    snprintf(report->label, sizeof(report->label), "%s", label);
    report->status = status;
    snprintf(report->detail, sizeof(report->detail), "%s", detail ? detail : "");
    return (int)backend_report_count++;
}

static int is_directory(const char *path) {
    struct stat status;
    return stat(path, &status) == 0 && S_ISDIR(status.st_mode);
}

/* Backend lookup order: $GREENMEASURER_BACKEND_DIR, <exe dir>/platform/<linux|macos>,
   <exe dir>/../lib/greenmeasurer/platform/<linux|macos>. Each root contains cpu/ and gpu/. */
static int get_backend_root(void) {
    const char *override_dir = getenv(BACKEND_DIR_ENV);
    if (override_dir && override_dir[0] != '\0') {
        if (!is_directory(override_dir) ||
            snprintf(backend_root, sizeof(backend_root), "%s", override_dir) >= (int)sizeof(backend_root)) {
            fprintf(stderr, "[ERROR] %s is not a usable directory: %s\n", BACKEND_DIR_ENV, override_dir);
            return -1;
        }
        return 0;
    }

    char executable_path[PATH_MAX];
#ifdef __APPLE__
    uint32_t path_size = sizeof(executable_path);
    if (_NSGetExecutablePath(executable_path, &path_size) != 0) {
        fprintf(stderr, "[ERROR] Unable to resolve executable path.\n");
        return -1;
    }
#else
    ssize_t length = readlink("/proc/self/exe", executable_path, sizeof(executable_path) - 1);
    if (length < 0 || (size_t)length >= sizeof(executable_path) - 1) {
        perror("[ERROR] Unable to resolve executable path");
        return -1;
    }

    executable_path[length] = '\0';
#endif
    char *separator = strrchr(executable_path, '/');
    if (!separator) {
        fprintf(stderr, "[ERROR] Executable path has no parent directory.\n");
        return -1;
    }
    *separator = '\0';

    static const char *const relative_roots[] = {
        BACKEND_PLATFORM_DIR,
        BACKEND_INSTALLED_DIR
    };
    for (size_t i = 0; i < sizeof(relative_roots) / sizeof(relative_roots[0]); i++) {
        char candidate[PATH_MAX];
        int written = snprintf(candidate, sizeof(candidate), "%s/%s", executable_path, relative_roots[i]);
        if (written > 0 && (size_t)written < sizeof(candidate) && is_directory(candidate)) {
            snprintf(backend_root, sizeof(backend_root), "%s", candidate);
            return 0;
        }
    }

    fprintf(stderr, "[ERROR] No backend directory found next to %s.\n"
                    "[HINT] Build the project with 'make' or set %s.\n",
            executable_path, BACKEND_DIR_ENV);
    return -1;
}

static int backend_is_valid(const EnergyBackendDescriptor *descriptor,
                            EnergyBackendType expected_type) {
    if (!descriptor || descriptor->type != expected_type) return 0;

    const char *name = NULL;
    const EnergyMetricDescriptor *metrics = NULL;
    size_t metric_count = 0;
    int callbacks_valid = 0;
    if (expected_type == ENERGY_BACKEND_CPU) {
        CpuBackend *backend = descriptor->backend.cpu;
        if (!backend) return 0;
        name = backend->name;
        metrics = backend->metrics;
        metric_count = backend->metric_count;
        callbacks_valid = backend->detect && backend->init &&
                          backend->sample_start && backend->sample_stop &&
                          backend->get_metric;
    } else {
        GpuBackend *backend = descriptor->backend.gpu;
        if (!backend) return 0;
        name = backend->name;
        metrics = backend->metrics;
        metric_count = backend->metric_count;
        callbacks_valid = backend->detect && backend->init &&
                          backend->sample_start && backend->sample_stop &&
                          backend->get_metric;
    }

    if (!name || !callbacks_valid || !metrics || metric_count == 0) return 0;
    for (size_t i = 0; i < metric_count; i++) {
        const EnergyMetricDescriptor *metric = &metrics[i];
        if (!metric->name || !metric->unit || !metric->name[0] ||
            !metric->unit[0] ||
            (metric->kind != ENERGY_METRIC_WINDOW &&
             metric->kind != ENERGY_METRIC_GAUGE &&
             metric->kind != ENERGY_METRIC_PEAK) ||
            (metric->scope != ENERGY_METRIC_DEVICE &&
             metric->scope != ENERGY_METRIC_PROCESS) ||
            (metric->flags & ~(ENERGY_METRIC_FLAG_CPU_TEMPERATURE |
                               ENERGY_METRIC_FLAG_GPU_TEMPERATURE)) != 0 ||
            (expected_type == ENERGY_BACKEND_CPU &&
             (metric->flags & ENERGY_METRIC_FLAG_GPU_TEMPERATURE)) ||
            (expected_type == ENERGY_BACKEND_GPU &&
             (metric->flags & ENERGY_METRIC_FLAG_CPU_TEMPERATURE))) {
            return 0;
        }
        for (const char *character = metric->name; *character; character++) {
            if (!( (*character >= 'a' && *character <= 'z') ||
                   (*character >= 'A' && *character <= 'Z') ||
                   (*character >= '0' && *character <= '9') ||
                   *character == '_' || *character == '-')) {
                return 0;
            }
        }
        for (const char *character = metric->unit; *character; character++) {
            if (*character == ',' || *character == '"' ||
                *character == '\n' || *character == '\r') {
                return 0;
            }
        }
        for (size_t previous = 0; previous < i; previous++) {
            if (strcmp(metric->name, metrics[previous].name) == 0) return 0;
        }
    }
    return 1;
}

static int register_selected_metrics(void) {
    size_t cpu_count = active_cpu_backend ? active_cpu_backend->metric_count : 0;
    size_t gpu_count = active_gpu_backend ? active_gpu_backend->metric_count : 0;
    active_metric_count = cpu_count + gpu_count;
    if (active_metric_count == 0) return 0;

    active_metrics = calloc(active_metric_count, sizeof(*active_metrics));
    if (!active_metrics) {
        fprintf(stderr, "[ERROR] Unable to allocate active metric registry.\n");
        return -1;
    }

    size_t output_index = 0;
    for (size_t i = 0; i < cpu_count; i++) {
        active_metrics[output_index++] = (ActiveMetric) {
            .descriptor = &active_cpu_backend->metrics[i],
            .backend_type = ENERGY_BACKEND_CPU,
            .backend_metric_index = i
        };
    }
    for (size_t i = 0; i < gpu_count; i++) {
        active_metrics[output_index++] = (ActiveMetric) {
            .descriptor = &active_gpu_backend->metrics[i],
            .backend_type = ENERGY_BACKEND_GPU,
            .backend_metric_index = i
        };
    }
    return 0;
}

static int load_backend_plugin(const char *plugin_path, const char *category,
                               const char *vendor_dir, EnergyBackendType expected_type) {
    void *handle = dlopen(plugin_path, RTLD_NOW | RTLD_LOCAL);
    if (!handle) {
        add_backend_report(category, vendor_dir, "load failed", dlerror());
        return -1;
    }

    dlerror();
    const EnergyBackendDescriptor *descriptor =
        (const EnergyBackendDescriptor *)dlsym(handle, ENERGY_BACKEND_DESCRIPTOR_SYMBOL);
    const char *symbol_error = dlerror();
    if (symbol_error || !backend_is_valid(descriptor, expected_type)) {
        add_backend_report(category, vendor_dir, "invalid",
                           symbol_error ? symbol_error : "invalid descriptor or missing callbacks");
        dlclose(handle);
        return -1;
    }

    LoadedBackend *new_backends = realloc(
        loaded_backends, (loaded_backend_count + 1) * sizeof(*loaded_backends));
    if (!new_backends) {
        fprintf(stderr, "[ERROR] Unable to allocate backend registry.\n");
        dlclose(handle);
        return -1;
    }

    const char *name = expected_type == ENERGY_BACKEND_CPU
        ? descriptor->backend.cpu->name : descriptor->backend.gpu->name;
    loaded_backends = new_backends;
    loaded_backends[loaded_backend_count].handle = handle;
    loaded_backends[loaded_backend_count].descriptor = descriptor;
    loaded_backends[loaded_backend_count].report_index = add_backend_report(category, name, "loaded", "");
    loaded_backend_count++;
    return 0;
}

static int load_backend_category(const char *category, EnergyBackendType type) {
    char category_path[PATH_MAX];
    int written = snprintf(category_path, sizeof(category_path), "%s/%s", backend_root, category);
    if (written < 0 || (size_t)written >= sizeof(category_path)) {
        fprintf(stderr, "[ERROR] Backend category path is too long.\n");
        return -1;
    }

    struct dirent **entries = NULL;
    int entry_count = scandir(category_path, &entries, NULL, alphasort);
    if (entry_count < 0) {
        if (errno != ENOENT) perror("[WARNING] Unable to scan backend directory");
        return 0;
    }

    for (int i = 0; i < entry_count; i++) {
        if (entries[i]->d_name[0] == '.') {
            free(entries[i]);
            continue;
        }

        char vendor_path[PATH_MAX];
        char plugin_path[PATH_MAX];
        written = snprintf(vendor_path, sizeof(vendor_path), "%s/%s",
                           category_path, entries[i]->d_name);
        if (written < 0 || (size_t)written >= sizeof(vendor_path) || !is_directory(vendor_path)) {
            free(entries[i]);
            continue;
        }

        written = snprintf(plugin_path, sizeof(plugin_path), "%s/backend.so", vendor_path);
        if (written >= 0 && (size_t)written < sizeof(plugin_path)) {
            if (access(plugin_path, R_OK) == 0) {
                load_backend_plugin(plugin_path, category, entries[i]->d_name, type);
            } else {
                add_backend_report(category, entries[i]->d_name, "not built",
                                   "backend.so missing (skipped or failed at build time)");
            }
        }
        free(entries[i]);
    }
    free(entries);
    return 0;
}

static void cleanup_backends(void) {
    if (active_cpu_backend && active_cpu_backend->cleanup) {
        active_cpu_backend->cleanup();
    }
    if (active_gpu_backend && active_gpu_backend->cleanup) {
        active_gpu_backend->cleanup();
    }
    for (size_t i = 0; i < loaded_backend_count; i++) {
        dlclose(loaded_backends[i].handle);
    }
    free(loaded_backends);
    loaded_backends = NULL;
    loaded_backend_count = 0;
    free(active_metrics);
    active_metrics = NULL;
    active_metric_count = 0;
}

/* Backends that do not apply to this machine are hidden unless the category
   has no selected backend (to explain why) or verbose output is requested. */
static void print_backend_summary(void) {
    const char *verbose_env = getenv("GREENMEASURER_VERBOSE_BACKENDS");
    int verbose = verbose_env && verbose_env[0] && strcmp(verbose_env, "0") != 0;

    printf("[INFO] Energy backends (%s)\n", backend_root);
    printf("  %-4s  %-28s  %-13s  %s\n", "TYPE", "BACKEND", "STATUS", "DETAILS");
    printf("  %-4s  %-28s  %-13s  %s\n", "----", "----------------------------", "-------------", "-------");
    for (size_t i = 0; i < backend_report_count; i++) {
        const BackendReport *report = &backend_reports[i];
        int show = verbose || strcmp(report->status, "selected") == 0;
        if (!show) {
            int category_has_selected = 0;
            for (size_t j = 0; j < backend_report_count; j++) {
                if (strcmp(backend_reports[j].category, report->category) == 0 &&
                    strcmp(backend_reports[j].status, "selected") == 0) {
                    category_has_selected = 1;
                    break;
                }
            }
            show = !category_has_selected || strcmp(report->status, "invalid") == 0;
        }
        if (show) {
            printf("  %-4s  %-28s  %-13s  %s\n",
                   report->category, report->label, report->status, report->detail);
        }
    }
}

static void init_backends(void) {
    if (get_backend_root() != 0) exit(EXIT_FAILURE);
    if (load_backend_category("cpu", ENERGY_BACKEND_CPU) != 0 ||
        load_backend_category("gpu", ENERGY_BACKEND_GPU) != 0) {
        exit(EXIT_FAILURE);
    }
    if (atexit(cleanup_backends) != 0) {
        fprintf(stderr, "[ERROR] Unable to register backend cleanup.\n");
        exit(EXIT_FAILURE);
    }

    int cpu_priority = INT_MIN, gpu_priority = INT_MIN;
    int cpu_report = -1, gpu_report = -1;
    for (size_t i = 0; i < loaded_backend_count; i++) {
        const EnergyBackendDescriptor *descriptor = loaded_backends[i].descriptor;
        int is_cpu = descriptor->type == ENERGY_BACKEND_CPU;
        int detected = is_cpu ? descriptor->backend.cpu->detect() == 1
                              : descriptor->backend.gpu->detect() == 1;
        BackendReport *report = &backend_reports[loaded_backends[i].report_index];

        if (!detected) {
            const char *hint = is_cpu
                ? (descriptor->backend.cpu->diagnose ? descriptor->backend.cpu->diagnose() : NULL)
                : (descriptor->backend.gpu->diagnose ? descriptor->backend.gpu->diagnose() : NULL);
            report->status = "not detected";
            snprintf(report->detail, sizeof(report->detail), "%s", hint ? hint : "hardware not found");
            continue;
        }

        report->status = "available";
        if (is_cpu && descriptor->priority > cpu_priority) {
            active_cpu_backend = descriptor->backend.cpu;
            cpu_priority = descriptor->priority;
            cpu_report = loaded_backends[i].report_index;
        } else if (!is_cpu && descriptor->priority > gpu_priority) {
            active_gpu_backend = descriptor->backend.gpu;
            gpu_priority = descriptor->priority;
            gpu_report = loaded_backends[i].report_index;
        }
    }
    if (cpu_report >= 0) backend_reports[cpu_report].status = "selected";
    if (gpu_report >= 0) backend_reports[gpu_report].status = "selected";

    if (register_selected_metrics() != 0) exit(EXIT_FAILURE);
    if (!header_only_mode) {
        print_backend_summary();
        printf("[INFO] Active backend metrics:");
        for (size_t i = 0; i < active_metric_count; i++) {
            const char *category =
                active_metrics[i].backend_type == ENERGY_BACKEND_CPU ? "cpu" : "gpu";
            printf(" %s.%s (%s)", category, active_metrics[i].descriptor->name,
                   active_metrics[i].descriptor->unit);
        }
        printf("\n");
    }

    if (!active_cpu_backend && !active_gpu_backend) {
        fprintf(stderr, "[ERROR] No usable CPU or GPU energy backend. See the table above for the reason of each backend.\n");
        exit(EXIT_FAILURE);
    }
    if (!active_cpu_backend) {
        fprintf(stderr, "[WARNING] No CPU energy backend active. CPU metrics will be reported as NOT_SUPPORTED.\n");
    }
    if (!active_gpu_backend) {
        fprintf(stderr, "[WARNING] No GPU energy backend active. GPU metrics will be reported as NOT_SUPPORTED.\n");
    }
    if (!header_only_mode) printf("\n");
}

static pid_t startCommand(const char *command) {
    pid_t child_pid = fork();
    if (child_pid != 0) return child_pid;

    execl("/bin/sh", "sh", "-c", command, (char *)NULL);
    perror("[ERROR] Unable to start workload command");
    _exit(127);
}

typedef struct {
    pid_t root_pid;
    pthread_mutex_t stop_mutex;
    pthread_cond_t stop_condition;
    int stop;
    double *peak_values;
} VramMonitorContext;

static void* monitor_vram_usage(void *arg) {
    VramMonitorContext *context = (VramMonitorContext *)arg;
    const struct timespec poll_delay = {.tv_sec = 0, .tv_nsec = 50000000L};

    for (;;) {
        for (size_t i = 0; i < active_metric_count; i++) {
            if (active_metrics[i].descriptor->kind != ENERGY_METRIC_PEAK) continue;
            double current_value =
                read_process_tree_metric(i, context->root_pid);
            if (current_value >= 0.0 &&
                (context->peak_values[i] < 0.0 ||
                 current_value > context->peak_values[i])) {
                context->peak_values[i] = current_value;
            }
        }

        struct timespec wake_time;
        clock_gettime(CLOCK_REALTIME, &wake_time);
        wake_time.tv_nsec += poll_delay.tv_nsec;
        if (wake_time.tv_nsec >= 1000000000L) {
            wake_time.tv_nsec -= 1000000000L;
            wake_time.tv_sec++;
        }

        pthread_mutex_lock(&context->stop_mutex);
        while (!context->stop) {
            int wait_status = pthread_cond_timedwait(
                &context->stop_condition, &context->stop_mutex, &wake_time);
            if (wait_status == ETIMEDOUT) break;
        }
        int should_stop = context->stop;
        pthread_mutex_unlock(&context->stop_mutex);
        if (should_stop) break;
    }
    return NULL;
}

static int waitForCommand(pid_t child_pid, double *peak_metric_values,
                          struct timeval *completion_time) {
    int status = 0;
    VramMonitorContext monitor = {
        .root_pid = child_pid,
        .peak_values = peak_metric_values
    };
    pthread_t monitor_thread;
    int monitor_started = 0;
    int monitor_sync_ready = 0;
    int has_peak_metric = 0;
    for (size_t i = 0; i < active_metric_count; i++) {
        if (active_metrics[i].descriptor->kind == ENERGY_METRIC_PEAK) {
            has_peak_metric = 1;
            break;
        }
    }

    if (peak_metric_values && has_peak_metric) {
        for (size_t i = 0; i < active_metric_count; i++) {
            if (active_metrics[i].descriptor->kind == ENERGY_METRIC_PEAK) {
                peak_metric_values[i] =
                    read_process_tree_metric(i, child_pid);
            }
        }
        int mutex_status = pthread_mutex_init(&monitor.stop_mutex, NULL);
        int condition_status = mutex_status == 0
            ? pthread_cond_init(&monitor.stop_condition, NULL) : -1;
        monitor_sync_ready = mutex_status == 0 && condition_status == 0;
        if (monitor_sync_ready &&
            pthread_create(&monitor_thread, NULL, monitor_vram_usage, &monitor) == 0) {
            monitor_started = 1;
        } else {
            for (size_t i = 0; i < active_metric_count; i++) {
                if (active_metrics[i].descriptor->kind == ENERGY_METRIC_PEAK) {
                    peak_metric_values[i] = METRIC_NOT_SUPPORTED;
                }
            }
            fprintf(stderr, "[WARNING] Unable to start peak-metric sampling; peak metrics will be unavailable.\n");
        }
    }

    pid_t wait_result;
    do {
        wait_result = waitpid(child_pid, &status, 0);
    } while (wait_result < 0 && errno == EINTR);
    if (wait_result < 0) {
        perror("[ERROR] Failed while waiting for workload");
        if (monitor_started) {
            pthread_mutex_lock(&monitor.stop_mutex);
            monitor.stop = 1;
            pthread_cond_signal(&monitor.stop_condition);
            pthread_mutex_unlock(&monitor.stop_mutex);
            pthread_join(monitor_thread, NULL);
        }
        if (completion_time) gettimeofday(completion_time, NULL);
        if (monitor_sync_ready) {
            pthread_cond_destroy(&monitor.stop_condition);
            pthread_mutex_destroy(&monitor.stop_mutex);
        }
        return -1;
    }
    if (completion_time) gettimeofday(completion_time, NULL);

    if (monitor_started) {
        pthread_mutex_lock(&monitor.stop_mutex);
        monitor.stop = 1;
        pthread_cond_signal(&monitor.stop_condition);
        pthread_mutex_unlock(&monitor.stop_mutex);
        pthread_join(monitor_thread, NULL);
    }
    if (monitor_sync_ready) {
        pthread_cond_destroy(&monitor.stop_condition);
        pthread_mutex_destroy(&monitor.stop_mutex);
    }
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return -1;
}

/* clock_nanosleep is not available on macOS, so sleep for the remaining time. */
static void sleep_until_monotonic(const struct timespec *deadline) {
#ifdef __APPLE__
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    struct timespec remaining = {
        .tv_sec = deadline->tv_sec - now.tv_sec,
        .tv_nsec = deadline->tv_nsec - now.tv_nsec
    };
    if (remaining.tv_nsec < 0) {
        remaining.tv_sec--;
        remaining.tv_nsec += 1000000000L;
    }
    if (remaining.tv_sec >= 0) nanosleep(&remaining, NULL);
#else
    clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, deadline, NULL);
#endif
}

static void* programWorks(void *arg) {
    FILE *fp = popen((const char *)arg, "r");
    if (!fp) { 
        threadData.returnValue = 0; 
        pthread_exit(NULL); 
    }
    char buf[512];
    while (fgets(buf, sizeof(buf), fp)) {}
    int status = pclose(fp);
    threadData.returnValue = (status != -1 && WIFEXITED(status) && WEXITSTATUS(status) == 0) ? 1 : 0;
    pthread_exit(NULL);
}

struct MeasureArgs {
    FILE *fp; 
    int core; 
    const char *command; 
    int memory; 
    double *metric_values;
};

static void* measureWorker(void *arg) {
    struct MeasureArgs *args = (struct MeasureArgs *)arg;
    struct timeval tvb, tva;

    int cpu_initialized = active_cpu_backend &&
                          active_cpu_backend->init(args->core) == 0;
    int gpu_initialized = active_gpu_backend &&
                          active_gpu_backend->init(0) == 0;
    int cpu_sample_ready = cpu_initialized &&
                           active_cpu_backend->sample_start(args->core) == 0;
    int gpu_sample_ready = gpu_initialized &&
                           active_gpu_backend->sample_start(0) == 0;

    gettimeofday(&tvb, NULL);
    pid_t command_pid = startCommand(args->command);
    if (command_pid < 0) {
        perror("[ERROR] Failed to create workload process");
        gettimeofday(&tva, NULL);
    } else {
        waitForCommand(command_pid, args->metric_values, &tva);
    }

    if (cpu_sample_ready && active_cpu_backend->sample_stop(args->core) != 0) {
        cpu_sample_ready = 0;
    }
    if (gpu_sample_ready && active_gpu_backend->sample_stop(0) != 0) {
        gpu_sample_ready = 0;
    }

    double elapsed_ms = ((tva.tv_sec - tvb.tv_sec) * 1000000.0 + (tva.tv_usec - tvb.tv_usec)) / 1000.0;

    fprintf(args->fp, "%G, ", elapsed_ms);
    if (args->memory < 0) fprintf(args->fp, "NOT_SUPPORTED");
    else fprintf(args->fp, "%d", args->memory);

    for (size_t i = 0; i < active_metric_count; i++) {
        const ActiveMetric *metric = &active_metrics[i];
        int initialized = metric->backend_type == ENERGY_BACKEND_CPU
            ? cpu_initialized : gpu_initialized;
        int sample_ready = metric->backend_type == ENERGY_BACKEND_CPU
            ? cpu_sample_ready : gpu_sample_ready;

        if (metric->descriptor->kind == ENERGY_METRIC_PEAK) {
            args->metric_values[i] =
                command_pid > 0 ? args->metric_values[i] : METRIC_NOT_SUPPORTED;
        } else if (metric->descriptor->kind == ENERGY_METRIC_WINDOW &&
                   (!initialized || !sample_ready)) {
            args->metric_values[i] = METRIC_NOT_SUPPORTED;
        } else if (metric->descriptor->scope == ENERGY_METRIC_PROCESS) {
            args->metric_values[i] = command_pid > 0
                ? read_process_tree_metric(i, command_pid)
                : METRIC_NOT_SUPPORTED;
        } else {
            args->metric_values[i] = read_active_metric(i, 0);
        }
        fprintf(args->fp, ", ");
        write_active_metric_value(args->fp, args->metric_values[i]);
    }
    fputc('\n', args->fp);

    fflush(args->fp);
    free(args->metric_values);
    free(args);
    return NULL;
}

void runTesting(int ntimes, int core, const char *command, const char *language, 
                const char *program, const char *output_file, int powercap, 
                int time_out_limit, float temp_threshold, int variance, int memory) {
    struct timespec ts;
    pthread_t th;

    /* 1. Check that the target program can run before measuring it. */
    if (clock_gettime(CLOCK_REALTIME, &ts) == -1) {
        perror("[ERROR] clock_gettime");
        return;
    }
    ts.tv_sec += time_out_limit;

    threadData.returnValue = 0;
    if (pthread_create(&th, NULL, programWorks, (void *)command) != 0) {
        writeErrorMessage(language, program, "ThreadCreateError", output_file, powercap);
        return;
    }

    int state = pthread_timedjoin_np(th, NULL, &ts);
    if (state != 0) {
        if (state == ETIMEDOUT) {
            printf("[WARNING] Timeout: Program pre-check exceeded %d seconds\n", time_out_limit);
            writeErrorMessage(language, program, "TimeOut", output_file, powercap);
        } else {
            writeErrorMessage(language, program, "JoinError", output_file, powercap);
        }
        return;
    }

    if (threadData.returnValue != 1) {
        writeErrorMessage(language, program, "ProgramError", output_file, powercap);
        return;
    }

    FILE *fp = fopen(output_file, "a");
    if (!fp) return;

    /* 2. Run repetitions under thermal control. */
    for (int i = 0; i < ntimes; i++) {
        float temp = get_current_cpu_temp(core);
        while (temp > temp_threshold + variance) {
            printf("[INFO] Temperature %.1f°C > Baseline %.1f°C + %d°C. Waiting 1s...\n", 
                   temp, temp_threshold, variance);
            sleep(1);
            temp = get_current_cpu_temp(core);
        }

        fprintf(fp, "%s, %s, %d, ", language, program, powercap);

        if (clock_gettime(CLOCK_REALTIME, &ts) == -1) {
            perror("[ERROR] clock_gettime");
            fclose(fp);
            return;
        }
        ts.tv_sec += time_out_limit;

        struct MeasureArgs *m = malloc(sizeof(struct MeasureArgs));
        if (!m) {
            fprintf(stderr, "[ERROR] Unable to allocate measurement state.\n");
            fclose(fp);
            return;
        }
        m->metric_values = malloc(active_metric_count * sizeof(*m->metric_values));
        if (active_metric_count > 0 && !m->metric_values) {
            fprintf(stderr, "[ERROR] Unable to allocate metric results.\n");
            free(m);
            fclose(fp);
            return;
        }
        for (size_t metric_index = 0; metric_index < active_metric_count; metric_index++) {
            m->metric_values[metric_index] = METRIC_NOT_SUPPORTED;
        }
        m->fp = fp; 
        m->core = core; 
        m->command = command; 
        m->memory = memory; 

        if (pthread_create(&th, NULL, measureWorker, m) != 0) {
            free(m->metric_values);
            free(m);
            writeErrorMessage(language, program, "ThreadCreateError", output_file, powercap);
            fclose(fp);
            return;
        }

        state = pthread_timedjoin_np(th, NULL, &ts);
        if (state != 0) {
            if (state == ETIMEDOUT) {
                printf("[WARNING] Timeout: Execution exceeded %d seconds\n", time_out_limit);
                writeErrorMessage(language, program, "TimeOut", output_file, powercap);
            } else {
                writeErrorMessage(language, program, "JoinError", output_file, powercap);
            }
            fclose(fp);
            return;
        }
    }
    fclose(fp);
}

/* Context for continuous sampling in overtime mode. */
struct OvertimeThreadCtx {
    volatile int run_flag;
    int interval_ms;
    pid_t workload_pid;
    const char *language;
    const char *program;
    int powercap;
    const char *output_file;
};

static void* overtimeLogger(void *arg) {
    struct OvertimeThreadCtx *ctx = (struct OvertimeThreadCtx *)arg;
    FILE *fp = fopen(ctx->output_file, "w");
    if (!fp) {
        perror("[ERROR] Failed to open overtime CSV");
        pthread_exit(NULL);
    }

    int cpu_sample_ready = active_cpu_backend &&
                           active_cpu_backend->init(0) == 0 &&
                           active_cpu_backend->sample_start(0) == 0;
    int gpu_sample_ready = active_gpu_backend &&
                           active_gpu_backend->init(0) == 0 &&
                           active_gpu_backend->sample_start(0) == 0;
    double *cumulative_values = calloc(active_metric_count,
                                        sizeof(*cumulative_values));
    if (active_metric_count > 0 && !cumulative_values) {
        fprintf(stderr, "[ERROR] Unable to allocate overtime metric state.\n");
        fclose(fp);
        pthread_exit(NULL);
    }
    energy_backend_write_csv_header(fp, 1);
    fflush(fp);

    struct timespec next_tick;
    clock_gettime(CLOCK_MONOTONIC, &next_tick);

    long interval_ns = (long)ctx->interval_ms * 1000000L;
    unsigned long current_timestamp = 0;

    while (ctx->run_flag && !global_terminate) {
        next_tick.tv_nsec += interval_ns;
        while (next_tick.tv_nsec >= 1000000000L) {
            next_tick.tv_nsec -= 1000000000L;
            next_tick.tv_sec += 1;
        }

        sleep_until_monotonic(&next_tick);
        if (!ctx->run_flag || global_terminate) break;

        current_timestamp += ctx->interval_ms;

        /* Take a stop sample to retrieve the accumulated readings. */
        if (cpu_sample_ready && active_cpu_backend->sample_stop(0) != 0) {
            cpu_sample_ready = 0;
        }
        if (gpu_sample_ready && active_gpu_backend->sample_stop(0) != 0) {
            gpu_sample_ready = 0;
        }

        fprintf(fp, "%s, %s, %d, ", ctx->language, ctx->program, ctx->powercap);
        fprintf(fp, "%lu", current_timestamp);
        for (size_t metric_index = 0; metric_index < active_metric_count; metric_index++) {
            const ActiveMetric *metric = &active_metrics[metric_index];
            int sample_ready = metric->backend_type == ENERGY_BACKEND_CPU
                ? cpu_sample_ready : gpu_sample_ready;
            double value;
            if (metric->descriptor->kind == ENERGY_METRIC_WINDOW && !sample_ready) {
                value = METRIC_NOT_SUPPORTED;
            } else {
                value = metric->descriptor->scope == ENERGY_METRIC_PROCESS
                    ? read_process_tree_metric(metric_index, ctx->workload_pid)
                    : read_active_metric(metric_index, 0);
            }
            if (metric->descriptor->kind == ENERGY_METRIC_WINDOW) {
                if (value < 0.0) {
                    cumulative_values[metric_index] = METRIC_NOT_SUPPORTED;
                } else if (cumulative_values[metric_index] >= 0.0) {
                    cumulative_values[metric_index] += value;
                }
                value = cumulative_values[metric_index];
            }
            fprintf(fp, ", ");
            write_active_metric_value(fp, value);
        }
        fputc('\n', fp);
        fflush(fp);

        if (active_cpu_backend) {
            cpu_sample_ready = active_cpu_backend->sample_start(0) == 0;
        }
        if (active_gpu_backend) {
            gpu_sample_ready = active_gpu_backend->sample_start(0) == 0;
        }
    }

    free(cumulative_values);
    fclose(fp);
    pthread_exit(NULL);
    return NULL;
}

static void show_help(const char *bin_name) {
    printf("\nUsage:\n");
    printf("  %s --temperature-calibration [-sleep-secs <sec>] [-calib-file <path>]\n", bin_name);
    printf("      Calibrates baseline temperature and stores it in the calibration file.\n\n");

    printf("  %s --powercap-calibration -range <min>-<max> -variance <V> -time_out_limit <hours> -n_times <N> -secs_to_sleep <sec> [-compile_cmd \"<cmd>\"] -command \"<cmd>\" -language <lang> -program <prog> [-calib-file <path>] [-output <path>]\n", bin_name);
    printf("      Calibrates powercap limits and evaluates consumption curves.\n\n");

    printf("  %s --standard -command \"<cmd>\" -language <lang> -program <prog> -n_times <N> -variance <V> -time_out_limit <hours> -sleep_time <sec> -powercap <W> [-calib-file <path>] [-output <path>]\n", bin_name);
    printf("      Runs standard aggregate energy measurements.\n\n");

    printf("  %s --overtime -command \"<cmd>\" -interval <ms> -language <lang> -program <prog> [-powercap <W>] [-variance <V>] [-sleep-secs <sec>] [-calib-file <path>] [-output <path>]\n", bin_name);
    printf("      Continuously measures energy over time at fixed sampling intervals.\n\n");
    printf("  %s --csv-header | --overtime-csv-header\n", bin_name);
    printf("      Prints the selected backends' CSV header for the requested measurement mode.\n\n");
}

static const char* resolve_python_venv(void) {
    if (access(".venv/bin/python3", X_OK) == 0) return ".venv/bin/python3";
    if (access("../.venv/bin/python3", X_OK) == 0) return "../.venv/bin/python3";
    if (access("venv/bin/python3", X_OK) == 0) return "venv/bin/python3";
    if (access("../venv/bin/python3", X_OK) == 0) return "../venv/bin/python3";
    return "python3";
}

int main(int argc, char **argv) {
    setlocale(LC_NUMERIC, "C");
    signal(SIGINT, sigint_handler);

    if (argc >= 2 && (!strcmp(argv[1], "--help") || !strcmp(argv[1], "-h"))) {
        show_help(argv[0]);
        return 0;
    }

    header_only_mode = argc >= 2 &&
        (!strcmp(argv[1], "--csv-header") ||
         !strcmp(argv[1], "--overtime-csv-header"));
    init_backends();
    if (argc >= 2 && (!strcmp(argv[1], "--csv-header") ||
                      !strcmp(argv[1], "--overtime-csv-header"))) {
        energy_backend_write_csv_header(stdout,
            strcmp(argv[1], "--overtime-csv-header") == 0);
        return 0;
    }

    /* ========================================================
       1. MODE: TEMPERATURE CALIBRATION STANDALONE
       ======================================================== */
    if (argc >= 2 && strcmp(argv[1], "--temperature-calibration") == 0) {
        int sleep_secs = 20;
        for (int i = 2; i < argc; ++i) {
            if ((!strcmp(argv[i], "-sleep-secs") || !strcmp(argv[i], "-sleep_time")) && i + 1 < argc) {
                sleep_secs = atoi(argv[++i]);
            } else if (!strcmp(argv[i], "-calib-file") && i + 1 < argc) {
                strncpy(calib_file, argv[++i], sizeof(calib_file) - 1);
            }
        }
        calibrateTemperature(calib_file, sleep_secs);
        return 0;
    }

    /* ========================================================
       2. MODE: POWERCAP CALIBRATION
       ======================================================== */
    if (argc >= 2 && strcmp(argv[1], "--powercap-calibration") == 0) {
        if (!active_cpu_backend || !active_cpu_backend->calibrate_powercap) {
            fprintf(stderr, "[ERROR] Powercap calibration is not supported by backend: %s\n",
                    active_cpu_backend ? active_cpu_backend->name : "None");
            return 1;
        }

        int min_powercap = -1, max_powercap = -1;
        int variance = 5, timeout_hours = 1, sleep_secs = 10, ntimes = 3;
        char command[500] = "", compile_cmd[500] = "", language[128] = "", program[128] = "";
        char calib_csv[512] = "powercap_calibration.csv";

        for (int i = 2; i < argc; ++i) {
            if (!strcmp(argv[i], "-range") && i + 1 < argc) {
                int parsed_min, parsed_max;
                char trailing;
                if (sscanf(argv[++i], "%d-%d%c", &parsed_min, &parsed_max, &trailing) != 2) {
                    fprintf(stderr, "[ERROR] Invalid -range; expected <min>-<max> in watts.\n");
                    return 1;
                }
                min_powercap = parsed_min;
                max_powercap = parsed_max;
            }
            else if ((!strcmp(argv[i], "-command") || !strcmp(argv[i], "-run_cmd")) && i + 1 < argc) {
                strncpy(command, argv[++i], sizeof(command) - 1);
            }
            else if (!strcmp(argv[i], "-compile_cmd") && i + 1 < argc) {
                strncpy(compile_cmd, argv[++i], sizeof(compile_cmd) - 1);
            }
            else if (!strcmp(argv[i], "-language") && i + 1 < argc) strncpy(language, argv[++i], sizeof(language) - 1);
            else if (!strcmp(argv[i], "-program") && i + 1 < argc) strncpy(program, argv[++i], sizeof(program) - 1);
            else if (!strcmp(argv[i], "-n_times") && i + 1 < argc) ntimes = atoi(argv[++i]);
            else if (!strcmp(argv[i], "-variance") && i + 1 < argc) variance = atoi(argv[++i]);
            else if (!strcmp(argv[i], "-time_out_limit") && i + 1 < argc) timeout_hours = atoi(argv[++i]);
            else if ((!strcmp(argv[i], "-secs_to_sleep") || !strcmp(argv[i], "-sleep_time")) && i + 1 < argc) {
                sleep_secs = atoi(argv[++i]);
            }
            else if (!strcmp(argv[i], "-calib-file") && i + 1 < argc) {
                strncpy(calib_file, argv[++i], sizeof(calib_file) - 1);
            }
            else if (!strcmp(argv[i], "-output") && i + 1 < argc) {
                strncpy(calib_csv, argv[++i], sizeof(calib_csv) - 1);
            }
        }

        if (min_powercap < 1 || max_powercap < min_powercap ||
            ntimes < 1 || strlen(command) == 0) {
            fprintf(stderr, "[ERROR] --powercap-calibration requires -command and a valid -range with minimum >= 1 W.\n");
            return 1;
        }

        if (strlen(compile_cmd) > 0) {
            printf("[INFO] Compiling benchmark target: %s\n", compile_cmd);
            if (system(compile_cmd) != 0) {
                fprintf(stderr, "[ERROR] Compilation command failed.\n");
                return 1;
            }
        }

        int memory = platform_measure_memory(command);

        if (access(calib_file, F_OK) != 0) {
            calibrateTemperature(calib_file, sleep_secs);
        }
        float ref_temp = getInitialTemperature(calib_file);
        if (ref_temp <= 0.0f) ref_temp = get_current_cpu_temp(0);

        int timeout_seconds = timeout_hours * 3600;

        if (active_cpu_backend->calibrate_powercap(
            ntimes, 0, command, language, program, calib_csv,
            NULL, min_powercap, max_powercap, timeout_seconds,
            ref_temp, variance, memory
        ) != 0) {
            fprintf(stderr, "[ERROR] Powercap calibration failed; see preceding diagnostics.\n");
            return 1;
        }

        const char *py_bin = resolve_python_venv();
        char proc_cmd[1024];
        if (access("Utils/processPowercapCalibrationCSV.py", F_OK) == 0) {
            snprintf(proc_cmd, sizeof(proc_cmd), "%s Utils/processPowercapCalibrationCSV.py %s", py_bin, calib_csv);
        } else {
            snprintf(proc_cmd, sizeof(proc_cmd), "%s ../Utils/processPowercapCalibrationCSV.py %s", py_bin, calib_csv);
        }
        printf("[INFO] Processing calibration CSV via: %s\n", proc_cmd);
        if (system(proc_cmd) != 0) {
            fprintf(stderr, "[WARNING] Calibration post-processing script exited with error.\n");
        }

        return 0;
    }

    /* ========================================================
       3. MODE: OVERTIME
       ======================================================== */
    if (argc >= 2 && (!strcmp(argv[1], "--overtime") || !strcmp(argv[1], "--over-time"))) {
        char command[500] = "", language[128] = "", program[128] = "";
        char ot_csv[512] = "measurements_ot.csv";
        int interval_ms = 100, powercap = -1, variance = 5, sleep_time = 5;

        for (int i = 2; i < argc; ++i) {
            if ((!strcmp(argv[i], "-command") || !strcmp(argv[i], "-run_cmd")) && i + 1 < argc) {
                strncpy(command, argv[++i], sizeof(command) - 1);
            }
            else if (!strcmp(argv[i], "-language") && i + 1 < argc) strncpy(language, argv[++i], sizeof(language) - 1);
            else if (!strcmp(argv[i], "-program") && i + 1 < argc) strncpy(program, argv[++i], sizeof(program) - 1);
            else if (!strcmp(argv[i], "-interval") && i + 1 < argc) interval_ms = atoi(argv[++i]);
            else if (!strcmp(argv[i], "-variance") && i + 1 < argc) variance = atoi(argv[++i]);
            else if (!strcmp(argv[i], "-powercap") && i + 1 < argc) powercap = atoi(argv[++i]);
            else if ((!strcmp(argv[i], "-sleep_time") || !strcmp(argv[i], "-sleep-secs")) && i + 1 < argc) {
                sleep_time = atoi(argv[++i]);
            }
            else if (!strcmp(argv[i], "-calib-file") && i + 1 < argc) {
                strncpy(calib_file, argv[++i], sizeof(calib_file) - 1);
            }
            else if (!strcmp(argv[i], "-output") && i + 1 < argc) {
                strncpy(ot_csv, argv[++i], sizeof(ot_csv) - 1);
            }
        }

        if (powercap != -1 && powercap < 1) {
            fprintf(stderr, "[ERROR] -powercap must be -1 (no GreenMeasurer cap) or at least 1 W.\n");
            return 1;
        }

        if (access(calib_file, F_OK) != 0) {
            calibrateTemperature(calib_file, sleep_time);
        }
        float ref_temp = getInitialTemperature(calib_file);
        if (ref_temp <= 0.0f) ref_temp = get_current_cpu_temp(0);

        float temp = get_current_cpu_temp(0);
        while (temp > ref_temp + variance) {
            printf("[INFO] Waiting for temperature (%.1f°C) to reach baseline (%.1f°C)...\n", temp, ref_temp);
            sleep(1);
            temp = get_current_cpu_temp(0);
        }

        if (powercap > 0) {
            if (!active_cpu_backend || !active_cpu_backend->set_powercap) {
                fprintf(stderr, "[ERROR] Active CPU backend does not support powercapping; overtime measurement aborted.\n");
                return 1;
            }
            if (active_cpu_backend->set_powercap(powercap) != 0) {
                fprintf(stderr, "[ERROR] Failed to set powercap to %d W; overtime measurement aborted.\n",
                        powercap);
                if (active_cpu_backend->destroy_powercap) {
                    active_cpu_backend->destroy_powercap();
                }
                return 1;
            }
        }

        struct OvertimeThreadCtx ctx = {
            .run_flag = 1,
            .interval_ms = interval_ms,
            .workload_pid = -1,
            .language = language,
            .program = program,
            .powercap = powercap,
            .output_file = ot_csv
        };

        pid_t command_pid = startCommand(command);
        if (command_pid < 0) {
            perror("[ERROR] Failed to create workload process");
            if (powercap > 0 && active_cpu_backend && active_cpu_backend->destroy_powercap &&
                active_cpu_backend->destroy_powercap() != 0) {
                fprintf(stderr, "[ERROR] Failed to destroy the RAPL powercap context.\n");
            }
            return 1;
        }
        ctx.workload_pid = command_pid;

        pthread_t logger_th;
        if (pthread_create(&logger_th, NULL, overtimeLogger, &ctx) != 0) {
            perror("[ERROR] Failed to start overtime sampling thread");
            kill(command_pid, SIGTERM);
            waitpid(command_pid, NULL, 0);
            if (powercap > 0 && active_cpu_backend && active_cpu_backend->destroy_powercap &&
                active_cpu_backend->destroy_powercap() != 0) {
                fprintf(stderr, "[ERROR] Failed to destroy the RAPL powercap context.\n");
            }
            return 1;
        }

        int command_status = waitForCommand(command_pid, NULL, NULL);

        ctx.run_flag = 0;
        pthread_join(logger_th, NULL);

        if (powercap > 0 && active_cpu_backend && active_cpu_backend->destroy_powercap) {
            if (active_cpu_backend->destroy_powercap() != 0) {
                fprintf(stderr, "[ERROR] Failed to destroy the RAPL powercap context.\n");
                command_status = -1;
            }
        }
        printf("[INFO] Overtime session finished -> %s\n", ot_csv);
        return command_status == 0 ? 0 : 1;
    }

    /* ========================================================
       4. MODE: STANDARD MEASUREMENT
       ======================================================== */
    char command[500] = "", language[128] = "C", program[128] = "Target";
    int ntimes = 1, powercap = -1, variance = 0, timeout = 3600;
    int sleep_time = 5;

    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--standard")) {
            continue;
        }
        else if ((!strcmp(argv[i], "-command") || !strcmp(argv[i], "-run_cmd")) && i + 1 < argc) {
            strncpy(command, argv[++i], sizeof(command) - 1);
        }
        else if (!strcmp(argv[i], "-language") && i + 1 < argc) strncpy(language, argv[++i], sizeof(language) - 1);
        else if (!strcmp(argv[i], "-program") && i + 1 < argc) strncpy(program, argv[++i], sizeof(program) - 1);
        else if (!strcmp(argv[i], "-n_times") && i + 1 < argc) ntimes = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-variance") && i + 1 < argc) variance = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-time_out_limit") && i + 1 < argc) timeout = atoi(argv[++i]) * 3600;
        else if (!strcmp(argv[i], "-powercap") && i + 1 < argc) powercap = atoi(argv[++i]);
        else if ((!strcmp(argv[i], "-sleep_time") || !strcmp(argv[i], "-sleep-secs")) && i + 1 < argc) {
            sleep_time = atoi(argv[++i]);
        }
        else if (!strcmp(argv[i], "-calib-file") && i + 1 < argc) {
            strncpy(calib_file, argv[++i], sizeof(calib_file) - 1);
        }
        else if ((!strcmp(argv[i], "-output") || !strcmp(argv[i], "-measurements-file")) && i + 1 < argc) {
            strncpy(measurements_file, argv[++i], sizeof(measurements_file) - 1);
        }
    }

    if (powercap != -1 && powercap < 1) {
        fprintf(stderr, "[ERROR] -powercap must be -1 (no GreenMeasurer cap) or at least 1 W.\n");
        return 1;
    }

    int memory = 0;
    if (strlen(command) > 0) {
        memory = platform_measure_memory(command);
    }

    if (access(calib_file, F_OK) != 0) {
        printf("[WARNING] Calibration file %s not found. Running temperature calibration...\n", calib_file);
        calibrateTemperature(calib_file, sleep_time);
    } else {
        printf("[INFO] Calibration file %s found. Skipping temperature calibration.\n", calib_file);
    }

    float ref_temp = getInitialTemperature(calib_file);
    if (ref_temp <= 0.0f) {
        ref_temp = get_current_cpu_temp(0);
    }

    printf("\n[INFO]\n======= Execution Details ========\n");
    printf(" Command               : %s\n", command);
    printf(" Language              : %s\n", language);
    printf(" Program               : %s\n", program);
    printf(" N_times               : %d\n", ntimes);
    printf(" Variance              : %d°C\n", variance);
    printf(" Timeout               : %d seconds\n", timeout);
    printf(" Sleep_time            : %d seconds\n", sleep_time);
    if (powercap == -1) printf(" PowerCap              : No power cap\n"); 
    else printf(" PowerCap              : %d watts\n", powercap);
    printf(" Calibration file      : %s\n", calib_file);
    printf(" Baseline temperature  : %.2f °C\n", ref_temp);
    printf(" Output CSV            : %s\n", measurements_file);
    printf("====================================\n\n");

    if (powercap > 0) {
        if (!active_cpu_backend || !active_cpu_backend->set_powercap) {
            fprintf(stderr, "[ERROR] Active CPU backend does not support powercapping; measurement aborted.\n");
            return 1;
        }
        if (active_cpu_backend->set_powercap(powercap) != 0) {
            fprintf(stderr, "[ERROR] Failed to set powercap to %d W; measurement aborted.\n",
                    powercap);
            if (active_cpu_backend->destroy_powercap) {
                active_cpu_backend->destroy_powercap();
            }
            return 1;
        }
    }

    FILE *fp = fopen(measurements_file, "w");
    if (!fp) {
        perror("[ERROR] Failed to open measurement CSV");
        if (powercap > 0 && active_cpu_backend && active_cpu_backend->destroy_powercap) {
            active_cpu_backend->destroy_powercap();
        }
        return 1;
    }
    energy_backend_write_csv_header(fp, 0);
    if (fclose(fp) != 0) {
        perror("[ERROR] Failed to write measurement CSV header");
        if (powercap > 0 && active_cpu_backend && active_cpu_backend->destroy_powercap) {
            active_cpu_backend->destroy_powercap();
        }
        return 1;
    }

    runTesting(ntimes, 0, command, language, program, measurements_file, powercap, timeout, ref_temp, variance, memory);

    if (powercap > 0 && active_cpu_backend && active_cpu_backend->destroy_powercap) {
        if (active_cpu_backend->destroy_powercap() != 0) {
            fprintf(stderr, "[ERROR] Failed to destroy the RAPL powercap context.\n");
            return 1;
        }
    }

    return 0;
}
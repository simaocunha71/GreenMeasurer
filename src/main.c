#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <time.h>
#include <pthread.h>
#include <unistd.h>
#include <signal.h>
#include <string.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <locale.h>
#include <math.h>
#include <errno.h>
#include <dirent.h>
#include <dlfcn.h>
#include <limits.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/resource.h>
#define BACKEND_PLATFORM_DIR "platform/linux"
#define BACKEND_INSTALLED_DIR "../lib/greenmeasurer/platform/linux"

#include "include/energy-backend.h"
#include "include/metric-guidance.h"
#include "metrics/metric_adapter_loader.h"
#include "metrics/system_metrics.h"

typedef struct {
    void *handle;
    const EnergyBackendDescriptor *descriptor;
    const EnergyMetricFunctionMap *function_map;
    int report_index;
    char *directory;
} LoadedBackend;

typedef struct {
    const EnergyMetricDescriptor *descriptor;
    EnergyBackendType backend_type;
    size_t backend_metric_index;
    const EnergyMetricFunction *function;
} ActiveMetric;

static CpuBackend *active_cpu_backend = NULL;
static GpuBackend *active_gpu_backend = NULL;
static ActiveMetric *active_metrics = NULL;
static size_t active_metric_count = 0;
static LoadedBackend *loaded_backends = NULL;
static size_t loaded_backend_count = 0;
static char backend_root[PATH_MAX];
static int header_only_mode = 0;
static int configured_overtime = 0;
static int configured_standard = 0;
static int configured_system[5];
static int configured_frequency_cores;
static const char **configured_backend_names;
static size_t configured_backend_name_count;
static unsigned char *configured_backend_selected;
static int configured_all;

typedef struct {
    const char *name;
    const char *path;
    GreenMetricAdapterHandle handle;
} ConfiguredAdapter;

typedef struct {
    size_t adapter_index;
    size_t metric_index;
    double *values;
} ConfiguredExternalMetric;

static ConfiguredAdapter *configured_adapters;
static size_t configured_adapter_count;
static ConfiguredExternalMetric *configured_external_metrics;
static size_t configured_external_count;

static void cleanup_configured_metrics(void) {
    for (size_t i = 0; i < configured_external_count; i++)
        free(configured_external_metrics[i].values);
    for (size_t i = 0; i < configured_adapter_count; i++)
        green_metric_adapter_close(&configured_adapters[i].handle);
    free(configured_external_metrics);
    free(configured_adapters);
    free(configured_backend_names);
    free(configured_backend_selected);
}

static int add_configured_external(const char *name, const char *path, const char *metric) {
    size_t adapter_index;
    for (adapter_index = 0; adapter_index < configured_adapter_count; adapter_index++) {
        if (strcmp(configured_adapters[adapter_index].name, name) == 0) break;
        if (strcmp(configured_adapters[adapter_index].path, path) == 0) {
            fprintf(stderr, "[ERROR] Adapter path '%s' is already used by '%s'.\n",
                    path, configured_adapters[adapter_index].name);
            return -1;
        }
    }
    if (adapter_index == configured_adapter_count) {
        ConfiguredAdapter *new_adapters = realloc(configured_adapters,
            (configured_adapter_count + 1) * sizeof(*new_adapters));
        if (!new_adapters) {
            fprintf(stderr, "[ERROR] Unable to allocate adapter registry.\n");
            return -1;
        }
        configured_adapters = new_adapters;
        configured_adapters[adapter_index] = (ConfiguredAdapter){.name = name, .path = path};
        if (green_metric_adapter_open(&configured_adapters[adapter_index].handle, path) != 0) {
            fprintf(stderr, "[ERROR] Cannot load configured adapter '%s'%s%s.\n",
                    name, metric ? " for metric " : "", metric ? metric : "");
            green_metric_extension_guidance();
            return 1;
        }
        configured_adapter_count++;
    } else if (strcmp(configured_adapters[adapter_index].path, path) != 0) {
        fprintf(stderr, "[ERROR] Adapter '%s' has conflicting paths.\n", name);
        return -1;
    }
    const GreenMetricAdapter *adapter = configured_adapters[adapter_index].handle.adapter;
    if (!metric) {
        for (size_t i = 0; i < adapter->metric_count; i++) {
            int result = add_configured_external(name, path, adapter->metrics[i].name);
            if (result != 0) return result;
        }
        return 0;
    }
    size_t metric_index;
    for (metric_index = 0; metric_index < adapter->metric_count; metric_index++) {
        if (strcmp(adapter->metrics[metric_index].name, metric) == 0) break;
    }
    if (metric_index == adapter->metric_count) {
        fprintf(stderr, "[ERROR] Adapter '%s' has no metric '%s'.\n", name, metric);
        green_metric_extension_guidance();
        return 1;
    }
    for (size_t i = 0; i < configured_external_count; i++) {
        if (configured_external_metrics[i].adapter_index == adapter_index &&
            configured_external_metrics[i].metric_index == metric_index) {
            fprintf(stderr, "[ERROR] Duplicate external metric '%s.%s'.\n", name, metric);
            return -1;
        }
    }
    ConfiguredExternalMetric *new_metrics = realloc(configured_external_metrics,
        (configured_external_count + 1) * sizeof(*new_metrics));
    if (!new_metrics) {
        fprintf(stderr, "[ERROR] Unable to allocate external metric registry.\n");
        return -1;
    }
    configured_external_metrics = new_metrics;
    configured_external_metrics[configured_external_count++] = (ConfiguredExternalMetric) {
        .adapter_index = adapter_index, .metric_index = metric_index
    };
    return 0;
}

static int configure_overtime_metrics(int argc, char **argv) {
    static const char *names[] = {"system.memory", "system.cores",
        "system.workload_processes", "system.running_processes", "system.core_frequency"};
    int unavailable_external = 0;
    if (atexit(cleanup_configured_metrics) != 0) {
        fprintf(stderr, "[ERROR] Unable to register adapter cleanup.\n");
        return -1;
    }
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--configured-all")) {
            if (configured_all) return fprintf(stderr, "[ERROR] Duplicate all selection.\n"), -1;
            configured_all = 1;
            for (size_t j = 0; j < sizeof(names) / sizeof(names[0]); j++) configured_system[j] = 1;
        } else if (!strcmp(argv[i], "--configured-adapter")) {
            if (i + 2 >= argc) return fprintf(stderr, "[ERROR] Missing adapter arguments.\n"), -1;
            int result = add_configured_external(argv[i + 1], argv[i + 2], NULL);
            if (result < 0) return -1;
            if (result > 0) {
                fprintf(stderr, "[WARNING] Skipping unavailable adapter '%s'.\n", argv[i + 1]);
                unavailable_external = 1;
            }
            i += 2;
        } else if (!strcmp(argv[i], "--configured-system")) {
            if (++i >= argc) return fprintf(stderr, "[ERROR] Missing system metric.\n"), -1;
            size_t j;
            for (j = 0; j < sizeof(names) / sizeof(names[0]); j++)
                if (!strcmp(argv[i], names[j])) break;
            if (j == sizeof(names) / sizeof(names[0]) || configured_system[j]) {
                fprintf(stderr, "[ERROR] Unknown or duplicate system metric '%s'.\n", argv[i]);
                return -1;
            }
            configured_system[j] = 1;
        } else if (!strcmp(argv[i], "--configured-external")) {
            if (i + 3 >= argc) return fprintf(stderr, "[ERROR] Missing external metric arguments.\n"), -1;
            int result = add_configured_external(argv[i + 1], argv[i + 2], argv[i + 3]);
            if (result < 0) return -1;
            if (result > 0) {
                fprintf(stderr, "[WARNING] Skipping unavailable external metric 'external.%s.%s'.\n",
                        argv[i + 1], argv[i + 3]);
                unavailable_external = 1;
            }
            i += 3;
        } else if (!strcmp(argv[i], "--configured-backend")) {
            if (++i >= argc) return fprintf(stderr, "[ERROR] Missing backend metric.\n"), -1;
            const char **names = realloc(configured_backend_names,
                (configured_backend_name_count + 1) * sizeof(*names));
            if (!names) return fprintf(stderr, "[ERROR] Unable to allocate metric selection.\n"), -1;
            configured_backend_names = names;
            configured_backend_names[configured_backend_name_count++] = argv[i];
        } else if (!strcmp(argv[i], "-command") || !strcmp(argv[i], "-language") ||
                   !strcmp(argv[i], "-program") || !strcmp(argv[i], "-variance") ||
                   !strcmp(argv[i], "-powercap") || !strcmp(argv[i], "-interval-us") ||
                   !strcmp(argv[i], "-sleep-secs") || !strcmp(argv[i], "-calib-file") ||
                   !strcmp(argv[i], "-output") || !strcmp(argv[i], "-n_times") ||
                   !strcmp(argv[i], "-timeout-seconds")) {
            if (++i >= argc) return fprintf(stderr, "[ERROR] Missing option value.\n"), -1;
        } else {
            fprintf(stderr, "[ERROR] Unknown configured measurement option '%s'.\n", argv[i]);
            return -1;
        }
    }
    if (!configured_external_count && !configured_backend_name_count &&
        !configured_system[0] && !configured_system[1] &&
        !configured_system[2] && !configured_system[3] && !configured_system[4]) {
        fprintf(stderr, unavailable_external
                ? "[ERROR] No selected metrics are available.\n"
                : "[ERROR] No configured metrics were selected.\n");
        return -1;
    }
    return 0;
}

static int configured_has_metrics(void);

static int select_configured_backend_metrics(void) {
    if (!configured_backend_name_count && !configured_all) return 0;
    configured_backend_selected = calloc(active_metric_count, sizeof(*configured_backend_selected));
    if (active_metric_count && !configured_backend_selected) {
        fprintf(stderr, "[ERROR] Unable to allocate backend metric selection.\n");
        return -1;
    }
    if (configured_all) {
        for (size_t i = 0; i < active_metric_count; i++) configured_backend_selected[i] = 1;
    }
    for (size_t j = 0; j < configured_backend_name_count; j++) {
        const char *name = configured_backend_names[j];
        size_t i;
        for (i = 0; i < active_metric_count; i++) {
            const ActiveMetric *metric = &active_metrics[i];
            const char *prefix = metric->backend_type == ENERGY_BACKEND_CPU ? "cpu." : "gpu.";
            size_t length = strlen(prefix);
            if (!strncmp(name, prefix, length) &&
                (!strcmp(name + length, metric->descriptor->name) ||
                 (metric->function && !strcmp(name + length, metric->function->name)))) break;
        }
        if (i == active_metric_count) {
            fprintf(stderr, "[WARNING] Backend metric '%s' is not provided by the selected backend "
                    "(or no backend is active for that category); skipping it.\n", name);
            green_metric_extension_guidance();
            continue;
        }
        if (configured_backend_selected[i]) {
            fprintf(stderr, "[ERROR] Backend metric '%s' is duplicated.\n", name);
            return -1;
        }
        configured_backend_selected[i] = 1;
    }
    if (!configured_has_metrics()) {
        fprintf(stderr, "[ERROR] No selected metrics are available.\n");
        return -1;
    }
    return 0;
}

static int read_configured_external(const ConfiguredExternalMetric *metric, double *value) {
    ConfiguredAdapter *adapter = &configured_adapters[metric->adapter_index];
    if (green_metric_adapter_read(&adapter->handle, metric->metric_index, value) == 0) return 0;
    fprintf(stderr, "[ERROR] Reading configured metric 'external.%s.%s' failed; "
            "check the adapter, library and runtime permissions.\n",
            adapter->name, adapter->handle.adapter->metrics[metric->metric_index].name);
    green_metric_extension_guidance();
    return -1;
}

static int configured_has_metrics(void) {
    if (configured_external_count) return 1;
    for (size_t i = 0; i < sizeof(configured_system) / sizeof(configured_system[0]); i++)
        if (configured_system[i]) return 1;
    for (size_t i = 0; i < active_metric_count; i++)
        if (configured_backend_selected && configured_backend_selected[i]) return 1;
    return 0;
}

static void skip_configured_system_metric(size_t index, const char *name) {
    fprintf(stderr, "[WARNING] Skipping unavailable system metric '%s' before measurement.\n", name);
    configured_system[index] = 0;
}

static void probe_configured_system(void) {
    int cores = 0;
    if (configured_system[1] || configured_system[4]) cores = sys_core_count();
    if (configured_system[1] && cores <= 0)
        skip_configured_system_metric(1, "system.cores");
    if (configured_system[3]) {
        double value = sys_running_processes();
        if (!isfinite(value) || value < 0)
            skip_configured_system_metric(3, "system.running_processes");
    }
    if (configured_system[4]) {
        int available = cores > 0 && cores <= SYS_MAX_CORES;
        for (int core = 0; available && core < cores; core++) {
            double value = sys_core_frequency_mhz(core);
            if (!isfinite(value) || value < 0) available = 0;
        }
        if (!available) skip_configured_system_metric(4, "system.core_frequency");
        else configured_frequency_cores = cores;
    }
}

static void skip_configured_backend_metric(size_t i) {
    const ActiveMetric *metric = &active_metrics[i];
    fprintf(stderr, "[WARNING] Skipping unavailable backend metric '%s.%s' before measurement.\n",
            metric->backend_type == ENERGY_BACKEND_CPU ? "cpu" : "gpu",
            metric->descriptor->name);
    configured_backend_selected[i] = 0;
}

static void skip_configured_backend_type(EnergyBackendType type) {
    for (size_t i = 0; i < active_metric_count; i++)
        if (configured_backend_selected && configured_backend_selected[i] &&
            active_metrics[i].backend_type == type)
            skip_configured_backend_metric(i);
}

static int probe_configured_external(void) {
    size_t i = 0;
    while (i < configured_external_count) {
        double value;
        if (read_configured_external(&configured_external_metrics[i], &value) == 0) {
            i++;
            continue;
        }
        fprintf(stderr, "[WARNING] Skipping unavailable external metric before measurement.\n");
        memmove(&configured_external_metrics[i], &configured_external_metrics[i + 1],
                (configured_external_count - i - 1) * sizeof(*configured_external_metrics));
        configured_external_count--;
    }
    if (!configured_has_metrics()) {
        fprintf(stderr, "[ERROR] No selected metrics are available.\n");
        return -1;
    }
    return 0;
}

static void write_external_metric_value(FILE *fp, double value) {
    if (isfinite(value)) fprintf(fp, "%.18g", value);
    else fputs("NOT_SUPPORTED", fp);
}

static void configured_backend_failure_guidance(EnergyBackendType type) {
    fprintf(stderr, "[INFO] Check the selected %s backend, hardware support and permissions. "
            "Affected configured metrics:", type == ENERGY_BACKEND_CPU ? "CPU" : "GPU");
    for (size_t i = 0; i < active_metric_count; i++) {
        if (configured_backend_selected && configured_backend_selected[i] &&
            active_metrics[i].backend_type == type)
            fprintf(stderr, " %s.%s", type == ENERGY_BACKEND_CPU ? "cpu" : "gpu",
                    active_metrics[i].descriptor->name);
    }
    fprintf(stderr, "\n");
    green_metric_extension_guidance();
}

/* Configurable calibration and measurement files */
static char calib_file[512] = "/tmp/cores_temperatures.txt";

static volatile sig_atomic_t global_terminate = 0;
static void sigint_handler(int signum) { 
    (void)signum; 
    global_terminate = 1; 
}

static struct { int returnValue; } threadData;

/* Standardized CSV metric formatters */
static void write_energy_metric(FILE *fp, double val) {
    if (!isfinite(val) || val < 0.0) {
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
    if (metric->descriptor->scope == ENERGY_METRIC_PROCESS && process_id <= 0)
        return METRIC_NOT_SUPPORTED;
    if (metric->function)
        return metric->function->read(0, metric->descriptor->scope == ENERGY_METRIC_DEVICE
                                         ? 0 : process_id);
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

    pid_t process_ids[SYS_MAX_PROCESS_TREE];
    size_t process_count =
        sys_collect_process_tree(root_pid, process_ids, SYS_MAX_PROCESS_TREE);

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
    if (configured_overtime) {
        const char *names[] = {"CPU_Memory_KB", "Cores",
            overtime ? "Workload_Processes" : "Workload_Processes_Peak",
            overtime ? "System_Running_Processes" : "System_Running_Processes_Peak"};
        for (size_t i = 0; i < 4; i++)
            if (configured_system[i]) fprintf(fp, ",%s", names[i]);
        if (configured_system[4]) {
            int cores = configured_frequency_cores;
            if (cores > SYS_MAX_CORES) cores = SYS_MAX_CORES;
            for (int core = 0; core < cores; core++)
                fprintf(fp, overtime ? ",Core%d_Freq_MHz" : ",Core%d_Avg_Freq_MHz", core);
        }
        for (size_t i = 0; i < active_metric_count; i++) {
            if (!configured_backend_selected || !configured_backend_selected[i]) continue;
            const ActiveMetric *metric = &active_metrics[i];
            fprintf(fp, ",%s.%s_%s",
                    metric->backend_type == ENERGY_BACKEND_CPU ? "cpu" : "gpu",
                    metric->descriptor->name, metric->descriptor->unit);
        }
        for (size_t i = 0; i < configured_external_count; i++) {
            ConfiguredExternalMetric *metric = &configured_external_metrics[i];
            ConfiguredAdapter *adapter = &configured_adapters[metric->adapter_index];
            const GreenMetricDescriptor *descriptor = &adapter->handle.adapter->metrics[metric->metric_index];
            fprintf(fp, ",external.%s.%s_%s", adapter->name, descriptor->name, descriptor->unit);
        }
        fputc('\n', fp);
        return;
    }
    fprintf(fp, ",CPU_Memory_KB,Cores");
    /* Standard rows summarise the run (peak / average); overtime rows are instantaneous. */
    fprintf(fp, overtime ? ",Workload_Processes,System_Running_Processes"
                         : ",Workload_Processes_Peak,System_Running_Processes_Peak");
    int cores = sys_core_count();
    if (cores > SYS_MAX_CORES) cores = SYS_MAX_CORES;
    for (int core = 0; core < cores; core++) {
        fprintf(fp, overtime ? ",Core%d_Freq_MHz" : ",Core%d_Avg_Freq_MHz", core);
    }
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

    FILE *snapshot = fopen(target_calib_file, "w");
    if (!snapshot) {
        perror("[ERROR] Failed to open calibration file");
        return -1;
    } else {
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
        if (fclose(snapshot) != 0) {
            perror("[ERROR] Failed to close calibration snapshot");
            return -1;
        }
    }

    float mean_temp = get_current_cpu_temp(0);
    if (mean_temp <= 0.0f) {
        fprintf(stderr, "[WARNING] Sensor read returned <= 0.0, defaulting baseline to 40.0°C\n");
        mean_temp = 40.0f;
    }

    FILE *output_file = fopen(target_calib_file, "a");
    if (output_file != NULL) {
        int write_failed = fprintf(output_file, "Average temperature: %.1f°C\n\n", mean_temp) < 0;
        if (fclose(output_file) != 0 || write_failed) {
            perror("[ERROR] Failed to write calibration file");
            return -1;
        }
        printf("[INFO] Calibrated baseline temperature saved to %s: %.1f°C\n", target_calib_file, mean_temp);
    } else {
        perror("[ERROR] Failed to write calibration file");
        return -1;
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

/* Backend lookup order: $GREENMEASURER_BACKEND_DIR, <exe dir>/platform/linux,
   <exe dir>/../lib/greenmeasurer/platform/linux. Each root contains cpu/ and gpu/. */
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
    ssize_t length = readlink("/proc/self/exe", executable_path, sizeof(executable_path) - 1);
    if (length < 0 || (size_t)length >= sizeof(executable_path) - 1) {
        perror("[ERROR] Unable to resolve executable path");
        return -1;
    }

    executable_path[length] = '\0';
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

    const EnergyMetricFunctionMap *maps[2] = {NULL, NULL};
    for (size_t i = 0; i < loaded_backend_count; i++) {
        const EnergyBackendDescriptor *backend = loaded_backends[i].descriptor;
        if ((backend->type == ENERGY_BACKEND_CPU && backend->backend.cpu == active_cpu_backend) ||
            (backend->type == ENERGY_BACKEND_GPU && backend->backend.gpu == active_gpu_backend))
            maps[backend->type] = loaded_backends[i].function_map;
    }
    size_t output_index = 0;
    for (size_t i = 0; i < cpu_count; i++) {
        active_metrics[output_index++] = (ActiveMetric) {
            .descriptor = &active_cpu_backend->metrics[i],
            .backend_type = ENERGY_BACKEND_CPU,
            .backend_metric_index = i,
            .function = maps[ENERGY_BACKEND_CPU] ? &maps[ENERGY_BACKEND_CPU]->functions[i] : NULL
        };
    }
    for (size_t i = 0; i < gpu_count; i++) {
        active_metrics[output_index++] = (ActiveMetric) {
            .descriptor = &active_gpu_backend->metrics[i],
            .backend_type = ENERGY_BACKEND_GPU,
            .backend_metric_index = i,
            .function = maps[ENERGY_BACKEND_GPU] ? &maps[ENERGY_BACKEND_GPU]->functions[i] : NULL
        };
    }
    return 0;
}

static int backend_function_map_valid(const EnergyMetricFunctionMap *map,
                                      const EnergyBackendDescriptor *backend) {
    if (!map) return 1;
    const EnergyMetricDescriptor *metrics = backend->type == ENERGY_BACKEND_CPU
        ? backend->backend.cpu->metrics : backend->backend.gpu->metrics;
    size_t count = backend->type == ENERGY_BACKEND_CPU
        ? backend->backend.cpu->metric_count : backend->backend.gpu->metric_count;
    if (map->version != ENERGY_METRIC_FUNCTION_MAP_VERSION ||
        map->count != count || !map->functions) return 0;
    for (size_t i = 0; i < count; i++) {
        const char *name = map->functions[i].name;
        if (!name || !name[0] || !map->functions[i].read ||
            !((*name >= 'a' && *name <= 'z') || (*name >= 'A' && *name <= 'Z') || *name == '_'))
            return 0;
        for (const char *p = name + 1; *p; p++)
            if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
                  (*p >= '0' && *p <= '9') || *p == '_')) return 0;
        for (size_t j = 0; j < count; j++)
            if ((i != j && !strcmp(name, metrics[j].name)) ||
                (j < i && !strcmp(name, map->functions[j].name))) return 0;
    }
    return 1;
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

    dlerror();
    const EnergyMetricFunctionMap *map = dlsym(handle, ENERGY_METRIC_FUNCTION_MAP_SYMBOL);
    if (dlerror()) map = NULL;
    if (!backend_function_map_valid(map, descriptor)) {
        add_backend_report(category, vendor_dir, "invalid", "invalid metric function map");
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
    char *directory = strdup(vendor_dir);
    if (!directory) {
        fprintf(stderr, "[ERROR] Unable to allocate backend directory name.\n");
        dlclose(handle);
        return -1;
    }
    loaded_backends[loaded_backend_count].handle = handle;
    loaded_backends[loaded_backend_count].descriptor = descriptor;
    loaded_backends[loaded_backend_count].function_map = map;
    loaded_backends[loaded_backend_count].directory = directory;
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
        free(loaded_backends[i].directory);
    }
    free(loaded_backends);
    loaded_backends = NULL;
    loaded_backend_count = 0;
    free(active_metrics);
    active_metrics = NULL;
    active_metric_count = 0;
}

static void print_backend_summary(void) {
    const char *verbose_env = getenv("GREENMEASURER_VERBOSE_BACKENDS");
    int verbose = verbose_env && verbose_env[0] && strcmp(verbose_env, "0") != 0;

    printf("[INFO] Energy backends (%s)\n", backend_root);
    printf("  %-4s  %-28s  %-13s  %s\n", "TYPE", "BACKEND", "STATUS", "DETAILS");
    printf("  %-4s  %-28s  %-13s  %s\n", "----", "----------------------------", "-------------", "-------");
    for (size_t i = 0; i < backend_report_count; i++) {
        const BackendReport *report = &backend_reports[i];
        int show = verbose || strcmp(report->status, "selected") == 0 ||
                   strcmp(report->status, "invalid") == 0 ||
                   strcmp(report->status, "load failed") == 0;
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
        if (!configured_overtime) {
            printf("[INFO] Active backend metrics:");
            for (size_t i = 0; i < active_metric_count; i++) {
                const char *category =
                    active_metrics[i].backend_type == ENERGY_BACKEND_CPU ? "cpu" : "gpu";
                printf(" %s.%s (%s)", category, active_metrics[i].descriptor->name,
                       active_metrics[i].descriptor->unit);
            }
            printf("\n");
        }
    }

    if (!active_cpu_backend) {
        fprintf(stderr, "[WARNING] No CPU energy backend active. CPU backend metrics are unavailable.\n");
    }
    if (!active_gpu_backend) {
        fprintf(stderr, "[WARNING] No GPU energy backend active. GPU backend metrics are unavailable.\n");
    }
    if ((!active_cpu_backend || !active_gpu_backend) && !header_only_mode) {
        fprintf(stderr, "[INFO] Set GREENMEASURER_VERBOSE_BACKENDS=1 for backend detection and build details.\n");
    }
    if (configured_overtime && select_configured_backend_metrics() != 0) exit(EXIT_FAILURE);
    if (!active_cpu_backend && !active_gpu_backend && !configured_overtime) {
        fprintf(stderr, "[ERROR] No usable CPU or GPU energy backend. Enable verbose backend diagnostics for details.\n");
        exit(EXIT_FAILURE);
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

/* Per-run system statistics, written only by the monitor thread while a run is active. */
static double run_peak_workload_processes;
static double run_peak_running_processes;
static double run_core_freq_sum_mhz[SYS_MAX_CORES];
static unsigned long run_core_freq_samples[SYS_MAX_CORES];

static void run_stats_reset(void) {
    run_peak_workload_processes = SYS_METRIC_NOT_SUPPORTED;
    run_peak_running_processes = SYS_METRIC_NOT_SUPPORTED;
    memset(run_core_freq_sum_mhz, 0, sizeof(run_core_freq_sum_mhz));
    memset(run_core_freq_samples, 0, sizeof(run_core_freq_samples));
}

static void run_stats_sample_workload_processes(pid_t root_pid) {
    double value = sys_workload_process_count(root_pid);
    if (value > run_peak_workload_processes) run_peak_workload_processes = value;
}

static void run_stats_sample_running_processes(void) {
    double value = sys_running_processes();
    if (value > run_peak_running_processes) run_peak_running_processes = value;
}

static void run_stats_sample_core_frequencies(void) {
    int cores = sys_core_count();
    if (cores > SYS_MAX_CORES) cores = SYS_MAX_CORES;
    for (int core = 0; core < cores; core++) {
        double mhz = sys_core_frequency_mhz(core);
        if (mhz >= 0.0) {
            run_core_freq_sum_mhz[core] += mhz;
            run_core_freq_samples[core]++;
        }
    }
}

/* Energy windows are split into short segments and summed in 64-bit doubles.
   A 32-bit hardware counter can wrap several times during a long run, which a
   single sample_start/sample_stop pair cannot detect; segments of one polling
   period (50 ms) can wrap at most once, and that wrap is handled by the backend. */
#define RUN_MAX_METRICS 64
static double run_window_total[RUN_MAX_METRICS];
static int run_sample_ready[2];
static int run_backend_core;

static int run_backend_sample_start(EnergyBackendType type) {
    if (type == ENERGY_BACKEND_CPU) return active_cpu_backend->sample_start(run_backend_core);
    return active_gpu_backend->sample_start(0);
}

static int run_backend_sample_stop(EnergyBackendType type) {
    if (type == ENERGY_BACKEND_CPU) return active_cpu_backend->sample_stop(run_backend_core);
    return active_gpu_backend->sample_stop(0);
}

/* Closes the current segment of every ready backend, adds it to the totals and,
   unless this is the last call, opens the next segment. */
static void run_window_accumulate(int restart) {
    for (int type = 0; type < 2; type++) {
        if (!run_sample_ready[type]) continue;
        int stopped = run_backend_sample_stop((EnergyBackendType)type) == 0;
        for (size_t i = 0; i < active_metric_count && i < RUN_MAX_METRICS; i++) {
            if (active_metrics[i].backend_type != (EnergyBackendType)type ||
                active_metrics[i].descriptor->kind != ENERGY_METRIC_WINDOW) continue;
            double segment = stopped ? read_active_metric(i, 0) : METRIC_NOT_SUPPORTED;
            if (segment < 0.0) run_window_total[i] = METRIC_NOT_SUPPORTED;
            else if (run_window_total[i] >= 0.0) run_window_total[i] += segment;
        }
        if (!stopped) run_sample_ready[type] = 0;
        else if (restart && run_backend_sample_start((EnergyBackendType)type) != 0) run_sample_ready[type] = 0;
    }
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
        run_stats_sample_workload_processes(context->root_pid);
        run_stats_sample_running_processes();
        run_stats_sample_core_frequencies();
        run_window_accumulate(1);
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
    if (peak_metric_values) {
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

    if (active_cpu_backend && !cpu_sample_ready) {
        fprintf(stderr, "[WARNING] CPU backend initialization or sampling failed; CPU window metrics are unavailable.\n");
    }
    if (active_gpu_backend && !gpu_sample_ready) {
        fprintf(stderr, "[WARNING] GPU backend initialization or sampling failed; GPU window metrics are unavailable.\n");
    }

    run_stats_reset();
    run_backend_core = args->core;
    run_sample_ready[ENERGY_BACKEND_CPU] = cpu_sample_ready;
    run_sample_ready[ENERGY_BACKEND_GPU] = gpu_sample_ready;
    for (size_t i = 0; i < RUN_MAX_METRICS; i++) {
        int ready = i < active_metric_count &&
                    run_sample_ready[active_metrics[i].backend_type];
        run_window_total[i] = ready ? 0.0 : METRIC_NOT_SUPPORTED;
    }
    gettimeofday(&tvb, NULL);
    pid_t command_pid = startCommand(args->command);
    if (command_pid < 0) {
        perror("[ERROR] Failed to create workload process");
        gettimeofday(&tva, NULL);
    } else {
        waitForCommand(command_pid, args->metric_values, &tva);
    }

    run_window_accumulate(0);

    double elapsed_ms = ((tva.tv_sec - tvb.tv_sec) * 1000000.0 + (tva.tv_usec - tvb.tv_usec)) / 1000.0;

    fprintf(args->fp, "%G, ", elapsed_ms);
    if (args->memory < 0) fprintf(args->fp, "NOT_SUPPORTED");
    else fprintf(args->fp, "%d", args->memory);

    fprintf(args->fp, ", %d, ", sys_core_count());
    write_active_metric_value(args->fp, run_peak_workload_processes);
    fprintf(args->fp, ", ");
    write_active_metric_value(args->fp, run_peak_running_processes);
    int freq_cores = sys_core_count();
    if (freq_cores > SYS_MAX_CORES) freq_cores = SYS_MAX_CORES;
    for (int core = 0; core < freq_cores; core++) {
        fprintf(args->fp, ", ");
        write_active_metric_value(args->fp, run_core_freq_samples[core] > 0
            ? run_core_freq_sum_mhz[core] / (double)run_core_freq_samples[core]
            : SYS_METRIC_NOT_SUPPORTED);
    }

    for (size_t i = 0; i < active_metric_count; i++) {
        const ActiveMetric *metric = &active_metrics[i];
        int initialized = metric->backend_type == ENERGY_BACKEND_CPU
            ? cpu_initialized : gpu_initialized;

        if (metric->descriptor->kind == ENERGY_METRIC_PEAK) {
            args->metric_values[i] =
                command_pid > 0 ? args->metric_values[i] : METRIC_NOT_SUPPORTED;
        } else if (metric->descriptor->kind == ENERGY_METRIC_WINDOW) {
            args->metric_values[i] = initialized && i < RUN_MAX_METRICS
                ? run_window_total[i] : METRIC_NOT_SUPPORTED;
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

#include "overtime.inc"
#include "standard.inc"

static void show_help(const char *bin_name) {
    printf("\nUsage:\n");
    printf("  %s --temperature-calibration [-sleep-secs <sec>] [-calib-file <path>]\n", bin_name);
    printf("      Calibrates baseline temperature and stores it in the calibration file.\n\n");

    printf("  %s --powercap-calibration -range <min>-<max> -variance <V> -time_out_limit <hours> -n_times <N> -secs_to_sleep <sec> [-compile_cmd \"<cmd>\"] -command \"<cmd>\" -language <lang> -program <prog> [-calib-file <path>] [-output <path>]\n", bin_name);
    printf("      Calibrates powercap limits and evaluates consumption curves.\n\n");

    printf("  %s --standard -command \"<cmd>\" -language <lang> -program <prog> -n_times <N> -variance <V> -time_out_limit <hours> -sleep_time <sec> -powercap <W> [-calib-file <path>] [-output <path>]\n", bin_name);
    printf("      Runs standard aggregate energy measurements.\n\n");

    printf("  %s --overtime -command \"<cmd>\" -language <lang> -program <prog> [-interval-us <microseconds>] [-powercap <W>] [-variance <V>] [-sleep-secs <sec>] [-calib-file <path>] [-output <path>]\n", bin_name);
    printf("      Samples metrics sequentially and dumps interval values to a CSV when the command ends.\n\n");
    printf("  %s --csv-header | --overtime-csv-header\n", bin_name);
    printf("      Prints the selected backends' CSV header for the requested measurement mode.\n\n");
    printf("  %s --config <file.toml> [measurement options]\n", bin_name);
    printf("      Runs the supported subset of TOML configurations; see README for limitations.\n\n");
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

    if (argc >= 2 && !strcmp(argv[1], "--config")) {
        char executable[PATH_MAX], script[PATH_MAX];
        ssize_t length = readlink("/proc/self/exe", executable, sizeof(executable) - 1);
        if (length < 0) {
            perror("[ERROR] Unable to locate executable");
            return 1;
        }
        if ((size_t)length >= sizeof(executable) - 1) {
            fprintf(stderr, "[ERROR] Executable path is too long.\n");
            return 1;
        }
        executable[length] = '\0';
        char *slash = strrchr(executable, '/');
        if (!slash) {
            fprintf(stderr, "[ERROR] Unable to locate configuration runner.\n");
            return 1;
        }
        *slash = '\0';
#ifdef GREENMEASURER_TEST_ENGINE
        int written = snprintf(script, sizeof(script), "%s/run_config_legacy.py", executable);
#else
        int written = snprintf(script, sizeof(script), "%s/../scripts/run_config.py", executable);
#endif
        if (written < 0 || (size_t)written >= sizeof(script)) {
            fprintf(stderr, "[ERROR] Configuration runner path is too long.\n");
            return 1;
        }
        char **arguments = calloc((size_t)argc + 2, sizeof(*arguments));
        if (!arguments) {
            fprintf(stderr, "[ERROR] Unable to allocate configuration arguments.\n");
            return 1;
        }
        arguments[0] = "python3";
        arguments[1] = script;
        for (int i = 2; i < argc; i++) arguments[i] = argv[i];
        execvp(arguments[0], arguments);
        perror("[ERROR] Unable to start Python configuration runner");
        free(arguments);
        return 1;
    }

    if (argc >= 2 && (!strcmp(argv[1], "--help") || !strcmp(argv[1], "-h"))) {
        show_help(argv[0]);
        return 0;
    }

    configured_standard = argc >= 2 && !strcmp(argv[1], "--configured-standard");
    configured_overtime = configured_standard ||
        (argc >= 2 && !strcmp(argv[1], "--configured-overtime"));
    if (configured_overtime && configure_overtime_metrics(argc, argv) != 0) return 1;
    header_only_mode = argc >= 2 &&
        (!strcmp(argv[1], "--csv-header") ||
         !strcmp(argv[1], "--overtime-csv-header"));
    init_backends();
    if (configured_standard) return configured_standard_run(argc, argv);
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
        return calibrateTemperature(calib_file, sleep_secs) < 0 ? 1 : 0;
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
            if (calibrateTemperature(calib_file, sleep_secs) < 0) return 1;
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
#ifdef GREENMEASURER_PROCESSOR
        const char *processor = GREENMEASURER_PROCESSOR;
#else
        const char *processor = access("Utils/processPowercapCalibrationCSV.py", F_OK) == 0
            ? "Utils/processPowercapCalibrationCSV.py" : "../Utils/processPowercapCalibrationCSV.py";
#endif
        printf("[INFO] Processing calibration CSV via: %s %s\n", py_bin, processor);
        pid_t processor_pid = fork();
        if (processor_pid == 0) {
            execlp(py_bin, py_bin, processor, calib_csv, (char *)NULL);
            _exit(127);
        }
        if (processor_pid < 0) {
            perror("[WARNING] Unable to start calibration post-processing");
        } else {
            int status;
            pid_t waited;
            do { waited = waitpid(processor_pid, &status, 0); } while (waited < 0 && errno == EINTR);
            if (waited < 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
                fprintf(stderr, "[WARNING] Calibration post-processing script exited with error.\n");
            }
        }

        return 0;
    }

    /* ========================================================
       3. MODE: OVERTIME
       ======================================================== */
    if (argc >= 2 && (!strcmp(argv[1], "--overtime") || !strcmp(argv[1], "--over-time") || configured_overtime)) {
        char command[500] = "", language[128] = "", program[128] = "";
        char ot_csv[512] = "measurements_ot.csv";
        int powercap = -1, variance = 5, sleep_time = 5;
        unsigned long interval_us = 1000UL;

        for (int i = 2; i < argc; ++i) {
            if (!strcmp(argv[i], "--configured-system")) {
                i++;
            }
            else if (!strcmp(argv[i], "--configured-all")) {
                continue;
            }
            else if (!strcmp(argv[i], "--configured-adapter")) {
                i += 2;
            }
            else if (!strcmp(argv[i], "--configured-external")) {
                i += 3;
            }
            else if (!strcmp(argv[i], "--configured-backend")) {
                i++;
            }
            else if ((!strcmp(argv[i], "-command") || !strcmp(argv[i], "-run_cmd")) && i + 1 < argc) {
                strncpy(command, argv[++i], sizeof(command) - 1);
            }
            else if (!strcmp(argv[i], "-language") && i + 1 < argc) strncpy(language, argv[++i], sizeof(language) - 1);
            else if (!strcmp(argv[i], "-program") && i + 1 < argc) strncpy(program, argv[++i], sizeof(program) - 1);
            else if (!strcmp(argv[i], "-variance") && i + 1 < argc) variance = atoi(argv[++i]);
            else if (!strcmp(argv[i], "-powercap") && i + 1 < argc) powercap = atoi(argv[++i]);
            else if (!strcmp(argv[i], "-interval-us")) {
                if (i + 1 >= argc || argv[i + 1][0] == '-') {
                    fprintf(stderr, "[ERROR] -interval-us requires a positive integer in microseconds.\n");
                    return 1;
                }
                char *end;
                errno = 0;
                interval_us = strtoul(argv[++i], &end, 10);
                if (errno || *end != '\0' || end == argv[i] || interval_us == 0 || interval_us > UINT_MAX) {
                    fprintf(stderr, "[ERROR] -interval-us requires a positive integer up to %u.\n", UINT_MAX);
                    return 1;
                }
            }
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

        if (command[0] == '\0') {
            fprintf(stderr, "[ERROR] --overtime requires a workload command.\n");
            return 1;
        }
        if (powercap != -1 && powercap < 1) {
            fprintf(stderr, "[ERROR] -powercap must be -1 (no GreenMeasurer cap) or at least 1 W.\n");
            return 1;
        }

        if (access(calib_file, F_OK) != 0) {
            if (calibrateTemperature(calib_file, sleep_time) < 0) return 1;
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

        struct OvertimeContext ctx = {
            .language = language,
            .program = program,
            .powercap = powercap,
            .output_file = ot_csv
        };

        int command_status = ot_run(command, &ctx, interval_us);

        if (powercap > 0 && active_cpu_backend && active_cpu_backend->destroy_powercap) {
            if (active_cpu_backend->destroy_powercap() != 0) {
                fprintf(stderr, "[ERROR] Failed to destroy the RAPL powercap context.\n");
                command_status = -1;
            }
        }
        if (command_status == 0) printf("[INFO] Overtime session finished -> %s\n", ot_csv);
        return command_status == 0 ? 0 : 1;
    }

    return configured_standard_run(argc, argv);
}
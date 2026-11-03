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

#include "include/energy-backend.h"

typedef struct {
    void *handle;
    const EnergyBackendDescriptor *descriptor;
    int report_index;
} LoadedBackend;

static CpuBackend *active_cpu_backend = NULL;
static GpuBackend *active_gpu_backend = NULL;
static LoadedBackend *loaded_backends = NULL;
static size_t loaded_backend_count = 0;
static char backend_root[PATH_MAX];

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

static void write_temperature_metric(FILE *fp, float val) {
    if (val < -0.5f) {
        fprintf(fp, "NOT_SUPPORTED");
    } else {
        fprintf(fp, "%.1f", val);
    }
}

static void writeErrorMessage(const char *language, const char *program, const char *errorMsg, 
                              const char *out_file, int powercap) {
    FILE *fp = fopen(out_file, "a");
    if (!fp) return;
    fprintf(fp, "%s, %s, %d, %s, %s, %s, %s, %s, %s, %s, %s\n",
            language, program, powercap,
            errorMsg, errorMsg, errorMsg, errorMsg, errorMsg, errorMsg, errorMsg, errorMsg);
    fclose(fp);
}

static float get_current_cpu_temp(int core) {
    if (active_cpu_backend && active_cpu_backend->get_temperature) {
        return active_cpu_backend->get_temperature(core);
    }
    return -1.0f;
}

static float get_current_gpu_temp(int device_idx) {
    if (active_gpu_backend && active_gpu_backend->get_temperature) {
        return active_gpu_backend->get_temperature(device_idx);
    }
    return -1.0f;
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
        "platform/linux",
        "../lib/greenmeasurer/platform/linux"
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

    if (expected_type == ENERGY_BACKEND_CPU) {
        CpuBackend *backend = descriptor->backend.cpu;
        return backend && backend->name && backend->detect && backend->init &&
               backend->sample_start && backend->sample_stop &&
               backend->get_package_energy && backend->get_core_energy &&
               backend->get_dram_energy;
    }

    GpuBackend *backend = descriptor->backend.gpu;
    return backend && backend->name && backend->detect && backend->init &&
           backend->sample_start && backend->sample_stop && backend->get_energy;
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
}

static void print_backend_summary(void) {
    printf("[INFO] Energy backends (%s)\n", backend_root);
    printf("  %-4s  %-28s  %-13s  %s\n", "TYPE", "BACKEND", "STATUS", "DETAILS");
    printf("  %-4s  %-28s  %-13s  %s\n", "----", "----------------------------", "-------------", "-------");
    for (size_t i = 0; i < backend_report_count; i++) {
        const BackendReport *report = &backend_reports[i];
        printf("  %-4s  %-28s  %-13s  %s\n",
               report->category, report->label, report->status, report->detail);
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

    print_backend_summary();

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
    printf("\n");
}

static void runCommand(const char *cmd) {
    if (system(cmd) == -1) exit(EXIT_FAILURE);
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
    float temp;
};

static void* measureWorker(void *arg) {
    struct MeasureArgs *args = (struct MeasureArgs *)arg;
    struct timeval tvb, tva;

    int cpu_sample_ready = active_cpu_backend &&
                           active_cpu_backend->init(args->core) == 0 &&
                           active_cpu_backend->sample_start(args->core) == 0;
    int gpu_sample_ready = active_gpu_backend &&
                           active_gpu_backend->init(0) == 0 &&
                           active_gpu_backend->sample_start(0) == 0;

    gettimeofday(&tvb, NULL);
    runCommand(args->command);
    gettimeofday(&tva, NULL);

    if (cpu_sample_ready && active_cpu_backend->sample_stop(args->core) != 0) {
        cpu_sample_ready = 0;
    }
    if (gpu_sample_ready && active_gpu_backend->sample_stop(0) != 0) {
        gpu_sample_ready = 0;
    }

    double elapsed_ms = ((tva.tv_sec - tvb.tv_sec) * 1000000.0 + (tva.tv_usec - tvb.tv_usec)) / 1000.0;

    double pkg_e  = cpu_sample_ready ? active_cpu_backend->get_package_energy(args->core) : METRIC_NOT_SUPPORTED;
    double core_e = cpu_sample_ready ? active_cpu_backend->get_core_energy(args->core) : METRIC_NOT_SUPPORTED;
    double dram_e = cpu_sample_ready ? active_cpu_backend->get_dram_energy(args->core) : METRIC_NOT_SUPPORTED;

    double gpu_e  = gpu_sample_ready ? active_gpu_backend->get_energy(0) : METRIC_NOT_SUPPORTED;
    float  gpu_t  = get_current_gpu_temp(0);

    write_energy_metric(args->fp, pkg_e);
    fprintf(args->fp, ", ");
    write_energy_metric(args->fp, core_e);
    fprintf(args->fp, ", ");
    write_energy_metric(args->fp, gpu_e);
    fprintf(args->fp, ", ");
    write_energy_metric(args->fp, dram_e);
    fprintf(args->fp, ", ");
    fprintf(args->fp, "%G, ", elapsed_ms);
    write_temperature_metric(args->fp, args->temp);
    fprintf(args->fp, ", ");
    write_temperature_metric(args->fp, gpu_t);
    fprintf(args->fp, ", ");

    if (args->memory < 0) {
        fprintf(args->fp, "NOT_SUPPORTED\n");
    } else {
        fprintf(args->fp, "%d\n", args->memory);
    }

    fflush(args->fp);
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
        m->fp = fp; 
        m->core = core; 
        m->command = command; 
        m->memory = memory; 
        m->temp = temp;

        if (pthread_create(&th, NULL, measureWorker, m) != 0) {
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

    fprintf(fp, "Language,Program,Powercap,Package,Core,GPU,DRAM,Timestamp,CpuTemperature,GpuTemperature\n");
    fflush(fp);

    int cpu_sample_ready = active_cpu_backend &&
                           active_cpu_backend->init(0) == 0 &&
                           active_cpu_backend->sample_start(0) == 0;
    int gpu_sample_ready = active_gpu_backend &&
                           active_gpu_backend->init(0) == 0 &&
                           active_gpu_backend->sample_start(0) == 0;

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

        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next_tick, NULL);
        if (!ctx->run_flag || global_terminate) break;

        current_timestamp += ctx->interval_ms;

        /* Take a stop sample to retrieve the accumulated readings. */
        if (cpu_sample_ready && active_cpu_backend->sample_stop(0) != 0) {
            cpu_sample_ready = 0;
        }
        if (gpu_sample_ready && active_gpu_backend->sample_stop(0) != 0) {
            gpu_sample_ready = 0;
        }

        double pkg_e  = cpu_sample_ready ? active_cpu_backend->get_package_energy(0) : METRIC_NOT_SUPPORTED;
        double core_e = cpu_sample_ready ? active_cpu_backend->get_core_energy(0) : METRIC_NOT_SUPPORTED;
        double dram_e = cpu_sample_ready ? active_cpu_backend->get_dram_energy(0) : METRIC_NOT_SUPPORTED;
        double gpu_e  = gpu_sample_ready ? active_gpu_backend->get_energy(0) : METRIC_NOT_SUPPORTED;

        float current_cpu_temp = get_current_cpu_temp(0);
        float current_gpu_temp = get_current_gpu_temp(0);

        fprintf(fp, "%s, %s, %d, ", ctx->language, ctx->program, ctx->powercap);
        write_energy_metric(fp, pkg_e);
        fprintf(fp, ", ");
        write_energy_metric(fp, core_e);
        fprintf(fp, ", ");
        write_energy_metric(fp, gpu_e);
        fprintf(fp, ", ");
        write_energy_metric(fp, dram_e);
        fprintf(fp, ", %lu, ", current_timestamp);
        write_temperature_metric(fp, current_cpu_temp);
        fprintf(fp, ", ");
        write_temperature_metric(fp, current_gpu_temp);
        fprintf(fp, "\n");
        fflush(fp);
    }

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

    init_backends();

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
                sscanf(argv[++i], "%d-%d", &min_powercap, &max_powercap);
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

        if (min_powercap < 0 || max_powercap < 0 || strlen(command) == 0) {
            fprintf(stderr, "[ERROR] --powercap-calibration: missing -range or -command\n");
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

        active_cpu_backend->calibrate_powercap(
            ntimes, 0, command, language, program, calib_csv,
            NULL, min_powercap, max_powercap, timeout_seconds,
            ref_temp, variance, memory
        );

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

        if (powercap > 0 && active_cpu_backend && active_cpu_backend->set_powercap) {
            active_cpu_backend->set_powercap(powercap);
        }

        struct OvertimeThreadCtx ctx = {
            .run_flag = 1,
            .interval_ms = interval_ms,
            .language = language,
            .program = program,
            .powercap = powercap,
            .output_file = ot_csv
        };

        pthread_t logger_th;
        if (pthread_create(&logger_th, NULL, overtimeLogger, &ctx) != 0) {
            perror("[ERROR] Failed to start overtime sampling thread");
            return 1;
        }

        runCommand(command);

        ctx.run_flag = 0;
        pthread_join(logger_th, NULL);

        if (powercap > 0 && active_cpu_backend && active_cpu_backend->destroy_powercap) {
            active_cpu_backend->destroy_powercap();
        }
        printf("[INFO] Overtime session finished -> %s\n", ot_csv);
        return 0;
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

    if (powercap > 0 && active_cpu_backend && active_cpu_backend->set_powercap) {
        active_cpu_backend->set_powercap(powercap);
    }

    FILE *fp = fopen(measurements_file, "w");
    if (fp) {
        fprintf(fp, "Language,Program,Powercap,Package,Core,GPU,DRAM,Time,CpuTemperature,GpuTemperature,Memory\n");
        fclose(fp);
    }

    runTesting(ntimes, 0, command, language, program, measurements_file, powercap, timeout, ref_temp, variance, memory);

    if (powercap > 0 && active_cpu_backend && active_cpu_backend->destroy_powercap) {
        active_cpu_backend->destroy_powercap();
    }

    return 0;
}
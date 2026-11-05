#include "intel.h"
#include <pthread.h>
#include <regex.h>

static int intel_cpu_model = -1;
static uint32_t package_before = 0, package_after = 0;
static uint32_t core_before = 0, core_after = 0;
static uint32_t dram_before = 0, dram_after = 0;
static int package_before_valid = 0, package_after_valid = 0;
static int core_before_valid = 0, core_after_valid = 0;
static int dram_before_valid = 0, dram_after_valid = 0;

static double energy_units = 0.0;
static int energy_units_valid = 0;
static raplcap global_rc;
static int raplcap_active = 0;

extern void runTesting(int ntimes, int core, const char *command, const char *language, 
                       const char *program, const char *measurements_file, int powercap, 
                       int time_out_limit, float temp_threshold, int variance, int memory);

static int open_msr(int core) {
    char msr_filename[BUFSIZ];
    snprintf(msr_filename, sizeof(msr_filename), "/dev/cpu/%d/msr", core);
    int fd = open(msr_filename, O_RDONLY);
    if (fd < 0) {
        if (errno == EACCES || errno == EPERM) {
            static int warned = 0;
            if (!warned) {
                fprintf(stderr, "[WARNING] Permission denied accessing %s (run with sudo for Intel CPU MSR metrics).\n", msr_filename);
                warned = 1;
            }
            return -1;
        } else if (errno == ENXIO) {
            fprintf(stderr, "[WARNING] rdmsr: No CPU %d\n", core);
            return -1;
        } else if (errno == EIO) {
            fprintf(stderr, "[WARNING] rdmsr: CPU %d doesn't support MSRs\n", core);
            return -1;
        } else {
            return -1;
        }
    }
    return fd;
}

static uint64_t read_msr(int fd, int which) {
    if (fd < 0) return (uint64_t)-1;
    uint64_t data = (uint64_t)-1;
    if (pread(fd, &data, sizeof(data), (off_t)which) != sizeof(data)) {
        return (uint64_t)-1;
    }
    return data;
}


static int detect_intel_cpu(void) {
    FILE *fff = fopen("/proc/cpuinfo", "r");
    if (!fff) return -1;

    int family = 0, model = -1;
    char buffer[BUFSIZ];
    char vendor[BUFSIZ] = "";

    while (fgets(buffer, BUFSIZ, fff) != NULL) {
        if (!strncmp(buffer, "vendor_id", 9)) {
            sscanf(buffer, "%*s%*s%s", vendor);
            if (strncmp(vendor, "GenuineIntel", 12)) {
                fclose(fff);
                return -1;
            }
        }
        if (!strncmp(buffer, "cpu family", 10)) {
            sscanf(buffer, "%*s%*s%*s%d", &family);
            if (family != 6) {
                fclose(fff);
                return -1;
            }
        }
        if (!strncmp(buffer, "model", 5)) {
            sscanf(buffer, "%*s%*s%d", &model);
        }
    }
    fclose(fff);
    return model;
}

static int intel_detect(void) {
    intel_cpu_model = detect_intel_cpu();
    return (intel_cpu_model >= 0) ? 1 : 0;
}

static const char *intel_diagnose(void) {
    return "CPU is not a supported Intel RAPL model";
}

static int intel_init(int core) {
    energy_units_valid = 0;
    if (intel_cpu_model < 0) {
        intel_cpu_model = detect_intel_cpu();
        if (intel_cpu_model < 0) return -1;
    }

    int fd = open_msr(core);
    if (fd < 0) return -1;

    uint64_t result = read_msr(fd, MSR_RAPL_POWER_UNIT);
    close(fd);

    // A VM can expose the MSR device but return zeros for every register.
    if (result == (uint64_t)-1 || result == 0) return -1;
    energy_units = pow(0.5, (double)((result >> 8) & 0x1f));
    energy_units_valid = 1;
    return 0;
}

static int intel_sample_start(int core) {
    package_before_valid = 0;
    core_before_valid = 0;
    dram_before_valid = 0;
    int fd = open_msr(core);
    if (fd < 0) {
        return -1;
    }

    uint64_t res;
    res = read_msr(fd, MSR_PKG_ENERGY_STATUS);
    if (res != (uint64_t)-1) {
        package_before = (uint32_t)res;
        package_before_valid = 1;
    }

    res = read_msr(fd, MSR_PP0_ENERGY_STATUS);
    if (res != (uint64_t)-1) {
        core_before = (uint32_t)res;
        core_before_valid = 1;
    }

    res = read_msr(fd, MSR_DRAM_ENERGY_STATUS);
    if (res != (uint64_t)-1) {
        dram_before = (uint32_t)res;
        dram_before_valid = 1;
    }

    close(fd);
    return (package_before_valid && core_before_valid) ? 0 : -1;
}

static int intel_sample_stop(int core) {
    package_after_valid = 0;
    core_after_valid = 0;
    dram_after_valid = 0;
    int fd = open_msr(core);
    if (fd < 0) {
        return -1;
    }

    uint64_t res;
    res = read_msr(fd, MSR_PKG_ENERGY_STATUS);
    if (res != (uint64_t)-1) {
        package_after = (uint32_t)res;
        package_after_valid = 1;
    }

    res = read_msr(fd, MSR_PP0_ENERGY_STATUS);
    if (res != (uint64_t)-1) {
        core_after = (uint32_t)res;
        core_after_valid = 1;
    }

    res = read_msr(fd, MSR_DRAM_ENERGY_STATUS);
    if (res != (uint64_t)-1) {
        dram_after = (uint32_t)res;
        dram_after_valid = 1;
    }

    close(fd);
    // A real RAPL counter never stays at 0; a stub or unsupported domain does.
    if (package_before == 0 && package_after == 0) { package_before_valid = package_after_valid = 0; }
    if (core_before == 0 && core_after == 0) { core_before_valid = core_after_valid = 0; }
    if (dram_before == 0 && dram_after == 0) { dram_before_valid = dram_after_valid = 0; }
    return (package_after_valid && core_after_valid) ? 0 : -1;
}

static double intel_get_package_energy(int core) {
    (void)core;
    if (!energy_units_valid || !package_before_valid || !package_after_valid) {
        return METRIC_NOT_SUPPORTED;
    }
    return (double)(uint32_t)(package_after - package_before) * energy_units;
}

static double intel_get_core_energy(int core) {
    (void)core;
    if (!energy_units_valid || !core_before_valid || !core_after_valid) {
        return METRIC_NOT_SUPPORTED;
    }
    return (double)(uint32_t)(core_after - core_before) * energy_units;
}

static double intel_get_dram_energy(int core) {
    (void)core;
    if (!energy_units_valid || !dram_before_valid || !dram_after_valid) {
        return METRIC_NOT_SUPPORTED;
    }
    return (double)(uint32_t)(dram_after - dram_before) * energy_units;
}

#define INTEL_MAX_TEMP_SENSORS 64
static int intel_temp_fds[INTEL_MAX_TEMP_SENSORS];
static int intel_temp_fd_count = 0;
static pthread_once_t intel_temp_once = PTHREAD_ONCE_INIT;

static int intel_read_small_file(const char *path, char *buffer, size_t size) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    ssize_t n = pread(fd, buffer, size - 1, 0);
    close(fd);
    if (n <= 0) return -1;
    buffer[n] = '\0';
    return 0;
}

/* Opens every "Core N" input of the coretemp hwmon device once; later reads are a single pread each. */
static void intel_temp_discover(void) {
    for (int hwmon = 0; hwmon < 64 && intel_temp_fd_count < INTEL_MAX_TEMP_SENSORS; hwmon++) {
        char path[160], text[64];
        snprintf(path, sizeof(path), "/sys/class/hwmon/hwmon%d/name", hwmon);
        if (intel_read_small_file(path, text, sizeof(text)) != 0 || strncmp(text, "coretemp", 8) != 0) continue;
        for (int index = 1; index < 256 && intel_temp_fd_count < INTEL_MAX_TEMP_SENSORS; index++) {
            snprintf(path, sizeof(path), "/sys/class/hwmon/hwmon%d/temp%d_label", hwmon, index);
            if (intel_read_small_file(path, text, sizeof(text)) != 0 || strncmp(text, "Core ", 5) != 0) continue;
            snprintf(path, sizeof(path), "/sys/class/hwmon/hwmon%d/temp%d_input", hwmon, index);
            int fd = open(path, O_RDONLY);
            if (fd >= 0) intel_temp_fds[intel_temp_fd_count++] = fd;
        }
    }
}

static float intel_get_temperature_sysfs(void) {
    pthread_once(&intel_temp_once, intel_temp_discover);
    double sum = 0.0;
    int count = 0;
    for (int i = 0; i < intel_temp_fd_count; i++) {
        char text[32];
        ssize_t n = pread(intel_temp_fds[i], text, sizeof(text) - 1, 0);
        if (n <= 0) continue;
        text[n] = '\0';
        long millidegrees = strtol(text, NULL, 10);
        if (millidegrees > 0) {
            sum += (double)millidegrees / 1000.0;
            count++;
        }
    }
    return count > 0 ? (float)(sum / count) : -1.0f;
}

static float intel_get_temperature_sensors_command(void) {
    FILE *fp = popen("sensors 2>/dev/null", "r");
    if (!fp) return -1.0f;

    char line[256];
    double sum = 0.0;
    int count = 0;

    while (fgets(line, sizeof(line), fp)) {
        if (strncmp(line, "Core ", 5) == 0) {
            char *plus = strchr(line, '+');
            if (plus) {
                float temp = 0.0f;
                if (sscanf(plus + 1, "%f", &temp) == 1 && temp > 0.0f) {
                    sum += temp;
                    count++;
                }
            }
        }
    }
    pclose(fp);
    return (count > 0) ? (float)(sum / count) : -1.0f;
}

static float intel_get_temperature(int core) {
    (void)core;
    float temperature = intel_get_temperature_sysfs();
    return temperature > 0.0f ? temperature : intel_get_temperature_sensors_command();
}

static const EnergyMetricDescriptor intel_metrics[] = {
    {"Package", "J", ENERGY_METRIC_WINDOW, ENERGY_METRIC_DEVICE, ENERGY_METRIC_FLAG_NONE},
    {"Core", "J", ENERGY_METRIC_WINDOW, ENERGY_METRIC_DEVICE, ENERGY_METRIC_FLAG_NONE},
    {"DRAM", "J", ENERGY_METRIC_WINDOW, ENERGY_METRIC_DEVICE, ENERGY_METRIC_FLAG_NONE},
    {"Temperature", "C", ENERGY_METRIC_GAUGE, ENERGY_METRIC_DEVICE,
     ENERGY_METRIC_FLAG_CPU_TEMPERATURE}
};

static double intel_get_metric(size_t metric_index, int core, pid_t process_id) {
    (void)process_id;
    switch (metric_index) {
        case 0: return intel_get_package_energy(core);
        case 1: return intel_get_core_energy(core);
        case 2: return intel_get_dram_energy(core);
        case 3: return (double)intel_get_temperature(core);
        default: return METRIC_NOT_SUPPORTED;
    }
}

static int intel_destroy_powercap(void);

static int intel_set_powercap(int watts) {
    if (watts <= 0) return 0;
    if (raplcap_active) {
        fprintf(stderr, "[ERROR] An earlier RAPL powercap context is still active.\n");
        return -1;
    }
    if (raplcap_init(&global_rc) != 0) {
        fprintf(stderr, "[ERROR] Failed to initialize the RAPL powercap context.\n");
        return -1;
    }
    raplcap_active = 1;

    uint32_t n = raplcap_get_num_packages(&global_rc);
    if (n == 0) {
        fprintf(stderr, "[ERROR] No RAPL packages were found.\n");
        intel_destroy_powercap();
        return -1;
    }

    for (uint32_t q = 0; q < n; q++) {
        uint32_t d = raplcap_get_num_die(&global_rc, q);
        if (d == 0) {
            fprintf(stderr, "[ERROR] No RAPL dies found for package %u.\n", q);
            intel_destroy_powercap();
            return -1;
        }
        for (uint32_t j = 0; j < d; j++) {
            raplcap_limit long_limit = { .watts = watts, .seconds = 0 };
            raplcap_limit short_limit = { .watts = watts, .seconds = 0 };
            if (raplcap_pd_set_limits(&global_rc, q, j, RAPLCAP_ZONE_PACKAGE,
                                      &long_limit, &short_limit) != 0) {
                perror("[ERROR] raplcap_pd_set_limits");
                intel_destroy_powercap();
                return -1;
            }
            if (raplcap_pd_set_zone_enabled(&global_rc, q, j,
                                            RAPLCAP_ZONE_PACKAGE, 1) != 0) {
                perror("[ERROR] raplcap_pd_set_zone_enabled");
                intel_destroy_powercap();
                return -1;
            }
        }
    }
    return 0;
}

static int intel_destroy_powercap(void) {
    if (!raplcap_active) return 0;
    if (raplcap_destroy(&global_rc) != 0) {
        fprintf(stderr, "[ERROR] Failed to destroy the RAPL powercap context.\n");
        return -1;
    }
    raplcap_active = 0;
    return 0;
}

static void intel_cleanup(void) {
    intel_destroy_powercap();
}

static int initialize_calibration_csv(const char *filename) {
    FILE *fp = fopen(filename, "w");
    if (!fp) {
        perror("[ERROR] Failed to create calibration measurement CSV");
        return -1;
    }
    energy_backend_write_csv_header(fp, 0);
    int result = ferror(fp) ? -1 : 0;
    if (fclose(fp) != 0) result = -1;
    return result;
}

static int append_calibration_rows(FILE *output, const char *filename) {
    FILE *input = fopen(filename, "r");
    if (!input) {
        perror("[ERROR] Failed to read calibration measurement CSV");
        return -1;
    }

    char buffer[1024];
    if (!fgets(buffer, sizeof(buffer), input)) {
        fprintf(stderr, "[ERROR] Calibration measurement CSV has no header: %s\n", filename);
        fclose(input);
        return -1;
    }
    int result = 0;
    size_t rows_written = 0;
    while (fgets(buffer, sizeof(buffer), input)) {
        if (fputs(buffer, output) == EOF) {
            result = -1;
            break;
        }
        rows_written++;
    }
    if (ferror(input)) result = -1;
    if (fclose(input) != 0) result = -1;
    if (rows_written == 0) {
        fprintf(stderr, "[ERROR] Calibration measurement CSV has no measurement rows: %s\n", filename);
        result = -1;
    }
    return result;
}

int intel_calibrate_powercap(int ntimes, int core, const char *command, const char *language,
                            const char *program, const char *calib_csv_file,
                            const char *process_script, int min_powercap, int max_powercap,
                            int time_out_limit, float temp_threshold, int variance, int memory) {
    if (min_powercap < 1 || max_powercap < min_powercap || !command || !*command) {
        fprintf(stderr, "[ERROR] Powercap calibration requires a command and a range of at least 1 W.\n");
        return -1;
    }
    if (!calib_csv_file || strlen(calib_csv_file) == 0) {
        calib_csv_file = "powercap_calibration.csv";
    }

    printf("[INFO] Powercap calibration: Testing %s with different power limits...\n", program);

    FILE *outputFile = fopen(calib_csv_file, "w");
    if (!outputFile) {
        perror("[ERROR] Failed to open calibration CSV for writing");
        return -1;
    }

    energy_backend_write_csv_header(outputFile, 0);

    int result = 0;

    for (int power_cap = min_powercap; power_cap <= max_powercap; power_cap++) {
        printf("[INFO] Powercap calibration: Testing with power cap = %d W\n", power_cap);

        if (intel_set_powercap(power_cap) != 0) {
            fprintf(stderr, "[ERROR] Failed to set powercap %d W. Skipping...\n", power_cap);
            result = -1;
            continue;
        }

        char filename[256];
        snprintf(filename, sizeof(filename), "/tmp/%s_%d.csv", program, power_cap);

        if (initialize_calibration_csv(filename) != 0) {
            result = -1;
            if (intel_destroy_powercap() != 0) result = -1;
            continue;
        }

        runTesting(ntimes, core, command, language, program, filename,
                   power_cap, time_out_limit, temp_threshold, variance, memory);

        if (intel_destroy_powercap() != 0) result = -1;
        if (append_calibration_rows(outputFile, filename) != 0) result = -1;
        remove(filename);

        sleep(1);
    }

    if (fclose(outputFile) != 0) result = -1;
    if (result != 0) return -1;

    if (process_script && access(process_script, F_OK) == 0) {
        char proc_cmd[1024];
        snprintf(proc_cmd, sizeof(proc_cmd), "python3 \"%s\" \"%s\"", process_script, calib_csv_file);
        printf("[INFO] Running calibration post-processor: %s\n", proc_cmd);
        if (system(proc_cmd) != 0) {
            fprintf(stderr, "[ERROR] Python post-processing script exited with error.\n");
            return -1;
        }
    } else {
        printf("[INFO] Powercap calibration CSV saved to %s\n", calib_csv_file);
    }
    return 0;
}

CpuBackend intel_cpu_backend = {
    .name = "Intel RAPL CPU",
    .detect = intel_detect,
    .init = intel_init,
    .cleanup = intel_cleanup,
    .sample_start = intel_sample_start,
    .sample_stop = intel_sample_stop,
    .metrics = intel_metrics,
    .metric_count = sizeof(intel_metrics) / sizeof(intel_metrics[0]),
    .get_metric = intel_get_metric,
    .set_powercap = intel_set_powercap,
    .destroy_powercap = intel_destroy_powercap,
    .calibrate_powercap = intel_calibrate_powercap,
    .diagnose = intel_diagnose
};

const EnergyBackendDescriptor energy_backend_descriptor = {
    .type = ENERGY_BACKEND_CPU,
    .priority = 0,
    .backend.cpu = &intel_cpu_backend
};
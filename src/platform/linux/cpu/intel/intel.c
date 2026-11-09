#include "intel.h"
#include "../../../../include/energy-counter.h"
#include <pthread.h>
#include <regex.h>
#include <sensors/sensors.h>

static double intel_metric_package(int core, pid_t pid);
static double intel_metric_core(int core, pid_t pid);
static double intel_metric_dram(int core, pid_t pid);
static double intel_metric_temperature(int core, pid_t pid);

static const EnergyMetricDefinition intel_metric_definitions[] = {
    {
        .descriptor = {
            .name = "Package",
            .unit = "J",
            .kind = ENERGY_METRIC_WINDOW,
            .scope = ENERGY_METRIC_DEVICE,
            .flags = ENERGY_METRIC_FLAG_NONE
        },
        .function = ENERGY_METRIC_FUNCTION(intel_metric_package),
        .info = {.description = "CPU package energy", .origin = "Intel RAPL package MSR"}
    },
    {
        .descriptor = {
            .name = "Core",
            .unit = "J",
            .kind = ENERGY_METRIC_WINDOW,
            .scope = ENERGY_METRIC_DEVICE,
            .flags = ENERGY_METRIC_FLAG_NONE
        },
        .function = ENERGY_METRIC_FUNCTION(intel_metric_core),
        .info = {.description = "CPU cores energy", .origin = "Intel RAPL PP0 MSR"}
    },
    {
        .descriptor = {
            .name = "DRAM",
            .unit = "J",
            .kind = ENERGY_METRIC_WINDOW,
            .scope = ENERGY_METRIC_DEVICE,
            .flags = ENERGY_METRIC_FLAG_NONE
        },
        .function = ENERGY_METRIC_FUNCTION(intel_metric_dram),
        .info = {.description = "DRAM energy", .origin = "Intel RAPL DRAM MSR"}
    },
    {
        .descriptor = {
            .name = "Temperature",
            .unit = "C",
            .kind = ENERGY_METRIC_GAUGE,
            .scope = ENERGY_METRIC_DEVICE,
            .flags = ENERGY_METRIC_FLAG_CPU_TEMPERATURE
        },
        .function = ENERGY_METRIC_FUNCTION(intel_metric_temperature),
        .info = {.description = "Average CPU core temperature", .origin = "lm-sensors coretemp"}
    }
};
ENERGY_DEFINE_METRIC_TABLE(intel, intel_metric_definitions)

static int intel_cpu_model = -1;
static uint32_t package_before = 0, package_after = 0;
static uint32_t core_before = 0, core_after = 0;
static uint32_t dram_before = 0, dram_after = 0;
static int package_before_valid = 0, package_after_valid = 0;
static int core_before_valid = 0, core_after_valid = 0;
static int dram_before_valid = 0, dram_after_valid = 0;
static int next_sample_has_previous = 0;

static double energy_units = 0.0;
static int energy_units_valid = 0;
static raplcap global_rc;
static int raplcap_active = 0;
typedef struct {
    uint32_t package, die;
    raplcap_limit long_limit, short_limit;
    int enabled;
    int modified;
} IntelPowercapSnapshot;
static IntelPowercapSnapshot *powercap_snapshots = NULL;
static size_t powercap_snapshot_count = 0;

extern int runTesting(int ntimes, int core, const char *command, const char *language, 
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
    next_sample_has_previous = 0;
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
    return (double)energy_counter_delta32(package_after, package_before) * energy_units;
}

static double intel_get_core_energy(int core) {
    (void)core;
    if (!energy_units_valid || !core_before_valid || !core_after_valid) {
        return METRIC_NOT_SUPPORTED;
    }
    return (double)energy_counter_delta32(core_after, core_before) * energy_units;
}

static double intel_get_dram_energy(int core) {
    (void)core;
    if (!energy_units_valid || !dram_before_valid || !dram_after_valid) {
        return METRIC_NOT_SUPPORTED;
    }
    return (double)energy_counter_delta32(dram_after, dram_before) * energy_units;
}

int energy_backend_sample_next(int core) {
    if (next_sample_has_previous) {
        package_before = package_after;
        core_before = core_after;
        dram_before = dram_after;
        package_before_valid = package_after_valid;
        core_before_valid = core_after_valid;
        dram_before_valid = dram_after_valid;
    }
    int result = intel_sample_stop(core);
    next_sample_has_previous = result == 0;
    return result;
}

static pthread_once_t intel_sensors_once = PTHREAD_ONCE_INIT;
static pthread_mutex_t intel_sensors_mutex = PTHREAD_MUTEX_INITIALIZER;
static int intel_sensors_status = -1;

static void intel_sensors_initialize(void) {
    intel_sensors_status = sensors_init(NULL);
    if (intel_sensors_status != 0)
        fprintf(stderr, "[WARNING] Failed to initialize the lm-sensors library.\n");
}

static float intel_get_temperature(int core) {
    (void)core;
    if (pthread_once(&intel_sensors_once, intel_sensors_initialize) != 0 ||
        intel_sensors_status != 0) return -1.0f;
    if (pthread_mutex_lock(&intel_sensors_mutex) != 0) return -1.0f;

    double sum = 0.0;
    size_t count = 0;
    int chip_index = 0;
    const sensors_chip_name *chip;
    while ((chip = sensors_get_detected_chips(NULL, &chip_index)) != NULL) {
        if (!chip->prefix || strcmp(chip->prefix, "coretemp") != 0) continue;
        int feature_index = 0;
        const sensors_feature *feature;
        while ((feature = sensors_get_features(chip, &feature_index)) != NULL) {
            if (feature->type != SENSORS_FEATURE_TEMP) continue;
            char *label = sensors_get_label(chip, feature);
            int is_core = label && strncmp(label, "Core ", 5) == 0;
            free(label);
            if (!is_core) continue;

            const sensors_subfeature *input = sensors_get_subfeature(
                chip, feature, SENSORS_SUBFEATURE_TEMP_INPUT);
            double value;
            if (input && (input->flags & SENSORS_MODE_R) &&
                sensors_get_value(chip, input->number, &value) == 0 &&
                isfinite(value) && value > 0.0) {
                sum += value;
                count++;
            }
        }
    }

    pthread_mutex_unlock(&intel_sensors_mutex);
    return count ? (float)(sum / (double)count) : -1.0f;
}

static void intel_sensors_cleanup(void) {
    if (intel_sensors_status == 0) {
        sensors_cleanup();
        intel_sensors_status = -1;
    }
}

static double intel_metric_package(int core, pid_t pid) {
    (void)pid;
    return intel_get_package_energy(core);
}
static double intel_metric_core(int core, pid_t pid) {
    (void)pid;
    return intel_get_core_energy(core);
}
static double intel_metric_dram(int core, pid_t pid) {
    (void)pid;
    return intel_get_dram_energy(core);
}
static double intel_metric_temperature(int core, pid_t pid) {
    (void)pid;
    return intel_get_temperature(core);
}

static int intel_destroy_powercap(void);

static int intel_set_powercap(int watts) {
    if (watts == -1) return 0;
    if (watts <= 0) {
        fprintf(stderr, "[ERROR] RAPL powercap must be -1 or a positive number of watts.\n");
        return -1;
    }
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
            IntelPowercapSnapshot *snapshots = realloc(
                powercap_snapshots, (powercap_snapshot_count + 1) * sizeof(*snapshots));
            if (!snapshots) {
                fprintf(stderr, "[ERROR] Failed to allocate RAPL powercap snapshots.\n");
                intel_destroy_powercap();
                return -1;
            }
            powercap_snapshots = snapshots;
            IntelPowercapSnapshot *snapshot = &powercap_snapshots[powercap_snapshot_count];
            memset(snapshot, 0, sizeof(*snapshot));
            snapshot->package = q;
            snapshot->die = j;
            snapshot->enabled = raplcap_pd_is_zone_enabled(&global_rc, q, j, RAPLCAP_ZONE_PACKAGE);
            if (snapshot->enabled < 0 ||
                raplcap_pd_get_limits(&global_rc, q, j, RAPLCAP_ZONE_PACKAGE,
                                      &snapshot->long_limit, &snapshot->short_limit) != 0 ||
                !isfinite(snapshot->long_limit.watts) || snapshot->long_limit.watts <= 0 ||
                !isfinite(snapshot->short_limit.watts) || snapshot->short_limit.watts <= 0 ||
                !isfinite(snapshot->long_limit.seconds) || snapshot->long_limit.seconds < 0 ||
                !isfinite(snapshot->short_limit.seconds) || snapshot->short_limit.seconds < 0) {
                fprintf(stderr, "[ERROR] Failed to read existing RAPL limits for package %u die %u.\n", q, j);
                intel_destroy_powercap();
                return -1;
            }
            powercap_snapshot_count++;
        }
    }

    raplcap_limit long_limit = { .watts = watts, .seconds = 0 };
    raplcap_limit short_limit = { .watts = watts, .seconds = 0 };
    for (size_t i = 0; i < powercap_snapshot_count; i++) {
        IntelPowercapSnapshot *snapshot = &powercap_snapshots[i];
        snapshot->modified = 1;
        if (raplcap_pd_set_limits(&global_rc, snapshot->package, snapshot->die, RAPLCAP_ZONE_PACKAGE,
                                  &long_limit, &short_limit) != 0) {
            perror("[ERROR] raplcap_pd_set_limits");
            intel_destroy_powercap();
            return -1;
        }
    }
    for (size_t i = 0; i < powercap_snapshot_count; i++) {
        IntelPowercapSnapshot *snapshot = &powercap_snapshots[i];
        if (raplcap_pd_set_zone_enabled(&global_rc, snapshot->package, snapshot->die,
                                        RAPLCAP_ZONE_PACKAGE, 1) != 0) {
            perror("[ERROR] raplcap_pd_set_zone_enabled");
            intel_destroy_powercap();
            return -1;
        }
    }
    return 0;
}

static int intel_destroy_powercap(void) {
    if (!raplcap_active) return 0;
    int result = 0;
    for (size_t i = 0; i < powercap_snapshot_count; i++) {
        IntelPowercapSnapshot *snapshot = &powercap_snapshots[i];
        if (!snapshot->modified) continue;
        if (raplcap_pd_set_limits(&global_rc, snapshot->package, snapshot->die,
                                  RAPLCAP_ZONE_PACKAGE, &snapshot->long_limit,
                                  &snapshot->short_limit) != 0) {
            fprintf(stderr, "[ERROR] Failed to restore RAPL limits for package %u die %u.\n",
                    snapshot->package, snapshot->die);
            result = -1;
        }
        if (raplcap_pd_set_zone_enabled(&global_rc, snapshot->package, snapshot->die,
                                       RAPLCAP_ZONE_PACKAGE, snapshot->enabled) != 0) {
            fprintf(stderr, "[ERROR] Failed to restore RAPL zone state for package %u die %u.\n",
                    snapshot->package, snapshot->die);
            result = -1;
        }
    }
    if (raplcap_destroy(&global_rc) != 0) {
        fprintf(stderr, "[ERROR] Failed to destroy the RAPL powercap context.\n");
        result = -1;
    }
    free(powercap_snapshots);
    powercap_snapshots = NULL;
    powercap_snapshot_count = 0;
    raplcap_active = 0;
    return result;
}

static void intel_cleanup(void) {
    intel_destroy_powercap();
    intel_sensors_cleanup();
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
    energy_share_output_file(fileno(outputFile));

    energy_backend_write_csv_header(outputFile, 0);

    int result = 0;

    printf("[INFO] Baseline uses the existing system limits; -1 does not remove them.\n");
    for (int power_cap = -1; ; power_cap = power_cap == -1 ? min_powercap : power_cap + 1) {
        printf("[INFO] Powercap calibration: Testing with power cap = %d W\n", power_cap);

        if (power_cap != -1 && intel_set_powercap(power_cap) != 0) {
            fprintf(stderr, "[ERROR] Failed to set powercap %d W; calibration aborted.\n", power_cap);
            result = -1;
            break;
        }

        char filename[] = "/tmp/greenmeasurer-calibration-XXXXXX";
        int temporary_fd = mkstemp(filename);
        if (temporary_fd < 0) {
            perror("[ERROR] Failed to create temporary calibration CSV");
            if (power_cap != -1 && intel_destroy_powercap() != 0) result = -1;
            result = -1;
            break;
        }
        if (close(temporary_fd) != 0) {
            perror("[ERROR] Failed to close temporary calibration CSV");
            remove(filename);
            if (power_cap != -1 && intel_destroy_powercap() != 0) result = -1;
            result = -1;
            break;
        }

        if (initialize_calibration_csv(filename) != 0) {
            result = -1;
            remove(filename);
            if (power_cap != -1 && intel_destroy_powercap() != 0) result = -1;
            break;
        }

        if (runTesting(ntimes, core, command, language, program, filename,
                       power_cap, time_out_limit, temp_threshold, variance, memory) != 0) {
            fprintf(stderr, "[ERROR] Calibration measurement failed for powercap %d.\n", power_cap);
            result = -1;
        }

        if (power_cap != -1 && intel_destroy_powercap() != 0) result = -1;
        if (result == 0 && append_calibration_rows(outputFile, filename) != 0) result = -1;
        if (remove(filename) != 0) {
            perror("[ERROR] Failed to remove temporary calibration CSV");
            result = -1;
        }

        if (result != 0 || power_cap == max_powercap) break;
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
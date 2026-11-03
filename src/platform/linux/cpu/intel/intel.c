#include "intel.h"
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

    if (result == (uint64_t)-1) return -1;
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

static float intel_get_temperature(int core) {
    (void)core;
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

static int intel_set_powercap(int watts) {
    if (watts <= 0) return 0;
    if (raplcap_init(&global_rc)) return -1;
    raplcap_active = 1;

    uint32_t n = raplcap_get_num_packages(NULL);
    if (n == 0) {
        raplcap_destroy(&global_rc);
        raplcap_active = 0;
        return -1;
    }

    uint32_t d = raplcap_get_num_die(&global_rc, 0);
    raplcap_limit rl = { .watts = watts, .seconds = 0 };

    for (uint32_t q = 0; q < n; q++) {
        for (uint32_t j = 0; j < d; j++) {
            raplcap_pd_set_limits(&global_rc, q, j, RAPLCAP_ZONE_PACKAGE, &rl, &rl);
            raplcap_pd_set_zone_enabled(&global_rc, q, j, RAPLCAP_ZONE_PACKAGE, 1);
        }
    }
    return 0;
}

static int intel_destroy_powercap(void) {
    if (raplcap_active) {
        raplcap_destroy(&global_rc);
        raplcap_active = 0;
    }
    return 0;
}

static void intel_cleanup(void) {
    intel_destroy_powercap();
}

void intel_calibrate_powercap(int ntimes, int core, const char *command, const char *language, 
                             const char *program, const char *calib_csv_file,
                             const char *process_script, int min_powercap, int max_powercap, 
                             int time_out_limit, float temp_threshold, int variance, int memory) {
    if (!calib_csv_file || strlen(calib_csv_file) == 0) {
        calib_csv_file = "powercap_calibration.csv";
    }

    printf("[INFO] Powercap calibration: Starting baseline and sweep for %s...\n", program);

    FILE *outputFile = fopen(calib_csv_file, "w");
    if (!outputFile) {
        perror("[ERROR] Failed to open calibration CSV for writing");
        return;
    }

    fprintf(outputFile, "Language,Program,Powercap,Package,Core,GPU,DRAM,Time,CpuTemperature,GpuTemperature,Memory\n");

    printf("[INFO] Powercap calibration: Measuring uncapped baseline (Powercap = -1)...\n");
    char base_filename[256];
    snprintf(base_filename, sizeof(base_filename), "/tmp/%s_baseline.csv", program);

    runTesting(ntimes, core, command, language, program, base_filename,
               -1, time_out_limit, temp_threshold, variance, memory);

    FILE *baseFile = fopen(base_filename, "r");
    if (baseFile) {
        char buffer[1024];
        while (fgets(buffer, sizeof(buffer), baseFile)) {
            fputs(buffer, outputFile);
        }
        fclose(baseFile);
        remove(base_filename);
    } else {
        fprintf(stderr, "[ERROR] Failed to read baseline calibration run\n");
    }

    sleep(1);

    for (int power_cap = min_powercap; power_cap <= max_powercap; power_cap++) {
        printf("[INFO] Powercap calibration: Testing with WHATTSCAP = %d\n", power_cap);

        if (intel_set_powercap(power_cap) != 0) {
            fprintf(stderr, "[ERROR] Failed to set powercap %d W. Skipping...\n", power_cap);
            continue;
        }

        char filename[256];
        snprintf(filename, sizeof(filename), "/tmp/%s_%d.csv", program, power_cap);

        runTesting(ntimes, core, command, language, program, filename,
                   power_cap, time_out_limit, temp_threshold, variance, memory);

        FILE *inputFile = fopen(filename, "r");
        if (inputFile) {
            char buffer[1024];
            while (fgets(buffer, sizeof(buffer), inputFile)) {
                fputs(buffer, outputFile);
            }
            fclose(inputFile);
            remove(filename);
        }

        if (intel_destroy_powercap() != 0) {
            fprintf(stderr, "[ERROR] Error destroying CAP after test with %d W\n", power_cap);
        }

        sleep(1);
    }

    fclose(outputFile);

    if (process_script && access(process_script, F_OK) == 0) {
        char proc_cmd[1024];
        snprintf(proc_cmd, sizeof(proc_cmd), "python3 \"%s\" \"%s\"", process_script, calib_csv_file);
        printf("[INFO] Running calibration post-processor: %s\n", proc_cmd);
        if (system(proc_cmd) != 0) {
            fprintf(stderr, "[WARNING] Python post-processing script exited with error\n");
        }
    } else {
        printf("[INFO] Powercap calibration CSV saved to %s\n", calib_csv_file);
    }
}

CpuBackend intel_cpu_backend = {
    .name = "Intel RAPL CPU",
    .detect = intel_detect,
    .init = intel_init,
    .cleanup = intel_cleanup,
    .sample_start = intel_sample_start,
    .sample_stop = intel_sample_stop,
    .get_package_energy = intel_get_package_energy,
    .get_core_energy = intel_get_core_energy,
    .get_dram_energy = intel_get_dram_energy,
    .get_temperature = intel_get_temperature,
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
#include "intel.h"
#include <regex.h>

static int intel_cpu_model = -1;
static double package_before, package_after;
static double pp0_before, pp0_after;
static double pp1_before = 0.0, pp1_after;
static double dram_before = 0.0, dram_after;

static double power_units, energy_units, time_units;

static raplcap global_rc;
static int raplcap_active = 0;

/* External callback to run benchmark testing loop defined in main runner */
extern void runTesting(int ntimes, int core, const char *command, const char *language, 
                       const char *program, const char *measurements_file, int powercap, 
                       int time_out_limit, float temp_threshold, int variance, int memory);

                       
static int open_msr(int core) {
    char msr_filename[BUFSIZ];
    int fd;

    snprintf(msr_filename, sizeof(msr_filename), "/dev/cpu/%d/msr", core);
    fd = open(msr_filename, O_RDONLY);
    if (fd < 0) {
        if (errno == ENXIO) {
            fprintf(stderr, "[WARNING] rdmsr: No CPU %d\n", core);
            exit(2);
        } else if (errno == EIO) {
            fprintf(stderr, "[WARNING] rdmsr: CPU %d doesn't support MSRs\n", core);
            exit(3);
        } else {
            perror("rdmsr:open");
            fprintf(stderr, "[ERROR] Trying to open %s\n", msr_filename);
            exit(127);
        }
    }
    return fd;
}

static uint64_t read_msr(int fd, int which) {
    uint64_t data = (uint64_t)-1;

    if (pread(fd, &data, sizeof(data), (off_t)which) != sizeof(data)) {
        if (errno == EIO) {
            fprintf(stderr, "[WARNING] MSR 0x%x not available on this CPU.\n", which);
        } else {
            fprintf(stderr, "[ERROR] reading MSR 0x%x: %s\n", which, strerror(errno));
        }
        return (uint64_t)-1;
    }
    return data;
}

static int detect_intel_cpu(void) {
    FILE *fff = fopen("/proc/cpuinfo", "r");
    if (fff == NULL) return -1;

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

    switch (model) {
        case CPU_SANDYBRIDGE:
        case CPU_SANDYBRIDGE_EP:
        case CPU_IVYBRIDGE:
        case CPU_IVYBRIDGE_EP:
        case CPU_HASWELL:
        case CPU_HASWELL_ULT:
        case CPU_HASWELL_GT3E:
        case CPU_HASWELL_EP:
        case CPU_BROADWELL:
        case CPU_BROADWELL_GT3E:
        case CPU_BROADWELL_EP:
        case CPU_BROADWELL_DE:
        case CPU_SKYLAKE:
        case CPU_SKYLAKE_HS:
        case CPU_SKYLAKE_X:
        case CPU_KNIGHTS_LANDING:
        case CPU_KNIGHTS_MILL:
        case CPU_KABYLAKE_MOBILE:
        case CPU_KABYLAKE:
        case CPU_ATOM_SILVERMONT:
        case CPU_ATOM_AIRMONT:
        case CPU_ATOM_MERRIFIELD:
        case CPU_ATOM_MOOREFIELD:
        case CPU_ATOM_GOLDMONT:
        case CPU_ATOM_GEMINI_LAKE:
        case CPU_ATOM_DENVERTON:
        case CANNONLAKE_L:
        case ICELAKE_L:
        case ICELAKE:
        case ICELAKE_D:
        case ICELAKE_X:
        case COMETLAKE_L:
        case COMETLAKE:
        case TIGERLAKE_L:
        case TIGERLAKE:
        case ALDERLAKE:
        case ALDERLAKE_L:
        case ALDERLAKE_N:
        case RAPTORLAKE:
        case RAPTORLAKE_P:
        case RAPTORLAKE_S:
        case METEORLAKE:
        case METEORLAKE_L:
        case SAPPHIRERAPIDS_X:
        case EMERALDRAPIDS_X:
            return model;
        default:
            return -1;
    }
}

static int intel_detect(void) {
    intel_cpu_model = detect_intel_cpu();
    return (intel_cpu_model >= 0) ? 1 : 0;
}

static int intel_init(int core) {
    if (intel_cpu_model < 0) {
        intel_cpu_model = detect_intel_cpu();
        if (intel_cpu_model < 0) return -1;
    }

    int fd = open_msr(core);
    uint64_t result = read_msr(fd, MSR_RAPL_POWER_UNIT);
    close(fd);

    if (result == (uint64_t)-1) return -1;

    power_units  = pow(0.5, (double)(result & 0xf));
    energy_units = pow(0.5, (double)((result >> 8) & 0x1f));
    time_units   = pow(0.5, (double)((result >> 16) & 0xf));

    return 0;
}

static int intel_before(int core) {
    int fd = open_msr(core);
    uint64_t result;
    int flag = 1;

    result = read_msr(fd, MSR_PKG_ENERGY_STATUS);
    package_before = (double)result * energy_units;
    if (package_before < 0) flag = -1;

    result = read_msr(fd, MSR_PP0_ENERGY_STATUS);
    pp0_before = (double)result * energy_units;
    if (pp0_before < 0) flag = -1;

    result = read_msr(fd, MSR_PP1_ENERGY_STATUS);
    pp1_before = (double)result * energy_units;
    if (pp1_before < 0) flag = -1;

    result = read_msr(fd, MSR_DRAM_ENERGY_STATUS);
    dram_before = (double)result * energy_units;
    if (dram_before < 0) flag = -1;

    close(fd);
    return flag;
}

static int intel_after(int core, EnergyMetrics *out) {
    int fd = open_msr(core);
    uint64_t result;
    int has_negative = 0;

    result = read_msr(fd, MSR_PKG_ENERGY_STATUS);
    package_after = (double)result * energy_units;
    double pkg_diff = package_after - package_before;
    if (pkg_diff < 0) has_negative = 1;

    result = read_msr(fd, MSR_PP0_ENERGY_STATUS);
    pp0_after = (double)result * energy_units;
    double pp0_diff = pp0_after - pp0_before;
    if (pp0_diff < 0) has_negative = 1;

    result = read_msr(fd, MSR_PP1_ENERGY_STATUS);
    pp1_after = (double)result * energy_units;
    double pp1_diff = pp1_after - pp1_before;
    if (pp1_diff < 0) has_negative = 1;

    result = read_msr(fd, MSR_DRAM_ENERGY_STATUS);
    dram_after = (double)result * energy_units;
    double dram_diff = dram_after - dram_before;
    if (dram_diff < 0) has_negative = 1;

    close(fd);

    if (has_negative) {
        fprintf(stderr, "[ERROR] Negative delta detected in intel_after().\n");
        return -1;
    }

    if (out) {
        out->package = pkg_diff;
        out->core = pp0_diff;
        out->gpu = pp1_diff;
        out->dram = dram_diff;
    }
    return 1;
}

int intel_set_powercap(int watts) {
    if (watts <= 0) return 0;

    if (raplcap_init(&global_rc) != 0) {
        perror("[ERROR] raplcap_init");
        return -1;
    }
    raplcap_active = 1;

    uint32_t n = raplcap_get_num_packages(NULL);
    if (n == 0) return -1;

    uint32_t d = raplcap_get_num_die(&global_rc, 0);
    if (d == 0) d = 1;

    raplcap_limit rl_short, rl_long;
    rl_long.watts = watts;
    rl_long.seconds = 0;
    rl_short.watts = watts;
    rl_short.seconds = 0;

    for (uint32_t q = 0; q < n; q++) {
        for (uint32_t j = 0; j < d; j++) {
            if (raplcap_pd_set_limits(&global_rc, q, j, RAPLCAP_ZONE_PACKAGE, &rl_long, &rl_short) != 0) {
                perror("[WARNING] raplcap_pd_set_limits");
            }
        }
    }

    for (uint32_t q = 0; q < n; q++) {
        for (uint32_t j = 0; j < d; j++) {
            if (raplcap_pd_set_zone_enabled(&global_rc, q, j, RAPLCAP_ZONE_PACKAGE, 1) != 0) {
                perror("[WARNING] raplcap_pd_set_zone_enabled");
            }
        }
    }

    return 0;
}

int intel_destroy_powercap(void) {
    if (raplcap_active) {
        uint32_t n = raplcap_get_num_packages(NULL);
        uint32_t d = raplcap_get_num_die(&global_rc, 0);
        if (d == 0) d = 1;

        for (uint32_t q = 0; q < n; q++) {
            for (uint32_t j = 0; j < d; j++) {
                raplcap_pd_set_zone_enabled(&global_rc, q, j, RAPLCAP_ZONE_PACKAGE, 0);
            }
        }

        raplcap_destroy(&global_rc);
        raplcap_active = 0;
    }
    return 0;
}

static void intel_cleanup(void) {
    intel_destroy_powercap();
}

/* Calibration routines moved from main.c */
double intel_calibrate_temperature(int seconds, const char *calib_file) {
    printf("[INFO] Calibrating temperature for %d seconds...\n", seconds);
    sleep(seconds);

    char cmd[512];
    snprintf(cmd, sizeof(cmd), "sensors > %s", calib_file);
    if (system(cmd) != 0) return -1.0;

    FILE *file = fopen(calib_file, "r");
    FILE *output_file = fopen(calib_file, "a");
    if (!file || !output_file) {
        if (file) fclose(file);
        if (output_file) fclose(output_file);
        return -1.0;
    }

    char line[256];
    double temperatures[256];
    int temp_count = 0;
    regex_t regex;
    regmatch_t matches[2];

    if (regcomp(&regex, "Core [0-9]+: *\\+([0-9.]+)°C", REG_EXTENDED) != 0) {
        fclose(file); fclose(output_file);
        return -1.0;
    }

    while (fgets(line, sizeof(line), file)) {
        if (regexec(&regex, line, 2, matches, 0) == 0) {
            char temp_str[32];
            int len = matches[1].rm_eo - matches[1].rm_so;
            if (len >= (int)sizeof(temp_str)) len = sizeof(temp_str) - 1;
            strncpy(temp_str, line + matches[1].rm_so, len);
            temp_str[len] = '\0';
            double temp_val = atof(temp_str);
            if (temp_count < 256) temperatures[temp_count++] = temp_val;

            char *core_name = strtok(line, ":");
            if (core_name) fprintf(output_file, "%s: %.1f°C\n", core_name, temp_val);
        }
    }

    double mean_temp = 0.0;
    for (int i = 0; i < temp_count; i++) mean_temp += temperatures[i];
    if (temp_count > 0) mean_temp /= temp_count;

    fprintf(output_file, "Average temperature: %.1f°C\n\n", mean_temp);
    regfree(&regex);
    fclose(file);
    fclose(output_file);

    return mean_temp;
}

void intel_calibrate_powercap(int ntimes, int core, const char *command, const char *language, 
                             const char *program, const char *calib_csv_file,
                             const char *process_script, int min_powercap, int max_powercap, 
                             int time_out_limit, float temp_threshold, int variance, int memory) {
    if (!calib_csv_file || strlen(calib_csv_file) == 0) {
        calib_csv_file = "powercap_calibration.csv";
    }

    printf("[INFO] Calibrating Intel power cap for %s (%s) -> %s\n", program, language, calib_csv_file);

    FILE *outputFile = fopen(calib_csv_file, "w");
    if (!outputFile) {
        perror("[ERROR] Failed to open powercap calibration CSV output");
        return;
    }

    fprintf(outputFile, "Language,Program,Powercap,Package,Core,GPU,DRAM,Time,CpuTemperature,GpuTemperature,Memory\n");

    int total_iterations = (max_powercap - min_powercap + 1) + 1;

    for (int i = 0; i < total_iterations; i++) {
        int power_cap = (i == 0) ? -1 : (min_powercap + (i - 1));
        if (power_cap == -1 && i != 0) continue;

        if (intel_set_powercap(power_cap) != 0) continue;

        char temp_filename[512];
        snprintf(temp_filename, sizeof(temp_filename), "/tmp/%s_%d_temp.csv", program, power_cap);

        runTesting(ntimes, core, command, language, program, temp_filename,
                   power_cap, time_out_limit, temp_threshold, variance, memory);

        FILE *inputFile = fopen(temp_filename, "r");
        if (inputFile) {
            char buffer[1024];
            while (fgets(buffer, sizeof(buffer), inputFile)) {
                fputs(buffer, outputFile);
            }
            fclose(inputFile);
            remove(temp_filename);
        }

        intel_destroy_powercap();
        sleep(1);
    }
    fclose(outputFile);

    if (process_script && strlen(process_script) > 0 && access(process_script, F_OK) == 0) {
        char proc_cmd[1024];
        snprintf(proc_cmd, sizeof(proc_cmd), "python3 \"%s\" \"%s\"", process_script, calib_csv_file);
        printf("[INFO] Running calibration post-processor: %s\n", proc_cmd);
        if (system(proc_cmd) != 0) {
            fprintf(stderr, "[WARNING] Python post-processing script exited with error\n");
        }
    } else {
        printf("[INFO] Calibration CSV saved to %s (post-processing skipped)\n", calib_csv_file);
    }
}

static int intel_measure_memory(const char *command) {
    printf("[INFO] Measuring memory consumption of \"%s\" (Intel/Linux)...\n", command);
    char cmd[2048];

    int n = snprintf(cmd, sizeof(cmd),
            "/usr/bin/time -v %s 2>&1 1>/dev/null | grep 'Maximum resident set size' | "
            "sed 's/[^0-9]*\\([0-9]*\\).*/\\1/'",
            command);
    if (n < 0 || n >= (int)sizeof(cmd)) {
        fprintf(stderr, "[ERROR] Command buffer overflow in intel_measure_memory\n");
        return -1;
    }

    FILE *fp = popen(cmd, "r");
    if (!fp) {
        perror("[ERROR] Failed to run /usr/bin/time for memory measurement");
        return -1;
    }

    char buf[128] = "";
    if (fgets(buf, sizeof(buf), fp) == NULL) {
        pclose(fp);
        fprintf(stderr, "[ERROR] Failed to read memory usage output\n");
        return -1;
    }
    pclose(fp);

    int result = atoi(buf);
    printf("[INFO] Memory consumption: %d KB\n", result);
    return result;
}

EnergyBackend intel_backend = {
    .name = "Intel RAPL",
    .detect = intel_detect,
    .init = intel_init,
    .before = intel_before,
    .after = intel_after,
    .cleanup = intel_cleanup,
    .measure_memory = intel_measure_memory,
    .set_powercap = intel_set_powercap,
    .destroy_powercap = intel_destroy_powercap,
    .calibrate_powercap = intel_calibrate_powercap
};
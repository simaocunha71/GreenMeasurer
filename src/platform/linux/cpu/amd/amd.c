#include "amd.h"

static uint32_t amd_pkg_raw_before = 0, amd_pkg_raw_after = 0;
static uint32_t amd_core_raw_before = 0, amd_core_raw_after = 0;
static int amd_pkg_before_valid = 0, amd_pkg_after_valid = 0;
static int amd_core_before_valid = 0, amd_core_after_valid = 0;
static double energy_units = 0.0;
static int energy_units_valid = 0;

static int open_msr(int core) {
    char msr_filename[BUFSIZ];
    snprintf(msr_filename, sizeof(msr_filename), "/dev/cpu/%d/msr", core);
    int fd = open(msr_filename, O_RDONLY);
    if (fd < 0) {
        static int warned = 0;
        if (!warned) {
            fprintf(stderr, "[WARNING] Cannot open %s: %s (AMD MSR energy metrics require root and the msr module).\n",
                    msr_filename, strerror(errno));
            warned = 1;
        }
    }
    return fd;
}

static uint64_t read_msr(int fd, int which) {
    uint64_t data = (uint64_t)-1;
    if (pread(fd, &data, sizeof(data), (off_t)which) != sizeof(data)) {
        return (uint64_t)-1;
    }
    return data;
}

static int amd_detect(void) {
    FILE *fff = fopen("/proc/cpuinfo", "r");
    if (!fff) return 0;

    int family = 0;
    char buffer[BUFSIZ], vendor[BUFSIZ] = "";
    while (fgets(buffer, BUFSIZ, fff)) {
        if (!strncmp(buffer, "vendor_id", 9)) sscanf(buffer, "%*s%*s%s", vendor);
        if (!strncmp(buffer, "cpu family", 10)) sscanf(buffer, "%*s%*s%*s%d", &family);
    }
    fclose(fff);

    return (!strncmp(vendor, "AuthenticAMD", 12) && family >= 0x17) ? 1 : 0;
}

static const char *amd_diagnose(void) {
    return "CPU is not a supported AMD Zen model";
}

static int amd_init(int core) {
    energy_units_valid = 0;
    int fd = open_msr(core);
    if (fd < 0) return -1;

    uint64_t result = read_msr(fd, AMD_MSR_RAPL_PWR_UNIT);
    close(fd);

    if (result == (uint64_t)-1) return -1;
    energy_units = pow(0.5, (double)((result >> 8) & 0x1f));
    energy_units_valid = 1;
    return 0;
}

static int amd_sample_start(int core) {
    int fd = open_msr(core);
    amd_pkg_before_valid = 0;
    amd_core_before_valid = 0;
    if (fd < 0) return -1;

    uint64_t package = read_msr(fd, AMD_MSR_PKG_ENERGY_STAT);
    uint64_t core_energy = read_msr(fd, AMD_MSR_CORE_ENERGY_STAT);
    close(fd);

    if (package != (uint64_t)-1) {
        amd_pkg_raw_before = (uint32_t)package;
        amd_pkg_before_valid = 1;
    }
    if (core_energy != (uint64_t)-1) {
        amd_core_raw_before = (uint32_t)core_energy;
        amd_core_before_valid = 1;
    }
    return (amd_pkg_before_valid && amd_core_before_valid) ? 0 : -1;
}

static int amd_sample_stop(int core) {
    int fd = open_msr(core);
    amd_pkg_after_valid = 0;
    amd_core_after_valid = 0;
    if (fd < 0) return -1;

    uint64_t package = read_msr(fd, AMD_MSR_PKG_ENERGY_STAT);
    uint64_t core_energy = read_msr(fd, AMD_MSR_CORE_ENERGY_STAT);
    close(fd);

    if (package != (uint64_t)-1) {
        amd_pkg_raw_after = (uint32_t)package;
        amd_pkg_after_valid = 1;
    }
    if (core_energy != (uint64_t)-1) {
        amd_core_raw_after = (uint32_t)core_energy;
        amd_core_after_valid = 1;
    }
    return (amd_pkg_after_valid && amd_core_after_valid) ? 0 : -1;
}

static double amd_get_package_energy(int core) {
    (void)core;
    if (!energy_units_valid || !amd_pkg_before_valid || !amd_pkg_after_valid) {
        return METRIC_NOT_SUPPORTED;
    }
    uint32_t delta_ticks = (uint32_t)(amd_pkg_raw_after - amd_pkg_raw_before);
    return (double)delta_ticks * energy_units;
}

static double amd_get_core_energy(int core) {
    (void)core;
    if (!energy_units_valid || !amd_core_before_valid || !amd_core_after_valid) {
        return METRIC_NOT_SUPPORTED;
    }
    uint32_t delta_ticks = (uint32_t)(amd_core_raw_after - amd_core_raw_before);
    return (double)delta_ticks * energy_units;
}

static double amd_get_dram_energy(int core) {
    (void)core;
    return METRIC_NOT_SUPPORTED; /* AMD Zen MSRs do not expose independent DRAM energy */
}

/* Reads the k10temp hwmon sensor directly; no root or lm-sensors required. */
static float amd_read_hwmon_temperature(void) {
    for (int hwmon_index = 0; hwmon_index < 64; hwmon_index++) {
        char path[128], name[64] = "";
        snprintf(path, sizeof(path), "/sys/class/hwmon/hwmon%d/name", hwmon_index);
        FILE *name_file = fopen(path, "r");
        if (!name_file) continue;
        int has_name = fgets(name, sizeof(name), name_file) != NULL;
        fclose(name_file);
        if (!has_name || strncmp(name, "k10temp", 7) != 0) continue;

        for (int sensor = 1; sensor <= 3; sensor++) {
            snprintf(path, sizeof(path), "/sys/class/hwmon/hwmon%d/temp%d_input",
                     hwmon_index, sensor);
            FILE *input = fopen(path, "r");
            if (!input) continue;
            long millidegrees = 0;
            int ok = fscanf(input, "%ld", &millidegrees) == 1;
            fclose(input);
            if (ok && millidegrees > 0 && millidegrees < 150000) {
                return (float)millidegrees / 1000.0f;
            }
        }
    }
    return -1.0f;
}

static float amd_get_temperature(int core) {
    (void)core;
    float hwmon_temperature = amd_read_hwmon_temperature();
    if (hwmon_temperature > 0.0f) return hwmon_temperature;

    const char *cmd = "sensors 2>/dev/null | awk -F: '/^(Tdie|Tctl):/ { split($2, a, \"+\"); split(a[2], b, \"°\"); printf \"%%.2f\\n\", b[1]; exit }'";
    FILE *fp = popen(cmd, "r");
    if (!fp) return -1.0f;

    char buf[64] = "";
    if (!fgets(buf, sizeof(buf), fp)) {
        pclose(fp);
        return -1.0f;
    }
    pclose(fp);
    float value = (float)atof(buf);
    return value > 0.0f ? value : -1.0f;
}

static const EnergyMetricDescriptor amd_metrics[] = {
    {"Package", "J", ENERGY_METRIC_WINDOW, ENERGY_METRIC_DEVICE, ENERGY_METRIC_FLAG_NONE},
    {"Core", "J", ENERGY_METRIC_WINDOW, ENERGY_METRIC_DEVICE, ENERGY_METRIC_FLAG_NONE},
    {"DRAM", "J", ENERGY_METRIC_WINDOW, ENERGY_METRIC_DEVICE, ENERGY_METRIC_FLAG_NONE},
    {"Temperature", "C", ENERGY_METRIC_GAUGE, ENERGY_METRIC_DEVICE,
     ENERGY_METRIC_FLAG_CPU_TEMPERATURE}
};

static double amd_get_metric(size_t metric_index, int core, pid_t process_id) {
    (void)process_id;
    switch (metric_index) {
        case 0: return amd_get_package_energy(core);
        case 1: return amd_get_core_energy(core);
        case 2: return amd_get_dram_energy(core);
        case 3: return (double)amd_get_temperature(core);
        default: return METRIC_NOT_SUPPORTED;
    }
}

static void amd_cleanup(void) {}

CpuBackend amd_cpu_backend = {
    .name = "AMD Zen RAPL CPU",
    .detect = amd_detect,
    .init = amd_init,
    .cleanup = amd_cleanup,
    .sample_start = amd_sample_start,
    .sample_stop = amd_sample_stop,
    .metrics = amd_metrics,
    .metric_count = sizeof(amd_metrics) / sizeof(amd_metrics[0]),
    .get_metric = amd_get_metric,
    .set_powercap = NULL,
    .destroy_powercap = NULL,
    .calibrate_powercap = NULL,
    .diagnose = amd_diagnose
};

const EnergyBackendDescriptor energy_backend_descriptor = {
    .type = ENERGY_BACKEND_CPU,
    .priority = 0,
    .backend.cpu = &amd_cpu_backend
};
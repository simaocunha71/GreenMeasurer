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
        perror("rdmsr:open");
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

static float amd_get_temperature(int core) {
    (void)core;
    const char *cmd = "sensors 2>/dev/null | awk -F: '/^(Tdie|Tctl):/ { split($2, a, \"+\"); split(a[2], b, \"°\"); printf \"%%.2f\\n\", b[1]; exit }'";
    FILE *fp = popen(cmd, "r");
    if (!fp) return -1.0f;

    char buf[64] = "";
    if (!fgets(buf, sizeof(buf), fp)) {
        pclose(fp);
        return -1.0f;
    }
    pclose(fp);
    return (float)atof(buf);
}

static void amd_cleanup(void) {}

CpuBackend amd_cpu_backend = {
    .name = "AMD Zen RAPL CPU",
    .detect = amd_detect,
    .init = amd_init,
    .cleanup = amd_cleanup,
    .sample_start = amd_sample_start,
    .sample_stop = amd_sample_stop,
    .get_package_energy = amd_get_package_energy,
    .get_core_energy = amd_get_core_energy,
    .get_dram_energy = amd_get_dram_energy,
    .get_temperature = amd_get_temperature,
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
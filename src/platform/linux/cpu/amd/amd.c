#include "amd.h"

/* 32-bit raw ticks for AMD delta computation */
static uint32_t amd_pkg_raw_before = 0;
static uint32_t amd_core_raw_before = 0;

static double power_units = 0.0;
static double energy_units = 0.0;
static double time_units = 0.0;

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

static int amd_detect(void) {
    FILE *fff = fopen("/proc/cpuinfo", "r");
    if (fff == NULL) return 0;

    int family = 0;
    char buffer[BUFSIZ];
    char vendor[BUFSIZ] = "";

    while (fgets(buffer, BUFSIZ, fff) != NULL) {
        if (!strncmp(buffer, "vendor_id", 9)) {
            sscanf(buffer, "%*s%*s%s", vendor);
        }
        if (!strncmp(buffer, "cpu family", 10)) {
            sscanf(buffer, "%*s%*s%*s%d", &family);
        }
    }
    fclose(fff);

    if (!strncmp(vendor, "AuthenticAMD", 12)) {
        if (family >= 0x17) {
            printf("[INFO] Found AMD Zen (Family >= 17h) CPU\n");
            return 1;
        } else {
            fprintf(stderr, "[ERROR] AMD CPU Family %d older than Zen is not supported\n", family);
            return 0;
        }
    }
    return 0;
}

static int amd_init(int core) {
    int fd = open_msr(core);
    uint64_t result = read_msr(fd, AMD_MSR_RAPL_PWR_UNIT);
    close(fd);

    if (result == (uint64_t)-1) {
        fprintf(stderr, "[ERROR] Failed to read AMD_MSR_RAPL_PWR_UNIT\n");
        return -1;
    }

    power_units  = pow(0.5, (double)(result & 0xf));
    energy_units = pow(0.5, (double)((result >> 8) & 0x1f));
    time_units   = pow(0.5, (double)((result >> 16) & 0xf));

    return 0;
}

static int amd_before(int core) {
    int fd = open_msr(core);

    uint64_t result = read_msr(fd, AMD_MSR_PKG_ENERGY_STAT);
    amd_pkg_raw_before = (uint32_t)(result & 0xFFFFFFFF);

    result = read_msr(fd, AMD_MSR_CORE_ENERGY_STAT);
    amd_core_raw_before = (uint32_t)(result & 0xFFFFFFFF);

    close(fd);
    return 1;
}

static int amd_after(int core, EnergyMetrics *out) {
    int fd = open_msr(core);

    uint64_t result = read_msr(fd, AMD_MSR_PKG_ENERGY_STAT);
    uint32_t amd_pkg_raw_after = (uint32_t)(result & 0xFFFFFFFF);

    result = read_msr(fd, AMD_MSR_CORE_ENERGY_STAT);
    uint32_t amd_core_raw_after = (uint32_t)(result & 0xFFFFFFFF);

    close(fd);

    uint32_t pkg_delta_ticks = (uint32_t)DELTA_WRAP32(amd_pkg_raw_after, amd_pkg_raw_before);
    uint32_t core_delta_ticks = (uint32_t)DELTA_WRAP32(amd_core_raw_after, amd_core_raw_before);

    double pkg_diff = (double)pkg_delta_ticks * energy_units;
    double core_diff = (double)core_delta_ticks * energy_units;
    double gpu_diff = 0.0;   /* Not exposed via these MSRs on AMD */
    double dram_diff = 0.0;  /* Not exposed via these MSRs on AMD */

    if (out != NULL) {
        out->package = pkg_diff;
        out->core = core_diff;
        out->gpu = gpu_diff;
        out->dram = dram_diff;
    }

    return 1;
}

static void amd_cleanup(void) {
    /* No persistent handles to clean up */
}

static int amd_measure_memory(const char *command) {
    printf("[INFO] Measuring memory consumption of \"%s\" (AMD/Linux)...\n", command);
    char cmd[2048];

    snprintf(cmd, sizeof(cmd),
            "/usr/bin/time -v %s 2>&1 1>/dev/null | grep 'Maximum resident set size' | "
            "sed 's/[^0-9]*\\([0-9]*\\).*/\\1/'",
            command);

    FILE *fp = popen(cmd, "r");
    if (!fp) return -1;

    char buf[128] = "";
    if (fgets(buf, sizeof(buf), fp) == NULL) {
        pclose(fp);
        return -1;
    }
    pclose(fp);

    int result = atoi(buf);
    printf("[INFO] Memory consumption: %d KB\n", result);
    return result;
}

EnergyBackend amd_backend = {
    .name = "AMD Zen RAPL",
    .detect = amd_detect,
    .init = amd_init,
    .before = amd_before,
    .after = amd_after,
    .cleanup = amd_cleanup,
    .measure_memory = amd_measure_memory,
    .set_powercap = NULL,
    .destroy_powercap = NULL
};
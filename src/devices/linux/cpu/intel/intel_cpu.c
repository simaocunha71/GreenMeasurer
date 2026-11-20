#include "intel_cpu.h"
#include "msr.h"
#include <string.h>

#define MEASURED_CORE 0

static RaplMsr msr = {.fd = -1};
static RaplDomain package = RAPL_DOMAIN(MSR_PKG_ENERGY_STATUS);
static RaplDomain core = RAPL_DOMAIN(MSR_PP0_ENERGY_STATUS);
static RaplDomain dram = RAPL_DOMAIN(MSR_DRAM_ENERGY_STATUS);

/* Only family 6 GenuineIntel CPUs expose the RAPL MSRs used here. */
static int is_supported_intel_cpu(void) {
    FILE *cpuinfo = fopen("/proc/cpuinfo", "r");
    if (!cpuinfo) return 0;

    int family = 0, model = -1;
    char line[BUFSIZ], vendor[BUFSIZ] = "";
    while (fgets(line, sizeof(line), cpuinfo)) {
        if (!strncmp(line, "vendor_id", 9)) {
            sscanf(line, "%*s%*s%s", vendor);
            if (strncmp(vendor, "GenuineIntel", 12)) break;
        }
        if (!strncmp(line, "cpu family", 10)) {
            sscanf(line, "%*s%*s%*s%d", &family);
            if (family != 6) break;
        }
        if (!strncmp(line, "model", 5)) sscanf(line, "%*s%*s%d", &model);
    }
    fclose(cpuinfo);
    return !strncmp(vendor, "GenuineIntel", 12) && family == 6 && model >= 0;
}

int intel_cpu_begin(void) {
    if (!is_supported_intel_cpu()) return 1;
    if (rapl_msr_open(&msr, MEASURED_CORE) != 0)
        fprintf(stderr, "[WARNING] Intel RAPL MSR energy measurements are unavailable.\n");
    return 0;
}

double intel_cpu_get_package(void) { return rapl_msr_energy(&msr, &package); }
double intel_cpu_get_core(void) { return rapl_msr_energy(&msr, &core); }
double intel_cpu_get_dram(void) { return rapl_msr_energy(&msr, &dram); }

void intel_cpu_end(void) {
    rapl_msr_close(&msr);
}

const char *intel_cpu_diagnose(void) {
    return "CPU is not a supported Intel RAPL model";
}

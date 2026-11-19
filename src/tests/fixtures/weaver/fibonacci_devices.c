#include <stdlib.h>

int intel_cpu_begin(void) { return 0; }
void intel_cpu_end(void) {}
double intel_cpu_get_package(void) {
    static int n;
    return getenv("MOCK_CPU_PACKAGE_UNAVAILABLE") ? -1.0 : ++n;
}
double intel_cpu_get_core(void) { static int n; return ++n; }
double intel_cpu_get_dram(void) { static int n; return ++n; }
double intel_cpu_get_temperature(void) { return 40.0; }
int intel_cpu_set_powercap(int watts) { (void)watts; return 0; }
int intel_cpu_destroy_powercap(void) { return 0; }

int runTesting(int, int, const char *, const char *, const char *, const char *, int, int,
               float, int, int);
int intel_cpu_calibrate_powercap(int ntimes, int core, const char *command, const char *language,
                                 const char *program, const char *csv, const char *script,
                                 int min, int max, int timeout, float temperature, int variance,
                                 int memory) {
    (void)script;
    (void)min;
    (void)max;
    return runTesting(ntimes, core, command, language, program, csv, -1, timeout,
                      temperature, variance, memory);
}

int intel_gpu_begin(void) { return getenv("MOCK_INTEL_GPU_UNAVAILABLE") != NULL; }
void intel_gpu_end(void) {}
double intel_gpu_get_energy(void) { static int n; return ++n; }
double intel_gpu_get_temperature(void) { return 45.0; }

int nvidia_begin(void) { return getenv("MOCK_NVIDIA_GPU_UNAVAILABLE") != NULL; }
void nvidia_end(void) {}
double nvidia_get_energy(void) { static int n; return ++n; }
double nvidia_get_power(void) { return 50.0; }
double nvidia_get_temperature(void) {
    return getenv("MOCK_NVIDIA_TEMPERATURE_UNAVAILABLE") ? -1.0 : 55.0;
}
double nvidia_get_utilization(void) { return 25.0; }
double nvidia_get_vram(void) { return 32.0; }

double system_get_core_count(void) { return 4.0; }
double system_get_core_frequency_mhz(void) { return 3000.0; }
double system_get_running_processes(void) { return 2.0; }
double system_get_workload_processes(void) { return 1.0; }
double system_get_workload_memory_kb(void) { return 1024.0; }

#include "../platform/linux/gpu/intel/intel.c"
#include <assert.h>

float intel_gpu_read_hwmon_temperature(void) { return 40.0f; }
int intel_gpu_hardware_present(void) { return 1; }

int main(void) {
    igpu_supported = 1;
    energy_units_valid = 1;
    energy_units = 0.25;
    pp1_before = 200;
    pp1_after = 208;
    pp1_before_valid = pp1_after_valid = 1;
    assert(intel_gpu_get_energy(0) == 2.0);
    assert(energy_backend_read_counter(0, 0, 0) == 52.0);
    assert(energy_backend_read_counter(1, 0, 0) == METRIC_NOT_SUPPORTED);
    pp1_after_valid = 0;
    assert(energy_backend_read_counter(0, 0, 0) == METRIC_NOT_SUPPORTED);
    return 0;
}

#ifndef GM_DEVICES_INTEL_CPU_TEMPERATURE_H
#define GM_DEVICES_INTEL_CPU_TEMPERATURE_H

int intel_cpu_temperature_begin(void);
void intel_cpu_temperature_end(void);
double intel_cpu_get_temperature(void);
/* Stabilizes for the requested seconds and returns the baseline temperature. */
double intel_cpu_calibrate_temperature(unsigned sleep_seconds);

#endif

#ifndef GM_DEVICES_INTEL_GPU_H
#define GM_DEVICES_INTEL_GPU_H

/* Intel integrated GPU metrics. Readers take no arguments and return a negative value when the
 * quantity is unavailable. */

/* Non-zero when there is no GPU RAPL counter, no GPU temperature sensor and no Intel GPU. */
int intel_gpu_begin(void);
void intel_gpu_end(void);

int intel_gpu_hardware_present(void);   /* 1 when an Intel display controller is on the PCI bus */

double intel_gpu_get_energy(void);       /* cumulative joules (RAPL PP1) */
double intel_gpu_get_temperature(void);
/* Stabilizes for the requested seconds and returns the baseline temperature. */
double intel_gpu_calibrate_temperature(unsigned sleep_seconds);  /* Celsius, first hwmon sensor labelled "GPU" */

#endif

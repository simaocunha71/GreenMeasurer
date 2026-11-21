#ifndef GM_DEVICES_NVIDIA_GPU_H
#define GM_DEVICES_NVIDIA_GPU_H

/* NVIDIA GPU 0 metrics through NVML. Readers take no arguments and return a negative value when
 * the quantity is unavailable. */

int nvidia_begin(void);   /* non-zero when NVML or the GPU is missing */
void nvidia_end(void);

double nvidia_get_energy(void);       /* cumulative joules */
double nvidia_get_power(void);        /* watts */
double nvidia_get_temperature(void);
/* Stabilizes for the requested seconds and returns the baseline temperature. */
double nvidia_calibrate_temperature(unsigned sleep_seconds);  /* Celsius */
double nvidia_get_utilization(void);  /* GPU utilization, percent */
double nvidia_get_vram(void);         /* GPU memory (MiB) used by the workload process tree root */

#endif

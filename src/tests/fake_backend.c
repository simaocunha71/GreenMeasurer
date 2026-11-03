/* Configurable fake backend plugin used by run_tests.sh.
   Build-time knobs: FAKE_NAME (string), FAKE_PRIORITY, FAKE_DETECT (0/1),
   FAKE_GPU (define for a GPU plugin), FAKE_INVALID (define to omit callbacks). */
#include "../include/energy-backend.h"

static int fake_detect(void) { return FAKE_DETECT; }
static int fake_init(int id) { (void)id; return 0; }
static int fake_sample(int id) { (void)id; return 0; }
static double fake_energy(int id) { (void)id; return 1.0; }
static const char *fake_diagnose(void) { return "fake backend is disabled"; }

#ifdef FAKE_GPU
static GpuBackend backend = {
    .name = FAKE_NAME,
    .detect = fake_detect,
#ifndef FAKE_INVALID
    .init = fake_init,
    .sample_start = fake_sample,
    .sample_stop = fake_sample,
    .get_energy = fake_energy,
#endif
    .diagnose = fake_diagnose
};
const EnergyBackendDescriptor energy_backend_descriptor = {
    .type = ENERGY_BACKEND_GPU, .priority = FAKE_PRIORITY, .backend.gpu = &backend
};
#else
static CpuBackend backend = {
    .name = FAKE_NAME,
    .detect = fake_detect,
    .init = fake_init,
    .sample_start = fake_sample,
    .sample_stop = fake_sample,
    .get_package_energy = fake_energy,
    .get_core_energy = fake_energy,
    .get_dram_energy = fake_energy,
    .diagnose = fake_diagnose
};
const EnergyBackendDescriptor energy_backend_descriptor = {
    .type = ENERGY_BACKEND_CPU, .priority = FAKE_PRIORITY, .backend.cpu = &backend
};
#endif

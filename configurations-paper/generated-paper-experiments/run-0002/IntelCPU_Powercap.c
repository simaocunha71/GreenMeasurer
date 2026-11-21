/* Woven by scripts/weave.py from intel-cpu-powercap.toml; do not edit. */
#include "gm_aspect.h"
#include "intel_cpu_powercap.h"

static void gm_IntelCPU_Powercap_teardown(void) { intel_cpu_powercap_end(); }

static const GmAspect gm_IntelCPU_Powercap_aspect = {
    "IntelCPU.Powercap", 0, NULL,
    NULL, gm_IntelCPU_Powercap_teardown, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, -1, NULL,
    intel_cpu_set_powercap, intel_cpu_destroy_powercap, intel_cpu_calibrate_powercap
};

const GmAspect *gm_aspect_IntelCPU_Powercap(void) { return &gm_IntelCPU_Powercap_aspect; }

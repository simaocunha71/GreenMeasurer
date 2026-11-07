#ifndef GREENMEASURER_METRIC_GUIDANCE_H
#define GREENMEASURER_METRIC_GUIDANCE_H

#include <stdio.h>

static inline void green_metric_extension_guidance(void) {
    fprintf(stderr,
            "[INFO] If the metric is not provided by GreenMeasurer and you have a library's "
            ".c and .h files, implement a metric adapter that calls its function, compile "
            "the adapter and library into a .so, and select external.<adapter>.<metric> "
            "in the TOML file. See examples/custom_metric and README.md. "
            "Use a CPU/GPU backend instead if the metric requires hardware-specific "
            "window or process sampling.\n");
}

#endif

#include "metric_adapter_loader.h"
#include "../include/metric-guidance.h"

#include <stdio.h>
#include <string.h>

int main(int argc, char **argv) {
    if (argc < 3 ||
        (strcmp(argv[2], "--all") == 0 ? argc != 3 :
         strcmp(argv[2], "--selected") != 0 || argc < 4)) {
        fprintf(stderr, "Usage: %s <adapter.so> --all | --selected <metric> [metric ...]\n", argv[0]);
        return 2;
    }

    GreenMetricAdapterHandle handle = {0};
    if (green_metric_adapter_open(&handle, argv[1]) != 0) return 1;

    int failed = 0;
    if (strcmp(argv[2], "--all") == 0) {
        for (size_t i = 0; i < handle.adapter->metric_count; i++) {
            double value;
            if (green_metric_adapter_read(&handle, i, &value) != 0) {
                fprintf(stderr, "[WARNING] Unavailable external metric '%s' was skipped.\n",
                        handle.adapter->metrics[i].name);
                continue;
            }
            printf("%s (%s): %.17g\n", handle.adapter->metrics[i].name,
                   handle.adapter->metrics[i].unit, value);
        }
    } else {
        for (int arg = 3; arg < argc; arg++) {
            size_t i;
            for (i = 0; i < handle.adapter->metric_count; i++) {
                if (strcmp(argv[arg], handle.adapter->metrics[i].name) == 0) break;
            }
            if (i == handle.adapter->metric_count) {
                fprintf(stderr, "[ERROR] Adapter '%s' has no metric '%s'.\n", argv[1], argv[arg]);
                green_metric_extension_guidance();
                failed = 1;
                continue;
            }
            double value;
            if (green_metric_adapter_read(&handle, i, &value) != 0) {
                fprintf(stderr, "[INFO] The metric exists in the adapter but could not be read; "
                        "check the adapter, library and runtime permissions.\n");
                green_metric_extension_guidance();
                failed = 1;
                continue;
            }
            printf("%s (%s): %.17g\n", handle.adapter->metrics[i].name,
                   handle.adapter->metrics[i].unit, value);
        }
    }
    green_metric_adapter_close(&handle);
    return failed ? 1 : 0;
}

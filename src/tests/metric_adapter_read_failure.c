#include "../include/metric-adapter.h"
#include <math.h>

static int read_metric(size_t index, double *value) {
    if (index == 1) return -1;
    *value = index == 0 ? 42.0 : index == 2 ? 9.0 : NAN;
    return 0;
}

static const GreenMetricDescriptor metrics[] = {
    {"Available", "count"},
    {"Unavailable", "count"},
    {"--all", "count"},
    {"NotFinite", "count"},
};

const GreenMetricAdapter greenmeasurer_metric_adapter = {
    .abi_version = GREENMEASURER_METRIC_ADAPTER_ABI,
    .metrics = metrics,
    .metric_count = sizeof(metrics) / sizeof(metrics[0]),
    .read = read_metric
};

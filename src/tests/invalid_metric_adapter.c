#include "../include/metric-adapter.h"

static int read_value(size_t index, double *value) {
    (void)index;
    (void)value;
    return 0;
}

static const GreenMetricDescriptor metrics[] = {{"Invalid", "count"}};

const GreenMetricAdapter greenmeasurer_metric_adapter = {
    .abi_version = GREENMEASURER_METRIC_ADAPTER_ABI + 1,
    .metrics = metrics,
    .metric_count = 1,
    .read = read_value
};

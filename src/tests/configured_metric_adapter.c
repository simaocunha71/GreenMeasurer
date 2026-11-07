#include "../include/metric-adapter.h"
#include <stdlib.h>

static int reads;

static int read_metric(size_t index, double *value) {
    const char *limit = getenv("GREEN_TEST_FAIL_AFTER");
    if (limit && reads >= atoi(limit)) return -1;
    reads++;
    if (index == 0) *value = -5.0;
    else if (index == 1) *value = (double)reads;
    else return -1;
    return 0;
}

static const GreenMetricDescriptor metrics[] = {
    {"Negative", "units"},
    {"Sequence", "count"}
};

const GreenMetricAdapter greenmeasurer_metric_adapter = {
    .abi_version = GREENMEASURER_METRIC_ADAPTER_ABI,
    .metrics = metrics,
    .metric_count = sizeof(metrics) / sizeof(metrics[0]),
    .read = read_metric
};

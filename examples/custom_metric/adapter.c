#include "../../src/include/metric-adapter.h"
#include "processes.h"

static int read_metric(size_t index, double *value) {
    if (index != 0 || !value) return -1;
    int count = get_number_of_running_processes();
    if (count < 0) return -1;
    *value = count;
    return 0;
}

static const GreenMetricDescriptor metrics[] = {
    {"RunningProcesses", "count"}
};

const GreenMetricAdapter greenmeasurer_metric_adapter = {
    .abi_version = GREENMEASURER_METRIC_ADAPTER_ABI,
    .metrics = metrics,
    .metric_count = sizeof(metrics) / sizeof(metrics[0]),
    .read = read_metric
};

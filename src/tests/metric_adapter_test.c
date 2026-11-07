#include "../metrics/metric_adapter_loader.h"

#include <stdio.h>
#include <string.h>

int main(int argc, char **argv) {
    if (argc != 3) return 1;
    GreenMetricAdapterHandle handle = {0};
    if (green_metric_adapter_open(&handle, argv[1]) != 0) return 1;
    if (handle.adapter->metric_count != 1 ||
        strcmp(handle.adapter->metrics[0].name, "RunningProcesses") != 0 ||
        strcmp(handle.adapter->metrics[0].unit, "count") != 0) return 1;
    double value;
    if (green_metric_adapter_read(&handle, 0, &value) != 0 || value < 0.0) return 1;
    if (green_metric_adapter_read(&handle, 1, &value) == 0) return 1;
    green_metric_adapter_close(&handle);
    if (handle.handle || handle.adapter) return 1;
    if (green_metric_adapter_open(&handle, "/nonexistent/adapter.so") == 0) return 1;
    if (green_metric_adapter_open(&handle, argv[2]) == 0) return 1;
    return 0;
}

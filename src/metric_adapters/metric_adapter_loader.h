#ifndef GREENMEASURER_METRIC_ADAPTER_LOADER_H
#define GREENMEASURER_METRIC_ADAPTER_LOADER_H

#include "../include/metric-adapter.h"

typedef struct {
    void *handle;
    const GreenMetricAdapter *adapter;
    int (*read_typed)(size_t index, GreenMetricValue *value);
} GreenMetricAdapterHandle;

int green_metric_adapter_open(GreenMetricAdapterHandle *handle, const char *path);
int green_metric_adapter_read(const GreenMetricAdapterHandle *handle, size_t index, double *value);
int green_metric_adapter_read_value(const GreenMetricAdapterHandle *handle, size_t index,
                                    GreenMetricValue *value);
void green_metric_adapter_close(GreenMetricAdapterHandle *handle);

#endif

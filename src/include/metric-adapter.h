#ifndef GREENMEASURER_METRIC_ADAPTER_H
#define GREENMEASURER_METRIC_ADAPTER_H

#include <stddef.h>
#include <stdint.h>

#define GREENMEASURER_METRIC_ADAPTER_ABI 1u
#define GREENMEASURER_METRIC_ADAPTER_SYMBOL "greenmeasurer_metric_adapter"

typedef struct {
    const char *name;
    const char *unit;
} GreenMetricDescriptor;

typedef struct {
    uint32_t abi_version;
    const GreenMetricDescriptor *metrics;
    size_t metric_count;
    int (*init)(void);
    int (*read)(size_t metric_index, double *value);
    void (*cleanup)(void);
} GreenMetricAdapter;

/* Each adapter exports this descriptor as a global symbol. */
extern const GreenMetricAdapter greenmeasurer_metric_adapter;

#endif

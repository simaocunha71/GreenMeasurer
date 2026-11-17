#ifndef GREENMEASURER_METRIC_ADAPTER_H
#define GREENMEASURER_METRIC_ADAPTER_H

#include <stddef.h>
#include <stdint.h>

#define GREENMEASURER_METRIC_ADAPTER_ABI 1u
#define GREENMEASURER_METRIC_ADAPTER_SYMBOL "greenmeasurer_metric_adapter"
#define GREENMEASURER_METRIC_TYPED_READ_SYMBOL "greenmeasurer_metric_read_typed"

typedef enum {
    GREEN_METRIC_INVALID = 0,
    GREEN_METRIC_INT,
    GREEN_METRIC_UNSIGNED_INT,
    GREEN_METRIC_FLOAT,
    GREEN_METRIC_DOUBLE
} GreenMetricValueType;

typedef struct {
    GreenMetricValueType type;
    union {
        int integer;
        unsigned int unsigned_integer;
        float real;
        double double_real;
    } value;
} GreenMetricValue;

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

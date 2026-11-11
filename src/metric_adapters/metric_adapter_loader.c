#include "metric_adapter_loader.h"

#include <dlfcn.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static int valid_identifier(const char *text) {
    if (!text || !*text) return 0;
    for (const unsigned char *p = (const unsigned char *)text; *p; p++) {
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
              (*p >= '0' && *p <= '9') || *p == '_' || *p == '-')) {
            return 0;
        }
    }
    return 1;
}

int green_metric_adapter_open(GreenMetricAdapterHandle *handle, const char *path) {
    if (!handle || !path || !*path) {
        fprintf(stderr, "[ERROR] A metric adapter handle and path are required.\n");
        return -1;
    }
    handle->handle = NULL;
    handle->adapter = NULL;
    void *library = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!library) {
        fprintf(stderr, "[ERROR] Failed to load metric adapter '%s': %s\n", path, dlerror());
        return -1;
    }
    dlerror();
    const GreenMetricAdapter *adapter = dlsym(library, GREENMEASURER_METRIC_ADAPTER_SYMBOL);
    const char *error = dlerror();
    if (error || !adapter || adapter->abi_version != GREENMEASURER_METRIC_ADAPTER_ABI ||
        !adapter->metrics || !adapter->metric_count || !adapter->read) {
        fprintf(stderr, "[ERROR] Invalid or incompatible metric adapter '%s'%s%s.\n",
                path, error ? ": " : "", error ? error : "");
        dlclose(library);
        return -1;
    }
    for (size_t i = 0; i < adapter->metric_count; i++) {
        const GreenMetricDescriptor *metric = &adapter->metrics[i];
        if (!valid_identifier(metric->name) || !valid_identifier(metric->unit)) {
            fprintf(stderr, "[ERROR] Adapter '%s' has an invalid metric name or unit at index %zu.\n",
                    path, i);
            dlclose(library);
            return -1;
        }
        for (size_t j = 0; j < i; j++) {
            if (!strcmp(metric->name, adapter->metrics[j].name)) {
                fprintf(stderr, "[ERROR] Adapter '%s' has duplicate metric '%s'.\n", path, metric->name);
                dlclose(library);
                return -1;
            }
        }
    }
    if (adapter->init && adapter->init() != 0) {
        fprintf(stderr, "[ERROR] Metric adapter '%s' initialization failed.\n", path);
        dlclose(library);
        return -1;
    }
    handle->handle = library;
    handle->adapter = adapter;
    return 0;
}

int green_metric_adapter_read(const GreenMetricAdapterHandle *handle, size_t index, double *value) {
    if (!handle || !handle->adapter || !value || index >= handle->adapter->metric_count) {
        fprintf(stderr, "[ERROR] Invalid metric adapter read request.\n");
        return -1;
    }
    *value = NAN;
    if (handle->adapter->read(index, value) != 0 || !isfinite(*value)) {
        fprintf(stderr, "[ERROR] Failed to read external metric '%s'.\n",
                handle->adapter->metrics[index].name);
        return -1;
    }
    return 0;
}

void green_metric_adapter_close(GreenMetricAdapterHandle *handle) {
    if (!handle || !handle->handle) return;
    if (handle->adapter->cleanup) handle->adapter->cleanup();
    if (dlclose(handle->handle) != 0) {
        fprintf(stderr, "[ERROR] Failed to unload metric adapter: %s\n", dlerror());
    }
    handle->handle = NULL;
    handle->adapter = NULL;
}

#define main greenmeasurer_unused_engine_main
#include "main.c"
#undef main

int main(int argc, char **argv) {
    if (argc > 3 || (argc > 1 && strcmp(argv[1], "cpu") && strcmp(argv[1], "gpu"))) {
        fprintf(stderr, "Usage: %s [cpu|gpu [backend-directory]]\n", argv[0]);
        return 2;
    }
    if (get_backend_root() != 0 ||
        load_backend_category("cpu", ENERGY_BACKEND_CPU) != 0 ||
        load_backend_category("gpu", ENERGY_BACKEND_GPU) != 0) {
        cleanup_backends();
        return 1;
    }
    int found = 0, failed = 0;
    for (size_t i = 0; i < backend_report_count; i++) {
        BackendReport *report = &backend_reports[i];
        if ((argc > 1 && strcmp(argv[1], report->category)) ||
            (argc > 2 && strcmp(argv[2], report->label))) continue;
        if (strcmp(report->status, "loaded") && strcmp(report->status, "not built")) {
            fprintf(stderr, "[WARNING] %s backend %s: %s (%s)\n",
                    report->category, report->label, report->status, report->detail);
            failed = 1;
        } else if (!strcmp(report->status, "not built")) {
            fprintf(stderr, "[INFO] %s backend %s: not built\n", report->category, report->label);
        }
    }
    for (size_t i = 0; i < loaded_backend_count; i++) {
        LoadedBackend *entry = &loaded_backends[i];
        const EnergyBackendDescriptor *descriptor = entry->descriptor;
        const char *category = descriptor->type == ENERGY_BACKEND_CPU ? "cpu" : "gpu";
        if ((argc > 1 && strcmp(argv[1], category)) ||
            (argc > 2 && strcmp(argv[2], entry->directory))) continue;
        found = 1;
        const char *name = descriptor->type == ENERGY_BACKEND_CPU
            ? descriptor->backend.cpu->name : descriptor->backend.gpu->name;
        const EnergyMetricDescriptor *metrics = descriptor->type == ENERGY_BACKEND_CPU
            ? descriptor->backend.cpu->metrics : descriptor->backend.gpu->metrics;
        size_t count = descriptor->type == ENERGY_BACKEND_CPU
            ? descriptor->backend.cpu->metric_count : descriptor->backend.gpu->metric_count;
        dlerror();
        const EnergyMetricInfoMap *info = dlsym(entry->handle, ENERGY_METRIC_INFO_MAP_SYMBOL);
        if (dlerror()) info = NULL;
        if (info && (info->version != ENERGY_METRIC_INFO_MAP_VERSION ||
                     info->count != count || !info->entries)) {
            fprintf(stderr, "[ERROR] Invalid metric information for %s/%s.\n", category, entry->directory);
            failed = 1;
            continue;
        }
        printf("%s/%s: %s\n", category, entry->directory, name);
        for (size_t j = 0; j < count; j++) {
            const EnergyMetricInfo *item = info ? &info->entries[j] : NULL;
            if (item && (!item->description || !*item->description || !item->origin || !*item->origin)) {
                fprintf(stderr, "[ERROR] Missing metric description or origin for %s/%s.%s.\n",
                        category, entry->directory, metrics[j].name);
                failed = 1;
            }
            printf("  %s.%s (%s)\n    Function: %s\n    Description: %s\n    Origin: %s\n",
                   category, metrics[j].name, metrics[j].unit,
                   entry->function_map ? entry->function_map->functions[j].name : "legacy get_metric",
                   item && item->description && *item->description ? item->description : "not provided by plugin",
                   item && item->origin && *item->origin ? item->origin : "not provided by plugin");
        }
        if (!info) fprintf(stderr, "[WARNING] Legacy backend %s/%s has no metric descriptions or origins.\n",
                           category, entry->directory);
    }
    if (!found) {
        fprintf(stderr, "[ERROR] No matching compiled backend found.\n");
        failed = 1;
    }
    cleanup_backends();
    return failed ? 1 : 0;
}

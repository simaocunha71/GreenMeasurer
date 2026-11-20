#include "intel_cpu_temperature.h"

#include <math.h>
#include <sensors/sensors.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int sensors_ready;

int intel_cpu_temperature_begin(void) {
    sensors_ready = sensors_init(NULL) == 0;
    if (!sensors_ready) fprintf(stderr, "[WARNING] Failed to initialize the lm-sensors library.\n");
    return !sensors_ready;
}

void intel_cpu_temperature_end(void) {
    if (sensors_ready) sensors_cleanup();
    sensors_ready = 0;
}

double intel_cpu_get_temperature(void) {
    if (!sensors_ready) return -1.0;
    double sum = 0.0;
    size_t count = 0;
    int chip_index = 0;
    const sensors_chip_name *chip;
    while ((chip = sensors_get_detected_chips(NULL, &chip_index)) != NULL) {
        if (!chip->prefix || strcmp(chip->prefix, "coretemp") != 0) continue;
        int feature_index = 0;
        const sensors_feature *feature;
        while ((feature = sensors_get_features(chip, &feature_index)) != NULL) {
            if (feature->type != SENSORS_FEATURE_TEMP) continue;
            char *label = sensors_get_label(chip, feature);
            int is_core = label && strncmp(label, "Core ", 5) == 0;
            free(label);
            if (!is_core) continue;

            const sensors_subfeature *input =
                sensors_get_subfeature(chip, feature, SENSORS_SUBFEATURE_TEMP_INPUT);
            double value;
            if (input && (input->flags & SENSORS_MODE_R) &&
                sensors_get_value(chip, input->number, &value) == 0 &&
                isfinite(value) && value > 0.0) {
                sum += value;
                count++;
            }
        }
    }
    return count ? sum / (double)count : -1.0;
}

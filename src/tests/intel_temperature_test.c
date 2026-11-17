#include "../platform/linux/cpu/intel/intel.c"
#include <assert.h>

static char core0_name[] = "temp1";
static char package_name[] = "temp2";
static char core1_name[] = "temp3";
static sensors_chip_name mock_chip = {.prefix = "coretemp"};
static sensors_feature mock_features[] = {
    {.name = core0_name, .number = 1, .type = SENSORS_FEATURE_TEMP},
    {.name = package_name, .number = 2, .type = SENSORS_FEATURE_TEMP},
    {.name = core1_name, .number = 3, .type = SENSORS_FEATURE_TEMP},
};
static sensors_subfeature mock_inputs[] = {
    {.name = "temp1_input", .number = 1, .type = SENSORS_SUBFEATURE_TEMP_INPUT,
     .flags = SENSORS_MODE_R},
    {.name = "temp2_input", .number = 2, .type = SENSORS_SUBFEATURE_TEMP_INPUT,
     .flags = SENSORS_MODE_R},
    {.name = "temp3_input", .number = 3, .type = SENSORS_SUBFEATURE_TEMP_INPUT,
     .flags = SENSORS_MODE_R},
};
static int init_calls;
static int cleanup_calls;
static int value_reads;

int __wrap_sensors_init(FILE *input) {
    assert(input == NULL);
    init_calls++;
    return 0;
}

void __wrap_sensors_cleanup(void) {
    cleanup_calls++;
}

const sensors_chip_name *__wrap_sensors_get_detected_chips(
    const sensors_chip_name *match, int *index) {
    assert(match == NULL);
    return (*index)++ == 0 ? &mock_chip : NULL;
}

const sensors_feature *__wrap_sensors_get_features(
    const sensors_chip_name *chip, int *index) {
    assert(chip == &mock_chip);
    return *index < 3 ? &mock_features[(*index)++] : NULL;
}

char *__wrap_sensors_get_label(
    const sensors_chip_name *chip, const sensors_feature *feature) {
    assert(chip == &mock_chip);
    const char *label = feature->number == 1 ? "Core 0"
        : feature->number == 2 ? "Package id 0" : "Core 1";
    size_t length = strlen(label) + 1;
    char *copy = malloc(length);
    assert(copy != NULL);
    memcpy(copy, label, length);
    return copy;
}

const sensors_subfeature *__wrap_sensors_get_subfeature(
    const sensors_chip_name *chip, const sensors_feature *feature,
    sensors_subfeature_type type) {
    assert(chip == &mock_chip);
    assert(type == SENSORS_SUBFEATURE_TEMP_INPUT);
    return &mock_inputs[feature->number - 1];
}

int __wrap_sensors_get_value(
    const sensors_chip_name *chip, int subfeature_number, double *value) {
    assert(chip == &mock_chip);
    value_reads++;
    *value = subfeature_number == 1 ? 50.0
        : subfeature_number == 2 ? 99.0 : 60.0;
    return 0;
}

int main(void) {
    energy_units = 0.5;
    energy_units_valid = 1;
    package_after = 200;
    core_after = 100;
    dram_after = 50;
    package_after_valid = core_after_valid = dram_after_valid = 1;
    assert(energy_backend_read_counter(0, 0, 0) == 100.0);
    assert(energy_backend_read_counter(1, 0, 0) == 50.0);
    assert(energy_backend_read_counter(2, 0, 0) == 25.0);
    assert(energy_backend_read_counter(3, 0, 0) == METRIC_NOT_SUPPORTED);
    core_after_valid = 0;
    assert(energy_backend_read_counter(1, 0, 0) == METRIC_NOT_SUPPORTED);
    assert(intel_get_temperature(0) == 55.0f);
    assert(intel_get_temperature(0) == 55.0f);
    assert(init_calls == 1);
    assert(value_reads == 4);
    intel_sensors_cleanup();
    assert(cleanup_calls == 1);
    puts("Intel lm-sensors API temperature test passed.");
    return 0;
}

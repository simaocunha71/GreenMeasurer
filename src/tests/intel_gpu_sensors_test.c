#include "../platform/linux/gpu/intel/sensors.c"
#include <assert.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

static void write_value(const char *path, const char *value) {
    FILE *file = fopen(path, "w");
    assert(file);
    assert(fputs(value, file) >= 0);
    assert(fclose(file) == 0);
}

int main(void) {
    char root[] = "/tmp/greenmeasurer-gpu-sensors-XXXXXX";
    assert(mkdtemp(root));
    char device[128], label[160], input[160];
    assert(snprintf(device, sizeof(device), "%s/hwmon0", root) > 0);
    assert(snprintf(label, sizeof(label), "%s/temp1_label", device) > 0);
    assert(snprintf(input, sizeof(input), "%s/temp1_input", device) > 0);
    assert(read_hwmon_temperature(root) == -1.0f);
    assert(mkdir(device, 0700) == 0);
    write_value(label, "CPU\n");
    write_value(input, "50500\n");
    assert(read_hwmon_temperature(root) == -1.0f);
    write_value(label, "gpu core\n");
    assert(read_hwmon_temperature(root) == 50.5f);
    write_value(input, "0\n");
    assert(read_hwmon_temperature(root) == -1.0f);
    write_value(input, "150000\n");
    assert(read_hwmon_temperature(root) == -1.0f);
    write_value(input, "invalid\n");
    assert(read_hwmon_temperature(root) == -1.0f);
    assert(unlink(input) == 0);
    assert(read_hwmon_temperature(root) == -1.0f);
    assert(unlink(label) == 0);
    assert(rmdir(device) == 0);
    assert(rmdir(root) == 0);
    assert(read_hwmon_temperature(root) == -1.0f);
    puts("Intel GPU sensor tests passed.");
    return 0;
}

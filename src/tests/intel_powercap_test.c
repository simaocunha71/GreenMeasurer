#include "../platform/linux/cpu/intel/intel.c"
#include <assert.h>

static raplcap_limit long_limits[2], short_limits[2];
static int zone_enabled[2], set_calls, fail_set_call, fail_get_die;
static int enable_calls, fail_enable_call;
static char operations[128];
static size_t operation_count;
static int measured_caps[8], measurement_count;
static int fail_measurement_cap;

int raplcap_init(raplcap *rc) { (void)rc; return 0; }
int raplcap_destroy(raplcap *rc) { (void)rc; return 0; }
uint32_t raplcap_get_num_packages(const raplcap *rc) { (void)rc; return 1; }
uint32_t raplcap_get_num_die(const raplcap *rc, uint32_t pkg) {
    (void)rc; (void)pkg; return 2;
}
int raplcap_pd_is_zone_enabled(const raplcap *rc, uint32_t pkg, uint32_t die, raplcap_zone zone) {
    (void)rc; (void)pkg; (void)zone; return zone_enabled[die];
}
int raplcap_pd_set_zone_enabled(const raplcap *rc, uint32_t pkg, uint32_t die,
                                raplcap_zone zone, int enabled) {
    (void)rc; (void)pkg;
    assert(zone == RAPLCAP_ZONE_PACKAGE);
    assert(operation_count < sizeof(operations));
    operations[operation_count++] = 'E';
    enable_calls++;
    zone_enabled[die] = enabled;
    if (enable_calls == fail_enable_call) {
        errno = EIO;
        return -1;
    }
    return 0;
}
int raplcap_pd_get_limits(const raplcap *rc, uint32_t pkg, uint32_t die, raplcap_zone zone,
                          raplcap_limit *long_limit, raplcap_limit *short_limit) {
    (void)rc; (void)pkg; (void)zone;
    if ((int)die == fail_get_die) return -1;
    *long_limit = long_limits[die];
    *short_limit = short_limits[die];
    return 0;
}
int raplcap_pd_set_limits(const raplcap *rc, uint32_t pkg, uint32_t die, raplcap_zone zone,
                          const raplcap_limit *long_limit, const raplcap_limit *short_limit) {
    (void)rc; (void)pkg;
    assert(zone == RAPLCAP_ZONE_PACKAGE);
    assert(operation_count < sizeof(operations));
    operations[operation_count++] = 'L';
    set_calls++;
    if (set_calls <= 2) {
        assert(long_limit->watts == 15 && short_limit->watts == 15);
        assert(long_limit->seconds == 0 && short_limit->seconds == 0);
    }
    long_limits[die].watts = long_limit->watts;
    short_limits[die].watts = short_limit->watts;
    if (long_limit->seconds != 0) long_limits[die].seconds = long_limit->seconds;
    if (short_limit->seconds != 0) short_limits[die].seconds = short_limit->seconds;
    if (set_calls == fail_set_call) {
        errno = EIO;
        return -1;
    }
    return 0;
}
void energy_backend_write_csv_header(FILE *stream, int overtime) {
    (void)overtime; fprintf(stream, "Powercap,Time_ms,cpu.Package_J\n");
}
int runTesting(int ntimes, int core, const char *command, const char *language,
                const char *program, const char *file, int powercap,
                int timeout, float temperature, int variance, int memory) {
    (void)core; (void)command; (void)language; (void)program;
    (void)timeout; (void)temperature; (void)variance; (void)memory;
    measured_caps[measurement_count++] = powercap;
    if (powercap == -1) assert(long_limits[0].watts == 45);
    else assert(long_limits[0].watts == powercap);
    if (powercap == fail_measurement_cap) return -1;
    FILE *fp = fopen(file, "a");
    assert(fp);
    for (int i = 0; i < ntimes; i++) fprintf(fp, "%d,1,1\n", powercap);
    assert(fclose(fp) == 0);
    return 0;
}

static void reset_mock(void) {
    long_limits[0] = (raplcap_limit){.watts = 45, .seconds = 28};
    short_limits[0] = (raplcap_limit){.watts = 80, .seconds = 2};
    long_limits[1] = (raplcap_limit){.watts = 55, .seconds = 12};
    short_limits[1] = (raplcap_limit){.watts = 90, .seconds = 3};
    zone_enabled[0] = 1; zone_enabled[1] = 0;
    set_calls = 0; fail_set_call = 0; fail_get_die = -1; measurement_count = 0;
    enable_calls = 0; fail_enable_call = 0; operation_count = 0;
    fail_measurement_cap = 0;
}

static void check_restored(void) {
    assert(long_limits[0].watts == 45 && long_limits[0].seconds == 28);
    assert(short_limits[0].watts == 80 && short_limits[0].seconds == 2);
    assert(long_limits[1].watts == 55 && long_limits[1].seconds == 12);
    assert(short_limits[1].watts == 90 && short_limits[1].seconds == 3);
    assert(zone_enabled[0] == 1 && zone_enabled[1] == 0);
}

int main(void) {
    reset_mock();
    assert(intel_set_powercap(-1) == 0 && set_calls == 0);
    assert(intel_set_powercap(0) != 0 && set_calls == 0);
    assert(intel_set_powercap(15) == 0);
    assert(long_limits[0].watts == 15 && long_limits[1].watts == 15);
    assert(short_limits[0].watts == 15 && short_limits[1].watts == 15);
    assert(operation_count == 4 && memcmp(operations, "LLEE", 4) == 0);
    assert(zone_enabled[0] == 1 && zone_enabled[1] == 1);
    assert(intel_destroy_powercap() == 0);
    check_restored();

    reset_mock();
    assert(intel_set_powercap(15) == 0);
    assert(energy_backend_commit_powercap() == 0);
    intel_cleanup();
    assert(long_limits[0].watts == 15 && zone_enabled[0] == 1);

    reset_mock();
    fail_get_die = 1;
    assert(intel_set_powercap(15) != 0 && set_calls == 0);
    check_restored();

    reset_mock();
    short_limits[1].watts = 0;
    assert(intel_set_powercap(15) != 0 && set_calls == 0);

    reset_mock();
    fail_set_call = 2;
    assert(intel_set_powercap(15) != 0);
    assert(operation_count >= 3 && memcmp(operations, "LLL", 3) == 0);
    check_restored();

    reset_mock();
    fail_enable_call = 2;
    assert(intel_set_powercap(15) != 0);
    assert(operation_count >= 4 && memcmp(operations, "LLEE", 4) == 0);
    check_restored();

    reset_mock();
    assert(intel_set_powercap(15) == 0);
    fail_set_call = 3;
    assert(intel_destroy_powercap() != 0);

    reset_mock();
    char filename[] = "/tmp/greenmeasurer-powercap-test-XXXXXX";
    int fd = mkstemp(filename);
    assert(fd >= 0 && close(fd) == 0);
    assert(intel_calibrate_powercap(2, 0, "true", "C", "test", filename,
                                    NULL, 15, 16, 1, 40, 5, 0) == 0);
    assert(measurement_count == 3);
    assert(measured_caps[0] == -1 && measured_caps[1] == 15 && measured_caps[2] == 16);
    assert(long_limits[0].watts == 45 && short_limits[0].watts == 80);
    assert(long_limits[1].watts == 55 && short_limits[1].watts == 90);
    assert(zone_enabled[0] == 0 && zone_enabled[1] == 0);
    FILE *fp = fopen(filename, "r");
    assert(fp);
    char buffer[256];
    assert(fgets(buffer, sizeof(buffer), fp));
    for (int i = 0; i < 6; i++) {
        assert(fgets(buffer, sizeof(buffer), fp));
        assert(atoi(buffer) == measured_caps[i / 2]);
    }
    assert(!fgets(buffer, sizeof(buffer), fp));
    assert(fclose(fp) == 0);

    reset_mock();
    fail_measurement_cap = 15;
    assert(intel_calibrate_powercap(1, 0, "true", "C", "test", filename,
                                    NULL, 15, 16, 1, 40, 5, 0) != 0);
    check_restored();

    reset_mock();
    fail_enable_call = 2;
    assert(intel_disable_powercap() != 0);
    check_restored();

    assert(remove(filename) == 0);
    puts("Intel powercap restoration and calibration tests passed.");
    return 0;
}

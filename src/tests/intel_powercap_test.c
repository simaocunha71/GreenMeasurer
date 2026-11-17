#include "../platform/linux/cpu/intel/intel.c"
#include <assert.h>

static raplcap_limit long_limits[2][2], short_limits[2][2];
static int zone_enabled[2][2], set_calls, enable_calls, destroy_calls, die_queries;
static char operations[128];
static uint32_t operation_packages[128], operation_dies[128];
static size_t operation_count;
static int measured_caps[8], measurement_count;
static int fail_measurement_cap;

static void clear_operations(void) {
    set_calls = 0;
    enable_calls = 0;
    destroy_calls = 0;
    die_queries = 0;
    operation_count = 0;
    memset(operations, 0, sizeof(operations));
}

int raplcap_init(raplcap *rc) { (void)rc; return 0; }
int raplcap_destroy(raplcap *rc) { (void)rc; destroy_calls++; return 0; }
uint32_t raplcap_get_num_packages(const raplcap *rc) { (void)rc; return 2; }
uint32_t raplcap_get_num_die(const raplcap *rc, uint32_t pkg) {
    (void)rc;
    die_queries++;
    return pkg == 0 ? 2 : 1;
}
int raplcap_pd_get_limits(const raplcap *rc, uint32_t pkg, uint32_t die, raplcap_zone zone,
                          raplcap_limit *long_limit, raplcap_limit *short_limit) {
    (void)rc;
    assert(zone == RAPLCAP_ZONE_PACKAGE);
    *long_limit = long_limits[pkg][die];
    *short_limit = short_limits[pkg][die];
    return 0;
}
int raplcap_pd_is_zone_enabled(const raplcap *rc, uint32_t pkg, uint32_t die,
                               raplcap_zone zone) {
    (void)rc;
    assert(zone == RAPLCAP_ZONE_PACKAGE);
    return zone_enabled[pkg][die];
}
int raplcap_pd_set_zone_enabled(const raplcap *rc, uint32_t pkg, uint32_t die,
                                raplcap_zone zone, int enabled) {
    (void)rc;
    assert(zone == RAPLCAP_ZONE_PACKAGE);
    assert(operation_count < sizeof(operations));
    operations[operation_count] = 'E';
    operation_packages[operation_count] = pkg;
    operation_dies[operation_count] = die;
    operation_count++;
    enable_calls++;
    zone_enabled[pkg][die] = enabled;
    return 0;
}
int raplcap_pd_set_limits(const raplcap *rc, uint32_t pkg, uint32_t die, raplcap_zone zone,
                          const raplcap_limit *long_limit, const raplcap_limit *short_limit) {
    (void)rc;
    assert(zone == RAPLCAP_ZONE_PACKAGE);
    assert(operation_count < sizeof(operations));
    operations[operation_count] = 'L';
    operation_packages[operation_count] = pkg;
    operation_dies[operation_count] = die;
    operation_count++;
    set_calls++;
    if (long_limit) {
        assert(long_limit->seconds == 0);
        long_limits[pkg][die].watts = long_limit->watts;
    }
    if (short_limit) {
        assert(short_limit->seconds == 0);
        short_limits[pkg][die].watts = short_limit->watts;
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
    if (powercap == -1) assert(long_limits[0][0].watts == 45);
    else {
        assert(long_limits[0][0].watts == powercap);
        assert(long_limits[0][1].watts == powercap);
        assert(long_limits[1][0].watts == powercap);
    }
    if (powercap == fail_measurement_cap) return -1;
    FILE *fp = fopen(file, "a");
    assert(fp);
    for (int i = 0; i < ntimes; i++) fprintf(fp, "%d,1,1\n", powercap);
    assert(fclose(fp) == 0);
    return 0;
}

static void reset_mock(void) {
    assert(intel_powercap_lock_fd < 0);
    assert(unlink(intel_powercap_baseline_path) == 0 || errno == ENOENT);
    char lock_path[512];
    snprintf(lock_path, sizeof(lock_path), "%s.lock", intel_powercap_baseline_path);
    assert(unlink(lock_path) == 0 || errno == ENOENT);
    long_limits[0][0] = (raplcap_limit){.watts = 45, .seconds = 28};
    short_limits[0][0] = (raplcap_limit){.watts = 80, .seconds = 0.002440};
    long_limits[0][1] = (raplcap_limit){.watts = 55, .seconds = 12};
    short_limits[0][1] = (raplcap_limit){.watts = 90, .seconds = 3};
    long_limits[1][0] = (raplcap_limit){.watts = 65, .seconds = 14};
    short_limits[1][0] = (raplcap_limit){.watts = 100, .seconds = 4};
    long_limits[1][1] = (raplcap_limit){.watts = 75, .seconds = 16};
    short_limits[1][1] = (raplcap_limit){.watts = 110, .seconds = 5};
    zone_enabled[0][0] = 1; zone_enabled[0][1] = 0;
    zone_enabled[1][0] = 0; zone_enabled[1][1] = 0;
    measurement_count = 0;
    clear_operations();
    fail_measurement_cap = 0;
}

static void check_original_state(void) {
    assert(long_limits[0][0].watts == 45 && long_limits[0][0].seconds == 28);
    assert(short_limits[0][0].watts == 80 && short_limits[0][0].seconds == 0.002440);
    assert(long_limits[0][1].watts == 55 && long_limits[0][1].seconds == 12);
    assert(short_limits[0][1].watts == 90 && short_limits[0][1].seconds == 3);
    assert(long_limits[1][0].watts == 65 && long_limits[1][0].seconds == 14);
    assert(short_limits[1][0].watts == 100 && short_limits[1][0].seconds == 4);
    assert(long_limits[1][1].watts == 75 && long_limits[1][1].seconds == 16);
    assert(short_limits[1][1].watts == 110 && short_limits[1][1].seconds == 5);
    assert(zone_enabled[0][0] == 1 && zone_enabled[0][1] == 0);
    assert(zone_enabled[1][0] == 0 && zone_enabled[1][1] == 0);
}

int main(void) {
    char baseline_path[] = "/tmp/greenmeasurer-powercap-baseline-XXXXXX";
    int baseline_fd = mkstemp(baseline_path);
    assert(baseline_fd >= 0 && close(baseline_fd) == 0);
    assert(unlink(baseline_path) == 0);
    intel_powercap_baseline_path = baseline_path;

    reset_mock();
    assert(intel_set_powercap(0) == 0 && set_calls == 0);
    assert(intel_set_powercap(-1) == 0);
    assert(set_calls == 0 && enable_calls == 0 && die_queries == 4);
    assert(operation_count == 0);
    assert(intel_destroy_powercap() == 0);
    assert(destroy_calls == 1 && operation_count == 0);
    check_original_state();

    reset_mock();
    assert(intel_set_powercap(15) == 0);
    assert(set_calls == 3 && enable_calls == 3 && die_queries == 4);
    assert(operation_count == 6 && memcmp(operations, "LLLEEE", 6) == 0);
    for (size_t i = 0; i < 3; i++) {
        assert(operation_packages[i] == (i == 2 ? 1 : 0));
        assert(operation_dies[i] == (i == 1 ? 1 : 0));
        assert(operation_packages[i + 3] == operation_packages[i]);
        assert(operation_dies[i + 3] == operation_dies[i]);
    }
    assert(intel_destroy_powercap() == 0);
    assert(destroy_calls == 1 && operation_count == 11);
    assert(memcmp(operations + 6, "LLLEE", 5) == 0);
    check_original_state();

    long_limits[0][0].watts = 5;
    short_limits[0][0].watts = 5;
    long_limits[0][1].watts = 5;
    short_limits[0][1].watts = 5;
    long_limits[1][0].watts = 5;
    short_limits[1][0].watts = 5;
    zone_enabled[0][0] = zone_enabled[0][1] = zone_enabled[1][0] = 1;
    clear_operations();
    assert(intel_set_powercap(-1) == 0);
    assert(long_limits[0][0].watts == 45 && short_limits[0][0].watts == 80);
    assert(long_limits[0][1].watts == 55 && short_limits[0][1].watts == 90);
    assert(long_limits[1][0].watts == 65 && short_limits[1][0].watts == 100);
    check_original_state();
    assert(intel_destroy_powercap() == 0);
    check_original_state();

    short_limits[0][0].seconds = 0.001952;
    long_limits[0][0].watts = 5;
    clear_operations();
    assert(intel_set_powercap(-1) != 0);
    assert(long_limits[0][0].watts == 45);
    assert(short_limits[0][0].seconds == 0.001952);
    assert(access(baseline_path, F_OK) == 0);
    assert(intel_powercap_lock_fd < 0);
    clear_operations();
    assert(intel_set_powercap(15) != 0);
    assert(long_limits[0][0].watts == 45);
    assert(operation_count == 0);

    reset_mock();
    char filename[] = "/tmp/greenmeasurer-powercap-test-XXXXXX";
    int fd = mkstemp(filename);
    assert(fd >= 0 && close(fd) == 0);
    assert(intel_calibrate_powercap(2, 0, "true", "C", "test", filename,
                                    NULL, 15, 16, 1, 40, 5, 0) == 0);
    assert(measurement_count == 3);
    assert(measured_caps[0] == -1 && measured_caps[1] == 15 && measured_caps[2] == 16);
    check_original_state();
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
    check_original_state();

    assert(remove(filename) == 0);
    assert(unlink(intel_powercap_baseline_path) == 0 || errno == ENOENT);
    char baseline_lock_path[512];
    snprintf(baseline_lock_path, sizeof(baseline_lock_path), "%s.lock", intel_powercap_baseline_path);
    assert(unlink(baseline_lock_path) == 0 || errno == ENOENT);
    puts("Intel powercap behavior tests passed.");
    return 0;
}

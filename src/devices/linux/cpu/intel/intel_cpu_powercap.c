#include "intel_cpu_powercap.h"
#include "output_file.h"
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <math.h>
#include <powercap/powercap.h>
#include <raplcap/raplcap.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

/* ---- Power capping ---- */

typedef struct {
    uint32_t package;
    uint32_t die;
    raplcap_limit long_limit;
    raplcap_limit short_limit;
    int zone_enabled;
} IntelPowercapSnapshot;

static raplcap global_rc;
static int raplcap_active = 0;
static IntelPowercapSnapshot *intel_powercap_snapshot;
static size_t intel_powercap_snapshot_count;
static const char *intel_powercap_baseline_path = "/var/lib/greenmeasurer/intel-rapl-baseline";
static int intel_powercap_lock_fd = -1;

static void intel_free_powercap_snapshot(void) {
    free(intel_powercap_snapshot);
    intel_powercap_snapshot = NULL;
    intel_powercap_snapshot_count = 0;
}

static char *intel_powercap_parent_directory(void) {
    char *directory = strdup(intel_powercap_baseline_path);
    if (!directory) return NULL;
    char *separator = strrchr(directory, '/');
    if (!separator) {
        free(directory);
        return strdup(".");
    } else if (separator == directory) {
        separator[1] = '\0';
    } else {
        *separator = '\0';
    }
    return directory;
}

static int intel_acquire_powercap_lock(void) {
    if (intel_powercap_lock_fd >= 0) {
        fprintf(stderr, "[ERROR] RAPL baseline lock is already held.\n");
        return -1;
    }

    char *directory = intel_powercap_parent_directory();
    if (!directory) {
        fprintf(stderr, "[ERROR] Failed to allocate RAPL baseline directory path.\n");
        return -1;
    }
    if (mkdir(directory, 0755) != 0 && errno != EEXIST) {
        perror("[ERROR] Failed to create RAPL baseline directory");
        free(directory);
        return -1;
    }
    struct stat directory_stat;
    if (lstat(directory, &directory_stat) != 0 || !S_ISDIR(directory_stat.st_mode)) {
        fprintf(stderr, "[ERROR] RAPL baseline path parent is not a directory.\n");
        free(directory);
        return -1;
    }
    free(directory);

    size_t path_length = strlen(intel_powercap_baseline_path);
    char *lock_path = malloc(path_length + sizeof(".lock"));
    if (!lock_path) {
        fprintf(stderr, "[ERROR] Failed to allocate RAPL baseline lock path.\n");
        return -1;
    }
    snprintf(lock_path, path_length + sizeof(".lock"), "%s.lock", intel_powercap_baseline_path);
    int lock_fd = open(lock_path, O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
    free(lock_path);
    if (lock_fd < 0) {
        perror("[ERROR] Failed to open RAPL baseline lock");
        return -1;
    }
    if (flock(lock_fd, LOCK_EX | LOCK_NB) != 0) {
        if (errno == EWOULDBLOCK)
            fprintf(stderr, "[ERROR] Another GreenMeasurer process is using RAPL power limits.\n");
        else
            perror("[ERROR] Failed to lock RAPL baseline");
        close(lock_fd);
        return -1;
    }
    intel_powercap_lock_fd = lock_fd;
    return 0;
}

static void intel_release_powercap_lock(void) {
    if (intel_powercap_lock_fd < 0) return;
    if (flock(intel_powercap_lock_fd, LOCK_UN) != 0)
        perror("[ERROR] Failed to unlock RAPL baseline");
    if (close(intel_powercap_lock_fd) != 0)
        perror("[ERROR] Failed to close RAPL baseline lock");
    intel_powercap_lock_fd = -1;
}

static int intel_allocate_powercap_snapshot(void) {
    uint32_t packages = raplcap_get_num_packages(&global_rc);
    if (packages == 0) {
        fprintf(stderr, "[ERROR] No RAPL packages were found.\n");
        return -1;
    }

    size_t state_count = 0;
    for (uint32_t package = 0; package < packages; package++) {
        uint32_t dies = raplcap_get_num_die(&global_rc, package);
        if (dies == 0 || state_count > SIZE_MAX - dies) {
            fprintf(stderr, "[ERROR] Invalid RAPL die count for package %u.\n", package);
            return -1;
        }
        state_count += dies;
    }
    if (state_count > SIZE_MAX / sizeof(*intel_powercap_snapshot)) {
        fprintf(stderr, "[ERROR] Too many RAPL package zones to save powercap state.\n");
        return -1;
    }

    IntelPowercapSnapshot *snapshot = calloc(state_count, sizeof(*snapshot));
    if (!snapshot) {
        fprintf(stderr, "[ERROR] Failed to allocate RAPL powercap state snapshot.\n");
        return -1;
    }

    size_t index = 0;
    for (uint32_t package = 0; package < packages; package++) {
        uint32_t dies = raplcap_get_num_die(&global_rc, package);
        if (dies == 0 || dies > state_count - index) {
            fprintf(stderr, "[ERROR] RAPL topology changed while saving powercap state.\n");
            free(snapshot);
            return -1;
        }
        for (uint32_t die = 0; die < dies; die++) {
            snapshot[index].package = package;
            snapshot[index].die = die;
            index++;
        }
    }
    if (index != state_count) {
        fprintf(stderr, "[ERROR] RAPL topology changed while saving powercap state.\n");
        free(snapshot);
        return -1;
    }
    intel_powercap_snapshot = snapshot;
    intel_powercap_snapshot_count = state_count;
    return 0;
}

static int intel_read_current_powercap_state(void) {
    for (size_t i = 0; i < intel_powercap_snapshot_count; i++) {
        IntelPowercapSnapshot *state = &intel_powercap_snapshot[i];
        if (raplcap_pd_get_limits(&global_rc, state->package, state->die,
                                  RAPLCAP_ZONE_PACKAGE, &state->long_limit,
                                  &state->short_limit) != 0) {
            fprintf(stderr, "[ERROR] Failed to read package power limits for package %u die %u.\n",
                    state->package, state->die);
            return -1;
        }
        int enabled = raplcap_pd_is_zone_enabled(&global_rc, state->package, state->die,
                                                 RAPLCAP_ZONE_PACKAGE);
        if (enabled < 0) {
            fprintf(stderr, "[ERROR] Failed to read package power-limit state for package %u die %u.\n",
                    state->package, state->die);
            return -1;
        }
        state->zone_enabled = enabled;
    }
    return 0;
}

static int intel_read_powercap_baseline(void) {
    int fd = open(intel_powercap_baseline_path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) {
        if (errno == ENOENT) return 1;
        perror("[ERROR] Failed to open saved RAPL baseline");
        return -1;
    }
    FILE *file = fdopen(fd, "r");
    if (!file) {
        perror("[ERROR] Failed to read saved RAPL baseline");
        close(fd);
        return -1;
    }

    char magic[64];
    unsigned version;
    size_t count;
    int result = -1;
    if (fscanf(file, "%63s %u %zu", magic, &version, &count) != 3 ||
        strcmp(magic, "GREENMEASURER_INTEL_RAPL_BASELINE") != 0 ||
        version != 1 || count != intel_powercap_snapshot_count) {
        fprintf(stderr, "[ERROR] Saved RAPL baseline has an invalid format or topology.\n");
        goto done;
    }
    for (size_t i = 0; i < count; i++) {
        IntelPowercapSnapshot *state = &intel_powercap_snapshot[i];
        uint32_t package, die;
        int enabled;
        double long_watts, long_seconds, short_watts, short_seconds;
        if (fscanf(file, "%"SCNu32" %"SCNu32" %lf %lf %lf %lf %d",
                   &package, &die, &long_watts, &long_seconds,
                   &short_watts, &short_seconds, &enabled) != 7 ||
            package != state->package || die != state->die ||
            !isfinite(long_watts) || long_watts <= 0 ||
            !isfinite(long_seconds) || long_seconds <= 0 ||
            !isfinite(short_watts) || short_watts <= 0 ||
            !isfinite(short_seconds) || short_seconds <= 0 ||
            (enabled != 0 && enabled != 1)) {
            fprintf(stderr, "[ERROR] Saved RAPL baseline contains invalid package state.\n");
            goto done;
        }
        state->long_limit = (raplcap_limit){.watts = long_watts, .seconds = long_seconds};
        state->short_limit = (raplcap_limit){.watts = short_watts, .seconds = short_seconds};
        state->zone_enabled = enabled;
    }
    char extra;
    if (fscanf(file, " %c", &extra) == 1 || ferror(file)) {
        fprintf(stderr, "[ERROR] Saved RAPL baseline contains trailing or unreadable data.\n");
        goto done;
    }
    result = 0;

done:
    if (fclose(file) != 0) {
        perror("[ERROR] Failed to close saved RAPL baseline");
        result = -1;
    }
    return result;
}

static int intel_write_powercap_baseline(void) {
    size_t path_length = strlen(intel_powercap_baseline_path);
    char *temporary_path = malloc(path_length + sizeof(".tmp.XXXXXX"));
    if (!temporary_path) {
        fprintf(stderr, "[ERROR] Failed to allocate temporary RAPL baseline path.\n");
        return -1;
    }
    snprintf(temporary_path, path_length + sizeof(".tmp.XXXXXX"),
             "%s.tmp.XXXXXX", intel_powercap_baseline_path);
    int fd = mkstemp(temporary_path);
    if (fd < 0) {
        perror("[ERROR] Failed to create temporary RAPL baseline");
        free(temporary_path);
        return -1;
    }
    FILE *file = fdopen(fd, "w");
    if (!file) {
        perror("[ERROR] Failed to write temporary RAPL baseline");
        close(fd);
        unlink(temporary_path);
        free(temporary_path);
        return -1;
    }

    int result = 0;
    if (fprintf(file, "GREENMEASURER_INTEL_RAPL_BASELINE 1 %zu\n",
                intel_powercap_snapshot_count) < 0) {
        result = -1;
    }
    for (size_t i = 0; result == 0 && i < intel_powercap_snapshot_count; i++) {
        const IntelPowercapSnapshot *state = &intel_powercap_snapshot[i];
        if (fprintf(file, "%"PRIu32" %"PRIu32" %.17g %.17g %.17g %.17g %d\n",
                    state->package, state->die,
                    state->long_limit.watts, state->long_limit.seconds,
                    state->short_limit.watts, state->short_limit.seconds,
                    state->zone_enabled) < 0) {
            result = -1;
        }
    }
    if (result == 0 && (fflush(file) != 0 || fsync(fileno(file)) != 0)) result = -1;
    if (fclose(file) != 0) result = -1;
    if (result == 0 && rename(temporary_path, intel_powercap_baseline_path) != 0) {
        perror("[ERROR] Failed to save RAPL baseline");
        result = -1;
    }
    if (result == 0) {
        char *directory = intel_powercap_parent_directory();
        int directory_fd = directory ? open(directory, O_RDONLY | O_CLOEXEC | O_DIRECTORY) : -1;
        if (directory_fd < 0 || fsync(directory_fd) != 0) {
            perror("[ERROR] Failed to sync RAPL baseline directory");
            result = -1;
        }
        if (directory_fd >= 0) close(directory_fd);
        free(directory);
    }
    if (result != 0) unlink(temporary_path);
    free(temporary_path);
    return result;
}

static int intel_load_or_capture_powercap_state(void) {
    if (intel_allocate_powercap_snapshot() != 0) return -1;
    int result = intel_read_powercap_baseline();
    if (result == 1) {
        if (intel_read_current_powercap_state() != 0 ||
            intel_write_powercap_baseline() != 0) {
            intel_free_powercap_snapshot();
            return -1;
        }
        fprintf(stderr, "[INFO] Saved this machine's RAPL baseline to %s.\n",
                intel_powercap_baseline_path);
        return 0;
    }
    if (result != 0) {
        intel_free_powercap_snapshot();
        return -1;
    }
    return 0;
}

static int intel_restore_powercap_state(void) {
    int result = 0;
    for (size_t i = 0; i < intel_powercap_snapshot_count; i++) {
        const IntelPowercapSnapshot *state = &intel_powercap_snapshot[i];
        raplcap_limit current_long, current_short;
        if (raplcap_pd_get_limits(&global_rc, state->package, state->die,
                                  RAPLCAP_ZONE_PACKAGE, &current_long, &current_short) != 0) {
            fprintf(stderr, "[ERROR] Failed to read package power limits for package %u die %u.\n",
                    state->package, state->die);
            result = -1;
            continue;
        }
        /* Time windows are never changed by this backend; rewriting them can quantize them. */
        raplcap_limit long_limit = {.watts = state->long_limit.watts, .seconds = 0};
        raplcap_limit short_limit = {.watts = state->short_limit.watts, .seconds = 0};
        const raplcap_limit *long_update =
            fabs(current_long.watts - long_limit.watts) > 1e-6 ? &long_limit : NULL;
        const raplcap_limit *short_update =
            fabs(current_short.watts - short_limit.watts) > 1e-6 ? &short_limit : NULL;
        if ((long_update || short_update) &&
            raplcap_pd_set_limits(&global_rc, state->package, state->die,
                                  RAPLCAP_ZONE_PACKAGE, long_update, short_update) != 0) {
            fprintf(stderr, "[ERROR] Failed to restore package power limits for package %u die %u.\n",
                    state->package, state->die);
            result = -1;
        }
    }
    for (size_t i = 0; i < intel_powercap_snapshot_count; i++) {
        const IntelPowercapSnapshot *state = &intel_powercap_snapshot[i];
        int enabled = raplcap_pd_is_zone_enabled(&global_rc, state->package, state->die,
                                                 RAPLCAP_ZONE_PACKAGE);
        if (enabled < 0) {
            fprintf(stderr, "[ERROR] Failed to read package power-limit state for package %u die %u.\n",
                    state->package, state->die);
            result = -1;
        } else if (enabled != state->zone_enabled &&
                   raplcap_pd_set_zone_enabled(&global_rc, state->package, state->die,
                                               RAPLCAP_ZONE_PACKAGE, state->zone_enabled) != 0) {
            fprintf(stderr, "[ERROR] Failed to restore package power-limit state for package %u die %u.\n",
                    state->package, state->die);
            result = -1;
        }
    }
    for (size_t i = 0; i < intel_powercap_snapshot_count; i++) {
        const IntelPowercapSnapshot *state = &intel_powercap_snapshot[i];
        raplcap_limit long_limit, short_limit;
        if (raplcap_pd_get_limits(&global_rc, state->package, state->die,
                                  RAPLCAP_ZONE_PACKAGE, &long_limit,
                                  &short_limit) != 0) {
            fprintf(stderr, "[ERROR] Failed to verify package power limits for package %u die %u.\n",
                    state->package, state->die);
            result = -1;
            continue;
        }
        int enabled = raplcap_pd_is_zone_enabled(&global_rc, state->package, state->die,
                                                 RAPLCAP_ZONE_PACKAGE);
        if (!isfinite(long_limit.watts) || !isfinite(long_limit.seconds) ||
            !isfinite(short_limit.watts) || !isfinite(short_limit.seconds) ||
            fabs(long_limit.watts - state->long_limit.watts) > 1e-6 ||
            fabs(long_limit.seconds - state->long_limit.seconds) > 1e-6 ||
            fabs(short_limit.watts - state->short_limit.watts) > 1e-6 ||
            fabs(short_limit.seconds - state->short_limit.seconds) > 1e-6 ||
            enabled != state->zone_enabled) {
            fprintf(stderr, "[ERROR] RAPL baseline mismatch for package %u die %u: "
                    "long %.12g W / %.12g s (expected %.12g W / %.12g s), "
                    "short %.12g W / %.12g s (expected %.12g W / %.12g s), "
                    "enabled %d (expected %d).\n",
                    state->package, state->die,
                    long_limit.watts, long_limit.seconds,
                    state->long_limit.watts, state->long_limit.seconds,
                    short_limit.watts, short_limit.seconds,
                    state->short_limit.watts, state->short_limit.seconds,
                    enabled, state->zone_enabled);
            result = -1;
        }
    }
    return result;
}

int intel_cpu_set_powercap(int watts) {
    if (watts == 0) return 0;
    if (watts < -1) {
        fprintf(stderr, "[ERROR] RAPL powercap must be -1 or a positive watt value.\n");
        return -1;
    }
    if (raplcap_active) {
        fprintf(stderr, "[ERROR] An earlier RAPL powercap context is still active.\n");
        return -1;
    }
    if (intel_acquire_powercap_lock() != 0) return -1;
    if (raplcap_init(&global_rc) != 0) {
        fprintf(stderr, "[ERROR] Failed to initialize the RAPL powercap context.\n");
        intel_release_powercap_lock();
        return -1;
    }
    raplcap_active = 1;

    if (intel_load_or_capture_powercap_state() != 0) {
        if (raplcap_destroy(&global_rc) != 0)
            fprintf(stderr, "[ERROR] Failed to destroy the RAPL powercap context after baseline failure.\n");
        raplcap_active = 0;
        intel_free_powercap_snapshot();
        intel_release_powercap_lock();
        return -1;
    }

    if (intel_restore_powercap_state() != 0) {
        intel_cpu_destroy_powercap();
        return -1;
    }
    if (watts == -1) return 0;

    raplcap_limit long_limit = { .watts = watts, .seconds = 0 };
    raplcap_limit short_limit = { .watts = watts, .seconds = 0 };
    int result = 0;
    for (size_t i = 0; i < intel_powercap_snapshot_count; i++) {
        const IntelPowercapSnapshot *state = &intel_powercap_snapshot[i];
        if (raplcap_pd_set_limits(&global_rc, state->package, state->die,
                                  RAPLCAP_ZONE_PACKAGE, &long_limit, &short_limit) != 0) {
            fprintf(stderr, "[ERROR] Failed to set package power limits for package %u die %u.\n",
                    state->package, state->die);
            result = -1;
        }
    }
    for (size_t i = 0; i < intel_powercap_snapshot_count; i++) {
        const IntelPowercapSnapshot *state = &intel_powercap_snapshot[i];
        if (raplcap_pd_set_zone_enabled(&global_rc, state->package, state->die,
                                        RAPLCAP_ZONE_PACKAGE, 1) != 0) {
            fprintf(stderr, "[ERROR] Failed to enable package power limits for package %u die %u.\n",
                    state->package, state->die);
            result = -1;
        }
    }
    if (result != 0) intel_cpu_destroy_powercap();
    return result;
}

int intel_cpu_destroy_powercap(void) {
    if (!raplcap_active) return 0;
    int result = intel_restore_powercap_state();
    if (raplcap_destroy(&global_rc) != 0) {
        fprintf(stderr, "[ERROR] Failed to destroy the RAPL powercap context.\n");
        result = -1;
    }
    raplcap_active = 0;
    intel_free_powercap_snapshot();
    intel_release_powercap_lock();
    return result;
}

void intel_cpu_powercap_end(void) {
    if (intel_cpu_destroy_powercap() != 0)
        fprintf(stderr, "[ERROR] Failed to restore powercap during teardown.\n");
}

/* ---- Powercap calibration ---- */

/* Copies the measurement rows of one per-powercap CSV; the header is kept only for the first. */
static int append_calibration_rows(FILE *output, const char *filename, int keep_header) {
    FILE *input = fopen(filename, "r");
    if (!input) {
        perror("[ERROR] Failed to read calibration measurement CSV");
        return -1;
    }

    char buffer[1024];
    if (!fgets(buffer, sizeof(buffer), input)) {
        fprintf(stderr, "[ERROR] Calibration measurement CSV has no header: %s\n", filename);
        fclose(input);
        return -1;
    }
    int result = keep_header && fputs(buffer, output) == EOF ? -1 : 0;
    size_t rows_written = 0;
    while (result == 0 && fgets(buffer, sizeof(buffer), input)) {
        if (fputs(buffer, output) == EOF) {
            result = -1;
            break;
        }
        rows_written++;
    }
    if (ferror(input)) result = -1;
    if (fclose(input) != 0) result = -1;
    if (rows_written == 0) {
        fprintf(stderr, "[ERROR] Calibration measurement CSV has no measurement rows: %s\n", filename);
        result = -1;
    }
    return result;
}

/* The engine provides runTesting; it stays optional so that libraries linked without it still link. */
extern int runTesting(int, int, const char *, const char *, const char *, int,
                      int, float, int, int) __attribute__((weak));


int intel_cpu_calibrate_powercap(int ntimes, int core, const char *command,
                                 const char *program, const char *calib_csv_file,
                                 const char *process_script, int min_powercap, int max_powercap,
                                 int time_out_limit, float temp_threshold, int variance, int memory) {
    if (min_powercap < 1 || max_powercap < min_powercap || !command || !*command || !runTesting) {
        fprintf(stderr, "[ERROR] Powercap calibration requires a command and a range of at least 1 W.\n");
        return -1;
    }
    if (!calib_csv_file || strlen(calib_csv_file) == 0) {
        calib_csv_file = "powercap_calibration.csv";
    }

    printf("[INFO] Powercap calibration: Testing %s with different power limits...\n", program);

    FILE *outputFile = fopen(calib_csv_file, "w");
    if (!outputFile) {
        perror("[ERROR] Failed to open calibration CSV for writing");
        return -1;
    }
    energy_share_output_file(fileno(outputFile));

    int result = 0;
    int first_measurement = 1;

    printf("[INFO] Powercap -1 uses the saved per-machine RAPL baseline.\n");
    for (int power_cap = -1; ; power_cap = power_cap == -1 ? min_powercap : power_cap + 1) {
        printf("[INFO] Powercap calibration: Testing with power cap = %d W\n", power_cap);

        if (intel_cpu_set_powercap(power_cap) != 0) {
            fprintf(stderr, "[ERROR] Failed to apply powercap state for %d W; calibration aborted.\n",
                    power_cap);
            result = -1;
            break;
        }

        char filename[] = "/tmp/greenmeasurer-calibration-XXXXXX";
        int temporary_fd = mkstemp(filename);
        if (temporary_fd < 0) {
            perror("[ERROR] Failed to create temporary calibration CSV");
            if (intel_cpu_destroy_powercap() != 0) result = -1;
            result = -1;
            break;
        }
        if (close(temporary_fd) != 0) {
            perror("[ERROR] Failed to close temporary calibration CSV");
            remove(filename);
            if (intel_cpu_destroy_powercap() != 0) result = -1;
            result = -1;
            break;
        }

        if (runTesting(ntimes, core, command, program, filename,
                       power_cap, time_out_limit, temp_threshold, variance, memory) != 0) {
            fprintf(stderr, "[ERROR] Calibration measurement failed for powercap %d.\n", power_cap);
            result = -1;
        }

        if (intel_cpu_destroy_powercap() != 0) result = -1;
        if (result == 0 && append_calibration_rows(outputFile, filename, first_measurement) != 0) result = -1;
        first_measurement = 0;
        if (remove(filename) != 0) {
            perror("[ERROR] Failed to remove temporary calibration CSV");
            result = -1;
        }

        if (result != 0 || power_cap == max_powercap) break;
        sleep(1);
    }

    if (fclose(outputFile) != 0) result = -1;
    if (result != 0) return -1;

    if (process_script && access(process_script, F_OK) == 0) {
        char proc_cmd[1024];
        snprintf(proc_cmd, sizeof(proc_cmd), "python3 \"%s\" \"%s\"", process_script, calib_csv_file);
        printf("[INFO] Running calibration post-processor: %s\n", proc_cmd);
        if (system(proc_cmd) != 0) {
            fprintf(stderr, "[ERROR] Python post-processing script exited with error.\n");
            return -1;
        }
    } else {
        printf("[INFO] Powercap calibration CSV saved to %s\n", calib_csv_file);
    }
    return 0;
}

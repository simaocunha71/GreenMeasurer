static int configured_standard_integer(const char *text, int minimum, int *result) {
    char *end;
    errno = 0;
    long value = strtol(text, &end, 10);
    if (errno || end == text || *end || value < minimum || value > INT_MAX) return -1;
    *result = (int)value;
    return 0;
}

typedef struct {
    int ready[2];
    int periodic;
    int window[2];
    double *values;
    unsigned char *available;
    unsigned char *failed;
    int workload_processes;
    int running_processes;
    double freq_sum[SYS_MAX_CORES];
    unsigned long freq_samples[SYS_MAX_CORES];
    unsigned char freq_failed[SYS_MAX_CORES];
    int workload_processes_failed;
    int running_processes_failed;
    int cores;
    long memory_kb;
} ConfiguredStandardBackends;

static int standard_backend_selected(size_t i) {
    return !configured_overtime || (configured_backend_selected && configured_backend_selected[i]);
}

static int standard_system_selected(int i) {
    return !configured_overtime || configured_system[i];
}

static int configured_standard_prepare_backends(ConfiguredStandardBackends *state) {
    if (!active_metric_count || (configured_overtime && !configured_backend_name_count && !configured_all)) return 0;
    state->values = calloc(active_metric_count, sizeof(*state->values));
    state->available = calloc(active_metric_count, sizeof(*state->available));
    state->failed = calloc(active_metric_count, sizeof(*state->failed));
    if (!state->values || !state->available || !state->failed) {
        fprintf(stderr, "[ERROR] Unable to allocate standard metric results.\n");
        return -1;
    }
    for (size_t i = 0; i < active_metric_count; i++) {
        if (!standard_backend_selected(i)) continue;
        const ActiveMetric *metric = &active_metrics[i];
        state->available[i] = 1;
        state->ready[metric->backend_type] = 1;
        if (metric->descriptor->kind != ENERGY_METRIC_GAUGE ||
            metric->descriptor->scope == ENERGY_METRIC_PROCESS ||
            (metric->descriptor->flags & (ENERGY_METRIC_FLAG_CPU_TEMPERATURE |
                                          ENERGY_METRIC_FLAG_GPU_TEMPERATURE))) {
            state->periodic = 1;
        }
        if (metric->descriptor->kind == ENERGY_METRIC_WINDOW)
            state->window[metric->backend_type] = 1;
    }
    ot_find_next_callbacks();
    for (int type = 0; type < 2; type++) {
        if (!state->ready[type]) continue;
        int initialized = type == ENERGY_BACKEND_CPU
            ? active_cpu_backend->init(0) == 0 : active_gpu_backend->init(0) == 0;
        if (!initialized || ot_backend_start((EnergyBackendType)type) != 0 ||
            ot_backend_stop((EnergyBackendType)type) != 0) {
            if (configured_overtime) {
                fprintf(stderr, "[WARNING] Failed to initialize or probe selected %s backend.\n",
                        type == ENERGY_BACKEND_CPU ? "CPU" : "GPU");
                configured_backend_failure_guidance((EnergyBackendType)type);
                skip_configured_backend_type((EnergyBackendType)type);
                state->ready[type] = 0;
                continue;
            }
            fprintf(stderr, "[WARNING] %s backend initialization or sampling failed; metrics are unavailable.\n",
                    type == ENERGY_BACKEND_CPU ? "CPU" : "GPU");
            state->ready[type] = 0;
            continue;
        }
        for (size_t i = 0; i < active_metric_count; i++) {
            if (!standard_backend_selected(i) || (int)active_metrics[i].backend_type != type ||
                active_metrics[i].descriptor->scope == ENERGY_METRIC_PROCESS) continue;
            double value = read_active_metric(i, 0);
            if (!isfinite(value) || value < 0.0) {
                if (!configured_overtime) {
                    state->available[i] = 0;
                    continue;
                }
                skip_configured_backend_metric(i);
                state->available[i] = 0;
            }
        }
        if (configured_overtime) {
            int selected = 0;
            for (size_t i = 0; i < active_metric_count; i++)
                if (configured_backend_selected[i] && (int)active_metrics[i].backend_type == type)
                    selected = 1;
            if (!selected) {
                state->ready[type] = 0;
                continue;
            }
        }
        if (state->window[type] && !ot_sample_next[type])
            fprintf(stderr, "[WARNING] %s backend does not support contiguous standard windows; "
                    "energy between windows will not be recorded.\n",
                    type == ENERGY_BACKEND_CPU ? "CPU" : "GPU");
    }
    if (configured_overtime && !configured_has_metrics()) {
        fprintf(stderr, "[ERROR] No selected metrics are available.\n");
        return -1;
    }
    return 0;
}

static int configured_standard_sample(ConfiguredStandardBackends *state, pid_t pid, int finished) {
    for (int type = 0; type < 2; type++) {
        if (!state->ready[type]) continue;
        if ((ot_sample_next[type] ? ot_sample_next[type](0)
                                  : ot_backend_stop((EnergyBackendType)type)) != 0) {
            if (configured_overtime) {
                fprintf(stderr, "[WARNING] Selected %s backend sample failed; "
                        "recording NOT_SUPPORTED for its metrics.\n",
                        type == ENERGY_BACKEND_CPU ? "CPU" : "GPU");
                state->ready[type] = 0;
                for (size_t i = 0; i < active_metric_count; i++)
                    if (standard_backend_selected(i) && (int)active_metrics[i].backend_type == type) {
                        state->failed[i] = 1;
                        state->values[i] = METRIC_NOT_SUPPORTED;
                    }
                continue;
            }
            fprintf(stderr, "[ERROR] Selected %s backend sample failed.\n",
                    type == ENERGY_BACKEND_CPU ? "CPU" : "GPU");
            if (configured_overtime) configured_backend_failure_guidance((EnergyBackendType)type);
            return -1;
        }
        for (size_t i = 0; i < active_metric_count; i++) {
            if (!standard_backend_selected(i) || !state->available[i] || state->failed[i] ||
                (int)active_metrics[i].backend_type != type) continue;
            const EnergyMetricDescriptor *metric = active_metrics[i].descriptor;
            if (metric->kind == ENERGY_METRIC_GAUGE && !finished &&
                metric->scope != ENERGY_METRIC_PROCESS &&
                !(metric->flags & (ENERGY_METRIC_FLAG_CPU_TEMPERATURE |
                                   ENERGY_METRIC_FLAG_GPU_TEMPERATURE))) continue;
            if (finished && metric->scope == ENERGY_METRIC_PROCESS) continue;
            double value = metric->scope == ENERGY_METRIC_PROCESS
                ? read_process_tree_metric(i, pid) : read_active_metric(i, 0);
            if (!isfinite(value) || value < 0.0) {
                if (metric->scope == ENERGY_METRIC_PROCESS) continue;
                if (configured_overtime) {
                    fprintf(stderr, "[WARNING] Failed to read selected metric '%s.%s'; "
                            "recording NOT_SUPPORTED for this repetition.\n",
                            type == ENERGY_BACKEND_CPU ? "cpu" : "gpu", metric->name);
                    state->failed[i] = 1;
                    state->values[i] = METRIC_NOT_SUPPORTED;
                    continue;
                }
                fprintf(stderr, "[ERROR] Failed to read selected metric '%s.%s'.\n",
                        type == ENERGY_BACKEND_CPU ? "cpu" : "gpu", metric->name);
                if (configured_overtime) {
                    fprintf(stderr, "[INFO] Check hardware support and permissions; a read failure "
                            "does not imply the metric is undeclared.\n");
                    green_metric_extension_guidance();
                }
                return -1;
            }
            if (metric->kind == ENERGY_METRIC_WINDOW) state->values[i] += value;
            else if (metric->kind == ENERGY_METRIC_PEAK) {
                if (value > state->values[i] || state->values[i] < 0) state->values[i] = value;
            } else state->values[i] = value;
            if (!isfinite(state->values[i])) {
                if (configured_overtime) {
                    fprintf(stderr, "[WARNING] Selected metric '%s.%s' overflowed; "
                            "recording NOT_SUPPORTED for this repetition.\n",
                            type == ENERGY_BACKEND_CPU ? "cpu" : "gpu", metric->name);
                    state->failed[i] = 1;
                    state->values[i] = METRIC_NOT_SUPPORTED;
                    continue;
                }
                fprintf(stderr, "[ERROR] Selected metric '%s.%s' overflowed.\n",
                        type == ENERGY_BACKEND_CPU ? "cpu" : "gpu", metric->name);
                if (configured_overtime) green_metric_extension_guidance();
                return -1;
            }
        }
        if (!finished && !ot_sample_next[type] && ot_backend_start((EnergyBackendType)type) != 0) {
            if (configured_overtime) {
                fprintf(stderr, "[WARNING] Failed to restart selected %s backend; "
                        "recording NOT_SUPPORTED for its metrics.\n",
                        type == ENERGY_BACKEND_CPU ? "CPU" : "GPU");
                state->ready[type] = 0;
                for (size_t i = 0; i < active_metric_count; i++)
                    if (standard_backend_selected(i) && (int)active_metrics[i].backend_type == type) {
                        state->failed[i] = 1;
                        state->values[i] = METRIC_NOT_SUPPORTED;
                    }
                continue;
            }
            fprintf(stderr, "[ERROR] Failed to restart selected %s backend.\n",
                    type == ENERGY_BACKEND_CPU ? "CPU" : "GPU");
            if (configured_overtime) configured_backend_failure_guidance((EnergyBackendType)type);
            return -1;
        }
    }
    return 0;
}

static int configured_standard_sample_system(ConfiguredStandardBackends *state, pid_t pid) {
    if (standard_system_selected(2)) {
        int value = sys_workload_process_count(pid);
        if (configured_overtime && value < 0) {
            if (!state->workload_processes_failed)
                fprintf(stderr, "[WARNING] Failed to read system.workload_processes; "
                        "recording NOT_SUPPORTED for this repetition.\n");
            state->workload_processes_failed = 1;
            state->workload_processes = SYS_METRIC_NOT_SUPPORTED;
        } else if (!state->workload_processes_failed && value > state->workload_processes)
            state->workload_processes = value;
    }
    if (standard_system_selected(3)) {
        int value = sys_running_processes();
        if (configured_overtime && value < 0) {
            if (!state->running_processes_failed)
                fprintf(stderr, "[WARNING] Failed to read system.running_processes; "
                        "recording NOT_SUPPORTED for this repetition.\n");
            state->running_processes_failed = 1;
            state->running_processes = SYS_METRIC_NOT_SUPPORTED;
        } else if (!state->running_processes_failed && value > state->running_processes)
            state->running_processes = value;
    }
    if (standard_system_selected(4)) {
        for (int core = 0; core < state->cores; core++) {
            double value = sys_core_frequency_mhz(core);
            if (value < 0 || !isfinite(value)) {
                if (configured_overtime) {
                    if (!state->freq_failed[core])
                        fprintf(stderr, "[WARNING] Failed to read system.core_frequency for core %d; "
                                "recording NOT_SUPPORTED for this repetition.\n", core);
                    state->freq_failed[core] = 1;
                }
                continue;
            }
            if (!state->freq_failed[core]) {
                state->freq_sum[core] += value;
                if (!isfinite(state->freq_sum[core]) && configured_overtime) {
                    fprintf(stderr, "[WARNING] system.core_frequency sum overflowed for core %d; "
                            "recording NOT_SUPPORTED for this repetition.\n", core);
                    state->freq_failed[core] = 1;
                } else {
                    state->freq_samples[core]++;
                }
            }
        }
    }
    return 0;
}

static int configured_standard_command(const char *command, int timeout, double *elapsed_ms,
                                       ConfiguredStandardBackends *state) {
    sigset_t mask, old_mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGCHLD);
    if (sigprocmask(SIG_BLOCK, &mask, &old_mask) != 0) {
        perror("[ERROR] Failed to block SIGCHLD");
        return -1;
    }
    int result = -1;
    pid_t pid = -1;
    struct timespec start, now;
    if (state->failed) memset(state->failed, 0, active_metric_count * sizeof(*state->failed));
    for (int type = 0; type < 2; type++) {
        if (state->ready[type] && ot_backend_start((EnergyBackendType)type) != 0) {
            if (configured_overtime) {
                fprintf(stderr, "[WARNING] Failed to start selected %s backend; "
                        "recording NOT_SUPPORTED for its metrics.\n",
                        type == ENERGY_BACKEND_CPU ? "CPU" : "GPU");
                state->ready[type] = 0;
                continue;
            }
            fprintf(stderr, "[ERROR] Failed to start selected %s backend.\n",
                    type == ENERGY_BACKEND_CPU ? "CPU" : "GPU");
            if (configured_overtime) configured_backend_failure_guidance((EnergyBackendType)type);
            goto restore;
        }
    }
    if (state->values) {
        for (size_t i = 0; i < active_metric_count; i++)
            state->values[i] = state->available[i] && state->ready[active_metrics[i].backend_type] &&
                active_metrics[i].descriptor->scope != ENERGY_METRIC_PROCESS ? 0.0 : METRIC_NOT_SUPPORTED;
    }
    state->workload_processes = state->running_processes = SYS_METRIC_NOT_SUPPORTED;
    state->workload_processes_failed = state->running_processes_failed = 0;
    memset(state->freq_sum, 0, sizeof(state->freq_sum));
    memset(state->freq_samples, 0, sizeof(state->freq_samples));
    memset(state->freq_failed, 0, sizeof(state->freq_failed));
    if (clock_gettime(CLOCK_MONOTONIC, &start) != 0) {
        perror("[ERROR] Failed to read monotonic clock");
        goto restore;
    }
    pid = ot_start_command(command, &old_mask);
    if (pid < 0) {
        perror("[ERROR] Failed to create workload process");
        goto restore;
    }
    if (configured_standard_sample_system(state, pid) != 0) goto abort;
    for (size_t i = 0; i < active_metric_count; i++) {
        if (!standard_backend_selected(i) || !state->ready[active_metrics[i].backend_type] ||
            active_metrics[i].descriptor->scope != ENERGY_METRIC_PROCESS) continue;
        double value = read_process_tree_metric(i, pid);
        if (isfinite(value) && value >= 0.0) state->values[i] = value;
    }
    for (;;) {
        siginfo_t info = {0};
        if (waitid(P_PID, (id_t)pid, &info, WEXITED | WNOHANG | WNOWAIT) != 0) {
            if (errno == EINTR) continue;
            perror("[ERROR] Failed while waiting for workload");
            goto abort;
        }
        if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
            perror("[ERROR] Failed to read monotonic clock");
            goto abort;
        }
        if (info.si_pid == pid) {
            int status;
            struct rusage usage;
            pid_t waited;
            do { waited = wait4(pid, &status, 0, &usage); } while (waited < 0 && errno == EINTR);
            if (waited < 0) {
                perror("[ERROR] Failed to reap workload");
                goto restore;
            }
            pid = -1;
            if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
                fprintf(stderr, "[ERROR] Standard workload failed.\n");
                goto restore;
            }
            *elapsed_ms = (double)(now.tv_sec - start.tv_sec) * 1000.0 +
                          (double)(now.tv_nsec - start.tv_nsec) / 1000000.0;
            state->memory_kb = usage.ru_maxrss;
            if (configured_standard_sample(state, 0, 1) != 0) goto restore;
            if (configured_overtime) {
                for (size_t i = 0; i < active_metric_count; i++) {
                    if (standard_backend_selected(i) &&
                        active_metrics[i].descriptor->scope == ENERGY_METRIC_PROCESS &&
                        state->values[i] < 0.0) {
                        fprintf(stderr, "[WARNING] Selected metric '%s.%s' was unavailable during the workload; "
                                "recording NOT_SUPPORTED.\n",
                                active_metrics[i].backend_type == ENERGY_BACKEND_CPU ? "cpu" : "gpu",
                                active_metrics[i].descriptor->name);
                        fprintf(stderr, "[INFO] Check hardware support, permissions and whether "
                                "the workload uses the device.\n");
                    }
                }
            }
            result = 0;
            goto restore;
        }
        if (global_terminate) {
            fprintf(stderr, "[ERROR] Standard measurement interrupted.\n");
            goto abort;
        }
        struct timespec remaining = {start.tv_sec + timeout - now.tv_sec,
                                     start.tv_nsec - now.tv_nsec};
        if (remaining.tv_nsec < 0) {
            remaining.tv_sec--;
            remaining.tv_nsec += 1000000000L;
        }
        if (remaining.tv_sec < 0 ||
            (remaining.tv_sec == 0 && remaining.tv_nsec == 0)) {
            fprintf(stderr, "[ERROR] Standard workload exceeded its timeout.\n");
            goto abort;
        }
        int sample_due = state->periodic && (remaining.tv_sec > 0 || remaining.tv_nsec > 50000000L);
        struct timespec delay = sample_due ? (struct timespec){0, 50000000L} : remaining;
        if (sigtimedwait(&mask, NULL, &delay) < 0) {
            if (errno == EAGAIN && sample_due) {
                if (configured_standard_sample_system(state, pid) != 0 ||
                    configured_standard_sample(state, pid, 0) != 0) goto abort;
            } else if (errno != EAGAIN && errno != EINTR) {
                perror("[ERROR] Failed while waiting for workload");
                goto abort;
            }
        }
    }
abort:
    if (pid > 0) ot_terminate_command(pid);
restore:
    if (sigprocmask(SIG_SETMASK, &old_mask, NULL) != 0) {
        perror("[ERROR] Failed to restore signal mask");
        result = -1;
    }
    return result;
}

static int configured_standard_run(int argc, char **argv) {
    const char *command = NULL, *language = "C", *program = "Target";
    const char *output = "measurements.csv";
    int repetitions = 1, timeout = 3600, variance = 0, sleep_time = 5;
    int powercaps[MAX_POWERCAPS] = {-1}, powercap_count = 1, powercap = -1;
    int powercap_report = 0, any_cap = 0;
    const char *compile_command = NULL;
    int status = 1, cap_applied = 0, fd = -1, temporary_created = 0;
    char *temporary = NULL;
    FILE *stream = NULL;
    ConfiguredStandardBackends backends = {0};

    for (int i = configured_standard || (argc > 1 && !strcmp(argv[1], "--standard")) ? 2 : 1;
         i < argc; i++) {
        if (!strcmp(argv[i], "--configured-system")) { i++; continue; }
        if (!strcmp(argv[i], "--configured-all")) continue;
        if (!strcmp(argv[i], "--configured-adapter")) { i += 2; continue; }
        if (!strcmp(argv[i], "--configured-external")) { i += 3; continue; }
        if (!strcmp(argv[i], "--configured-backend")) { i++; continue; }
        if (!strcmp(argv[i], "--powercap-report")) { powercap_report = 1; continue; }
        const char *option = argv[i];
        if (++i >= argc) return fprintf(stderr, "[ERROR] Missing option value.\n"), 1;
        const char *value = argv[i];
        if (!strcmp(option, "-command") || !strcmp(option, "-run_cmd")) command = value;
        else if (!strcmp(option, "-language")) language = value;
        else if (!strcmp(option, "-program")) program = value;
        else if (!strcmp(option, "-compile_cmd")) compile_command = value;
        else if (!strcmp(option, "-powercap")) {
            powercap_count = parse_powercap_list(value, powercaps, MAX_POWERCAPS);
            if (powercap_count < 0)
                return fprintf(stderr, "[ERROR] -powercap must be a comma-separated list of unique values, "
                                       "each -1 (no GreenMeasurer cap) or at least 1 W.\n"), 1;
        }
        else if (!strcmp(option, "-output") || !strcmp(option, "-measurements-file")) output = value;
        else if (!strcmp(option, "-calib-file")) {
            if (strlen(value) >= sizeof(calib_file)) return fprintf(stderr, "[ERROR] Calibration path is too long.\n"), 1;
            strcpy(calib_file, value);
        } else {
            int *target = NULL, minimum = 0;
            if (!strcmp(option, "-n_times")) { target = &repetitions; minimum = 1; }
            else if (!strcmp(option, "-timeout-seconds")) { target = &timeout; minimum = 1; }
            else if (!strcmp(option, "-variance")) target = &variance;
            else if (!strcmp(option, "-sleep-secs") || !strcmp(option, "-sleep_time")) target = &sleep_time;
            else if (!strcmp(option, "-time_out_limit")) {
                int hours;
                if (configured_standard_integer(value, 1, &hours) != 0 || hours > INT_MAX / 3600)
                    return fprintf(stderr, "[ERROR] Invalid standard timeout.\n"), 1;
                timeout = hours * 3600;
                continue;
            }
            if (!target || configured_standard_integer(value, minimum, target) != 0) {
                fprintf(stderr, "[ERROR] Invalid configured standard option '%s'.\n", option);
                return 1;
            }
        }
    }
    if (!command || !*command) return fprintf(stderr, "[ERROR] Standard workload command is required.\n"), 1;
    if (strpbrk(language, ",\"\r\n") || strpbrk(program, ",\"\r\n"))
        return fprintf(stderr, "[ERROR] Invalid CSV metadata.\n"), 1;
    if (compile_command && *compile_command) {
        printf("[INFO] Compiling benchmark target: %s\n", compile_command);
        if (system(compile_command) != 0) {
            fprintf(stderr, "[ERROR] Compilation command failed.\n");
            return 1;
        }
    }
    if (configured_overtime) probe_configured_system();
    if (configured_overtime && probe_configured_external() != 0) return 1;
    if (!configured_overtime && (standard_system_selected(1) || standard_system_selected(4)) && sys_core_count() <= 0) {
        fprintf(stderr, "[ERROR] Failed to read CPU core count.\n");
        if (configured_overtime) green_metric_extension_guidance();
        return 1;
    }
    if (configured_standard_prepare_backends(&backends) != 0) {
        free(backends.values);
        free(backends.available);
        free(backends.failed);
        return 1;
    }
    backends.cores = configured_overtime && configured_system[4]
        ? configured_frequency_cores : sys_core_count();
    if (backends.cores > SYS_MAX_CORES && configured_system[4]) {
        fprintf(stderr, "[ERROR] Frequency sampling supports at most %d cores, found %d.\n",
                SYS_MAX_CORES, backends.cores);
        goto cleanup;
    }
    if (backends.cores > SYS_MAX_CORES) backends.cores = SYS_MAX_CORES;
    if (standard_system_selected(2) || standard_system_selected(3) || standard_system_selected(4))
        backends.periodic = 1;
    if (access(calib_file, F_OK) != 0 && calibrateTemperature(calib_file, sleep_time) < 0) goto cleanup;
    float baseline;
    if (resolve_cpu_temperature_baseline(calib_file, &baseline) != 0) goto cleanup;
    for (int c = 0; c < powercap_count; c++) if (powercaps[c] > 0) any_cap = 1;
    if (any_cap) {
        if (!active_cpu_backend || !active_cpu_backend->set_powercap ||
            !active_cpu_backend->destroy_powercap) {
            fprintf(stderr, "[ERROR] Active CPU backend does not support powercapping.\n");
            goto cleanup;
        }
    }
    /* Every applied limit is restored as soon as its measurements end. */
    temporary = malloc(strlen(output) + sizeof(".tmp.XXXXXX"));
    if (!temporary) {
        fprintf(stderr, "[ERROR] Unable to allocate temporary output path.\n");
        goto cleanup;
    }
    sprintf(temporary, "%s.tmp.XXXXXX", output);
    fd = mkstemp(temporary);
    if (fd >= 0) {
        temporary_created = 1;
        energy_share_output_file(fd);
    }
    if (fd < 0 || !(stream = fdopen(fd, "w"))) {
        perror("[ERROR] Failed to create temporary measurement CSV");
        if (fd >= 0) close(fd);
        goto cleanup;
    }
    energy_backend_write_csv_header(stream, 0);
    for (int cap_index = 0; cap_index < powercap_count; cap_index++) {
        powercap = powercaps[cap_index];
        if (powercap > 0) {
            if (active_cpu_backend->set_powercap(powercap) != 0) {
                fprintf(stderr, "[ERROR] Failed to set powercap to %d W; measurement aborted.\n", powercap);
                if (active_cpu_backend->destroy_powercap() != 0)
                    fprintf(stderr, "[ERROR] Failed to destroy powercap context.\n");
                goto cleanup;
            }
            cap_applied = 1;
        }
        for (int repetition = 0; repetition < repetitions; repetition++) {
            float measurement_temperature;
            if (wait_for_cpu_temperature(baseline, variance, "Standard",
                                         &measurement_temperature) != 0) goto cleanup;
            double elapsed;
            if (configured_standard_command(command, timeout, &elapsed, &backends) != 0) goto cleanup;
            for (size_t i = 0; i < active_metric_count; i++) {
                if (standard_backend_selected(i) &&
                    active_metrics[i].backend_type == ENERGY_BACKEND_CPU &&
                    (active_metrics[i].descriptor->flags & ENERGY_METRIC_FLAG_CPU_TEMPERATURE) &&
                    backends.available[i]) {
                    backends.values[i] = measurement_temperature;
                }
            }
            fprintf(stream, "%s, %s, %d, %.6f", language, program, powercap, elapsed);
            if (standard_system_selected(0)) fprintf(stream, ", %ld", backends.memory_kb);
            if (standard_system_selected(1)) {
                int cores = sys_core_count();
                if (cores <= 0) {
                    if (!configured_overtime) {
                        fprintf(stderr, "[ERROR] Failed to read system.cores.\n");
                        goto cleanup;
                    }
                    fprintf(stderr, "[WARNING] Failed to read system.cores; recording NOT_SUPPORTED.\n");
                    fprintf(stream, ", NOT_SUPPORTED");
                } else {
                    fprintf(stream, ", %d", cores);
                }
            }
            if (standard_system_selected(2)) {
                fprintf(stream, ", ");
                write_system_integer_value(stream, backends.workload_processes);
            }
            if (standard_system_selected(3)) {
                fprintf(stream, ", ");
                write_system_integer_value(stream, backends.running_processes);
            }
            if (standard_system_selected(4)) {
                for (int core = 0; core < backends.cores; core++) {
                    fprintf(stream, ", ");
                    write_active_metric_value(stream, !backends.freq_failed[core] && backends.freq_samples[core]
                        ? backends.freq_sum[core] / (double)backends.freq_samples[core]
                        : SYS_METRIC_NOT_SUPPORTED);
                }
            }
            for (size_t i = 0; i < active_metric_count; i++) {
                if (!standard_backend_selected(i)) continue;
                fprintf(stream, ", ");
                write_active_metric_value(stream, backends.values[i]);
            }
            for (size_t i = 0; i < configured_external_count; i++) {
                ConfiguredExternalMetric *metric = &configured_external_metrics[i];
                double value;
                if (read_configured_external(metric, &value) != 0) {
                    fprintf(stderr, "[WARNING] Recording NOT_SUPPORTED for the failed external metric sample.\n");
                    value = NAN;
                }
                fprintf(stream, ", ");
                write_external_metric_value(stream, value);
            }
            fputc('\n', stream);
        }
        if (cap_applied) {
            cap_applied = 0;
            if (active_cpu_backend->destroy_powercap() != 0) {
                fprintf(stderr, "[ERROR] Failed to destroy powercap context.\n");
                goto cleanup;
            }
        }
        if (cap_index + 1 < powercap_count) sleep(1);
    }
    if (ferror(stream)) {
        fprintf(stderr, "[ERROR] Failed to write standard CSV.\n");
        goto cleanup;
    }
    if (fclose(stream) != 0) {
        stream = NULL;
        perror("[ERROR] Failed to close standard CSV");
        goto cleanup;
    }
    stream = NULL;
    if (rename(temporary, output) != 0) {
        perror("[ERROR] Failed to publish standard CSV");
        goto cleanup;
    }
    printf("[INFO] Execution Details: Standard session finished -> %s\n", output);
    if (powercap_report) run_powercap_processor(output);
    status = 0;
cleanup:
    if (stream && fclose(stream) != 0) perror("[ERROR] Failed to close standard CSV");
    if (cap_applied && active_cpu_backend->destroy_powercap() != 0)
        fprintf(stderr, "[ERROR] Failed to destroy powercap context.\n");
    if (temporary_created && status != 0 && unlink(temporary) != 0 && errno != ENOENT)
        perror("[ERROR] Failed to remove temporary measurement CSV");
    free(temporary);
    free(backends.values);
    free(backends.available);
    free(backends.failed);
    return status;
}

/* Over-time mode runs the sampler in the parent process, without sampling threads. */
#ifndef OT_MAX_SAMPLES
#define OT_MAX_SAMPLES 262144
#endif
#define OT_MAX_METRICS 64
#define OT_MAX_EXTERNAL_METRICS 30
#define OT_SCALAR_COUNT 4

struct OvertimeContext {
    const char *language;
    const char *program;
    int powercap;
    const char *output_file;
    int append;
};

static long ot_memory_kb[OT_MAX_SAMPLES];
static int ot_cores[OT_MAX_SAMPLES];
static int ot_process_values[2][OT_MAX_SAMPLES];
static float ot_freq_mhz[OT_MAX_SAMPLES][SYS_MAX_CORES];
static double ot_backend_values[OT_MAX_METRICS][OT_MAX_SAMPLES];
static GreenMetricValue ot_external_values[OT_MAX_EXTERNAL_METRICS][OT_MAX_SAMPLES];
static double ot_timestamp_us[OT_MAX_SAMPLES];
static size_t ot_count;
static int ot_core_count;
static unsigned char ot_scalar_warned[OT_SCALAR_COUNT];
static unsigned char ot_frequency_warned[SYS_MAX_CORES];
static int ot_backend_ready[2];
static int (*ot_sample_next[2])(int);
static EnergyBackendReadCounter ot_read_counter[2];
static unsigned char ot_counter_warned[2];
static unsigned char ot_counter_metric_warned[OT_MAX_METRICS];
static struct timespec ot_start_time;

static int ot_backend_start(EnergyBackendType type) {
    return type == ENERGY_BACKEND_CPU
        ? active_cpu_backend->sample_start(0)
        : active_gpu_backend->sample_start(0);
}

static int ot_backend_stop(EnergyBackendType type) {
    return type == ENERGY_BACKEND_CPU
        ? active_cpu_backend->sample_stop(0)
        : active_gpu_backend->sample_stop(0);
}

static void ot_find_next_callbacks(void) {
    memset(ot_sample_next, 0, sizeof(ot_sample_next));
    memset(ot_read_counter, 0, sizeof(ot_read_counter));
    for (size_t i = 0; i < loaded_backend_count; i++) {
        const EnergyBackendDescriptor *descriptor = loaded_backends[i].descriptor;
        if (!descriptor) continue;
        EnergyBackendType type = descriptor->type;
        if ((type == ENERGY_BACKEND_CPU && descriptor->backend.cpu == active_cpu_backend) ||
            (type == ENERGY_BACKEND_GPU && descriptor->backend.gpu == active_gpu_backend)) {
            ot_sample_next[type] = dlsym(loaded_backends[i].handle,
                                         ENERGY_BACKEND_SAMPLE_NEXT_SYMBOL);
            ot_read_counter[type] = dlsym(loaded_backends[i].handle,
                                          ENERGY_BACKEND_READ_COUNTER_SYMBOL);
        }
    }
}

static int ot_prepare(void) {
    if ((!configured_overtime || configured_backend_name_count || configured_all) && active_metric_count > OT_MAX_METRICS) {
        fprintf(stderr, "[ERROR] Too many metrics for over-time sampling (max %d).\n", OT_MAX_METRICS);
        return -1;
    }
    ot_count = 0;
    memset(ot_scalar_warned, 0, sizeof(ot_scalar_warned));
    memset(ot_frequency_warned, 0, sizeof(ot_frequency_warned));
    memset(ot_counter_warned, 0, sizeof(ot_counter_warned));
    memset(ot_counter_metric_warned, 0, sizeof(ot_counter_metric_warned));
    if (configured_overtime) probe_configured_system();
    ot_core_count = sys_core_count();
    if (configured_overtime && configured_system[4]) ot_core_count = configured_frequency_cores;
    if (ot_core_count > SYS_MAX_CORES) ot_core_count = SYS_MAX_CORES;
    if (configured_overtime) {
        if (probe_configured_external() != 0) return -1;
        if (configured_external_count > OT_MAX_EXTERNAL_METRICS) {
            fprintf(stderr, "[ERROR] Too many external metrics for over-time sampling (max %d).\n",
                    OT_MAX_EXTERNAL_METRICS);
            return -1;
        }
    }
    ot_find_next_callbacks();
    for (int type = 0; type < 2; type++) {
        int present = type == ENERGY_BACKEND_CPU ? active_cpu_backend != NULL : active_gpu_backend != NULL;
        if (configured_overtime) {
            int selected = 0;
            for (size_t i = 0; i < active_metric_count; i++)
                if (configured_backend_selected && configured_backend_selected[i] &&
                    (int)active_metrics[i].backend_type == type) selected = 1;
            if (!selected) continue;
        }
        if (!present) continue;
        int initialized = type == ENERGY_BACKEND_CPU
            ? active_cpu_backend->init(0) == 0 : active_gpu_backend->init(0) == 0;
        ot_backend_ready[type] = initialized && ot_backend_start((EnergyBackendType)type) == 0;
        if (!ot_backend_ready[type]) {
            if (configured_overtime) {
                fprintf(stderr, "[WARNING] Failed to start selected %s backend.\n",
                        type == ENERGY_BACKEND_CPU ? "CPU" : "GPU");
                configured_backend_failure_guidance((EnergyBackendType)type);
                skip_configured_backend_type((EnergyBackendType)type);
                continue;
            }
            fprintf(stderr, "[WARNING] Failed to start over-time %s backend window; "
                    "window metrics are unavailable.\n", type == ENERGY_BACKEND_CPU ? "CPU" : "GPU");
        }
        if (ot_backend_ready[type]) {
            if (ot_backend_stop((EnergyBackendType)type) != 0) {
                if (configured_overtime) {
                    fprintf(stderr, "[WARNING] Failed to probe selected %s backend.\n",
                            type == ENERGY_BACKEND_CPU ? "CPU" : "GPU");
                    configured_backend_failure_guidance((EnergyBackendType)type);
                    skip_configured_backend_type((EnergyBackendType)type);
                    ot_backend_ready[type] = 0;
                    continue;
                }
                fprintf(stderr, "[WARNING] Failed to probe over-time %s backend window.\n",
                        type == ENERGY_BACKEND_CPU ? "CPU" : "GPU");
                ot_backend_ready[type] = 0;
                continue;
            }
            for (size_t i = 0; i < active_metric_count; i++) {
                if (!configured_overtime || !configured_backend_selected[i] ||
                    (int)active_metrics[i].backend_type != type ||
                    active_metrics[i].descriptor->scope == ENERGY_METRIC_PROCESS) continue;
                double value = read_active_metric(i, 0);
                if (!isfinite(value) || value < 0.0) {
                    skip_configured_backend_metric(i);
                }
            }
            if (configured_overtime) {
                int selected = 0;
                for (size_t i = 0; i < active_metric_count; i++)
                    if (configured_backend_selected[i] && (int)active_metrics[i].backend_type == type)
                        selected = 1;
                if (!selected) {
                    ot_backend_ready[type] = 0;
                    continue;
                }
            }
        }
    }
    for (int type = 0; type < 2; type++) {
        if (ot_backend_ready[type] && ot_backend_start((EnergyBackendType)type) != 0) {
            if (configured_overtime) {
                fprintf(stderr, "[WARNING] Failed to restart selected %s backend after probe.\n",
                        type == ENERGY_BACKEND_CPU ? "CPU" : "GPU");
                configured_backend_failure_guidance((EnergyBackendType)type);
                skip_configured_backend_type((EnergyBackendType)type);
                ot_backend_ready[type] = 0;
                continue;
            }
            fprintf(stderr, "[WARNING] Failed to restart over-time %s backend after probe.\n",
                    type == ENERGY_BACKEND_CPU ? "CPU" : "GPU");
            ot_backend_ready[type] = 0;
        }
    }
    if (configured_overtime && !configured_has_metrics()) {
        fprintf(stderr, "[ERROR] No selected metrics are available.\n");
        return -1;
    }
    return 0;
}

static double ot_counter_value(const ActiveMetric *metric, pid_t workload_pid) {
    EnergyBackendReadCounter read_counter = ot_read_counter[metric->backend_type];
    if (metric->descriptor->scope != ENERGY_METRIC_PROCESS)
        return read_counter(metric->backend_metric_index, 0, 0);

    pid_t processes[SYS_MAX_PROCESS_TREE];
    size_t count = sys_collect_process_tree(workload_pid, processes, SYS_MAX_PROCESS_TREE);
    double total = 0.0;
    int found = 0;
    for (size_t i = 0; i < count; i++) {
        double value = read_counter(metric->backend_metric_index, 0, processes[i]);
        if (isfinite(value) && value >= 0.0) {
            total += value;
            found = 1;
        }
    }
    return found ? total : METRIC_NOT_SUPPORTED;
}

static int ot_sample(pid_t workload_pid, int finished) {
    if (ot_count >= OT_MAX_SAMPLES) {
        fprintf(stderr, "[ERROR] Over-time sample capacity (%d) exceeded; measurement aborted.\n", OT_MAX_SAMPLES);
        return -1;
    }
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        perror("[ERROR] Failed to timestamp over-time sample");
        return -1;
    }
    size_t slot = ot_count++;
    ot_timestamp_us[slot] = (double)(now.tv_sec - ot_start_time.tv_sec) * 1000000.0 +
                            (double)(now.tv_nsec - ot_start_time.tv_nsec) / 1000.0;
    int stopped[2] = {0, 0};
    for (int type = 0; type < 2; type++) {
        if (!ot_backend_ready[type]) continue;
        stopped[type] = (ot_sample_next[type] ? ot_sample_next[type](0)
                                              : ot_backend_stop((EnergyBackendType)type)) == 0;
    }

    if (!configured_overtime || configured_system[0])
        ot_memory_kb[slot] = sys_workload_memory_kb(workload_pid);
    if (!configured_overtime || configured_system[1])
        ot_cores[slot] = sys_core_count();
    if (!configured_overtime || configured_system[2])
        ot_process_values[0][slot] = sys_workload_process_count(workload_pid);
    if (!configured_overtime || configured_system[3])
        ot_process_values[1][slot] = sys_running_processes();
    if (!configured_overtime || configured_system[4]) {
        for (int core = 0; core < ot_core_count; core++) {
            ot_freq_mhz[slot][core] = (float)sys_core_frequency_mhz(core);
            if (configured_overtime &&
                (!isfinite(ot_freq_mhz[slot][core]) || ot_freq_mhz[slot][core] < 0.0)) {
                if (!ot_frequency_warned[core]) {
                    fprintf(stderr, "[WARNING] Failed to read system.core_frequency for core %d; "
                            "recording NOT_SUPPORTED for failed samples.\n", core);
                    ot_frequency_warned[core] = 1;
                }
                ot_freq_mhz[slot][core] = SYS_METRIC_NOT_SUPPORTED;
            }
        }
    }
    if (configured_overtime) {
        static const char *names[] = {
            "system.memory", "system.cores",
            "system.workload_processes", "system.running_processes"
        };
        for (int i = 0; i < 2; i++) {
            long value = i == 0 ? ot_memory_kb[slot] : ot_cores[slot];
            if (configured_system[i] && (value < 0 || (i == 1 && value == 0))) {
                if (!ot_scalar_warned[i]) {
                    fprintf(stderr, "[WARNING] Failed to read %s; recording NOT_SUPPORTED "
                            "for failed samples.\n", names[i]);
                    ot_scalar_warned[i] = 1;
                }
                if (i == 0) ot_memory_kb[slot] = SYS_METRIC_NOT_SUPPORTED;
                else ot_cores[slot] = SYS_METRIC_NOT_SUPPORTED;
            }
        }
        static const char *process_names[] = {
            "system.workload_processes", "system.running_processes"
        };
        for (int i = 0; i < 2; i++) {
            int system_index = i + 2;
            int value = ot_process_values[i][slot];
            if (configured_system[system_index] && value < 0 &&
                !(finished && i == 0)) {
                if (!ot_scalar_warned[system_index]) {
                    fprintf(stderr, "[WARNING] Failed to read %s; recording NOT_SUPPORTED "
                            "for failed samples.\n", process_names[i]);
                    ot_scalar_warned[system_index] = 1;
                }
                ot_process_values[i][slot] = SYS_METRIC_NOT_SUPPORTED;
            }
        }
        for (size_t i = 0; i < configured_external_count; i++) {
            ConfiguredExternalMetric *metric = &configured_external_metrics[i];
            if (read_configured_external(metric, &ot_external_values[i][slot]) != 0) {
                fprintf(stderr, "[WARNING] Recording NOT_SUPPORTED for the failed external metric sample.\n");
                ot_external_values[i][slot].type = GREEN_METRIC_INVALID;
            }
        }
    }
    for (size_t i = 0; i < active_metric_count; i++) {
        if (configured_overtime && (!configured_backend_selected || !configured_backend_selected[i])) continue;
        const ActiveMetric *metric = &active_metrics[i];
        int type = metric->backend_type;
        if (metric->descriptor->kind == ENERGY_METRIC_WINDOW) {
            if (!ot_read_counter[type] && !ot_counter_warned[type]) {
                fprintf(stderr, "[WARNING] %s backend does not expose counter readings; "
                        "its window metrics will be recorded as NOT_SUPPORTED.\n",
                        type == ENERGY_BACKEND_CPU ? "CPU" : "GPU");
                ot_counter_warned[type] = 1;
            }
            ot_backend_values[i][slot] = stopped[type] && ot_read_counter[type]
                ? ot_counter_value(metric, workload_pid)
                : METRIC_NOT_SUPPORTED;
            if (stopped[type] && ot_read_counter[type] &&
                (!isfinite(ot_backend_values[i][slot]) || ot_backend_values[i][slot] < 0.0) &&
                !ot_counter_metric_warned[i]) {
                fprintf(stderr, "[WARNING] Counter reading for '%s.%s' is unavailable; "
                        "recording NOT_SUPPORTED.\n",
                        type == ENERGY_BACKEND_CPU ? "cpu" : "gpu", metric->descriptor->name);
                ot_counter_metric_warned[i] = 1;
            }
        } else {
            ot_backend_values[i][slot] =
                !stopped[type] && configured_overtime ? METRIC_NOT_SUPPORTED
                : metric->descriptor->scope == ENERGY_METRIC_PROCESS
                    ? read_process_tree_metric(i, workload_pid) : read_active_metric(i, 0);
        }
        double value = ot_backend_values[i][slot];
        if (configured_overtime && (!isfinite(value) || value < 0.0) &&
            !(finished && metric->descriptor->scope == ENERGY_METRIC_PROCESS && stopped[type])) {
            if (ot_backend_ready[type])
                fprintf(stderr, "[WARNING] Failed to read selected metric '%s.%s'; recording NOT_SUPPORTED.\n",
                        type == ENERGY_BACKEND_CPU ? "cpu" : "gpu", metric->descriptor->name);
            ot_backend_values[i][slot] = METRIC_NOT_SUPPORTED;
        }
    }
    for (int type = 0; type < 2; type++) {
        if (!ot_backend_ready[type]) continue;
        if (!stopped[type]) {
            if (configured_overtime) {
                fprintf(stderr, "[WARNING] Selected %s backend sample failed; "
                        "its metrics are unavailable for the rest of the measurement.\n",
                        type == ENERGY_BACKEND_CPU ? "CPU" : "GPU");
                ot_backend_ready[type] = 0;
                continue;
            }
            fprintf(stderr, "[ERROR] Selected %s backend sample failed.\n",
                    type == ENERGY_BACKEND_CPU ? "CPU" : "GPU");
            if (configured_overtime) configured_backend_failure_guidance((EnergyBackendType)type);
            return -1;
        } else if (!finished && !ot_sample_next[type] && ot_backend_start((EnergyBackendType)type) != 0) {
            if (configured_overtime) {
                fprintf(stderr, "[WARNING] Failed to restart selected %s backend; "
                        "its metrics are unavailable for the rest of the measurement.\n",
                        type == ENERGY_BACKEND_CPU ? "CPU" : "GPU");
                ot_backend_ready[type] = 0;
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

static int ot_dump(const struct OvertimeContext *ctx) {
    FILE *fp = fopen(ctx->output_file, ctx->append ? "a" : "w");
    if (!fp) {
        perror("[ERROR] Failed to open over-time CSV");
        return -1;
    }
    energy_share_output_file(fileno(fp));
    if (!ctx->append) energy_backend_write_csv_header(fp, 1);
    for (size_t row = 0; row < ot_count; row++) {
        fprintf(fp, "%s, %s, %d, %.6f", ctx->language, ctx->program, ctx->powercap,
                ot_timestamp_us[row]);
        for (int i = 0; i < OT_SCALAR_COUNT; i++) {
            if (configured_overtime && !configured_system[i]) continue;
            fprintf(fp, ", ");
            if (i == 0) write_system_integer_value(fp, ot_memory_kb[row]);
            else if (i == 1) write_system_integer_value(fp, ot_cores[row]);
            else write_system_integer_value(fp, ot_process_values[i - 2][row]);
        }
        for (int core = 0; (!configured_overtime || configured_system[4]) && core < ot_core_count; core++) {
            fprintf(fp, ", ");
            write_active_metric_value(fp, (double)ot_freq_mhz[row][core]);
        }
        for (size_t i = 0; i < active_metric_count; i++) {
            if (configured_overtime && (!configured_backend_selected || !configured_backend_selected[i])) continue;
            fprintf(fp, ", ");
            write_active_metric_value(fp, ot_backend_values[i][row]);
        }
        for (size_t i = 0; configured_overtime && i < configured_external_count; i++) {
            fprintf(fp, ", ");
            write_external_metric_value(fp, &configured_external_metrics[i],
                                        ot_external_values[i][row]);
        }
        fputc('\n', fp);
    }
    int failed = ferror(fp);
    if (fclose(fp) != 0) failed = 1;
    if (failed) fprintf(stderr, "[ERROR] Failed to write over-time CSV.\n");
    return failed ? -1 : 0;
}

static pid_t ot_start_command(const char *command, const sigset_t *old_mask) {
    pid_t pid = fork();
    if (pid != 0) return pid;
    if (sigprocmask(SIG_SETMASK, old_mask, NULL) != 0) {
        perror("[ERROR] Failed to prepare workload process");
        _exit(127);
    }
    execl("/bin/sh", "sh", "-c", command, (char *)NULL);
    perror("[ERROR] Unable to start workload command");
    _exit(127);
}

static void ot_terminate_command(pid_t pid) {
    pid_t processes[SYS_MAX_PROCESS_TREE];
    size_t count = sys_collect_process_tree(pid, processes, SYS_MAX_PROCESS_TREE);
    if (count == SYS_MAX_PROCESS_TREE) {
        fprintf(stderr, "[WARNING] Workload process tree exceeds the termination limit.\n");
    }
    for (size_t i = 0; i < count; i++) {
        if (kill(processes[i], SIGKILL) != 0 && errno != ESRCH) {
            perror("[ERROR] Failed to terminate workload process");
        }
    }
    pid_t waited;
    do { waited = waitpid(pid, NULL, 0); } while (waited < 0 && errno == EINTR);
    if (waited < 0 && errno != ECHILD) perror("[ERROR] Failed to reap terminated workload");
}

static int ot_run(const char *command, const struct OvertimeContext *ctx,
                  unsigned long interval_us) {
    sigset_t mask, old_mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGCHLD);
    if (sigprocmask(SIG_BLOCK, &mask, &old_mask) != 0) {
        perror("[ERROR] Failed to block SIGCHLD");
        return -1;
    }
    int result = -1;
    pid_t pid = -1;
    if (ot_prepare() != 0 || clock_gettime(CLOCK_MONOTONIC, &ot_start_time) != 0) {
        fprintf(stderr, "[ERROR] Failed to initialize over-time sampling.\n");
        goto restore_mask;
    }
    pid = ot_start_command(command, &old_mask);
    if (pid < 0) {
        perror("[ERROR] Failed to create workload process");
        goto restore_mask;
    }

    struct timespec deadline = ot_start_time;
    deadline.tv_sec += (time_t)(interval_us / 1000000UL);
    deadline.tv_nsec += (long)(interval_us % 1000000UL) * 1000L;
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec++;
        deadline.tv_nsec -= 1000000000L;
    }
    for (;;) {
        siginfo_t info = {0};
        if (waitid(P_PID, (id_t)pid, &info, WEXITED | WNOHANG | WNOWAIT) != 0) {
            if (errno == EINTR) continue;
            perror("[ERROR] Failed while checking workload");
            goto abort_workload;
        }
        struct timespec now;
        if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
            perror("[ERROR] Failed to read monotonic clock");
            goto abort_workload;
        }
        int finished = info.si_pid == pid;
        int due = now.tv_sec > deadline.tv_sec ||
                  (now.tv_sec == deadline.tv_sec && now.tv_nsec >= deadline.tv_nsec);
        if (finished || due) {
            if (ot_sample(pid, finished) != 0) goto abort_workload;
            if (finished) {
                int status;
                pid_t waited;
                do { waited = waitpid(pid, &status, 0); } while (waited < 0 && errno == EINTR);
                if (waited < 0) {
                    perror("[ERROR] Failed to reap workload");
                    goto restore_mask;
                }
                pid = -1;
                result = WIFEXITED(status) ? WEXITSTATUS(status)
                    : WIFSIGNALED(status) ? 128 + WTERMSIG(status) : -1;
                if (result == 0 && ot_dump(ctx) != 0) result = -1;
                goto restore_mask;
            }
            if (clock_gettime(CLOCK_MONOTONIC, &deadline) != 0) {
                perror("[ERROR] Failed to read monotonic clock");
                goto abort_workload;
            }
            deadline.tv_sec += (time_t)(interval_us / 1000000UL);
            deadline.tv_nsec += (long)(interval_us % 1000000UL) * 1000L;
            if (deadline.tv_nsec >= 1000000000L) {
                deadline.tv_sec++;
                deadline.tv_nsec -= 1000000000L;
            }
            continue;
        }
        struct timespec timeout = {deadline.tv_sec - now.tv_sec, deadline.tv_nsec - now.tv_nsec};
        if (timeout.tv_nsec < 0) {
            timeout.tv_sec--;
            timeout.tv_nsec += 1000000000L;
        }
        if (sigtimedwait(&mask, NULL, &timeout) < 0 && errno != EAGAIN && errno != EINTR) {
            perror("[ERROR] Failed while waiting for workload");
            goto abort_workload;
        }
        if (global_terminate) {
            fprintf(stderr, "[ERROR] Over-time measurement interrupted.\n");
            goto abort_workload;
        }
    }

abort_workload:
    ot_terminate_command(pid);
restore_mask:
    if (sigprocmask(SIG_SETMASK, &old_mask, NULL) != 0) {
        perror("[ERROR] Failed to restore signal mask");
        result = -1;
    }
    return result;
}
